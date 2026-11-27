// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Device-wide publication state that physical queues share without the device mutex.
//!
//! A firmware session owns its port, and the views it holds name that session's
//! firmware-shared memory. Code that does not hold the device mutex reaches the port only
//! through a preparation lease ([`super::Shared::port`]): shutdown and device loss join every
//! lease before they detach the session. Each piece below is atomic or has its own lock.
//!
//! Lock order: device mutex -> outer_{compute,render} -> compute queue cell -> the locks of
//! this port. Among the port's locks: render publication lock -> M5 outer-ring lock ->
//! admission pool, QoS, mailbox. None of their holders ever takes the device mutex.

use super::{
    fw::{
        channels::{WorkSlot, WORK_RING_SLOTS},
        queue::{DataMaster, Policy},
    },
    qos,
};
use core::sync::atomic::{fence, AtomicBool, AtomicU32, AtomicU64, AtomicU8, Ordering};
use kernel::{
    new_mutex, new_spinlock,
    prelude::*,
    soc::apple::rtkit,
    sync::{Arc, Mutex, MutexGuard, SetOnce, SpinLock},
};

const PRIORITIES: usize = Policy::PRIORITIES as usize;

/// Minimum utilization of the primary coprocessor's receive thread, which handles every
/// completion, while the GPU itself is the bottleneck (see `Feed::gpu_bound`). Otherwise the
/// thread keeps the scheduler's RT default, which takes its CPU cluster to the top frequency for
/// each completion: the completion may be on the critical path, of a waiter, of an idle GPU or of
/// work bound by host refills. While the GPU is the bottleneck the burst gains nothing, and at the
/// SoC's thermal limit the power it takes comes out of the GPU's clock.
const RX_UTIL_MIN_GPU_BOUND: u32 = 256;
const RX_STATE_UNSET: u8 = 0;
const RX_STATE_DEFAULT: u8 = 1;
const RX_STATE_GPU_BOUND: u8 = 2;

/// One priority class's compute work ring: its slot array and the cursors of its two firmware
/// consumers and of the host producer.
#[derive(Clone, Copy)]
pub(super) struct RingView {
    pub(super) slots: *mut u8,
    pub(super) consumers: [*const AtomicU32; 2],
    pub(super) producer: *const AtomicU32,
}

impl RingView {
    fn cursors(&self) -> Result<[u32; 3]> {
        // SAFETY: The views name 32-bit words of the session's retained firmware objects, which
        // the port's owner keeps alive. Other agents only use single 32-bit accesses.
        let values = unsafe {
            [
                (*self.consumers[0]).load(Ordering::Relaxed),
                (*self.consumers[1]).load(Ordering::Relaxed),
                (*self.producer).load(Ordering::Relaxed),
            ]
        };
        if values.iter().any(|&value| value >= WORK_RING_SLOTS) {
            return Err(EINVAL);
        }
        Ok(values)
    }

    /// Slots the host may still fill before reaching either consumer.
    fn free(&self) -> Result<u32> {
        let [first, second, producer] = self.cursors()?;
        let room = |consumer: u32| (consumer + WORK_RING_SLOTS - producer - 1) % WORK_RING_SLOTS;
        Ok(room(first).min(room(second)))
    }
}

/// Raw pointers to the M5 submission-history table in firmware-shared memory.
#[derive(Clone, Copy)]
pub(super) struct M5HistoryView {
    pub(super) generation: *const AtomicU32,
    pub(super) config: [*const AtomicU32; 4],
    pub(super) slots: *mut u8,
}

/// Raw views of the session's firmware-shared memory, fixed once the firmware is built.
pub(super) struct Views {
    pub(super) compute: [RingView; PRIORITIES],
    pub(super) tiling: [RingView; PRIORITIES],
    pub(super) fragment: [RingView; PRIORITIES],
    /// Completion stamp page A: one word per QID, written by the firmware.
    pub(super) stamps: *const AtomicU32,
    pub(super) qos: *mut u8,
    pub(super) recovery: *const AtomicU32,
    pub(super) mailbox: *const super::Mailbox,
    pub(super) m5_credits: Option<Arc<super::initdata::outer_credit::Credits>>,
    pub(super) m5_history: Option<M5HistoryView>,
}

// SAFETY: The views only name memory and a mailbox owned by the firmware session, which outlives
// every user of the port. Accesses are atomic words or serialized by the port's locks.
unsafe impl Send for Views {}
// SAFETY: As above.
unsafe impl Sync for Views {}

