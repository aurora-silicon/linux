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
//! stamps continue across owners. A quarantined lane is never reused, and a failed kick keeps
//! its command pinned.
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

use super::compute::{GRAPH_SIZE, ITEMS, POINTERS};
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

/// A grown pool's old backing and the client's old view of it ([`Channel::grow`]), dropped off
/// the device mutex: the view first.
pub(crate) struct Grown {
    _view: Option<mmu::KernelMapping>,
    _backing: Option<M5Backing>,
}

/// What a failure of a lane's or pair's work says of its state ([`Channel::fail`],
/// [`super::m5_render::Pair::fail`]). Only the first failure decides whether the failed work's
/// drain can end the quarantine; a later one can only take that away, when it is `Ambiguous`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Failure {
    /// Recovery settled it: the firmware restarted and resumed or failed the work, so its exact
    /// retirement is the drain witness that ends the quarantine.
    Settled,
    /// The work failed with the firmware's view of it unknown (a timeout, its VM failed): the
    /// quarantine stays. After a settled failure it is a repeat and changes nothing.
    Final,
    /// A retirement witness could not be read: the quarantine stays, whatever came first.
    Ambiguous,
}

impl Failure {
    /// Whether the failed work's drain ends the quarantine, given whether it did before this
    /// failure (`awaiting`) and whether this is the first.
    pub(crate) fn awaits_drain(self, first: bool, awaiting: bool) -> bool {
        match (first, self) {
            (true, kind) => kind == Self::Settled,
            (false, Self::Ambiguous) => false,
            (false, _) => awaiting,
        }
    }
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

    /// The firmware finished every item up to `target` and holds nothing else: `done`, its own
    /// cursor (+0x30) and the producer all stand there, the host's idle condition
    /// ([`PointerBlock`]).
    pub(crate) fn idle_at(&self, target: u32) -> Result<bool> {
        Ok(self.word(POINTERS)? == target
            && self.word(POINTERS + 0x30)? == target
            && self.word(POINTERS + self.producer)? == target)
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
    /// and every failed kick stays in `failed`, pinning its command.
    quarantine: Option<Error>,
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
    /// consumed before the drain ([`Self::end_drain_wait`]).
    awaiting_drain: bool,
    /// The oldest kick finished but is not retirable yet (`Self::retire`): its record or inner
    /// item is visible ahead of the stamp, the pool counter or the work node. Its completion
    /// event has been consumed, so the event worker polls until it retires, as for a Neo
    /// compute queue (`queue::compute::Queue::retirement_polling`).
    retire_pending: bool,
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
}

impl ParkedLane {
    pub(crate) fn qid(&self) -> u8 {
        self.lane.qid()
    }
    /// Its previous binding's client views are unmapped (a new owner in the same VM may map
    /// the lane at the same addresses), and no release of its pool is in flight.
    pub(crate) fn adoptable(&self) -> bool {
        self.unmapped.done() && !self.pool_state.pending()
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
                quarantine: None,
                failed: KVec::new(),
                closed: false,
                faulted: false,
                awaiting_drain: false,
                retire_pending: false,
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
                quarantine: None,
                failed: KVec::new(),
                closed: false,
                faulted: false,
                awaiting_drain: false,
                retire_pending: false,
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
            || self.quarantine.is_some()
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
            "G17 M5: lane {}: closed {} installed {} quarantine {:?} faulted {} active {} failed {}, cursors {:x?}, idle {:?}, pool {:?}/{:?}, stamp {:x?} for kick {:#x}\n",
            self.qid(), self.closed, self.installed, self.quarantine, self.faulted, self.active.len(),
            self.failed.len(), self.lane.cursors(),
            self.lane.cursors().and_then(|cursors| self.lane.idle_at(cursors.producer)),
            self.pool.completed_submissions(), self.pool.submitted(), self.stamp(), next.prev().get(),
        );
    }

