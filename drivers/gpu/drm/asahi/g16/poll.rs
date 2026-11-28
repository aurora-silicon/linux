// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The poll work: drain, retire, growth, hang checks, publication; and the repoll timers.
//!
//! Order of one pass, under the device lock: drain the receive rings; authenticate recovery;
//! retire flights using stage stamps, NC queue READ/WRITE and submit-pipe frontiers, normal
//! notifier/UMA receipts and internal timestamps; renew the progress deadline; answer PB
//! growth ([`Firmware::answer_growth`]); publish ([`Firmware::publish_checked`]). Then drop
//! the lock and complete retired jobs ([`super::job::complete_all`]) before periodic 1 ms
//! housekeeping (`session.check`, `objects.health`, `service_power`, administrative hang
//! checks). If an EP 0x20 event arrived with flights outstanding and nothing retired yet, up
//! to [`EVENT_RETIRE_POLLS`] trailing 5 us waits run with `Shared::state` dropped so concurrent
//! submissions are not blocked, stopping as soon as any flight retires. Wakeups: the EP 0x20
//! event kick, then an hrtimer re-poll every `g16_resnap_us` (100) while anything is
//! published, or `g16_repoll_us` (8) while a ready prefix waits for record consumption or a
//! post-event receipt tail did not settle, with a 1-jiffy delayed-work backup.

use core::sync::atomic::Ordering;

use kernel::{
    bindings, c_str, debugfs, fmt, impl_has_hr_timer,
    prelude::*,
    sync::{Arc, ArcBorrow},
    time::{
        delay,
        hrtimer::{
            HrTimerCallback, HrTimerCallbackContext, HrTimerPointer, HrTimerRestart, RelativeMode,
        },
        Delta, Jiffies, Monotonic,
    },
    workqueue::RawWorkItem,
};

use super::channel::Channel;
use super::engine::{complete, now_ns, rmb, Flight, Lap};
use super::fw::graph::{manager, notifier, notifier_page, qstate};
use super::job::{self, Completions, Job};
use super::recovery::logic::{Retirement, ABORT_GRACE_NS};
use super::regs::EngineState;
use super::{params, Firmware, Shared, FAST_POLL_WORK, POLL_WORK, SCRUB_WORK, UNBOUND_POLL_WORK};
use crate::module_parameters;

/// Cadence of periodic housekeeping when idle or entering recovery (1 ms).
const HOUSEKEEPING_NS: u64 = 1_000_000;
/// Cadence of periodic housekeeping while flights are active (5 ms, aligned with
/// `power::policy::SAMPLE_NS` and far below the 100 ms minimum hang deadline).
const ACTIVE_HOUSEKEEPING_NS: u64 = 5_000_000;
/// Minimum interval between auxiliary log/ktrace/stats ring drains while flights are active.
const AUX_DRAIN_NS: u64 = 10_000_000;
/// Maximum trailing receipt polls after an EP 0x20 event (stepped 1/2/3/4/5 us, 45 us total).
const EVENT_RETIRE_POLLS: u32 = 11;
/// Maximum consecutive in-place EP 0x20 rechecks in `run_poll()` before yielding back to
/// the workqueue so short bursts avoid extra worker round-trips without starving housekeeping.
const MAX_EVENT_RECHECKS: u32 = 8;
/// Minimum interval between 1-jiffy delayed-work safety backup rearms (2 ms).
const BACKUP_REARM_NS: u64 = 2_000_000;

impl_has_hr_timer! {
    impl HasHrTimer<Self> for Shared {
        mode: RelativeMode<Monotonic>, field: self.resnap
    }
}

impl HrTimerCallback for Shared {
    type Pointer<'a> = Arc<Self>;

    /// Runs the poll now.
    fn run(this: ArcBorrow<'_, Self>, _ctx: HrTimerCallbackContext<'_, Self>) -> HrTimerRestart {
        let this: Arc<Self> = this.into();
        this.resnap_deadline_ns.store(0, Ordering::Release);
        this.poll_now();
        HrTimerRestart::NoRestart
    }
}

/// The `aurora-g16/stats` debugfs file contents.
fn stats_show(shared: &Arc<Shared>, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    let state = shared.state.lock();
    match state.as_deref() {
        Some(firmware) => firmware.write_stats(f),
        None => Ok(()),
    }
}

impl Shared {
    /// Schedules a delayed poll while idle so DVFS utilization decays to S1 and a held CLPC
    /// memory/fabric floor expires cleanly after all flights drain.
    fn arm_idle_power_poll(self: &Arc<Self>, firmware: &Firmware) {
        if firmware.recovery.failed().is_some() {
            return;
        }
        let delay_ns = if firmware.power.clpc_holding() {
            super::power::policy::CLPC_IDLE_HOLD_NS
        } else if firmware.power.utilization.decaying() {
            super::power::policy::WINDOW_NS
        } else {
            return;
        };
        self.mod_poll(kernel::time::msecs_to_jiffies((delay_ns / 1_000_000) as u32).max(1));
    }

    /// Queues the backup poll for idle housekeeping that a pass skipped inside its cadence, at
    /// the end of that cadence. It may replace a later idle power poll: that pass runs the
    /// housekeeping, including `service_power()`, and arms the power poll again.
    fn queue_owed_housekeeping(self: &Arc<Self>, firmware: &Firmware) {
        if !firmware.engine.housekeeping_owed || firmware.recovery.failed().is_some() {
            return;
        }
        let elapsed = now_ns().wrapping_sub(firmware.last_housekeeping_ns);
        let ms = HOUSEKEEPING_NS.saturating_sub(elapsed).div_ceil(1_000_000) as u32;
        self.mod_poll(kernel::time::msecs_to_jiffies(ms).max(1));
    }

    /// One poll pass (the poll work item).
    pub(crate) fn run_poll(self: Arc<Self>) {
        // The domain may be off. The sleep's end queues a pass.
        if self.sleeping.load(Ordering::Acquire) {
            return;
        }
        let signalling = kernel::dma_fence::SignallingGuard::new();
        let mut evented = false;
        let mut tail = 0u32;
        let mut housekeeping_deferred = false;
        let mut step = 0u32;
        let mut rechecks = 0u32;

        loop {
            if self.sleeping.load(Ordering::Acquire) {
                break;
            }
            let mut done = Completions::new();
            let mut scrubs = super::slots::ScrubBatch::new();
            let mut retry_tail = false;
            let mut poll_ok = true;
            if let Some(firmware) = self.state.lock().as_deref_mut() {
                if self.sleeping.load(Ordering::Acquire) {
                    return;
                }
                if firmware.engine.shared.is_none() {
                    firmware.engine.shared = Some(self.clone());
                }
                if step == 0 {
                    evented = self.evented.swap(false, Ordering::AcqRel);
                    tail = if evented { EVENT_RETIRE_POLLS } else { 0 };
                }
                match firmware.poll_step(
                    evented,
                    tail,
                    &mut retry_tail,
                    &mut housekeeping_deferred,
                    &mut done,
                ) {
                    Ok(true) if retry_tail => {}
                    Ok(true) => {
                        let soon = firmware.engine.chan_wait || firmware.engine.short_repoll;
                        if firmware.engine.coalesce_deadline_ns != 0 && !soon {
                            let rem_us = (firmware
                                .engine
                                .coalesce_deadline_ns
                                .saturating_sub(now_ns())
                                .div_ceil(1000) as u32)
                                .clamp(1, super::hw::sizes::COALESCE_WINDOW_US);
                            self.rearm_after(rem_us.min(self.rearm_us(false)));
                        } else {
                            self.rearm(soon);
                        }
                    }
                    // Idle: arm a delayed poll while DVFS utilization is decaying or a CLPC
                    // floor hold is still pending so both expire cleanly to idle power.
                    Ok(false) => {
                        self.arm_idle_power_poll(firmware);
                        self.queue_owed_housekeeping(firmware);
                        if firmware.recovery.failed().is_some() {
                            self.cancel_resnap();
                        } else {
                            self.idle_resnap();
                        }
                    }
                    Err(error) => {
                        poll_ok = false;
                        self.cancel_resnap();
                        firmware.fail_stop(error, &mut done);
                        self.fail_stop(error);
                    }
                }
                let allow_stage = firmware.recovery.failed().is_none()
                    && !firmware.engine.frozen
                    && !self.sleeping.load(Ordering::Acquire);
                scrubs = firmware.slots.stage_and_take_scrubs(allow_stage);
                self.sync_active_flights(firmware);
            }
            job::complete_all(done);
            self.enqueue_scrubs(scrubs);
            self.queue_feed();
            if !retry_tail {
                if poll_ok
                    && !self.sleeping.load(Ordering::Acquire)
                    && !self.crashed.load(Ordering::Acquire)
                    && self.evented.load(Ordering::Acquire)
                {
                    if rechecks < MAX_EVENT_RECHECKS {
                        rechecks += 1;
                        step = 0;
                        evented = false;
                        tail = 0;
                        continue;
                    }
                    self.poll_now();
                }
                break;
            }
            let delay_us = match step {
                0 => 1,
                1 => 2,
                2 => 3,
                3 => 4,
                _ => 5,
            };
            tail -= 1;
            step += 1;
            delay::udelay(Delta::from_micros(delay_us));
        }

        if housekeeping_deferred && !self.sleeping.load(Ordering::Acquire) {
            let mut done = Completions::new();
            if let Some(firmware) = self.state.lock().as_deref_mut() {
                if !self.sleeping.load(Ordering::Acquire) {
                    if let Err(error) = firmware.run_housekeeping(&mut done) {
                        self.cancel_resnap();
                        firmware.fail_stop(error, &mut done);
                        self.fail_stop(error);
                    } else if firmware.recovery.needs_repoll() || firmware.engine.frozen {
                        self.rearm(false);
                    } else if firmware.engine.flights.is_empty() {
                        self.arm_idle_power_poll(firmware);
                    }
                    self.sync_active_flights(firmware);
                }
            }
            job::complete_all(done);
        }
        drop(signalling);
        self.notify_wedged();
    }

