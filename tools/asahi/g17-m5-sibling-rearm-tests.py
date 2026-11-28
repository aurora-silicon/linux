#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: M5 sibling compute channels come from vacant and other-VM idle queues too.

Once earlier clients have taken compute QIDs up to the fresh-QID ceiling
(`compute_fits()` false), every idle compute queue is either vacant (its pool
given back after the idle expiry) or still holds the pool of a VM that is gone.
A sibling that could only bind a populated pool of its own VM, or a fresh QID,
then never came up again until the GPU slept. This test compiles the production
code that decides and performs the sibling's acquisition against host stubs:

- the queue predicates of drivers/gpu/drm/asahi/g17/queue/compute.rs that say
  which queue may change owner (`reusable`, `can_rearm`, `rearm_open`,
  `shareable`, `recycled_pending`, `vacancy_pending`, `vacate_candidate`) and
  the queue's pool give-back and re-arm transitions (`require_fresh_backing`,
  `vacate`, `observe_vacancy`, `rearm`, `bind_sibling`);
- the registry's `sibling_source()`, `has_compute_sibling_headroom()`,
  `wants_compute_sibling()`, `bind_reusable_sibling()`, `give_back_foreign()`,
  `reserve_rearm()`, `rearm_compute()`, `bind_rearmed()` and the pool pass
  `vacate_idle_pools()` from drivers/gpu/drm/asahi/g17/runtime.rs.

It checks that with no fresh QID a new client still gets every sibling it
wants, first from vacant queues (re-armed with fresh backing, bound as sibling
channels) and then from other VMs' idle queues (given back one at a time,
vacated by the pool pass, re-armed), across clients that come and go; that a
queue with an owner or work, an open client's idle channel, a queue another
client is re-arming, one whose scheduler release is in flight and one whose
pool release is in flight are never taken; that a fresh QID comes before
another VM's pool is given back; and, structurally, that
`try_add_compute_sibling()` takes these sources through the primary's re-arm
path, lets an owner waiting for its first channel go first and that the primary
path gives back other VMs' queues through the same helper.

Each mutation restores the old rule or drops one part of the new one and must
fail an assertion of the compiled harness.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G17 = ROOT / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def ordered_ok(source, *parts):
    at = -1
    for part in parts:
        idx = source.find(part, at + 1)
        if idx < 0:
            return False
        at = idx
    return True


# The headroom rule before this change: a populated same-VM pool or a fresh QID only.
OLD_SOURCE = '''fn sibling_source(
        &self,
        owner: u64,
        context: &Arc<Context>,
        required: super::freelist::ComputeCapacity,
    ) -> Option<SiblingSource> {
        let teardown = &self.teardown;
        let vm = context.vm();
        let warm = self
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .filter(|queue| {
                queue.reusable()
                    && !queue.needs_fresh_backing()
                    && !queue.pool_vacant()
                    && queue.pool_serves(vm)
                    && queue.pool_fits(required)
                    && queue.has_ring()
                    && !queue.context().is_some_and(|bound| teardown.releasing(bound))
            })
            .map(|queue| (queue.released_at(), queue.qid()))
            .min_by_key(|(key, _)| *key)
            .map(|(_, qid)| SiblingSource::Warm(qid));
        warm.or_else(|| {
            self.qids
                .compute_fits(self.admission.queues() as usize)
                .then_some(SiblingSource::Fresh)
        })
    }'''

