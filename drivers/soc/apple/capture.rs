// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Shared capture ownership; an idle biometric file does not own the sensor.

use crate::SepData;
use kernel::prelude::*;
use kernel::sync::atomic::Relaxed;

enum Owner {
    Idle,
    Sync(u64),
    Bio(u64),
}

pub(crate) struct State {
    generation: u64,
    owner: Owner,
}

impl State {
    pub(crate) fn new() -> Self {
        Self {
            generation: 0,
            owner: Owner::Idle,
        }
    }

    fn acquire(&mut self) -> Result<u64> {
        if !matches!(self.owner, Owner::Idle) {
            return Err(EBUSY);
        }
        self.generation = self.generation.checked_add(1).ok_or(EOVERFLOW)?;
        self.owner = Owner::Sync(self.generation);
        Ok(self.generation)
    }

    /// Hands a synchronous owner's capture to the bio worker `epoch`.
    fn transfer_to_bio(&mut self, generation: u64, epoch: u64) {
        if matches!(self.owner, Owner::Sync(active) if active == generation) {
            self.owner = Owner::Bio(epoch);
        }
    }

    /// Ends a synchronous owner, unless it was handed on or replaced.
    fn release(&mut self, generation: u64) {
        if matches!(self.owner, Owner::Sync(active) if active == generation) {
            self.owner = Owner::Idle;
        }
    }

    pub(crate) fn finish_bio(&mut self, epoch: u64) {
        if matches!(self.owner, Owner::Bio(active) if active == epoch) {
            self.owner = Owner::Idle;
        }
    }
}

/// A synchronous firmware operation joins the suspend drain. Ownership can be
/// transferred to a queued bio worker, which releases it only after cleanup.
pub(crate) struct Lease<'a> {
    sep: &'a SepData,
    generation: u64,
    running: bool,
}

impl<'a> Lease<'a> {
    pub(crate) fn acquire(sep: &'a SepData) -> Result<Self> {
        let generation = {
            let mut state = sep.capture.lock();
            if sep.shutting_down.load(Relaxed) {
                return Err(ENODEV);
            }
            if sep.suspending.load(Relaxed) {
                return Err(EBUSY);
            }
            state.acquire()?
        };
        let mut lease = Self {
            sep,
            generation,
            running: false,
        };
        if !sep.capture_begin() {
            return Err(EBUSY);
        }
        lease.running = true;
        Ok(lease)
    }

    pub(crate) fn into_bio(mut self, epoch: u64) {
        self.sep.capture.lock().transfer_to_bio(self.generation, epoch);
        // The queued worker will join the drain when it starts; a suspend
        // before then aborts its session, and capture_begin refuses hardware.
        self.sep.capture_end();
        self.running = false;
    }
}

impl Drop for Lease<'_> {
    fn drop(&mut self) {
        if self.running {
            self.sep.capture_end();
        }
        self.sep.capture.lock().release(self.generation);
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_capture_owner)]
mod tests {
    use super::*;

    #[test]
    fn one_owner_at_a_time() {
        let mut state = State::new();
        let first = state.acquire().unwrap();
        assert_eq!(state.acquire().err(), Some(EBUSY));
        state.release(first);
        let second = state.acquire().unwrap();
        assert!(second != first);
        // A stale owner's release does not end the current one.
        state.release(first);
        assert_eq!(state.acquire().err(), Some(EBUSY));
        state.release(second);
        assert!(state.acquire().is_ok());
    }

    #[test]
    fn a_bio_worker_keeps_the_capture_until_it_finishes() {
        let mut state = State::new();
        let lease = state.acquire().unwrap();
        state.transfer_to_bio(lease, 7);
        // The lease that handed it on no longer owns it.
        state.release(lease);
        assert_eq!(state.acquire().err(), Some(EBUSY));
        state.finish_bio(6);
        assert_eq!(state.acquire().err(), Some(EBUSY));
        state.finish_bio(7);
        assert!(state.acquire().is_ok());
    }
}

