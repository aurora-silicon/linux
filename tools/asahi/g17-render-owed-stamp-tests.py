#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""G17 render owed-stamp stepping, recovery/retirement guards, and timeout heads.

A render group whose completion stamps lag retires on its pass record and keeps
its descriptor slot in `Pair::active` (`Active::stamps_owed`) until cumulative
stamps cover it or its closed context is killed. The real render retirement
tracker and pollers (`queue/render/retirement.rs`), observation front helpers
(`queue/render/observation.rs`), replay policy (`replay.rs`), pair stamp-wait /
retirement / recovery / drain / timeout-head / failure paths (`queue/render.rs`),
runtime timeout settlement (`runtime/render/settle.rs`), registry timeout
decisions (`runtime/render.rs`), backend timeout deferral (`runtime/backend.rs`),
and timeout attribution policy (`timeout.rs`) are compiled against mock firmware
state.

This verifies that:
- `Pair::timeout_head()` skips `stamps_owed` and `drained_result` entries and
  suppresses quarantined pairs that already executed unstamped or were parked
  out of service (`DRAIN_RETRY_LIMIT`), while still reporting genuinely hung
  in-flight live or quarantined render work (including multi-entry pairs with
  one `stamps_owed` entry and one genuinely hung successor);
- `Pair::note_stamp_wait()`, `Pair::stamp_waiting()`, `Pair::expire_stamp_wait()`,
  `Pair::report_timeout()`, `Pair::observation_packet()`, `Pair::observe_front()`,
  and `Pair::retire_front_command()` (both `retire()` and `retire_observed()`)
  step past leading `stamps_owed` entries so later groups retire cleanly, keep
  their FIFO order until both TA and fragment stamps cover leading held entries,
  record `executed_generation` on stampless retirement too, keep `self.previous`
  held until `note_retirement()` runs on stamp pop at index 0 or in
  `release_witnessed()`, and never run `replay_abandoned()` on a `stamps_owed`
  front;
- `Pair::oldest_spared()`, `Pair::quarantine()`, `Pair::visit_contexts()`,
  `Pair::classify()`, `Pair::pass_started()`, `Pair::note_interrupted()`,
  `Pair::oldest_packet()`, `Pair::front_untouched()`, `Pair::running_start()`,
  and `Pair::live_unparked()` all ignore retired `stamps_owed` entries (and
  `note_interrupted()` returns `Interrupted::None` on an exact-stamp match for a
  `stamps_owed` entry instead of falling through to a successor);
- `Backend::defer_timeout()`, `Registry::render_timeout_decision()`,
  `Firmware::fail_render_packet()`, and `Pair::fail_after_observation()` return
  cleanly (`Decision::Retire` / `None` / `Ok(())` / `Ok(false)`) for groups that
  retired stampless rather than failing them or quarantining running successors.

Each `--mutation` restores one defect or drops one guard and must fail.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi'
G17 = ROOT / 'g17'