# name -> [(file, old, new)]: each must apply exactly once.
MUTATIONS = {
    # The old rule: no vacant and no other-VM queue is ever a sibling source.
    'old-headroom': [('source', None, OLD_SOURCE)],
    'no-vacant-sibling': [('runtime', '        if let Some((_, qid)) = vacant {\n', '        if let Some((_, qid)) = vacant.filter(|_| false) {\n')],
    'no-foreign-sibling': [('runtime', '        foreign.map(|(_, qid)| SiblingSource::Foreign(qid))\n', '        None\n')],
    # Never steal: an open client's idle channel, a busy queue, a queue another client re-arms,
    # a queue whose scheduler release is in flight.
    'steal-shareable': [('runtime', '            .filter(|queue| queue.reusable())\n'
                         '            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))\n'
                         '        {\n            let best = if queue.pool_vacant() {',
                         '            .filter(|queue| queue.reusable() || queue.shareable())\n'
                         '            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))\n'
                         '        {\n            let best = if queue.pool_vacant() {')],
    'steal-unowned': [('runtime', '            .filter(|queue| queue.reusable())\n'
                       '            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))\n'
                       '        {\n            let best = if queue.pool_vacant() {',
                       '            .filter(|queue| queue.owner() != Some(owner))\n'
                       '            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))\n'
                       '        {\n            let best = if queue.pool_vacant() {')],
    'ignore-rearm-reservation': [('runtime', '                if !queue.rearm_open(owner) {\n                    continue;\n'
                                  '                }\n                &mut vacant\n', '                &mut vacant\n')],
    'ignore-releasing': [('runtime', '            .filter(|queue| queue.reusable())\n'
                          '            .filter(|queue| !queue.context().is_some_and(|bound| teardown.releasing(bound)))\n'
                          '        {\n            let best = if queue.pool_vacant() {',
                          '            .filter(|queue| queue.reusable())\n'
                          '        {\n            let best = if queue.pool_vacant() {')],
    # Give back several other-VM pools at once.
    'parallel-give-back': [('runtime', '            .any(|queue| queue.vacancy_pending())\n        {\n            return None;\n        }\n'
                            '        foreign.map(', '            .any(|queue| queue.vacancy_pending() && false)\n        {\n'
                            '            return None;\n        }\n        foreign.map(')],
    # Give another VM's pool back while a fresh QID fits.
    'foreign-before-fresh': [('runtime', '        if self.qids.compute_fits(self.admission.render_demand(0)) {\n'
                              '            return Some(SiblingSource::Fresh);\n        }\n        // A pool release',
                              '        if foreign.is_none() && self.qids.compute_fits(self.admission.render_demand(0)) {\n'
                              '            return Some(SiblingSource::Fresh);\n        }\n        // A pool release')],
    # A re-armed sibling is bound as the owner's primary.
    'rearm-binds-primary': [('runtime', '        if channel_index == 0 {\n            queue.bind_owner(owner, context.clone())',
                             '        if true {\n            queue.bind_owner(owner, context.clone())')],
    # Structural: try_add_compute_sibling() without the re-arm, the waiter rule, the shared give-back.
    'try-add-ignores-waiters': [('runtime', '                || self.shared.channels.has_waiters()\n', '')],
    'try-add-skips-rearm': [('runtime', '                    firmware.queues.reserve_rearm(qid, self.owner)?;\n                    Err(qid)\n',
                             '                    return Ok(false);\n')],
    'rearm-any-index': [('runtime', '            && (channel_index == 0\n                || firmware.queues.next_sibling_index(self.owner, self.compute_channels)\n'
                         '                    == Some(channel_index));\n', ';\n')],
    'primary-own-give-back': [('runtime', '        if let Some(qid) = foreign {\n            self.give_back_foreign(qid)?;\n',
                               '        if let Some(qid) = foreign {\n            self.compute_queue_mut(qid).ok_or(EIO)?.require_fresh_backing();\n')],
}


