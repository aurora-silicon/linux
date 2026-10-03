// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Queue records and the commands of a queue's item ring.
//!
//! A hardware queue is described by a [`QueueRecord`]. Its [`PointerBlock`]
//! holds the cursors of the queue's item ring, an array of [`ITEM_RING_ENTRIES`]
//! work pointers: 64-bit GPU addresses of commands. Every command starts with
//! a 32-bit [`CommandTag`]: work descriptors carry the tiling, fragment and
//! compute tags (see `render.rs` and `compute.rs`), and the per-kick records
//! defined here carry the others.
//!
//! One kick appends, in order, the work descriptor, a [`QueueConfig`], a
//! [`KickAnnounce`] record and, when a compute queue is first installed or
//! explicitly waits on another queue, a [`KickPredecessor`].
//! Compute kicks always carry the [`QueueConfig`]. A render kick carries it
//! only when it installs the queue, updates the context or QoS identity,
//! carries a completion seed, or configures the queue differently from the
//! last [`QueueConfig`] published on it.

use core::sync::atomic::{AtomicU32, Ordering};
use kernel::prelude::*;

use super::kick::{KickTimestamp, QID_MAX};

/// Priority of every queue the driver creates.
///
/// The same value appears in the queue record, [`QueueConfig`],
/// [`KickAnnounce`] and the kick entry header; the work rings it selects are
/// listed in `channels.rs`.
pub(crate) const QUEUE_PRIORITY: u8 = 2;
/// Scheduling policy of every queue the driver creates.
pub(crate) const QUEUE_POLICY: u16 = 2;
/// Number of work pointers in a queue's item ring.
pub(crate) const ITEM_RING_ENTRIES: u32 = 0x500;

/// Tag in the first word of every command.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum CommandTag {
    /// Tiling (TA) work descriptor.
    Tiling = 0,
    /// Fragment (3D) work descriptor.
    Fragment = 1,
    /// Compute work descriptor.
    Compute = 3,
    /// [`KickAnnounce`].
    KickAnnounce = 0x0e,
    /// [`QueueConfig`].
    QueueConfig = 0x0f,
    /// [`KickPredecessor`].
    KickPredecessor = 0x10,
}

/// The engine that executes a queue's work.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum DataMaster {
    /// Tiling (TA).
    Tiling = 0,
    /// Fragment (3D).
    Fragment = 1,
    /// Compute (CL).
    Compute = 2,
}

/// Cursors of a queue's item ring.
///
/// The firmware writes `done` and `unk_30`. The host writes the block on
/// three paths, never with a structure copy once the queue was published:
///
/// - creation: the whole block, [`PointerBlock::new`], before the queue is
///   first published;
/// - publication: `producer`, with one aligned 32-bit store per publication
///   step, after a barrier that orders the work pointers and commands before
///   it;
/// - reuse of a render queue for a new owner: [`PointerBlock::rebase`].
#[repr(C)]
#[derive(Debug)]
pub(crate) struct PointerBlock {
    /// Item-ring position up to which the firmware has finished commands.
    /// Free ring space is measured from it, so it bounds `producer`.
    pub(crate) done: AtomicU32,
    pub(crate) unk_04: [u32; 11],
    /// Written by the firmware. The host treats the queue as idle only when
    /// this cursor, `done` and `producer` are equal.
    pub(crate) unk_30: AtomicU32,
    pub(crate) unk_34: [u32; 3],
    /// Item-ring position after the last work pointer the host published.
    pub(crate) producer: AtomicU32,
    pub(crate) unk_44: [u32; 3],
    /// On recovery, the host copies producer here before acknowledging resume.
    pub(crate) recovery_target: u32,
    pub(crate) unk_54: [u32; 3],
    /// Number of entries in the item ring.
    pub(crate) entries: u32,
    pub(crate) unk_64: [u32; 7],
}

static_assert!(size_of::<PointerBlock>() == 0x80);
static_assert!(core::mem::offset_of!(PointerBlock, unk_30) == 0x30);
static_assert!(core::mem::offset_of!(PointerBlock, producer) == 0x40);
static_assert!(core::mem::offset_of!(PointerBlock, entries) == 0x60);