    fn submit_locked(
        self: &Arc<Self>,
        firmware: &mut Firmware,
        job: Arc<Job>,
        done: &mut Completions,
    ) -> Result {
        if firmware.engine.shared.is_none() {
            firmware.engine.shared = Some(self.clone());
        }
        let accepted = firmware.enqueue(job);
        if accepted.is_ok() && !self.sleeping.load(Ordering::Acquire) {
            let now = now_ns();
            let had_flights = !firmware.engine.flights.is_empty();
            let housekeeping_interval = if had_flights {
                ACTIVE_HOUSEKEEPING_NS
            } else {
                HOUSEKEEPING_NS
            };
            let housekeeping_due =
                now.wrapping_sub(firmware.last_housekeeping_ns) >= housekeeping_interval;
            let was_evented = had_flights && self.evented.swap(false, Ordering::AcqRel);
            let recovery_pending = firmware.recovery_pending().unwrap_or(true);
            let mut retired_any = false;
            let mut restored_event = false;
            let mut pub_res = Ok(());
            if had_flights
                && (was_evented || recovery_pending || firmware.oldest_stamp_reached())
            {
                let stage_before = firmware.events.stage_events;
                let term_before = firmware.oldest_terminal_events();
                pub_res = firmware.session.transport().and_then(|()| {
                    retired_any = firmware.retire_once(done)?;
                    firmware.answer_growth()?;
                    if firmware.recovery.receipt_fault || recovery_pending {
                        firmware.check_hangs(done)?;
                    }
                    if was_evented && !retired_any && !firmware.engine.flights.is_empty() {
                        let non_terminal_only = term_before.is_some()
                            && firmware.events.stage_events != stage_before
                            && firmware.oldest_terminal_events() == term_before
                            && !firmware.oldest_stamp_reached()
                            && !firmware.recovery.receipt_fault
                            && !recovery_pending;
                        if !non_terminal_only {
                            self.evented.store(true, Ordering::Release);
                            restored_event = true;
                        }
                    }
                    Ok(())
                });
            }
            let mut coalesce_us = None;
            if pub_res.is_ok() {
                if retired_any || !had_flights {
                    firmware.reclaim_closed_vm_slots();
                }
                if !recovery_pending && !retired_any && !restored_event && had_flights {
                    coalesce_us = firmware.defer_busy_coalesce(now);
                }
                if coalesce_us.is_none() {
                    pub_res = if recovery_pending {
                        let r = firmware.publish(done);
                        if r.is_ok() {
                            firmware.last_housekeeping_ns = now;
                            firmware.last_aux_drain_ns = now;
                        }
                        r
                    } else {
                        firmware.publish_checked(done)
                    };
                }
            }
            if pub_res.is_ok()
                && !firmware.engine.flights.is_empty()
                && firmware.power.needs_ramp(now)
            {
                pub_res = firmware.service_power();
            }
            if let Err(error) = pub_res {
                firmware.fail_stop(error, done);
                self.fail_stop(error);
            }
            if !firmware.engine.flights.is_empty() || firmware.engine.chan_wait {
                // If an EP 0x20 event retired an older flight while later flights remain active,
                // or arrived before the oldest receipt became visible, arm a short repoll so
                // trailing completions never fall back to `resnap_us`. A ready job this
                // publication left waiting on slot headroom repolls as soon as
                // `poll_step()` would rearm it, not a full `resnap_us` later.
                if let Some(us) = coalesce_us {
                    self.rearm_after(us.min(self.rearm_us(false)));
                } else {
                    let paced = !firmware.engine.frozen
                        && !firmware.engine.pending.is_empty()
                        && (firmware.engine.slot_wait || firmware.engine.held_wait);
                    self.rearm(
                        firmware.engine.chan_wait
                            || restored_event
                            || paced
                            || (was_evented && retired_any && !firmware.engine.flights.is_empty()),
                    );
                }
            } else {
                firmware.power.report_work(false);
                self.arm_idle_power_poll(firmware);
            }
            if pub_res.is_ok()
                && (restored_event || (housekeeping_due && !recovery_pending))
            {
                self.poll_now();
            }
        }
        accepted
    }

    /// Hands a job to the engine and publishes what can be published. Err: the job was not
    /// accepted (the engine failed: EIO) and the caller fails it; once accepted, its fence is
    /// signalled by the engine, also on failure.
    pub(crate) fn submit(self: &Arc<Self>, job: Arc<Job>) -> Result {
        let signalling = kernel::dma_fence::SignallingGuard::new();
        let mut done = Completions::new();
        let (accepted, scrubs) = {
            let mut state = self.state.lock();
            let firmware = state.as_deref_mut().ok_or(ENODEV)?;
            let res = self.submit_locked(firmware, job, &mut done);
            let allow_stage = firmware.recovery.failed().is_none()
                && !firmware.engine.frozen
                && !self.sleeping.load(Ordering::Acquire);
            self.sync_active_flights(firmware);
            (res, firmware.slots.stage_and_take_scrubs(allow_stage))
        };
        job::complete_all(done);
        self.enqueue_scrubs(scrubs);
        self.queue_feed();
        drop(signalling);
        self.notify_wedged();
        accepted
    }

    /// Inline SUBMIT ioctl publication: acquires `Shared::state` directly when the GPU is
    /// neither sleeping nor recovering so brief poll-lock overlap does not bounce ready
    /// submissions onto `drm_sched` kworker wakeups. Returns `None` during sleep or recovery
    /// so `drm_sched` handles those paths without stalling SUBMIT.
    pub(crate) fn try_submit_inline(self: &Arc<Self>, job: Arc<Job>) -> Option<Result> {
        if self.sleeping.load(Ordering::Acquire) || self.recovering.load(Ordering::Acquire) {
            return None;
        }
        let signalling = kernel::dma_fence::SignallingGuard::new();
        let mut done = Completions::new();
        let mut scrubs = super::slots::ScrubBatch::new();
        let accepted = {
            let mut state = self.state.lock();
            if self.sleeping.load(Ordering::Acquire) {
                return None;
            }
            match state.as_deref_mut() {
                Some(firmware) => {
                    let res = self.submit_locked(firmware, job, &mut done);
                    let allow_stage = firmware.recovery.failed().is_none()
                        && !firmware.engine.frozen
                        && !self.sleeping.load(Ordering::Acquire);
                    scrubs = firmware.slots.stage_and_take_scrubs(allow_stage);
                    self.sync_active_flights(firmware);
                    res
                }
                None => Err(ENODEV),
            }
        };
        job::complete_all(done);
        self.enqueue_scrubs(scrubs);
        self.queue_feed();
        drop(signalling);
        self.notify_wedged();
        Some(accepted)
    }

    /// Hands staged displaced pass scrubs to the dedicated unbound scrub workqueue so SUBMIT
    /// and completion polling never stall on write-combining zeroing.
    pub(crate) fn enqueue_scrubs(self: &Arc<Self>, batch: super::slots::ScrubBatch) {
        if batch.is_empty() {
            return;
        }
        let mut queue = self.scrub_queue.lock();
        if self.sleeping.load(Ordering::Acquire) {
            drop(queue);
            drop(batch);
            return;
        }
        if queue.push_batch(batch) {
            // Queue under `scrub_queue` so `sleep.rs::join_workers()` cannot overlap
            // `scrub_wq.drain()` with a concurrent enqueue once `sleeping` is set.
            let _ = self
                .scrub_wq
                .enqueue::<Arc<Shared>, SCRUB_WORK>(self.clone());
        }
    }

    /// Drains queued displaced pass scrubs on the dedicated unbound scrub workqueue.
    pub(crate) fn run_scrubs(self: &Arc<Self>) {
        loop {
            let Some(mut item) = self.scrub_queue.lock().pop() else {
                break;
            };
            if self.sleeping.load(Ordering::Acquire) {
                drop(item);
                break;
            }
            if item.run_one() && !self.sleeping.load(Ordering::Acquire) {
                self.poll_now();
            }
        }
    }

    /// Deliver userspace notification outside the device mutex and fence completion path.
    fn notify_wedged(&self) {
        if self.wedged.load(Ordering::Acquire) && !self.wedged_notified.swap(true, Ordering::AcqRel)
        {
            if let Err(error) = self.drm.report_wedged() {
                dev_warn!(
                    self.dev.as_ref(),
                    "G16: could not deliver wedged uevent ({:?})\n",
                    error
                );
            }
        }
    }

    /// Publishes the active flight depth for lock-free completion worker placement.
    fn sync_active_flights(&self, firmware: &Firmware) {
        self.active_flights
            .store(firmware.engine.flights.len() as u32, Ordering::Release);
    }

    /// Queues fast completion polling: pinned to the submitting CPU for single-flight (`d=1`)
    /// wakeups so the sleeping waiter stays cache-hot, and unbound when pipelined (`d>=2`) so
    /// completion polling does not preempt an actively recording submitter.
    pub(crate) fn poll_now(self: &Arc<Self>) {
        if self.active_flights.load(Ordering::Acquire) >= 2 {
            let _ = self
                .events_unbound
                .enqueue::<Arc<Shared>, UNBOUND_POLL_WORK>(self.clone());
            return;
        }
        let _ = match self.submitter_cpu() {
            Some(cpu) => self
                .events
                .enqueue_on::<Arc<Shared>, FAST_POLL_WORK>(cpu, self.clone()),
            None => self
                .events
                .enqueue::<Arc<Shared>, FAST_POLL_WORK>(self.clone()),
        };
    }

    /// Disarms the completion hrtimer when idle or entering system sleep.
    ///
    /// Always takes `resnap_handle` rather than checking a lock-free shadow flag so
    /// `sleep::join_workers()` strictly orders with any concurrent `rearm()`: `sleep_down()`
    /// stores `sleeping = true` before calling `cancel_resnap()`, and `rearm()` re-checks
    /// `sleeping` under `resnap_handle.lock()` before installing a new handle.
    pub(crate) fn cancel_resnap(&self) {
        let mut handle = self.resnap_handle.lock();
        self.resnap_deadline_ns.store(0, Ordering::Release);
        drop(handle.take());
    }

    /// Relaxes the completion hrtimer on healthy idle transitions without cancelling a
    /// short (`<= resnap_us`) already-armed timer so back-to-back `d=1` bursts avoid
    /// per-job cancel/restart mutex and hrtimer-rbtree churn. System sleep and fail-stop
    /// paths still call [`Self::cancel_resnap`] unconditionally.
    pub(crate) fn idle_resnap(&self) {
        let armed_ns = self.resnap_deadline_ns.load(Ordering::Acquire);
        if armed_ns == 0 {
            return;
        }
        let now = now_ns();
        let keep_ns = u64::from(params::resnap_us()).saturating_mul(1000);
        if armed_ns > now && armed_ns.saturating_sub(now) <= keep_ns {
            return;
        }
        self.cancel_resnap();
    }

    /// Queues the backup poll on `system_highpri_wq` after `delay` jiffies, or moves an already
    /// pending one to that expiry.
    fn mod_poll(self: &Arc<Self>, delay: Jiffies) {
        // SAFETY: The closure returns false only when `mod_delayed_work_on` found the work
        // already pending (it then only modified its timer), so the queue keeps the reference
        // it took when it was queued; otherwise the work is queued and owns this reference.
        let _ = unsafe {
            <Arc<Shared> as RawWorkItem<POLL_WORK>>::__enqueue(self.clone(), |work| {
                let dwork = kernel::container_of!(work, bindings::delayed_work, work);
                !bindings::mod_delayed_work_on(
                    bindings::wq_misc_consts_WORK_CPU_UNBOUND as kernel::ffi::c_int,
                    bindings::system_highpri_wq,
                    dwork,
                    delay,
                )
            })
        };
    }

    /// Computes the hrtimer repoll delay in microseconds: `repoll_us` when `soon`, otherwise
    /// adapts down to `clamp(ewma_flight_us / 2, repoll_us, resnap_us)` on sub-`2 * resnap_us`
    /// bursts while respecting `g16_repoll_us == 0` as a full disable of shortened repolls.
    pub(crate) fn rearm_us(&self, soon: bool) -> u32 {
        let resnap = params::resnap_us();
        if module_parameters::g16_repoll_us.value() == 0 {
            return resnap;
        }
        let repoll = params::repoll_us();
        if soon {
            return repoll;
        }
        let ewma = self.ewma_flight_us.load(Ordering::Relaxed);
        if ewma != 0 && ewma < resnap.saturating_mul(2) {
            (ewma / 2).clamp(repoll, resnap)
        } else {
            resnap
        }
    }

