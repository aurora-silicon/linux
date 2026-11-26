// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! GPU event manager
//!
//! The GPU firmware manages work completion by using event objects (Apple calls them "stamps"),
//! which are monotonically incrementing counters. There are a fixed number of objects, and
//! they are managed with a `SlotAllocator`.
//!
//! This module manages the set of available events and lets users compute expected values.
//! It also manages signaling owners when the GPU firmware reports that an event fired.

use crate::debug::*;
use crate::fw::types::*;
use crate::{
    gpu,
    slotalloc,
    workqueue, //
};
use core::cmp;
use core::sync::atomic::Ordering;
use kernel::prelude::*;
use kernel::sync::Arc;
use kernel::{
    c_str,
    static_lock_class, //
};

const DEBUG_CLASS: DebugFlags = DebugFlags::Event;

/// Number of events managed by the firmware.
const NUM_EVENTS: u32 = 128;

/// Inner data associated with a given event slot.
pub(crate) struct EventInner {
    /// CPU pointer to the driver notification event stamp
    stamp: *const AtomicU32,
    /// GPU pointer to the driver notification event stamp
    gpu_stamp: GpuWeakPointer<Stamp>,
    /// GPU pointer to the firmware-internal event stamp
    gpu_fw_stamp: GpuWeakPointer<FwStamp>,
}

/// SAFETY: The event slots are safe to send across threads.
unsafe impl Send for EventInner {}

/// Alias for an event token, which allows requesting the same event.
pub(crate) type Token = slotalloc::SlotToken;
/// Alias for an allocated `Event` that has a slot.
pub(crate) type Event = slotalloc::Guard<EventInner>;

/// Represents a given stamp value for an event.
#[derive(Eq, PartialEq, Copy, Clone, Debug)]
#[repr(transparent)]
pub(crate) struct EventValue(u32);

impl EventValue {
    #[cfg(CONFIG_KUNIT)]
    pub(crate) const fn from_raw(raw: u32) -> Self {
        EventValue(raw)
    }

    #[cfg(CONFIG_DRM_ASAHI_G14_V27_2)]
    pub(crate) fn raw(self) -> u32 {
        self.0
    }

    /// Returns the `EventValue` that succeeds this one.
    pub(crate) fn next(&self) -> EventValue {
        EventValue(self.0.wrapping_add(0x100))
    }

    /// Increments this `EventValue` in place.
    pub(crate) fn increment(&mut self) {
        self.0 = self.0.wrapping_add(0x100);
    }

    /// Computes the delta between this event and another event.
    pub(crate) fn delta(&self, other: &EventValue) -> i32 {
        (self.0.wrapping_sub(other.0) as i32) >> 8
    }
}

impl PartialOrd for EventValue {
    fn partial_cmp(&self, other: &Self) -> Option<cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for EventValue {
    fn cmp(&self, other: &Self) -> cmp::Ordering {
        self.delta(other).cmp(&0)
    }
}

impl EventInner {
    /// Returns the GPU pointer to the driver notification stamp
    pub(crate) fn stamp_pointer(&self) -> GpuWeakPointer<Stamp> {
        self.gpu_stamp
    }

    /// Returns the GPU pointer to the firmware internal stamp
    pub(crate) fn fw_stamp_pointer(&self) -> GpuWeakPointer<FwStamp> {
        self.gpu_fw_stamp
    }

    /// Fetches the current event value from shared memory
    pub(crate) fn current(&self) -> EventValue {
        // SAFETY: The pointer is always valid as constructed in
        // EventManager below, and outside users cannot construct
        // new EventInners, nor move or copy them, and Guards as
        // returned by the SlotAllocator hold a reference to the
        // SlotAllocator containing the EventManagerInner, which
        // keeps the GpuObject the stamp is contained within alive.
        EventValue(unsafe { &*self.stamp }.load(Ordering::Acquire))
    }

    /// Read a completion stamp written by an agent outside the CPU coherency domain.
    #[cfg(CONFIG_DRM_ASAHI_G14_V27_2)]
    pub(crate) fn current_noncoherent(&self) -> EventValue {
        crate::pgtable::clean_invalidate_poc(self.stamp.cast(), core::mem::size_of::<u32>());
        crate::pgtable::poc_barrier();
        self.current()
    }
}

impl slotalloc::SlotItem for EventInner {
    type Data = EventManagerInner;

