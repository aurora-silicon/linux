#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: a lent render pair's completions are observed by the borrower that returns it.

The direct render path lends an owner's pair out of the registry under the device mutex,
prepares and publishes into it without that mutex, and takes it back under the mutex again.
While it is lent, event passes skip it and record the completion for its borrower
(`missed_event`). This compiles the production code that keeps such a completion from waiting
for the end of the owner's run of publications against host stubs:

- `retain_render_completions()`, `render_needs_rescan()`, `take_missed_event()`, `take_pair()`
  and `return_pair()` from drivers/gpu/drm/asahi/g17/runtime/render.rs;
- `next_render_observation()`, `next_render_observation_in()`, `try_fast_observe_render()`,
  `observe_returned_render()` and `fast_observe_renders()` from runtime/render/observe.rs,
  with the production `FastRenderOutcome` and `completion_noted()`.

It checks that

a. an event pass that meets only completions owed to borrowers neither asks for a rescan nor
   falls back to the multi-hold slow path, while a borrowed pair whose completion was noted
   before it was lent still does;
b. the borrower's observation of the slot it returned retires every completion that arrived
   during the loan (also several), in order, exactly once, touches no other slot, reports a
   rescan while published work stays in flight, and leaves everything to the event worker in
   any state the worker's fast path would not handle (another slot's error, timeout, close or
   growth, a replay, recovery, a fault, no room in the collector);
c. with a publisher, an event worker and the GPU running on three threads, every command
   retires exactly once and in order, and the publisher retires completions on return;
d. structurally, that `render_direct()` returns the missed flag from `return_render()`,
   observes in the hold that put the pair back only with a live preparation lease and no
   recovery, takes the collector with `try_lock()`, finishes it after the device mutex and
   otherwise requests the event pass or, for a rescan, the poll; and that `runtime_event_turn()`
   ends in its first hold a pass whose render completions are owed to borrowers or that leaves
   only a rescan, arming the poll for it.

Each mutation restores the old behaviour or drops one part of the rule, and must fail an
assertion of the compiled harness.
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


# name -> [(file, old, new)]: each must apply exactly once.
MUTATIONS = {
    # (a) The old rescan rule: a completion owed to the borrower still asks for a rescan.
    'rescan-owed': [('render', '                        && entry.borrowed_active\n                        && !entry.missed_event,',
                     '                        && entry.borrowed_active,')],
    # (b) The returned slot's observation scans every slot, or ignores the steady-state gate.
    'observe-all-slots': [('observe', 'self.fast_observe_renders(slot, slot + 1, deferred)',
                           'self.fast_observe_renders(1, self.queues.render.entries.len(), deferred)')],
    'observe-ignores-gate': [('observe', '            if entry.pending_error.is_some()\n                || entry.pending_timeout\n'
                              '                || entry.closed\n                || entry.growing\n',
                              '            if false\n')],
    # (b) The old loop: it stops at the first retirement that leaves work in flight, or calls that
    # a fallback.
    'rescan-in-loop-bail': [('observe', '                || self.render_grow_ready()\n            {\n                return Ok(FastRenderOutcome::Fallback);\n            }\n'
                             '            // Work still in flight',
                             '                || self.render_grow_ready()\n                || self.queues.render_needs_rescan()\n            {\n'
                             '                return Ok(FastRenderOutcome::Fallback);\n            }\n            // Work still in flight')],
    'rescan-is-fallback': [('observe', '        if self.queues.render_needs_rescan() {\n            return Ok(FastRenderOutcome::Rescan);\n        }',
                            '        if self.queues.render_needs_rescan() {\n            return Ok(FastRenderOutcome::Fallback);\n        }'),
                           ('observe', '                return Ok(if self.queues.render_needs_rescan() {\n                    FastRenderOutcome::Rescan',
                            '                return Ok(if self.queues.render_needs_rescan() {\n                    FastRenderOutcome::Fallback')],
    # (d) Structural: the borrower drops the missed flag, observes without the guards, blocks on the
    # collector, signals under the device mutex, never requests the pass, or the event pass
    # takes the slow path for owed completions.
    'missed-dropped': [('direct', '        Ok(missed)\n    }', '        Ok(false)\n    }')],
    'observe-in-recovery': [('direct', '                            if !recovery_started\n                                && lease.is_current()\n'
                             '                                && !self.shared.crashed.load(Ordering::Acquire)\n                            {\n',
                             '                            if true {\n')],
    'collector-blocks': [('direct', 'collector = self.shared.render_collector.try_lock();',
                          'collector = Some(self.shared.render_collector.lock());')],
    'finish-under-mutex': [('direct', '                    drop(state);\n                    // Fences signal off the device mutex, before another borrower can collect.\n'
                            '                    if let Some(batch) = collector.as_deref_mut() {\n                        batch.finish();\n                    }\n',
                            '                    if let Some(batch) = collector.as_deref_mut() {\n                        batch.finish();\n                    }\n'
                            '                    drop(state);\n')],
    'no-pass-requested': [('direct', 'FastRenderOutcome::Fallback => self.shared.queue_event_records(),',
                           'FastRenderOutcome::Fallback => {}')],
    'rescan-no-poll': [('direct', 'FastRenderOutcome::Rescan => self.shared.queue_poll(),', 'FastRenderOutcome::Rescan => {}')],
    'worker-rescan-slow-path': [('events', '                            rescan = true;\n                            true\n',
                                 '                            rescan = true;\n                            false\n')],
    'owed-slow-path': [('events', 'if render_clean && (compute_scan.is_some() || render_changed || masks != [0; 2]) {',
                        'if render_clean && (compute_scan.is_some() || render_changed) {')],
}


