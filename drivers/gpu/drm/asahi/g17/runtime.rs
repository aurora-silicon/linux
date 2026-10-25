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

pub(super) use super::now_ns;
use super::{
    buffer::{BufferIds, MetricsIds},
    context::{Context, QosIds},
    fw::queue::DataMaster,
    kick, qos,
    queue::{self, admission, compute},
};
use super::preparation::Admission;
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};

const QIDS: usize = 128;
const FIRST_COMPUTE_QID: u8 = 4;
const RECOVERY_SETTLE_POLLS: usize = 200;
const WORK_RING_WAIT_NS: u64 = super::job::TIMEOUT_MS as u64 * 1_000_000;

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
    replay_outer_wait: Option<u64>,
    pub(super) observations: compute::retirement::Batch,
    worker_deferred: Option<DeferredBatch>,
    teardown: super::teardown::Pending,
}

impl Registry {
    /// Initialize inside the firmware allocation instead of staging the whole
    /// registry on the probe stack. No additional allocation is required.
    pub(super) fn new(
        clusters: u32,
        deflake: crate::g17::fw::render::Deflake,
        descriptor_flags: [u32; 2],
        variant: &'static crate::g17::fw::Variant,
    ) -> impl Init<Self, Error> {
        kernel::try_init!(Self {
            qids <- kick::QueueIds::new(variant.qid_floor).chain(|qids| {
                qids.reserve_initial_render()
            }),
            buffers <- BufferIds::new(),
            accounting: qos::Accounting::new(),
            metrics: Arc::new(MetricsIds::new(), GFP_KERNEL)?,
            qos_ids: QosIds::new()?,
            admission: admission::Pool::new(kick::queue_limit(variant.qid_floor))?,
            render: render::RenderState::new(clusters, deflake, descriptor_flags)?,
            identities: queue::Identities::new(),
            compute: {
                let mut compute = KVec::with_capacity(QIDS, GFP_KERNEL)?;
                for _ in 0..QIDS {
                    compute.push(None, GFP_KERNEL)?;
                }
                compute
            },
            compute_building: false,
            compute_scheduler_building: false,
            replay_outer_wait: None,
            observations: compute::retirement::Batch::new()?,
            worker_deferred: Some(DeferredBatch::worker()?),
            teardown: super::teardown::Pending::new()?,
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
    /// A vacant queue (USC backing given back) is offered for re-arming only
    /// when no populated idle queue exists.
    pub(super) fn rebind_compute(&mut self, owner: u64, context: &Arc<Context>, required: usize, admission: &Admission<'_>) -> Result<Rebind> {
        admission.check()?;
        if let Some(queue) = self.compute(owner) {
            return Ok(if queue.matches(owner, queue.qid(), context)? {
                if queue.pool_fits(required) { Rebind::Done } else { Rebind::Grow }
            } else {
                Rebind::Fresh
            });
        }
        // Physical channels are a device budget, not a permanent entitlement
        // of each open logical queue. Only exact idle retirement can yield one.
        for queue in self.compute.iter_mut().flatten().filter_map(|entry| entry.queue.as_deref_mut()) {
            admission.check()?;
            queue.share_idle()?;
        }
        let mut vacant = None;
        let teardown = &self.teardown;
        for queue in self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .filter(|queue| queue.reusable())
        {
            admission.check()?;
            // The release of the scheduler these records were bound to is armed
            // or not yet consumed: the firmware may process it against this
            // queue, which therefore must not change owner until it has.
            if queue.context().is_some_and(|bound| teardown.releasing(bound)) {
                continue;
            }
            if queue.pool_vacant() {
                vacant.get_or_insert(queue.qid());
                continue;
            }
            if !queue.pool_fits(required) { continue; }
            queue.bind_owner(owner, context.clone())?;
            return Ok(Rebind::Done);
        }
        Ok(vacant.map_or(Rebind::Fresh, Rebind::Rearm))
    }

    /// Installs fresh USC backing into the vacant queue `qid`; the caller writes
    /// the returned descriptor row before binding. EAGAIN if the queue was taken.
    pub(super) fn rearm_compute(
        &mut self,
        qid: u8,
        pages: crate::g17::freelist::Pages,
        ring: compute::DescriptorRing,
    ) -> Result<(u16, u64)> {
        let queue = self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == qid)
            .ok_or(EAGAIN)?;
        queue.rearm(pages, ring)
    }

    pub(super) fn bind_rearmed(&mut self, qid: u8, owner: u64, context: &Arc<Context>) -> Result {
        let teardown = &self.teardown;
        let queue = self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == qid && queue.reusable())
            .ok_or(EAGAIN)?;
        // Its last owner may have closed while the backing was rebuilt.
        if queue.context().is_some_and(|bound| teardown.releasing(bound)) {
            return Err(EAGAIN);
        }
        queue.bind_owner(owner, context.clone())
    }

    /// Delay before retrying pools a turn could not release.
    const POOL_RETRY_NS: u64 = 100_000_000;
    /// Publishes releases for the oldest idle retained pools beyond `cap` and
    /// for every pool released at least `expiry` before `now`, at most four
    /// per pass. Returns the number published and the delay until the next
    /// retained pool expires or a retry is due.
    pub(super) fn vacate_idle_pools(
        &mut self,
        init: &super::initdata::InitData,
        cap: usize,
        now: u64,
        expiry: u64,
    ) -> Result<(usize, Option<u64>)> {
        // Count first: event turns are frequent and the steady state has no excess.
        let mut forced = 0;
        let mut count = 0;
        let mut next = None::<u64>;
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.vacate_candidate())
        {
            count += 1;
            let released = queue.released_at();
            let age = now.saturating_sub(released);
            if queue.needs_fresh_backing() || (released != 0 && age >= expiry) {
                forced += 1;
            } else if released != 0 {
                let left = expiry - age;
                next = Some(next.map_or(left, |next| next.min(left)));
            }
        }
        if count <= cap && forced == 0 {
            return Ok((0, next));
        }
        let mut candidates: KVec<(u64, u8)> = KVec::with_capacity(count, GFP_KERNEL)?;
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.vacate_candidate())
        {
            // Recycled killed queues sort first (time 0) and count beyond the cap.
            let key = if queue.needs_fresh_backing() { 0 } else { queue.released_at() };
            candidates.push((key, queue.qid()), GFP_KERNEL)?;
        }
        // Recycled and expired pools are the oldest keys and sort first.
        candidates.sort_unstable();
        let excess = candidates.len().saturating_sub(cap).max(forced);
        let mut published = 0;
        for (_, qid) in candidates.iter().take(excess.min(4)) {
            let queue = self
                .compute
                .iter_mut()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref_mut())
                .find(|queue| queue.qid() == *qid)
                .ok_or(EIO)?;
            match queue.vacate(init) {
                Err(error) if error == EAGAIN => break,
                result => result?,
            }
            published += 1;
        }
        if published < excess {
            // Beyond this turn's bound, or not yet releasable: retry soon.
            next = Some(next.map_or(Self::POOL_RETRY_NS, |next| next.min(Self::POOL_RETRY_NS)));
        }
        Ok((published, next))
    }

    /// Frees the backing of every pool whose release the firmware consumed.
    pub(super) fn observe_pool_releases(
        &mut self,
        init: &super::initdata::InitData,
        deferred: &mut DeferredBatch,
    ) -> Result<usize> {
        fence(Ordering::Acquire);
        let consumer = init.control_consumer()?.load(Ordering::Relaxed);
        let producer = init.control_producer()?.load(Ordering::Relaxed);
        let mut freed = 0;
        for queue in self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            if !deferred.vacated_room() { break; }
            if let Some(vacated) = queue.observe_vacancy(consumer, producer)? {
                deferred.defer_vacated(vacated);
                freed += 1;
            }
        }
        Ok(freed)
    }

    /// Builders and released queues waiting for their exact retirement or pool
    /// witness temporarily occupy identities. A failed admission must let those
    /// transactions finish rather than fail a healthy successor immediately.
    fn compute_admission_error(&self, error: Error) -> Error {
        if error == ENOSPC {
            EAGAIN
        } else {
            error
        }
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
            .ok_or_else(|| self.compute_admission_error(
                crate::g17::capacity::refused(crate::g17::capacity::Site::ComputeSlots)))?;
        let identity = self.identities.reserve(1)?.start;
        let id = self
            .qids
            .reserve(identity, DataMaster::Compute, Some(FIRST_COMPUTE_QID))
            .map_err(|error| self.compute_admission_error(error))?;
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

/// Outcome of offering an owner a retained compute queue.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) enum Rebind {
    /// The owner now holds an installed, populated queue.
    Done,
    Grow,
    /// Only a vacant queue is available; re-arm `qid` with fresh backing.
    Rearm(u8),
    /// No retained queue suits the owner: reserve a fresh QID.
    Fresh,
}


