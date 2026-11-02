// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 compute channels: one hardware compute queue (QID) and its graph, built on the M5 queue,
//! descriptor and pool encodings ([`crate::g17::fw::m5`]) and published through
//! [`Port::publish_m5_compute`]. DRM compute submission and the arithmetic qualification
//! (`g17/m5_test.rs`) both run work through a [`Channel`]: one construction, one installation,
//! one publication transaction ([`Channel::publish`]) and one retirement predicate
//! ([`Channel::retire`]).
//!
//! An installed lane (QID, kick ring, graph, descriptors, support page and pool) stays with its
//! firmware session: a channel binds it
//! to one logical queue, parks it once that queue closed, the lane is idle and its context's
//! scheduler release was consumed ([`Channel::park`]), and a later queue adopts it
//! ([`Channel::adopt`]), reconfiguring the hardware queue at its first kick. Kick timestamps and
//! stamps continue across owners. A quarantined lane is not reused, and a failed kick keeps its
//! command pinned, until a witness ends the quarantine: a settled failure's exact drain returns
//! the lane as it is, and a killed context's exact drain recycles it with fresh backing
//! ([`Channel::recycle_killed`]), as Neo's compute queue is recycled
//! (`queue::compute::Queue::recycle_killed`).
//!
//! Retirement is exact and strict: the completion record of this queue, kick and
//! descriptor, the queue's stamp covering the kick, the inner consumer past the kick's items, the
//! pool's completed count at the kick's target, the work-state node drained and the record's end
//! time equal to the descriptor's, with recovery closed. A kick may retire across a firmware
//! restart, as on Neo (`retirement::poll`'s stamp rule): the restart's recovery table names
//! every kick it interrupted, and recovery marks those (`Channel::note_interrupted`) so that
//! they fail however their witnesses read. There is no aged-record or recordless retirement. These
//! witnesses prove that the firmware retired the kick, not that every workgroup ran: dispatches
//! that lost workgroups retired with all of them.

use super::compute::{awaits_witness, exit, Failure, GRAPH_SIZE, ITEMS, POINTERS};
use super::pool_state::{PoolPhase, PoolState};
use crate::g17::{
    context::{Context, WorkStateLease},
    freelist::{M5Backing, M5Pool},
    fw::{
        self,
        kick::{KickArgs, KickQos, KickTimestamp},
        m5::{
            compute::{self, Descriptor, DescriptorArgs, Gpu, RegisterArgs, Resources},
            queue::{HardwareSubmission, Notification},
        },
        queue::*,
        variant::PointerLayout,
    },
    initdata::InitData,
    job::Packet,
    kick,
    object::{Allocator, CpuMap, KernelObject},
    port::Port,
    qos,
    queue::compute::retirement::{self, Batch, Cursors, Observation, Recordless, Ticket},
    recovery,
    status::VmStatus,
    vm_layout::Domain,
};
use crate::{driver::AsahiDrmDevice, mmu};
use core::sync::atomic::{fence, AtomicBool, Ordering};
use kernel::{
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta},
};

const PAGE: usize = mmu::UAT_PGSZ;
/// Kicks a channel holds in flight: the retirement batch's bound per QID. A descriptor and a
/// notification slot are reused only after the kick that last used them retired.
pub(crate) const DEPTH: usize = retirement::IN_FLIGHT;
const _: () = assert!(DEPTH <= fw::kick::KICK_SLOTS);

/// Queue graph: the graph ([`super::compute::initialize_m5_graph`]), then the records its items
/// name.
const GRAPH_BYTES: usize = 0x10000;
/// Initial queue configuration.
const CONFIG: usize = GRAPH_SIZE;
/// Context and pool update of the first kick.
const UPDATE: usize = 0x8200;
/// Current-kick notifications, one per in-flight slot.
const ANNOUNCES: usize = 0x9000;
const ANNOUNCE_STRIDE: usize = 0x40;
const _: () = assert!(ANNOUNCES + DEPTH * ANNOUNCE_STRIDE <= GRAPH_BYTES);
/// Descriptor ring: one descriptor per in-flight slot.
/// The firmware reaches past the descriptor's described prefix: with a 0x1000-byte stride the
/// last slot's kick ran (its outputs exact) but never completed, at the ring's guard page,
/// though a retired descriptor holds nothing past +0xfec. The qualification's descriptor
/// had 0x8000 bytes to itself before its usage record.
const DESCRIPTOR_BYTES: usize = 0x8000;
const _: () = assert!(DESCRIPTOR_BYTES >= compute::DESCRIPTOR_PREFIX_BYTES);
/// Firmware start and end timestamps in a descriptor.
const DESCRIPTOR_TIMES: usize = 0xf2c;
/// Support page: the completion stamps and status words, then the firmware usage record.
const STAMPS: usize = 0;
const STATUS: usize = 0x80;
const USAGE: usize = 0x1000;
const SUPPORT_BYTES: usize = 0x4000;
/// Polls of 1 ms for the installation to be consumed.
const INSTALL_POLLS: u32 = 2000;
/// How long an installation may take to be consumed, polled or awaited.
pub(crate) const INSTALL_WAIT_MS: u32 = INSTALL_POLLS;

/// What a channel uses of its firmware session, borrowed under the device mutex.
pub(crate) struct Host<'a> {
    pub(crate) init: &'a InitData,
    pub(crate) port: &'a Port,
    pub(crate) qids: &'a mut kick::QueueIds,
    pub(crate) crashed: &'a AtomicBool,
}

/// What one kick runs: the user's CDM stream (its first span) and its planned resources.
#[derive(Clone, Copy)]
pub(crate) struct Dispatch {
    pub(crate) cdm_va: u64,
    /// The stream's final word: its exclusive end minus four.
    pub(crate) cdm_end_va: u64,
    pub(crate) resources: Resources,
}

impl Dispatch {
    /// The dispatch of a validated M5 compute command, as every DRM publication path takes it.
    pub(crate) fn of(command: &crate::g17::command::Validated) -> Result<Self> {
        match command {
            crate::g17::command::Validated::Compute {
                cdm_va,
                cdm_end_va,
                resources: Some(resources),
                ..
            } => Ok(Self {
                cdm_va: *cdm_va,
                cdm_end_va: *cdm_end_va,
                resources: *resources,
            }),
            _ => Err(EINVAL),
        }
    }
}

/// A grown pool's old backing and the client's old view of it ([`grow_pool`]), dropped off the
/// device mutex: the view first. With the blocks it held and holds now.
pub(crate) struct Grown {
    _view: Option<mmu::KernelMapping>,
    _backing: Option<M5Backing>,
    pub(crate) from: usize,
    pub(crate) to: usize,
}

/// One M5 queue's private pool as a growth sees it: a compute lane's ([`Channel::grow`]) or a
/// render pair's (`m5_render::Pair::grow`), the pool's registration, and the binding's client
/// views (in `vm`) and quarantine.
pub(crate) struct PoolSite<'a> {
    /// The QID whose page-pool slot the pool takes (a pair's tiling queue).
    pub(crate) qid: u8,
    pub(crate) pool: &'a mut M5Pool,
    pub(crate) state: &'a mut PoolState,
    pub(crate) vm: &'a mmu::Vm,
    pub(crate) aliases: &'a mut KVec<mmu::KernelMapping>,
    pub(crate) quarantine: &'a mut Option<Error>,
}

/// Whether a command whose plan needs `blocks` must back `pool` first ([`grow_pool`]): it is
/// smaller, vacant, or being released.
pub(crate) fn needs_pool(pool: &M5Pool, state: &PoolState, blocks: usize) -> bool {
    state.phase() != PoolPhase::Populated || blocks > pool.blocks()
}

/// The one growth rule of an M5 private pool, a compute lane's or a render pair's: backs the
/// pool for a command whose plan needs `blocks` (at most `maximum`, the inventory; at least
/// `minimum`, a fresh pool's) before that command is published, through the release
/// transaction of a Neo compute queue's growth (`queue::compute::Queue::begin_growth`,
/// `finish_growth`). A kick queue whose private-memory pool changed is reconfigured at its next
/// submission, and only while it is empty. So: EAGAIN until the queue is `idle`
/// (asked only of a backed pool, with the pool to read its counts); then the pool's
/// registration is released (FreeListRelease, announced through `notify`) and EAGAIN again until
/// the firmware consumed it, the publication loop waiting on the event worker's progress, as
/// Neo's growth does. Once the pool is vacant, fresh backing and its client view are built
/// (ENOMEM refuses this command only; the pool stays vacant), installed with its page-pool row,
/// and the next publication registers the pool with its configuration update, as an adopted
/// queue's first does. A failure after the release quarantines the queue. Returns the old
/// backing and view, which the caller drops off the device mutex.
pub(crate) fn grow_pool(
    alloc: &Allocator<'_>,
    init: &InitData,
    notify: impl FnOnce() -> Result,
    site: PoolSite<'_>,
    idle: impl FnOnce(&M5Pool) -> Result<bool>,
    blocks: usize,
    (minimum, maximum): (usize, usize),
) -> Result<Option<Grown>> {
    let PoolSite {
        qid,
        pool,
        state,
        vm,
        aliases,
        quarantine,
    } = site;
    if blocks > maximum {
        return Err(EINVAL);
    }
    if !needs_pool(pool, state, blocks) {
        return Ok(None);
    }
    if state.phase() == PoolPhase::Populated {
        if !idle(pool)? {
            return Err(EAGAIN);
        }
        if state.release(init, pool.release_record(1))? {
            notify()?;
            return Err(EAGAIN);
        }
    }
    fence(Ordering::Acquire);
    let consumer = init.control_consumer()?.load(Ordering::Relaxed);
    let producer = init.control_producer()?.load(Ordering::Relaxed);
    state.observe(consumer, producer)?;
    if state.phase() != PoolPhase::Vacant {
        return Err(EAGAIN);
    }
    let target = blocks.max(minimum);
    let geometry =
        fw::m5::pool::PoolGeometry::of(target, pool.block_bytes()).map_err(|_| EINVAL)?;
    let no_memory = |error: Error| if error == EINVAL { error } else { ENOMEM };
    let fresh = M5Backing::new(alloc, qid, geometry).map_err(no_memory)?;
    let view = fresh.map_client(vm).map_err(no_memory)?;
    aliases.reserve(1, GFP_KERNEL)?;
    let old_va = pool.backing_va().ok();
    let from = pool.blocks();
    let result = (|| {
        let old = pool.rearm(fresh)?;
        init.install_m5_pool(pool.id(), pool.descriptor()?)?;
        pool.publish_to_contexts()?;
        view.publish_to_contexts();
        Ok(old)
    })();
    let old = match result {
        Ok(old) => old,
        Err(error) => {
            *quarantine = Some(error);
            return Err(error);
        }
    };
    let index = old_va.and_then(|va| aliases.iter().position(|alias| alias.iova() == va));
    let old_view = match index {
        Some(index) => Some(core::mem::replace(&mut aliases[index], view)),
        None => {
            aliases.push_within_capacity(view).map_err(|_| ENOMEM)?;
            None
        }
    };
    state.populate();
    Ok(Some(Grown {
        _view: old_view,
        _backing: old,
        from,
        to: target,
    }))
}