    /// Re-checks a published command soon: hrtimer, with a throttled 1-jiffy backup; sooner while
    /// a ready prefix waits for record consumption or a post-event receipt tail is settling.
    pub(crate) fn rearm(self: &Arc<Self>, soon: bool) {
        self.rearm_after(self.rearm_us(soon));
    }

    /// Arms the poll hrtimer for `us` microseconds (coalescing with an already-armed deadline
    /// within 5 us) plus the throttled 1-jiffy backup poll.
    pub(crate) fn rearm_after(self: &Arc<Self>, us: u32) {
        if self.sleeping.load(Ordering::Acquire) {
            return;
        }
        let now = now_ns();
        let target_ns = now.saturating_add(u64::from(us) * 1000);
        let armed_ns = self.resnap_deadline_ns.load(Ordering::Acquire);
        if armed_ns == 0 || armed_ns <= now || target_ns.saturating_add(5_000) < armed_ns {
            let mut handle = self.resnap_handle.lock();
            if self.sleeping.load(Ordering::Acquire) {
                return;
            }
            let armed_ns = self.resnap_deadline_ns.load(Ordering::Acquire);
            if armed_ns == 0 || armed_ns <= now || target_ns.saturating_add(5_000) < armed_ns {
                drop(handle.take());
                self.resnap_deadline_ns.store(target_ns, Ordering::Release);
                *handle = Some(self.clone().start(Delta::from_micros(i64::from(us))));
            }
        }
        let backup_ns = self.backup_deadline_ns.load(Ordering::Acquire);
        if now >= backup_ns {
            self.backup_deadline_ns
                .store(now.saturating_add(BACKUP_REARM_NS), Ordering::Release);
            self.mod_poll(1);
        }
    }

    /// Starts what the engine needs besides the firmware state: the `aurora-g16/stats` file.
    pub(crate) fn start_engine(self: &Arc<Self>) -> Result {
        let dir = debugfs::Dir::new(c_str!("aurora-g16"));
        let file = KBox::pin_init(
            dir.read_callback_file(c_str!("stats"), self.clone(), &stats_show),
            GFP_KERNEL,
        )?;
        let mut state = self.state.lock();
        let firmware = state.as_deref_mut().ok_or(ENODEV)?;
        firmware.engine.shared = Some(self.clone());
        firmware.engine.debugfs = Some(file);
        Ok(())
    }
}

/// Native modular stage completion, including zero on 32-bit wrap.
pub(crate) fn stamp_reached(obs: u32, own: u32, tail: u32) -> bool {
    super::recovery::logic::stamp_reached(obs, own, tail)
}

/// Cursor `c` of a ring of `m` entries passed end `h` of entries from `b`, within tail `t`.
pub(crate) fn ring_passed(c: u32, b: u32, h: u32, t: u32, m: u32) -> bool {
    super::recovery::logic::ring_passed(c, b, h, t, m)
}

impl Firmware {
    /// Reclaims displaced slot leases belonging to closed VMs so dead processes do not keep
    /// multi-hundred-MiB render/compute arenas and TTBAT ASIDs pinned while idle.
    ///
    /// Respects all hardware displacement contracts (active, in-flight, undisplaced, scrubbing,
    /// and quarantined slots are preserved) and re-arms `Shared::closed_vm_reclaim` whenever a
    /// non-quarantined slot still holds a closed VM awaiting retirement, displacement, or scrub.
    pub(crate) fn reclaim_closed_vm_slots(&mut self) {
        let Some(shared) = self.engine.shared.as_ref() else {
            return;
        };
        let armed = shared.closed_vm_reclaim.load(Ordering::Acquire)
            && shared.closed_vm_reclaim.swap(false, Ordering::AcqRel);
        // A closed VM's render retired and recorded its last epoch after the VM was closed.
        if !self.pools.take_closed_vm_last() && !armed {
            return;
        }
        let remaining = self.pools.reclaim_closed() | self.slots.reclaim_closed();
        if remaining {
            shared.closed_vm_reclaim.store(true, Ordering::Release);
        }
    }

    /// One non-sleeping poll step under the device lock.
    ///
    /// Retires finished flights and publishes ready work before periodic 1 ms housekeeping so
    /// completion fences wake userspace promptly. When an EP 0x20 event arrived with flights
    /// outstanding and nothing retired yet, sets `*retry_tail` so [`Shared::run_poll`] drops
    /// `Shared::state` across the 5 us receipt-tail delay instead of holding the device lock.
    pub(crate) fn poll_step(
        &mut self,
        evented: bool,
        tail: u32,
        retry_tail: &mut bool,
        housekeeping_deferred: &mut bool,
        done: &mut Completions,
    ) -> Result<bool> {
        if self.recovery.failed().is_some() {
            return Ok(false);
        }
        self.engine.short_repoll = false;
        self.session.transport()?;
        let now = now_ns();
        let had_flights = !self.engine.flights.is_empty();
        let housekeeping_interval = if had_flights {
            ACTIVE_HOUSEKEEPING_NS
        } else {
            HOUSEKEEPING_NS
        };
        let housekeeping_due =
            now.wrapping_sub(self.last_housekeeping_ns) >= housekeeping_interval;
        let mut ran_housekeeping = false;
        // With nothing published only a pending firmware recovery notification needs this
        // pass: one that arrived after its prefix's receipts is answered
        // here (the firmware halted and waits for the resume) instead of being taken for a
        // failure of the next prefix. Drain the receive rings before deciding, published or
        // not, so the error count is current and idle log/trace/stats traffic is consumed. With
        // prefixes published the retirement loop below drains first.
        //
        // Inside the housekeeping cadence, with no recovery pending, an idle pass drains only
        // the event and statistics rings, which carry completion and recovery notifications.
        // The session, object, power and auxiliary ring checks then wait for the cadence, so
        // back-to-back shallow submissions do not repeat them, under the device lock, between
        // one completion and the next SUBMIT. `run_poll()` queues a backup poll for them.
        if !had_flights {
            let full = housekeeping_due || self.recovery.receipt_fault || {
                self.drain_events_and_stats()?;
                self.recovery_pending()?
            };
            if full {
                self.session.check()?;
                self.objects.health()?;
                self.service_power()?;
                self.last_housekeeping_ns = now;
                self.last_aux_drain_ns = now;
                ran_housekeeping = true;
                self.engine.housekeeping_owed = false;
                self.drain_rings()?;
            } else {
                self.engine.housekeeping_owed = true;
            }
            self.reclaim_closed_vm_slots();
            if self.recovery_idle()? && self.engine.pending.is_empty() {
                let _ = self.slots.take_scrub_woke();
                if let Some(shared) = self.engine.shared.as_ref() {
                    shared.publish_wake.store(false, Ordering::Release);
                }
                return Ok(false);
            }
        }
        let was_frozen = self.engine.frozen;
        let stage_before = self.events.stage_events;
        let term_before = self.oldest_terminal_events();
        let mut lap = Lap::start();
        let start = lap.on().then(now_ns);
        let retired_any = self.retire_once(done)?;
        lap.lap(&mut self.engine.prof.retire_ns);
        self.answer_growth()?;
        let recovery_pending = self.recovery.receipt_fault || self.recovery_pending()?;
        if recovery_pending {
            self.check_hangs(done)?;
        }
        let unfrozen = was_frozen && !self.engine.frozen;
        let scrub_woke = self.slots.take_scrub_woke();
        let publish_woke = self
            .engine
            .shared
            .as_ref()
            .is_some_and(|shared| shared.publish_wake.swap(false, Ordering::AcqRel));
        if retired_any || publish_woke || scrub_woke || housekeeping_due {
            self.reclaim_closed_vm_slots();
        }
        // Skip redundant `publish_checked()` walks on busy polls where nothing could have
        // unblocked `engine.pending`. Standby release wakes `Cache::prepare` waiters, which
        // enter `submit_locked()` directly. `publish_woke` covers external state changes
        // including a closed VM's final reservation release.
        let coalesce_due = self.engine.coalesce_deadline_ns != 0
            && (!evented || now >= self.engine.coalesce_deadline_ns);
        let should_publish = !self.engine.pending.is_empty()
            && (retired_any
                || !had_flights
                || self.engine.flights.is_empty()
                || self.engine.chan_wait
                || unfrozen
                || scrub_woke
                || publish_woke
                || coalesce_due
                || housekeeping_due);
        let published = if should_publish {
            self.publish_checked(done)
        } else {
            Ok(())
        };
        if self.engine.flights.is_empty() {
            self.power.report_work(false);
        }
        let s = &mut self.engine.stats;
        if self.engine.flights.is_empty() && s.idle_from_ns != 0 {
            // Idle with nothing ready is not a gap: the clients are late.
            s.starve_from_ns = s.idle_from_ns;
            s.idle_from_ns = 0;
        }
        if let Some(start) = start {
            self.engine.prof.polls += 1;
            self.engine.prof.poll_ns += now_ns().wrapping_sub(start);
        }
        published?;
        // A TA-only (0x70) EP 0x20 interrupt while FS is still running does not signal flight
        // retirement: avoid burning the receipt-tail spin or arming a short repoll storm for it,
        // unless a ready job waits behind its own VM's flights (`held_wait`).
        let non_terminal_only = !retired_any
            && term_before.is_some()
            && self.events.stage_events != stage_before
            && self.oldest_terminal_events() == term_before
            && !self.oldest_stamp_reached()
            && !self.recovery.receipt_fault
            && !recovery_pending
            && !self.engine.held_wait;
        // After an EP 0x20 event, retry up to `EVENT_RETIRE_POLLS` times (outside the device
        // lock) only while no flight has retired yet and flights remain in flight. Once any
        // flight retires, stop spinning so deeper queues do not burn tail waits per interrupt.
        if evented
            && had_flights
            && !retired_any
            && !non_terminal_only
            && tail != 0
            && !self.engine.flights.is_empty()
            && !self.recovery.receipt_fault
            && !self.engine.frozen
            && self.recovery.failed().is_none()
        {
            *retry_tail = true;
            return Ok(true);
        }
        if !ran_housekeeping && housekeeping_due {
            if !done.is_empty() || !self.engine.flights.is_empty() {
                *housekeeping_deferred = true;
            } else {
                self.run_housekeeping(done)?;
            }
        }
        // If a terminal EP 0x20 event arrived with flights outstanding and nothing retired within
        // the trailing polls, or a ready same-VM pending job is blocked on slot/flight/work-ring
        // headroom, or held behind its own VM's flights, repoll at `repoll_us` instead of
        // sleeping `resnap_us`.
        self.engine.short_repoll = !self.engine.flights.is_empty()
            && !self.engine.frozen
            && !non_terminal_only
            && ((evented && had_flights && !retired_any)
                || (!self.engine.pending.is_empty()
                    && (self.engine.slot_wait || self.engine.held_wait)));
        Ok(!self.engine.flights.is_empty()
            || self.engine.chan_wait
            || self.recovery.needs_repoll()
            || self.engine.frozen)
    }

