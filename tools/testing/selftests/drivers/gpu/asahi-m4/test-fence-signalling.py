#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production DMA-fence lockdep guard and Timeline off-pending signalling."""
from common import kernel_root, output_dir, rustc, sha
import json
import os
import pathlib
import shlex
import subprocess
import tempfile

root = kernel_root()
out = output_dir()
production = root / 'rust/kernel/dma_fence.rs'
source = production.read_text()
guard = source[source.index('/// A lockdep annotation'):source.index('mod private {')]
harness = r'''
#![allow(dead_code)]
mod bindings {
    use std::cell::{Cell,RefCell};
    std::thread_local! {
        static EVENTS:RefCell<Vec<(bool,bool)>> = const { RefCell::new(Vec::new()) };
        static HELD:Cell<bool> = const { Cell::new(false) };
    }
    pub fn events()->Vec<(bool,bool)> { EVENTS.with(|e|e.borrow().clone()) }
    pub unsafe fn dma_fence_begin_signalling()->bool {
        let cookie=HELD.with(|held|held.replace(true));
        EVENTS.with(|e|e.borrow_mut().push((true,cookie))); cookie
    }
    pub unsafe fn dma_fence_end_signalling(cookie:bool) {
        if !cookie { HELD.with(|held|held.set(false)); }
        EVENTS.with(|e|e.borrow_mut().push((false,cookie)));
    }
}
__GUARD__
#[test]
fn nested_sections_keep_their_own_cookie_and_close_in_order() {
    let outer=SignallingGuard::new(); let inner=SignallingGuard::new(); let deepest=SignallingGuard::new();
    drop(deepest); drop(inner); drop(outer);
    #[cfg(CONFIG_LOCKDEP)] assert_eq!(bindings::events(),[(true,false),(true,true),(true,true),(false,true),(false,true),(false,false)]);
    #[cfg(not(CONFIG_LOCKDEP))] assert!(bindings::events().is_empty());
}
#[test]
fn early_return_closes_the_annotation() {
    fn refuse()->Result<(),()> { let _section=SignallingGuard::new(); Err(()) }
    assert!(refuse().is_err());
    #[cfg(CONFIG_LOCKDEP)] assert_eq!(bindings::events(),[(true,false),(false,false)]);
    #[cfg(not(CONFIG_LOCKDEP))] assert!(bindings::events().is_empty());
}
'''.replace('__GUARD__', guard)
unit = out / 'fence-signalling.rs'
unit.write_text(harness)
logs = ''
for lockdep in [False, True]:
    binary = out / ('fence-signalling-' + str(int(lockdep)))
    cfg = ['--cfg', 'CONFIG_LOCKDEP'] if lockdep else []
    for command in [rustc() + ['--edition=2021', '--test', str(unit), '-o', str(binary)] + cfg,
                    [str(binary), '--nocapture']]:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs += result.stdout
        print(result.stdout, end='')
        (out / 'fence-signalling.log').write_text(logs)
        if result.returncode:
            raise SystemExit(result.returncode)
# The cookie belongs to a task: moving its production guard to another thread must fail.
not_send = out / 'fence-signalling-not-send.rs'
not_send.write_text(harness + '\nfn main(){ let guard=SignallingGuard::new(); std::thread::spawn(move||drop(guard)); }\n')
result = subprocess.run(rustc() + ['--edition=2021', str(not_send), '-o', str(out/'not-send')],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'fence-signalling-not-send.log').write_text(result.stdout)
assert result.returncode and 'E0277' in result.stdout and 'cannot be sent between threads safely' in result.stdout, result.stdout

fence_source_path = root / 'drivers/gpu/drm/asahi/g16/fence.rs'
fence_source = fence_source_path.read_text()


def extract_function(text, name):
    marker = text.index('fn ' + name + '(')
    start = text.rfind('\n', 0, marker) + 1
    first = text.index('{', marker)
    depth = 1
    at = first + 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[start:at]


