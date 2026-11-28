// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Jobs: one per SUBMIT, scheduled by a per-queue DRM scheduler and executed by the engine.
//!
//! - One `drm_sched` scheduler and entity per logical queue ([`Scheduler`], credit limit
//!   [`super::hw::sizes::SCHEDULER_CREDITS`], one credit per job); every input sync is a scheduler
//!   dependency and remains owned by the job. Its status is consumed before engine admission
//!   and checked again before every hardware command. The host priority
//!   lives in [`super::queue::QueueState::priority`] for the engine's pick: with one entity per
//!   scheduler, the entity priority orders nothing.
//! - Timeout [`super::hw::tuning::SCHED_TIMEOUT_MS`], always "no hang": the engine's
//!   progress-based deadlines own failure.
//! - A sticky VM failure rejects scheduler-delayed jobs even when all of their inputs succeeded.
//!   Firmware failure is latched under the device lock before another command can publish.
//! - `run` hands the job to the engine ([`super::Firmware::enqueue`], then
//!   [`super::Firmware::publish`]) under the device lock and returns the job's fence; FINISHED
//!   fences stay in queue order ([`super::fence::Timeline`]).
//! - A hardware failure latches its VM error before its fence signals ([`Job::complete`]).
//!   An inherited producer failure cancels its unexecuted accepted ordinals: its exact-prior
//!   hole is never reported as a successful receipt. Earlier accepted work and other queues
//!   retain a usable VM.
//!
//! # For the engine
//!
//! The engine owns an `Arc<Job>` from [`super::Firmware::enqueue`] on. It completes the job
//! exactly once with [`Job::complete`] (later calls are ignored), through [`Completions`]
//! collected under the device lock and [`complete_all`] after it is dropped. A job whose storage
//! the firmware may still reference stays in `Engine::retained` (failed jobs until reboot): its
//! [`mmu::VmJobGuard`] keeps the VM's mappings and its VM state keeps the VM's TTBAT slot until
//! the last `Arc<Job>` drops.

use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use kernel::{
    c_str,
    cpu::CpuId,
    cpumask::Cpumask,
    device,
    dma_fence::Fence,
    drm::sched,
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
    workqueue,
};

use super::{
    command::{Command, Commands, Order, Orders},
    fence::{FenceOrigin, FwWaits, InputFences, JobFence},
    hw,
    queue::QueueState,
    vm::VmState,
    Shared,
};
use crate::mmu;

/// Mapping retirement can run after the final outer Job reference disappears. Keep
/// the G16 binding lease alive until the mapping guard finishes draining; declaration
/// order drops the guard before releasing the VM state, independently of worker order.
struct MappingGuard {
    guard: mmu::VmJobGuard,
    _vm: Arc<VmState>,
}

impl core::ops::Deref for MappingGuard {
    type Target = mmu::VmJobGuard;

    fn deref(&self) -> &Self::Target {
        &self.guard
    }
}

/// A completion link allocated with its job, before scheduler acceptance.
struct CompletionLink {
    /// Claimed once for the lifetime of this job, including after it is drained.
    queued: bool,
    result: Result,
    next: Option<Arc<Job>>,
}

impl CompletionLink {
    const fn new() -> Self {
        Self {
            queued: false,
            result: Ok(()),
            next: None,
        }
    }
}

