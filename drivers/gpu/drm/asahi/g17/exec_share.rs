// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5: logical queues of one VM share one execution context.
//!
//! Every logical queue takes its own execution context (TTBAT slot and context id), even the
//! queues of one VM. Kicks of different contexts take turns on the M5's compute hardware, so
//! a client that spreads independent compute over several of its queues gains nothing from it,
//! while kicks of several firmware queues of one context may run side by side. With
//! `aurora.m5_shared_context` set (exec_share_param.c, read when a queue is created), the first
//! queue of a VM offers its new context and the VM's later queues join it while it is current.
//! Queues created while it is clear, and every queue of other GPUs, are unchanged.
//!
//! Each sharer keeps its own scheduler page, QoS identity, work list, render pairs and compute
//! channel with its QID; only the execution identity (its TTBAT slot and generation) is common.
//! Commands name that identity with a work-state word, so the sharers draw their words from one
//! sequence and never hold the same node index at once ([`SharedStates`]): an identity and word
//! pair stays unique, as it is for an unshared context.
//!
//! A sharer counts as current only until its own release, so exited-owner detach and rebinding
//! behave as for an unshared context. The root is returned by the last sharer's explicit
//! release, and only if every sharer left by one or was never published: a published sharer
//! dropped without it (whose scheduler release or retirement was not witnessed) leaves the root
//! to the last reference of the execution context, as an unshared context does.

use super::context::SharedStates;
use crate::mmu::{ExecutionContext, Vm};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use kernel::{prelude::*, sync::Arc};

unsafe extern "C" {
    fn asahi_g17_m5_shared_context() -> bool;
}

/// Whether M5 queues created now share their VM's execution context.
pub(crate) fn enabled() -> bool {
    // SAFETY: Reads a module parameter word.
    unsafe { asahi_g17_m5_shared_context() }
}

/// The accounting of one shared execution context's sharers.
struct Sharers {
    /// Sharers that have not left.
    users: u32,
    /// A sharer left without an explicit release.
    unwitnessed: bool,
}

impl Sharers {
    const fn new() -> Self {
        Self {
            users: 1,
            unwitnessed: false,
        }
    }

    fn join(&mut self) -> Option<()> {
        self.users = self.users.checked_add(1)?;
        Some(())
    }

    /// One sharer leaves, `witnessed` when by its explicit release or never published. Once the
    /// last one left: whether the root may be returned now.
    fn leave(&mut self, witnessed: bool) -> Option<bool> {
        self.users = self.users.saturating_sub(1);
        self.unwitnessed |= !witnessed;
        (self.users == 0).then_some(!self.unwitnessed)
    }
}

struct Entry {
    execution: Arc<ExecutionContext>,
    states: Arc<SharedStates>,
    sharers: Sharers,
    /// TEST ONLY (gpubench_opt bits 12 and 13): sharers that have not left, readable without
    /// the registry lock, and the number of uses ever made of this context.
    peers: Arc<AtomicU32>,
    joined: u32,
}

kernel::sync::global_lock! {
    // SAFETY: Module initialization initializes this lock before registering adapters.
    unsafe(uninit) static SHARES: Mutex<KVec<Entry>> = KVec::new();
}

/// Initialize the registry before a platform adapter can probe.
///
/// # Safety
///
/// Called exactly once per module instance, before any queue is created.
pub(crate) unsafe fn init() {
    // SAFETY: The module initializer establishes the single-call ordering above.
    unsafe { SHARES.init() };
}

fn same_vm(first: &Vm, second: &Vm) -> bool {
    match (first.status(), second.status()) {
        (Some(a), Some(b)) => first.id() == second.id() && Arc::ptr_eq(a, b),
        _ => false,
    }
}

/// One logical queue's use of a shared execution context.
pub(crate) struct Use {
    execution: Arc<ExecutionContext>,
    states: Arc<SharedStates>,
    left: AtomicBool,
    peers: Arc<AtomicU32>,
    index: u32,
}

impl Use {
    fn new(entry: &Entry, index: u32) -> Self {
        Self {
            execution: entry.execution.clone(),
            states: entry.states.clone(),
            left: AtomicBool::new(false),
            peers: entry.peers.clone(),
            index,
        }
    }