signal_batch_line = [
    line for line in fence_source.splitlines() if line.startswith('const SIGNAL_BATCH:')
][0]
timeline_complete_fn = extract_function(fence_source, 'complete')
input_fences_src = fence_source[
    fence_source.index('/// Inline storage for the common 0-2 input fences'):
    fence_source.index('/// Reads a retained dependency')
]
fw_waits_src = fence_source[
    fence_source.index('/// Maximum distinct cross-queue firmware waits'):
    fence_source.index('/// Fence operations of G16 job fences.')
]
syncs_src = fence_source[
    fence_source.index('/// The syncs of one SUBMIT.'):
]

timeline_harness = r'''
#![allow(dead_code)]
use std::sync::{Arc, Barrier, Mutex as StdMutex};
use std::sync::atomic::{AtomicI32, AtomicU64, Ordering};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Error(i32);
const EIO: Error = Error(-5);
const ENOMEM: Error = Error(-12);
const ECANCELED: Error = Error(-125);
const GFP_KERNEL: u32 = 0;
type Result<T = ()> = std::result::Result<T, Error>;

mod input_fences_test {
    use super::{Result, ENOMEM, GFP_KERNEL};
    use std::cell::Cell;

    std::thread_local! {
        static ALLOCS: Cell<u32> = const { Cell::new(0) };
        static FAIL_NEXT: Cell<bool> = const { Cell::new(false) };
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub(crate) struct Fence(pub(crate) u32);

    pub(crate) struct KVec<T>(Vec<T>);
    impl<T> KVec<T> {
        fn with_capacity(cap: usize, _flags: u32) -> Result<Self> {
            if FAIL_NEXT.with(|f| f.replace(false)) {
                return Err(ENOMEM);
            }
            ALLOCS.with(|a| a.set(a.get() + 1));
            Ok(Self(Vec::with_capacity(cap)))
        }
        fn push(&mut self, v: T, _flags: u32) -> Result {
            if FAIL_NEXT.with(|f| f.replace(false)) {
                return Err(ENOMEM);
            }
            self.0.push(v);
            Ok(())
        }
        fn push_within_capacity(&mut self, v: T) -> std::result::Result<(), T> {
            self.0.push(v);
            Ok(())
        }
    }
    impl<T> core::ops::Deref for KVec<T> {
        type Target = [T];
        fn deref(&self) -> &[T] { &self.0 }
    }

    INPUT_FENCES_SRC

    #[test]
    fn input_fences_inline_zero_to_two_and_spill_safely() {
        ALLOCS.with(|a| a.set(0));
        let mut inputs = InputFences::new();
        assert_eq!(&*inputs, &[]);
        inputs.push(Fence(10)).unwrap();
        assert_eq!(&*inputs, &[Fence(10)]);
        inputs.push(Fence(20)).unwrap();
        assert_eq!(&*inputs, &[Fence(10), Fence(20)]);
        assert_eq!(ALLOCS.with(|a| a.get()), 0);
        // OOM during 2 -> 3 spill preserves the 2 inline fences intact.
        FAIL_NEXT.with(|f| f.set(true));
        assert_eq!(inputs.push(Fence(30)), Err(ENOMEM));
        assert_eq!(&*inputs, &[Fence(10), Fence(20)]);
        assert_eq!(ALLOCS.with(|a| a.get()), 0);
        // Spill succeeds with one allocation and keeps exact order.
        inputs.push(Fence(30)).unwrap();
        inputs.push(Fence(40)).unwrap();
        assert_eq!(ALLOCS.with(|a| a.get()), 1);
        assert_eq!(&*inputs, &[Fence(10), Fence(20), Fence(30), Fence(40)]);
    }
}

struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> {
    fn new(v: T) -> Self { Self(StdMutex::new(v)) }
    fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() }
    fn try_lock(&self) -> std::sync::TryLockResult<std::sync::MutexGuard<'_, T>> {
        self.0.try_lock()
    }
}

#[derive(Clone)]
struct JobFenceOps;
struct FenceInner {
    seqno: u64,
    error: AtomicI32,
    log: Arc<StdMutex<Vec<(u64, i32)>>>,
    on_signal: Option<Arc<dyn Fn(u64) + Send + Sync>>,
}
#[derive(Clone)]
struct UserFence<T>(Arc<FenceInner>, core::marker::PhantomData<T>);
impl UserFence<JobFenceOps> {
    fn new(
        seqno: u64,
        log: Arc<StdMutex<Vec<(u64, i32)>>>,
        on_signal: Option<Arc<dyn Fn(u64) + Send + Sync>>,
    ) -> Self {
        Self(
            Arc::new(FenceInner {
                seqno,
                error: AtomicI32::new(0),
                log,
                on_signal,
            }),
            core::marker::PhantomData,
        )
    }
    fn raw(&self) -> *const FenceInner { Arc::as_ptr(&self.0) }
    fn set_error(&self, error: Error) { self.0.error.store(error.0, Ordering::Release); }
    fn signal(&self) {
        if let Some(cb) = &self.0.on_signal {
            cb(self.0.seqno);
        }
        self.0.log.lock().unwrap().push((self.0.seqno, self.0.error.load(Ordering::Acquire)));
    }
}

fn signal_fence(fence: &UserFence<JobFenceOps>) {
    fence.signal();
}

SIGNAL_BATCH_LINE

struct Pending {
    fence: UserFence<JobFenceOps>,
    result: Option<Result>,
}

struct Timeline {
    next_seq: AtomicU64,
    signaller: Mutex<()>,
    pending: Mutex<Vec<Pending>>,
    log: Arc<StdMutex<Vec<(u64, i32)>>>,
}

impl Timeline {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            next_seq: AtomicU64::new(1),
            signaller: Mutex::new(()),
            pending: Mutex::new(Vec::new()),
            log: Arc::new(StdMutex::new(Vec::new())),
        })
    }

    fn next(&self, on_signal: Option<Arc<dyn Fn(u64) + Send + Sync>>) -> UserFence<JobFenceOps> {
        let seq = self.next_seq.fetch_add(1, Ordering::AcqRel);
        let fence = UserFence::new(seq, self.log.clone(), on_signal);
        self.pending.lock().push(Pending { fence: fence.clone(), result: None });
        fence
    }

    TIMELINE_COMPLETE_FN
}

#[test]
fn out_of_order_and_large_batches_signal_strictly_in_order() {
    let tl = Timeline::new();
    let count = SIGNAL_BATCH * 3 + 2;
    let fences: Vec<_> = (0..count).map(|_| tl.next(None)).collect();
    for i in (1..count).rev() {
        let res = if i % 7 == 0 { Err(EIO) } else { Ok(()) };
        tl.complete(&fences[i], res);
    }
    assert!(tl.log.lock().unwrap().is_empty());
    tl.complete(&fences[1], Err(ECANCELED));
    tl.complete(&fences[0], Ok(()));
    let log = tl.log.lock().unwrap().clone();
    assert_eq!(log.len(), count);
    for (idx, (seq, err)) in log.iter().enumerate() {
        assert_eq!(*seq, (idx + 1) as u64);
        assert_eq!(*err, if idx != 0 && idx % 7 == 0 { EIO.0 } else { 0 });
    }
    assert!(tl.pending.lock().is_empty());
    tl.complete(&fences[0], Err(EIO));
    assert_eq!(tl.log.lock().unwrap().len(), count);
}

#[test]
fn signal_callbacks_do_not_hold_pending_lock_and_allow_next() {
    let tl = Timeline::new();
    let tl_cb = tl.clone();
    let f1 = tl.next(Some(Arc::new(move |_| {
        assert!(tl_cb.pending.try_lock().is_ok());
        let _f_nested = tl_cb.next(None);
    })));
    tl.complete(&f1, Ok(()));
    assert_eq!(tl.log.lock().unwrap().len(), 1);
    assert_eq!(tl.pending.lock().len(), 1);
}

#[test]
fn concurrent_completers_preserve_strict_fifo_signal_order() {
    for _ in 0..64 {
        let tl = Timeline::new();
        let n = 16usize;
        let fences: Vec<_> = (0..n).map(|_| tl.next(None)).collect();
        let barrier = Arc::new(Barrier::new(n));
        let mut threads = Vec::with_capacity(n);
        for (i, fence) in fences.into_iter().enumerate() {
            let tl = tl.clone();
            let barrier = barrier.clone();
            threads.push(std::thread::spawn(move || {
                barrier.wait();
                tl.complete(&fence, if i == 5 { Err(ECANCELED) } else { Ok(()) });
            }));
        }
        for t in threads {
            t.join().unwrap();
        }
        let log = tl.log.lock().unwrap().clone();
        assert_eq!(log.len(), n);
        for (idx, (seq, err)) in log.iter().enumerate() {
            assert_eq!(*seq, (idx + 1) as u64);
            assert_eq!(*err, if idx == 5 { ECANCELED.0 } else { 0 });
        }
        assert!(tl.pending.lock().is_empty());
    }
}

mod cumulative_syncs_test {
    use super::{Error, Result, EIO, ENOMEM};
    use std::cell::RefCell;
    use std::rc::Rc;

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub(crate) struct Fence(pub(crate) i32);

    pub(crate) struct Timeline;
    impl Timeline {
        pub(crate) fn refused(&self, error: Error) -> Fence {
            Fence(error.0)
        }
    }

    pub(crate) mod g17 {
        pub(crate) mod sync_output {
            use super::super::{Fence, Result, ENOMEM};
            use std::cell::RefCell;
            use std::rc::Rc;

            #[derive(Default)]
            pub(crate) struct BundleLog {
                pub(crate) published: Vec<Fence>,
                pub(crate) compute_only: Vec<bool>,
                pub(crate) failed: Vec<Fence>,
                pub(crate) fail_publish: bool,
            }

            pub(crate) struct SignalBundle(pub(crate) Rc<RefCell<BundleLog>>);
            impl SignalBundle {
                pub(crate) fn publish(&mut self, fence: &Fence, compute_only: bool) -> Result {
                    assert!(
                        !compute_only,
                        "G16 must not trigger G17 compute-waiter effort accounting"
                    );
                    let mut log = self.0.borrow_mut();
                    log.compute_only.push(compute_only);
                    if log.fail_publish {
                        return Err(ENOMEM);
                    }
                    log.published.push(*fence);
                    Ok(())
                }
                pub(crate) fn fail(&mut self, fence: &Fence) {
                    self.0.borrow_mut().failed.push(*fence);
                }
            }
        }
    }

    pub(crate) struct SyncItem {
        pub(crate) installed: Rc<RefCell<Vec<Fence>>>,
        pub(crate) cumulative: Option<g17::sync_output::SignalBundle>,
    }

    pub(crate) type KVVec<T> = Vec<T>;

    fn install(outputs: impl Iterator<Item = SyncItem>, fence: &Fence) {
        for sync in outputs {
            sync.installed.borrow_mut().push(*fence);
        }
    }

    SYNCS_SRC

    #[test]
    fn cumulative_bundle_publishes_and_handles_oom_refusal_cleanly() {
        let log = Rc::new(RefCell::new(g17::sync_output::BundleLog::default()));
        let out_log = Rc::new(RefCell::new(Vec::new()));
        let items = vec![
            SyncItem { installed: out_log.clone(), cumulative: None },
            SyncItem {
                installed: out_log.clone(),
                cumulative: Some(g17::sync_output::SignalBundle(log.clone())),
            },
        ];
        let mut syncs = Syncs::new(items, 1);
        assert_eq!(syncs.inputs().len(), 1);
        syncs.publish(&Fence(42)).unwrap();
        assert_eq!(log.borrow().published, vec![Fence(42)]);
        assert!(log.borrow().failed.is_empty());
        assert!(out_log.borrow().is_empty());
        // Calling refuse() after successful publish() is a no-op.
        syncs.refuse(&Timeline, EIO);
        assert!(log.borrow().failed.is_empty());

        // ENOMEM during publish() leaves bundle armed so refuse() installs error fence.
        let log2 = Rc::new(RefCell::new(g17::sync_output::BundleLog {
            fail_publish: true,
            ..Default::default()
        }));
        let items2 = vec![SyncItem {
            installed: out_log.clone(),
            cumulative: Some(g17::sync_output::SignalBundle(log2.clone())),
        }];
        let mut syncs2 = Syncs::new(items2, 0);
        assert_eq!(syncs2.publish(&Fence(99)), Err(ENOMEM));
        assert!(log2.borrow().published.is_empty());
        syncs2.refuse(&Timeline, ENOMEM);
        assert_eq!(log2.borrow().failed, vec![Fence(ENOMEM.0)]);
    }
}

mod fw_waits_test {
    use super::{Error, Result, GFP_KERNEL};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;

    const E2BIG: Error = Error(-7);

    pub(crate) struct QueueState(u32);

    pub(crate) struct KVec<T>(Vec<T>);
    impl<T> KVec<T> {
        fn with_capacity(cap: usize, _flags: u32) -> Result<Self> {
            Ok(Self(Vec::with_capacity(cap)))
        }
        fn push(&mut self, v: T, _flags: u32) -> Result {
            self.0.push(v);
            Ok(())
        }
        fn push_within_capacity(&mut self, v: T) -> std::result::Result<(), T> {
            self.0.push(v);
            Ok(())
        }
    }
    impl<T> core::ops::Deref for KVec<T> {
        type Target = [T];
        fn deref(&self) -> &[T] { &self.0 }
    }
    impl<T> core::ops::DerefMut for KVec<T> {
        fn deref_mut(&mut self) -> &mut [T] { &mut self.0 }
    }

    FW_WAITS_SRC

    #[test]
    fn fw_waits_coalesces_deep_two_queue_timeline_chain_and_merges_engines() {
        let q0 = Arc::new(QueueState(0));
        let q1 = Arc::new(QueueState(1));
        let mut waits = FwWaits::Empty;
        // 64 alternating timeline hops across 2 queues coalesce to 2 per-queue entries.
        for hop in 1..=64u64 {
            let (q, r, c) = if hop % 2 == 1 {
                (q0.clone(), Some((hop + 1) / 2), Some(hop))
            } else {
                (q1.clone(), None, Some(hop / 2))
            };
            let origin = Arc::new(FenceOrigin::new(1, 7, q, [r, c]));
            waits.push_unique(origin).unwrap();
        }
        assert_eq!(waits.len(), 2);
        let w0 = waits.iter().find(|w| Arc::ptr_eq(&w.queue, &q0)).unwrap();
        let w1 = waits.iter().find(|w| Arc::ptr_eq(&w.queue, &q1)).unwrap();
        assert_eq!(w0.last_ordinal, [Some(32), Some(63)]);
        assert_eq!(w1.last_ordinal, [None, Some(32)]);

        // Merging an earlier render-only origin with a later compute-only origin on q1 keeps both.
        waits
            .push_unique(Arc::new(FenceOrigin::new(1, 7, q1.clone(), [Some(5), None])))
            .unwrap();
        let w1 = waits.iter().find(|w| Arc::ptr_eq(&w.queue, &q1)).unwrap();
        assert_eq!(w1.last_ordinal, [Some(5), Some(32)]);
    }

    #[test]
    fn fw_waits_caps_distinct_queues_at_max_fw_waits() {
        let mut waits = FwWaits::Empty;
        for idx in 0..MAX_FW_WAITS {
            let q = Arc::new(QueueState(idx as u32));
            let origin = Arc::new(FenceOrigin::new(1, 7, q, [None, Some(1)]));
            waits.push_unique(origin).unwrap();
        }
        assert_eq!(waits.len(), MAX_FW_WAITS);
        let extra = Arc::new(FenceOrigin::new(
            1,
            7,
            Arc::new(QueueState(99)),
            [None, Some(1)],
        ));
        assert_eq!(waits.push_unique(extra), Err(E2BIG));
    }
}
'''.replace('SIGNAL_BATCH_LINE', signal_batch_line).replace(
    'TIMELINE_COMPLETE_FN', timeline_complete_fn
).replace('INPUT_FENCES_SRC', input_fences_src).replace(
    'FW_WAITS_SRC', fw_waits_src
).replace(
    'SYNCS_SRC', syncs_src.replace('crate::g17::', 'g17::')
)