/// A job as the engine sees it.
#[pin_data]
pub(crate) struct Job {
    /// Submission ID (debugging).
    pub(crate) id: u64,
    /// The submitting queue (priority, service turn, horizon history).
    pub(crate) queue: Arc<QueueState>,
    /// The job's VM (TTBAT slot, status).
    pub(crate) vm: Arc<VmState>,
    /// Validated commands, in submission order (at most 64).
    pub(crate) commands: Commands,
    /// Any command traces rays or builds acceleration structures (`Command::long`).
    long: bool,
    pub(crate) resources: super::resources::Prepared,
    /// Immutable dependency/order ledger, one entry for every accepted command.
    orders: Orders,
    /// The job's hardware fence.
    pub(crate) fence: JobFence,
    /// The engine's bookkeeping; `progress.signaled` makes [`Job::complete`] act once.
    pub(crate) progress: super::engine::Progress,
    /// Inputs and resolved historical-prefix dependencies, held through every command.
    inputs: InputFences,
    /// Exported fence identity for same-GPU, same-VM cross-queue firmware handoff.
    pub(crate) origin: Arc<FenceOrigin>,
    /// Same-GPU, same-VM cross-queue waits resolved into firmware ring barriers.
    pub(crate) fw_waits: FwWaits,
    /// Bitmask of commands (`0..MAX_COMMANDS`) with full owner-retirement proof; an aborted
    /// receipt never sets its bit. Tracks per-command bits so concurrent compute/render
    /// commands within one job may retire in either hardware completion order.
    full_retired: AtomicU64,
    /// No future publication and every previously published owner fully retired. This proof
    /// is separate from an error result: an affected full receipt may still fail the job.
    terminal_retired: AtomicBool,
    /// Retains the VM's mappings (deferred unmaps, deferred VM close) while the firmware may
    /// access them.
    _guard: crate::cleanup::Deferred<MappingGuard>,
    /// Pushed to the scheduler: from now on the job counts as submitted work of its VM.
    accepted: AtomicBool,
    /// Handed to the engine (or refused at `run`): the scheduler no longer cancels it.
    started: AtomicBool,
    /// Only the completion queue touches this lock; never held across GPU, VM or fence calls.
    #[pin]
    completion: Mutex<CompletionLink>,
}

