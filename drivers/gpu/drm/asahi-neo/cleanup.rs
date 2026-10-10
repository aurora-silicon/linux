// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Reservation-taking timestamp destruction must not block fence producers.
//!
//! The module owns this queue outside its platform registration. Files and
//! exported buffers retain the module; detached scheduler cleanup retains it
//! until its driver callback returns. Unregister joins the firmware/event work.
//! Only after those producer boundaries close may the module destroy this queue.

use core::{
    ops::Deref,
    ptr::{self, NonNull},
    sync::atomic::{AtomicPtr, Ordering},
};
use kernel::{
    bindings, c_str, impl_has_work, new_work,
    prelude::*,
    workqueue::{Queue, Work, WorkItem},
};

static QUEUE: AtomicPtr<bindings::workqueue_struct> = AtomicPtr::new(ptr::null_mut());

/// Destroyed by module exit after platform-driver unregister, never by a worker.
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
                c_str!("asahi-ts-cleanup").as_char_ptr(),
                bindings::wq_flags_WQ_UNBOUND | bindings::wq_flags_WQ_MEM_RECLAIM,
                0,
            )
        };
        let raw = NonNull::new(raw).ok_or(ENOMEM)?;
        if QUEUE
            .compare_exchange(ptr::null_mut(), raw.as_ptr(), Ordering::AcqRel, Ordering::Acquire)
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
        // No external producers remain: module registration was dropped first.
        // Pending cleanup owns no queue reference and cannot enqueue more work.
        // SAFETY: The module's unique owner retains the queue through its drain.
        unsafe { bindings::destroy_workqueue(self.0.as_ptr()) };
        QUEUE.store(ptr::null_mut(), Ordering::Release);
    }
}

#[pin_data]
struct Item<T: Send + 'static> {
    value: T,
    #[pin]
    work: Work<Self>,
}

impl_has_work! {
    impl{T: Send + 'static} HasWork<Item<T>> for Item<T> { self.work }
}

impl<T: Send + 'static> WorkItem for Item<T> {
    type Pointer = Pin<KBox<Self>>;

    fn run(this: Self::Pointer) {
        drop(this);
    }
}

/// Allocate the destruction work before publication; final handoff cannot fail.
pub(crate) struct Deferred<T: Send + 'static>(Option<Pin<KBox<Item<T>>>>);

impl<T: Send + 'static> Deferred<T> {
    pub(crate) fn new(value: T) -> Result<Self> {
        let item = KBox::pin_init(
            pin_init!(Item {
                value,
                work <- new_work!("Asahi timestamp cleanup"),
            }),
            GFP_KERNEL,
        )?;
        Ok(Self(Some(item)))
    }
}

impl<T: Send + 'static> Deref for Deferred<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.0.as_ref().unwrap().value
    }
}

impl<T: Send + 'static> Drop for Deferred<T> {
    fn drop(&mut self) {
        if let Some(item) = self.0.take() {
            let raw = QUEUE.load(Ordering::Acquire);
            assert!(!raw.is_null());
            // SAFETY: Module ownership and producer joins retain the queue until
            // this handoff has returned. The unique pinned box is never queued
            // twice; enqueue transfers it without allocation or a failure path.
            unsafe { Queue::from_raw(raw) }.enqueue(item);
        }
    }
}
