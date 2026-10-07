// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Retained physical compute queues. Logical ownership and command retirement never free an
//! installed graph; the device retains it until both firmware processors have stopped.

pub(crate) mod exit;
pub(crate) mod retirement;

use crate::g17::{
    context::Context,
    fw::{compute::COMPUTE_DESCRIPTOR_SIZE, queue::*},
    object::{Allocator, CpuMap, KernelObject},
};
use crate::{hw::t8140::dynamic, mmu};
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};
use retirement::{Cursors, RECORD_SLOTS};

const DESCRIPTORS: usize = 256;
const GRAPH_SIZE: usize = 0x8000;
const CONTEXT_SIZE: usize = 0x20000;
const RECORD_STRIDE: usize = 0x1000;
const CONFIG: usize = 0;
const ANNOUNCE: usize = 0x800;
const PREDECESSOR: usize = 0x400;
const POINTERS: usize = 0x100;
const ITEMS: usize = 0x4000;
const STAMP: usize = 0x300;

#[repr(C)]
struct GraphLayout {
    queue: QueueRecord,
    unk_c0: [u8; 0x40],
    pointers: PointerBlock,
    unk_180: [u8; 0x80],
    jobs: JobListHead,
    unk_210: [u8; 0x3df0],
    items: [u64; ITEM_RING_ENTRIES as usize],
    unk_6800: [u8; 0x1800],
}
// SAFETY: The layout contains only integer storage and integer atomics, all valid when zeroed.
unsafe impl Zeroable for GraphLayout {}
static_assert!(core::mem::size_of::<GraphLayout>() == GRAPH_SIZE);
static_assert!(core::mem::offset_of!(GraphLayout, pointers) == POINTERS);
static_assert!(core::mem::offset_of!(GraphLayout, items) == ITEMS);

/// Aliases are destroyed before the logical root they name. Superseded bindings remain owned
/// until the first exact successful publication under their replacement has retired.
struct Binding {
    _kick_alias: mmu::KernelMapping,
    _pool_alias: mmu::KernelMapping,
    _descriptors: mmu::KernelMapping,
    preempt: mmu::KernelMapping,
    usage: mmu::KernelMapping,
    operand: mmu::KernelMapping,
    _queue_context: mmu::KernelMapping,
    context: Arc<Context>,
}

/// Firmware-shared storage of one compute QID, separate from its common kick ring.
struct Graph {
    descriptor_high: mmu::KernelMapping,
    descriptors: KernelObject,
    preempt: KernelObject,
    usage: KernelObject,
    operand: KernelObject,
    support: KernelObject,
    graph: KernelObject,
    context_low: KernelObject,
    context_high: KernelObject,
    records: KernelObject,
    record_ends: [Option<u32>; RECORD_SLOTS],
    reclaimed: u32,
}

impl Graph {
    fn new(alloc: &Allocator<'_>, context: &Context) -> Result<Self> {
        let page = mmu::UAT_PGSZ as u64;
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let cpu = CpuMap::WriteCombined;
        let descriptors = alloc.lower(DESCRIPTORS * COMPUTE_DESCRIPTOR_SIZE, page, prot, cpu)?;
        let mut high = alloc.uat.geometry().kernel_range();
        high.start += dynamic::KERNEL_OFFSET;
        let descriptor_high = descriptors.alias_in(alloc.uat.kernel_vm(), high, page, prot)?;
        let preempt = alloc.kernel(0x4000, page, prot, cpu)?;
        let usage = alloc.kernel(mmu::UAT_PGSZ, page, prot, cpu)?;
        let operand = alloc.lower(0x14000, page, prot, cpu)?;
        let mut support = alloc.kernel(0xc000, page, prot, cpu)?;
        let support_va = support.gpu_va();
        support.write(0, support_va + page)?;
        support.write(0x100, support_va + page + 4)?;
        support.write(0x110, 0x50u32)?;
        support.write(mmu::UAT_PGSZ + 4, 1u32)?;
        let mut graph = alloc.kernel(GRAPH_SIZE, page, prot, cpu)?;
        let graph_va = graph.gpu_va();
        graph.initialize::<GraphLayout>(0, |view| {
            view.queue = QueueRecord::new(&QueueRecordArgs {
                pointers_va: graph_va + POINTERS as u64,
                items_va: graph_va + ITEMS as u64,
                job_list_va: context.work_head_va(),
                scheduler_va: context.scheduler_va(),
                owner_pid: context.owner_pid(),
            });
            view.pointers = PointerBlock::new();
            view.jobs = JobListHead::new(graph_va + 0x200);
            Ok(())
        })?;
        let context_low = alloc.lower(CONTEXT_SIZE, page, prot, cpu)?;
        let context_high = alloc.kernel(CONTEXT_SIZE, page, prot, cpu)?;
        let records = alloc.kernel(RECORD_SLOTS * RECORD_STRIDE, page, prot, cpu)?;
        Ok(Self {
            descriptor_high,
            descriptors,
            preempt,
            usage,
            operand,
            support,
            graph,
            context_low,
            context_high,
            records,
            record_ends: [None; RECORD_SLOTS],
            reclaimed: 0,
        })
    }

    /// All fallible mapping work finishes before replacing a physical queue's logical owner.
    fn bind(
        &mut self,
        context: Arc<Context>,
        kick_alias: mmu::KernelMapping,
        pool_alias: mmu::KernelMapping,
    ) -> Result<Binding> {
        if !context.is_current() {
            return Err(EFAULT);
        }
        let vm = context.vm();
        vm.install_job_context_aliases()?;
        let descriptors =
            self.descriptors
                .map_alias(vm, self.descriptors.gpu_va(), mmu::PROT_GPU_SHARED_RO)?;
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let page = mmu::UAT_PGSZ as u64;
        let preempt = self
            .preempt
            .alias_in(vm, dynamic::CLIENT_LOWER, page, prot)?;
        let usage = self.usage.alias_in(vm, dynamic::CLIENT_LOWER, page, prot)?;
        let operand = self
            .operand
            .alias_in(vm, dynamic::CLIENT_LOWER, page, prot)?;
        let queue_context = self
            .context_low
            .alias_in(vm, dynamic::CLIENT_LOWER, page, prot)?;
        Ok(Binding {
            _kick_alias: kick_alias,
            _pool_alias: pool_alias,
            _descriptors: descriptors,
            preempt,
            usage,
            operand,
            _queue_context: queue_context,
            context,
        })
    }