#[pin_data]
pub(super) struct Port {
    views: SetOnce<Views>,
    regs: SetOnce<crate::regs::G17Resources>,
    registration: SetOnce<super::fw::variant::QueueRegistration>,
    /// Compute work-ring slots reserved by publishers that have not exposed them yet.
    #[pin]
    rings: SpinLock<[u32; PRIORITIES]>,
    #[pin]
    qos: SpinLock<qos::Accounting>,
    /// Sets the completion receive thread's minimum utilization, from `feed`'s view of the GPU.
    rx: SetOnce<rtkit::RxUtilMin>,
    feed: Arc<super::feed::Feed>,
    /// The request last made, `None` before the first.
    #[pin]
    rx_applied: SpinLock<Option<Option<u32>>>,
    /// Lock-free mirror of `rx_applied` (`RX_STATE_*`) so steady-state updates avoid locking.
    rx_state: AtomicU8,
    /// Serializes every render publication, with or without the device mutex: the tiling
    /// and fragment work rings have one host producer at a time, and same-VM attachment
    /// overlaps are ordered by publication order (see hazard.rs).
    #[pin]
    render: Mutex<()>,
    /// Serializes M5 shared outer-ring writes, host-port gating transactions, and submission
    /// history geometry/updates across direct compute publishers and device-mutex publishers.
    #[pin]
    m5_outer: Mutex<Option<(u32, u64, u64)>>,
    /// The accepted recovery generation, mirrored for publishers without the device mutex.
    generation: AtomicU64,
    /// Device publication order shared by compute and render commands.
    publications: AtomicU64,
    /// Full firmware effort is applied; cleared before an idle clear rechecks `submissions`.
    effort_full: AtomicBool,
    demand: super::power::Demand,
    /// Serializes busy-share window accounting and CLPC busy updates.
    #[pin]
    busy: SpinLock<super::power::BusyShare>,
    /// Mirrored from `busy` and `qos` for a lock-free fast path in `note_gpu_activity()`.
    busy_window_start: AtomicU64,
    busy_below_top: AtomicBool,
    busy_inactive_since: AtomicU64,
    oldest_in_flight_any: AtomicU64,
    statistics_epoch: AtomicU64,
    last_submission_ns: AtomicU64,
    submissions: AtomicU64,
}

/// Room in both render work rings of one priority, observed under the render publication
/// lock. While that lock is held no other host producer moves, and the firmware consumers only
/// advance, so the observed producers stay current and the room stays available.
#[derive(Clone, Copy)]
pub(super) struct RenderRoom {
    priority: u8,
    /// Fragment, then tiling producer.
    producers: [u32; 2],
}

impl RenderRoom {
    pub(super) fn priority(&self) -> u8 {
        self.priority
    }
}

/// Room for two slots on M5's shared outer work ring of one priority, reserved under
/// both the render publication lock and `m5_outer`.
#[derive(Clone, Copy)]
pub(super) struct M5RenderRoom {
    priority: u8,
    start: u32,
    next: [u32; 2],
}

impl M5RenderRoom {
    pub(super) fn priority(&self) -> u8 {
        self.priority
    }
}

/// Serializes entry and Drop of an M5 [`super::initdata::OuterScope`] under `Port::m5_outer`
/// so `Credits::scoped` and `reserved[]` cannot race with direct M5 reservations or publications.
pub(super) struct PortOuterScope<'a> {
    scope: Option<super::initdata::OuterScope<'a>>,
    port: Arc<Port>,
}

impl Drop for PortOuterScope<'_> {
    fn drop(&mut self) {
        if let Some(scope) = self.scope.take() {
            let _m5_outer = self.port.m5_outer_lock();
            drop(scope);
        }
    }
}

/// A compute work-ring slot reserved before any command becomes visible. Dropping an unused
/// claim returns the slot.
pub(super) struct OuterClaim<'a> {
    port: &'a Port,
    priority: usize,
    armed: bool,
}

impl Drop for OuterClaim<'_> {
    fn drop(&mut self) {
        if self.armed {
            let mut reserved = self.port.rings.lock();
            reserved[self.priority] = reserved[self.priority].saturating_sub(1);
        }
    }
}

