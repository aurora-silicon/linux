// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 firmware-initiated same-boot recovery.
//!
//! FirmwareData+0x4fb0 follows 1 -> host 2 -> firmware 3 -> host 0. At phase
//! 1 every affected snapshot identity is resolved to retained published work,
//! and unfinished TA TPC storage is cleared, before acknowledging 2. Generation
//! must remain unchanged through phase 3. No G15 kill controls are written.
//! A host timeout without this protocol fail-stops and retains published owners.

pub(crate) mod logic;
mod seams;

use kernel::{
    bindings, device,
    prelude::*,
    sync::Arc,
    time::{ClockSource, Monotonic},
};

use logic::{RecoveryAction, Retirement, SnapshotEntry};

use super::job::{Completions, Job};
use super::regs::wmb;
use super::{params, Firmware};

/// S1, the lowest powered row of the firmware DVFS table, as the Globals p-state selectors
/// express it (hundredths of a state). The host starts the firmware there too.
const FLOOR_SELECTOR: u32 = 100;

/// Monotonic time, ns.
pub(crate) fn now_ns() -> u64 {
    <Monotonic as ClockSource>::ktime_get() as u64
}

fn poll_sleep() {
    // SAFETY: Recovery runs under the sleepable device mutex, never a spinlock.
    unsafe { bindings::usleep_range_state(20, 50, bindings::TASK_UNINTERRUPTIBLE) };
}

fn barrier() {
    crate::mem::sync();
}

/// Queue completion once while preserving the job if allocation fails.
pub(crate) fn push_completion(done: &mut Completions, job: &Arc<Job>, result: Result) {
    if let Err(error) = result {
        job.latch_failure(error);
    }
    done.push(job, result);
}

const _: () = assert!(logic::RETIRED_COMPUTE_PIPES == super::hw::sizes::COMPUTE_PIPES);

pub(crate) struct Recovery {
    /// Render host deadlines, ordinary and long-job ([`logic::Deadlines`]).
    pub(crate) deadlines: logic::Deadlines,
    /// Per-compute-pipe host deadlines, ordinary and long-job, so progress on one pipe
    /// cannot mask a stalled flight on another compute pipe.
    pub(crate) compute_deadlines: [logic::Deadlines; super::hw::sizes::COMPUTE_PIPES],
    /// Render long-command warning deadline; diagnostic only.
    warning_deadline_ns: [u64; 2],
    /// Per-compute-pipe long-command warning deadlines; diagnostic only.
    compute_warning_deadline_ns: [u64; super::hw::sizes::COMPUTE_PIPES],
    pub(crate) long_commands: u64,
    pub(crate) publication_wait_deadline: Option<u64>,
    /// Recently retired compute pipe and TA/FS stamps, heap-boxed off the probe stack.
    pub(crate) last_retired: KBox<logic::RetiredStamps>,
    /// Queue engine order: render, compute. A quarantined/exhausted class stays offline.
    pub(crate) unavailable: [Option<Error>; 2],
    pub(crate) errors_seen: u32,
    pub(crate) statistics_seen: u32,
    pub(crate) receipt_fault: bool,
    pub(crate) failed: Option<Error>,
    pub(crate) recoveries: u64,
    pub(crate) hang_kills: u64,
    /// Affected job fences failed even though every normal owner receipt arrived.
    pub(crate) affected_full_receipts: u64,
    /// Passed aborts whose backing and VM lease remain quarantined until reboot.
    pub(crate) aborted_receipts: u64,
    window_ns: u64,
    window_count: u32,
    running: bool,
    pub(crate) fwlog_remaining: usize,
}

