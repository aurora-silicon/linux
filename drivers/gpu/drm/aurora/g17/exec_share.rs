// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Logical queues of one VM share one UAT execution context (G17P).
//!
//! GPU caches are tagged by address and context: queues of one VM in separate contexts see
//! each other's writes only after a memory-scope write-back at the end of each command, so
//! every cross-queue hop pays for it. One context per VM lets the firmware order queues of a
//! client by kick dependencies alone and saves TTBAT slots.
//!
//! The sharers keep their own scheduler pages, QoS identities and work lists; only the
//! TTBAT slot (the context id) is common. Work keys are the context id plus the low six bits
//! of a work-state word, so the sharers draw their work-state indices from one claim map.
//! The root is released by the last sharer's explicit release; a sharer dropped without one
//! leaves the root to the last reference, as an unshared context does.

use crate::mmu::{ExecutionContext, Vm};
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc};

struct Entry {
    vm: u64,
    execution: Arc<ExecutionContext>,
    claims: Arc<AtomicU64>,
    users: u32,
}

kernel::sync::global_lock! {
    // SAFETY: Module initialization initializes this lock before registering adapters.
    unsafe(uninit) static SHARES: Mutex<KVec<Entry>> = KVec::new();
}

/// # Safety
///
/// Called exactly once per module instance, before any queue is created.
pub(crate) unsafe fn init() {
    // SAFETY: The module initializer establishes the single-call ordering above.
    unsafe { SHARES.init() };
}

/// One logical queue's use of a shared execution context.
pub(crate) struct Use {
    vm: u64,
    execution: Arc<ExecutionContext>,
    claims: Arc<AtomicU64>,
    done: AtomicBool,
}

impl Use {
    /// The work-state index claims of the sharers.
    pub(crate) fn claims(&self) -> &Arc<AtomicU64> {
        &self.claims
    }
    /// Called where an unshared context releases its root: the last sharer releases it.
    pub(crate) fn release_execution(&self) {
        if self.done.swap(true, Ordering::AcqRel) {
            return;
        }
        if let Some(last) = leave(self.vm, &self.execution) {
            last.release();
        }
    }
}

impl Drop for Use {
    fn drop(&mut self) {
        if !self.done.swap(true, Ordering::AcqRel) {
            drop(leave(self.vm, &self.execution));
        }
    }
}

fn leave(vm: u64, execution: &Arc<ExecutionContext>) -> Option<Arc<ExecutionContext>> {
    let mut shares = SHARES.lock();
    let index = shares
        .iter()
        .position(|entry| entry.vm == vm && Arc::ptr_eq(&entry.execution, execution))?;
    shares[index].users -= 1;
    if shares[index].users != 0 {
        return None;
    }
    let entry = shares.swap_remove(index);
    drop(shares);
    Some(entry.execution)
}

/// Joins the live shared context of `vm`, if one exists.
pub(crate) fn join(vm: &Vm) -> Option<(Arc<ExecutionContext>, Use)> {
    let mut shares = SHARES.lock();
    let entry = shares
        .iter_mut()
        .find(|entry| entry.vm == vm.id() && entry.execution.is_current())?;
    entry.users = entry.users.checked_add(1)?;
    Some((
        entry.execution.clone(),
        Use {
            vm: entry.vm,
            execution: entry.execution.clone(),
            claims: entry.claims.clone(),
            done: AtomicBool::new(false),
        },
    ))
}

/// Offers a new context of `vm` for sharing.
pub(crate) fn offer(vm: &Vm, execution: &Arc<ExecutionContext>) -> Result<Use> {
    let claims = Arc::new(AtomicU64::new(0), GFP_KERNEL)?;
    let mut shares = SHARES.lock();
    shares.push(
        Entry {
            vm: vm.id(),
            execution: execution.clone(),
            claims: claims.clone(),
            users: 1,
        },
        GFP_KERNEL,
    )?;
    Ok(Use {
        vm: vm.id(),
        execution: execution.clone(),
        claims,
        done: AtomicBool::new(false),
    })
}