impl Job {
    #[expect(
        clippy::too_many_arguments,
        reason = "The job owns independently prepared command, mapping and fence resources."
    )]
    pub(crate) fn new(
        id: u64,
        queue: Arc<QueueState>,
        vm: Arc<VmState>,
        commands: Commands,
        orders: Orders,
        resources: super::resources::Prepared,
        fence: JobFence,
        guard: mmu::VmJobGuard,
        inputs: InputFences,
        origin: Arc<FenceOrigin>,
        fw_waits: FwWaits,
    ) -> Result<Arc<Self>> {
        if commands.len() != orders.len() {
            return Err(EINVAL);
        }
        let long = commands.iter().any(Command::long);
        let progress = super::engine::Progress::new(commands.len());
        let guard = MappingGuard {
            guard,
            _vm: vm.clone(),
        };
        Arc::pin_init(
            try_pin_init!(Self {
                id,
                queue,
                vm,
                commands,
                long,
                orders,
                resources,
                fence,
                progress,
                inputs,
                origin,
                fw_waits,
                full_retired: AtomicU64::new(0),
                terminal_retired: AtomicBool::new(false),
                // Scheduler free can race the final off-lock completion reference.
                // Prepare destruction work before acceptance; dropping a completed job
                // within SignallingGuard must not drain mappings or take reservations.
                _guard: crate::cleanup::Deferred::new(guard)?,
                accepted: AtomicBool::new(false),
                started: AtomicBool::new(false),
                completion <- new_mutex!(CompletionLink::new(), "g16::Job::completion"),
            }),
            GFP_KERNEL,
        )
    }

    /// The job gets the long execution allowance (ray traversal or acceleration-structure
    /// builds, `recovery::logic::LONG_DEADLINE_MS`).
    pub(crate) fn long(&self) -> bool {
        self.long
    }

    /// Latches a hardware/publication failure before any later work can be published.
    /// Called under the device lock; it deliberately does not signal the fence.
    pub(crate) fn latch_failure(&self, error: Error) {
        self.origin.mark_failed();
        if error == ECANCELED
            || (self.terminal_retired.load(Ordering::Acquire)
                && error != EIO
                && error != ENODEV
                && error != ETIMEDOUT)
        {
            // A proven never-published resource/validation failure leaves an accepted
            // ordinal hole, not a hardware fault in this VM. Preserve its raw error in
            // client status while closing only this logical queue.
            self.queue.cancel(&self.orders, self.progress.sent());
            self.vm.status.report_failure(error);
        } else {
            if !self.terminal_retired.load(Ordering::Acquire) {
                self._guard.quarantine();
            }
            self.vm.status.record(error);
        }
    }

    /// Completes the job: latches the VM status on failure, then signals the fence (after every
    /// earlier job of the queue). Called by the engine, without the device lock. Only the first
    /// call has an effect.
    ///
    /// A hardware failure makes the VM's error sticky: later SUBMITs, QUEUE_CREATEs and mapping changes
    /// of the VM are refused (EIO), and mapping changes stay blocked while the job lives (the
    /// engine retains a job the firmware may still reference).
    #[track_caller]
    pub(crate) fn complete(&self, result: Result) {
        self.finish(result, true);
    }

    /// Completes the job; `engine`: the engine had it, so a failure quarantines its mappings.
    /// ECANCELED cancels only its unexecuted ordinals, never making the VM sticky.
    #[track_caller]
    fn finish(&self, result: Result, engine: bool) {
        if self.progress.signaled.swap(true, Ordering::AcqRel) {
            return;
        }
        if let Err(error) = result {
            self.origin.mark_failed();
            if error == ECANCELED
                || ((!engine || self.terminal_retired.load(Ordering::Acquire))
                    && error != EIO
                    && error != ENODEV
                    && error != ETIMEDOUT)
            {
                if self.accepted.load(Ordering::Acquire) {
                    self.queue.cancel(&self.orders, self.progress.sent());
                }
                self.vm.status.report_failure(error);
            } else {
                if engine && !self.terminal_retired.load(Ordering::Acquire) {
                    // Before the error fence becomes visible (G17's rule).
                    self._guard.quarantine();
                }
                self.vm.status.record(error);
            }
        }
        self.fence.signal(result);
    }

    /// Whether this job has already been handed to the engine (inline or by drm_sched).
    pub(crate) fn is_started(&self) -> bool {
        self.started.load(Ordering::Acquire)
    }

    /// Attempts to submit a ready job directly from SUBMIT ioctl context, bypassing `drm_sched`.
    /// Only runs when all input fences are already signalled, the device and VM are healthy,
    /// and the GPU is neither sleeping nor recovering; returns `true` when the job was started
    /// inline (accepted or already finished with error), or `false` to fall back to `drm_sched`.
    pub(crate) fn try_start_inline(self: &Arc<Self>, shared: &Arc<Shared>) -> bool {
        if shared.crashed.load(Ordering::Acquire)
            || shared.sleeping.load(Ordering::Acquire)
            || shared.recovering.load(Ordering::Acquire)
            || self.vm.status.get() != 0
        {
            return false;
        }
        if let Some(order) = self.orders.first() {
            if self.queue.check(order).is_err() {
                return false;
            }
        }
        if super::fence::check_dependencies(&self.inputs).is_err() {
            return false;
        }
        if let Some(result) = shared.try_submit_inline(self.clone()) {
            self.started.store(true, Ordering::Release);
            if let Err(error) = result {
                self.finish(Err(if error == ENODEV { EIO } else { error }), false);
            }
            true
        } else {
            false
        }
    }

    /// Hands the job to the engine, or refuses a failed GPU or VM. A queued
    /// job may have been accepted before another command made its VM sticky.
    fn start(self: &Arc<Self>, shared: &Arc<Shared>) {
        if self.started.swap(true, Ordering::AcqRel) {
            return;
        }
        if shared.crashed.load(Ordering::Acquire) {
            self.finish(Err(EIO), false);
            return;
        }
        let error = self.vm.status.get();
        if error != 0 {
            self.finish(Err(Error::from_errno(error)), false);
            return;
        }
        if let Some(order) = self.orders.first() {
            if let Err(error) = self.queue.check(order) {
                self.finish(Err(error), false);
                return;
            }
        }
        // drm_sched waits for dependency signalling; consume the actual error status as well.
        // This may wait only outside Shared::state, before any firmware-visible publication.
        if super::fence::wait_dependencies(&self.inputs).is_err() {
            // No command executed, so this is not a fault in this VM. Cancel the queue
            // before signalling; later accepted ordinals must not jump this hole.
            self.queue.cancel(&self.orders, self.progress.sent());
            self.finish(Err(ECANCELED), false);
            shared.wake_publish();
            return;
        }
        // `Shared::submit`: enqueue, publish and rearm under the device lock,
        // completions after it.
        if let Err(error) = shared.submit(self.clone()) {
            // An engine that refuses work is a device failure for the job's VM.
            self.finish(Err(if error == ENODEV { EIO } else { error }), false);
        }
    }

    /// Returns the immutable dependency order for command `index`.
    pub(crate) fn command_order(&self, index: u32) -> Option<Order> {
        self.orders.get(index as usize).copied()
    }

    /// Whether command `index` is a ready, barrier-free compute command continuing a compute
    /// horizon at queue-local compute ordinal `expected`.
    pub(crate) fn can_fold_horizon_cmd(&self, index: u32, expected: u64) -> bool {
        if !self.fw_waits.is_empty()
            || self.terminal_retired.load(Ordering::Acquire)
            || self.vm.status.get() != 0
        {
            return false;
        }
        let Some(Command::Compute(_)) = self.commands.get(index as usize) else {
            return false;
        };
        let Some(order) = self.orders.get(index as usize) else {
            return false;
        };
        order.engine == super::command::COMPUTE
            && order.sequence == expected
            && order.wait_through == [None, None]
            && self.queue.check(order).is_ok()
            && super::fence::check_dependencies(&self.inputs).is_ok()
    }

    /// Whether every command of this job is a ready, barrier-free compute command continuing
    /// sequentially from `first_expected`.
    pub(crate) fn can_fold_horizon_job(&self, first_expected: u64) -> bool {
        let count = self.progress.count();
        if count == 0
            || !self.fw_waits.is_empty()
            || self.progress.sent() != 0
            || self.terminal_retired.load(Ordering::Acquire)
            || self.vm.status.get() != 0
            || super::fence::check_dependencies(&self.inputs).is_err()
        {
            return false;
        }
        for k in 0..count {
            let Some(expected) = first_expected.checked_add(u64::from(k)) else {
                return false;
            };
            let Some(Command::Compute(_)) = self.commands.get(k as usize) else {
                return false;
            };
            let Some(order) = self.orders.get(k as usize) else {
                return false;
            };
            if order.engine != super::command::COMPUTE
                || order.sequence != expected
                || order.wait_through != [None, None]
                || self.queue.check(order).is_err()
            {
                return false;
            }
        }
        true
    }

    /// Whether command `index` is a ready render command continuing a same-SUBMIT render batch
    /// at queue-local render ordinal `expected` (treating `expected - 1` as already published
    /// within the in-progress render flight).
    pub(crate) fn can_fold_render_cmd(&self, index: u32, expected: u64) -> bool {
        if !self.fw_waits.is_empty()
            || self.terminal_retired.load(Ordering::Acquire)
            || self.vm.status.get() != 0
        {
            return false;
        }
        let Some(Command::Render(_)) = self.commands.get(index as usize) else {
            return false;
        };
        let Some(order) = self.orders.get(index as usize) else {
            return false;
        };
        if order.engine != super::command::RENDER
            || order.sequence != expected
            || self.queue.check(order).is_err()
            || super::fence::check_dependencies(&self.inputs).is_err()
        {
            return false;
        }
        let completed = self
            .queue
            .completed
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        let mut published = self
            .queue
            .published
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        published[super::command::RENDER] =
            published[super::command::RENDER].max(expected.saturating_sub(1));
        order.ready(completed, published)
    }

    /// Whether every command of this job is a ready render command continuing a render
    /// horizon sequentially from `first_expected` (treating earlier passes in the in-progress
    /// render flight as already published).
    pub(crate) fn can_fold_render_horizon_job(&self, first_expected: u64) -> bool {
        let count = self.progress.count();
        if count == 0
            || !self.fw_waits.is_empty()
            || self.progress.sent() != 0
            || self.terminal_retired.load(Ordering::Acquire)
            || self.vm.status.get() != 0
            || super::fence::check_dependencies(&self.inputs).is_err()
        {
            return false;
        }
        let completed = self
            .queue
            .completed
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        let base_published = self
            .queue
            .published
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        for k in 0..count {
            let Some(expected) = first_expected.checked_add(u64::from(k)) else {
                return false;
            };
            let Some(Command::Render(_)) = self.commands.get(k as usize) else {
                return false;
            };
            let Some(order) = self.orders.get(k as usize) else {
                return false;
            };
            if order.engine != super::command::RENDER
                || order.sequence != expected
                || self.queue.check(order).is_err()
            {
                return false;
            }
            let mut published = base_published;
            published[super::command::RENDER] =
                published[super::command::RENDER].max(expected.saturating_sub(1));
            if !order.ready(completed, published) {
                return false;
            }
        }
        true
    }

    /// Checks the retained dependency set for each command immediately before publication.
    /// Never waits or signals fences under the device lock. The scheduler and start() have
    /// already waited; a pending fence asks the engine to defer rather than publish early.
    pub(crate) fn check_command_dependencies(&self, index: u32) -> Result {
        if self.terminal_retired.load(Ordering::Acquire) {
            return Err(EIO);
        }
        if index as usize >= self.commands.len() {
            return Err(EINVAL);
        }
        let status = self.vm.status.get();
        if status != 0 {
            return Err(Error::from_errno(status));
        }
        let order = self.orders.get(index as usize).ok_or(EINVAL)?;
        self.queue.check(order)?;
        super::fence::check_dependencies(&self.inputs)?;
        for wait in self.fw_waits.iter() {
            if wait.is_failed() {
                return Err(ECANCELED);
            }
            for engine in 0..2 {
                if let Some(seq) = wait.last_ordinal[engine] {
                    wait.queue.cancelled_through(engine, seq)?;
                    if wait.queue.published[engine].load(Ordering::Acquire) < seq {
                        return Err(EAGAIN);
                    }
                }
            }
        }
        let completed = self
            .queue
            .completed
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        let published = self
            .queue
            .published
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        if !order.ready(completed, published) {
            return Err(EAGAIN);
        }
        Ok(())
    }

    /// Advances exactly one queue-local engine ordinal after a full successful wire receipt.
    /// Called under the device lock before another command is considered. No allocation,
    /// fence callback or aggregate-job completion occurs here.
    /// Advances the queue's published frontier past command `index` (under the device
    /// lock, when its first producer became visible).
    pub(crate) fn command_published(&self, index: u32) {
        if let Some(order) = self.orders.get(index as usize) {
            self.queue.published[order.engine].fetch_max(order.sequence, Ordering::AcqRel);
        }
    }

    pub(crate) fn command_retired(&self, index: u32) -> Result {
        let order = self.orders.get(index as usize).ok_or(EINVAL)?;
        let previous = order.sequence.checked_sub(1).ok_or(EINVAL)?;
        if order.engine != super::command::COMPUTE {
            self.queue.completed[order.engine]
                .compare_exchange(
                    previous,
                    order.sequence,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                )
                .map_err(|_| EIO)?;
            return Ok(());
        }
        // Concurrent 2-pipe compute commands on the same logical queue may retire out of
        // sequence order. Record published out-of-order full receipts above `completed + 1`
        // in `compute_ooo` and advance `completed[COMPUTE]` across contiguous retired runs.
        let completed = self.queue.completed[super::command::COMPUTE].load(Ordering::Acquire);
        if order.sequence <= completed {
            return Err(EIO);
        }
        if order.sequence == completed + 1 {
            let mut next = order.sequence;
            let mut ooo = self.queue.compute_ooo.load(Ordering::Acquire);
            while ooo & 1 != 0 {
                next += 1;
                ooo >>= 1;
            }
            // Re-anchor remaining OOO bits above `next + 1` (since `next` advanced by at least 1
            // and bit 0 of `ooo` is now `next + 1`, which is 0).
            ooo >>= 1;
            self.queue.compute_ooo.store(ooo, Ordering::Release);
            self.queue.completed[super::command::COMPUTE]
                .compare_exchange(completed, next, Ordering::AcqRel, Ordering::Acquire)
                .map_err(|_| EIO)?;
            return Ok(());
        }
        let published = self.queue.published[super::command::COMPUTE].load(Ordering::Acquire);
        let shift = order.sequence - completed - 2;
        if order.sequence > published || shift >= 64 {
            return Err(EIO);
        }
        let bit = 1u64 << shift;
        let ooo = self.queue.compute_ooo.load(Ordering::Acquire);
        if ooo & bit != 0 {
            return Err(EIO);
        }
        self.queue.compute_ooo.store(ooo | bit, Ordering::Release);
        Ok(())
    }

    /// Records a unique full owner receipt independently of success/error attribution.
    /// Only the receipt owner may call this after successful resource-ledger retirement.
    pub(crate) fn note_full_retirement(&self, first: u32, count: u32) -> Result {
        let end = first.checked_add(count).ok_or(EOVERFLOW)?;
        if count == 0 || end > self.progress.sent() || end > 64 {
            return Err(EINVAL);
        }
        let bits = if count == 64 {
            u64::MAX
        } else {
            ((1u64 << count) - 1) << first
        };
        let mut current = self.full_retired.load(Ordering::Acquire);
        loop {
            if current & bits != 0 {
                return Err(EIO);
            }
            match self.full_retired.compare_exchange_weak(
                current,
                current | bits,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Ok(()),
                Err(observed) => current = observed,
            }
        }
    }

    /// Certifies a terminal job before publishing its error. The caller first removes it
    /// from publication and proves there are no other reachable flights. Counting only
    /// full receipts prevents an earlier aborted owner from being accidentally blessed.
    /// A never-published job has the same proof with all three counters equal to zero.
    pub(crate) fn mark_terminal_retired(&self) -> Result {
        let sent = self.progress.sent();
        let expected = if sent >= 64 {
            u64::MAX
        } else {
            (1u64 << sent) - 1
        };
        if self.progress.is_pending()
            || self.progress.next() != sent
            || self.full_retired.load(Ordering::Acquire) != expected
        {
            return Err(EIO);
        }
        self.terminal_retired.store(true, Ordering::Release);
        Ok(())
    }

    /// Whether this job holds a cross-queue firmware wait on `queue`.
    pub(crate) fn waits_on_queue(&self, queue: &Arc<QueueState>) -> bool {
        self.fw_waits.iter().any(|w| Arc::ptr_eq(&w.queue, queue))
    }

    /// Cancels a job the scheduler never ran (queue teardown, or a failed submission).
    fn cancel_unstarted(&self) {
        if self.started.swap(true, Ordering::AcqRel) {
            return;
        }
        if self.accepted.load(Ordering::Acquire) {
            self.finish(Err(ECANCELED), false);
        } else if !self.progress.signaled.swap(true, Ordering::AcqRel) {
            // A refused submission's job: its fence orders nothing and was never published.
            self.origin.mark_failed();
            self.fence.signal(Err(ECANCELED));
        }
    }
}