def item(source, signature, attributes=False):
    """The item that starts at `signature`, through its closing brace."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    if attributes:
        while True:
            line_start = source.rfind('\n', 0, start - 1) + 1
            if not source[line_start:start].strip().startswith('#['):
                break
            start = line_start
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def line(source, prefix):
    found = [text for text in source.splitlines() if text.strip().startswith(prefix)]
    assert len(found) == 1, prefix
    return found[0]


def fixture():
    retirement_src = (G17 / 'queue/render/retirement.rs').read_text()
    observation_src = (G17 / 'queue/render/observation.rs').read_text()
    render_src = (G17 / 'queue/render.rs').read_text()
    replay_src = (G17 / 'replay.rs').read_text()
    completion_src = (G17 / 'completion.rs').read_text()
    settle_src = (G17 / 'runtime/render/settle.rs').read_text()
    rt_render_src = (G17 / 'runtime/render.rs').read_text()
    backend_src = (G17 / 'runtime/backend.rs').read_text()
    timeout_src = (G17 / 'timeout.rs').read_text()
    depth = re.search(r'pub\(crate\) const RENDER_DEPTH: u8 = (\d+);',
                      (ROOT / 'hw/t8140.rs').read_text()).group(1)

    retired = '\n'.join([
        f'const DEPTH: usize = {depth};',
        line(retirement_src, 'const MAX_GROUP_ITEMS:'),
        item(retirement_src, 'pub(crate) struct Cursors {', True),
        item(retirement_src, 'impl Cursors {'),
        item(retirement_src, 'pub(crate) struct Window {', True),
        item(retirement_src, 'impl Window {'),
        item(retirement_src, 'pub(crate) struct Ticket {', True),
        item(retirement_src, 'pub(crate) struct Snapshot {', True),
        item(retirement_src, 'impl Snapshot {'),
        item(retirement_src, 'pub(crate) struct Observation {', True),
        item(retirement_src, 'pub(crate) fn observe('),
        line(retirement_src, 'pub(crate) const STAMP_FALLBACK_NS:'),
        item(retirement_src, 'pub(crate) enum StampWait {', True),
        item(retirement_src, 'pub(crate) fn stamp_wait('),
        line(retirement_src, 'static STAMPLESS:'),
        item(retirement_src, 'pub(crate) fn note_stampless('),
        item(retirement_src, 'pub(crate) fn poll_stampless('),
        item(retirement_src, 'pub(crate) const fn stamp_covers('),
        item(retirement_src, 'fn visible('),
        item(retirement_src, 'pub(crate) trait Memory {'),
        item(retirement_src, 'pub(crate) enum Poll {', True),
        item(retirement_src, 'pub(crate) fn poll('),
        item(retirement_src, 'pub(crate) fn poll_observed('),
        item(retirement_src, 'fn complete_poll('),
        item(retirement_src, 'pub(crate) fn abandoned('),
        item(retirement_src, 'pub(crate) fn drained('),
        item(retirement_src, 'pub(crate) struct Tracker<T> {'),
        item(retirement_src, 'impl<T> Tracker<T> {'),
    ])

    replay_items = '\n'.join([
        item(replay_src, 'pub(crate) struct Group {', True),
        item(replay_src, 'pub(crate) enum Decision {', True),
        item(replay_src, 'pub(crate) fn decide('),
    ])

    obs_items = '\n'.join([
        item(observation_src, 'pub(crate) struct Observed {').replace('pub(super) ', 'pub(crate) '),
    ])
    obs_methods = '\n'.join(item(observation_src, sig) for sig in [
        'pub(crate) fn observation_packet(',
        'pub(crate) fn observe_front(',
    ])

    pair_methods = '\n'.join(item(render_src, sig) for sig in [
        'pub(crate) fn note_stamp_wait(',
        'fn stamp_wait_expired(',
        'pub(crate) fn stamp_waiting(',
        'pub(crate) fn expire_stamp_wait(',
        'pub(crate) fn report_timeout(',
        'fn selected(',
        'pub(crate) fn timeout_head(',
        'pub(crate) fn contains_packet(',
        'pub(crate) fn retired_held(',
        'pub(crate) fn replay_contains(',
        'pub(crate) fn oldest_packet(',
        'fn holds_only_owed_stamps(',
        'pub(crate) fn oldest_spared(',
        'pub(crate) fn note_interrupted(',
        'pub(crate) fn visit_contexts(',
        'pub(crate) fn classify(',
        'pub(crate) fn pass_started(',
        'pub(crate) fn front_untouched(',
        'fn running(',
        'pub(crate) fn running_start(',
        'pub(crate) fn quarantine(',
        'pub(crate) fn retire(',
        'pub(crate) fn retire_observed(',
        'fn retire_front(',
        'fn retire_front_command(',
        'pub(crate) fn executed_since(',
        'pub(crate) fn live_unparked(',
        'fn replay_abandoned(',
        'fn fail_dropped(',
        'fn settle_interrupted_front(',
        'pub(crate) fn firmware_drained(',
        'fn executed_unstamped(',
        'pub(crate) fn release_witnessed(',
        'pub(crate) fn fail_after_observation(',
        'fn note_retirement(',
        'pub(crate) fn take_retired_binding(',
    ])

    settle_fn = item(settle_src, 'pub(super) fn fail_render_packet(').replace('pub(super) ', '', 1)
    registry_fns = '\n'.join(
        item(rt_render_src, sig).replace('pub(in crate::g17) ', '', 1)
        for sig in [
            'pub(in crate::g17) fn defer_render_timeout(',
            'pub(in crate::g17) fn render_timeout_heads(',
            'pub(in crate::g17) fn render_timeout_decision(',
        ]
    )
    defer_timeout_fn = item(backend_src, '    fn defer_timeout(')

    return r'''
#![allow(warnings)]
use std::cell::{Cell, RefCell, RefMut};
use std::collections::{BTreeMap, VecDeque};
use std::sync::atomic::{fence, Ordering};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(i32);
pub type Result<T = ()> = std::result::Result<T, Error>;
pub const EIO: Error = Error(-5);
pub const EAGAIN: Error = Error(-11);
pub const EINVAL: Error = Error(-22);
pub const EOVERFLOW: Error = Error(-75);
pub const ETIMEDOUT: Error = Error(-110);
pub const ECANCELED: Error = Error(-125);
pub const ENODATA: Error = Error(-61);

thread_local! {
    static NOW: Cell<u64> = Cell::new(1_000_000_000);
    static WARNS: RefCell<Vec<String>> = RefCell::new(Vec::new());
}
pub fn now_ns() -> u64 { NOW.with(|now| now.get()) }
fn advance(ns: u64) { NOW.with(|now| now.set(now.get() + ns)); }
fn take_warns() -> Vec<String> { WARNS.with(|w| std::mem::take(&mut *w.borrow_mut())) }

macro_rules! pr_info { ($($t:tt)*) => {}; }
macro_rules! pr_warn {
    ($($t:tt)*) => {
        crate::WARNS.with(|w| w.borrow_mut().push(format!($($t)*)));
    };
}
macro_rules! dev_info { ($dev:expr, $($t:tt)*) => { let _ = $dev; }; }
macro_rules! dev_warn { ($dev:expr, $($t:tt)*) => { let _ = $dev; }; }

mod debug {
    pub(crate) enum DebugFlags { VerboseFaults }
    pub(crate) fn debug_enabled(_: DebugFlags) -> bool { false }
}

mod module_parameters {
    pub(crate) struct Parameter(pub(crate) u32);
    impl Parameter { pub(crate) fn value(&self) -> u32 { self.0 } }
    pub(crate) static compute_timeout_max_ms: Parameter = Parameter(30_000);
}

mod g17 {
    pub(crate) fn now_ns() -> u64 { crate::now_ns() }
    pub(crate) mod completion {
''' + line(completion_src, 'pub(crate) const MAX_REPLAYS:') + r'''
    }
    pub(crate) mod replay {
''' + replay_items + r'''
    }
    pub(crate) mod timeout {
''' + timeout_src + r'''
    }
    pub(crate) mod recovery {
        pub(crate) fn note_retired() {}
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub(crate) enum Class { Spared, Guilty }
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub(crate) enum Interrupted { Exact, Oldest, None }
        pub(crate) struct Memory;
        impl Memory {
            pub(crate) fn recovery_state(init: &crate::Init) -> crate::Result<u32> {
                Ok(init.recovery_state.get())
            }
        }
    }
    pub(crate) mod trace {
        pub(crate) const RENDER: u8 = 2;
        pub(crate) fn complete(_: u8, _: u8, _: u32, _: &std::sync::Arc<crate::Packet>, _: [u64; 2]) {}
    }
}

mod qos {
    #[derive(Clone, Copy)]
    pub(crate) struct Owner;
}

mod retirement {
    use core::sync::atomic::{fence, Ordering};
    use crate::{Result, EIO, EINVAL, EOVERFLOW};
''' + retired + r'''
}

use g17::recovery;
use retirement::{Cursors, Snapshot, Ticket, Tracker, Window};

''' + line(render_src, 'const ABANDON_GRACE_NS:') + '\n' + line(render_src, 'pub(crate) const DRAIN_RETRY_LIMIT:') + '\n' + obs_items + r'''

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Engine { Compute, Render }

pub struct VmStatus {
    recorded: RefCell<Vec<Error>>,
}
impl VmStatus {
    fn new() -> Arc<Self> { Arc::new(Self { recorded: RefCell::new(Vec::new()) }) }
    fn record(&self, error: Error) { self.recorded.borrow_mut().push(error); }
    fn unobserved_teardown(&self, _: Error) -> bool { false }
}

pub struct WorkState { drained: Cell<bool> }
impl WorkState {
    fn drained(&self) -> Result<bool> { Ok(self.drained.get()) }
}

pub struct Completion {
    status: Arc<VmStatus>,
    slot: Cell<Option<u8>>,
    work: WorkState,
    class: Cell<Option<recovery::Class>>,
    interrupted: Cell<bool>,
    blamed: Cell<bool>,
    replays: Cell<u8>,
    firmware_consumer: Cell<bool>,
    retirement_queued: Cell<bool>,
    deferred_timeout: Cell<Option<Error>>,
    quarantined: Cell<bool>,
    results: RefCell<Vec<Result<[u64; 4]>>>,
}
impl Completion {
    fn new(status: Arc<VmStatus>, slot: u8, drained: bool) -> Arc<Self> {
        Arc::new(Self {
            status,
            slot: Cell::new(Some(slot)),
            work: WorkState { drained: Cell::new(drained) },
            class: Cell::new(None),
            interrupted: Cell::new(false),
            blamed: Cell::new(false),
            replays: Cell::new(0),
            firmware_consumer: Cell::new(false),
            retirement_queued: Cell::new(false),
            deferred_timeout: Cell::new(None),
            quarantined: Cell::new(false),
            results: RefCell::new(Vec::new()),
        })
    }
    fn status(&self) -> &Arc<VmStatus> { &self.status }
    fn render_slot(&self) -> Option<u8> { self.slot.get() }
    fn work_state(&self) -> Result<&WorkState> { Ok(&self.work) }
    fn spared(&self) -> bool { self.class.get() == Some(recovery::Class::Spared) }
    fn guilty(&self) -> bool { self.class.get() == Some(recovery::Class::Guilty) }
    fn class(&self) -> Option<recovery::Class> { self.class.get() }
    fn classify(&self, class: recovery::Class) { self.class.set(Some(class)); }
    fn note_interrupted(&self) { self.interrupted.set(true); }
    fn interrupted(&self) -> bool { self.interrupted.get() }
    fn blamed(&self) -> bool { self.blamed.get() }
    fn replay_count(&self) -> u8 { self.replays.get() }
    fn begin_replay(&self) -> bool {
        let count = self.replays.get();
        if count >= g17::completion::MAX_REPLAYS {
            return false;
        }
        self.replays.set(count + 1);
        true
    }
    fn has_firmware_consumer(&self) -> bool { self.firmware_consumer.get() }
    fn note_retirement_queued(&self) { self.retirement_queued.set(true); }
    fn retirement_queued(&self) -> bool { self.retirement_queued.get() }
    fn defer_timeout(&self, error: Error) {
        if self.deferred_timeout.get().is_none() {
            self.deferred_timeout.set(Some(error));
        }
    }
    fn quarantine_render(&self) { self.quarantined.set(true); }
    fn signal(&self, result: Result<[u64; 4]>) { self.results.borrow_mut().push(result); }
    fn result(&self) -> Option<Result<[u64; 4]>> { self.results.borrow().first().copied() }
    fn signal_count(&self) -> usize { self.results.borrow().len() }
}

pub enum Deferred {
    Retired(Arc<Completion>, Result<[u64; 4]>),
    FailedOwned(Arc<Completion>, Error),
    Drained(Arc<Completion>, Result<[u64; 4]>),
}
impl Deferred {
    fn finish(self) {
        match self {
            Self::Retired(completion, result) => completion.signal(result),
            Self::FailedOwned(completion, error) => completion.signal(Err(error)),
            Self::Drained(completion, result) => completion.signal(result),
        }
    }
}
pub struct DeferredBatch(Vec<Deferred>);
impl DeferredBatch {
    fn new() -> Self { Self(Vec::new()) }
    fn worker() -> Result<Self> { Ok(Self::new()) }
    fn push(&mut self, event: Deferred) -> Result {
        if let Deferred::Retired(ref completion, _) | Deferred::Drained(ref completion, _) = event {
            completion.note_retirement_queued();
        }
        self.0.push(event);
        Ok(())
    }
    fn finish(&mut self) { for event in self.0.drain(..) { event.finish(); } }
}

pub struct Vm(u64);
impl Vm { fn id(&self) -> u64 { self.0 } }
pub struct Context { vm: Vm, id: u32 }
impl Context {
    fn vm(&self) -> &Vm { &self.vm }
    fn id(&self) -> u32 { self.id }
}

pub struct Packet {
    completion: Arc<Completion>,
    context: Arc<Context>,
    allowance: Cell<bool>,
}
impl Packet {
    fn engine(&self) -> Engine { Engine::Render }
    fn check_firmware_producers(&self) -> Result { Ok(()) }
    fn within_allowance(&self, _: u64) -> bool { self.allowance.get() }
    fn reset_publication_for_replay(&self) {}
}

pub struct KickTimestamp;
pub struct Prepared { packet: Arc<Packet> }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RetiredBinding {
    publication: Option<u64>,
    ready: bool,
}

pub struct FreeList {
    id: u16,
    counters: Cell<[u64; 2]>,
}
impl FreeList {
    fn id(&self) -> u16 { self.id }
    fn counters(&self) -> Result<[u64; 2]> { Ok(self.counters.get()) }
}

pub struct Graph { m5: bool }
impl Graph { fn is_m5(&self) -> bool { self.m5 } }

pub struct RenderMemory {
    context: Arc<Context>,
    graph: Graph,
    owned: RefCell<BTreeMap<u64, ()>>,
    retired: RefCell<Vec<u64>>,
    cursors: Cell<[Cursors; 2]>,
    job_list_empty: Cell<bool>,
    passes: RefCell<BTreeMap<u64, [u64; 2]>>,
}
impl RenderMemory {
    fn owns(&self, ordinal: u64) -> bool { self.owned.borrow().contains_key(&ordinal) }
    fn retire(&mut self, ordinal: u64) -> Result {
        if self.owned.borrow_mut().remove(&ordinal).is_none() {
            return Err(EIO);
        }
        self.retired.borrow_mut().push(ordinal);
        Ok(())
    }
    fn pass(&self, ordinal: u64) -> Result<[u64; 2]> {
        Ok(*self.passes.borrow().get(&ordinal).unwrap_or(&[0, 0]))
    }
    fn stage_timestamps(&self, ordinal: u64) -> Result<[u64; 4]> {
        let [start, end] = self.pass(ordinal)?;
        Ok([start, start, start, end])
    }
}

mod observation {
    use super::{RenderMemory, Result, Snapshot, EIO};
    pub(super) fn snapshot(memory: &RenderMemory, ordinal: u64) -> Result<Snapshot> {
        if !memory.owns(ordinal) {
            return Err(EIO);
        }
        Ok(Snapshot {
            cursors: memory.cursors.get(),
            job_list_empty: memory.job_list_empty.get(),
            pass: memory.pass(ordinal)?,
        })
    }
}
use observation::snapshot;

pub trait Host {
    fn epoch(&self) -> Result<(u64, u32)>;
    fn generation(&self) -> u64;
    fn stamps(&self, qids: [u8; 2]) -> Result<[u32; 2]>;
    fn complete_render(
        &mut self,
        owners: Option<[qos::Owner; 2]>,
        pool_id: u16,
        generation: u64,
        buffer_id: u8,
    ) -> Result;
}

pub struct FakeHost {
    generation: u64,
    recovery_state: u32,
    stamps: RefCell<VecDeque<[u32; 2]>>,
    last_stamps: Cell<[u32; 2]>,
    completed_renders: usize,
    fail_complete_render: bool,
    epoch_reads: Cell<usize>,
}
impl FakeHost {
    fn new(initial: [u32; 2]) -> Self {
        Self {
            generation: 1,
            recovery_state: 0,
            stamps: RefCell::new(VecDeque::new()),
            last_stamps: Cell::new(initial),
            completed_renders: 0,
            fail_complete_render: false,
            epoch_reads: Cell::new(0),
        }
    }
    fn set_stamps(&self, stamps: [u32; 2]) {
        self.stamps.borrow_mut().clear();
        self.last_stamps.set(stamps);
    }
    fn queue_stamps(&self, seq: &[[u32; 2]]) {
        self.stamps.borrow_mut().extend(seq.iter().copied());
    }
}
impl Host for FakeHost {
    fn epoch(&self) -> Result<(u64, u32)> {
        self.epoch_reads.set(self.epoch_reads.get() + 1);
        Ok((self.generation, self.recovery_state))
    }
    fn generation(&self) -> u64 { self.generation }
    fn stamps(&self, _qids: [u8; 2]) -> Result<[u32; 2]> {
        if let Some(next) = self.stamps.borrow_mut().pop_front() {
            self.last_stamps.set(next);
        }
        Ok(self.last_stamps.get())
    }
    fn complete_render(&mut self, _: Option<[qos::Owner; 2]>, _: u16, _: u64, _: u8) -> Result {
        if self.fail_complete_render {
            return Err(EIO);
        }
        self.completed_renders += 1;
        Ok(())
    }
}

struct Observer<'a, H> {
    memory: &'a RenderMemory,
    qids: [u8; 2],
    host: &'a mut H,
}
impl<H: Host> retirement::Memory for Observer<'_, H> {
    fn snapshot(&mut self, ordinal: u64) -> Result<Snapshot> {
        observation::snapshot(self.memory, ordinal)
    }
    fn stamps(&mut self) -> Result<[u32; 2]> { self.host.stamps(self.qids) }
    fn pass(&mut self, ordinal: u64) -> Result<[u64; 2]> { self.memory.pass(ordinal) }
    fn stage_timestamps(&mut self, ordinal: u64) -> Result<[u64; 4]> {
        self.memory.stage_timestamps(ordinal)
    }
}

fn completion_noted(qids: [u8; 2], masks: [u64; 2]) -> bool {
    let fragment = usize::from(qids[1]);
    masks[fragment / 64] & (1u64 << (fragment % 64)) != 0
}

''' + item(render_src, 'struct Active {') + r'''

pub struct Pair {
    slot: u8,
    qids: [u8; 2],
    owner: Option<u64>,
    ordinal: u64,
    quarantined: bool,
    guilty_released: bool,
    collateral_at: Option<u64>,
    prepared: Option<Prepared>,
    active: Tracker<Active>,
    replay: Tracker<Arc<Packet>>,
    replay_since: Option<u64>,
    published_completions: Option<[u64; 2]>,
    memory: RenderMemory,
    free_list: FreeList,
    terminal: Option<(Arc<VmStatus>, Error)>,
    executed_generation: Option<u64>,
    drain_failures: u32,
    drain_retry_at: u64,
    context_update: bool,
    identity_update: bool,
    previous: Option<RetiredBinding>,
    displaced: Option<RetiredBinding>,
    context_killed: bool,
    resync_calls: usize,
}

impl Pair {
    fn qids(&self) -> [u8; 2] { self.qids }
    fn qos_owners(&self) -> [qos::Owner; 2] { [qos::Owner, qos::Owner] }
    fn buffer_id(&self) -> u8 { 0 }
    fn release_task_order_if_held(&self) {}
    fn retire_configuration(&mut self, _host: &impl Host) -> Result<bool> { Ok(false) }
    fn take_terminal(&mut self) -> Option<(Arc<VmStatus>, Error)> { self.terminal.take() }
    fn context_killed(&self) -> bool { self.context_killed }
    fn guilty_killed(&self, _generation: u64, _state: u32) -> bool { false }
    fn resync(&mut self) { self.resync_calls += 1; }
    fn published_in_flight(&self) -> bool {
        !self.quarantined && self.active.iter().any(|active| active.outer_published)
    }
''' + obs_methods + '\n' + pair_methods + r'''
}

pub struct BorrowedDeps(Vec<Arc<Packet>>);
impl BorrowedDeps {
    fn contains_packet(&self, packet: &Arc<Packet>) -> bool {
        self.0.iter().any(|p| Arc::ptr_eq(p, packet))
    }
}
pub struct Entry {
    pair: Option<Box<Pair>>,
    preparing: Option<()>,
    growing: bool,
    pending_timeout: bool,
    borrowed_dependencies: Option<BorrowedDeps>,
}
impl Entry {
    fn new(pair: Pair) -> Self {
        Self {
            pair: Some(Box::new(pair)),
            preparing: None,
            growing: false,
            pending_timeout: false,
            borrowed_dependencies: None,
        }
    }
}
pub struct RenderRegistry { entries: Vec<Option<Entry>> }
impl RenderRegistry {
    fn collect_retired(&mut self, _slot: usize) -> Result { Ok(()) }
    fn entry_mut(&mut self, slot: u8) -> Result<&mut Entry> {
        self.entries
            .get_mut(usize::from(slot))
            .and_then(Option::as_mut)
            .ok_or(EIO)
    }
}
pub struct ComputeQueueStub { head: Option<g17::timeout::Head> }
impl ComputeQueueStub {
    fn timeout_head(&self) -> Option<g17::timeout::Head> { self.head }
}
pub struct ComputeEntry { queue: Option<Box<ComputeQueueStub>> }
pub struct Queues {
    render: RenderRegistry,
    compute: Vec<Option<ComputeEntry>>,
    compute_vm_failed: RefCell<Vec<Error>>,
}
impl Queues {
    fn fail_compute_vm(&mut self, _status: &Arc<VmStatus>, error: Error, _deferred: &mut DeferredBatch) -> Result {
        self.compute_vm_failed.borrow_mut().push(error);
        Ok(())
    }
''' + registry_fns + r'''
}
pub struct Recovery { generation: u64 }
impl Recovery { fn generation(&self) -> u64 { self.generation } }
pub struct Init {
    stamps: Vec<core::sync::atomic::AtomicU32>,
    recovery_state: Cell<u32>,
}
impl Init {
    fn stamp(&self, qid: u8) -> Result<&core::sync::atomic::AtomicU32> {
        self.stamps.get(usize::from(qid)).ok_or(EIO)
    }
    fn set_stamps(&self, qids: [u8; 2], values: [u32; 2]) {
        self.stamps[usize::from(qids[0])].store(values[0], core::sync::atomic::Ordering::Release);
        self.stamps[usize::from(qids[1])].store(values[1], core::sync::atomic::Ordering::Release);
    }
}
pub struct Firmware {
    queues: Queues,
    recovery: Recovery,
    init: Init,
    host: FakeHost,
    render_vm_settled: Vec<Error>,
}
impl Firmware {
    fn with_render<R>(&mut self, slot: usize, f: impl FnOnce(&mut Pair, &mut FakeHost) -> Result<R>) -> Result<R> {
        let pair = self.queues.render.entries[slot].as_mut().and_then(|e| e.pair.as_mut()).ok_or(EIO)?;
        f(pair, &mut self.host)
    }
    fn release_render_lists(&mut self) -> Result { Ok(()) }
    fn scan_compute(&mut self, _: Option<[u64; 2]>, _: &mut DeferredBatch) -> Result { Ok(()) }
    fn settle_render_vm(&mut self, _: &Arc<VmStatus>, error: Error, _: &mut DeferredBatch) -> Result {
        self.render_vm_settled.push(error);
        Ok(())
    }
    /// Matches production `Firmware::scan_renders`: latches/checks stamp wait and retires ready groups.
    fn scan_renders(&mut self, deferred: &mut DeferredBatch) -> Result {
        let state = g17::recovery::Memory::recovery_state(&self.init)?;
        let generation = self.recovery.generation();
        for slot in 1..self.queues.render.entries.len() {
            if !self.queues.render.entries[slot].as_ref().is_some_and(|entry| {
                entry.pair.as_ref().is_some_and(|pair| pair.published_in_flight())
                    && entry.preparing.is_none()
                    && !entry.growing
                    && !entry.pending_timeout
            }) {
                continue;
            }
            if let Some(pair) = self.queues.render.entries[slot].as_mut().and_then(|e| e.pair.as_mut()) {
                let qids = pair.qids();
                let stamps = [
                    self.init.stamp(qids[0])?.load(Ordering::Relaxed),
                    self.init.stamp(qids[1])?.load(Ordering::Relaxed),
                ];
                fence(Ordering::Acquire);
                pair.note_stamp_wait(stamps, state, now_ns())?;
            }
            while self.with_render(slot, |pair, host| {
                pair.retire(None, generation, state, host, &mut |event| deferred.push(event))
            })? {}
        }
        Ok(())
    }
''' + settle_fn + r'''
}

pub struct Dev;
impl Dev { fn as_ref(&self) -> &Self { self } }
pub struct State(RefCell<Option<Box<Firmware>>>);
impl State { fn lock(&self) -> RefMut<'_, Option<Box<Firmware>>> { self.0.borrow_mut() } }
pub struct Shared { state: State, dev: Dev }
pub struct Backend { owner: u64, shared: Shared }
impl Backend {
    fn defer_compute_timeout(&self, _: &Arc<Packet>, _: u8, _: u32) -> g17::timeout::Decision {
        unreachable!("render-only harness")
    }
    fn behind_own_head(&self, _: &Arc<Packet>, _: &Init, _: u8) -> Option<(u32, u32)> { None }
''' + defer_timeout_fn + r'''
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::g17::recovery::{Class, Interrupted};
    use crate::g17::timeout::{attribute, Decision, Excuse, Head, MAX_RENEWALS};
    use crate::retirement::STAMP_FALLBACK_NS;

    const SLOT: u8 = 5;
    const QIDS: [u8; 2] = [10, 11];
    const CAP: u32 = 1280;
    const MS: u64 = 1_000_000;

    fn stamp(published: u64) -> u32 { (published as u32) << 8 }
    fn frag_mask(qids: [u8; 2]) -> [u64; 2] {
        let mut masks = [0u64; 2];
        let q = usize::from(qids[1]);
        masks[q / 64] |= 1u64 << (q % 64);
        masks
    }

    fn make_pair(vm_id: u64, m5: bool) -> (Pair, Arc<Context>, Arc<VmStatus>) {
        let ctx = Arc::new(Context { vm: Vm(vm_id), id: (vm_id % 63 + 1) as u32 });
        let status = VmStatus::new();
        let pair = Pair {
            slot: SLOT,
            qids: QIDS,
            owner: Some(1),
            ordinal: 0,
            quarantined: false,
            guilty_released: false,
            collateral_at: None,
            prepared: None,
            active: Tracker::new(),
            replay: Tracker::new(),
            replay_since: None,
            published_completions: None,
            memory: RenderMemory {
                context: ctx.clone(),
                graph: Graph { m5 },
                owned: RefCell::new(BTreeMap::new()),
                retired: RefCell::new(Vec::new()),
                cursors: Cell::new([Cursors { done: 0, read: 0, write: 0 }; 2]),
                job_list_empty: Cell::new(false),
                passes: RefCell::new(BTreeMap::new()),
            },
            free_list: FreeList { id: 1, counters: Cell::new([0, 0]) },
            terminal: None,
            executed_generation: None,
            drain_failures: 0,
            drain_retry_at: 0,
            context_update: false,
            identity_update: false,
            previous: None,
            displaced: None,
            context_killed: false,
            resync_calls: 0,
        };
        (pair, ctx, status)
    }

    fn push_group(
        pair: &mut Pair,
        ctx: &Arc<Context>,
        status: &Arc<VmStatus>,
        ordinal: u64,
        published: u64,
        base: u32,
        drained: bool,
    ) -> Arc<Packet> {
        let packet = Arc::new(Packet {
            completion: Completion::new(status.clone(), pair.slot, drained),
            context: ctx.clone(),
            allowance: Cell::new(false),
        });
        let win = Window::new(base, 2, CAP).unwrap();
        let prev = Cursors { done: base, read: base, write: base };
        let ticket = Ticket { ordinal, windows: [win; 2], previous: [prev; 2] };
        pair.memory.owned.borrow_mut().insert(ordinal, ());
        pair.memory.passes.borrow_mut().insert(ordinal, [0, 0]);
        let next_w = win.target();
        let mut cur = pair.memory.cursors.get();
        for c in &mut cur { c.write = next_w; }
        pair.memory.cursors.set(cur);
        pair.ordinal = pair.ordinal.max(ordinal + 1);
        pair.active.push(Active {
            published,
            qos_completed: false,
            flist_completed: false,
            outer_published: true,
            generation: 1,
            packet: packet.clone(),
            ticket,
            kicks: [KickTimestamp, KickTimestamp],
            context_update: pair.context_update,
            failed: false,
            drained_result: None,
            abandoned_since: None,
            stamp_wait_since: None,
            stamps_owed: false,
        }).ok().unwrap();
        packet
    }

    fn finish_gpu(pair: &Pair, ordinal: u64, pass: [u64; 2], done: u32) {
        pair.memory.passes.borrow_mut().insert(ordinal, pass);
        let mut cur = pair.memory.cursors.get();
        for c in &mut cur {
            c.done = done;
            c.read = done;
        }
        pair.memory.cursors.set(cur);
    }

    fn make_firmware(pair: Pair, stamps: [u32; 2]) -> Firmware {
        let slot = usize::from(pair.slot);
        let mut entries: Vec<Option<Entry>> = (0..16).map(|_| None).collect();
        entries[slot] = Some(Entry::new(pair));
        let init = Init {
            stamps: (0..32).map(|_| core::sync::atomic::AtomicU32::new(0)).collect(),
            recovery_state: Cell::new(0),
        };
        init.set_stamps(QIDS, stamps);
        Firmware {
            queues: Queues {
                render: RenderRegistry { entries },
                compute: Vec::new(),
                compute_vm_failed: RefCell::new(Vec::new()),
            },
            recovery: Recovery { generation: 1 },
            init,
            host: FakeHost::new(stamps),
            render_vm_settled: Vec::new(),
        }
    }

    fn pair_ref(fw: &Firmware) -> &Pair {
        fw.queues.render.entries[usize::from(SLOT)].as_ref().unwrap().pair.as_deref().unwrap()
    }
    fn pair_mut(fw: &mut Firmware) -> &mut Pair {
        fw.queues.render.entries[usize::from(SLOT)].as_mut().unwrap().pair.as_deref_mut().unwrap()
    }

    #[test]
    fn timeout_head_skips_stamps_owed_and_keeps_genuinely_pending_work() {
        let (mut pair, ctx, status) = make_pair(69954, false);
        let _p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        let _p1 = push_group(&mut pair, &ctx, &status, 11, 200, 2, true);
        // Leading group retired stampless: head must be p1 (published=200), not p0 (100).
        pair.active.get_mut(0).unwrap().stamps_owed = true;
        assert_eq!(
            pair.timeout_head().map(|h| (h.vm, h.published, h.failed)),
            Some((69954, 200, false)),
            "live timeout_head must skip leading stamps_owed entry"
        );
        // Same pair quarantined with p1 still genuinely hung: head stays p1 with failed=true.
        pair.quarantined = true;
        assert_eq!(
            pair.timeout_head().map(|h| (h.vm, h.published, h.failed)),
            Some((69954, 200, true)),
            "hung quarantined successor must stay visible as failed head"
        );
        // If p1 is also accounted drained, nothing remains as head (p0 is stamps_owed).
        pair.active.get_mut(1).unwrap().drained_result = Some(Ok([1, 1, 1, 2]));
        assert_eq!(pair.timeout_head(), None, "owed + drained entries produce no head");
        // Live pair holding only owed entries produces no phantom blocker head.
        pair.quarantined = false;
        pair.active.get_mut(1).unwrap().drained_result = None;
        pair.active.get_mut(1).unwrap().stamps_owed = true;
        assert_eq!(pair.timeout_head(), None, "all-owed pair is not a live blocker");
        // Quarantined pair holding only owed entries produces no blocker_failed=1 head.
        pair.quarantined = true;
        assert_eq!(pair.timeout_head(), None, "all-owed quarantined pair leaks no blocker_failed=1");
    }

    #[test]
    fn timeout_head_suppresses_executed_unstamped_and_parked_quarantined_pairs() {
        // Case A: closed-owner quarantined pair whose work already ran unstamped.
        let (mut pair, ctx, status) = make_pair(69954, false);
        let _p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        pair.quarantined = true;
        pair.owner = None;
        pair.free_list.counters.set([4, 4]);
        finish_gpu(&pair, 10, [100, 200], 2);
        assert!(pair.executed_unstamped());
        assert_eq!(pair.timeout_head(), None,
            "executed-unstamped quarantined pair must not leak blocker_failed=1");

        // Cross-VM timeout attribution sees no blocker behind an executed-unstamped pair.
        let own = Head { vm: 69959, published: 200, finished: false, failed: false, excused: false };
        let excuse = Excuse { used: 0, limit: 15 };
        let verdict = attribute(own, 0, excuse, pair.timeout_head().into_iter());
        assert_eq!(verdict.blocker, None);
        assert_eq!(verdict.decision, Decision::Own);

        // Multi-entry executed_unstamped: 1 owed executed group + 1 genuinely hung successor.
        let (mut multi, mctx, mstatus) = make_pair(69955, false);
        let _m0 = push_group(&mut multi, &mctx, &mstatus, 10, 100, 0, true);
        let _m1 = push_group(&mut multi, &mctx, &mstatus, 11, 105, 2, true);
        multi.active.get_mut(0).unwrap().stamps_owed = true;
        multi.quarantined = true;
        multi.owner = None;
        multi.free_list.counters.set([4, 4]);
        finish_gpu(&multi, 10, [100, 200], 4);
        multi.memory.passes.borrow_mut().insert(11, [210, 0]);
        assert!(!multi.executed_unstamped(), "hung successor m1 must keep executed_unstamped false");
        assert_eq!(
            multi.timeout_head().map(|h| (h.vm, h.published, h.failed)),
            Some((69955, 105, true)),
            "1 owed + 1 hung quarantined pair must keep reporting m1 as failed head"
        );
        // Once m1 also finishes on GPU, executed_unstamped becomes true and head disappears.
        finish_gpu(&multi, 11, [210, 300], 4);
        assert!(multi.executed_unstamped());
        assert_eq!(multi.timeout_head(), None);

        // Negative checks: genuinely hung quarantined work is NEVER suppressed!
        // 1. Fragment pass never finished (pass[1] == 0).
        finish_gpu(&pair, 10, [100, 0], 2);
        assert!(!pair.executed_unstamped());
        assert_eq!(pair.timeout_head().map(|h| h.failed), Some(true));
        // 2. Ring entries not yet consumed (done < write).
        finish_gpu(&pair, 10, [100, 200], 1);
        assert!(!pair.executed_unstamped());
        assert_eq!(pair.timeout_head().map(|h| h.failed), Some(true));
        // 3. Free-list blocks still in use.
        finish_gpu(&pair, 10, [100, 200], 2);
        pair.free_list.counters.set([4, 3]);
        assert!(!pair.executed_unstamped());
        assert_eq!(pair.timeout_head().map(|h| h.failed), Some(true));
        // 4. Owner still open (not closed).
        pair.free_list.counters.set([4, 4]);
        pair.owner = Some(9);
        assert!(!pair.executed_unstamped());
        assert_eq!(pair.timeout_head().map(|h| h.failed), Some(true));

        // Case B: quarantined pair parked out of service after DRAIN_RETRY_LIMIT failures.
        pair.drain_failures = DRAIN_RETRY_LIMIT - 1;
        assert_eq!(pair.timeout_head().map(|h| h.failed), Some(true),
            "below DRAIN_RETRY_LIMIT the quarantined pair still reports its head");
        pair.drain_failures = DRAIN_RETRY_LIMIT;
        assert_eq!(pair.timeout_head(), None,
            "parked out-of-service quarantined pair must not leak blocker_failed=1");
        // Live (non-quarantined) pair is never suppressed by the quarantine gate.
        pair.quarantined = false;
        assert_eq!(pair.timeout_head().map(|h| h.failed), Some(false),
            "unquarantined live work is never suppressed");
    }

    #[test]
    fn note_stamp_wait_and_retire_step_past_owed_entries_and_pop_on_late_stamps() {
        let (mut pair, ctx, status) = make_pair(70, false);
        let p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        let p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, true);
        let p2 = push_group(&mut pair, &ctx, &status, 12, 102, 4, true);
        // p0 and p1 executed and consumed; p2 still running (pass[1] == 0).
        finish_gpu(&pair, 10, [100, 200], 6);
        finish_gpu(&pair, 11, [210, 300], 6);
        pair.memory.passes.borrow_mut().insert(12, [310, 0]);
        let lagging = [stamp(10), stamp(10)];
        let masks = frag_mask(QIDS);
        let mut host = FakeHost::new(lagging);
        host.generation = 7;
        let mut batch = DeferredBatch::new();

        // observation_packet rejects lagging stamps, so the scan falls through to note_stamp_wait.
        assert!(pair.observation_packet(masks, lagging, 0).unwrap().is_none());

        // Step 1: p0 latches and expires its stamp wait, retiring stampless.
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        assert!(pair.stamp_waiting());
        advance(STAMP_FALLBACK_NS);
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(true));
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        batch.finish();
        assert_eq!(p0.completion.result(), Some(Ok([100, 100, 100, 200])));
        assert!(pair.retired_held(&p0));
        assert_eq!(pair.executed_generation, Some(7),
            "stampless retirement executed on GPU and must record executed_generation");
        assert!(pair.executed_since(7));
        assert_eq!(pair.active.len(), 3, "p0 keeps its descriptor slot while stamps lag");
        assert!(pair.memory.owns(10));
        assert!(!pair.stamp_waiting(), "cleared owed front must not stay stamp_waiting");

        // Step 2: p1 behind owed p0 latches, sets stamp_waiting, and retires via observe_front + retire_observed!
        assert!(pair.observation_packet(masks, lagging, 0).unwrap().is_none());
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        assert!(pair.stamp_waiting(), "stamp_waiting must see p1 behind owed p0");
        advance(STAMP_FALLBACK_NS);
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(true));
        // Real observe_renders() calls observe_front() (which observes active[0] = p0) then retire_observed().
        let observed = pair.observe_front().expect("observe_front reads active[0]");
        assert_eq!(observed.ticket.ordinal, 10);
        assert_eq!(pair.retire_observed(observed, 0, &mut host, &mut |e| batch.push(e)), Ok(true),
            "retire_observed must step past owed active[0] without Err(EIO)");
        batch.finish();
        assert_eq!(p1.completion.result(), Some(Ok([210, 210, 210, 300])));
        assert!(pair.retired_held(&p1));
        assert_eq!(pair.active.len(), 3, "p0 and p1 both keep their descriptor slots");
        assert!(pair.memory.owns(10) && pair.memory.owns(11));

        // Step 3: p2 still running behind TWO owed entries (p0, p1) does not retire and never probes epoch for replay.
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        assert!(!pair.stamp_waiting());
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(false));
        assert_eq!(host.epoch_reads.get(), 0,
            "pending successor behind owed front must not invoke replay_abandoned");
        assert_eq!(p2.completion.result(), None);
        assert_eq!(pair.timeout_head().map(|h| h.published), Some(102));

        // Step 4: partial stamp (only TA stamp updated, fragment still lagging) releases nothing.
        host.set_stamps([stamp(13), stamp(10)]);
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(false));
        assert_eq!(pair.active.len(), 3);

        // Step 5: p2 finishes while stamps STILL lag -> must step past BOTH owed p0 and p1!
        let p3 = push_group(&mut pair, &ctx, &status, 13, 103, 6, true);
        finish_gpu(&pair, 12, [310, 400], 8);
        pair.memory.passes.borrow_mut().insert(13, [410, 0]);
        host.set_stamps(lagging);
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        advance(STAMP_FALLBACK_NS);
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(true));
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(true),
            "retire must step past multiple consecutive owed entries (p0 and p1) to retire p2");
        batch.finish();
        assert_eq!(p2.completion.result(), Some(Ok([310, 310, 310, 400])));
        assert!(pair.retired_held(&p2));
        assert_eq!(pair.active.len(), 4);

        // Step 6: p3 finishes and cumulative stamps cover all four (stamp(14)).
        finish_gpu(&pair, 13, [410, 500], 8);
        let covered = [stamp(14), stamp(14)];
        assert!(Arc::ptr_eq(&pair.observation_packet(masks, covered, 0).unwrap().unwrap(), &p0),
            "observation_packet selects owed front once cumulative stamps cover it");
        host.set_stamps(covered);
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(pair.active.len(), 3, "p0 popped first");
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(pair.active.len(), 2, "p1 popped second");
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(pair.active.len(), 1, "p2 popped third");
        assert_eq!(pair.retire(None, 7, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        batch.finish();
        assert_eq!(pair.active.len(), 0, "p3 retired and popped fourth");
        assert_eq!(*pair.memory.retired.borrow(), vec![10, 11, 12, 13]);
        assert_eq!(p0.completion.signal_count(), 1, "p0 reported once");
        assert_eq!(p1.completion.signal_count(), 1, "p1 reported once");
        assert_eq!(p2.completion.signal_count(), 1, "p2 reported once");
        assert_eq!(p3.completion.result(), Some(Ok([410, 410, 410, 500])));
        assert_eq!(pair.executed_generation, Some(7));
    }

    #[test]
    fn watchdog_expire_and_fail_render_packet_step_past_owed_and_spare_running_work() {
        let _ = take_warns();
        let (mut pair, ctx, status) = make_pair(71, false);
        let p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, false);
        let p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, false);
        let p2 = push_group(&mut pair, &ctx, &status, 12, 102, 4, true);
        // p0 already retired stampless; p1 finished on GPU while its ioctl node isn't drained;
        // p2 is genuinely still running (pass[1] == 0).
        pair.active.get_mut(0).unwrap().stamps_owed = true;
        pair.active.get_mut(0).unwrap().flist_completed = true;
        finish_gpu(&pair, 10, [100, 200], 6);
        finish_gpu(&pair, 11, [210, 300], 6);
        pair.memory.passes.borrow_mut().insert(12, [310, 0]);
        let lagging = [stamp(10), stamp(10)];

        // Only the oldest unretired group (p1) can have its stamp wait expired.
        assert_eq!(pair.expire_stamp_wait(&p0, lagging, 0), Ok(false), "p0 is already owed");
        assert_eq!(pair.expire_stamp_wait(&p2, lagging, 0), Ok(false), "p2 is behind p1");

        let mut fw = make_firmware(pair, lagging);
        let mut batch = DeferredBatch::new();
        assert_eq!(fw.fail_render_packet(&p1, ETIMEDOUT, &mut batch), Ok(()));
        batch.finish();

        // Real report_timeout must report unretired p1 (ordinal 11) once and exit once p1 is retired_held
        // without looping to report successor p2 (ordinal 12) or owed p0 (ordinal 10).
        let warns = take_warns();
        assert_eq!(warns.len(), 1,
            "fail_render_packet must exit on retired_held(p1) without a second timeout report: {:?}", warns);
        assert!(warns[0].contains("render timeout slot 5 ordinal 11"),
            "real report_timeout must inspect unretired ordinal 11, got: {:?}", warns);

        // p1 retired on its pass record; pair is NOT quarantined; p2 is untouched; VM NOT poisoned!
        assert_eq!(p1.completion.result(), Some(Ok([210, 210, 210, 300])));
        assert_eq!(p2.completion.result(), None, "running successor p2 must not be failed");
        assert!(status.recorded.borrow().is_empty(), "VM must not be poisoned");
        assert!(fw.render_vm_settled.is_empty());
        assert!(!pair_ref(&fw).quarantined, "pair must stay healthy when p1 retired stampless");
        assert!(pair_ref(&fw).retired_held(&p1));

        // Direct guard check on fail_after_observation for an already-owed packet.
        assert_eq!(
            pair_mut(&mut fw).fail_after_observation(&p1, ETIMEDOUT, &mut FakeHost::new(lagging), &mut |e| batch.push(e)),
            Ok(false),
            "fail_after_observation must not quarantine a pair for an already-owed packet"
        );
        assert!(!pair_ref(&fw).quarantined);

        // Now p2 genuinely hangs (pass[1] == 0): its watchdog fails p2 and quarantines the pair,
        // without real replay_abandoned probing firmware/host epoch on the owed front!
        fw.host.epoch_reads.set(0);
        assert_eq!(fw.fail_render_packet(&p2, ETIMEDOUT, &mut batch), Ok(()));
        batch.finish();
        assert_eq!(fw.host.epoch_reads.get(), 0,
            "real replay_abandoned must return early when active.front() is stamps_owed");
        assert!(pair_ref(&fw).quarantined);
        assert_eq!(p2.completion.result(), Some(Err(ETIMEDOUT)));
        assert_eq!(p1.completion.signal_count(), 1, "owed p1 keeps its Ok result");
        assert_eq!(*status.recorded.borrow(), vec![ETIMEDOUT]);
    }

    #[test]
    fn successor_retiring_behind_owed_when_stamps_race_keeps_fifo_order() {
        let (mut pair, ctx, status) = make_pair(72, false);
        let p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        let p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, true);
        pair.active.get_mut(0).unwrap().stamps_owed = true;
        pair.active.get_mut(0).unwrap().flist_completed = true;
        finish_gpu(&pair, 10, [100, 200], 4);
        finish_gpu(&pair, 11, [210, 300], 4);

        // Stamps lag during at=0 poll of p0, then land before at=1 poll of p1!
        let mut host = FakeHost::new([stamp(10), stamp(10)]);
        host.queue_stamps(&[[stamp(10), stamp(10)], [stamp(12), stamp(12)]]);
        let mut batch = DeferredBatch::new();
        assert_eq!(pair.retire(None, 1, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        batch.finish();
        assert_eq!(p1.completion.result(), Some(Ok([210, 210, 210, 300])));
        // Must NOT pop p0 out of order or retire ordinal 11 before ordinal 10!
        assert_eq!(pair.active.len(), 2, "p1 stays behind held p0 until p0 pops");
        assert!(pair.retired_held(&p0) && pair.retired_held(&p1));
        assert!(pair.memory.owns(10) && pair.memory.owns(11));
        assert!(pair.memory.retired.borrow().is_empty());

        // Subsequent polls pop p0 then p1 in strict FIFO order.
        assert_eq!(pair.retire(None, 1, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(pair.retire(None, 1, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(pair.active.len(), 0);
        assert_eq!(*pair.memory.retired.borrow(), vec![10, 11]);
    }

    #[test]
    fn oldest_packet_front_untouched_and_m5_guard_behave_correctly() {
        let (mut pair, ctx, status) = make_pair(73, false);
        let p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        let p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, true);
        pair.active.get_mut(0).unwrap().stamps_owed = true;
        finish_gpu(&pair, 10, [100, 200], 2);
        // p1 has not started yet (pass == [0, 0]).
        assert!(Arc::ptr_eq(&pair.oldest_packet().unwrap(), &p1),
            "oldest_packet must skip leading owed p0");
        assert_eq!(pair.front_untouched(), Some(true),
            "front_untouched must inspect unretired p1, not already-executed owed p0");
        // Once p0 is the only entry, oldest_packet and front_untouched are None.
        let mut all_owed = make_pair(74, false).0;
        let _q0 = push_group(&mut all_owed, &ctx, &status, 20, 200, 0, true);
        all_owed.active.get_mut(0).unwrap().stamps_owed = true;
        assert!(all_owed.oldest_packet().is_none());
        assert_eq!(all_owed.front_untouched(), None);
        let _ = p0;

        // M5 never uses stampless fallback or expire_stamp_wait.
        let (mut m5, m5_ctx, m5_status) = make_pair(75, true);
        let mp = push_group(&mut m5, &m5_ctx, &m5_status, 10, 100, 0, true);
        finish_gpu(&m5, 10, [100, 200], 2);
        assert_eq!(m5.note_stamp_wait([stamp(10), stamp(10)], 0, now_ns()), Ok(false));
        assert_eq!(m5.expire_stamp_wait(&mp, [stamp(10), stamp(10)], 0), Ok(false));
    }

    #[test]
    fn recovery_classification_quarantine_and_interrupted_ignore_stamps_owed() {
        // oldest_spared() must skip a leading spared stamps_owed entry.
        let (mut pair, ctx, status) = make_pair(80, false);
        let p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        let p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, true);
        p0.completion.classify(Class::Spared);
        pair.active.get_mut(0).unwrap().stamps_owed = true;
        assert_eq!(pair.oldest_spared(), Some(false),
            "spared stamps_owed p0 must not exonerate unspared successor p1");

        // classify() must skip stamps_owed entries and classify only unretired work.
        let seen = Cell::new(0usize);
        pair.classify(&|_| { seen.set(seen.get() + 1); Class::Guilty });
        assert_eq!(seen.get(), 1, "classify must not visit stamps_owed p0");
        assert_eq!(p0.completion.class(), Some(Class::Spared),
            "classify must leave stamps_owed p0's class untouched");
        assert_eq!(p1.completion.class(), Some(Class::Guilty),
            "classify must classify unretired p1");

        // All-owed pair has oldest_spared() == None.
        let (mut all_owed, ctx2, status2) = make_pair(81, false);
        let q0 = push_group(&mut all_owed, &ctx2, &status2, 20, 200, 0, true);
        q0.completion.classify(Class::Spared);
        all_owed.active.get_mut(0).unwrap().stamps_owed = true;
        assert_eq!(all_owed.oldest_spared(), None);

        // quarantine() must ignore a leading spared stamps_owed entry so guilty successors
        // still poison the VM.
        let (mut qpair, qctx, qstatus) = make_pair(82, false);
        let qp0 = push_group(&mut qpair, &qctx, &qstatus, 10, 100, 0, true);
        let qp1 = push_group(&mut qpair, &qctx, &qstatus, 11, 101, 2, true);
        qp0.completion.classify(Class::Spared);
        qpair.active.get_mut(0).unwrap().stamps_owed = true;
        let mut batch = DeferredBatch::new();
        qpair.quarantine(ETIMEDOUT, &mut |e| batch.push(e)).unwrap();
        assert!(!qp1.completion.spared(),
            "leading spared stamps_owed p0 must not reclassify unretired p1 as Spared");
        assert!(qpair.terminal.is_some(),
            "unspared p1 must still record terminal VM error on quarantine");

        // quarantine() with an unspared stamps_owed front, a spared successor, and an unspared
        // tail must spare the tail while leaving the retired stamps_owed front untouched.
        let (mut qpair2, qctx2, qstatus2) = make_pair(83, false);
        let rp0 = push_group(&mut qpair2, &qctx2, &qstatus2, 10, 100, 0, true);
        let rp1 = push_group(&mut qpair2, &qctx2, &qstatus2, 11, 101, 2, true);
        let rp2 = push_group(&mut qpair2, &qctx2, &qstatus2, 12, 102, 4, true);
        qpair2.active.get_mut(0).unwrap().stamps_owed = true;
        rp1.completion.classify(Class::Spared);
        qpair2.quarantine(ETIMEDOUT, &mut |e| batch.push(e)).unwrap();
        assert_eq!(rp0.completion.class(), None,
            "quarantine reclassification loop must leave stamps_owed p0 untouched");
        assert!(rp2.completion.spared(), "spared unretired p1 must still spare successor p2");
        assert!(qpair2.terminal.is_none(), "spared unretired entries must not set terminal error");

        // note_interrupted() exact-owed, exact-live, and unmatched fallback branches.
        let (mut ipair, ictx, istatus) = make_pair(84, false);
        let ip0 = push_group(&mut ipair, &ictx, &istatus, 10, 100, 0, true);
        let ip1 = push_group(&mut ipair, &ictx, &istatus, 11, 101, 2, true);
        ipair.active.get_mut(0).unwrap().stamps_owed = true;

        // Exact stamp of stamps_owed p0 returns Interrupted::None without marking p0 or falling
        // through to successor p1.
        assert_eq!(ipair.note_interrupted(stamp(11)), Interrupted::None,
            "exact-stamp match on stamps_owed p0 must return Interrupted::None");
        assert!(!ip0.completion.interrupted(), "retired stamps_owed p0 must not be marked interrupted");
        assert!(!ip1.completion.interrupted(),
            "exact-stamp match on stamps_owed p0 must not fall through to mark successor p1");

        // Unmatched stamp falls back to oldest unretired p1 (not stamps_owed p0).
        assert_eq!(ipair.note_interrupted(stamp(99)), Interrupted::Oldest,
            "unmatched stamp must fall back to oldest unretired published entry");
        assert!(!ip0.completion.interrupted(), "fallback must skip stamps_owed p0");
        assert!(ip1.completion.interrupted(), "fallback must mark unretired p1");

        // Exact stamp of live p1 returns Interrupted::Exact.
        ip1.completion.interrupted.set(false);
        assert_eq!(ipair.note_interrupted(stamp(12)), Interrupted::Exact);
        assert!(ip1.completion.interrupted());

        // All-owed pair returns Interrupted::None on unmatched stamp too.
        assert_eq!(all_owed.note_interrupted(stamp(99)), Interrupted::None);
    }

    #[test]
    fn previous_binding_stays_held_until_stamp_pop_or_release_witnessed() {
        // Case A: stampless retirement + successor behind owed keep previous binding held until stamp pops p0.
        let (mut pair, ctx, status) = make_pair(90, false);
        pair.context_update = true;
        pair.identity_update = true;
        pair.previous = Some(RetiredBinding { publication: Some(10), ready: false });
        let _p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        let _p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, true);
        finish_gpu(&pair, 10, [100, 200], 4);
        finish_gpu(&pair, 11, [210, 300], 4);
        let lagging = [stamp(10), stamp(10)];
        let mut host = FakeHost::new(lagging);
        host.generation = 4;
        let mut batch = DeferredBatch::new();

        // p0 retires stampless -> previous MUST remain held and context_update stays true!
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        advance(STAMP_FALLBACK_NS);
        assert_eq!(pair.note_stamp_wait(lagging, 0, now_ns()), Ok(true));
        assert_eq!(pair.retire(None, 4, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        batch.finish();
        assert!(pair.take_retired_binding().is_none(),
            "stampless retirement must NOT mark self.previous ready");
        assert!(pair.context_update && pair.identity_update,
            "stampless retirement must NOT clear context_update/identity_update");

        // p1 retires behind owed p0 (even when stamps race in for p1) -> previous STILL held!
        host.queue_stamps(&[[stamp(10), stamp(10)], [stamp(12), stamp(12)]]);
        assert_eq!(pair.retire(None, 4, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        batch.finish();
        assert!(pair.take_retired_binding().is_none(),
            "retirement with index at != 0 behind owed p0 must NOT mark self.previous ready");

        // Late stamps pop p0 when index at == 0 -> NOW real note_retirement marks self.previous ready!
        host.set_stamps([stamp(12), stamp(12)]);
        assert_eq!(pair.retire(None, 4, 0, &mut host, &mut |e| batch.push(e)), Ok(true));
        assert!(!pair.context_update && !pair.identity_update);
        assert_eq!(
            pair.take_retired_binding(),
            Some(RetiredBinding { publication: Some(10), ready: true }),
            "popping leading stamps_owed entry on stamps must invoke real note_retirement"
        );

        // Case B: fallible host.complete_render on normal !stampless at == 0 must NOT mark previous ready!
        let (mut fpair, fctx, fstatus) = make_pair(91, false);
        fpair.context_update = true;
        fpair.identity_update = true;
        fpair.previous = Some(RetiredBinding { publication: Some(10), ready: false });
        let _fp0 = push_group(&mut fpair, &fctx, &fstatus, 10, 100, 0, true);
        finish_gpu(&fpair, 10, [100, 200], 2);
        let mut fhost = FakeHost::new([stamp(11), stamp(11)]);
        fhost.fail_complete_render = true;
        assert_eq!(fpair.retire(None, 1, 0, &mut fhost, &mut |e| batch.push(e)), Err(EIO));
        assert!(fpair.take_retired_binding().is_none(),
            "failed complete_render must not have called note_retirement");
        fhost.fail_complete_render = false;
        assert_eq!(fpair.retire(None, 1, 0, &mut fhost, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(
            fpair.take_retired_binding(),
            Some(RetiredBinding { publication: Some(10), ready: true }),
            "successful !stampless at == 0 pop marks self.previous ready"
        );

        // Case C: release_witnessed draining a closed executed_unstamped+context_killed stamps_owed pair
        // must invoke note_retirement so self.previous is released!
        let (mut dpair, dctx, dstatus) = make_pair(92, false);
        dpair.context_update = true;
        dpair.identity_update = true;
        dpair.previous = Some(RetiredBinding { publication: Some(10), ready: false });
        let _dp0 = push_group(&mut dpair, &dctx, &dstatus, 10, 100, 0, true);
        finish_gpu(&dpair, 10, [100, 200], 2);
        let mut dhost = FakeHost::new(lagging);
        assert_eq!(dpair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        advance(STAMP_FALLBACK_NS);
        assert_eq!(dpair.note_stamp_wait(lagging, 0, now_ns()), Ok(true));
        assert_eq!(dpair.retire(None, 1, 0, &mut dhost, &mut |e| batch.push(e)), Ok(true));
        batch.finish();
        assert!(dpair.take_retired_binding().is_none());

        dpair.owner = None;
        dpair.free_list.counters.set([2, 2]);
        dpair.context_killed = true;
        assert_eq!(dpair.firmware_drained(&dhost), Ok(true));
        assert_eq!(dpair.release_witnessed(&dhost, &mut |e| batch.push(e)), Ok(true));
        assert_eq!(
            dpair.take_retired_binding(),
            Some(RetiredBinding { publication: Some(10), ready: true }),
            "release_witnessed must call note_retirement for drained stamps_owed entries"
        );
    }

    #[test]
    fn live_unparked_and_defer_timeout_handle_stamps_owed_cleanly() {
        // live_unparked() must ignore stamps_owed entries.
        let (mut pair, ctx, status) = make_pair(95, false);
        let p0 = push_group(&mut pair, &ctx, &status, 10, 100, 0, true);
        assert!(pair.live_unparked(), "unretired active entry is live_unparked");
        pair.active.get_mut(0).unwrap().stamps_owed = true;
        assert!(!pair.live_unparked(),
            "pair holding only stamps_owed entries must not stall compute replay");
        let p1 = push_group(&mut pair, &ctx, &status, 11, 101, 2, true);
        assert!(pair.live_unparked(), "unretired successor behind owed front IS live_unparked");
        let _ = (p0, p1);

        // Backend::defer_timeout() returns Decision::Retire cleanly when scan_renders retires
        // the packet stampless on its final renewal (renewals == MAX_RENEWALS), and
        // Registry::render_timeout_decision() skips retired_held packets.
        let (mut tpair, tctx, tstatus) = make_pair(96, false);
        let tp0 = push_group(&mut tpair, &tctx, &tstatus, 10, 100, 0, true);
        let tp1 = push_group(&mut tpair, &tctx, &tstatus, 11, 200, 2, true);
        finish_gpu(&tpair, 10, [100, 200], 4);
        tpair.memory.passes.borrow_mut().insert(11, [210, 0]);
        let lagging = [stamp(10), stamp(10)];
        // Latch tp0's stamp wait and advance past STAMP_FALLBACK_NS so scan_renders retires it stampless.
        assert_eq!(tpair.note_stamp_wait(lagging, 0, now_ns()), Ok(false));
        advance(STAMP_FALLBACK_NS);

        let mut fw = make_firmware(tpair, lagging);
        // Add an older live blocker of another VM (vm=99, published=150) between tp0 (100) and tp1 (200).
        fw.queues.compute.push(Some(ComputeEntry {
            queue: Some(Box::new(ComputeQueueStub {
                head: Some(Head { vm: 99, published: 150, finished: false, failed: false, excused: false }),
            })),
        }));

        let backend = Backend {
            owner: 1,
            shared: Shared { state: State(RefCell::new(Some(Box::new(fw)))), dev: Dev },
        };
        // On final renewal (MAX_RENEWALS), scan_renders retires tp0 stampless -> Decision::Retire!
        let decision = backend.defer_timeout(&tp0, MAX_RENEWALS, 0);
        assert_eq!(decision, Decision::Retire,
            "defer_timeout must return Decision::Retire when scan_renders retired the packet");
        assert_eq!(tp0.completion.result(), Some(Ok([100, 100, 100, 200])),
            "defer_timeout must finish its deferred batch before returning Decision::Retire");
        assert!(!tp0.completion.spared(), "retired packet must not be classified Spared");
        assert!(tp1.completion.result().is_none(), "running successor tp1 stays untouched");

        // Direct check on Registry::render_timeout_decision for retired_held tp0:
        // Must return None rather than attributing tp1's newer head (200) against blocker (150)!
        let state = backend.shared.state.lock();
        let fw_ref = (*state).as_deref().unwrap();
        let excuse = Excuse { used: 0, limit: 15 };
        assert_eq!(
            fw_ref.queues.render_timeout_decision(&tp0, MAX_RENEWALS, excuse),
            None,
            "render_timeout_decision must skip retired_held packet tp0"
        );
        // Unretired tp1 (published=200) still sees older blocker (published=150) -> Decision::Spare!
        assert_eq!(
            fw_ref.queues.render_timeout_decision(&tp1, MAX_RENEWALS, excuse).map(|v| v.decision),
            Some(Decision::Spare),
            "render_timeout_decision still attributes unretired tp1 normally"
        );
    }

    #[test]
    fn recovery_context_pass_started_and_running_start_ignore_stamps_owed() {
        // An all-stamps_owed pair must be omitted from visit_contexts(), pass_started(),
        // and running_start() so recovery attribution cannot add its QIDs (whose
        // oldest_spared() is None) or blame an already-completed pass.
        let (mut owed_only, ctx, status) = make_pair(97, false);
        let _p0 = push_group(&mut owed_only, &ctx, &status, 10, 100, 0, true);
        owed_only.active.get_mut(0).unwrap().stamps_owed = true;
        finish_gpu(&owed_only, 10, [100, 200], 2);

        let mut contexts = Vec::new();
        owed_only.visit_contexts(&mut |id| contexts.push(id));
        assert!(contexts.is_empty(),
            "all-stamps_owed pair must visit no contexts, got {:?}", contexts);
        assert_eq!(owed_only.pass_started(11), None,
            "pass_started must ignore retired stamps_owed ordinal 10");

        // Defense-in-depth: even if pass[1] were unwritten on a stamps_owed entry,
        // running_start() must still skip it.
        owed_only.memory.passes.borrow_mut().insert(10, [100, 0]);
        assert!(owed_only.running_start().is_none(),
            "running_start must ignore stamps_owed entry even with pass[1] == 0");

        // A mixed [owed, live] pair skips the owed front while still visiting the live
        // successor's context, reporting its pass_started state, and reporting its running start.
        let (mut mixed, mctx, mstatus) = make_pair(98, false);
        let _mp0 = push_group(&mut mixed, &mctx, &mstatus, 20, 200, 0, true);
        let _mp1 = push_group(&mut mixed, &mctx, &mstatus, 21, 201, 2, true);
        mixed.active.get_mut(0).unwrap().stamps_owed = true;
        finish_gpu(&mixed, 20, [100, 200], 4);
        mixed.memory.passes.borrow_mut().insert(21, [0, 0]);

        let mut mixed_contexts = Vec::new();
        mixed.visit_contexts(&mut |id| mixed_contexts.push(id));
        assert_eq!(mixed_contexts, vec![mctx.id()],
            "mixed [owed, live] pair must visit only live successor's context once");
        assert_eq!(mixed.pass_started(21), None,
            "pass_started must ignore leading stamps_owed ordinal 20");
        assert_eq!(mixed.pass_started(22), Some(false),
            "pass_started must report unstarted live successor ordinal 21");
        assert!(mixed.running_start().is_none(),
            "unstarted live successor is not yet running");

        // Once live successor starts tiling (pass[0] != 0, pass[1] == 0), both pass_started
        // and running_start report it while still ignoring the owed front.
        mixed.memory.passes.borrow_mut().insert(20, [100, 0]);
        mixed.memory.passes.borrow_mut().insert(21, [210, 0]);
        assert_eq!(mixed.pass_started(21), None,
            "pass_started must keep ignoring stamps_owed ordinal 20");
        assert_eq!(mixed.pass_started(22), Some(true),
            "pass_started must report started live successor ordinal 21");
        let (owner, start) = mixed.running_start().expect("started live successor has a running start");
        assert!(Arc::ptr_eq(owner, &mstatus));
        assert_eq!(start, 210);
    }
}
'''


MUTATIONS = {
    'head-includes-owed': [
        ('.find(|active| !active.stamps_owed && active.drained_result.is_none())',
         '.find(|active| active.drained_result.is_none())'),
    ],
    'head-includes-executed': [
        ('&& (self.drain_failures >= DRAIN_RETRY_LIMIT || self.executed_unstamped())',
         '&& self.drain_failures >= DRAIN_RETRY_LIMIT'),
    ],
    'head-includes-parked': [
        ('&& (self.drain_failures >= DRAIN_RETRY_LIMIT || self.executed_unstamped())',
         '&& self.executed_unstamped()'),
    ],
    'head-suppresses-live': [
        ('if self.quarantined\n            && (self.drain_failures >= DRAIN_RETRY_LIMIT || self.executed_unstamped())',
         'if self.drain_failures >= DRAIN_RETRY_LIMIT || self.executed_unstamped()'),
    ],
    'note-front-only': [
        ('        let Some(front) = self.active.iter_mut().find(|active| !active.stamps_owed) else {\n'
         '            return Ok(false);\n        };',
         '        let Some(front) = self.active.front_mut() else {\n'
         '            return Ok(false);\n        };'),
    ],
    'waiting-front-only': [
        ('pub(crate) fn stamp_waiting(&self) -> bool {\n        self.active\n            .iter()\n'
         '            .find(|active| !active.stamps_owed)\n            .is_some_and(|front| front.stamp_wait_since.is_some())',
         'pub(crate) fn stamp_waiting(&self) -> bool {\n        self.active\n            .front()\n'
         '            .is_some_and(|front| front.stamp_wait_since.is_some())'),
    ],
    'expire-front-only': [
        ('        if !self\n            .active\n            .iter()\n            .find(|active| !active.stamps_owed)\n'
         '            .is_some_and(|front| Arc::ptr_eq(&front.packet, packet))',
         '        if !self\n            .active\n            .front()\n'
         '            .is_some_and(|front| Arc::ptr_eq(&front.packet, packet))'),
        ('        if let Some(front) = self.active.iter_mut().find(|active| !active.stamps_owed) {',
         '        if let Some(front) = self.active.front_mut() {'),
    ],
    'report-timeout-front-only': [
        ('        let Some(front) = self\n            .active\n            .iter()\n'
         '            .find(|active| !active.stamps_owed)\n            .or_else(|| self.active.front())',
         '        let Some(front) = self.active.front()'),
    ],
    'retire-no-step-past': [
        ('                    Ok(retirement::Poll::Pending) | Ok(retirement::Poll::Retry) => {\n'
         '                        at += 1;\n                        continue;\n                    }',
         '                    Ok(retirement::Poll::Pending) | Ok(retirement::Poll::Retry) => {\n'
         '                        return Ok(false);\n                    }'),
    ],
    'retire-no-step-second-owed': [
        ('                if at != 0 {\n                    at += 1;\n                    continue;\n                }',
         '                if at != 0 {\n                    return Ok(false);\n                }'),
    ],
    'retire-pops-behind-owed': [
        ('        if stampless || at != 0 {', '        if stampless {'),
    ],
    'retire-observed-rejects-step': [
        ('            if at == 0 && observed.as_ref().is_some_and(|observed| observed.ticket != front.ticket)',
         '            if observed.as_ref().is_some_and(|observed| observed.ticket != front.ticket)'),
    ],
    'replay-abandoned-owed-front': [
        ('        if self.quarantined\n            || self.active.front().is_none_or(|front| front.stamps_owed)\n'
         '            || self.replay.len() != 0',
         '        if self.quarantined\n            || self.active.len() == 0\n'
         '            || self.replay.len() != 0'),
    ],
    'settle-fails-retired': [
        ('            if pair.replay_contains(packet)\n                || !pair.contains_packet(packet)\n'
         '                || pair.retired_held(packet)\n            {',
         '            if pair.replay_contains(packet) || !pair.contains_packet(packet) {'),
    ],
    'fail-after-obs-includes-owed': [
        ('        if !self\n            .active\n            .iter()\n'
         '            .any(|a| !a.stamps_owed && Arc::ptr_eq(&a.packet, packet))\n        {',
         '        if !self.active.iter().any(|a| Arc::ptr_eq(&a.packet, packet)) {'),
    ],
    'oldest-includes-owed': [
        ('pub(crate) fn oldest_packet(&self) -> Option<Arc<Packet>> {\n        self.active\n'
         '            .iter()\n            .find(|active| !active.stamps_owed)\n'
         '            .map(|active| active.packet.clone())',
         'pub(crate) fn oldest_packet(&self) -> Option<Arc<Packet>> {\n'
         '        self.active.front().map(|active| active.packet.clone())'),
    ],
    'untouched-includes-owed': [
        ('pub(crate) fn front_untouched(&self) -> Option<bool> {\n        let front = self\n'
         '            .active\n            .iter()\n'
         '            .find(|active| active.outer_published && !active.stamps_owed)?;',
         'pub(crate) fn front_untouched(&self) -> Option<bool> {\n        let front = self\n'
         '            .active\n            .iter()\n'
         '            .find(|active| active.outer_published)?;'),
    ],
    'oldest-spared-includes-owed': [
        ('pub(crate) fn oldest_spared(&self) -> Option<bool> {\n        self.active\n'
         '            .iter()\n            .find(|active| !active.stamps_owed)\n'
         '            .map(|active| active.packet.completion.spared())',
         'pub(crate) fn oldest_spared(&self) -> Option<bool> {\n        self.active\n'
         '            .front()\n'
         '            .map(|active| active.packet.completion.spared())'),
    ],
    'quarantine-spared-check-includes-owed': [
        ('.any(|active| !active.stamps_owed && active.packet.completion.spared())',
         '.any(|active| active.packet.completion.spared())'),
    ],
    'quarantine-reclassify-includes-owed': [
        ('            for active in self.active.iter().filter(|active| !active.stamps_owed) {\n'
         '                active.packet.completion.classify(recovery::Class::Spared);',
         '            for active in self.active.iter() {\n'
         '                active.packet.completion.classify(recovery::Class::Spared);'),
    ],
    'classify-includes-owed': [
        ('pub(crate) fn classify(&self, classify: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {\n'
         '        for active in self.active.iter().filter(|active| !active.stamps_owed) {',
         'pub(crate) fn classify(&self, classify: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {\n'
         '        for active in self.active.iter() {'),
    ],
    'interrupted-exact-marks-owed': [
        ('            if active.stamps_owed {\n                return recovery::Interrupted::None;\n'
         '            }\n            active.packet.completion.note_interrupted();\n'
         '            return recovery::Interrupted::Exact;',
         '            active.packet.completion.note_interrupted();\n'
         '            return recovery::Interrupted::Exact;'),
    ],
    'interrupted-exact-falls-through': [
        ('            .filter(|active| active.outer_published)\n'
         '            .find(|active| (active.ticket.ordinal.wrapping_add(1) as u32) & 0xff_ffff == sequence)',
         '            .filter(|active| active.outer_published && !active.stamps_owed)\n'
         '            .find(|active| (active.ticket.ordinal.wrapping_add(1) as u32) & 0xff_ffff == sequence)'),
    ],
    'interrupted-fallback-includes-owed': [
        ('        match self\n            .active\n            .iter()\n'
         '            .find(|active| active.outer_published && !active.stamps_owed)\n        {',
         '        match self.active.iter().find(|active| active.outer_published) {'),
    ],
    'note-retirement-on-stampless': [
        ('        if stampless {\n            retirement::note_stampless(u16::from(self.slot), ordinal);\n'
         '        }\n        if !front.flist_completed {',
         '        if stampless {\n            retirement::note_stampless(u16::from(self.slot), ordinal);\n'
         '        }\n        self.note_retirement(ordinal, context_update);\n'
         '        let front = self.active.get_mut(at).ok_or(EIO)?;\n        if !front.flist_completed {'),
    ],
    'note-retirement-missing-owed-pop': [
        ('                    Ok(retirement::Poll::Complete(_)) => {\n'
         '                        self.memory.retire(ordinal)?;\n'
         '                        self.active.pop().ok_or(EIO)?;\n'
         '                        self.note_retirement(ordinal, context_update);\n'
         '                        return Ok(true);',
         '                    Ok(retirement::Poll::Complete(_)) => {\n'
         '                        self.memory.retire(ordinal)?;\n'
         '                        self.active.pop().ok_or(EIO)?;\n'
         '                        return Ok(true);'),
    ],
    'note-retirement-missing-release-witnessed': [
        ('            if owed {\n                self.note_retirement(ordinal, context_update);\n            }',
         '            let _ = (owed, context_update);'),
    ],
    'stampless-skips-executed-generation': [
        ('            front.stamps_owed = true;\n'
         '            // Work of this pair retires: a later blame on it is collateral again.\n'
         '            self.collateral_at = None;\n'
         '            self.executed_generation = Some(host.generation());\n'
         '            return Ok(true);',
         '            front.stamps_owed = true;\n'
         '            self.collateral_at = None;\n'
         '            if !stampless {\n'
         '                self.executed_generation = Some(host.generation());\n            }\n'
         '            return Ok(true);'),
    ],
    'live-unparked-includes-owed': [
        ('pub(crate) fn live_unparked(&self) -> bool {\n'
         '        self.replay.len() == 0 && self.active.iter().any(|active| !active.stamps_owed)',
         'pub(crate) fn live_unparked(&self) -> bool {\n'
         '        self.replay.len() == 0 && self.active.len() != 0'),
    ],
    'defer-timeout-ignores-retirement-queued': [
        ('            if packet.completion.retirement_queued() {\n'
         '                return Ok(Decision::Retire);\n            }\n',
         ''),
    ],
    'render-timeout-decision-includes-owed': [
        ('.find(|pair| pair.contains_packet(packet) && !pair.retired_held(packet))',
         '.find(|pair| pair.contains_packet(packet))'),
    ],
    'visit-contexts-includes-owed': [
        ('pub(crate) fn visit_contexts(&self, visit: &mut dyn FnMut(u32)) {\n'
         '        for active in self.active.iter().filter(|active| !active.stamps_owed) {',
         'pub(crate) fn visit_contexts(&self, visit: &mut dyn FnMut(u32)) {\n'
         '        for active in self.active.iter() {'),
    ],
    'pass-started-includes-owed': [
        ('        let ordinal = self\n            .active\n            .iter()\n'
         '            .filter(|active| !active.stamps_owed)\n'
         '            .map(|active| active.ticket.ordinal)',
         '        let ordinal = self\n            .active\n            .iter()\n'
         '            .map(|active| active.ticket.ordinal)'),
    ],
    'running-owner-includes-owed': [
        ('                active.outer_published\n'
         '                    && !active.stamps_owed\n'
         '                    && !active.packet.completion.interrupted()',
         '                active.outer_published\n'
         '                    && !active.packet.completion.interrupted()'),
    ],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    code = fixture()
    for old, new in MUTATIONS.get(args.mutation, []):
        assert code.count(old) == 1, (args.mutation, old)
        code = code.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='g17-render-owed-') as tmp:
        path = Path(tmp) / 'render_owed.rs'
        binary = path.with_suffix('')
        path.write_text(code)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '--test', str(path),
                        '-o', str(binary)], check=True)
        # A killed mutant exits with the failed test's panic in its output.
        result = subprocess.run([str(binary)])
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
