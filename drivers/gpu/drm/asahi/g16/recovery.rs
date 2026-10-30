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
use super::{params, Firmware};

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

pub(crate) struct Recovery {
    pub(crate) deadline_ns: u64,
    pub(crate) compute_deadline_ns: u64,
    /// A long command warning is diagnostic, not evidence that the GPU has stopped.
    warning_deadline_ns: u64,
    pub(crate) long_commands: u64,
    pub(crate) publication_wait_deadline: Option<u64>,
    /// Last fully retired compute/TA/FS identity, for late recovery snapshots.
    pub(crate) last_retired: [Option<u32>; 3],
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
}

impl Recovery {
    pub(crate) fn new() -> Self {
        Self {
            deadline_ns: logic::EXPIRED,
            compute_deadline_ns: logic::EXPIRED,
            warning_deadline_ns: u64::MAX,
            long_commands: 0,
            publication_wait_deadline: None,
            last_retired: [None; 3],
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
        }
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

    pub(crate) fn renew_deadlines(&mut self) {
        let (deadline, compute) = logic::renew(
            now_ns(),
            params::deadline_ms(),
            params::compute_deadline_ms(),
        );
        self.recovery.deadline_ns = deadline;
        self.recovery.compute_deadline_ns = compute;
        self.recovery.warning_deadline_ns = now_ns().saturating_add(5_000_000_000);
    }

    fn new_recovery(&self) -> bool {
        self.events.errors != self.recovery.errors_seen
            || self.events.recovery_statistics != self.recovery.statistics_seen
    }

    pub(crate) fn recovery_pending(&self) -> Result<bool> {
        Ok(self.recovery.running || self.new_recovery()
            || self.firmware_r32(logic::PHASE)? != 0)
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
    pub(crate) fn retire_outcome(&mut self, receipt: Retirement) -> Result {
        let affected = self
            .engine
            .flights
            .first()
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
            self.progress();
            let now = now_ns();
            if logic::expired(now, self.recovery.warning_deadline_ns) {
                self.recovery.warning_deadline_ns = u64::MAX;
                self.recovery.long_commands = self.recovery.long_commands.saturating_add(1);
                dev_warn!(self.dev(), "M4 GPU command has no completion-stamp progress for 5s; execution may still be active\n");
            }
            let compute_only = self
                .engine
                .oldest()
                .is_some_and(|flight| flight.computes != 0 && flight.renders == 0);
            let deadline = if compute_only {
                self.recovery.compute_deadline_ns
            } else {
                self.recovery.deadline_ns
            };
            if logic::expired(now, deadline) {
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
        }
        self.engine.frozen = true;
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

    /// Flights own firmware-visible resources and slot claims. Failed-stop
    /// flights are retained whole, never released to the free slot lists.
    pub(crate) fn fail_flights(&mut self, error: Error, done: &mut Completions) {
        let flights = core::mem::take(&mut self.engine.flights);
        // A failed device never publishes again: release the memory floor now.
        self.power.report_work(false);
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
        let lo = self.firmware_r32(logic::GENERATION)?;
        let hi = self.firmware_r32(logic::GENERATION + 4)?;
        Ok(lo as u64 | ((hi as u64) << 32))
    }

    /// Resolve the snapshot while the firmware is stopped. A missing retained
    /// owner prevents acknowledgement: its failure cannot safely be attributed.
    fn mark_recovery(&mut self, stage: [u32; 3], event: u32, asid: Option<u32>) -> Result {
        for i in 0..logic::SNAPSHOT_ENTRIES {
            let offset = logic::SNAPSHOT + i * 8;
            let flags = self.firmware_r32(offset)?;
            let target = self.firmware_r32(offset + 4)?;
            let Some(entry) = SnapshotEntry::decode(flags, target) else {
                break;
            };
            self.recovery_prepare(entry.flags, entry.event, entry.target, stage)?;
        }
        self.recovery_prepare_suspects(stage, event, asid)
    }

    fn recover(&mut self) -> Result {
        let start = self.events.recovery_started_ns.unwrap_or_else(now_ns);
        if !logic::admit(
            start,
            &mut self.recovery.window_ns,
            &mut self.recovery.window_count,
        ) {
            dev_err!(self.dev(), "M4 GPU recovery storm: fail-stop\n");
            return Err(EIO);
        }
        self.recovery.running = true;
        self.engine.frozen = true;
        self.events.grow.clear();
        // These frontiers must be captured before the host answers 2: the
        // firmware can force the stage target while aborting a work item.
        let stage = self.recovery_snapshot()?;
        let fault = self.session.regs.engine_state()?;
        let asid = fault
            .status
            .iter()
            .find(|status| **status & 1 != 0)
            .map(|status| ((*status >> 17) & 0x3f) as u32)
            .filter(|asid| *asid >= 1);
        let deadline = start.saturating_add(logic::RECOVERY_BUDGET_NS);
        let mut answered = false;
        let mut generation = 0;
        loop {
            if let Some(error) = self.transport_error() {
                return Err(error);
            }
            let now = now_ns();
            if logic::expired(now, deadline) {
                dev_err!(self.dev(), "M4 GPU recovery exceeded 1000 ms\n");
                return Err(ETIMEDOUT);
            }
            let phase = self.firmware_r32(logic::PHASE)?;
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
                    self.mark_recovery(stage, event, asid)?;
                    // Snapshot reads and all TPC repairs precede acknowledgement.
                    barrier();
                    self.firmware_w32(logic::PHASE, 2)?;
                    barrier();
                    answered = true;
                }
                RecoveryAction::Resume => {
                    barrier();
                    self.firmware_w32(logic::PHASE, 0)?;
                    barrier();
                    self.recovery.recoveries += 1;
                    self.recovery.errors_seen = self.events.errors;
                    self.recovery.statistics_seen = self.events.recovery_statistics;
                    self.events.recovery_started_ns = None;
                    self.recovery.running = false;
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
                RecoveryAction::Wait => {}
            }
            self.drain_rings()?;
            poll_sleep();
        }
    }
}