def build(mutation=None):
    files = {
        'runtime': (G17 / 'runtime.rs').read_text(),
        'queue': (G17 / 'queue/compute.rs').read_text(),
        'pairs': (G17 / 'pairs.rs').read_text(),
        'freelist': (G17 / 'freelist.rs').read_text(),
    }
    old_source = False
    for name, old, new in MUTATIONS.get(mutation, []):
        if name == 'source':
            old_source = True
            continue
        assert files[name].count(old) == 1, (mutation, old)
        files[name] = files[name].replace(old, new)
    runtime, queue, pairs, freelist = files['runtime'], files['queue'], files['pairs'], files['freelist']
    source_fn = item(runtime, 'fn sibling_source(')
    if old_source:
        runtime = runtime.replace(source_fn, OLD_SOURCE)
        source_fn = OLD_SOURCE

    queue_fns = '\n'.join(item(queue, signature) for signature in [
        'pub(crate) fn reusable(&self)', 'pub(crate) fn can_rearm(&self)', 'pub(crate) fn rearm_open(',
        'pub(crate) fn reserve_rearm(', 'pub(crate) fn release_rearm(', 'pub(crate) fn require_fresh_backing(',
        'pub(crate) fn needs_fresh_backing(', 'pub(crate) fn pool_vacant(', 'pub(crate) fn pool_serves(',
        'pub(crate) fn pool_fits(', 'pub(crate) fn recycled_pending(', 'pub(crate) fn vacancy_pending(',
        'pub(crate) fn pool_release_pending(', 'pub(crate) fn vacate_candidate(', 'pub(crate) fn released_at(',
        'pub(crate) fn has_ring(', 'pub(crate) fn rearm(\n', 'pub(crate) fn vacate(&mut self',
        'pub(crate) fn observe_vacancy(', 'pub(crate) fn shareable(', 'pub(crate) fn owner(&self)',
        'pub(crate) fn channel_index(&self)', 'pub(crate) fn is_primary(&self)', 'pub(crate) fn active_len(&self)',
        'pub(crate) fn has_replays(&self)', 'pub(crate) fn configuring(&self)', 'pub(crate) fn context(&self)',
        'pub(crate) fn bind_sibling(', 'pub(crate) fn bind_owner(&mut self',
    ]).replace('pub(crate) ', 'pub ').replace('mmu::Vm', 'Vm') \
        .replace('crate::g17::freelist::Pages', 'Pages').replace('crate::g17::initdata::InitData', 'InitData') \
        .replace('crate::g17::now_ns()', 'now_ns()').replace('crate::g17::freelist::control_consumed', 'control_consumed')
    pool_fns = '\n'.join(item(freelist, signature) for signature in [
        'pub(crate) fn populated(&self)', 'pub(crate) fn serves(&self', 'pub(crate) fn take_pages(',
    ]).replace('pub(crate) ', 'pub ').replace('mmu::Vm', 'Vm')
    registry_fns = '\n'.join(item(runtime, signature) for signature in [
        'fn compute_channels_of(', 'fn next_sibling_index(', 'fn has_compute_sibling_headroom(',
        'fn wants_compute_sibling(', 'fn bind_reusable_sibling(', 'fn compute_queue_mut(',
        'fn give_back_foreign(', 'fn reserve_rearm(&mut self, qid', 'pub(super) fn release_rearm(',
        'pub(super) fn rearm_compute(', 'pub(super) fn bind_rearmed(', 'pub(super) fn vacate_idle_pools(',
    ] if True) + '\n' + source_fn
    registry_fns = registry_fns.replace('pub(super) ', '').replace('super::freelist::ComputeCapacity', 'ComputeCapacity') \
        .replace('super::pairs::', '').replace('super::initdata::InitData', 'InitData') \
        .replace('crate::g17::freelist::Pages', 'Pages').replace('compute::DescriptorRing', 'DescriptorRing') \
        .replace('super::freelist::PoolDescriptor', 'PoolDescriptor').replace('compute::Vacated', 'Vacated')
    enum = item(runtime, 'pub(super) enum SiblingSource').replace('pub(super) ', '')
    pairs_fns = '\n'.join([
        'const SIBLING_MIN_IN_FLIGHT: usize = '
        + pairs.split('pub(crate) const SIBLING_MIN_IN_FLIGHT: usize = ', 1)[1].split(';', 1)[0] + ';',
        item(pairs, 'pub(crate) fn overlaps(').replace('pub(crate) ', ''),
    ])

    try_add = item(runtime, 'fn try_add_compute_sibling(')
    rearm_vacant = item(runtime, 'fn rearm_vacant(')
    rebind = item(runtime, 'pub(super) fn rebind_compute(')
    build_compute = item(runtime, 'fn build_compute(')
    structure = {
        'try_add takes every source through its own path': ordered_ok(
            try_add, '|| self.shared.channels.has_waiters()', 'wants_compute_sibling(',
            'firmware.queues.sibling_source(self.owner, &self.context, required)',
            'Some(SiblingSource::Warm(_)) =>', 'bind_reusable_sibling(',
            'Some(SiblingSource::Foreign(qid)) =>', 'firmware.queues.give_back_foreign(qid)?;',
            'drop(state);', 'self.shared.queue_events();', 'return Ok(false);',
            'Some(SiblingSource::Vacant(qid)) =>', 'firmware.queues.reserve_rearm(qid, self.owner)?;', 'Err(qid)',
            'Some(SiblingSource::Fresh) =>', 'reserve_compute_channel(self.owner, self.compute_channels)',
            'Err(qid) => {', 'self.rearm_vacant(\n                    qid,\n                    channel_index,\n                    required,',
            'if !bound {', 'firmware.queues.release_rearm(qid, self.owner);', 'admission.check()?;',
            'return Ok(bound);', 'compute::PoolBacking::new('),
        'a re-arm binds the channel it was reserved for, through the primary path': ordered_ok(
            rearm_vacant, 'crate::g17::freelist::Pages::for_compute(', 'compute::DescriptorRing::new(',
            'let mut state = self.shared.state.lock();', 'if !lease.is_current()', 'self.admission_open()',
            'queue.qid() == qid && queue.can_rearm() && queue.rearm_open(self.owner)',
            '&& (channel_index == 0\n                || firmware.queues.next_sibling_index(self.owner, self.compute_channels)\n'
            '                    == Some(channel_index));',
            'firmware.queues.rearm_compute(qid, pages, ring)', 'firmware.init.install_compute_pool(descriptor)?;',
            'firmware.queues.bind_rearmed(qid, self.owner, &self.context, channel_index)?;'),
        'the primary re-arms channel 0': 'self.rearm_vacant(qid, 0, required,' in build_compute,
        'the primary gives back other VMs\' queues through the same helper': ordered_ok(
            rebind, '.filter(|queue| queue.reusable() && !queue.pool_vacant() && !queue.pool_serves(vm))',
            'if let Some(qid) = foreign {', 'self.give_back_foreign(qid)?;', 'return Err(ENOSPC);'),
    }
    checks = '\n'.join(f'    assert!({str(ok).lower()}, "structure: {name}");' for name, ok in structure.items())
    return (TEMPLATE.replace('@@ENUM@@', enum).replace('@@PAIRS@@', pairs_fns).replace('@@QUEUE@@', queue_fns)
            .replace('@@POOL@@', pool_fns).replace('@@REGISTRY@@', registry_fns).replace('@@STRUCTURE@@', checks))