    /// Periodic 1 ms housekeeping (session/object health, power governor, auxiliary log/trace
    /// ring drain, administrative hang checks), run after retired completion fences have been
    /// signalled when work retired.
    pub(crate) fn run_housekeeping(&mut self, done: &mut Completions) -> Result {
        if self.recovery.failed().is_some() {
            return Ok(());
        }
        self.session.transport()?;
        self.session.check()?;
        self.objects.health()?;
        self.service_power()?;
        let now = now_ns();
        let recovery_pending = self.recovery.receipt_fault || self.recovery_pending()?;
        if self.engine.flights.is_empty()
            || recovery_pending
            || now.wrapping_sub(self.last_aux_drain_ns) >= AUX_DRAIN_NS
        {
            self.drain_auxiliary_rings()?;
            self.last_aux_drain_ns = now;
        }
        self.last_housekeeping_ns = now;
        self.engine.housekeeping_owed = false;
        self.reclaim_closed_vm_slots();
        if !self.engine.flights.is_empty() || recovery_pending {
            self.check_hangs(done)?;
        }
        Ok(())
    }

    /// Terminal completion-event counters for the oldest active flight of each class
    /// (`(compute_events, fs_events)`), used to distinguish TA-only (0x70) non-terminal events.
    fn oldest_terminal_events(&self) -> Option<(Option<u32>, Option<u32>)> {
        if self.engine.flights.is_empty()
            || self
                .engine
                .flights
                .iter()
                .any(|f| f.computes != 0 && f.renders != 0)
        {
            return None;
        }
        let compute = self
            .engine
            .flights
            .iter()
            .any(|f| f.computes != 0)
            .then_some(self.events.compute_events);
        let render = self
            .engine
            .flights
            .iter()
            .any(|f| f.renders != 0)
            .then_some(self.events.fs_events);
        Some((compute, render))
    }

    /// Cheap single-stage stamp check for the oldest compute pipe 0/1 and oldest render flights'
    /// terminal stages so `submit_locked()` can eagerly retire finished work without scanning
    /// all rings when nothing has completed.
    fn oldest_stamp_reached(&self) -> bool {
        if self.engine.flights.is_empty() {
            return false;
        }
        let mut oldest_compute: [Option<&Flight>; super::hw::sizes::COMPUTE_PIPES] =
            [None; super::hw::sizes::COMPUTE_PIPES];
        let mut compute_behind = [false; super::hw::sizes::COMPUTE_PIPES];
        let mut oldest_render: Option<&Flight> = None;
        let mut render_behind = false;
        for f in self.engine.flights.iter() {
            if f.computes != 0 {
                let p = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
                if oldest_compute[p].is_none() {
                    oldest_compute[p] = Some(f);
                } else {
                    compute_behind[p] = true;
                }
            }
            if f.renders != 0 {
                if oldest_render.is_none() {
                    oldest_render = Some(f);
                } else {
                    render_behind = true;
                }
            }
        }
        rmb();
        for p in 0..super::hw::sizes::COMPUTE_PIPES {
            if let Some(f) = oldest_compute[p] {
                if let Ok(obs) = self.graph.compute[p].fw_stamp.r32(0) {
                    let reached = if compute_behind[p] {
                        stamp_reached(obs, f.target, self.engine.target[p])
                    } else {
                        obs == f.target
                    };
                    if reached {
                        return true;
                    }
                }
            }
        }
        if let Some(f) = oldest_render {
            if let Ok(obs) = self.graph.render.fw_stamp[1].r32(0) {
                let reached = if render_behind {
                    stamp_reached(obs, f.fs_target, self.engine.fs_target)
                } else {
                    obs == f.fs_target
                };
                if reached {
                    return true;
                }
            }
        }
        false
    }

    /// Drains the event/stats rings and retires all currently complete oldest compute and render
    /// flights independently without sleeping. Returns whether at least one flight retired. A
    /// receipt fault is left in `Recovery::receipt_fault` for the hang checks.
    pub(crate) fn retire_once(&mut self, done: &mut Completions) -> Result<bool> {
        let mut lap = Lap::start();
        self.drain_events_and_stats()?;
        lap.lap(&mut self.engine.prof.drain_ns);
        // Recovery may force stage progress: authenticate it before any receipt.
        if self.recovery.receipt_fault || self.recovery_pending()? {
            self.check_hangs(done)?;
        }
        if self.engine.flights.is_empty() {
            return Ok(false);
        }
        let mut retired_any = false;
        let (mut check_compute, mut check_render) = ([true; super::hw::sizes::COMPUTE_PIPES], true);
        let mut ret = self.retired_flight_classes(&mut check_compute, &mut check_render);
        while let Ok(Some((index, receipt))) = ret {
            let outcome = self.retire_outcome_at(index, receipt);
            self.retire_oldest(index, receipt, outcome, done);
            retired_any = true;
            ret = self.retired_flight_classes(&mut check_compute, &mut check_render);
        }
        self.recovery.receipt_fault = ret.is_err();
        Ok(retired_any)
    }

    #[allow(dead_code)]
    fn r32(o: &super::object::FwObject, offset: usize) -> Result<u32> {
        o.r32(offset)
    }

    /// A compute unit's GPU start and end ticks. 25G83 writes them into the unit's ITS pair;
    /// ABIs with `work_ticks` leave ITS 0/0 and write them into the work object. Check
    /// `work_ticks` first when the ABI defines them so 26B123 skips two always-zero WC reads
    /// per command, invalidating the cacheable `FwCoherent` work lines to PoC before reading
    /// (`sync_for_cpu`), and falling back to ITS if `work_ticks` holds no interval.
    fn compute_ticks(&self, unit: &super::slots::ComputeSlot) -> Result<(u64, u64)> {
        Self::compute_ticks_with(self.init.abi.compute().work_ticks, unit)
    }

    fn compute_ticks_with(
        work_ticks: Option<(usize, usize)>,
        unit: &super::slots::ComputeSlot,
    ) -> Result<(u64, u64)> {
        if let Some((start_off, end_off)) = work_ticks {
            let w = &unit.work;
            let lo = start_off.min(end_off);
            let hi = start_off.max(end_off).checked_add(8).ok_or(EINVAL)?;
            w.sync_for_cpu(lo, hi - lo)?;
            // Work tick offsets are 4-byte aligned (not 8-byte), so read hi/lo/hi to avoid tearing.
            let at = |o: usize| -> Result<u64> {
                for _ in 0..4 {
                    let hi1 = w.r32(o + 4)?;
                    let lo = w.r32(o)?;
                    let hi2 = w.r32(o + 4)?;
                    if hi1 == hi2 {
                        return Ok(u64::from(lo) | (u64::from(hi1) << 32));
                    }
                }
                Err(EIO)
            };
            let (start, end) = (at(start_off)?, at(end_off)?);
            if start != 0 && end >= start {
                return Ok((start, end));
            }
        }
        Ok((unit.its.r64(0)?, unit.its.r64(8)?))
    }

    /// Exact queue-ring proof for one serialized publication on its workqueue pipe.
    fn queue_ring_receipt(
        &self,
        state: &super::object::FwObject,
        info: &super::object::FwObject,
        head: u32,
    ) -> Result<bool> {
        if head >= 256 {
            return Err(EINVAL);
        }
        // M4's qualified NC cursors are READ and WRITE. Offset zero is not an M4
        // completion witness, and must not inherit G15's DONE semantics.
        for offset in [qstate::READ, qstate::WRITE] {
            if state.r32(offset)? != head {
                return Ok(false);
            }
        }
        if !super::hw::tuning::FW_CACHED && !super::graph::qinfo_cached() {
            for offset in [0x20, 0x24, 0x28] {
                if info.r32(offset)? != head {
                    return Ok(false);
                }
            }
            if info.r32(self.init.abi.qinfo().busy)? != 0 {
                return Ok(false);
            }
        }
        // With ASC-cached QI, its three consumers and busy flag can remain stale until
        // terminal CIVAC. The working M4 protocol substitutes the NC READ cursor above.
        Ok(true)
    }

    /// Passed queue-ring proof when later flights are queued behind `f` on the same pipe.
    fn queue_ring_passed(
        &self,
        channel: Channel,
        state: &super::object::FwObject,
        info: &super::object::FwObject,
        f: &Flight,
    ) -> Result<bool> {
        let cap = super::hw::sizes::QUEUE_CAP;
        let (start, head, tail) = match channel {
            Channel::Compute => {
                let p = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
                (f.c_start, f.head, self.engine.head[p])
            }
            Channel::Ta => (f.ta_start, f.ta_head, self.engine.ta_head),
            Channel::Fs => (f.fs_start, f.fs_head, self.engine.fs_head),
        };
        if head >= cap || start >= cap || tail >= cap {
            return Err(EINVAL);
        }
        if !ring_passed(state.r32(qstate::READ)?, start, head, tail, cap) {
            return Ok(false);
        }
        if !super::hw::tuning::FW_CACHED && !super::graph::qinfo_cached() {
            for offset in [0x20, 0x24, 0x28] {
                if !ring_passed(info.r32(offset)?, start, head, tail, cap) {
                    return Ok(false);
                }
            }
        }
        Ok(true)
    }

    /// Submit-channel proof for `record`: exact when no later record exists on `channel`,
    /// otherwise passed within the current channel producer frontier.
    fn channel_receipt(&self, channel: Channel, record: u32, passed: bool) -> Result<bool> {
        let slots = super::fw::channels::RING_SLOTS;
        if record >= slots {
            return Err(EINVAL);
        }
        let pipe = self.channels.channel(channel);
        if passed {
            let read = pipe.read()?;
            let comp = pipe.completion()?;
            if read >= slots || comp >= slots {
                return Ok(false);
            }
            let first = (record + slots - 1) % slots;
            let producer = self.channels.producer(channel);
            Ok(ring_passed(read, first, record, producer, slots)
                && ring_passed(comp, first, record, producer, slots))
        } else {
            Ok(pipe.read()? == record && pipe.completion()? == record && pipe.write()? == record)
        }
    }

    /// Latches `Channel::Compute` read and completion proofs onto active compute flights before
    /// the shared submit ring can lap them, so a long flight on one compute pipe stays retirable
    /// even after other pipes wrap the 256-entry submit channel.
    pub(crate) fn latch_compute_channel_receipts(&mut self) -> Result {
        if !self
            .engine
            .flights
            .iter()
            .any(|f| f.computes != 0 && !f.chan_settled)
        {
            return Ok(());
        }
        let slots = super::fw::channels::RING_SLOTS;
        let pipe = self.channels.channel(Channel::Compute);
        let read = pipe.read()?;
        let comp = pipe.completion()?;
        if read >= slots || comp >= slots {
            return Ok(());
        }
        let producer = self.channels.producer(Channel::Compute);
        for f in self.engine.flights.iter_mut() {
            if f.computes == 0 || f.chan_settled {
                continue;
            }
            let record = f.rec[2];
            if record >= slots {
                return Err(EINVAL);
            }
            let first = (record + slots - 1) % slots;
            if ring_passed(read, first, record, producer, slots)
                && ring_passed(comp, first, record, producer, slots)
            {
                f.chan_settled = true;
            }
        }
        Ok(())
    }