with tempfile.TemporaryDirectory(prefix='m4-rust-timeline-') as directory:
    directory = pathlib.Path(directory)
    tl_unit = directory / 'timeline.rs'
    tl_unit.write_text(timeline_harness)
    tl_bin = directory / 'timeline'
    for command in [
        rustc() + ['--edition=2021', '--test', '-Dwarnings', str(tl_unit), '-o', str(tl_bin)],
        [str(tl_bin), '--nocapture'],
    ]:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs += result.stdout
        print(result.stdout, end='')
        (out / 'fence-signalling.log').write_text(logs)
        if result.returncode:
            raise SystemExit(result.returncode)

# Exercise the actual production g17/sync_input.c collector against lightweight C dma_fence
# and dma_fence_chain shims to prove deep 2-queue timeline chains coalesce without -ENOSPC,
# skip signalled leaves, and preserve error leaves.
sync_input_c = root / 'drivers/gpu/drm/asahi/g17/sync_input.c'
sync_input_dir = sync_input_c.parent
c_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
#define ASAHI_G17_SYNC_SCOPE_H 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GFP_KERNEL 0

struct dma_fence {
	uint64_t context;
	uint64_t seqno;
	int status;
	int refcount;
	bool is_chain;
	bool is_container;
	struct dma_fence *chain_next;
	struct dma_fence *chain_fence;
};