impl Recovery {
    pub(crate) fn new() -> Result<Self> {
        let mut retired = KBox::<logic::RetiredStamps>::new_uninit(GFP_KERNEL)?;
        logic::RetiredStamps::init_zero(&mut *retired);
        // SAFETY: `init_zero` initialized the entire `RetiredStamps` in place.
        let last_retired = unsafe { retired.assume_init() };
        Ok(Self {
            deadlines: logic::Deadlines::EXPIRED,
            compute_deadlines: [logic::Deadlines::EXPIRED; super::hw::sizes::COMPUTE_PIPES],
            warning_deadline_ns: [u64::MAX; 2],
            compute_warning_deadline_ns: [u64::MAX; super::hw::sizes::COMPUTE_PIPES],
            long_commands: 0,
            publication_wait_deadline: None,
            last_retired,
            unavailable: [None; 2],
            errors_seen: 0,
            statistics_seen: 0,
            receipt_fault: false,
            failed: None,
            recoveries: 0,
            hang_kills: 0,
            affected_full_receipts: 0,
            aborted_receipts: 0,
            window_ns: 0,
            window_count: 0,
            running: false,
            fwlog_remaining: super::channel::RECOVERY_FWLOG_CAP,
        })
    }

    pub(crate) fn failed(&self) -> Option<Error> {
        self.failed
    }

    pub(crate) fn needs_repoll(&self) -> bool {
        self.running
    }
}

impl Firmware {
    pub(crate) fn dev(&self) -> &device::Device {
        &self.pools.dev
    }

    pub(crate) fn renew_render_deadline(&mut self) {
        let now = now_ns();
        let renewed = logic::Deadlines::renew(now, params::deadline_ms(), 0);
        self.recovery.deadlines.any = renewed.any;
        self.recovery.deadlines.long_any = renewed.long_any;
        self.recovery.warning_deadline_ns[super::command::RENDER] =
            now.saturating_add(5_000_000_000);
    }

    pub(crate) fn renew_compute_pipe_deadline(&mut self, pipe: usize) {
        let pipe = pipe.min(super::hw::sizes::COMPUTE_PIPES - 1);
        let now = now_ns();
        let renewed = logic::Deadlines::renew(now, 0, params::compute_deadline_ms());
        self.recovery.compute_deadlines[pipe].compute = renewed.compute;
        self.recovery.compute_deadlines[pipe].long_compute = renewed.long_compute;
        self.recovery.compute_warning_deadline_ns[pipe] = now.saturating_add(5_000_000_000);
    }

    pub(crate) fn renew_compute_deadline(&mut self) {
        for pipe in 0..super::hw::sizes::COMPUTE_PIPES {
            self.renew_compute_pipe_deadline(pipe);
        }
    }

    pub(crate) fn renew_deadlines(&mut self) {
        self.renew_render_deadline();
        self.renew_compute_deadline();
    }

    fn new_recovery(&self) -> bool {
        self.events.errors != self.recovery.errors_seen
            || self.events.recovery_statistics != self.recovery.statistics_seen
    }

    pub(crate) fn recovery_pending(&self) -> Result<bool> {
        Ok(self.recovery.running
            || self.new_recovery()
            || self.firmware_r32(self.init.firmware_data().phase)? != 0)
    }

    pub(crate) fn recovery_idle(&self) -> Result<bool> {
        Ok(!self.recovery_pending()?)
    }

    pub(crate) fn recovery_counts(&self) -> (u64, u64) {
        (self.recovery.recoveries, self.recovery.hang_kills)
    }

    /// A full hardware receipt still fails an explicitly affected job. Merely
    /// suspect work can complete normally; the poll owner separately proves an
    /// abort before retiring it without a full receipt.
    pub(crate) fn retire_outcome_at(&mut self, index: usize, receipt: Retirement) -> Result {
        let affected = self
            .engine
            .flights
            .get(index)
            .is_some_and(|flight| flight.doomed);
        if receipt.job_failed(affected) {
            Err(EIO)
        } else {
            Ok(())
        }
    }

