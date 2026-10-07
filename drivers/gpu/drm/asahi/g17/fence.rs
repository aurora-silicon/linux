// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware completion fences and the aggregate fence of one submit ioctl.
//!
//! These fences can complete out of order, so each has an independent timeline.
//! Signalling belongs outside the device lock, after timestamp and VM-status writes.

use super::context::{Context, WorkStateLease};
use crate::file::SyncItem;
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicUsize, Ordering};
use kernel::{
    bindings, c_str,
    dma_fence::{Fence, FenceContexts, FenceObject, FenceOps, RawDmaFence, UserFence},
    new_mutex,
    prelude::*,
    str::CStr,
    sync::{Arc, Mutex},
};

pub(crate) struct CompletionFence;

#[vtable]
impl FenceOps for CompletionFence {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("asahi")
    }

    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("g17-submit")
    }
}

/// Status of an explicitly signalled `CompletionFence`, without taking its lock.
/// This is only for our own fences; foreign dependencies still use the DMA-fence API.
pub(crate) fn completion_status(fence: &Fence) -> i32 {
    let raw = fence.raw();
    // SAFETY: the owned reference retains the naturally aligned unsigned-long
    // flags. DMA-fence updates published flags with atomic bitops; usize matches
    // unsigned long on the supported architectures. Acquire observes the error,
    // VM status and host timestamps published before the ordered signal bit.
    let flags = unsafe { AtomicUsize::from_ptr(core::ptr::addr_of_mut!((*raw).flags).cast()) }
        .load(Ordering::Acquire);
    if flags & (1usize << bindings::dma_fence_flag_bits_DMA_FENCE_FLAG_SIGNALED_BIT) == 0 {
        return 0;
    }
    // SAFETY: CompletionFence has no signaled callback. Its error is set before
    // signalling and is immutable afterwards. The acquired signal bit makes that
    // write visible; read once, as the DMA-fence status API does under its lock.
    let error = unsafe { core::ptr::read_volatile(core::ptr::addr_of!((*raw).error)) };
    if error == 0 { 1 } else { error }
}

/// Give independently retired work a timeline that fence merging cannot subsume.
pub(crate) fn independent(contexts: &FenceContexts) -> Result<UserFence<CompletionFence>> {
    let fence = contexts.new_fence(0, CompletionFence)?;
    // SAFETY: The fence is unique and unpublished: there are no waiters, callbacks or
    // shared references. The DMA-fence allocator supplies a globally unique context.
    unsafe { (*fence.raw()).context = bindings::dma_fence_context_alloc(1) };
    Ok(fence.into())
}

/// An immutable failure fence allocated before the queue is exposed to ioctls.
/// Reusing it for rejected submissions cannot retain or subsume accepted work.
pub(crate) fn allocation_failure(contexts: &FenceContexts) -> Result<Fence> {
    let fence = independent(contexts)?;
    fence.set_error(ENOMEM);
    fence.signal();
    Ok(Fence::from_fence(&fence))
}

/// All command members of an ioctl share its work-state node and aggregate fence.
/// The initial member keeps the aggregate unsignalled until enqueue finishes.
#[pin_data]
pub(crate) struct Submission {
    fence: UserFence<CompletionFence>,
    /// Successful retirement drops only this bookkeeping owner; runtime owners
    /// and failed submissions keep their work-state allocation.
    #[pin]
    work_state: Mutex<Option<Arc<WorkStateLease>>>,
    remaining: AtomicU32,
    error: AtomicI32,
}

impl Submission {
    fn new(fence: UserFence<CompletionFence>, context: Arc<Context>) -> Result<Arc<Self>> {
        let work_state = Arc::new(WorkStateLease::deferred(context), GFP_KERNEL)?;
        Arc::pin_init(
            pin_init!(Self {
                fence,
                work_state <- new_mutex!(Some(work_state), "G17 submission work state"),
                remaining: AtomicU32::new(1),
                error: AtomicI32::new(0),
            }),
            GFP_KERNEL,
        )
    }

    pub(crate) fn fence(&self) -> Fence {
        Fence::from_fence(&self.fence)
    }

    /// Add one command before enqueue ends. The parser bounds the count to 64.
    pub(crate) fn member(self: &Arc<Self>) -> Member {
        self.remaining.fetch_add(1, Ordering::Relaxed);
        Member {
            submission: self.clone(),
            complete: AtomicBool::new(false),
        }
    }