    /// Caller has excluded publication and proved the retained graph idle.
    fn set_owner(&mut self, context: &Context) -> Result {
        let pointer = self
            .graph
            .pointer(0, core::mem::size_of::<QueueRecord>())?
            .cast::<QueueRecord>();
        // SAFETY: The checked record is in the retained mapping, has byte alignment and consists
        // only of integer storage. The device mutex and idle witness exclude simultaneous access.
        unsafe {
            (*pointer).set_compute_owner(
                context.work_head_va(),
                context.scheduler_va(),
                context.owner_pid(),
            )
        };
        Ok(())
    }

    fn cursors(&self) -> Result<Cursors> {
        Ok(Cursors {
            producer: self.graph.word(POINTERS + 0x40)?.load(Ordering::Relaxed),
            consumer: self.graph.word(POINTERS)?.load(Ordering::Relaxed),
            count: self.graph.word(POINTERS + 0x60)?.load(Ordering::Relaxed),
        })
    }
    fn stamp(&self) -> Result<u32> {
        fence(Ordering::Acquire);
        Ok(self.support.word(STAMP)?.load(Ordering::Relaxed))
    }
    fn descriptor_vas(&self, slot: u8) -> (u64, u64) {
        let offset = u64::from(slot) * COMPUTE_DESCRIPTOR_SIZE as u64;
        (
            self.descriptor_high.iova() + offset,
            self.descriptors.gpu_va() + offset,
        )
    }
    fn ensure_record_slot(&self, slot: usize) -> Result {
        if let Some(end) = *self.record_ends.get(slot).ok_or(EINVAL)? {
            let cursors = self.cursors()?;
            if !cursors.consumed(end, cursors.count)? {
                return Err(EBUSY);
            }
        }
        Ok(())
    }

    /// Reclaim only consumed item pointers, with individual aligned stores and a release fence.
    /// This leaves records still named by firmware untouched across arbitrarily many wraps.
    fn recycle(&mut self, cursors: Cursors) -> Result {
        if cursors.count != ITEM_RING_ENTRIES {
            return Err(EIO);
        }
        if cursors.consumer >= cursors.count || self.reclaimed >= cursors.count {
            return Err(ERANGE);
        }
        let done = cursors.consumer;
        while self.reclaimed != done {
            self.graph
                .dword(ITEMS + self.reclaimed as usize * 8)?
                .store(0, Ordering::Relaxed);
            self.reclaimed = (self.reclaimed + 1) % cursors.count;
        }
        fence(Ordering::SeqCst);
        Ok(())
    }
}

use crate::g17::{
    completion::Deferred, context::WorkStateLease, freelist::ComputePool, job::Packet, kick, qos,
    recovery, status::VmStatus,
};
use retirement::{Batch, Observation, Recordless, Ticket, IN_FLIGHT};

struct Fifo<T> {
    entries: [Option<T>; IN_FLIGHT],
    head: usize,
    len: usize,
}
impl<T> Fifo<T> {
    fn new() -> Self {
        Self {
            entries: core::array::from_fn(|_| None),
            head: 0,
            len: 0,
        }
    }
    fn front(&self) -> Option<&T> {
        self.entries[self.head].as_ref()
    }
    fn back(&self) -> Option<&T> {
        if self.len == 0 {
            None
        } else {
            self.entries[(self.head + self.len - 1) % IN_FLIGHT].as_ref()
        }
    }
    fn back_mut(&mut self) -> Option<&mut T> {
        if self.len == 0 {
            None
        } else {
            self.entries[(self.head + self.len - 1) % IN_FLIGHT].as_mut()
        }
    }
    fn push(&mut self, value: T) -> core::result::Result<(), T> {
        if self.len == IN_FLIGHT {
            return Err(value);
        }
        self.entries[(self.head + self.len) % IN_FLIGHT] = Some(value);
        self.len += 1;
        Ok(())
    }
    fn pop(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        let value = self.entries[self.head].take();
        self.head = (self.head + 1) % IN_FLIGHT;
        self.len -= 1;
        value
    }
    fn pop_back(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }
        self.len -= 1;
        self.entries[(self.head + self.len) % IN_FLIGHT].take()
    }
    fn iter(&self) -> impl Iterator<Item = &T> {
        Iterator::chain(
            self.entries[self.head..].iter(),
            self.entries[..self.head].iter(),
        )
        .take(self.len)
        .filter_map(Option::as_ref)
    }
    fn iter_mut(&mut self) -> impl Iterator<Item = &mut T> {
        let (before, after) = self.entries.split_at_mut(self.head);
        Iterator::chain(after.iter_mut(), before.iter_mut())
            .take(self.len)
            .filter_map(Option::as_mut)
    }
}

struct Active {
    packet: Arc<Packet>,
    published: u64,
    work: Arc<WorkStateLease>,
    ticket: Option<Ticket>,
    observed: Option<Observation>,
    failure_deferred: bool,
    qos_pending: bool,
}
struct Previous {
    binding: Option<Binding>,
    context: Arc<Context>,
    publication: Option<(u64, u64)>,
}
#[derive(Copy, Clone, PartialEq, Eq)]
struct ConfigIdentity {
    scheduler: u64,
    qos: u8,
    context: u32,
}

/// A private physical pool whose descriptor row is installed before allocating its kick ring.
/// The QID remains unpublished throughout construction.
pub(crate) struct PoolBacking {
    pool: ComputePool,
    id: kick::Id,
}

impl PoolBacking {
    pub(crate) fn new(alloc: &Allocator<'_>, id: kick::Id) -> Result<Self> {
        let pool = ComputePool::new(alloc, id.qid())?;
        Ok(Self { pool, id })
    }

    /// Fresh page-pool row data, installed once while the QID is still unpublished.
    pub(crate) fn pool_descriptor(&self) -> (u16, u64) {
        (self.pool.id(), self.pool.page_list_va())
    }
}

/// A physical pool and kick ring prepared before the device-global compute scheduler and
/// descriptor graph. No logical VM aliases or firmware publication exist at this stage.
pub(crate) struct Backing {
    pool: ComputePool,
    kick: kick::Queue,
}

impl Backing {
    pub(crate) fn new(alloc: &Allocator<'_>, backing: PoolBacking) -> Result<Self> {
        let PoolBacking { pool, id } = backing;
        let kick = kick::Queue::compute(alloc, id)?;
        Ok(Self { pool, kick })
    }
}