    /// Protocol errors and transport loss always retain uncertain owners.
    /// A deadline without a firmware recovery notification is not permission
    /// to manipulate undocumented preemption controls.
    #[inline(never)]
    pub(crate) fn check_hangs(&mut self, done: &mut Completions) -> Result {
        if let Some(error) = self.recovery.failed {
            return Err(error);
        }
        let fault = core::mem::take(&mut self.recovery.receipt_fault);
        if let Some(error) = self.transport_error() {
            self.fail_stop(error, done);
            return Err(error);
        }
        if self.recovery_pending()? {
            if !params::recovery() {
                self.fail_stop(EIO, done);
                return Err(EIO);
            }
            if let Err(error) = self.recover() {
                self.fail_stop(error, done);
                return Err(error);
            }
            return Ok(());
        }
        if fault {
            self.fail_stop(EIO, done);
            return Err(EIO);
        }
        if !self.engine.flights.is_empty() {
            const CP: usize = super::hw::sizes::COMPUTE_PIPES;
            self.progress();
            let now = now_ns();
            let mut has_render = false;
            let mut has_long_render = false;
            let mut has_compute = [false; CP];
            let mut has_long_compute = [false; CP];
            for flight in &self.engine.flights {
                if flight.renders != 0 {
                    has_render = true;
                    if flight.job.long() {
                        has_long_render = true;
                    }
                }
                if flight.computes != 0 {
                    let p = (flight.compute_pipe as usize).min(CP - 1);
                    if !has_compute[p] {
                        has_compute[p] = true;
                        has_long_compute[p] = flight.job.long();
                    }
                }
            }
            if has_render
                && logic::expired(
                    now,
                    self.recovery.warning_deadline_ns[super::command::RENDER],
                )
            {
                self.recovery.warning_deadline_ns[super::command::RENDER] = u64::MAX;
                self.recovery.long_commands = self.recovery.long_commands.saturating_add(1);
                dev_warn!(
                    self.dev(),
                    "M4 GPU render command has no completion-stamp progress for 5s; execution may still be active\n"
                );
            }
            let mut compute_expired = false;
            for p in 0..CP {
                if !has_compute[p] {
                    continue;
                }
                if logic::expired(now, self.recovery.compute_warning_deadline_ns[p]) {
                    self.recovery.compute_warning_deadline_ns[p] = u64::MAX;
                    self.recovery.long_commands = self.recovery.long_commands.saturating_add(1);
                    dev_warn!(
                        self.dev(),
                        "M4 GPU compute pipe {} command has no completion-stamp progress for 5s; execution may still be active\n",
                        p
                    );
                }
                let pipe_deadline = self.recovery.compute_deadlines[p]
                    .select(true, has_long_compute[p]);
                if logic::expired(now, pipe_deadline) {
                    compute_expired = true;
                }
            }
            // Ray traversal and AS builds: every flight of such a job (`Job::long`).
            let render_deadline = self.recovery.deadlines.select(false, has_long_render);
            if (has_render && logic::expired(now, render_deadline)) || compute_expired {
                dev_err!(
                    self.dev(),
                    "M4 GPU administrative command deadline exceeded without firmware recovery; retaining owners and fail-stop (not proof of quiescence)\n"
                );
                self.fail_stop(ETIMEDOUT, done);
                return Err(ETIMEDOUT);
            }
        }
        Ok(())
    }

    /// Class-local exhaustion is terminal until reboot but does not fault a healthy VM or
    /// disable the other hardware class. Publish it once, including a userspace notification.
    pub(crate) fn mark_class_unavailable(&mut self, class: usize, error: Error) {
        let Some(state) = self.recovery.unavailable.get_mut(class) else {
            return;
        };
        if state.is_some() {
            return;
        }
        *state = Some(error);
        let name = if class == super::command::COMPUTE {
            "compute"
        } else {
            "render"
        };
        dev_err!(self.dev(), "G16: {} class unavailable until reboot ({:?}); retained owners cannot be recycled safely\n", name, error);
        if let Some(shared) = &self.engine.shared {
            shared
                .wedged
                .store(true, core::sync::atomic::Ordering::Release);
        }
    }

    pub(crate) fn fail_stop(&mut self, error: Error, done: &mut Completions) {
        if self.recovery.failed.is_none() {
            self.recovery.failed = Some(error);
            self.floor_power(error);
        }
        self.engine.frozen = true;
        self.engine.coalesce_deadline_ns = 0;
        self.events.grow.clear();
        self.fail_flights(error, done);
        self.fail_all_vms(EIO);
        let pending = core::mem::take(&mut self.engine.pending);
        for job in pending.iter() {
            push_completion(done, job, Err(EIO));
            if self.job_partly_published(job) {
                self.retain(job);
            }
        }
    }