// SAFETY: PointerBlock contains only integers and integer atomics.
unsafe impl Zeroable for PointerBlock {}

impl PointerBlock {
    pub(crate) const PRODUCER: usize = core::mem::offset_of!(Self, producer);
    pub(crate) const RECOVERY_TARGET: usize = core::mem::offset_of!(Self, recovery_target);

    /// Returns the cursors of an empty item ring.
    pub(crate) const fn new() -> Self {
        Self {
            done: AtomicU32::new(0),
            unk_04: [0; 11],
            unk_30: AtomicU32::new(0),
            unk_34: [0; 3],
            producer: AtomicU32::new(0),
            unk_44: [0; 3],
            recovery_target: u32::MAX,
            unk_54: [0; 3],
            entries: ITEM_RING_ENTRIES,
            unk_64: [0; 7],
        }
    }

    /// Moves the cursors of an empty queue to item-ring position `base`.
    ///
    /// Used when a render queue pair that was kicked before is handed to a new
    /// owner, with `base` the position the host tracks for the new owner's
    /// queues. Only valid while the firmware does not run the queue, with all
    /// three cursors zero. Writes `done`, `unk_30` and `producer`, in this
    /// order, with single stores; the caller issues a full barrier afterwards.
    pub(crate) fn rebase(&self, base: u32) {
        self.done.store(base, Ordering::Relaxed);
        self.unk_30.store(base, Ordering::Relaxed);
        self.producer.store(base, Ordering::Relaxed);
    }
}

/// Head of a queue's job list.
///
/// Live state: the firmware links and unlinks jobs; the host writes the head
/// only at queue creation.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct JobListHead {
    /// GPU address of the first job, or zero.
    pub(crate) first_va: u64,
    /// GPU address of the last link: the head itself when the list is empty.
    pub(crate) last_va: u64,
}

impl JobListHead {
    /// Returns an empty list whose head is at `self_va`.
    pub(crate) const fn new(self_va: u64) -> Self {
        Self {
            first_va: 0,
            last_va: self_va,
        }
    }
}

/// Addresses and owner a [`QueueRecord`] names.
pub(crate) struct QueueRecordArgs {
    /// GPU address of the queue's [`PointerBlock`].
    pub(crate) pointers_va: u64,
    /// GPU address of the queue's item ring.
    pub(crate) items_va: u64,
    /// GPU address of the job list the queue's work is linked into.
    pub(crate) job_list_va: u64,
    /// GPU address of the owning logical queue's scheduler-state page.
    pub(crate) scheduler_va: u64,
    /// Thread-group ID of the owning process (0 while unowned).
    pub(crate) owner_pid: u32,
}

/// Description of one hardware queue, named by its work-ring slots.
///
/// The host writes it whole at queue creation and changes the owner fields of
/// an idle queue with [`QueueRecord::set_owner`] or
/// [`QueueRecord::set_compute_owner`].
#[repr(C, packed)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct QueueRecord {
    /// GPU address of the [`PointerBlock`].
    pub(crate) pointers_va: u64,
    /// GPU address of the item ring.
    pub(crate) items_va: u64,
    /// GPU address of the job list.
    pub(crate) job_list_va: u64,
    pub(crate) unk_18: [u8; 0xc],
    pub(crate) unk_24: u32,
    /// Queue priority, stored twice.
    pub(crate) priority: [u32; 2],
    pub(crate) unk_30: [u8; 6],
    pub(crate) unk_36: u16,
    /// Zero at creation; cleared again when a compute queue changes owner.
    pub(crate) unk_38: u32,
    pub(crate) unk_3c: u32,
    /// Scheduling policy.
    pub(crate) policy: u32,
    pub(crate) unk_44: u32,
    /// Thread-group ID of the owning process.
    pub(crate) owner_pid: u32,
    pub(crate) unk_4c: [u8; 0x50],
    /// GPU address of the owner's scheduler-state page.
    pub(crate) scheduler_va: u64,
    pub(crate) unk_a4: [u8; 0x1c],
}

