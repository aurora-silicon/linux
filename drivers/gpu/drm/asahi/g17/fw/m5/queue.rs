// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 queue encodings (linux-aurora t6050-gpu's g17/fw/m5/queue.rs): the queue record, the
//! current-kick notification, the M5 meanings of the common [`QueueConfig`] and the host command
//! port transaction, for compute queues and render pairs.

use super::super::{
    kick::{KickTimestamp, QID_MAX},
    queue::{
        DataMaster, KickPredecessor, Policy, QueueConfig, QueueConfigArgs, QueueConfigTarget,
        QueueRecordArgs,
    },
};
use kernel::prelude::*;

/// One M5 scheduling class: the profile of a queue at that firmware priority class, and the
/// class-specific record words stored for it and its QoS byte (the QoS byte
/// is that of a queue registered alone, the
/// full share of one isolated queue).
struct Class {
    policy: Policy,
    reserved_30: [u8; 6],
    sentinel_36: u16,
    reserved_38: u32,
}

/// The four M5 classes, indexed by firmware priority class: DRM's `REALTIME - priority` as
/// `file.rs` passes it, so REALTIME is class 0 (the high profile), HIGH class 1, MEDIUM
/// class 2 (the default) and LOW class 3 (the background profile). Each class has
/// its own work ring and completion class; nothing else varies with it.
const CLASSES: [Class; Policy::PRIORITIES as usize] = [
    Class {
        policy: Policy::new(0, 1, 15, 8192),
        reserved_30: [0, 0, 0xff, 0xff, 0xff, 0xff],
        sentinel_36: u16::MAX,
        reserved_38: 1,
    },
    Class {
        policy: Policy::new(1, 0, 20, 8192),
        reserved_30: [0, 0, 0, 0, 0xff, 0xff],
        sentinel_36: u16::MAX,
        reserved_38: 0,
    },
    Class {
        policy: Policy::new(2, 2, 4, 8192),
        reserved_30: [0; 6],
        sentinel_36: u16::MAX,
        reserved_38: 0,
    },
    Class {
        policy: Policy::new(3, 0, 20, 8192),
        reserved_30: [0; 6],
        sentinel_36: 0,
        reserved_38: 0,
    },
];

/// The profile of firmware priority class `priority` (`REALTIME - priority` for a DRM queue).
pub(crate) fn scheduling_policy(priority: u32) -> Result<Policy> {
    Ok(CLASSES
        .get(usize::try_from(priority)?)
        .ok_or(EINVAL)?
        .policy)
}

/// The default class, DRM's MEDIUM: the qualification's queues run at it.
pub(crate) const MEDIUM: Policy = CLASSES[2].policy;

/// The M5 queue record. The scheduler pointer moved from the Neo record's 0x9c to 0x70; the
/// policy and owner fields also moved.
#[repr(C, packed)]
#[derive(Clone, Copy)]
pub(crate) struct QueueRecord {
    pointers_va: u64,
    items_va: u64,
    job_list_va: u64,
    reserved_18: [u8; 0xc],
    sentinel_24: u32,
    priority: [u32; 2],
    reserved_30: [u8; 6],
    sentinel_36: u16,
    reserved_38: u32,
    policy: u32,
    sentinel_40: u32,
    owner_pid: u32,
    reserved_48: [u8; 0x28],
    scheduler_va: u64,
    reserved_78: [u8; 8],
}

static_assert!(size_of::<QueueRecord>() == 0x80);
static_assert!(core::mem::offset_of!(QueueRecord, policy) == 0x3c);
static_assert!(core::mem::offset_of!(QueueRecord, owner_pid) == 0x44);
static_assert!(core::mem::offset_of!(QueueRecord, scheduler_va) == 0x70);

impl QueueRecord {
    /// The record words of `policy`'s class. Only the profiles of [`CLASSES`] are encodable.
    fn scheduling_words(policy: Policy) -> Result<([u8; 6], u16, u32)> {
        match CLASSES.get(usize::from(policy.priority())) {
            Some(class) if class.policy == policy => {
                Ok((class.reserved_30, class.sentinel_36, class.reserved_38))
            }
            _ => Err(EINVAL),
        }
    }