def build(mutation=None):
    files = {
        'render': (G17 / 'runtime/render.rs').read_text(),
        'observe': (G17 / 'runtime/render/observe.rs').read_text(),
        'direct': (G17 / 'runtime/render/direct.rs').read_text(),
        'events': (G17 / 'runtime/events.rs').read_text(),
        'queue': (G17 / 'queue/render.rs').read_text(),
    }
    for name, old, new in MUTATIONS.get(mutation, []):
        assert files[name].count(old) == 1, (mutation, old)
        files[name] = files[name].replace(old, new)
    render, observe, direct, events, queue = (files[k] for k in ('render', 'observe', 'direct', 'events', 'queue'))

    def host(code):
        return (code.replace('pub(in crate::g17::runtime) ', '').replace('pub(in crate::g17) ', '')
                .replace('pub(super) ', '').replace('pub(crate) ', '')
                .replace('crate::g17::recovery::Memory::recovery_state', 'Memory::recovery_state')
                .replace('crate::g17::queue::render::completion_noted', 'completion_noted')
                .replace('crate::hw::t8140::queues::RENDER_SLOTS', 'RENDER_SLOTS')
                .replace('crate::g17::now_ns()', 'now_ns()').replace('kick::Id::qid', 'Id::qid')
                .replace('KBox<Pair>', 'Box<Pair>'))

    outcome = host(item(observe, 'pub(in crate::g17::runtime) enum FastRenderOutcome'))
    noted = host(item(queue, 'pub(crate) fn completion_noted('))
    state_fns = host('\n'.join(item(render, signature) for signature in [
        '    fn entry(&self, slot: u8)', '    fn entry_mut(&mut self, slot: u8)', 'fn take_missed_event(',
        'fn take_pair(&mut self, slot: u8', 'fn return_pair(&mut self, slot: u8',
    ]))
    registry_fns = host('\n'.join(item(render, signature) for signature in [
        'pub(in crate::g17) fn render_needs_rescan(', 'pub(in crate::g17) fn retain_render_completions(',
    ]))
    firmware_fns = host('\n'.join(item(observe, signature) for signature in [
        'fn next_render_observation(&mut self', 'fn next_render_observation_in(',
        'pub(in crate::g17::runtime) fn try_fast_observe_render(', 'pub(in crate::g17::runtime) fn observe_returned_render(',
        'fn fast_observe_renders(',
    ]))

    render_direct = item(direct, 'pub(in crate::g17::runtime) fn render_direct(')
    return_fn = item(direct, 'fn return_render(')
    turn = item(events, 'fn runtime_event_turn(')
    structure = {
        'return_render reports a completion missed during the loan': ordered_ok(
            return_fn, ') -> Result<bool> {', 'self.queues.render.return_pair(slot, &mut loan.pair)?;',
            'let missed = self.queues.render.take_missed_event(slot);', 'Ok(missed)')
            and 'queue_event_records' not in return_fn,
        'render_direct observes in the returning hold, signals after it': ordered_ok(
            render_direct, 'let mut collector = None;', 'let mut state = self.shared.state.lock();',
            'let returned = firmware.return_render(slot, &mut loan, activation, packet);',
            'let returned = match returned {', 'Ok(true) => {',
            'if !recovery_started\n                                && lease.is_current()\n'
            '                                && !self.shared.crashed.load(Ordering::Acquire)\n                            {',
            'collector = self.shared.render_collector.try_lock();',
            'Some(batch) => firmware.observe_returned_render(slot, batch),',
            'None => Ok(FastRenderOutcome::Fallback),',
            'FastRenderOutcome::Fallback => self.shared.queue_event_records(),',
            'FastRenderOutcome::Rescan => self.shared.queue_poll(),',
            'FastRenderOutcome::Done => {', 'if !firmware.queues.render_in_flight() {', 'self.shared.queue_idle(0);', 'FastRenderOutcome::Idle => {}', 'Ok(false) => Ok(()),',
            'Err(error) => Err(error),', 'drop(state);', 'batch.finish();', 'drop(collector);',
            'self.shared.note_changed();', 'drop(lease);', 'if let Err(error) = returned {',
            'self.shared.lose_device_quietly();'),
        'the collector is never waited for': 'render_collector.lock()' not in render_direct,
        'an event pass whose completions are owed, or that leaves a rescan, ends in its first hold':
            ordered_ok(turn, 'let mut rescan = false;', 'firmware.try_fast_observe_render(deferred)?',
                       'FastRenderOutcome::Rescan => {', 'rescan = true;\n                            true\n',
                       'if render_clean && (compute_scan.is_some() || render_changed || masks != [0; 2]) {',
                       'rescan || scan.polling || firmware.non_compute_runtime_polling(now);',
                       'if polling {\n                            self.queue_poll();', 'fast_done = true;'),
    }
    checks = '\n'.join(f'    assert!({str(ok).lower()}, "structure: {name}");' for name, ok in structure.items())
    return (TEMPLATE.replace('@@OUTCOME@@', outcome).replace('@@NOTED@@', noted).replace('@@STATE@@', state_fns)
            .replace('@@REGISTRY@@', registry_fns).replace('@@FIRMWARE@@', firmware_fns)
            .replace('@@STRUCTURE@@', checks))


