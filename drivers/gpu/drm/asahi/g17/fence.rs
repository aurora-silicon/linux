// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware completion fences and the aggregate fence of one submit ioctl.
//!
//! These fences can complete out of order, so each has an independent timeline.
//! Signalling belongs outside the device lock, after timestamp and VM-status writes.

use super::context::{Context, WorkStateLease};
use crate::file::SyncItem;
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use kernel::{
    bindings, c_str,
    dma_fence::{Fence, FenceContexts, FenceObject, FenceOps, RawDmaFence, UserFence},
    prelude::*,
    str::CStr,
    sync::Arc,
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

/// Give independently retired work a timeline that fence merging cannot subsume.
pub(crate) fn independent(contexts: &FenceContexts) -> Result<UserFence<CompletionFence>> {
    let fence = contexts.new_fence(0, CompletionFence)?;
    // SAFETY: The fence is unique and unpublished: there are no waiters, callbacks or
    // shared references. The DMA-fence allocator supplies a globally unique context.
    unsafe { (*fence.raw()).context = bindings::dma_fence_context_alloc(1) };
    Ok(fence.into())
}

/// All command members of an ioctl share its work-state node and aggregate fence.
/// The initial member keeps the aggregate unsignalled until enqueue finishes.
pub(crate) struct Submission {
    fence: UserFence<CompletionFence>,
    work_state: Arc<WorkStateLease>,
    remaining: AtomicU32,
    error: AtomicI32,
}

impl Submission {
    fn new(fence: UserFence<CompletionFence>, context: Arc<Context>) -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                fence,
                work_state: Arc::new(WorkStateLease::deferred(context), GFP_KERNEL)?,
                remaining: AtomicU32::new(1),
                error: AtomicI32::new(0),
            },
            GFP_KERNEL,
        )?)
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
    pub(crate) fn work_state(&self) -> Arc<WorkStateLease> {
        self.submission.work_state.clone()
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
        syncs: KVec<SyncItem>,
        input_count: usize,
    ) -> Result<Self> {
        if input_count > syncs.len() {
            return Err(EINVAL);
        }
        Ok(Self {
            fallback: independent(contexts)?,
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