struct dma_fence_unwrap {
	struct dma_fence *chain;
	struct dma_fence *array;
	unsigned int index;
};

static inline struct dma_fence *dma_fence_get(struct dma_fence *f)
{
	if (f)
		++f->refcount;
	return f;
}

static inline void dma_fence_put(struct dma_fence *f)
{
	if (f) {
		assert(f->refcount > 0);
		--f->refcount;
	}
}

static inline int dma_fence_get_status(struct dma_fence *f)
{
	return f ? f->status : 1;
}

static inline bool dma_fence_is_later(struct dma_fence *f1, struct dma_fence *f2)
{
	return f1->seqno > f2->seqno;
}

static inline bool dma_fence_is_container(struct dma_fence *f)
{
	return f && (f->is_chain || f->is_container);
}

static inline struct dma_fence *dma_fence_unwrap_first(struct dma_fence *head,
						       struct dma_fence_unwrap *cursor)
{
	cursor->chain = NULL;
	cursor->array = NULL;
	cursor->index = 0;
	if (!head)
		return NULL;
	if (head->is_chain) {
		cursor->chain = dma_fence_get(head);
		return head->chain_fence;
	}
	return head;
}

static inline struct dma_fence *dma_fence_unwrap_next(struct dma_fence_unwrap *cursor)
{
	struct dma_fence *next;

	if (!cursor->chain)
		return NULL;
	next = cursor->chain->chain_next;
	dma_fence_put(cursor->chain);
	if (!next) {
		cursor->chain = NULL;
		return NULL;
	}
	cursor->chain = dma_fence_get(next);
	return next->chain_fence;
}

