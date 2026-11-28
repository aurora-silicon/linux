#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: a G17 output fence's error is decided and signalled in one step (linux#3278).

Compiles the real `Submission`, `Aggregate`, `Member` and `ExportFence` code from
`drivers/gpu/drm/asahi/g17/fence.rs` and the real `asahi_g17_finish_fence()` path
from `g17/sync_scope.c` against a host `dma_fence` model. The model keeps the
kernel rules: `dma_fence_set_error()` WARNs on a signalled fence (the WARN of
linux#3278 at include/linux/dma-fence.h:695), and the signalled bit only
changes under the fence lock. This is not GPU execution. Checks:

- The last member decides the output: success, a failure, or an enqueue error
  that wins over a dropped member's cancellation.
- linux#3278: an early completion path (the M5 bottom-half signal) signals the
  output while its last command is still outstanding, then queue destruction
  cancels that command (`fail_while_owned(ECANCELED)`). The fence keeps the
  success its waiters saw, no WARN fires, and the submission state is still
  released by the last owner. Unscoped and scoped outputs.
- The early signal landing just before the member takes the fence lock, and the
  two racing on separate threads: never a WARN, and the error a waiter can read
  is the one the fence had when it signalled.

`--mutation` applies one known-bad change to the compiled copy; the run must
then fail:
- `unlocked-member-done`: the old member_done (set_error, then a separate signal).
- `set-error-when-signalled`: the helper sets the error even on a signalled fence.
- `check-outside-lock`: the helper tests the signalled bit before taking the lock.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'

MUTATIONS = ('unlocked-member-done', 'set-error-when-signalled', 'check-outside-lock')


def item(source, signature):
    """The item beginning at `signature`, through its matching closing brace."""
    assert source.count(signature) == 1, f'{signature!r}: {source.count(signature)} matches'
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def replace(text, old, new, count=1):
    assert text.count(old) == count, f'{old!r}: {text.count(old)} matches, expected {count}'
    return text.replace(old, new)


def fence_source(mutation):
    text = (G17 / 'fence.rs').read_text()
    parts = [
        item(text, 'unsafe extern "C" {\n    fn asahi_g17_finish_fence('),
        item(text, 'fn finish_exported('),
        item(text, 'pub(crate) struct ExportFence {'),
        item(text, 'fn waited(flag: &AtomicBool) {'),
        item(text, 'fn unwaited(flag: &AtomicBool) {'),
        item(text, 'fn compute_waited(flag: &AtomicBool) {'),
        item(text, 'fn compute_unwaited(flag: &AtomicBool) {'),
        item(text, 'impl Drop for ExportFence {'),
        '#[derive(Clone)]\n' + item(text, 'pub(crate) struct PublishedSource {'),
        item(text, 'struct State {'),
        item(text, 'pub(crate) struct Submission {'),
        item(text, 'impl Submission {'),
        '#[derive(Clone)]\npub(crate) struct Aggregate(UserFence<ExportFence>);',
        item(text, 'impl Aggregate {'),
        item(text, 'pub(crate) struct Member {'),
        item(text, 'impl Member {'),
        item(text, 'impl Drop for Member {'),
    ]
    source = '\n\n'.join(parts)
    source = source.replace('    #[pin]\n', '')
    source = replace(source, 'Arc::new(', 'arc_new(')
    source = replace(source, 'Arc::pin_init(', 'arc_pin_init(')
    source = replace(source, 'pin_init!(Self {', '(Self {')
    source = replace(source, 'state <- new_mutex!(', 'state: new_mutex!(')
    if mutation == 'unlocked-member-done':
        source = replace(source, 'finish_exported(fence, error, fence.scoped);',
                         'if error != 0 { fence.set_error(Error::from_errno(error)); } fence.signal();')
    return source


def scope_source(mutation):
    text = (G17 / 'sync_scope.c').read_text()
    parts = [
        item(text, 'static bool signal_with_error('),
        item(text, 'static bool signal_draining_scopes('),
        item(text, 'void asahi_g17_signal_fence('),
        item(text, 'bool asahi_g17_finish_fence('),
    ]
    source = '\n\n'.join(parts)
    if mutation == 'set-error-when-signalled':
        source = replace(source, 'if (first && error)', 'if (error)')
    elif mutation == 'check-outside-lock':
        source = replace(source, '\tdma_fence_lock_irqsave(fence, flags);\n\tfirst = !dma_fence_test_signaled_flag(fence);\n',
                         '\tfirst = !dma_fence_test_signaled_flag(fence);\n\tdma_fence_lock_irqsave(fence, flags);\n')
    return source


C_PRELUDE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define DMA_FENCE_FLAG_SIGNALED_BIT 0
#define MAX_ERRNO 4095
#define SCOPE_ATOMIC_BATCH 64

struct dma_fence {
	pthread_mutex_t lock;
	pthread_t owner;
	int locked;
	unsigned long flags;
	int error;
	int refs;
	/* Model bookkeeping: transitions to signalled and the error at that moment. */
	int signals;
	int error_at_signal;
};

static int total_warns;
static struct dma_fence *race_fence;

#define WARN_ON(cond) ({ int __c = !!(cond); \
	if (__c) { __atomic_fetch_add(&total_warns, 1, __ATOMIC_SEQ_CST); \
		fprintf(stderr, "WARNING: %s:%d %s\n", __FILE__, __LINE__, #cond); } __c; })

static void host_lock(struct dma_fence *fence)
{
	/* Another CPU's signal can land in any instant before this one takes the lock. */
	if (__atomic_exchange_n(&race_fence, NULL, __ATOMIC_SEQ_CST) == fence) {
		void dma_fence_signal(struct dma_fence *fence);
		dma_fence_signal(fence);
	}
	pthread_mutex_lock(&fence->lock);
	fence->owner = pthread_self();
	fence->locked = 1;
}

static void host_unlock(struct dma_fence *fence)
{
	assert(fence->locked && pthread_equal(fence->owner, pthread_self()));
	fence->locked = 0;
	pthread_mutex_unlock(&fence->lock);
}

#define dma_fence_lock_irqsave(fence, fl) do { (fl) = 0; host_lock(fence); } while (0)
#define dma_fence_unlock_irqrestore(fence, fl) do { (void)(fl); host_unlock(fence); } while (0)

static bool dma_fence_begin_signalling(void) { return true; }
static void dma_fence_end_signalling(bool cookie) { (void)cookie; }

static bool dma_fence_test_signaled_flag(struct dma_fence *fence)
{
	return __atomic_load_n(&fence->flags, __ATOMIC_ACQUIRE) & (1ul << DMA_FENCE_FLAG_SIGNALED_BIT);
}

/* include/linux/dma-fence.h: the WARN of linux#3278 is the first one. */
void dma_fence_set_error(struct dma_fence *fence, int error)
{
	WARN_ON(dma_fence_test_signaled_flag(fence));
	WARN_ON(error >= 0 || error < -MAX_ERRNO);
	fence->error = error;
}

static void dma_fence_signal_locked(struct dma_fence *fence)
{
	assert(fence->locked && pthread_equal(fence->owner, pthread_self()));
	if (__atomic_fetch_or(&fence->flags, 1ul << DMA_FENCE_FLAG_SIGNALED_BIT, __ATOMIC_ACQ_REL) &
	    (1ul << DMA_FENCE_FLAG_SIGNALED_BIT))
		return;
	fence->error_at_signal = fence->error;
	fence->signals++;
}

void dma_fence_signal(struct dma_fence *fence)
{
	host_lock(fence);
	dma_fence_signal_locked(fence);
	host_unlock(fence);
}

/* Per-CPU ready list of completed queue scopes; this test attaches none. */
struct llist_node { struct llist_node *next; };
struct llist_head { struct llist_node *first; };
#define LLIST_HEAD_INIT(name) { NULL }
#define llist_for_each_safe(pos, n, node) \
	for ((pos) = (node); (pos) && ((n) = (pos)->next, 1); (pos) = (n))
static struct llist_node *llist_del_all(struct llist_head *head)
{
	return __atomic_exchange_n(&head->first, NULL, __ATOMIC_ACQ_REL);
}
struct asahi_g17_scope { struct llist_node ready; };
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
static __thread struct llist_head *active_scopes;
#define __this_cpu_read(var) (var)
#define __this_cpu_write(var, value) ((var) = (value))
#define preempt_disable() do { } while (0)
#define preempt_enable() do { } while (0)
#define local_irq_save(fl) ((fl) = 0)
#define local_irq_restore(fl) ((void)(fl))
static void scope_complete(struct asahi_g17_scope *scope) { (void)scope; abort(); }
static void scope_defer_list(struct llist_node *nodes) { if (nodes) abort(); }

void asahi_g17_signal_fence(struct dma_fence *fence);
bool asahi_g17_finish_fence(struct dma_fence *fence, int error, bool scoped);
'''

C_EPILOGUE = r'''
struct dma_fence *host_fence_new(void)
{
	struct dma_fence *fence = calloc(1, sizeof(*fence));
	assert(fence);
	pthread_mutex_init(&fence->lock, NULL);
	fence->refs = 1;
	return fence;
}
void host_fence_get(struct dma_fence *fence) { __atomic_fetch_add(&fence->refs, 1, __ATOMIC_SEQ_CST); }
void host_fence_put(struct dma_fence *fence)
{
	if (__atomic_sub_fetch(&fence->refs, 1, __ATOMIC_SEQ_CST) == 0)
		free(fence);
}
bool host_fence_signalled(struct dma_fence *fence) { return dma_fence_test_signaled_flag(fence); }
int host_fence_error(struct dma_fence *fence) { return fence->error; }
int host_fence_error_at_signal(struct dma_fence *fence) { return fence->error_at_signal; }
int host_fence_signals(struct dma_fence *fence) { return fence->signals; }
int host_warns(void) { return __atomic_load_n(&total_warns, __ATOMIC_SEQ_CST); }
void host_race_before_lock(struct dma_fence *fence) { __atomic_store_n(&race_fence, fence, __ATOMIC_SEQ_CST); }
'''

RUST_PRELUDE = r'''
#![allow(dead_code, unused_imports, unused_variables, unused_parens, unexpected_cfgs, private_interfaces)]

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(i32);
impl Error {
    pub fn from_errno(errno: i32) -> Error { Error(errno) }
    pub fn to_errno(self) -> i32 { self.0 }
}
pub type Result<T = ()> = core::result::Result<T, Error>;
pub const EIO: Error = Error(-5);
pub const ENOMEM: Error = Error(-12);
pub const ECANCELED: Error = Error(-125);
#[derive(Clone, Copy)] pub struct Flags;
pub const GFP_KERNEL: Flags = Flags;
pub fn arc_new<T>(value: T, _: Flags) -> Result<std::sync::Arc<T>> { Ok(std::sync::Arc::new(value)) }
pub fn arc_pin_init<T>(value: T, _: Flags) -> Result<std::sync::Arc<T>> { Ok(std::sync::Arc::new(value)) }
macro_rules! new_mutex { ($value:expr, $name:expr $(,)?) => { crate::kernel::sync::Mutex::new($value) } }

pub mod kernel {
    pub mod bindings {
        #[repr(C)] pub struct dma_fence { _private: [u8; 0] }
    }
    pub mod sync {
        pub type Arc<T> = std::sync::Arc<T>;
        pub struct Mutex<T>(std::sync::Mutex<T>);
        impl<T> Mutex<T> {
            pub fn new(value: T) -> Self { Self(std::sync::Mutex::new(value)) }
            pub fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() }
        }
    }
    pub mod dma_fence {
        use super::bindings;
        unsafe extern "C" {
            pub fn dma_fence_signal(fence: *mut bindings::dma_fence);
            pub fn dma_fence_set_error(fence: *mut bindings::dma_fence, error: i32);
            pub fn host_fence_new() -> *mut bindings::dma_fence;
            pub fn host_fence_get(fence: *mut bindings::dma_fence);
            pub fn host_fence_put(fence: *mut bindings::dma_fence);
        }
        pub trait RawDmaFence {
            fn raw(&self) -> *mut bindings::dma_fence;
            fn signal(&self) { unsafe { dma_fence_signal(self.raw()) } }
            fn set_error(&self, err: crate::Error) { unsafe { dma_fence_set_error(self.raw(), err.to_errno()) } }
        }
        pub struct Fence(*mut bindings::dma_fence);
        unsafe impl Send for Fence {}
        unsafe impl Sync for Fence {}
        impl Fence {
            pub fn from_fence(fence: &dyn RawDmaFence) -> Fence {
                unsafe { host_fence_get(fence.raw()) };
                Fence(fence.raw())
            }
        }
        impl Clone for Fence { fn clone(&self) -> Self { unsafe { host_fence_get(self.0) }; Fence(self.0) } }
        impl Drop for Fence { fn drop(&mut self) { unsafe { host_fence_put(self.0) } } }
        impl RawDmaFence for Fence { fn raw(&self) -> *mut bindings::dma_fence { self.0 } }
        /// A driver fence and its payload, as `FenceObject<T>` keeps them.
        pub struct Object<T> { raw: *mut bindings::dma_fence, pub payload: T }
        impl<T> Drop for Object<T> { fn drop(&mut self) { unsafe { host_fence_put(self.raw) } } }
        pub struct UserFence<T>(std::sync::Arc<Object<T>>);
        unsafe impl<T: Send> Send for UserFence<T> {}
        unsafe impl<T: Sync> Sync for UserFence<T> {}
        impl<T> UserFence<T> {
            pub fn new(payload: T) -> Self { Self(std::sync::Arc::new(Object { raw: unsafe { host_fence_new() }, payload })) }
        }
        impl<T> Clone for UserFence<T> { fn clone(&self) -> Self { Self(self.0.clone()) } }
        impl<T> core::ops::Deref for UserFence<T> { type Target = T; fn deref(&self) -> &T { &self.0.payload } }
        impl<T> RawDmaFence for UserFence<T> { fn raw(&self) -> *mut bindings::dma_fence { self.0.raw } }
    }
}

unsafe extern "C" {
    fn host_fence_signalled(fence: *mut kernel::bindings::dma_fence) -> bool;
    fn host_fence_error(fence: *mut kernel::bindings::dma_fence) -> i32;
    fn host_fence_error_at_signal(fence: *mut kernel::bindings::dma_fence) -> i32;
    fn host_fence_signals(fence: *mut kernel::bindings::dma_fence) -> i32;
    fn host_warns() -> i32;
    fn host_race_before_lock(fence: *mut kernel::bindings::dma_fence);
}

/// What a waiter can read once the fence signalled.
#[derive(Debug, PartialEq, Eq)]
pub struct Seen { signalled: bool, error: i32, error_at_signal: i32, signals: i32 }
pub fn seen(raw: *mut kernel::bindings::dma_fence) -> Seen {
    unsafe {
        Seen {
            signalled: host_fence_signalled(raw),
            error: host_fence_error(raw),
            error_at_signal: host_fence_error_at_signal(raw),
            signals: host_fence_signals(raw),
        }
    }
}
pub fn warns() -> i32 { unsafe { host_warns() } }
pub fn race_before_lock(raw: *mut kernel::bindings::dma_fence) { unsafe { host_race_before_lock(raw) } }

pub mod g17 {
    pub fn wake_for_compute_waiter() {}
    pub mod status { pub struct VmStatus; }
    pub mod dependency { pub const PRODUCER_FAILED: crate::Error = crate::Error(-71); }
    pub mod trace {
        pub fn output(_fence: &dyn crate::kernel::dma_fence::RawDmaFence, _result: i32) {}
    }
    pub mod fw { pub mod kick { #[derive(Clone)] pub struct KickDependency; } }
    pub mod context {
        use crate::kernel::sync::Arc;
        pub struct Context;
        pub struct WorkStateLease(Arc<super::status::VmStatus>);
        impl WorkStateLease {
            pub fn deferred(_context: Arc<Context>) -> crate::Result<Self> { Ok(Self(Arc::new(super::status::VmStatus))) }
            pub fn status(&self) -> &Arc<super::status::VmStatus> { &self.0 }
        }
    }
    pub mod fence {
        use super::context::{Context, WorkStateLease};
        use crate::kernel::{bindings, dma_fence::{Fence, RawDmaFence, UserFence}, sync::{Arc, Mutex}};
        use crate::{arc_new, arc_pin_init, Error, Result, ECANCELED, EIO, ENOMEM, GFP_KERNEL};
        use crate::{race_before_lock, seen, warns, Seen};
        use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, AtomicU8, AtomicUsize, Ordering};

        static WAITERS: AtomicU32 = AtomicU32::new(0);
        static WAITS: AtomicU64 = AtomicU64::new(0);
        static COMPUTE_WAITERS: AtomicU32 = AtomicU32::new(0);
        fn asahi_g17_note_compute_waiter() { COMPUTE_WAITERS.fetch_add(1, Ordering::AcqRel); }
        fn asahi_g17_unnote_compute_waiter() { COMPUTE_WAITERS.fetch_sub(1, Ordering::AcqRel); }

@FENCE@

        include!("scenarios.rs");
    }
}

fn main() {
    g17::fence::run();
    println!("PASS g17-output-fence-tests");
}
'''

SCENARIOS = r'''
fn new_aggregate(scoped: bool) -> Aggregate {
    let mut export = ExportFence {
        source: Some(Submission::with_work(None, false).unwrap()),
        scoped,
        compute_only: false,
        waited: AtomicBool::new(false),
        compute_waited: AtomicBool::new(false),
    };
    export.scoped = scoped;
    Aggregate(UserFence::new(export))
}

fn released(aggregate: &Aggregate) -> bool {
    let source = aggregate.source();
    source.owners.load(Ordering::Acquire) == 0 && source.state.lock().ready.is_none()
}

/// The ioctl publishes one command and finishes enqueueing it: only its member is left.
fn one_command(scoped: bool) -> (Aggregate, Member) {
    let aggregate = new_aggregate(scoped);
    aggregate.source().state.lock().ready = Some(Fence::from_fence(&UserFence::new(())));
    let member = aggregate.member();
    aggregate.finish_enqueue(Ok(()));
    assert!(!seen(aggregate.0.raw()).signalled, "the enqueue guard alone must not signal");
    (aggregate, member)
}

fn decided_by_last_member() {
    for scoped in [false, true] {
        let (aggregate, member) = one_command(scoped);
        member.complete(Ok(()));
        let raw = aggregate.0.raw();
        assert_eq!(seen(raw), Seen { signalled: true, error: 0, error_at_signal: 0, signals: 1 });
        assert!(aggregate.source().state.lock().ready.is_none(), "success releases the state");
        drop(member);
        assert!(released(&aggregate));

        let (aggregate, member) = one_command(scoped);
        member.complete(Err(EIO));
        let raw = aggregate.0.raw();
        assert_eq!(seen(raw), Seen { signalled: true, error: EIO.to_errno(),
                                     error_at_signal: EIO.to_errno(), signals: 1 });
        assert!(aggregate.source().state.lock().ready.is_some(), "a failure keeps the state until its owner goes");
        drop(member);
        assert!(released(&aggregate));

        let aggregate = new_aggregate(scoped);
        drop(aggregate.member());
        aggregate.finish_enqueue(Err(ENOMEM));
        let raw = aggregate.0.raw();
        assert_eq!(seen(raw), Seen { signalled: true, error: ENOMEM.to_errno(),
                                     error_at_signal: ENOMEM.to_errno(), signals: 1 });
    }
    assert_eq!(warns(), 0, "the last member signalled with a WARN");
}

/// linux#3278: the M5 bottom half signals the output once the stamp of its only remaining
/// command lands (remaining == 1, no error), before the command retires. Queue destruction
/// then cancels the unretired command: `fail_while_owned(ECANCELED)` completes its member.
fn early_signal_then_cancel() {
    for scoped in [false, true] {
        let (aggregate, member) = one_command(scoped);
        let raw = aggregate.0.raw();
        aggregate.0.signal();
        assert_eq!(seen(raw), Seen { signalled: true, error: 0, error_at_signal: 0, signals: 1 });
        member.complete(Err(ECANCELED));
        assert_eq!(warns(), 0, "dma_fence_set_error() on a signalled output fence (scoped={})", scoped);
        assert_eq!(seen(raw), Seen { signalled: true, error: 0, error_at_signal: 0, signals: 1 },
                   "the output must keep the success its waiters saw (scoped={})", scoped);
        drop(member);
        assert!(released(&aggregate), "the last owner releases the state");
    }
}

/// The early signal lands just before the cancelled member takes the fence lock.
fn early_signal_before_lock() {
    for scoped in [false, true] {
        let (aggregate, member) = one_command(scoped);
        let raw = aggregate.0.raw();
        race_before_lock(raw);
        member.complete(Err(ECANCELED));
        assert_eq!(warns(), 0, "dma_fence_set_error() after a racing signal (scoped={})", scoped);
        assert_eq!(seen(raw), Seen { signalled: true, error: 0, error_at_signal: 0, signals: 1 },
                   "the error changed after the fence signalled (scoped={})", scoped);
    }
}

fn spin(turns: usize) {
    for _ in 0..turns * 8 {
        core::hint::spin_loop();
    }
}

/// The early signal and the cancellation on two threads.
fn early_signal_races_cancel() {
    let (mut early, mut late) = (0, 0);
    for round in 0..97 * 97 {
        let scoped = round % 2 == 1;
        let (aggregate, member) = one_command(scoped);
        let raw = aggregate.0.raw() as usize;
        let start = std::sync::Arc::new(std::sync::Barrier::new(2));
        let bh = {
            let start = start.clone();
            std::thread::spawn(move || {
                start.wait();
                spin(round % 97);
                // SAFETY: `aggregate` outlives the join below.
                unsafe { crate::kernel::dma_fence::dma_fence_signal(raw as *mut bindings::dma_fence) };
            })
        };
        start.wait();
        spin(round / 97 % 97);
        member.complete(Err(ECANCELED));
        bh.join().unwrap();
        let seen = seen(raw as *mut bindings::dma_fence);
        assert_eq!(warns(), 0, "dma_fence_set_error() raced a signal in round {}", round);
        assert_eq!(seen.signals, 1);
        assert_eq!(seen.error, seen.error_at_signal, "the error changed after the fence signalled");
        if seen.error == 0 { early += 1 } else { late += 1 }
        drop(member);
        assert!(released(&aggregate));
    }
    println!("race rounds: early signal won {} cancel won {}", early, late);
}

pub fn run() {
    decided_by_last_member();
    early_signal_then_cancel();
    early_signal_before_lock();
    early_signal_races_cancel();
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=MUTATIONS)
    args = ap.parse_args()

    if args.mutation is None:
        # Source rules: the output fence is decided only through the locked helper.
        fence = (G17 / 'fence.rs').read_text()
        member_done = item(fence, '    fn member_done(')
        assert 'finish_exported(fence, error, fence.scoped);' in member_done
        assert '.set_error(' not in member_done and '.signal()' not in member_done
        assert 'self.complete.swap(true, Ordering::AcqRel)' in item(fence, '    pub(crate) fn complete(')

    cc = os.environ.get('HOSTCC', 'cc')
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-output-fence-') as td:
        td = Path(td)
        (td / 'model.c').write_text(C_PRELUDE + scope_source(args.mutation) + C_EPILOGUE)
        subprocess.run([cc, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-c',
                        '-o', str(td / 'model.o'), str(td / 'model.c')], check=True)
        subprocess.run(['ar', 'rcs', str(td / 'libmodel.a'), str(td / 'model.o')], check=True)
        (td / 'scenarios.rs').write_text(SCENARIOS)
        indented = '\n'.join(('        ' + line) if line else '' for line in
                             fence_source(args.mutation).splitlines())
        (td / 'test.rs').write_text(RUST_PRELUDE.replace('@FENCE@', indented))
        subprocess.run([rustc, '--edition=2021', '-O', '-C', 'debug-assertions', '-o', str(td / 'test'),
                        str(td / 'test.rs'), '-L', str(td), '-l', 'static=model',
                        '-C', 'link-arg=-pthread'], check=True)
        result = subprocess.run([str(td / 'test')])
        sys.exit(result.returncode)


if __name__ == '__main__':
    main()