    /// A failed device must not stay at its last performance request: the J773g idled at 98 C
    /// after a fail-stop, against 83 C with the GPU idle. The Globals ceiling selectors are
    /// host-owned (`service_power` writes them at runtime and stops after a failure): drop them
    /// to S1 and log the one fail-stop summary. Nothing firmware-owned is touched, and no GPU
    /// register is read.
    fn floor_power(&mut self, error: Error) {
        let before = self.power.effective;
        let selectors = self.init.layout.pstate_selectors;
        let mut ok = 0usize;
        let mut failed = 0usize;
        if let Some(globals) = self.init.globals.as_ref() {
            // Write every selector without short-circuiting so a single write error cannot
            // leave the remaining selectors unfloored.
            for offset in selectors {
                match globals.w32(offset, FLOOR_SELECTOR) {
                    Ok(()) => ok += 1,
                    Err(write_err) => {
                        failed += 1;
                        dev_err!(
                            self.dev(),
                            "G16: fail-stop: failed to floor Globals +{:#x}: {:?}\n",
                            offset,
                            write_err
                        );
                    }
                }
            }
        }
        if ok != 0 {
            wmb();
        }
        let all_floored = ok == selectors.len() && failed == 0;
        if all_floored {
            self.power.effective = 1;
        }
        let outcome = if all_floored {
            "S1"
        } else if ok != 0 {
            "partial S1"
        } else {
            "unchanged (Globals unwritable)"
        };
        dev_err!(
            self.dev(),
            "G16: fail-stop ({:?}): host ceiling S{} -> {} ({}/{} selectors written), temperature {} mC\n",
            error,
            before,
            outcome,
            ok,
            selectors.len(),
            self.power.temperature_mc
        );
    }

    /// Flights own firmware-visible resources and slot claims. Failed-stop
    /// flights are retained whole, never released to the free slot lists.
    pub(crate) fn fail_flights(&mut self, error: Error, done: &mut Completions) {
        let flights = core::mem::take(&mut self.engine.flights);
        // A failed device never publishes again: release the memory floor now.
        self.power.release_clpc_now();
        for flight in flights.iter() {
            for job in Iterator::chain(core::iter::once(&flight.job), flight.group.iter()) {
                push_completion(done, job, Err(error));
                self.retain(job);
            }
        }
        core::mem::forget(flights);
    }

    pub(crate) fn retain(&mut self, job: &Arc<Job>) {
        if self
            .engine
            .retained
            .iter()
            .any(|kept| Arc::ptr_eq(kept, job))
        {
            return;
        }
        if self.engine.retained.push(job.clone(), GFP_NOWAIT).is_err() {
            core::mem::forget(job.clone());
        }
    }

    fn firmware_r32(&self, offset: usize) -> Result<u32> {
        self.init.control.as_ref().ok_or(ENODEV)?.r32(offset)
    }

    fn firmware_w32(&self, offset: usize, value: u32) -> Result {
        self.init.control.as_ref().ok_or(ENODEV)?.w32(offset, value)
    }

    fn firmware_generation(&self) -> Result<u64> {
        // The firmware is stopped while its published generation is consumed.
        let generation = self.init.firmware_data().generation;
        let lo = self.firmware_r32(generation)?;
        let hi = self.firmware_r32(generation + 4)?;
        Ok(u64::from(lo) | (u64::from(hi) << 32))
    }

    /// Resolve the snapshot while the firmware is stopped. A missing retained
    /// owner prevents acknowledgement: its failure cannot safely be attributed.
    fn mark_recovery(
        &mut self,
        stage: [u32; 2 + super::hw::sizes::COMPUTE_PIPES],
        event: u32,
        asid: Option<u32>,
    ) -> Result {
        let data = self.init.firmware_data();
        for i in 0..data.snapshot_entries {
            let offset = data.snapshot + i * 8;
            let flags = self.firmware_r32(offset)?;
            let target = self.firmware_r32(offset + 4)?;
            let Some(entry) = SnapshotEntry::decode(flags, target) else {
                break;
            };
            self.recovery_prepare(entry.flags, entry.event, entry.target, stage)?;
        }
        self.recovery_prepare_suspects(stage, event, asid)
    }