static_assert!(size_of::<QueueRecord>() == 0xc0);
static_assert!(core::mem::offset_of!(QueueRecord, priority) == 0x28);
static_assert!(core::mem::offset_of!(QueueRecord, unk_38) == 0x38);
static_assert!(core::mem::offset_of!(QueueRecord, policy) == 0x40);
static_assert!(core::mem::offset_of!(QueueRecord, scheduler_va) == 0x9c);

impl QueueRecord {
    /// Builds the record of a newly created queue.
    pub(crate) fn new(args: &QueueRecordArgs) -> Self {
        Self {
            pointers_va: args.pointers_va,
            items_va: args.items_va,
            job_list_va: args.job_list_va,
            unk_18: [0; 0xc],
            unk_24: u32::MAX,
            priority: [QUEUE_PRIORITY as u32; 2],
            unk_30: [0; 6],
            unk_36: u16::MAX,
            unk_38: 0,
            unk_3c: 0,
            policy: QUEUE_POLICY as u32,
            unk_44: u32::MAX,
            owner_pid: args.owner_pid,
            unk_4c: [0; 0x50],
            scheduler_va: args.scheduler_va,
            unk_a4: [0; 0x1c],
        }
    }

    /// Hands an idle compute queue to a new owner.
    ///
    /// Replaces the job head, process identity and scheduler page, and clears `unk_38`.
    pub(crate) fn set_compute_owner(
        &mut self,
        job_list_va: u64,
        scheduler_va: u64,
        owner_pid: u32,
    ) {
        self.job_list_va = job_list_va;
        self.unk_38 = 0;
        self.owner_pid = owner_pid;
        self.scheduler_va = scheduler_va;
    }
}

/// Free list a queue's USC private memory is allocated from.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct FreeListBinding {
    /// GPU address of the free list's shared control object.
    pub(crate) va: u64,
    /// Generation of the free list's registration.
    pub(crate) generation: u64,
    /// Buffer slot of the free list.
    pub(crate) slot: u32,
}

/// Parameter-buffer binding of a tiling queue.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct TilingBinding {
    /// GPU address of the parameter-buffer state object.
    pub(crate) pb_va: u64,
    /// Allocation token of the parameter buffer's buffer slot.
    pub(crate) pb_token: u64,
    /// Buffer slot of the parameter buffer.
    pub(crate) pb_slot: u32,
    /// QID of the fragment queue paired with this tiling queue.
    pub(crate) fragment_qid: u8,
}

/// Queue whose configuration a [`QueueConfig`] carries.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum QueueConfigTarget {
    /// A tiling queue and its parameter-buffer binding.
    Tiling(TilingBinding),
    /// A fragment queue.
    Fragment,
    /// A compute queue.
    Compute,
}

/// Inputs of one [`QueueConfig`].
pub(crate) struct QueueConfigArgs {
    /// Kind of queue.
    pub(crate) target: QueueConfigTarget,
    /// GPU address of the queue's kick ring.
    pub(crate) kick_ring_va: u64,
    /// Firmware alias of the queue's kick ring.
    pub(crate) kick_ring_fw_va: u64,
    /// Configured queue.
    pub(crate) qid: u8,
    /// The firmware has not yet installed this QID.
    pub(crate) install: bool,
    /// Completion value the firmware loads for the queue, or `None` to keep
    /// the value it holds.
    pub(crate) completion_seed: Option<u64>,
    /// UAT context the queue's work runs in.
    pub(crate) context_id: u16,
    /// USC private-memory free list.
    pub(crate) free_list: FreeListBinding,
    /// GPU address of the owner's scheduler-state page.
    pub(crate) scheduler_va: u64,
    /// QoS buffer slot of the logical queue.
    pub(crate) qos_slot: u8,
    /// The QoS identity or queue membership changed since the last update.
    pub(crate) qos_update: bool,
    /// Thread-group ID of the owning process.
    pub(crate) owner_pid: u32,
    /// The queue's UAT context changed since the last update.
    pub(crate) context_update: bool,
}