    /// Exact M4 queue and submit-pipe proof for one serialized publication. Event
    /// notifications are wakeups; neither an event nor elapsed time releases an owner.
    pub(crate) fn queue_receipt(
        &self,
        channel: Channel,
        state: &super::object::FwObject,
        info: &super::object::FwObject,
        head: u32,
        record: u32,
    ) -> Result<bool> {
        Ok(self.queue_ring_receipt(state, info, head)?
            && self.channel_receipt(channel, record, false)?)
    }

    /// In-order receipt of flight `f` with later flights queued behind it: the NC READ cursor
    /// (and with uncached queue info its consumers) passed its ring entries, and the submit
    /// pipe's read and completion cursors passed its record.
    fn queue_passed(
        &self,
        channel: Channel,
        state: &super::object::FwObject,
        info: &super::object::FwObject,
        f: &Flight,
    ) -> Result<bool> {
        let record = match channel {
            Channel::Compute => f.rec[2],
            Channel::Ta => f.rec[0],
            Channel::Fs => f.rec[1],
        };
        Ok(self.queue_ring_passed(channel, state, info, f)?
            && self.channel_receipt(channel, record, true)?)
    }

    /// A full receipt includes stage and notifier stamps, every physical consumer,
    /// the private allocator completion and the internal timestamp pair. An aborted
    /// receipt omits only the writes firmware does not make for an aborted command,
    /// after the recovery grace period and its queue/pipe/stage passed proof.
    pub(crate) fn retired(&mut self) -> Result<Retirement> {
        self.retired_at(0)
    }

    fn retired_at(&mut self, index: usize) -> Result<Retirement> {
        match self.retired_snapshot_at(index) {
            // A changing packed receipt is not an authenticated firmware failure.
            Err(error) if error == EAGAIN => Ok(Retirement::Pending),
            result => result,
        }
    }

    /// Inspects the oldest compute flight per pipe and oldest render flight independently so any
    /// pipe/class can retire as soon as its hardware receipt settles without waiting on another.
    pub(crate) fn retired_flight(&mut self) -> Result<Option<(usize, Retirement)>> {
        let (mut check_compute, mut check_render) = ([true; super::hw::sizes::COMPUTE_PIPES], true);
        self.retired_flight_classes(&mut check_compute, &mut check_render)
    }

    /// Like [`Self::retired_flight`], skipping pipes/classes that are inactive or already known
    /// pending in the current [`Self::retire_once`] pass and never polling a mixed flight twice.
    fn retired_flight_classes(
        &mut self,
        check_compute: &mut [bool; super::hw::sizes::COMPUTE_PIPES],
        check_render: &mut bool,
    ) -> Result<Option<(usize, Retirement)>> {
        let mut compute_idx = [None; super::hw::sizes::COMPUTE_PIPES];
        let mut render_idx = None;
        if check_compute.iter().any(|&c| c) || *check_render {
            for (i, f) in self.engine.flights.iter().enumerate() {
                if f.computes != 0 {
                    let p = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
                    if check_compute[p] && compute_idx[p].is_none() {
                        compute_idx[p] = Some(i);
                    }
                }
                if *check_render && render_idx.is_none() && f.renders != 0 {
                    render_idx = Some(i);
                }
            }
        }
        for p in 0..super::hw::sizes::COMPUTE_PIPES {
            if compute_idx[p].is_none() {
                check_compute[p] = false;
            }
        }
        if render_idx.is_none() {
            *check_render = false;
        }
        for p in 0..super::hw::sizes::COMPUTE_PIPES {
            if let Some(index) = compute_idx[p] {
                let receipt = self.retired_at(index)?;
                if receipt.complete() {
                    return Ok(Some((index, receipt)));
                }
                check_compute[p] = false;
                if render_idx == Some(index) {
                    *check_render = false;
                    render_idx = None;
                }
            }
        }
        if let Some(index) = render_idx {
            let receipt = self.retired_at(index)?;
            if receipt.complete() {
                return Ok(Some((index, receipt)));
            }
            *check_render = false;
        }
        Ok(None)
    }

    fn retired_snapshot(&mut self) -> Result<Retirement> {
        self.retired_snapshot_at(0)
    }

    fn retired_snapshot_at(&mut self, index: usize) -> Result<Retirement> {
        let Some(f) = self.engine.flights.get(index) else {
            return Ok(Retirement::Pending);
        };
        rmb();
        let mut full = true;
        let mut ticks = [0u64; 4];
        // Later flights of the same pipe/class queued behind this one: that pipe's queue and
        // stamp cells already moved on, so its receipt is "passed", in order; the newest flight
        // of a pipe keeps its exact queue receipt while Channel::Compute is checked independently
        // against the shared submit-channel producer frontier.
        if f.computes != 0 {
            let pipe = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
            let pipe_behind = self
                .engine
                .flights
                .iter()
                .filter(|o| o.computes != 0 && o.compute_pipe == f.compute_pipe)
                .nth(1)
                .is_some();
            let chan_behind = self
                .engine
                .flights
                .iter()
                .filter(|o| o.computes != 0)
                .nth(1)
                .is_some()
                || f.rec[2] != self.channels.producer(Channel::Compute);
            let g = &self.graph.compute[pipe];
            if pipe_behind {
                let tail = self.engine.target[pipe];
                if !stamp_reached(g.fw_stamp.r32(0)?, f.target, tail)
                    || !self.queue_ring_passed(Channel::Compute, &g.qstate, &g.qinfo, f)?
                    || !(f.chan_settled
                        || self.channel_receipt(Channel::Compute, f.rec[2], chan_behind)?)
                {
                    return Ok(Retirement::Pending);
                }
                rmb();
                let stamp = g.notifier.r32(notifier_page::COMPUTE_STAMP)?;
                full &= stamp_reached(stamp, f.target, tail)
                    && (stamp != f.target
                        || g.notifier.r32(notifier_page::COMPUTE_CONTEXT)? == f.job.vm.slot);
            } else {
                if g.fw_stamp.r32(0)? != f.target
                    || !self.queue_ring_receipt(&g.qstate, &g.qinfo, f.head)?
                    || !(f.chan_settled
                        || self.channel_receipt(Channel::Compute, f.rec[2], chan_behind)?)
                {
                    return Ok(Retirement::Pending);
                }
                rmb();
                full &= g.notifier.r32(notifier_page::COMPUTE_STAMP)? == f.target
                    && g.notifier.r32(notifier_page::COMPUTE_CONTEXT)? == f.job.vm.slot;
            }
            let count = f.claims.slot_count as usize;
            full &= count != 0 && count == f.computes as usize;
            if full {
                for m in 0..count {
                    let k = f.claims.slot[m] as usize;
                    let unit = f.job.resources.compute_unit(k)?;
                    let submitted = f.csubmitted[m];
                    if submitted == 0
                        || unit.counter.r64(0)? != submitted
                        || unit.descriptor.r64(manager::COMPLETED)? != submitted
                        || unit.descriptor.r64(manager::COUNTER)? != unit.counter.va
                    {
                        full = false;
                        break;
                    }
                    let (start, end) = self.compute_ticks(unit)?;
                    if start == 0 || end < start {
                        full = false;
                        break;
                    }
                    if m == 0 && count == 1 {
                        ticks = [start, end, 0, 0];
                    }
                }
            }
        }
        if f.renders != 0 {
            let render_behind = self
                .engine
                .flights
                .iter()
                .filter(|o| o.renders != 0)
                .nth(1)
                .is_some();
            let g = &self.graph.render;
            let a = f.pb_slot as usize;
            if render_behind {
                let e = &self.engine;
                // Check FS (terminal stage) before TA so running renders fail fast on 1 WC read.
                if !stamp_reached(g.fw_stamp[1].r32(0)?, f.fs_target, e.fs_target)
                    || !stamp_reached(g.fw_stamp[0].r32(0)?, f.ta_target, e.ta_target)
                    || !self.queue_passed(Channel::Ta, &g.qstate[0], &g.qinfo[0], f)?
                    || !self.queue_passed(Channel::Fs, &g.qstate[1], &g.qinfo[1], f)?
                {
                    return Ok(Retirement::Pending);
                }
                rmb();
                let fs_stamp = g.notifier.r32(notifier_page::FS_STAMP)?;
                let ta_stamp = g.notifier.r32(notifier_page::TA_STAMP)?;
                full &= stamp_reached(fs_stamp, f.fs_target, e.fs_target)
                    && (fs_stamp != f.fs_target
                        || g.notifier
                            .r32(notifier_page::FS_RECORD + notifier::CONTEXT)?
                            == f.job.vm.slot)
                    && stamp_reached(ta_stamp, f.ta_target, e.ta_target)
                    && (ta_stamp != f.ta_target
                        || g.notifier.r32(notifier::CONTEXT)? == f.job.vm.slot)
                    && f.job
                        .resources
                        .pb_retired_at(a, f.uma_submitted)?;
            } else {
                // Check FS (terminal stage) before TA so running renders fail fast on 1 WC read.
                if g.fw_stamp[1].r32(0)? != f.fs_target
                    || g.fw_stamp[0].r32(0)? != f.ta_target
                    || !self.queue_receipt(
                        Channel::Ta,
                        &g.qstate[0],
                        &g.qinfo[0],
                        f.ta_head,
                        f.rec[0],
                    )?
                    || !self.queue_receipt(
                        Channel::Fs,
                        &g.qstate[1],
                        &g.qinfo[1],
                        f.fs_head,
                        f.rec[1],
                    )?
                {
                    return Ok(Retirement::Pending);
                }
                rmb();
                full &= g.notifier.r32(notifier_page::FS_STAMP)? == f.fs_target
                    && g.notifier.r32(notifier_page::TA_STAMP)? == f.ta_target
                    && g.notifier.r32(notifier::CONTEXT)? == f.job.vm.slot
                    && g.notifier
                        .r32(notifier_page::FS_RECORD + notifier::CONTEXT)?
                        == f.job.vm.slot
                    && f.job.resources.pb_retired(a)?;
            }
            if full {
                let count = (f.renders as usize).min(f.claims.pass.len());
                for m in 0..count {
                    let k = f.claims.pass[m] as usize;
                    let unit = f.job.resources.render_pass(k)?;
                    let ta_start = unit.scene.r64(0x600)?;
                    let ta_end = unit.scene.r64(0x608)?;
                    let fs_start = unit.scene.r64(0x620)?;
                    let fs_end = unit.scene.r64(0x628)?;
                    if count == 1 && f.computes == 0 {
                        ticks = [ta_start, ta_end, fs_start, fs_end];
                    }
                    full &= ta_start != 0
                        && ta_end >= ta_start
                        && fs_start != 0
                        && fs_end >= fs_start;
                    if !full {
                        break;
                    }
                }
            }
        }
        if full {
            self.engine.retired_ticks = ticks;
            return Ok(Retirement::Full);
        }
        if f.recovery_suspect {
            // Queues, pipes and every participating stage passed the retained command, yet normal
            // receipts have not settled. Measure the abort grace from this first gate-passed poll
            // and require a second unsettled poll so an innocent suspect flight finishing > 20 ms
            // after recovery cannot race a transient WC/notifier write into a false abort.
            let now = now_ns();
            let aborted = if let Some(f) = self.engine.flights.get_mut(index) {
                let first = *f.gate_passed_ns.get_or_insert(now);
                f.unsettled_polls = f.unsettled_polls.saturating_add(1);
                if f.unsettled_polls >= 2 && now.saturating_sub(first) >= ABORT_GRACE_NS {
                    f.doomed = true;
                    true
                } else {
                    false
                }
            } else {
                false
            };
            if aborted {
                return Ok(Retirement::Aborted);
            }
        }
        Ok(Retirement::Pending)
    }