/// A kick in flight.
struct Active {
    ticket: Ticket,
    /// Its descriptor and notification slot.
    slot: usize,
    /// The pool's submission count that covers this kick.
    pool_target: u64,
    work: Arc<WorkStateLease>,
    /// The DRM command; `None` for the qualification's own kicks.
    packet: Option<Arc<Packet>>,
    observed: Option<Observation>,
    /// Its QoS publication is open (`Port::qos_publish` succeeded): its retirement, or its
    /// failure's drain, completes it, as for a Neo compute command (`qos_pending`).
    qos: bool,
    /// Its publication order across both engines (`Port::next_publication`), for cross-VM
    /// timeout attribution.
    published: u64,
}

/// A kick just published.
pub(crate) struct Published {
    pub(crate) ticket: Ticket,
    pub(crate) pool_target: u64,
    /// The outer work ring's producer after this kick.
    pub(crate) outer: u32,
}

/// One retirement of [`Channel::retire`]: the kick, its command and its result.
pub(crate) struct Retired {
    pub(crate) ticket: Ticket,
    pub(crate) packet: Option<Arc<Packet>>,
    pub(crate) result: Result<[u64; 2]>,
    /// A failed kick that drained: its error (`result`) is reported again with the release, so
    /// that a fence whose failure never reached it signals that error, not success.
    pub(crate) drained: bool,
}

/// One M5 hardware queue: its graph ([`super::compute::initialize_m5_graph`]: the queue record,
/// cursors, job-list head and item ring, then the installation configuration, the first kick's
/// update and the kick notifications) and its kick ring. A compute channel has one; a render
/// pair ([`super::m5_render::Pair`]) a tiling and a fragment lane.
pub(crate) struct Lane {
    pub(crate) id: kick::Id,
    pub(crate) graph: KernelObject,
    pub(crate) kick: kick::Queue,
    /// Offset of the item-ring producer in the pointer block of this firmware build.
    producer: usize,
}

