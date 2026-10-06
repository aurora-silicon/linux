// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Device ownership of physical queues and off-lock construction reservations.
//!
//! The device mutex protects this registry. Logical queues may disappear while
//! their published graphs remain here; only exact retirement permits rebinding,
//! and only processor stop permits dropping installed firmware addresses.
//! A preparation lease keeps Shared::state populated: shutdown and loss join
//! every lease before taking the session, including builders asleep on changed.

mod backend;
mod compute_control;
mod compute_replay;
mod compute_exit;
mod events;
mod feed;
mod render;
pub(super) mod teardown;

use super::{
    buffer::{BufferIds, MetricsIds},
    context::{Context, QosIds},
    fw::queue::DataMaster,
    kick, qos,
    queue::{self, admission, compute},
};
use kernel::{prelude::*, sync::Arc};

const QIDS: usize = 128;
const FIRST_COMPUTE_QID: u8 = 4;
const RECOVERY_SETTLE_POLLS: usize = 200;

/// A reservation remains identifiable while its backing is built without the
/// device mutex. Failed construction returns the slot, never its host identity.
#[derive(Clone, Copy)]
pub(super) struct ComputeReservation {
    slot: usize,
    pub(super) id: kick::Id,
    owner: u64,
}

struct ComputeEntry {
    reservation: ComputeReservation,
    queue: Option<KBox<compute::Queue>>,
}

pub(super) struct Registry {
    pub(super) qids: kick::QueueIds,
    pub(super) buffers: BufferIds,
    pub(super) accounting: qos::Accounting,
    pub(super) metrics: Arc<MetricsIds>,
    pub(super) qos_ids: Arc<QosIds>,
    pub(super) admission: Arc<admission::Pool>,
    render: render::RenderState,
    identities: queue::Identities,
    compute: KVec<Option<ComputeEntry>>,
    pub(super) compute_building: bool,
    compute_scheduler_building: bool,
    pub(super) observations: compute::retirement::Batch,
    worker_deferred: Option<DeferredBatch>,
    teardown: super::teardown::Pending,
    exit_acks: compute::exit::Acks,
}

impl Registry {
    pub(super) fn new(clusters: u32, descriptor_flags: [u32; 2]) -> Result<Self> {
        let mut compute = KVec::with_capacity(QIDS, GFP_KERNEL)?;
        for _ in 0..QIDS {
            compute.push(None, GFP_KERNEL)?;
        }
        let mut qids = kick::QueueIds::new();
        qids.reserve(1, DataMaster::Tiling, Some(0))?;
        qids.reserve(2, DataMaster::Fragment, Some(1))?;
        Ok(Self {
            qids,
            buffers: BufferIds::new(),
            accounting: qos::Accounting::new(),
            metrics: Arc::new(MetricsIds::new(), GFP_KERNEL)?,
            qos_ids: QosIds::new()?,
            admission: admission::Pool::new()?,
            render: render::RenderState::new(clusters, descriptor_flags)?,
            identities: queue::Identities::new(),
            compute,
            compute_building: false,
            compute_scheduler_building: false,
            observations: compute::retirement::Batch::new()?,
            worker_deferred: Some(DeferredBatch::worker()?),
            teardown: super::teardown::Pending::new()?,
            exit_acks: compute::exit::Acks::new(),
        })
    }