#[kunit_tests(asahi_g17_uninstalled_cleanup)]
mod uninstalled_cleanup_tests {
    use super::*;
    use crate::g17::fw::render::Deflake;
    use crate::g17::fw::variant::V26A428;
    #[test]
    fn empty_reservation_prevents_reuse_until_explicit_cancellation() {
        let mut registry = KBox::init(
            Registry::new(1, Deflake::ONE_CLUSTER, [0; 2], &V26A428),
            GFP_KERNEL,
        )
        .unwrap();
        let first = registry.reserve_compute(1).unwrap();
        assert!(registry.reserved(first).is_ok());
        assert!(registry.reserve_compute(1).is_err());
        let second = registry.reserve_compute(2).unwrap();
        assert_ne!(first.id.qid(), second.id.qid());
        registry.cancel_compute(first).unwrap();
        assert!(registry.reserved(first).is_err());
        let next = registry.reserve_compute(3).unwrap();
        assert_eq!(first.id.qid(), next.id.qid());
        assert!(first.id != next.id);
    }
}

/// An immutable logical owner shared by its two scheduler entities. Installed
/// physical graphs retain the context independently of this frontend lifetime.
pub(super) struct Backend {
    shared: Arc<super::Shared>,
    pub(super) context: Arc<Context>,
    owner: u64,
    backlog: core::sync::atomic::AtomicU32,
    closed: core::sync::atomic::AtomicBool,
}

