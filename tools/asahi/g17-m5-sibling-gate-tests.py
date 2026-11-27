#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test for the M5 Pro sibling compute fan-out gate.

Compiles the production gate against host stubs: the heavy-kick run in
drivers/gpu/drm/asahi/g17/pairs.rs, `Backend::fans_out()`, the lock-free channel
selection section of `compute_direct()`, the `ComputeCell` hint mirror, and the
registry's `select_compute_channel()` / `wants_compute_sibling()`. It checks:

a. A command with cross-queue input sources never leaves the primary channel and
   never brings up a sibling, on the direct and the device-mutex paths, and the
   direct path still resolves those sources before routing (published sources,
   binding loans).
b. Kicks whose firmware execution records stay under the heavy threshold stream
   down the primary ring: no sibling routing, no detour to the device-mutex path
   to add one, no new sibling; one long kick among short ones does not open the
   gate and one short kick closes it.
c. Sustained heavy kicks fan out onto an idle sibling (or ask for one when every
   channel is busy) only once two kicks of the owner are already in flight, so a
   kick queued behind one in flight stays on the primary ring; each sibling has
   its own compute pool, with monotonic per-context ordinals and object ids, and
   the gate is read through the primary cell's lock-free hint.
d. The sibling channels keep the published-source, binding-loan and idle
   housekeeping paths of the queues they share a context with.

Each mutation restores one old behaviour or drops one part of the rule, and must
fail an assertion of the compiled harness.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASAHI = ROOT / 'drivers/gpu/drm/asahi'
G17 = ASAHI / 'g17'


def item(source, signature):
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def const(source, name):
    match = re.search(r'(?:pub\(crate\) )?const ' + name + r': [^=]+= [^;]+;', source, re.S)
    assert match, name
    return match.group(0).replace('pub(crate) ', '')


def ordered_ok(source, *parts):
    at = -1
    for part in parts:
        idx = source.find(part, at + 1)
        if idx < 0:
            return False
        at = idx
    return True


# name -> [(file, old, new)]: each must apply exactly once.
MUTATIONS = {
    # (a) a command with sources fans out like any other.
    'sources-fan-out': [('runtime', '            && packet.completion.sources().is_none()\n            && primary_sustained()',
                         '            && primary_sustained()')],
    'select-sources-anywhere': [('runtime', '        if let Some((qid, _)) = primary.filter(|_| packet.completion.sources().is_some()) {\n            return Some(qid);\n        }\n', '')],
    # (b) short kicks fan out.
    'tiny-fans-out': [('pairs', 'if end.saturating_sub(start) >= SIBLING_EXECUTION_TICKS {', 'if end >= start {')],
    'no-sustain-run': [('pairs', 'run >= SIBLING_SUSTAINED_KICKS', 'run >= 1')],
    'short-keeps-run': [('pairs', '    } else {\n        0\n    }\n}', '    } else {\n        run\n    }\n}')],
    'direct-ignores-gate': [('runtime', 'let target_idx = if cached_count == 1 || !fans_out {', 'let target_idx = if cached_count == 1 {')],
    'direct-can-add-ungated': [('runtime', '        let can_add_sibling = fans_out\n            && cached_count',
                                '        let can_add_sibling = packet.order.wait_through[1].is_none()\n            && cached_count')],
    'wants-ignores-sustained': [('runtime', 'if !sustained_primary || !all_busy', 'if !all_busy')],
    'select-ignores-sustained': [('runtime', '        if let Some((qid, false)) = primary {\n            return Some(qid);\n        }\n', '')],
    # (c) the lock-free hint loses the gate, or the gate takes a lock first.
    'cell-drops-sustained': [('runtime', 'self.sustained.store(queue.sustained(), Ordering::Release);',
                              'self.sustained.store(false, Ordering::Release);')],
    'fans-out-locks-first': [('runtime', '        self.compute_channels > 1\n            && packet.order.wait_through[1].is_none()\n'
                              '            && packet.completion.sources().is_none()\n            && primary_sustained()',
                              '        primary_sustained()\n            && self.compute_channels > 1\n'
                              '            && packet.order.wait_through[1].is_none()\n            && packet.completion.sources().is_none()')],
    # (c) A kick overlaps the owner's work while only one kick is in flight.
    'overlap-at-one': [('pairs', 'in_flight >= SIBLING_MIN_IN_FLIGHT', 'in_flight >= 1')],
    'direct-ignores-in-flight': [('runtime', '                if !super::pairs::overlaps(in_flight) {\n                    0\n                } else if',
                                  '                if false {\n                    0\n                } else if')],
    'select-ignores-in-flight': [('runtime', '        if let Some((qid, _)) = primary.filter(|_| !super::pairs::overlaps(in_flight)) {\n'
                                  '            return Some(qid);\n        }\n', '')],
    'wants-ignores-in-flight': [('runtime', ' || !super::pairs::overlaps(in_flight) {', ' {')],
    'detour-at-one': [('runtime', '                    && super::pairs::overlaps(queue.active_len()))',
                       '                    && (queue.active_len() != 0 || queue.configuring()))')],
    # A reused channel keeps the previous owner's run of heavy kicks.
    'keep-run-across-owners': [('queue', '        self.channel_index = channel_index;\n        self.heavy_run = 0;\n',
                                '        self.channel_index = channel_index;\n')],
}


