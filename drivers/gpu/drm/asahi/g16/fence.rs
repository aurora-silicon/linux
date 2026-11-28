// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Job fences.
//!
//! One fence per job (= per SUBMIT, the aggregate of its commands) on a per-queue context
//! ([`Timeline`]), signalled strictly in queue order. The VM status is latched (and mirrored)
//! before a failed job's fence signals ([`super::job::Job`]). Signalling happens after the device
//! lock is dropped.
//!
//! The engine retires a queue's jobs in order, but a later job may still be completed first
//! (a prefix failed before an earlier job's last prefix retired, or a never-run job cancelled at
//! queue teardown). Fences of one context must signal in order, so a [`Timeline`] holds a
//! completed fence back until every earlier fence of the queue has completed.

use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use kernel::{
    c_str,
    dma_fence::{Fence, FenceContexts, FenceObject, FenceOps, RawDmaFence, UserFence},
    new_mutex,
    prelude::*,
    str::CStr,
    sync::{Arc, LockClassKey, Mutex},
};

use super::queue::QueueState;
use crate::file::SyncItem;

/// Inline storage for the common 0-2 input fences, spilling to a heap `KVec` above 2.
pub(crate) enum InputFences {
    Empty,
    One([Fence; 1]),
    Two([Fence; 2]),
    Heap(KVec<Fence>),
}

impl InputFences {
    pub(crate) const fn new() -> Self {
        Self::Empty
    }

    pub(crate) fn push(&mut self, fence: Fence) -> Result {
        match self {
            Self::Heap(vec) => vec.push(fence, GFP_KERNEL)?,
            Self::Two(_) => {
                // Allocate before moving inline entries so OOM leaves `self` intact.
                let mut vec = KVec::with_capacity(4, GFP_KERNEL)?;
                if let Self::Two([a, b]) = core::mem::replace(self, Self::Empty) {
                    let _ = vec.push_within_capacity(a);
                    let _ = vec.push_within_capacity(b);
                    let _ = vec.push_within_capacity(fence);
                }
                *self = Self::Heap(vec);
            }
            Self::One(_) => {
                if let Self::One([a]) = core::mem::replace(self, Self::Empty) {
                    *self = Self::Two([a, fence]);
                }
            }
            Self::Empty => *self = Self::One([fence]),
        }
        Ok(())
    }
}

impl core::ops::Deref for InputFences {
    type Target = [Fence];

    fn deref(&self) -> &[Fence] {
        match self {
            Self::Empty => &[],
            Self::One(items) => items,
            Self::Two(items) => items,
            Self::Heap(vec) => vec,
        }
    }
}

/// Reads a retained dependency's completion status without waiting or invoking callbacks.
pub(crate) fn dependency_status(fence: &Fence) -> i32 {
    // SAFETY: The Fence reference keeps its dma_fence alive throughout the status read.
    unsafe { kernel::bindings::dma_fence_get_status(fence.raw()) }
}

/// Every command consumes errors as well as readiness, including failures in another VM.
pub(crate) fn check_dependencies(inputs: &[Fence]) -> Result {
    for fence in inputs {
        let status = dependency_status(fence);
        if status < 0 {
            // A producer failure says nothing about the consumer VM. All dependency
            // errnos are inherited cancellation, including allocation/validation errors.
            return Err(ECANCELED);
        }
        if status == 0 {
            return Err(EAGAIN);
        }
    }
    Ok(())
}

/// Final dependency wait at scheduler run, before acquiring the device mutex.
pub(crate) fn wait_dependencies(inputs: &[Fence]) -> Result {
    for fence in inputs {
        // SAFETY: Every input Fence remains owned by the Job during the wait. No GPU or
        // completion mutex is held, so the producer can publish and signal normally.
        let waited = unsafe {
            kernel::bindings::dma_fence_wait_timeout(
                fence.raw(),
                true,
                kernel::task::MAX_SCHEDULE_TIMEOUT,
            )
        };
        if waited < 0 {
            return Err(Error::from_errno(waited as i32));
        }
        if waited == 0 {
            return Err(ECANCELED);
        }
    }
    check_dependencies(inputs)
}

static TIMELINE_KEY: Pin<&LockClassKey> = kernel::static_lock_class!();
static ERROR_KEY: Pin<&LockClassKey> = kernel::static_lock_class!();
static JOB_FENCE_OPS: AtomicUsize = AtomicUsize::new(0);

