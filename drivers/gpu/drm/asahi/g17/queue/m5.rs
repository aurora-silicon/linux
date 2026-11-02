// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 Ultra compute lanes: one hardware compute queue (QID) and its graph, built on the M5 queue,
//! descriptor and pool encodings ([`crate::g17::fw::m5`]) and published through
//! [`Port::publish_m5_compute`]. A lane is Neo's compute queue ([`Queue`]) with its `m5` arms:
//! the hardware half in `compute::Graph` and `ComputePool`, the lane's own state in
//! [`LaneState`], and the arms here. DRM compute submission and the arithmetic qualification
//! (`g17/m5_test.rs`) both run work through it: one construction, one installation, one
//! publication transaction ([`Queue::transact`]) and one retirement predicate
//! ([`Queue::check`]). The queue keeps DRM kicks in flight; the qualification keeps its own.
//!
//! An installed lane (QID, kick ring, graph, descriptors, support page and pool) stays with its
//! firmware session, as the firmware has no queue uninstall: a queue binds it
//! to one logical queue, parks it once that queue closed, the lane is idle and its context's
//! scheduler release was consumed ([`Queue::park_m5`]), and a later queue adopts it
//! ([`Queue::adopt_m5`]), reconfiguring the hardware queue at its first kick. Kick timestamps and
//! stamps continue across owners. A quarantined lane is not reused, and a failed kick keeps its
//! command pinned, until a witness ends the quarantine: a settled failure's exact drain returns
//! the lane as it is, and a killed context's exact drain settles the kill and recycles it with
//! fresh backing through Neo's `Queue::settle_exit` and `Queue::recycle_killed`.
//!
//! Retirement is exact and strict: the completion record of this queue, kick and
//! descriptor, the queue's stamp covering the kick, the inner consumer past the kick's items, the
//! pool's completed count at the kick's target, the work-state node drained and the record's end
//! time equal to the descriptor's, with recovery closed. A kick may retire across a firmware
//! restart, as on Neo (`retirement::poll`'s stamp rule): the restart's recovery table names
//! every kick it interrupted, and recovery marks those (`Queue::note_interrupted`) so that
//! they fail however their witnesses read. There is no aged-record or recordless retirement. These
//! witnesses prove that the firmware retired the kick, not that every workgroup ran: dispatches
//! that lost workgroups retired with all of them.