    pub(super) fn compute(&self, owner: u64) -> Option<&compute::Queue> {
        self.compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.owner() == Some(owner))
    }

    pub(super) fn compute_mut(&mut self, owner: u64) -> Option<&mut compute::Queue> {
        self.compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.owner() == Some(owner))
    }

    /// An installed idle queue keeps its original QID and backing. Mapping a
    /// replacement context must succeed before its old binding is displaced.
    pub(super) fn rebind_compute(&mut self, owner: u64, context: &Arc<Context>) -> Result<bool> {
        if let Some(queue) = self.compute(owner) {
            return queue.matches(owner, queue.qid(), context);
        }
        let Some(queue) = self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.reusable())
        else {
            return Ok(false);
        };
        queue.bind_owner(owner, context.clone())?;
        Ok(true)
    }

    pub(super) fn reserve_compute(&mut self, owner: u64) -> Result<ComputeReservation> {
        if owner == 0
            || self.compute.iter().flatten().any(|entry| {
                entry
                    .queue
                    .as_ref()
                    .map_or(entry.reservation.owner == owner, |queue| {
                        queue.owner() == Some(owner)
                    })
            })
        {
            return Err(EINVAL);
        }
        let slot = self
            .compute
            .iter()
            .position(Option::is_none)
            .ok_or(ENOSPC)?;
        let identity = self.identities.reserve(1)?.start;
        let id = self
            .qids
            .reserve(identity, DataMaster::Compute, Some(FIRST_COMPUTE_QID))?;
        let reservation = ComputeReservation { slot, id, owner };
        self.compute[slot] = Some(ComputeEntry {
            reservation,
            queue: None,
        });
        Ok(reservation)
    }

    fn reserved(&mut self, reservation: ComputeReservation) -> Result<&mut ComputeEntry> {
        let entry = self
            .compute
            .get_mut(reservation.slot)
            .and_then(Option::as_mut)
            .ok_or(EIO)?;
        if entry.reservation.id != reservation.id
            || entry.reservation.owner != reservation.owner
            || entry.queue.is_some()
        {
            return Err(EIO);
        }
        Ok(entry)
    }

    /// The caller checked its preparation epoch and retained the same device
    /// session before placing this graph where publication can find it.
    pub(super) fn install_compute(
        &mut self,
        reservation: ComputeReservation,
        queue: &mut Option<KBox<compute::Queue>>,
    ) -> Result {
        let built = queue.as_ref().ok_or(EINVAL)?;
        if built.qid() != reservation.id.qid() || built.owner() != Some(reservation.owner) {
            return Err(EIO);
        }
        let entry = self.reserved(reservation)?;
        entry.queue = queue.take();
        Ok(())
    }

    pub(super) fn cancel_compute(&mut self, reservation: ComputeReservation) -> Result {
        self.reserved(reservation)?;
        self.qids.cancel_unpublished(reservation.id)?;
        self.compute[reservation.slot] = None;
        Ok(())
    }

    pub(super) fn release_compute(&mut self, owner: u64) -> Result {
        let Some(slot) = self.compute.iter().position(|entry| {
            entry
                .as_ref()
                .and_then(|entry| entry.queue.as_ref())
                .is_some_and(|queue| queue.owner() == Some(owner))
        }) else {
            return Ok(());
        };
        let entry = self.compute[slot].as_mut().ok_or(EIO)?;
        if entry.queue.as_mut().ok_or(EIO)?.release_owner()? {
            self.qids.cancel_unpublished(entry.reservation.id)?;
            self.compute[slot] = None;
        }
        Ok(())
    }
}

/// An immutable logical owner shared by its two scheduler entities. Installed
/// physical graphs retain the context independently of this frontend lifetime.
pub(super) struct Backend {
    shared: Arc<super::Shared>,
    pub(super) context: Arc<Context>,
    owner: u64,
    backlog: core::sync::atomic::AtomicU32,
}

impl Backend {
    pub(super) fn new(
        shared: &Arc<super::Shared>,
        vm: &crate::mmu::Vm,
        owner: u64,
    ) -> Result<(Arc<Self>, admission::QueueSlot)> {
        let lease = shared.preparations.enter(true)?;
        let (uat, pool, qos, execution, slot) = {
            let mut state = shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if shared.crashed.load(core::sync::atomic::Ordering::Acquire) {
                return Err(EIO);
            }
            let slot = firmware.queues.admission.reserve_queue()?;
            let execution = firmware.uat.new_execution_context(vm)?;
            (
                firmware.uat.clone(),
                firmware.pool.clone(),
                firmware.queues.qos_ids.clone(),
                execution,
                slot,
            )
        };
        let alloc = super::object::Allocator {
            dev: &shared.dev,
            uat: &uat,
        };
        let owner_pid =
            u32::try_from(kernel::current!().group_leader().pid()).map_err(|_| EOVERFLOW)?;
        let context = Arc::new(
            Context::new(&alloc, &pool, &qos, execution, owner_pid)?,
            GFP_KERNEL,
        )?;
        let state = shared.state.lock();
        if !lease.is_current() || state.is_none() {
            return Err(EAGAIN);
        }
        drop(state);
        Ok((
            Arc::new(
                Self {
                    shared: shared.clone(),
                    context,
                    owner,
                    backlog: core::sync::atomic::AtomicU32::new(0),
                },
                GFP_KERNEL,
            )?,
            slot,
        ))
    }