impl Lane {
    /// Builds the lane of QID `id` for `context`, pushing its client aliases to `aliases`.
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        context: &Context,
        id: kick::Id,
        pointers: &PointerLayout,
        aliases: &mut KVec<mmu::KernelMapping>,
    ) -> Result<Self> {
        let mut kick = kick::Queue::lower(alloc, id)?;
        aliases.push(kick.map_client(context.vm())?, GFP_KERNEL)?;
        let mut graph = alloc.kernel(
            GRAPH_BYTES,
            PAGE as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
            CpuMap::WriteCombined,
        )?;
        super::compute::initialize_m5_graph(&mut graph, context, pointers)?;
        Ok(Self {
            id,
            graph,
            kick,
            producer: pointers.producer,
        })
    }

    pub(crate) fn qid(&self) -> u8 {
        self.id.qid()
    }

    /// Rewrites the record's host-owned words for `context` ([`QueueRecord::set_owner`]). The
    /// caller proved the lane idle with its previous owner's scheduler release consumed.
    pub(crate) fn set_owner(&mut self, context: &Context) -> Result {
        let pointer = self
            .graph
            .pointer(0, core::mem::size_of::<fw::m5::queue::QueueRecord>())?
            .cast::<fw::m5::queue::QueueRecord>();
        // SAFETY: The checked packed record (alignment 1, integer fields only) stays mapped
        // with the graph; the device mutex and the idle proof exclude any other writer.
        let result = unsafe {
            (*pointer).set_owner(
                context.work_head_va(),
                context.scheduler_va(),
                context.owner_pid(),
                context.policy(),
            )
        };
        fence(Ordering::SeqCst);
        result
    }

    fn word(&self, offset: usize) -> Result<u32> {
        Ok(self.graph.word(offset)?.load(Ordering::Acquire))
    }

    /// The inner item ring's cursors.
    pub(crate) fn cursors(&self) -> Result<Cursors> {
        Ok(Cursors {
            producer: self.word(POINTERS + self.producer)?,
            consumer: self.word(POINTERS)?,
            count: ITEM_RING_ENTRIES,
        })
    }

    /// Stages the installation: `config` as the first item.
    pub(crate) fn stage_install(&mut self, config: QueueConfig) -> Result {
        self.graph.write(CONFIG, config)?;
        self.graph.write(ITEMS, self.graph.gpu_va() + CONFIG as u64)
    }

    /// Stages a reinstallation: `config` as item `producer`, the installation's record reused (its
    /// item was consumed long ago). Returns the producer after it.
    fn stage_reinstall(&mut self, producer: u32, config: QueueConfig) -> Result<u32> {
        self.graph.write(CONFIG, config)?;
        self.graph.write(
            ITEMS + producer as usize * 8,
            self.graph.gpu_va() + CONFIG as u64,
        )?;
        Ok((producer + 1) % ITEM_RING_ENTRIES)
    }

    /// The firmware finished every item up to `target` and holds nothing else: `done`, its own
    /// cursor (+0x30) and the producer all stand there, the host's idle condition
    /// ([`PointerBlock`]).
    pub(crate) fn idle_at(&self, target: u32) -> Result<bool> {
        Ok(self.word(POINTERS)? == target
            && self.word(POINTERS + 0x30)? == target
            && self.word(POINTERS + self.producer)? == target)
    }

    /// A killed queue's item ring restarts empty at the firmware's consumer cursor, as Neo's
    /// (`queue::compute` `Graph::reset_items`): every item slot is cleared and the producer set to
    /// the consumer. Only once the ring is idle at its producer with nothing pending.
    fn reset_items(&mut self) -> Result {
        let cursors = self.cursors()?;
        if cursors.count != ITEM_RING_ENTRIES || cursors.consumer >= cursors.count {
            return Err(EIO);
        }
        for index in 0..ITEM_RING_ENTRIES as usize {
            self.graph
                .dword(ITEMS + index * 8)?
                .store(0, Ordering::Relaxed);
        }
        fence(Ordering::SeqCst);
        self.graph
            .word(POINTERS + self.producer)?
            .store(cursors.consumer, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Waits until `installed` holds for every lane's installation item.
    pub(crate) fn await_install(
        crashed: &AtomicBool,
        lanes: &[&Lane],
        installed: impl Fn(&Lane) -> Result<bool>,
    ) -> Result {
        for _ in 0..INSTALL_POLLS {
            if crashed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            let mut done = true;
            for lane in lanes {
                done &= installed(lane)?;
            }
            if done {
                return Ok(());
            }
            fsleep(Delta::from_millis(1));
        }
        Err(ETIMEDOUT)
    }

    /// Writes the records of a kick from item `producer` on: `update`, the first kick's
    /// configuration, and the notification of `timestamp` in notification slot `slot`. Returns
    /// the producer after them. Nothing is visible until the producer moves.
    pub(crate) fn stage_kick(
        &mut self,
        producer: u32,
        update: Option<QueueConfig>,
        slot: usize,
        timestamp: KickTimestamp,
    ) -> Result<u32> {
        let announce = ANNOUNCES + slot * ANNOUNCE_STRIDE;
        if slot >= DEPTH {
            return Err(EINVAL);
        }
        self.graph
            .write(announce, Notification::new(self.qid(), timestamp)?)?;
        let mut target = producer;
        let mut records = [0u64; 2];
        let mut count = 0;
        if let Some(update) = update {
            self.graph.write(UPDATE, update)?;
            records[0] = self.graph.gpu_va() + UPDATE as u64;
            count = 1;
        }
        records[count] = self.graph.gpu_va() + announce as u64;
        count += 1;
        for &record in &records[..count] {
            self.graph.write(ITEMS + target as usize * 8, record)?;
            target = (target + 1) % ITEM_RING_ENTRIES;
        }
        Ok(target)
    }

    /// Moves the inner producer to `producer`.
    pub(crate) fn set_producer(&self, producer: u32) -> Result {
        fence(Ordering::SeqCst);
        self.graph
            .word(POINTERS + self.producer)?
            .store(producer, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// The outer work slot naming this queue's items up to `producer`: M5 routes every engine's
    /// pointer list through the common compute channel.
    pub(crate) fn work_slot(&self, producer: u32, first: bool) -> Result<fw::channels::WorkSlot> {
        fw::channels::WorkSlot::new(
            DataMaster::Compute,
            self.graph.gpu_va(),
            self.qid(),
            producer.try_into()?,
            first,
        )
    }

    /// Exposes the inner items up to `producer` through the claimed outer slot, then runs
    /// `submission` (a kick's) through the host command port. Returns the outer producer.
    pub(crate) fn expose(
        &self,
        port: &Port,
        claim: crate::g17::port::OuterClaim<'_>,
        producer: u32,
        first: bool,
        submission: Option<&HardwareSubmission>,
    ) -> Result<u32> {
        self.set_producer(producer)?;
        port.publish_m5_compute(claim, &self.work_slot(producer, first)?, submission)
    }
}

/// One M5 hardware compute queue: an installed lane (its QID, kick ring, graph, descriptors,
/// support page, pool, operand and preemption storage), bound to one logical queue's context.
/// The lane outlives the binding ([`Self::park`], [`Self::adopt`]). Field order drops every
/// alias before its backing.
pub(crate) struct Channel {
    /// The client aliases of the binding: in the owner's VM.
    aliases: KVec<mmu::KernelMapping>,
    context: Arc<Context>,
    owner: u64,
    /// The scheduler page and work storage of the lane's previous owner, which its record named
    /// until this binding rewrote it.
    _previous_pages: Option<crate::g17::context::FirmwarePages>,
    /// Set by an adoption until the binding's first kick: whether the class (and so the outer
    /// work ring) changed.
    rebound: Option<bool>,
    /// An adopted recycled lane's reinstallation ([`Self::reinstall`]).
    reinstall: Reinstall,
    /// The class whose outer work ring last carried this queue's first slot.
    class: u8,
    /// Set once this binding's client views are unmapped, after it parks.
    unmapped: Unmapped,
    gpu: Gpu,
    /// The descriptors' canonical low alias (the kernel's lower VM); it stays with the lane.
    descriptors_lower: mmu::KernelMapping,
    lane: Lane,
    descriptors: KernelObject,
    descriptors_low: u64,
    support: KernelObject,
    pool: M5Pool,
    operand: KernelObject,
    preempt: KernelObject,
    usage: KernelObject,
    /// The client views of the preemption buffer and the usage (robustness) page.
    preempt_va: u64,
    usage_va: u64,
    installed: bool,
    ordinal: u32,
    /// The pool as the firmware sees it, carried to the parked lane and the next binding.
    pool_state: PoolState,
    active: KVec<Active>,
    /// Set once a kick failed or the channel's state became ambiguous: nothing more is published,
    /// and every failed kick stays in `failed`, pinning its command. With `awaiting_witness` these
    /// are Neo's quarantine fields (`queue::compute::Queue`), under their names so the lane folds
    /// into that queue (spec 2026-10-31-m5-registry-fold §7.2, L1-L2). Neo's separate
    /// `quarantined` flag is left out: every M5 quarantine has its error, so it would only repeat
    /// `is_some()`.
    quarantine_error: Option<Error>,
    failed: KVec<Active>,
    /// The logical queue closed; the channel stays installed.
    closed: bool,
    /// The firmware reported a failed kick of this queue.
    faulted: bool,
    /// The quarantine is a settled failure (the firmware faulted a kick or failed it across a
    /// restart, both followed by recovery): the channel's state is not in doubt, so the failed
    /// kicks' exact retirement ([`Self::drained`]) is the drain witness that releases them and ends
    /// the quarantine, as a Neo compute queue's quarantine ends once its ring is proved retired
    /// (`retirement::quarantined_retired`). A broken retirement witness, a timeout or the VM's
    /// failure keeps the quarantine (`Registry::fail_pair` says why), as does the context's release
    /// consumed before the drain ([`Self::end_witness_wait`]).
    awaiting_witness: bool,
    /// The oldest kick finished but is not retirable yet (`Self::retire`): its record or inner
    /// item is visible ahead of the stamp, the pool counter or the work node. Its completion
    /// event has been consumed, so the event worker polls until it retires, as for a Neo
    /// compute queue (`queue::compute::Queue::retirement_polling`).
    retire_pending: bool,
    /// The kill of this lane's closed context, published because its failed kicks would not
    /// drain on their own ([`Self::kill_candidate`]): Neo's compute-exit transaction
    /// (`queue::compute::exit::Transaction`).
    exit: Option<exit::Transaction>,
    /// The kill was refused (its scheduler page named a queue outside the context, or it was
    /// inactive): the lane keeps its quarantine as before.
    kill_refused: bool,
    /// When the kill was settled ([`Self::settle_kill`]): the failed kicks' drain is awaited from
    /// then on.
    killed_at: Option<u64>,
    /// The lane was recycled after its context's kill ([`Self::recycle_killed`]): its pool is
    /// released and vacant before any owner adopts it, as for a recycled Neo queue
    /// (`queue::compute::Queue::needs_fresh_backing`).
    needs_fresh_backing: bool,
}

/// The reinstallation of a lane recycled after a kill, at its adoption ([`Channel::reinstall`]).
#[derive(Clone, Copy, PartialEq, Eq)]
enum Reinstall {
    /// Not needed: a fresh channel, or a lane adopted as it parked.
    None,
    /// Due before the binding's first kick.
    Due,
    /// Published; consumed once the firmware's consumer reaches this producer.
    Published(u32),
}

/// Maps a lane's client-visible storage into `vm`: the kick ring (unless `Lane::new` did), the
/// descriptors and operand state at their canonical addresses, the pool, and client-private
/// views of the preemption buffer and the usage page. Returns those two views' addresses.
#[expect(
    clippy::too_many_arguments,
    reason = "One argument per lane object a binding maps."
)]
fn client_aliases(
    alloc: &Allocator<'_>,
    vm: &mmu::Vm,
    aliases: &mut KVec<mmu::KernelMapping>,
    kick: Option<&mut kick::Queue>,
    (descriptors, descriptors_low): (&KernelObject, u64),
    pool: &M5Pool,
    operand: &KernelObject,
    preempt: &KernelObject,
    usage: &KernelObject,
) -> Result<(u64, u64)> {
    let shared = mmu::PROT_GPU_SHARED_RW;
    if let Some(kick) = kick {
        aliases.push(kick.map_client(vm)?, GFP_KERNEL)?;
    }
    aliases.push(
        descriptors.map_alias(vm, descriptors_low, shared)?,
        GFP_KERNEL,
    )?;
    if !pool.vacant() {
        aliases.push(pool.map_client(vm)?, GFP_KERNEL)?;
    }
    aliases.push(operand.map_alias(vm, operand.gpu_va(), shared)?, GFP_KERNEL)?;
    // Kernel-owned buffers the GPU writes in the client's context: client-private views.
    let private = alloc.domain(Domain::Private);
    let preempt_alias = preempt.alias_in(vm, private.clone(), PAGE as u64, shared)?;
    let preempt_va = preempt_alias.iova();
    aliases.push(preempt_alias, GFP_KERNEL)?;
    let usage_alias = usage.alias_in(vm, private, PAGE as u64, shared)?;
    let usage_va = usage_alias.iova();
    aliases.push(usage_alias, GFP_KERNEL)?;
    Ok((preempt_va, usage_va))
}

/// An installed compute lane without an owner: everything the
/// firmware's installed queue still names, and the previous owner's scheduler page and work
/// storage its record names until the next owner rewrites it. Never dropped while the session
/// runs: its QID stays configured with this ring.
pub(crate) struct ParkedLane {
    gpu: Gpu,
    descriptors_lower: mmu::KernelMapping,
    lane: Lane,
    descriptors: KernelObject,
    descriptors_low: u64,
    support: KernelObject,
    pool: M5Pool,
    operand: KernelObject,
    preempt: KernelObject,
    usage: KernelObject,
    class: u8,
    pages: crate::g17::context::FirmwarePages,
    unmapped: Unmapped,
    pool_state: PoolState,
    /// When it parked: the vacate pass releases the least recently parked pools first.
    parked_at: u64,
    /// Recycled after a kill ([`Channel::recycle_killed`]): adoptable only once its pool is
    /// vacant, and vacated whatever the warm cap.
    needs_fresh_backing: bool,
}

impl ParkedLane {
    pub(crate) fn qid(&self) -> u8 {
        self.lane.qid()
    }
    /// Its previous binding's client views are unmapped (a new owner in the same VM may map
    /// the lane at the same addresses), and no release of its pool is in flight.
    /// A recycled lane's pool must be vacant too: its next owner's first command backs it afresh
    /// (as Neo's `queue::compute::Queue::reusable`).
    pub(crate) fn adoptable(&self) -> bool {
        self.unmapped.done()
            && !self.pool_state.pending()
            && (!self.needs_fresh_backing || self.pool_state.phase() == PoolPhase::Vacant)
    }
    /// Recycled after a kill and still holding the backing the killed work used: the vacate pass
    /// releases it first and beyond the cap (`Registry::vacate_idle_pools`).
    pub(crate) fn needs_fresh_backing(&self) -> bool {
        self.needs_fresh_backing
    }
    /// Its pool is backed and kept: an adoption needs no fresh backing.
    pub(crate) fn backed(&self) -> bool {
        self.pool_state.phase() == PoolPhase::Populated && !self.pool.vacant()
    }
    /// An idle pool the vacate pass may give back (`Registry::vacate_idle_pools`), as for a
    /// retained Neo compute queue (`queue::compute::Queue::vacate_candidate`).
    pub(crate) fn vacate_candidate(&self) -> bool {
        self.backed() && self.unmapped.done()
    }
    /// When the lane parked.
    pub(crate) fn released_at(&self) -> u64 {
        self.parked_at
    }
    /// Releases this parked lane's pool (the caller holds the device mutex and announces a
    /// published record): the backing is freed once the firmware consumed the release
    /// ([`Self::observe_vacancy`]). The lane, its QID, ring and stamps stay installed; the next
    /// owner's first command backs the pool for its plan.
    pub(crate) fn vacate(&mut self, init: &InitData) -> Result<bool> {
        if !self.vacate_candidate() {
            return Err(EBUSY);
        }
        // Generation 1, as every binding's configuration publishes it.
        self.pool_state.release(init, self.pool.release_record(1))
    }
    /// The backing of a vacant pool no client maps any more, once the firmware consumed its
    /// release; the caller frees it off the device mutex.
    pub(crate) fn observe_vacancy(
        &mut self,
        consumer: u32,
        producer: u32,
    ) -> Result<Option<M5Backing>> {
        self.pool_state.observe(consumer, producer)?;
        if self.pool_state.phase() != PoolPhase::Vacant || !self.unmapped.done() {
            return Ok(None);
        }
        Ok(self.pool.vacate())
    }
    pub(crate) fn pool_release_pending(&self) -> bool {
        self.pool_state.pending()
    }
    pub(crate) fn pool_release_stalled(&self, now: u64) -> bool {
        self.pool_state.stalled(now)
    }
    /// The class whose outer ring last carried this lane.
    pub(crate) fn class(&self) -> u8 {
        self.class
    }
}

/// Whether a binding's client views are unmapped, shared by the binding and its parked lane:
/// the lane is adoptable only once they are, since a new owner in the same VM maps the lane at
/// the same addresses (the ring, command storage, pool and operand state sit at fixed VAs).
#[derive(Clone)]
pub(crate) struct Unmapped(Arc<AtomicBool>);

impl Unmapped {
    pub(crate) fn new() -> Result<Self> {
        Ok(Self(Arc::new(AtomicBool::new(false), GFP_KERNEL)?))
    }
    pub(crate) fn done(&self) -> bool {
        self.0.load(Ordering::Acquire)
    }
}

/// A parked lane's (or render pair's) former owner binding: its client views in that owner's
/// VM, a pair's storage there, then its context. Dropped off the device mutex; its lane becomes
/// adoptable once it is ([`ParkedLane::adoptable`]).
pub(crate) struct Binding {
    aliases: KVec<mmu::KernelMapping>,
    render: Option<KBox<super::m5_render::Storage>>,
    context: Option<Arc<Context>>,
    owner: u64,
    unmapped: Unmapped,
}

impl Binding {
    pub(crate) fn render(
        aliases: KVec<mmu::KernelMapping>,
        storage: KBox<super::m5_render::Storage>,
        context: Arc<Context>,
        owner: u64,
        unmapped: Unmapped,
    ) -> Self {
        Self {
            aliases,
            render: Some(storage),
            context: Some(context),
            owner,
            unmapped,
        }
    }

    pub(crate) fn owner(&self) -> u64 {
        self.owner
    }
}

impl Drop for Binding {
    fn drop(&mut self) {
        // Every view first (each unmaps as it drops), then the storage behind a pair's, then
        // the context; only then may a new owner map the lane.
        drop(core::mem::replace(&mut self.aliases, KVec::new()));
        drop(self.render.take());
        drop(self.context.take());
        self.unmapped.0.store(true, Ordering::Release);
    }
}

impl Channel {
    /// Builds the channel of QID `id` for `context` and maps it into the context's VM. Nothing is
    /// visible to the firmware yet, so a failure here frees everything; the caller cancels the
    /// unpublished QID. Boxed: a channel holds a dozen objects, and the boot-time qualification
    /// builds it deep in probe's stack.
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        id: kick::Id,
        owner: u64,
        gpu: Gpu,
        pointers: &PointerLayout,
    ) -> Result<KBox<Self>> {
        let vm = context.vm();
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let cpu = CpuMap::WriteCombined;
        let mut aliases = KVec::with_capacity(8, GFP_KERNEL)?;
        let lane = Lane::new(alloc, &context, id, pointers, &mut aliases)?;
        let descriptors = alloc.kernel(DEPTH * DESCRIPTOR_BYTES, PAGE as u64, prot, cpu)?;
        // The canonical low arena, as for every descriptor ring, and the same address in the VM.
        let low = descriptors.alias_in(
            alloc.uat.kernel_lower_vm(),
            alloc.domain(Domain::Canonical),
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
        )?;
        let descriptors_low = low.iova();
        let support = alloc.kernel(SUPPORT_BYTES, PAGE as u64, prot, cpu)?;
        let geometry = fw::m5::pool::PoolGeometry::of(gpu.pool_blocks, gpu.block_bytes())
            .map_err(|_| EINVAL)?;
        let pool = M5Pool::new(alloc, id.qid(), geometry, fw::m5::pool::Engine::Compute)?;
        let operand = alloc.lower(
            gpu.profile.operand.bytes,
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
            cpu,
        )?;
        let preempt = alloc.kernel(compute::PREEMPT_BYTES, PAGE as u64, prot, cpu)?;
        let usage = alloc.kernel(PAGE, PAGE as u64, prot, cpu)?;
        let (preempt_va, usage_va) = client_aliases(
            alloc,
            vm,
            &mut aliases,
            None,
            (&descriptors, descriptors_low),
            &pool,
            &operand,
            &preempt,
            &usage,
        )?;
        let channel = KBox::new(
            Self {
                aliases,
                context,
                owner,
                _previous_pages: None,
                unmapped: Unmapped::new()?,
                rebound: None,
                reinstall: Reinstall::None,
                class: u8::MAX,
                gpu,
                descriptors_lower: low,
                lane,
                descriptors,
                descriptors_low,
                support,
                pool,
                operand,
                preempt,
                usage,
                preempt_va,
                usage_va,
                installed: false,
                ordinal: 0,
                pool_state: PoolState::FRESH,
                active: KVec::with_capacity(DEPTH, GFP_KERNEL)?,
                quarantine_error: None,
                failed: KVec::new(),
                closed: false,
                faulted: false,
                awaiting_witness: false,
                retire_pending: false,
                exit: None,
                kill_refused: false,
                killed_at: None,
                needs_fresh_backing: false,
            },
            GFP_KERNEL,
        )?;
        channel.publish_mappings()?;
        Ok(channel)
    }

    /// Publishes the complete mapping set to every context naming its roots, before any record
    /// names it. The kick ring's own aliases are published when its first entry is installed.
    fn publish_mappings(&self) -> Result {
        self.descriptors_lower.publish_to_contexts();
        for alias in &self.aliases {
            alias.publish_to_contexts();
        }
        for object in [
            &self.lane.graph,
            &self.descriptors,
            &self.support,
            &self.operand,
            &self.preempt,
            &self.usage,
        ] {
            object.publish_to_contexts()?;
        }
        self.pool.publish_to_contexts()
    }

    /// Adopts parked lane `index` for `context`'s logical queue `owner`: maps
    /// the lane into the new VM and rewrites the record's host-owned words. The firmware's
    /// queue is reconfigured at the binding's first kick ([`Self::publish`]); the kick
    /// timestamp and the stamps continue. On failure the lane stays parked.
    pub(crate) fn adopt(
        alloc: &Allocator<'_>,
        parked: &mut KVec<ParkedLane>,
        index: usize,
        context: Arc<Context>,
        owner: u64,
    ) -> Result<KBox<Self>> {
        let lane = parked.get_mut(index).ok_or(EINVAL)?;
        if !lane.adoptable() {
            return Err(EAGAIN);
        }
        // A vacant pool's leftover backing (no registration, no client view) goes: the binding's
        // first command backs the pool for its plan ([`Self::grow`]).
        if lane.pool_state.phase() == PoolPhase::Vacant {
            drop(lane.pool.vacate());
        }
        let unmapped = Unmapped::new()?;
        let mut aliases = KVec::with_capacity(8, GFP_KERNEL)?;
        let (preempt_va, usage_va) = client_aliases(
            alloc,
            context.vm(),
            &mut aliases,
            Some(&mut lane.lane.kick),
            (&lane.descriptors, lane.descriptors_low),
            &lane.pool,
            &lane.operand,
            &lane.preempt,
            &lane.usage,
        )?;
        let active = KVec::with_capacity(DEPTH, GFP_KERNEL)?;
        let slot = KBox::<Self>::new_uninit(GFP_KERNEL)?;
        // The lane is idle and its owner's release consumed: no reader of the record remains.
        lane.lane.set_owner(&context)?;
        let lane = parked.remove(index).map_err(|_| EINVAL)?;
        let recycled = lane.needs_fresh_backing;
        if recycled {
            pr_info!(
                "G17 M5: lane {}: recycled after a kill, adopted by queue {} (context {})\n",
                lane.qid(),
                owner,
                context.id()
            );
        }
        let class_changed = lane.class != context.policy().priority();
        let channel = KBox::write(
            slot,
            Self {
                aliases,
                context,
                owner,
                _previous_pages: Some(lane.pages),
                unmapped,
                rebound: Some(class_changed),
                reinstall: if recycled {
                    Reinstall::Due
                } else {
                    Reinstall::None
                },
                class: lane.class,
                gpu: lane.gpu,
                descriptors_lower: lane.descriptors_lower,
                lane: lane.lane,
                descriptors: lane.descriptors,
                descriptors_low: lane.descriptors_low,
                support: lane.support,
                pool: lane.pool,
                operand: lane.operand,
                preempt: lane.preempt,
                usage: lane.usage,
                preempt_va,
                usage_va,
                installed: true,
                ordinal: 0,
                pool_state: lane.pool_state,
                active,
                quarantine_error: None,
                failed: KVec::new(),
                closed: false,
                faulted: false,
                awaiting_witness: false,
                retire_pending: false,
                exit: None,
                kill_refused: false,
                killed_at: None,
                needs_fresh_backing: false,
            },
        );
        if let Err(error) = channel.publish_mappings() {
            let (lane, _binding) = channel.park();
            parked.push_within_capacity(lane).map_err(|_| ENOMEM)?;
            return Err(error);
        }
        // The rewritten record names this context's scheduler page.
        channel.context.mark_published();
        Ok(channel)
    }

    /// The channel's logical queue closed and its lane is idle: no kick in flight or failed, the
    /// inner ring consumed to its producer, the pool's count complete and the stamp at the
    /// lane's last kick. A quarantined lane never parks.
    pub(crate) fn parkable(&self) -> Result<bool> {
        if !self.closed
            || !self.installed
            || self.quarantine_error.is_some()
            || self.faulted
            || !self.active.is_empty()
            || !self.failed.is_empty()
        {
            return Ok(false);
        }
        if !self.lane.idle_at(self.lane.cursors()?.producer)?
            || self.pool.completed_submissions()? != self.pool.submitted()?
        {
            return Ok(false);
        }
        let next = self.lane.kick.timestamp();
        Ok(next.get() == KickTimestamp::FIRST.get()
            || retirement::stamp_covers(self.stamp()?, next.prev().get()))
    }

    /// Logs the park witnesses ([`Self::parkable`]) one by one, for a stalled release.
    pub(crate) fn report_park(&self) {
        let next = self.lane.kick.timestamp();
        pr_warn!(
            "G17 M5: lane {}: closed {} installed {} quarantine {:?} faulted {} active {} failed {}, kill: due {} published {} settled {} refused {}, cursors {:x?}, idle {:?}, pool {:?}/{:?}, stamp {:x?} for kick {:#x}\n",
            self.qid(), self.closed, self.installed, self.quarantine_error, self.faulted, self.active.len(),
            self.failed.len(), self.kill_candidate(), self.exit.is_some(), self.killed_at.is_some(),
            self.kill_refused, self.lane.cursors(),
            self.lane.cursors().and_then(|cursors| self.lane.idle_at(cursors.producer)),
            self.pool.completed_submissions(), self.pool.submitted(), self.stamp(), next.prev().get(),
        );
    }

    /// The lane needs nothing more before its context's release: parkable, or quarantined with
    /// the firmware idle on its ring. A drain still awaited does not hold the release back, as a
    /// quarantined Neo queue does not (`Queue::idle_for_context` waits for the firmware, not for
    /// the witness): the healthy lanes of the context park, and this one parks only if it drained
    /// before the release was consumed ([`Self::end_witness_wait`]).
    /// A lane whose context is to be killed waits for the kill ([`Self::kill_holds_release`]).
    pub(crate) fn settled(&self) -> Result<bool> {
        if self.quarantine_error.is_none() {
            return self.parkable();
        }
        Ok(self.closed
            && self.lane.idle_at(self.lane.cursors()?.producer)?
            && !self.kill_holds_release(crate::g17::now_ns()))
    }

    /// The firmware may still run this closed lane's failed kicks: they are held (`failed`) with
    /// no drain awaited, as after a timeout or its VM's failure ([`Failure::Final`]), and an
    /// unreadable witness ([`Failure::Ambiguous`]). The firmware consumes a kick's items when it
    /// launches its work, so an idle ring does not say the work ended: a measured compute spin kept
    /// the GPU busy after its timeout. Its context is killed as a Neo queue's closed context with
    /// retained commands is (`Firmware::publish_compute_exit`).
    pub(crate) fn kill_candidate(&self) -> bool {
        self.closed
            && self.quarantine_error.is_some()
            && !self.awaiting_witness
            && !self.failed.is_empty()
            && self.exit.is_none()
            && !self.kill_refused
    }

    /// The context's scheduler release waits for its kill: due, published and not yet settled
    /// (for [`exit::POLL_NS`] at most), or settled with the drain awaited (as long again). Past
    /// those bounds the release goes as before the kill, and the lane stays quarantined
    /// ([`Self::end_witness_wait`]).
    fn kill_holds_release(&self, now: u64) -> bool {
        if self.kill_candidate() {
            return true;
        }
        match (&self.exit, self.killed_at) {
            (Some(exit), None) => {
                !exit.released() && now.saturating_sub(exit.published_at()) < exit::POLL_NS
            }
            (Some(_), Some(killed)) => {
                self.awaiting_witness && now.saturating_sub(killed) < exit::POLL_NS
            }
            (None, _) => false,
        }
    }

    /// The kill of this lane's context was published with `cookie`.
    pub(crate) fn record_kill(&mut self, cookie: u64, now: u64) {
        self.exit = Some(exit::Transaction::new(cookie, now));
    }

    /// The kill could not be published for this context; the lane stays quarantined.
    pub(crate) fn refuse_kill(&mut self) {
        self.kill_refused = true;
    }

    /// The kill's doorbell, retried until it rings once ([`exit::Transaction::announce`]).
    pub(crate) fn announce_kill(&mut self, now: u64, notify: impl FnOnce() -> Result) -> Result {
        match self.exit.as_mut() {
            Some(exit) => exit.announce(now, notify),
            None => Ok(()),
        }
    }

    /// The event worker polls while the kill awaits its acknowledgement, and while the settled
    /// kill's drain is awaited, each within [`exit::POLL_NS`].
    pub(crate) fn kill_polling(&self, now: u64) -> bool {
        self.exit.as_ref().is_some_and(|exit| exit.polling(now))
            || self.killed_at.is_some_and(|killed| {
                self.awaiting_witness && now.saturating_sub(killed) < exit::POLL_NS
            })
    }

    /// The firmware acknowledged the kill with `cookie` (event 12): only this kill's exact cookie.
    pub(crate) fn acknowledge_kill(&mut self, cookie: u64) -> bool {
        let qid = self.qid();
        self.exit
            .as_mut()
            .is_some_and(|exit| exit.acknowledge(cookie, qid))
    }

    /// Settles an acknowledged kill once recovery is closed (`recovery_closed`) and the context's
    /// scheduler page shows it killed (+0x34 == 1); until then it is re-read on later passes. The
    /// acknowledgement and the killed state say the firmware stopped the context, not that it
    /// retired the failed kicks: from here their exact drain ([`Self::drained`]) is awaited, as a
    /// settled failure's is, and it alone gives them up ([`Self::rehabilitate`]); the lane is then
    /// recycled ([`Self::recycle_killed`]).
    pub(crate) fn settle_kill(&mut self, recovery_closed: bool, now: u64) {
        let Some(transaction) = self.exit.as_mut() else {
            return;
        };
        if !transaction.settling() || !recovery_closed {
            return;
        }
        let Ok(state) = exit::State::read(&self.context) else {
            return;
        };
        if state.killed != 1 {
            transaction.mark_unproven();
            return;
        }
        transaction.mark_released();
        self.killed_at = Some(now.max(1));
        self.awaiting_witness = true;
        pr_info!(
            "G17 M5: lane {}: its context {} was killed; awaiting its {} failed kicks' drain\n",
            self.qid(),
            self.context.id(),
            self.failed.len()
        );
    }

    /// Splits a parked channel ([`Self::parkable`], its context's release consumed) into its
    /// lane and its binding. The lane keeps the context's scheduler page and work storage, which
    /// the record names until the next owner rewrites it.
    pub(crate) fn park(self: KBox<Self>) -> (ParkedLane, Binding) {
        let Self {
            aliases,
            context,
            owner,
            class,
            gpu,
            descriptors_lower,
            lane,
            descriptors,
            descriptors_low,
            support,
            pool,
            operand,
            preempt,
            usage,
            unmapped,
            pool_state,
            needs_fresh_backing,
            ..
        } = KBox::into_inner(self);
        let pages = context.retain_firmware_pages();
        (
            ParkedLane {
                gpu,
                descriptors_lower,
                lane,
                descriptors,
                descriptors_low,
                support,
                pool,
                operand,
                preempt,
                usage,
                class,
                pages,
                unmapped: unmapped.clone(),
                pool_state,
                parked_at: crate::g17::now_ns(),
                needs_fresh_backing,
            },
            Binding {
                aliases,
                render: None,
                context: Some(context),
                owner,
                unmapped,
            },
        )
    }

    /// An adopted lane whose class differs from its previous binding's, until its first kick.
    pub(crate) fn class_changed(&self) -> bool {
        self.rebound == Some(true)
    }

    /// The timestamp the lane's next kick takes.
    pub(crate) fn next_kick(&self) -> u64 {
        self.lane.kick.timestamp().get()
    }

    pub(crate) fn qid(&self) -> u8 {
        self.lane.qid()
    }
    /// The QID and the host identity that reserved it: the compute entry's reservation.
    pub(crate) fn id(&self) -> kick::Id {
        self.lane.id
    }
    pub(crate) fn gpu(&self) -> &Gpu {
        &self.gpu
    }
    pub(crate) fn owner(&self) -> u64 {
        self.owner
    }
    pub(crate) fn context(&self) -> &Arc<Context> {
        &self.context
    }
    pub(crate) fn installed(&self) -> bool {
        self.installed
    }
    pub(crate) fn in_flight(&self) -> bool {
        !self.active.is_empty()
    }
    /// Its logical queue is open.
    pub(crate) fn open(&self) -> bool {
        !self.closed
    }
    /// A settled failure's drain is awaited ([`Self::rehabilitate`]). The event worker does not
    /// poll for it: a quarantined channel has no kick in flight ([`Self::in_flight`]), as a
    /// quarantined Neo queue is not in flight (`Queue::in_flight`); each event pass checks it.
    pub(crate) fn awaiting_witness(&self) -> bool {
        self.awaiting_witness
    }
    /// The context's scheduler release was consumed while this quarantined channel still awaited
    /// its failed kicks' drain: the channel stays quarantined, leaked under the cap, its kicks
    /// pinned. The firmware was idle on its ring (`Self::settled`) and no longer names the
    /// context, and nothing else can witness the drain: on M5 neither a kill's acknowledgement nor
    /// the scheduler page's killed state proves the firmware finished reading (S
    /// `queue/compute/exit.rs` `settle_exit`). Returns whether a wait ended.
    pub(crate) fn end_witness_wait(&mut self) -> bool {
        if !self.awaiting_witness {
            return false;
        }
        self.awaiting_witness = false;
        pr_warn!("G17 M5: lane {}: its context was released before its {} failed kicks drained; the lane stays quarantined\n",
            self.qid(), self.failed.len());
        true
    }
    /// The oldest command in flight.
    pub(crate) fn oldest_packet(&self) -> Option<Arc<Packet>> {
        self.commands().next().map(|(_, packet)| packet.clone())
    }
    /// Room for another kick: installed, healthy, open and below [`DEPTH`].
    pub(crate) fn has_room(&self) -> bool {
        self.installed
            && self.quarantine_error.is_none()
            && !self.closed
            && self.active.len() < DEPTH
    }
    pub(crate) fn quarantined(&self) -> Option<Error> {
        self.quarantine_error
    }
    pub(crate) fn close(&mut self) {
        self.closed = true;
    }
    /// The firmware failed a kick of this queue: the next retirement pass fails the channel.
    pub(crate) fn note_fault(&mut self) {
        self.faulted = true;
    }
    /// Kicks published since installation.
    pub(crate) fn kicks(&self) -> u64 {
        self.lane.kick.timestamp().get() - KickTimestamp::FIRST.get()
    }
    /// The oldest DRM kick still holding the GPU, for cross-VM timeout attribution
    /// (`timeout::attribute`), as a Neo compute queue's (`queue::compute::Queue::timeout_head`):
    /// a failed kick awaiting its drain holds the GPU and is marked failed; a quarantine with no
    /// drain to await describes no live pipe order. The qualification's kicks carry no packet
    /// and take no part.
    pub(crate) fn timeout_head(&self) -> Option<crate::g17::timeout::Head> {
        if self.quarantine_error.is_some() && !self.awaiting_witness {
            return None;
        }
        Iterator::chain(self.failed.iter(), self.active.iter())
            .filter_map(|active| Some((active, active.packet.as_ref()?)))
            .min_by_key(|(active, _)| active.published)
            .map(|(active, packet)| crate::g17::timeout::Head {
                vm: packet.context.vm().id(),
                published: active.published,
                finished: active.observed.is_some(),
                failed: self.quarantine_error.is_some(),
            })
    }

    /// The watchdog of `packet` may classify this channel's work (Neo:
    /// `queue::compute::Queue::can_classify_timeout`): it is healthy and `packet` is in flight.
    pub(crate) fn can_classify_timeout(&self, packet: &Arc<Packet>) -> bool {
        self.quarantine_error.is_none()
            && self.active.iter().any(|active| {
                active
                    .packet
                    .as_ref()
                    .is_some_and(|own| Arc::ptr_eq(own, packet))
            })
    }

    /// The oldest kick is a published long job (ray traversal or AS build) whose completion
    /// record the driver has not seen yet (Neo: `queue::compute::Queue::long_job_running`).
    pub(crate) fn long_job_running(&self) -> bool {
        self.quarantine_error.is_none()
            && self.active.first().is_some_and(|active| {
                active.observed.is_none()
                    && active
                        .packet
                        .as_ref()
                        .is_some_and(|packet| packet.is_published() && packet.command.long_job())
            })
    }

    /// The firmware posts a completion (stamp) event naming this queue's QID for every kick,
    /// as for a Neo compute queue: measured on J775D, 2782 stamp events for 2782 kicks of one
    /// queue, and 48237 for 48237 kicks of eight, 97% of them retired by the pass of their own
    /// event. So the event worker polls a channel only while its oldest kick finished ahead of
    /// its retirement witnesses ([`Self::retire_pending`]), not while any kick is in flight.
    pub(crate) fn retirement_polling(&self) -> bool {
        self.quarantine_error.is_none() && self.retire_pending
    }

    /// The oldest kick's completion record was seen, or the firmware consumed its inner items
    /// (Neo: `queue::compute::Queue::front_finished`).
    fn front_finished(&self) -> bool {
        self.active.first().is_some_and(|active| {
            active.observed.is_some()
                || self
                    .cursors()
                    .and_then(|cursors| {
                        cursors.consumed(active.ticket.item_producer, active.ticket.item_count)
                    })
                    .unwrap_or(false)
        })
    }

    /// The DRM command `packet` is in flight on this channel, or failed on it.
    pub(crate) fn owns_packet(&self, packet: &Arc<Packet>) -> bool {
        Iterator::chain(self.active.iter(), self.failed.iter()).any(|active| {
            active
                .packet
                .as_ref()
                .is_some_and(|own| Arc::ptr_eq(own, packet))
        })
    }

    /// The DRM commands in flight, oldest first, for recovery attribution (`recovery::Queues`);
    /// the qualification's own kicks carry none and take no part.
    fn commands(&self) -> impl Iterator<Item = (&Ticket, &Arc<Packet>)> {
        self.active
            .iter()
            .filter_map(|active| Some((&active.ticket, active.packet.as_ref()?)))
    }
    pub(crate) fn oldest_spared(&self) -> Option<bool> {
        self.commands()
            .next()
            .map(|(_, packet)| packet.completion.spared())
    }
    /// A recovery record names this queue and the completion stamp of the kick it interrupted,
    /// `kick << 8` like the queue's stamp. Without a stamp match the oldest kick in flight is
    /// marked, as on Neo (`queue::compute::Queue::note_interrupted`).
    pub(crate) fn note_interrupted(&self, stamp: u32) -> recovery::Interrupted {
        let sequence = stamp >> 8;
        if let Some((_, packet)) = self
            .commands()
            .find(|(ticket, _)| (ticket.kick as u32) & 0xff_ffff == sequence)
        {
            packet.completion.note_interrupted();
            return recovery::Interrupted::Exact;
        }
        match self.commands().next() {
            Some((_, packet)) => {
                packet.completion.note_interrupted();
                recovery::Interrupted::Oldest
            }
            None => recovery::Interrupted::None,
        }
    }
    /// Logs what the firmware made of a kick a restart interrupted, which fails however its
    /// witnesses read (`Completion::note_interrupted`): its record, and its descriptor's times,
    /// which the firmware writes only for a kick that launched work. Neither proves the dispatch
    /// whole: a measured interrupted kick ran to its record's end and still left 64 workgroups
    /// wrong.
    fn report_interrupted(&self, front: &Active, outcome: core::result::Result<[u64; 2], Error>) {
        let words = self
            .descriptors
            .read_words::<4>(front.slot * DESCRIPTOR_BYTES + DESCRIPTOR_TIMES);
        let launched = match words.as_ref().is_ok_and(|w| w[0] != 0 || w[1] != 0) {
            true => "launched",
            false => "launched no work",
        };
        let (how, record) = match outcome {
            Ok(times) => ("retired by the firmware", Some(times)),
            Err(_) => (
                "failed by the firmware",
                front.observed.map(|o| o.timestamps),
            ),
        };
        pr_info!("G17 M5: lane {}: kick {:#x} ({}), interrupted by a restart, {} ({:?}): record {:?}, descriptor times {:x?} ({}); it fails\n",
            self.qid(), front.ticket.kick, innocence(front), how, outcome.err(), record, words, launched);
    }
    pub(crate) fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
        for (_, packet) in self.commands() {
            visit(packet.completion.status());
        }
    }
    pub(crate) fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {
        for (_, packet) in self.commands() {
            packet
                .completion
                .classify(class(packet.completion.status()));
        }
    }
    /// Notes the DRM kicks in flight for recovery's attribution, read while the firmware is
    /// halted ([`recovery::Progress`]): the lane runs them in order, so the newest one whose
    /// descriptor has a start time (cleared at its publication, written once it launched work)
    /// is the one running, and with none the lane's work never started. A flush-only or
    /// empty-grid kick writes no times; as unstarted it can lose a blame, never gain one.
    pub(crate) fn note_progress(&self, progress: &mut recovery::Progress) {
        let mut running = None;
        fence(Ordering::Acquire);
        for active in self.active.iter().filter(|active| active.packet.is_some()) {
            let Ok(words) = self
                .descriptors
                .read_words::<2>(active.slot * DESCRIPTOR_BYTES + DESCRIPTOR_TIMES)
            else {
                return;
            };
            let start = u64::from(words[0]) | (u64::from(words[1]) << 32);
            if start != 0 {
                running = Some((active.ticket.kick, start));
            }
            running.get_or_insert((active.ticket.kick, 0));
        }
        let Some((kick, start)) = running else {
            return;
        };
        progress.note(
            [self.qid(); 2],
            recovery::Work::Kick,
            kick,
            (start != 0).then_some(start),
        );
    }

    /// The inner item ring's cursors.
    pub(crate) fn cursors(&self) -> Result<Cursors> {
        self.lane.cursors()
    }

    /// The oldest kick's witnesses besides the stamp and cursors, for a diagnostic: its ticket,
    /// its completion record if the scan saw one, its descriptor's times, the pool target
    /// against the pool and whether its work node drained.
    pub(crate) fn front_witnesses(
        &self,
    ) -> Option<(
        Ticket,
        Option<Observation>,
        Result<[u32; 4]>,
        u64,
        Result<u64>,
        Result<bool>,
    )> {
        let front = self.active.first()?;
        Some((
            front.ticket,
            front.observed,
            self.descriptors
                .read_words::<4>(front.slot * DESCRIPTOR_BYTES + DESCRIPTOR_TIMES),
            front.pool_target,
            self.pool.completed_submissions(),
            front.work.drained(),
        ))
    }

    /// The queue's completion stamp.
    pub(crate) fn stamp(&self) -> Result<u32> {
        fence(Ordering::Acquire);
        Ok(self.support.word(STAMPS)?.load(Ordering::Relaxed))
    }

    pub(crate) fn completed_submissions(&self) -> Result<u64> {
        self.pool.completed_submissions()
    }

    fn qos_owner(&self) -> qos::Owner {
        qos::Owner {
            qid: self.qid(),
            qos: self.context.qos_id(),
            data_master: DataMaster::Compute as u8,
        }
    }

    /// The queue configuration: the installation's (`install`), the first kick's context and
    /// pool update, or an adopted lane's first kick: a new owner's context, scheduler and QoS
    /// identity on the queue's continuing completion value, reinstalled when its class changed
    /// (S `queue_config`).
    fn config(&self, install: bool, completion_seed: Option<u64>) -> Result<QueueConfig> {
        let rebound = (!install).then_some(self.rebound).flatten();
        QueueConfig::new(&QueueConfigArgs {
            target: QueueConfigTarget::Compute,
            kick_ring_va: self.lane.kick.low_va(),
            kick_ring_fw_va: self.lane.kick.firmware_va(),
            qid: self.qid(),
            install: install || rebound == Some(true),
            // An adopted lane's completion value continues, unless no kick ever ran on it: then
            // its first kick is the lane's first and takes the fresh lane's seed.
            completion_seed: match rebound {
                Some(_) if self.lane.kick.timestamp().get() != KickTimestamp::FIRST.get() => None,
                _ => completion_seed,
            },
            context_id: self.context.id().try_into()?,
            // The pool is bound with the first kick's context update.
            free_list: match install {
                true => FreeListBinding {
                    va: 0,
                    generation: u64::MAX,
                    slot: u32::MAX,
                },
                false => FreeListBinding {
                    va: self.pool.control_va(),
                    generation: 1,
                    slot: u32::from(self.pool.id()),
                },
            },
            scheduler_va: self.context.scheduler_va(),
            policy: self.context.policy(),
            qos_slot: self.context.qos_id(),
            qos_update: install || rebound.is_some(),
            owner_pid: self.context.owner_pid(),
            context_update: !install,
        })
    }

    /// Installs the queue: its pool row, the initial configuration, the QID, and the first outer
    /// slot naming it. The firmware's consumption of the configuration is awaited off the device
    /// mutex ([`Self::install_consumed`]). From the publication on the channel is retained
    /// whatever happens; a failed installation leaves it quarantined. EAGAIN: no outer slot yet,
    /// and nothing was published.
    pub(crate) fn install(&mut self, host: &mut Host<'_>) -> Result {
        if self.installed {
            return Err(EINVAL);
        }
        let claim = host
            .port
            .reserve_compute(self.context.policy().priority())?;
        host.init
            .install_m5_pool(self.pool.id(), self.pool.descriptor()?)?;
        self.lane.stage_install(self.config(true, None)?)?;
        host.qids.publish(self.lane.id)?;
        self.installed = true;
        // The installation names the context's scheduler page: its close releases it.
        self.context.mark_published();
        self.class = self.context.policy().priority();
        let result = self
            .lane
            .expose(host.port, claim, 1, true, None)
            .map(|_| ());
        if let Err(error) = result {
            self.quarantine_error = Some(error);
        }
        result
    }

    /// Reinstalls an adopted recycled lane for its new owner before the binding's first kick: the
    /// installation's configuration (the new context, its scheduler and QoS identity, the
    /// completion value continuing) as one item with no kick, exposed through the outer slot as a
    /// fresh lane's installation is, its consumption awaited as that one's
    /// ([`Self::install_consumed`]). A lane adopted as it parked has nothing to reinstall. The
    /// firmware fails the first kick on a killed context's lane at once with a zero-duration
    /// record, after the item-ring reset, the cleared parent, the cleared descriptors and fresh
    /// pool backing alike; the configuration reaching the firmware before any kick is what
    /// a fresh lane has. EAGAIN: no outer slot yet, nothing published. A failure after the
    /// publication quarantines the channel.
    pub(crate) fn reinstall(&mut self, host: &mut Host<'_>) -> Result {
        if self.reinstall != Reinstall::Due {
            return Ok(());
        }
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        let priority = self.context.policy().priority();
        let producer = self.lane.cursors()?.producer;
        if !self.lane.idle_at(producer)? {
            return Err(EIO);
        }
        let claim = host.port.reserve_compute(priority)?;
        let target = self
            .lane
            .stage_reinstall(producer, self.config(true, None)?)?;
        let first = self.class != priority;
        self.class = priority;
        self.rebound = Some(false);
        self.reinstall = Reinstall::Published(target);
        self.context.mark_published();
        let result = self
            .lane
            .expose(host.port, claim, target, first, None)
            .map(|_| ());
        if let Err(error) = result {
            self.quarantine_error = Some(error);
        }
        result
    }

    /// Whether the firmware consumed the installation's configuration ([`Self::install`]); an
    /// adopted lane's binding has none and is consumed at once. A crash, or the configuration
    /// still unconsumed once the wait `expired` ([`INSTALL_WAIT_MS`]), quarantines the channel.
    pub(crate) fn install_consumed(&mut self, crashed: &AtomicBool, expired: bool) -> Result<bool> {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        let consumed = match self.reinstall {
            Reinstall::Published(target) => self.lane.cursors()?.consumer == target,
            Reinstall::Due => false,
            Reinstall::None => self.rebound.is_some() || self.lane.cursors()?.consumer == 1,
        };
        if consumed {
            if let Reinstall::Published(target) = self.reinstall {
                self.reinstall = Reinstall::None;
                pr_info!(
                    "G17 M5: lane {}: reinstalled for queue {} (consumed at {:#x})\n",
                    self.qid(),
                    self.owner,
                    target
                );
            }
            return Ok(true);
        }
        let error = match () {
            _ if crashed.load(Ordering::Acquire) => EIO,
            _ if expired => ETIMEDOUT,
            _ => return Ok(false),
        };
        self.quarantine_error = Some(error);
        Err(error)
    }

    /// Whether a command whose plan needs `blocks` must back the pool first ([`Self::grow`]):
    /// it is smaller, vacant, or being released.
    pub(crate) fn needs_pool(&self, blocks: usize) -> bool {
        needs_pool(&self.pool, &self.pool_state, blocks)
    }

    /// Backs the lane's pool for a command whose plan needs `blocks`, before that command is
    /// published ([`grow_pool`], the rule a render pair's pool shares): the lane is idle once no
    /// kick is in flight or failed, the item ring is consumed to its producer, the pool's count
    /// is complete and the stamp covers the lane's last kick. Returns the old backing and view,
    /// which the caller drops off the device mutex.
    pub(crate) fn grow(
        &mut self,
        alloc: &Allocator<'_>,
        host: &mut Host<'_>,
        notify: impl FnOnce() -> Result,
        blocks: usize,
    ) -> Result<Option<Grown>> {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        if !self.installed || self.closed {
            return Err(EINVAL);
        }
        let (lane, support, faulted) = (&self.lane, &self.support, self.faulted);
        let (active, failed) = (&self.active, &self.failed);
        let idle = |pool: &M5Pool| -> Result<bool> {
            let next = lane.kick.timestamp();
            // The queue's completion stamp ([`Self::stamp`]), read last, as the growth always did.
            let stamp = || -> Result<u32> {
                fence(Ordering::Acquire);
                Ok(support.word(STAMPS)?.load(Ordering::Relaxed))
            };
            let busy = faulted
                || !active.is_empty()
                || !failed.is_empty()
                || !lane.idle_at(lane.cursors()?.producer)?
                || pool.completed_submissions()? != pool.submitted()?
                || (next.get() != KickTimestamp::FIRST.get()
                    && !retirement::stamp_covers(stamp()?, next.prev().get()));
            Ok(!busy)
        };
        let site = PoolSite {
            qid: lane.qid(),
            pool: &mut self.pool,
            state: &mut self.pool_state,
            vm: self.context.vm(),
            aliases: &mut self.aliases,
            quarantine: &mut self.quarantine_error,
        };
        let grown = grow_pool(
            alloc,
            host.init,
            notify,
            site,
            idle,
            blocks,
            (self.gpu.pool_blocks, self.gpu.max_pool_blocks),
        )?;
        if let Some(grown) = &grown {
            pr_info!(
                "M5 lane {}: pool backed with {} blocks (from {}) for queue {}\n",
                self.qid(),
                grown.to,
                grown.from,
                self.owner
            );
        }
        Ok(grown)
    }

    /// A release of the pool is in flight (the event worker reports its consumption as
    /// progress).
    pub(crate) fn pool_release_pending(&self) -> bool {
        self.pool_state.pending()
    }
    pub(crate) fn pool_release_stalled(&self, now: u64) -> bool {
        self.pool_state.stalled(now)
    }

    /// The one M5 publication transaction: descriptor, notification and kick entry, the inner
    /// items, then the outer slot and the host command. `work` holds the kick's
    /// work-state node; `epoch` is the recovery generation the caller checked with recovery
    /// closed. Everything fallible precedes the first firmware-visible store; a failure after it
    /// quarantines the channel. EAGAIN: no room yet, and nothing changed.
    pub(crate) fn publish(
        &mut self,
        host: &mut Host<'_>,
        dispatch: &Dispatch,
        work: &Arc<WorkStateLease>,
        submission: u64,
        epoch: u64,
        packet: Option<Arc<Packet>>,
    ) -> Result<Published> {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        if !self.installed || self.closed {
            return Err(EINVAL);
        }
        if self.active.len() >= DEPTH {
            return Err(EAGAIN);
        }
        let policy = self.context.policy();
        let cursors = self.cursors()?;
        let first = self.ordinal == 0;
        let items = if first || !self.pool_state.registered() {
            2
        } else {
            1
        };
        if (cursors.consumer + cursors.count - cursors.producer - 1) % cursors.count < items {
            return Err(EAGAIN);
        }
        // A command that needs a larger pool, or a pool at all, backs it first ([`Self::grow`]).
        if self.needs_pool(dispatch.resources.pool_blocks) {
            return Err(EINVAL);
        }
        let claim = host.port.reserve_compute(policy.priority())?;
        let timestamp = self.lane.kick.timestamp();
        let slot = timestamp.slot() % DEPTH;
        let offset = (slot * DESCRIPTOR_BYTES) as u64;
        let high = self.descriptors.gpu_va() + offset;
        let low = self.descriptors_low + offset;
        let work_word = work.word();
        let work_node = work.node_va()?;
        let stamps = self.support.gpu_va() + STAMPS as u64;
        let status = self.support.gpu_va() + STATUS as u64;
        let args = DescriptorArgs {
            registers: RegisterArgs {
                auxiliary_allocation: 0,
                preempt_va: self.preempt_va,
                cdm_va: dispatch.cdm_va,
                operand_va: self.operand.gpu_va(),
                usage_va: self.usage_va,
                free_list_slot: u32::from(self.pool.id()),
                context_id: self.context.id().try_into()?,
                work_state: work_word,
                object_ids: compute::OBJECT_IDS,
                resource_policy: dispatch.resources.policy,
                preempt_state_offset: self.gpu.profile.preempt_state_offset,
                operand: self.gpu.profile.operand,
                resource_control: dispatch.resources.control,
                dispatch_control: self.gpu.profile.dispatch_control,
                auxiliary_control: self.gpu.profile.auxiliary_control,
            },
            descriptor_va: low,
            submit_sequence: 0,
            work_state_va: work_node,
            cdm_end_va: dispatch.cdm_end_va,
            stamp_va: [stamps, stamps + 0x40],
            stamp_value: fw::stamp_value(timestamp.get() as u32),
            qid: u32::from(self.qid()),
            kick_timestamp: timestamp.get(),
            completion_ordinal: 0,
            status_va: [status, status + 8],
            timestamp_va: [0; 2],
            shared_control_va: self.pool.control_va(),
            event_metadata: [0; 16],
            usage_fw_va: self.support.gpu_va() + USAGE as u64,
            completion_identity: compute::COMPLETION_IDENTITY,
        };
        // The slot's previous kick retired: firmware no longer reads its descriptor or record.
        self.descriptors
            .initialize::<Descriptor>(slot * DESCRIPTOR_BYTES, |descriptor| {
                descriptor.write(&args)
            })?;
        // The first kick binds the pool with the context; a later one rebinds a grown pool, the
        // queue's completion value continuing.
        let update = match (first, self.pool_state.registered()) {
            (true, _) => Some(self.config(false, Some(0))?),
            (false, false) => Some(self.config(false, None)?),
            (false, true) => None,
        };
        let target = self
            .lane
            .stage_kick(cursors.producer, update, slot, timestamp)?;
        let command = HardwareSubmission::compute(
            self.qid(),
            self.lane.kick.low_va(),
            policy.priority(),
            first,
        )?;
        let kick_args = KickArgs {
            qid: self.qid(),
            timestamp,
            parent: self.lane.kick.parent(),
            barriers: &[],
            dependency_types: Some(&[compute::PARENT_DEPENDENCY]),
            descriptor_va: high,
            queue_va: self.lane.graph.gpu_va(),
            qos: KickQos {
                slot: self.context.qos_id(),
                class: compute::KICK_QOS_CLASS,
            },
            mcache: None,
            event_mask: compute::KICK_EVENT_MASK,
            register_arrays: compute::bindings(low)?,
            scs_enabled: false,
            priority: policy.priority(),
        };
        self.active.reserve(1, GFP_KERNEL)?;
        let prepared = self.lane.kick.prepare_entry(&kick_args)?;
        if first {
            prepared.prepare_lower_install()?;
        }
        work.add_submitted_kicks(1)?;
        let pool_target = match self.pool.reserve_submission() {
            Ok(target) => target,
            Err(error) => {
                // The pool counter is unchanged: nothing is visible yet.
                work.cancel_unpublished_kicks(1)?;
                return Err(error);
            }
        };
        // From here on the firmware may see this kick: the pool counter names it, and the
        // queue may be running earlier kicks that read the cursors.
        prepared.commit();
        let published = host.port.next_publication();
        self.pool_state.register();
        if let Some(packet) = &packet {
            packet.mark_published();
        }
        let mut qos_row = None;
        let result = (|| {
            let publication =
                host.port
                    .qos_publish(self.qos_owner(), self.context.scheduler_va(), policy)?;
            qos_row = Some(publication.record);
            self.context.mark_published();
            fence(Ordering::SeqCst);
            // The work ring names the queue and the firmware hears of it before the host
            // command makes it runnable; whether the cores are powered is not a prerequisite.
            // The first slot naming the queue on a class's outer ring says so: an adopted lane
            // whose class changed starts on its new ring.
            self.lane.expose(
                host.port,
                claim,
                target,
                self.class != policy.priority(),
                Some(&command),
            )
        })();
        let ticket = Ticket {
            owner: self.owner,
            submission,
            context_id: self.context.id(),
            qid: self.qid(),
            descriptor: high,
            work_node,
            kick: timestamp.get(),
            ordinal: self.ordinal,
            item_producer: target,
            item_count: ITEM_RING_ENTRIES,
            epoch,
        };
        if first && result.is_ok() {
            self.log_class(policy, qos_row, &command);
        }
        self.class = policy.priority();
        self.rebound = None;
        self.ordinal = self.ordinal.saturating_add(1);
        self.active
            .push_within_capacity(Active {
                ticket,
                slot,
                pool_target,
                work: work.clone(),
                packet,
                observed: None,
                qos: qos_row.is_some(),
                published,
            })
            .map_err(|_| ENOMEM)?;
        match result {
            Ok(outer) => Ok(Published {
                ticket,
                pool_target,
                outer,
            }),
            Err(error) => {
                // The kick stays in flight and is failed with the channel.
                self.quarantine_error = Some(error);
                Err(error)
            }
        }
    }

    /// Stages the exact identity of every kick not yet observed for the shared completion-ring
    /// scan. Returns whether any was staged.
    pub(crate) fn request(&self, batch: &mut Batch) -> Result<bool> {
        if self.active.is_empty() || self.quarantine_error.is_some() {
            return Ok(false);
        }
        batch.stage_frontier(self.qid(), self.lane.kick.timestamp().get())?;
        let stamp = self.stamp()?;
        let mut requested = false;
        for active in self
            .active
            .iter()
            .filter(|active| active.observed.is_none())
        {
            batch.request(
                &active.ticket,
                retirement::stamp_covers(stamp, active.ticket.kick),
            )?;
            requested = true;
        }
        Ok(requested)
    }

    /// Retires the kicks the scan of `batch` completed, oldest first, reporting each to
    /// `retired`; `state` is the device's recovery state. A kick whose witnesses are missing or
    /// disagree stays in flight, for the scheduler's bounded timeout to fail. A zero-duration
    /// record (the firmware failed the kick) or a fault the firmware reported on this queue
    /// quarantines the channel ([`Self::fail`]).
    pub(crate) fn retire(
        &mut self,
        batch: &mut Batch,
        state: u32,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        retired: &mut impl FnMut(Retired) -> Result,
    ) -> Result {
        if self.quarantine_error.is_some() {
            return self.rehabilitate(state, complete_qos, retired);
        }
        if self.faulted {
            return self.fail(EIO, Failure::Settled, retired);
        }
        for active in self
            .active
            .iter_mut()
            .filter(|active| active.observed.is_none())
        {
            active.observed = batch.take(&active.ticket)?;
        }
        self.retire_pending = false;
        while let Some(front) = self.active.first() {
            match self.check(front, state) {
                Ok(Some(timestamps)) => {
                    if front
                        .packet
                        .as_ref()
                        .is_some_and(|p| p.completion.interrupted())
                    {
                        self.report_interrupted(front, Ok(timestamps));
                    }
                    complete_qos(self.qos_owner())?;
                    // Reported before the channel gives the kick up, as Neo's release does
                    // (`Pair::release_witnessed`): a refused report leaves it owned here.
                    retired(Retired {
                        ticket: front.ticket,
                        packet: front.packet.clone(),
                        result: Ok(timestamps),
                        drained: false,
                    })?;
                    let front = self.active.remove(0).map_err(|_| EINVAL)?;
                    // The GPU is retiring work: a watchdog of a queue beside it renews
                    // (`timeout::renew_shared`), as for a Neo compute queue's retirement.
                    crate::g17::recovery::note_retired();
                    // DRM kicks only: the qualification's thousands of kicks carry no packet.
                    if front.packet.is_some()
                        && crate::module_parameters::m5_trace_completions.value() != 0
                    {
                        pr_info!(
                            "M5 retire q{} class {} kick {:#x}: fw start {} end {}\n",
                            self.qid(),
                            self.context.policy().priority(),
                            front.ticket.kick,
                            timestamps[0],
                            timestamps[1],
                        );
                    }
                }
                Ok(None) => {
                    self.retire_pending = self.front_finished();
                    break;
                }
                // A zero-duration record is the firmware's own failure of the kick (ENODATA,
                // `retirement::poll`); any other error is a witness that cannot be read.
                Err(error) => {
                    if front
                        .packet
                        .as_ref()
                        .is_some_and(|p| p.completion.interrupted())
                    {
                        self.report_interrupted(front, Err(error));
                    }
                    let failure = if error == ENODATA {
                        Failure::Settled
                    } else {
                        Failure::Ambiguous
                    };
                    return self.fail(error, failure, retired);
                }
            }
        }
        Ok(())
    }

    /// The retirement predicate of one kick: `Some` with its record's timestamps once every
    /// witness agrees, `None` while any is missing or disagrees. The witnesses: its completion
    /// record, the stamp, the consumed item ring, the pool's count and the drained work node.
    /// The descriptor's times are not a witness: the firmware writes them only for a kick that
    /// launched work, which neither a flush-only stream nor a dispatch of an empty grid
    /// (CTS `indirect_dispatch.upload_buffer.empty_command`) does. Where it wrote them they are
    /// checked against the record as a diagnostic.
    fn check(&self, active: &Active, state: u32) -> Result<Option<[u64; 2]>> {
        if state != 0 {
            return Ok(None);
        }
        let stamp = self.stamp()?;
        let Some(timestamps) = retirement::poll(
            &active.ticket,
            active.observed,
            self.cursors()?,
            stamp,
            Recordless::NotYet,
            0,
            None,
            false,
        )?
        else {
            return Ok(None);
        };
        if self.pool.completed_submissions()? < active.pool_target || !active.work.drained()? {
            return Ok(None);
        }
        let offset = active.slot * DESCRIPTOR_BYTES;
        let words = self
            .descriptors
            .read_words::<4>(offset + DESCRIPTOR_TIMES)?;
        let descriptor = [
            u64::from(words[0]) | (u64::from(words[1]) << 32),
            u64::from(words[2]) | (u64::from(words[3]) << 32),
        ];
        if descriptor != [0; 2] && (timestamps[1] != descriptor[1] || timestamps[0] > descriptor[0])
        {
            kernel::pr_warn_once!(
                "G17 M5: queue {} kick {:#x}: descriptor times {:?} disagree with the record's {:?}\n",
                self.qid(), active.ticket.kick, descriptor, timestamps
            );
        }
        Ok(Some(timestamps))
    }

    /// Quarantines the channel with `error` and fails every kick in flight. The kicks stay
    /// owned by the channel, pinning their commands, until they drain: a [`Failure::Settled`]
    /// one ends there ([`Self::rehabilitate`]); others keep the kicks and the quarantine for the
    /// session.
    pub(crate) fn fail(
        &mut self,
        error: Error,
        failure: Failure,
        retired: &mut impl FnMut(Retired) -> Result,
    ) -> Result {
        // The M5 Ultra's rule (`awaits_witness`): the first failure decides. Every kick carries
        // its ticket from publication, and an empty lane's witness is its idle ring
        // ([`Self::drained`]), so the work is always witnessable.
        self.awaiting_witness = awaits_witness(
            Some(failure),
            self.quarantine_error.is_none(),
            self.awaiting_witness,
            true,
        );
        self.quarantine_error.get_or_insert(error);
        let error = self.quarantine_error.unwrap_or(error);
        self.failed.reserve(self.active.len(), GFP_KERNEL)?;
        while let Some(front) = self.active.first() {
            // Reported before the kick moves to `failed`, as Neo reports before it gives a command
            // up (`Pair::release_witnessed`); the channel owns it either way.
            retired(Retired {
                ticket: front.ticket,
                packet: front.packet.clone(),
                result: Err(error),
                drained: false,
            })?;
            let active = self.active.remove(0).map_err(|_| EINVAL)?;
            self.failed
                .push_within_capacity(active)
                .map_err(|_| ENOMEM)?;
        }
        Ok(())
    }

    /// Every failed kick retired exactly, with recovery closed (`state`): the stamp covers the
    /// newest, the inner ring is consumed to its producer, the pool's count is complete and each
    /// kick's work node drained. The firmware names nothing of them any more.
    fn drained(&self, state: u32) -> Result<bool> {
        if state != 0
            || !self.lane.idle_at(self.lane.cursors()?.producer)?
            || self.pool.completed_submissions()? != self.pool.submitted()?
        {
            return Ok(false);
        }
        if let Some(newest) = self.failed.last() {
            if !retirement::stamp_covers(self.stamp()?, newest.ticket.kick) {
                return Ok(false);
            }
        }
        for failed in self.failed.iter() {
            if !failed.work.drained()? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Ends a settled failure's quarantine once its kicks drained ([`Self::drained`]), as a Neo
    /// pair's exact drain ends its quarantine (`Pair::release_witnessed`): each kick is reported
    /// drained with its error, in Neo's order (`Deferred::Drained`: the pinned command released,
    /// then a fence the failure never reached failed), and only then given up; the channel is
    /// healthy again and a closed one parks. Witnesses that cannot be read make the failure
    /// Ambiguous: the quarantine stays, and so do the kicks, pinned.
    /// Each drained kick's QoS publication is completed first, as a Neo compute queue's exact drain
    /// completes it (`Queue::release_witnessed`) and a render pair's drained pass does
    /// (`Pair::retire`): left open, the QID's in-flight count and its owner's QoS row outlive the
    /// kick, and the lane's next owner with another QoS slot is refused (EBUSY) at every
    /// publication.
    fn rehabilitate(
        &mut self,
        state: u32,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        retired: &mut impl FnMut(Retired) -> Result,
    ) -> Result {
        if !self.awaiting_witness {
            return Ok(());
        }
        match self.drained(state) {
            Ok(true) => {}
            Ok(false) => return Ok(()),
            Err(error) => {
                pr_warn!("G17 M5: lane {}: its failed kicks' drain witnesses unreadable ({:?}); the lane stays quarantined\n",
                    self.qid(), error);
                self.awaiting_witness = false;
                return Ok(());
            }
        }
        let error = self.quarantine_error.unwrap_or(EIO);
        while let Some(front) = self.failed.first() {
            if front.qos {
                complete_qos(self.qos_owner())?;
                self.failed.first_mut().ok_or(EIO)?.qos = false;
            }
            let front = self.failed.first().ok_or(EIO)?;
            retired(Retired {
                ticket: front.ticket,
                packet: front.packet.clone(),
                result: Err(error),
                drained: true,
            })?;
            self.failed.remove(0).map_err(|_| EINVAL)?;
        }
        if self.killed_at.is_some() {
            self.awaiting_witness = false;
            return self.recycle_killed(error);
        }
        self.quarantine_error = None;
        self.awaiting_witness = false;
        self.faulted = false;
        pr_info!(
            "G17 M5: lane {}: its failed kicks ({:?}) drained; the lane returns\n",
            self.qid(),
            error
        );
        Ok(())
    }

    /// Returns a lane whose context was killed to service once the kill settled
    /// ([`Self::settle_kill`]) and its failed kicks drained exactly ([`Self::drained`]), as Neo's
    /// compute queue is recycled (`queue::compute::Queue::recycle_killed`): the item ring restarts
    /// empty at the firmware's consumer, the next kick names no parent (the killed kick was the
    /// last), every descriptor slot is cleared, and the pool is released and vacant before any
    /// owner adopts the lane ([`ParkedLane::adoptable`]), whose first command backs it afresh
    /// ([`Self::grow`]). Returning the lane as it was failed its next owner's first kick at once,
    /// each time, with a zero-duration record. A ring that cannot be reset keeps the
    /// quarantine.
    fn recycle_killed(&mut self, error: Error) -> Result {
        if !self.installed
            || !self.exit.as_ref().is_some_and(|exit| exit.released())
            || !self.active.is_empty()
            || !self.failed.is_empty()
        {
            return Ok(());
        }
        if let Err(reset) = self.lane.reset_items() {
            pr_warn!("G17 M5: lane {}: its ring could not be reset after its context's kill ({:?}); the lane stays quarantined\n",
                self.qid(), reset);
            return Ok(());
        }
        self.lane.kick.clear_parent_after_recovery();
        self.descriptors.bytes_mut().fill(0);
        fence(Ordering::SeqCst);
        self.exit = None;
        self.killed_at = None;
        self.quarantine_error = None;
        self.faulted = false;
        self.retire_pending = false;
        self.needs_fresh_backing = true;
        pr_info!("G17 M5: lane {}: its failed kicks ({:?}) drained after its context's kill; the lane is recycled\n",
            self.qid(), error);
        Ok(())
    }

    /// The end of the last nonzero word of descriptor slot `slot`: how far the firmware wrote.
    pub(crate) fn descriptor_extent(&self, slot: usize) -> Result<usize> {
        let base = slot * DESCRIPTOR_BYTES;
        let mut end = 0;
        for offset in (0..DESCRIPTOR_BYTES).step_by(4) {
            if self
                .descriptors
                .word(base + offset)?
                .load(Ordering::Relaxed)
                != 0
            {
                end = offset + 4;
            }
        }
        Ok(end)
    }

    /// Logs what the queue's first publication handed the firmware for its class: the record's
    /// scheduling words as stored (+0x28..+0x40: priority twice, the class words, the policy),
    /// the QoS row written, the work ring and doorbell, and the host port's configuration.
    fn log_class(&self, policy: Policy, qos_row: Option<[u8; 8]>, command: &HardwareSubmission) {
        let record: [u32; 6] =
            core::array::from_fn(|i| self.lane.word(0x28 + 4 * i).unwrap_or(u32::MAX));
        let ring = fw::channels::work_ring_index(policy.priority(), DataMaster::Compute);
        pr_info!(
            "M5 queue {}: class {} policy {} QoS {}/{}: record +0x28 {:08x?}, QoS row {:02x?}, compute ring {:?}, notify {:#018x}, host config {:#018x?}\n",
            self.qid(),
            policy.priority(),
            policy.policy(),
            policy.qos_class(),
            policy.qos_share(),
            record,
            qos_row,
            ring,
            fw::channels::work_notification(policy.priority(), DataMaster::Compute),
            command.configuration(),
        );
    }

    /// Logs the counters and nonzero stamp and status words of a kick that did not retire.
    pub(crate) fn report(&self, dev: &AsahiDrmDevice, published: &Published) {
        dev_info!(
            dev.as_ref(),
            "M5 channel {}: kick {:#x}, pool completed {:?}/{}, cursors {:?}, quarantine {:?}\n",
            self.qid(),
            published.ticket.kick,
            self.pool.completed_submissions(),
            published.pool_target,
            self.cursors().map(|c| (c.producer, c.consumer)),
            self.quarantine_error
        );
        for offset in (0..STATUS + 0x10).step_by(4) {
            if let Ok(value) = self
                .support
                .word(STAMPS + offset)
                .map(|w| w.load(Ordering::Relaxed))
            {
                if value != 0 {
                    dev_info!(dev.as_ref(), "M5 completion +{:#x}={:#x}\n", offset, value);
                }
            }
        }
    }
}

/// How a recovery classified an interrupted kick, for its log line.
fn innocence(active: &Active) -> &'static str {
    match active.packet.as_ref().map(|packet| &packet.completion) {
        Some(c) if c.blamed() => "blamed",
        Some(c) if c.guilty() => "guilty",
        Some(c) if c.spared() => "spared",
        Some(_) => "unclassified",
        None => "no command",
    }
}