    /// The lane needs nothing more before its context's release: parkable, or quarantined with
    /// the firmware idle on its ring. A drain still awaited does not hold the release back, as a
    /// quarantined Neo queue does not (`Queue::idle_for_context` waits for the firmware, not for
    /// the witness): the healthy lanes of the context park, and this one parks only if it drained
    /// before the release was consumed ([`Self::end_drain_wait`]).
    pub(crate) fn settled(&self) -> Result<bool> {
        if self.quarantine.is_none() {
            return self.parkable();
        }
        Ok(self.closed && self.lane.idle_at(self.lane.cursors()?.producer)?)
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
    pub(crate) fn awaiting_drain(&self) -> bool {
        self.awaiting_drain
    }
    /// The context's scheduler release was consumed while this quarantined channel still awaited
    /// its failed kicks' drain: the channel stays quarantined, leaked under the cap, its kicks
    /// pinned. The firmware was idle on its ring (`Self::settled`) and no longer names the
    /// context, and nothing else can witness the drain: on M5 neither a kill's acknowledgement nor
    /// the scheduler page's killed state proves the firmware finished reading (S
    /// `queue/compute/exit.rs` `settle_exit`). Returns whether a wait ended.
    pub(crate) fn end_drain_wait(&mut self) -> bool {
        if !self.awaiting_drain {
            return false;
        }
        self.awaiting_drain = false;
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
        self.installed && self.quarantine.is_none() && !self.closed && self.active.len() < DEPTH
    }
    pub(crate) fn quarantined(&self) -> Option<Error> {
        self.quarantine
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
        if self.quarantine.is_some() && !self.awaiting_drain {
            return None;
        }
        Iterator::chain(self.failed.iter(), self.active.iter())
            .filter_map(|active| Some((active, active.packet.as_ref()?)))
            .min_by_key(|(active, _)| active.published)
            .map(|(active, packet)| crate::g17::timeout::Head {
                vm: packet.context.vm().id(),
                published: active.published,
                finished: active.observed.is_some(),
                failed: self.quarantine.is_some(),
            })
    }

    /// The watchdog of `packet` may classify this channel's work (Neo:
    /// `queue::compute::Queue::can_classify_timeout`): it is healthy and `packet` is in flight.
    pub(crate) fn can_classify_timeout(&self, packet: &Arc<Packet>) -> bool {
        self.quarantine.is_none()
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
        self.quarantine.is_none()
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
        self.quarantine.is_none() && self.retire_pending
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
            self.quarantine = Some(error);
        }
        result
    }

    /// Whether the firmware consumed the installation's configuration ([`Self::install`]); an
    /// adopted lane's binding has none and is consumed at once. A crash, or the configuration
    /// still unconsumed once the wait `expired` ([`INSTALL_WAIT_MS`]), quarantines the channel.
    pub(crate) fn install_consumed(&mut self, crashed: &AtomicBool, expired: bool) -> Result<bool> {
        if let Some(error) = self.quarantine {
            return Err(error);
        }
        if self.rebound.is_some() || self.lane.cursors()?.consumer == 1 {
            return Ok(true);
        }
        let error = match () {
            _ if crashed.load(Ordering::Acquire) => EIO,
            _ if expired => ETIMEDOUT,
            _ => return Ok(false),
        };
        self.quarantine = Some(error);
        Err(error)
    }

    /// Whether a command whose plan needs `blocks` must back the pool first ([`Self::grow`]):
    /// it is smaller, vacant, or being released.
    pub(crate) fn needs_pool(&self, blocks: usize) -> bool {
        self.pool_state.phase() != PoolPhase::Populated || blocks > self.pool.blocks()
    }

