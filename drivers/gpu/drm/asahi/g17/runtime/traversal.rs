// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Scheduler side of [`crate::g17::traversal`]: the device's admission state over job fences,
//! the question the scheduler asks it, and the look the inline publication paths take at it.

use super::Backend;
use crate::g17::{job::{Engine, Packet}, traversal::Admission};
use core::sync::atomic::{AtomicBool, Ordering};
use kernel::{
    dma_fence::{Fence, RawDmaFence},
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
};

/// A scheduler job, by the fence that signals when it ends.
#[derive(Clone)]
struct Job(Fence);

impl PartialEq for Job {
    fn eq(&self, other: &Self) -> bool {
        self.0.raw() == other.0.raw()
    }
}

impl Job {
    /// The job completed, failed or was cancelled.
    fn ended(&self) -> bool {
        // SAFETY: The owned fence keeps the raw DMA fence alive for the call.
        unsafe { kernel::bindings::dma_fence_get_status(self.0.raw()) != 0 }
    }
}

/// The device's admission state. An ended job's fence stays referenced until the next command
/// asks.
#[pin_data]
pub(crate) struct Gate {
    #[pin]
    state: Mutex<Admission<Job>>,
    /// Something is admitted or waiting; compute commands that trace no rays skip the lock
    /// otherwise.
    busy: AtomicBool,
}

impl Gate {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            try_pin_init!(Self {
                state <- new_mutex!(Admission::new(), "g17::traversal::Gate"),
                busy: AtomicBool::new(false),
            }),
            GFP_KERNEL,
        )
    }

    /// Admission of the command whose job fence is `mine`, of `client`. `None`: it may go
    /// on. `Some(fence)`: wait for that fence, then ask again.
    fn admit(&self, client: u64, traces_rays: bool, mine: &Fence) -> Option<Fence> {
        if !traces_rays && !self.busy.load(Ordering::Acquire) {
            return None;
        }
        let job = Job(mine.clone());
        let mut state = self.state.lock();
        let wait = if traces_rays {
            state.traversal(client, &job, Job::ended)
        } else {
            state.compute(client, &job, Job::ended)
        };
        self.busy.store(state.busy(), Ordering::Release);
        wait.map(|job| job.0)
    }

    /// Nothing is admitted or waiting.
    fn idle(&self) -> bool {
        !self.busy.load(Ordering::Acquire)
    }
}

impl Backend {
    /// The last step of a command's admission in its scheduler: `Some(fence)` while the
    /// ray-traversal rule ([`crate::g17::traversal`]) keeps the command out of the GPU.
    ///
    /// Only the scheduler asks, for a job whose dependencies and capacity are there: a command
    /// that waits here holds other clients' ray-traversal work, so it must be able to run. A
    /// job that ends early stops holding others when its fence signals, which bounds their
    /// wait by its watchdog.
    pub(super) fn traversal_admission(&self, packet: &Arc<Packet>) -> Option<Fence> {
        if !self.shared.variant.traversal_exclusion {
            return None;
        }
        let traces_rays = packet.command.traces_rays();
        if !traces_rays && packet.engine() != Engine::Compute {
            return None;
        }
        if packet.early_result().is_some()
            || packet.cancel_requested()
            || packet.completion.status().get() != 0
        {
            return None;
        }
        self.shared.traversal.admit(
            packet.context.vm().id(),
            traces_rays,
            &packet.completion.scheduler_fence(),
        )
    }

    /// Whether the command may be published from its submit ioctl: never one that traces
    /// rays, and a compute command only while nothing is admitted or waiting. This only looks.
    pub(super) fn traversal_inline(&self, packet: &Arc<Packet>) -> bool {
        if !self.shared.variant.traversal_exclusion {
            return true;
        }
        if packet.command.traces_rays() {
            return false;
        }
        packet.engine() != Engine::Compute || self.shared.traversal.idle()
    }
}
