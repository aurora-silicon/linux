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

/// 26B123: where the firmware leaves a compute work's GPU start and end ticks (the TS records'
/// +0x34 names work +0x7dc; the ticks land at its +0x10 and +0x18).
const WORK_TICK_START: usize = 0x7ec;
const WORK_TICK_END: usize = 0x7f4;
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
        this.resnapped.store(true, Ordering::Release);
        this.poll_now();
        HrTimerRestart::NoRestart
    }
}

/// The `aurora-g16/tune` debugfs file: current tunables.
fn tune_show(_shared: &Arc<Shared>, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    super::params::tune::show(f)
}

/// The `aurora-g16/tune` debugfs file: `name=value` lines.
fn tune_write(_shared: &Arc<Shared>, reader: &mut kernel::uaccess::UserSliceReader) -> Result {
    let mut buf = [0u8; 128];
    let len = reader.len().min(buf.len());
    reader.read_slice(&mut buf[..len])?;
    let text = core::str::from_utf8(&buf[..len]).map_err(|_| EINVAL)?;
    for line in text.split(|c| c == '\n' || c == ' ' || c == ',') {
        if !line.trim().is_empty() {
            super::params::tune::set(line)?;
        }
    }
    Ok(())
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
    /// One poll pass (the poll work item).
    pub(crate) fn run_poll(self: Arc<Self>) {
        // The domain may be off. The sleep's end queues a pass.
        if self.sleeping.load(Ordering::Acquire) {
            return;
        }
        let signalling = kernel::dma_fence::SignallingGuard::new();
        let mut done = Completions::new();
        {
            let mut state = self.state.lock();
            if let Some(firmware) = state.as_deref_mut() {
                if firmware.engine.shared.is_none() {
                    firmware.engine.shared = Some(self.clone());
                }
                let evented = self.evented.swap(false, Ordering::AcqRel);
                let resnapped = self.resnapped.swap(false, Ordering::AcqRel);
                let trigger = R3::trigger(evented, resnapped);
                let s = &mut firmware.engine.stats;
                let (jobs, prefixes) = (s.jobs, s.prefixes.iter().sum::<u64>());
                s.r3.trigger = trigger;
                s.r3.kicks = self.kicks.load(Ordering::Relaxed);
                s.r3.kick_ns = self.kick_ns.load(Ordering::Relaxed);
                s.r3.passes[trigger] += 1;
                let polled = firmware.poll(evented, &mut done);
                let s = &mut firmware.engine.stats;
                s.r3.fences[trigger] += s.jobs - jobs;
                s.r3.retired[trigger] += s.prefixes.iter().sum::<u64>() - prefixes;
                match polled {
                    Ok(true) => self.rearm(firmware.engine.chan_wait),
                    // Idle: one more pass after a window gives the state back. A failed
                    // engine's poll no longer services power, so it has nothing to give back.
                    Ok(false)
                        if firmware.recovery.failed().is_none()
                            && firmware.power.utilization.decaying() =>
                    {
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
            firmware.note_submission(&job);
            let accepted = firmware.enqueue(job);
            // Nothing is published while a render is in flight: the poll pass after its
            // retirement publishes this job. Skipping the pass here keeps queued submissions
            // from holding the device lock (ring drains, health checks) on that path.
            let render_busy = firmware.engine.flights.iter().any(|f| f.renders != 0);
            if accepted.is_ok() && !render_busy {
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
            *handle = Some(self.clone().start(Delta::from_micros(i64::from(us))));
        }
        self.mod_poll(1);
    }

    /// Starts what the engine needs besides the firmware state: the `aurora-g16/stats` file.
    pub(crate) fn start_engine(self: &Arc<Self>) -> Result {
        let dir = debugfs::Dir::new(c_str!("aurora-g16"));
        let file = KBox::pin_init(
            dir.read_callback_file(c_str!("stats"), self.clone(), &stats_show),
            GFP_KERNEL,
        )?;
        let tune = KBox::pin_init(
            dir.read_write_callback_file(c_str!("tune"), self.clone(), &tune_show, &tune_write),
            GFP_KERNEL,
        )?;
        let mut state = self.state.lock();
        let firmware = state.as_deref_mut().ok_or(ENODEV)?;
        firmware.engine.shared = Some(self.clone());
        firmware.engine.debugfs = Some(file);
        firmware.engine.tune_file = Some(tune);
        Ok(())
    }
}

/// Native modular stage completion, including zero on 32-bit wrap.
pub(crate) fn stamp_reached(obs: u32, own: u32, tail: u32) -> bool {
    super::recovery::logic::stamp_reached(obs, own, tail)
}

/// Cursor `c` of a ring of `m` entries passed end `h` of entries from `b`, within tail `t`.
pub(crate) fn ring_passed(c: u32, b: u32, h: u32, t: u32, m: u32) -> bool {
    let d = |x: u32| x.wrapping_add(m).wrapping_sub(b) % m;
    let (dc, dh, dt) = (d(c), d(h), d(t));
    dh <= dc && dc <= dt
}

/// R3 diagnostic kit (not for merge): the receipt terms of one queue (compute, TA, FS), in
/// the order `stats` prints them.
const R3_TERMS: [&str; 15] = [
    "fw_stamp",
    "read",
    "write",
    "qinfo20",
    "qinfo24",
    "qinfo28",
    "busy",
    "pipe_read",
    "pipe_completion",
    "pipe_write",
    "notifier_stamp",
    "notifier_context",
    "counter",
    "ticks",
    "pools",
];
const R3_QUEUES: [&str; 3] = ["compute", "ta", "fs"];
const R3_TRIGGERS: [&str; 3] = ["kick", "hrtimer", "backup"];
/// Upper bounds (us) of the kick-to-full-receipt histogram; the last bucket is open.
const R3_HIST_US: [u64; 5] = [5, 25, 100, 250, 1000];

type R3Terms = [[bool; R3_TERMS.len()]; R3_QUEUES.len()];

/// R3 diagnostic kit (not for merge). Which receipt term keeps the oldest flight pending
/// when an EP 0x20 kick starts the pass, before anything retires in it; and what starts the
/// passes that signal fences.
#[derive(Default)]
pub(crate) struct R3 {
    /// The current pass's trigger; the kick count and last kick time when it started.
    pub(crate) trigger: usize,
    pub(crate) kicks: u64,
    pub(crate) kick_ns: u64,
    /// Passes, fences signalled and flights retired, by trigger.
    pub(crate) passes: [u64; 3],
    pub(crate) fences: [u64; 3],
    pub(crate) retired: [u64; 3],
    /// Kicks whose oldest flight was pending before anything retired; of those, still
    /// pending after the trailing polls; pending although every term read as met.
    pub(crate) kick_pending: u64,
    pub(crate) tail_pending: u64,
    pub(crate) kick_unexplained: u64,
    /// Failing terms of those flights, and those that failed alone.
    pub(crate) terms: [[u64; R3_TERMS.len()]; R3_QUEUES.len()],
    pub(crate) sole: [[u64; R3_TERMS.len()]; R3_QUEUES.len()],
    /// The flight (publication time) last found pending at a kick, and that kick's time.
    watch: u64,
    watch_kick_ns: u64,
    /// Kick to full receipt of those flights: count, sum, max, histogram, by the trigger of
    /// the pass that retired them.
    pub(crate) late: u64,
    pub(crate) late_ns: u64,
    pub(crate) late_max_ns: u64,
    pub(crate) late_hist: [u64; R3_HIST_US.len() + 1],
    pub(crate) late_by: [u64; 3],
    /// Full receipts in kick-started passes: count, sum and max of the time since the kick.
    pub(crate) kick_full: u64,
    pub(crate) kick_full_ns: u64,
    pub(crate) kick_full_max_ns: u64,
}

impl R3 {
    /// 0: an EP 0x20 kick, 1: the repoll hrtimer, 2: the delayed-work backup or another
    /// caller of the poll work.
    fn trigger(evented: bool, resnapped: bool) -> usize {
        if evented {
            0
        } else if resnapped {
            1
        } else {
            2
        }
    }

    fn write(&self, f: &mut fmt::Formatter<'_>, compute: u32, stage: u32) -> fmt::Result {
        for (name, counts) in [
            ("passes", &self.passes),
            ("fences", &self.fences),
            ("retired", &self.retired),
        ] {
            write!(f, "r3_{}:", name)?;
            for (trigger, n) in R3_TRIGGERS.iter().zip(counts.iter()) {
                write!(f, " {} {}", trigger, n)?;
            }
            writeln!(f)?;
        }
        writeln!(
            f,
            "r3_kick: kicks {} compute_events {} stage_events {} pending {} tail_pending {} unexplained {}",
            self.kicks,
            compute,
            stage,
            self.kick_pending,
            self.tail_pending,
            self.kick_unexplained
        )?;
        for (name, table) in [("terms", &self.terms), ("sole", &self.sole)] {
            for (queue, row) in R3_QUEUES.iter().zip(table.iter()) {
                write!(f, "r3_{}_{}:", queue, name)?;
                for (term, n) in R3_TERMS.iter().zip(row.iter()) {
                    write!(f, " {} {}", term, n)?;
                }
                writeln!(f)?;
            }
        }
        write!(
            f,
            "r3_late: count {} sum_ns {} max_ns {} by_kick {} by_hrtimer {} by_backup {} hist_us",
            self.late,
            self.late_ns,
            self.late_max_ns,
            self.late_by[0],
            self.late_by[1],
            self.late_by[2]
        )?;
        for (bound, n) in R3_HIST_US.iter().zip(self.late_hist.iter()) {
            write!(f, " <{} {}", bound, n)?;
        }
        writeln!(f, " more {}", self.late_hist[R3_HIST_US.len()])?;
        writeln!(
            f,
            "r3_kick_full: count {} sum_ns {} max_ns {}",
            self.kick_full, self.kick_full_ns, self.kick_full_max_ns
        )
    }
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
        let published = self
            .service_power()
            .and_then(|()| self.publish_checked(done));
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
        // R3: the flight found pending at the kick, before anything retired in this pass.
        let mut at_kick = evented;
        let mut kick_pending = None;

        loop {
            let mut lap = Lap::start();
            self.drain_rings()?;
            lap.lap(&mut self.engine.prof.drain_ns);
            // Recovery may force stage progress: authenticate it before any receipt.
            self.check_hangs(done)?;
            ret = self.retired();
            if core::mem::take(&mut at_kick) && matches!(ret, Ok(Retirement::Pending)) {
                kick_pending = self.r3_kick();
            }
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
        if kick_pending.is_some() && kick_pending == self.engine.oldest().map(|f| f.published_ns) {
            self.engine.stats.r3.tail_pending += 1;
        }
        self.recovery.receipt_fault = ret.is_err();
        Ok(())
    }

    /// R3: counts the failing receipt terms of the oldest flight, pending at a kick, and
    /// starts its kick-to-full-receipt timer. Returns its publication time.
    fn r3_kick(&mut self) -> Option<u64> {
        let f = self.engine.oldest()?;
        let id = f.published_ns;
        let terms = self.r3_terms(f).ok()?;
        let r = &mut self.engine.stats.r3;
        r.kick_pending += 1;
        let failing = terms.iter().flatten().filter(|&&failed| failed).count();
        if failing == 0 {
            r.kick_unexplained += 1;
        }
        for (queue, row) in terms.iter().enumerate() {
            for (term, &failed) in row.iter().enumerate() {
                if failed {
                    r.terms[queue][term] += 1;
                    if failing == 1 {
                        r.sole[queue][term] += 1;
                    }
                }
            }
        }
        if r.watch != id {
            r.watch = id;
            r.watch_kick_ns = r.kick_ns;
        }
        Some(id)
    }

    /// R3: every term of `f`'s receipt ([`Self::retired_snapshot`]), each read separately,
    /// true where it is not met.
    fn r3_terms(&self, f: &Flight) -> Result<R3Terms> {
        let mut t: R3Terms = Default::default();
        let e = &self.engine;
        let busy = self.init.abi.qinfo().busy;
        if f.computes != 0 {
            let g = &self.graph.compute;
            let c = &mut t[0];
            let pipe = self.channels.channel(Channel::Compute);
            if e.flights.len() > 1 {
                // The passed receipt of a flight with others queued behind it.
                let cap = super::hw::sizes::QUEUE_CAP;
                let passed = |cursor| ring_passed(cursor, f.c_start, f.head, e.head, cap);
                c[0] = !stamp_reached(g.fw_stamp.r32(0)?, f.target, e.target);
                c[1] = !passed(g.qstate.r32(qstate::READ)?);
                if !super::hw::tuning::FW_CACHED {
                    for (k, offset) in [0x20, 0x24, 0x28].into_iter().enumerate() {
                        c[3 + k] = !passed(g.qinfo.r32(offset)?);
                    }
                }
                let slots = super::fw::channels::RING_SLOTS;
                let record = f.rec[2];
                let first = (record + slots - 1) % slots;
                let producer = self.channels.producer(Channel::Compute);
                c[7] = !ring_passed(pipe.read()?, first, record, producer, slots);
                c[8] = !ring_passed(pipe.completion()?, first, record, producer, slots);
                let stamp = g.notifier.r32(notifier_page::COMPUTE_STAMP)?;
                c[10] = !stamp_reached(stamp, f.target, e.target);
                c[11] = stamp == f.target
                    && g.notifier.r32(notifier_page::COMPUTE_CONTEXT)? != f.job.vm.slot;
            } else {
                c[0] = g.fw_stamp.r32(0)? != f.target;
                c[1] = g.qstate.r32(qstate::READ)? != f.head;
                c[2] = g.qstate.r32(qstate::WRITE)? != f.head;
                if !super::hw::tuning::FW_CACHED {
                    for (k, offset) in [0x20, 0x24, 0x28].into_iter().enumerate() {
                        c[3 + k] = g.qinfo.r32(offset)? != f.head;
                    }
                    c[6] = g.qinfo.r32(busy)? != 0;
                }
                c[7] = pipe.read()? != f.rec[2];
                c[8] = pipe.completion()? != f.rec[2];
                c[9] = pipe.write()? != f.rec[2];
                c[10] = g.notifier.r32(notifier_page::COMPUTE_STAMP)? != f.target;
                c[11] = g.notifier.r32(notifier_page::COMPUTE_CONTEXT)? != f.job.vm.slot;
            }
            let unit = f.job.resources.compute_unit(f.claims.slot[0] as usize)?;
            let submitted = unit.counter.r64(0)?;
            c[12] = submitted == 0
                || unit.work.r64(compute::DESCRIPTOR_COMPLETED)? != submitted
                || unit.work.r64(compute::DESCRIPTOR_COUNTER)? != unit.counter.va;
            let (start, end) = self.compute_ticks(unit)?;
            c[13] = start == 0 || end <= start;
        }
        if f.renders != 0 {
            let g = &self.graph.render;
            let stages = [
                (
                    Channel::Ta,
                    f.ta_target,
                    f.ta_head,
                    f.rec[0],
                    notifier_page::TA_STAMP,
                    notifier::CONTEXT,
                ),
                (
                    Channel::Fs,
                    f.fs_target,
                    f.fs_head,
                    f.rec[1],
                    notifier_page::FS_STAMP,
                    notifier_page::FS_RECORD + notifier::CONTEXT,
                ),
            ];
            for (q, (channel, target, head, record, stamp, context)) in
                stages.into_iter().enumerate()
            {
                let r = &mut t[1 + q];
                let pipe = self.channels.channel(channel);
                r[0] = g.fw_stamp[q].r32(0)? != target;
                r[1] = g.qstate[q].r32(qstate::READ)? != head;
                r[2] = g.qstate[q].r32(qstate::WRITE)? != head;
                if !super::hw::tuning::FW_CACHED {
                    for (k, offset) in [0x20, 0x24, 0x28].into_iter().enumerate() {
                        r[3 + k] = g.qinfo[q].r32(offset)? != head;
                    }
                    r[6] = g.qinfo[q].r32(busy)? != 0;
                }
                r[7] = pipe.read()? != record;
                r[8] = pipe.completion()? != record;
                r[9] = pipe.write()? != record;
                r[10] = g.notifier.r32(stamp)? != target;
                r[11] = g.notifier.r32(context)? != f.job.vm.slot;
            }
            let k = f.claims.pass[0] as usize;
            let arena = f.job.resources.render()?;
            let unit = arena.passes.get(k).ok_or(EINVAL)?;
            for (q, base) in [(1, 0x600), (2, 0x620)] {
                let (start, end) = (unit.scene.r64(base)?, unit.scene.r64(base + 8)?);
                t[q][13] = start == 0 || end <= start;
            }
            t[2][14] = !arena.pools.retired(k)?;
        }
        Ok(t)
    }

    #[allow(dead_code)]
    fn r32(o: &super::object::FwObject, offset: usize) -> Result<u32> {
        o.r32(offset)
    }

    /// Exact M4 queue and submit-pipe proof for one serialized publication. Event
    /// notifications are wakeups; neither an event nor elapsed time releases an owner.
    /// A compute unit's GPU start/end ticks. The 26B123 firmware writes them into the work at
    /// +0x7ec/+0x7f4 (macOS 27.2, hv25: its ITS pair stays 0/0 while those hold the 0.77 ms of
    /// the job); 25G83 writes the ITS pair. The ITS pair is used whenever it holds an interval.
    fn compute_ticks(&self, unit: &super::slots::ComputeSlot) -> Result<(u64, u64)> {
        let (start, end) = (unit.its.r64(0)?, unit.its.r64(8)?);
        if (start != 0 && end > start) || self.session.adt.layout.start != super::hw::Start::MacOs27_2 {
            return Ok((start, end));
        }
        let w = &unit.work;
        let at = |o: usize| -> Result<u64> { Ok(w.r32(o)? as u64 | (w.r32(o + 4)? as u64) << 32) };
        Ok((at(WORK_TICK_START)?, at(WORK_TICK_END)?))
    }

    /// Stall diagnostics: every input of the oldest flight's compute receipt, so a stall names
    /// the condition that keeps it pending.
    pub(crate) fn log_retirement(&self, why: &str) {
        let Some(f) = self.engine.oldest() else {
            return;
        };
        let dev = self.dev();
        let x32 = |r: Result<u32>| r.unwrap_or(u32::MAX);
        let x64 = |r: Result<u64>| r.unwrap_or(u64::MAX);
        let g = &self.graph.compute;
        dev_err!(
            dev,
            "G16 {}: oldest flight target {} (engine {}) head {} rec {:?} computes {} renders {}; fw_stamp {}; qstate read {} write {}; qinfo {} {} {} busy {} (+0x80 {:#x}); notifier stamp {} context {} (vm slot {}); compute pipe read {} completion {} write {}\n",
            why,
            f.target,
            self.engine.target,
            f.head,
            f.rec,
            f.computes,
            f.renders,
            x32(g.fw_stamp.r32(0)),
            x32(g.qstate.r32(qstate::READ)),
            x32(g.qstate.r32(qstate::WRITE)),
            x32(g.qinfo.r32(0x20)),
            x32(g.qinfo.r32(0x24)),
            x32(g.qinfo.r32(0x28)),
            x32(g.qinfo.r32(self.init.abi.qinfo().busy)),
            x32(g.qinfo.r32(0x80)),
            x32(g.notifier.r32(notifier_page::COMPUTE_STAMP)),
            x32(g.notifier.r32(notifier_page::COMPUTE_CONTEXT)),
            f.job.vm.slot,
            x32(self.channels.channel(Channel::Compute).read()),
            x32(self.channels.channel(Channel::Compute).completion()),
            x32(self.channels.channel(Channel::Compute).write())
        );
        if f.computes == 0 {
            return;
        }
        let k = f.claims.slot[0] as usize;
        match f.job.resources.compute_unit(k) {
            Ok(unit) => dev_err!(
                dev,
                "G16 {}: compute unit {}: submitted {:#x} completed {:#x} counter {:#x} (counter va {:#x}) its start {:#x} end {:#x}; work ticks {:#x} {:#x} {:#x} {:#x}\n",
                why,
                k,
                x64(unit.counter.r64(0)),
                x64(unit.work.r64(compute::DESCRIPTOR_COMPLETED)),
                x64(unit.work.r64(compute::DESCRIPTOR_COUNTER)),
                unit.counter.va,
                x64(unit.its.r64(0)),
                x64(unit.its.r64(8)),
                x32(unit.work.r32(WORK_TICK_START)),
                x32(unit.work.r32(WORK_TICK_START + 4)),
                x32(unit.work.r32(WORK_TICK_END)),
                x32(unit.work.r32(WORK_TICK_END + 4))
            ),
            Err(e) => dev_err!(dev, "G16 {}: compute unit {}: {:?}\n", why, k, e),
        }
    }

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
            if info.r32(self.init.abi.qinfo().busy)? != 0 {
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

    /// In-order receipt of compute flight `f` with later flights queued behind it: the NC
    /// READ cursor (and with uncached queue info its consumers) passed its ring entries,
    /// and the submit pipe's read and completion cursors passed its record.
    fn queue_passed(
        &self,
        channel: Channel,
        state: &super::object::FwObject,
        info: &super::object::FwObject,
        f: &Flight,
    ) -> Result<bool> {
        let cap = super::hw::sizes::QUEUE_CAP;
        let tail = self.engine.head;
        if f.head >= cap || f.c_start >= cap || tail >= cap {
            return Err(EINVAL);
        }
        if !ring_passed(state.r32(qstate::READ)?, f.c_start, f.head, tail, cap) {
            return Ok(false);
        }
        if !super::hw::tuning::FW_CACHED {
            for offset in [0x20, 0x24, 0x28] {
                if !ring_passed(info.r32(offset)?, f.c_start, f.head, tail, cap) {
                    return Ok(false);
                }
            }
        }
        let slots = super::fw::channels::RING_SLOTS;
        let record = f.rec[2];
        let first = (record + slots - 1) % slots;
        let producer = self.channels.producer(channel);
        let pipe = self.channels.channel(channel);
        Ok(ring_passed(pipe.read()?, first, record, producer, slots)
            && ring_passed(pipe.completion()?, first, record, producer, slots))
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
        // Later compute flights queued behind this one: the queue, pipe and stamp cells
        // already moved on, so their receipt is "passed", in order; the newest flight keeps
        // the exact receipt.
        let behind = self.engine.flights.len() > 1;
        if f.computes != 0 && behind {
            let g = &self.graph.compute;
            let e = &self.engine;
            if !stamp_reached(g.fw_stamp.r32(0)?, f.target, e.target)
                || !self.queue_passed(Channel::Compute, &g.qstate, &g.qinfo, f)?
            {
                return Ok(Retirement::Pending);
            }
            let k = f.claims.slot[0] as usize;
            let unit = f.job.resources.compute_unit(k)?;
            let submitted = unit.counter.r64(0)?;
            let (start, end) = self.compute_ticks(unit)?;
            let stamp = g.notifier.r32(notifier_page::COMPUTE_STAMP)?;
            full &= stamp_reached(stamp, f.target, e.target)
                && (stamp != f.target
                    || g.notifier.r32(notifier_page::COMPUTE_CONTEXT)? == f.job.vm.slot)
                && submitted != 0
                && unit.work.r64(compute::DESCRIPTOR_COMPLETED)? == submitted
                && unit.work.r64(compute::DESCRIPTOR_COUNTER)? == unit.counter.va
                && start != 0
                && end > start;
        } else if f.computes != 0 {
            let g = &self.graph.compute;
            if g.fw_stamp.r32(0)? != f.target
                || !self.queue_receipt(Channel::Compute, &g.qstate, &g.qinfo, f.head, f.rec[2])?
            {
                return Ok(Retirement::Pending);
            }
            let k = f.claims.slot[0] as usize;
            let unit = f.job.resources.compute_unit(k)?;
            let submitted = unit.counter.r64(0)?;
            let (start, end) = self.compute_ticks(unit)?;
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

    /// Logs the fault bank of `state` when it holds a fault.
    #[allow(dead_code)]
    pub(crate) fn log_fault(&self, state: &EngineState) {
        let (bank, f, va) = (state.bank, state.status, state.address);
        if f != 0 {
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
            self.r3_full(&f);
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
                let committed = f.pb.as_ref().ok_or(EINVAL).and_then(|epoch| {
                    let k = timestamp_claims.pass[0] as usize;
                    self.pools.retire_binding(k, epoch)?;
                    let enabled = params::pb_reuse();
                    self.pools
                        .retire_epoch(enabled, k, f.pb_binding, result.is_ok());
                    Ok(())
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
            // The aborted storage stays mapped with its retained job and is never used again
            // (its VM keeps a sticky error); its units serve other storage, so one client's
            // fault does not take compute or rendering away from every other client.
            if f.renders != 0 {
                // Keep no epoch of the aborted render for reuse.
                self.pools.forget(timestamp_claims.pass[0] as usize);
            }
            // Its VM's sticky error before its units return to service: from here on no job
            // of that VM is published (`publish` refuses it), so nothing loads the aborted
            // storage again, also not before the job's own completion runs.
            f.job.latch_failure(EIO);
            self.retain(&f.job);
            self.slots.abandon(&mut f.claims);
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
        if result.is_err() {
            // Its queue's later commands queued behind it fail with it, as unpublished ones
            // are cancelled: they never report a successful receipt after this hole.
            for later in self.engine.flights.iter_mut() {
                if Arc::ptr_eq(&later.job.queue, &job.queue) {
                    later.doomed = true;
                }
            }
        }
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
            s.job_cmds += u64::from(p.count());
            s.job_max = s.job_max.max(u64::from(p.count()));
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
                    s.job_cmds += u64::from(p2.count());
                    s.job_max = s.job_max.max(u64::from(p2.count()));
                }
            }
            complete(done, j2, Ok(()));
        }
    }

    /// R3: a full receipt: the time since the kick that found it pending, and since the kick
    /// that started this pass.
    fn r3_full(&mut self, f: &Flight) {
        let now = now_ns();
        let r = &mut self.engine.stats.r3;
        if r.watch == f.published_ns && r.watch != 0 {
            let ns = now.saturating_sub(r.watch_kick_ns);
            r.late += 1;
            r.late_ns = r.late_ns.saturating_add(ns);
            r.late_max_ns = r.late_max_ns.max(ns);
            let us = ns / 1000;
            let bucket = R3_HIST_US.iter().position(|&bound| us < bound);
            r.late_hist[bucket.unwrap_or(R3_HIST_US.len())] += 1;
            r.late_by[r.trigger] += 1;
            r.watch = 0;
        }
        if r.trigger == 0 {
            let ns = now.saturating_sub(r.kick_ns);
            r.kick_full += 1;
            r.kick_full_ns = r.kick_full_ns.saturating_add(ns);
            r.kick_full_max_ns = r.kick_full_max_ns.max(ns);
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
                    let unit = flight.job.resources.compute_unit(index)?;
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
            if start == 0 || end < start {
                return;
            }
            f.job
                .queue
                .usage
                .add(engine, start, end, super::hw::COMMAND_TIMESTAMP_HZ as u32);
            *sum = sum.saturating_add(end - start);
            lo = lo.min(start);
            hi = hi.max(end);
        };
        for k in 0..f.renders as usize {
            let Some(arena) = f.job.resources.render.as_ref() else {
                return;
            };
            let p = &arena.passes[f.claims.pass[k] as usize];
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
            stage(&c.its, 0, &mut s.cdm_ticks, crate::usage::Engine::Compute);
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
        let (g, r, e) = (&self.graph.compute, &self.graph.render, &self.engine);
        let ch = &self.channels;
        let rd = |o: &super::object::FwObject, off| o.r32(off).unwrap_or(0xdeadbeef);
        let state = self
            .session
            .engine_state()
            .ok()
            .flatten()
            .unwrap_or_default();
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
                "{}_owners: fresh {} preparing {} published {} retired {} quarantined {} reusable {} abandoned {}",
                name, owners.fresh, owners.preparing, owners.published, owners.retired,
                owners.quarantined, owners.reusable, owners.abandoned
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
            super::params::tune::show(f)?;
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
        )?;
        let (r3, events) = (&self.engine.stats.r3, &self.events);
        r3.write(f, events.compute_events, events.stage_events)
    }
}