    /// Backs the lane's pool for a command whose plan needs `blocks`, before that command is
    /// published, through the release transaction of a Neo compute queue's growth
    /// (`queue::compute::Queue::begin_growth`, `finish_growth`). A kick queue whose
    /// private-memory pool changed is reconfigured at the submission, only on an empty queue
    ///. So: EAGAIN until the lane is idle; then the pool's registration is
    /// released (FreeListRelease, announced through `notify`) and EAGAIN again until the
    /// firmware consumed it, the publication loop waiting on the event worker's progress, as
    /// Neo's growth does. Once the pool is vacant, fresh backing for `blocks` (at least a fresh
    /// pool's) and its client view are built (ENOMEM refuses this command only; the pool stays
    /// vacant), installed with its page-pool row, and the next kick registers the pool with its
    /// configuration update, as an adopted lane's first kick does. A failure after the release
    /// quarantines the channel. Returns the old backing and view, which the caller drops off
    /// the device mutex.
    pub(crate) fn grow(
        &mut self,
        alloc: &Allocator<'_>,
        host: &mut Host<'_>,
        notify: impl FnOnce() -> Result,
        blocks: usize,
    ) -> Result<Option<Grown>> {
        if let Some(error) = self.quarantine {
            return Err(error);
        }
        if !self.installed || self.closed || blocks > self.gpu.max_pool_blocks {
            return Err(EINVAL);
        }
        if !self.needs_pool(blocks) {
            return Ok(None);
        }
        if self.pool_state.phase() == PoolPhase::Populated {
            let next = self.lane.kick.timestamp();
            if self.faulted
                || !self.active.is_empty()
                || !self.failed.is_empty()
                || !self.lane.idle_at(self.lane.cursors()?.producer)?
                || self.pool.completed_submissions()? != self.pool.submitted()?
                || (next.get() != KickTimestamp::FIRST.get()
                    && !retirement::stamp_covers(self.stamp()?, next.prev().get()))
            {
                return Err(EAGAIN);
            }
            if self
                .pool_state
                .release(host.init, self.pool.release_record(1))?
            {
                notify()?;
                return Err(EAGAIN);
            }
        }
        fence(Ordering::Acquire);
        let consumer = host.init.control_consumer()?.load(Ordering::Relaxed);
        let producer = host.init.control_producer()?.load(Ordering::Relaxed);
        self.pool_state.observe(consumer, producer)?;
        if self.pool_state.phase() != PoolPhase::Vacant {
            return Err(EAGAIN);
        }
        let target = blocks.max(self.gpu.pool_blocks);
        let geometry =
            fw::m5::pool::PoolGeometry::of(target, self.gpu.block_bytes()).map_err(|_| EINVAL)?;
        let no_memory = |error: Error| if error == EINVAL { error } else { ENOMEM };
        let fresh = M5Backing::new(alloc, self.qid(), geometry).map_err(no_memory)?;
        let view = fresh.map_client(self.context.vm()).map_err(no_memory)?;
        self.aliases.reserve(1, GFP_KERNEL)?;
        let old_va = self.pool.backing_va().ok();
        let from = self.pool.blocks();
        let result = (|| {
            let old = self.pool.rearm(fresh)?;
            host.init
                .install_m5_pool(self.pool.id(), self.pool.descriptor()?)?;
            self.pool.publish_to_contexts()?;
            view.publish_to_contexts();
            Ok(old)
        })();
        let old = match result {
            Ok(old) => old,
            Err(error) => {
                self.quarantine = Some(error);
                return Err(error);
            }
        };
        let index = old_va.and_then(|va| self.aliases.iter().position(|alias| alias.iova() == va));
        let old_view = match index {
            Some(index) => Some(core::mem::replace(&mut self.aliases[index], view)),
            None => {
                self.aliases
                    .push_within_capacity(view)
                    .map_err(|_| ENOMEM)?;
                None
            }
        };
        self.pool_state.populate();
        pr_info!(
            "M5 lane {}: pool backed with {} blocks (from {}) for queue {}\n",
            self.qid(),
            target,
            from,
            self.owner
        );
        Ok(Some(Grown {
            _view: old_view,
            _backing: old,
        }))
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
        if let Some(error) = self.quarantine {
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
                self.quarantine = Some(error);
                Err(error)
            }
        }
    }

    /// Stages the exact identity of every kick not yet observed for the shared completion-ring
    /// scan. Returns whether any was staged.
    pub(crate) fn request(&self, batch: &mut Batch) -> Result<bool> {
        if self.active.is_empty() || self.quarantine.is_some() {
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
        if self.quarantine.is_some() {
            return self.rehabilitate(state, retired);
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
        self.awaiting_drain = failure.awaits_drain(self.quarantine.is_none(), self.awaiting_drain);
        self.quarantine.get_or_insert(error);
        let error = self.quarantine.unwrap_or(error);
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
    fn rehabilitate(&mut self, state: u32, retired: &mut impl FnMut(Retired) -> Result) -> Result {
        if !self.awaiting_drain {
            return Ok(());
        }
        match self.drained(state) {
            Ok(true) => {}
            Ok(false) => return Ok(()),
            Err(error) => {
                pr_warn!("G17 M5: lane {}: its failed kicks' drain witnesses unreadable ({:?}); the lane stays quarantined\n",
                    self.qid(), error);
                self.awaiting_drain = false;
                return Ok(());
            }
        }
        let error = self.quarantine.unwrap_or(EIO);
        while let Some(front) = self.failed.first() {
            retired(Retired {
                ticket: front.ticket,
                packet: front.packet.clone(),
                result: Err(error),
                drained: true,
            })?;
            self.failed.remove(0).map_err(|_| EINVAL)?;
        }
        self.quarantine = None;
        self.awaiting_drain = false;
        self.faulted = false;
        pr_info!(
            "G17 M5: lane {}: its failed kicks ({:?}) drained; the lane returns\n",
            self.qid(),
            error
        );
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
            self.quarantine
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