    /// Decodes and logs every non-zero fault bank captured while firmware is halted in phase 1,
    /// together with whether the faulting context matches any in-flight flight or open VM.
    #[inline(never)]
    fn log_recovery_faults(&self, scan: Option<&super::regs::FaultScan>, event: u32) {
        let Some(scan) = scan else {
            dev_warn!(
                self.dev(),
                "G16: fault_scan: GPU powered down (event={:#x}, flights={})\n",
                event,
                self.engine.flights.len()
            );
            return;
        };
        let mut nonzero = 0usize;
        for entry in scan.banks() {
            if entry.status == 0 {
                continue;
            }
            nonzero += 1;
            self.log_fault(entry);
            let ctx = ((entry.status >> 17) & 0x3f) as u32;
            let flight_matches = self
                .engine
                .flights
                .iter()
                .filter(|f| f.job.vm.slot == ctx)
                .count();
            let live_vm = self.vms.find_by_slot(ctx);
            dev_warn!(
                self.dev(),
                "G16: fault bank {} attribution: busy={} saved_bank={} ctx={} flight_matches={} live_vm={:?} event={:#x} flights={}\n",
                entry.bank,
                scan.busy,
                scan.saved_bank,
                ctx,
                flight_matches,
                live_vm,
                event,
                self.engine.flights.len()
            );
        }
        if nonzero == 0 {
            dev_warn!(
                self.dev(),
                "G16: fault_scan: all {} banks zero (busy={}, saved_bank={}, event={:#x}, flights={})\n",
                scan.banks().len(),
                scan.busy,
                scan.saved_bank,
                event,
                self.engine.flights.len()
            );
        }
    }