/// Configuration of one hardware queue (tag 15).
///
/// The firmware applies it when it consumes the command, which may be after
/// later kicks were published, so a record must not be rewritten while a
/// work pointer names it.
#[repr(C, packed)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct QueueConfig {
    /// [`CommandTag::QueueConfig`].
    pub(crate) tag: u32,
    pub(crate) unk_04: u32,
    /// GPU address of the queue's kick ring.
    pub(crate) kick_ring_va: u64,
    /// Firmware alias of the queue's kick ring.
    pub(crate) kick_ring_fw_va: u64,
    /// Configured queue.
    pub(crate) qid: u16,
    /// Non-zero to install the QID.
    pub(crate) install: u16,
    pub(crate) unk_1c: u16,
    /// Queue priority.
    pub(crate) priority: u16,
    pub(crate) unk_20: u16,
    /// [`DataMaster`] of the queue.
    pub(crate) data_master: u16,
    pub(crate) unk_24: u16,
    /// Non-zero when `completion_seed` is valid.
    pub(crate) completion_seed_valid: u32,
    /// Completion value: kick timestamp in bits 39:0, render-pass sequence in
    /// bits 47:40 (see [`KickTimestamp::completion_value`]).
    pub(crate) completion_seed: u64,
    /// UAT context of the queue's work.
    pub(crate) context_id: u16,
    pub(crate) unk_34: u16,
    /// GPU address of the USC free list's shared control object.
    pub(crate) free_list_va: u64,
    /// Generation of the USC free list.
    pub(crate) free_list_generation: u64,
    /// Buffer slot of the USC free list.
    pub(crate) free_list_slot: u32,
    /// GPU address of the owner's scheduler-state page.
    pub(crate) scheduler_va: u64,
    /// Non-zero when the QoS identity or membership changed.
    pub(crate) qos_update: u32,
    /// QoS buffer slot.
    pub(crate) qos_slot: u16,
    pub(crate) unk_58: u16,
    /// Thread-group ID of the owning process.
    pub(crate) owner_pid: u32,
    /// Scheduling policy.
    pub(crate) policy: u16,
    pub(crate) unk_60: u16,
    /// Non-zero when the UAT context changed.
    pub(crate) context_update: u16,
    pub(crate) unk_64: u16,
    pub(crate) unk_66: u16,
    pub(crate) unk_68: [u8; 6],
    /// Tiling queues: GPU address of the parameter-buffer state; else zero.
    pub(crate) pb_va: u64,
    /// Tiling queues: parameter-buffer slot allocation token; else all ones.
    pub(crate) pb_token: u64,
    /// Tiling queues: parameter-buffer slot; else all ones.
    pub(crate) pb_slot: u32,
    /// Tiling queues: QID of the paired fragment queue; else all ones.
    pub(crate) fragment_qid: u32,
    pub(crate) unk_86: [u8; 0x3a],
}

static_assert!(size_of::<QueueConfig>() == 0xc0);
static_assert!(core::mem::offset_of!(QueueConfig, completion_seed_valid) == 0x26);
static_assert!(core::mem::offset_of!(QueueConfig, free_list_va) == 0x36);
static_assert!(core::mem::offset_of!(QueueConfig, scheduler_va) == 0x4a);
static_assert!(core::mem::offset_of!(QueueConfig, qos_slot) == 0x56);
static_assert!(core::mem::offset_of!(QueueConfig, owner_pid) == 0x5a);
static_assert!(core::mem::offset_of!(QueueConfig, pb_va) == 0x6e);
static_assert!(core::mem::offset_of!(QueueConfig, fragment_qid) == 0x82);

impl QueueConfig {
    const UNK_66: u16 = 1;