    /// A new owner's host-owned words on an installed record (S `set_compute_owner`): job-list
    /// head, scheduler page, owner, priority and class words. A class change invalidates +0x40.
    /// Firmware-owned words stay.
    pub(crate) fn set_owner(
        &mut self,
        job_list_va: u64,
        scheduler_va: u64,
        owner_pid: u32,
        policy: Policy,
    ) -> Result {
        let (reserved_30, sentinel_36, reserved_38) = Self::scheduling_words(policy)?;
        let priority = self.priority;
        if priority[0] != u32::from(policy.priority()) {
            self.sentinel_40 = u32::MAX;
        }
        self.job_list_va = job_list_va;
        self.scheduler_va = scheduler_va;
        self.owner_pid = owner_pid;
        self.priority = [u32::from(policy.priority()); 2];
        self.reserved_30 = reserved_30;
        self.sentinel_36 = sentinel_36;
        self.reserved_38 = reserved_38;
        self.policy = u32::from(policy.policy());
        Ok(())
    }

    /// Builds the record described by `args`.
    pub(crate) fn new(args: &QueueRecordArgs) -> Result<Self> {
        let (reserved_30, sentinel_36, reserved_38) = Self::scheduling_words(args.policy)?;
        Ok(Self {
            pointers_va: args.pointers_va,
            items_va: args.items_va,
            job_list_va: args.job_list_va,
            reserved_18: [0; 0xc],
            sentinel_24: u32::MAX,
            priority: [u32::from(args.policy.priority()); 2],
            reserved_30,
            sentinel_36,
            reserved_38,
            policy: u32::from(args.policy.policy()),
            sentinel_40: u32::MAX,
            owner_pid: args.owner_pid,
            reserved_48: [0; 0x28],
            scheduler_va: args.scheduler_va,
            reserved_78: [0; 8],
        })
    }
}

/// The M5 current-kick notification: the common tag-16 record naming the kick being published
/// (not its predecessor), zero-padded to 24 bytes. The descriptor is reached through the kick
/// entry.
#[repr(C, packed)]
#[derive(Clone, Copy)]
pub(crate) struct Notification {
    record: KickPredecessor,
    reserved: [u8; 6],
}

static_assert!(size_of::<Notification>() == 0x18);

impl Notification {
    /// Names `current`, the kick of queue `qid` being published, with the gate that kick may
    /// carry: only a written notification can open it ([`EntryGate`]).
    pub(crate) fn new(qid: u8, current: KickTimestamp) -> Result<(Self, EntryGate)> {
        Ok((
            Self {
                record: KickPredecessor::new(qid, current)?,
                reserved: [0; 6],
            },
            EntryGate(()),
        ))
    }
}

/// The entry-signal gate of a kick that has a current-kick notification: event 254, which the
/// firmware clears in the kick the notification names once it has handled the record. A kick
/// carrying it is not eligible before then. Only [`Notification::new`] makes one, so a kick
/// cannot wait on a gate that no notification opens.
#[derive(Clone, Copy)]
pub(crate) struct EntryGate(());

impl EntryGate {
    /// The gate's event: bit 62 of the event mask's fourth word.
    const EVENT: u32 = 254;

    /// The event mask with only the gate set.
    pub(crate) const fn events(self) -> [u64; 4] {
        let mut mask = [0; 4];
        mask[(Self::EVENT / 64) as usize] = 1 << (Self::EVENT % 64);
        mask
    }
}

/// The M5 meanings of fields in the common configuration record (S `ConfigProfile`): soft faults
/// enabled, the extended flag, and for a bound tiling queue the parameter manager's slot in
/// place of the paired fragment QID.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct ConfigProfile {
    pub(crate) soft_fault_enabled: bool,
    pub(crate) extended: bool,
    pub(crate) parameter_manager: Option<u32>,
}

/// The configuration record of `args` with the M5 meanings of `profile`.
pub(crate) fn config(args: &QueueConfigArgs, profile: ConfigProfile) -> Result<QueueConfig> {
    if profile.parameter_manager.is_some() && !matches!(args.target, QueueConfigTarget::Tiling(_)) {
        return Err(EINVAL);
    }
    let mut out = QueueConfig::new(args)?;
    out.unk_66 = u16::from(profile.soft_fault_enabled);
    out.unk_68[2..6].copy_from_slice(&u32::from(profile.extended).to_le_bytes());
    if let Some(manager) = profile.parameter_manager {
        out.fragment_qid = manager;
    }
    Ok(out)
}

/// A queue transaction on the M5 host command port: the configuration of each queue at its first
/// use, then the advances that make the new kicks runnable. Compute queues run alone; a render
/// pair configures and advances its fragment queue, then its tiling queue.
#[derive(Clone, Copy)]
pub(crate) struct HardwareSubmission {
    writes: [(usize, u64); 16],
    count: usize,
}