/// Maximum distinct cross-queue firmware waits carried inline/on-heap by one job.
pub(crate) const MAX_FW_WAITS: usize = 8;

/// Immutable origin and atomic lifecycle flags attached to every G16 job fence so another
/// logical queue on the same GPU and VM can resolve a binary or timeline semaphore into an
/// in-firmware ring barrier instead of round-tripping through `drm_sched` and CPU retirement.
pub(crate) struct FenceOrigin {
    pub(crate) gpu_id: usize,
    pub(crate) vm_id: u64,
    pub(crate) queue: Arc<QueueState>,
    pub(crate) last_ordinal: [Option<u64>; 2],
    enqueued: AtomicBool,
    failed: AtomicBool,
}

impl FenceOrigin {
    pub(crate) fn new(
        gpu_id: usize,
        vm_id: u64,
        queue: Arc<QueueState>,
        last_ordinal: [Option<u64>; 2],
    ) -> Self {
        Self {
            gpu_id,
            vm_id,
            queue,
            last_ordinal,
            enqueued: AtomicBool::new(false),
            failed: AtomicBool::new(false),
        }
    }

    pub(crate) fn mark_enqueued(&self) {
        self.enqueued.store(true, Ordering::Release);
    }

    pub(crate) fn is_enqueued(&self) -> bool {
        self.enqueued.load(Ordering::Acquire)
    }

    pub(crate) fn mark_failed(&self) {
        self.failed.store(true, Ordering::Release);
    }

    pub(crate) fn is_failed(&self) -> bool {
        self.failed.load(Ordering::Acquire)
    }
}

/// One per-queue cross-queue firmware barrier target, coalescing multiple fences from the same
/// logical queue (e.g. from a timeline semaphore chain or repeated input syncs) to the highest
/// required render and compute ordinals on that queue.
pub(crate) struct FwWait {
    pub(crate) queue: Arc<QueueState>,
    pub(crate) last_ordinal: [Option<u64>; 2],
    origin: Arc<FenceOrigin>,
}

impl FwWait {
    pub(crate) fn is_failed(&self) -> bool {
        self.origin.is_failed()
    }
}

/// Cross-queue firmware waits resolved at `SUBMIT` (inline for 0-1 queues, bounded to
/// [`MAX_FW_WAITS`] distinct logical queues).
pub(crate) enum FwWaits {
    Empty,
    One([FwWait; 1]),
    Heap(KVec<FwWait>),
}

impl FwWaits {
    fn push_unique(&mut self, origin: Arc<FenceOrigin>) -> Result {
        let existing = match self {
            Self::Empty => None,
            Self::One(items) => items
                .iter_mut()
                .find(|wait| Arc::ptr_eq(&wait.queue, &origin.queue)),
            Self::Heap(vec) => vec
                .iter_mut()
                .find(|wait| Arc::ptr_eq(&wait.queue, &origin.queue)),
        };
        if let Some(wait) = existing {
            let mut newer = false;
            for engine in 0..2 {
                if let Some(seq) = origin.last_ordinal[engine] {
                    match wait.last_ordinal[engine] {
                        Some(cur) if seq > cur => {
                            wait.last_ordinal[engine] = Some(seq);
                            newer = true;
                        }
                        None => {
                            wait.last_ordinal[engine] = Some(seq);
                            newer = true;
                        }
                        _ => {}
                    }
                }
            }
            if newer {
                wait.origin = origin;
            }
            return Ok(());
        }
        if self.len() >= MAX_FW_WAITS {
            return Err(E2BIG);
        }
        let wait = FwWait {
            queue: origin.queue.clone(),
            last_ordinal: origin.last_ordinal,
            origin,
        };
        match self {
            Self::Empty => *self = Self::One([wait]),
            Self::One(_) => {
                let mut vec = KVec::with_capacity(4, GFP_KERNEL)?;
                if let Self::One([first]) = core::mem::replace(self, Self::Empty) {
                    let _ = vec.push_within_capacity(first);
                    let _ = vec.push_within_capacity(wait);
                }
                *self = Self::Heap(vec);
            }
            Self::Heap(vec) => vec.push(wait, GFP_KERNEL)?,
        }
        Ok(())
    }
}