    /// TEST ONLY: how many queues use this execution context now, this one included (it may
    /// change as soon as it is read).
    pub(crate) fn peers(&self) -> u32 {
        self.peers.load(Ordering::Relaxed)
    }

    /// TEST ONLY: this use's position among the uses of the context, in creation order: 0 for
    /// the queue that offered it, then 1, 2, ... for the queues that joined it.
    pub(crate) fn index(&self) -> u32 {
        self.index
    }

    pub(crate) fn states(&self) -> &Arc<SharedStates> {
        &self.states
    }

    /// This queue released its use ([`Self::release_execution`]).
    pub(crate) fn released(&self) -> bool {
        self.left.load(Ordering::Acquire)
    }

    /// Called where an unshared context returns its root, once its scheduler release and every
    /// command of it were witnessed.
    pub(crate) fn release_execution(&self) {
        self.leave(true);
    }

    /// Called when the sharing context is dropped: one that was `published` and did not
    /// release its use leaves the root to the last reference.
    pub(crate) fn drop_context(&self, published: bool) {
        self.leave(!published);
    }

    fn leave(&self, witnessed: bool) {
        if self.left.swap(true, Ordering::AcqRel) {
            return;
        }
        self.peers.fetch_sub(1, Ordering::Relaxed);
        let mut shares = SHARES.lock();
        let Some(index) = shares
            .iter()
            .position(|entry| Arc::ptr_eq(&entry.execution, &self.execution))
        else {
            return;
        };
        let Some(release) = shares[index].sharers.leave(witnessed) else {
            return;
        };
        let last = shares.swap_remove(index);
        drop(shares);
        if release {
            last.execution.release();
        }
    }
}

impl Drop for Use {
    fn drop(&mut self) {
        // Left already, unless no context took this use: then nothing firmware-visible names
        // it (a creation that failed after joining).
        self.leave(true);
    }
}

/// Joins the live shared execution context of `vm`, if it has one.
pub(crate) fn join(vm: &Vm) -> Option<(Arc<ExecutionContext>, Use)> {
    let mut shares = SHARES.lock();
    let entry = shares
        .iter_mut()
        .find(|entry| same_vm(entry.execution.vm(), vm) && entry.execution.is_current())?;
    entry.sharers.join()?;
    let index = entry.joined;
    entry.joined = entry.joined.saturating_add(1);
    entry.peers.fetch_add(1, Ordering::Relaxed);
    Some((entry.execution.clone(), Use::new(entry, index)))
}

/// Offers `execution`, just created for a queue of its VM, to that VM's later queues.
pub(crate) fn offer(execution: &Arc<ExecutionContext>) -> Result<Use> {
    let entry = Entry {
        execution: execution.clone(),
        states: SharedStates::new()?,
        sharers: Sharers::new(),
        peers: Arc::new(AtomicU32::new(1), GFP_KERNEL)?,
        joined: 1,
    };
    let share = Use::new(&entry, 0);
    let pushed = SHARES.lock().push(entry, GFP_KERNEL);
    if pushed.is_err() {
        // Never listed: leaving must not find or release anything.
        share.left.store(true, Ordering::Release);
        return Err(ENOMEM);
    }
    Ok(share)
}

#[kunit_tests(asahi_g17_exec_share)]
mod tests {
    use super::*;

    #[test]
    fn the_last_explicit_release_returns_the_root() {
        let mut sharers = Sharers::new();
        sharers.join().unwrap();
        assert_eq!(sharers.leave(true), None);
        assert_eq!(sharers.leave(true), Some(true));
    }

    #[test]
    fn a_dropped_sharer_leaves_the_root_to_the_last_reference() {
        // Dropped first, then the last one releases explicitly.
        let mut sharers = Sharers::new();
        sharers.join().unwrap();
        assert_eq!(sharers.leave(false), None);
        assert_eq!(sharers.leave(true), Some(false));
        // Released first, then the last one is dropped.
        let mut sharers = Sharers::new();
        sharers.join().unwrap();
        assert_eq!(sharers.leave(true), None);
        assert_eq!(sharers.leave(false), Some(false));
    }

    #[test]
    fn a_lone_sharer_behaves_as_an_unshared_context() {
        let mut sharers = Sharers::new();
        assert_eq!(sharers.leave(true), Some(true));
        let mut sharers = Sharers::new();
        assert_eq!(sharers.leave(false), Some(false));
    }
}