    /// The shared aperture has exactly one builder. Other queues sleep without
    /// holding the device mutex; recovery joins its lease before closing roots.
    fn shared_compute(&self) -> Result {
        loop {
            let lease = self.shared.preparations.enter(false)?;
            let uat = {
                let mut state = self.shared.state.lock();
                loop {
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if self
                        .shared
                        .crashed
                        .load(core::sync::atomic::Ordering::Acquire)
                    {
                        return Err(EIO);
                    }
                    if !lease.is_current() {
                        return Err(EAGAIN);
                    }
                    if firmware.init.has_compute_shared() {
                        return firmware.init.cache_compute_vm(self.context.vm());
                    }
                    if !firmware.queues.compute_building {
                        firmware.queues.compute_building = true;
                        break firmware.uat.clone();
                    }
                    self.shared.changed.wait(&mut state);
                }
            };
            let alloc = super::object::Allocator {
                dev: &self.shared.dev,
                uat: &uat,
            };
            let mut built = Some(super::initdata::ComputeShared::new(&alloc));
            let result = {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() {
                    Err(EAGAIN)
                } else {
                    built
                        .take()
                        .ok_or(EIO)?
                        .and_then(|built| firmware.init.install_compute_shared(built))
                        .and_then(|()| firmware.init.cache_compute_vm(self.context.vm()))
                }
            };
            drop(built);
            {
                let mut state = self.shared.state.lock();
                (*state)
                    .as_deref_mut()
                    .ok_or(ENODEV)?
                    .queues
                    .compute_building = false;
            }
            self.shared.changed.notify_all();
            drop(lease);
            if result != Err(EAGAIN) {
                return result;
            }
        }
    }

    /// Uses the queue builder's existing lease. Entering another closed epoch
    /// here would make recovery wait for the very builder blocked on reopening.
    fn compute_scheduler(
        &self,
        alloc: &super::object::Allocator<'_>,
        lease: &super::preparation::Lease,
    ) -> Result {
        {
            let mut state = self.shared.state.lock();
            loop {
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() {
                    return Err(EAGAIN);
                }
                if firmware.init.has_compute_scheduler() {
                    return Ok(());
                }
                if !firmware.queues.compute_scheduler_building {
                    firmware.queues.compute_scheduler_building = true;
                    break;
                }
                self.shared.changed.wait(&mut state);
            }
        }
        let mut built = Some(super::initdata::ComputeScheduler::new(alloc));
        let result = {
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() {
                Err(EAGAIN)
            } else {
                built
                    .take()
                    .ok_or(EIO)?
                    .and_then(|built| firmware.init.install_compute_scheduler(built))
            }
        };
        drop(built);
        {
            let mut state = self.shared.state.lock();
            (*state)
                .as_deref_mut()
                .ok_or(ENODEV)?
                .queues
                .compute_scheduler_building = false;
        }
        self.shared.changed.notify_all();
        result
    }

    fn build_compute(&self) -> Result {
        loop {
            match self.shared_compute() {
                Err(error) if error == EAGAIN => continue,
                result => {
                    result?;
                    break;
                }
            }
        }
        loop {
            let lease = self.shared.preparations.enter(false)?;
            let (uat, reservation) = {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if self
                    .shared
                    .crashed
                    .load(core::sync::atomic::Ordering::Acquire)
                {
                    return Err(EIO);
                }
                if firmware.queues.rebind_compute(self.owner, &self.context)? {
                    return Ok(());
                }
                let reservation = firmware.queues.reserve_compute(self.owner)?;
                (firmware.uat.clone(), reservation)
            };
            let alloc = super::object::Allocator {
                dev: &self.shared.dev,
                uat: &uat,
            };
            let built = (|| {
                let pool = compute::PoolBacking::new(&alloc, reservation.id)?;
                {
                    let mut state = self.shared.state.lock();
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if !lease.is_current() {
                        return Err(EAGAIN);
                    }
                    firmware.queues.reserved(reservation)?;
                    let (slot, page_list) = pool.pool_descriptor();
                    firmware.init.initialize_compute_pool(slot, page_list)?;
                }
                let backing = compute::Backing::new(&alloc, pool)?;
                // The global scheduler is created after pool/kick backing and
                // retained even if this queue's later descriptor graph fails.
                self.compute_scheduler(&alloc, &lease)?;
                let queue = compute::Queue::new(&alloc, backing, self.owner, self.context.clone())?;
                Ok(KBox::new(queue, GFP_KERNEL)?)
            })();
            let (mut built, build_error) = match built {
                Ok(queue) => (Some(queue), None),
                Err(error) => (None, Some(error)),
            };
            let result = {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() {
                    Err(EAGAIN)
                } else if let Some(error) = build_error {
                    Err(error)
                } else {
                    firmware.queues.install_compute(reservation, &mut built)
                }
            };
            // The old aliases must disappear before a waiting constructor can
            // reuse the QID and its queue-local free-list identity. Keep the
            // preparation lease until both mappings and reservation are gone.
            drop(built);
            if result.is_err() {
                let mut state = self.shared.state.lock();
                (*state)
                    .as_mut()
                    .ok_or(ENODEV)?
                    .queues
                    .cancel_compute(reservation)?;
            }
            drop(lease);
            if result != Err(EAGAIN) {
                return result;
            }
        }
    }
}

