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
    sync::aref::ARef,
    task::Pid, //
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
    /// The process that took the first of these queues, for the capacity snapshot.
    tgid: Pid,
    comm: [u8; 16],
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

/// [`may_take`] for a resource a process keeps until it closes it (a G16 VM's TTBAT slot):
/// even a process alone leaves [`reserve`] free, or one client started before the compositor
/// would hold every slot until it exits.
fn may_take_held(held: u32, total: u32, max: u32) -> bool {
    held < max / 2 || max.saturating_sub(total.saturating_add(1)) >= reserve(max)
}

/// Counts a new queue against the process `key`; EBUSY when [`may_take`] refuses it.
fn take(processes: &mut KVec<Process>, key: usize, mm: Option<ARef<Mm>>, max: u32) -> Result {
    take_with(processes, key, mm, max, GFP_KERNEL, may_take)
}

fn take_with(processes: &mut KVec<Process>, key: usize, mm: Option<ARef<Mm>>, max: u32,
             flags: kernel::alloc::Flags, allowed: fn(u32, u32, u32) -> bool) -> Result {
    let total = processes.iter().map(|process| process.held).sum::<u32>();
    let index = processes.iter().position(|process| process.key == key);
    let held = index.map_or(0, |index| processes[index].held);
    if !allowed(held, total, max) {
        return Err(EBUSY);
    }
    match index {
        Some(index) => processes[index].held += 1,
        None => {
            let (tgid, comm) = current_name();
            processes.push(Process { key, _mm: mm, held: 1, tgid, comm }, flags)?
        }
    }
    Ok(())
}

/// The current thread group and the current thread's name.
fn current_name() -> (Pid, [u8; 16]) {
    let task = kernel::current!();
    // SAFETY: `comm` is a fixed array inside the live current task. A concurrent rename by
    // another thread of the process may tear the copy, as with C's unlocked `current->comm`.
    let comm: [u8; 16] =
        unsafe { core::ptr::read_volatile(core::ptr::addr_of!((*task.as_ptr()).comm).cast()) };
    (task.tgid(), comm)
}

/// Writes the device's queue share for the capacity snapshot: the limit, its reserve, the
/// queues held and, largest first, the processes holding the most.
pub(crate) fn write_capacity(f: &mut kernel::fmt::Formatter<'_>, max: u32) -> kernel::fmt::Result {
    const SHOWN: usize = 8;
    let processes = PROCESSES.lock();
    let held = processes.iter().map(|process| process.held).sum::<u32>();
    writeln!(f, "queue_share held={} max={} reserve={} processes={}", held, max, reserve(max),
             processes.len())?;
    let mut shown = [usize::MAX; SHOWN];
    for slot in 0..SHOWN {
        let next = (0..processes.len())
            .filter(|index| !shown[..slot].contains(index))
            .max_by_key(|&index| processes[index].held);
        let Some(index) = next else { break };
        shown[slot] = index;
        let process = &processes[index];
        let len = process.comm.iter().position(|&b| b == 0).unwrap_or(process.comm.len());
        // The name last: it may contain spaces.
        writeln!(f, "queue_share_process tgid={} held={} comm={}", process.tgid, process.held,
                 core::str::from_utf8(&process.comm[..len]).unwrap_or("?"))?;
    }
    Ok(())
}

/// Gives back one queue of `key`; returns the process once it holds none, to drop unlocked.
fn give_back(processes: &mut KVec<Process>, key: usize) -> Option<Process> {
    let index = processes.iter().position(|process| process.key == key)?;
    processes[index].held -= 1;
    (processes[index].held == 0).then(|| processes.swap_remove(index))
}

/// The current process's key and an `mm` reference that keeps it from being reused.
fn current_key() -> (usize, Option<ARef<Mm>>) {
    let mm = kernel::current!().mm().map(|mm| ARef::from(&**mm));
    (mm.as_ref().map_or(0, |mm| mm.as_raw() as usize), mm)
}

/// Another device-wide resource shared between processes, under its owner's lock: by the
/// queues' rule, except that a process alone also leaves the reserve ([`may_take_held`]).
pub(crate) struct Shares(KVec<Process>);

impl Shares {
    pub(crate) const fn new() -> Self {
        Self(KVec::new())
    }

    /// Counts one more of the `max` against the current process and returns its key; EBUSY
    /// when [`may_take_held`] refuses it.
    pub(crate) fn take_current(&mut self, max: u32, flags: kernel::alloc::Flags) -> Result<usize> {
        let (key, mm) = current_key();
        take_with(&mut self.0, key, mm, max, flags, may_take_held)?;
        Ok(key)
    }

    /// Gives one back of the process `key`; returns the process once it holds none, to drop
    /// outside the owner's lock.
    pub(crate) fn give_back(&mut self, key: usize) -> Option<Process> {
        give_back(&mut self.0, key)
    }
}

/// One queue counted against the process that created it; dropping it gives the queue back.
pub(crate) struct Share {
    key: usize,
}

impl Share {
    /// Counts a new queue against the current process, of the device's `max` queues.
    pub(crate) fn take_current(max: u32) -> Result<Self> {
        let (key, mm) = current_key();
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
    fn a_lone_vm_holder_still_leaves_the_reserve() -> Result {
        // 63 G16 VM slots keep 7 free. A process alone stops at 56: the compositor started
        // after it still gets a slot.
        assert_eq!(reserve(63), 7);
        assert!(may_take_held(55, 55, 63));
        assert!(!may_take_held(56, 56, 63));
        // Below half its share a process may still take from the reserve.
        assert!(may_take_held(3, 60, 63));
        let mut processes = KVec::new();
        for _ in 0..56 {
            take_with(&mut processes, 1, None, 63, GFP_KERNEL, may_take_held)?;
        }
        assert_eq!(
            take_with(&mut processes, 1, None, 63, GFP_KERNEL, may_take_held),
            Err(EBUSY)
        );
        // A second process (the compositor) gets one of the slots that were held back.
        take_with(&mut processes, 2, None, 63, GFP_KERNEL, may_take_held)?;
        // The same lone process may take every queue: queues are not held until close.
        assert!(may_take(56, 56, 63));
        Ok(())
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
