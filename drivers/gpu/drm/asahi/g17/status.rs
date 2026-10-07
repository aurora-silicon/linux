// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! VM admission errors and client failure reporting. No firmware state is accessed here.

use crate::gem;
use core::sync::atomic::{AtomicI32, Ordering};
use kernel::{new_mutex, prelude::*, sync::Mutex};

const MIRROR_MAX_SIZE: usize = 64 * 1024;

/// A VM and its accepted jobs share this status, including after queue destruction.
/// Client failure is separate from hardware admission: recovery may preserve the
/// VM while failed accepted work must still be reported before signalling fences.
#[pin_data]
pub(crate) struct VmStatus {
    error: AtomicI32,
    reported_error: AtomicI32,
    /// The client's mapped status word, retained with its object only while the
    /// client's VM handle exists; released by `release_mirror` at VM close.
    #[pin]
    mirror: Mutex<Option<Mirror>>,
}

/// One aligned word inside the retained, vmapped mirror object.
struct Mirror {
    word: *mut i32,
    _owner: gem::ObjectRef,
}
// SAFETY: The word points into the mapping the retained owner keeps alive; it is
// only accessed atomically through this pointer while the mutex is held.
unsafe impl Send for Mirror {}

impl VmStatus {
    pub(crate) fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            error: AtomicI32::new(0),
            reported_error: AtomicI32::new(0),
            mirror <- new_mutex!(None, "Asahi VM status mirror"),
        })
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
        let mirror = self.mirror.lock();
        if let Some(mirror) = mirror.as_ref() {
            // SAFETY: attach_mirror stored only an aligned word within a mapping the
            // retained owner keeps alive while this entry exists under the mutex.
            unsafe { AtomicI32::from_ptr(mirror.word) }.store(error, Ordering::SeqCst);
        }
    }

    /// Rejected submissions and contention leave the VM usable.
    pub(crate) fn record_if_device_loss(&self, error: Error) {
        if error == EIO || error == ETIMEDOUT || error == ENODEV {
            self.record(error);
        }
    }

    /// Retain one mapped GEM word until the client's VM handle is dropped.
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
        let mut mirror = self.mirror.lock();
        if mirror.is_some() {
            return Err(EBUSY);
        }
        // No reporter runs while the mutex is held, so the word holds the first
        // reported error (or zero) when it becomes visible.
        let error = self.reported_error.load(Ordering::SeqCst);
        // SAFETY: The retained owner keeps this checked, aligned word mapped.
        unsafe { AtomicI32::from_ptr(word) }.store(error, Ordering::SeqCst);
        *mirror = Some(Mirror {
            word,
            _owner: owner,
        });
        Ok(())
    }

    /// The client's VM handle is gone and nobody reads the mirror any more. The
    /// mapping and object reference drop outside the mutex (GEM release may sleep).
    pub(crate) fn release_mirror(&self) {
        let taken = self.mirror.lock().take();
        drop(taken);
    }
}
