// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use core::cell::UnsafeCell;
use kernel::soc::apple::mailbox::Message;
use kernel::sync::atomic::{Acquire, Atomic, Relaxed, Release};

const RING_LEN: u32 = 64;
const RING_MASK: u32 = RING_LEN - 1;

pub(crate) struct RxRing {
    slots: [UnsafeCell<Message>; RING_LEN as usize],
    head: Atomic<u32>,
    tail: Atomic<u32>,
    dropped: Atomic<u32>,
}

// SAFETY: ownership of each slot passes between the two sides via the
// release/acquire pairing on `head`/`tail`, so no slot is touched by both at
// once; single-producer/consumer are enforced by rx_lock and workqueue non-reentrancy.
unsafe impl Sync for RxRing {}
// SAFETY: `Message` is a plain data struct with no thread affinity.
unsafe impl Send for RxRing {}

impl RxRing {
    pub(crate) fn new() -> Self {
        RxRing {
            slots: core::array::from_fn(|_| UnsafeCell::new(Message { msg0: 0, msg1: 0 })),
            head: Atomic::new(0),
            tail: Atomic::new(0),
            dropped: Atomic::new(0),
        }
    }

    pub(crate) fn push(&self, msg: Message) -> bool {
        let head = self.head.load(Relaxed);
        // Acquire pairs with the consumer's release of `tail`, so the reused slot is free.
        let tail = self.tail.load(Acquire);

        if head.wrapping_sub(tail) >= RING_LEN {
            self.dropped
                .store(self.dropped.load(Relaxed).wrapping_add(1), Relaxed);
            return false;
        }

        let slot = &self.slots[(head & RING_MASK) as usize];
        // SAFETY: `head` is not published yet, so the consumer cannot be looking at
        // this slot, and we are the only producer.
        unsafe { slot.get().write(msg) };

        // Release pairs with the consumer's acquire of `head`, publishing the slot write.
        self.head.store(head.wrapping_add(1), Release);
        true
    }

    pub(crate) fn pop(&self) -> Option<Message> {
        let tail = self.tail.load(Relaxed);
        // Acquire pairs with the producer's release of `head`.
        let head = self.head.load(Acquire);

        if head == tail {
            return None;
        }

        let slot = &self.slots[(tail & RING_MASK) as usize];
        // SAFETY: `head` is past `tail`, so the producer finished this slot and
        // will not touch it again until we publish `tail`.
        let msg = unsafe { slot.get().read() };

        // Release pairs with the producer's acquire of `tail`, handing the slot back.
        self.tail.store(tail.wrapping_add(1), Release);
        Some(msg)
    }

    pub(crate) fn dropped(&self) -> u32 {
        self.dropped.load(Relaxed)
    }
}