/// Installed graph and its exact logical owner. A released idle graph may change owners only
/// after a retirement witness. Failed work retains its pointers independently of fence state.
pub(crate) struct Queue {
    owner: Option<u64>,
    binding: Binding,
    previous: Option<Previous>,
    kick: kick::Queue,
    pool: ComputePool,
    graph: Graph,
    active: Fifo<Active>,
    replays: Fifo<Arc<Packet>>,
    replay_retirement: bool,
    replay_error: Option<Error>,
    replay_prepared: bool,
    exit: Option<exit::Transaction>,
    installed: bool,
    outer_started: bool,
    published_config: Option<ConfigIdentity>,
    ordinal: u32,
    submitted: u32,
    last_end: u64,
    released: bool,
    retirement_ready: bool,
    quarantined: bool,
    retired_by_teardown: bool,
    retire_pending: bool,
    awaiting_witness: bool,
    spared_quarantine: bool,
    spared_deferred: bool,
    terminal: Option<(Arc<VmStatus>, Error)>,
    quarantine_error: Option<Error>,
    failure_status_pending: bool,
    retirement_proved: bool,
}

impl Queue {
    /// The caller reserves a QID first, cancels it on construction failure, and retains every
    /// successfully installed queue until processor stop. Shared VM apertures precede this call.
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        backing: Backing,
        owner: u64,
        context: Arc<Context>,
    ) -> Result<Self> {
        let Backing { mut pool, mut kick } = backing;
        let mut graph = Graph::new(alloc, &context)?;
        let kick_alias = kick.map_client(context.vm())?;
        let pool_alias = pool.map_client(context.vm())?;
        let binding = graph.bind(context.clone(), kick_alias, pool_alias)?;
        context.mark_published();
        Ok(Self {
            owner: Some(owner),
            binding,
            previous: None,
            kick,
            pool,
            graph,
            active: Fifo::new(),
            replays: Fifo::new(),
            replay_retirement: false,
            replay_error: None,
            replay_prepared: false,
            exit: None,
            installed: false,
            outer_started: false,
            published_config: None,
            ordinal: 0,
            submitted: 0,
            last_end: 0,
            released: false,
            retirement_ready: false,
            quarantined: false,
            retired_by_teardown: false,
            retire_pending: false,
            awaiting_witness: false,
            spared_quarantine: false,
            spared_deferred: false,
            terminal: None,
            quarantine_error: None,
            failure_status_pending: false,
            retirement_proved: false,
        })
    }
    pub(crate) fn qid(&self) -> u8 {
        self.kick.id().qid()
    }
    pub(crate) fn owner(&self) -> Option<u64> {
        self.owner
    }
    pub(crate) fn context(&self) -> &Arc<Context> {
        &self.binding.context
    }
    /// Quarantined commands still retain the scheduler until their retirement witness.
    pub(crate) fn idle_for_context(&self, context: &Arc<Context>) -> bool {
        !Arc::ptr_eq(self.context(), context) || (self.active.len == 0 && self.replays.len == 0)
    }
    pub(crate) fn in_flight(&self) -> bool {
        self.active.len != 0 && !self.quarantined
    }
    pub(crate) fn room(&self) -> bool {
        !self.quarantined && !self.released && self.replays.len == 0 && self.active.len < IN_FLIGHT
    }
    /// Quarantines without a pending witness no longer describe live pipe order.
    pub(crate) fn timeout_head(&self) -> Option<crate::g17::timeout::Head> {
        if self.quarantined && !self.awaiting_witness {
            return None;
        }
        self.active.front().map(|active| crate::g17::timeout::Head {
            vm: Arc::as_ptr(active.packet.completion.status()) as usize,
            published: active.published,
            finished: active.observed.is_some(),
        })
    }

    pub(crate) fn can_classify_timeout(&self, packet: &Arc<Packet>) -> bool {
        !self.quarantined && self.owns_packet(packet)
    }

    pub(crate) fn oldest_spared(&self) -> Option<bool> {
        self.active
            .front()
            .map(|a| a.packet.completion.spared() && a.packet.completion.replay_count() == 0)
    }
    pub(crate) fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
        for active in self.active.iter() {
            visit(active.packet.completion.status());
        }
    }
    pub(crate) fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {
        for active in self.active.iter() {
            active
                .packet
                .completion
                .classify(class(active.packet.completion.status()));
        }
    }
    fn qos_owner(&self) -> qos::Owner {
        qos::Owner {
            qid: self.qid(),
            qos: self.binding.context.qos_id(),
            data_master: DataMaster::Compute as u8,
        }
    }
    /// A completion hint can require a second visibility/recordless pass even
    /// when firmware has no more events to send. Do not poll idle or failed queues.
    pub(crate) fn retirement_polling(&self) -> bool {
        !self.quarantined && self.retire_pending
    }

    fn selected(&self, masks: Option<[u64; 2]>) -> bool {
        self.retired_by_teardown
            || self.retire_pending
            || masks.is_none_or(|m| m[self.qid() as usize / 64] & (1 << (self.qid() % 64)) != 0)
    }
    /// Stage every unlatched exact identity before the device's single shared-ring scan.
    pub(crate) fn request_observations(
        &self,
        batch: &mut Batch,
        masks: Option<[u64; 2]>,
    ) -> Result<bool> {
        if self.quarantined || !self.selected(masks) {
            return Ok(false);
        }
        let mut requested = false;
        for active in self.active.iter() {
            let Some(ticket) = active.ticket else {
                break;
            };
            if active.observed.is_none() {
                batch.request(&ticket)?;
                requested = true;
            }
        }
        Ok(requested)
    }
    pub(crate) fn latch_observations(
        &mut self,
        batch: &mut Batch,
        masks: Option<[u64; 2]>,
    ) -> Result {
        if self.quarantined || !self.selected(masks) {
            return Ok(());
        }
        for active in self.active.iter_mut() {
            let Some(ticket) = active.ticket else {
                break;
            };
            if active.observed.is_none() {
                active.observed = batch.take(&ticket)?;
            }
        }
        Ok(())
    }
    fn front_finished(&self) -> bool {
        self.active.front().is_some_and(|active| {
            active.observed.is_some()
                || active.ticket.is_some_and(|ticket| {
                    self.graph
                        .cursors()
                        .and_then(|c| c.consumed(ticket.item_producer, ticket.item_count))
                        .unwrap_or(false)
                })
        })
    }
    fn valid_ticket(&self, active: &Active, ticket: &Ticket) -> Result {
        if ticket.qid != self.qid()
            || ticket.descriptor == 0
            || ticket.item_count == 0
            || ticket.work_node != active.work.node_va()?
            || self.submitted == 0
            || self.owner != Some(ticket.owner)
            || active.packet.order.sequence != ticket.submission
            || active.packet.context.id() != ticket.context_id
            || active.packet.context.id() != self.binding.context.id()
        {
            return Err(EIO);
        }
        Ok(())
    }

    /// Return VM identity for queue-local failure fanout. Deferred callbacks are collected under
    /// the mutex and invoked afterwards; failed packets stay retained until their witness.
    pub(crate) fn quarantine(
        &mut self,
        error: Error,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<Option<Arc<VmStatus>>> {
        if !self.quarantined {
            self.quarantined = true;
            self.quarantine_error = Some(error);
            self.failure_status_pending = true;
            self.retirement_proved = false;
            self.retired_by_teardown = false;
            let all_spared =
                self.active.len != 0 && self.active.iter().all(|a| a.packet.completion.spared());
            self.spared_quarantine = all_spared && error == ENODATA;
            self.spared_deferred = self.spared_quarantine;
        }
        let error = self.quarantine_error.ok_or(EIO)?;
        let mut terminal = None;
        for active in self.active.iter_mut() {
            if !self.spared_deferred && !active.failure_deferred {
                defer(Deferred::FailedOwned(
                    active.packet.completion.clone(),
                    error,
                ))?;
                active.failure_deferred = true;
            }
            if self.failure_status_pending
                && !active.packet.completion.spared()
                && terminal.is_none()
            {
                terminal = Some(active.packet.completion.status().clone());
            }
        }
        self.failure_status_pending = false;
        self.awaiting_witness =
            self.active.len != 0 && self.active.iter().all(|a| a.ticket.is_some());
        Ok(terminal)
    }

    /// Poll only the ordered prefix; the next pass may recheck a finished front on another QID's
    /// event. A final identity-less scan allows recordless retirement without the one-pass grace.
    pub(crate) fn retire(
        &mut self,
        masks: Option<[u64; 2]>,
        epoch: u64,
        recovery_state: u32,
        mut complete_qos: impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        if self.quarantined || !self.selected(masks) {
            return Ok(());
        }
        let finished_before = self.retire_pending;
        self.retire_pending = false;
        let mut oldest = true;
        while let Some(front) = self.active.front() {
            let result = (|| {
                let ticket = front.ticket.ok_or(EIO)?;
                self.valid_ticket(front, &ticket)?;
                if recovery_state != 0 {
                    return Err(EAGAIN);
                }
                let recordless = if epoch != ticket.epoch {
                    Recordless::CrossedRecovery
                } else if masks.is_none() || (finished_before && oldest) {
                    Recordless::Allowed
                } else {
                    Recordless::NotYet
                };
                let next_end = self
                    .active
                    .iter()
                    .skip(1)
                    .find_map(|a| a.observed.map(|o| o.timestamps[1]));
                let timestamps = retirement::poll(
                    &ticket,
                    front.observed,
                    self.graph.cursors()?,
                    self.graph.stamp()?,
                    recordless,
                    self.last_end,
                    next_end,
                )?;
                if timestamps.is_some()
                    && self.previous.as_ref().is_some_and(|p| {
                        p.publication != Some((front.packet.order.sequence, ticket.epoch))
                    })
                {
                    return Err(EIO);
                }
                Ok(timestamps)
            })();
            oldest = false;
            match result {
                Ok(Some(timestamps)) => {
                    let submitted = self.submitted.checked_sub(1).ok_or(EIO)?;
                    if front.qos_pending {
                        complete_qos(self.qos_owner())?;
                        self.active.iter_mut().next().ok_or(EIO)?.qos_pending = false;
                    }
                    let front = self.active.front().ok_or(EIO)?;
                    self.retire_pending = true;
                    defer(Deferred::Retired(
                        front.packet.completion.clone(),
                        Ok([timestamps[0], timestamps[1], 0, 0]),
                    ))?;
                    self.retire_pending = false;
                    self.submitted = submitted;
                    let active = self.active.pop().ok_or(EIO)?;
                    if active.observed.is_some() {
                        self.last_end = self.last_end.max(timestamps[1]);
                    }
                    drop(self.previous.take());
                    self.retirement_ready = self.active.len == 0;
                    if self.released && self.retirement_ready {
                        self.owner = None;
                    }
                }
                Ok(None) | Err(EAGAIN) => {
                    self.retire_pending = self.front_finished();
                    break;
                }
                Err(error) => {
                    self.terminal = self.quarantine(error, defer)?.map(|status| (status, error));
                    return Ok(());
                }
            }
        }
        Ok(())
    }

    /// Exact terminal witness releases command pins, not installed graph ownership. Spared
    /// zero-duration work can return its queue to service after settling its accounting.
    pub(crate) fn release_witnessed(
        &mut self,
        recovery_state: u32,
        mut complete_qos: impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        self.release_witnessed_inner(recovery_state, false, &mut complete_qos, defer)
    }

    pub(crate) fn prepare_replays(
        &mut self,
        recovery_state: u32,
        complete_qos: impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        self.release_witnessed_inner(recovery_state, true, complete_qos, defer)
    }

    fn release_witnessed_inner(
        &mut self,
        recovery_state: u32,
        replay: bool,
        mut complete_qos: impl FnMut(qos::Owner) -> Result,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        if !self.awaiting_witness || recovery_state != 0 {
            return Ok(());
        }
        if !self.retirement_proved {
            for active in self.active.iter() {
                self.valid_ticket(active, &active.ticket.ok_or(EIO)?)?;
            }
            let newest = self.active.back().and_then(|a| a.ticket).ok_or(EIO)?;
            if !retirement::quarantined_retired(
                &newest,
                self.graph.cursors()?,
                self.graph.stamp()?,
                self.active.len as u32,
                self.submitted,
            )? {
                return Ok(());
            }
            let candidate = self.spared_deferred
                && self.spared_quarantine
                && !self.released
                && self.owner == Some(newest.owner)
                && !self
                    .active
                    .iter()
                    .any(|active| active.packet.completion.has_firmware_consumer())
                && self.active.iter().all(|active| {
                    active
                        .packet
                        .completion
                        .replay_witness_words()
                        .is_some_and(|words| words[0] == words[1])
                });
            if candidate && !replay {
                return Ok(());
            }
            self.replay_retirement = candidate;
            self.retirement_proved = true;
        }
        while let Some(front) = self.active.front() {
            if front.qos_pending {
                complete_qos(self.qos_owner())?;
                self.active.iter_mut().next().ok_or(EIO)?.qos_pending = false;
            }
            let front = self.active.front().ok_or(EIO)?;
            let completion = front.packet.completion.clone();
            if self.replay_retirement {
                self.replays.push(front.packet.clone()).map_err(|_| EIO)?;
            } else if self.spared_deferred {
                defer(Deferred::Retired(completion, Err(ENODATA)))?;
            } else {
                defer(Deferred::Release(completion))?;
            }
            drop(self.active.pop());
        }
        self.awaiting_witness = false;
        self.retirement_proved = false;
        self.replay_retirement = false;
        self.spared_deferred = false;
        if self.spared_quarantine {
            self.spared_quarantine = false;
            self.quarantine_error = None;
            self.submitted = 0;
            self.kick.clear_parent_after_recovery();
            self.quarantined = false;
            self.retire_pending = false;
            self.retirement_ready = true;
            if self
                .previous
                .as_ref()
                .is_some_and(|p| p.publication.is_some())
            {
                drop(self.previous.take());
            }
            if self.released {
                self.owner = None;
            }
        }
        Ok(())
    }

    /// Both FIFOs are bounded by IN_FLIGHT. Count all retained commands of
    /// queues which this pass can release, replay or quarantine on failure.
    pub(crate) fn replay_commands(&self) -> usize {
        if self.awaiting_witness || self.replays.len != 0 {
            self.active.len + self.replays.len
        } else {
            0
        }
    }

    pub(crate) fn replay_front(&self) -> Option<Arc<Packet>> {
        self.replays.front().cloned()
    }

    /// Drop the witnessed owner only after either publication retained it or
    /// the off-lock completion collector accepted its terminal result.
    pub(crate) fn finish_replay(&mut self, packet: &Arc<Packet>) -> Result {
        if !self
            .replays
            .front()
            .is_some_and(|front| Arc::ptr_eq(front, packet))
        {
            return Err(EIO);
        }
        drop(self.replays.pop());
        self.replay_prepared = false;
        if self.replays.len == 0 {
            self.replay_error = None;
        }
        Ok(())
    }

    pub(crate) fn prepare_replay(&mut self, packet: &Arc<Packet>) -> Result {
        if !self
            .replays
            .front()
            .is_some_and(|front| Arc::ptr_eq(front, packet))
        {
            return Err(EIO);
        }
        if let Some(error) = self.replay_error {
            return Err(error);
        }
        if !self.replay_prepared {
            if !packet.completion.begin_replay() {
                return Err(EAGAIN);
            }
            packet.completion.reset_for_replay()?;
            self.replay_prepared = true;
        }
        Ok(())
    }

    pub(crate) fn fail_replays(&mut self, error: Error) {
        if self.replay_error.is_none() {
            self.replay_error = Some(error);
        }
    }

    /// Caller already stopped both processors. No firmware owner can access retained mappings.
    pub(crate) fn stopped(&mut self, defer: &mut impl FnMut(Deferred) -> Result) -> Result {
        while let Some(packet) = self.replays.front() {
            defer(Deferred::Retired(packet.completion.clone(), Err(EIO)))?;
            drop(self.replays.pop());
        }
        while let Some(active) = self.active.front() {
            defer(Deferred::Retired(
                active.packet.completion.clone(),
                Err(EIO),
            ))?;
            drop(self.active.pop());
        }
        Ok(())
    }
}

