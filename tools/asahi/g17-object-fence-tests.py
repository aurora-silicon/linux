#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the G17 object cache and work-state return marks against host stubs.

The pool (`g17/object.rs`, from the family list to the end of the file) and the
context accessors that hand out a work state's address (`g17/context.rs`) are
compiled with rustc. Firmware, mappings and the control ring are host stubs;
this is not GPU execution. Checks:

- A18 Pro (`after_release`): a work state its context never exposed comes back
  with fence 0; one whose owner's release was consumed with an empty work list
  comes back with that witness; one a record or command may name, or that a
  retained queue record named, keeps `control_fence()`.
- Firmware-cached families (`prefix != 0`, work states and descriptors on every
  G17) never overrun a waiting pool fence when full: they hand out a fresh
  unpooled/uncached object instead and count it.
- Uncached pooled objects (`prefix == 0`) without `after_release` (M4, M5)
  match the pool before linux#1978 (BASE) on the same random operations.
- Source & recovery rules: every accessor that hands the address to the
  firmware marks it; only the clean release path of `retain_linked_work` marks
  it released; a killed context or failed VM retains its work state until
  firmware stop even with an empty work list; a compute queue recycled after a
  kill or quarantine retains its descriptor ring until firmware stop.
- Cache policy (linux#3170 and follow-ups): records the host rewrites
  in place (render queue records, the compute graph, M5 scheduler pages) stay
  uncached on every G17; work nodes and compute descriptors keep their
  firmware-cached prefix on every G17 (G17P and M5).

`--mutation` applies one known-bad change to the compiled copy; the run must
then fail.
"""
import argparse
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/gpu/drm/asahi'
# The merge the object-cache fence change (linux#1978) is based on: pool decisions before it.
BASE = 'fc050503ffcd1032cd1590fb30252eb5a722a5a5'

MUTATIONS = {
    # Work states and cached objects may overrun a fence again when their family is full.
    'overrun-work-states': ('object', 'if ready || (!room && !no_overrun) => {', 'if ready || !room => {'),
    # Firmware-cached objects on non-after_release GPUs (M5) overrun their fence again.
    'overrun-cached-prefix': ('object', 'let no_overrun = marked || prefix != 0;', 'let no_overrun = marked;'),
    # An unseen work state keeps the strict fence.
    'unseen-strict': ('object', 'Self::UNSEEN => (RETURN_UNSEEN, 0),',
                      'Self::UNSEEN => (RETURN_UNSEEN, super::channel::control_fence()),'),
    # A retained record's mark can be replaced by a later release.
    'retain-not-sticky': ('object', 'self.0.store(Self::RETAINED, Ordering::Release);',
                          'self.0.store(Self::STRICT, Ordering::Release);'),
    # A command's node address does not mark the work state.
    'node-unmarked': ('context', '        self.context.expose_work();\n', ''),
    # A consumed release with an empty list does not mark the work state.
    'no-release': ('context', 'mark.released(super::channel::control_witnessed());', 'let _ = mark;'),
    # A killed context with an empty list returns its cached work state to the pool.
    'reuse-killed-work-state': ('context',
                                '            || self.kill_published()\n', ''),
    # A stampless-drained context with an empty list returns its cached work state to the pool.
    'reuse-stampless-drained-work-state': ('context',
                                           '            || self.stampless_drained()\n', ''),
    # A failed VM with an empty list returns its cached work state to the pool.
    'reuse-failed-work-state': ('context',
                                '            || self.status.get() != 0\n', ''),
    # Stampless render drops its WorkStateLease before completion stamps arrive.
    'drop-stampless-work-lease': (
        'policy-render',
        '            front.stamps_owed_work = Some(front.packet.completion.work_state()?);\n',
        '',
    ),
    # Pair-wide drain of a stampless render group fails to pin its work-state lease.
    'drop-stampless-drain-pin': (
        'policy-render',
        '                if let Some(work) = &active.stamps_owed_work {\n'
        '                    work.pin_until_stop();\n'
        '                } else {\n'
        '                    active.packet.context.note_stampless_drain();\n'
        '                }\n',
        '                active.packet.context.note_stampless_drain();\n',
    ),
    # WorkStates::pin forgets to record the pin bit, allowing same-context reuse after unstamped drain.
    'same-context-reuse-pinned-slot': (
        'identity',
        '        self.pinned.fetch_or(mask, Ordering::AcqRel);\n',
        '',
    ),
    # A compute queue recycled after kill/quarantine returns its cached descriptor ring to the pool.
    'reuse-recycled-descriptor-ring': (
        'policy-runtime',
        '            if queue.recycle_killed()? || queue.recycle_quarantined_idle()? {\n'
        '                queue.retain_ring_until_stop(&self.pool);\n'
        '            }',
        '            queue.recycle_killed()?;\n'
        '            queue.recycle_quarantined_idle()?;',
    ),
    # Work nodes lose their firmware-cached prefix (the linux#3170 light-submission cost).
    'uncached-nodes': ('policy-context', 'Family::WorkState,\n            2 * UAT_PGSZ,\n            UAT_PGSZ,',
                       'Family::WorkState,\n            2 * UAT_PGSZ,\n            0,'),
    # Compute descriptors lose their firmware-cached prefix.
    'uncached-descriptors': ('policy-compute', 'Family::Descriptor,\n            size,\n            size,',
                             'Family::Descriptor,\n            size,\n            0,'),
    # Render queue records, which the host retargets in place, are firmware-cached again.
    'cached-queue-records': ('policy-memory', 'GRAPH_SIZE,\n            0,\n', 'GRAPH_SIZE,\n            0x8000,\n'),
    # M5 scheduler pages, which are zeroed and reinitialised on reuse, are firmware-cached again.
    'cached-scheduler': ('policy-context', 'Family::Scheduler,\n                UAT_PGSZ,\n                0,',
                         'Family::Scheduler,\n                UAT_PGSZ,\n                UAT_PGSZ,'),
}


def block(source, start, after=0):
    """The item beginning at `start`, through its matching closing brace."""
    at = source.index(start, after)
    opening = source.index('{', at)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[at:end]


def replace(text, old, new, count=1):
    assert text.count(old) == count, f'{old!r}: {text.count(old)} matches, expected {count}'
    return text.replace(old, new)


def pool_source(text):
    """CpuMap and the pool part of object.rs, with pin-init and kernel allocation spelled for std."""
    cpu = block(text, '#[derive(Copy, Clone, Debug, PartialEq, Eq)]\npub(crate) enum CpuMap')
    tail = text[text.index('/// Families whose upper mappings'):]
    tail = replace(tail, '#[pin_data]\n', '')
    tail = replace(tail, '    #[pin]\n', '')
    tail = replace(tail, 'kernel::sync::Arc::pin_init(', 'kernel::sync::pin_init(')
    tail = replace(tail, 'pin_init!(Self {', 'Self {')
    tail = replace(tail, '            }),\n            GFP_KERNEL,', '            },\n            GFP_KERNEL,')
    tail = replace(tail, 'inner <- kernel::new_mutex!(', 'inner: kernel::new_mutex!(')
    if 'kernel::sync::Arc::new(' in tail:
        tail = replace(tail, 'kernel::sync::Arc::new(', 'kernel::sync::arc_new(')
    return cpu + '\n' + tail


def context_source(text):
    pages = block(text, 'pub(crate) struct FirmwarePages {')
    methods = '\n'.join(block(text, name) for name in [
        '    pub(crate) fn retain_firmware_pages(', '    pub(crate) fn retain_linked_work(',
        '    pub(crate) fn work_head_va(', '    pub(crate) fn work_head_address(', '    fn expose_work('])
    node = block(text, '    pub(crate) fn node_va(&self) -> Result<u64> {')
    return pages, methods, node


def identity_source(text):
    """Extract the real WorkStates allocator, pin, older_live, used, and may_take from context/identity.rs."""
    consts = '\n'.join(re.search(r'^pub\(super\) const ' + name + r': [^;]+;', text, re.M).group(0)
                        for name in ['FREE', 'STATE_COUNT', 'ENGINES'])
    may_take = block(text, 'pub(super) const fn may_take(')
    struct = block(text, 'pub(super) struct WorkStates {')
    impl1 = block(text, 'impl WorkStates {')
    impl2 = block(text, 'impl WorkStates {', text.index('impl WorkStates {') + 1)
    return '\n'.join([consts, may_take, struct, impl1, impl2])


PRELUDE = r'''
#![allow(dead_code, unused_imports, unused_variables, unused_mut, unused_unsafe, unused_macros)]
#![allow(private_interfaces, private_bounds)]
use std::cell::{Cell, RefCell};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub i32);
pub type Result<T = ()> = core::result::Result<T, Error>;
pub const ENODEV: Error = Error(-19);
pub const EINVAL: Error = Error(-22);
pub const ENOMEM: Error = Error(-12);
pub struct AllocError;
impl From<AllocError> for Error { fn from(_: AllocError) -> Error { ENOMEM } }
#[derive(Clone, Copy)] pub struct Flags;
pub const GFP_KERNEL: Flags = Flags;
macro_rules! pr_err { ($($t:tt)*) => { eprintln!($($t)*) } }
macro_rules! pr_warn { ($($t:tt)*) => { eprintln!($($t)*) } }
macro_rules! new_mutex_stub { ($value:expr, $name:expr $(,)?) => { $crate::kernel::sync::Mutex::new($value) } }

pub mod kernel {
    pub(crate) use new_mutex_stub as new_mutex;
    pub mod sync {
        pub type Arc<T> = std::sync::Arc<T>;
        pub fn arc_new<T>(value: T, _: crate::Flags) -> core::result::Result<Arc<T>, crate::AllocError> {
            Ok(std::sync::Arc::new(value))
        }
        pub fn pin_init<T>(value: T, _: crate::Flags) -> crate::Result<Arc<T>> { Ok(std::sync::Arc::new(value)) }
        pub struct Mutex<T>(std::sync::Mutex<T>);
        impl<T> Mutex<T> {
            pub fn new(value: T) -> Self { Self(std::sync::Mutex::new(value)) }
            pub fn lock(&self) -> std::sync::MutexGuard<'_, T> { self.0.lock().unwrap() }
        }
    }
    pub mod fmt { pub use core::fmt::{Formatter, Result}; }
}

pub struct PushError<T>(pub T);
pub struct KVec<T>(Vec<T>);
impl<T> Default for KVec<T> { fn default() -> Self { Self(Vec::new()) } }
impl<T> KVec<T> {
    pub fn new() -> Self { Self(Vec::new()) }
    pub fn with_capacity(n: usize, _: Flags) -> core::result::Result<Self, AllocError> { Ok(Self(Vec::with_capacity(n))) }
    pub fn push(&mut self, value: T, _: Flags) -> core::result::Result<(), AllocError> { self.0.push(value); Ok(()) }
    pub fn reserve(&mut self, n: usize, _: Flags) -> core::result::Result<(), AllocError> { self.0.reserve(n); Ok(()) }
    pub fn push_within_capacity(&mut self, value: T) -> core::result::Result<(), PushError<T>> {
        if self.0.len() < self.0.capacity() { self.0.push(value); Ok(()) } else { Err(PushError(value)) }
    }
}
impl<T> core::ops::Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> core::ops::DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }

pub mod mmu {
    use std::cell::Cell;
    pub type Prot = u8;
    pub const UAT_PGSZ: usize = 0x4000;
    thread_local! {
        pub static ZERO_PREFIX_CALLS: Cell<u64> = const { Cell::new(0) };
    }
    pub struct KernelMapping { pub iova: u64 }
    impl KernelMapping {
        pub fn iova(&self) -> u64 { self.iova }
        pub unsafe fn set_fw_cached_prefix(&self, prefix: usize) -> (crate::Result<[u64; 2]>, bool) {
            if prefix == 0 {
                ZERO_PREFIX_CALLS.with(|c| c.set(c.get() + 1));
            }
            (Ok([0; 2]), true)
        }
    }
}

thread_local! {
    /// Objects whose mapping was torn down, in order.
    pub static UNMAPPED: RefCell<Vec<u64>> = RefCell::new(Vec::new());
}

/// A firmware object: `id` is unique per allocation, the mapping is its address.
pub struct KernelObject { pub id: u64, pub mapping: Option<mmu::KernelMapping>, bytes: Vec<u8> }
impl KernelObject {
    pub fn size(&self) -> usize { self.bytes.len() }
    pub fn bytes_mut(&mut self) -> &mut [u8] { &mut self.bytes }
    pub fn gpu_va(&self) -> u64 { self.mapping.as_ref().map_or(0, |m| m.iova) }
}
impl Drop for KernelObject {
    fn drop(&mut self) { UNMAPPED.with(|u| u.borrow_mut().push(self.id)); }
}
pub fn unmapped(id: u64) -> bool { UNMAPPED.with(|u| u.borrow().contains(&id)) }
/// Starts a scenario: objects of earlier ones, which reuse the same ids, no longer count.
pub fn forget_unmapped() { UNMAPPED.with(|u| u.borrow_mut().clear()) }

pub struct Allocator<'a> { pub next: &'a Cell<u64> }
impl Allocator<'_> {
    pub fn kernel(&self, size: usize, _align: u64, _prot: mmu::Prot, _cpu: impl Sized) -> Result<KernelObject> {
        let id = self.next.get();
        self.next.set(id + 1);
        Ok(KernelObject { id, mapping: Some(mmu::KernelMapping { iova: id << 20 }), bytes: vec![0xa5; size] })
    }
}

pub mod identity {
    use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
    use std::sync::Arc;
@IDENTITY@
}
mod channel {
    use core::sync::atomic::{AtomicU64, Ordering};
@CHANNEL@
    /// Sets the published and consumed control-record counts.
    pub fn set(published: u64, consumed: u64) {
        assert!(consumed <= published);
        CONTROL_PUBLISHED.store(published, Ordering::SeqCst);
        CONTROL_CONSUMED.store(consumed, Ordering::SeqCst);
    }
    pub fn publish() { CONTROL_PUBLISHED.fetch_add(1, Ordering::SeqCst); }
    /// The firmware consumes up to `records` more of the published records.
    pub fn consume(records: u64) {
        let published = CONTROL_PUBLISHED.load(Ordering::SeqCst);
        let consumed = CONTROL_CONSUMED.load(Ordering::SeqCst);
        CONTROL_CONSUMED.store(published.min(consumed + records), Ordering::SeqCst);
    }
}

pub struct Rng(pub u64);
impl Rng {
    pub fn next(&mut self) -> u64 { self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17; self.0 }
    pub fn below(&mut self, n: u64) -> u64 { self.next() % n }
}

/// One step of the differential trace.
#[derive(Clone, Copy, Debug)]
pub enum Op { Alloc(usize, usize), Drop(usize, usize), Expose(usize), Retain(usize), Release(usize), Publish, Consume(u64), Snapshot, Stop }

pub fn ops(seed: u64, steps: usize) -> Vec<Op> {
    let mut rng = Rng(seed);
    let mut ops = Vec::new();
    for step in 0..steps {
        let family = rng.below(5) as usize;
        ops.push(match rng.below(100) {
            0..=33 => Op::Alloc(family, rng.below(4) as usize),
            34..=66 => Op::Drop(family, rng.next() as usize),
            67..=72 => Op::Expose(rng.next() as usize),
            73..=75 => Op::Retain(rng.next() as usize),
            76..=79 => Op::Release(rng.next() as usize),
            80..=86 => Op::Publish,
            87..=95 => Op::Consume(rng.below(3)),
            _ => Op::Snapshot,
        });
        if step == steps - 3 && seed % 3 == 0 {
            ops.push(Op::Stop);
        }
    }
    ops.push(Op::Snapshot);
    ops
}

/// Size and uncached prefix per family for BASE parity; `variant` 3 gives descriptors a second size.
pub fn geometry(family: usize, variant: usize) -> (usize, usize) {
    let page = mmu::UAT_PGSZ;
    match family {
        0 if variant == 3 => (2 * page, 0),
        1 => (2 * page, 0),
        _ => (page, 0),
    }
}

/// The counts the old and new snapshots share: per family used/returned/poisoned, and the totals.
pub fn shared_counts(snapshot: &str) -> String {
    let mut out = Vec::new();
    for line in snapshot.lines() {
        if line.starts_with("cached_objects_fenced") || line.starts_with("cached_object_bytes")
            || line.starts_with("firmware_linked_objects") {
            out.push(line.to_string());
        } else if line.starts_with("cached_objects_") {
            let words: Vec<_> = line.split(' ')
                .filter(|w| !w.starts_with("waiting=") && !w.starts_with("overruns=") && !w.starts_with("uncached="))
                .collect();
            out.push(words.join(" "));
        }
    }
    out.join("\n")
}

/// The value of `key=` on the snapshot line starting with `line`.
pub fn count(snapshot: &str, line: &str, key: &str) -> u64 {
    let row = snapshot.lines().find(|l| l.starts_with(line)).unwrap_or_else(|| panic!("no {line} line in\n{snapshot}"));
    let word = row.split(' ').find(|w| w.starts_with(key)).unwrap_or_else(|| panic!("no {key} in {row}"));
    word[key.len()..].parse().unwrap()
}

macro_rules! driver {
    ($new:expr, $marks:expr) => {
        pub struct Snap<'a>(pub &'a Pool);
        impl core::fmt::Display for Snap<'_> {
            fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result { self.0.write_capacity(f) }
        }
        pub fn snapshot(pool: &Pool) -> String { format!("{}", Snap(pool)) }

        const FAMILIES: [Family; 5] =
            [Family::Descriptor, Family::WorkState, Family::QueueState, Family::QueueConfig, Family::Scheduler];

        /// Runs `ops` and returns every allocation decision and snapshot.
        pub fn differential(ops: &[crate::Op], after_release: bool) -> Vec<String> {
            crate::channel::set(0, 0);
            let next = Cell::new(1);
            let alloc = Allocator { next: &next };
            let pool = $new(after_release);
            let mut live: Vec<Vec<PooledObject>> = (0..5).map(|_| Vec::new()).collect();
            let mut trace = Vec::new();
            for op in ops {
                match *op {
                    crate::Op::Alloc(f, variant) => {
                        let (size, prefix) = crate::geometry(f, variant);
                        match pool.allocate(&alloc, FAMILIES[f], size, prefix, 0, CpuMap::WriteCombined) {
                            Ok(object) => {
                                trace.push(format!("alloc {f} id={} cached={}", object.id, object.lease.is_some()));
                                live[f].push(object);
                            }
                            Err(error) => trace.push(format!("alloc {f} {error:?}")),
                        }
                    }
                    crate::Op::Drop(f, k) if !live[f].is_empty() => {
                        let index = k % live[f].len();
                        let object = live[f].remove(index);
                        trace.push(format!("drop {f} id={}", object.id));
                    }
                    crate::Op::Expose(k) | crate::Op::Retain(k) | crate::Op::Release(k) if !live[1].is_empty() => {
                        let n = live[1].len();
                        $marks(&live[1][k % n], op);
                    }
                    crate::Op::Publish => crate::channel::publish(),
                    crate::Op::Consume(n) => crate::channel::consume(n),
                    crate::Op::Snapshot => trace.push(crate::shared_counts(&snapshot(&pool))),
                    crate::Op::Stop => pool.stop_after_firmware(),
                    _ => {}
                }
            }
            drop(live);
            trace.push(crate::shared_counts(&snapshot(&pool)));
            trace
        }
    };
}

/// The pool before linux#1978.
pub mod base {
    use super::*;
@BASE@
    driver!(|_| Pool::new().unwrap(), |_: &PooledObject, _: &crate::Op| {});
}

pub mod now {
    use super::*;
    use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
@NOW@
    driver!(|after_release| Pool::new(after_release).unwrap(), |object: &PooledObject, op: &crate::Op| {
        // Without after_release there is no mark: the context's updates change nothing.
        assert!(object.return_mark().is_none(), "a mark without after_release");
    });

    const WORK: usize = 2 * mmu::UAT_PGSZ;

    fn work_state(pool: &kernel::sync::Arc<Pool>, alloc: &Allocator<'_>) -> PooledObject {
        pool.allocate(alloc, Family::WorkState, WORK, mmu::UAT_PGSZ, 0, CpuMap::WriteCombined).unwrap()
    }

    fn descriptor(pool: &kernel::sync::Arc<Pool>, alloc: &Allocator<'_>) -> PooledObject {
        pool.allocate(alloc, Family::Descriptor, mmu::UAT_PGSZ, mmu::UAT_PGSZ, 0, CpuMap::WriteCombined).unwrap()
    }

    /// The fence of the returned object `id` of `family`, if it is in the cache.
    fn fence(pool: &Pool, family: Family, id: u64) -> Option<u64> {
        let inner = pool.inner.lock();
        let begin = family as usize * POOL_SLOTS;
        inner.returned[begin..begin + POOL_SLOTS].iter().flatten().find(|e| e.object.id == id).map(|e| e.fence)
    }

    fn returns(pool: &Pool) -> [u64; 3] {
        let s = snapshot(pool);
        ["unseen=", "released=", "strict="].map(|key| crate::count(&s, "workstate_returns", key))
    }

    /// Uncached pooled objects (`prefix == 0`) skip `set_fw_cached_prefix` while keeping
    /// their pool lease, fence, return mark, reuse zeroing and retention until stop.
    pub fn uncached_pooled() {
        for after_release in [true, false] {
            let next = Cell::new(1);
            let alloc = Allocator { next: &next };
            let pool = Pool::new(after_release).unwrap();
            crate::forget_unmapped();
            crate::channel::set(5, 3);

            let mut ws = pool.allocate(&alloc, Family::WorkState, WORK, 0, 0, CpuMap::WriteCombined).unwrap();
            assert!(ws.lease.is_some(), "prefix == 0 keeps a pool lease");
            assert_eq!(ws.return_mark().is_some(), after_release);
            ws.bytes_mut()[0] = 0x42;
            let ws_id = ws.id;
            if let Some(mark) = ws.return_mark() {
                mark.expose();
            }
            drop(ws);
            assert!(!unmapped(ws_id), "prefix == 0 stays mapped on drop");
            assert_eq!(fence(&pool, Family::WorkState, ws_id), Some(6));

            // Still behind fence: a fresh object is allocated instead of reusing ws_id.
            let fresh = pool.allocate(&alloc, Family::WorkState, WORK, 0, 0, CpuMap::WriteCombined).unwrap();
            assert_ne!(fresh.id, ws_id);
            drop(fresh);

            // Once the fence is met, ws_id is reused and zeroed.
            crate::channel::set(6, 6);
            let reused = pool.allocate(&alloc, Family::WorkState, WORK, 0, 0, CpuMap::WriteCombined).unwrap();
            assert_eq!(reused.id, ws_id, "prefix == 0 is reused once its fence is met");
            assert_eq!(reused.bytes[0], 0, "reused prefix == 0 object is zeroed");
            drop(reused);

            // retain_until_stop keeps an uncached pooled object mapped until firmware stop.
            let mut kept = pool.allocate(&alloc, Family::QueueState, mmu::UAT_PGSZ, 0, 0, CpuMap::WriteCombined).unwrap();
            assert!(kept.lease.is_some());
            let kept_id = kept.id;
            kept.retain_until_stop(&pool);
            drop(kept);
            assert!(!unmapped(kept_id));
            pool.stop_after_firmware();
            assert!(unmapped(kept_id));
            assert!(unmapped(ws_id));
            assert_eq!(mmu::ZERO_PREFIX_CALLS.with(|c| c.get()), 0,
                       "set_fw_cached_prefix must not be called for prefix == 0");
        }
        println!("uncached pooled: prefix == 0 skips firmware caching while keeping fencing, reuse and retention");
    }

    /// Each return class decides the fence; reuse never waits on a met fence.
    pub fn return_classes() {
        let next = Cell::new(1);
        let alloc = Allocator { next: &next };
        let pool = Pool::new(true).unwrap();
        // Three records published, two of them consumed.
        crate::channel::set(3, 1);

        // Unseen: fence 0, reused at once with no further record.
        let object = work_state(&pool, &alloc);
        assert!(object.lease.is_some() && object.return_mark().is_some());
        let id = object.id;
        drop(object);
        assert_eq!(fence(&pool, Family::WorkState, id), Some(0), "an unseen work state needs no fence");
        let object = work_state(&pool, &alloc);
        assert_eq!(object.id, id, "an unseen work state is reused before any later record");
        assert!(object.return_mark().unwrap().0.load(Ordering::Relaxed) == ReturnMark::UNSEEN, "a reuse starts unseen");

        // Released: the witness at release, already met.
        let mark = object.return_mark().unwrap();
        mark.expose();
        mark.released(crate::channel::control_witnessed());
        drop(mark);
        drop(object);
        assert_eq!(fence(&pool, Family::WorkState, id), Some(1), "a released work state waits for its witness");
        let object = work_state(&pool, &alloc);
        assert_eq!(object.id, id, "a released work state is reused before any later record");

        // Strict: exposed without a release.
        object.return_mark().unwrap().expose();
        drop(object);
        assert_eq!(fence(&pool, Family::WorkState, id), Some(4), "an exposed work state keeps control_fence()");
        let fresh = work_state(&pool, &alloc);
        assert_ne!(fresh.id, id, "a waiting work state is not reused while there is room");
        assert!(fresh.lease.is_some());
        crate::channel::set(4, 4);
        let again = work_state(&pool, &alloc);
        assert_eq!(again.id, id, "reused once the firmware consumed a later record");

        // A release before a later exposure does not hold: strict again.
        let mark = again.return_mark().unwrap();
        mark.expose();
        mark.released(4);
        mark.expose();
        drop(mark);
        let id = again.id;
        drop(again);
        assert_eq!(fence(&pool, Family::WorkState, id), Some(5), "exposed after its release");

        // Retained by a queue record: strict whatever the owner does before or after.
        for order in 0..3 {
            let object = work_state(&pool, &alloc);
            let mark = object.return_mark().unwrap();
            match order {
                0 => { mark.expose(); mark.retain(); mark.released(4); }
                1 => { mark.expose(); mark.released(4); mark.retain(); }
                _ => { mark.retain(); mark.expose(); mark.released(4); }
            }
            drop(mark);
            let id = object.id;
            drop(object);
            assert_eq!(fence(&pool, Family::WorkState, id), Some(5), "a retained record's work state, order {order}");
        }
        // An unseen work state stays unseen through a release.
        let object = work_state(&pool, &alloc);
        object.return_mark().unwrap().released(4);
        let id = object.id;
        drop(object);
        assert_eq!(fence(&pool, Family::WorkState, id), Some(0));

        // Other families keep #1978's fence; only work states carry a mark.
        let object = descriptor(&pool, &alloc);
        assert!(object.return_mark().is_none());
        let id = object.id;
        drop(object);
        assert_eq!(fence(&pool, Family::Descriptor, id), Some(4), "descriptors wait for every record published");
        drop(fresh);
        let s = snapshot(&pool);
        assert_eq!(returns(&pool), [3, 1, 5], "return classes\n{s}");
        assert_eq!(crate::count(&s, "cached_objects_1 ", "overruns="), 0);
        println!("return classes: unseen 0, released at its witness, exposed or retained control_fence()");
    }

    /// A full cached work-state or descriptor family (`prefix > 0`) whose returned entries
    /// all wait leaves a new one unpooled/uncached on every G17 (`after_release` true or false),
    /// never overrunning a fence. Uncached pooled families (`prefix == 0`) still overrun without
    /// `after_release`, matching BASE.
    pub fn full_family() {
        for after_release in [true, false] {
            let next = Cell::new(1);
            let alloc = Allocator { next: &next };
            let pool = Pool::new(after_release).unwrap();
            crate::forget_unmapped();
            crate::channel::set(10, 7);
            let mut live: Vec<_> = (0..POOL_SLOTS).map(|_| work_state(&pool, &alloc)).collect();
            assert!(live.iter().all(|o| o.lease.is_some()));
            for object in &live {
                if let Some(mark) = object.return_mark() { mark.expose(); }
            }
            let waiting: Vec<_> = live.drain(..POOL_SLOTS / 2).collect();
            drop(waiting);
            let s = snapshot(&pool);
            assert_eq!(crate::count(&s, "cached_objects_1 ", "waiting="), 32, "{s}");
            assert_eq!(crate::count(&s, "cached_objects_1 ", "used="), 64, "{s}");
            let object = work_state(&pool, &alloc);
            let s = snapshot(&pool);
            assert!(object.lease.is_none(),
                    "a cached work state is never reused before its fence (after_release={after_release})");
            assert_eq!(object.id, 65);
            assert_eq!(crate::count(&s, "cached_objects_1 ", "overruns="), 0, "{s}");
            assert_eq!(crate::count(&s, "cached_objects_1 ", "uncached="), 1, "{s}");
            assert_eq!(crate::count(&s, "cached_objects_1 ", "returned="), 32, "{s}");
            assert!(object.return_mark().is_none(), "an uncached work state has no mark");
            let id = object.id;
            drop(object);
            assert!(unmapped(id), "an uncached object is unmapped when dropped");
            assert_eq!(crate::count(&snapshot(&pool), "cached_objects_1 ", "returned="), 32);
            if after_release {
                // An entry whose fence is met is still reused when the family is full.
                let ready = live.pop().unwrap();
                ready.return_mark().unwrap().released(7);
                let id = ready.id;
                drop(ready);
                assert_eq!(work_state(&pool, &alloc).id, id, "a met fence is reused in a full family");
            } else {
                assert_eq!(returns(&pool), [0, 0, 0], "work-state classes are counted on the A18 Pro only");
                // Cached descriptors never overrun their fence either.
                let mut desc_live: Vec<_> = (0..POOL_SLOTS).map(|_| descriptor(&pool, &alloc)).collect();
                let desc_waiting: Vec<_> = desc_live.drain(..POOL_SLOTS / 2).collect();
                drop(desc_waiting);
                let fresh_desc = descriptor(&pool, &alloc);
                let s = snapshot(&pool);
                assert!(fresh_desc.lease.is_none(), "a cached descriptor is never reused before its fence");
                assert_eq!(crate::count(&s, "cached_objects_0 ", "overruns="), 0, "{s}");
                assert_eq!(crate::count(&s, "cached_objects_0 ", "uncached="), 1, "{s}");
                // Uncached pooled objects (prefix == 0) still overrun when full without after_release.
                let mut qs_live: Vec<_> = (0..POOL_SLOTS).map(|_| {
                    pool.allocate(&alloc, Family::QueueState, mmu::UAT_PGSZ, 0, 0, CpuMap::WriteCombined).unwrap()
                }).collect();
                let qs_waiting: Vec<_> = qs_live.drain(..POOL_SLOTS / 2).collect();
                let oldest = qs_waiting[0].id;
                drop(qs_waiting);
                let overrun = pool.allocate(&alloc, Family::QueueState, mmu::UAT_PGSZ, 0, 0, CpuMap::WriteCombined).unwrap();
                assert!(overrun.lease.is_some());
                assert_eq!(overrun.id, oldest, "uncached pooled objects still overrun when full without after_release");
                let s = snapshot(&pool);
                assert_eq!(crate::count(&s, "cached_objects_2 ", "overruns="), 1, "{s}");
            }
        }
        // Cached descriptors on the A18 Pro never overrun in a full family either.
        let next = Cell::new(1);
        let alloc = Allocator { next: &next };
        let pool = Pool::new(true).unwrap();
        crate::channel::set(10, 7);
        let mut live: Vec<_> = (0..POOL_SLOTS).map(|_| descriptor(&pool, &alloc)).collect();
        let returned: Vec<_> = live.drain(..POOL_SLOTS / 2).collect();
        drop(returned);
        let fresh = descriptor(&pool, &alloc);
        assert!(fresh.lease.is_none(), "a full cached descriptor family stays uncached instead of overrunning");
        let s = snapshot(&pool);
        assert_eq!(crate::count(&s, "cached_objects_0 ", "overruns="), 0, "{s}");
        assert_eq!(crate::count(&s, "cached_objects_0 ", "uncached="), 1, "{s}");
        println!("full family: firmware-cached work states and descriptors never overrun on any G17; uncached pooled unchanged");
    }

    #[derive(Clone, Copy, PartialEq, Debug)]
    enum Model { Unseen, Strict, Retained, Released(u64) }

    /// Random A18 Pro operations against the specified marks.
    pub fn model(seed: u64, steps: usize) {
        crate::channel::set(0, 0);
        let next = Cell::new(1);
        let alloc = Allocator { next: &next };
        let pool = Pool::new(true).unwrap();
        crate::forget_unmapped();
        let mut rng = crate::Rng(seed);
        let mut work: Vec<(PooledObject, Model)> = Vec::new();
        let mut others: Vec<PooledObject> = Vec::new();
        let mut expected = std::collections::HashMap::new();
        let mut classes = [0u64; 3];
        let mut uncached = 0;
        for _ in 0..steps {
            let witnessed = crate::channel::control_witnessed();
            // Allocations outnumber drops below a full family and the reverse above it.
            let roll = rng.below(100);
            let roll = if work.len() < POOL_SLOTS { roll } else { (roll + 10) % 100 };
            match roll {
                0..=29 => {
                    let (reserved, ready) = {
                        let inner = pool.inner.lock();
                        let begin = Family::WorkState as usize * POOL_SLOTS;
                        (inner.reserved[1], inner.returned[begin..begin + POOL_SLOTS].iter().flatten()
                            .any(|e| e.fence <= witnessed))
                    };
                    let object = work_state(&pool, &alloc);
                    match expected.remove(&object.id) {
                        Some(fence) => assert!(fence <= witnessed, "work state reused at {witnessed} before its fence {fence}"),
                        None => {
                            assert!(!ready, "a ready work state was not reused");
                            assert_eq!(object.lease.is_some(), reserved < POOL_SLOTS, "fresh work state caching");
                            uncached += u64::from(object.lease.is_none());
                        }
                    }
                    work.push((object, Model::Unseen));
                }
                30..=54 if !work.is_empty() => {
                    let index = rng.below(work.len() as u64) as usize;
                    let (object, mark) = work.swap_remove(index);
                    let published = crate::channel::control_published();
                    let cached = object.lease.is_some();
                    let (class, fence) = match mark {
                        Model::Unseen => (0, 0),
                        Model::Released(w) => (1, w),
                        Model::Strict | Model::Retained => (2, published + 1),
                    };
                    let id = object.id;
                    drop(object);
                    if cached {
                        classes[class] += 1;
                        assert_eq!(self::fence(&pool, Family::WorkState, id), Some(fence), "{mark:?}");
                        expected.insert(id, fence);
                    } else {
                        assert!(unmapped(id));
                    }
                }
                55..=74 if !work.is_empty() => {
                    let index = rng.below(work.len() as u64) as usize;
                    let (object, mark) = &mut work[index];
                    let Some(handle) = object.return_mark() else { continue };
                    match rng.below(3) {
                        0 => { handle.expose(); if *mark != Model::Retained { *mark = Model::Strict; } }
                        1 => { handle.retain(); *mark = Model::Retained; }
                        _ => {
                            // The witness when the owner's release was found consumed.
                            handle.released(witnessed);
                            if *mark == Model::Strict { *mark = Model::Released(witnessed.max(1)); }
                        }
                    }
                }
                75..=82 => {
                    if others.len() < 80 && rng.below(2) == 0 {
                        others.push(descriptor(&pool, &alloc));
                    } else if !others.is_empty() {
                        let object = others.swap_remove(rng.below(others.len() as u64) as usize);
                        let (id, cached) = (object.id, object.lease.is_some());
                        let published = crate::channel::control_published();
                        drop(object);
                        if cached {
                            assert_eq!(fence(&pool, Family::Descriptor, id), Some(published));
                        }
                    }
                }
                83..=90 => crate::channel::publish(),
                _ => crate::channel::consume(rng.below(3)),
            }
        }
        let s = snapshot(&pool);
        assert_eq!(crate::count(&s, "cached_objects_1 ", "overruns="), 0, "{s}");
        assert_eq!(crate::count(&s, "cached_objects_1 ", "uncached="), uncached, "{s}");
        assert_eq!(returns(&pool), classes, "{s}");
        println!("model seed {seed}: classes unseen/released/strict {classes:?}, uncached {uncached}, overruns 0");
    }

    /// The context accessors, compiled from context.rs, against the pool above.
    pub mod context {
        use super::*;
        type Arc<T> = std::sync::Arc<T>;
        use crate::kernel::sync::Mutex;
        const UAT_PGSZ: usize = crate::mmu::UAT_PGSZ;
        const FREE: u32 = u32::MAX;
        const NODE_STRIDE: usize = 0x100;
        pub struct SchedulerStorage;
        pub mod mmu { pub struct ContextHold; }
        pub struct QosLease;
        pub struct VmStatus(core::sync::atomic::AtomicI32);
        impl VmStatus {
            fn new() -> Self { Self(core::sync::atomic::AtomicI32::new(0)) }
            fn get(&self) -> i32 { self.0.load(Ordering::Acquire) }
            fn fail(&self, err: i32) { self.0.store(err, Ordering::Release); }
        }
        pub struct WorkStorage { pub object: PooledObject }
@PAGES@
        pub struct Context {
            work: Arc<Mutex<WorkStorage>>,
            work_base: u64,
            work_mark: Option<Arc<ReturnMark>>,
            scheduler: Arc<SchedulerStorage>,
            hold: Arc<mmu::ContextHold>,
            qos: Arc<QosLease>,
            kill_published: core::sync::atomic::AtomicBool,
            stampless_drained: core::sync::atomic::AtomicBool,
            status: Arc<VmStatus>,
            /// The work-list head the firmware left.
            head: Cell<[u64; 2]>,
        }
        impl Context {
            fn work_head(&self) -> Result<[u64; 2]> { Ok(self.head.get()) }
            fn kill_published(&self) -> bool { self.kill_published.load(Ordering::Acquire) }
            fn stampless_drained(&self) -> bool { self.stampless_drained.load(Ordering::Acquire) }
@METHODS@
        }
        pub struct WorkStateLease { context: Arc<Context>, word: core::sync::atomic::AtomicU32 }
        impl WorkStateLease {
            fn word(&self) -> u32 { self.word.load(Ordering::Acquire) }
@NODE@
        }

        fn new_context(pool: &kernel::sync::Arc<Pool>, alloc: &Allocator<'_>) -> (Arc<Context>, u64) {
            let object = super::work_state(pool, alloc);
            let (id, work_base, work_mark) = (object.id, object.gpu_va(), object.return_mark());
            let context = Arc::new(Context {
                work: Arc::new(Mutex::new(WorkStorage { object })), work_base, work_mark,
                scheduler: Arc::new(SchedulerStorage), hold: Arc::new(mmu::ContextHold), qos: Arc::new(QosLease),
                kill_published: core::sync::atomic::AtomicBool::new(false),
                stampless_drained: core::sync::atomic::AtomicBool::new(false),
                status: Arc::new(VmStatus::new()),
                head: Cell::new([0, work_base + (UAT_PGSZ + 0x400) as u64]),
            });
            (context, id)
        }

        pub fn accessors() {
            for after_release in [true, false] {
                let next = Cell::new(1);
                let alloc = Allocator { next: &next };
                let pool = Pool::new(after_release).unwrap();
                crate::forget_unmapped();
                crate::channel::set(6, 4);
                let strict = 7;
                let (unseen, released) = if after_release { (0, 4) } else { (7, 7) };
                let check = |id: u64, want: u64, what: &str| {
                    assert_eq!(super::fence(&pool, Family::WorkState, id), Some(want), "{what}, after_release {after_release}")
                };

                // Never published: the head is compared, never handed out.
                let (context, id) = new_context(&pool, &alloc);
                assert_eq!(context.work_head_address(), context.work_base + 0x4400);
                assert!(!context.retain_linked_work(&pool));
                drop(context);
                check(id, unseen, "a context that never exposed its work state");

                // A queue record names the head; the release is consumed with the list empty.
                let (context, id) = new_context(&pool, &alloc);
                assert_eq!(context.work_head_va(), context.work_head_address());
                assert!(!context.retain_linked_work(&pool));
                drop(context);
                check(id, released, "a released context");

                // A command names a node; the context goes without a consumed release.
                let (context, id) = new_context(&pool, &alloc);
                let lease = WorkStateLease { context: context.clone(), word: FREE.into() };
                assert!(lease.node_va().is_err());
                lease.word.store(5, Ordering::Release);
                assert_eq!(lease.node_va().unwrap(), context.work_base + 5 * NODE_STRIDE as u64);
                drop((lease, context));
                check(id, strict, "a command's work state");

                // A retained queue record names it, before or after the release.
                for before in [true, false] {
                    let (context, id) = new_context(&pool, &alloc);
                    let _ = context.work_head_va();
                    let pages = if before { Some(context.retain_firmware_pages()) } else { None };
                    assert!(!context.retain_linked_work(&pool));
                    let pages = pages.unwrap_or_else(|| context.retain_firmware_pages());
                    drop(context);
                    assert_eq!(super::fence(&pool, Family::WorkState, id), None, "the record's pages hold it");
                    drop(pages);
                    check(id, strict, "a retained record's work state");
                }

                // A linked list keeps the storage until stop.
                let (context, id) = new_context(&pool, &alloc);
                let _ = context.work_head_va();
                context.head.set([context.work_base, context.work_base + 0x98]);
                assert!(context.retain_linked_work(&pool));
                drop(context);
                assert_eq!(super::fence(&pool, Family::WorkState, id), None, "linked work is kept");
                assert!(!unmapped(id));

                // A killed context with an empty list keeps its cached work state until stop.
                let (context, killed_id) = new_context(&pool, &alloc);
                let _ = context.work_head_va();
                context.kill_published.store(true, Ordering::Release);
                assert!(!context.retain_linked_work(&pool));
                drop(context);
                assert_eq!(super::fence(&pool, Family::WorkState, killed_id), None,
                           "a killed context's work state is kept out of reuse until stop");
                assert!(!unmapped(killed_id));

                // A stampless-drained render context with an empty list keeps its cached work state until stop.
                let (context, stampless_id) = new_context(&pool, &alloc);
                let _ = context.work_head_va();
                context.stampless_drained.store(true, Ordering::Release);
                assert!(!context.retain_linked_work(&pool));
                drop(context);
                assert_eq!(super::fence(&pool, Family::WorkState, stampless_id), None,
                           "a stampless-drained context's work state is kept out of reuse until stop");
                assert!(!unmapped(stampless_id));

                // A failed VM (recovery/fault) with an empty list keeps its cached work state until stop.
                let (context, failed_id) = new_context(&pool, &alloc);
                let _ = context.work_head_va();
                context.status.fail(-5);
                assert!(!context.retain_linked_work(&pool));
                drop(context);
                assert_eq!(super::fence(&pool, Family::WorkState, failed_id), None,
                           "a failed VM's work state is kept out of reuse until stop");
                assert!(!unmapped(failed_id));

                // Even after advancing the control fence, a fresh allocation never reuses kept work states.
                crate::channel::set(100, 100);
                let next_ws = super::work_state(&pool, &alloc);
                assert_ne!(next_ws.id, id);
                assert_ne!(next_ws.id, killed_id);
                assert_ne!(next_ws.id, stampless_id);
                assert_ne!(next_ws.id, failed_id);
                drop(next_ws);

                pool.stop_after_firmware();
                assert!(unmapped(id) && unmapped(killed_id) && unmapped(failed_id),
                        "kept work states are unmapped once firmware stops");
            }
            println!("context: records, commands and retained records mark the work state; killed/failed/linked contexts retain until stop");
        }
    }
}

fn work_states_pin_prevents_same_context_reuse() {
    use crate::identity::{WorkStates, STATE_COUNT};
    let states = WorkStates::new();
    let drained = states.allocate(0).expect("initial render slot");
    let drained_slot = drained & 63;
    let newer = states.allocate(0).expect("newer slot");
    assert_eq!(states.older_live(newer), Some(drained));
    assert!(states.pin(drained, 0), "pinning a live slot succeeds once");
    assert!(!states.pin(drained, 0), "double pin is a no-op");
    assert_eq!(states.older_live(newer), None, "pinned slot must not stall older_live");
    assert_eq!(states.used(), 2, "pinned slot stays counted in used()");
    assert!(states.release(newer, 0));
    assert_eq!(states.used(), 1);
    for _ in 0..(STATE_COUNT * 4) {
        let w = states.allocate(0).expect("same-context submission");
        assert_ne!(w & 63, drained_slot, "same-context submission re-acquired pinned slot");
        assert!(states.release(w, 0));
    }
    for _ in 1..(STATE_COUNT - 2) {
        let w = states.allocate(0).expect("slot to pin");
        assert!(states.pin(w, 0));
    }
    let r = states.allocate(0).expect("render takes 1 of 2 remaining");
    let c = states.allocate(1).expect("compute takes last remaining");
    assert!(states.release(r, 0));
    assert!(states.release(c, 1));
    for _ in 0..2 {
        let w = states.allocate(0).expect("final slot to pin");
        assert!(states.pin(w, 0));
    }
    assert!(states.exhausted(), "all 63 slots pinned must report exhausted()");
    assert!(states.allocate(0).is_none());
    assert!(states.allocate(1).is_none());
    let claims = std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0));
    let ctx_a = WorkStates::shared(claims.clone());
    let ctx_b = WorkStates::shared(claims.clone());
    let wa = ctx_a.allocate(0).expect("shared ctx_a slot");
    let sa = wa & 63;
    assert!(ctx_a.pin(wa, 0));
    assert_eq!(claims.load(std::sync::atomic::Ordering::Acquire) & (1u64 << sa), 0,
               "pinned slot releases shared work-key claim for sibling context");
    assert_eq!(ctx_a.used(), 1, "pinning context still accounts for its pinned slot");
    for _ in 0..(STATE_COUNT * 2) {
        let w = ctx_a.allocate(0).expect("shared ctx_a retry");
        assert_ne!(w & 63, sa, "shared context re-acquired its own pinned slot");
        assert!(ctx_a.release(w, 0));
    }
    let wb = ctx_b.allocate(0).expect("sibling ctx_b slot");
    assert!(ctx_b.release(wb, 0));
    println!("same-context WorkStates pin: pinned slot never re-acquired, older_live unblocked, exhaustion clean");
}

fn main() {
    work_states_pin_prevents_same_context_reuse();
    now::uncached_pooled();
    now::return_classes();
    now::full_family();
    let (mut allocations, mut uncached, mut overruns) = (0, 0, 0);
    for seed in 1..=24u64 {
        let ops = ops(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15), 6000);
        let trace = now::differential(&ops, false);
        assert_eq!(base::differential(&ops, false), trace,
                   "decisions without after_release differ from the pool before #1978 (seed {seed})");
        allocations += trace.iter().filter(|line| line.starts_with("alloc")).count();
        uncached += trace.iter().filter(|line| line.ends_with("cached=false")).count();
        overruns += count(trace.last().unwrap(), "cached_objects_fenced", "overruns=");
    }
    assert!(uncached > 0 && overruns > 0, "the traces must fill families");
    println!("differential: without after_release, 24 random traces match the pool before linux#1978 \
              ({allocations} allocations, {uncached} uncached, {overruns} overruns)");
    for seed in 1..=24u64 {
        now::model(seed.wrapping_mul(0x2545_f491_4f6c_dd1d), 6000);
    }
    now::context::accessors();
}
'''


def source_rules(context, observation, tree):
    """Every exposure of the work storage address goes through a marking accessor."""
    # Only these use work_base: the field, Context::new, the head and the node accessor.
    assert context.count('work_base') == 6, context.count('work_base')
    head = block(context, '    pub(crate) fn work_head_address(')
    node = block(context, '    pub(crate) fn node_va(&self) -> Result<u64> {')
    assert 'self.work_base + (UAT_PGSZ + 0x400) as u64' in head
    assert 'self.context.work_base + u64::from(word & 63) * NODE_STRIDE as u64' in node
    assert node.index('return Err(EINVAL);') < node.index('self.context.expose_work();') < node.index('Ok(')
    assert 'self.expose_work();' in block(context, '    pub(crate) fn work_head_va(')
    assert 'mark.retain();' in block(context, '    pub(crate) fn retain_firmware_pages(')
    # The only gpu_va reads of the work object: its base, and a node's own counter link.
    assert context.count('object.gpu_va()') == 2
    assert 'let work_base = object.gpu_va();' in context
    assert 'fw::WorkNode::new(self.object.gpu_va() + counter as u64, word)' in context
    # A release is marked only where retain_linked_work found a clean, unkilled, empty-list context.
    linked = block(context, '    pub(crate) fn retain_linked_work(')
    assert re.search(r'if linked\s+\|\| self\.kill_published\(\)\s+\|\| self\.stampless_drained\(\)\s+\|\| self\.status\.get\(\) != 0\s+\{\n'
                     r'\s+self\.work\.lock\(\)\.object\.retain_until_stop\(pool\);\n'
                     r'\s+\} else if let Some\(mark\) = &self\.work_mark \{\n'
                     r'\s+mark\.released\(super::channel::control_witnessed\(\)\);', linked), linked
    assert context.count('.released(') == 1 and context.count('.retain()') == 1
    # The non-marking address only feeds comparisons with the host's own reads.
    users = []
    for path in sorted(tree.rglob('*.rs')):
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if 'work_head_address()' in line and 'fn work_head_address' not in line:
                users.append((path.relative_to(tree).as_posix(), line.strip()))
    assert all(name in ('g17/context.rs', 'g17/queue/render/observation.rs') for name, _ in users), users
    for name, line in users:
        assert ('== [0, ' in line or '!= [0, ' in line or line.startswith('must_retain(')
                or line == 'self.work_head_address()'), (name, line)
    assert len(users) == 4, users
    assert 'job_list_empty: head == [0, memory.context.work_head_address()],' in observation
    print('source: records and commands mark the work state; only a consumed empty-list release releases it')


def cache_policy(context, compute, memory, runtime, render):
    """The firmware-cached prefix and recovery retention policy each pooled G17 object family gets."""
    failures = []

    def need(text, pattern, what):
        if not re.search(pattern, text):
            failures.append(what)

    # Work nodes: the node page is cached on every G17; the counter page stays uncached.
    need(context, r'pool\.allocate\(\s*alloc,\s*Family::WorkState,\s*2 \* UAT_PGSZ,\s*UAT_PGSZ,',
         'work nodes: node page cached')
    # M5 scheduler pages, which are zeroed and reinitialised on reuse, stay uncached.
    need(context, r'Family::Scheduler,\s*UAT_PGSZ,\s*0,', 'M5 scheduler pages uncached')
    # Compute descriptors: cached on every G17.
    need(compute, r'Family::Descriptor,\s*size,\s*size,', 'compute descriptors cached')
    # A compute queue recycled after kill or quarantine retains its descriptor ring until stop.
    need(compute, r'pub\(crate\) fn retain_ring_until_stop\(&mut self, pool: &Arc<Pool>\) \{\s*'
         r'if let Some\(ring\) = self\.graph\.ring\.as_mut\(\) \{\s*'
         r'ring\.object\.retain_until_stop\(pool\);', 'compute retain_ring_until_stop helper')
    need(runtime, r'if queue\.recycle_killed\(\)\? \|\| queue\.recycle_quarantined_idle\(\)\? \{\s*'
         r'queue\.retain_ring_until_stop\(&self\.pool\);', 'recycled compute queues retain descriptor ring until stop')
    # The compute graph, whose queue record is rebound in place, is an ordinary uncached allocation.
    need(compute, r'let mut graph = alloc\.kernel\(GRAPH_SIZE \+ mmu::UAT_PGSZ, page, prot, cpu\)\?;',
         'compute graph uncached')
    if 'Family::QueueState' in compute:
        failures.append('compute graph not pooled')
    # Render queue records, retargeted and bound to work slots in place, stay uncached.
    need(memory, r'Family::QueueState,\s*GRAPH_SIZE,\s*0,\s*prot,', 'render queue records uncached')
    # Stampless render retains its WorkStateLease until post-maintenance stamps arrive,
    # and every unstamped drain path pins the slot via pin_until_stop() (same-context + cross-context).
    need(render, r'if stampless \{[^}]*front\.stamps_owed_work = Some\(front\.packet\.completion\.work_state\(\)\?\);[^}]*\}\s*defer\(Deferred::Retired\(',
         'stampless render retains WorkStateLease across stamps_owed')
    need(render, r'if active\.stamps_owed \{\s*if let Some\(work\) = &active\.stamps_owed_work \{\s*work\.pin_until_stop\(\);',
         'stampless render drain pins WorkStateLease until stop')
    need(render, r'fn fail_dropped\([\s\S]*?work\.pin_until_stop\(\);',
         'fail_dropped pins WorkStateLease until stop')
    need(render, r'fn settle_interrupted_front\([\s\S]*?work\.pin_until_stop\(\);',
         'settle_interrupted_front pins WorkStateLease until stop')
    need(render, r'fn fail_after_observation\([\s\S]*?work\.pin_until_stop\(\);',
         'fail_after_observation pins WorkStateLease until stop')
    need(context, r'pub\(crate\) fn pin_until_stop\(&self\) \{\s*self\.pinned\.store\(true, Ordering::Release\);\s*self\.context\.note_stampless_drain\(\);\s*\}',
         'WorkStateLease::pin_until_stop sets pinned and stampless_drained')
    need(context, r'if self\.pinned\.load\(Ordering::Acquire\) \{\s*self\.context\.pin_state\(word, engine\);',
         'WorkStateLease::drop pins slot on unstamped drain')
    need(context, r'if self\.context\.states\.exhausted\(\) \{\s*return Err\(EIO\);\s*\}',
         'WorkStateLease::acquire fails cleanly when all slots are pinned')
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()

    obj = (DRIVER / 'g17/object.rs').read_text()
    ctx = (DRIVER / 'g17/context.rs').read_text()
    chan = (DRIVER / 'g17/channel.rs').read_text()
    observation = (DRIVER / 'g17/queue/render/observation.rs').read_text()
    base = subprocess.run(['git', '-C', str(ROOT), 'show', f'{BASE}:drivers/gpu/drm/asahi/g17/object.rs'],
                          check=True, capture_output=True, text=True).stdout
    source_rules(ctx, observation, DRIVER)

    policy = {'context': ctx, 'compute': (DRIVER / 'g17/queue/compute.rs').read_text(),
              'memory': (DRIVER / 'g17/queue/render/memory.rs').read_text(),
              'runtime': (DRIVER / 'g17/runtime.rs').read_text(),
              'render': (DRIVER / 'g17/queue/render.rs').read_text()}
    if args.mutation and MUTATIONS[args.mutation][0].startswith('policy-'):
        where, old, new = MUTATIONS[args.mutation]
        policy[where[len('policy-'):]] = replace(policy[where[len('policy-'):]], old, new)
    failures = cache_policy(policy['context'], policy['compute'], policy['memory'],
                            policy['runtime'], policy['render'])
    for failure in failures:
        print(f'FAIL line 0: cache policy: {failure}')
    if failures:
        return 1
    print('cache policy: in-place rewritten records uncached everywhere; work nodes and compute descriptors '
          'cached on every G17')
    if args.mutation and MUTATIONS[args.mutation][0].startswith('policy-'):
        return 0

    now = pool_source(obj)
    pages, methods, node = context_source(ctx)
    ident = identity_source((DRIVER / 'g17/context/identity.rs').read_text())
    if args.mutation:
        where, old, new = MUTATIONS[args.mutation]
        if where == 'object':
            now = replace(now, old, new)
        elif where == 'identity':
            ident = replace(ident, old, new)
        else:
            methods, node = (replace(methods, old, new), node) if old in methods else (methods, replace(node, old, new))
    channel = '\n'.join(re.search(r'(?ms)^(?:///[^\n]*\n)*static ' + name + r': AtomicU64 = AtomicU64::new\(0\);', chan).group(0)
                        for name in ['CONTROL_PUBLISHED', 'CONTROL_CONSUMED'])
    channel += '\n' + '\n'.join(block(chan, 'pub(crate) fn ' + name + '()')
                                for name in ['control_fence', 'control_published', 'control_witnessed'])
    code = (PRELUDE.replace('@CHANNEL@', channel).replace('@IDENTITY@', ident).replace('@BASE@', pool_source(base)).replace('@NOW@', now)
            .replace('@PAGES@', pages).replace('@METHODS@', methods).replace('@NODE@', node))

    with tempfile.TemporaryDirectory() as work:
        fixture = Path(work) / 'object-fence.rs'
        fixture.write_text(code)
        binary = Path(work) / 'object-fence'
        built = subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(fixture), '-o', str(binary)],
                               capture_output=True, text=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            print('object-fence harness did not compile')
            return 1
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout + ran.stderr, end='')
        return ran.returncode


if __name__ == '__main__':
    sys.exit(main())
