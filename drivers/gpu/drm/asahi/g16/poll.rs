// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The poll work: drain, retire, growth, hang checks, publication; and the repoll timers.
//!
//! Order of one pass, under the device lock: drain the receive rings; authenticate recovery;
//! retire flights using stage stamps, NC queue READ/WRITE and submit-pipe frontiers, normal
//! notifier/UMA receipts and internal timestamps; renew the progress deadline. Up to four
//! trailing 5 us polls follow an EP 0x20 event. Then answer PB
//! growth ([`Firmware::answer_growth`]); check hangs and recover ([`Firmware::check_hangs`],
//! which calls [`Firmware::progress`]); publish ([`Firmware::publish`]). Then drop the lock
//! and complete the retired jobs ([`super::job::complete_all`]). Wakeups: the EP 0x20 event
//! kick, then an hrtimer re-poll every `g16_resnap_us` (100) while anything is published, or
//! `g16_repoll_us` (15) while a ready prefix waits for record consumption, with a 1-jiffy
//! delayed-work backup.

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
use super::fw::graph::{notifier, notifier_page, qstate};
use super::fw::work::compute;
use super::job::{self, Completions, Job};
use super::recovery::logic::Retirement;
use super::regs::EngineState;
use super::{params, Firmware, Shared, POLL_WORK};
use crate::module_parameters;

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
        this.poll_now();
        HrTimerRestart::NoRestart
    }
}

/// The `asahi-g16/stats` debugfs file contents.
fn stats_show(shared: &Arc<Shared>, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    let state = shared.state.lock();
    match state.as_deref() {
        Some(firmware) => firmware.write_stats(f),
        None => Ok(()),
    }
}

impl Shared {
    /// One poll pass (the poll work item).
    pub(crate) fn run_poll(self: Arc<Self>) {
        let signalling = kernel::dma_fence::SignallingGuard::new();
        let mut done = Completions::new();
        {
            let mut state = self.state.lock();
            if let Some(firmware) = state.as_deref_mut() {
                if firmware.engine.shared.is_none() {
                    firmware.engine.shared = Some(self.clone());
                }
                let evented = self.evented.swap(false, Ordering::AcqRel);
                match firmware.poll(evented, &mut done) {
                    Ok(true) => self.rearm(firmware.engine.chan_wait),
                    // Idle: one more pass after a window gives the state back.
                    Ok(false) if firmware.power.utilization.decaying() => {
                        self.mod_poll(kernel::time::msecs_to_jiffies(
                            (super::power::policy::WINDOW_NS / 1_000_000) as u32,
                        ))
                    }
                    Ok(false) => {}
                    Err(error) => {
                        firmware.fail_stop(error, &mut done);
                        self.fail_stop(error);
                    }
                }
            }
        }
        job::complete_all(done);
        drop(signalling);
        self.notify_wedged();
    }