/// Allocation-free FIFO of jobs retired or failed under the device lock.
///
/// Each link belongs to its Job and was allocated before acceptance. Claiming the
/// link once prevents a job from belonging to two lists and avoids any OOM path
/// during retirement. Private link locks are never nested, and are released before
/// VM status, GPU locks or fence callbacks. The caller drains after the device lock
/// is dropped, through [`complete_all`].
pub(crate) struct Completions {
    head: Option<Arc<Job>>,
    tail: Option<Arc<Job>>,
}

impl Completions {
    pub(crate) const fn new() -> Self {
        Self {
            head: None,
            tail: None,
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.head.is_none()
    }

    /// Queues the first completion of `job`; subsequent attempts have no effect.
    pub(crate) fn push(&mut self, job: &Arc<Job>, result: Result) {
        {
            let mut link = job.completion.lock();
            if link.queued {
                return;
            }
            link.queued = true;
            link.result = result;
        }
        // The new job's lock is released before the old tail is locked. Every
        // completion mutex shares one lock class; no double locking is needed.
        if let Some(tail) = self.tail.take() {
            tail.completion.lock().next = Some(job.clone());
        } else {
            self.head = Some(job.clone());
        }
        self.tail = Some(job.clone());
    }

    fn pop(&mut self) -> Option<(Arc<Job>, Result)> {
        let job = self.head.take()?;
        let (next, result) = {
            let mut link = job.completion.lock();
            (link.next.take(), link.result)
        };
        self.head = next;
        if self.head.is_none() {
            self.tail = None;
        }
        Some((job, result))
    }
}

/// Completes jobs in FIFO order, without holding a completion or device lock.
#[track_caller]
pub(crate) fn complete_all(mut done: Completions) {
    while let Some((job, result)) = done.pop() {
        job.complete(result);
    }
}

/// The scheduler's view of a job.
struct SchedJob {
    job: Arc<Job>,
    shared: Arc<Shared>,
    /// Count of queued (unstarted) `drm_sched` jobs on this queue. Decremented once when
    /// `run()`, `cancel()`, or `drop()` completes so inline submission preserves queue order
    /// without retaining `Arc<Job>` in [`Scheduler`].
    pending_sched: Arc<core::sync::atomic::AtomicUsize>,
    counted: bool,
}

impl SchedJob {
    fn finish_pending(&mut self) {
        if self.counted {
            self.counted = false;
            self.pending_sched.fetch_sub(1, Ordering::AcqRel);
        }
    }
}

impl sched::JobImpl for SchedJob {
    type TimeoutData = ();