/// One device transaction can fail each retained kick once and then retire it.
/// The physical namespace bounds the batch independently of open logical queues.
/// Collection failure leaves the engine's Active owner intact for a stopped retry.
pub(super) struct DeferredBatch {
    values: KVVec<super::completion::Deferred>,
}
impl DeferredBatch {
    const DEPTH: usize = if crate::hw::t8140::queues::RENDER_DEPTH as usize
        > crate::hw::t8140::queues::COMPUTE_DEPTH
    {
        crate::hw::t8140::queues::RENDER_DEPTH as usize
    } else {
        crate::hw::t8140::queues::COMPUTE_DEPTH
    };

    /// Reused by the sole event work item; no allocation on a completion pass.
    fn worker() -> Result<Self> {
        Self::with_capacity(2 * QIDS * Self::DEPTH)
    }
    /// One publication can quarantine only its selected physical queue/pair.
    /// VM-wide settlement is a separate transaction before publishing its error.
    fn publication() -> Result<Self> {
        Self::with_capacity(2 * Self::DEPTH)
    }
    fn with_capacity(capacity: usize) -> Result<Self> {
        Ok(Self {
            values: KVVec::with_capacity(capacity, GFP_KERNEL)?,
        })
    }
    /// A recovery pass can fail and retire each retained command at most once.
    /// Reserve before changing queues, plus a full publication for its caller.
    /// Healthy submissions keep the small publication collector unchanged.
    fn reserve_replays(&mut self, commands: usize) -> Result {
        self.values
            .reserve(2 * commands + 2 * Self::DEPTH, GFP_KERNEL)?;
        Ok(())
    }
    pub(super) fn push(&mut self, value: super::completion::Deferred) -> Result {
        use super::completion::Deferred;
        // Publishers may acquire the device mutex as soon as this pass drops
        // it. Expose terminal VM status now, before off-lock fence callbacks.
        // Recovery-spared and cancelled commands never poison their VM.
        if let Deferred::FailedOwned(completion, error) = &value {
            if *error != ECANCELED && !completion.spared() {
                completion.status().record(*error);
            }
        }
        self.values.push_within_capacity(value).map_err(|_| EIO)
    }
    /// Called only after releasing the device mutex.
    pub(super) fn finish(&mut self) {
        for value in self.values.drain_all() {
            value.finish();
        }
    }
}

struct ComputeHost<'a> {
    init: &'a super::initdata::InitData,
    primary: &'a mut super::Coprocessor,
    effort: &'a mut super::power::Effort,
    qids: &'a mut kick::QueueIds,
    accounting: &'a mut qos::Accounting,
    generation: u64,
}
impl compute::Host for ComputeHost<'_> {
    fn epoch(&self) -> Result<(u64, u32)> {
        Ok((
            self.generation,
            super::recovery::Memory::recovery_state(self.init)?,
        ))
    }
    fn prepare_compute_shared(&mut self) -> Result {
        self.init.activate_compute()
    }
    fn qos_publish(&mut self, owner: qos::Owner, scheduler: u64) -> Result<qos::Publication> {
        self.accounting.publish(
            &qos::View::new(self.init.qos()?)?,
            owner,
            scheduler,
            qos::clock,
        )
    }
    fn qos_cancel(&mut self, publication: qos::Publication) -> Result {
        self.accounting
            .cancel(&qos::View::new(self.init.qos()?)?, publication)
    }
    fn publish_qid(&mut self, id: kick::Id) -> Result {
        self.qids.publish(id)
    }
    fn publish_outer(&mut self, slot: &super::fw::channels::WorkSlot) -> Result {
        self.init.publish_work(slot).map(|_| ())
    }
    fn notify(&mut self, message: u64) -> Result {
        self.primary.notify(message)
    }
    fn note_submission(&mut self) -> Result {
        self.effort.submit();
        super::Firmware::apply_shared_effort(self.init, self.effort, super::power::FULL_EFFORT)
    }
}

