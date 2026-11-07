// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Each process's share of the GPU's independent queues.
//!
//! A process can open the device as often as it likes, so a queue limit per open file limits
//! nothing. Queues are therefore counted, device-wide, against the process that created them,
//! identified by its mm as [`crate::memacct`] attributes memory, for as long as they exist.
//!
//! A process may always hold up to half of the device's queues. It may take more while at least
//! [`reserve`] queues stay free for the other processes (the compositor and anything started
//! later), or while no other process holds a queue, so a process alone can still use every one.
//! A process below half its share is never refused here, so the reserve stays open to them.

use kernel::{
    mm::Mm,
    prelude::*,
    sync::aref::ARef, //
};

kernel::sync::global_lock! {
    // SAFETY: Module initialization initializes this lock before registering adapters.
    unsafe(uninit) static PROCESSES: Mutex<KVec<Process>> = KVec::new();
}

/// Initialize the registry before a platform adapter can probe.
///
/// # Safety
///
/// Called exactly once per module instance, before any queue is created.
pub(crate) unsafe fn init() {
    // SAFETY: The module initializer establishes the single-call ordering above.
    unsafe { PROCESSES.init() };
}

/// The queues one process holds.
pub(crate) struct Process {
    key: usize,
    /// Keeps the `mm_struct`, and with it `key`, from being reused while queues count against it.
    _mm: Option<ARef<Mm>>,
    held: u32,
}

/// Queues kept free for the other processes once one process holds half of the device's `max`.
fn reserve(max: u32) -> u32 {
    (max / 8).max(1)
}

/// Whether a process holding `held` of the `total` queues counted device-wide may take another
/// of the device's `max`. The device itself refuses a queue it has no room for.
fn may_take(held: u32, total: u32, max: u32) -> bool {
    held < max / 2 || held == total || max.saturating_sub(total.saturating_add(1)) >= reserve(max)
}

/// Counts a new queue against the process `key`; EBUSY when [`may_take`] refuses it.
fn take(processes: &mut KVec<Process>, key: usize, mm: Option<ARef<Mm>>, max: u32) -> Result {
    let total = processes.iter().map(|process| process.held).sum::<u32>();
    let index = processes.iter().position(|process| process.key == key);
    let held = index.map_or(0, |index| processes[index].held);
    if !may_take(held, total, max) {
        return Err(EBUSY);
    }
    match index {
        Some(index) => processes[index].held += 1,
        None => processes.push(Process { key, _mm: mm, held: 1 }, GFP_KERNEL)?,
    }
    Ok(())
}

/// Gives back one queue of `key`; returns the process once it holds none, to drop unlocked.
fn give_back(processes: &mut KVec<Process>, key: usize) -> Option<Process> {
    let index = processes.iter().position(|process| process.key == key)?;
    processes[index].held -= 1;
    (processes[index].held == 0).then(|| processes.swap_remove(index))
}

/// One queue counted against the process that created it; dropping it gives the queue back.
pub(crate) struct Share {
    key: usize,
}

impl Share {
    /// Counts a new queue against the current process, of the device's `max` queues.
    pub(crate) fn take_current(max: u32) -> Result<Self> {
        let mm = kernel::current!().mm().map(|mm| ARef::from(&**mm));
        let key = mm.as_ref().map_or(0, |mm| mm.as_raw() as usize);
        take(&mut PROCESSES.lock(), key, mm, max)?;
        Ok(Self { key })
    }
}

impl Drop for Share {
    fn drop(&mut self) {
        let gone = give_back(&mut PROCESSES.lock(), self.key);
        // Drop the process, and with it the mm reference, after the lock.
        drop(gone);
    }
}

#[kunit_tests(asahi_queue_share)]
mod tests {
    use super::*;

    #[test]
    fn a_process_alone_may_use_every_queue() {
        assert!(may_take(57, 57, 58));
        assert!(may_take(41, 41, 42));
    }

    #[test]
    fn a_large_holder_leaves_the_reserve_to_others() {
        // 58 queues keep 7 free for the others; 42 keep 5.
        assert_eq!(reserve(58), 7);
        assert_eq!(reserve(42), 5);
        // With a compositor holding 3, one process gets to 48 of 58, not 29.
        assert!(may_take(47, 50, 58));
        assert!(!may_take(48, 51, 58));
        // Below half its share, a process may take queues from the reserve.
        assert!(may_take(3, 51, 58));
        assert!(may_take(28, 57, 58));
    }

    #[test]
    fn a_lone_process_with_two_files_reaches_the_full_budget() -> Result {
        let mut processes = KVec::new();
        // Another process held queues and gave them all back: it no longer counts.
        take(&mut processes, 2, None, 58)?;
        assert!(give_back(&mut processes, 2).is_some());
        assert!(processes.is_empty());
        // Both files of process 1 count against one key, which may take all 58.
        for _ in 0..58 {
            take(&mut processes, 1, None, 58)?;
        }
        assert_eq!(processes.len(), 1);
        assert_eq!(processes[0].held, 58);
        Ok(())
    }

    #[test]
    fn reopening_does_not_reset_the_count() -> Result {
        let mut processes = KVec::new();
        // One process (key 1) beside a compositor (key 2) holding 3 queues of 58.
        for _ in 0..3 {
            take(&mut processes, 2, None, 58)?;
        }
        // Every file of process 1 counts against the same key.
        for _ in 0..48 {
            take(&mut processes, 1, None, 58)?;
        }
        assert_eq!(take(&mut processes, 1, None, 58), Err(EBUSY));
        // The compositor still gets one, and so does a new process.
        take(&mut processes, 2, None, 58)?;
        take(&mut processes, 3, None, 58)?;
        // Giving every queue back removes the process.
        for _ in 0..47 {
            assert!(give_back(&mut processes, 1).is_none());
        }
        assert!(give_back(&mut processes, 1).is_some());
        assert!(processes.iter().all(|process| process.key != 1));
        Ok(())
    }
}