def build(mutation=None):
    files = {
        'pairs': (G17 / 'pairs.rs').read_text(),
        'runtime': (G17 / 'runtime.rs').read_text(),
        'queue': (G17 / 'queue/compute.rs').read_text(),
    }
    for name, old, new in MUTATIONS.get(mutation, []):
        assert files[name].count(old) == 1, (mutation, old)
        files[name] = files[name].replace(old, new)
    pairs, runtime, queue = files['pairs'], files['runtime'], files['queue']
    context = (G17 / 'context.rs').read_text()
    hw = (ASAHI / 'hw/t8140.rs').read_text()
    events = (G17 / 'runtime/events.rs').read_text()
    submit = (G17 / 'runtime/render/submit.rs').read_text()

    hz = const(hw, 'COMMAND_TIMESTAMP_HZ')
    gate = '\n'.join([
        const(pairs, 'SIBLING_EXECUTION_NS'),
        const(pairs, 'SIBLING_EXECUTION_TICKS').replace('crate::hw::t8140::resources::', ''),
        const(pairs, 'SIBLING_SUSTAINED_KICKS'),
        const(pairs, 'SIBLING_MIN_IN_FLIGHT'),
        item(pairs, 'pub(crate) fn heavy_run('),
        item(pairs, 'pub(crate) fn sustained('),
        item(pairs, 'pub(crate) fn overlaps('),
    ]).replace('pub(crate) ', '')
    next_id = item(context, 'pub(crate) fn next_compute_identity(').replace('pub(crate) ', '')
    queue_sustained = item(queue, 'pub(crate) fn sustained(&self)').replace('pub(crate) ', 'pub ') \
        .replace('crate::g17::pairs::sustained', 'crate::sustained')
    sync_stats = item(runtime, 'fn sync_stats(')
    load_hint = item(runtime, 'pub(super) fn load_hint(').replace('pub(super) ', '')
    cell_sustained = item(runtime, 'pub(super) fn sustained(&self)').replace('pub(super) ', '')
    fans_out = item(runtime, 'fn fans_out(').replace('super::job::Packet', 'Packet')
    direct = item(runtime, 'fn compute_direct(')
    select_start = direct.index('        let primary = cells[0].as_ref()?;')
    select_end = direct.index('        let target_cell = cells[target_idx].as_ref()?;')
    route = direct[select_start:select_end].replace('super::pairs::', '')
    registry = '\n'.join(item(runtime, signature) for signature in [
        'fn compute_channels_of(', 'fn next_sibling_index(', 'fn sibling_source(',
        'fn has_compute_sibling_headroom(', 'fn wants_compute_sibling(', 'fn select_compute_channel(',
    ]).replace('super::job::Packet', 'Packet').replace('super::freelist::ComputeCapacity', 'ComputeCapacity') \
        .replace('super::pairs::', '')
    sibling_source = item(runtime, 'pub(super) enum SiblingSource').replace('pub(super) ', '')
    inline = item(runtime, 'fn compute_inline(')
    packet_fn = item(runtime, '    fn compute_packet(')
    readiness = item(runtime, 'fn compute_readiness(&self, packet:')
    bind_owner = item(queue, 'fn bind_owner_channel(')
    clear_owner = item(queue, 'fn clear_owner(')
    publish = item(queue, 'pub(in crate::g17) fn publish_with_native_waits(')
    retire_arm_start = queue.index('Ok(Some((timestamps, stampless))) => {')
    retire_arm = queue[retire_arm_start:queue.index('Ok(None) | Err(EAGAIN) => {', retire_arm_start)]
    render_deps = item(submit, 'fn render_dependencies(')

    # Structural rules that the compiled part cannot see, each a Rust assertion below.
    structure = {
        # (a) Both device-mutex paths ask the gate before any sibling work.
        'mutex paths ask fans_out before binding a sibling': (
            ordered_ok(inline, 'if self.fans_out(packet, || {', 'compute_primary(self.owner)',
                       '.is_some_and(|queue| queue.sustained())', 'wants_compute_sibling(',
                       'bind_reusable_sibling(')
            and ordered_ok(packet_fn, 'if !sibling_tried', '&& self.fans_out(packet, || {',
                           '.is_some_and(|queue| queue.sustained())', 'wants_compute_sibling(',
                           'bind_reusable_sibling(', 'self.try_add_compute_sibling(required, &admission)?')
            and 'packet.order.wait_through[1].is_none()\n                {' not in packet_fn),
        # (a, d) Direct sources still resolve lock-free from their PublishedSource snapshots with
        # binding loans before any routing, and the holds follow the publication.
        'direct sources resolve before routing, holds follow publication': ordered_ok(
            direct, 'let generation = port.generation();',
            'self.resolve_direct_sources(port, generation, target_qid, m5, sources)?',
            'let fans_out = self.fans_out(packet, || primary.sustained());',
            'let target_cell = cells[target_idx].as_ref()?;', 'direct_frontier.as_slice()',
            'if packet.is_published() {', 'sources.set_holds(direct_holds);', 'drop(direct_holds);'),
        'readiness claims the primary for sources': ordered_ok(
            readiness, 'let sources_checked =', 'if bound_count != 0',
            'cells[1..].iter().flatten()', 'queue.claim()', 'self.hold_claim(Some(primary));'),
        # (b, c) The direct path detours to the device-mutex path for a sibling only when it may
        # add one: a busy single channel under a closed gate, or with a single kick in flight,
        # keeps publishing directly.
        'direct detours only when it may add a sibling': ordered_ok(
            direct, '(cached_count == 1\n                    && can_add_sibling\n'
            '                    && super::pairs::overlaps(queue.active_len()))'),
        # (b, c) The run is measured on exact execution records only, once per retired kick,
        # and starts over for every owner.
        'only exact records measure a kick': ordered_ok(
            retire_arm, 'let execution = front.observed.map(|record| record.timestamps);',
            'defer(match producers {', 'self.retire_pending = false;',
            'if let Some([start, end]) = execution {',
            'self.heavy_run = crate::g17::pairs::heavy_run(self.heavy_run, start, end);'),
        'a new owner starts a new run': (
            'self.heavy_run = 0;' in clear_owner
            and ordered_ok(bind_owner, 'self.channel_index = channel_index;', 'self.heavy_run = 0;')),
        # (d) A sibling's own QID and source readers are what its PublishedSource names; the
        # shared context keeps one ordinal sequence across channels.
        'siblings publish their own QID as a source': ordered_ok(
            publish, 'packet.context.next_compute_identity()',
            'crate::g17::fw::kick::KickDependency::new(qid, kick_timestamp)',
            'packet.completion.note_source_published(', 'readers: self.source_readers.clone(),'),
        # (d) The deferred idle housekeeping counts every compute queue, siblings included, as
        # in flight; renders wait on every compute channel of the owner.
        'idle housekeeping sees sibling work': ordered_ok(
            events, '.compute', '.iter()', '.flatten()', '.filter_map(|entry| entry.queue.as_deref())',
            '.any(|queue| queue.in_flight())', 'firmware.idle_check(now, in_flight)?'),
        'renders wait on every compute channel': ordered_ok(
            render_deps, '.filter(|queue| queue.owner() == Some(owner))', '.compute_count',
            'dependencies.compute_count += 1;'),
    }
    checks = '\n'.join(f'    assert!({str(ok).lower()}, "structure: {name}");'
                       for name, ok in structure.items())

    return (TEMPLATE
            .replace('@@HZ@@', hz)
            .replace('@@GATE@@', gate)
            .replace('@@NEXT_ID@@', next_id)
            .replace('@@QUEUE_SUSTAINED@@', queue_sustained)
            .replace('@@SYNC_STATS@@', sync_stats)
            .replace('@@LOAD_HINT@@', load_hint)
            .replace('@@CELL_SUSTAINED@@', cell_sustained)
            .replace('@@FANS_OUT@@', fans_out)
            .replace('@@ROUTE@@', route)
            .replace('@@REGISTRY@@', registry)
            .replace('@@SIBLING_SOURCE@@', sibling_source)
            .replace('@@STRUCTURE@@', checks))