impl super::Firmware {
    fn publish_compute(
        &mut self,
        owner: u64,
        packet: Arc<super::job::Packet>,
        dependencies: &[super::fw::kick::KickDependency],
        deferred: &mut DeferredBatch,
    ) -> Result {
        if self.recovery.pending() {
            return Err(EAGAIN);
        }
        let queues = &mut self.queues;
        let queue = queues
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.owner() == Some(owner))
            .ok_or(EIO)?;
        let mut host = ComputeHost {
            init: &self.init,
            primary: &mut self.primary,
            effort: &mut self.effort,
            qids: &mut queues.qids,
            accounting: &mut queues.accounting,
            generation: self.recovery.generation(),
        };
        queue.publish(packet, dependencies, &mut host, &mut |event| {
            deferred.push(event)
        })
    }
}

impl Registry {
    fn release_compute_witness(
        &mut self,
        init: &super::initdata::InitData,
        owner: u64,
        deferred: &mut DeferredBatch,
    ) -> Result<bool> {
        let Some(queue) = self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.owner() == Some(owner))
        else {
            return Ok(false);
        };
        if !queue.spared_pending() {
            return Ok(false);
        }
        let view = qos::View::new(init.qos()?)?;
        queue.release_witnessed(
            super::recovery::Memory::recovery_state(init)?,
            |owner| self.accounting.complete(&view, &[owner]),
            &mut |event| deferred.push(event),
        )?;
        Ok(queue.spared_pending())
    }

    fn fail_compute_vm(
        &mut self,
        status: &Arc<super::status::VmStatus>,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        for queue in self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            if queue.in_flight() && Arc::ptr_eq(queue.context().status(), status) {
                queue.quarantine(error, &mut |event| deferred.push(event))?;
            }
        }
        Ok(())
    }
}

impl super::Firmware {
    /// Latch one shared snapshot for all selected queues before settling any VM.
    /// A later queue's exact success in this batch wins over a peer's failure.
    fn scan_compute(&mut self, masks: Option<[u64; 2]>, deferred: &mut DeferredBatch) -> Result {
        let state = super::recovery::Memory::recovery_state(&self.init)?;
        let generation = self.recovery.generation();
        let registry = &mut self.queues;
        registry.observations.clear();
        let mut requested = false;
        for queue in registry
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
        {
            requested |= queue.request_observations(&mut registry.observations, masks)?;
        }
        if requested {
            self.init
                .scan_compute_completions(|record| registry.observations.fold(record))?;
        }
        let view = qos::View::new(self.init.qos()?)?;
        for queue in registry
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            queue.latch_observations(&mut registry.observations, masks)?;
            queue.retire(
                masks,
                generation,
                state,
                |owner| registry.accounting.complete(&view, &[owner]),
                &mut |event| deferred.push(event),
            )?;
        }
        // All compute observations in this batch are latched before a VM
        // failure can settle its render peers. Exact successes win first.
        for index in 0..self.queues.compute.len() {
            let terminal = self.queues.compute[index]
                .as_mut()
                .and_then(|entry| entry.queue.as_deref_mut())
                .and_then(compute::Queue::take_terminal);
            if let Some((status, error)) = terminal {
                status.record(error);
                self.settle_render_vm(&status, error, deferred)?;
                self.queues.fail_compute_vm(&status, error, deferred)?;
            }
        }
        let view = qos::View::new(self.init.qos()?)?;
        let registry = &mut self.queues;
        for queue in registry
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            queue.release_witnessed(
                state,
                |owner| registry.accounting.complete(&view, &[owner]),
                &mut |event| deferred.push(event),
            )?;
        }
        Ok(())
    }
}