    /// Hands a job to the engine and publishes what can be published. Err: the job was not
    /// accepted (the engine failed: EIO) and the caller fails it; once accepted, its fence is
    /// signalled by the engine, also on failure.
    pub(crate) fn submit(self: &Arc<Self>, job: Arc<Job>) -> Result {
        let signalling = kernel::dma_fence::SignallingGuard::new();
        let mut done = Completions::new();
        let accepted = {
            let mut state = self.state.lock();
            let firmware = state.as_deref_mut().ok_or(ENODEV)?;
            if firmware.engine.shared.is_none() {
                firmware.engine.shared = Some(self.clone());
            }
            let accepted = firmware.enqueue(job);
            if accepted.is_ok() {
                if let Err(error) = firmware.publish(&mut done) {
                    firmware.fail_stop(error, &mut done);
                    self.fail_stop(error);
                }
                if !firmware.engine.flights.is_empty() || firmware.engine.chan_wait {
                    self.rearm(firmware.engine.chan_wait);
                }
            }
            accepted
        };
        job::complete_all(done);
        drop(signalling);
        self.notify_wedged();
        accepted
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

    /// Queues the poll work now, or brings a pending one forward. Callable from any context.
    pub(crate) fn poll_now(self: &Arc<Self>) {
        self.mod_poll(0);
    }

    /// Queues the poll on `system_highpri_wq` after `delay` jiffies, or moves an already
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

    /// Re-checks a published command soon: hrtimer, with a one-jiffy backup; sooner while a
    /// ready prefix waits for a channel record's consumption.
    pub(crate) fn rearm(self: &Arc<Self>, chan_wait: bool) {
        let mut us = params::resnap_us();
        if chan_wait && module_parameters::g16_repoll_us.value() != 0 {
            us = params::repoll_us();
        }
        {
            let mut handle = self.resnap_handle.lock();
            // Dropping the previous handle cancels a still pending expiry before the timer
            // is started again.
            drop(handle.take());
            *handle = Some(self.clone().start(Delta::from_micros(us as i64)));
        }
        self.mod_poll(1);
    }

    /// Starts what the engine needs besides the firmware state: the `asahi-g16/stats` file.
    pub(crate) fn start_engine(self: &Arc<Self>) -> Result {
        let dir = debugfs::Dir::new(c_str!("asahi-g16"));
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
#[allow(dead_code)]
pub(crate) fn ring_passed(c: u32, b: u32, h: u32, t: u32, m: u32) -> bool {
    let d = |x: u32| x.wrapping_add(m).wrapping_sub(b) % m;
    let (dc, dh, dt) = (d(c), d(h), d(t));
    dh <= dc && dc <= dt
}

impl Firmware {
    /// One poll pass under the device lock. Returns whether to re-arm the poll timers.
    pub(crate) fn poll(&mut self, evented: bool, done: &mut Completions) -> Result<bool> {
        if self.recovery.failed().is_some() {
            return Ok(false);
        }
        self.session.check()?;
        self.objects.health()?;
        self.service_power()?;
        // With nothing published only a pending firmware recovery notification needs this
        // pass: one that arrived after its prefix's receipts is answered
        // here (the firmware halted and waits for the resume) instead of being taken for a
        // failure of the next prefix. Drain the receive rings before deciding, published or
        // not, so the error count is current and idle log/trace/stats traffic is consumed. With
        // prefixes published the retirement loop below drains first.
        if self.engine.flights.is_empty() {
            self.drain_rings()?;
            if self.recovery_idle()? && self.engine.pending.is_empty() {
                return Ok(false);
            }
        }
        let mut lap = Lap::start();
        let start = lap.on().then(now_ns);
        self.retire(evented, done)?;
        lap.lap(&mut self.engine.prof.retire_ns);
        self.answer_growth()?;
        self.check_hangs(done)?;
        let published = self.publish(done);
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
        Ok(!self.engine.flights.is_empty()
            || self.engine.chan_wait
            || self.recovery.needs_repoll()
            || self.engine.frozen)
    }

    /// Drains the rings and retires finished flights, with up to four trailing 5 us polls
    /// after an event. A receipt fault is left in `Recovery::receipt_fault` for the hang
    /// checks.
    pub(crate) fn retire(&mut self, evented: bool, done: &mut Completions) -> Result {
        // After an event, a few short trailing polls catch the final receipts.
        let mut tail = if evented { 4 } else { 0 };
        let mut ret;

        loop {
            let mut lap = Lap::start();
            self.drain_rings()?;
            lap.lap(&mut self.engine.prof.drain_ns);
            // Recovery may force stage progress: authenticate it before any receipt.
            self.check_hangs(done)?;
            ret = self.retired();
            while let Ok(receipt) = ret {
                if !receipt.complete() {
                    break;
                }
                let outcome = self.retire_outcome(receipt);
                self.retire_oldest(receipt, outcome, done);
                ret = self.retired();
            }
            if ret.is_err() || tail == 0 || self.engine.flights.is_empty() {
                break;
            }
            tail -= 1;
            delay::udelay(Delta::from_micros(5));
        }
        self.recovery.receipt_fault = ret.is_err();
        Ok(())
    }

    #[allow(dead_code)]
    fn r32(o: &super::object::FwObject, offset: usize) -> Result<u32> {
        o.r32(offset)
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
        if head >= 256 || record >= 256 {
            return Err(EINVAL);
        }
        // M4's qualified NC cursors are READ and WRITE. Offset zero is not an M4
        // completion witness, and must not inherit G15's DONE semantics.
        for offset in [qstate::READ, qstate::WRITE] {
            if state.r32(offset)? != head {
                return Ok(false);
            }
        }
        if !super::hw::tuning::FW_CACHED {
            for offset in [0x20, 0x24, 0x28] {
                if info.r32(offset)? != head {
                    return Ok(false);
                }
            }
            if info.r32(0x80)? != 0 {
                return Ok(false);
            }
        }
        // With ASC-cached QI, its three consumers and busy flag can remain stale until
        // terminal CIVAC. The working M4 protocol substitutes the NC READ cursor above.
        let pipe = self.channels.channel(channel);
        if pipe.read()? != record || pipe.completion()? != record || pipe.write()? != record {
            return Ok(false);
        }
        Ok(true)
    }

    /// A full receipt includes stage and notifier stamps, every physical consumer,
    /// the private allocator completion and the internal timestamp pair. An aborted
    /// receipt omits only the writes firmware does not make for an aborted command,
    /// after the recovery grace period and its queue/pipe/stage passed proof.
    pub(crate) fn retired(&mut self) -> Result<Retirement> {
        match self.retired_snapshot() {
            // A changing packed receipt is not an authenticated firmware failure.
            Err(error) if error == EAGAIN => Ok(Retirement::Pending),
            result => result,
        }
    }

    fn retired_snapshot(&mut self) -> Result<Retirement> {
        let Some(f) = self.engine.oldest() else {
            return Ok(Retirement::Pending);
        };
        rmb();
        let mut full = true;
        if f.computes != 0 {
            let g = &self.graph.compute;
            if g.fw_stamp.r32(0)? != f.target
                || !self.queue_receipt(Channel::Compute, &g.qstate, &g.qinfo, f.head, f.rec[2])?
            {
                return Ok(Retirement::Pending);
            }
            let k = f.claims.slot[0] as usize;
            let unit = f.job.resources.compute()?.compute.get(k).ok_or(EINVAL)?;
            let submitted = unit.counter.r64(0)?;
            let start = unit.its.r64(0)?;
            let end = unit.its.r64(8)?;
            full &= g.notifier.r32(notifier_page::COMPUTE_STAMP)? == f.target
                && g.notifier.r32(notifier_page::COMPUTE_CONTEXT)? == f.job.vm.slot
                && submitted != 0
                && unit.work.r64(compute::DESCRIPTOR_COMPLETED)? == submitted
                && unit.work.r64(compute::DESCRIPTOR_COUNTER)? == unit.counter.va
                && start != 0
                && end > start;
        }
        if f.renders != 0 {
            let g = &self.graph.render;
            if g.fw_stamp[0].r32(0)? != f.ta_target
                || g.fw_stamp[1].r32(0)? != f.fs_target
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
            let k = f.claims.pass[0] as usize;
            let unit = f.job.resources.render()?.passes.get(k).ok_or(EINVAL)?;
            let ta_start = unit.scene.r64(0x600)?;
            let ta_end = unit.scene.r64(0x608)?;
            let fs_start = unit.scene.r64(0x620)?;
            let fs_end = unit.scene.r64(0x628)?;
            full &= g.notifier.r32(notifier_page::TA_STAMP)? == f.ta_target
                && g.notifier.r32(notifier_page::FS_STAMP)? == f.fs_target
                && g.notifier.r32(notifier::CONTEXT)? == f.job.vm.slot
                && g.notifier
                    .r32(notifier_page::FS_RECORD + notifier::CONTEXT)?
                    == f.job.vm.slot
                && f.job.resources.render()?.pools.retired(k)?
                && ta_start != 0
                && ta_end > ta_start
                && fs_start != 0
                && fs_end > fs_start;
        }
        if full {
            return Ok(Retirement::Full);
        }
        if f.recovery_suspect && now_ns().saturating_sub(f.recovery_started_ns) >= 20_000_000 {
            // Queues, pipes and every participating stage passed the retained command.
            // The firmware recovered it without normal completion: fail and quarantine.
            if let Some(f) = self.engine.flights.first_mut() {
                f.doomed = true;
            }
            return Ok(Retirement::Aborted);
        }
        Ok(Retirement::Pending)
    }

    /// Logs the non-zero fault banks of `state`.
    #[allow(dead_code)]
    pub(crate) fn log_fault(&self, state: &EngineState) {
        for bank in 0..2 {
            let (f, va) = (state.status[bank], state.address[bank]);
            if f == 0 {
                continue;
            }
            let access = if (f >> 4) & 1 != 0 { "read" } else { "write" };
            let reason = if (f >> 1) & 7 != 0 {
                "reason!=unmapped"
            } else {
                "unmapped"
            };
            dev_info!(
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

    /// Retire the oldest prefix's scheduling ownership. Only a full normal receipt retires
    /// its storage; a passed abort quarantines it. Job failure independently drops remaining
    /// unpublished commands even when the storage fully retired.
    pub(crate) fn retire_oldest(
        &mut self,
        receipt: Retirement,
        result: Result,
        done: &mut Completions,
    ) {
        if self.engine.flights.is_empty() {
            return;
        }
        let Ok(mut f) = self.engine.flights.remove(0) else {
            return;
        };
        let timestamp_claims = f.claims;
        if result.is_ok() {
            self.stat_retire(&f);
        }
        if receipt.owner_retired() {
            if let Err(error) = self.slots.retire(&mut f.claims) {
                self.slots.quarantine(&mut f.claims);
                self.retain(&f.job);
                complete(done, &f.job, Err(error));
                self.fail_stop(error, done);
                return;
            }
            if f.computes != 0 {
                self.recovery.last_retired[0] = Some(f.target);
            }
            if f.renders != 0 {
                self.recovery.last_retired[1] = Some(f.ta_target);
                self.recovery.last_retired[2] = Some(f.fs_target);
                let committed = f.job.resources.pb(f.first).and_then(|epoch| {
                    self.pools
                        .retire_binding(timestamp_claims.pass[0] as usize, epoch)
                });
                if let Err(error) = committed {
                    self.retain(&f.job);
                    complete(done, &f.job, Err(error));
                    self.fail_stop(error, done);
                    return;
                }
            }
            if let Err(error) = f.job.note_full_retirement(f.first as u32, f.count as u32) {
                self.retain(&f.job);
                complete(done, &f.job, Err(error));
                self.fail_stop(error, done);
                return;
            }
            if result.is_err() {
                self.recovery.affected_full_receipts =
                    self.recovery.affected_full_receipts.saturating_add(1);
            }
        } else {
            // Only an actual passed abort lacks the full owner receipt. An affected job
            // with a full normal receipt fails its fence without poisoning healthy storage.
            self.slots.quarantine(&mut f.claims);
            self.retain(&f.job);
            self.recovery.aborted_receipts = self.recovery.aborted_receipts.saturating_add(1);
        }
        if self.slots.compute_exhausted() {
            self.mark_class_unavailable(super::command::COMPUTE, EIO);
        }
        if self.slots.render_exhausted() {
            self.mark_class_unavailable(super::command::RENDER, EIO);
        }
        if result.is_ok() {
            if let Err(error) = self.publish_timestamps(&f, &timestamp_claims) {
                self.retain(&f.job);
                complete(done, &f.job, Err(error));
                self.fail_stop(error, done);
                return;
            }
            // Publish queue-local EXACT_PRIOR frontiers only after every normal hardware
            // receipt and owner-ledger retirement succeeded. No callbacks or allocations.
            for index in f.first..f.first + f.count {
                if let Err(error) = f.job.command_retired(index as u32) {
                    self.retain(&f.job);
                    complete(done, &f.job, Err(error));
                    self.fail_stop(error, done);
                    return;
                }
            }
        }
        if self.engine.flights.is_empty() {
            // Drained: a gap until the next publication.
            self.engine.stats.idle_from_ns = now_ns();
            self.power.utilization.idle(self.engine.stats.idle_from_ns);
        }
        // The next prefix starts executing about now.
        self.renew_deadlines();
        let job = f.job.clone();
        let p = &job.progress;
        p.next.store(p.next() + f.count as u32, Ordering::Relaxed);
        if let Err(error) = result {
            if p.is_pending() {
                // Drop its unpublished commands.
                p.pending.store(false, Ordering::Relaxed);
                self.engine.pending.retain(|j| !Arc::ptr_eq(j, &job));
                p.count.store(p.sent(), Ordering::Relaxed);
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
                    self.fail_stop(proof_error, done);
                    return;
                }
            }
            complete(done, &job, Err(error));
        }
        if p.next() < p.sent() || p.sent() < p.count() {
            return; // more of it published or still to publish
        }
        let s = &mut self.engine.stats;
        if result.is_ok() {
            s.jobs += 1;
            s.job_cmds += p.count() as u64;
            s.job_max = s.job_max.max(p.count() as u64);
        }
        complete(done, &job, Ok(()));
        // The rest of a compute horizon, after the first job (fence order).
        for j2 in f.group.iter() {
            let p2 = &j2.progress;
            p2.next.store(p2.count(), Ordering::Relaxed);
            match result {
                Err(error) => complete(done, j2, Err(error)),
                Ok(()) => {
                    s.jobs += 1;
                    s.job_cmds += p2.count() as u64;
                    s.job_max = s.job_max.max(p2.count() as u64);
                }
            }
            complete(done, j2, Ok(()));
        }
    }

    /// Progress of the oldest prefix: a FW stamp of an engine on which it still has unreached
    /// work moved since the last poll (later prefixes' work on other engines does not count).
    /// Renews the deadline; the first call for a prefix only records its position.
    /// [`Self::check_hangs`] calls it while no firmware recovery notification is pending.
    pub(crate) fn progress(&mut self) {
        let Some(f) = self.engine.oldest() else {
            return;
        };
        let (g, r, e) = (&self.graph.compute, &self.graph.render, &self.engine);
        // None denotes an inactive/reached stage. A real unreached stamp may be zero
        // immediately after native wrap and must remain distinguishable from completion.
        let pending =
            |observed, own, tail| (!stamp_reached(observed, own, tail)).then_some(observed);
        let (mut c, mut ta, mut fs) = (None, None, None);
        if f.computes != 0 {
            c = pending(g.fw_stamp.r32(0).unwrap_or(0), f.target, e.target);
        }
        if f.renders != 0 {
            ta = pending(r.fw_stamp[0].r32(0).unwrap_or(0), f.ta_target, e.ta_target);
            fs = pending(r.fw_stamp[1].r32(0).unwrap_or(0), f.fs_target, e.fs_target);
        }
        let moved = e.progress_flight == f.published_ns && [c, ta, fs] != e.progress;
        let published = f.published_ns;
        if moved {
            self.renew_deadlines();
        }
        self.engine.progress_flight = published;
        self.engine.progress = [c, ta, fs];
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
                    let unit = flight
                        .job
                        .resources
                        .compute()?
                        .compute
                        .get(index)
                        .ok_or(EINVAL)?;
                    let ticks = [unit.its.r64(0)?, unit.its.r64(8)?, 0, 0];
                    command
                        .timestamp_outputs
                        .publish(ticks, super::hw::COMMAND_TIMESTAMP_HZ);
                    compute += 1;
                }
                super::command::Command::Render(command) => {
                    let index = *passes.get(render).ok_or(EINVAL)? as usize;
                    let pass = flight
                        .job
                        .resources
                        .render()?
                        .passes
                        .get(index)
                        .ok_or(EINVAL)?;
                    let ticks = [
                        pass.scene.r64(0x600)?,
                        pass.scene.r64(0x608)?,
                        pass.scene.r64(0x620)?,
                        pass.scene.r64(0x628)?,
                    ];
                    command
                        .timestamp_outputs
                        .publish(ticks, super::hw::COMMAND_TIMESTAMP_HZ);
                    render += 1;
                }
            }
        }
        Ok(())
    }

    /// A retired prefix: publication to retirement (including any wait behind its
    /// predecessor), event times, internal stage times of its members.
    fn stat_retire(&mut self, f: &Flight) {
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
        let s = &mut self.engine.stats;

        // Add one stage interval read from internal timestamp cells (start, end).
        let mut stage = |o: &super::object::FwObject, off: usize, sum: &mut u64| {
            let rd = |x: usize| o.r32(x).unwrap_or(0) as u64;
            let start = rd(off + 4) << 32 | rd(off);
            let end = rd(off + 12) << 32 | rd(off + 8);
            if start == 0 || end < start {
                return;
            }
            *sum = sum.saturating_add(end - start);
            lo = lo.min(start);
            hi = hi.max(end);
        };
        for k in 0..f.renders as usize {
            let Some(arena) = f.job.resources.render.as_ref() else {
                return;
            };
            let p = &arena.passes[f.claims.pass[k] as usize];
            stage(&p.scene, 0x600, &mut s.ta_ticks);
            stage(&p.scene, 0x620, &mut s.fs_ticks);
        }
        for k in 0..(f.computes as usize).min(f.claims.slot_count as usize) {
            let Some(arena) = f.job.resources.compute.as_ref() else {
                return;
            };
            let c = &arena.compute[f.claims.slot[k] as usize];
            stage(&c.its, 0, &mut s.cdm_ticks);
        }
        if hi > lo {
            s.span_ticks = s.span_ticks.saturating_add(hi - lo);
        }
        for b in 0..3 {
            let t = self.events.event_ns[b];
            let skip = if b == 2 {
                f.computes == 0
            } else {
                f.renders == 0
            };
            if skip {
                continue;
            }
            first = first.min(t);
            last = last.max(t);
        }
        if first > f.published_ns && last <= now {
            s.first_ns[kind] += first - f.published_ns;
            s.last_ns[kind] += last - f.published_ns;
        }
        s.count[0] += f.computes as u64;
        s.count[1] += f.renders as u64;
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
        let (g, r, e) = (&self.graph.compute, &self.graph.render, &self.engine);
        let ch = &self.channels;
        let rd = |o: &super::object::FwObject, off| o.r32(off).unwrap_or(0xdeadbeef);
        let state = self.session.regs.engine_state().unwrap_or_default();
        let f = e.oldest();
        let ms = f.map_or(0, |f| now_ns().wrapping_sub(f.published_ns) / 1_000_000);

        dev_info!(
            self.graph.dev.as_ref(),
            "G16: {}: ctx={} prefixes={} n={} target={:#x} user={:#x} fw={:#x} done={} read={} write={} head={} ch={}/{} events={}/{} errors={} busy={} fault={:#x} {}ms\n",
            what,
            f.map_or(0, |f| f.job.vm.slot),
            e.flights.len(),
            e.ordinal,
            e.target,
            rd(&g.user_stamp, 0),
            rd(&g.fw_stamp, 0),
            rd(&g.qstate, qstate::DONE),
            rd(&g.qstate, qstate::READ),
            rd(&g.qstate, qstate::WRITE),
            e.head,
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

    /// The `asahi-g16/stats` contents. Scripts parse the keys: keep them stable.
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
            slots.pass_scrubs, slots.tpc_clear_bytes, slots.tilemap_clear_bytes
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
