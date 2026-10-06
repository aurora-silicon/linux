// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! VM admission errors and client failure reporting. No firmware state is accessed here.

use crate::gem;
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, Ordering};
use kernel::prelude::*;

const MIRROR_MAX_SIZE: usize = 64 * 1024;

/// A VM and its accepted jobs share this status, including after queue destruction.
/// Client failure is separate from hardware admission: recovery may preserve the
/// VM while failed accepted work must still be reported before signalling fences.
pub(crate) struct VmStatus {
    error: AtomicI32,
    reported_error: AtomicI32,
    mirror: AtomicPtr<i32>,
    mirror_claimed: AtomicBool,
    mirror_owner: UnsafeCell<Option<gem::ObjectRef>>,
}

// SAFETY: Only the winner of mirror_claimed writes mirror_owner. The owner is never
// read or replaced while the status lives, and its mapping is retained until drop.
// Other shared accesses use atomics, including accesses through the mirror pointer.
unsafe impl Sync for VmStatus {}
// SAFETY: The retained GEM reference and mapping can move between threads. Moving
// the status does not move that mapping, and its remaining fields are atomic.
unsafe impl Send for VmStatus {}

impl VmStatus {
    pub(crate) fn new() -> Self {
        Self {
            error: AtomicI32::new(0),
            reported_error: AtomicI32::new(0),
            mirror: AtomicPtr::new(core::ptr::null_mut()),
            mirror_claimed: AtomicBool::new(false),
            mirror_owner: UnsafeCell::new(None),
        }
    }

    pub(crate) fn get(&self) -> i32 {
        self.error.load(Ordering::Acquire)
    }

    /// Record permanent admission failure and report it to the client.
    pub(crate) fn record(&self, error: Error) {
        self.report_failure(error);
        let _ = self.error.compare_exchange(
            0,
            error.to_errno(),
            Ordering::SeqCst,
            Ordering::SeqCst,
        );
    }

    pub(crate) fn reported_error(&self) -> i32 {
        self.reported_error.load(Ordering::Acquire)
    }

    /// Report failed accepted work without preventing recovery of the VM.
    /// Every reporter publishes the first error before returning: a losing
    /// reporter may signal its fence before the winning reporter resumes.
    pub(crate) fn report_failure(&self, error: Error) {
        let error = match self.reported_error.compare_exchange(
            0,
            error.to_errno(),
            Ordering::SeqCst,
            Ordering::SeqCst,
        ) {
            Ok(_) => error.to_errno(),
            Err(first) => first,
        };
        let word = self.mirror.load(Ordering::SeqCst);
        if !word.is_null() {
            // SAFETY: attach_mirror publishes only an aligned word within a mapping
            // owned by this status. Every kernel access to this word is atomic.
            unsafe { AtomicI32::from_ptr(word) }.store(error, Ordering::SeqCst);
        }
    }

    /// Rejected submissions and contention leave the VM usable.
    pub(crate) fn record_if_device_loss(&self, error: Error) {
        if error == EIO || error == ETIMEDOUT || error == ENODEV {
            self.record(error);
        }
    }

    /// Retain one mapped GEM word until the last reference to this status is dropped.
    pub(crate) fn attach_mirror(&self, mut owner: gem::ObjectRef, offset: usize) -> Result {
        if owner.size() > MIRROR_MAX_SIZE
            || offset % core::mem::align_of::<i32>() != 0
            || offset
                .checked_add(core::mem::size_of::<i32>())
                .is_none_or(|end| end > owner.size())
        {
            return Err(EINVAL);
        }
        let base = owner.vmap()?.as_mut_ptr();
        // SAFETY: The checked range is inside the retained, page-aligned mapping.
        let word = unsafe { base.add(offset) }.cast::<i32>();
        if self.mirror_claimed.swap(true, Ordering::AcqRel) {
            return Err(EBUSY);
        }
        // SAFETY: This is the only thread that may write mirror_owner. It is not
        // read again until the status is dropped, after all shared references end.
        unsafe { *self.mirror_owner.get() = Some(owner) };
        // SAFETY: The retained owner keeps this checked, aligned word mapped.
        unsafe { AtomicI32::from_ptr(word) }.store(0, Ordering::SeqCst);

        // The pointer publication and reported error form a store/load handshake:
        // either this load sees the error or the reporter sees our pointer. Stores
        // after publishing the pointer write only the same first error, never zero.
        self.mirror.store(word, Ordering::SeqCst);
        let error = self.reported_error.load(Ordering::SeqCst);
        if error != 0 {
            // SAFETY: The mapping remains owned by this status as above.
            unsafe { AtomicI32::from_ptr(word) }.store(error, Ordering::SeqCst);
        }
        Ok(())
    }
}