impl Queue {
    pub(crate) fn matches(&self, owner: u64, qid: u8, context: &Arc<Context>) -> Result<bool> {
        if self.owner != Some(owner) {
            return Ok(false);
        }
        if self.released
            || self.quarantined
            || self.qid() != qid
            || !Arc::ptr_eq(&self.binding.context, context)
            || !context.is_current()
        {
            return Err(EIO);
        }
        Ok(true)
    }
    pub(crate) fn reusable(&self) -> bool {
        self.installed
            // An issued kill still names this graph even if its commands retire
            // before the acknowledgement. It cannot acquire a successor owner.
            && self.exit.is_none()
            && self.owner.is_none()
            && self.released
            && self.retirement_ready
            && !self.quarantined
            && self.active.len == 0
            && self.replays.len == 0
            && self.previous.is_none()
    }
    pub(crate) fn bind_owner(&mut self, owner: u64, context: Arc<Context>) -> Result {
        if !self.reusable() {
            return Err(EBUSY);
        }
        if !context.is_current() {
            return Err(EFAULT);
        }
        let old = self.binding.context.clone();
        if Arc::ptr_eq(old.status(), context.status()) && old.is_current() {
            self.graph.set_owner(&context)?;
            self.binding.context = context.clone();
            self.previous = Some(Previous {
                binding: None,
                context: old,
                publication: None,
            });
        } else {
            let kick_alias = self.kick.map_client(context.vm())?;
            let pool_alias = self.pool.map_client(context.vm())?;
            let binding = self.graph.bind(context.clone(), kick_alias, pool_alias)?;
            self.graph.set_owner(&context)?;
            let old_binding = core::mem::replace(&mut self.binding, binding);
            self.previous = Some(Previous {
                binding: Some(old_binding),
                context: old,
                publication: None,
            });
        }
        context.mark_published();
        self.owner = Some(owner);
        self.released = false;
        self.retirement_ready = false;
        self.retired_by_teardown = false;
        Ok(())
    }
    /// True allows the registry to cancel the never-published QID and drop this graph. An
    /// installed graph remains retained even after the logical owner exits without work.
    pub(crate) fn release_owner(&mut self) -> Result<bool> {
        if !self.installed && self.active.len == 0 && !self.quarantined && self.previous.is_none() {
            return Ok(true);
        }
        self.released = true;
        if self.active.len == 0 && !self.quarantined {
            if let Some(previous) = self.previous.as_ref().filter(|p| p.publication.is_none()) {
                if let Err(error) = self.graph.set_owner(&previous.context) {
                    self.quarantined = true;
                    self.retired_by_teardown = true;
                    return Err(error);
                }
                let mut previous = self.previous.take().ok_or(EIO)?;
                if let Some(binding) = previous.binding.take() {
                    self.binding = binding;
                } else {
                    self.binding.context = previous.context;
                }
                self.retirement_ready = true;
            }
            if self.retirement_ready {
                self.owner = None;
            }
        }
        Ok(false)
    }
    /// Exact host identity remains valid even when several commands share an ioctl ID.
    pub(crate) fn owns_packet(&self, packet: &Arc<Packet>) -> bool {
        self.active
            .iter()
            .any(|active| Arc::ptr_eq(&active.packet, packet))
    }

