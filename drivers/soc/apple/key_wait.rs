// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Killable/freezable wait bridge. No Rust state lock crosses a sleep.

use kernel::prelude::*;

extern "C" {
    fn sep_key_event_new() -> *mut c_void;
    fn sep_key_event_free(event: *mut c_void);
    fn sep_key_event_sequence(event: *mut c_void) -> u64;
    fn sep_key_event_wake(event: *mut c_void);
    fn sep_key_event_wait(event: *mut c_void, sequence: u64, timeout_ms: u32) -> c_int;
}

pub(crate) struct Event(*mut c_void);
// SAFETY: the C object contains only a waitqueue and an atomic sequence.
unsafe impl Send for Event {}
// SAFETY: every operation is synchronized by that queue/atomic.
unsafe impl Sync for Event {}

impl Event {
    pub(crate) fn new() -> Result<Self> {
        // SAFETY: allocation has no preconditions.
        let event = unsafe { sep_key_event_new() };
        if event.is_null() {
            Err(ENOMEM)
        } else {
            Ok(Self(event))
        }
    }

    pub(crate) fn sequence(&self) -> u64 {
        // SAFETY: self owns a live event.
        unsafe { sep_key_event_sequence(self.0) }
    }

    pub(crate) fn wake(&self) {
        // SAFETY: self owns a live event.
        unsafe { sep_key_event_wake(self.0) };
    }

    pub(crate) fn wait(&self, sequence: u64, deadline_ns: u64) -> Result {
        let ms = deadline_ns
            .saturating_sub(crate::shim::monotonic_ns())
            .div_ceil(1_000_000);
        if ms == 0 {
            return Ok(());
        }
        // SAFETY: the event lives throughout the call. Callers hold no locks.
        let status = unsafe { sep_key_event_wait(self.0, sequence, ms.min(120_000) as u32) };
        if status == 0 {
            Ok(())
        } else {
            Err(Error::from_errno(status))
        }
    }
}

impl Drop for Event {
    fn drop(&mut self) {
        // SAFETY: all waiters own a reference to the containing object.
        unsafe { sep_key_event_free(self.0) };
    }
}