    /// Logs the fault bank of `state` when it holds a fault.
    pub(crate) fn log_fault(&self, state: &EngineState) {
        let (bank, f, va) = (state.bank, state.status, state.address);
        if f != 0 {
            let access = if (f >> 4) & 1 != 0 { "read" } else { "write" };
            let reason = if (f >> 1) & 7 != 0 {
                "reason!=unmapped"
            } else {
                "unmapped"
            };
            dev_warn!(
                self.graph.dev.as_ref(),
                "G16: fault bank {}: status {:#x} va {:#x} ({} {} level {} unit {:#x} ctx {})\n",
                bank,
                f,
                va,
                access,
                reason,
                (f >> 7) & 3,
                (f >> 9) & 0xff,
                (f >> 17) & 0x3f
            );
        }
    }

    /// Publishes lockless quarantine hints so `SUBMIT` expands extension and recovery slots
    /// immediately when a base or PB allocator slot has been permanently quarantined.
    pub(crate) fn sync_quarantine_hints(&self) {
        if let Some(shared) = &self.engine.shared {
            shared
                .base_compute_quarantined
                .store(self.slots.base_compute_quarantined(), Ordering::Release);
            shared
                .quarantined_compute_mask
                .store(self.slots.quarantined_compute_mask(), Ordering::Release);
            shared
                .base_render_quarantined
                .store(self.slots.base_render_quarantined(), Ordering::Release);
            shared
                .quarantined_pb_mask
                .store(self.pools.quarantined_mask(), Ordering::Release);
        }
    }

    /// Retire the flight at `index` (the oldest of its engine class). Only a full normal receipt
    /// retires its storage; a passed abort quarantines it. Job failure independently drops
    /// remaining unpublished commands even when the storage fully retired.
    #[inline(never)]
    pub(crate) fn retire_oldest(
        &mut self,
        index: usize,
        receipt: Retirement,
        result: Result,
        done: &mut Completions,
    ) {
        if index >= self.engine.flights.len() {
            return;
        }
        let Ok(mut f) = self.engine.flights.remove(index) else {
            return;
        };
        let timestamp_claims = f.claims;
        let retired_ticks = self.engine.retired_ticks;
        self.engine.retired_ticks = [0; 4];
        if result.is_ok() {
            self.stat_retire(&f, retired_ticks);
            if let Some(shared) = &self.engine.shared {
                let now = now_ns();
                shared.feed.note_flight(f.published_ns, now);
                if f.published_ns != 0 && now > f.published_ns {
                    let dur_us = ((now - f.published_ns) / 1000).clamp(1, 10_000) as u32;
                    shared.note_flight_us(dur_us);
                }
            }
        }
        if receipt.owner_retired() {
            if let Err(error) = self.slots.retire(&mut f.claims) {
                self.slots.quarantine(&mut f.claims);
                self.sync_quarantine_hints();
                self.retain(&f.job);
                complete(done, &f.job, Err(error));
                for j2 in f.group.iter() {
                    self.retain(j2);
                    complete(done, j2, Err(error));
                }
                self.fail_stop(error, done);
                core::mem::forget(f);
                return;
            }
            if f.computes != 0 {
                let p = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
                for &stamp in &f.cstamp[..(f.computes as usize).min(f.cstamp.len())] {
                    self.recovery.last_retired.push(p, stamp);
                }
            }
            if f.renders != 0 {
                for m in 0..(f.renders as usize).min(f.r_ta_stamp.len()) {
                    self.recovery
                        .last_retired
                        .push(super::hw::sizes::COMPUTE_PIPES, f.r_ta_stamp[m]);
                    self.recovery
                        .last_retired
                        .push(super::hw::sizes::COMPUTE_PIPES + 1, f.r_fs_stamp[m]);
                }
                let committed = f.pb.as_ref().ok_or(EINVAL).and_then(|epoch| {
                    self.pools.retire_flight(
                        f.pb_slot as usize,
                        epoch,
                        f.pb_binding,
                        result.is_ok(),
                    )
                });
                if let Err(error) = committed {
                    self.retain(&f.job);
                    complete(done, &f.job, Err(error));
                    for j2 in f.group.iter() {
                        self.retain(j2);
                        complete(done, j2, Err(error));
                    }
                    self.fail_stop(error, done);
                    core::mem::forget(f);
                    return;
                }
            }
            let retired_ok = (|| -> Result {
                f.job.note_full_retirement(f.first as u32, f.count as u32)?;
                for j2 in f.group.iter() {
                    j2.note_full_retirement(0, j2.progress.count())?;
                }
                Ok(())
            })();
            if let Err(error) = retired_ok {
                self.retain(&f.job);
                complete(done, &f.job, Err(error));
                for j2 in f.group.iter() {
                    self.retain(j2);
                    complete(done, j2, Err(error));
                }
                self.fail_stop(error, done);
                core::mem::forget(f);
                return;
            }
            if result.is_err() {
                self.slots.mark_dirty(&timestamp_claims);
                self.recovery.affected_full_receipts =
                    self.recovery.affected_full_receipts.saturating_add(1);
            }
        } else {
            // Only an actual passed abort lacks the full owner receipt. An affected job
            // with a full normal receipt fails its fence without poisoning healthy storage.
            if f.renders != 0 {
                // Quarantine the aborted allocator slot permanently and retain the aborted
                // flight's epoch alongside the prior slot owner.
                self.pools.quarantine_flight(f.pb_slot as usize, f.pb.take());
            }
            self.slots.quarantine(&mut f.claims);
            self.sync_quarantine_hints();
            self.retain(&f.job);
            for j2 in f.group.iter() {
                self.retain(j2);
            }
            self.recovery.aborted_receipts = self.recovery.aborted_receipts.saturating_add(1);
        }
        if self.slots.compute_exhausted() {
            self.mark_class_unavailable(super::command::COMPUTE, EIO);
        }
        if self.slots.render_exhausted() || self.pools.exhausted() {
            self.mark_class_unavailable(super::command::RENDER, EIO);
        }
        if result.is_ok() {
            if let Err(error) = self.publish_timestamps(&f, &timestamp_claims) {
                self.retain(&f.job);
                complete(done, &f.job, Err(error));
                for j2 in f.group.iter() {
                    self.retain(j2);
                    complete(done, j2, Err(error));
                }
                self.fail_stop(error, done);
                core::mem::forget(f);
                return;
            }
            // Publish queue-local EXACT_PRIOR frontiers only after every normal hardware
            // receipt and owner-ledger retirement succeeded. No callbacks or allocations.
            for index in f.first..f.first + f.count {
                if let Err(error) = f.job.command_retired(index as u32) {
                    self.retain(&f.job);
                    complete(done, &f.job, Err(error));
                    for j2 in f.group.iter() {
                        self.retain(j2);
                        complete(done, j2, Err(error));
                    }
                    self.fail_stop(error, done);
                    core::mem::forget(f);
                    return;
                }
            }
            for j2 in f.group.iter() {
                for index in 0..j2.progress.count() {
                    if let Err(error) = j2.command_retired(index) {
                        self.retain(&f.job);
                        complete(done, &f.job, Err(error));
                        for j3 in f.group.iter() {
                            self.retain(j3);
                            complete(done, j3, Err(error));
                        }
                        self.fail_stop(error, done);
                        core::mem::forget(f);
                        return;
                    }
                }
            }
            if receipt.owner_retired() && f.renders != 0 {
                self.slots.note_render_retired();
            }
        }
        if self.engine.flights.is_empty() {
            // Drained: a gap until the next publication.
            self.engine.stats.idle_from_ns = now_ns();
            self.power.utilization.idle(self.engine.stats.idle_from_ns);
            self.power.report_work(false);
        }
        // The next prefix on the retired flight's pipe/class starts executing about now.
        if f.renders != 0 {
            self.renew_render_deadline();
        }
        if f.computes != 0 {
            let pipe = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
            self.renew_compute_pipe_deadline(pipe);
        }
        let job = f.job.clone();
        let p = &job.progress;
        p.next.store(p.next() + f.count as u32, Ordering::Relaxed);
        if result.is_err() {
            // Later commands of this queue, and any cross-queue flights transitively waiting
            // on it via firmware handoff, fail with it so no dependent command reports success
            // after an upstream failure.
            let mut changed = false;
            for later in self.engine.flights.iter_mut() {
                if !later.doomed
                    && (Arc::ptr_eq(&later.job.queue, &job.queue)
                        || later.job.waits_on_queue(&job.queue))
                {
                    later.doomed = true;
                    changed = true;
                }
            }
            while changed {
                changed = false;
                for i in 0..self.engine.flights.len() {
                    if self.engine.flights[i].doomed {
                        continue;
                    }
                    let dep_doomed = self.engine.flights.iter().any(|other| {
                        other.doomed
                            && (Arc::ptr_eq(&other.job.queue, &self.engine.flights[i].job.queue)
                                || self.engine.flights[i].job.waits_on_queue(&other.job.queue))
                    });
                    if dep_doomed {
                        self.engine.flights[i].doomed = true;
                        changed = true;
                    }
                }
            }
        }
        if let Err(error) = result {
            if p.is_pending() {
                // Drop its unpublished commands.
                p.pending.store(false, Ordering::Relaxed);
                self.engine.pending.retain(|j| !Arc::ptr_eq(j, &job));
                p.count.store(p.sent(), Ordering::Relaxed);
                job.resources.discard_unpublished_renders();
            }
            if receipt.owner_retired()
                && self.engine.flights.iter().all(|other| {
                    !Arc::ptr_eq(&other.job, &job)
                        && other.group.iter().all(|member| !Arc::ptr_eq(member, &job))
                })
            {
                // This terminal error has no future publication, no other flight, and
                // exact Full receipts for every published command. Preserve the sticky
                // VM error without retaining mappings that the GPU can no longer access.
                if let Err(proof_error) = job.mark_terminal_retired() {
                    self.retain(&job);
                    complete(done, &job, Err(proof_error));
                    for j2 in f.group.iter() {
                        self.retain(j2);
                        complete(done, j2, Err(proof_error));
                    }
                    self.fail_stop(proof_error, done);
                    core::mem::forget(f);
                    return;
                }
            }
            complete(done, &job, Err(error));
        }
        let job_done = p.next() >= p.sent() && p.sent() >= p.count();
        if job_done {
            let s = &mut self.engine.stats;
            if result.is_ok() {
                s.jobs += 1;
                s.job_cmds += u64::from(p.count());
                s.job_max = s.job_max.max(u64::from(p.count()));
            }
            complete(done, &job, Ok(()));
        }
        // Complete folded horizon jobs unconditionally (even if an earlier command of `job`
        // is still in flight; `Timeline` preserves queue fence order).
        for j2 in f.group.iter() {
            let p2 = &j2.progress;
            p2.next.store(p2.count(), Ordering::Relaxed);
            match result {
                Err(error) => {
                    if receipt.owner_retired()
                        && self.engine.flights.iter().all(|other| {
                            !Arc::ptr_eq(&other.job, j2)
                                && other.group.iter().all(|member| !Arc::ptr_eq(member, j2))
                        })
                    {
                        if let Err(proof_error) = j2.mark_terminal_retired() {
                            self.retain(j2);
                            complete(done, j2, Err(proof_error));
                            self.fail_stop(proof_error, done);
                            core::mem::forget(f);
                            return;
                        }
                    }
                    complete(done, j2, Err(error));
                }
                Ok(()) => {
                    let s = &mut self.engine.stats;
                    s.jobs += 1;
                    s.job_cmds += u64::from(p2.count());
                    s.job_max = s.job_max.max(u64::from(p2.count()));
                    complete(done, j2, Ok(()));
                }
            }
        }
    }