    /// A watchdog settles spared, deferred waiters without changing their VM status.
    /// Fully ticketed spared work can still return this queue after its drain witness.
    pub(crate) fn timeout(
        &mut self,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<Option<Arc<VmStatus>>> {
        let fresh = !self.quarantined;
        let return_spared = self.active.len != 0
            && self
                .active
                .iter()
                .all(|active| active.packet.completion.spared() && active.ticket.is_some());
        if self.spared_deferred {
            for active in self.active.iter_mut() {
                if !active.failure_deferred {
                    defer(Deferred::FailedOwned(
                        active.packet.completion.clone(),
                        ETIMEDOUT,
                    ))?;
                    active.failure_deferred = true;
                }
            }
            self.spared_deferred = false;
        }
        let result = self.quarantine(ETIMEDOUT, defer);
        if fresh {
            self.spared_quarantine = return_spared;
        }
        result
    }

    pub(crate) fn cancel(
        &mut self,
        packet: &Arc<Packet>,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result<bool> {
        if !self.owns_packet(packet) {
            return Ok(false);
        }
        if self.quarantined {
            if self.spared_deferred {
                for active in self.active.iter_mut() {
                    if !active.failure_deferred {
                        defer(Deferred::FailedOwned(
                            active.packet.completion.clone(),
                            ECANCELED,
                        ))?;
                        active.failure_deferred = true;
                    }
                }
                self.spared_deferred = false;
            }
            return Ok(true);
        }
        self.released = true;
        self.retired_by_teardown = true;
        if let Some(active) = self
            .active
            .iter_mut()
            .find(|a| Arc::ptr_eq(&a.packet, packet))
        {
            if !active.failure_deferred {
                defer(Deferred::FailedOwned(
                    active.packet.completion.clone(),
                    ECANCELED,
                ))?;
                active.failure_deferred = true;
            }
        }
        Ok(true)
    }
    /// Firmware is halted and publication is closed while its target is copied.
    pub(crate) fn publish_recovery_target(&self) -> Result {
        let producer = self
            .graph
            .graph
            .word(POINTERS + PointerBlock::PRODUCER)?
            .load(Ordering::Relaxed);
        self.graph
            .graph
            .word(POINTERS + PointerBlock::RECOVERY_TARGET)?
            .store(producer, Ordering::Relaxed);
        Ok(())
    }

    /// The newest covered kick of a QID subsumes its earlier prefix; later independent work
    /// must not become a dependency. Errors are observed before returning any firmware token.
    pub(crate) fn dependency(
        &self,
        owner: u64,
        status: &Arc<VmStatus>,
        through: Option<u64>,
        recovery_state: u32,
    ) -> Result<Option<crate::g17::fw::kick::KickDependency>> {
        if through.is_none() || self.owner != Some(owner) {
            return Ok(None);
        }
        let Some(active) = self
            .active
            .iter()
            .filter(|a| crate::g17::job::Order::contains(through, a.packet.order.sequence))
            .last()
        else {
            return Ok(None);
        };
        if self.quarantined && self.spared_quarantine {
            return Err(ECANCELED);
        }
        if !Arc::ptr_eq(active.packet.completion.status(), status)
            || self.quarantined
            || recovery_state != 0
        {
            return Err(EIO);
        }
        let ticket = active.ticket.ok_or(EIO)?;
        let state = crate::g17::job::fence_status(&active.packet.completion.fence());
        if state < 0 {
            return Err(crate::g17::dependency::inherited_error(Error::from_errno(
                state,
            )));
        }
        if state > 0 {
            return Ok(None);
        }
        active.packet.completion.note_firmware_consumer();
        let kick = crate::g17::fw::kick::KickTimestamp::new(ticket.kick).ok_or(EIO)?;
        Ok(Some(
            crate::g17::fw::kick::KickDependency::new(self.qid(), kick).ok_or(EIO)?,
        ))
    }
}

impl Graph {
    fn record<T: Copy>(
        &mut self,
        slot: Option<usize>,
        offset: usize,
        window: usize,
        value: T,
    ) -> Result<u64> {
        let (object, start) = if let Some(slot) = slot {
            if slot >= RECORD_SLOTS {
                return Err(EINVAL);
            }
            (&mut self.records, slot * RECORD_STRIDE + offset)
        } else {
            (&mut self.graph, 0x6800 + offset)
        };
        if core::mem::size_of::<T>() > window {
            return Err(EINVAL);
        }
        let pointer = object.pointer(start, window)?;
        // SAFETY: The checked record window is not named by any unconsumed item pointer. The
        // device mutex excludes other host publishers, and no Rust reference to it is formed.
        unsafe { pointer.write_bytes(0, window) };
        object.write(start, value)?;
        fence(Ordering::SeqCst);
        Ok(object.gpu_va() + start as u64)
    }
    fn append(&self, expected: u32, address: u64, before_publish: impl FnOnce()) -> Result<u32> {
        let cursors = self.cursors()?;
        if cursors.producer != expected
            || cursors.count != ITEM_RING_ENTRIES
            || expected >= ITEM_RING_ENTRIES
        {
            return Err(EBUSY);
        }
        let next = (expected + 1) % ITEM_RING_ENTRIES;
        if next == cursors.consumer {
            return Err(EBUSY);
        }
        let slot = self.graph.dword(ITEMS + expected as usize * 8)?;
        let producer = self.graph.word(POINTERS + 0x40)?;
        if slot.load(Ordering::Relaxed) != 0 {
            return Err(EBUSY);
        }
        before_publish();
        slot.store(address, Ordering::Relaxed);
        fence(Ordering::SeqCst);
        producer.store(next, Ordering::Relaxed);
        Ok(next)
    }
}

/// Borrowed device operations used during one publication. Implementations borrow the initdata,
/// coprocessor and accounting fields separately from the Registry that owns physical queues.
pub(crate) trait Host {
    fn next_compute_publication(&mut self) -> Result<u64>;
    fn epoch(&self) -> Result<(u64, u32)>;
    fn prepare_compute_shared(&mut self) -> Result;
    fn qos_publish(&mut self, owner: qos::Owner, scheduler: u64) -> Result<qos::Publication>;
    fn qos_cancel(&mut self, publication: qos::Publication) -> Result;
    fn publish_qid(&mut self, id: kick::Id) -> Result;
    fn publish_outer(&mut self, slot: &crate::g17::fw::channels::WorkSlot) -> Result;
    fn notify(&mut self, message: u64) -> Result;
    fn note_submission(&mut self) -> Result;
}

impl Queue {
    pub(crate) fn spared_pending(&self) -> bool {
        self.quarantined && self.spared_quarantine && self.awaiting_witness
    }

    /// Retains the packet before any potentially visible store. An error before publication
    /// rolls back its work count; every ambiguous error quarantines the graph and packet.
    pub(crate) fn publish(
        &mut self,
        packet: Arc<Packet>,
        dependencies: &[crate::g17::fw::kick::KickDependency],
        host: &mut impl Host,
        defer: &mut impl FnMut(Deferred) -> Result,
    ) -> Result {
        use crate::g17::fw::{
            channels::WorkSlot,
            compute::{ComputeArgs, ComputeDescriptor, COMPUTE_KICK_EVENT_MASK, COMPUTE_QOS_CLASS},
            kick::{KickArgs, KickQos},
        };
        let (epoch, state) = host.epoch()?;
        if state != 0 {
            return Err(EBUSY);
        }
        let replay_front = self
            .replays
            .front()
            .is_some_and(|front| Arc::ptr_eq(front, &packet));
        if !self.room()
            && !(replay_front && !self.quarantined && !self.released && self.active.len < IN_FLIGHT)
        {
            return Err(EBUSY);
        }
        if !packet.context.is_current() || !Arc::ptr_eq(&packet.context, &self.binding.context) {
            return Err(EFAULT);
        }
        if packet.completion.status().get() != 0 {
            return Err(EIO);
        }
        let crate::g17::command::Validated::Compute {
            scratch,
            usc_base,
            cdm_va,
            cdm_end_va,
            sampler_heap_va,
            sampler_count,
            ..
        } = &packet.command
        else {
            return Err(EINVAL);
        };
        let work = packet.completion.work_state()?;
        let work_word = work.word();
        if work_word == u32::MAX {
            return Err(EINVAL);
        }
        let work_node = work.node_va()?;
        let timestamp_va = packet.completion.compute_timestamp_va()?;
        let kick_timestamp = self.kick.timestamp();
        let kick_parent = self.kick.parent();
        let qid = self.qid();
        let ordinal = self.ordinal.checked_add(1).ok_or(EOVERFLOW)?;
        let (descriptor_high, descriptor_low) =
            self.graph.descriptor_vas(kick_timestamp.slot() as u8);
        let first = !self.installed && self.ordinal == 0;
        let entry_signal = first
            || dependencies
                .iter()
                .any(|dependency| dependency.qid() != qid);
        let record_slot = (!first).then_some(self.ordinal as usize % RECORD_SLOTS);
        if let Some(slot) = record_slot {
            self.graph.ensure_record_slot(slot)?;
        }
        let args = ComputeArgs {
            scratch: *scratch,
            usc_base: *usc_base,
            cdm_va: *cdm_va,
            cdm_end_va: *cdm_end_va,
            sampler_heap_va: *sampler_heap_va,
            sampler_count: *sampler_count,
            descriptor_va: descriptor_low,
            context_id: packet.context.id().try_into()?,
            context_generation: packet.context.generation(),
            work_state: work_word,
            work_state_va: work_node,
            qid,
            kick_count: self.ordinal,
            kick: kick_timestamp,
            preempt_va: self.binding.preempt.iova(),
            operand_state_va: self.binding.operand.iova(),
            usage_va: self.binding.usage.iova(),
            usage_fw_va: self.graph.usage.gpu_va(),
            free_list_slot: u32::from(self.pool.id()),
            free_list_control_va: self.pool.control_va(),
            stamp_va: self.graph.support.gpu_va() + STAMP as u64,
            aux_stamp_va: self.graph.support.gpu_va() + STAMP as u64 + 8,
            status_va: [
                self.graph.support.gpu_va() + 0x310,
                self.graph.support.gpu_va() + 0x318,
            ],
            timestamp_va,
        };
        let identity = ConfigIdentity {
            scheduler: packet.context.scheduler_va(),
            qos: packet.context.qos_id(),
            context: packet.context.id(),
        };
        let config = QueueConfig::new(&QueueConfigArgs {
            target: QueueConfigTarget::Compute,
            kick_ring_va: self.kick.low_va(),
            kick_ring_fw_va: self.kick.firmware_va(),
            qid,
            install: first,
            completion_seed: None,
            context_id: args.context_id,
            free_list: FreeListBinding {
                va: self.pool.control_va(),
                generation: u64::from(packet.context.scheduler_generation()),
                slot: u32::from(self.pool.id()),
            },
            scheduler_va: identity.scheduler,
            qos_slot: identity.qos,
            qos_update: self
                .published_config
                .is_none_or(|p| p.scheduler != identity.scheduler || p.qos != identity.qos),
            owner_pid: packet.context.owner_pid(),
            context_update: self
                .published_config
                .is_none_or(|p| p.scheduler != identity.scheduler || p.context != identity.context),
        })?;
        let announce = KickAnnounce::new(qid, DataMaster::Compute, kick_timestamp)?;
        let predecessor = KickPredecessor::new(qid, kick_parent)?;
        let kick_args = KickArgs {
            qid,
            timestamp: kick_timestamp,
            parent: kick_parent,
            barriers: dependencies,
            descriptor_va: descriptor_high,
            queue_va: self.graph.graph.gpu_va(),
            qos: KickQos {
                slot: identity.qos,
                class: COMPUTE_QOS_CLASS,
            },
            mcache: None,
            event_mask: COMPUTE_KICK_EVENT_MASK,
            register_arrays: ComputeDescriptor::register_bindings(descriptor_low, *scratch)?,
            compute_scratch: scratch.enabled(),
        };
        let published = host.next_compute_publication()?;
        self.active
            .push(Active {
                packet: packet.clone(),
                published,
                work: work.clone(),
                ticket: None,
                observed: None,
                failure_deferred: false,
                qos_pending: false,
            })
            .map_err(|_| EBUSY)?;
        self.retirement_ready = false;
        let mut visible = false;
        let mut accounted = false;
        let result = (|| -> Result {
            work.add_submitted_kicks(1)?;
            accounted = true;
            self.graph.descriptors.initialize::<ComputeDescriptor>(
                kick_timestamp.slot() * COMPUTE_DESCRIPTOR_SIZE,
                |descriptor| descriptor.write(&args),
            )?;
            if first {
                self.graph.context_high.write(
                    0x200,
                    crate::g17::fw::compute::QueueContextItem::new(
                        descriptor_high,
                        self.graph.graph.gpu_va(),
                        qid,
                    ),
                )?;
            } else {
                // The retained context item remains at index one; only its descriptor changes.
                self.graph.context_high.write(0x210, descriptor_high)?;
                let cursors = self.graph.cursors()?;
                let _ = self.graph.recycle(cursors);
            }
            let config_va = self.graph.record(record_slot, CONFIG, 0xc0, config)?;
            let cursors = self.graph.cursors()?;
            if cursors.count != ITEM_RING_ENTRIES {
                return Err(EIO);
            }
            if cursors.producer >= cursors.count || cursors.consumer >= cursors.count {
                return Err(ERANGE);
            }
            let free = (cursors.consumer + cursors.count - cursors.producer - 1) % cursors.count;
            if free < if entry_signal { 4 } else { 3 } {
                return Err(ENOSPC);
            }
            let prepared_kick = self.kick.prepare_entry(&kick_args)?;
            let mut producer;
            if first {
                producer = self.graph.first_descriptor(descriptor_high)?;
                host.prepare_compute_shared()?;
                packet.mark_published();
                visible = true;
                let qos = host.qos_publish(
                    qos::Owner {
                        qid,
                        qos: identity.qos,
                        data_master: 2,
                    },
                    identity.scheduler,
                )?;
                self.active.back_mut().ok_or(EIO)?.qos_pending = true;
                let target = self.pool.reserve_submission().or_else(|error| {
                    host.qos_cancel(qos)?;
                    self.active.back_mut().ok_or(EIO)?.qos_pending = false;
                    Err(error)
                })?;
                producer = self
                    .graph
                    .append(producer, config_va, || {})
                    .or_else(|error| {
                        self.pool.cancel_unpublished(target)?;
                        host.qos_cancel(qos)?;
                        self.active.back_mut().ok_or(EIO)?.qos_pending = false;
                        Err(error)
                    })?;
            } else {
                let target = self.pool.reserve_submission()?;
                producer = self
                    .graph
                    .append(cursors.producer, descriptor_high, || {
                        packet.mark_published()
                    })
                    .or_else(|error| {
                        self.pool.cancel_unpublished(target)?;
                        Err(error)
                    })?;
                visible = true;
                let qos = host.qos_publish(
                    qos::Owner {
                        qid,
                        qos: identity.qos,
                        data_master: 2,
                    },
                    identity.scheduler,
                )?;
                self.active.back_mut().ok_or(EIO)?.qos_pending = true;
                producer = self
                    .graph
                    .append(producer, config_va, || {})
                    .or_else(|error| {
                        host.qos_cancel(qos)?;
                        self.active.back_mut().ok_or(EIO)?.qos_pending = false;
                        Err(error)
                    })?;
            }
            if first {
                prepared_kick.prepare_compute_install()?;
            }
            prepared_kick.commit();
            self.submitted = self.submitted.checked_add(1).ok_or(EOVERFLOW)?;
            let announce_va = self.graph.record(record_slot, ANNOUNCE, 0x40, announce)?;
            producer = self.graph.append(producer, announce_va, || {})?;
            fence(Ordering::SeqCst);
            if entry_signal {
                let predecessor_va =
                    self.graph
                        .record(record_slot, PREDECESSOR, 0x400, predecessor)?;
                producer = self.graph.append(producer, predecessor_va, || {})?;
            }
            if let Some(slot) = record_slot {
                self.graph.record_ends[slot] = Some(producer);
            }
            host.publish_qid(self.kick.id())?;
            host.publish_outer(&WorkSlot::new(
                DataMaster::Compute,
                self.graph.graph.gpu_va(),
                qid,
                producer.try_into()?,
                !self.outer_started,
            )?)?;
            let notify_activation = !self.outer_started;
            self.outer_started = true;
            self.installed = true;
            self.published_config = Some(identity);
            self.ordinal = ordinal;
            let (accepted, state) = host.epoch()?;
            if state != 0 || accepted != epoch {
                return Err(EIO);
            }
            let ticket = Ticket {
                owner: self.owner.ok_or(EIO)?,
                submission: packet.order.sequence,
                context_id: packet.context.id(),
                qid,
                descriptor: descriptor_high,
                work_node,
                kick: kick_timestamp.get(),
                ordinal,
                item_producer: producer,
                item_count: ITEM_RING_ENTRIES,
                epoch,
            };
            self.active.back_mut().ok_or(EIO)?.ticket = Some(ticket);
            if let Some(previous) = self.previous.as_mut() {
                if previous.publication.is_none() {
                    previous.publication = Some((packet.order.sequence, epoch));
                }
            }
            if notify_activation {
                fence(Ordering::SeqCst);
                host.notify((0x83 << 48) | 0x0a)?;
            }
            fence(Ordering::SeqCst);
            host.notify((0x83 << 48) | 0x10)?;
            host.note_submission()
        })();
        if let Err(error) = result {
            if !visible {
                if accounted {
                    if let Err(error) = work.cancel_unpublished_kicks(1) {
                        self.terminal =
                            self.quarantine(error, defer)?.map(|status| (status, error));
                        return Err(error);
                    }
                }
                drop(self.active.pop_back());
                return Err(error);
            }
            self.terminal = self.quarantine(error, defer)?.map(|status| (status, error));
            return Err(error);
        }
        Ok(())
    }
}

impl Graph {
    /// The queue has never been installed. Retry staging replaces its initial three item slots;
    /// no firmware reader can name this graph until its first complete outer publication.
    fn first_descriptor(&self, address: u64) -> Result<u32> {
        let first = self.graph.dword(ITEMS)?;
        let second = self.graph.dword(ITEMS + 8)?;
        let third = self.graph.dword(ITEMS + 16)?;
        let producer = self.graph.word(POINTERS + 0x40)?;
        first.store(address, Ordering::Relaxed);
        second.store(0, Ordering::Relaxed);
        third.store(0, Ordering::Relaxed);
        producer.store(1, Ordering::Relaxed);
        Ok(1)
    }
}
impl Queue {
    pub(crate) fn take_terminal(&mut self) -> Option<(Arc<VmStatus>, Error)> {
        self.terminal.take()
    }
}