use super::compute::{
    awaits_witness, exit, Active, DescriptorRing, Failure, Graph, Queue, Storage, GRAPH_SIZE,
    ITEMS, POINTERS,
};
use super::pool_state::{PoolPhase, PoolState};
use crate::g17::{
    completion::Deferred,
    context::{Context, WorkStateLease},
    freelist::{ComputePool, M5Backing, M5Pool},
    fw::{
        self,
        kick::{KickArgs, KickQos, KickTimestamp},
        m5::{
            compute::{self, Descriptor, DescriptorArgs, Gpu, RegisterArgs, Resources},
            queue::{EntryGate, HardwareSubmission, Notification},
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
/// Kicks a lane holds in flight: the retirement batch's bound per QID. A descriptor and a
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
pub(super) const STAMPS: usize = 0;
const STATUS: usize = 0x80;
const USAGE: usize = 0x1000;
const SUPPORT_BYTES: usize = 0x4000;
/// Polls of 1 ms for the installation to be consumed.
const INSTALL_POLLS: u32 = 2000;
/// How long an installation may take to be consumed, polled or awaited.
pub(crate) const INSTALL_WAIT_MS: u32 = INSTALL_POLLS;

/// What a lane uses of its firmware session, borrowed under the device mutex.
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

/// One M5 queue's private pool as a growth sees it: a compute lane's ([`Queue::grow`]) or a
/// render pair's (`m5_render::Pair::grow`), the pool's registration, and the binding's client
/// views (in `vm`) and quarantine.
pub(crate) struct PoolSite<'a> {
    /// The QID whose page-pool slot the pool takes (a pair's tiling queue).
    pub(crate) qid: u8,
    pub(crate) pool: &'a mut M5Pool,
    pub(crate) state: &'a mut PoolState,
    pub(crate) vm: &'a mmu::Vm,
    pub(crate) views: PoolView<'a>,
    pub(crate) quarantine: &'a mut Option<Error>,
}

/// Where a binding keeps its client view of the pool: a compute lane's binding has a field for it
/// (`compute::Binding::pool_alias`), a render pair's keeps it among its aliases.
pub(crate) enum PoolView<'a> {
    Field(&'a mut Option<mmu::KernelMapping>),
    Aliases(&'a mut KVec<mmu::KernelMapping>),
}

impl PoolView<'_> {
    /// Makes room for the view, so that installing it cannot fail.
    fn reserve(&mut self) -> Result {
        match self {
            Self::Field(_) => Ok(()),
            Self::Aliases(aliases) => Ok(aliases.reserve(1, GFP_KERNEL)?),
        }
    }

    /// Installs `view` in place of the view of the backing at `old_va`, returned; with no such
    /// view it is added.
    fn replace(
        &mut self,
        old_va: Option<u64>,
        view: mmu::KernelMapping,
    ) -> Result<Option<mmu::KernelMapping>> {
        match self {
            Self::Field(field) => Ok(field.replace(view)),
            Self::Aliases(aliases) => {
                let index =
                    old_va.and_then(|va| aliases.iter().position(|alias| alias.iova() == va));
                match index {
                    Some(index) => Ok(Some(core::mem::replace(&mut aliases[index], view))),
                    None => {
                        aliases.push_within_capacity(view).map_err(|_| ENOMEM)?;
                        Ok(None)
                    }
                }
            }
        }
    }
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
        mut views,
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
    views.reserve()?;
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
    let old_view = views.replace(old_va, view)?;
    state.populate();
    Ok(Some(Grown {
        _view: old_view,
        _backing: old,
        from,
        to: target,
    }))
}

/// A kick the publication transaction made visible ([`Queue::transact`]): what its caller
/// records of it, and the publication's result. A failure after the kick became visible
/// quarantined the queue (`outer`); the kick is in flight all the same, and fails with it.
pub(crate) struct Kick {
    pub(crate) ticket: Ticket,
    /// The pool's submission count that covers this kick.
    pub(crate) pool_target: u64,
    /// Its publication order across both engines (`Port::next_publication`), for cross-VM
    /// timeout attribution.
    pub(crate) published: u64,
    /// Its QoS publication is open (`Port::qos_publish` succeeded).
    pub(crate) qos: bool,
    /// The outer work ring's producer after this kick.
    pub(crate) outer: Result<u32>,
}

/// The descriptor and notification slot of the kick `ticket` names: its kick timestamp's ring
/// slot, as its publication chose it.
pub(crate) fn slot_of(ticket: &Ticket) -> Result<usize> {
    Ok(KickTimestamp::new(ticket.kick).ok_or(EIO)?.slot() % DEPTH)
}

/// The M5 lane's ring encodings over its graph object ([`super::compute::initialize_m5_graph`]:
/// the queue record, the cursors at `POINTERS` and the item ring at `ITEMS`, then the
/// installation configuration, the first kick's update and the kick notifications): one body
/// for a compute queue's graph (the `m5` arm of `compute::Graph`) and a render pair's lanes
/// ([`Lane`]). `producer` is the offset of the item-ring producer in the pointer block of this
/// firmware build.
pub(crate) mod ring {
    use super::*;

    pub(crate) fn word(graph: &KernelObject, offset: usize) -> Result<u32> {
        Ok(graph.word(offset)?.load(Ordering::Acquire))
    }

    /// Rewrites the record's host-owned words for `context` ([`QueueRecord::set_owner`]). The
    /// caller proved the lane idle with its previous owner's scheduler release consumed.
    pub(crate) fn set_owner(graph: &KernelObject, context: &Context) -> Result {
        let pointer = graph
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

    /// The inner item ring's cursors.
    pub(crate) fn cursors(graph: &KernelObject, producer: usize) -> Result<Cursors> {
        Ok(Cursors {
            producer: word(graph, POINTERS + producer)?,
            consumer: word(graph, POINTERS)?,
            count: ITEM_RING_ENTRIES,
        })
    }

    /// Stages the installation: `config` as the first item.
    pub(crate) fn stage_install(graph: &mut KernelObject, config: QueueConfig) -> Result {
        graph.write(CONFIG, config)?;
        graph.write(ITEMS, graph.gpu_va() + CONFIG as u64)
    }

    /// Stages a reinstallation: `config` as item `producer`, the installation's record reused (its
    /// item was consumed long ago). Returns the producer after it.
    pub(crate) fn stage_reinstall(
        graph: &mut KernelObject,
        producer: u32,
        config: QueueConfig,
    ) -> Result<u32> {
        graph.write(CONFIG, config)?;
        graph.write(
            ITEMS + producer as usize * 8,
            graph.gpu_va() + CONFIG as u64,
        )?;
        Ok((producer + 1) % ITEM_RING_ENTRIES)
    }

    /// The firmware finished every item up to `target` and holds nothing else: `done`, its own
    /// cursor (+0x30) and the producer all stand there, the host's idle condition
    /// ([`PointerBlock`]).
    pub(crate) fn idle_at(graph: &KernelObject, producer: usize, target: u32) -> Result<bool> {
        Ok(word(graph, POINTERS)? == target
            && word(graph, POINTERS + 0x30)? == target
            && word(graph, POINTERS + producer)? == target)
    }

    /// Writes the records of a kick of QID `qid` from item `producer` on: `update`, the first
    /// kick's configuration, and the notification of `timestamp` in notification slot `slot`.
    /// Returns the producer after them and the gate the notification opens, for the kick's event
    /// mask. Nothing is visible until the producer moves.
    pub(crate) fn stage_kick(
        graph: &mut KernelObject,
        qid: u8,
        producer: u32,
        update: Option<QueueConfig>,
        slot: usize,
        timestamp: KickTimestamp,
    ) -> Result<(u32, EntryGate)> {
        let announce = ANNOUNCES + slot * ANNOUNCE_STRIDE;
        if slot >= DEPTH {
            return Err(EINVAL);
        }
        let (notification, gate) = Notification::new(qid, timestamp)?;
        graph.write(announce, notification)?;
        let mut target = producer;
        let mut records = [0u64; 2];
        let mut count = 0;
        if let Some(update) = update {
            graph.write(UPDATE, update)?;
            records[0] = graph.gpu_va() + UPDATE as u64;
            count = 1;
        }
        records[count] = graph.gpu_va() + announce as u64;
        count += 1;
        for &record in &records[..count] {
            graph.write(ITEMS + target as usize * 8, record)?;
            target = (target + 1) % ITEM_RING_ENTRIES;
        }
        Ok((target, gate))
    }

    /// Moves the inner producer (at pointer-block offset `offset`) to `producer`.
    pub(crate) fn set_producer(graph: &KernelObject, offset: usize, producer: u32) -> Result {
        fence(Ordering::SeqCst);
        graph
            .word(POINTERS + offset)?
            .store(producer, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// The outer work slot naming QID `qid`'s items up to `producer`: M5 routes every engine's
    /// pointer list through the common compute channel.
    pub(crate) fn work_slot(
        graph: &KernelObject,
        qid: u8,
        producer: u32,
        first: bool,
    ) -> Result<fw::channels::WorkSlot> {
        fw::channels::WorkSlot::new(
            DataMaster::Compute,
            graph.gpu_va(),
            qid,
            producer.try_into()?,
            first,
        )
    }

    /// Exposes QID `qid`'s inner items up to `producer` through the claimed outer slot, then runs
    /// `submission` (a kick's) through the host command port. Returns the outer producer.
    #[expect(
        clippy::too_many_arguments,
        reason = "The ring, its QID, and the publication's own arguments."
    )]
    pub(crate) fn expose(
        graph: &KernelObject,
        offset: usize,
        qid: u8,
        port: &Port,
        claim: crate::g17::port::OuterClaim<'_>,
        producer: u32,
        first: bool,
        submission: Option<&HardwareSubmission>,
    ) -> Result<u32> {
        set_producer(graph, offset, producer)?;
        port.publish_m5_compute(claim, &work_slot(graph, qid, producer, first)?, submission)
    }
}

/// One M5 hardware queue of a render pair ([`super::m5_render::Pair`]), its tiling or fragment
/// lane: its graph, on the [`ring`] encodings, and its kick ring. A compute queue's graph is the
/// `m5` arm of `compute::Graph` (`Graph::new_m5`) on the same encodings.
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
        let graph = new_graph(alloc, context, pointers)?;
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

    /// [`ring::set_owner`].
    pub(crate) fn set_owner(&mut self, context: &Context) -> Result {
        ring::set_owner(&self.graph, context)
    }

    /// The inner item ring's cursors.
    pub(crate) fn cursors(&self) -> Result<Cursors> {
        ring::cursors(&self.graph, self.producer)
    }

    /// Stages the installation: `config` as the first item.
    pub(crate) fn stage_install(&mut self, config: QueueConfig) -> Result {
        ring::stage_install(&mut self.graph, config)
    }

    /// [`ring::idle_at`].
    pub(crate) fn idle_at(&self, target: u32) -> Result<bool> {
        ring::idle_at(&self.graph, self.producer, target)
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

    /// [`ring::stage_kick`] on this lane's QID.
    pub(crate) fn stage_kick(
        &mut self,
        producer: u32,
        update: Option<QueueConfig>,
        slot: usize,
        timestamp: KickTimestamp,
    ) -> Result<(u32, EntryGate)> {
        let qid = self.qid();
        ring::stage_kick(&mut self.graph, qid, producer, update, slot, timestamp)
    }

    /// Moves the inner producer to `producer`.
    pub(crate) fn set_producer(&self, producer: u32) -> Result {
        ring::set_producer(&self.graph, self.producer, producer)
    }

    /// [`ring::work_slot`] on this lane's QID.
    pub(crate) fn work_slot(&self, producer: u32, first: bool) -> Result<fw::channels::WorkSlot> {
        ring::work_slot(&self.graph, self.qid(), producer, first)
    }
}

/// The client view of `pool` in `vm`: none while the pool is vacant.
fn pool_view(pool: &M5Pool, vm: &mmu::Vm) -> Result<Option<mmu::KernelMapping>> {
    if pool.vacant() {
        return Ok(None);
    }
    pool.map_client(vm).map(Some)
}

/// A lane's graph object for `context`, initialized ([`super::compute::initialize_m5_graph`]).
fn new_graph(
    alloc: &Allocator<'_>,
    context: &Context,
    pointers: &PointerLayout,
) -> Result<KernelObject> {
    let mut graph = alloc.kernel(
        GRAPH_BYTES,
        PAGE as u64,
        mmu::PROT_GPU_FW_SHARED_RW,
        CpuMap::WriteCombined,
    )?;
    super::compute::initialize_m5_graph(&mut graph, context, pointers)?;
    Ok(graph)
}

/// The M5 Ultra arms of Neo's compute graph (S's `Graph::m5`): its objects are built here, on
/// the M5 encodings, and the arms in `compute::Graph`'s methods (cursors, stamp, owner record,
/// item-ring reset) read them; Neo's graph never reaches these.
impl Graph {
    /// Builds the M5 Ultra compute graph for `context` (the lane's graph, its descriptor slots
    /// with their canonical low alias, support page, operand state, preemption buffer and usage
    /// page) and, between the support page and the operand state as the lane always allocated
    /// them, its pool (`pool`).
    fn new_m5(
        alloc: &Allocator<'_>,
        context: &Context,
        gpu: &Gpu,
        pointers: &PointerLayout,
        pool: impl FnOnce() -> Result<ComputePool>,
    ) -> Result<(Self, ComputePool)> {
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let cpu = CpuMap::WriteCombined;
        let graph = new_graph(alloc, context, pointers)?;
        let descriptors = alloc.kernel(DEPTH * DESCRIPTOR_BYTES, PAGE as u64, prot, cpu)?;
        // The canonical low arena, as for every descriptor ring, and the same address in the VM.
        let low = descriptors.alias_in(
            alloc.uat.kernel_lower_vm(),
            alloc.domain(Domain::Canonical),
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
        )?;
        let support = alloc.kernel(SUPPORT_BYTES, PAGE as u64, prot, cpu)?;
        let pool = pool()?;
        let operand = alloc.lower(
            gpu.profile.operand.bytes,
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
            cpu,
        )?;
        let preempt = alloc.kernel(compute::PREEMPT_BYTES, PAGE as u64, prot, cpu)?;
        let usage = alloc.kernel(PAGE, PAGE as u64, prot, cpu)?;
        let ring = DescriptorRing {
            low,
            object: Storage::Kernel(descriptors),
        };
        let graph = Self::with_m5_objects(
            ring,
            [preempt, usage, operand, support, graph],
            *pointers,
            alloc.domain(Domain::Private),
        );
        Ok((graph, pool))
    }

    /// The descriptor slots, which an M5 graph always has.
    fn descriptors(&self) -> Result<&KernelObject> {
        self.ring.as_ref().map(|ring| &*ring.object).ok_or(EIO)
    }
    fn descriptors_mut(&mut self) -> Result<&mut KernelObject> {
        self.ring.as_mut().map(|ring| &mut *ring.object).ok_or(EIO)
    }
    /// Clears every descriptor slot of a recycled lane (`Queue::recycle_settled`'s `m5` arm).
    pub(in crate::g17::queue) fn clear_descriptors_m5(&mut self) -> Result {
        self.descriptors_mut()?.bytes_mut().fill(0);
        fence(Ordering::SeqCst);
        Ok(())
    }
    /// The descriptor of slot `slot`: its firmware (high) and canonical low addresses.
    fn descriptor_slot_vas(&self, slot: usize) -> Result<(u64, u64)> {
        let ring = self.ring.as_ref().ok_or(EIO)?;
        let offset = (slot * DESCRIPTOR_BYTES) as u64;
        Ok((ring.object.gpu_va() + offset, ring.low.iova() + offset))
    }
    /// The times the firmware wrote into descriptor slot `slot`.
    fn descriptor_times<const N: usize>(&self, slot: usize) -> Result<[u32; N]> {
        self.descriptors()?
            .read_words::<N>(slot * DESCRIPTOR_BYTES + DESCRIPTOR_TIMES)
    }

    /// [`ring::idle_at`].
    fn idle_at(&self, target: u32) -> Result<bool> {
        ring::idle_at(&self.graph, self.pointers.producer, target)
    }

    /// [`ring::expose`] on QID `qid`.
    fn expose(
        &self,
        qid: u8,
        port: &Port,
        claim: crate::g17::port::OuterClaim<'_>,
        producer: u32,
        first: bool,
        submission: Option<&HardwareSubmission>,
    ) -> Result<u32> {
        ring::expose(
            &self.graph,
            self.pointers.producer,
            qid,
            port,
            claim,
            producer,
            first,
            submission,
        )
    }

    /// `Graph::bind`'s `m5` arm: the binding of `context`'s VM to this queue. With the kick ring's
    /// view and the pool's (`None` while the pool is vacant), which the caller mapped, it maps the
    /// descriptors and the operand state at their canonical addresses and client-private views of
    /// the preemption buffer and the usage page, the GPU-written kernel buffers. Neo's job-context
    /// aliases and queue context are not the M5 Ultra's.
    pub(in crate::g17::queue) fn bind_m5(
        &self,
        context: Arc<Context>,
        kick_alias: mmu::KernelMapping,
        pool_alias: Option<mmu::KernelMapping>,
    ) -> Result<super::compute::Binding> {
        let shared = mmu::PROT_GPU_SHARED_RW;
        let vm = context.vm();
        let ring = self.ring.as_ref().ok_or(EIO)?;
        let descriptors = ring.object.map_alias(vm, ring.low.iova(), shared)?;
        let operand = self.operand.map_alias(vm, self.operand.gpu_va(), shared)?;
        let preempt = self
            .preempt
            .alias_in(vm, self.private.clone(), PAGE as u64, shared)?;
        let usage = self
            .usage
            .alias_in(vm, self.private.clone(), PAGE as u64, shared)?;
        Ok(super::compute::Binding {
            _kick_alias: kick_alias,
            pool_alias,
            descriptors: Some(descriptors),
            preempt,
            usage,
            operand,
            _queue_context: None,
            context,
        })
    }

    /// Publishes the queue's mappings to every context naming their roots: the descriptors' low
    /// alias, `binding`'s client views (the kick ring, the descriptors, the pool, the operand
    /// state, the preemption buffer, the usage page), then each object of the graph.
    fn publish_m5_mappings(&self, binding: &super::compute::Binding) -> Result {
        let ring = self.ring.as_ref().ok_or(EIO)?;
        ring.low.publish_to_contexts();
        let views = [
            Some(&binding._kick_alias),
            binding.descriptors.as_ref(),
            binding.pool_alias.as_ref(),
            Some(&binding.operand),
            Some(&binding.preempt),
            Some(&binding.usage),
        ];
        for view in views.into_iter().flatten() {
            view.publish_to_contexts();
        }
        for object in [
            &*self.graph,
            &*ring.object,
            &self.support,
            &self.operand,
            &self.preempt,
            &self.usage,
        ] {
            object.publish_to_contexts()?;
        }
        Ok(())
    }
}

/// The M5 Ultra lane's own state in its compute queue (`compute::Queue::m5`), beside the Neo
/// fields the lane shares: the profile; the adoption state (the class whose outer ring last
/// carried the queue, `rebound` and `unmapped`), which stays the lane's while
/// it parks and is adopted; and the pool count each descriptor slot's kick waits for. The rest of
/// the lane's lifecycle is Neo's fields: `owner`, `binding`,
/// `retained_owner` (the previous owner's pages), `installed`, `ordinal`, `pool_state`, `active`
/// (a quarantined lane's failed kicks stay there, `failure_deferred`), `quarantined` with
/// `quarantine_error`, `released` (the logical queue closed), `fault_stamp` (the firmware failed
/// a kick), `awaiting_witness`, `retire_pending`, `exit` (its context's kill, from its publication
/// to its settlement), `needs_fresh_backing` (a recycled lane, until its
/// reinstallation is published) and `m5_install` (the installation or reinstallation in flight).
pub(crate) struct LaneState {
    gpu: Gpu,
    /// The class whose outer work ring last carried this queue's first slot.
    class: u8,
    /// Set by an adoption until the binding's first kick: whether the class (and so the outer
    /// work ring) changed.
    rebound: Option<bool>,
    /// Set once this binding's client views are unmapped, after it parks.
    unmapped: Unmapped,
    /// The pool's submission count that covers the kick in each descriptor slot, beside the
    /// slots it belongs to: it runs across owners and kills, so no ticket field gives it.
    pool_targets: [u64; DEPTH],
}

impl LaneState {
    fn new(gpu: Gpu, class: u8, rebound: Option<bool>) -> Result<KBox<Self>> {
        Ok(KBox::new(
            Self {
                gpu,
                class,
                rebound,
                unmapped: Unmapped::new()?,
                pool_targets: [0; DEPTH],
            },
            GFP_KERNEL,
        )?)
    }
}

/// An installed compute lane without an owner: everything the
/// firmware's installed queue still names, and the previous owner's scheduler page and work
/// storage its record names until the next owner rewrites it. Never dropped while the session
/// runs: its QID stays configured with this ring.
pub(crate) struct ParkedLane {
    gpu: Gpu,
    kick: kick::Queue,
    graph: Graph,
    /// The `m5` arm of Neo's compute pool.
    pool: ComputePool,
    class: u8,
    pages: crate::g17::context::FirmwarePages,
    unmapped: Unmapped,
    pool_state: PoolState,
    /// When it parked: the vacate pass releases the least recently parked pools first.
    parked_at: u64,
    /// Recycled after a kill (`Queue::recycle_killed`): adoptable only once its pool is
    /// vacant, and vacated whatever the warm cap.
    needs_fresh_backing: bool,
}

impl ParkedLane {
    pub(crate) fn qid(&self) -> u8 {
        self.kick.id().qid()
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
        self.pool_state.phase() == PoolPhase::Populated && self.pool.populated()
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
        Ok(self.pool.m5_mut()?.vacate())
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
    /// A compute lane's binding: its client views and its context.
    compute: Option<super::compute::Binding>,
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
            compute: None,
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
        // Every view first (each unmaps as it drops; a lane's binding drops its views before its
        // context), then the storage behind a pair's, then the context; only then may a new
        // owner map the lane.
        drop(core::mem::replace(&mut self.aliases, KVec::new()));
        drop(self.compute.take());
        drop(self.render.take());
        drop(self.context.take());
        self.unmapped.0.store(true, Ordering::Release);
    }
}

/// The M5 Ultra arms of Neo's compute queue: its lane's construction, adoption and parking,
/// installation, publication transaction, strict retirement, failure, drain, and the kill's
/// candidate and settlement, on the lane's hardware (the `m5` arms of `compute::Graph` and
/// `ComputePool`) and Neo's lifecycle fields ([`LaneState`] says which). Only [`Queue::new_m5`] and
/// [`Queue::adopt_m5`] build a queue with `m5` set; Neo's queues never reach these.
impl Queue {
    /// Builds the M5 Ultra compute queue of QID `id` for `context` and maps it into the context's
    /// VM. Nothing is visible to the firmware yet, so a failure here frees everything; the caller
    /// cancels the unpublished QID. Boxed: the queue holds a dozen objects, and the boot-time
    /// qualification builds it deep in probe's stack.
    pub(crate) fn new_m5(
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        id: kick::Id,
        owner: u64,
        gpu: Gpu,
        pointers: &PointerLayout,
    ) -> Result<KBox<Self>> {
        let vm = context.vm();
        let mut kick = kick::Queue::lower(alloc, id)?;
        let kick_alias = kick.map_client(vm)?;
        let geometry = fw::m5::pool::PoolGeometry::of(gpu.pool_blocks, gpu.block_bytes())
            .map_err(|_| EINVAL)?;
        let (mut graph, pool) = Graph::new_m5(alloc, &context, &gpu, pointers, || {
            ComputePool::new_m5(alloc, id.qid(), geometry)
        })?;
        let pool_alias = pool_view(pool.m5()?, vm)?;
        let binding = graph.bind(context, kick_alias, pool_alias)?;
        let readers = Arc::new(core::sync::atomic::AtomicU32::new(0), GFP_KERNEL)?;
        let lane = LaneState::new(gpu, u8::MAX, None)?;
        let queue = KBox::new(
            Self::assemble(
                Some(owner),
                Some(binding),
                kick,
                pool,
                graph,
                readers,
                Some(lane),
            ),
            GFP_KERNEL,
        )?;
        queue.publish_m5_mappings()?;
        Ok(queue)
    }

    /// Publishes the complete mapping set to every context naming its roots, before any record
    /// names it. The kick ring's own aliases are published when its first entry is installed.
    fn publish_m5_mappings(&self) -> Result {
        self.graph
            .publish_m5_mappings(self.binding.as_ref().ok_or(EINVAL)?)?;
        self.pool.m5()?.publish_to_contexts()
    }

    fn lane(&self) -> Result<&LaneState> {
        self.m5.as_deref().ok_or(EINVAL)
    }

    /// The bound owner's context, which an M5 queue always has.
    fn m5_context(&self) -> Result<&Arc<Context>> {
        self.context().ok_or(EINVAL)
    }

    /// Quarantines the lane with `error` unless it already is, both of Neo's quarantine fields
    /// together: nothing more is published, and every failed kick stays in `active`, pinning its
    /// command, until its drain ([`Self::rehabilitate`]).
    fn quarantine_m5(&mut self, error: Error) {
        self.quarantined = true;
        self.quarantine_error.get_or_insert(error);
    }

    /// Ends the quarantine, after the failed kicks drained.
    fn end_quarantine_m5(&mut self) {
        self.quarantined = false;
        self.quarantine_error = None;
        self.fault_stamp = None;
    }

    /// The failed kicks awaiting their drain, oldest first.
    fn failed(&self) -> impl Iterator<Item = &Active> {
        self.active.iter().filter(|active| active.failure_deferred)
    }

    /// The queue is bound to `context`.
    pub(crate) fn binds(&self, context: &Arc<Context>) -> bool {
        self.context()
            .is_some_and(|bound| Arc::ptr_eq(bound, context))
    }

    /// Adopts parked lane `index` for `context`'s logical queue `owner`: maps
    /// the lane into the new VM and rewrites the record's host-owned words. The firmware's
    /// queue is reconfigured at the binding's first kick ([`Self::publish_m5`]); the kick
    /// timestamp and the stamps continue. On failure the lane stays parked.
    pub(crate) fn adopt_m5(
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
            drop(lane.pool.m5_mut()?.vacate());
        }
        let recycled = lane.needs_fresh_backing;
        let class_changed = lane.class != context.policy().priority();
        let state = LaneState::new(lane.gpu, lane.class, Some(class_changed))?;
        let kick_alias = lane.kick.map_client(context.vm())?;
        let pool_alias = pool_view(lane.pool.m5()?, context.vm())?;
        let binding = lane.graph.bind(context.clone(), kick_alias, pool_alias)?;
        let readers = Arc::new(core::sync::atomic::AtomicU32::new(0), GFP_KERNEL)?;
        let slot = KBox::<Self>::new_uninit(GFP_KERNEL)?;
        // The lane is idle and its owner's release consumed: no reader of the record remains.
        lane.graph.set_owner(&context)?;
        let lane = parked.remove(index).map_err(|_| EINVAL)?;
        if recycled {
            pr_info!(
                "G17 M5: lane {}: recycled after a kill, adopted by queue {} (context {})\n",
                lane.qid(),
                owner,
                context.id()
            );
        }
        let ParkedLane {
            kick,
            graph,
            pool,
            pages,
            pool_state,
            ..
        } = lane;
        let mut queue = KBox::write(
            slot,
            Self::assemble(
                Some(owner),
                Some(binding),
                kick,
                pool,
                graph,
                readers,
                Some(state),
            ),
        );
        queue.retained_owner = Some(pages);
        queue.installed = true;
        queue.pool_state = pool_state;
        // A recycled lane keeps needing its fresh backing until its reinstallation
        // ([`Self::reinstall`]) publishes; parked before that, it is reinstalled by its next owner.
        queue.needs_fresh_backing = recycled;
        if let Err(error) = queue.publish_m5_mappings() {
            let (lane, _binding) = queue.park_m5()?;
            parked.push_within_capacity(lane).map_err(|_| ENOMEM)?;
            return Err(error);
        }
        // The rewritten record names this context's scheduler page.
        context.mark_published();
        Ok(queue)
    }

    /// The queue's logical queue closed and its lane is idle: no kick in flight or failed, the
    /// inner ring consumed to its producer, the pool's count complete and the stamp at the
    /// lane's last kick. A quarantined lane never parks.
    pub(crate) fn parkable(&self) -> Result<bool> {
        if !self.released
            || !self.installed
            || self.quarantined
            || self.fault_stamp.is_some()
            || self.active.len != 0
        {
            return Ok(false);
        }
        if !self.graph.idle_at(self.graph.cursors()?.producer)?
            || self.pool.m5()?.completed_submissions()? != self.pool.m5()?.submitted()?
        {
            return Ok(false);
        }
        let next = self.kick.timestamp();
        Ok(next.get() == KickTimestamp::FIRST.get()
            || retirement::stamp_covers(self.stamp()?, next.prev().get()))
    }

    /// Logs the park witnesses ([`Self::parkable`]) one by one, for a stalled release.
    pub(crate) fn report_park(&self) {
        let next = self.kick.timestamp();
        let due = self
            .context()
            .is_some_and(|context| self.exit_candidate_m5(context, u64::MAX));
        pr_warn!(
            "G17 M5: lane {}: closed {} installed {} quarantine {:?} faulted {} active {} failed {}, kill: due {} published {} settled {}, cursors {:x?}, idle {:?}, pool {:?}/{:?}, stamp {:x?} for kick {:#x}\n",
            self.qid(), self.released, self.installed, self.quarantine_error, self.fault_stamp.is_some(),
            self.live().count(), self.failed().count(), due, self.exit.is_some(),
            self.exit.as_ref().is_some_and(exit::Transaction::released), self.graph.cursors(),
            self.graph.cursors().and_then(|cursors| self.graph.idle_at(cursors.producer)),
            self.pool.m5().and_then(M5Pool::completed_submissions),
            self.pool.m5().and_then(M5Pool::submitted), self.stamp(), next.prev().get(),
        );
    }

    /// The lane needs nothing more before its context's release: parkable, or quarantined with
    /// the firmware idle on its ring. A drain still awaited does not hold the release back, as a
    /// quarantined Neo queue does not (`Queue::idle_for_context` waits for the firmware, not for
    /// the witness): the healthy lanes of the context park, and this one parks only if it drained
    /// before the release was consumed ([`Self::end_witness_wait`]).
    /// A lane whose context is to be killed waits for the kill ([`Self::kill_holds_release`]).
    pub(crate) fn settled(&self) -> Result<bool> {
        if !self.quarantined {
            return self.parkable();
        }
        Ok(self.released
            && self.graph.idle_at(self.graph.cursors()?.producer)?
            && !self.kill_holds_release(crate::g17::now_ns()))
    }

    /// `Queue::exit_candidate`'s `m5` arm: the firmware may still run this closed lane's failed
    /// kicks. They are held with no drain awaited, as after a timeout or its VM's failure
    /// ([`Failure::Final`]), and an unreadable witness ([`Failure::Ambiguous`]); a settled
    /// failure awaits its drain instead and is never killed. The firmware consumes a kick's items
    /// when it launches its work, so an idle ring does not say the work ended: a measured compute
    /// spin kept the GPU busy after its timeout. Its context is killed through Neo's
    /// `Firmware::publish_compute_exit` once it has been closed for
    /// [`exit::QUARANTINED_DELAY_NS`] (`age`), as a quarantined Neo queue's is.
    pub(in crate::g17::queue) fn exit_candidate_m5(
        &self,
        context: &Arc<Context>,
        age: u64,
    ) -> bool {
        self.binds(context)
            && self.released
            && self.quarantined
            && !self.awaiting_witness
            && self.failed().next().is_some()
            && self.exit.is_none()
            && age >= exit::QUARANTINED_DELAY_NS
    }

    /// The context's scheduler release waits for its kill: due, or published and not yet
    /// settled for [`exit::M5_SETTLE_NS`] at most (the acknowledgement's window, then the
    /// drain's). Past that bound the release goes, and the lane stays quarantined
    /// ([`Self::end_witness_wait`]).
    fn kill_holds_release(&self, now: u64) -> bool {
        if self
            .context()
            .is_some_and(|context| self.exit_candidate_m5(context, u64::MAX))
        {
            return true;
        }
        self.exit
            .as_ref()
            .is_some_and(|exit| exit.open_within(now, exit::M5_SETTLE_NS))
    }

    /// `Queue::settle_exit`'s `m5` arm: settles the acknowledged kill of this lane's context with
    /// the exact predicate: recovery closed, the context's scheduler page showing it
    /// killed (+0x34 == 1), and the failed kicks' exact drain ([`Self::drained`]). The
    /// acknowledgement and the killed state say the firmware stopped the context, not that it
    /// retired the kicks, so Neo's acceptance of their completion records does not apply. Until
    /// the page shows the kill, the transaction is unproven and re-read on later passes; until the
    /// drain, it stays acknowledged. Then the kicks are given up ([`Self::give_up_drained`]), the
    /// kill is settled, and the lane is recycled through Neo's `recycle_killed`: returning it as
    /// it was failed its next owner's first kick at once, each time, with a zero-duration record.
    /// Unreadable drain witnesses end the transaction with the kicks held, and a ring that
    /// cannot be reset keeps the quarantine.
    pub(in crate::g17::queue) fn settle_exit_m5(
        &mut self,
        recovery_closed: bool,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        if !recovery_closed || !self.exit.as_ref().is_some_and(exit::Transaction::settling) {
            return Ok(());
        }
        let Some(context) = self.context().cloned() else {
            return Ok(());
        };
        let Ok(state) = exit::State::read(&context) else {
            return Ok(());
        };
        if state.killed != 1 {
            self.exit.as_mut().ok_or(EIO)?.mark_unproven();
            return Ok(());
        }
        let (error, count) = (self.quarantine_error.unwrap_or(EIO), self.failed().count());
        let drained = self.give_up_drained(complete_qos, defer)?;
        if drained == Some(false) {
            return Ok(());
        }
        let exit = self.exit.as_mut().ok_or(EIO)?;
        exit.mark_released();
        if drained.is_none() {
            return Ok(());
        }
        let waited = crate::g17::now_ns().saturating_sub(exit.published_at());
        match self.recycle_killed() {
            Ok(true) => pr_info!("G17 M5: lane {}: kill of context {} acknowledged, killed (+0x34 = 1) and its {} failed kicks ({:?}) drained, {} ms after its publication; the lane is recycled\n",
                self.qid(), context.id(), count, error, waited / 1_000_000),
            Ok(false) => {}
            Err(reset) => pr_warn!("G17 M5: lane {}: its ring could not be reset after its context's kill ({:?}); the lane stays quarantined\n",
                self.qid(), reset),
        }
        Ok(())
    }

    /// Splits a parked queue ([`Self::parkable`], its context's release consumed) into its lane
    /// and its binding. The lane keeps the context's scheduler page and work storage, which the
    /// record names until the next owner rewrites it.
    pub(crate) fn park_m5(mut self: KBox<Self>) -> Result<(ParkedLane, Binding)> {
        let (Some(lane), Some(binding), Some(owner)) =
            (self.m5.take(), self.binding.take(), self.owner)
        else {
            // Not a bound M5 lane: kept, as an installed queue always is, never dropped here.
            let _ = KBox::into_raw(self);
            return Err(EINVAL);
        };
        let queue = KBox::into_inner(self);
        let pages = binding.context.retain_firmware_pages();
        let LaneState {
            gpu,
            class,
            unmapped,
            ..
        } = KBox::into_inner(lane);
        Ok((
            ParkedLane {
                gpu,
                kick: queue.kick,
                graph: queue.graph,
                pool: queue.pool,
                class,
                pages,
                unmapped: unmapped.clone(),
                pool_state: queue.pool_state,
                parked_at: crate::g17::now_ns(),
                needs_fresh_backing: queue.needs_fresh_backing,
            },
            Binding {
                aliases: KVec::new(),
                compute: Some(binding),
                render: None,
                context: None,
                owner,
                unmapped,
            },
        ))
    }

    /// An adopted lane whose class differs from its previous binding's, until its first kick.
    pub(crate) fn class_changed(&self) -> bool {
        self.lane().is_ok_and(|lane| lane.rebound == Some(true))
    }

    /// The timestamp the lane's next kick takes.
    pub(crate) fn next_kick(&self) -> u64 {
        self.kick.timestamp().get()
    }

    /// The QID and the host identity that reserved it: the compute entry's reservation.
    pub(crate) fn id(&self) -> kick::Id {
        self.kick.id()
    }
    pub(crate) fn gpu(&self) -> Result<&Gpu> {
        Ok(&self.lane()?.gpu)
    }
    /// Its logical queue is open.
    pub(crate) fn open(&self) -> bool {
        !self.released
    }
    /// Kicks in flight the lane has not failed: a healthy lane's, or those a failed publication
    /// left before the lane failed them.
    pub(crate) fn unfailed_kicks(&self) -> bool {
        self.live().next().is_some()
    }
    /// A settled failure's drain is awaited ([`Self::rehabilitate`]). The event worker does not
    /// poll for it: a quarantined queue is not in flight (`Queue::in_flight`); each event pass
    /// checks it.
    pub(crate) fn awaiting_witness(&self) -> bool {
        self.awaiting_witness
    }
    /// The context's scheduler release was consumed while this quarantined lane still awaited
    /// its failed kicks' drain, a settled failure's or its kill's: the lane stays quarantined,
    /// leaked under the cap, its kicks pinned. The firmware was idle on its ring (`Self::settled`)
    /// and no longer names the context, and nothing else can witness the drain: on M5 neither a
    /// kill's acknowledgement nor the scheduler page's killed state proves the firmware finished
    /// reading (S `queue/compute/exit.rs` `settle_exit`). An unsettled kill ends here, its later
    /// acknowledgement unmatched. Returns whether a wait ended.
    pub(crate) fn end_witness_wait(&mut self) -> bool {
        let kill = self.exit.as_mut().filter(|exit| !exit.released());
        let killed = kill.is_some();
        if let Some(exit) = kill {
            exit.mark_released();
        }
        if !self.awaiting_witness && !killed {
            return false;
        }
        self.awaiting_witness = false;
        pr_warn!("G17 M5: lane {}: its context was released before its {} failed kicks drained; the lane stays quarantined\n",
            self.qid(), self.failed().count());
        true
    }
    /// The oldest command in flight that the lane has not failed.
    pub(crate) fn oldest_packet(&self) -> Option<Arc<Packet>> {
        self.live().next().map(|active| active.packet.clone())
    }
    /// Room for another kick: installed, healthy, open and below [`DEPTH`].
    pub(crate) fn has_room(&self) -> bool {
        self.installed && !self.quarantined && !self.released && self.active.len < DEPTH
    }
    pub(crate) fn quarantined(&self) -> Option<Error> {
        self.quarantine_error
    }
    /// Its logical queue closed; the lane stays installed.
    pub(crate) fn close(&mut self) {
        self.released = true;
    }
    /// Kicks published since installation.
    pub(crate) fn kicks(&self) -> u64 {
        self.kick.timestamp().get() - KickTimestamp::FIRST.get()
    }

    /// Logs what the firmware made of a kick a restart interrupted, which fails however its
    /// witnesses read (`Completion::note_interrupted`): its record, and its descriptor's times,
    /// which the firmware writes only for a kick that launched work. Neither proves the dispatch
    /// whole: a measured interrupted kick ran to its record's end and still left 64 workgroups
    /// wrong.
    fn report_interrupted(
        &self,
        front: &Active,
        ticket: &Ticket,
        outcome: core::result::Result<[u64; 2], Error>,
    ) {
        let words = slot_of(ticket).and_then(|slot| self.graph.descriptor_times::<4>(slot));
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
            self.qid(), ticket.kick, innocence(front), how, outcome.err(), record, words, launched);
    }
    /// Notes the DRM kicks in flight for recovery's attribution, read while the firmware is
    /// halted ([`recovery::Progress`]): the lane runs them in order, so the newest one whose
    /// descriptor has a start time (cleared at its publication, written once it launched work)
    /// is the one running, and with none the lane's work never started. A flush-only or
    /// empty-grid kick writes no times; as unstarted it can lose a blame, never gain one.
    pub(crate) fn note_progress(&self, progress: &mut recovery::Progress) {
        let mut running = None;
        fence(Ordering::Acquire);
        for active in self.live() {
            let Some(ticket) = active.ticket else {
                return;
            };
            let Ok(words) =
                slot_of(&ticket).and_then(|slot| self.graph.descriptor_times::<2>(slot))
            else {
                return;
            };
            let start = u64::from(words[0]) | (u64::from(words[1]) << 32);
            if start != 0 {
                running = Some((ticket.kick, start));
            }
            running.get_or_insert((ticket.kick, 0));
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
        self.graph.cursors()
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
        Result<u64>,
        Result<u64>,
        Result<bool>,
    )> {
        let front = self.live().next()?;
        let ticket = front.ticket?;
        let slot = slot_of(&ticket);
        Some((
            ticket,
            front.observed,
            slot.and_then(|slot| self.graph.descriptor_times::<4>(slot)),
            slot.and_then(|slot| Ok(self.lane()?.pool_targets[slot])),
            self.pool.m5().and_then(M5Pool::completed_submissions),
            front.work.drained(),
        ))
    }

    /// The queue's completion stamp.
    pub(crate) fn stamp(&self) -> Result<u32> {
        self.graph.stamp()
    }

    pub(crate) fn completed_submissions(&self) -> Result<u64> {
        self.pool.m5()?.completed_submissions()
    }

    /// The queue configuration: the installation's (`install`), the first kick's context and
    /// pool update, or an adopted lane's first kick: a new owner's context, scheduler and QoS
    /// identity on the queue's continuing completion value, reinstalled when its class changed
    /// (S `queue_config`).
    fn config(&self, install: bool, completion_seed: Option<u64>) -> Result<QueueConfig> {
        let context = self.m5_context()?;
        let rebound = (!install).then_some(self.lane()?.rebound).flatten();
        QueueConfig::new(&QueueConfigArgs {
            target: QueueConfigTarget::Compute,
            kick_ring_va: self.kick.low_va(),
            kick_ring_fw_va: self.kick.firmware_va(),
            qid: self.qid(),
            install: install || rebound == Some(true),
            // An adopted lane's completion value continues, unless no kick ever ran on it: then
            // its first kick is the lane's first and takes the fresh lane's seed.
            completion_seed: match rebound {
                Some(_) if self.kick.timestamp().get() != KickTimestamp::FIRST.get() => None,
                _ => completion_seed,
            },
            context_id: context.id().try_into()?,
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
            scheduler_va: context.scheduler_va(),
            policy: context.policy(),
            qos_slot: context.qos_id(),
            qos_update: install || rebound.is_some(),
            owner_pid: context.owner_pid(),
            context_update: !install,
        })
    }

    /// Installs the queue: its pool row, the initial configuration, the QID, and the first outer
    /// slot naming it. The firmware's consumption of the configuration is awaited off the device
    /// mutex ([`Self::install_consumed`]). From the publication on the queue is retained
    /// whatever happens; a failed installation leaves it quarantined. EAGAIN: no outer slot yet,
    /// and nothing was published.
    pub(crate) fn install(&mut self, host: &mut Host<'_>) -> Result {
        if self.installed || self.m5.is_none() {
            return Err(EINVAL);
        }
        let context = self.m5_context()?.clone();
        let claim = host.port.reserve_compute(context.policy().priority())?;
        host.init
            .install_m5_pool(self.pool.id(), self.pool.m5()?.descriptor()?)?;
        let config = self.config(true, None)?;
        ring::stage_install(&mut self.graph.graph, config)?;
        host.qids.publish(self.kick.id())?;
        self.installed = true;
        // The configuration is the ring's first item.
        self.m5_install = Some(1);
        // The installation names the context's scheduler page: its close releases it.
        context.mark_published();
        if let Some(lane) = self.m5.as_deref_mut() {
            lane.class = context.policy().priority();
        }
        let result = self
            .graph
            .expose(self.qid(), host.port, claim, 1, true, None)
            .map(|_| ());
        if let Err(error) = result {
            self.quarantine_m5(error);
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
    /// a fresh lane has. A reinstallation is due while the adopted lane `needs_fresh_backing`
    /// (`Queue::recycle_killed`); its publication ends that. EAGAIN: no outer slot yet,
    /// nothing published. A failure after the publication quarantines the queue.
    pub(crate) fn reinstall(&mut self, host: &mut Host<'_>) -> Result {
        if !self.needs_fresh_backing {
            return Ok(());
        }
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        let context = self.m5_context()?.clone();
        let priority = context.policy().priority();
        let producer = self.graph.cursors()?.producer;
        if !self.graph.idle_at(producer)? {
            return Err(EIO);
        }
        let claim = host.port.reserve_compute(priority)?;
        let config = self.config(true, None)?;
        let target = ring::stage_reinstall(&mut self.graph.graph, producer, config)?;
        let lane = self.m5.as_deref_mut().ok_or(EINVAL)?;
        let first = lane.class != priority;
        lane.class = priority;
        lane.rebound = Some(false);
        self.m5_install = Some(target);
        self.needs_fresh_backing = false;
        context.mark_published();
        let result = self
            .graph
            .expose(self.qid(), host.port, claim, target, first, None)
            .map(|_| ());
        if let Err(error) = result {
            self.quarantine_m5(error);
        }
        result
    }

    /// Whether the firmware consumed the installation's or the reinstallation's configuration
    /// ([`Self::install`], [`Self::reinstall`]: `m5_install`); an adopted lane with no
    /// reinstallation due has none and is consumed at once. A crash, or the configuration still
    /// unconsumed once the wait `expired` ([`INSTALL_WAIT_MS`]), quarantines the queue.
    pub(crate) fn install_consumed(&mut self, crashed: &AtomicBool, expired: bool) -> Result<bool> {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        let adopted = self.lane()?.rebound.is_some();
        let consumed = match self.m5_install {
            Some(target) => self.graph.cursors()?.consumer == target,
            None => self.installed && !self.needs_fresh_backing,
        };
        if consumed {
            if let Some(target) = self.m5_install.take() {
                if adopted {
                    pr_info!(
                        "G17 M5: lane {}: reinstalled for queue {} (consumed at {:#x})\n",
                        self.kick.id().qid(),
                        self.owner.unwrap_or(0),
                        target
                    );
                }
            }
            return Ok(true);
        }
        let error = match () {
            _ if crashed.load(Ordering::Acquire) => EIO,
            _ if expired => ETIMEDOUT,
            _ => return Ok(false),
        };
        self.quarantine_m5(error);
        Err(error)
    }

    /// Whether a command whose plan needs `blocks` must back the pool first ([`Self::grow`]):
    /// it is smaller, vacant, or being released.
    pub(crate) fn needs_pool(&self, blocks: usize) -> bool {
        self.pool
            .m5()
            .map_or(true, |pool| needs_pool(pool, &self.pool_state, blocks))
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
        if !self.installed || self.released {
            return Err(EINVAL);
        }
        let bounds = {
            let gpu = &self.lane()?.gpu;
            (gpu.pool_blocks, gpu.max_pool_blocks)
        };
        let binding = self.binding.as_mut().ok_or(EINVAL)?;
        let (kick, graph, faulted) = (&self.kick, &self.graph, self.fault_stamp.is_some());
        let in_flight = self.active.len != 0;
        let idle = |pool: &M5Pool| -> Result<bool> {
            let next = kick.timestamp();
            // The queue's completion stamp ([`Self::stamp`]), read last, as the growth always did.
            let stamp = || graph.stamp();
            let busy = faulted
                || in_flight
                || !graph.idle_at(graph.cursors()?.producer)?
                || pool.completed_submissions()? != pool.submitted()?
                || (next.get() != KickTimestamp::FIRST.get()
                    && !retirement::stamp_covers(stamp()?, next.prev().get()));
            Ok(!busy)
        };
        let site = PoolSite {
            qid: kick.id().qid(),
            pool: self.pool.m5_mut()?,
            state: &mut self.pool_state,
            vm: binding.context.vm(),
            views: PoolView::Field(&mut binding.pool_alias),
            quarantine: &mut self.quarantine_error,
        };
        let grown = grow_pool(alloc, host.init, notify, site, idle, blocks, bounds);
        // A failure after the release quarantined the lane (`PoolSite::quarantine`).
        self.quarantined |= self.quarantine_error.is_some();
        let grown = grown?;
        if let Some(grown) = &grown {
            pr_info!(
                "M5 lane {}: pool backed with {} blocks (from {}) for queue {}\n",
                self.qid(),
                grown.to,
                grown.from,
                self.owner.unwrap_or(0)
            );
        }
        Ok(grown)
    }

    /// Publishes DRM command `packet` ([`Self::transact`]) and keeps its kick in flight, Neo's
    /// `Active` with its ticket, until it retires or fails. EAGAIN: no room yet, and nothing
    /// changed.
    pub(crate) fn publish_m5(
        &mut self,
        host: &mut Host<'_>,
        dispatch: &Dispatch,
        work: &Arc<WorkStateLease>,
        epoch: u64,
        packet: Arc<Packet>,
    ) -> Result {
        let kick = self.transact(
            host,
            dispatch,
            work,
            packet.order.sequence,
            epoch,
            Some(&packet),
        )?;
        // Room was checked before anything became visible.
        self.active
            .push(Active {
                packet,
                published: kick.published,
                work: work.clone(),
                ticket: Some(kick.ticket),
                observed: None,
                observed_at: None,
                failure_deferred: false,
                qos_pending: kick.qos,
            })
            .map_err(|_| ENOMEM)?;
        kick.outer.map(|_| ())
    }

    /// The one M5 publication transaction, which DRM publication ([`Self::publish_m5`]) and the
    /// boot-time qualification (`g17/m5_test.rs`) both run: descriptor, notification and kick
    /// entry, the inner items, then the outer slot and the host command. `work`
    /// holds the kick's work-state node; `epoch` is the recovery generation the caller checked
    /// with recovery closed; `packet` is the DRM command, if any. Everything fallible precedes the
    /// first firmware-visible store; a failure after it quarantines the queue and is returned
    /// in the kick (`Kick::outer`), which the caller records as in flight. EAGAIN: no room yet,
    /// and nothing changed.
    pub(crate) fn transact(
        &mut self,
        host: &mut Host<'_>,
        dispatch: &Dispatch,
        work: &Arc<WorkStateLease>,
        submission: u64,
        epoch: u64,
        packet: Option<&Arc<Packet>>,
    ) -> Result<Kick> {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        if !self.installed || self.released {
            return Err(EINVAL);
        }
        if self.active.len >= DEPTH {
            return Err(EAGAIN);
        }
        let owner = self.owner.ok_or(EINVAL)?;
        let gpu = self.lane()?.gpu;
        let context = self.m5_context()?.clone();
        let (preempt_va, usage_va) = match self.binding.as_ref() {
            Some(binding) => (binding.preempt.iova(), binding.usage.iova()),
            None => return Err(EINVAL),
        };
        let policy = context.policy();
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
        let class = self.lane()?.class;
        let timestamp = self.kick.timestamp();
        let slot = timestamp.slot() % DEPTH;
        let (high, low) = self.graph.descriptor_slot_vas(slot)?;
        let work_word = work.word();
        let work_node = work.node_va()?;
        let stamps = self.graph.support.gpu_va() + STAMPS as u64;
        let status = self.graph.support.gpu_va() + STATUS as u64;
        let args = DescriptorArgs {
            registers: RegisterArgs {
                auxiliary_allocation: 0,
                preempt_va,
                cdm_va: dispatch.cdm_va,
                operand_va: self.graph.operand.gpu_va(),
                usage_va,
                free_list_slot: u32::from(self.pool.id()),
                context_id: context.id().try_into()?,
                work_state: work_word,
                object_ids: compute::OBJECT_IDS,
                resource_policy: dispatch.resources.policy,
                preempt_state_offset: gpu.profile.preempt_state_offset,
                operand: gpu.profile.operand,
                resource_control: dispatch.resources.control,
                dispatch_control: gpu.profile.dispatch_control,
                auxiliary_control: gpu.profile.auxiliary_control,
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
            usage_fw_va: self.graph.support.gpu_va() + USAGE as u64,
            completion_identity: compute::COMPLETION_IDENTITY,
        };
        // The slot's previous kick retired: firmware no longer reads its descriptor or record.
        self.graph
            .descriptors_mut()?
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
        let qid = self.qid();
        let (target, gate) = ring::stage_kick(
            &mut self.graph.graph,
            qid,
            cursors.producer,
            update,
            slot,
            timestamp,
        )?;
        let command =
            HardwareSubmission::compute(self.qid(), self.kick.low_va(), policy.priority(), first)?;
        let kick_args = KickArgs {
            qid: self.qid(),
            timestamp,
            parent: self.kick.parent(),
            barriers: &[],
            dependency_types: Some(&[compute::PARENT_DEPENDENCY]),
            descriptor_va: high,
            queue_va: self.graph.graph.gpu_va(),
            qos: KickQos {
                slot: context.qos_id(),
                class: compute::KICK_QOS_CLASS,
            },
            mcache: None,
            event_mask: compute::kick_event_mask(context.id(), gate)?,
            register_arrays: compute::bindings(low)?,
            scs_enabled: false,
            priority: policy.priority(),
        };
        let prepared = self.kick.prepare_entry(&kick_args)?;
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
        if let Some(lane) = self.m5.as_deref_mut() {
            lane.pool_targets[slot] = pool_target;
        }
        if let Some(packet) = packet {
            packet.mark_published();
        }
        let mut qos_row = None;
        let result = (|| {
            let publication =
                host.port
                    .qos_publish(self.qos_owner(), context.scheduler_va(), policy)?;
            qos_row = Some(publication.record);
            context.mark_published();
            fence(Ordering::SeqCst);
            // The work ring names the queue and the firmware hears of it before the host
            // command makes it runnable; whether the cores are powered is not a prerequisite.
            // The first slot naming the queue on a class's outer ring says so: an adopted lane
            // whose class changed starts on its new ring.
            self.graph.expose(
                self.qid(),
                host.port,
                claim,
                target,
                class != policy.priority(),
                Some(&command),
            )
        })();
        let ticket = Ticket {
            owner,
            submission,
            context_id: context.id(),
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
        if let Some(lane) = self.m5.as_deref_mut() {
            lane.class = policy.priority();
            lane.rebound = None;
        }
        self.ordinal = self.ordinal.saturating_add(1);
        if let Err(error) = result {
            // The kick stays in flight and is failed with the queue.
            self.quarantine_m5(error);
        }
        Ok(Kick {
            ticket,
            pool_target,
            published,
            qos: qos_row.is_some(),
            outer: result,
        })
    }

    /// Stages `tickets`, the qualification's own kicks of this lane, for the shared completion-ring
    /// scan, behind the lane's frontier, as Neo's `request_observations` stages a queue's DRM
    /// kicks (which the lane's DRM retirement uses). Returns whether any was staged.
    pub(crate) fn stage<'a>(
        &self,
        batch: &mut Batch,
        tickets: impl Iterator<Item = &'a Ticket>,
    ) -> Result<bool> {
        batch.stage_frontier(self.qid(), self.kick.timestamp().get())?;
        let stamp = self.stamp()?;
        let mut requested = false;
        for ticket in tickets {
            batch.request(ticket, retirement::stamp_covers(stamp, ticket.kick))?;
            requested = true;
        }
        Ok(requested)
    }

    /// `Queue::retire`'s `m5` arm, strict retirement: retires the kicks whose records the scan
    /// latched ([`Self::latch_observations`], before this, as for a Neo queue), oldest first,
    /// reporting each through `defer` as Neo's retirement does; `state` is the device's recovery
    /// state. A kick whose witnesses are missing or disagree stays in flight, for the scheduler's
    /// bounded timeout to fail. A zero-duration record (the firmware failed the kick) or a fault
    /// the firmware reported on this queue quarantines the queue ([`Self::fail`]). A quarantined
    /// lane retires nothing here: its failed kicks' drain is `release_witnessed`'s arm
    /// ([`Self::rehabilitate`]). Every scan runs it, whatever its event masks: there is no
    /// recordless pass for the masks to time.
    pub(in crate::g17::queue) fn retire_strict(
        &mut self,
        state: u32,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        if self.quarantined {
            return Ok(());
        }
        if self.fault_stamp.is_some() {
            return self.fail(EIO, Failure::Settled, defer);
        }
        self.retire_pending = false;
        while let Some(front) = self.active.front() {
            let ticket = front.ticket.ok_or(EIO)?;
            match self.check(&ticket, front.observed, &front.work, state) {
                Ok(Some(timestamps)) => {
                    if front.packet.completion.interrupted() {
                        self.report_interrupted(front, &ticket, Ok(timestamps));
                    }
                    complete_qos(self.qos_owner())?;
                    // Reported before the queue gives the kick up, as Neo's release does
                    // (`Pair::release_witnessed`): a refused report leaves it owned here.
                    defer(Deferred::Retired(
                        front.packet.completion.clone(),
                        Ok([timestamps[0], timestamps[1], 0, 0]),
                    ))?;
                    self.active.pop().ok_or(EIO)?;
                    // The GPU is retiring work: a watchdog of a queue beside it renews
                    // (`timeout::renew_shared`), as for a Neo compute queue's retirement.
                    crate::g17::recovery::note_retired();
                    if crate::module_parameters::m5_trace_completions.value() != 0 {
                        pr_info!(
                            "M5 retire q{} class {} kick {:#x}: fw start {} end {}\n",
                            self.qid(),
                            self.m5_context()
                                .map_or(u8::MAX, |context| context.policy().priority()),
                            ticket.kick,
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
                    if front.packet.completion.interrupted() {
                        self.report_interrupted(front, &ticket, Err(error));
                    }
                    let failure = if error == ENODATA {
                        Failure::Settled
                    } else {
                        Failure::Ambiguous
                    };
                    return self.fail(error, failure, defer);
                }
            }
        }
        Ok(())
    }

    /// The strict retirement predicate of the kick `ticket` names, with its completion record
    /// (`observed`) and work-state node (`work`), which DRM retirement ([`Self::retire_strict`])
    /// and the qualification both apply: `Some` with its record's timestamps once every witness
    /// agrees, `None` while any is missing or disagrees. The witnesses: its completion record, the
    /// stamp, the consumed item ring, the pool's count at its slot's target and the drained work
    /// node. The descriptor's times are not a witness: the firmware writes them only for a kick
    /// that launched work, which neither a flush-only stream nor a dispatch of an empty grid
    /// (CTS `indirect_dispatch.upload_buffer.empty_command`) does. Where it wrote them they are
    /// checked against the record as a diagnostic.
    pub(crate) fn check(
        &self,
        ticket: &Ticket,
        observed: Option<Observation>,
        work: &WorkStateLease,
        state: u32,
    ) -> Result<Option<[u64; 2]>> {
        if state != 0 {
            return Ok(None);
        }
        let stamp = self.stamp()?;
        let Some(timestamps) = retirement::poll(
            ticket,
            observed,
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
        let slot = slot_of(ticket)?;
        if self.pool.m5()?.completed_submissions()? < self.lane()?.pool_targets[slot]
            || !work.drained()?
        {
            return Ok(None);
        }
        let offset = slot * DESCRIPTOR_BYTES;
        let words = self
            .graph
            .descriptors()?
            .read_words::<4>(offset + DESCRIPTOR_TIMES)?;
        let descriptor = [
            u64::from(words[0]) | (u64::from(words[1]) << 32),
            u64::from(words[2]) | (u64::from(words[3]) << 32),
        ];
        if descriptor != [0; 2] && (timestamps[1] != descriptor[1] || timestamps[0] > descriptor[0])
        {
            kernel::pr_warn_once!(
                "G17 M5: queue {} kick {:#x}: descriptor times {:?} disagree with the record's {:?}\n",
                self.qid(), ticket.kick, descriptor, timestamps
            );
        }
        Ok(Some(timestamps))
    }

    /// The M5 Ultra's quarantine, with the failure's kind (`awaits_witness`): quarantines the
    /// queue with `error` and fails every kick in flight, each reported through `defer` as Neo's
    /// quarantine reports it (`Deferred::FailedOwned`). The kicks stay owned by the queue, pinning
    /// their commands, until they drain: a [`Failure::Settled`] one ends there
    /// ([`Self::rehabilitate`]); others keep the kicks and the quarantine for the session.
    /// `Queue::quarantine`'s `m5` arm is this with [`Failure::Final`].
    pub(crate) fn fail(
        &mut self,
        error: Error,
        failure: Failure,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        // The M5 Ultra's rule (`awaits_witness`): the first failure decides. Every kick carries
        // its ticket from publication, and an empty lane's witness is its idle ring
        // ([`Self::drained`]), so the work is always witnessable.
        self.awaiting_witness = awaits_witness(
            Some(failure),
            !self.quarantined,
            self.awaiting_witness,
            true,
        );
        self.quarantine_m5(error);
        let error = self.quarantine_error.unwrap_or(error);
        for active in self
            .active
            .iter_mut()
            .filter(|active| !active.failure_deferred)
        {
            // Reported before the kick counts as failed, as Neo's quarantine reports it; the queue
            // owns it either way.
            defer(Deferred::FailedOwned(
                active.packet.completion.clone(),
                error,
            ))?;
            active.failure_deferred = true;
        }
        Ok(())
    }

    /// Every failed kick retired exactly (recovery is closed, `release_witnessed`'s guard): the
    /// stamp covers the newest, the inner ring is consumed to its producer, the pool's count is
    /// complete and each kick's work node drained. The firmware names nothing of them any more.
    fn drained(&self) -> Result<bool> {
        if !self.graph.idle_at(self.graph.cursors()?.producer)?
            || self.pool.m5()?.completed_submissions()? != self.pool.m5()?.submitted()?
        {
            return Ok(false);
        }
        if let Some(newest) = self.failed().last() {
            if !retirement::stamp_covers(self.stamp()?, newest.ticket.ok_or(EIO)?.kick) {
                return Ok(false);
            }
        }
        for failed in self.failed() {
            if !failed.work.drained()? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// `Queue::release_witnessed`'s `m5` arm, entered past its guard (a witness awaited, recovery
    /// closed): ends a settled failure's quarantine once its kicks drained ([`Self::drained`]), as
    /// a Neo pair's exact drain ends its quarantine (`Pair::release_witnessed`): each kick is
    /// reported drained with its error, in Neo's order (`Deferred::Drained`: the pinned command
    /// released, then a fence the failure never reached failed), and only then given up; the queue
    /// is healthy again and a closed one parks. Witnesses that cannot be read make the failure
    /// Ambiguous: the quarantine stays, and so do the kicks, pinned.
    /// Each drained kick's QoS publication is completed first, as a Neo compute queue's exact drain
    /// completes it (`Queue::release_witnessed`) and a render pair's drained pass does
    /// (`Pair::retire`): left open, the QID's in-flight count and its owner's QoS row outlive the
    /// kick, and the lane's next owner with another QoS slot is refused (EBUSY) at every
    /// publication.
    pub(in crate::g17::queue) fn rehabilitate(
        &mut self,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        let error = self.quarantine_error.unwrap_or(EIO);
        match self.give_up_drained(complete_qos, defer)? {
            Some(true) => {}
            Some(false) => return Ok(()),
            None => {
                self.awaiting_witness = false;
                return Ok(());
            }
        }
        self.end_quarantine_m5();
        self.awaiting_witness = false;
        pr_info!(
            "G17 M5: lane {}: its failed kicks ({:?}) drained; the lane returns\n",
            self.qid(),
            error
        );
        Ok(())
    }

    /// Gives the failed kicks up once they drained exactly ([`Self::drained`]): each is reported
    /// drained with the quarantine's error, its QoS publication completed first, oldest first.
    /// Returns whether they drained; `None` when the drain witnesses cannot be read (logged),
    /// with every kick still held.
    fn give_up_drained(
        &mut self,
        complete_qos: &mut impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<Option<bool>> {
        match self.drained() {
            Ok(true) => {}
            Ok(false) => return Ok(Some(false)),
            Err(error) => {
                pr_warn!("G17 M5: lane {}: its failed kicks' drain witnesses unreadable ({:?}); the lane stays quarantined\n",
                    self.qid(), error);
                return Ok(None);
            }
        }
        let error = self.quarantine_error.unwrap_or(EIO);
        while let Some(front) = self.active.front().filter(|front| front.failure_deferred) {
            if front.qos_pending {
                complete_qos(self.qos_owner())?;
                self.active.iter_mut().next().ok_or(EIO)?.qos_pending = false;
            }
            let front = self.active.front().ok_or(EIO)?;
            defer(Deferred::Drained(
                front.packet.completion.clone(),
                Err(error),
            ))?;
            self.active.pop().ok_or(EIO)?;
        }
        Ok(Some(true))
    }

    /// The end of the last nonzero word of descriptor slot `slot`: how far the firmware wrote.
    pub(crate) fn descriptor_extent(&self, slot: usize) -> Result<usize> {
        let base = slot * DESCRIPTOR_BYTES;
        let mut end = 0;
        for offset in (0..DESCRIPTOR_BYTES).step_by(4) {
            if self
                .graph
                .descriptors()?
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
        let record: [u32; 6] = core::array::from_fn(|i| {
            ring::word(&self.graph.graph, 0x28 + 4 * i).unwrap_or(u32::MAX)
        });
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
    pub(crate) fn report(&self, dev: &AsahiDrmDevice, published: &Kick) {
        dev_info!(
            dev.as_ref(),
            "M5 lane {}: kick {:#x}, pool completed {:?}/{}, cursors {:?}, quarantine {:?}\n",
            self.qid(),
            published.ticket.kick,
            self.pool.m5().and_then(M5Pool::completed_submissions),
            published.pool_target,
            self.cursors().map(|c| (c.producer, c.consumer)),
            self.quarantine_error
        );
        for offset in (0..STATUS + 0x10).step_by(4) {
            if let Ok(value) = self
                .graph
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
    match &active.packet.completion {
        c if c.blamed() => "blamed",
        c if c.guilty() => "guilty",
        c if c.spared() => "spared",
        _ => "unclassified",
    }
}