    fn release(&mut self, data: &mut Self::Data, slot: u32) {
        mod_pr_debug!("EventManager: Released slot {}\n", slot);
        data.owners[slot as usize] = None;
    }
}

/// Returns the owner registered for `slot`. Slot numbers come from firmware messages, so one
/// outside the table is treated like an empty slot instead of being trusted as an index.
pub(crate) fn slot_owner<T: Clone>(owners: &[Option<T>], slot: u32) -> Option<T> {
    owners.get(slot as usize).and_then(|owner| owner.as_ref().cloned())
}

/// Inner data for the event manager, to be protected by the SlotAllocator lock.
pub(crate) struct EventManagerInner {
    stamps: GpuArray<Stamp>,
    fw_stamps: GpuArray<FwStamp>,
    // Note: Use dyn to avoid having to version this entire module.
    owners: KVec<Option<Arc<dyn workqueue::WorkQueue + Send + Sync>>>,
}

/// Top-level EventManager object.
pub(crate) struct EventManager {
    alloc: slotalloc::SlotAllocator<EventInner>,
}

impl EventManager {
    /// Create a new EventManager.
    #[inline(never)]
    pub(crate) fn new(alloc: &mut gpu::KernelAllocators) -> Result<EventManager> {
        let mut owners = KVec::new();
        for _i in 0..(NUM_EVENTS as usize) {
            owners.push(None, GFP_KERNEL)?;
        }
        let inner = EventManagerInner {
            stamps: alloc.shared.array_empty(NUM_EVENTS as usize)?,
            fw_stamps: alloc.private.array_empty(NUM_EVENTS as usize)?,
            owners,
        };

        for slot in 0..NUM_EVENTS {
            inner.stamps[slot as usize]
                .0
                .store(slot << 24, Ordering::Relaxed);
        }

        Ok(EventManager {
            alloc: slotalloc::SlotAllocator::new(
                NUM_EVENTS,
                inner,
                |inner: &mut EventManagerInner, slot| {
                    Some(EventInner {
                        stamp: &inner.stamps[slot as usize].0,
                        gpu_stamp: inner.stamps.weak_item_pointer(slot as usize),
                        gpu_fw_stamp: inner.fw_stamps.weak_item_pointer(slot as usize),
                    })
                },
                c_str!("EventManager::SlotAllocator"),
                static_lock_class!(),
                static_lock_class!(),
            )?,
        })
    }

    /// Gets a free `Event`, optionally trying to reuse the last one allocated by this caller.
    pub(crate) fn get(
        &self,
        token: Option<Token>,
        owner: Arc<dyn workqueue::WorkQueue + Send + Sync>,
    ) -> Result<Event> {
        let ev = self.alloc.get_inner(token, |inner, ev| {
            mod_pr_debug!(
                "EventManager: Registered owner {:p} on slot {}\n",
                &*owner,
                ev.slot()
            );
            inner.owners[ev.slot() as usize] = Some(owner);
            Ok(())
        })?;
        Ok(ev)
    }

    /// Make freshly initialized stamps visible before a non-coherent firmware starts.
    #[cfg(CONFIG_DRM_ASAHI_G14_V27_2)]
    pub(crate) fn sync_initial_stamps(&self) {
        self.alloc.with_inner(|inner| {
            let stamps = inner.stamps.as_slice();
            let fw_stamps = inner.fw_stamps.as_slice();
            crate::pgtable::clean_poc(stamps.as_ptr().cast(), core::mem::size_of_val(stamps));
            crate::pgtable::clean_poc(fw_stamps.as_ptr().cast(), core::mem::size_of_val(fw_stamps));
        });
        crate::pgtable::poc_barrier();
    }

    /// Reserve the normal stamp pair for a kernel queue that polls its own completion.
    #[cfg(CONFIG_DRM_ASAHI_G14_V27_2)]
    pub(crate) fn try_get_polled(&self) -> Result<Event> {
        self.alloc.try_get(None)
    }

    /// Signals an event by slot, indicating completion (of one or more commands).
    pub(crate) fn signal(&self, slot: u32) {
        match self
            .alloc
            .with_inner(|inner| slot_owner(&inner.owners, slot))
        {
            Some(owner) => {
                owner.signal();
            }
            None => {
                mod_pr_debug!("EventManager: Received event for empty slot {}\n", slot);
            }
        }
    }

    /// Marks the owner of an event as having lost its work due to a GPU error.
    pub(crate) fn mark_error(&self, slot: u32, wait_value: u32, error: workqueue::WorkError) {
        match self
            .alloc
            .with_inner(|inner| slot_owner(&inner.owners, slot))
        {
            Some(owner) => {
                owner.mark_error(EventValue(wait_value), error);
            }
            None => {
                pr_err!("Received error for empty slot {}\n", slot);
            }
        }
    }

    /// Returns a reference to the workqueue owning an event.
    pub(crate) fn get_owner(
        &self,
        slot: u32,
    ) -> Option<Arc<dyn workqueue::WorkQueue + Send + Sync>> {
        self.alloc
            .with_inner(|inner| slot_owner(&inner.owners, slot))
    }

    /// Fail all commands, used when the GPU crashes.
    pub(crate) fn fail_all(&self, error: workqueue::WorkError) {
        let mut owners: KVec<Arc<dyn workqueue::WorkQueue + Send + Sync>> = KVec::new();

        self.alloc.with_inner(|inner| {
            for wq in inner.owners.iter().filter_map(|o| o.as_ref()).cloned() {
                if owners.push(wq, GFP_KERNEL).is_err() {
                    pr_err!("Failed to signal failure to WorkQueue\n");
                }
            }
        });

        for wq in owners {
            wq.fail_all(error);
        }
    }
}

#[kunit_tests(asahi_event_slot_owner)]
mod tests {
    use super::slot_owner;

    #[test]
    fn firmware_slot_outside_the_table_has_no_owner() {
        let owners = [Some(7u32), None];
        assert_eq!(slot_owner(&owners, 0), Some(7));
        assert_eq!(slot_owner(&owners, 1), None);
        assert_eq!(slot_owner(&owners, 2), None);
        assert_eq!(slot_owner(&owners, u32::MAX), None);
    }
}