TEMPLATE = r'''
#![allow(dead_code, unused_variables, unused_mut, unused_imports, unreachable_code)]
use std::collections::VecDeque;
use std::sync::atomic::{fence, AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

const EIO: i32 = -5;
const EAGAIN: i32 = -11;
const EBUSY: i32 = -16;
const EINVAL: i32 = -22;
type Result<T = ()> = std::result::Result<T, i32>;
const RENDER_SLOTS: usize = 64;
fn now_ns() -> u64 { 1 }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
@@OUTCOME@@

@@NOTED@@

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Id(u8);
impl Id { fn qid(self) -> u8 { self.0 } }
pub struct Context;
pub struct Packet { context: Arc<Context>, ordinal: u64 }
pub struct VmStatus;
#[derive(Clone, Copy)]
pub struct Dependencies;
#[derive(Clone, Copy)]
struct Reservation { slot: u8, ids: [Id; 2], owner: u64 }

/// The production completion record a pair retires: the command's ordinal.
#[derive(Debug, PartialEq, Eq)]
pub enum Deferred { Retired(u64), Failed(u64) }
pub struct DeferredBatch { values: Vec<Deferred>, cap: usize }
impl DeferredBatch {
    fn new(cap: usize) -> Self { Self { values: Vec::with_capacity(cap), cap } }
    fn push(&mut self, value: Deferred) -> Result {
        if self.values.len() == self.cap { return Err(EIO); }
        self.values.push(value);
        Ok(())
    }
    fn has_room(&self, need: usize) -> bool { self.cap - self.values.len() >= need }
    fn pending(&self) -> usize { self.values.len() }
    fn finish(&mut self) -> Vec<Deferred> { std::mem::take(&mut self.values) }
}

pub struct Observed { ordinal: u64 }
pub struct Host;
/// A render pair: its published commands in order and the fragment stamp the GPU writes.
pub struct Pair {
    slot: u8, qids: [u8; 2], owner: u64, active: VecDeque<u64>, retired: Vec<u64>,
    quarantined: bool, terminal: Option<(Arc<VmStatus>, i32)>, fault_at: Option<u64>,
}
impl Pair {
    fn slot(&self) -> u8 { self.slot }
    fn qids(&self) -> [u8; 2] { self.qids }
    fn owner(&self) -> Option<u64> { Some(self.owner) }
    fn matches(&self, owner: u64, _: &Arc<Context>) -> Result<bool> { Ok(owner == self.owner) }
    fn published_in_flight(&self) -> bool { !self.quarantined && !self.active.is_empty() }
    fn in_flight(&self) -> bool { !self.active.is_empty() }
    fn stamp_waiting(&self) -> bool { false }
    fn dependency_snapshot(&self) -> Dependencies { Dependencies }
    fn switch_in_flight(&self) -> Option<u64> { None }
    /// The front is done once both stamps cover it.
    fn observation_packet(&self, _masks: [u64; 2], stamps: [u32; 2], state: u32) -> Result<Option<u64>> {
        if self.quarantined || state != 0 { return Ok(None); }
        Ok(self.active.front().copied().filter(|&ordinal| stamps.iter().all(|&s| u64::from(s) > ordinal)))
    }
    fn note_stamp_wait(&mut self, _: [u32; 2], _: u32, _: u64) -> Result<bool> { Ok(false) }
    fn observe_front(&self) -> Result<Observed> { Ok(Observed { ordinal: *self.active.front().ok_or(EIO)? }) }
    fn retire_observed(&mut self, observed: Observed, _: u32, _: &mut Host,
                       defer: &mut impl FnMut(Deferred) -> Result) -> Result<bool> {
        if self.active.front() != Some(&observed.ordinal) { return Err(EIO); }
        if self.fault_at == Some(observed.ordinal) {
            self.quarantined = true;
            self.terminal = Some((Arc::new(VmStatus), EIO));
            for &ordinal in self.active.iter() { defer(Deferred::Failed(ordinal))?; }
            self.active.clear();
            return Ok(true);
        }
        self.active.pop_front();
        self.retired.push(observed.ordinal);
        defer(Deferred::Retired(observed.ordinal))?;
        Ok(true)
    }
    fn take_terminal(&mut self) -> Option<(Arc<VmStatus>, i32)> { self.terminal.take() }
}

struct Entry {
    reservation: Reservation, pair: Option<Box<Pair>>, preparing: Option<Arc<Packet>>,
    borrowed_active: bool, borrowed_dependencies: Option<Dependencies>, borrowed_switch: Option<u64>,
    event_masks: [u64; 2], missed_event: bool, closed: bool,
    pending_error: Option<(Arc<VmStatus>, i32)>, pending_timeout: bool, growing: bool,
}
pub struct RenderState { entries: Vec<Option<Entry>>, replay: bool }
impl RenderState {
@@STATE@@
    fn replay_parked(&self) -> bool { self.replay }
    fn collect_retired(&mut self, _slot: usize) -> Result { Ok(()) }
}
pub struct Registry { render: RenderState }
impl Registry {
@@REGISTRY@@
}
struct Recovery { pending: bool }
impl Recovery { fn pending(&self) -> bool { self.pending } }
struct Init { stamps: Vec<AtomicU32>, recovery_state: AtomicU32 }
impl Init { fn stamp(&self, qid: u8) -> Result<&AtomicU32> { self.stamps.get(usize::from(qid)).ok_or(EIO) } }
struct Memory;
impl Memory { fn recovery_state(init: &Init) -> Result<u32> { Ok(init.recovery_state.load(Ordering::Relaxed)) } }
struct SharedStub { event_records: AtomicUsize }
impl SharedStub { fn queue_event_records(&self) { self.event_records.fetch_add(1, Ordering::Relaxed); } }
struct StateStub { shared: SharedStub }
struct Primary { state: StateStub }
pub struct Firmware { recovery: Recovery, queues: Registry, init: Init, primary: Primary, grow: bool, settled: Vec<usize> }
impl Firmware {
@@FIRMWARE@@
    fn render_grow_pending(&self) -> bool { self.grow }
    fn render_grow_ready(&self) -> bool { false }
    fn release_render_lists(&mut self) -> Result { Ok(()) }
    fn with_render<R>(&mut self, slot: usize, operation: impl FnOnce(&mut Pair, &mut Host) -> Result<R>) -> Result<R> {
        let pair = self.queues.render.entries[slot].as_mut().and_then(|entry| entry.pair.as_mut()).ok_or(EIO)?;
        operation(pair, &mut Host)
    }
    /// The production settlement fans a terminal out to its VM; here it is recorded.
    fn settle_render_terminal(&mut self, slot: usize, _deferred: &mut DeferredBatch) -> Result<bool> {
        let terminal = self.queues.render.entries[slot].as_mut().and_then(|entry| entry.pair.as_mut())
            .and_then(|pair| pair.take_terminal());
        if terminal.is_some() { self.settled.push(slot); }
        Ok(terminal.is_some())
    }
}

const OWNER: u64 = 3;
const FRAGMENT_QID: u8 = 21;

fn pair(slot: u8, qids: [u8; 2], ordinals: &[u64]) -> Box<Pair> {
    Box::new(Pair { slot, qids, owner: OWNER, active: ordinals.iter().copied().collect(), retired: Vec::new(),
        quarantined: false, terminal: None, fault_at: None })
}
fn entry(slot: u8, qids: [u8; 2], ordinals: &[u64]) -> Entry {
    Entry { reservation: Reservation { slot, ids: [Id(qids[0]), Id(qids[1])], owner: OWNER },
        pair: Some(pair(slot, qids, ordinals)), preparing: None, borrowed_active: false,
        borrowed_dependencies: None, borrowed_switch: None, event_masks: [0; 2], missed_event: false,
        closed: false, pending_error: None, pending_timeout: false, growing: false }
}
fn firmware(entries: Vec<Option<Entry>>) -> Firmware {
    Firmware { recovery: Recovery { pending: false }, queues: Registry { render: RenderState { entries, replay: false } },
        init: Init { stamps: (0..128).map(|_| AtomicU32::new(0)).collect(), recovery_state: AtomicU32::new(0) },
        primary: Primary { state: StateStub { shared: SharedStub { event_records: AtomicUsize::new(0) } } },
        grow: false, settled: Vec::new() }
}
/// The GPU finished every command below `through` on `qids`.
fn complete(fw: &Firmware, qids: [u8; 2], through: u32) {
    for qid in qids { fw.init.stamp(qid).unwrap().store(through, Ordering::Release); }
}
fn mask(qid: u8) -> [u64; 2] {
    let mut masks = [0u64; 2];
    masks[usize::from(qid) / 64] |= 1u64 << (qid % 64);
    masks
}
fn retired(batch: &mut DeferredBatch) -> Vec<u64> {
    batch.finish().into_iter().map(|d| match d { Deferred::Retired(o) => o, Deferred::Failed(o) => 1000 + o }).collect()
}
fn packet(ordinal: u64) -> Arc<Packet> { Arc::new(Packet { context: Arc::new(Context), ordinal }) }
/// The direct path's loan: the production take_pair(), a publication, the production return_pair().
fn lend(fw: &mut Firmware, slot: u8, ordinal: u64) -> Box<Pair> {
    fw.queues.render.take_pair(slot, OWNER, &packet(ordinal)).unwrap()
}
fn put_back(fw: &mut Firmware, slot: u8, mut lent: Box<Pair>) -> bool {
    let mut lent = Some(lent);
    fw.queues.render.return_pair(slot, &mut lent).unwrap();
    fw.queues.render.take_missed_event(slot)
}

fn main() {
@@STRUCTURE@@

    let qids = [20u8, FRAGMENT_QID];
    // (a, b) One owner publishing a run of passes into slot 1 while the GPU finishes earlier ones.
    let mut fw = firmware(vec![None, Some(entry(1, qids, &[0, 1, 2]))]);
    let mut worker = DeferredBatch::new(64);
    let mut collector = DeferredBatch::new(64);
    let mut next = 3u64;
    for done in 1..=3u32 {
        let mut lent = lend(&mut fw, 1, next);
        lent.active.push_back(next);
        next += 1;
        // The GPU finishes the front while the pair is lent; the event pass skips it and records
        // the completion for its borrower.
        complete(&fw, qids, done);
        let matched = fw.queues.retain_render_completions(mask(FRAGMENT_QID));
        assert_eq!(matched, mask(FRAGMENT_QID));
        assert!(!fw.queues.render_needs_rescan(), "a completion owed to the borrower needs no rescan");
        assert_eq!(fw.try_fast_observe_render(&mut worker), Ok(FastRenderOutcome::Idle),
            "the event pass ends in its first hold");
        assert!(retired(&mut worker).is_empty());
        // The return observes it in the same hold: retired, in order; later passes stay in flight.
        assert!(put_back(&mut fw, 1, lent), "the completion is owed");
        assert_eq!(fw.observe_returned_render(1, &mut collector), Ok(FastRenderOutcome::Rescan));
        assert_eq!(retired(&mut collector), vec![u64::from(done) - 1], "retired by the return of its loan");
    }
    // Two completions during one loan are both retired by its return.
    let mut lent = lend(&mut fw, 1, next);
    lent.active.push_back(next);
    complete(&fw, qids, 5);
    fw.queues.retain_render_completions(mask(FRAGMENT_QID));
    assert!(put_back(&mut fw, 1, lent));
    assert_eq!(fw.observe_returned_render(1, &mut collector), Ok(FastRenderOutcome::Rescan));
    assert_eq!(retired(&mut collector), vec![3, 4]);
    // The GPU drains: the event worker retires the rest and clears the masks.
    complete(&fw, qids, 7);
    fw.queues.retain_render_completions(mask(FRAGMENT_QID));
    assert_eq!(fw.try_fast_observe_render(&mut worker), Ok(FastRenderOutcome::Done));
    assert_eq!(retired(&mut worker), vec![5, 6]);
    assert_eq!(fw.queues.render.entries[1].as_ref().unwrap().event_masks, [0; 2]);
    assert_eq!(fw.queues.render.entries[1].as_ref().unwrap().pair.as_ref().unwrap().retired, (0..7).collect::<Vec<_>>());
    // Nothing in flight, nothing missed.
    let lent = lend(&mut fw, 1, 7);
    assert!(!put_back(&mut fw, 1, lent));
    assert_eq!(fw.observe_returned_render(1, &mut collector), Ok(FastRenderOutcome::Idle));

    // (a) A completion noted before the loan (no missed record) still asks for a rescan.
    let mut fw = firmware(vec![None, Some(entry(1, qids, &[0]))]);
    fw.queues.retain_render_completions(mask(FRAGMENT_QID));
    let lent = lend(&mut fw, 1, 1);
    assert!(fw.queues.render_needs_rescan(), "noted before the loan: the rescan stays");
    assert_eq!(fw.try_fast_observe_render(&mut worker), Ok(FastRenderOutcome::Rescan));
    put_back(&mut fw, 1, lent);

    // (b) The returned slot only: another owner's ready pair in slot 2 stays with the worker.
    let other = [30u8, 31];
    let mut fw = firmware(vec![None, Some(entry(1, qids, &[0])), Some(entry(2, other, &[0]))]);
    let lent = lend(&mut fw, 1, 1);
    complete(&fw, qids, 1);
    complete(&fw, other, 1);
    fw.queues.retain_render_completions(mask(FRAGMENT_QID));
    fw.queues.retain_render_completions(mask(31));
    assert!(put_back(&mut fw, 1, lent));
    assert_eq!(fw.observe_returned_render(1, &mut collector), Ok(FastRenderOutcome::Rescan));
    assert_eq!(retired(&mut collector), vec![0], "only the returned slot is observed");
    assert_eq!(fw.queues.render.entries[2].as_ref().unwrap().pair.as_ref().unwrap().active.len(), 1,
        "the other slot is left to the event worker");
    assert_eq!(fw.try_fast_observe_render(&mut worker), Ok(FastRenderOutcome::Done));
    assert_eq!(retired(&mut worker), vec![0]);

    // (b) Every state the worker's fast path leaves to its full pass is left to it here too.
    for case in 0..7 {
        let mut fw = firmware(vec![None, Some(entry(1, qids, &[0, 1])), Some(entry(2, other, &[]))]);
        let lent = lend(&mut fw, 1, 2);
        complete(&fw, qids, 2);
        fw.queues.retain_render_completions(mask(FRAGMENT_QID));
        assert!(put_back(&mut fw, 1, lent));
        let mut room = 64;
        {
            let e2 = fw.queues.render.entries[2].as_mut().unwrap();
            match case {
                0 => e2.pending_error = Some((Arc::new(VmStatus), EIO)),
                1 => e2.pending_timeout = true,
                2 => e2.closed = true,
                3 => e2.growing = true,
                4 => fw.queues.render.replay = true,
                5 => fw.recovery.pending = true,
                _ => room = 1,
            }
        }
        let mut small = DeferredBatch::new(room);
        assert_eq!(fw.observe_returned_render(1, &mut small), Ok(FastRenderOutcome::Fallback), "case {} falls back", case);
        assert!(retired(&mut small).is_empty(), "case {} retires nothing", case);
    }
    // A fault is settled in the same hold, then left to the worker's full pass.
    let mut fw = firmware(vec![None, Some(entry(1, qids, &[0, 1]))]);
    let lent = lend(&mut fw, 1, 2);
    complete(&fw, qids, 1);
    fw.queues.render.entries[1].as_mut().unwrap().preparing.as_ref().unwrap();
    fw.queues.retain_render_completions(mask(FRAGMENT_QID));
    let mut lent = lent;
    lent.fault_at = Some(0);
    assert!(put_back(&mut fw, 1, lent));
    assert_eq!(fw.observe_returned_render(1, &mut collector), Ok(FastRenderOutcome::Fallback));
    assert_eq!(fw.settled, vec![1], "the terminal is settled, not dropped");
    assert_eq!(retired(&mut collector), vec![1000, 1001]);

    // (c) Three threads: a publisher lending and returning slot 1, the event worker, the GPU.
    const PASSES: u64 = 4000;
    let fw = Arc::new(Mutex::new(firmware(vec![None, Some(entry(1, qids, &[]))])));
    let collector = Arc::new(Mutex::new(DeferredBatch::new(256)));
    let published = Arc::new(AtomicU64::new(0));
    let events = Arc::new(AtomicU64::new(0));
    let stop = Arc::new(AtomicBool::new(false));
    let by_return = Arc::new(AtomicU64::new(0));
    let gpu_done = Arc::new(AtomicU64::new(0));
    let worker_seen = Arc::new(AtomicU64::new(0));
    let signalled = Arc::new(Mutex::new(Vec::<u64>::new()));
    let gpu = {
        let (fw, published, events, stop, gpu_done) =
            (fw.clone(), published.clone(), events.clone(), stop.clone(), gpu_done.clone());
        std::thread::spawn(move || {
            let mut done = 0u64;
            while !stop.load(Ordering::Acquire) || done < published.load(Ordering::Acquire) {
                let target = published.load(Ordering::Acquire);
                if done < target {
                    done += 1;
                    complete(&fw.lock().unwrap(), qids, done as u32);
                    gpu_done.store(done, Ordering::Release);
                    events.fetch_add(1, Ordering::Release);
                }
                std::thread::yield_now();
            }
        })
    };
    let worker = {
        let (fw, events, stop, signalled, published, worker_seen) =
            (fw.clone(), events.clone(), stop.clone(), signalled.clone(), published.clone(), worker_seen.clone());
        std::thread::spawn(move || {
            let mut seen = 0u64;
            let mut batch = DeferredBatch::new(256);
            loop {
                let pending = events.load(Ordering::Acquire);
                let requested = {
                    let fw = fw.lock().unwrap();
                    fw.primary.state.shared.event_records.swap(0, Ordering::AcqRel)
                };
                if pending == seen && requested == 0 {
                    if stop.load(Ordering::Acquire) {
                        let fw = fw.lock().unwrap();
                        let e = fw.queues.render.entries[1].as_ref().unwrap();
                        if e.pair.as_ref().is_some_and(|p| p.active.is_empty()) { break; }
                        drop(fw);
                        events.fetch_add(1, Ordering::Release);
                    }
                    std::thread::yield_now();
                    continue;
                }
                seen = pending;
                // One hold of the pass; its fences signal after it, as the full pass's
                // observe_renders() does per hold, and a ready observation left behind (a
                // full collector) gets the next hold.
                let again = {
                    let mut fw = fw.lock().unwrap();
                    fw.queues.retain_render_completions(mask(FRAGMENT_QID));
                    match fw.try_fast_observe_render(&mut batch).unwrap() {
                        FastRenderOutcome::Fallback => fw.next_render_observation(1).unwrap().is_some(),
                        // The rescan poll requests another pass.
                        FastRenderOutcome::Rescan => {
                            fw.primary.state.shared.queue_event_records();
                            false
                        }
                        _ => false,
                    }
                };
                if again {
                    events.fetch_add(1, Ordering::Release);
                }
                worker_seen.store(seen, Ordering::Release);
                signalled.lock().unwrap().extend(retired(&mut batch));
            }
        })
    };
    {
        let (fw, collector, published, signalled, by_return) =
            (fw.clone(), collector.clone(), published.clone(), signalled.clone(), by_return.clone());
        for ordinal in 0..PASSES {
            let mut lent = lend(&mut fw.lock().unwrap(), 1, ordinal);
            lent.active.push_back(ordinal);
            published.store(ordinal + 1, Ordering::Release);
            for _ in 0..(ordinal % 7) { std::thread::yield_now(); }
            if ordinal % 16 == 0 {
                // Keep this loan until the GPU finished its command and the event pass met the
                // lent pair: the return then owes, and makes, that observation.
                while gpu_done.load(Ordering::Acquire) <= ordinal { std::thread::yield_now(); }
                let event = events.load(Ordering::Acquire);
                while worker_seen.load(Ordering::Acquire) < event { std::thread::yield_now(); }
            }
            let mut held = None;
            {
                let mut fw = fw.lock().unwrap();
                if put_back(&mut fw, 1, lent) {
                    held = collector.try_lock().ok();
                    let outcome = match held.as_deref_mut() {
                        Some(batch) => fw.observe_returned_render(1, batch).unwrap(),
                        None => FastRenderOutcome::Fallback,
                    };
                    // Fallback requests the pass, Rescan the poll that requests it.
                    if matches!(outcome, FastRenderOutcome::Fallback | FastRenderOutcome::Rescan) {
                        fw.primary.state.shared.queue_event_records();
                    }
                }
            }
            if let Some(batch) = held.as_deref_mut() {
                let done = retired(batch);
                by_return.fetch_add(done.len() as u64, Ordering::Relaxed);
                signalled.lock().unwrap().extend(done);
            }
        }
    }
    stop.store(true, Ordering::Release);
    gpu.join().unwrap();
    worker.join().unwrap();
    let fw = fw.lock().unwrap();
    let pair = fw.queues.render.entries[1].as_ref().unwrap().pair.as_ref().unwrap();
    assert_eq!(pair.retired, (0..PASSES).collect::<Vec<_>>(), "every command retires once, in order");
    let mut all = signalled.lock().unwrap().clone();
    all.sort_unstable();
    assert_eq!(all, (0..PASSES).collect::<Vec<_>>(), "every completion is signalled exactly once");
    assert!(by_return.load(Ordering::Relaxed) > 0, "returns retire what arrived during their loans");

    println!("PASS g17-render-return-observe: completions of a lent pair are retired by its return");
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    source = build(args.mutation)
    with tempfile.TemporaryDirectory(prefix='g17-render-return-') as tmp:
        path = Path(tmp) / 'observe.rs'
        binary = path.with_suffix('')
        path.write_text(source)
        built = subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '-O', str(path),
                                '-o', str(binary)], capture_output=True, text=True)
        if built.returncode:
            sys.stderr.write(built.stderr)
            return built.returncode
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=600)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
