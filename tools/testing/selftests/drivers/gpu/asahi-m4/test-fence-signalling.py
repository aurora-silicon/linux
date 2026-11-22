#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production DMA-fence lockdep guard and Timeline off-pending signalling."""
from common import kernel_root, output_dir, rustc, sha
import json
import pathlib
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
'''.replace('SIGNAL_BATCH_LINE', signal_batch_line).replace(
    'TIMELINE_COMPLETE_FN', timeline_complete_fn
).replace('INPUT_FENCES_SRC', input_fences_src)

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

(out / 'fence-signalling.json').write_text(json.dumps({
    'passed': True, 'tests': 7, 'negative_compile_checks': 1, 'cpu_only': True,
    'scope': 'Production RAII annotation compiled in both CONFIG_LOCKDEP variants and g16 Timeline::complete() off-pending FIFO batch signalling.',
    'source_sha256': sha(production),
    'timeline_source_sha256': sha(fence_source_path),
}, indent=2) + '\n')