impl Port {
    pub(super) fn new(
        demand: super::power::Demand,
        feed: Arc<super::feed::Feed>,
    ) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                views: SetOnce::new(),
                regs: SetOnce::new(),
                registration: SetOnce::new(),
                rings <- new_spinlock!([0; PRIORITIES], "g17::Port::rings"),
                qos <- new_spinlock!(qos::Accounting::new(), "g17::Port::qos"),
                rx: SetOnce::new(),
                feed,
                rx_applied <- new_spinlock!(None, "g17::Port::rx_applied"),
                rx_state: AtomicU8::new(RX_STATE_UNSET),
                render <- new_mutex!((), "g17::Port::render"),
                m5_outer <- new_mutex!(None, "g17::Port::m5_outer"),
                generation: AtomicU64::new(0),
                publications: AtomicU64::new(0),
                effort_full: AtomicBool::new(false),
                demand,
                busy <- new_spinlock!(super::power::BusyShare::new(), "g17::Port::busy"),
                busy_window_start: AtomicU64::new(0),
                busy_below_top: AtomicBool::new(false),
                busy_inactive_since: AtomicU64::new(0),
                oldest_in_flight_any: AtomicU64::new(0),
                statistics_epoch: AtomicU64::new(0),
                last_submission_ns: AtomicU64::new(0),
                submissions: AtomicU64::new(0),
            }),
            GFP_KERNEL,
        )
    }

    /// Called once while the session is built, before any publisher can see the port.
    pub(super) fn attach(
        &self,
        views: Views,
        regs: crate::regs::G17Resources,
        registration: super::fw::variant::QueueRegistration,
        clamp_rx: bool,
    ) -> Result {
        // SAFETY: The session owns the mailbox and outlives every port user.
        let rx = unsafe { (*views.mailbox).rx_util_min_handle() };
        // Without `clamp_rx` (M5) no handle is attached and the receive thread keeps its
        // default; rx_update() then returns at once.
        if self.views.populate(views)
            && self.regs.populate(regs)
            && self.registration.populate(registration)
            && (!clamp_rx || self.rx.populate(rx))
        {
            Ok(())
        } else {
            Err(EBUSY)
        }
    }

    pub(super) fn register_queues(&self, queues: &[super::fw::kick::KsmQueue]) -> Result {
        use super::fw::variant::QueueRegistration;
        if self.registration.as_ref() != Some(&QueueRegistration::Mailbox) {
            return Ok(());
        }
        let regs = self.regs.as_ref().ok_or(ENODEV)?;
        for queue in queues {
            regs.register_ksm_queue(queue.qid, queue.descriptor)?;
        }
        Ok(())
    }

    fn views(&self) -> Result<&Views> {
        self.views.as_ref().ok_or(ENODEV)
    }

    /// Both MMU fault banks, for recovery diagnostics while the firmware is halted.
    pub(super) fn fault_banks(&self) -> Result<[(u64, u64); 2]> {
        self.regs.as_ref().ok_or(ENODEV)?.fault_banks()
    }

    fn ring(&self, priority: u8) -> Result<(usize, RingView)> {
        let index = usize::from(priority);
        Ok((index, *self.views()?.compute.get(index).ok_or(EINVAL)?))
    }

    /// Reserves one slot of the compute work ring of `priority`. EAGAIN while firmware has not
    /// consumed enough slots; only consumption, never a waiting publisher, frees one.
    pub(super) fn reserve_compute(&self, priority: u8) -> Result<OuterClaim<'_>> {
        let (index, ring) = self.ring(priority)?;
        let mut reserved = self.rings.lock();
        if ring.free()? <= reserved[index] {
            return Err(EAGAIN);
        }
        reserved[index] += 1;
        Ok(OuterClaim {
            port: self,
            priority: index,
            armed: true,
        })
    }

    /// Whether a compute publication at `priority` can reserve a slot now.
    pub(super) fn compute_ready(&self, priority: u8) -> Result<bool> {
        let (index, ring) = self.ring(priority)?;
        let reserved = self.rings.lock();
        Ok(ring.free()? > reserved[index])
    }

    /// Exposes `record` in its reserved slot. Inner queue records must be visible first. Returns
    /// the new producer, the retained consumer witness for this slot.
    pub(super) fn publish_compute(
        &self,
        mut claim: OuterClaim<'_>,
        record: &WorkSlot,
    ) -> Result<u32> {
        if record.data_master != DataMaster::Compute as u32 || !core::ptr::eq(claim.port, self) {
            return Err(EINVAL);
        }
        let ring = self.views()?.compute[claim.priority];
        let mut reserved = self.rings.lock();
        let [first, second, producer] = ring.cursors()?;
        let next = (producer + 1) % WORK_RING_SLOTS;
        if next == first || next == second {
            return Err(EIO);
        }
        // SAFETY: The ring view spans WORK_RING_SLOTS records, checked when the session built
        // it; `producer` is in range. The reservation proves firmware does not own this slot and
        // the ring lock excludes every other host writer. The record is a padding-free ABI value.
        unsafe {
            core::ptr::copy_nonoverlapping(
                core::ptr::from_ref(record).cast::<u8>(),
                ring.slots
                    .add(producer as usize * core::mem::size_of::<WorkSlot>()),
                core::mem::size_of::<WorkSlot>(),
            );
        }
        fence(Ordering::SeqCst);
        // SAFETY: As for `RingView::cursors`.
        unsafe { (*ring.producer).store(next, Ordering::Relaxed) };
        fence(Ordering::SeqCst);
        reserved[claim.priority] = reserved[claim.priority].saturating_sub(1);
        claim.armed = false;
        Ok(next)
    }

    /// Taken around every render publication; see `render`.
    pub(super) fn render_lock(&self) -> MutexGuard<'_, ()> {
        self.render.lock()
    }

    /// Serializes M5 shared outer-ring and host-port updates with direct M5 compute publishers.
    pub(super) fn m5_outer_lock(&self) -> MutexGuard<'_, Option<(u32, u64, u64)>> {
        self.m5_outer.lock()
    }

    /// Enters an M5 `OuterScope` under `m5_outer` and returns a guard that re-acquires
    /// `m5_outer` around the inner scope's Drop, preventing races with direct M5 reservations.
    pub(super) fn outer_scope<'a>(
        self: &Arc<Self>,
        init: &super::initdata::InitData,
        claim: &'a mut super::initdata::OuterClaim,
    ) -> Result<PortOuterScope<'a>> {
        let _m5_outer = self.m5_outer.lock();
        let scope = init.outer_scope(claim)?;
        Ok(PortOuterScope {
            scope: Some(scope),
            port: self.clone(),
        })
    }

    /// Clears the cached M5 submission-history geometry after firmware recovery restarts.
    pub(super) fn reset_m5_history(&self) {
        *self.m5_outer.lock() = None;
    }

    /// M5: the availability domain outer-ring claims wait on. None off M5 or before attach.
    pub(super) fn outer_domain(&self) -> Option<Arc<super::availability::Availability>> {
        self.views()
            .ok()?
            .m5_credits
            .as_ref()
            .map(|credits| credits.available())
    }

    /// Reserves `count` slots on M5's shared outer ring without taking the device mutex.
    /// Returns EBUSY if a device-mutex OuterScope is active or fewer than `count` slots are free.
    pub(super) fn reserve_m5_outer(
        &self,
        priority: u8,
        count: u32,
    ) -> Result<super::initdata::OuterClaim> {
        let views = self.views()?;
        let credits = views.m5_credits.as_ref().ok_or(EINVAL)?;
        let (_, ring) = self.ring(priority)?;
        let _guard = self.m5_outer.lock();
        if credits.is_scoped() {
            return Err(EBUSY);
        }
        let cursors = ring.cursors()?;
        credits.reserve(priority, count, cursors)
    }

    /// Reserves one slot on M5's shared outer ring for a direct compute publisher without
    /// taking the device mutex. Returns EBUSY if a device-mutex OuterScope is active or full.
    pub(super) fn reserve_m5_compute(&self, priority: u8) -> Result<super::initdata::OuterClaim> {
        self.reserve_m5_outer(priority, 1)
    }

    /// Publishes one compute slot on M5's shared outer ring while the caller holds `m5_outer`,
    /// consuming the caller's direct reservation.
    pub(super) fn publish_m5_compute_locked(
        &self,
        _guard: &MutexGuard<'_, Option<(u32, u64, u64)>>,
        priority: u8,
        claim: &mut super::initdata::OuterClaim,
        record: &WorkSlot,
    ) -> Result<u32> {
        if record.data_master != DataMaster::Compute as u32 {
            return Err(EINVAL);
        }
        let views = self.views()?;
        let credits = views.m5_credits.as_ref().ok_or(EINVAL)?;
        let (_, ring) = self.ring(priority)?;
        let [first, second, producer] = ring.cursors()?;
        let next = (producer + 1) % WORK_RING_SLOTS;
        if next == first || next == second {
            return Err(EIO);
        }
        // SAFETY: The ring view spans WORK_RING_SLOTS records, checked when the session built
        // it; `producer` is in range. The reservation proves firmware does not own this slot and
        // `m5_outer` excludes every other host writer on M5's shared outer ring.
        unsafe {
            core::ptr::copy_nonoverlapping(
                core::ptr::from_ref(record).cast::<u8>(),
                ring.slots
                    .add(producer as usize * core::mem::size_of::<WorkSlot>()),
                core::mem::size_of::<WorkSlot>(),
            );
        }
        fence(Ordering::SeqCst);
        // SAFETY: As for `RingView::cursors`.
        unsafe { (*ring.producer).store(next, Ordering::Relaxed) };
        fence(Ordering::SeqCst);
        credits.consume_direct(claim)?;
        Ok(next)
    }

    /// Issues an M5 host-port compute submission with gating held off while `m5_outer` is held.
    pub(super) fn submit_m5_work_held_locked(
        &self,
        _guard: &MutexGuard<'_, Option<(u32, u64, u64)>>,
        submission: &super::fw::m5::queue::HardwareSubmission,
    ) -> Result {
        self.regs.as_ref().ok_or(ENODEV)?.submit_m5_work_held(submission)
    }

    /// Counts `submissions` in the M5 submission-history table without taking the device mutex,
    /// while the caller holds `m5_outer`.
    pub(super) fn note_m5_submission_history_locked(
        &self,
        geometry: &mut MutexGuard<'_, Option<(u32, u64, u64)>>,
        submissions: u8,
    ) -> Result {
        let Some(history) = self.views()?.m5_history else {
            return Ok(());
        };
        for _ in 0..64 {
            // SAFETY: Pointers name retained, bounds-checked firmware-shared words.
            let generation = unsafe { (*history.generation).load(Ordering::Acquire) };
            if generation & 1 != 0 {
                core::hint::spin_loop();
                continue;
            }
            let (period, count) = match **geometry {
                Some((seen, period, count)) if seen == generation => (period, count),
                _ => {
                    // SAFETY: As above; seqlock generation recheck validates consistency.
                    let c = unsafe {
                        [
                            (*history.config[0]).load(Ordering::Relaxed),
                            (*history.config[1]).load(Ordering::Relaxed),
                            (*history.config[2]).load(Ordering::Relaxed),
                            (*history.config[3]).load(Ordering::Relaxed),
                        ]
                    };
                    fence(Ordering::Acquire);
                    // SAFETY: As above.
                    if generation != unsafe { (*history.generation).load(Ordering::Acquire) } {
                        core::hint::spin_loop();
                        continue;
                    }
                    let period = u64::from(c[0]) | (u64::from(c[1]) << 32);
                    let count = u64::from(c[2]) | (u64::from(c[3]) << 32);
                    **geometry = Some((generation, period, count));
                    (period, count)
                }
            };
            if period == 0 || count == 0 || count > 256 {
                return Err(EINVAL);
            }
            let (counter, frequency) = qos::clock();
            let ticks = qos::time_from_host(counter, frequency)?;
            let index = (ticks % period) / 25_000;
            if index >= count {
                return Err(ERANGE);
            }
            // SAFETY: `slots` spans 256 WC bytes and `index < count <= 256`.
            unsafe {
                let byte = history.slots.add(index as usize);
                let value = core::ptr::read_volatile(byte).wrapping_add(submissions);
                core::ptr::write_volatile(byte, value);
            }
            fence(Ordering::SeqCst);
            return Ok(());
        }
        Err(EBUSY)
    }

    /// Reserves two slots on M5's shared outer ring for a direct render pair while
    /// `m5_outer` is already held. Returns EBUSY if a device-mutex OuterScope is active
    /// or fewer than two slots are free.
    pub(super) fn reserve_m5_render_locked(
        &self,
        _guard: &MutexGuard<'_, Option<(u32, u64, u64)>>,
        priority: u8,
    ) -> Result<(M5RenderRoom, super::initdata::OuterClaim)> {
        let views = self.views()?;
        let credits = views.m5_credits.as_ref().ok_or(EINVAL)?;
        let (_, ring) = self.ring(priority)?;
        if credits.is_scoped() {
            return Err(EBUSY);
        }
        let cursors = ring.cursors()?;
        let start = cursors[2];
        let next = [(start + 1) % WORK_RING_SLOTS, (start + 2) % WORK_RING_SLOTS];
        if next.iter().any(|&p| p == cursors[0] || p == cursors[1]) {
            return Err(EBUSY);
        }
        let claim = credits.reserve(priority, 2, cursors)?;
        Ok((
            M5RenderRoom {
                priority,
                start,
                next,
            },
            claim,
        ))
    }

    /// Copies an M5 render pair's fragment, then tiling record into the two reserved slots
    /// of M5's shared outer ring without exposing them, commits accounting, issues the
    /// host-port advance, and publishes inner producers (as `Firmware::submit_m5_render_pair`).
    #[expect(
        clippy::too_many_arguments,
        reason = "Mirrors write_render_pair with the held M5 outer lock."
    )]
    pub(super) fn write_m5_render_pair(
        &self,
        _render: &MutexGuard<'_, ()>,
        _m5_outer: &MutexGuard<'_, Option<(u32, u64, u64)>>,
        room: &M5RenderRoom,
        fragment: &WorkSlot,
        tiling: &WorkSlot,
        commit_accounting: impl FnOnce() -> Result,
        publish_inner: impl FnOnce(),
    ) -> Result<[u32; 2]> {
        if !matches!(fragment.data_master, 1 | 2) || !matches!(tiling.data_master, 0 | 2) {
            return Err(EINVAL);
        }
        let regs = self.regs.as_ref().ok_or(ENODEV)?;
        let (_, ring) = self.ring(room.priority)?;
        let records = [
            WorkSlot::new(
                DataMaster::Compute,
                fragment.queue_va,
                fragment.qid,
                fragment.items_end,
                fragment.first != 0,
            )?,
            WorkSlot::new(
                DataMaster::Compute,
                tiling.queue_va,
                tiling.qid,
                tiling.items_end,
                tiling.first != 0,
            )?,
        ];
        let submission = super::fw::m5::queue::HardwareSubmission::advance_pair(
            fragment.qid,
            tiling.qid,
        )?;
        let destinations = [room.start, room.next[0]].map(|index| {
            // SAFETY: The ring view spans WORK_RING_SLOTS records; `reserve_m5_render_locked`
            // validated both indices are in range and host-owned while `m5_outer` stays held.
            unsafe {
                ring.slots
                    .add(index as usize * core::mem::size_of::<WorkSlot>())
            }
        });
        for (record, destination) in records.iter().zip(destinations) {
            // SAFETY: Both slots are distinct, reserved, and protected by `m5_outer`.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    core::ptr::from_ref(record).cast::<u8>(),
                    destination,
                    core::mem::size_of::<WorkSlot>(),
                );
            }
        }
        commit_accounting()?;
        regs.submit_m5_work(&submission)?;
        publish_inner();
        Ok(room.next)
    }

    /// Exposes a retained M5 render pair written by `write_m5_render_pair` with one
    /// producer update and consumes its two direct outer-ring credits.
    pub(super) fn expose_m5_render_pair(
        &self,
        _render: &MutexGuard<'_, ()>,
        _m5_outer: &MutexGuard<'_, Option<(u32, u64, u64)>>,
        room: &M5RenderRoom,
        claim: &mut super::initdata::OuterClaim,
        next: [u32; 2],
    ) -> Result {
        if next != room.next {
            return Err(EBUSY);
        }
        let views = self.views()?;
        let credits = views.m5_credits.as_ref().ok_or(EINVAL)?;
        let (_, ring) = self.ring(room.priority)?;
        if ring.cursors()?[2] != room.start {
            return Err(EBUSY);
        }
        fence(Ordering::SeqCst);
        // SAFETY: As for `RingView::cursors`; `m5_outer` has been held continuously
        // since `reserve_m5_render_locked` and `write_m5_render_pair`.
        unsafe { (*ring.producer).store(room.next[1], Ordering::Relaxed) };
        fence(Ordering::SeqCst);
        credits.consume_direct_count(claim, 2)
    }

    fn render_rings(&self, priority: u8) -> Result<(RingView, RingView)> {
        let views = self.views()?;
        let index = usize::from(priority);
        Ok((
            *views.fragment.get(index).ok_or(EINVAL)?,
            *views.tiling.get(index).ok_or(EINVAL)?,
        ))
    }

    /// Reads both render work rings of `priority` once. Returns their producers if both have a
    /// free slot; the room holds for as long as `guard`. Only firmware consumption, never a
    /// publisher without the render lock, changes it.
    pub(super) fn observe_render_room(
        &self,
        _guard: &MutexGuard<'_, ()>,
        priority: u8,
    ) -> Result<Option<RenderRoom>> {
        let (fragment, tiling) = self.render_rings(priority)?;
        let cursors = [fragment.cursors()?, tiling.cursors()?];
        let room = |[first, second, producer]: [u32; 3]| {
            let room =
                |consumer: u32| (consumer + WORK_RING_SLOTS - producer - 1) % WORK_RING_SLOTS;
            room(first).min(room(second))
        };
        Ok(
            (room(cursors[0]) > 0 && room(cursors[1]) > 0).then_some(RenderRoom {
                priority,
                producers: [cursors[0][2], cursors[1][2]],
            }),
        )
    }

    /// Copies a render pair's fragment, then tiling record into the slots after the producers
    /// that `room` observed, without exposing them, as `InitData::publish_work_pair` does for a
    /// deferred pair. Returns the producers that expose them.
    pub(super) fn write_render_pair(
        &self,
        _guard: &MutexGuard<'_, ()>,
        room: &RenderRoom,
        fragment: &WorkSlot,
        tiling: &WorkSlot,
        commit_accounting: impl FnOnce() -> Result,
        publish_late_tiling: impl FnOnce(),
    ) -> Result<[u32; 2]> {
        if fragment.data_master != DataMaster::Fragment as u32
            || tiling.data_master != DataMaster::Tiling as u32
        {
            return Err(EINVAL);
        }
        let (fragment_ring, tiling_ring) = self.render_rings(room.priority)?;
        let slot = |ring: &RingView, producer: u32| -> Result<(*mut u8, u32)> {
            if producer >= WORK_RING_SLOTS {
                return Err(EINVAL);
            }
            // SAFETY: The ring view spans WORK_RING_SLOTS records; `producer` is in range.
            let slot = unsafe {
                ring.slots
                    .add(producer as usize * core::mem::size_of::<WorkSlot>())
            };
            Ok((slot, (producer + 1) % WORK_RING_SLOTS))
        };
        let (fragment_slot, fragment_next) = slot(&fragment_ring, room.producers[0])?;
        let (tiling_slot, tiling_next) = slot(&tiling_ring, room.producers[1])?;
        let copy = |record: &WorkSlot, destination: *mut u8| {
            // SAFETY: The room proves that the slot after the producer is host-owned until the
            // producer moves past it, and the render lock excludes every other host writer.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    core::ptr::from_ref(record).cast::<u8>(),
                    destination,
                    core::mem::size_of::<WorkSlot>(),
                )
            }
        };
        copy(fragment, fragment_slot);
        commit_accounting()?;
        fence(Ordering::SeqCst);
        publish_late_tiling();
        copy(tiling, tiling_slot);
        fence(Ordering::SeqCst);
        Ok([fragment_next, tiling_next])
    }

    /// Exposes a pair written by `write_render_pair` with `room`, fragment first, as
    /// `InitData::publish_retained_work_pair` does: `next` must follow the observed producers.
    pub(super) fn expose_render_pair(
        &self,
        _guard: &MutexGuard<'_, ()>,
        room: &RenderRoom,
        next: [u32; 2],
    ) -> Result {
        let (fragment_ring, tiling_ring) = self.render_rings(room.priority)?;
        for (producer, next) in room.producers.into_iter().zip(next) {
            if next != (producer + 1) % WORK_RING_SLOTS {
                return Err(EBUSY);
            }
        }
        fence(Ordering::SeqCst);
        // SAFETY: As for `RingView::cursors`.
        unsafe { (*fragment_ring.producer).store(next[0], Ordering::Relaxed) };
        fence(Ordering::SeqCst);
        // SAFETY: As above.
        unsafe { (*tiling_ring.producer).store(next[1], Ordering::Relaxed) };
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// The completion stamp of `qid` in page A.
    pub(super) fn stamp(&self, qid: u8) -> Result<u32> {
        if qid > super::fw::kick::QID_MAX {
            return Err(EINVAL);
        }
        let base = self.views()?.stamps;
        // SAFETY: Page A holds one word per QID up to QID_MAX; the session retains it.
        let value = unsafe { (*base.add(usize::from(qid))).load(Ordering::Relaxed) };
        fence(Ordering::Acquire);
        Ok(value)
    }

    fn qos_view(&self) -> Result<qos::View<'_>> {
        // SAFETY: The session retains the QoS table for at least as long as this port.
        unsafe { qos::View::from_raw(self.views()?.qos) }
    }

    pub(super) fn qos_publish(
        &self,
        owner: qos::Owner,
        scheduler: u64,
        policy: Policy,
    ) -> Result<qos::Publication> {
        let view = self.qos_view()?;
        let publication = {
            let mut qos = self.qos.lock();
            let publication =
                qos.publish(&view, owner, scheduler, policy, qos::clock, super::now_ns())?;
            self.oldest_in_flight_any
                .store(qos.oldest_in_flight_any(), Ordering::Relaxed);
            publication
        };
        self.rx_update();
        Ok(publication)
    }

    /// Publishes the two owners of a render command, fragment then tiling.
    pub(super) fn qos_publish_pair(
        &self,
        owners: [qos::Owner; 2],
        scheduler: u64,
        policy: Policy,
    ) -> Result<[qos::Publication; 2]> {
        let view = self.qos_view()?;
        let publications = {
            let mut qos = self.qos.lock();
            let publications = qos.publish_owners(
                &view,
                owners,
                scheduler,
                policy,
                qos::clock,
                super::now_ns(),
            )?;
            self.oldest_in_flight_any
                .store(qos.oldest_in_flight_any(), Ordering::Relaxed);
            publications
        };
        self.rx_update();
        Ok(publications)
    }

    pub(super) fn qos_cancel(&self, publication: qos::Publication) -> Result {
        let view = self.qos_view()?;
        let mut qos = self.qos.lock();
        qos.cancel(&view, publication)?;
        self.oldest_in_flight_any
            .store(qos.oldest_in_flight_any(), Ordering::Relaxed);
        Ok(())
    }

    /// Clears the QoS table rows of released QoS IDs (`QosIds::forget_released`); returns
    /// those left because work still runs on them.
    pub(super) fn qos_forget(&self, ids: u128) -> Result<u128> {
        if ids == 0 {
            return Ok(0);
        }
        let view = self.qos_view()?;
        Ok(self.qos.lock().forget(&view, ids))
    }

    pub(super) fn qos_complete(&self, owners: &[qos::Owner]) -> Result {
        let view = self.qos_view()?;
        {
            let mut qos = self.qos.lock();
            qos.complete(&view, owners, super::now_ns())?;
            self.oldest_in_flight_any
                .store(qos.oldest_in_flight_any(), Ordering::Relaxed);
        }
        self.rx_update();
        Ok(())
    }

    /// Requests the receive thread's minimum utilization for the feed's current view of the GPU,
    /// where the port clamps it (not on M5, see attach()). Called at every publication and
    /// completion: the feed's state lingers for a few milliseconds, so after a pause the
    /// publication that ends it restores the default before its completion arrives.
    fn rx_update(&self) {
        let Some(rx) = self.rx.as_ref() else {
            return;
        };
        let want_state = if self.feed.gpu_bound() {
            RX_STATE_GPU_BOUND
        } else {
            RX_STATE_DEFAULT
        };
        if self.rx_state.load(Ordering::SeqCst) == want_state {
            return;
        }
        let mut applied = self.rx_applied.lock();
        // Publish the transitioning state before re-reading `feed.gpu_bound()`,
        // so a concurrent caller observing a newer feed state cannot return
        // early against the stale mirrored value while this update is in flight.
        self.rx_state.store(RX_STATE_UNSET, Ordering::SeqCst);
        let gpu_bound = self.feed.gpu_bound();
        let want = gpu_bound.then_some(RX_UTIL_MIN_GPU_BOUND);
        let want_state = if gpu_bound {
            RX_STATE_GPU_BOUND
        } else {
            RX_STATE_DEFAULT
        };
        if *applied != Some(want) {
            if rx.set(want).is_ok() {
                *applied = Some(want);
                self.rx_state.store(want_state, Ordering::Release);
            }
        } else {
            self.rx_state.store(want_state, Ordering::Release);
        }
    }

    /// Rings the primary coprocessor's doorbell.
    pub(super) fn notify(&self, message: u64) -> Result {
        // SAFETY: The session's primary coprocessor owns the mailbox and outlives the port.
        let mailbox = unsafe { &*self.views()?.mailbox };
        super::trace::doorbell(super::trace::COMPUTE, || {
            self.regs.as_ref().map_or(0, |regs| regs.g17_core_power())
        });
        super::trace::message(super::trace::MAILBOX, message);
        mailbox.send(super::EP_DOORBELL, message)
    }

    pub(super) fn recovery_state(&self) -> Result<u32> {
        let word = self.views()?.recovery;
        fence(Ordering::Acquire);
        // SAFETY: As for `RingView::cursors`.
        Ok(unsafe { (*word).load(Ordering::Relaxed) })
    }

    pub(super) fn generation(&self) -> u64 {
        self.generation.load(Ordering::Acquire)
    }

    /// Called with the device mutex held after the accepted generation changed, before
    /// publication reopens.
    pub(super) fn set_generation(&self, generation: u64) {
        self.generation.store(generation, Ordering::Release);
    }

    /// Unique reservation order shared by both engines. Concurrent publishers may expose
    /// their work in a different order; queue serialization and visibility use other locks.
    /// The watchdog uses this number only to choose bounded cross-VM timeout grace.
    pub(super) fn next_publication(&self) -> u64 {
        self.publications.fetch_add(1, Ordering::Relaxed) + 1
    }

    /// A publisher without the device mutex counts itself before it checks that full effort is
    /// applied. The idle clear withdraws the flag before it rechecks the count, so either the
    /// clear sees this submission or this submission sees the flag withdrawn.
    pub(super) fn note_submission(&self) -> bool {
        self.submissions.fetch_add(1, Ordering::SeqCst);
        self.note_gpu_activity();
        self.effort_full.load(Ordering::SeqCst)
    }

    pub(super) fn submissions(&self) -> u64 {
        self.submissions.load(Ordering::SeqCst)
    }

    fn resume_busy_if_inactive(&self, now_ns: u64) {
        if self.busy_inactive_since.load(Ordering::Acquire) == 0 {
            return;
        }
        let now_ns = super::now_ns().max(now_ns);
        let mut busy = self.busy.lock();
        if busy.resume(now_ns) {
            self.demand.clear_busy();
            self.busy_window_start.store(0, Ordering::Relaxed);
            self.busy_below_top.store(false, Ordering::Relaxed);
        }
        self.busy_inactive_since.store(0, Ordering::Release);
    }

    pub(super) fn set_effort_full(&self, full: bool) {
        self.effort_full.store(full, Ordering::SeqCst);
        if full {
            self.resume_busy_if_inactive(super::now_ns());
            self.demand.set_active(true);
        } else {
            let now = super::now_ns();
            let mut busy = self.busy.lock();
            self.demand.set_active(false);
            if self.demand.has_provider() {
                busy.note_inactive(now);
                self.busy_inactive_since.store(
                    busy.inactive_since().map_or(0, |s| s.max(1)),
                    Ordering::Release,
                );
            }
        }
    }

    pub(super) fn clear_effort_and_busy(&self) {
        self.effort_full.store(false, Ordering::SeqCst);
        let mut busy = self.busy.lock();
        self.demand.set_active(false);
        self.demand.clear_busy();
        busy.clear_active();
        self.busy_window_start.store(0, Ordering::Relaxed);
        self.busy_below_top.store(false, Ordering::Relaxed);
        self.busy_inactive_since.store(0, Ordering::Release);
    }

    /// The next statistics drain and reader discard what the decoder holds.
    pub(super) fn statistics_epoch_bump(&self) {
        self.statistics_epoch.fetch_add(1, Ordering::Release);
    }

    pub(super) fn statistics_epoch(&self) -> u64 {
        self.statistics_epoch.load(Ordering::Acquire)
    }

    /// Reports the busiest GPU engine's share of the last window (including unretired
    /// in-flight commands) to CLPC once a window elapses or an in-flight command
    /// saturates its engine. Lock-free on the common in-window path.
    pub(super) fn report_busy(&self, now_ns: u64) {
        if !self.demand.has_provider() {
            return;
        }
        let start = self.busy_window_start.load(Ordering::Relaxed);
        let window_due =
            start == 0 || now_ns.saturating_sub(start) >= super::power::BUSY_WINDOW_NS;
        let long_due = !window_due
            && self.busy_below_top.load(Ordering::Relaxed)
            && {
                let oldest = self.oldest_in_flight_any.load(Ordering::Relaxed);
                oldest != 0
                    && now_ns.saturating_sub(oldest) >= super::power::BUSY_LONG_COMMAND_NS
            };
        if !window_due && !long_due {
            return;
        }
        // Read retired time (Acquire) before qos (lock Acquire): retirement calls
        // qos_complete() before usage::Client::add() Release-adds ENGINE_NS, so we
        // never observe both the retired time and the old in-flight timestamp.
        let retired = crate::usage::ENGINE_NS
            .each_ref()
            .map(|ns| ns.load(Ordering::Acquire));
        let oldest = self.qos.lock().oldest_in_flight();
        let now_ns = super::now_ns().max(now_ns);
        let mut busy = self.busy.lock();
        if busy.inactive_since().is_some() {
            if busy.resume(now_ns) {
                self.demand.clear_busy();
            }
            self.busy_inactive_since.store(0, Ordering::Release);
        }
        if let Some(permille) = busy.sample(now_ns, retired, oldest) {
            self.demand.set_busy(permille);
        }
        self.busy_window_start
            .store(busy.window_start().map_or(0, |s| s.max(1)), Ordering::Relaxed);
        self.busy_below_top
            .store(busy.below_top(), Ordering::Relaxed);
    }

    /// Lifts CLPC to the top memory/fabric column immediately when a sustained busy run
    /// ([`super::power::BusyRuns::bound`]) begins while the last reported share is below
    /// the top column. Lock-free once at the top column.
    pub(super) fn lift_busy_if_below_top(&self, now_ns: u64) {
        if !self.demand.has_provider() || !self.busy_below_top.load(Ordering::Relaxed) {
            return;
        }
        let retired = crate::usage::ENGINE_NS
            .each_ref()
            .map(|ns| ns.load(Ordering::Acquire));
        let oldest = self.qos.lock().oldest_in_flight();
        let now_ns = super::now_ns().max(now_ns);
        let mut busy = self.busy.lock();
        if let Some(permille) = busy.lift_saturated(now_ns, retired, oldest) {
            self.demand.set_busy(permille);
            self.busy_inactive_since.store(0, Ordering::Release);
        }
        self.busy_window_start
            .store(busy.window_start().map_or(0, |s| s.max(1)), Ordering::Relaxed);
        self.busy_below_top
            .store(busy.below_top(), Ordering::Relaxed);
    }

    /// Wake delay (ns) for `idle_check` while a solo long command is running below
    /// the top CLPC busy column, or `None` when already at top or idle.
    pub(super) fn busy_recheck_delay(&self, now_ns: u64) -> Option<u64> {
        if !self.demand.has_provider() || !self.busy_below_top.load(Ordering::Relaxed) {
            return None;
        }
        let oldest = self.oldest_in_flight_any.load(Ordering::Relaxed);
        super::power::BusyShare::recheck_delay_for(now_ns, oldest)
    }

    pub(super) fn note_gpu_activity(&self) {
        let now = super::now_ns();
        // The firmware gates the cores only after they idled for its idle-off delay, which
        // follows the last job, so a publication within that delay of the previous one cannot
        // follow a gating: skip the power register read (a few hundred ns) on back-to-back work.
        // A direct publisher notes itself before it may fall back to its scheduler; a fallback
        // published past the delay can then miss a gating, costing one statistics sample.
        let idle_off_ns = u64::from(super::power::FIRMWARE_IDLE_OFF_MS) * 1_000_000;
        let previous = self.last_submission_ns.load(Ordering::Relaxed);
        let recent = now.saturating_sub(previous) < idle_off_ns;
        // Otherwise observe gating before activity can wake the cores. A lockless
        // publisher cannot clear the decoder under the device mutex; this
        // epoch makes both the next drain and the next reader discard it.
        if !recent
            && self
                .regs
                .as_ref()
                .is_some_and(|regs| regs.g17_core_power() & 0xf == 0)
        {
            self.statistics_epoch.fetch_add(1, Ordering::Release);
        }
        self.last_submission_ns.store(now, Ordering::Relaxed);
        self.resume_busy_if_inactive(now);
        self.demand.set_active(true);
        self.report_busy(now);
    }

    pub(super) fn write_power(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(
            f,
            "effort_full={} direct_submissions={} last_submission_ns={}",
            self.effort_full.load(Ordering::SeqCst),
            self.submissions(),
            self.last_submission_ns.load(Ordering::Relaxed)
        )
    }
}
