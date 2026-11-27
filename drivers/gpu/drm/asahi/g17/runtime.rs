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
pub(super) mod traversal;

pub(super) use super::now_ns;
use super::{
    buffer::{BufferIds, MetricsIds},
    context::{Context, QosIds},
    fw::queue::DataMaster,
    kick, qos,
    queue::{self, admission, compute},
};
use super::preparation::{Admission, AdmissionWait, ADMISSION_WAITS};
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};

const QIDS: usize = 128;
const FIRST_COMPUTE_QID: u8 = 4;
const WORK_RING_WAIT_NS: u64 = super::job::TIMEOUT_MS as u64 * 1_000_000;

/// Pending admission keeps exactly retired compute backing available for reuse.
/// The guard spans retries and disappears on publication, cancellation or error.
/// It borrows Shared, so the accounting cannot retain the device after teardown.
struct ComputeDemand<'a>(&'a Arc<super::Shared>);

impl<'a> ComputeDemand<'a> {
    fn new(shared: &'a Arc<super::Shared>) -> Self {
        // Each admitted logical queue has at most one scheduler publisher.
        // The execution-context bound is far below the counter's range.
        shared.compute_demand.fetch_add(1, Ordering::AcqRel);
        Self(shared)
    }
}

impl Drop for ComputeDemand<'_> {
    fn drop(&mut self) {
        if self.0.compute_demand.fetch_sub(1, Ordering::AcqRel) == 1 {
            // Reinstate normal idle retention even if the last publisher was
            // cancelled before a kick could produce another event.
            self.0.queue_events();
        }
    }
}

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
    queue: ComputeSlot,
}

/// A compute queue taken out of the registry under the device mutex. Its empty entry keeps the
/// reservation's QID until [`Backend::finish_uninstalled_compute`] drops the queue (and its
/// aliases) after that mutex is released and then cancels the reservation.
type Uninstalled = (ComputeReservation, KBox<compute::Queue>);

/// One place per compute channel of an owner. An owner holds at most `COMPUTE_CHANNELS_MAX`
/// channels, so what its channels give up under the device mutex is kept here without
/// allocating there, and is dropped or finished only after that mutex is released.
type PerChannel<T> = [Option<T>; super::pairs::COMPUTE_CHANNELS_MAX];

/// A physical compute queue behind its own lock. Publication into a bound queue takes only
/// this lock; device-mutex holders take it to observe or change the queue (device mutex ->
/// cell). A logical queue may keep the cell after the registry gave the queue up: the cell is
/// then empty, so the cached reference never extends any firmware-visible lifetime.
#[pin_data]
pub(super) struct ComputeCell {
    #[pin]
    queue: kernel::sync::Mutex<Option<KBox<compute::Queue>>>,
    /// Lock-free mirror of the installed queue's QID (`u8::MAX` when empty).
    qid: core::sync::atomic::AtomicU8,
    /// Lock-free mirror of `(configuring, active_len, last_published)` updated while the cell
    /// mutex is held, so `compute_direct()` can keep single-in-flight work on channel 0 without
    /// locking sibling cells and pick the least-loaded channel without extra mutex trips.
    active_len: core::sync::atomic::AtomicU32,
    configuring: core::sync::atomic::AtomicBool,
    last_published: core::sync::atomic::AtomicU64,
    /// Lock-free mirror of [`compute::Queue::sustained`], so `compute_direct()` keeps every kick
    /// of an owner whose primary channel runs short kicks on that channel without a lock.
    sustained: core::sync::atomic::AtomicBool,
}

impl ComputeCell {
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                queue <- kernel::new_mutex!(None, "g17::ComputeCell::queue"),
                qid: core::sync::atomic::AtomicU8::new(u8::MAX),
                active_len: core::sync::atomic::AtomicU32::new(0),
                configuring: core::sync::atomic::AtomicBool::new(false),
                last_published: core::sync::atomic::AtomicU64::new(0),
                sustained: core::sync::atomic::AtomicBool::new(false),
            }),
            GFP_KERNEL,
        )
    }

    pub(super) fn qid(&self) -> Option<u8> {
        let qid = self.qid.load(Ordering::Acquire);
        (qid != u8::MAX).then_some(qid)
    }

    fn sync_stats(&self, queue: Option<&compute::Queue>) {
        match queue {
            Some(queue) => {
                self.active_len
                    .store(queue.active_len() as u32, Ordering::Release);
                self.configuring
                    .store(queue.configuring(), Ordering::Release);
                self.last_published
                    .store(queue.last_published(), Ordering::Release);
                self.sustained.store(queue.sustained(), Ordering::Release);
            }
            None => {
                self.active_len.store(0, Ordering::Release);
                self.configuring.store(false, Ordering::Release);
                self.last_published.store(0, Ordering::Release);
                self.sustained.store(false, Ordering::Release);
            }
        }
    }

    /// Lock-free routing hint `(configuring, active_len, last_published)`.
    pub(super) fn load_hint(&self) -> (bool, u32, u64) {
        (
            self.configuring.load(Ordering::Acquire),
            self.active_len.load(Ordering::Acquire),
            self.last_published.load(Ordering::Acquire),
        )
    }

    /// Lock-free hint: this cell's queue runs sustained heavy work ([`compute::Queue::sustained`]).
    pub(super) fn sustained(&self) -> bool {
        self.sustained.load(Ordering::Acquire)
    }

    /// The installed queue, locked; None once the registry gave it up.
    pub(super) fn lock(&self) -> Option<QueueGuard<'_>> {
        QueueGuard::new(self, self.queue.lock())
    }
}

/// A locked installed queue.
pub(super) struct QueueGuard<'a> {
    cell: &'a ComputeCell,
    queue: core::ptr::NonNull<compute::Queue>,
    _guard: kernel::sync::MutexGuard<'a, Option<KBox<compute::Queue>>>,
}

impl<'a> QueueGuard<'a> {
    fn new(
        cell: &'a ComputeCell,
        mut guard: kernel::sync::MutexGuard<'a, Option<KBox<compute::Queue>>>,
    ) -> Option<Self> {
        let queue = core::ptr::NonNull::from((*guard).as_deref_mut()?);
        Some(Self { cell, queue, _guard: guard })
    }
}

impl Drop for QueueGuard<'_> {
    fn drop(&mut self) {
        // SAFETY: `_guard` drops after `Drop::drop`, so `self.queue` remains locked and valid.
        let queue = unsafe { self.queue.as_ref() };
        self.cell.sync_stats(Some(queue));
    }
}

impl core::ops::Deref for QueueGuard<'_> {
    type Target = compute::Queue;
    fn deref(&self) -> &compute::Queue {
        // SAFETY: The boxed queue cannot be taken or dropped while the guard holds its lock.
        unsafe { self.queue.as_ref() }
    }
}

impl core::ops::DerefMut for QueueGuard<'_> {
    fn deref_mut(&mut self) -> &mut compute::Queue {
        // SAFETY: As for `deref`; the lock makes this the only reference.
        unsafe { self.queue.as_mut() }
    }
}

/// The registry's handle on one compute cell. Dropping it drops the queue in the cell, so a
/// cached cell never keeps a removed queue (or its QID's aliases) alive.
struct ComputeSlot(Arc<ComputeCell>);

impl ComputeSlot {
    fn new() -> Result<Self> {
        Ok(Self(ComputeCell::new()?))
    }
    fn as_deref(&self) -> Option<QueueGuard<'_>> {
        self.0.lock()
    }
    fn as_deref_mut(&mut self) -> Option<QueueGuard<'_>> {
        self.0.lock()
    }
    fn is_some(&self) -> bool {
        self.0.queue.lock().is_some()
    }
    fn is_none(&self) -> bool {
        !self.is_some()
    }
    /// Gives the queue up to the caller, which drops it outside the cell lock.
    fn take(&mut self) -> Option<KBox<compute::Queue>> {
        let mut guard = self.0.queue.lock();
        let queue = guard.take();
        self.0.qid.store(u8::MAX, Ordering::Release);
        self.0.sync_stats(None);
        queue
    }
    fn put(&mut self, queue: Option<KBox<compute::Queue>>) {
        let qid = queue.as_ref().map_or(u8::MAX, |queue| queue.qid());
        let old = {
            let mut guard = self.0.queue.lock();
            self.0.qid.store(qid, Ordering::Release);
            self.0.sync_stats(queue.as_deref());
            core::mem::replace(&mut *guard, queue)
        };
        drop(old);
    }
}

impl Drop for ComputeSlot {
    fn drop(&mut self) {
        let queue = {
            let mut guard = self.0.queue.lock();
            let queue = guard.take();
            self.0.sync_stats(None);
            queue
        };
        drop(queue);
    }
}

pub(super) struct Registry {
    pub(super) qids: kick::QueueIds,
    pub(super) buffers: BufferIds,
    pub(super) metrics: Arc<MetricsIds>,
    pub(super) qos_ids: Arc<QosIds>,
    pub(super) admission: Arc<admission::Pool>,
    render: render::RenderState,
    identities: queue::Identities,
    compute: KVec<Option<ComputeEntry>>,
    pub(super) compute_building: bool,
    compute_scheduler_building: bool,
    replay_outer_wait: Option<u64>,
    /// A compute replay waits for its owner's renders to park or idle (compute_replay).
    replay_consumer_wait: Option<u64>,
    pub(super) observations: compute::retirement::Batch,
    /// Completion-ring slot where the next compute scan starts.
    completion_cursor: usize,
    /// Control word of each completion-ring slot as of the last scan that read it.
    completion_seen: KVVec<u64>,
    teardown: super::teardown::Pending,
}

impl Registry {
    /// Initialize inside the firmware allocation instead of staging the whole
    /// registry on the probe stack. No additional allocation is required.
    #[cfg(CONFIG_KUNIT = "y")]
    pub(super) fn new(
        clusters: u32,
        deflake: crate::g17::fw::render::Deflake,
        descriptor_flags: [u32; 2],
        variant: &'static crate::g17::fw::Variant,
    ) -> impl Init<Self, Error> {
        Self::with_waiters(clusters, deflake, descriptor_flags, variant, None, None)
    }

    /// `queues` wakes logical-queue creation when a logical queue slot is returned;
    /// `renders` wakes render publication when a render pair or lease returns.
    pub(super) fn with_waiters(
        clusters: u32,
        deflake: crate::g17::fw::render::Deflake,
        descriptor_flags: [u32; 2],
        variant: &'static crate::g17::fw::Variant,
        queues: Option<Arc<super::availability::Availability>>,
        renders: Option<Arc<super::availability::Availability>>,
    ) -> impl Init<Self, Error> {
        kernel::try_init!(Self {
            qids <- kick::QueueIds::new(variant.qid_floor).chain(|qids| {
                qids.reserve_initial_render()
            }),
            buffers <- BufferIds::new(),
            metrics: Arc::new(MetricsIds::for_firmware(variant.m5_commands)?, GFP_KERNEL)?,
            qos_ids: QosIds::new()?,
            admission: admission::Pool::with_waiters(kick::queue_limit(variant.qid_floor), queues, renders)?,
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
            replay_consumer_wait: None,
            observations: compute::retirement::Batch::new()?,
            completion_cursor: 0,
            completion_seen: {
                let slots = super::initdata::COMPUTE_COMPLETION_SLOTS;
                let mut seen = KVVec::with_capacity(slots, GFP_KERNEL)?;
                for _ in 0..slots {
                    seen.push_within_capacity(0).map_err(|_| ENOMEM)?;
                }
                seen
            },
            teardown: super::teardown::Pending::new()?,
        })
    }