#define dma_fence_unwrap_for_each(fence, cursor, head) \
	for ((fence) = dma_fence_unwrap_first((head), (cursor)); (fence); \
	     (fence) = dma_fence_unwrap_next((cursor)))

static inline void *kmalloc_array(size_t n, size_t size, int flags)
{
	(void)flags;
	return calloc(n, size);
}

static inline void kfree(void *p)
{
	free(p);
}

static inline struct dma_fence *__dma_fence_unwrap_merge(unsigned int n,
							 struct dma_fence **f,
							 struct dma_fence_unwrap *c)
{
	(void)n;
	(void)f;
	(void)c;
	return NULL;
}

static inline bool asahi_g17_is_scope(struct dma_fence *f)
{
	(void)f;
	return false;
}

static inline unsigned int asahi_g17_scope_children(struct dma_fence *f,
						    struct dma_fence *out[2])
{
	(void)f;
	(void)out;
	return 0;
}

#include "sync_input.h"
#include "sync_input.c"

int main(void)
{
	enum { HOPS = 256 };
	struct dma_fence leaves[HOPS];
	struct dma_fence chains[HOPS];
	struct asahi_g17_fence_leaf out[4];
	int n, i;

	/* 256 alternating unsignalled hops across 2 contexts (10 and 20). */
	memset(leaves, 0, sizeof(leaves));
	memset(chains, 0, sizeof(chains));
	for (i = 0; i < HOPS; ++i) {
		leaves[i].context = (i % 2 == 0) ? 10 : 20;
		leaves[i].seqno = (uint64_t)(i / 2 + 1);
		leaves[i].status = 0;
		leaves[i].refcount = 1;

		chains[i].is_chain = true;
		chains[i].refcount = 1;
		chains[i].chain_fence = &leaves[i];
		chains[i].chain_next = (i > 0) ? &chains[i - 1] : NULL;
	}

	/* Coalesced collector fits a 256-hop 2-queue timeline chain into capacity=4 with 2 leaves. */
	n = asahi_g17_collect_fences_coalesced(&chains[HOPS - 1], out, 4);
	assert(n == 2);
	assert((out[0].fence == &leaves[HOPS - 1] && out[1].fence == &leaves[HOPS - 2]) ||
	       (out[1].fence == &leaves[HOPS - 1] && out[0].fence == &leaves[HOPS - 2]));
	for (i = 0; i < n; ++i) {
		assert(out[i].status == 0);
		asahi_g17_put_fence(out[i].fence);
	}

	/* Non-coalesced collector still sees distinct unsignalled leaves and hits -ENOSPC at cap=4. */
	n = asahi_g17_collect_fences(&chains[HOPS - 1], out, 4);
	assert(n == -ENOSPC);

	/* Already-signalled leaves (status > 0) are skipped by both collectors. */
	for (i = 0; i < HOPS; ++i)
		leaves[i].status = 1;
	n = asahi_g17_collect_fences_coalesced(&chains[HOPS - 1], out, 4);
	assert(n == 0);
	n = asahi_g17_collect_fences(&chains[HOPS - 1], out, 4);
	assert(n == 0);

	/* Error leaf (status < 0) is preserved even when a later same-context leaf is unsignalled. */
	leaves[5].status = -5;
	leaves[HOPS - 1].status = 0;
	leaves[HOPS - 2].status = 0;
	n = asahi_g17_collect_fences_coalesced(&chains[HOPS - 1], out, 4);
	assert(n == 3);
	{
		bool saw_err = false, saw_ctx10 = false, saw_ctx20 = false;
		for (i = 0; i < n; ++i) {
			if (out[i].fence == &leaves[5] && out[i].status == -5)
				saw_err = true;
			else if (out[i].fence == &leaves[HOPS - 2] && out[i].status == 0)
				saw_ctx10 = true;
			else if (out[i].fence == &leaves[HOPS - 1] && out[i].status == 0)
				saw_ctx20 = true;
			asahi_g17_put_fence(out[i].fence);
		}
		assert(saw_err && saw_ctx10 && saw_ctx20);
	}

	/* Every refcount returns cleanly to 1 (no leaks or double puts). */
	for (i = 0; i < HOPS; ++i) {
		assert(leaves[i].refcount == 1);
		assert(chains[i].refcount == 1);
	}
	printf("sync_input C coalescing tests passed (256-hop chain -> 2 leaves, error preserved)\n");
	return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='m4-c-sync-input-') as directory:
    directory = pathlib.Path(directory)
    (directory / 'linux').mkdir()
    for hdr in ('dma-fence.h', 'dma-fence-unwrap.h', 'slab.h', 'types.h'):
        (directory / 'linux' / hdr).write_text('/* host shim */\n')
    (directory / 'linux' / 'errno.h').write_text('#define ENOSPC 28\n#define EOPNOTSUPP 95\n')
    (directory / 'sync_scope.h').write_text('/* host shim */\n')
    c_unit = directory / 'test_sync_input.c'
    c_unit.write_text(c_harness)
    c_bin = directory / 'test_sync_input'
    cc = shlex.split(os.environ.get('CC', 'cc'))
    for command in [
        cc + ['-Wall', '-Wextra', '-Werror', '-I', str(directory), '-I', str(sync_input_dir),
              str(c_unit), '-o', str(c_bin)],
        [str(c_bin)],
    ]:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        logs += result.stdout
        print(result.stdout, end='')
        (out / 'fence-signalling.log').write_text(logs)
        if result.returncode:
            raise SystemExit(result.returncode)

(out / 'fence-signalling.json').write_text(json.dumps({
    'passed': True, 'tests': 11, 'negative_compile_checks': 1, 'cpu_only': True,
    'scope': 'Production RAII annotation compiled in both CONFIG_LOCKDEP variants, g16 Timeline::complete() off-pending FIFO batch signalling, FwWaits per-queue coalescing, and g17/sync_input.c chain coalescing.',
    'source_sha256': sha(production),
    'timeline_source_sha256': sha(fence_source_path),
    'sync_input_source_sha256': sha(sync_input_c),
}, indent=2) + '\n')