    /// Progress of the oldest compute pipes and oldest render flights: a FW stamp of a pipe on
    /// which its oldest flight still has unreached work moved since the last poll. Renews the
    /// deadline; the first call for a flight only records its position.
    /// [`Self::check_hangs`] calls it while no firmware recovery notification is pending.
    pub(crate) fn progress(&mut self) {
        if self.engine.flights.is_empty() {
            return;
        }
        const CP: usize = super::hw::sizes::COMPUTE_PIPES;
        let (g, r, e) = (&self.graph.compute, &self.graph.render, &self.engine);
        let mut fc: [Option<&Flight>; CP] = [None; CP];
        let mut fr = None;
        for f in e.flights.iter() {
            if f.computes != 0 {
                let p = (f.compute_pipe as usize).min(CP - 1);
                if fc[p].is_none() {
                    fc[p] = Some(f);
                }
            }
            if fr.is_none() && f.renders != 0 {
                fr = Some(f);
            }
        }
        // None denotes an inactive/reached stage. A real unreached stamp may be zero
        // immediately after native wrap and must remain distinguishable from completion.
        let pending =
            |observed, own, tail| (!stamp_reached(observed, own, tail)).then_some(observed);
        let mut next_progress = [None; CP + 2];
        let mut published = [0u64; CP + 1];
        for p in 0..CP {
            if let Some(f) = fc[p] {
                next_progress[p] = pending(
                    g[p].fw_stamp.r32(0).unwrap_or(0),
                    f.target,
                    e.target[p],
                );
                published[p] = f.published_ns;
            }
        }
        if let Some(fr) = fr {
            next_progress[CP] = pending(
                r.fw_stamp[0].r32(0).unwrap_or(0),
                fr.ta_target,
                e.ta_target,
            );
            next_progress[CP + 1] =
                pending(r.fw_stamp[1].r32(0).unwrap_or(0), fr.fs_target, e.fs_target);
            published[CP] = fr.published_ns;
        }
        let mut compute_moved = [false; CP];
        for p in 0..CP {
            compute_moved[p] = fc[p].is_some()
                && e.progress_flight[p] == published[p]
                && next_progress[p] != e.progress[p];
        }
        let render_moved = fr.is_some()
            && e.progress_flight[CP] == published[CP]
            && [next_progress[CP], next_progress[CP + 1]] != [e.progress[CP], e.progress[CP + 1]];
        for (p, moved) in compute_moved.into_iter().enumerate() {
            if moved {
                self.renew_compute_pipe_deadline(p);
            }
        }
        if render_moved {
            self.renew_render_deadline();
        }
        self.engine.progress_flight = published;
        self.engine.progress = next_progress;
    }

    /// The device mutex still excludes slot reuse after owner-ledger retirement. Read the
    /// authenticated internal cells and publish through the retained CPU mapping before
    /// advancing queue frontiers or publishing the next command, including same-job copies.
    fn publish_timestamps(&self, flight: &Flight, claims: &super::slots::Claims) -> Result {
        let mut compute = 0;
        let mut render = 0;
        let end = flight.first.checked_add(flight.count).ok_or(EOVERFLOW)?;
        let commands = flight.job.commands.get(flight.first..end).ok_or(EINVAL)?;
        let slots = claims
            .slot
            .get(..claims.slot_count as usize)
            .ok_or(EINVAL)?;
        let passes = claims.pass.get(..flight.renders as usize).ok_or(EINVAL)?;
        for command in commands {
            match command {
                super::command::Command::Compute(command) => {
                    let index = *slots.get(compute).ok_or(EINVAL)? as usize;
                    if !command.timestamp_outputs.is_empty() {
                        let unit = flight.job.resources.compute_unit(index)?;
                        let (start, end) = self.compute_ticks(unit)?;
                        let ticks = [start, end, 0, 0];
                        command
                            .timestamp_outputs
                            .publish(ticks, super::hw::COMMAND_TIMESTAMP_HZ);
                    }
                    compute += 1;
                }
                super::command::Command::Render(command) => {
                    let index = *passes.get(render).ok_or(EINVAL)? as usize;
                    if !command.timestamp_outputs.is_empty() {
                        let pass = flight.job.resources.render_pass(index)?;
                        let ticks = [
                            pass.scene.r64(0x600)?,
                            pass.scene.r64(0x608)?,
                            pass.scene.r64(0x620)?,
                            pass.scene.r64(0x628)?,
                        ];
                        command
                            .timestamp_outputs
                            .publish(ticks, super::hw::COMMAND_TIMESTAMP_HZ);
                    }
                    render += 1;
                }
            }
        }
        for member in flight.group.iter() {
            for command in member.commands.iter() {
                match command {
                    super::command::Command::Compute(command) => {
                        let index = *slots.get(compute).ok_or(EINVAL)? as usize;
                        if !command.timestamp_outputs.is_empty() {
                            let unit = flight.job.resources.compute_unit(index)?;
                            let (start, end) = self.compute_ticks(unit)?;
                            let ticks = [start, end, 0, 0];
                            command
                                .timestamp_outputs
                                .publish(ticks, super::hw::COMMAND_TIMESTAMP_HZ);
                        }
                        compute += 1;
                    }
                    super::command::Command::Render(command) => {
                        let index = *passes.get(render).ok_or(EINVAL)? as usize;
                        if !command.timestamp_outputs.is_empty() {
                            let pass = flight.job.resources.render_pass(index)?;
                            let ticks = [
                                pass.scene.r64(0x600)?,
                                pass.scene.r64(0x608)?,
                                pass.scene.r64(0x620)?,
                                pass.scene.r64(0x628)?,
                            ];
                            command
                                .timestamp_outputs
                                .publish(ticks, super::hw::COMMAND_TIMESTAMP_HZ);
                        }
                        render += 1;
                    }
                }
            }
        }
        Ok(())
    }