    pub(super) fn compute(&self, owner: u64) -> Option<QueueGuard<'_>> {
        self.compute_primary(owner).or_else(|| {
            self.compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .find(|queue| queue.owner() == Some(owner))
        })
    }

    pub(super) fn compute_primary(&self, owner: u64) -> Option<QueueGuard<'_>> {
        self.compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.owner() == Some(owner) && queue.is_primary())
    }

    pub(super) fn compute_for_packet(
        &self,
        owner: u64,
        packet: &Arc<super::job::Packet>,
    ) -> Option<QueueGuard<'_>> {
        self.compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.owner() == Some(owner) && queue.owns_packet(packet))
    }

    pub(super) fn compute_for_packet_mut(
        &mut self,
        owner: u64,
        packet: &Arc<super::job::Packet>,
    ) -> Option<QueueGuard<'_>> {
        self.compute_for_packet(owner, packet)
    }

    /// Count and used channel-index bitmask of `owner`'s bound compute channels.
    fn compute_channels_of(&self, owner: u64) -> (usize, u8) {
        let mut count = 0usize;
        let mut mask = 0u8;
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.owner() == Some(owner))
        {
            count += 1;
            let idx = queue.channel_index();
            if idx < 8 {
                mask |= 1u8 << idx;
            }
        }
        (count, mask)
    }

    fn next_sibling_index(&self, owner: u64, limit: u8) -> Option<u8> {
        let (count, mask) = self.compute_channels_of(owner);
        if count == 0 || count >= usize::from(limit) {
            return None;
        }
        (1..limit).find(|idx| mask & (1u8 << idx) == 0)
    }

    /// Where `owner`'s next sibling channel would come from, in the order [`Self::rebind_compute`]
    /// gives a primary binding its channel: an idle reusable queue whose populated pool serves the
    /// owner's VM and fits, else a vacant reusable queue no other owner is re-arming, else a fresh
    /// QID, else an idle reusable queue whose pool still serves another VM (an exited owner's,
    /// say), given back first while no other pool release is in flight. Only
    /// [`compute::Queue::reusable`] queues whose scheduler release is not in flight qualify, so a
    /// channel with an owner, work, a pending pool release or a closing context is never taken,
    /// and neither is an open owner's idle channel, which only a primary binding may share. The
    /// longest released queue goes first. None keeps the owner on its bound channels.
    fn sibling_source(
        &self,
        owner: u64,
        context: &Arc<Context>,
        required: super::freelist::ComputeCapacity,
    ) -> Option<SiblingSource> {
        let teardown = &self.teardown;
        let vm = context.vm();
        let (mut warm, mut vacant, mut foreign) = (None, None, None);
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.reusable())
            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))
        {
            let best = if queue.pool_vacant() {
                if !queue.rearm_open(owner) {
                    continue;
                }
                &mut vacant
            } else if queue.pool_serves(vm) {
                if queue.needs_fresh_backing() || !queue.pool_fits(required) || !queue.has_ring() {
                    continue;
                }
                &mut warm
            } else {
                &mut foreign
            };
            let key = (queue.released_at(), queue.qid());
            if best.is_none_or(|old| key < old) {
                *best = Some(key);
            }
        }
        if let Some((_, qid)) = warm {
            return Some(SiblingSource::Warm(qid));
        }
        if let Some((_, qid)) = vacant {
            return Some(SiblingSource::Vacant(qid));
        }
        if self.qids.compute_fits(self.admission.render_demand(0)) {
            return Some(SiblingSource::Fresh);
        }
        // A pool release in flight ends in a vacant queue; give back one at a time.
        if self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.vacancy_pending())
        {
            return None;
        }
        foreign.map(|(_, qid)| SiblingSource::Foreign(qid))
    }

    /// Whether a sibling channel for `owner` is available now ([`Self::sibling_source`]).
    fn has_compute_sibling_headroom(
        &self,
        owner: u64,
        context: &Arc<Context>,
        required: super::freelist::ComputeCapacity,
    ) -> bool {
        self.sibling_source(owner, context, required).is_some()
    }

    /// Whether `owner` benefits from acquiring another compute channel now: its primary channel
    /// runs sustained heavy work ([`compute::Queue::sustained`]), all currently bound channels are
    /// busy with at least [`super::pairs::SIBLING_MIN_IN_FLIGHT`] kicks in flight between them, no
    /// replay is pending on any of them, and a sibling source exists
    /// ([`Self::has_compute_sibling_headroom`]). A primary channel streaming short kicks never
    /// brings up a sibling: its ring runs them faster than several QIDs would.
    fn wants_compute_sibling(
        &self,
        owner: u64,
        context: &Arc<Context>,
        limit: u8,
        required: super::freelist::ComputeCapacity,
    ) -> bool {
        if limit <= 1 || self.next_sibling_index(owner, limit).is_none() {
            return false;
        }
        let mut sustained_primary = false;
        let mut all_busy = true;
        let mut in_flight = 0usize;
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.owner() == Some(owner))
        {
            if queue.has_replays() {
                return false;
            }
            if queue.is_primary() {
                sustained_primary = queue.sustained();
            }
            if queue.active_len() == 0 && !queue.configuring() {
                all_busy = false;
            }
            in_flight += queue.active_len();
        }
        if !sustained_primary || !all_busy || !super::pairs::overlaps(in_flight) {
            return false;
        }
        self.has_compute_sibling_headroom(owner, context, required)
    }

    /// Binds a warm sibling source ([`SiblingSource::Warm`]) as an additional compute channel of
    /// `owner`. Returns `Ok(Some(true))` if bound, `Ok(Some(false))` if no warm queue is ready (a
    /// vacant queue to re-arm, a fresh QID or another VM's idle queue to give back is built or
    /// released off the device mutex, [`Backend::try_add_compute_sibling`]), or `Ok(None)` if no
    /// sibling slot is needed.
    fn bind_reusable_sibling(
        &mut self,
        owner: u64,
        context: &Arc<Context>,
        limit: u8,
        required: super::freelist::ComputeCapacity,
    ) -> Result<Option<bool>> {
        let Some(channel_index) = self.next_sibling_index(owner, limit) else {
            return Ok(None);
        };
        let Some(SiblingSource::Warm(qid)) = self.sibling_source(owner, context, required) else {
            return Ok(Some(false));
        };
        self.compute_queue_mut(qid)
            .ok_or(EIO)?
            .bind_sibling(owner, context.clone(), channel_index)?;
        Ok(Some(true))
    }

    /// The installed compute queue with QID `qid`.
    fn compute_queue_mut(&mut self, qid: u8) -> Option<QueueGuard<'_>> {
        self.compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == qid)
    }

    /// Gives back the idle reusable queue `qid`, whose pool serves another VM, for fresh backing:
    /// the next pool pass releases its pool ([`Self::vacate_idle_pools`]) and leaves it vacant,
    /// ready to re-arm for the VM that asked.
    fn give_back_foreign(&mut self, qid: u8) -> Result {
        self.compute_queue_mut(qid).ok_or(EIO)?.require_fresh_backing();
        Ok(())
    }

    /// Picks the bound compute channel of `owner` for `packet`. A command with cross-queue input
    /// sources always takes the primary channel, so a chain of semaphore hops between queues
    /// stays on one ring per queue. Dependent commands stay on the sole active channel when only
    /// one is active so the implicit parent covers them without cross-channel barriers. Other
    /// commands take the primary channel unless it runs sustained heavy work
    /// ([`compute::Queue::sustained`]) and enough kicks are in flight to overlap
    /// ([`super::pairs::overlaps`]); then they spread across the least-loaded channel (oldest
    /// `last_published` breaks ties).
    fn select_compute_channel(&self, owner: u64, packet: &super::job::Packet) -> Option<u8> {
        let primary = self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.owner() == Some(owner) && queue.is_primary())
            .map(|queue| (queue.qid(), queue.sustained()));
        if let Some((qid, _)) = primary.filter(|_| packet.completion.sources().is_some()) {
            return Some(qid);
        }
        if packet.order.wait_through[1].is_some() {
            let mut active_qid = None;
            let mut active_channels = 0usize;
            for queue in self
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .filter(|queue| queue.owner() == Some(owner))
            {
                if queue.active_len() != 0 || queue.configuring() {
                    active_qid = Some(queue.qid());
                    active_channels += 1;
                }
            }
            if active_channels == 1 {
                return active_qid;
            }
        }
        if let Some((qid, false)) = primary {
            return Some(qid);
        }
        let in_flight: usize = self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.owner() == Some(owner))
            .map(|queue| queue.active_len())
            .sum();
        if let Some((qid, _)) = primary.filter(|_| !super::pairs::overlaps(in_flight)) {
            return Some(qid);
        }
        self.compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.owner() == Some(owner))
            .min_by_key(|queue| {
                (
                    queue.configuring(),
                    queue.active_len(),
                    queue.last_published(),
                    queue.channel_index(),
                )
            })
            .map(|queue| queue.qid())
    }

    /// Appends covered active compute kicks from every compute channel of `owner` other than
    /// `exclude_qid` into `frontier`. On boards whose firmware waits require visible completion
    /// (`require_visible`), any covered kick on another channel without it returns `EAGAIN`.
    pub(super) fn append_compute_prefix(
        &self,
        owner: u64,
        exclude_qid: Option<u8>,
        packet: &Arc<super::job::Packet>,
        recovery_state: u32,
        require_visible: bool,
        frontier: &mut super::dependency::Frontier,
    ) -> Result {
        let through = packet.order.wait_through[1];
        if through.is_none() {
            return Ok(());
        }
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.owner() == Some(owner) && Some(queue.qid()) != exclude_qid)
        {
            if let Some(dep) = queue.dependency_checked(
                owner,
                packet.completion.status(),
                through,
                recovery_state,
                require_visible,
            )? {
                frontier.insert(0, dep).map_err(|_| EAGAIN)?;
            }
        }
        Ok(())
    }

    /// The cells of `owner`'s bound compute channels, indexed by `channel_index`, which its
    /// publisher may lock one at a time without the device mutex.
    pub(super) fn compute_cells(
        &self,
        owner: u64,
    ) -> [Option<Arc<ComputeCell>>; super::pairs::COMPUTE_CHANNELS_MAX] {
        let mut cells = [const { None }; super::pairs::COMPUTE_CHANNELS_MAX];
        for entry in self.compute.iter().flatten() {
            let Some(queue) = entry.queue.as_deref() else {
                continue;
            };
            if queue.owner() == Some(owner) {
                let index = usize::from(queue.channel_index());
                if index < cells.len() {
                    cells[index] = Some(entry.queue.0.clone());
                }
            }
        }
        cells
    }

    /// An installed idle queue keeps its original QID and backing. Mapping a
    /// replacement context must succeed before its old binding is displaced.
    /// A vacant queue (USC backing given back) is offered for re-arming only
    /// when no populated idle queue of the owner's VM exists. A populated pool
    /// serves one VM: a queue whose backing served another VM is not bound.
    /// An open owner's idle channel serving another VM is shared for fresh
    /// backing; while any queue waits for such a release, none is shared, and
    /// the requester waits for the vacant queue.
    pub(super) fn rebind_compute(&mut self, owner: u64, context: &Arc<Context>, required: super::freelist::ComputeCapacity, admission: &Admission<'_>) -> Result<Rebind> {
        admission.check()?;
        if let Some(queue) = self.compute(owner) {
            let matches = queue.matches(owner, queue.qid(), context)?;
            let fits = queue.pool_fits(required);
            drop(queue);
            let all_fit = fits
                && self
                    .compute
                    .iter()
                    .flatten()
                    .filter_map(|entry| entry.queue.as_deref())
                    .filter(|queue| queue.owner() == Some(owner))
                    .all(|queue| queue.pool_fits(required));
            return Ok(if matches {
                if all_fit { Rebind::Done } else { Rebind::Grow }
            } else {
                Rebind::Fresh
            });
        }
        // A binding persists for its logical queue's lifetime while physical
        // QIDs allow: take a channel of a closed owner, then a fresh QID, and
        // only then the least recently idle channel of one open owner.
        if let Some(rebind) = self.bind_reusable(owner, context, required, admission)? {
            return Ok(rebind);
        }
        if self.compute_qid_fits() {
            return Ok(Rebind::Fresh);
        }
        if self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.vacancy_pending())
        {
            return Err(ENOSPC);
        }
        let teardown = &self.teardown;
        let vm = context.vm();
        // A released channel whose backing served another VM goes first: vacate it, then
        // re-arm it fresh for this one.
        let foreign = self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.reusable() && !queue.pool_vacant() && !queue.pool_serves(vm))
            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))
            .map(|queue| (queue.released_at(), queue.qid()))
            .min_by_key(|(key, _)| *key)
            .map(|(_, qid)| qid);
        if let Some(qid) = foreign {
            self.give_back_foreign(qid)?;
            return Err(ENOSPC);
        }
        let idle = self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.shareable())
            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))
            // A channel serving the requester's VM first, then the best fit, then the longest idle.
            .map(|queue| ((!queue.pool_serves(vm), !queue.pool_fits(required), queue.last_published()), queue.qid()))
            .min_by_key(|(key, _)| *key)
            .map(|(_, qid)| qid);
        let Some(qid) = idle else { return Err(ENOSPC); };
        admission.check()?;
        {
            let mut queue = self
                .compute
                .iter_mut()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref_mut())
                .find(|queue| queue.qid() == qid)
                .ok_or(EIO)?;
            if !queue.share_idle()? {
                return Err(ENOSPC);
            }
            if !queue.pool_serves(vm) {
                // Its backing holds another VM's scratch: vacate it, then re-arm fresh.
                queue.require_fresh_backing();
                return Err(ENOSPC);
            }
        }
        Ok(self.bind_reusable(owner, context, required, admission)?.unwrap_or(Rebind::Fresh))
    }

    /// Whether a fresh compute QID fits beside current render demand, either
    /// directly or after cancelling one idle unpublished render spare.
    fn compute_qid_fits(&self) -> bool {
        let demand = self.admission.render_demand(0);
        self.qids.compute_fits(demand)
            || (self.has_cancelable_render_spare().unwrap_or(false)
                && self.qids.compute_fits_with_reclaim(demand, 2))
    }

    /// Whether `owner` can bind a channel now, without changing any binding. Owner 0 asks
    /// only whether some channel is free for a new owner.
    fn compute_readiness(&self, owner: u64, query: Option<(&Arc<Context>, super::freelist::ComputeCapacity)>) -> Readiness {
        if owner != 0
            && self
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .any(|queue| queue.owner() == Some(owner) && queue.spared_pending())
        {
            return Readiness::Progress;
        }
        if let (Some((context, required)), Some(queue)) = (query, self.compute(owner).filter(|_| owner != 0)) {
            return if queue.spared_pending() {
                Readiness::Progress
            } else if !matches!(queue.matches(owner, queue.qid(), context), Ok(true)) {
                // Publication reports the mismatch.
                Readiness::Ready
            } else if queue.pool_fits(required) || !queue.growing() {
                Readiness::Ready
            } else {
                Readiness::Progress
            };
        }
        let teardown = &self.teardown;
        let available = self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| {
                (queue.reusable() || queue.shareable() || queue.can_rearm())
                    && !queue.context().is_some_and(|bound| teardown.releasing(bound))
            });
        if available || self.compute_qid_fits() {
            Readiness::Ready
        } else {
            Readiness::Channel
        }
    }

    /// Binds `owner` to a reusable channel whose last owner released it, or
    /// names a vacant one to re-arm. None if no channel is reusable.
    fn bind_reusable(&mut self, owner: u64, context: &Arc<Context>, required: super::freelist::ComputeCapacity, admission: &Admission<'_>) -> Result<Option<Rebind>> {
        let mut vacant = None;
        let teardown = &self.teardown;
        for mut queue in self
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
                if queue.rearm_open(owner) {
                    vacant.get_or_insert(queue.qid());
                }
                continue;
            }
            if !queue.pool_fits(required) || !queue.pool_serves(context.vm()) { continue; }
            queue.bind_owner(owner, context.clone())?;
            return Ok(Some(Rebind::Done));
        }
        let Some(qid) = vacant else { return Ok(None) };
        // Reserved for this owner until it binds the queue or gives up: other owners wait for
        // a channel instead of each building a pool for the same queue.
        self.compute.iter_mut().flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == qid)
            .ok_or(EIO)?
            .reserve_rearm(owner);
        Ok(Some(Rebind::Rearm(qid)))
    }

    /// Installs fresh USC backing into the vacant queue `qid`; the caller writes
    /// the returned descriptor row before binding. EAGAIN if the queue was taken.
    pub(super) fn rearm_compute(
        &mut self,
        qid: u8,
        pages: crate::g17::freelist::Pages,
        ring: compute::DescriptorRing,
    ) -> Result<(super::freelist::PoolDescriptor, Option<compute::Vacated>)> {
        let mut queue = self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == qid)
            .ok_or(EAGAIN)?;
        queue.rearm(pages, ring)
    }

    /// Reserves the vacant queue `qid` for `owner`'s re-arm, as [`Self::bind_reusable`] does for a
    /// primary binding.
    fn reserve_rearm(&mut self, qid: u8, owner: u64) -> Result {
        self.compute_queue_mut(qid).ok_or(EIO)?.reserve_rearm(owner);
        Ok(())
    }

    /// Ends `owner`'s re-arm reservation of `qid` when it did not bind the queue.
    pub(super) fn release_rearm(&mut self, qid: u8, owner: u64) {
        if let Some(mut queue) = self.compute.iter_mut().flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == qid)
        {
            queue.release_rearm(owner);
        }
    }

    /// Binds the re-armed queue `qid` as `owner`'s compute channel `channel_index`: its primary
    /// (0) or a sibling.
    pub(super) fn bind_rearmed(
        &mut self,
        qid: u8,
        owner: u64,
        context: &Arc<Context>,
        channel_index: u8,
    ) -> Result {
        let teardown = &self.teardown;
        let mut queue = self
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
        if channel_index == 0 {
            queue.bind_owner(owner, context.clone())
        } else {
            queue.bind_sibling(owner, context.clone(), channel_index)
        }
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
            let mut queue = self
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
        super::channel::observe_control(consumer, producer);
        let mut freed = 0;
        for mut queue in self
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
        self.reserve_compute_channel(owner, 1)
    }

    pub(super) fn reserve_compute_channel(
        &mut self,
        owner: u64,
        limit: u8,
    ) -> Result<ComputeReservation> {
        if owner == 0 || limit == 0 {
            return Err(EINVAL);
        }
        let mut owned = 0usize;
        for entry in self.compute.iter().flatten() {
            match entry.queue.as_deref() {
                None if entry.reservation.owner == owner => return Err(EINVAL),
                Some(queue) if queue.owner() == Some(owner) => owned += 1,
                _ => {}
            }
        }
        if owned >= usize::from(limit) {
            return Err(EINVAL);
        }
        let slot = self
            .compute
            .iter()
            .position(Option::is_none)
            .ok_or_else(|| self.compute_admission_error(
                crate::g17::capacity::refused(crate::g17::capacity::Site::ComputeSlots)))?;
        let identity = self.identities.reserve(1)?.start;
        let (reclaimable, installed) = self.reclaimable_render_slots()?;
        let qids = &mut self.qids;
        let id = self
            .admission
            .reserve_compute_with_budget(reclaimable, installed, |demand| {
                let id = qids.reserve_for(
                    identity,
                    DataMaster::Compute,
                    Some(FIRST_COMPUTE_QID),
                    demand,
                )?;
                Ok((id, qids.render_pair_fits(), qids.render_pair_capacity()))
            })
            .map_err(|error| self.compute_admission_error(error))?;
        let reservation = ComputeReservation { slot, id, owner };
        self.compute[slot] = Some(ComputeEntry {
            reservation,
            queue: ComputeSlot::new()?,
        });
        Ok(reservation)
    }

    /// Reserves a fresh compute channel, storing any cancelled unpublished
    /// render spare in `reclaimed` so the caller drops its backing off-lock.
    fn reserve_compute_reclaiming(
        &mut self,
        owner: u64,
        reclaimed: &mut Option<render::UnusedRenderSpare>,
    ) -> Result<ComputeReservation> {
        let demand = self.admission.render_demand(0);
        if !self.qids.compute_fits(demand) && self.qids.compute_fits_with_reclaim(demand, 2) {
            *reclaimed = self.reclaim_unused_render_spare(None, true)?;
        }
        self.reserve_compute(owner)
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
        entry.queue.put(queue.take());
        Ok(())
    }

    pub(super) fn cancel_compute(&mut self, reservation: ComputeReservation) -> Result {
        self.reserved(reservation)?;
        self.qids.cancel_unpublished(reservation.id)?;
        self.compute[reservation.slot] = None;
        self.note_build_room();
        Ok(())
    }

    /// Releases idle sibling channels of `owner` before primary pool growth. Returns `Err(EAGAIN)`
    /// while any sibling still has active, replaying or configuring work. Each unpublished
    /// sibling queue goes to `retired`, which starts empty, for off-lock alias drop and QID
    /// cancellation; on failure it still holds every queue taken before it.
    fn drain_compute_siblings(
        &mut self,
        owner: u64,
        retired: &mut PerChannel<Uninstalled>,
    ) -> Result {
        let mut channel = 0;
        for slot in 0..self.compute.len() {
            let is_sibling = self.compute[slot]
                .as_ref()
                .and_then(|entry| entry.queue.as_deref())
                .is_some_and(|queue| queue.owner() == Some(owner) && !queue.is_primary());
            if !is_sibling {
                continue;
            }
            let busy = self.compute[slot]
                .as_ref()
                .and_then(|entry| entry.queue.as_deref())
                .is_some_and(|queue| {
                    queue.active_len() != 0 || queue.has_replays() || queue.configuring()
                });
            if busy {
                return Err(EAGAIN);
            }
            // Refuse before releasing a channel whose queue would have no place in `retired`.
            let place = retired.get_mut(channel).filter(|place| place.is_none()).ok_or(EIO)?;
            channel += 1;
            let entry = self.compute[slot].as_mut().ok_or(EIO)?;
            let unpublished = entry.queue.as_deref_mut().ok_or(EIO)?.release_owner()?;
            if unpublished {
                *place = entry.queue.take().map(|queue| (entry.reservation, queue));
            }
        }
        Ok(())
    }

    /// Rolls back `owner`'s unpublished bindings to `context` on every channel, siblings before
    /// the primary for deterministic teardown. Each displaced binding goes to `bindings` and each
    /// cancelled queue to `retired`, both starting empty, for the caller to drop (and finish)
    /// after releasing the device mutex; on failure they still hold everything taken before it.
    fn rollback_unpublished_compute(
        &mut self,
        owner: u64,
        context: &Arc<Context>,
        retired: &mut PerChannel<Uninstalled>,
        bindings: &mut PerChannel<compute::Binding>,
    ) -> Result {
        let mut channel = 0;
        for primary_pass in [false, true] {
            for slot in 0..self.compute.len() {
                let matches = self.compute[slot]
                    .as_ref()
                    .and_then(|entry| entry.queue.as_deref())
                    .is_some_and(|queue| {
                        queue.owner() == Some(owner) && queue.is_primary() == primary_pass
                    });
                if !matches {
                    continue;
                }
                // Refuse before rolling back a channel whose binding or queue would have no place.
                let binding = bindings.get_mut(channel).filter(|place| place.is_none()).ok_or(EIO)?;
                let place = retired.get_mut(channel).filter(|place| place.is_none()).ok_or(EIO)?;
                channel += 1;
                let entry = self.compute[slot].as_mut().ok_or(EIO)?;
                let (cancel, replaced) = entry
                    .queue
                    .as_deref_mut()
                    .ok_or(EIO)?
                    .rollback_unpublished(owner, context)?;
                *binding = replaced;
                if cancel {
                    *place = entry.queue.take().map(|queue| (entry.reservation, queue));
                }
            }
        }
        Ok(())
    }

    pub(super) fn release_compute(&mut self, owner: u64) -> Result {
        // Release sibling channels before the primary channel for deterministic teardown order.
        for primary_pass in [false, true] {
            for slot in 0..self.compute.len() {
                let matches = self.compute[slot]
                    .as_ref()
                    .and_then(|entry| entry.queue.as_deref())
                    .is_some_and(|queue| {
                        queue.owner() == Some(owner) && queue.is_primary() == primary_pass
                    });
                if !matches {
                    continue;
                }
                let entry = self.compute[slot].as_mut().ok_or(EIO)?;
                let unpublished = entry.queue.as_deref_mut().ok_or(EIO)?.release_owner()?;
                if unpublished {
                    self.qids.cancel_unpublished(entry.reservation.id)?;
                    self.compute[slot] = None;
                    self.note_build_room();
                }
            }
        }
        Ok(())
    }
}