impl Backend {
    fn ensure_compute(&self) -> Result {
        use kernel::time::{delay::fsleep, Delta};
        const SPARED_WAIT_MS: usize = 200;
        let mut deferred = DeferredBatch::publication()?;
        for _ in 0..SPARED_WAIT_MS {
            let pending = (|| {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                firmware.replay_compute(&mut deferred)?;
                firmware
                    .queues
                    .release_compute_witness(&firmware.init, self.owner, &mut deferred)
            })();
            deferred.finish();
            let pending = pending?;
            if !pending {
                break;
            }
            // Other owners continue submitting while this owner gives its
            // queue-local drain witness time to arrive.
            fsleep(Delta::from_millis(1));
        }
        self.build_compute()
    }

    /// Called after taking this packet's publication claim. All waits and work
    /// state allocation precede the device mutex; only the checked epoch commits.
    fn compute_packet(
        &self,
        packet: &Arc<super::job::Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        packet.check_dependencies()?;
        let mut recovery_polls = 0;
        loop {
            if packet.completion.status().get() != 0 {
                return Err(EIO);
            }
            let lease = self.shared.preparations.enter(false)?;
            packet.completion.work_state()?.acquire()?;
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() {
                drop(state);
                continue;
            }
            if packet.cancel_requested() {
                return Err(ECANCELED);
            }
            if self
                .shared
                .crashed
                .load(core::sync::atomic::Ordering::Acquire)
            {
                return Err(EIO);
            }
            let recovery_state = super::recovery::Memory::recovery_state(&firmware.init)?;
            if firmware.recovery.pending() || recovery_state != 0 {
                if recovery_polls == RECOVERY_SETTLE_POLLS {
                    return Err(EBUSY);
                }
                recovery_polls += 1;
                drop(state);
                drop(lease);
                self.shared.queue_events();
                kernel::time::delay::fsleep(kernel::time::Delta::from_millis(1));
                continue;
            }
            firmware.replay_compute(deferred)?;
            let dependencies = firmware.queues.render_prefix(self.owner, packet)?;
            let result = firmware.publish_compute(
                self.owner,
                packet.clone(),
                dependencies.as_slice(),
                deferred,
            );
            if matches!(result, Err(EBUSY) | Err(EAGAIN))
                && !packet.is_published()
                && (firmware.recovery.pending()
                    || super::recovery::Memory::recovery_state(&firmware.init)? != 0)
            {
                if recovery_polls == RECOVERY_SETTLE_POLLS {
                    return Err(EBUSY);
                }
                recovery_polls += 1;
                drop(state);
                drop(lease);
                self.shared.queue_events();
                kernel::time::delay::fsleep(kernel::time::Delta::from_millis(1));
                continue;
            }
            return result;
        }
    }
}

impl super::Firmware {
    /// Both processors are stopped and their owner detached before this call.
    /// Error fences release their command mappings without the device mutex
    /// or an allocation-dependent collector.
    fn finish_stopped_queues(&mut self) {
        self.stop_render();
        for queue in self
            .queues
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            let result = queue.stopped(&mut |event| {
                event.finish();
                Ok(())
            });
            if let Err(error) = result {
                dev_err!(
                    self.primary.state.shared.dev.as_ref(),
                    "Could not settle stopped compute queue: {:?}\n",
                    error
                );
            }
        }
        self.queues
            .admission
            .release_retained_owners_after_processor_stop();
    }
}

impl super::Shared {
    /// Joins construction and serializes the stop writes with detachment.
    /// After installation, seeing no owner proves that an earlier stop caller
    /// has already stopped both processors. Callbacks and destruction follow
    /// outside the device mutex.
    pub(super) fn stop_queues(&self) {
        self.preparations.wait_drained();
        let firmware = {
            let mut state = self.state.lock();
            if let Some(firmware) = (*state).as_deref_mut() {
                firmware.secondary.stop();
                firmware.primary.stop();
            }
            state.take()
        };
        if let Some(mut firmware) = firmware {
            firmware.finish_stopped_queues();
        }
    }
}

/// One monotonic domain for recovery grace, idle effort and control retirement.
fn now_ns() -> u64 {
    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get() as u64
}