impl Backend {
    pub(super) fn new(
        shared: &Arc<super::Shared>,
        vm: &crate::mmu::Vm,
        owner: u64,
        policy: super::fw::queue::Policy,
    ) -> Result<(Arc<Self>, admission::QueueSlot)> {
        // A closed queue keeps its application context until its scheduler
        // release is consumed and the context is returned. When only such
        // contexts block this one, wait (bounded) for the teardown instead of
        // failing a live client; the preparation lease is not held while
        // sleeping, and nothing of the new queue is visible yet.
        const RETAINED_CONTEXT_WAIT_NS: u64 = 4 * WORK_RING_WAIT_NS;
        let started = now_ns();
        let (lease, uat, pool, qos, execution, slot) = loop {
            let lease = shared.preparations.enter(true)?;
            let mut state = shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if shared.crashed.load(core::sync::atomic::Ordering::Acquire) {
                return Err(EIO);
            }
            let slot = firmware.queues.admission.reserve_queue()?;
            match firmware.uat.new_execution_context(vm) {
                Ok(execution) => {
                    break (
                        lease,
                        firmware.uat.clone(),
                        firmware.pool.clone(),
                        firmware.queues.qos_ids.clone(),
                        execution,
                        slot,
                    )
                }
                Err(error) if error == ENOSPC => {
                    // Every open queue holds one context; this one is counted
                    // in `queues()` already but holds none yet.
                    let open = firmware.queues.admission.queues().saturating_sub(1);
                    let closing = firmware.uat.execution_contexts_used().saturating_sub(open);
                    if closing == 0 || now_ns().saturating_sub(started) >= RETAINED_CONTEXT_WAIT_NS {
                        return Err(super::capacity::refused(super::capacity::Site::ExecutionContexts));
                    }
                    drop(slot);
                    drop(state);
                    drop(lease);
                    // Nothing is visible yet: a signalled creator restarts.
                    if kernel::current!().signal_pending() {
                        return Err(ERESTARTSYS);
                    }
                    shared.queue_events();
                    kernel::time::delay::fsleep(kernel::time::Delta::from_millis(1));
                }
                Err(error) => return Err(error),
            }
        };
        let dev = shared.drm()?;
        let alloc = super::object::Allocator {
            dev: &dev,
            uat: &uat,
        };
        let owner_pid =
            u32::try_from(kernel::current!().group_leader().pid()).map_err(|_| EOVERFLOW)?;
        let context = Arc::new(
            Context::new(&alloc, &pool, &qos, execution, owner_pid, policy)?,
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
                    closed: core::sync::atomic::AtomicBool::new(false),
                },
                GFP_KERNEL,
            )?,
            slot,
        ))
    }

    /// The shared aperture has exactly one builder. Other queues sleep without
    /// holding the device mutex; recovery joins its lease before closing roots.
    fn shared_compute(&self, admission: &Admission<'_>) -> Result {
        loop {
            admission.check()?;
            let lease = self.shared.preparations.enter_until(admission)?;
            let dev = self.shared.drm()?;
            let uat = {
                let mut state = self.shared.state.lock();
                loop {
                    admission.check()?;
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
                    let _ = self.shared.changed.wait_timeout(
                        &mut state, kernel::time::msecs_to_jiffies(admission.slice_ms()?)
                    );
                }
            };
            let alloc = super::object::Allocator {
                dev: &dev,
                uat: &uat,
            };
            let mut built = Some(super::initdata::ComputeShared::new(&alloc));
            let result = {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if let Err(error) = admission.check() {
                    Err(error)
                } else if !lease.is_current() {
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
        admission: &Admission<'_>,
    ) -> Result {
        {
            let mut state = self.shared.state.lock();
            loop {
                admission.check()?;
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
                let _ = self.shared.changed.wait_timeout(
                    &mut state, kernel::time::msecs_to_jiffies(admission.slice_ms()?)
                );
            }
        }
        let mut built = Some(super::initdata::ComputeScheduler::new(alloc));
        let result = {
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if let Err(error) = admission.check() {
                Err(error)
            } else if !lease.is_current() {
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

    /// The caller keeps its preparation lease through this two-phase rollback.
    /// An empty registry entry prevents reusing the QID/free-list identity while
    /// its old aliases still occupy canonical or client addresses. Recovery joins
    /// this lease before removing the session, even if its epoch was invalidated.
    fn finish_uninstalled_compute(
        &self, retired: Option<(ComputeReservation, KBox<compute::Queue>)>,
    ) -> Result {
        let Some((reservation, queue)) = retired else { return Ok(()); };
        drop(queue);
        let mut state = self.shared.state.lock();
        let firmware = state.as_deref_mut().ok_or(ENODEV)?;
        firmware.queues.cancel_compute(reservation)
    }

    fn grow_compute(&self, required: usize, admission: &Admission<'_>) -> Result {
        use kernel::time::{delay::fsleep, Delta};
        admission.check()?;
        {
            let state = self.shared.state.lock();
            let firmware = state.as_ref().ok_or(ENODEV)?;
            if firmware.queues.compute(self.owner).is_some_and(|queue| queue.pool_fits(required)) {
                return Ok(());
            }
        }
        let lease = self.shared.preparations.enter_until(admission)?;
        let dev = self.shared.drm()?;
        let uat = self.shared.state.lock().as_ref().ok_or(ENODEV)?.uat.clone();
        let alloc = super::object::Allocator { dev: &dev, uat: &uat };
        let mut pages = Some(super::freelist::Pages::compute_sized(&alloc, required)?);
        let mut alias = Some(pages.as_ref().ok_or(EIO)?.map_client(self.context.vm())?);
        let mut deferred = DeferredBatch::publication()?;
        let mut growth = None;
        let result = (|| loop {
            let result = (|| {
                let mut state = self.shared.state.lock();
                admission.check()?;
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() { return Err(EAGAIN); }
                if self.shared.crashed.load(Ordering::Acquire) { return Err(EIO); }
                firmware.scan_compute(None, &mut deferred)?;
                let queue = firmware.queues.compute_mut(self.owner).ok_or(EAGAIN)?;
                if queue.pool_fits(required) { return Ok(None); }
                if growth.is_none() {
                    let notify = queue.begin_growth(&firmware.init, required)?;
                    // Capture physical identity before any following fallible
                    // operation. Close can remove the owner-keyed lookup.
                    growth = queue.growth_token();
                    if notify {
                        fence(Ordering::SeqCst);
                        firmware.primary.notify(crate::g17::MSG_CONTROL_NOTIFY)?;
                    }
                }
                queue.finish_growth(&firmware.init, &mut pages, &mut alias, required).map(Some)
            })();
            deferred.finish();
            match result {
                Ok(old) => { drop(old); return Ok(()); }
                Err(error) if error == EAGAIN && lease.is_current() => {
                        self.shared.queue_events();
                        fsleep(Delta::from_millis(1));
                    }
                Err(error) => return Err(error),
            }
        })();
        if result.is_err() {
            if let Some(token) = growth {
                let mut retired = None;
                let mut state = self.shared.state.lock();
                if let Some(firmware) = state.as_deref_mut() {
                    for index in 0..firmware.queues.compute.len() {
                        let Some(entry) = firmware.queues.compute[index].as_mut() else { continue; };
                        let Some(queue) = entry.queue.as_deref_mut() else { continue; };
                        if queue.abort_growth(token)? == Some(true) {
                            // Keep the empty entry and QID reserved until all
                            // graph aliases have been destroyed outside the lock.
                            retired = entry.queue.take().map(|queue| (entry.reservation, queue));
                            break;
                        }
                    }
                }
                drop(state);
                // Tentative replacement mappings also belong to this builder.
                // Discard them before a fresh QID can acquire a successor.
                drop(alias.take());
                drop(pages.take());
                self.finish_uninstalled_compute(retired)?;
                self.shared.queue_events();
            }
        }
        result
    }

    fn build_compute(&self, required: usize, admission: &Admission<'_>) -> Result {
        admission.check()?;
        self.admission_open()?;
        loop {
            match self.shared_compute(admission) {
                Err(error) if error == EAGAIN => continue,
                result => {
                    result?;
                    break;
                }
            }
        }
        loop {
            admission.check()?;
            let lease = self.shared.preparations.enter_until(admission)?;
            let dev = self.shared.drm()?;
            let (uat, reservation) = {
                let mut state = self.shared.state.lock();
                admission.check()?;
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if self
                    .shared
                    .crashed
                    .load(core::sync::atomic::Ordering::Acquire)
                {
                    return Err(EIO);
                }
                self.admission_open()?;
                let rebound = match firmware.queues.rebind_compute(self.owner, &self.context, required, admission) {
                    Err(error) if error == ENOSPC => {
                        drop(state);
                        drop(lease);
                        self.wait_compute_capacity(admission)?;
                        continue;
                    }
                    result => result?,
                };
                match rebound {
                    Rebind::Done => return Ok(()),
                    Rebind::Grow => { drop(state); drop(lease); return self.grow_compute(required, admission); }
                    Rebind::Rearm(qid) => (firmware.uat.clone(), Err(qid)),
                    Rebind::Fresh => {
                        let reservation = match firmware.queues.reserve_compute(self.owner) {
                            Ok(reservation) => reservation,
                            Err(error) if error == EAGAIN => {
                                // The event worker owns release observation and
                                // drops vacated aliases/backing off this mutex.
                                drop(state);
                                drop(lease);
                                self.wait_compute_capacity(admission)?;
                                continue;
                            }
                            Err(error) if error == ENOSPC => {
                                let reclaimed = firmware.queues.reclaim_unused_render_spare(None, true)?;
                                drop(state);
                                if reclaimed.is_none() {
                                    return Err(error);
                                }
                                drop(reclaimed);
                                continue;
                            }
                            Err(error) => return Err(error),
                        };
                        (firmware.uat.clone(), Ok(reservation))
                    }
                }
            };
            let alloc = super::object::Allocator {
                dev: &dev,
                uat: &uat,
            };
            let reservation = match reservation {
                Ok(reservation) => reservation,
                Err(qid) => {
                    // Fresh zeroed backing is built without the device mutex; the
                    // queue may have been taken or re-armed by another owner since.
                    let built = (|| -> Result<_> {
                        let pages = crate::g17::freelist::Pages::compute_sized(&alloc, required)?;
                        Ok((pages, compute::DescriptorRing::new(&alloc)?))
                    })();
                    let (pages, ring) = match built {
                        Err(error) if error == ENOSPC => {
                            drop(lease);
                            self.wait_compute_capacity(admission)?;
                            continue;
                        }
                        result => result?,
                    };
                    let mut state = self.shared.state.lock();
                    admission.check()?;
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if !lease.is_current() {
                        continue;
                    }
                    if let Err(error) = self.admission_open() { drop(state); return Err(error); }
                    // A competing ready owner may have taken/rearmed it while
                    // backing was built off-lock. Leave rejected backing local
                    // so it is dropped after the device guard, not in rearm().
                    let ready = firmware.queues.compute.iter().flatten()
                        .filter_map(|entry| entry.queue.as_deref())
                        .any(|queue| queue.qid() == qid && queue.can_rearm());
                    if !ready { drop(state); continue; }
                    let (slot, page_list) = match firmware.queues.rearm_compute(qid, pages, ring) {
                        Err(error) if error == EAGAIN => continue,
                        result => result?,
                    };
                    // The firmware consumed this slot's release before the backing
                    // was dropped; the row is rewritten before the queue is kicked.
                    firmware.init.initialize_compute_pool_blocks(slot, page_list, required)?;
                    firmware.queues.bind_rearmed(qid, self.owner, &self.context)?;
                    return Ok(());
                }
            };
            let built = (|| {
                let pool = compute::PoolBacking::new(&alloc, reservation.id, required)?;
                {
                    let mut state = self.shared.state.lock();
                    admission.check()?;
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if !lease.is_current() {
                        return Err(EAGAIN);
                    }
                    self.admission_open()?;
                    firmware.queues.reserved(reservation)?;
                    let (slot, page_list) = pool.pool_descriptor()?;
                    firmware.init.initialize_compute_pool_blocks(slot, page_list, required)?;
                }
                let backing = compute::Backing::new(&alloc, pool)?;
                // The global scheduler is created after pool/kick backing and
                // retained even if this queue's later descriptor graph fails.
                self.compute_scheduler(&alloc, &lease, admission)?;
                let queue = compute::Queue::new(
                    &alloc,
                    backing,
                    self.owner,
                    self.context.clone(),
                    self.shared.variant.pointers,
                    self.shared.operand_state,
                )?;
                Ok(KBox::new(queue, GFP_KERNEL)?)
            })();
            let (mut built, build_error) = match built {
                Ok(queue) => (Some(queue), None),
                Err(error) => (None, Some(error)),
            };
            let result = {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if let Err(error) = admission.check() {
                    Err(error)
                } else if !lease.is_current() {
                    Err(EAGAIN)
                } else if let Some(error) = build_error {
                    Err(error)
                } else {
                    admission.check().and_then(|()| firmware.queues.install_compute(reservation, &mut built))
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
            if result == Err(ENOSPC) {
                self.wait_compute_capacity(admission)?;
                continue;
            }
            if result != Err(EAGAIN) { return result; }
        }
    }
}

/// One device transaction can fail each retained kick once and then retire it.
/// The physical namespace bounds the batch independently of open logical queues.
/// Collection failure leaves the engine's Active owner intact for a stopped retry.
pub(super) struct DeferredBatch {
    values: KVVec<super::completion::Deferred>,
    /// Exited owners detached from idle retained compute queues and the backing
    /// of vacated pools: unmapping and freeing happen in `finish`, off-lock.
    bindings: KVVec<super::queue::compute::Binding>,
    vacated: KVVec<super::queue::compute::Vacated>,
    /// Backing of render pools vacated for evicted idle clients.
    render: KVVec<super::freelist::RenderBacking>,
    /// Tiler-heap extensions removed from idle heaps, with their aliases.
    tvb: KVVec<super::buffer::TvbRelease>,
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
        Self::with_capacity(2 * QIDS * Self::DEPTH, QIDS)
    }
    /// One publication can quarantine only its selected physical queue/pair.
    /// VM-wide settlement is a separate transaction before publishing its error.
    fn publication() -> Result<Self> {
        Self::with_capacity(2 * Self::DEPTH, 0)
    }
    fn with_capacity(capacity: usize, detached: usize) -> Result<Self> {
        let render_slots = if detached != 0 {
            crate::hw::t8140::queues::RENDER_SLOTS
        } else {
            0
        };
        Ok(Self {
            values: KVVec::with_capacity(capacity, GFP_KERNEL)?,
            bindings: KVVec::with_capacity(detached, GFP_KERNEL)?,
            vacated: KVVec::with_capacity(detached, GFP_KERNEL)?,
            render: KVVec::with_capacity(render_slots, GFP_KERNEL)?,
            tvb: KVVec::with_capacity(render_slots, GFP_KERNEL)?,
        })
    }
    /// Keeps the detached owner's unmapping and VM teardown out of the device
    /// mutex. Without reserved room the binding is dropped here instead.
    pub(super) fn defer_binding(&mut self, binding: super::queue::compute::Binding) {
        if let Err(binding) = self.bindings.push_within_capacity(binding) {
            drop(binding);
        }
    }
    /// Room for one more vacated render backing; the worker vacates no more.
    pub(super) fn render_backing_room(&self) -> bool {
        self.render.len() < self.render.capacity()
    }
    pub(super) fn defer_render_backing(&mut self, backing: super::freelist::RenderBacking) {
        if let Err(backing) = self.render.push_within_capacity(backing) {
            drop(backing);
        }
    }
    pub(super) fn tvb_room(&self) -> bool {
        self.tvb.len() < self.tvb.capacity()
    }
    pub(super) fn defer_tvb(&mut self, release: super::buffer::TvbRelease) {
        if let Err(release) = self.tvb.push_within_capacity(release) {
            drop(release);
        }
    }
    pub(super) fn vacated_room(&self) -> bool {
        self.vacated.len() < self.vacated.capacity()
    }
    pub(super) fn defer_vacated(&mut self, vacated: super::queue::compute::Vacated) {
        if let Err(vacated) = self.vacated.push_within_capacity(vacated) {
            drop(vacated);
        }
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
        self.values.push_within_capacity(value).map_err(|_| EIO)?;
        // A later publisher can borrow this pair before finish signals its old
        // fence. Publish the exact completion's witness only after collection
        // succeeds; failure fences and rejected events are not retirement.
        if let Some(
            Deferred::Retired(completion, _)
            | Deferred::Drained(completion, _)
            | Deferred::Release(completion),
        ) = self.values.last()
        {
            completion.note_retirement_queued();
        }
        Ok(())
    }
    /// Called only after releasing the device mutex.
    pub(super) fn finish(&mut self) {
        for value in self.values.drain_all() {
            value.finish();
        }
        for binding in self.bindings.drain_all() {
            drop(binding);
        }
        for vacated in self.vacated.drain_all() {
            drop(vacated);
        }
        for backing in self.render.drain_all() {
            drop(backing);
        }
        for release in self.tvb.drain_all() {
            drop(release);
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
    fn next_compute_publication(&mut self) -> Result<u64> {
        self.qids.next_publication()
    }
    fn epoch(&self) -> Result<(u64, u32)> {
        Ok((
            self.generation,
            super::recovery::Memory::recovery_state(self.init)?,
        ))
    }
    fn prepare_compute_shared(&mut self) -> Result {
        self.init.activate_compute()
    }
    fn qos_publish(
        &mut self,
        owner: qos::Owner,
        scheduler: u64,
        policy: super::fw::queue::Policy,
    ) -> Result<qos::Publication> {
        self.accounting.publish(
            &qos::View::new(self.init.qos()?)?,
            owner,
            scheduler,
            policy,
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
    fn publish_outer(&mut self, priority: u8, slot: &super::fw::channels::WorkSlot) -> Result {
        self.init.publish_work(priority, slot).map(|_| ())
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
        // No descriptor, accounting or inner producer changes precede this
        // capacity check. The same device lock covers queue.publish below.
        if !self
            .init
            .work_ready(packet.context.policy().priority(), DataMaster::Compute)?
        {
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
        // The first command after an owner change waits while the scheduler
        // the queue was bound to is being released: a live previous owner may
        // have closed after giving up the queue.
        if queue
            .pending_switch_from()
            .is_some_and(|previous| queues.teardown.releasing(previous))
        {
            return Err(EAGAIN);
        }
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
            if queue.in_flight()
                && queue
                    .context()
                    .is_some_and(|context| Arc::ptr_eq(context.status(), status))
            {
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
    fn ensure_compute(&self, scs_pages: u16, admission: &Admission<'_>) -> Result {
        admission.check()?;
        let required = super::fw::compute_pool::required_blocks(self.shared.active_cores, scs_pages)?;
        use kernel::time::{delay::fsleep, Delta};
        const SPARED_WAIT_MS: usize = 200;
        let mut deferred = DeferredBatch::publication()?;
        for _ in 0..SPARED_WAIT_MS {
            admission.check()?;
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
                match self.build_compute(required, admission) {
                    Err(error) if error == EAGAIN => continue,
                    result => return result,
                }
            }
            // Other owners continue submitting while this owner gives its
            // queue-local drain witness time to arrive.
            fsleep(Delta::from_millis(1));
        }
        // No new command has been accepted. Keep the spared queue and its
        // retained work intact until a later admission observes its witness.
        admission.check()?;
        Err(EBUSY)
    }

    /// Called after taking this packet's publication claim. All waits and work
    /// state allocation precede the device mutex; only the checked epoch commits.
    fn compute_packet(
        &self,
        packet: &Arc<super::job::Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        packet.check_dependencies()?;
        let super::command::Validated::Compute { scratch, .. } = &packet.command else { return Err(EINVAL); };
        let mut recovery_polls = 0;
        let mut state_delay = 20i64;
        let deadline = now_ns().saturating_add(WORK_RING_WAIT_NS);
        let check = || {
            if packet.cancel_requested() { return Err(ECANCELED); }
            self.admission_open()?;
            if packet.completion.status().get() != 0 { return Err(EIO); }
            Ok(())
        };
        let admission = Admission::new(deadline, &check);
        loop {
            admission.check()?;
            // All DRM dependencies were satisfied before this scheduler callback.
            // Recheck on every retry: pressure may yield an idle binding between
            // admission and publication without changing this logical queue.
            Backend::ensure_compute(self, scratch.pages(), &admission)?;
            admission.check()?;
            let lease = self.shared.preparations.enter_until(&admission)?;
            // Work states free as this context's earlier commands retire.
            // Wait for one under the packet's admission deadline, as render
            // publication does, instead of failing the packet.
            match packet.completion.work_state()?.acquire() {
                Err(error) if error == EBUSY => {
                    drop(lease);
                    admission.check()?;
                    self.shared.queue_events();
                    kernel::time::delay::fsleep(kernel::time::Delta::from_micros(state_delay));
                    state_delay = (state_delay * 2).min(1000);
                    continue;
                }
                result => {
                    result?;
                }
            }
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() {
                drop(state);
                continue;
            }
            admission.check()?;
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
            if firmware.queues.compute(self.owner).is_none() {
                drop(state);
                drop(lease);
                continue;
            }
            let dependencies = firmware.queues.render_prefix(self.owner, packet)?;
            admission.check()?;
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
            if matches!(result, Err(EAGAIN)) && !packet.is_published() {
                // Backpressure owns no firmware-visible attempt. Other queues
                // can submit and the event worker can retire while we sleep.
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
    /// Charge only real watchdog expirations, after the per-job allowance. A
    /// changing client or recovery generation cannot mint a fresh storm budget.
    pub(super) fn watchdog_timeout(self: &Arc<Self>) -> bool {
        if self.crashed.load(Ordering::Acquire) { return true; }
        let stop = {
            let mut state = self.state.lock();
            state.as_deref_mut().is_none_or(|firmware| firmware.recovery.timeout_limit(now_ns()))
        };
        if stop { self.lose_device_quietly(); }
        stop
    }

    /// Workload-triggered stop: halt producers first, then join only for a
    /// bounded interval. A stalled borrower leaves stopped firmware and all its
    /// mappings owned in state. Only a later joined stop may detach/free them.
    pub(super) fn stop_runtime(&self) {
        let Some(_stopping) = self.stop.try_lock() else { return; };
        {
            let mut state = self.state.lock();
            if let Some(firmware) = state.as_deref_mut() {
                if let Some(secondary) = firmware.secondary.as_mut() {
                    secondary.stop();
                }
                firmware.primary.stop();
            }
        }
        if self.preparations.wait_drained_until(now_ns().saturating_add(WORK_RING_WAIT_NS)).is_err() {
            dev_warn!(self.dev.as_ref(), "Stopped GPU retains outstanding preparation owners\n");
            return;
        }
        let firmware = self.state.lock().take();
        if let Some(mut firmware) = firmware { firmware.finish_stopped_queues(); }
    }

    /// Joins construction and complete host teardown, including RTKit drops.
    /// The stop-only mutex outlives detached firmware destruction, so unbind
    /// cannot return while another stop caller still owns mailbox clients.
    /// Completion callbacks remain outside the device mutex. Direct handlers
    /// do not synchronously request or join stop; this path does not join
    /// scheduler workers.
    pub(super) fn stop_queues(&self) {
        self.preparations.wait_drained();
        let _stopping = self.stop.lock();
        let firmware = {
            let mut state = self.state.lock();
            if let Some(firmware) = (*state).as_deref_mut() {
                if let Some(secondary) = firmware.secondary.as_mut() {
                    secondary.stop();
                }
                firmware.primary.stop();
            }
            state.take()
        };
        if let Some(mut firmware) = firmware {
            firmware.finish_stopped_queues();
            // RTKit destruction still dereferences the mailbox provider.
            drop(firmware);
        }
    }
}

/// Idle retained compute queues that keep their USC pool populated; older idle
/// pools give their backing back.
const COMPUTE_WARM_CAP: usize = 4;
/// A retained pool idle this long gives its backing back regardless of the
/// cap: a burst of short-lived clients still finds warm pools, but the pools
/// of clients that are gone do not stay resident. The macOS driver frees a
/// client's USC pools with the client and has an idle shrink; ten seconds
/// matches its idle parameter-buffer shrink.
const COMPUTE_WARM_EXPIRY_NS: u64 = 10_000_000_000;

impl super::Firmware {
    /// Event-worker service step: witness consumed pool releases, then release the
    /// oldest idle retained pools beyond the warm cap.
    /// Detached owner bindings and vacated pool backing go to `deferred`, which
    /// the worker finishes after releasing the device mutex.
    /// Returns the delay until a retained pool expires, for the worker's wake.
    pub(in crate::g17) fn service_compute_pools(
        &mut self,
        deferred: &mut DeferredBatch,
        now: u64,
    ) -> Result<Option<u64>> {
        for queue in self
            .queues
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            queue.recycle_killed()?;
            queue.recycle_quarantined_idle()?;
            if let Some(binding) = queue.detach_exited_owner() {
                deferred.defer_binding(binding);
            }
        }
        self.queues.observe_pool_releases(&self.init, deferred)?;
        if self.queues.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.pool_release_stalled(now)) {
            return Err(ETIMEDOUT);
        }
        if self.render_control_backpressured() {
            return Ok(None);
        }
        // While the shared lower window refuses free-list backing, no idle
        // retained compute pool stays warm: render and new compute need it more.
        let cap = if super::freelist::lower_pressure() {
            0
        } else {
            COMPUTE_WARM_CAP
        };
        let (published, next) = self.queues.vacate_idle_pools(
            &self.init,
            cap,
            now,
            COMPUTE_WARM_EXPIRY_NS,
        )?;
        if published != 0 {
            fence(Ordering::SeqCst);
            self.primary.notify(crate::g17::MSG_CONTROL_NOTIFY)?;
        }
        Ok(next)
    }
}


impl Registry {
    pub(super) fn write_capacity(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        self.qids.write_capacity(f)?;
        self.admission.write_capacity(f)?;
        self.buffers.write_capacity(f)?;
        writeln!(f, "qos_ids used={} total=126", self.qos_ids.used())?;
        writeln!(f, "metrics_ids used={} total={}", self.metrics.used(), super::fw::buffer::METRICS_SLOTS)?;
        self.render.write_capacity(f)?;
        writeln!(f, "compute_slots used={} total={} reusable={} building={}",
                 self.compute.iter().flatten().count(), self.compute.len(),
                 self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()).filter(|queue| queue.reusable()).count(),
                 self.compute.iter().flatten().filter(|entry| entry.queue.is_none()).count())?;
        writeln!(f, "compute_channels used={} total={}",
                 self.compute.iter().flatten().count(), super::kick::COMPUTE_CHANNELS)?;
        writeln!(f, "scheduler_teardown pending={}", self.teardown.len())?;
        for queue in self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()) {
            queue.write_capacity(f)?;
        }
        Ok(())
    }
}

impl Backend {
    pub(super) fn wait_capacity(&self, started: u64) -> Result {
        let check = || self.admission_open();
        let admission = Admission::new(started.saturating_add(WORK_RING_WAIT_NS), &check);
        self.wait_compute_capacity(&admission)
    }

    fn wait_compute_capacity(&self, admission: &Admission<'_>) -> Result {
        admission.check()?;
        let mut deferred = DeferredBatch::publication()?;
        let result: Result = (|| {
            // A short reclaim transaction obeys the same session/epoch gate as
            // publication. Never keep this lease while sleeping for capacity.
            let lease = self.shared.preparations.try_enter()?;
            let mut state = self.shared.state.lock();
            let firmware = state.as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() || self.shared.crashed.load(Ordering::Acquire) {
                return Err(EAGAIN);
            }
            if firmware.recovery.pending() || super::recovery::Memory::recovery_state(&firmware.init)? != 0 {
                return Ok(());
            }
            for queue in firmware.queues.compute.iter_mut().flatten().filter_map(|entry| entry.queue.as_deref_mut()) {
                admission.check()?;
                queue.share_idle()?;
            }
            // Consumption, not publication or an elapsed timeout, frees backing.
            firmware.queues.observe_pool_releases(&firmware.init, &mut deferred)?;
            firmware.queues.reclaim_idle_render_pools(&firmware.init, &mut deferred)?;
            if !firmware.render_control_backpressured() {
                let (published, _) = firmware.queues.vacate_idle_pools(&firmware.init, 0, now_ns(), 0)?;
                if published != 0 { firmware.primary.notify(super::MSG_CONTROL_NOTIFY)?; }
            }
            Ok(())
        })();
        deferred.finish();
        self.shared.queue_events();
        match result {
            Err(error) if error == EAGAIN => {}
            result => result?,
        }
        admission.check()?;
        kernel::time::delay::fsleep(kernel::time::Delta::from_millis(1));
        admission.check()
    }
}

#[kunit_tests(asahi_g17_shared_capacity)]
mod shared_capacity_tests {
    use super::*;
    use crate::g17::fw::render::Deflake;
    use crate::g17::fw::variant::V26A428;

    #[test]
    fn all_logical_render_owners_fit_beside_shared_compute() {
        let mut registry = KBox::init(
            Registry::new(1, Deflake::ONE_CLUSTER, [0; 2], &V26A428),
            GFP_KERNEL,
        )
        .unwrap();
        let mut leases = KVec::with_capacity(crate::mmu::MAX_EXECUTION_CONTEXTS as usize, GFP_KERNEL).unwrap();
        // Physical compute channels are device-owned and bounded independently
        // of how many logical clients have queued unsignalled work.
        for owner in 1..=super::super::kick::COMPUTE_CHANNELS as u64 {
            registry.reserve_compute(owner).unwrap();
        }
        assert!(matches!(registry.reserve_compute(1000), Err(EAGAIN)));
        for owner in 1..=u64::from(crate::mmu::MAX_EXECUTION_CONTEXTS) {
            let lease = registry.admission.acquire_render(owner, None, u64::MAX, || Ok(())).unwrap();
            registry.ensure_render_ids(lease.slot(), owner).unwrap();
            leases.push(lease, GFP_KERNEL).unwrap();
        }
        assert_eq!(leases.len(), crate::mmu::MAX_EXECUTION_CONTEXTS as usize);
        // Every accepted logical render owner now has its physical pair; no
        // published identity was cancelled or retyped to achieve the fit.
    }

    #[test]
    fn a_qid_floor_admits_only_the_render_pairs_it_leaves() {
        use crate::g17::fw::variant::V25G83;
        let limit = super::super::kick::queue_limit(V25G83.qid_floor);
        assert_eq!(limit, 42);
        let mut registry = KBox::init(
            Registry::new(2, Deflake::TWO_CLUSTERS, [0; 2], &V25G83),
            GFP_KERNEL,
        )
        .unwrap();
        for owner in 1..=super::super::kick::COMPUTE_CHANNELS as u64 {
            registry.reserve_compute(owner).unwrap();
        }
        let mut slots = KVec::with_capacity(limit as usize, GFP_KERNEL).unwrap();
        let mut leases = KVec::with_capacity(limit as usize, GFP_KERNEL).unwrap();
        for owner in 1..=u64::from(limit) {
            slots.push(registry.admission.reserve_queue().unwrap(), GFP_KERNEL).unwrap();
            let lease = registry.admission.acquire_render(owner, None, u64::MAX, || Ok(())).unwrap();
            registry.ensure_render_ids(lease.slot(), owner).unwrap();
            leases.push(lease, GFP_KERNEL).unwrap();
        }
        // The next logical queue is refused when it is created, not left waiting for a pair.
        assert!(matches!(registry.admission.reserve_queue(), Err(ENOSPC)));
    }

    #[test]
    fn four_clients_have_capacity_before_any_owner_closes() {
        let mut registry = KBox::init(
            Registry::new(1, Deflake::ONE_CLUSTER, [0; 2], &V26A428),
            GFP_KERNEL,
        )
        .unwrap();
        let mut leases = KVec::with_capacity(36, GFP_KERNEL).unwrap();
        for client in 0..4u64 {
            for queue in 0..9u64 {
                let owner = 1 + client * 9 + queue;
                let lease = registry.admission.acquire_render(owner, None, u64::MAX, || Ok(())).unwrap();
                registry.ensure_render_ids(lease.slot(), owner).unwrap();
                leases.push(lease, GFP_KERNEL).unwrap();
            }
        }
        for owner in 1..=super::super::kick::COMPUTE_CHANNELS as u64 {
            registry.reserve_compute(owner).unwrap();
        }
        assert_eq!(leases.len(), 36);
    }
}

impl Backend {
    /// Logical close is independent of whether an off-lock builder has already
    /// installed a physical graph. No constructor can resurrect a closed owner.
    fn admission_open(&self) -> Result {
        if self.closed.load(Ordering::Acquire) { return Err(ECANCELED); }
        if self.context.status().get() != 0 || self.shared.crashed.load(Ordering::Acquire) {
            return Err(EIO);
        }
        if !self.context.is_current() { return Err(EFAULT); }
        Ok(())
    }
}