    fn recover(&mut self) -> Result {
        struct RecoveringGuard(Option<Arc<super::Shared>>);
        impl Drop for RecoveringGuard {
            fn drop(&mut self) {
                if let Some(shared) = &self.0 {
                    shared
                        .recovering
                        .store(false, core::sync::atomic::Ordering::Release);
                }
            }
        }
        if let Some(shared) = &self.engine.shared {
            shared
                .recovering
                .store(true, core::sync::atomic::Ordering::Release);
        }
        let _recovering = RecoveringGuard(self.engine.shared.clone());
        let start = self.events.recovery_started_ns.unwrap_or_else(now_ns);
        self.recovery.running = true;
        self.engine.frozen = true;
        let deadline = start.saturating_add(logic::RECOVERY_BUDGET_NS);
        let mut answered = false;
        let mut generation = 0;
        let mut last_phase = 0;
        loop {
            if let Some(error) = self.transport_error() {
                return Err(error);
            }
            let now = now_ns();
            if logic::expired(now, deadline) {
                let _ = self.drain_auxiliary_rings();
                // Which side stalled, and whether tiling or fragment work was left behind.
                let stage = self
                    .recovery_snapshot()
                    .unwrap_or([0; 2 + super::hw::sizes::COMPUTE_PIPES]);
                dev_err!(
                    self.dev(),
                    "M4 GPU recovery exceeded 1000 ms: {} (phase {}, TA {:#x}/{:#x}, FS {:#x}/{:#x}, {} flights)\n",
                    logic::Stall::of(last_phase, answered).describe(),
                    last_phase,
                    stage[0],
                    self.engine.ta_target,
                    stage[1],
                    self.engine.fs_target,
                    self.engine.flights.len()
                );
                return Err(ETIMEDOUT);
            }
            let phase = self.firmware_r32(self.init.firmware_data().phase)?;
            last_phase = phase;
            let current_generation = if phase == 1 || phase == 3 {
                self.firmware_generation()?
            } else {
                generation
            };
            match logic::action(phase, answered, current_generation == generation) {
                RecoveryAction::MarkAndAcknowledge => {
                    // Give the corresponding event up to 50 ms to arrive;
                    // statistics alone can still initiate a real phase-1 halt.
                    let have_event = self.events.errors != self.recovery.errors_seen;
                    if !have_event && now.saturating_sub(start) <= logic::EVENT_WAIT_NS {
                        self.drain_rings()?;
                        poll_sleep();
                        continue;
                    }
                    if !logic::admit(
                        now,
                        &mut self.recovery.window_ns,
                        &mut self.recovery.window_count,
                    ) {
                        dev_err!(self.dev(), "M4 GPU recovery storm: fail-stop\n");
                        return Err(EIO);
                    }
                    generation = current_generation;
                    let event = if have_event {
                        let event = self.events.last_error.ok_or(super::boot::eproto())?;
                        if event.generation().wrapping_add(1) != generation {
                            dev_err!(self.dev(), "M4 recovery event generation mismatch\n");
                            return Err(super::boot::eproto());
                        }
                        event.slot as u32
                    } else {
                        u32::MAX
                    };
                    // Firmware is now halted in phase 1: invalidate bound PB state and
                    // force a full scrub on the next claim of every compute and render slot,
                    // then capture stage frontiers and fault-bank state before answering 2
                    // (firmware may force stage targets while aborting work, and fault_scan
                    // writes the fault-bank selector so it must not run against active FW).
                    self.pools.invalidate();
                    self.slots.invalidate_scrub();
                    self.events.grow.clear();
                    let stage = self.recovery_snapshot()?;
                    // Powered down, the GPU holds no fault to read.
                    let scan = self.session.fault_scan()?;
                    self.log_recovery_faults(scan.as_ref(), event);
                    let _ = self.drain_auxiliary_rings();
                    let asid = scan
                        .as_ref()
                        .and_then(super::regs::FaultScan::first_latched)
                        .map(|fault| ((fault.status >> 17) & 0x3f) as u32)
                        .filter(|asid| *asid >= 1);
                    self.mark_recovery(stage, event, asid)?;
                    // Snapshot reads and all TPC repairs precede acknowledgement.
                    barrier();
                    self.firmware_w32(self.init.firmware_data().phase, 2)?;
                    barrier();
                    answered = true;
                }
                RecoveryAction::Resume => {
                    let _ = self.drain_auxiliary_rings();
                    barrier();
                    self.firmware_w32(self.init.firmware_data().phase, 0)?;
                    barrier();
                    self.recovery.recoveries += 1;
                    self.recovery.errors_seen = self.events.errors;
                    self.recovery.statistics_seen = self.events.recovery_statistics;
                    self.events.recovery_started_ns = None;
                    self.recovery.running = false;
                    self.recovery.fwlog_remaining = super::channel::RECOVERY_FWLOG_CAP;
                    self.engine.frozen = false;
                    self.renew_deadlines();
                    dev_warn!(
                        self.dev(),
                        "M4 GPU recovery generation {} resumed\n",
                        generation
                    );
                    return Ok(());
                }
                RecoveryAction::ProtocolError => {
                    dev_err!(
                        self.dev(),
                        "M4 recovery invalid phase {} (answered={})\n",
                        phase,
                        answered
                    );
                    return Err(super::boot::eproto());
                }
                RecoveryAction::Wait => {
                    if logic::phantom_phase0(
                        phase,
                        answered,
                        self.engine.flights.is_empty(),
                        now.saturating_sub(start),
                    ) {
                        let _ = self.drain_auxiliary_rings();
                        self.recovery.errors_seen = self.events.errors;
                        self.recovery.statistics_seen = self.events.recovery_statistics;
                        self.events.recovery_started_ns = None;
                        self.recovery.running = false;
                        self.recovery.fwlog_remaining = super::channel::RECOVERY_FWLOG_CAP;
                        self.engine.frozen = false;
                        dev_warn!(
                            self.dev(),
                            "M4 GPU idle recovery notification cleared without phase 1\n"
                        );
                        return Ok(());
                    }
                }
            }
            self.drain_rings()?;
            poll_sleep();
        }
    }
}