TEMPLATE = r'''
#![allow(dead_code, unused_variables, unused_mut, unused_imports)]
use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

const EIO: i32 = -5;
type Result<T = ()> = std::result::Result<T, i32>;

@@HZ@@
@@GATE@@

thread_local! { static NOW: Cell<u64> = Cell::new(1_000_000_000); }
fn now_ns() -> u64 { NOW.with(|now| now.get()) }

/// Ticks of an execution record lasting `us` microseconds.
fn ticks(us: u64) -> u64 { us * COMMAND_TIMESTAMP_HZ / 1_000_000 }

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct ComputeCapacity(u32);

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
@@SIBLING_SOURCE@@

struct Completion { sources: Option<()> }
impl Completion { fn sources(&self) -> Option<&()> { self.sources.as_ref() } }
struct Order { wait_through: [Option<u64>; 2] }
struct Packet { order: Order, completion: Completion }
fn packet(sources: bool, through: Option<u64>) -> Packet {
    Packet { order: Order { wait_through: [None, through] },
        completion: Completion { sources: sources.then_some(()) } }
}

struct Context { vm_id: u32, compute_ordinal: AtomicU32 }
impl Context {
    fn new(vm_id: u32) -> Arc<Self> { Arc::new(Self { vm_id, compute_ordinal: AtomicU32::new(0) }) }
    fn vm(&self) -> u32 { self.vm_id }
    @@NEXT_ID@@
}

mod compute {
    /// The routing-relevant state of one physical compute channel.
    pub struct Queue {
        pub qid: u8, pub owner: Option<u64>, pub channel_index: u8, pub active: usize,
        pub replays: usize, pub configuring: bool, pub last_published: u64, pub heavy_run: u8,
        pub reusable: bool, pub pool_vm: u32, pub pool_cap: u32,
    }
    impl Queue {
        pub fn active_len(&self) -> usize { self.active }
        pub fn configuring(&self) -> bool { self.configuring }
        pub fn last_published(&self) -> u64 { self.last_published }
        @@QUEUE_SUSTAINED@@
    }
}

/// The hint fields of the production cell; the mutex-held queue is passed in.
struct ComputeCell {
    active_len: AtomicU32, configuring: AtomicBool, last_published: AtomicU64, sustained: AtomicBool,
}
impl ComputeCell {
    fn new() -> Arc<Self> {
        Arc::new(Self { active_len: AtomicU32::new(0), configuring: AtomicBool::new(false),
            last_published: AtomicU64::new(0), sustained: AtomicBool::new(false) })
    }
    @@SYNC_STATS@@
    @@LOAD_HINT@@
    @@CELL_SUSTAINED@@
}

struct Backend { compute_channels: u8, sibling_backoff_until: AtomicU64, gate_asked: Cell<u32> }
impl Backend {
    @@FANS_OUT@@
    /// compute_direct()'s lock-free channel selection: `None` sends the packet to the
    /// device-mutex path, otherwise `(target channel, may add a sibling)`.
    fn route(&self, packet: &Packet, cells: &[Option<Arc<ComputeCell>>; 3], cached_count: usize)
        -> Option<(usize, bool)>
    {
@@ROUTE@@
        Some((target_idx, can_add_sibling))
    }
}

impl compute::Queue {
    fn qid(&self) -> u8 { self.qid }
    fn owner(&self) -> Option<u64> { self.owner }
    fn channel_index(&self) -> u8 { self.channel_index }
    fn is_primary(&self) -> bool { self.channel_index == 0 }
    fn has_replays(&self) -> bool { self.replays != 0 }
    fn reusable(&self) -> bool { self.reusable }
    fn needs_fresh_backing(&self) -> bool { false }
    fn pool_vacant(&self) -> bool { false }
    fn rearm_open(&self, _: u64) -> bool { true }
    fn released_at(&self) -> u64 { 0 }
    fn vacancy_pending(&self) -> bool { false }
    fn pool_serves(&self, vm: u32) -> bool { self.pool_vm == vm }
    fn pool_fits(&self, required: ComputeCapacity) -> bool { self.pool_cap >= required.0 }
    fn has_ring(&self) -> bool { true }
    fn context(&self) -> Option<&Arc<Context>> { None }
}

struct Slot(RefCell<compute::Queue>);
impl Slot { fn as_deref(&self) -> Option<std::cell::Ref<'_, compute::Queue>> { Some(self.0.borrow()) } }
struct Entry { queue: Slot }
struct Qids { fit: bool }
impl Qids { fn compute_fits(&self, _: usize) -> bool { self.fit } }
struct Admission;
impl Admission {
    fn queues(&self) -> u32 { 1 }
    fn render_demand(&self, extra: usize) -> usize { 1 + extra }
}
struct Teardown;
impl Teardown { fn releasing(&self, _: &Arc<Context>) -> bool { false } }
struct Registry { compute: Vec<Option<Entry>>, qids: Qids, admission: Admission, teardown: Teardown }
impl Registry {
@@REGISTRY@@
}

const OWNER: u64 = 7;

fn channel(qid: u8, channel_index: u8, active: usize, heavy_run: u8) -> compute::Queue {
    compute::Queue { qid, owner: Some(OWNER), channel_index, active, replays: 0, configuring: false,
        last_published: u64::from(qid), heavy_run, reusable: false, pool_vm: 70, pool_cap: 16 }
}

fn registry(queues: Vec<compute::Queue>) -> Registry {
    Registry { compute: queues.into_iter().map(|queue| Some(Entry { queue: Slot(RefCell::new(queue)) })).collect(),
        qids: Qids { fit: true }, admission: Admission, teardown: Teardown }
}

/// The cells compute_direct() caches, each hint mirrored from its queue by the production
/// sync_stats(), as the queue guard does when it drops.
fn cells(queues: &[compute::Queue]) -> [Option<Arc<ComputeCell>>; 3] {
    let mut cells = [None, None, None];
    for queue in queues {
        let cell = ComputeCell::new();
        cell.sync_stats(Some(queue));
        cells[usize::from(queue.channel_index)] = Some(cell);
    }
    cells
}

fn backend() -> Backend {
    Backend { compute_channels: 3, sibling_backoff_until: AtomicU64::new(0), gate_asked: Cell::new(0) }
}

fn run_of(records_us: &[u64]) -> u8 {
    records_us.iter().fold(0, |run, &us| heavy_run(run, 5_000, 5_000 + ticks(us)))
}

fn main() {
@@STRUCTURE@@

    // The threshold, from the record's 24 MHz command timestamps.
    assert_eq!(SIBLING_EXECUTION_TICKS, ticks(SIBLING_EXECUTION_NS / 1_000));
    assert_eq!(SIBLING_EXECUTION_TICKS, 2_400, "100 us at 24 MHz");

    // (b) Short kicks never build a run; one long kick among short ones does not open the
    // gate; a short kick closes it at once.
    assert_eq!(run_of(&[3; 64]), 0, "a stream of 3 us kicks has no heavy run");
    assert_eq!(run_of(&[99]), 0, "just under the threshold is short");
    assert!(!sustained(run_of(&[3, 3, 900, 3, 3])), "one long kick does not fan a stream out");
    assert!(!sustained(run_of(&[270])), "one heavy kick is not yet sustained");
    assert!(sustained(run_of(&[100, 270])), "two consecutive heavy kicks are");
    assert!(sustained(run_of(&[490, 490, 490])));
    assert!(!sustained(run_of(&[490, 490, 490, 3])), "a short kick closes the gate");
    assert_eq!(run_of(&[490; 300]), u8::MAX, "the run saturates");
    assert_eq!(heavy_run(5, 9_000, 1_000), 0, "a backwards record measures nothing");

    let b = backend();
    // (a, b) The gate: only independent commands without sources, and the primary's state is
    // asked last so a command that cannot fan out never looks at (or locks) the primary.
    let ask = |answer: bool| { b.gate_asked.set(b.gate_asked.get() + 1); answer };
    assert!(b.fans_out(&packet(false, None), || ask(true)));
    assert!(!b.fans_out(&packet(false, None), || ask(false)), "a light primary keeps its kicks");
    let asked = b.gate_asked.get();
    assert!(!b.fans_out(&packet(true, None), || ask(true)), "sources never fan out");
    assert!(!b.fans_out(&packet(false, Some(4)), || ask(true)), "nor in-queue barriers");
    assert_eq!(b.gate_asked.get(), asked, "the primary is not asked for them");
    let single = Backend { compute_channels: 1, ..backend() };
    assert!(!single.fans_out(&packet(false, None), || ask(true)), "one channel per owner");

    // (c) The lock-free mirror of the gate.
    let heavy_primary = channel(10, 0, 1, 2);
    let cell = ComputeCell::new();
    cell.sync_stats(Some(&heavy_primary));
    assert!(cell.sustained());
    assert_eq!(cell.load_hint(), (false, 1, 10));
    cell.sync_stats(None);
    assert!(!cell.sustained(), "an emptied cell is not sustained");

    let light = [channel(10, 0, 4, 0), channel(11, 1, 0, 0), channel(12, 2, 0, 0)];
    let heavy = [channel(10, 0, 2, 2), channel(11, 1, 0, 0), channel(12, 2, 0, 0)];
    let heavy_one = [channel(10, 0, 1, 2), channel(11, 1, 0, 0), channel(12, 2, 0, 0)];
    let heavy_busy = [channel(10, 0, 1, 2), channel(11, 1, 1, 0)];
    let indep = packet(false, None);
    let sourced = packet(true, None);

    // (b) Short kicks stay on the busy primary ring even with idle siblings bound, and a lone
    // busy primary does not detour to the device-mutex path to add a sibling.
    assert_eq!(b.route(&indep, &cells(&light), 3), Some((0, false)), "short kicks stream down channel 0");
    assert_eq!(b.route(&indep, &cells(&light[..1]), 1), Some((0, false)),
        "a busy light primary keeps publishing directly");
    // (a) A command with sources takes the primary even when it is busy and heavy.
    assert_eq!(b.route(&sourced, &cells(&heavy), 3), Some((0, false)), "sources stay on channel 0");
    assert_eq!(b.route(&sourced, &cells(&heavy[..1]), 1), Some((0, false)),
        "sources never ask for a sibling");
    // (c) Sustained heavy work fans out onto the least-loaded idle sibling, asks the
    // device-mutex path for a sibling when every bound channel is busy, and a lone heavy
    // busy primary may add one.
    assert_eq!(b.route(&indep, &cells(&heavy), 3), Some((1, false)), "heavy kicks take an idle sibling");
    // (c) ... once two are in flight: behind a single kick the next one queues on channel 0.
    assert_eq!(b.route(&indep, &cells(&heavy_one), 3), Some((0, false)),
        "a kick queues behind a single kick in flight");
    assert_eq!(b.route(&indep, &cells(&[channel(10, 0, 1, 2), channel(11, 1, 0, 0)]), 2).map(|(idx, _)| idx),
        Some(0), "also with an idle sibling bound");
    assert_eq!(b.route(&indep, &cells(&heavy_busy), 2), None, "all busy: bring up the third channel");
    assert_eq!(b.route(&indep, &cells(&heavy[..1]), 1), Some((0, true)));
    b.sibling_backoff_until.store(now_ns() + 1, Ordering::Relaxed);
    assert_eq!(b.route(&indep, &cells(&heavy_busy), 2), Some((0, false)),
        "after a headroom miss the bound channels are used");
    b.sibling_backoff_until.store(0, Ordering::Relaxed);
    let idle_heavy = [channel(10, 0, 0, 2), channel(11, 1, 1, 0)];
    assert_eq!(b.route(&indep, &cells(&idle_heavy), 2), Some((0, true)),
        "an idle primary comes first; only a busy lone channel detours for a sibling");

    // (c) Fanned-out kicks get one monotonic sequence of ordinals and object ids from their
    // shared context, whichever channel publishes them.
    let context = Context::new(70);
    let mut seen = HashSet::new();
    let mut last = 0;
    for kick in 0..64u32 {
        let (ordinal, object_ids) = context.next_compute_identity();
        assert_eq!(ordinal, kick + 1);
        assert!(ordinal > last && seen.insert(object_ids as u32));
        assert_eq!((object_ids >> 32) as u32, last.max(1), "each id names its predecessor");
        last = ordinal;
    }

    // Device-mutex selection and sibling admission follow the same gate.
    let req = ComputeCapacity(16);
    let ctx = Context::new(70);
    // (b) A light primary brings up no sibling though every channel is busy and QIDs are free.
    let reg = registry(vec![channel(10, 0, 3, 0), channel(11, 1, 1, 0)]);
    assert!(!reg.wants_compute_sibling(OWNER, &ctx, 3, req), "a light primary wants no sibling");
    // (c) A sustained heavy primary does.
    let reg = registry(vec![channel(10, 0, 1, 2), channel(11, 1, 1, 0)]);
    assert!(reg.wants_compute_sibling(OWNER, &ctx, 3, req));
    // ... only while every channel is busy.
    let reg = registry(vec![channel(10, 0, 1, 2), channel(11, 1, 0, 0)]);
    assert!(!reg.wants_compute_sibling(OWNER, &ctx, 3, req));
    // ... and two kicks are in flight: one kick behind which the next queues brings up none.
    let reg = registry(vec![channel(10, 0, 1, 2)]);
    assert!(!reg.wants_compute_sibling(OWNER, &ctx, 3, req), "one kick in flight wants no sibling");
    let reg = registry(vec![channel(10, 0, 2, 2)]);
    assert!(reg.wants_compute_sibling(OWNER, &ctx, 3, req), "two kicks in flight do");

    let reg = registry(light.into_iter().collect());
    assert_eq!(reg.select_compute_channel(OWNER, &indep), Some(10), "short kicks take the primary");
    let reg = registry(heavy.into_iter().collect());
    assert_eq!(reg.select_compute_channel(OWNER, &indep), Some(11), "heavy kicks spread");
    let reg = registry(heavy_one.into_iter().collect());
    assert_eq!(reg.select_compute_channel(OWNER, &indep), Some(10),
        "behind a single kick in flight the next one queues on the primary");
    let reg = registry(vec![channel(10, 0, 2, 2), channel(11, 1, 0, 0), channel(12, 2, 0, 0)]);
    assert_eq!(reg.select_compute_channel(OWNER, &sourced), Some(10), "sources take the primary");
    assert_eq!(reg.select_compute_channel(OWNER, &packet(true, Some(3))), Some(10),
        "even when a dependency could stay on another channel");
    // A dependent command still follows the sole active channel (no cross-channel barrier).
    let reg = registry(vec![channel(10, 0, 0, 2), channel(11, 1, 2, 0)]);
    assert_eq!(reg.select_compute_channel(OWNER, &packet(false, Some(3))), Some(11));

    // The threshold itself.
    assert!(!overlaps(0) && !overlaps(1) && overlaps(2) && overlaps(3));

    println!("PASS g17-m5-sibling-gate: sources, short kicks and a kick behind a single one stay on the primary ring, sustained heavy kicks fan out");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    source = build(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-sibling-gate-') as tmp:
        path = Path(tmp) / 'gate.rs'
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