    /// A retired prefix: publication to retirement (including any wait behind its
    /// predecessor), event times, internal stage times of its members.
    fn stat_retire(&mut self, f: &Flight, ticks: [u64; 4]) {
        let now = now_ns();
        let d = now.wrapping_sub(f.published_ns);
        let kind = if f.computes != 0 && f.renders != 0 {
            2
        } else if f.renders != 0 {
            1
        } else {
            0
        };
        let (mut lo, mut hi) = (u64::MAX, 0u64);
        let (mut first, mut last) = (u64::MAX, 0u64);
        let mut cdm_flight_ticks = 0u64;
        let mut cdm_counted = 0u32;
        let work_ticks = self.init.abi.compute().work_ticks;
        let s = &mut self.engine.stats;

        let mut add_interval =
            |start: u64, end: u64, sum: &mut u64, engine: crate::usage::Engine| {
                if start == 0 || end < start {
                    return;
                }
                f.job
                    .queue
                    .usage
                    .add_union(engine, start, end, super::hw::COMMAND_TIMESTAMP_HZ as u32);
                *sum = sum.saturating_add(end - start);
                lo = lo.min(start);
                hi = hi.max(end);
            };
        if f.renders == 1 && f.computes == 0 && ticks[0] != 0 {
            add_interval(
                ticks[0],
                ticks[1],
                &mut s.ta_ticks,
                crate::usage::Engine::Vertex,
            );
            add_interval(
                ticks[2],
                ticks[3],
                &mut s.fs_ticks,
                crate::usage::Engine::Fragment,
            );
        } else if f.computes == 1 && f.renders == 0 && ticks[0] != 0 {
            add_interval(
                ticks[0],
                ticks[1],
                &mut s.cdm_ticks,
                crate::usage::Engine::Compute,
            );
            if ticks[1] >= ticks[0] {
                cdm_flight_ticks = ticks[1] - ticks[0];
                cdm_counted = 1;
            }
        } else {
            // Multi-command prefix fallback: read each member's internal timestamp cells.
            let mut stage = |o: &super::object::FwObject,
                             off: usize,
                             sum: &mut u64,
                             engine: crate::usage::Engine| {
                let (Ok(start_lo), Ok(start_hi), Ok(end_lo), Ok(end_hi)) =
                    (o.r32(off), o.r32(off + 4), o.r32(off + 8), o.r32(off + 12))
                else {
                    return;
                };
                let start = u64::from(start_hi) << 32 | u64::from(start_lo);
                let end = u64::from(end_hi) << 32 | u64::from(end_lo);
                add_interval(start, end, sum, engine);
            };
            for k in 0..f.renders as usize {
                let Ok(p) = f.job.resources.render_pass(f.claims.pass[k] as usize) else {
                    return;
                };
                stage(
                    &p.scene,
                    0x600,
                    &mut s.ta_ticks,
                    crate::usage::Engine::Vertex,
                );
                stage(
                    &p.scene,
                    0x620,
                    &mut s.fs_ticks,
                    crate::usage::Engine::Fragment,
                );
            }
            for k in 0..(f.computes as usize).min(f.claims.slot_count as usize) {
                let Ok(c) = f.job.resources.compute_unit(f.claims.slot[k] as usize) else {
                    return;
                };
                if let Ok((start, end)) = Self::compute_ticks_with(work_ticks, c) {
                    add_interval(start, end, &mut s.cdm_ticks, crate::usage::Engine::Compute);
                    if start != 0 && end >= start {
                        cdm_flight_ticks = cdm_flight_ticks.saturating_add(end - start);
                        cdm_counted += 1;
                    }
                }
            }
        }
        if cdm_counted != 0 {
            let per_cmd_ticks = cdm_flight_ticks / u64::from(cdm_counted);
            let per_cmd_us = (per_cmd_ticks.saturating_mul(1_000_000)
                / super::hw::COMMAND_TIMESTAMP_HZ)
                .clamp(1, 100_000) as u32;
            f.job.queue.note_compute_us(per_cmd_us);
        }
        if hi > lo {
            s.span_ticks = s.span_ticks.saturating_add(hi - lo);
        }
        if f.renders != 0 {
            for b in 0..2 {
                let t = self.events.event_ns[b];
                first = first.min(t);
                last = last.max(t);
            }
        }
        if f.computes != 0 {
            let p = (f.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
            let t = self.events.event_ns[2 + p];
            first = first.min(t);
            last = last.max(t);
        }
        if first > f.published_ns && last <= now {
            s.first_ns[kind] += first - f.published_ns;
            s.last_ns[kind] += last - f.published_ns;
        }
        s.count[0] += u64::from(f.computes);
        s.count[1] += u64::from(f.renders);
        if kind == 2 {
            s.count[2] += f.count as u64;
        }
        s.prefixes[kind] += 1;
        s.busy_ns[kind] += d;
        s.max_ns[kind] = s.max_ns[kind].max(d);
    }

    /// Engine state report for failures and hang requests (without the initdata Runtime
    /// readiness and Flags words).
    #[allow(dead_code)]
    pub(crate) fn report(&self, what: &str) {
        let (r, e) = (&self.graph.render, &self.engine);
        let ch = &self.channels;
        let rd = |o: &super::object::FwObject, off| o.r32(off).unwrap_or(0xdeadbeef);
        let state = self
            .session
            .engine_state()
            .ok()
            .flatten()
            .unwrap_or_default();
        let f = e.oldest();
        let pipe = f
            .map_or(0, |f| {
                if f.computes != 0 {
                    f.compute_pipe as usize
                } else {
                    0
                }
            })
            .min(super::hw::sizes::COMPUTE_PIPES - 1);
        let g = &self.graph.compute[pipe];
        let ms = f.map_or(0, |f| now_ns().wrapping_sub(f.published_ns) / 1_000_000);

        dev_info!(
            self.graph.dev.as_ref(),
            "G16: {}: ctx={} prefixes={} n={} pipe={} target={:#x} user={:#x} fw={:#x} done={} read={} write={} head={} ch={}/{} events={}/{} errors={} busy={} fault={:#x} {}ms\n",
            what,
            f.map_or(0, |f| f.job.vm.slot),
            e.flights.len(),
            e.ordinal,
            pipe,
            e.target[pipe],
            rd(&g.user_stamp, 0),
            rd(&g.fw_stamp, 0),
            rd(&g.qstate, qstate::DONE),
            rd(&g.qstate, qstate::READ),
            rd(&g.qstate, qstate::WRITE),
            e.head[pipe],
            ch.read(Channel::Compute).unwrap_or(0xdeadbeef),
            ch.write_cell(Channel::Compute).unwrap_or(0xdeadbeef),
            self.events.compute_events,
            e.events,
            self.events.errors,
            state.busy,
            state.fault(),
            ms
        );
        if f.is_some_and(|f| f.renders != 0) {
            dev_info!(
                self.graph.dev.as_ref(),
                "G16:   render: draw={} ta {:#x}/{:#x}/{:#x} done={} read={} write={} head={} ch0={}/{}; fs {:#x}/{:#x}/{:#x} done={} read={} write={} head={} ch1={}/{}; stage events={}/{}\n",
                e.draw,
                e.ta_target,
                rd(&r.user_stamp[0], 0),
                rd(&r.fw_stamp[0], 0),
                rd(&r.qstate[0], qstate::DONE),
                rd(&r.qstate[0], qstate::READ),
                rd(&r.qstate[0], qstate::WRITE),
                e.ta_head,
                ch.read(Channel::Ta).unwrap_or(0xdeadbeef),
                ch.write_cell(Channel::Ta).unwrap_or(0xdeadbeef),
                e.fs_target,
                rd(&r.user_stamp[1], 0),
                rd(&r.fw_stamp[1], 0),
                rd(&r.qstate[1], qstate::DONE),
                rd(&r.qstate[1], qstate::READ),
                rd(&r.qstate[1], qstate::WRITE),
                e.fs_head,
                ch.read(Channel::Fs).unwrap_or(0xdeadbeef),
                ch.write_cell(Channel::Fs).unwrap_or(0xdeadbeef),
                self.events.stage_events,
                e.stage_seen
            );
        }
    }

    /// The `aurora-g16/stats` contents. Scripts parse the keys: keep them stable.
    pub(crate) fn write_stats(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = &self.engine.stats;
        let slots = &self.slots.stats;
        let (recoveries, hang_kills) = self.recovery_counts();
        let us = |ns: u64| ns / 1000;

        writeln!(
            f,
            "health: failed {} errno {} degraded {}",
            u8::from(self.recovery.failed().is_some()),
            self.recovery.failed().map_or(0, Error::to_errno),
            u8::from(self.recovery.unavailable.iter().any(Option::is_some))
        )?;
        writeln!(
            f,
            "classes: render_failed {} render_errno {} compute_failed {} compute_errno {}",
            u8::from(self.recovery.unavailable[super::command::RENDER].is_some()),
            self.recovery.unavailable[super::command::RENDER].map_or(0, Error::to_errno),
            u8::from(self.recovery.unavailable[super::command::COMPUTE].is_some()),
            self.recovery.unavailable[super::command::COMPUTE].map_or(0, Error::to_errno)
        )?;
        writeln!(f, "command_policy: render_deadline_ms {} compute_deadline_ms {} long_commands {} firmware_stamp_period {} compute_ordinal {} render_ordinal {}",
            params::deadline_ms(), params::compute_deadline_ms(), self.recovery.long_commands,
            super::recovery::logic::FIRMWARE_STAMP_PERIOD, self.engine.ordinal, self.engine.draw)?;
        writeln!(
            f,
            "power: governor {} budget {} demand S{} utilization {} effective S{} temperature_mc {} updates {} sensor_errors {}",
            self.power.governor,
            self.power.budget,
            self.power.utilization.demand,
            self.power.utilization.average,
            self.power.effective, self.power.temperature_mc, self.power.updates,
            self.power.sensor_errors
        )?;
        writeln!(
            f,
            "publications: compute {} render {}",
            s.publications[0], s.publications[1]
        )?;
        let (live, handles) = self.vms.counts();
        writeln!(f, "vm_bindings: live {} handles {}", live, handles)?;
        writeln!(f, "epoch_budget_waits: {}", super::budget::waits())?;
        writeln!(
            f,
            "resources: compute_cross_vm {} render_cross_vm {}",
            slots.cross_vm_retargets[0], slots.cross_vm_retargets[1]
        )?;
        writeln!(
            f,
            "recovery_receipts: affected_full {} aborted {}",
            self.recovery.affected_full_receipts, self.recovery.aborted_receipts
        )?;
        for (name, owners) in ["compute", "render"]
            .into_iter()
            .zip(self.slots.lifetime_counts())
        {
            writeln!(
                f,
                "{}_owners: fresh {} preparing {} published {} retired {} quarantined {} reusable {}",
                name, owners.fresh, owners.preparing, owners.published, owners.retired,
                owners.quarantined, owners.reusable
            )?;
        }
        for (kind, name) in ["compute", "render"].iter().enumerate() {
            let proof = &slots.last_retarget[kind];
            writeln!(f,
                "{}_retarget: unit {} old_vm {} new_vm {} old_asid {} new_asid {} old_generation {} new_generation {}",
                name, proof.unit, proof.old_vm, proof.new_vm, proof.old_asid,
                proof.new_asid, proof.old_generation, proof.new_generation)?;
        }

        for (e, name) in ["compute", "render", "mixed"].iter().enumerate() {
            writeln!(
                f,
                "{}: count {} prefixes {} busy_us {} max_us {} first_us {} last_us {}",
                name,
                s.count[e],
                s.prefixes[e],
                us(s.busy_ns[e]),
                us(s.max_ns[e]),
                us(s.first_ns[e]),
                us(s.last_ns[e])
            )?;
        }
        writeln!(
            f,
            "gaps {} gap_us {} yields {} overlaps {}",
            s.gaps,
            us(s.gap_ns),
            s.yields,
            s.overlaps
        )?;
        writeln!(
            f,
            "jobs {} job_cmds {} job_max {} early {} recoveries {} hang_kills {} clustered {}",
            s.jobs, s.job_cmds, s.job_max, s.early, recoveries, hang_kills, s.clustered
        )?;
        writeln!(
            f,
            "ct_sync {} slot_sync {} starved_us {} prep_us {} grouped {} pass_sync {}",
            slots.ct_sync,
            slots.slot_sync,
            us(s.starved_ns),
            us(s.prep_ns),
            s.grouped,
            slots.pass_sync
        )?;
        writeln!(
            f,
            "pass_scrub: passes {} tpc_clear_bytes {} tilemap_clear_bytes {}",
            self.slots.pass_scrubs_total(),
            slots.tpc_clear_bytes,
            self.slots.tilemap_clear_bytes_total()
        )?;
        writeln!(f, "type0: fs2c {} c2ta {}", s.fs2c0, s.c2ta0)?;
        write!(f, "horizon:")?;
        for n in s.hz.iter() {
            write!(f, " {}", n)?;
        }
        writeln!(
            f,
            " stop {}/{}/{}/{} behind {} hold {}/{}/{} ahead {}",
            s.hz_stop[0],
            s.hz_stop[1],
            s.hz_stop[2],
            s.hz_stop[3],
            s.hz_behind,
            s.hold[0],
            s.hold[1],
            s.hold[2],
            s.hz_ahead
        )?;
        self.pools.write_stats(f)?;
        writeln!(
            f,
            "stage: ta_us {} fs_us {} cdm_us {} span_us {}",
            s.ta_ticks / 24,
            s.fs_ticks / 24,
            s.cdm_ticks / 24,
            s.span_ticks / 24
        )?;
        // The phase timers of debug_flags bit 49, and the scrub flush waits.
        {
            let s = &self.engine.stats;
            writeln!(
                f,
                "pipeline: behind {} max_flights {} compute_published {} hold {}/{}/{}",
                s.behind,
                s.max_flights,
                self.slots.compute_published(),
                s.hold[0],
                s.hold[1],
                s.hold[2]
            )?;
            write!(f, "hold_causes:")?;
            for (name, n) in super::engine::HoldCause::NAMES.iter().zip(s.hold_causes) {
                write!(f, " {} {}", name, n)?;
            }
            writeln!(f)?;
        }
        let p = &self.engine.prof;
        writeln!(
            f,
            "prof: prefixes {} select_us {} encode_us {} claim_us {} write_us {} post_us {} polls {} drain_us {} retire_us {} poll_us {} flushes {} flush_us {}",
            p.prefixes,
            us(p.select_ns),
            us(p.encode_ns),
            us(p.claim_ns),
            us(p.write_ns),
            us(p.post_ns),
            p.polls,
            us(p.drain_ns),
            us(p.retire_ns),
            us(p.poll_ns),
            slots.flushes,
            us(slots.flush_ns)
        )
    }
}
