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

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use kernel::{
    c_str, device,
    dma_fence::Fence,
    drm::sched,
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
};

use super::{
    command::{Command, Order},
    fence::JobFence,
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
    pub(crate) commands: KVec<Command>,
    pub(crate) resources: super::resources::Prepared,
    /// Immutable dependency/order ledger, one entry for every accepted command.
    orders: KVec<Order>,
    /// The job's hardware fence.
    pub(crate) fence: JobFence,
    /// The engine's bookkeeping; `progress.signaled` makes [`Job::complete`] act once.
    pub(crate) progress: super::engine::Progress,
    /// Inputs and resolved historical-prefix dependencies, held through every command.
    inputs: KVec<Fence>,
    /// Consecutive commands with full owner-retirement proof; an aborted receipt never counts.
    full_retired: AtomicU32,
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
        commands: KVec<Command>,
        orders: KVec<Order>,
        resources: super::resources::Prepared,
        fence: JobFence,
        guard: mmu::VmJobGuard,
        inputs: KVec<Fence>,
    ) -> Result<Arc<Self>> {
        if commands.len() != orders.len() {
            return Err(EINVAL);
        }
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
                orders,
                resources,
                fence,
                progress,
                inputs,
                full_retired: AtomicU32::new(0),
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

    /// Latches a hardware/publication failure before any later work can be published.
    /// Called under the device lock; it deliberately does not signal the fence.
    pub(crate) fn latch_failure(&self, error: Error) {
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

    /// Hands the job to the engine, or refuses a failed GPU or VM. A queued
    /// job may have been accepted before another command made its VM sticky.
    fn start(self: &Arc<Self>, shared: &Arc<Shared>) {
        self.started.store(true, Ordering::Release);
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
            return;
        }
        // `Shared::submit`: enqueue, publish and rearm under the device lock,
        // completions after it.
        if let Err(error) = shared.submit(self.clone()) {
            // An engine that refuses work is a device failure for the job's VM.
            self.finish(Err(if error == ENODEV { EIO } else { error }), false);
        }
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
        let completed = self
            .queue
            .completed
            .each_ref()
            .map(|value| value.load(Ordering::Acquire));
        if !order.ready(completed) {
            return Err(EAGAIN);
        }
        Ok(())
    }

    /// Advances exactly one queue-local engine ordinal after a full successful wire receipt.
    /// Called under the device lock before another command is considered. No allocation,
    /// fence callback or aggregate-job completion occurs here.
    pub(crate) fn command_retired(&self, index: u32) -> Result {
        let order = self.orders.get(index as usize).ok_or(EINVAL)?;
        let previous = order.sequence.checked_sub(1).ok_or(EINVAL)?;
        self.queue.completed[order.engine]
            .compare_exchange(
                previous,
                order.sequence,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .map_err(|_| EIO)?;
        Ok(())
    }

    /// Records a consecutive full owner receipt independently of success/error attribution.
    /// Only the receipt owner may call this after successful resource-ledger retirement.
    pub(crate) fn note_full_retirement(&self, first: u32, count: u32) -> Result {
        let end = first.checked_add(count).ok_or(EOVERFLOW)?;
        if count == 0 || end > self.progress.sent() {
            return Err(EINVAL);
        }
        self.full_retired
            .compare_exchange(first, end, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| EIO)?;
        Ok(())
    }

    /// Certifies a terminal job before publishing its error. The caller first removes it
    /// from publication and proves there are no other reachable flights. Counting only
    /// full receipts prevents an earlier aborted owner from being accidentally blessed.
    /// A never-published job has the same proof with all three counters equal to zero.
    pub(crate) fn mark_terminal_retired(&self) -> Result {
        let sent = self.progress.sent();
        if self.progress.is_pending()
            || self.progress.next() != sent
            || self.full_retired.load(Ordering::Acquire) != sent
        {
            return Err(EIO);
        }
        self.terminal_retired.store(true, Ordering::Release);
        Ok(())
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
}

impl sched::JobImpl for SchedJob {
    type TimeoutData = ();

    const MODULE: Option<&'static kernel::ThisModule> =
        Some(kernel::module::this_module::<crate::LocalModule>());

    fn timeout_data(&self) {}

    fn run(job: &mut Self) -> Result<Option<Fence>> {
        let fence = job.job.fence.fence();
        job.job.start(&job.shared);
        Ok(Some(fence))
    }

    /// The engine's own deadlines own failure; late delivery is not a hang.
    fn timed_out(_data: &mut (), _finished: bool) -> sched::Status {
        sched::Status::NoHang
    }

    fn cancel(job: &mut Self) {
        job.job.cancel_unstarted();
    }
}

impl Drop for SchedJob {
    fn drop(&mut self) {
        // Entity teardown frees jobs it never ran without calling `cancel`.
        self.job.cancel_unstarted();
    }
}

/// The scheduler and entity of one logical queue.
pub(crate) struct Scheduler {
    // Dropped first: the entity flush waits until every queued job has run (QUEUE_DESTROY
    // drains), and only then the scheduler is finalized.
    entity: sched::Entity<SchedJob>,
    _scheduler: sched::Scheduler<SchedJob>,
}

impl Scheduler {
    pub(crate) fn new(dev: &device::Device) -> Result<Self> {
        let scheduler = sched::Scheduler::new(
            dev,
            1,
            hw::sizes::SCHEDULER_CREDITS,
            0,
            hw::tuning::SCHED_TIMEOUT_MS,
            c_str!("aurora_g16"),
        )?;
        // With one run queue, only the kernel priority is valid.
        let entity = sched::Entity::new(&scheduler, sched::Priority::Kernel)?;
        Ok(Self {
            entity,
            _scheduler: scheduler,
        })
    }

    /// Pushes a job that waits for its retained inputs. On failure the job is cancelled (not counted as
    /// submitted work of its VM) and the error is returned.
    pub(crate) fn push(&mut self, job: Arc<Job>, shared: Arc<Shared>) -> Result {
        let handle = job.clone();
        let mut pending = self.entity.new_job(1, SchedJob { job, shared })?;
        for fence in handle.inputs.iter() {
            pending.add_dependency(fence.clone())?;
        }
        let armed = pending.arm();
        handle.accepted.store(true, Ordering::Release);
        armed.push();
        Ok(())
    }
}