impl HardwareSubmission {
    const TAG_PORT: usize = 0x4090;
    const DATA_PORT: usize = 0x4098;
    const VALID_LOW: usize = 0x21068;
    const VALID_HIGH: usize = 0x21070;
    const INVALID_LOW: usize = 0x21078;
    const INVALID_HIGH: usize = 0x21080;
    const CONFIG_TAG: u64 = 0x0020_0800_0002_1008;
    const QID_SHIFT: u32 = 44;
    /// The advance command's offset from the queue's configuration tag.
    const ADVANCE: u64 = 0x10;
    const ADDRESS_MASK: u64 = 0x0000_07ff_ffff_ffe0;

    fn push(&mut self, writes: &[(usize, u64)]) -> Result {
        let end = self.count.checked_add(writes.len()).ok_or(EINVAL)?;
        self.writes
            .get_mut(self.count..end)
            .ok_or(EINVAL)?
            .copy_from_slice(writes);
        self.count = end;
        Ok(())
    }

    /// Invalidates queue `qid` of `engine`, whose kick ring is at `va`, configures it at
    /// priority class `priority` and makes it valid; each validity mask is stored high before
    /// low.
    fn configure(&mut self, qid: u8, va: u64, engine: DataMaster, priority: u8) -> Result {
        let tag = Self::configuration_tag(qid)?;
        if priority >= Policy::PRIORITIES || va & !Self::ADDRESS_MASK != 0 {
            return Err(EINVAL);
        }
        let (low, high) = if qid < 64 {
            (1u64 << qid, 0)
        } else {
            (0, 1u64 << (qid - 64))
        };
        let completion = (3 * (4 - u64::from(priority)) + 0x3fe).wrapping_shl(54);
        let payload = ((engine as u64 + 1) << 60) | va | completion;
        self.push(&[
            (Self::INVALID_HIGH, high),
            (Self::INVALID_LOW, low),
            (Self::TAG_PORT, tag),
            (Self::DATA_PORT, payload),
            (Self::VALID_HIGH, high),
            (Self::VALID_LOW, low),
        ])
    }

    fn advance(&mut self, qid: u8) -> Result {
        let tag = Self::configuration_tag(qid)?;
        self.push(&[(Self::TAG_PORT, tag + Self::ADVANCE), (Self::DATA_PORT, 1)])
    }

    /// The transaction of compute queue `qid`, whose kick ring is at `va`, at priority class
    /// `priority`, configured first with `configure`.
    pub(crate) fn compute(qid: u8, va: u64, priority: u8, configure: bool) -> Result<Self> {
        let mut out = Self {
            writes: [(0, 0); 16],
            count: 0,
        };
        if configure {
            out.configure(qid, va, DataMaster::Compute, priority)?;
        }
        out.advance(qid)?;
        Ok(out)
    }

    /// The transaction of a render pair, `(qid, kick ring)` of its fragment and tiling queues,
    /// both configured first with `configure` (S `HardwareSubmission::new`, `advance_pair`).
    pub(crate) fn render(
        fragment: (u8, u64),
        tiling: (u8, u64),
        priority: u8,
        configure: bool,
    ) -> Result<Self> {
        if fragment.0 == tiling.0 {
            return Err(EINVAL);
        }
        let mut out = Self {
            writes: [(0, 0); 16],
            count: 0,
        };
        if configure {
            out.configure(fragment.0, fragment.1, DataMaster::Fragment, priority)?;
            out.configure(tiling.0, tiling.1, DataMaster::Tiling, priority)?;
        }
        out.advance(fragment.0)?;
        out.advance(tiling.0)?;
        Ok(out)
    }

    /// The tag-port value selecting queue `qid`'s configuration word.
    pub(crate) fn configuration_tag(qid: u8) -> Result<u64> {
        if qid > QID_MAX {
            return Err(EINVAL);
        }
        Ok(Self::CONFIG_TAG | (u64::from(qid) << Self::QID_SHIFT))
    }

    /// The queue configuration payload a configuring transaction stores, `None` for an advance.
    pub(crate) fn configuration(&self) -> Option<u64> {
        (self.count == 8).then_some(self.writes[3].1)
    }

    /// The 64-bit register writes, in order.
    pub(crate) fn writes(&self) -> &[(usize, u64)] {
        &self.writes[..self.count]
    }
}
