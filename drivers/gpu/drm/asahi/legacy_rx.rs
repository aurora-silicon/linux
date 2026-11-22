// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Dedicated receive work for the legacy firmware path.
//!
//! RTKit normally allocates a work item for every mailbox message. The legacy
//! GPU only sends a doorbell here, and the event ring itself is authoritative,
//! so one preallocated, coalescing driver work item is enough.

use core::{
    ptr::{self, NonNull},
    sync::atomic::{AtomicPtr, Ordering},
};
use kernel::{bindings, c_str, prelude::*, workqueue::Queue};

unsafe extern "C" {
    fn asahi_legacy_set_current_util_min(util_min: i32) -> i32;
}

static QUEUE: AtomicPtr<bindings::workqueue_struct> = AtomicPtr::new(ptr::null_mut());

/// Destroyed after platform-driver unregister has stopped every RTKit producer.
pub(crate) struct QueueOwner(NonNull<bindings::workqueue_struct>);

// SAFETY: QueueOwner only transfers ownership of a thread-safe kernel queue.
unsafe impl Send for QueueOwner {}
// SAFETY: The queue API synchronizes all accesses; destruction requires ownership.
unsafe impl Sync for QueueOwner {}

impl QueueOwner {
    pub(crate) fn new() -> Result<Self> {
        // SAFETY: The name is NUL terminated and contains no format specifiers.
        let raw = unsafe {
            bindings::alloc_workqueue_noprof(
                c_str!("asahi-legacy-rx").as_char_ptr(),
                bindings::wq_flags_WQ_UNBOUND
                    | bindings::wq_flags_WQ_HIGHPRI
                    | bindings::wq_flags_WQ_MEM_RECLAIM,
                1,
            )
        };
        let raw = NonNull::new(raw).ok_or(ENOMEM)?;
        if QUEUE
            .compare_exchange(
                ptr::null_mut(),
                raw.as_ptr(),
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_err()
        {
            // SAFETY: This fresh queue has never been published or used.
            unsafe { bindings::destroy_workqueue(raw.as_ptr()) };
            return Err(EBUSY);
        }
        Ok(Self(raw))
    }
}

impl Drop for QueueOwner {
    fn drop(&mut self) {
        // Registration has already been dropped, so no callback can enqueue
        // after this drain begins.
        unsafe { bindings::destroy_workqueue(self.0.as_ptr()) };
        QUEUE.store(ptr::null_mut(), Ordering::Release);
    }
}

pub(crate) fn get() -> Option<&'static Queue> {
    let raw = QUEUE.load(Ordering::Acquire);
    if raw.is_null() {
        None
    } else {
        // SAFETY: QueueOwner retains the allocation until driver unregister has
        // joined the callback producers and drained this queue.
        Some(unsafe { Queue::from_raw(raw) })
    }
}

/// A scoped capacity request for the shared unbound worker executing RX work.
/// Resetting it before returning prevents the hint leaking to unrelated work.
pub(crate) struct UtilMinGuard(bool);

impl UtilMinGuard {
    pub(crate) fn new(value: u32) -> Self {
        Self(Self::set(value.min(1024) as i32))
    }

    fn set(value: i32) -> bool {
        // SAFETY: The helper changes only current's utilization clamp and
        // accepts either 0..1024 or -1 to restore the default.
        unsafe { asahi_legacy_set_current_util_min(value) == 0 }
    }
}

impl Drop for UtilMinGuard {
    fn drop(&mut self) {
        if self.0 {
            // -1 restores the task's default clamp, matching the mailbox RX
            // hint contract.
            let _ = Self::set(-1);
        }
    }
}
