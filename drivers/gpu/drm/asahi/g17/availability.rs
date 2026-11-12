// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Resource-specific scheduler waits. A caller samples a domain before a failed try, then
//! registers against that sample after rolling back unpublished claims; the release that frees
//! the resource bumps the generation and signals exactly the waiters of its own domain.
//! Successful acquisition never allocates a fence, and a release without waiters costs one
//! lock round trip.

use super::fence::{self, CompletionFence};
use kernel::{
    c_str,
    dma_fence::{Fence, FenceContexts, RawDmaFence, UserFence},
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
};

struct Entry {
    fence: UserFence<CompletionFence>,
    next: Option<KBox<Entry>>,
}

struct State {
    generation: u64,
    waiters: Option<KBox<Entry>>,
}

#[pin_data]
pub(crate) struct Availability {
    #[pin]
    state: Mutex<State>,
    fences: FenceContexts,
}

impl Availability {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            try_pin_init!(Self {
                state <- new_mutex!(
                    State { generation: 0, waiters: None },
                    "g17::Availability"
                ),
                fences: FenceContexts::new(1, c_str!("aurora_resource"), kernel::static_lock_class!())?,
            }),
            GFP_KERNEL,
        )
    }

    /// Sample before checking capacity under the resource's own synchronization.
    pub(crate) fn observe(&self) -> u64 {
        self.state.lock().generation
    }

    pub(crate) fn has_waiters(&self) -> bool {
        self.state.lock().waiters.is_some()
    }

    /// A fence the next notification signals. None means capacity changed after the sample:
    /// retry instead of waiting. Called with no resource lock held. The recheck after allocation
    /// closes both the failed-check/registration and the fence-allocation/release races. A
    /// waiter that goes away leaves its entry for the next notification to signal.
    pub(crate) fn wait(&self, observed: u64) -> Result<Option<Fence>> {
        if self.observe() != observed {
            return Ok(None);
        }
        let fence = fence::independent(&self.fences)?;
        let mut entry = KBox::new(
            Entry { fence: fence.clone(), next: None },
            GFP_KERNEL,
        )?;
        let mut state = self.state.lock();
        if state.generation != observed {
            return Ok(None);
        }
        entry.next = state.waiters.take();
        state.waiters = Some(entry);
        Ok(Some(Fence::from_fence(&fence)))
    }

    /// Blocks until a notification after `observed`, for at most `timeout_ms`. For callers
    /// outside the scheduler that own no other wait.
    pub(crate) fn block(&self, observed: u64, timeout_ms: u32, interruptible: bool) -> Result {
        let Some(fence) = self.wait(observed)? else { return Ok(()); };
        // SAFETY: The owned fence keeps the raw DMA fence alive throughout the wait.
        let waited = unsafe {
            kernel::bindings::dma_fence_wait_timeout(
                fence.raw(),
                interruptible,
                kernel::time::msecs_to_jiffies(timeout_ms) as isize,
            )
        };
        if waited < 0 {
            return Err(Error::from_errno(waited as i32));
        }
        Ok(())
    }

    /// Wakes every waiter of this domain. Call after dropping the resource's own lock; fence
    /// callbacks run outside the domain lock.
    pub(crate) fn notify(&self) {
        self.advance();
    }

    /// [`Self::notify`] for a caller that sampled `observed` and is giving back its own claim,
    /// never published, before it waits. Returns the sample it waits on next: the new
    /// generation if nothing else changed the domain since `observed`, so its own notification
    /// does not end its wait at once, and `observed` otherwise, so it retries.
    pub(crate) fn notify_own(&self, observed: u64) -> u64 {
        if self.advance() == observed {
            observed.wrapping_add(1)
        } else {
            observed
        }
    }

    /// Notifies, returning the generation before this notification.
    fn advance(&self) -> u64 {
        let (previous, mut waiters) = {
            let mut state = self.state.lock();
            let previous = state.generation;
            state.generation = previous.wrapping_add(1);
            (previous, state.waiters.take())
        };
        while let Some(mut entry) = waiters {
            waiters = entry.next.take();
            entry.fence.signal();
        }
        previous
    }
}
