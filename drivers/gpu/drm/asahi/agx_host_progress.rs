// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Host-verified retirement counters, independent of logs and firmware IRQs.
//!
//! One serialized runtime writer calls record_completion only after proving
//! retirement. Readers never touch MMIO, allocate, or acquire the runtime lock.
use core::sync::atomic::{AtomicU64, Ordering};
use kernel::time::{ClockSource, Monotonic};

pub(crate) struct Progress {
    completed: AtomicU64,
    last_completion_ns: AtomicU64,
    generation_ns: u64,
}
impl Progress {
    pub(crate) fn new() -> Self {
        Self { completed: AtomicU64::new(0), last_completion_ns: AtomicU64::new(0),
            generation_ns: Monotonic::ktime_get() as u64 }
    }
    pub(crate) fn record_completion(&self) {
        // A reader observing a new count sees this timestamp or a later REAL
        // completion. A timestamp alone must never authorize watchdog renewal.
        self.last_completion_ns.store(Monotonic::ktime_get() as u64, Ordering::Release);
        self.completed.fetch_add(1, Ordering::Release);
    }
    pub(crate) fn snapshot(&self) -> (u64, u64, u64) {
        let count = self.completed.load(Ordering::Acquire);
        let timestamp = self.last_completion_ns.load(Ordering::Acquire);
        (self.generation_ns, count, timestamp)
    }
}