TEMPLATE = r'''
#![allow(dead_code, unused_variables, unused_mut, unused_imports, unreachable_code, unused_assignments)]
use std::cell::RefCell;
use std::sync::Arc;
use std::sync::atomic::{fence, AtomicU32, Ordering};

const EIO: i32 = -5;
const EAGAIN: i32 = -11;
const EBUSY: i32 = -16;
const EPERM: i32 = -1;
const ENOSPC: i32 = -28;
type Result<T = ()> = std::result::Result<T, i32>;
const GFP_KERNEL: () = ();
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn with_capacity(n: usize, _: ()) -> Result<Self> { Ok(Self(Vec::with_capacity(n))) }
    fn push(&mut self, value: T, _: ()) -> Result { self.0.push(value); Ok(()) }
}
impl<T> std::ops::Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> std::ops::DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }

fn now_ns() -> u64 { 5_000_000_000 }

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct ComputeCapacity(u32);

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
@@ENUM@@

@@PAIRS@@

pub struct Vm(u32);
impl Vm { pub fn id(&self) -> u32 { self.0 } }
pub struct Context { id: u32, vm: Vm, current: bool }
impl Context {
    fn new(id: u32, vm: u32) -> Arc<Self> { Arc::new(Self { id, vm: Vm(vm), current: true }) }
    fn vm(&self) -> &Vm { &self.vm }
    fn is_current(&self) -> bool { self.current }
}

#[derive(Debug)]
pub struct Pages(u32);
pub struct DescriptorRing;
#[derive(Debug, PartialEq)]
pub struct PoolDescriptor(u32);
pub struct InitData { control_cursor: std::cell::Cell<u32> }
pub struct KernelMapping;
pub struct Vacated { _alias: Option<KernelMapping>, _pages: Option<Pages>, _descriptors: Option<KernelMapping>, _ring: Option<DescriptorRing> }
pub struct FreeListRelease;
enum ControlRecord { FreeListRelease(FreeListRelease) }
struct Rings;
impl Rings {
    fn publish_control(init: &InitData, _: &ControlRecord) -> Result<u32> {
        let cursor = init.control_cursor.get() + 1;
        init.control_cursor.set(cursor);
        Ok(cursor)
    }
}
/// The firmware consumed every control record up to `consumer`.
fn control_consumed(consumer: u32, _producer: u32, cursor: u32) -> Result<bool> { Ok(consumer >= cursor) }

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum PoolPhase { Populated, ReleaseSent(u32), Vacant }
pub struct Fifo { len: usize }
pub struct SourceHint;
impl SourceHint { fn has_readers(&self, count: &AtomicU32) -> bool { count.load(Ordering::Acquire) != 0 } }
pub struct Graph { ring: Option<DescriptorRing>, m5: bool }
impl Graph {
    fn rearm_ring(&mut self, ring: DescriptorRing) -> Option<DescriptorRing> { self.ring.replace(ring) }
    fn vacate_ring(&mut self) -> Option<DescriptorRing> { self.ring.take() }
}
pub struct Binding { context: Arc<Context>, pool_alias: Option<KernelMapping>, descriptors: Option<KernelMapping> }
pub struct Pool { id: u16, client_vm: u32, pages: Option<Pages>, cap: u32 }
impl Pool {
@@POOL@@
    fn fits_capacity(&self, required: ComputeCapacity) -> bool { self.pages.is_some() && self.cap >= required.0 }
    fn rearm(&mut self, pages: Pages) -> Result { if self.pages.is_some() { return Err(EBUSY); } self.pages = Some(pages); Ok(()) }
    fn descriptor(&self) -> Result<PoolDescriptor> { Ok(PoolDescriptor(u32::from(self.id))) }
    fn release_record(&self, _generation: u64) -> FreeListRelease { FreeListRelease }
}

/// The owner-change state of one physical compute queue, with the production field names.
pub struct Queue {
    qid: u8, growing: bool, channel_error: bool, installed: bool, configuration: Option<()>,
    pool_phase: PoolPhase, needs_fresh_backing: bool, exit: Option<()>, owner: Option<u64>,
    released: bool, retirement_ready: bool, quarantined: bool, active: Fifo, replays: Fifo,
    previous: Option<()>, source_hint: SourceHint, source_readers: AtomicU32, rearm_for: Option<u64>,
    pool: Pool, graph: Graph, binding: Option<Binding>, release_sent_ns: Option<u64>,
    free_list_generation: u64, released_at: u64, claims: u32, awaiting_witness: bool,
    channel_index: u8, sustained: bool,
}
impl Queue {
@@QUEUE@@
    fn qid(&self) -> u8 { self.qid }
    fn sustained(&self) -> bool { self.sustained }
    /// The production entry check of bind_owner_channel(); the views of the new VM are not modelled.
    fn bind_owner_channel(&mut self, owner: u64, context: Arc<Context>, channel_index: u8) -> Result {
        if !self.reusable() {
            return Err(EBUSY);
        }
        if !self.pool.serves(context.vm()) {
            return Err(EPERM);
        }
        self.pool.client_vm = context.vm().id();
        self.binding = Some(Binding { context, pool_alias: None, descriptors: None });
        self.owner = Some(owner);
        self.channel_index = channel_index;
        self.rearm_for = None;
        self.released = false;
        self.retirement_ready = false;
        Ok(())
    }
}

type QueueGuard<'a> = std::cell::RefMut<'a, Queue>;
struct Slot(RefCell<Queue>);
impl Slot {
    fn as_deref(&self) -> Option<std::cell::Ref<'_, Queue>> { Some(self.0.borrow()) }
    fn as_deref_mut(&mut self) -> Option<QueueGuard<'_>> { Some(self.0.borrow_mut()) }
}
struct Entry { queue: Slot }
struct Qids { fit: bool }
impl Qids { fn compute_fits(&self, _: usize) -> bool { self.fit } }
struct Admission;
impl Admission {
    fn queues(&self) -> u32 { 4 }
    fn render_demand(&self, extra: usize) -> usize { 4 + extra }
}
struct Teardown { releasing: Vec<u32> }
impl Teardown { fn releasing(&self, context: &Arc<Context>) -> bool { self.releasing.contains(&context.id) } }
struct Registry { compute: Vec<Option<Entry>>, qids: Qids, admission: Admission, teardown: Teardown }
impl Registry {
    const POOL_RETRY_NS: u64 = 100_000_000;
@@REGISTRY@@
    fn queue(&self, qid: u8) -> std::cell::Ref<'_, Queue> {
        self.compute.iter().flatten().map(|entry| entry.queue.0.borrow()).find(|queue| queue.qid == qid).unwrap()
    }
    fn queue_mut(&self, qid: u8) -> std::cell::RefMut<'_, Queue> {
        self.compute.iter().flatten().map(|entry| entry.queue.0.borrow_mut()).find(|queue| queue.qid == qid).unwrap()
    }
    /// An event pass: the production pool pass gives back what must go, then the firmware
    /// consumes the control ring and the release observation leaves those queues vacant.
    fn pool_pass(&mut self, init: &InitData) {
        vacate_idle_pools_pass(self, init);
        let consumer = init.control_cursor.get();
        for entry in self.compute.iter_mut().flatten() {
            let mut queue = entry.queue.0.borrow_mut();
            let _ = queue.observe_vacancy(consumer, consumer).unwrap();
        }
    }
    fn push(&mut self, queue: Queue) { self.compute.push(Some(Entry { queue: Slot(RefCell::new(queue)) })); }
}
fn vacate_idle_pools_pass(registry: &mut Registry, init: &InitData) {
    // Idle pools stay warm (cap 3, no expiry): only queues given back are released.
    registry.vacate_idle_pools(init, 3, now_ns(), u64::MAX).unwrap();
}

/// An installed queue owned by `owner` (channel 0) with `active` kicks in flight.
fn bound(qid: u8, owner: u64, context: &Arc<Context>, active: usize) -> Queue {
    Queue {
        qid, growing: false, channel_error: false, installed: true, configuration: None,
        pool_phase: PoolPhase::Populated, needs_fresh_backing: false, exit: None, owner: Some(owner),
        released: false, retirement_ready: true, quarantined: false, active: Fifo { len: active },
        replays: Fifo { len: 0 }, previous: None, source_hint: SourceHint, source_readers: AtomicU32::new(0),
        rearm_for: None, pool: Pool { id: u16::from(qid), client_vm: context.vm().id(), pages: Some(Pages(1)), cap: 16 },
        graph: Graph { ring: Some(DescriptorRing), m5: true },
        binding: Some(Binding { context: context.clone(), pool_alias: None, descriptors: None }),
        release_sent_ns: None, free_list_generation: 1, released_at: 0, claims: 0, awaiting_witness: false,
        channel_index: 0, sustained: true,
    }
}
/// An idle queue whose owner released it at `released_at`; its pool still serves `vm`.
fn released(qid: u8, vm: u32, released_at: u64) -> Queue {
    let mut queue = bound(qid, 0, &Context::new(1000 + u32::from(qid), vm), 0);
    queue.owner = None;
    queue.released = true;
    queue.released_at = released_at;
    queue.sustained = false;
    queue
}
/// The same, after its pool was given back (the idle expiry): vacant, without a ring.
fn vacant(qid: u8, released_at: u64) -> Queue {
    let mut queue = released(qid, 0, released_at);
    queue.pool_phase = PoolPhase::Vacant;
    queue.pool.pages = None;
    queue.pool.client_vm = 0;
    queue.graph.ring = None;
    queue
}

/// try_add_compute_sibling()'s locked decision and the off-lock re-arm, through the production
/// registry calls in the order the backend makes them. Returns whether a sibling was bound.
fn add_sibling(reg: &mut Registry, init: &InitData, owner: u64, context: &Arc<Context>) -> Result<bool> {
    let req = ComputeCapacity(16);
    if !reg.wants_compute_sibling(owner, context, 3, req) {
        return Ok(false);
    }
    let Some(channel_index) = reg.next_sibling_index(owner, 3) else { return Ok(false) };
    match reg.sibling_source(owner, context, req) {
        None => Ok(false),
        Some(SiblingSource::Warm(_)) => Ok(reg.bind_reusable_sibling(owner, context, 3, req)? == Some(true)),
        Some(SiblingSource::Foreign(qid)) => { reg.give_back_foreign(qid)?; Ok(false) }
        Some(SiblingSource::Fresh) => Ok(false),
        Some(SiblingSource::Vacant(qid)) => {
            reg.reserve_rearm(qid, owner)?;
            // Off the device mutex: fresh pages and a descriptor ring; then, locked again, the
            // production rearm_vacant() checks and steps.
            let ready = {
                let queue = reg.queue(qid);
                queue.can_rearm() && queue.rearm_open(owner)
            } && reg.next_sibling_index(owner, 3) == Some(channel_index);
            if !ready {
                reg.release_rearm(qid, owner);
                return Ok(false);
            }
            let (descriptor, _vacated) = reg.rearm_compute(qid, Pages(2), DescriptorRing)?;
            assert_eq!(descriptor, PoolDescriptor(u32::from(qid)), "the re-armed queue's row is written");
            reg.bind_rearmed(qid, owner, context, channel_index)?;
            Ok(true)
        }
    }
}

fn registry() -> Registry {
    Registry { compute: Vec::new(), qids: Qids { fit: false }, admission: Admission, teardown: Teardown { releasing: Vec::new() } }
}

const B: u64 = 2;
const C: u64 = 3;
const D: u64 = 4;
const OTHER: u64 = 9;

fn main() {
@@STRUCTURE@@

    let init = InitData { control_cursor: std::cell::Cell::new(0) };
    let req = ComputeCapacity(16);
    let b = Context::new(20, 200);
    let c = Context::new(30, 300);
    let d = Context::new(40, 400);
    let mut reg = registry();
    // B's primary runs sustained heavy work with two kicks in flight. No fresh QID fits.
    reg.push(bound(10, B, &b, 2));
    // An exited client's queues: one given back at the idle expiry (vacant), one still holding
    // the exited VM's pool.
    reg.push(vacant(11, 5));
    reg.push(released(12, 999, 6));
    // Never to be taken: C's busy queue, D's idle channel (D is open), a vacant queue another
    // client is re-arming and a queue whose scheduler release is in flight.
    reg.push(bound(13, C, &c, 1));
    reg.push(bound(14, D, &d, 0));
    let mut reserved = vacant(15, 1);
    reserved.rearm_for = Some(OTHER);
    reg.push(reserved);
    reg.push(released(16, 998, 2));
    reg.teardown.releasing.push(1016);
    let untouchable = [13u8, 14, 15, 16];
    assert!(reg.queue(14).shareable(), "D's idle channel is shareable, so only a primary may take it");
    assert!(!reg.queue(14).reusable());

    // No lockout: the sibling comes from the vacant queue, re-armed and bound as channel 1.
    assert!(reg.has_compute_sibling_headroom(B, &b, req), "vacant and other-VM queues are headroom");
    assert!(reg.wants_compute_sibling(B, &b, 3, req));
    assert_eq!(reg.bind_reusable_sibling(B, &b, 3, req), Ok(Some(false)), "nothing warm to bind at once");
    assert_eq!(reg.sibling_source(B, &b, req), Some(SiblingSource::Vacant(11)));
    assert_eq!(add_sibling(&mut reg, &init, B, &b), Ok(true));
    {
        let q = reg.queue(11);
        assert_eq!((q.owner, q.channel_index, q.pool_phase), (Some(B), 1, PoolPhase::Populated));
        assert!(q.has_ring() && !q.needs_fresh_backing() && q.rearm_for.is_none());
    }
    assert_eq!(reg.compute_channels_of(B), (2, 0b11));

    // The third channel. While a pool release is in flight (it ends in a vacant queue) no other
    // VM's pool is given back, and the releasing queue itself is not taken.
    reg.queue_mut(11).active.len = 1;
    let mut releasing = released(17, 997, 3);
    releasing.pool_phase = PoolPhase::ReleaseSent(u32::MAX);
    reg.push(releasing);
    assert!(!reg.queue(17).reusable() && reg.queue(17).vacancy_pending());
    assert_eq!(reg.sibling_source(B, &b, req), None);
    assert_eq!(add_sibling(&mut reg, &init, B, &b), Ok(false));
    assert!(!reg.queue(12).needs_fresh_backing(), "nothing given back meanwhile");
    reg.compute.pop();
    // Then no vacant queue is left, so the exited VM's queue is given back.
    assert_eq!(reg.sibling_source(B, &b, req), Some(SiblingSource::Foreign(12)));
    assert_eq!(add_sibling(&mut reg, &init, B, &b), Ok(false), "given back, not yet vacant");
    assert!(reg.queue(12).needs_fresh_backing() && reg.queue(12).vacancy_pending());
    assert!(!reg.queue(12).reusable(), "nobody binds its old pool meanwhile");
    // One give-back at a time: nothing else is offered while that release is pending.
    assert_eq!(reg.sibling_source(B, &b, req), None);
    assert!(!reg.wants_compute_sibling(B, &b, 3, req));
    // The pool pass releases it; once consumed the queue is vacant and is re-armed for B.
    reg.pool_pass(&init);
    assert_eq!(reg.queue(12).pool_phase, PoolPhase::Vacant);
    assert!(reg.queue(12).pool_serves(b.vm()), "fresh backing serves any VM");
    assert_eq!(reg.sibling_source(B, &b, req), Some(SiblingSource::Vacant(12)));
    assert_eq!(add_sibling(&mut reg, &init, B, &b), Ok(true));
    assert_eq!((reg.queue(12).owner, reg.queue(12).channel_index), (Some(B), 2));
    assert_eq!(reg.compute_channels_of(B), (3, 0b111));
    assert!(!reg.wants_compute_sibling(B, &b, 3, req), "three channels at most");

    // Nothing was stolen.
    for qid in untouchable {
        let q = reg.queue(qid);
        assert_ne!(q.owner, Some(B), "queue {} was taken", qid);
        assert!(!q.needs_fresh_backing(), "queue {} was given back", qid);
    }
    assert_eq!(reg.queue(13).owner, Some(C));
    assert_eq!(reg.queue(14).owner, Some(D));
    assert_eq!(reg.queue(15).rearm_for, Some(OTHER));

    // Clients come and go: each new one gets its siblings back from what the last one left.
    for round in 0..4u32 {
        let vm = 500 + round;
        let next = Context::new(50 + round, vm);
        let owner = 100 + u64::from(round);
        // The previous owner's channels (B's, then each round's) are released idle; their
        // pools serve the exited VM. The newcomer's primary is one of them, rebound (modelled).
        let mut freed: Vec<u8> = Vec::new();
        for entry in reg.compute.iter().flatten() {
            let mut q = entry.queue.0.borrow_mut();
            if q.owner.is_some() && !untouchable.contains(&q.qid) {
                q.owner = None;
                q.released = true;
                q.retirement_ready = true;
                q.active.len = 0;
                q.released_at = u64::from(round) * 10 + u64::from(q.qid);
                freed.push(q.qid);
            }
        }
        assert_eq!(freed.len(), 3, "round {}: three channels released", round);
        let primary = freed[0];
        {
            let mut q = reg.queue_mut(primary);
            q.owner = Some(owner);
            q.channel_index = 0;
            q.released = false;
            q.pool_phase = PoolPhase::Vacant;
            q.pool.pages = Some(Pages(3));
            q.pool_phase = PoolPhase::Populated;
            q.pool.client_vm = vm;
            q.sustained = true;
            q.active.len = 2;
        }
        let mut added = 0;
        for _ in 0..8 {
            if add_sibling(&mut reg, &init, owner, &next).unwrap() {
                added += 1;
                // Every bound channel stays busy.
                for entry in reg.compute.iter().flatten() {
                    let mut q = entry.queue.0.borrow_mut();
                    if q.owner == Some(owner) && q.active.len == 0 {
                        q.active.len = 1;
                    }
                }
            } else {
                reg.pool_pass(&init);
            }
        }
        assert_eq!(added, 2, "round {}: no lockout", round);
        assert_eq!(reg.compute_channels_of(owner).0, 3);
        for qid in untouchable {
            assert_ne!(reg.queue(qid).owner, Some(owner));
        }
    }

    // A fresh QID comes before another VM's pool is given back.
    let mut reg = registry();
    reg.qids.fit = true;
    reg.push(bound(10, B, &b, 2));
    reg.push(released(12, 999, 6));
    assert_eq!(reg.sibling_source(B, &b, req), Some(SiblingSource::Fresh));
    // A warm same-VM queue comes before a vacant one.
    reg.push(vacant(11, 1));
    reg.push(released(18, 200, 9));
    assert_eq!(reg.sibling_source(B, &b, req), Some(SiblingSource::Warm(18)));
    assert_eq!(reg.bind_reusable_sibling(B, &b, 3, req), Ok(Some(true)));
    assert_eq!(reg.queue(18).channel_index, 1);
    // ... and a vacant one before a fresh QID; the longest released first.
    reg.push(vacant(19, 0));
    assert_eq!(reg.sibling_source(B, &b, req), Some(SiblingSource::Vacant(19)));

    println!("PASS g17-m5-sibling-rearm: vacant and other-VM idle queues become siblings, nothing busy or open is taken");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    source = build(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-sibling-rearm-') as tmp:
        path = Path(tmp) / 'rearm.rs'
        binary = path.with_suffix('')
        path.write_text(source)
        built = subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '-O', str(path),
                                '-o', str(binary)], capture_output=True, text=True)
        if built.returncode:
            sys.stderr.write(built.stderr)
            return built.returncode
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