impl core::ops::Deref for FwWaits {
    type Target = [FwWait];

    fn deref(&self) -> &[FwWait] {
        match self {
            Self::Empty => &[],
            Self::One(items) => items,
            Self::Heap(vec) => vec,
        }
    }
}

/// Fence operations of G16 job fences.
pub(crate) struct JobFenceOps {
    origin: Arc<FenceOrigin>,
}

#[vtable]
impl FenceOps for JobFenceOps {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("aurora")
    }

    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("g16")
    }
}

/// Fence operations of pre-signalled refused-submission fences (distinct vtable from
/// [`JobFenceOps`] so refused error fences never match [`origin_from_raw`]).
struct RefusedFenceOps;

#[vtable]
impl FenceOps for RefusedFenceOps {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("aurora")
    }

    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("g16-refused")
    }
}

/// Resolves a G16 [`FenceOrigin`] from a raw `dma_fence` pointer if and only if its vtable
/// pointer matches the registered [`JobFenceOps`] vtable.
///
/// # Safety
///
/// `raw` must be null or point to a live, initialized `dma_fence` whose reference is held for
/// the duration of this call.
unsafe fn origin_from_raw(raw: *mut kernel::bindings::dma_fence) -> Option<Arc<FenceOrigin>> {
    let expected = JOB_FENCE_OPS.load(Ordering::Acquire);
    if expected == 0 || raw.is_null() {
        return None;
    }
    // SAFETY: Caller retains a live dma_fence reference. `JobFenceOps` has a distinct timeline
    // name and nontrivial payload drop (`Arc<FenceOrigin>`). No cast precedes the exact
    // registered vtable check, and `FenceObject` begins with its `dma_fence`.
    let object = unsafe {
        if (*raw).ops as usize != expected {
            return None;
        }
        &*(raw as *const FenceObject<JobFenceOps>)
    };
    Some(object.origin.clone())
}

/// Resolves `inputs` into same-GPU, same-VM in-firmware cross-queue waits outside the device
/// lock. Returns `None` if any leaf is failed, foreign, cross-VM, not yet enqueued, or exceeds
/// [`MAX_FW_WAITS`], leaving the caller to retain normal `drm_sched` / `InputFences` waits.
pub(crate) fn resolve_fw_waits(inputs: &[Fence], gpu_id: usize, vm_id: u64) -> Option<FwWaits> {
    if inputs.is_empty() {
        return Some(FwWaits::Empty);
    }
    let mut waits = FwWaits::Empty;
    for input in inputs {
        let status = dependency_status(input);
        if status < 0 {
            return None;
        }
        if status > 0 {
            continue;
        }
        let leaves = crate::g17::sync_input::collect_coalesced(input)?;
        for leaf in leaves.iter() {
            if leaf.status < 0 {
                return None;
            }
            if leaf.status > 0 {
                continue;
            }
            // SAFETY: `leaf` retains an owned reference to `leaf.fence` until dropped.
            let origin = unsafe { origin_from_raw(leaf.fence) }?;
            if origin.gpu_id != gpu_id
                || origin.vm_id != vm_id
                || !origin.is_enqueued()
                || origin.is_failed()
                || origin.last_ordinal == [None, None]
            {
                return None;
            }
            waits.push_unique(origin).ok()?;
        }
    }
    Some(waits)
}

/// Stack batch size for signalling completed fences outside `Timeline::pending`.
const SIGNAL_BATCH: usize = 8;

unsafe extern "C" {
    fn asahi_g17_signal_fence(fence: *mut kernel::bindings::dma_fence);
}

/// Signals a completed job fence, draining any attached cumulative output scopes on the
/// current CPU instead of bouncing each scope through `irq_work`.
fn signal_fence(fence: &UserFence<JobFenceOps>) {
    // SAFETY: `fence` is a live `dma_fence` reference kept alive for the call.
    unsafe { asahi_g17_signal_fence(fence.raw()) };
}

/// An accepted fence of a timeline that has not signalled yet.
struct Pending {
    fence: UserFence<JobFenceOps>,
    /// The result, once the job completed.
    result: Option<Result>,
}