    const MODULE: Option<&'static kernel::ThisModule> =
        Some(kernel::module::this_module::<crate::LocalModule>());

    fn timeout_data(&self) {}

    fn run(job: &mut Self) -> Result<Option<Fence>> {
        let fence = job.job.fence.fence();
        job.job.start(&job.shared);
        job.finish_pending();
        Ok(Some(fence))
    }

    /// The engine's own deadlines own failure; late delivery is not a hang.
    fn timed_out(_data: &mut (), _finished: bool) -> sched::Status {
        sched::Status::NoHang
    }

    fn cancel(job: &mut Self) {
        job.job.cancel_unstarted();
        job.finish_pending();
    }
}

impl Drop for SchedJob {
    fn drop(&mut self) {
        // Entity teardown frees jobs it never ran without calling `cancel`.
        self.job.cancel_unstarted();
        self.finish_pending();
    }
}

/// The scheduler and entity of one logical queue.
pub(crate) struct Scheduler {
    // Dropped first: the entity flush waits until every queued job has run (QUEUE_DESTROY
    // drains), and only then the scheduler is finalized.
    entity: sched::Entity<SchedJob>,
    _scheduler: sched::Scheduler<SchedJob>,
    /// Number of jobs currently queued in `drm_sched` that have not yet started or cancelled.
    /// Inline submit is permitted only when zero, preserving strict queue order without
    /// retaining any `Arc<Job>` (or its `VmJobGuard` / prepared arenas).
    pending_sched: Arc<core::sync::atomic::AtomicUsize>,
}

impl Scheduler {
    pub(crate) fn new(dev: &device::Device) -> Result<Self> {
        // Keep drm_sched run/free work in the creating CPU's cluster (matching g17/job.rs).
        // Completion work still follows the per-submit submitter_cpu, not queue creation, and
        // an unbound workqueue restricted via apply_workqueue_attrs() falls back to
        // wq_unbound_cpumask if the cluster goes offline (failing soft if restrict() errors).
        let submit = workqueue::OwnedQueue::new_ordered_reclaim(c_str!("aurora_g16"))?;
        let _ = submit.restrict(Cpumask::cluster_of(CpuId::current()));
        let scheduler = sched::Scheduler::new_with_queues(
            dev,
            1,
            hw::sizes::SCHEDULER_CREDITS,
            0,
            hw::tuning::SCHED_TIMEOUT_MS,
            c_str!("aurora_g16"),
            Some(submit),
            None,
        )?;
        // With one run queue, only the kernel priority is valid.
        let entity = sched::Entity::new(&scheduler, sched::Priority::Kernel)?;
        let pending_sched = Arc::new(core::sync::atomic::AtomicUsize::new(0), GFP_KERNEL)?;
        Ok(Self {
            entity,
            _scheduler: scheduler,
            pending_sched,
        })
    }

    /// Pushes a job that waits for its retained inputs. When the predecessor has already
    /// started and all inputs are signalled, submits directly inline and bypasses `drm_sched`
    /// allocation and kworker wakeup. On failure the job is cancelled (not counted as submitted
    /// work of its VM) and the error is returned.
    pub(crate) fn push(&mut self, job: Arc<Job>, shared: Arc<Shared>) -> Result {
        shared.note_submitter();
        let prev_started = self.pending_sched.load(Ordering::Acquire) == 0;
        let handle = job.clone();
        if prev_started {
            handle.accepted.store(true, Ordering::Release);
            if handle.try_start_inline(&shared) {
                return Ok(());
            }
            handle.accepted.store(false, Ordering::Release);
        }
        self.pending_sched.fetch_add(1, Ordering::AcqRel);
        let mut pending = self.entity.new_job(
            1,
            SchedJob {
                job,
                shared,
                pending_sched: self.pending_sched.clone(),
                counted: true,
            },
        )?;
        for fence in handle.inputs.iter() {
            pending.add_dependency(fence.clone())?;
        }
        let armed = pending.arm();
        handle.accepted.store(true, Ordering::Release);
        armed.push();
        Ok(())
    }
}