/// What a compute packet still lacks before publication.
#[derive(Copy, Clone, PartialEq, Eq)]
pub(super) enum Readiness {
    Ready,
    /// A physical channel: every one is busy and no QID fits.
    Channel,
    /// Retirement-bound capacity or reopened publication.
    Progress,
    /// M5: no unclaimed room on the shared work ring.
    Outer,
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

/// Where an owner's next sibling compute channel comes from ([`Registry::sibling_source`]).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) enum SiblingSource {
    /// An idle reusable queue whose populated pool serves the owner's VM and fits: bind it.
    Warm(u8),
    /// A vacant reusable queue no other owner is re-arming: re-arm it with fresh backing.
    Vacant(u8),
    /// No reusable queue to bind or re-arm, but a fresh QID fits.
    Fresh,
    /// An idle reusable queue whose pool still serves another VM: give it back, so it is vacated
    /// and can be re-armed.
    Foreign(u8),
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
#[pin_data]
pub(super) struct Backend {
    shared: Arc<super::Shared>,
    pub(super) context: Arc<Context>,
    owner: u64,
    backlog: core::sync::atomic::AtomicU32,
    compute_backlog: core::sync::atomic::AtomicU32,
    closed: core::sync::atomic::AtomicBool,
    /// The cells of the compute channels this owner last published to under the device mutex,
    /// indexed by `channel_index`, with the shared compute objects mapped into its VM. Direct
    /// publication revalidates each binding under its own cell lock; a stale cache only sends
    /// the packet back to the device-mutex path.
    #[pin]
    direct: kernel::sync::Mutex<[Option<Arc<ComputeCell>>; super::pairs::COMPUTE_CHANNELS_MAX]>,
    /// Earliest monotonic timestamp (ns) at which `compute_direct()` may yield again to try
    /// adding another sibling channel after a headroom miss.
    sibling_backoff_until: core::sync::atomic::AtomicU64,
    /// The channel a prepared packet is about to publish into. Its claim keeps the idle
    /// channel from being shared away between prepare_job and run_job.
    #[pin]
    claimed: kernel::sync::Mutex<Option<Arc<ComputeCell>>>,
    /// M5: this owner's claims on the work ring M5 TA, fragment and compute share, one per
    /// engine, taken while preparing a publication and consumed by it
    /// ([`Backend::claim_outer`], [`Backend::publish_claimed`]).
    #[pin]
    outer_compute: kernel::sync::Mutex<Option<super::initdata::OuterClaim>>,
    #[pin]
    outer_render: kernel::sync::Mutex<Option<super::initdata::OuterClaim>>,
    /// Render pair admission, shared by every logical queue of the session.
    admission: Arc<admission::Pool>,
    /// The render pair this owner last published to without the device mutex
    /// (zero for none): a hint, revalidated by every lend.
    direct_slot: core::sync::atomic::AtomicU32,
    /// Render pairs this owner may hold, fixed at creation with its scheduler's
    /// credits and its render pool's sizing.
    pairs: u8,
    /// Physical compute channels this owner may hold (up to three on M5 Pro, one elsewhere).
    compute_channels: u8,
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
        let (lease, uat, pool, qos, execution, slot, share, render_id) = loop {
            let observed = shared.queues.observe();
            let lease = shared.preparations.enter(true)?;
            let mut reclaimed = None;
            let mut state = shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if shared.crashed.load(core::sync::atomic::Ordering::Acquire) {
                return Err(EIO);
            }
            // Check every capacity the queue needs before reserving its slot.
            // A refused creator must hold nothing: returning a reserved slot
            // notifies the queue domain observed above, so the creator's own
            // wait would end at once and admission would spin on the device
            // mutex for the whole wait, starving the queues it waits for.
            //
            // Installed compute channels and non-reclaimable sibling render
            // pairs narrow render's QIDs: every open logical queue keeps a
            // dedicated first render pair within reach, while idle unpublished
            // siblings give way before refusing a new queue.
            if !firmware.queues.admission.has_room()
                || !firmware
                    .queues
                    .qids
                    .render_fits(firmware.queues.admission.render_demand(1))
            {
                return Err(super::capacity::refused(super::capacity::Site::LogicalQueues));
            }
            let (mut reclaimable, mut installed) = firmware.queues.reclaimable_render_slots()?;
            let mut render_capacity = firmware.queues.qids.render_pair_capacity();
            if firmware
                .queues
                .admission
                .needs_warmed_sibling_reclaim(render_capacity, reclaimable)
            {
                reclaimed = firmware.queues.reclaim_unused_render_sibling()?;
                if reclaimed.is_none() {
                    return Err(super::capacity::refused(super::capacity::Site::LogicalQueues));
                }
                render_capacity = firmware.queues.qids.render_pair_capacity();
                (reclaimable, installed) = firmware.queues.reclaimable_render_slots()?;
                if firmware
                    .queues
                    .admission
                    .needs_warmed_sibling_reclaim(render_capacity, reclaimable)
                {
                    drop(state);
                    drop(reclaimed);
                    drop(lease);
                    continue;
                }
            }
            let admission = &firmware.queues.admission;
            // A context that takes its render pool identity now is refused here, holding
            // nothing, when render graphs kept until the firmware stops hold the rest.
            let render_id = if shared.variant.render_id_at_create {
                Some(super::freelist::RenderId::new(&firmware.render_ids)?)
            } else {
                None
            };
            // The new context may reuse a pooled scheduler page: clear released QoS rows first.
            firmware.queues.qos_ids.forget_released(|ids| firmware.port.qos_forget(ids));
            // G17P queues of one VM join its live execution context (super::exec_share).
            let joined = if firmware.uat.is_m5() { None } else { super::exec_share::join(vm) };
            let attempt = if let Some((execution, share)) = joined {
                Ok((
                    execution,
                    admission.try_reserve_queue_reclaiming(
                        render_capacity,
                        reclaimable,
                        installed,
                    )?,
                    Some(share),
                ))
            } else if firmware.uat.execution_contexts_used() < crate::mmu::MAX_EXECUTION_CONTEXTS {
                // Creators hold the device mutex, and try_reserve_queue_reclaiming
                // reclaims unbuilt/parked/reusable siblings and re-checks demand
                // atomically against render_capacity before taking a slot.
                let slot = admission.try_reserve_queue_reclaiming(
                    render_capacity,
                    reclaimable,
                    installed,
                )?;
                firmware.uat.new_execution_context(vm, super::channel::control_witnessed()).map(|execution| (execution, slot, None))
            } else {
                Err(ENOSPC)
            };
            match attempt {
                Ok((execution, slot, share)) => {
                    let result = (
                        lease,
                        firmware.uat.clone(),
                        firmware.pool.clone(),
                        firmware.queues.qos_ids.clone(),
                        execution,
                        slot,
                        share,
                        render_id,
                    );
                    drop(state);
                    drop(reclaimed);
                    break result;
                }
                Err(error) if error == ENOSPC => {
                    // Every open queue holds one context; this one holds no
                    // slot or context any more.
                    let open = firmware.queues.admission.queues();
                    let closing = firmware.uat.execution_contexts_used().saturating_sub(open);
                    if closing == 0 || now_ns().saturating_sub(started) >= RETAINED_CONTEXT_WAIT_NS {
                        return Err(super::capacity::refused(super::capacity::Site::ExecutionContexts));
                    }
                    drop(render_id);
                    drop(state);
                    drop(reclaimed);
                    drop(lease);
                    // Nothing is visible yet: a signalled creator restarts.
                    if kernel::current!().signal_pending() {
                        return Err(ERESTARTSYS);
                    }
                    shared.queue_events();
                    // A closing queue returns its context when its release
                    // is reclaimed; that reclaim wakes this creator.
                    let left = RETAINED_CONTEXT_WAIT_NS.saturating_sub(now_ns().saturating_sub(started));
                    shared.queues.block(observed, left.div_ceil(1_000_000) as u32, true)?;
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
        let share = match share {
            Some(share) => Some(share),
            None if !uat.is_m5() => Some(super::exec_share::offer(vm, &execution)?),
            None => None,
        };
        let context = Arc::new(
            Context::new(
                &alloc,
                &pool,
                &qos,
                execution,
                owner_pid,
                policy,
                shared.renders.clone(),
                shared.progress.clone(),
                share,
                render_id,
            )?,
            GFP_KERNEL,
        )?;
        let state = shared.state.lock();
        if !lease.is_current() || state.is_none() {
            return Err(EAGAIN);
        }
        drop(state);
        let admission = slot.pool().clone();
        Ok((
            Arc::pin_init(
                pin_init!(Self {
                    shared: shared.clone(),
                    context,
                    owner,
                    backlog: core::sync::atomic::AtomicU32::new(0),
                    compute_backlog: core::sync::atomic::AtomicU32::new(0),
                    closed: core::sync::atomic::AtomicBool::new(false),
                    direct <- kernel::new_mutex!(
                        [const { None }; super::pairs::COMPUTE_CHANNELS_MAX],
                        "g17::Backend::direct"
                    ),
                    sibling_backoff_until: core::sync::atomic::AtomicU64::new(0),
                    claimed <- kernel::new_mutex!(None, "g17::Backend::claimed"),
                    outer_compute <- kernel::new_mutex!(None, "g17::Backend::outer_compute"),
                    outer_render <- kernel::new_mutex!(None, "g17::Backend::outer_render"),
                    admission,
                    direct_slot: core::sync::atomic::AtomicU32::new(0),
                    pairs: super::pairs::per_owner(),
                    compute_channels: super::pairs::compute_channels_per_owner(),
                }),
                GFP_KERNEL,
            )?,
            slot,
        ))
    }

    /// The shared aperture has exactly one builder. Other queues sleep without
    /// holding the device mutex; recovery joins its lease before closing roots.
    fn shared_compute(&self, admission: &Admission<'_>) -> Result {
        // The shared aperture is G17P compute state. M5 compute queues need only
        // their own pools, and the objects it would install are not M5's.
        if self.context.is_m5() {
            return Ok(());
        }
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
            self.shared.note_changed();
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
        self.shared.note_changed();
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
        firmware.queues.cancel_compute(reservation)?;
        drop(state);
        // The slot and its never-published QID are free again.
        self.shared.channels.notify();
        Ok(())
    }

    /// Finishes every queue in `retired` as [`Self::finish_uninstalled_compute`] finishes one,
    /// going on past a failure so no other reservation stays behind; returns the first failure.
    fn finish_retired_compute(&self, retired: &mut PerChannel<Uninstalled>) -> Result {
        retired.iter_mut().fold(Ok(()), |result, item| {
            let finished = self.finish_uninstalled_compute(item.take());
            result.and(finished)
        })
    }

    fn grow_compute(&self, required: super::freelist::ComputeCapacity, admission: &Admission<'_>) -> Result {
        admission.check()?;
        {
            let state = self.shared.state.lock();
            let firmware = state.as_ref().ok_or(ENODEV)?;
            if firmware.queues.compute(self.owner).is_some()
                && firmware
                    .queues
                    .compute
                    .iter()
                    .flatten()
                    .filter_map(|entry| entry.queue.as_deref())
                    .filter(|queue| queue.owner() == Some(self.owner))
                    .all(|queue| queue.pool_fits(required))
            {
                return Ok(());
            }
        }
        let lease = self.shared.preparations.enter_until(admission)?;
        let dev = self.shared.drm()?;
        let uat = self.shared.state.lock().as_ref().ok_or(ENODEV)?.uat.clone();
        let alloc = super::object::Allocator { dev: &dev, uat: &uat };
        let mut pages = Some(super::freelist::Pages::for_compute(&alloc, required,
            self.context.vm().memory_client())?);
        let mut alias = Some(pages.as_mut().ok_or(EIO)?.map_client(self.context.vm())?);
        let mut deferred = DeferredBatch::publication()?;
        let mut growth = None;
        let result = (|| loop {
            // Growth waits for this queue's retirement and for firmware to consume the pool
            // release; both are progress the event worker reports.
            let observed = self.shared.progress.observe();
            let mut retired_siblings: PerChannel<Uninstalled> = Default::default();
            let result = (|| {
                let mut state = self.shared.state.lock();
                admission.check()?;
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() { return Err(EAGAIN); }
                if self.shared.crashed.load(Ordering::Acquire) { return Err(EIO); }
                firmware.scan_compute(None, &mut deferred)?;
                // Release sibling channels before growing the primary channel so undersized
                // sibling pools are detached and later re-acquired at the grown capacity.
                firmware.queues.drain_compute_siblings(self.owner, &mut retired_siblings)?;
                let mut queue = firmware.queues.compute_primary(self.owner).ok_or(EAGAIN)?;
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
                queue.finish_growth(&firmware.init, &mut pages, &mut alias).map(Some)
            })();
            self.finish_retired_compute(&mut retired_siblings)?;
            deferred.finish();
            match result {
                Ok(old) => { drop(old); return Ok(()); }
                Err(error) if error == EAGAIN && lease.is_current() => {
                    self.shared.queue_events();
                    self.wait_release(&self.shared.progress, observed, admission)?;
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
                        let Some(aborted) = entry.queue.as_deref_mut()
                            .map(|mut queue| queue.abort_growth(token)).transpose()? else { continue; };
                        if aborted == Some(true) {
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

    /// Re-arms the vacant queue `qid` reserved for this owner with fresh zeroed backing, built
    /// without the device mutex, and binds it as compute channel `channel_index` (0: the primary;
    /// a sibling only while that index is still the owner's next free one). Some(true): bound;
    /// Some(false): retry (the preparation epoch changed, the queue is no longer re-armable or
    /// the sibling index was taken); None: no memory for the backing now.
    fn rearm_vacant(
        &self,
        qid: u8,
        channel_index: u8,
        required: super::freelist::ComputeCapacity,
        alloc: &super::object::Allocator<'_>,
        objects: &Arc<super::object::Pool>,
        lease: &super::preparation::Lease,
        admission: &Admission<'_>,
    ) -> Result<Option<bool>> {
        let built = (|| -> Result<_> {
            let pages = crate::g17::freelist::Pages::for_compute(alloc, required,
                self.context.vm().memory_client())?;
            Ok((pages, compute::DescriptorRing::new(alloc, objects)?))
        })();
        let (pages, ring) = match built {
            Err(error) if error == ENOSPC => return Ok(None),
            result => result?,
        };
        let mut state = self.shared.state.lock();
        admission.check()?;
        let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
        if !lease.is_current() {
            return Ok(Some(false));
        }
        if let Err(error) = self.admission_open() { drop(state); return Err(error); }
        // Leave rejected backing local so it is dropped after the device guard, not in
        // rearm().
        let ready = firmware.queues.compute.iter().flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.qid() == qid && queue.can_rearm() && queue.rearm_open(self.owner))
            && (channel_index == 0
                || firmware.queues.next_sibling_index(self.owner, self.compute_channels)
                    == Some(channel_index));
        if !ready { drop(state); return Ok(Some(false)); }
        let (descriptor, vacated) = match firmware.queues.rearm_compute(qid, pages, ring) {
            Err(error) if error == EAGAIN => return Ok(Some(false)),
            result => result?,
        };
        // The firmware consumed this slot's release before the backing
        // was dropped; the row is rewritten before the queue is kicked.
        firmware.init.install_compute_pool(descriptor)?;
        firmware.queues.bind_rearmed(qid, self.owner, &self.context, channel_index)?;
        drop(state);
        drop(vacated);
        Ok(Some(true))
    }

    fn build_compute(&self, required: super::freelist::ComputeCapacity, admission: &Admission<'_>) -> Result {
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
            // Sampled before any capacity check of this attempt.
            let observed = self.shared.channels.observe();
            let lease = self.shared.preparations.enter_until(admission)?;
            let dev = self.shared.drm()?;
            let mut reclaimed = None;
            let (uat, objects, reservation) = {
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
                        self.wait_channels(observed, admission, true)?;
                        continue;
                    }
                    result => result?,
                };
                match rebound {
                    Rebind::Done => return Ok(()),
                    Rebind::Grow => { drop(state); drop(lease); return self.grow_compute(required, admission); }
                    Rebind::Rearm(qid) => (firmware.uat.clone(), firmware.pool.clone(), Err(qid)),
                    Rebind::Fresh => {
                        let reservation = match firmware
                            .queues
                            .reserve_compute_reclaiming(self.owner, &mut reclaimed)
                        {
                            Ok(reservation) => reservation,
                            Err(error) if error == EAGAIN => {
                                // All physical channels are occupied. Retain their
                                // USC backing for exact-retirement handoff; releasing
                                // it cannot create a QID and forces fresh allocation.
                                drop(state);
                                drop(reclaimed);
                                drop(lease);
                                self.wait_channels(observed, admission, false)?;
                                continue;
                            }
                            Err(error) if error == ENOSPC => {
                                let extra = if reclaimed.is_some() {
                                    None
                                } else {
                                    firmware.queues.reclaim_unused_render_spare(None, true)?
                                };
                                drop(state);
                                drop(reclaimed);
                                if extra.is_none() {
                                    return Err(error);
                                }
                                drop(extra);
                                continue;
                            }
                            Err(error) => return Err(error),
                        };
                        (firmware.uat.clone(), firmware.pool.clone(), Ok(reservation))
                    }
                }
            };
            drop(reclaimed);
            let alloc = super::object::Allocator {
                dev: &dev,
                uat: &uat,
            };
            let reservation = match reservation {
                Ok(reservation) => reservation,
                Err(qid) => {
                    // bind_reusable reserved `qid` for this owner: any outcome but binding it
                    // ends the reservation, so another owner may re-arm it.
                    let result =
                        self.rearm_vacant(qid, 0, required, &alloc, &objects, &lease, admission);
                    if !matches!(result, Ok(Some(true))) {
                        if let Some(firmware) = self.shared.state.lock().as_deref_mut() {
                            firmware.queues.release_rearm(qid, self.owner);
                        }
                    }
                    match result? {
                        Some(true) => return Ok(()),
                        Some(false) => continue,
                        None => {
                            drop(lease);
                            self.wait_channels(observed, admission, true)?;
                            continue;
                        }
                    }
                }
            };
            let built = (|| {
                let pool = compute::PoolBacking::new(&alloc, reservation.id, required,
                    self.context.vm().memory_client())?;
                {
                    let mut state = self.shared.state.lock();
                    admission.check()?;
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if !lease.is_current() {
                        return Err(EAGAIN);
                    }
                    self.admission_open()?;
                    firmware.queues.reserved(reservation)?;
                    firmware.init.install_compute_pool(pool.pool_descriptor()?)?;
                }
                let backing = compute::Backing::new(&alloc, pool)?;
                // The global scheduler is created after pool/kick backing and
                // retained even if this queue's later descriptor graph fails.
                // It is G17P compute state; M5 has none.
                if !self.context.is_m5() {
                    self.compute_scheduler(&alloc, &lease, admission)?;
                }
                let queue = compute::Queue::new(
                    &alloc,
                    &objects,
                    backing,
                    self.owner,
                    self.context.clone(),
                    self.shared.variant.pointers,
                    self.shared.operand_state,
                    self.shared.m5_board,
                    self.shared.variant.compute,
                    alloc.uat.is_m5(),
                    self.shared.variant.mcache,
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
                self.wait_channels(observed, admission, true)?;
                continue;
            }
            if result != Err(EAGAIN) { return result; }
        }
    }

    /// Best-effort acquisition of an additional compute channel with its own dedicated compute
    /// pool for a multi-channel M5 owner, from the source [`Registry::sibling_source`] names: a
    /// warm reusable queue is bound at once, a vacant one is re-armed with fresh backing and
    /// bound as [`Self::rearm_vacant`] re-arms a primary's, a fresh QID gets a new queue, and an
    /// idle queue still holding another VM's pool is given back for the next attempt. An owner
    /// waiting for its first channel goes first. Never blocks or fails the submission if no
    /// sibling is available.
    fn try_add_compute_sibling(
        &self,
        required: super::freelist::ComputeCapacity,
        admission: &Admission<'_>,
    ) -> Result<bool> {
        if self.compute_channels <= 1 || !self.context.is_m5() {
            return Ok(false);
        }
        admission.check()?;
        let lease = self.shared.preparations.enter_until(admission)?;
        let dev = self.shared.drm()?;
        let (uat, objects, source, channel_index) = {
            let mut state = self.shared.state.lock();
            admission.check()?;
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() || self.shared.crashed.load(Ordering::Acquire) {
                return Ok(false);
            }
            if self.admission_open().is_err()
                || self.shared.channels.has_waiters()
                || !firmware.queues.wants_compute_sibling(
                    self.owner,
                    &self.context,
                    self.compute_channels,
                    required,
                )
            {
                return Ok(false);
            }
            let Some(channel_index) = firmware
                .queues
                .next_sibling_index(self.owner, self.compute_channels)
            else {
                return Ok(false);
            };
            let source = match firmware.queues.sibling_source(self.owner, &self.context, required) {
                None => return Ok(false),
                Some(SiblingSource::Warm(_)) => {
                    return Ok(firmware.queues.bind_reusable_sibling(
                        self.owner,
                        &self.context,
                        self.compute_channels,
                        required,
                    )? == Some(true));
                }
                Some(SiblingSource::Foreign(qid)) => {
                    firmware.queues.give_back_foreign(qid)?;
                    drop(state);
                    // A full event pass releases its pool; a later kick re-arms it.
                    self.shared.queue_events();
                    return Ok(false);
                }
                Some(SiblingSource::Vacant(qid)) => {
                    firmware.queues.reserve_rearm(qid, self.owner)?;
                    Err(qid)
                }
                Some(SiblingSource::Fresh) => match firmware
                    .queues
                    .reserve_compute_channel(self.owner, self.compute_channels)
                {
                    Ok(reservation) => Ok(reservation),
                    Err(_) => return Ok(false),
                },
            };
            (
                firmware.uat.clone(),
                firmware.pool.clone(),
                source,
                channel_index,
            )
        };
        let alloc = super::object::Allocator {
            dev: &dev,
            uat: &uat,
        };
        let reservation = match source {
            Ok(reservation) => reservation,
            Err(qid) => {
                // The reservation ends with any outcome but binding the queue.
                let result = self.rearm_vacant(
                    qid,
                    channel_index,
                    required,
                    &alloc,
                    &objects,
                    &lease,
                    admission,
                );
                let bound = matches!(result, Ok(Some(true)));
                if !bound {
                    if let Some(firmware) = self.shared.state.lock().as_deref_mut() {
                        firmware.queues.release_rearm(qid, self.owner);
                    }
                }
                drop(lease);
                // As for a fresh sibling, a failed re-arm leaves the owner on its bound channels.
                admission.check()?;
                return Ok(bound);
            }
        };
        let built = (|| {
            let pool = compute::PoolBacking::new(
                &alloc,
                reservation.id,
                required,
                self.context.vm().memory_client(),
            )?;
            {
                let mut state = self.shared.state.lock();
                admission.check()?;
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() {
                    return Err(EAGAIN);
                }
                self.admission_open()?;
                firmware.queues.reserved(reservation)?;
                firmware.init.install_compute_pool(pool.pool_descriptor()?)?;
            }
            let backing = compute::Backing::new(&alloc, pool)?;
            let queue = compute::Queue::new_channel(
                &alloc,
                &objects,
                backing,
                self.owner,
                self.context.clone(),
                self.shared.variant.pointers,
                self.shared.operand_state,
                self.shared.m5_board,
                self.shared.variant.compute,
                true,
                self.shared.variant.mcache,
                channel_index,
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
            } else if !lease.is_current() || self.admission_open().is_err() {
                Err(EAGAIN)
            } else if let Some(error) = build_error {
                Err(error)
            } else {
                firmware.queues.install_compute(reservation, &mut built)
            }
        };
        drop(built);
        if result.is_err() {
            let mut state = self.shared.state.lock();
            if let Some(firmware) = (*state).as_deref_mut() {
                let _ = firmware.queues.cancel_compute(reservation);
            }
        }
        drop(lease);
        Ok(result.is_ok())
    }
}

/// One device transaction can fail each retained kick once and then retire it.
/// The physical namespace bounds the batch independently of open logical queues.
/// Collection failure leaves the engine's Active owner intact for a stopped retry.
pub(super) struct DeferredBatch {
    has_source_completions: bool,
    values: KVVec<super::completion::Deferred>,
    /// Exited owners detached from idle retained compute queues and the backing
    /// of vacated pools: unmapping and freeing happen in `finish`, off-lock.
    bindings: KVVec<super::queue::compute::Binding>,
    vacated: KVVec<super::queue::compute::Vacated>,
    /// Backing of render pools vacated for evicted idle clients, or given back
    /// by a capacity wait's reclaim.
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
    pub(super) fn worker() -> Result<Self> {
        Self::with_capacity(2 * QIDS * Self::DEPTH, QIDS)
    }
    /// One publication can quarantine only its selected physical queue/pair.
    /// VM-wide settlement is a separate transaction before publishing its error.
    fn publication() -> Result<Self> {
        Self::with_capacity(2 * Self::DEPTH, 0)
    }
    /// The reclaim transaction of a capacity wait: also room for the backing of
    /// every render pool it gives back, which `finish` frees off-lock.
    fn reclaim() -> Result<Self> {
        let mut batch = Self::publication()?;
        batch.render = KVVec::with_capacity(crate::hw::t8140::queues::RENDER_SLOTS, GFP_KERNEL)?;
        Ok(batch)
    }
    fn with_capacity(capacity: usize, detached: usize) -> Result<Self> {
        let render_slots = if detached != 0 {
            crate::hw::t8140::queues::RENDER_SLOTS
        } else {
            0
        };
        Ok(Self {
            has_source_completions: false,
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
    /// Room for one more vacated render backing; the caller vacates no more.
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
            if completion.poisons(*error) {
                completion.status().record(*error);
            }
        }
        match &value {
            Deferred::Retired(completion, Err(error)) | Deferred::Drained(completion, Err(error))
            | Deferred::FailedOwned(completion, error) | Deferred::Unpublished(completion, error) =>
                { completion.note_input_error(*error); },
            Deferred::Retired(completion, Ok(_))
            | Deferred::RetiredCompute(completion, _, _)
            | Deferred::Drained(completion, Ok(_)) => completion.note_source_retired(),
            _ => {},
        }
        self.values.push_within_capacity(value).map_err(|_| EIO)?;
        // A later publisher can borrow this pair before finish signals its old
        // fence. Publish the exact completion's witness only after collection
        // succeeds; failure fences and rejected events are not retirement.
        if let Some(
            Deferred::Retired(completion, _)
            | Deferred::RetiredCompute(completion, _, _)
            | Deferred::Drained(completion, _)
            | Deferred::Release(completion),
        ) = self.values.last()
        {
            completion.note_retirement_queued();
            self.has_source_completions |= completion.sources().is_some();
        }
        Ok(())
    }
    /// Completions collected so far.
    pub(super) fn pending(&self) -> usize {
        self.values.len()
    }
    /// Whether the preallocated batch still has room for at least `need` entries.
    pub(super) fn has_room(&self, need: usize) -> bool {
        self.values.capacity().saturating_sub(self.values.len()) >= need
    }
    /// Called only after releasing the device mutex.
    pub(super) fn finish(&mut self) {
        use super::completion::Deferred;
        if self.has_source_completions {
            for _ in 0..self.values.len() {
                let mut changed = false;
                for value in &self.values {
                    if let Deferred::Retired(completion, Ok(_))
                        | Deferred::RetiredCompute(completion, _, _)
                        | Deferred::Drained(completion, Ok(_)) = value
                    {
                        if let Err(error) = completion.check_source_inputs() {
                            changed |= completion.note_input_error(error);
                        }
                    }
                }
                if !changed { break; }
            }
        }
        self.has_source_completions = false;
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
    shared: &'a Arc<super::Shared>,
    init: &'a super::initdata::InitData,
    regs: &'a crate::regs::G17Resources,
    port: &'a super::port::Port,
    outer: Option<super::port::OuterClaim<'a>>,
    m5_outer: Option<kernel::sync::MutexGuard<'a, Option<(u32, u64, u64)>>>,
    /// M5: the shared compute ring's room check of this publication, by priority.
    outer_cursors: Option<(u8, [u32; 3])>,
    effort: &'a mut super::power::Effort,
    qids: &'a mut kick::QueueIds,
    generation: u64,
}
impl compute::Host for ComputeHost<'_> {
    fn next_compute_publication(&mut self) -> Result<u64> {
        Ok(self.port.next_publication())
    }
    fn epoch(&self) -> Result<(u64, u32)> {
        Ok((
            self.generation,
            super::recovery::Memory::recovery_state(self.init)?,
        ))
    }
    fn prepare_compute_shared(&mut self) -> Result {
        // The global compute state is G17P's; M5 has none to activate.
        if self.init.is_m5() {
            return Ok(());
        }
        self.init.activate_compute()
    }
    fn qos_publish(
        &mut self,
        owner: qos::Owner,
        scheduler: u64,
        policy: super::fw::queue::Policy,
    ) -> Result<qos::Publication> {
        self.port.qos_publish(owner, scheduler, policy)
    }
    fn qos_cancel(&mut self, publication: qos::Publication) -> Result {
        self.port.qos_cancel(publication)
    }
    fn publish_qid(&mut self, id: kick::Id) -> Result {
        self.qids.publish(id)
    }
    fn register_queue(&mut self, queue: &super::fw::kick::KsmQueue) -> Result {
        self.port.register_queues(core::slice::from_ref(queue))
    }
    fn publish_outer(&mut self, priority: u8, slot: &super::fw::channels::WorkSlot) -> Result {
        if self.init.is_m5() {
            // The room check's sample serves the first record only: it moves the producer.
            let cursors = self.outer_cursors.take()
                .filter(|&(checked, _)| checked == priority)
                .map(|(_, cursors)| cursors);
            return self.init.publish_work_at(priority, slot, cursors).map(|_| ());
        }
        self.port.publish_compute(self.outer.take().ok_or(EIO)?, slot).map(|_| ())
    }
    fn notify(&mut self, message: u64) -> Result {
        // Every M5 pipe-2 doorbell (0x83) is noted in the firmware's submission history first,
        // as the render doorbell notes its pair.
        if self.init.is_m5() && message >> super::MSG_TYPE_SHIFT == 0x83 && message & 3 == 2 {
            if let Some(guard) = self.m5_outer.as_mut() {
                if let Err(error) = self.port.note_m5_submission_history_locked(guard, 1) {
                    pr_warn!("asahi: M5 submission history skipped: {:?}\n", error);
                }
                // Drop `m5_outer` before ringing the mailbox doorbell so competing M5 submitters
                // do not wait behind mailbox I/O.
                self.m5_outer = None;
            } else if let Err(error) = self.init.note_m5_submission_history(1) {
                pr_warn!("asahi: M5 submission history skipped: {:?}\n", error);
            }
        }
        self.port.notify(message)
    }
    fn note_submission(&mut self) -> Result {
        let now = super::now_ns();
        self.shared.note_submitter();
        self.port.note_gpu_activity();
        self.shared.note_gpu_activity();
        self.effort.submit();
        self.effort.note_waiters(now, super::fence::waiters());
        self.effort.note_compute_waiters(now, super::fence::compute_waiters());
        self.effort.note_bound(self.shared.feed.busy_run());
        if self.effort.bound() {
            self.port.lift_busy_if_below_top(now);
        }
        super::Firmware::apply_shared_effort(
            self.shared,
            self.init,
            self.effort,
            self.port,
            super::power::FULL_EFFORT,
        )
    }
    fn submit_m5(&mut self, submission: &super::fw::m5::queue::HardwareSubmission) -> Result {
        // Compute only (publication and installation); render pairs submit through
        // Firmware::submit_m5_render_pair, which keeps its own order.
        self.regs.submit_m5_work_held(submission)
    }
}

/// Publication into a bound, installed queue through the port alone. The caller checked the
/// recovery state, full effort and the queue's binding; first installation never comes here.
struct DirectHost<'a> {
    port: &'a super::port::Port,
    outer: Option<super::port::OuterClaim<'a>>,
    m5_outer: Option<&'a mut super::initdata::OuterClaim>,
    m5_outer_guard: Option<kernel::sync::MutexGuard<'a, Option<(u32, u64, u64)>>>,
    m5: bool,
    generation: u64,
}
impl compute::Host for DirectHost<'_> {
    fn next_compute_publication(&mut self) -> Result<u64> {
        Ok(self.port.next_publication())
    }
    fn epoch(&self) -> Result<(u64, u32)> {
        Ok((self.generation, self.port.recovery_state()?))
    }
    fn prepare_compute_shared(&mut self) -> Result {
        Err(EAGAIN)
    }
    fn qos_publish(
        &mut self,
        owner: qos::Owner,
        scheduler: u64,
        policy: super::fw::queue::Policy,
    ) -> Result<qos::Publication> {
        self.port.qos_publish(owner, scheduler, policy)
    }
    fn qos_cancel(&mut self, publication: qos::Publication) -> Result {
        self.port.qos_cancel(publication)
    }
    fn publish_qid(&mut self, _id: kick::Id) -> Result {
        // An installed queue published its identity with its first command.
        Ok(())
    }
    fn register_queue(&mut self, _queue: &super::fw::kick::KsmQueue) -> Result {
        Err(EAGAIN)
    }
    fn publish_outer(&mut self, priority: u8, slot: &super::fw::channels::WorkSlot) -> Result {
        if self.m5 {
            let claim = self.m5_outer.as_deref_mut().ok_or(EIO)?;
            let guard = self
                .m5_outer_guard
                .get_or_insert_with(|| self.port.m5_outer_lock());
            return self
                .port
                .publish_m5_compute_locked(guard, priority, claim, slot)
                .map(|_| ());
        }
        self.port.publish_compute(self.outer.take().ok_or(EIO)?, slot).map(|_| ())
    }
    fn notify(&mut self, message: u64) -> Result {
        if self.m5 && message >> super::MSG_TYPE_SHIFT == 0x83 && message & 3 == 2 {
            let guard = self
                .m5_outer_guard
                .get_or_insert_with(|| self.port.m5_outer_lock());
            if let Err(error) = self.port.note_m5_submission_history_locked(guard, 1) {
                pr_warn!("asahi: M5 submission history skipped: {:?}\n", error);
            }
            // Drop `m5_outer` before ringing the mailbox doorbell so competing M5 submitters
            // do not wait behind mailbox I/O.
            self.m5_outer_guard = None;
        }
        self.port.notify(message)
    }
    fn note_submission(&mut self) -> Result {
        Ok(())
    }
    fn submit_m5(&mut self, submission: &super::fw::m5::queue::HardwareSubmission) -> Result {
        if !self.m5 {
            return Err(EINVAL);
        }
        let guard = self
            .m5_outer_guard
            .get_or_insert_with(|| self.port.m5_outer_lock());
        self.port.submit_m5_work_held_locked(guard, submission)
    }
}

impl super::Firmware {
    #[inline]
    fn publish_compute(&mut self, owner: u64, packet: Arc<super::job::Packet>,
        dependencies: &[super::fw::kick::KickDependency], deferred: &mut DeferredBatch) -> Result
    {
        self.publish_compute_on(owner, None, packet, dependencies, deferred)
    }

    pub(super) fn publish_compute_on(
        &mut self,
        owner: u64,
        target_qid: Option<u8>,
        packet: Arc<super::job::Packet>,
        dependencies: &[super::fw::kick::KickDependency],
        deferred: &mut DeferredBatch,
    ) -> Result {
        let qid = match target_qid {
            Some(qid) => qid,
            None => self.queues.select_compute_channel(owner, &packet).ok_or(EIO)?,
        };
        let mut combined = super::dependency::Frontier::new();
        for dependency in dependencies {
            combined.insert(0, *dependency)?;
        }
        if target_qid.is_none() && packet.order.wait_through[1].is_some() {
            let recovery_state = super::recovery::Memory::recovery_state(&self.init)?;
            let require_visible = self
                .primary
                .state
                .shared
                .m5_board
                .is_some_and(|board| board.completion_inputs());
            self.queues.append_compute_prefix(
                owner,
                Some(qid),
                &packet,
                recovery_state,
                require_visible,
                &mut combined,
            )?;
        }
        if packet.completion.sources().is_some() {
            self.publish_compute_inputs(owner, qid, packet, combined.as_slice(), deferred)
        } else {
            self.publish_compute_raw(owner, qid, packet, combined.as_slice(), &[], deferred)
        }
    }

    #[inline(never)]
    fn publish_compute_inputs(&mut self, owner: u64, target_qid: u8,
        packet: Arc<super::job::Packet>, dependencies: &[super::fw::kick::KickDependency],
        deferred: &mut DeferredBatch) -> Result
    {
        if self.recovery.pending() || super::recovery::Memory::recovery_state(&self.init)? != 0 {
            return Err(EAGAIN);
        }
        let sources = packet.completion.sources().ok_or(EINVAL)?.clone();
        sources.check()?;
        let mut merged = super::dependency::Frontier::new();
        for dependency in dependencies { merged.insert(0, *dependency)?; }
        let mut holds = KVec::with_capacity(sources.sources.len(), GFP_KERNEL)?;
        let mut native: KVec<super::sync_input::NativeWait> =
            KVec::with_capacity(sources.sources.len(), GFP_KERNEL)?;
        // M5 shared-ring queues have no completion-wait records: the consumer's kick entry names
        // the source's kick, and the firmware orders it. So is a G17P source of the consumer's
        // own execution context: one context needs no write-back between the two kicks.
        let m5 = self.init.is_m5();
        let mut waited: u128 = 0;
        for source in &sources.sources {
            if source.submission.source_status()? { continue; }
            let found = self.queues.compute.iter_mut().flatten()
                .filter_map(|entry| entry.queue.as_deref_mut())
                .find_map(|mut queue| match queue.source_dependency(source, packet.completion.status(),
                    self.recovery.generation(), &packet.context) {
                    Ok(None) => None,
                    result => Some(result),
                }).ok_or(EAGAIN)??;
            let Some((sequence, dependency, stamp_va, event_slot, hold, same_execution)) = found else {
                return Err(EAGAIN);
            };
            if m5 && dependency.qid() == target_qid {
                // Same physical QID in this epoch: the queue's implicit parent already strictly
                // orders earlier kicks on this QID; drop the self-reader hold and skip adding a
                // duplicate explicit barrier.
                drop(hold);
                continue;
            }
            waited |= 1u128 << dependency.qid();
            if m5 || same_execution {
                merged.insert(sequence, dependency)?;
                holds.push(hold, GFP_KERNEL)?;
                continue;
            }
            // The completion-wait record orders the consumer behind its
            // source; naming the source in the kick entry as well only adds
            // a second, slower wait and a predecessor record.
            let wait = super::sync_input::NativeWait { sequence, dependency, stamp_va, event_slot };
            if let Some(old) = native.iter_mut().find(|old|
                old.dependency.qid() == dependency.qid()) {
                if sequence > old.sequence { *old = wait; }
            } else {
                native.push(wait, GFP_KERNEL)?;
            }
            holds.push(hold, GFP_KERNEL)?;
        }
        let result = self.publish_compute_raw(owner, target_qid, packet.clone(), merged.as_slice(),
            native.as_slice(), deferred);
        if packet.is_published() {
            for queue in self.queues.compute.iter().flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .filter(|queue| waited & (1u128 << queue.qid()) != 0)
            {
                for source in &sources.sources { queue.note_input_consumer(source); }
            }
            sources.set_holds(holds);
            super::sync_input::note_published();
            super::sync_input::note_native(native.len());
        }
        result
    }

    fn publish_compute_raw(
        &mut self,
        owner: u64,
        target_qid: u8,
        packet: Arc<super::job::Packet>,
        dependencies: &[super::fw::kick::KickDependency],
        native: &[super::sync_input::NativeWait],
        deferred: &mut DeferredBatch,
    ) -> Result {
        if self.recovery.pending() {
            return Err(EAGAIN);
        }
        let queues = &mut self.queues;
        let mut queue = queues
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .find(|queue| queue.qid() == target_qid && queue.owner() == Some(owner))
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
        // No descriptor, accounting or inner producer changes precede this
        // reservation; dropping it unused returns the work-ring slot. M5 compute
        // serializes the shared outer ring and host-port updates under `m5_outer`
        // (after the compute cell lock, matching direct M5 publishers).
        let m5_outer = self.init.is_m5().then(|| self.port.m5_outer_lock());
        let mut outer_cursors = None;
        let outer = if self.init.is_m5() {
            let priority = packet.context.policy().priority();
            let Some(cursors) = self.init.m5_compute_room(priority)? else {
                return Err(EAGAIN);
            };
            outer_cursors = Some((priority, cursors));
            None
        } else {
            Some(self.port.reserve_compute(packet.context.policy().priority())?)
        };
        let mut host = ComputeHost {
            shared: &self.primary.state.shared,
            init: &self.init,
            regs: &self.bridge.regs,
            port: &self.port,
            outer,
            m5_outer,
            outer_cursors,
            effort: &mut self.effort,
            qids: &mut queues.qids,
            generation: self.recovery.generation(),
        };
        if native.is_empty() {
            queue.publish(packet, dependencies, &mut host, &mut |event| deferred.push(event))
        } else {
            queue.publish_with_native_waits(packet, dependencies, native,
                &mut host, &mut |event| deferred.push(event))
        }
    }
}

impl Registry {
    /// The firmware restarted after a real sleep powered its domain off, which lost every M5
    /// compute queue's host-port configuration. Sends it again for each installed queue
    /// ([`compute::Queue::wake_configuration`]), with the device mutex held and publication still
    /// closed, so before any doorbell: no advance and no doorbell, and the next kick is an
    /// ordinary one. Returns how many were sent and how many failed.
    pub(in crate::g17) fn restore_compute_port_configurations(
        &self,
        regs: &crate::regs::G17Resources,
    ) -> (u32, u32) {
        let (mut sent, mut failed) = (0, 0);
        for entry in self.compute.iter().flatten() {
            let Some(queue) = entry.queue.as_deref() else {
                continue;
            };
            match queue.wake_configuration() {
                Ok(None) => {}
                Ok(Some(submission)) if regs.submit_m5_work(&submission).is_ok() => sent += 1,
                _ => failed += 1,
            }
        }
        (sent, failed)
    }

    /// Everything ensure_compute establishes for `owner`: the shared compute
    /// objects mapped into its VM, and its queue bound to `context` with a
    /// pool that fits and no spared witness to release.
    fn compute_bound(&self, owner: u64, context: &Arc<Context>, required: super::freelist::ComputeCapacity) -> Result<bool> {
        let Some(primary) = self.compute_primary(owner) else {
            return Ok(false);
        };
        if primary.spared_pending()
            || !primary.pool_fits(required)
            || !primary.matches(owner, primary.qid(), context)?
        {
            return Ok(false);
        }
        drop(primary);
        for queue in self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| queue.owner() == Some(owner) && !queue.is_primary())
        {
            if queue.spared_pending()
                || !queue.pool_fits(required)
                || !queue.matches(owner, queue.qid(), context)?
            {
                return Ok(false);
            }
        }
        Ok(true)
    }

    fn release_compute_witness(
        &mut self,
        init: &super::initdata::InitData,
        port: &super::port::Port,
        owner: u64,
        deferred: &mut DeferredBatch,
    ) -> Result<bool> {
        let mut pending = false;
        let recovery_state = super::recovery::Memory::recovery_state(init)?;
        for mut queue in self
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
            .filter(|queue| queue.owner() == Some(owner))
        {
            if !queue.spared_pending() {
                continue;
            }
            queue.release_witnessed(
                recovery_state,
                |owner| port.qos_complete(&[owner]),
                &mut |event| deferred.push(event),
            )?;
            pending |= queue.spared_pending();
        }
        Ok(pending)
    }

    fn fail_compute_vm(
        &mut self,
        status: &Arc<super::status::VmStatus>,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        for mut queue in self
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

/// Post-scan compute state collected while `scan_compute_summary` already holds each cell lock,
/// allowing the event worker to avoid redundant full-registry cell scans.
#[derive(Clone, Copy, Default)]
pub(super) struct ComputeScan {
    pub(super) in_flight: bool,
    pub(super) replay: bool,
    pub(super) polling: bool,
}

impl super::Firmware {
    fn scan_compute(&mut self, masks: Option<[u64; 2]>, deferred: &mut DeferredBatch) -> Result {
        self.scan_compute_summary(masks, deferred).map(|_| ())
    }

    /// Latch one shared snapshot for all selected queues before settling any VM.
    /// A later queue's exact success in this batch wins over a peer's failure.
    fn scan_compute_summary(
        &mut self,
        masks: Option<[u64; 2]>,
        deferred: &mut DeferredBatch,
    ) -> Result<ComputeScan> {
        let state = super::recovery::Memory::recovery_state(&self.init)?;
        let generation = self.recovery.generation();
        let registry = &mut self.queues;
        registry.observations.clear();
        let mut requested = false;
        let mut retire_mask = 0u128;
        let mut terminal_mask = 0u128;
        let mut witness_mask = 0u128;
        let mut in_flight_mask = 0u128;
        let mut replay_mask = 0u128;
        let mut polling_mask = 0u128;
        // Every queue's pending identities are staged, not only the ones this batch
        // retires: the scan passes each new record once, and a queue retired by a later
        // batch finds its observation already latched.
        for (index, entry) in registry.compute.iter().enumerate() {
            let Some(queue) = entry.as_ref().and_then(|entry| entry.queue.as_deref()) else {
                continue;
            };
            let bit = 1u128.checked_shl(index as u32).unwrap_or(0);
            let slot_requested = queue.request_observations(&mut registry.observations)?;
            requested |= slot_requested;
            if slot_requested || queue.needs_scan_retire(masks) {
                retire_mask |= bit;
            }
            if queue.has_terminal() {
                terminal_mask |= bit;
            }
            if queue.awaiting_witness() {
                witness_mask |= bit;
            }
            if queue.in_flight() {
                in_flight_mask |= bit;
            }
            if queue.replay_commands() != 0 {
                replay_mask |= bit;
            }
            if queue.retirement_polling() || queue.pool_release_pending() {
                polling_mask |= bit;
            }
        }
        // Completion records are appended in order: fold only the records written since
        // the previous scan. A request whose stamp already covers it has its record in the
        // ring; if the scan ended before finding one, read the whole ring once.
        if requested {
            let observations = &mut registry.observations;
            let seen = &mut registry.completion_seen;
            registry.completion_cursor = self.init.fold_compute_completions(
                registry.completion_cursor,
                seen,
                |record| observations.fold(record),
            )?;
            if observations.covered_pending() {
                registry.completion_cursor = self
                    .init
                    .resync_compute_completions(seen, |record| observations.fold_any(record))?;
            }
        }
        let port = &self.port;
        let retired = deferred.pending();
        let mut freed = false;
        let mut pending = retire_mask;
        while pending != 0 {
            let index = pending.trailing_zeros() as usize;
            let bit = 1u128 << index;
            pending &= !bit;
            let Some(mut queue) = registry.compute[index]
                .as_mut()
                .and_then(|entry| entry.queue.as_deref_mut())
            else {
                continue;
            };
            let busy = !queue.channel_free();
            if requested {
                queue.latch_observations(&mut registry.observations)?;
            }
            queue.retire(
                masks,
                generation,
                state,
                |owner| port.qos_complete(&[owner]),
                &mut |event| deferred.push(event),
            )?;
            freed |= busy && queue.channel_free();
            if queue.has_terminal() {
                terminal_mask |= bit;
            }
            if queue.awaiting_witness() {
                witness_mask |= bit;
            }
            if queue.in_flight() {
                in_flight_mask |= bit;
            } else {
                in_flight_mask &= !bit;
            }
            if queue.replay_commands() != 0 {
                replay_mask |= bit;
            } else {
                replay_mask &= !bit;
            }
            if queue.retirement_polling() || queue.pool_release_pending() {
                polling_mask |= bit;
            } else {
                polling_mask &= !bit;
            }
        }
        // All compute observations in this batch are latched before a VM
        // failure can settle its render peers. Exact successes win first.
        let had_terminal = terminal_mask != 0;
        if had_terminal {
            for index in 0..self.queues.compute.len() {
                let terminal = self.queues.compute[index]
                    .as_mut()
                    .and_then(|entry| entry.queue.as_deref_mut())
                    .and_then(|mut queue| queue.take_terminal());
                if let Some((status, error)) = terminal {
                    status.record(error);
                    self.settle_render_vm(&status, error, deferred)?;
                    self.queues.fail_compute_vm(&status, error, deferred)?;
                }
            }
        }
        if witness_mask != 0 || had_terminal {
            let port = &self.port;
            let registry = &mut self.queues;
            for (index, entry) in registry.compute.iter_mut().enumerate() {
                let bit = 1u128.checked_shl(index as u32).unwrap_or(0);
                if !had_terminal && (witness_mask & bit) == 0 {
                    continue;
                }
                let Some(mut queue) = entry.as_mut().and_then(|entry| entry.queue.as_deref_mut())
                else {
                    continue;
                };
                let busy = !queue.channel_free();
                queue.release_witnessed(
                    state,
                    |owner| port.qos_complete(&[owner]),
                    &mut |event| deferred.push(event),
                )?;
                freed |= busy && queue.channel_free();
                if queue.in_flight() {
                    in_flight_mask |= bit;
                } else {
                    in_flight_mask &= !bit;
                }
                if queue.replay_commands() != 0 {
                    replay_mask |= bit;
                } else {
                    replay_mask &= !bit;
                }
                if queue.retirement_polling() || queue.pool_release_pending() {
                    polling_mask |= bit;
                } else {
                    polling_mask &= !bit;
                }
            }
        }
        // Wake only the admission waits this batch can end. The fences gate
        // scheduler entities alone; their callbacks never take the device mutex.
        let shared = &self.primary.state.shared;
        if freed {
            shared.channels.notify();
        }
        if deferred.pending() != retired {
            shared.progress.notify();
        }
        Ok(ComputeScan {
            in_flight: in_flight_mask != 0,
            replay: replay_mask != 0,
            polling: polling_mask != 0,
        })
    }
}

impl Backend {
    /// The compute pool capacity `command` needs, in its family's terms: the M5
    /// plan's geometry, or the G17P block count of its SCS request.
    pub(super) fn compute_capacity(&self, command: &super::command::Validated) -> Result<super::freelist::ComputeCapacity> {
        use super::freelist::ComputeCapacity;
        let super::command::Validated::Compute { scratch, plan, .. } = command else { return Err(EINVAL); };
        if self.context.is_m5() {
            return Ok(ComputeCapacity::M5(plan.map(|plan| plan.geometry)));
        }
        super::fw::compute_pool::required_blocks(self.shared.compute_blocks, self.shared.active_cores, scratch.pages())
            .map(ComputeCapacity::Blocks)
    }

    fn ensure_compute(&self, required: super::freelist::ComputeCapacity, admission: &Admission<'_>) -> Result {
        admission.check()?;
        let mut deferred = DeferredBatch::publication()?;
        loop {
            admission.check()?;
            let observed = self.shared.progress.observe();
            let pending = (|| {
                let mut state = self.shared.state.lock();
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                firmware.replay_compute(&mut deferred)?;
                firmware
                    .queues
                    .release_compute_witness(&firmware.init, &firmware.port, self.owner, &mut deferred)
            })();
            deferred.finish();
            let pending = pending?;
            if !pending {
                match self.build_compute(required, admission) {
                    Err(error) if error == EAGAIN => continue,
                    result => return result,
                }
            }
            // Other owners continue submitting while this owner waits for its
            // queue-local drain witness. No new command has been accepted; if the
            // admission deadline passes, the spared queue and its retained work stay
            // intact until a later admission observes its witness.
            admission.extend_for_recovery();
            self.shared.queue_events();
            self.wait_release(&self.shared.progress, observed, admission)?;
        }
    }

    /// Resolves same-VM cross-QID M5 compute sources lock-free from each producer's
    /// `PublishedSource` snapshot under a live preparation lease. Sources already published on
    /// `target_qid` in `generation` are ordered by this queue's implicit parent, so they skip the
    /// duplicate explicit barrier, `Hold`, and `firmware_consumer` mark while still checking
    /// `source_status()` (error propagation) and epoch validity. Any unpublished, errored,
    /// cancelled, or epoch-mismatched source returns `None` so the device-mutex path handles it.
    fn resolve_direct_sources(
        &self,
        port: &super::port::Port,
        generation: u64,
        target_qid: u8,
        sources: &super::sync_input::Sources,
    ) -> Option<(super::dependency::Frontier, KVec<super::sync_input::Hold>, u64)> {
        sources.check().ok()?;
        let mut frontier = super::dependency::Frontier::new();
        let mut holds = KVec::new();
        let mut cross_qid_mask = 0u64;
        for (idx, source) in sources.sources.iter().enumerate() {
            if source.submission.source_status().ok()? {
                continue;
            }
            let published = source.submission.published_source(generation)?;
            if published.dependency.qid() == target_qid {
                if port.recovery_state().ok()? != 0 || port.generation() != generation {
                    return None;
                }
                if source.submission.source_status().ok()? {
                    continue;
                }
                if !source.submission.has_published_epoch(generation) {
                    return None;
                }
                continue;
            }
            let hold = super::sync_input::Hold::new(&published.readers).ok()?;
            // Re-validate after taking the binding loan so the producer cannot have retired,
            // faulted, cancelled, or crossed an epoch before `readers` became non-zero.
            if port.recovery_state().ok()? != 0 || port.generation() != generation {
                return None;
            }
            if source.submission.source_status().ok()? {
                continue;
            }
            if !source.submission.has_published_epoch(generation) {
                return None;
            }
            frontier
                .insert(published.published, published.dependency)
                .ok()?;
            holds.push(hold, GFP_KERNEL).ok()?;
            cross_qid_mask |= 1u64 << idx;
        }
        Some((frontier, holds, cross_qid_mask))
    }

    /// Whether `packet` may run on, or bring up, a sibling compute channel of this owner: only an
    /// independent command (no in-queue barrier, no cross-queue input sources) of an owner whose
    /// primary channel runs sustained heavy work (`primary_sustained`, asked last, from
    /// [`compute::Queue::sustained`]). Every other command streams down the primary ring: short
    /// kicks run faster there than spread over several QIDs, and a semaphore hop between queues
    /// stays on one ring per queue.
    fn fans_out(
        &self,
        packet: &super::job::Packet,
        primary_sustained: impl FnOnce() -> bool,
    ) -> bool {
        self.compute_channels > 1
            && packet.order.wait_through[1].is_none()
            && packet.completion.sources().is_none()
            && primary_sustained()
    }

    /// Steady-state publication into this owner's bound channel(s) without the device mutex: only
    /// one channel's own lock at a time and the port's small locks are taken, and nothing
    /// device-wide is scanned. On M5, same-VM cross-QID compute sources whose producers have
    /// published their kick metadata also resolve directly, always onto the primary channel.
    /// None leaves the packet untouched for the device-mutex path, which owns binding, first
    /// installation, cross-channel barriers, non-M5 native-wait sources, render prefixes, replay
    /// and recovery.
    fn compute_direct(
        &self,
        packet: &Arc<super::job::Packet>,
        required: super::freelist::ComputeCapacity,
        deferred: &mut DeferredBatch,
    ) -> Option<Result> {
        let m5 = self.context.is_m5();
        let has_sources = packet.completion.sources().is_some();
        let bound_count = self.context.compute_binding_count() as usize;
        if (has_sources && !m5)
            || self.context.render_seen()
            || bound_count == 0
            || bound_count > super::pairs::COMPUTE_CHANNELS_MAX
            || (bound_count > 1 && packet.order.wait_through[1].is_some())
            || packet.cancel_requested()
            || packet.completion.status().get() != 0
            || self.admission_open().is_err()
        {
            return None;
        }
        let cells = self.direct.lock().clone();
        let cached_count = cells.iter().filter(|cell| cell.is_some()).count();
        if cached_count != bound_count || cells[0].is_none() {
            return None;
        }
        let target_qid = cells[0].as_ref()?.qid()?;
        let lease = self.shared.preparations.try_enter().ok()?;
        let port = self.shared.port(&lease)?;
        if self.shared.crashed.load(Ordering::Acquire) || port.recovery_state().ok()? != 0 {
            return None;
        }
        let generation = port.generation();
        let (direct_frontier, direct_holds, cross_qid_mask) =
            if let Some(sources) = packet.completion.sources() {
                self.resolve_direct_sources(port, generation, target_qid, sources)?
            } else {
                (super::dependency::Frontier::new(), KVec::new(), 0)
            };
        // Only an independent kick without input sources of an owner whose primary channel runs
        // sustained heavy work may leave the primary ring ([`Self::fans_out`]); the primary
        // cell's lock-free hint answers that without a lock.
        let primary = cells[0].as_ref()?;
        let fans_out = self.fans_out(packet, || primary.sustained());
        let can_add_sibling = fans_out
            && cached_count < usize::from(self.compute_channels)
            && now_ns() >= self.sibling_backoff_until.load(Ordering::Relaxed);
        // Lock-free channel selection using each cell's atomic `(configuring, active_len,
        // last_published)` mirror:
        // - A kick that does not fan out, or finds primary channel 0 idle (`active == 0 &&
        //   !configuring`), routes straight to channel 0 without locking any sibling cell mutexes
        //   at all, keeping short kicks, input sources and single-in-flight work streaming down
        //   the primary ring with zero extra mutex overhead.
        // - Otherwise, scan bound cells' atomic hints lock-free. While fewer than
        //   `SIBLING_MIN_IN_FLIGHT` kicks are in flight the kick queues behind them on channel 0
        //   ([`super::pairs::overlaps`]); from there it takes the least-loaded channel (oldest
        //   `last_published` breaks ties), falling back to add a sibling only when every bound
        //   channel already has >= 1 kick in flight (`all_busy`).
        let target_idx = if cached_count == 1 || !fans_out {
            0
        } else {
            let (primary_configuring, primary_active, _) = primary.load_hint();
            if primary_active == 0 && !primary_configuring {
                0
            } else {
                let mut best: Option<(usize, (bool, u32, u64, usize))> = None;
                let mut all_busy = true;
                let mut in_flight = 0usize;
                for (idx, cell) in cells.iter().enumerate() {
                    let Some(cell) = cell else {
                        continue;
                    };
                    let (configuring, active, last_published) = cell.load_hint();
                    if active == 0 && !configuring {
                        all_busy = false;
                    }
                    in_flight += active as usize;
                    let key = (configuring, active, last_published, idx);
                    if best.is_none_or(|(_, best_key)| key < best_key) {
                        best = Some((idx, key));
                    }
                }
                if !super::pairs::overlaps(in_flight) {
                    0
                } else if can_add_sibling && all_busy {
                    return None;
                } else {
                    best?.0
                }
            }
        };
        let target_cell = cells[target_idx].as_ref()?;
        // Full effort is applied by the device-mutex path when it is not already.
        if !port.note_submission() {
            return None;
        }
        self.shared.note_submitter();
        self.shared.note_gpu_activity();
        match packet.completion.work_state().ok()?.acquire(crate::g17::job::Engine::Compute) {
            Ok(_) => {}
            Err(error) if error == EBUSY => return None,
            Err(error) => return Some(Err(error)),
        }
        let priority = packet.context.policy().priority();
        // Reuse the M5 outer claim already reserved during prepare_compute() when present and
        // matching priority, avoiding a redundant Port::m5_outer lock trip and unused-claim drop
        // notification; otherwise reserve before locking the compute cell.
        let (mut m5_outer, from_held) = if m5 {
            let claimed = {
                let mut held = self.outer_compute.lock();
                if held.as_ref().is_some_and(|claim| {
                    claim.remaining() != 0 && claim.priority() == usize::from(priority)
                }) {
                    held.take()
                } else {
                    None
                }
            };
            match claimed {
                Some(claim) => (Some(claim), true),
                None => (Some(port.reserve_m5_compute(priority).ok()?), false),
            }
        } else {
            (None, false)
        };
        let result = (|| {
            let mut queue = target_cell.lock()?;
            if !queue.installed()
                || queue.spared_pending()
                || !queue.pool_fits(required)
                || queue.pending_switch_from().is_some()
                || (has_sources && queue.qid() != target_qid)
                || (m5 && queue.outer_slots() != 1)
                || (cached_count == 1
                    && can_add_sibling
                    && super::pairs::overlaps(queue.active_len()))
                || !matches!(queue.matches(self.owner, queue.qid(), &self.context), Ok(true))
                || self.context.compute_binding_count() as usize != cached_count
                || !lease.is_current()
            {
                return None;
            }
            let outer = if m5 {
                None
            } else {
                Some(port.reserve_compute(priority).ok()?)
            };
            let mut host = DirectHost {
                port,
                outer,
                m5_outer: m5_outer.as_mut(),
                m5_outer_guard: None,
                m5,
                generation,
            };
            Some(queue.publish(
                packet.clone(),
                direct_frontier.as_slice(),
                &mut host,
                &mut |event| deferred.push(event),
            ))
        })();
        if packet.is_published() {
            if let Some(sources) = packet.completion.sources() {
                if cross_qid_mask != 0 {
                    for (idx, source) in sources.sources.iter().enumerate() {
                        if cross_qid_mask & (1u64 << idx) != 0 {
                            source.submission.note_firmware_consumer();
                        }
                    }
                    sources.set_holds(direct_holds);
                } else {
                    drop(direct_holds);
                }
                super::sync_input::note_published();
                super::sync_input::note_native(0);
            }
        } else {
            drop(direct_holds);
        }
        let outcome = match result {
            None => None,
            Some(Err(error)) if (error == EAGAIN || error == EBUSY) && !packet.is_published() => {
                None
            }
            Some(result) => Some(result),
        };
        if outcome.is_none() && from_held {
            if let Some(claim) = m5_outer.take().filter(|claim| claim.remaining() != 0) {
                let mut held = self.outer_compute.lock();
                if !held.as_ref().is_some_and(|existing| existing.remaining() != 0) {
                    // Replace None or an already-spent claim (whose drop notifies nobody).
                    *held = Some(claim);
                } else {
                    drop(held);
                    drop(claim);
                }
            }
        }
        drop(m5_outer);
        outcome
    }

    /// Scheduler admission of a compute packet. Shared capacity the packet lacks (a channel, a
    /// work state, retirement-bound room, reopened publication) becomes a fence that the release
    /// providing it signals, so publication finds it ready instead of sleeping for it. Errors
    /// and anything prepare cannot judge are left to publication.
    pub(super) fn prepare_compute(&self, packet: &Arc<super::job::Packet>) -> Option<kernel::dma_fence::Fence> {
        if packet.early_result().is_some()
            || packet.cancel_requested()
            || packet.completion.status().get() != 0
            || self.admission_open().is_err()
        {
            return None;
        }
        let required = self.compute_capacity(&packet.command).ok()?;
        let outer = self.outer_domain();
        loop {
            let channels = self.shared.channels.observe();
            let progress = self.shared.progress.observe();
            let claimed = outer.as_ref().map(|domain| domain.observe());
            let (domain, observed) = match (self.compute_readiness(packet, required), &outer, claimed) {
                (Readiness::Ready, ..) => return None,
                (Readiness::Channel, ..) => (&self.shared.channels, channels),
                (Readiness::Progress, ..) => (&self.shared.progress, progress),
                (Readiness::Outer, Some(domain), Some(observed)) => (domain, observed),
                (Readiness::Outer, ..) => return None,
            };
            match domain.wait(observed) {
                Ok(Some(fence)) => {
                    // Registered first: the event worker polls outer consumption only for waiters.
                    self.shared.queue_events();
                    return Some(fence);
                }
                Ok(None) => continue,
                Err(_) => return None,
            }
        }
    }

    /// M5: the domain outer-ring claims wait on. None off M5 or without firmware.
    fn outer_domain(&self) -> Option<Arc<super::availability::Availability>> {
        if !self.context.is_m5() {
            return None;
        }
        let lease = self.shared.preparations.try_enter().ok()?;
        self.shared.port(&lease)?.outer_domain()
    }

    /// M5: holds this owner's compute outer-ring claim through `Port` without taking the device
    /// mutex, for a channel already bound and installed.
    fn claim_compute_outer_direct(&self, count: u32) -> Result<bool> {
        let mut held = self.outer_compute.lock();
        if held.as_ref().is_some_and(|claim| claim.remaining() != 0) {
            return Ok(true);
        }
        let Ok(lease) = self.shared.preparations.try_enter() else {
            return Ok(true);
        };
        let Some(port) = self.shared.port(&lease) else {
            return Ok(true);
        };
        match port.reserve_m5_outer(self.context.policy().priority(), count) {
            Ok(claim) => {
                *held = Some(claim);
                Ok(true)
            }
            Err(error) if error == EBUSY => Ok(false),
            Err(error) => Err(error),
        }
    }

    /// M5: holds this owner's claim on the shared work ring for the next compute or render
    /// publication, reserving one under the device mutex unless one with credits left is
    /// held. False: no unclaimed room; wait on [`Self::outer_domain`] from a sample taken
    /// before this call. Always true off M5.
    fn claim_outer(&self, firmware: &super::Firmware, engine: crate::g17::job::Engine) -> Result<bool> {
        if !firmware.init.is_m5() {
            return Ok(true);
        }
        let (held, count) = match engine {
            crate::g17::job::Engine::Compute => {
                let slots = firmware
                    .queues
                    .compute
                    .iter()
                    .flatten()
                    .filter_map(|entry| entry.queue.as_deref())
                    .filter(|queue| queue.owner() == Some(self.owner))
                    .map(|queue| queue.outer_slots())
                    .max()
                    .unwrap_or(2);
                (&self.outer_compute, slots)
            }
            // Tiling and fragment: one slot each.
            crate::g17::job::Engine::Render => (&self.outer_render, 2),
        };
        let mut held = held.lock();
        if held.as_ref().is_some_and(|claim| claim.remaining() != 0) {
            return Ok(true);
        }
        let _m5_outer = firmware.port.m5_outer_lock();
        match firmware.init.reserve_outer(self.context.policy().priority(), count) {
            Ok(claim) => {
                // Replaces only a spent claim, whose drop notifies nobody.
                *held = Some(claim);
                Ok(true)
            }
            Err(error) if error == EBUSY => Ok(false),
            Err(error) => Err(error),
        }
    }

    /// Publishes into this owner's compute channel, lending the held outer claim's credits.
    fn publish_claimed(
        &self,
        firmware: &mut super::Firmware,
        packet: &Arc<super::job::Packet>,
        dependencies: &[super::fw::kick::KickDependency],
        deferred: &mut DeferredBatch,
    ) -> Result {
        if !firmware.init.is_m5() {
            return firmware.publish_compute(self.owner, packet.clone(), dependencies, deferred);
        }
        let mut held = self.outer_compute.lock();
        let _scope = (*held)
            .as_mut()
            .filter(|claim| claim.remaining() != 0)
            .map(|claim| firmware.port.outer_scope(&firmware.init, claim))
            .transpose()?;
        firmware.publish_compute(self.owner, packet.clone(), dependencies, deferred)
    }

    /// Returns an engine's unspent outer claim after its publication, cancellation or close.
    /// Outside the device mutex: returned credits notify waiters.
    pub(super) fn release_outer(&self, engine: crate::g17::job::Engine) {
        if !self.context.is_m5() {
            return;
        }
        let held = match engine {
            crate::g17::job::Engine::Compute => &self.outer_compute,
            crate::g17::job::Engine::Render => &self.outer_render,
        };
        let claim = held.lock().take();
        drop(claim);
    }

    fn compute_readiness(&self, packet: &Arc<super::job::Packet>, required: super::freelist::ComputeCapacity) -> Readiness {
        match packet.completion.work_state().and_then(|work| work.acquire(crate::g17::job::Engine::Compute)) {
            Err(error) if error == EBUSY => return Readiness::Progress,
            Err(_) => return Readiness::Ready,
            Ok(_) => {}
        }
        let sources_checked = if packet.completion.sources().is_some() {
            match self.sources_published(packet) {
                Ok(false) => return Readiness::Progress,
                Err(_) => return Readiness::Ready,
                Ok(true) => true,
            }
        } else {
            false
        };
        // The steady state: this owner's bound channel(s) are all cached and installed.
        let bound_count = self.context.compute_binding_count() as usize;
        if (!sources_checked || self.context.is_m5())
            && bound_count != 0
            && bound_count <= super::pairs::COMPUTE_CHANNELS_MAX
        {
            let cells = self.direct.lock().clone();
            let cached_count = cells.iter().filter(|cell| cell.is_some()).count();
            if cached_count == bound_count {
                if let Some(primary) = cells[0].clone() {
                    // Validate siblings first so primary is claimed only once every channel passes.
                    let mut all_ok = true;
                    let mut outer_slots = 1u32;
                    for cell in cells[1..].iter().flatten() {
                        match cell.lock() {
                            Some(queue)
                                if queue.installed()
                                    && !queue.spared_pending()
                                    && queue.pool_fits(required)
                                    && matches!(
                                        queue.matches(self.owner, queue.qid(), &self.context),
                                        Ok(true)
                                    ) =>
                            {
                                outer_slots = outer_slots.max(queue.outer_slots());
                            }
                            _ => {
                                all_ok = false;
                                break;
                            }
                        }
                    }
                    let bound = if all_ok {
                        primary.lock().and_then(|mut queue| {
                            let ok = queue.installed()
                                && !queue.spared_pending()
                                && queue.pool_fits(required)
                                && matches!(
                                    queue.matches(self.owner, queue.qid(), &self.context),
                                    Ok(true)
                                )
                                && self.context.compute_binding_count() as usize == cached_count;
                            if ok {
                                queue.claim();
                                Some(outer_slots.max(queue.outer_slots()))
                            } else {
                                None
                            }
                        })
                    } else {
                        None
                    };
                    if let Some(outer_slots) = bound {
                        self.hold_claim(Some(primary));
                        if !self.context.is_m5() {
                            return Readiness::Ready;
                        }
                        return match self.claim_compute_outer_direct(outer_slots) {
                            Ok(false) => Readiness::Outer,
                            _ => Readiness::Ready,
                        };
                    }
                }
            }
        }
        if !sources_checked {
            match self.sources_published(packet) {
                Ok(false) => return Readiness::Progress,
                Err(_) => return Readiness::Ready,
                Ok(true) => {}
            }
        }
        let lease = match self.shared.preparations.try_enter() {
            Ok(lease) => lease,
            Err(error) if error == EAGAIN => return Readiness::Progress,
            Err(_) => return Readiness::Ready,
        };
        let state = self.shared.state.lock();
        let Some(firmware) = state.as_deref() else { return Readiness::Ready; };
        if !lease.is_current()
            || firmware.recovery.pending()
            || super::recovery::Memory::recovery_state(&firmware.init).unwrap_or(0) != 0
        {
            return Readiness::Progress;
        }
        match firmware.queues.compute_readiness(self.owner, Some((&self.context, required))) {
            Readiness::Ready => match self.claim_outer(firmware, crate::g17::job::Engine::Compute) {
                Ok(false) => Readiness::Outer,
                _ => Readiness::Ready,
            },
            other => other,
        }
    }

    /// Replaces the prepared packet's channel claim; None releases it after publication,
    /// cancellation or close.
    pub(super) fn hold_claim(&self, cell: Option<Arc<ComputeCell>>) {
        let old = core::mem::replace(&mut *self.claimed.lock(), cell);
        let freed = old.as_ref().and_then(|old| old.lock()).is_some_and(|mut queue| {
            let busy = !queue.channel_free();
            queue.release_claim();
            busy && queue.channel_free()
        });
        if freed {
            // A channel nobody prepares into any more is shareable now.
            self.shared.channels.notify();
        }
    }

    /// Called with the device mutex held after a successful publication: remembers all bound
    /// channels for direct publication.
    fn remember_direct(&self, registry: &Registry) {
        let cells = registry.compute_cells(self.owner);
        let mut direct = self.direct.lock();
        let current = direct.iter().zip(cells.iter()).all(|pair| match pair {
            (Some(old), Some(new)) => Arc::ptr_eq(old, new),
            (None, None) => true,
            _ => false,
        });
        if !current {
            let old = core::mem::replace(&mut *direct, cells);
            drop(direct);
            drop(old);
        }
    }

    /// Publishes a dependency-free compute command from its submit ioctl when
    /// its queue is already bound and nothing has to wait, as renders do. Any
    /// wait leaves the command, unpublished, to its scheduler.
    pub(super) fn try_compute_early(&self, packet: &Arc<super::job::Packet>) -> bool {
        if self.compute_backlog.load(Ordering::Acquire) != 0 || packet.has_dependencies() {
            return false;
        }
        // A command the ray-traversal rule could hold now is left to its scheduler.
        if !self.traversal_inline(packet) {
            return false;
        }
        if !packet.begin_publication() {
            packet.completion.fail_unpublished(ECANCELED);
            packet.set_early_result(Err(ECANCELED));
            return true;
        }
        let mut deferred = match DeferredBatch::publication() {
            Ok(deferred) => deferred,
            Err(error) => {
                if packet.completion.status().get() != 0 {
                    <Self as super::job::Backend>::fail_vm(self, error);
                }
                packet.completion.fail_unpublished(error);
                packet.set_early_result(Err(error));
                return true;
            }
        };
        let result = match self.compute_direct_packet(packet, &mut deferred) {
            Some(result) => result,
            None => self.compute_inline(packet, &mut deferred),
        };
        if matches!(result, Err(error) if error == render::FALLBACK) && !packet.is_published() {
            deferred.finish();
            packet.abandon_publication();
            return false;
        }
        let result = self
            .finish_publication(packet, result, &mut deferred)
            .map(|_| ());
        packet.set_early_result(result);
        true
    }

    fn compute_direct_packet(
        &self,
        packet: &Arc<super::job::Packet>,
        deferred: &mut DeferredBatch,
    ) -> Option<Result> {
        let required = match self.compute_capacity(&packet.command) {
            Ok(required) => required,
            Err(error) => return Some(Err(error)),
        };
        self.compute_direct(packet, required, deferred)
    }

    /// compute_packet's publication section without its waits: every condition
    /// it would sleep or retry for returns FALLBACK instead.
    fn compute_inline(
        &self,
        packet: &Arc<super::job::Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        let required = self.compute_capacity(&packet.command)?;
        if packet.cancel_requested() || self.admission_open().is_err() || packet.completion.status().get() != 0 {
            return Err(render::FALLBACK);
        }
        let lease = self.enter_preparation(false)?;
        match packet.completion.work_state()?.acquire(crate::g17::job::Engine::Compute) {
            Err(error) if error == EBUSY => return Err(render::FALLBACK),
            result => {
                result?;
            }
        }
        // A lone admission keeps the original latency-sensitive probe.
        // Competing publishers otherwise spin on a binding they do not own.
        if !self.context.compute_binding_hint()
            && self.shared.compute_demand.load(Ordering::Relaxed) > 1
        {
            return Err(render::FALLBACK);
        }
        let mut state = self.shared.state.lock();
        let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
        if !lease.is_current()
            || self.shared.crashed.load(Ordering::Acquire)
            || firmware.recovery.pending()
            || super::recovery::Memory::recovery_state(&firmware.init)? != 0
        {
            return Err(render::FALLBACK);
        }
        firmware.replay_compute(deferred)?;
        if !firmware.init.compute_vm_cached(self.context.vm())
            || !firmware.queues.compute_bound(self.owner, &self.context, required)?
        {
            return Err(render::FALLBACK);
        }
        if self.fans_out(packet, || {
            firmware.queues.compute_primary(self.owner).is_some_and(|queue| queue.sustained())
        }) {
            if firmware.queues.wants_compute_sibling(
                self.owner,
                &self.context,
                self.compute_channels,
                required,
            ) {
                // Bind a warm reusable sibling inline if one is ready; otherwise fall back once
                // so `compute_packet` can build a fresh surplus sibling off-lock.
                match firmware.queues.bind_reusable_sibling(
                    self.owner,
                    &self.context,
                    self.compute_channels,
                    required,
                )? {
                    Some(true) => self.sibling_backoff_until.store(0, Ordering::Relaxed),
                    Some(false) => return Err(render::FALLBACK),
                    None => {}
                }
            } else if !firmware
                .queues
                .has_compute_sibling_headroom(self.owner, &self.context, required)
            {
                self.sibling_backoff_until.store(
                    now_ns().saturating_add(super::pairs::SIBLING_HEADROOM_BACKOFF_NS),
                    Ordering::Relaxed,
                );
            }
        }
        if !self.claim_outer(firmware, crate::g17::job::Engine::Compute)? {
            return Err(render::FALLBACK);
        }
        let dependencies = firmware.queues.render_prefix(self.owner, packet)?;
        match self.publish_claimed(firmware, packet, dependencies.as_slice(), deferred) {
            Err(error) if (error == EBUSY || error == EAGAIN) && !packet.is_published() => {
                Err(render::FALLBACK)
            }
            result => {
                if result.is_ok() {
                    self.remember_direct(&firmware.queues);
                }
                result
            }
        }
    }

    /// A scheduled dependency may refer to a previous recovery epoch. Do not
    /// acquire consumer capacity until every live source is published now.
    fn sources_published(&self, packet: &super::job::Packet) -> Result<bool> {
        let Some(sources) = packet.completion.sources() else { return Ok(true); };
        sources.check()?;
        if let Ok(lease) = self.shared.preparations.try_enter() {
            if let Some(port) = self.shared.port(&lease) {
                if !self.shared.crashed.load(Ordering::Acquire)
                    && port.recovery_state().ok() == Some(0)
                {
                    let generation = port.generation();
                    let mut all_fast = true;
                    for source in &sources.sources {
                        if source.submission.source_status()? {
                            continue;
                        }
                        if !source.submission.has_published_epoch(generation) {
                            all_fast = false;
                            break;
                        }
                    }
                    if all_fast {
                        return Ok(true);
                    }
                }
            }
        }
        let state = self.shared.state.lock();
        let firmware = state.as_deref().ok_or(ENODEV)?;
        if firmware.recovery.pending()
            || super::recovery::Memory::recovery_state(&firmware.init)? != 0
        { return Ok(false); }
        for source in &sources.sources {
            if source.submission.source_status()? { continue; }
            if !firmware.queues.compute.iter().flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .any(|queue| queue.has_input_source(&source.submission,
                    packet.completion.status(), firmware.recovery.generation()))
            { return Ok(false); }
        }
        Ok(true)
    }

    /// Called after taking this packet's publication claim. All waits and work
    /// state allocation precede the device mutex; only the checked epoch commits.
    fn compute_packet(
        &self,
        packet: &Arc<super::job::Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        packet.check_dependencies()?;
        let required = self.compute_capacity(&packet.command)?;
        if let Some(result) = self.compute_direct(packet, required, deferred) {
            return result;
        }
        // A bound queue needs none of ensure_compute's admission passes. Try
        // without them; the publication section below checks the same state.
        // Every retry blocks on the release that ends its wait: retirement
        // progress (work states, ring room, replays) and the reopening of
        // publication after recovery both signal `progress`.
        let mut bound = true;
        let mut sibling_tried = false;
        let mut demand = None;
        let deadline = now_ns().saturating_add(super::preparation::QUEUE_WAIT_NS);
        let check = || {
            if packet.cancel_requested() { return Err(ECANCELED); }
            self.admission_open()?;
            if packet.completion.status().get() != 0 { return Err(EIO); }
            Ok(())
        };
        let admission = Admission::new(deadline, &check);
        let progress = &self.shared.progress;
        let outer = self.outer_domain();
        // The waits below share one budget. When it runs out, the report names where it went.
        let result = (|| -> Result {
            loop {
                admission.check()?;
                let observed = progress.observe();
                let claimed = outer.as_ref().map(|domain| domain.observe());
                if !self.sources_published(packet)? {
                    let recovering = self.shared.state.lock().as_deref().is_some_and(|firmware| {
                        firmware.recovery.pending()
                            || super::recovery::Memory::recovery_state(&firmware.init).unwrap_or(0) != 0
                    });
                    if recovering {
                        admission.extend_for_recovery();
                    }
                    self.shared.queue_events();
                    let wait = if recovering { AdmissionWait::Recovery } else { AdmissionWait::Sources };
                    admission.timed(wait, || self.wait_release(progress, observed, &admission))?;
                    continue;
                }
                // All DRM dependencies were satisfied before this scheduler callback.
                // Recheck on every retry: pressure may yield an idle binding between
                // admission and publication without changing this logical queue.
                if !bound {
                    demand.get_or_insert_with(|| ComputeDemand::new(&self.shared));
                    admission.timed(AdmissionWait::Channel, || {
                        Backend::ensure_compute(self, required, &admission)
                    })?;
                    admission.check()?;
                }
                let lease = admission.timed(AdmissionWait::Preparation, || {
                    self.shared.preparations.enter_until(&admission)
                })?;
                // Work states free as this context's earlier commands retire.
                match packet.completion.work_state()?.acquire(crate::g17::job::Engine::Compute) {
                    Err(error) if error == EBUSY => {
                        drop(lease);
                        self.shared.queue_events();
                        admission.timed(AdmissionWait::WorkState, || {
                            self.wait_release(progress, observed, &admission)
                        })?;
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
                    admission.extend_for_recovery();
                    drop(state);
                    drop(lease);
                    self.shared.queue_events();
                    admission.timed(AdmissionWait::Recovery, || {
                        self.wait_release(progress, observed, &admission)
                    })?;
                    continue;
                }
                firmware.replay_compute(deferred)?;
                if !firmware.init.compute_vm_cached(self.context.vm())
                    || !firmware.queues.compute_bound(self.owner, &self.context, required)?
                {
                    bound = false;
                    drop(state);
                    drop(lease);
                    continue;
                }
                if !sibling_tried
                    && self.fans_out(packet, || {
                        firmware
                            .queues
                            .compute_primary(self.owner)
                            .is_some_and(|queue| queue.sustained())
                    })
                {
                    if firmware.queues.wants_compute_sibling(
                        self.owner,
                        &self.context,
                        self.compute_channels,
                        required,
                    ) {
                        sibling_tried = true;
                        match firmware.queues.bind_reusable_sibling(
                            self.owner,
                            &self.context,
                            self.compute_channels,
                            required,
                        )? {
                            Some(true) => {
                                self.sibling_backoff_until.store(0, Ordering::Relaxed);
                            }
                            Some(false) => {
                                drop(state);
                                drop(lease);
                                if self.try_add_compute_sibling(required, &admission)? {
                                    self.sibling_backoff_until.store(0, Ordering::Relaxed);
                                } else {
                                    self.sibling_backoff_until.store(
                                        now_ns().saturating_add(
                                            super::pairs::SIBLING_HEADROOM_BACKOFF_NS,
                                        ),
                                        Ordering::Relaxed,
                                    );
                                }
                                continue;
                            }
                            None => {}
                        }
                    } else if !firmware
                        .queues
                        .has_compute_sibling_headroom(self.owner, &self.context, required)
                    {
                        sibling_tried = true;
                        self.sibling_backoff_until.store(
                            now_ns().saturating_add(super::pairs::SIBLING_HEADROOM_BACKOFF_NS),
                            Ordering::Relaxed,
                        );
                    }
                }
                let held = self.claim_outer(firmware, crate::g17::job::Engine::Compute)?;
                if let (false, Some(domain), Some(claimed)) = (held, &outer, claimed) {
                    // The shared work ring has no unclaimed room: consumption or returned claims
                    // free it.
                    drop(state);
                    drop(lease);
                    self.shared.queue_events();
                    admission.timed(AdmissionWait::OuterRoom, || {
                        self.wait_release(domain, claimed, &admission)
                    })?;
                    continue;
                }
                let dependencies = firmware.queues.render_prefix(self.owner, packet)?;
                admission.check()?;
                let result = self.publish_claimed(firmware, packet, dependencies.as_slice(), deferred);
                if !packet.is_published()
                    && (matches!(result, Err(EAGAIN))
                        || (matches!(result, Err(EBUSY))
                            && (firmware.recovery.pending()
                                || super::recovery::Memory::recovery_state(&firmware.init)? != 0)))
                {
                    // Recovery or backpressure: nothing is firmware-visible. Other
                    // queues keep submitting and the event worker retires meanwhile.
                    let recovering = firmware.recovery.pending()
                        || super::recovery::Memory::recovery_state(&firmware.init)? != 0;
                    if recovering {
                        admission.extend_for_recovery();
                    }
                    drop(state);
                    drop(lease);
                    self.shared.queue_events();
                    let wait = if recovering { AdmissionWait::Recovery } else { AdmissionWait::Publication };
                    admission.timed(wait, || self.wait_release(progress, observed, &admission))?;
                    continue;
                }
                if result.is_ok() {
                    self.remember_direct(&firmware.queues);
                }
                return result;
            }
        })();
        if matches!(result, Err(error) if error == EBUSY) && !packet.is_published() && admission.expired() {
            self.report_admission_expiry(&admission);
        }
        result
    }

    /// Warns about a compute publication whose admission budget ran out, so that the lost
    /// device it causes can be attributed: the time spent in each wait, the owner's queue
    /// installation state and the recovery state. Rate-limited to the first few per boot.
    fn report_admission_expiry(&self, admission: &Admission<'_>) {
        static REPORTS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
        const ADMISSION_REPORTS: u32 = 8;
        const NAMES: [&str; ADMISSION_WAITS] =
            ["sources", "channel", "preparation", "work state", "recovery", "outer room", "publication"];
        if REPORTS.load(Ordering::Relaxed) >= ADMISSION_REPORTS {
            return;
        }
        let (total, spent) = admission.spent_ms();
        // Read under the device mutex, print after it is released.
        let (queue, pending, recovery_state) = {
            let state = self.shared.state.lock();
            let Some(firmware) = (*state).as_deref() else {
                return;
            };
            (
                firmware.queues.compute(self.owner).map(|queue| queue.admission_state()),
                firmware.recovery.pending(),
                super::recovery::Memory::recovery_state(&firmware.init),
            )
        };
        if REPORTS.fetch_add(1, Ordering::Relaxed) >= ADMISSION_REPORTS {
            return;
        }
        pr_warn!(
            "asahi: G17: compute publication of owner {} gave up after {} ms; ms by wait: {}={} {}={} {}={} {}={} {}={} {}={} {}={}; queue {:?}; recovery pending {}, state {:?}\n",
            self.owner, total,
            NAMES[0], spent[0], NAMES[1], spent[1], NAMES[2], spent[2], NAMES[3], spent[3],
            NAMES[4], spent[4], NAMES[5], spent[5], NAMES[6], spent[6],
            queue, pending, recovery_state,
        );
    }
}

impl super::Firmware {
    /// Both processors are stopped and their owner detached before this call.
    /// Error fences release their command mappings without the device mutex
    /// or an allocation-dependent collector.
    fn finish_stopped_queues(&mut self) {
        self.stop_render();
        for mut queue in self
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
    /// Charge only real watchdog expirations, after the per-job allowance. A command's first
    /// expiry is its own client's: settling it fails that client's work, or spares it behind
    /// another VM's, and the client can do that once per command it submits. Counted for the
    /// device, one process's hung commands, one per queue, stopped the GPU for every client
    /// until reboot. Only an expiry that settling the command did not end (`again`) counts
    /// toward the storm limit; a changing client or recovery generation cannot mint a fresh
    /// storm budget for those.
    pub(super) fn watchdog_timeout(self: &Arc<Self>, again: bool) -> bool {
        if self.crashed.load(Ordering::Acquire) { return true; }
        if !again { return false; }
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
        self.disable_rescan();
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
        self.forget_port();
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
        self.forget_port();
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
            if !firmware.init.has_engine_retirement_witness() {
                // Engines may still write into the session's pages after the processors
                // stop, and nothing here proves they have stopped: keep the session. Its
                // callbacks run this module's code: the start pinned the module, so this
                // only confirms the pin.
                if super::retain_session().is_err() {
                    dev_err!(self.dev.as_ref(), "GPU session kept without a module pin\n");
                }
                core::mem::forget(firmware);
                return;
            }
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
        let mut freed = false;
        let mut releasing = false;
        for mut queue in self
            .queues
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            let busy = !queue.channel_free();
            if queue.recycle_killed()? || queue.recycle_quarantined_idle()? {
                queue.retain_ring_until_stop(&self.pool);
            }
            if deferred.vacated_room() {
                if let Some(vacated) = queue.take_recycled_ring() {
                    deferred.defer_vacated(vacated);
                    freed = true;
                }
            }
            if let Some(binding) = queue.detach_exited_owner() {
                deferred.defer_binding(binding);
            }
            freed |= busy && queue.channel_free();
            releasing |= queue.pool_release_pending();
        }
        freed |= self.queues.observe_pool_releases(&self.init, deferred)? != 0;
        let shared = &self.primary.state.shared;
        // Edge notifications cover the transitions this driver makes; every
        // event pass also rechecks the level, so capacity that appeared through
        // another engine (render QIDs, closed logical queues, render-held work
        // states) still ends a scheduler wait that has no timeout of its own.
        if freed
            || (shared.channels.has_waiters()
                && self.queues.compute_readiness(0, None) == Readiness::Ready)
        {
            shared.channels.notify();
        }
        // A pool release has no completion event: each observation pass is
        // the progress a growing owner waits for.
        if releasing || shared.progress.has_waiters() {
            shared.progress.notify();
        }
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
        } else if self.primary.state.shared.compute_demand.load(Ordering::Acquire) != 0 {
            // A retired channel being requested by another runnable publisher
            // is not an unused warm pool. Keep its existing backing through
            // handoff; real allocation pressure above still forces reclamation.
            super::kick::COMPUTE_CHANNELS
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
        let now = now_ns();
        let waits = || self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref())
            .filter_map(|queue| queue.kill_waiting(now));
        compute_exit::write_kill_waits(f, waits().count(), waits().max().unwrap_or(0))?;
        compute_exit::write_render_kills(f, self.teardown.kill_waits(now).count(),
                                         self.teardown.kill_waits(now).max().unwrap_or(0))?;
        writeln!(f, "compute_slots used={} total={} reusable={} building={}",
                 self.compute.iter().flatten().count(), self.compute.len(),
                 self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()).filter(|queue| queue.reusable()).count(),
                 self.compute.iter().flatten().filter(|entry| entry.queue.is_none()).count())?;
        writeln!(f, "compute_channels used={} fresh_fits={}",
                 self.compute.iter().flatten().count(),
                 u8::from(self.compute_qid_fits()))?;
        writeln!(f, "scheduler_teardown pending={}", self.teardown.len())?;
        for queue in self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()) {
            queue.write_capacity(f)?;
        }
        Ok(())
    }
}

impl Backend {
    /// Compute admission without a channel: give back what pressure allows, then block until a
    /// channel is released after `observed` (sampled before the failed check).
    fn wait_channels(&self, observed: u64, admission: &Admission<'_>, reclaim_backing: bool) -> Result {
        self.reclaim_capacity(admission, reclaim_backing)?;
        self.wait_release(&self.shared.channels, observed, admission)
    }

    /// Blocks until `domain` is signalled after `observed`, this packet's admission ends, or one
    /// admission slice passes: cancellation and logical close have no release to wait for.
    fn wait_release(
        &self,
        domain: &super::availability::Availability,
        observed: u64,
        admission: &Admission<'_>,
    ) -> Result {
        use kernel::dma_fence::RawDmaFence;
        let slice = admission.slice_ms()?;
        if let Some(fence) = domain.wait(observed)? {
            // SAFETY: The owned fence keeps the raw DMA fence alive throughout the wait.
            let waited = unsafe {
                kernel::bindings::dma_fence_wait_timeout(
                    fence.raw(),
                    false,
                    kernel::time::msecs_to_jiffies(slice) as isize,
                )
            };
            if waited < 0 {
                return Err(Error::from_errno(waited as i32));
            }
        }
        admission.check()
    }

    /// The shared lower window refuses free-list backing, or has no room for one
    /// more render pool: idle render pools are worth giving back. Never on M5,
    /// which places and reclaims its pools by identity.
    fn lower_window_short(&self) -> bool {
        if self.context.is_m5() {
            return false;
        }
        if super::freelist::lower_pressure() {
            return true;
        }
        let uat = {
            let state = self.shared.state.lock();
            match (*state).as_deref() {
                Some(firmware) => firmware.uat.clone(),
                None => return false,
            }
        };
        // Off the device mutex: this takes the address space's lock.
        uat.kernel_lower_vm()
            .capacity(uat.kernel_lower_dynamic_range())
            .is_ok_and(|(_, largest)| largest < super::freelist::RENDER_POOL_SPAN)
    }

    /// Reclaim steps of a capacity wait that never sleep: observe consumed pool releases and,
    /// under backing pressure, give idle pools back.
    fn reclaim_capacity(&self, admission: &Admission<'_>, reclaim_backing: bool) -> Result {
        admission.check()?;
        let mut deferred = DeferredBatch::reclaim()?;
        let pools = reclaim_backing && self.lower_window_short();
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
            // Consumption, not publication or an elapsed timeout, frees backing.
            firmware.queues.observe_pool_releases(&firmware.init, &mut deferred)?;
            // Pool eviction addresses backing allocation pressure, not channel
            // occupancy. Normal idle expiry and the retained-pool cap still run
            // in the event worker for either kind of admission wait.
            if pools {
                firmware.queues.reclaim_idle_render_pools(&firmware.init, &mut deferred)?;
            }
            if reclaim_backing && !firmware.render_control_backpressured() {
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
        let mut slots = KVec::with_capacity(crate::mmu::MAX_EXECUTION_CONTEXTS as usize, GFP_KERNEL).unwrap();
        for _ in 0..crate::mmu::MAX_EXECUTION_CONTEXTS {
            slots.push(registry.admission.reserve_queue().unwrap(), GFP_KERNEL).unwrap();
        }
        // With every execution context open, compute keeps only the channels
        // that leave each logical queue a dedicated render pair.
        for owner in 1..=super::super::kick::COMPUTE_FLOOR as u64 {
            registry.reserve_compute(owner).unwrap();
        }
        assert!(matches!(registry.reserve_compute(1000), Err(EAGAIN)));
        assert!(registry.qids.render_fits(slots.len()));
        for owner in 1..=u64::from(crate::mmu::MAX_EXECUTION_CONTEXTS) {
            let lease = registry.admission.acquire_render(owner, 1, None, u64::MAX, || Ok(())).unwrap();
            registry.ensure_render_ids(lease.slot(), owner).unwrap();
            leases.push(lease, GFP_KERNEL).unwrap();
        }
        assert_eq!(leases.len(), crate::mmu::MAX_EXECUTION_CONTEXTS as usize);
        // Every accepted logical render owner now has its physical pair; no
        // published identity was cancelled or retyped to achieve the fit.
    }

    #[test]
    fn a_reclaim_has_room_for_the_render_pools_it_gives_back() {
        assert!(!DeferredBatch::publication().unwrap().render_backing_room());
        assert!(DeferredBatch::reclaim().unwrap().render_backing_room());
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
        let mut slots = KVec::with_capacity(limit as usize, GFP_KERNEL).unwrap();
        let mut leases = KVec::with_capacity(limit as usize, GFP_KERNEL).unwrap();
        for owner in 1..=u64::from(limit) {
            slots.push(registry.admission.reserve_queue().unwrap(), GFP_KERNEL).unwrap();
            let lease = registry.admission.acquire_render(owner, 1, None, u64::MAX, || Ok(())).unwrap();
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
                let lease = registry.admission.acquire_render(owner, 1, None, u64::MAX, || Ok(())).unwrap();
                registry.ensure_render_ids(lease.slot(), owner).unwrap();
                leases.push(lease, GFP_KERNEL).unwrap();
            }
        }
        for owner in 1..=super::super::kick::COMPUTE_FLOOR as u64 {
            registry.reserve_compute(owner).unwrap();
        }
        assert_eq!(leases.len(), 36);
    }

    #[test]
    fn compute_grows_with_free_qids_and_narrows_queue_admission() {
        let mut registry = KBox::init(Registry::new(1, Deflake::ONE_CLUSTER, [0; 2], &V26A428), GFP_KERNEL).unwrap();
        let mut slots = KVec::with_capacity(20, GFP_KERNEL).unwrap();
        for _ in 0..20 {
            slots.push(registry.admission.reserve_queue().unwrap(), GFP_KERNEL).unwrap();
        }
        // Sixteen open compute clients each keep their own channel.
        for owner in 1..=16u64 {
            registry.reserve_compute(owner).unwrap();
        }
        // Growth stops where open queues plus headroom would lose their pairs.
        let mut owner = 17;
        while registry.reserve_compute(owner).is_ok() {
            owner += 1;
        }
        assert!(registry.qids.render_fits(slots.len() + 24));
        assert!(!registry.qids.render_fits(crate::mmu::MAX_EXECUTION_CONTEXTS as usize));
    }

    #[test]
    fn cancelable_siblings_do_not_block_compute_on_full_qid_table() {
        let mut registry = KBox::init(
            Registry::new(1, Deflake::ONE_CLUSTER, [0; 2], &V26A428),
            GFP_KERNEL,
        )
        .unwrap();
        let mut slots = KVec::with_capacity(32, GFP_KERNEL).unwrap();
        for owner in 1..=32u64 {
            slots.push(registry.admission.reserve_queue().unwrap(), GFP_KERNEL).unwrap();
            let lease = registry
                .admission
                .acquire_render(owner, 2, None, u64::MAX, || Ok(()))
                .unwrap();
            registry.ensure_render_ids(lease.slot(), owner).unwrap();
            registry.publish_render_ids(lease.slot()).unwrap();
            if owner <= 31 {
                let sibling = registry
                    .admission
                    .warmable_sibling(owner, lease.slot())
                    .unwrap();
                registry.ensure_render_ids(sibling.slot(), owner).unwrap();
                drop(sibling);
            }
            drop(lease);
        }
        // All 128 QIDs are now occupied: 32 published primaries (64), 31 idle
        // unpublished siblings (62), and the two bootstrap render reservations.
        assert!(!registry.qids.render_pair_fits());
        // Consume both bootstrap replacements with installed compute channels.
        registry.reserve_compute(1000).unwrap();
        registry.reserve_compute(1001).unwrap();
        // Raw compute_fits() is false because no free or bootstrap QID remains,
        // yet idle unpublished siblings can yield QIDs for fresh compute work.
        assert!(!registry.qids.compute_fits(registry.admission.render_demand(0)));
        assert!(registry.has_cancelable_render_spare().unwrap());
        assert!(registry.compute_qid_fits());
        assert!(matches!(registry.compute_readiness(1002, None), Readiness::Ready));
        let mut reclaimed = None;
        registry.reserve_compute_reclaiming(1002, &mut reclaimed).unwrap();
        assert!(reclaimed.is_some());
        drop(reclaimed);
        // Compute grows to the full budget left above render_reserve(32) = 114.
        let mut count = 3usize;
        let mut owner = 1003u64;
        while registry.compute_qid_fits() {
            let mut spare = None;
            registry.reserve_compute_reclaiming(owner, &mut spare).unwrap();
            drop(spare);
            count += 1;
            owner += 1;
        }
        assert_eq!(count, super::super::kick::QID_COUNT - super::super::kick::render_reserve(32));
        assert!(matches!(registry.compute_readiness(owner, None), Readiness::Channel));
        let mut spare = None;
        assert!(matches!(registry.reserve_compute_reclaiming(owner, &mut spare), Err(EAGAIN)));
        assert!(spare.is_none());
        assert!(registry.qids.render_fits(slots.len() + 24));
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