/// The fence timeline of one logical queue.
#[pin_data]
pub(crate) struct Timeline {
    /// One context for job fences, signalled in creation order.
    jobs: FenceContexts,
    /// One context for the error fences of refused submissions, each signalled at creation.
    errors: FenceContexts,
    /// Serializes dma-fence signalling outside `pending` so fences of this context always
    /// signal in creation order without holding `pending` across callbacks.
    /// Lock order: `signaller` -> `pending` (never reversed; `next()` locks only `pending`).
    #[pin]
    signaller: Mutex<()>,
    /// Accepted fences that have not signalled, oldest first.
    #[pin]
    pending: Mutex<KVec<Pending>>,
    /// Pre-signalled ENOMEM fence for a refused submission when no error fence can be allocated.
    allocation_failure: Fence,
}

impl Timeline {
    pub(crate) fn new() -> Result<Arc<Self>> {
        let errors = FenceContexts::new(1, c_str!("aurora_g16_refused"), ERROR_KEY)?;
        let allocation_failure = error_fence(&errors, ENOMEM)?;
        Arc::pin_init(
            try_pin_init!(Self {
                jobs: FenceContexts::new(1, c_str!("aurora_g16_queue"), TIMELINE_KEY)?,
                errors,
                signaller <- new_mutex!((), "g16::Timeline::signaller"),
                pending <- new_mutex!(KVec::new(), "g16::Timeline::pending"),
                allocation_failure,
            }),
            GFP_KERNEL,
        )
    }

    /// Creates the fence of the next accepted job. Fences are created in submission order: the
    /// caller holds the queue (SUBMIT is serialized per queue).
    pub(crate) fn next(self: &Arc<Self>, origin: Arc<FenceOrigin>) -> Result<JobFence> {
        // SUBMIT already holds the logical queue, so allocating the next sequence before
        // taking pending preserves creation order. Fence completion needs pending too:
        // never enter reclaim while holding that mutex.
        let fence: UserFence<JobFenceOps> = self.jobs.new_fence(0, JobFenceOps { origin })?.into();
        // SAFETY: `fence` is a live `UserFence<JobFenceOps>` whose `dma_fence::ops` is static.
        let ops = unsafe { (*fence.raw()).ops as usize };
        let previous = JOB_FENCE_OPS.load(Ordering::Acquire);
        if previous == 0 {
            match JOB_FENCE_OPS.compare_exchange(0, ops, Ordering::AcqRel, Ordering::Acquire) {
                Ok(_) => {}
                Err(existing) if existing == ops => {}
                Err(_) => return Err(EIO),
            }
        } else if previous != ops {
            return Err(EIO);
        }
        let mut pending = self.pending.lock();
        pending.reserve(1, GFP_NOWAIT)?;
        pending
            .push_within_capacity(Pending {
                fence: fence.clone(),
                result: None,
            })
            .map_err(|_| ENOMEM)?;
        Ok(JobFence {
            fence,
            timeline: self.clone(),
        })
    }

    /// Records the result of `fence` and signals every completed fence at the head, in order,
    /// without holding `pending` across `dma_fence_signal` callbacks.
    fn complete(&self, fence: &UserFence<JobFenceOps>, result: Result) {
        let head_ready = {
            let mut pending = self.pending.lock();
            let Some(entry) = pending
                .iter_mut()
                .find(|entry| entry.fence.raw() == fence.raw())
            else {
                // Already signalled.
                return;
            };
            if entry.result.is_some() {
                return;
            }
            entry.result = Some(result);
            pending.first().is_some_and(|entry| entry.result.is_some())
        };
        if !head_ready {
            return;
        }
        let _signaller = self.signaller.lock();
        loop {
            let mut batch: [Option<(UserFence<JobFenceOps>, Result)>; SIGNAL_BATCH] =
                [const { None }; SIGNAL_BATCH];
            let (count, more) = {
                let mut pending = self.pending.lock();
                let ready = pending
                    .iter()
                    .take(SIGNAL_BATCH)
                    .take_while(|entry| entry.result.is_some())
                    .count();
                let mut idx = 0;
                for entry in pending.drain(..ready) {
                    if let Some(res) = entry.result {
                        batch[idx] = Some((entry.fence, res));
                        idx += 1;
                    }
                }
                let more = pending.first().is_some_and(|entry| entry.result.is_some());
                (idx, more)
            };
            for item in &mut batch[..count] {
                if let Some((fence, res)) = item.take() {
                    if let Err(error) = res {
                        fence.set_error(error);
                    }
                    signal_fence(&fence);
                }
            }
            if !more {
                break;
            }
        }
    }