    /// Builds the record described by `args`.
    pub(crate) fn new(args: &QueueConfigArgs) -> Result<Self> {
        if args.qid > QID_MAX {
            return Err(EINVAL);
        }
        let (data_master, pb_va, pb_token, pb_slot, fragment_qid) = match args.target {
            QueueConfigTarget::Tiling(binding) => {
                if binding.fragment_qid > QID_MAX {
                    return Err(EINVAL);
                }
                (
                    DataMaster::Tiling,
                    binding.pb_va,
                    binding.pb_token,
                    binding.pb_slot,
                    binding.fragment_qid as u32,
                )
            }
            QueueConfigTarget::Fragment => (DataMaster::Fragment, 0, u64::MAX, u32::MAX, u32::MAX),
            QueueConfigTarget::Compute => (DataMaster::Compute, 0, u64::MAX, u32::MAX, u32::MAX),
        };
        Ok(Self {
            tag: CommandTag::QueueConfig as u32,
            unk_04: 0,
            kick_ring_va: args.kick_ring_va,
            kick_ring_fw_va: args.kick_ring_fw_va,
            qid: args.qid as u16,
            install: args.install as u16,
            unk_1c: 0,
            priority: QUEUE_PRIORITY as u16,
            unk_20: 0,
            data_master: data_master as u16,
            unk_24: 0,
            completion_seed_valid: args.completion_seed.is_some() as u32,
            completion_seed: args.completion_seed.unwrap_or(0),
            context_id: args.context_id,
            unk_34: 0,
            free_list_va: args.free_list.va,
            free_list_generation: args.free_list.generation,
            free_list_slot: args.free_list.slot,
            scheduler_va: args.scheduler_va,
            qos_update: args.qos_update as u32,
            qos_slot: args.qos_slot as u16,
            unk_58: 0,
            owner_pid: args.owner_pid,
            policy: QUEUE_POLICY,
            unk_60: 0,
            context_update: args.context_update as u16,
            unk_64: 0,
            unk_66: Self::UNK_66,
            unk_68: [0; 6],
            pb_va,
            pb_token,
            pb_slot,
            fragment_qid,
            unk_86: [0; 0x3a],
        })
    }
}

/// Announcement of a newly written kick entry of a queue (tag 14).
///
/// A record slot larger than this structure must be zero beyond it.
#[repr(C, packed)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct KickAnnounce {
    /// [`CommandTag::KickAnnounce`].
    pub(crate) tag: u32,
    /// Kicked queue.
    pub(crate) qid: u16,
    /// Number of entries announced.
    pub(crate) count: u16,
    /// Queue priority.
    pub(crate) priority: u8,
    /// Timestamp of the announced entry.
    pub(crate) timestamp: u64,
    /// [`DataMaster`] of the queue.
    pub(crate) data_master: u32,
    pub(crate) unk_15: [u8; 3],
}

static_assert!(size_of::<KickAnnounce>() == 0x18);
static_assert!(core::mem::offset_of!(KickAnnounce, timestamp) == 0x09);
static_assert!(core::mem::offset_of!(KickAnnounce, data_master) == 0x11);

impl KickAnnounce {
    /// Announces the entry of queue `qid` written at `timestamp`.
    pub(crate) fn new(qid: u8, data_master: DataMaster, timestamp: KickTimestamp) -> Result<Self> {
        if qid > QID_MAX {
            return Err(EINVAL);
        }
        Ok(Self {
            tag: CommandTag::KickAnnounce as u32,
            qid: qid as u16,
            count: 1,
            priority: QUEUE_PRIORITY,
            timestamp: timestamp.get(),
            data_master: data_master as u32,
            unk_15: [0; 3],
        })
    }
}

/// Names the kick of a compute queue that precedes the announced one
/// (tag 16).
///
/// A record slot larger than this structure must be zero beyond it.
#[repr(C, packed)]
#[derive(Copy, Clone, Debug)]
pub(crate) struct KickPredecessor {
    /// [`CommandTag::KickPredecessor`].
    pub(crate) tag: u32,
    /// Kicked queue.
    pub(crate) qid: u16,
    /// Timestamp of the queue's kick preceding the announced one.
    pub(crate) previous: u64,
    pub(crate) unk_0e: u32,
}

static_assert!(size_of::<KickPredecessor>() == 0x12);

impl KickPredecessor {
    const UNK_0E: u32 = 0xfe;

    /// Names `previous` as the kick of queue `qid` preceding the announced
    /// one.
    pub(crate) fn new(qid: u8, previous: KickTimestamp) -> Result<Self> {
        if qid > QID_MAX {
            return Err(EINVAL);
        }
        Ok(Self {
            tag: CommandTag::KickPredecessor as u32,
            qid: qid as u16,
            previous: previous.get(),
            unk_0e: Self::UNK_0E,
        })
    }
}