    fn member_done(&self, result: Result) {
        if let Err(error) = result {
            let _ = self.error.compare_exchange(
                0,
                error.to_errno(),
                Ordering::AcqRel,
                Ordering::Acquire,
            );
        }
        if self.remaining.fetch_sub(1, Ordering::AcqRel) == 1 {
            let error = self.error.load(Ordering::Acquire);
            if error != 0 {
                self.fence.set_error(Error::from_errno(error));
            } else {
                // Scheduler cleanup can wait behind submission on the same worker.
                // Once every member and enqueue have succeeded, retained packets
                // must not prevent new submissions from acquiring work-state nodes.
                let retired = self.work_state.lock().take();
                drop(retired);
            }
            self.fence.signal();
        }
    }

    fn finish_enqueue(&self, result: Result) {
        // A dropped, never-enqueued member reports cancellation. The ioctl's actual
        // failure takes precedence, while published members still retain their work.
        if let Err(error) = result {
            self.error.store(error.to_errno(), Ordering::Release);
        }
        self.member_done(result);
    }
}

/// One command's contribution to its submission, including construction failure.
pub(crate) struct Member {
    submission: Arc<Submission>,
    complete: AtomicBool,
}

impl Member {
    pub(crate) fn work_state(&self) -> Result<Arc<WorkStateLease>> {
        self.submission
            .work_state
            .lock()
            .as_ref()
            .cloned()
            .ok_or(ECANCELED)
    }

    pub(crate) fn complete(&self, result: Result) {
        if !self.complete.swap(true, Ordering::AcqRel) {
            self.submission.member_done(result);
        }
    }
}

impl Drop for Member {
    fn drop(&mut self) {
        self.complete(Err(ECANCELED));
    }
}

fn install(outputs: impl Iterator<Item = SyncItem>, fence: &Fence) {
    for mut sync in outputs {
        if let Some(chain) = sync.chain_fence.take() {
            sync.syncobj.add_point(chain, fence, sync.timeline_value);
        } else {
            sync.syncobj.replace_fence(Some(fence));
        }
    }
}

/// Keeps resolved output syncobjs reachable on every fallible submission path.
/// Input syncobjs stay in the same vector, avoiding a separate output allocation.
pub(crate) struct Outputs {
    fallback: UserFence<CompletionFence>,
    syncs: KVec<SyncItem>,
    input_count: usize,
    submission: Option<Arc<Submission>>,
    result: Result,
}

impl Outputs {
    pub(crate) fn new(
        contexts: &FenceContexts,
        mut syncs: KVec<SyncItem>,
        input_count: usize,
        allocation_failure: &Fence,
    ) -> Result<Self> {
        if input_count > syncs.len() {
            return Err(EINVAL);
        }
        let fallback = match independent(contexts) {
            Ok(fence) => fence,
            Err(error) => {
                // Syncobjs and timeline-chain storage are already resolved.
                // No work has been accepted; publish the pre-signaled ENOMEM
                // fence without another allocation before returning the error.
                install(syncs.drain(input_count..), allocation_failure);
                return Err(error);
            }
        };
        Ok(Self {
            fallback,
            syncs,
            input_count,
            submission: None,
            result: Err(ECANCELED),
        })
    }

    pub(crate) fn inputs(&self) -> &[SyncItem] {
        &self.syncs[..self.input_count]
    }

    /// Publish the actual aggregate before enqueueing any command.
    pub(crate) fn publish(&mut self, context: Arc<Context>) -> Result<Arc<Submission>> {
        let submission = Submission::new(self.fallback.clone(), context)?;
        install(self.syncs.drain(self.input_count..), &submission.fence());
        self.submission = Some(submission.clone());
        Ok(submission)
    }

    /// The caller first records terminal VM errors and settles prior failed work.
    pub(crate) fn finish(mut self, result: Result) -> Result {
        self.result = result;
        result
    }
}

impl Drop for Outputs {
    fn drop(&mut self) {
        if let Some(submission) = self.submission.take() {
            submission.finish_enqueue(self.result);
            return;
        }
        if let Err(error) = self.result {
            self.fallback.set_error(error);
        }
        install(
            self.syncs.drain(self.input_count..),
            &Fence::from_fence(&self.fallback),
        );
        self.fallback.signal();
    }
}