    /// A signalled fence carrying `error`, for the output syncs of a refused submission.
    pub(crate) fn refused(&self, error: Error) -> Fence {
        error_fence(&self.errors, error).unwrap_or_else(|_| self.allocation_failure.clone())
    }
}

/// A fence on `contexts` signalled with `error`. Every fence of an error context signals at
/// creation, so the context's fences signal in order.
fn error_fence(contexts: &FenceContexts, error: Error) -> Result<Fence> {
    let fence = contexts.new_fence(0, RefusedFenceOps)?;
    fence.set_error(error);
    fence.signal();
    Ok(Fence::from_fence(&fence))
}

/// The hardware fence of one job.
pub(crate) struct JobFence {
    fence: UserFence<JobFenceOps>,
    timeline: Arc<Timeline>,
}

impl JobFence {
    /// Signals the fence, with an error on failure, after every earlier fence of the queue. Only
    /// the first call has an effect.
    pub(crate) fn signal(&self, result: Result) {
        self.timeline.complete(&self.fence, result);
    }

    /// A reference to the fence.
    pub(crate) fn fence(&self) -> Fence {
        Fence::from_fence(&self.fence)
    }
}

impl Drop for JobFence {
    fn drop(&mut self) {
        // Every fence of a timeline must complete, or no later fence of the queue would signal.
        // A no-op for a completed fence (a job that was never accepted, or a bug, is cancelled).
        self.timeline.complete(&self.fence, Err(ECANCELED));
    }
}

/// Installs a fence in the output syncs of a submission.
fn install(outputs: impl Iterator<Item = SyncItem>, fence: &Fence) {
    for mut sync in outputs {
        if let Some(chain) = sync.chain_fence.take() {
            sync.syncobj.add_point(chain, fence, sync.timeline_value);
        } else {
            sync.syncobj.replace_fence(Some(fence));
        }
    }
}

/// The syncs of one SUBMIT. Every output sync gets a fence: the job's fence once the job is
/// accepted, a signalled error fence when the submission is refused (the shared `file.rs`
/// contract of `tracked_submission`). Cumulative output bundles reuse the shared
/// `g17::sync_output::SignalBundle` scope chain.
pub(crate) struct Syncs {
    syncs: KVVec<SyncItem>,
    input_count: usize,
    cumulative: Option<crate::g17::sync_output::SignalBundle>,
}

impl Syncs {
    pub(crate) fn new(mut syncs: KVVec<SyncItem>, input_count: usize) -> Self {
        let cumulative = syncs.iter_mut().find_map(|sync| sync.cumulative.take());
        Self {
            syncs,
            input_count,
            cumulative,
        }
    }

    /// The input syncs (all resolved to fences or already signalled).
    pub(crate) fn inputs(&self) -> &[SyncItem] {
        &self.syncs[..self.input_count.min(self.syncs.len())]
    }

    /// Publishes the job's fence in every output sync before enqueuing the job. If publishing a
    /// cumulative bundle fails (`ENOMEM`), the bundle remains unpublished so [`Self::refuse`]
    /// installs a signalled error fence in every target.
    pub(crate) fn publish(&mut self, fence: &Fence) -> Result {
        let first = self.input_count.min(self.syncs.len());
        if let Some(bundle) = &mut self.cumulative {
            // G16 has its own power/DVFS path; never poke G17's non-M5 compute-waiter hook.
            bundle.publish(fence, false)?;
            self.cumulative = None;
            self.syncs.truncate(first);
            return Ok(());
        }
        install(self.syncs.drain(first..), fence);
        Ok(())
    }

    /// Publishes a signalled error fence in every output sync of a refused submission.
    pub(crate) fn refuse(mut self, timeline: &Timeline, error: Error) {
        if let Some(mut bundle) = self.cumulative.take() {
            bundle.fail(&timeline.refused(error));
            return;
        }
        let first = self.input_count.min(self.syncs.len());
        if first < self.syncs.len() {
            install(self.syncs.drain(first..), &timeline.refused(error));
        }
    }
}
