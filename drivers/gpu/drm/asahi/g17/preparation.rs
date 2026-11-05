// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Admission and lifetime of off-lock graph construction.
//!
//! Entering and leaving an open epoch use only atomics. Recovery closes the epoch while holding
//! the device mutex, then drops that mutex before waiting for builders. A builder's lease is
//! stored after its tentative mappings so those mappings are destroyed before the join completes.

use core::sync::atomic::{AtomicU64, Ordering};
use kernel::{
    new_condvar, new_mutex,
    prelude::*,
    sync::{Arc, CondVar, Mutex},
};

const COUNT_BITS: u32 = 20;
const COUNT_MASK: u64 = (1 << COUNT_BITS) - 1;
const CLOSED: u64 = 1 << COUNT_BITS;
const REMOVED: u64 = CLOSED << 1;
const FAILED: u64 = CLOSED << 2;
const EPOCH: u64 = CLOSED << 3;

#[pin_data]
pub(super) struct Gate {
    state: AtomicU64,
    #[pin]
    wait: Mutex<()>,
    #[pin]
    changed: CondVar,
}

impl Gate {
    pub(super) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                state: AtomicU64::new(EPOCH),
                wait <- new_mutex!((), "g17::Gate::wait"),
                changed <- new_condvar!("g17::Gate::changed"),
            }),
            GFP_KERNEL,
        )
    }

    pub(super) fn try_enter(self: &Arc<Self>) -> Result<Lease> {
        let mut previous = self.state.load(Ordering::Acquire);
        loop {
            if previous & (REMOVED | FAILED) != 0 {
                return Err(ENODEV);
            }
            if previous & CLOSED != 0 {
                return Err(EAGAIN);
            }
            if previous & COUNT_MASK == COUNT_MASK {
                return Err(ENOSPC);
            }
            match self.state.compare_exchange_weak(
                previous,
                previous + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => {
                    return Ok(Lease {
                        gate: self.clone(),
                        epoch: previous & !COUNT_MASK,
                    })
                }
                Err(current) => previous = current,
            }
        }
    }

    /// Wait only for a temporary recovery closure. Called with no device lock or existing lease.
    pub(super) fn enter(self: &Arc<Self>, interruptible: bool) -> Result<Lease> {
        match self.try_enter() {
            Err(error) if error == EAGAIN => {}
            result => return result,
        }
        let mut wait = self.wait.lock();
        loop {
            match self.try_enter() {
                Err(error) if error == EAGAIN => {}
                result => return result,
            }
            if interruptible {
                if self.changed.wait_interruptible(&mut wait) {
                    return Err(ERESTARTSYS);
                }
            } else {
                self.changed.wait(&mut wait);
            }
        }
    }

    fn invalidate(&self, flags: u64) {
        let mut previous = self.state.load(Ordering::Acquire);
        loop {
            let mut next = previous | CLOSED | flags;
            if previous & CLOSED == 0 {
                next = next
                    .checked_add(EPOCH)
                    .unwrap_or(previous | CLOSED | REMOVED | flags);
            }
            match self.state.compare_exchange_weak(
                previous,
                next,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return,
                Err(current) => previous = current,
            }
        }
    }

    pub(super) fn close(&self) {
        self.invalidate(0);
    }

    pub(super) fn remove(&self) {
        self.invalidate(REMOVED);
        let _wait = self.wait.lock();
        self.changed.notify_all();
    }

    pub(super) fn fail(&self) {
        self.invalidate(FAILED);
        let _wait = self.wait.lock();
        self.changed.notify_all();
    }

    /// Join builders with no device, queue or mapping lock held.
    pub(super) fn wait_drained(&self) {
        let mut wait = self.wait.lock();
        while self.state.load(Ordering::Acquire) & COUNT_MASK != 0 {
            self.changed.wait(&mut wait);
        }
    }

    /// Reopen only an empty, temporary closure. Failed or removed devices remain closed.
    pub(super) fn reopen(&self) -> bool {
        let _wait = self.wait.lock();
        let mut previous = self.state.load(Ordering::Acquire);
        loop {
            if previous & (REMOVED | FAILED | COUNT_MASK) != 0 {
                return false;
            }
            match self.state.compare_exchange_weak(
                previous,
                previous & !CLOSED,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => {
                    self.changed.notify_all();
                    return true;
                }
                Err(current) => previous = current,
            }
        }
    }
}

/// An off-lock builder's admission epoch. Keep after all of its tentative mappings.
pub(super) struct Lease {
    gate: Arc<Gate>,
    epoch: u64,
}

impl Lease {
    /// Must be checked under the device mutex immediately before publishing the built graph.
    pub(super) fn is_current(&self) -> bool {
        self.gate.state.load(Ordering::Acquire) & !COUNT_MASK == self.epoch
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        let previous = self.gate.state.fetch_sub(1, Ordering::AcqRel);
        if previous & COUNT_MASK == 1 && previous & CLOSED != 0 {
            // Pair the final decrement with the joiner's check/sleep lock to avoid a lost wakeup.
            let _wait = self.gate.wait.lock();
            self.gate.changed.notify_all();
        }
    }
}
