#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute production G16 inline submit retirement and EP 0x20 poll-step logic."""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

production = kernel_root() / 'drivers/gpu/drm/asahi/g16/poll.rs'
queue_source = (kernel_root() / 'drivers/gpu/drm/asahi/g16/queue.rs').read_text()
event_source = (kernel_root() / 'drivers/gpu/drm/asahi/g16/event.rs').read_text()
mod_source = (kernel_root() / 'drivers/gpu/drm/asahi/g16/mod.rs').read_text()
sleep_source = (kernel_root() / 'drivers/gpu/drm/asahi/g16/sleep.rs').read_text()
core_source = (kernel_root() / 'drivers/gpu/drm/asahi/asahi_core.rs').read_text()
source = production.read_text()
output = output_dir()

assert 'const EVENT_RETIRE_POLLS: u32 = 11;' in source, 'EVENT_RETIRE_POLLS expected to be 11'
assert 'const ACTIVE_HOUSEKEEPING_NS: u64 = 5_000_000;' in source, 'ACTIVE_HOUSEKEEPING_NS expected to be 5 ms'
assert 'const MAX_EVENT_RECHECKS: u32 = 8;' in source, 'MAX_EVENT_RECHECKS expected to be 8'
assert 'rechecks < MAX_EVENT_RECHECKS' in source, 'run_poll must loop-recheck evented completions'
assert 'self.idle_resnap();' in source, 'run_poll idle paths must use idle_resnap()'
assert 'g16_repoll_us: u32 {\n            default: 8,' in core_source, 'g16_repoll_us default expected to be 8'
assert (
    'OwnedQueue::new_percpu_reclaim(c_str!("aurora_g16_events"), true)' in mod_source
), 'aurora_g16_events must use new_percpu_reclaim(..., true)'
assert (
    'pub(crate) usage: Arc<crate::usage::Client>' in queue_source
), 'QueueState::usage must reference the per-file usage::Client'
assert (
    '.add_union(engine, start, end, super::hw::COMMAND_TIMESTAMP_HZ as u32)' in source
), 'G16 poll retirement must deduplicate per-file intervals via Client::add_union'
assert (
    'OwnedQueue::new_unbound_reclaim(c_str!("aurora_g16_events_unbound"), true)' in mod_source
), 'aurora_g16_events_unbound must use new_unbound_reclaim(..., true)'
assert (
    'self.events_unbound.drain();' in sleep_source
), 'sleep.rs must drain events_unbound on suspend'
assert (
    'self.cancel_resnap();' in sleep_source
), 'sleep.rs must still call unconditional cancel_resnap()'


def item(declaration, text=source):
    start = text.index(declaration)
    at = text.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


harness = r'''
#![allow(dead_code, unused_variables)]
use std::cell::Cell;
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct Error(i32);
type Result<T = ()> = std::result::Result<T, Error>;

const HOUSEKEEPING_NS: u64 = 1_000_000;
const ACTIVE_HOUSEKEEPING_NS: u64 = 5_000_000;
const AUX_DRAIN_NS: u64 = 10_000_000;
const EVENT_RETIRE_POLLS: u32 = 11;
mod hw {
    pub mod sizes {
        pub const TA_WAVE_PASSES: usize = 4;
        pub const TA_WAVE_PASSES_LIGHT: usize = 8;
        pub const TA_WAVE_REFILL_HEADROOM: usize = 2;
        pub const COMPUTE_PIPES: usize = 16;
        pub const COALESCE_WINDOW_US: u32 = 18;
    }
}

thread_local! {
    static CLOCK: Cell<u64> = const { Cell::new(0) };
}
fn now_ns() -> u64 {
    CLOCK.with(|c| c.get())
}
fn rmb() {}
fn stamp_reached(obs: u32, own: u32, tail: u32) -> bool {
    let diff = obs.wrapping_sub(own) as i32;
    let span = tail.wrapping_sub(own) as i32;
    diff >= 0 && diff <= span
}

struct Job {
    id: u32,
}
type Completions = Vec<u32>;

struct Lap;
impl Lap {
    fn start() -> Self {
        Self
    }
    fn on(&self) -> bool {
        false
    }
    fn lap(&mut self, _acc: &mut u64) {}
}

struct Cell32 {
    val: Cell<u32>,
    reads: Cell<u32>,
}
impl Cell32 {
    fn new(v: u32) -> Self {
        Self {
            val: Cell::new(v),
            reads: Cell::new(0),
        }
    }
    fn set(&self, v: u32) {
        self.val.set(v);
    }
    fn r32(&self, _off: usize) -> Result<u32> {
        self.reads.set(self.reads.get() + 1);
        Ok(self.val.get())
    }
}

struct ComputeGraph {
    fw_stamp: Cell32,
}
struct RenderGraph {
    fw_stamp: [Cell32; 2],
}
struct Graph {
    compute: [ComputeGraph; hw::sizes::COMPUTE_PIPES],
    render: RenderGraph,
}

#[derive(Default)]
struct Stats {
    idle_from_ns: u64,
    starve_from_ns: u64,
}
#[derive(Default)]
struct Prof {
    retire_ns: u64,
    polls: u64,
    poll_ns: u64,
}

struct Flight {
    computes: u32,
    compute_pipe: u8,
    renders: u32,
    target: u32,
    fs_target: u32,
}

struct Engine {
    shared: Option<Arc<Shared>>,
    flights: Vec<Flight>,
    pending: Vec<Arc<Job>>,
    target: [u32; hw::sizes::COMPUTE_PIPES],
    fs_target: u32,
    chan_wait: bool,
    slot_wait: bool,
    ta_wave_wait: bool,
    short_repoll: bool,
    housekeeping_owed: bool,
    held_wait: bool,
    coalesce_deadline_ns: u64,
    frozen: bool,
    stats: Stats,
    prof: Prof,
}
impl Engine {
    fn oldest(&self) -> Option<&Flight> {
        self.flights.first()
    }
}

#[derive(Default)]
struct Events {
    stage_events: u32,
    fs_events: u32,
    compute_events: u32,
}

#[derive(Default)]
struct Recovery {
    receipt_fault: bool,
    pending: bool,
    failed: Option<Error>,
}
impl Recovery {
    fn failed(&self) -> Option<Error> {
        self.failed
    }
    fn needs_repoll(&self) -> bool {
        self.pending
    }
}

struct Session;
impl Session {
    fn transport(&self) -> Result {
        Ok(())
    }
    fn check(&self) -> Result {
        Ok(())
    }
}
struct Objects;
impl Objects {
    fn health(&self) -> Result {
        Ok(())
    }
}

struct Shared {
    sleeping: AtomicBool,
    evented: AtomicBool,
    publish_wake: AtomicBool,
    closed_vm_reclaim: AtomicBool,
    rearmed: Cell<u32>,
    idle_power_polls: Cell<u32>,
    rearm_soon: Cell<u32>,
    rearm_after_calls: Cell<u32>,
    last_rearm_us: Cell<u32>,
    poll_now_calls: Cell<u32>,
}
impl Shared {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            sleeping: AtomicBool::new(false),
            evented: AtomicBool::new(false),
            publish_wake: AtomicBool::new(false),
            closed_vm_reclaim: AtomicBool::new(false),
            rearmed: Cell::new(0),
            idle_power_polls: Cell::new(0),
            rearm_soon: Cell::new(0),
            rearm_after_calls: Cell::new(0),
            last_rearm_us: Cell::new(0),
            poll_now_calls: Cell::new(0),
        })
    }
    fn rearm_us(&self, soon: bool) -> u32 {
        if soon { 25 } else { 100 }
    }
    fn rearm_after(&self, us: u32) {
        self.rearm_after_calls.set(self.rearm_after_calls.get() + 1);
        self.last_rearm_us.set(us);
    }
    fn rearm(&self, soon: bool) {
        self.rearmed.set(self.rearmed.get() + 1);
        if soon {
            self.rearm_soon.set(self.rearm_soon.get() + 1);
        }
    }
    fn poll_now(&self) {
        self.poll_now_calls.set(self.poll_now_calls.get() + 1);
    }
    fn arm_idle_power_poll(&self, _firmware: &Firmware) {
        self.idle_power_polls.set(self.idle_power_polls.get() + 1);
    }
    fn fail_stop(&self, _e: Error) {}
    __SUBMIT_LOCKED__
}

#[derive(Default)]
struct Power {
    ramp: bool,
    idle_reports: u32,
}
impl Power {
    fn needs_ramp(&self, _now: u64) -> bool {
        self.ramp
    }
    fn report_work(&mut self, in_flight: bool) {
        if !in_flight {
            self.idle_reports += 1;
        }
    }
}

#[derive(Default)]
struct Pools {
    reclaim_calls: u32,
    reclaim_remaining: bool,
    closed_vm_last: bool,
}
impl Pools {
    fn reclaim_closed(&mut self) -> bool {
        self.reclaim_calls += 1;
        self.reclaim_remaining
    }
    fn take_closed_vm_last(&mut self) -> bool {
        core::mem::take(&mut self.closed_vm_last)
    }
}

#[derive(Default)]
struct Slots {
    scrub_woke: AtomicBool,
    reclaim_calls: u32,
    reclaim_remaining: bool,
}
impl Slots {
    fn take_scrub_woke(&self) -> bool {
        self.scrub_woke.swap(false, Ordering::AcqRel)
    }
    fn reclaim_closed(&mut self) -> bool {
        self.reclaim_calls += 1;
        self.reclaim_remaining
    }
}

struct Firmware {
    session: Session,
    objects: Objects,
    graph: Graph,
    engine: Engine,
    events: Events,
    recovery: Recovery,
    power: Power,
    pools: Pools,
    slots: Slots,
    last_housekeeping_ns: u64,
    last_aux_drain_ns: u64,
    // Test script knobs for retire_once / publish / housekeeping observation.
    retire_on_call: bool,
    unfreeze_on_hang_check: bool,
    drain_stage_delta: u32,
    drain_fs_delta: u32,
    drain_compute_delta: u32,
    ta_pending: usize,
    ta_wave_cap: usize,
    coalesce_us: Option<u32>,
    publish_waits: Option<(bool, bool, bool)>,
    retire_calls: u32,
    publish_full_calls: u32,
    publish_checked_calls: u32,
    housekeeping_calls: u32,
    aux_drain_calls: u32,
    power_calls: u32,
    hot_drain_calls: u32,
}

impl Firmware {
    fn new() -> Self {
        Self {
            session: Session,
            objects: Objects,
            graph: Graph {
                compute: core::array::from_fn(|_| ComputeGraph {
                    fw_stamp: Cell32::new(0),
                }),
                render: RenderGraph {
                    fw_stamp: [Cell32::new(0), Cell32::new(0)],
                },
            },
            engine: Engine {
                shared: None,
                flights: vec![],
                pending: vec![],
                target: [0; hw::sizes::COMPUTE_PIPES],
                fs_target: 0,
                chan_wait: false,
                slot_wait: false,
                ta_wave_wait: false,
                short_repoll: false,
                housekeeping_owed: false,
                held_wait: false,
                coalesce_deadline_ns: 0,
                frozen: false,
                stats: Stats::default(),
                prof: Prof::default(),
            },
            events: Events::default(),
            recovery: Recovery::default(),
            power: Power::default(),
            pools: Pools::default(),
            slots: Slots::default(),
            last_housekeeping_ns: 0,
            last_aux_drain_ns: 0,
            retire_on_call: false,
            unfreeze_on_hang_check: false,
            drain_stage_delta: 0,
            drain_fs_delta: 0,
            drain_compute_delta: 0,
            ta_pending: 0,
            ta_wave_cap: hw::sizes::TA_WAVE_PASSES,
            coalesce_us: None,
            publish_waits: None,
            retire_calls: 0,
            publish_full_calls: 0,
            publish_checked_calls: 0,
            housekeeping_calls: 0,
            aux_drain_calls: 0,
            power_calls: 0,
            hot_drain_calls: 0,
        }
    }

    fn enqueue(&mut self, job: Arc<Job>) -> Result {
        self.engine.pending.push(job);
        Ok(())
    }
    fn recovery_pending(&self) -> Result<bool> {
        Ok(self.recovery.pending)
    }
    fn recovery_idle(&mut self) -> Result<bool> {
        Ok(!self.recovery.pending)
    }
    fn service_power(&mut self) -> Result {
        self.power_calls += 1;
        Ok(())
    }
    fn drain_rings(&mut self) -> Result {
        Ok(())
    }
    fn drain_events_and_stats(&mut self) -> Result {
        self.hot_drain_calls += 1;
        Ok(())
    }
    fn drain_auxiliary_rings(&mut self) -> Result {
        self.aux_drain_calls += 1;
        Ok(())
    }
    fn answer_growth(&mut self) -> Result {
        Ok(())
    }
    fn check_hangs(&mut self, _done: &mut Completions) -> Result {
        if self.unfreeze_on_hang_check {
            self.engine.frozen = false;
            self.recovery.pending = false;
        }
        Ok(())
    }
    fn fail_stop(&mut self, error: Error, _done: &mut Completions) {
        self.recovery.failed = Some(error);
    }
    __RUN_HOUSEKEEPING__
    fn retire_once(&mut self, done: &mut Completions) -> Result<bool> {
        self.retire_calls += 1;
        self.events.stage_events += self.drain_stage_delta;
        self.events.fs_events += self.drain_fs_delta;
        self.events.compute_events += self.drain_compute_delta;
        if self.retire_on_call && !self.engine.flights.is_empty() {
            self.engine.flights.remove(0);
            done.push(1);
            return Ok(true);
        }
        Ok(false)
    }
    fn publish(&mut self, _done: &mut Completions) -> Result {
        self.publish_full_calls += 1;
        Ok(())
    }
    fn publish_checked(&mut self, _done: &mut Completions) -> Result {
        self.engine.coalesce_deadline_ns = 0;
        if let Some((slot_wait, ta_wave_wait, held_wait)) = self.publish_waits {
            self.engine.slot_wait = slot_wait;
            self.engine.ta_wave_wait = ta_wave_wait;
            self.engine.held_wait = held_wait;
        }
        self.publish_checked_calls += 1;
        Ok(())
    }
    fn defer_busy_coalesce(&mut self, _now: u64) -> Option<u32> {
        self.coalesce_us
    }
    fn ta_pending_passes(&self) -> usize {
        self.ta_pending
    }
    fn active_ta_wave_cap(&self) -> usize {
        self.ta_wave_cap
    }

    __RECLAIM_CLOSED_VM_SLOTS__
    __POLL_STEP__
    __OLDEST_TERMINAL_EVENTS__
    __TA_WAVE_READY__
    __OLDEST_STAMP_REACHED__
}

#[test]
fn idle_poll_step_runs_full_housekeeping_only_on_its_cadence() {
    let mut fw = Firmware::new();
    let (mut retry_tail, mut hk_deferred, mut done) = (false, false, vec![]);
    let mut idle_pass = |fw: &mut Firmware, now: u64| {
        CLOCK.with(|c| c.set(now));
        fw.poll_step(false, 0, &mut retry_tail, &mut hk_deferred, &mut done)
            .unwrap()
    };
    // Due: the full idle housekeeping runs and owes nothing.
    assert!(!idle_pass(&mut fw, 2_000_000));
    assert_eq!((fw.power_calls, fw.hot_drain_calls), (1, 0));
    assert_eq!(fw.last_housekeeping_ns, 2_000_000);
    assert!(!fw.engine.housekeeping_owed);
    // Inside the cadence: only the event and statistics rings drain; the rest is owed.
    assert!(!idle_pass(&mut fw, 2_300_000));
    assert_eq!((fw.power_calls, fw.hot_drain_calls), (1, 1));
    assert_eq!(fw.last_housekeeping_ns, 2_000_000);
    assert!(fw.engine.housekeeping_owed);
    // A recovery found by that drain runs the full pass inside the cadence.
    fw.recovery.pending = true;
    assert!(idle_pass(&mut fw, 2_400_000));
    assert_eq!((fw.power_calls, fw.hot_drain_calls), (2, 2));
    assert_eq!(fw.last_housekeeping_ns, 2_400_000);
    assert!(!fw.engine.housekeeping_owed);
    fw.recovery.pending = false;
    // So does a receipt fault, without a hot drain first.
    fw.recovery.receipt_fault = true;
    assert!(!idle_pass(&mut fw, 2_500_000));
    assert_eq!((fw.power_calls, fw.hot_drain_calls), (3, 2));
    fw.recovery.receipt_fault = false;
    assert!(!idle_pass(&mut fw, 2_600_000));
    assert_eq!((fw.power_calls, fw.hot_drain_calls), (3, 3));
    assert!(fw.engine.housekeeping_owed);
    // Once the cadence elapses the owed housekeeping runs.
    assert!(!idle_pass(&mut fw, 3_500_000));
    assert_eq!((fw.power_calls, fw.hot_drain_calls), (4, 3));
    assert!(!fw.engine.housekeeping_owed);
    // Periodic housekeeping also settles what an idle pass owed.
    assert!(!idle_pass(&mut fw, 3_600_000));
    assert!(fw.engine.housekeeping_owed);
    CLOCK.with(|c| c.set(4_600_000));
    fw.run_housekeeping(&mut done).unwrap();
    assert!(!fw.engine.housekeeping_owed);
}

#[test]
fn idle_poll_step_reclaims_closed_vm_slots_and_rearms_while_remaining() {
    CLOCK.with(|c| c.set(1_000_000));
    let shared = Shared::new();
    let mut fw = Firmware::new();
    fw.engine.shared = Some(shared.clone());
    shared.closed_vm_reclaim.store(true, Ordering::Release);
    shared.publish_wake.store(true, Ordering::Release);
    fw.pools.reclaim_remaining = true;

    let (mut retry_tail, mut hk_deferred, mut done) = (false, false, vec![]);
    let busy = fw
        .poll_step(false, 0, &mut retry_tail, &mut hk_deferred, &mut done)
        .unwrap();
    assert!(!busy);
    assert_eq!((fw.pools.reclaim_calls, fw.slots.reclaim_calls), (1, 1));
    assert!(!shared.publish_wake.load(Ordering::Acquire));
    assert!(
        shared.closed_vm_reclaim.load(Ordering::Acquire),
        "remaining undisplaced closed VM keeps reclaim armed"
    );

    // Once remaining clears, idle poll clears closed_vm_reclaim and later polls skip scans.
    fw.pools.reclaim_remaining = false;
    let _ = fw
        .poll_step(false, 0, &mut retry_tail, &mut hk_deferred, &mut done)
        .unwrap();
    assert_eq!((fw.pools.reclaim_calls, fw.slots.reclaim_calls), (2, 2));
    assert!(!shared.closed_vm_reclaim.load(Ordering::Acquire));

    let _ = fw
        .poll_step(false, 0, &mut retry_tail, &mut hk_deferred, &mut done)
        .unwrap();
    assert_eq!((fw.pools.reclaim_calls, fw.slots.reclaim_calls), (2, 2));

    // Idle submit_locked also reclaims before publish_checked when armed.
    shared.closed_vm_reclaim.store(true, Ordering::Release);
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 9 }), &mut done)
        .unwrap();
    assert_eq!((fw.pools.reclaim_calls, fw.slots.reclaim_calls), (3, 3));
    assert!(!shared.closed_vm_reclaim.load(Ordering::Acquire));

    // A closed VM's render retired after the flag cleared and recorded its last PB epoch:
    // the next pass reclaims once without the flag, then scans stop again.
    fw.pools.closed_vm_last = true;
    let _ = fw
        .poll_step(false, 0, &mut retry_tail, &mut hk_deferred, &mut done)
        .unwrap();
    assert_eq!((fw.pools.reclaim_calls, fw.slots.reclaim_calls), (4, 4));
    assert!(!shared.closed_vm_reclaim.load(Ordering::Acquire));
    let _ = fw
        .poll_step(false, 0, &mut retry_tail, &mut hk_deferred, &mut done)
        .unwrap();
    assert_eq!((fw.pools.reclaim_calls, fw.slots.reclaim_calls), (4, 4));
}

#[test]
fn submit_locked_retires_inline_on_reached_stamp_without_full_housekeeping() {
    CLOCK.with(|c| c.set(100_000));
    let shared = Shared::new();
    shared.evented.store(true, Ordering::Release);
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw.engine.target[0] = 0x100;
    fw.graph.compute[0].fw_stamp.set(0x100);
    fw.retire_on_call = true;

    let mut done = vec![];
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 2 }), &mut done)
        .unwrap();
    assert_eq!(fw.retire_calls, 1);
    assert_eq!(done, vec![1]);
    assert_eq!(fw.publish_checked_calls, 1);
    assert_eq!(fw.publish_full_calls, 0);
    assert!(!shared.evented.load(Ordering::Acquire));
}

#[test]
fn submit_locked_preserves_evented_when_irq_precedes_receipt() {
    CLOCK.with(|c| c.set(100_000));
    let shared = Shared::new();
    shared.evented.store(true, Ordering::Release);
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x100,
    });
    fw.engine.fs_target = 0x100;

    let mut done = vec![];
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 2 }), &mut done)
        .unwrap();
    assert_eq!(fw.retire_calls, 1);
    assert!(shared.evented.load(Ordering::Acquire));
    assert_eq!(shared.rearm_soon.get(), 1);
    assert_eq!(shared.poll_now_calls.get(), 1);

    // TA-only non-terminal event must NOT restore evented, arm short repoll, or kick poll_now.
    let shared_ta = Shared::new();
    shared_ta.evented.store(true, Ordering::Release);
    let mut fw_ta = Firmware::new();
    fw_ta.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x100,
    });
    fw_ta.engine.fs_target = 0x100;
    fw_ta.drain_stage_delta = 1;
    fw_ta.drain_fs_delta = 0;
    shared_ta
        .submit_locked(&mut fw_ta, Arc::new(Job { id: 3 }), &mut done)
        .unwrap();
    assert!(!shared_ta.evented.load(Ordering::Acquire));
    assert_eq!(shared_ta.rearm_soon.get(), 0);
    assert_eq!(shared_ta.poll_now_calls.get(), 0);
}

#[test]
fn submit_locked_defers_housekeeping_and_arms_short_repoll_on_coalesced_event() {
    // Active bursts hold housekeeping at ACTIVE_HOUSEKEEPING_NS (5 ms): at 4 ms (< 5 ms),
    // submit_locked skips poll_now(); at 5 ms, it calls publish_checked() and kicks poll_now()
    // so housekeeping runs off SUBMIT.
    CLOCK.with(|c| c.set(5_000_000));
    let shared = Shared::new();
    let mut fw = Firmware::new();
    fw.last_housekeeping_ns = 1_000_000;
    fw.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x200,
        fs_target: 0,
    });
    fw.engine.target[0] = 0x200;

    let mut done = vec![];
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 3 }), &mut done)
        .unwrap();
    assert_eq!(fw.publish_full_calls, 0);
    assert_eq!(fw.publish_checked_calls, 1);
    assert_eq!(fw.retire_calls, 0);
    assert_eq!(shared.poll_now_calls.get(), 0);
    assert_eq!(shared.rearm_soon.get(), 0);

    // At 5 ms (>= ACTIVE_HOUSEKEEPING_NS), submit_locked defers housekeeping via poll_now().
    CLOCK.with(|c| c.set(6_000_000));
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 4 }), &mut done)
        .unwrap();
    assert_eq!(fw.publish_full_calls, 0);
    assert_eq!(fw.publish_checked_calls, 2);
    assert_eq!(shared.poll_now_calls.get(), 1);
    assert_eq!(shared.rearm_soon.get(), 0);

    // Coalesced EP 0x20 event retires flight 0 while flight 1 stays active -> short repoll.
    shared.evented.store(true, Ordering::Release);
    fw.retire_on_call = true;
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 5 }), &mut done)
        .unwrap();
    assert_eq!(fw.retire_calls, 1);
    assert_eq!(fw.engine.flights.len(), 1);
    assert_eq!(shared.rearm_soon.get(), 1);
}

#[test]
fn poll_step_defers_active_housekeeping_until_five_ms_and_off_hot_lock() {
    let mut fw = Firmware::new();
    fw.last_housekeeping_ns = 1_000_000;
    fw.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw.engine.target[0] = 0x100;
    let mut retry_tail = false;
    let mut deferred = false;
    let mut done = vec![];

    // At 4 ms (< 5 ms) while busy: housekeeping neither runs inline nor defers.
    CLOCK.with(|c| c.set(5_000_000));
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!deferred);
    assert_eq!(fw.last_housekeeping_ns, 1_000_000);

    // At 5 ms (>= 5 ms) while busy: defers off the hot poll_step lock instead of running inline.
    CLOCK.with(|c| c.set(6_000_000));
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(deferred);
    assert_eq!(fw.last_housekeeping_ns, 1_000_000);
}

#[test]
fn run_housekeeping_throttles_auxiliary_ring_drain_while_flights_active() {
    CLOCK.with(|c| c.set(1_000_000));
    let mut fw = Firmware::new();
    fw.last_aux_drain_ns = 1_000_000;
    fw.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    let mut done = vec![];

    // Within AUX_DRAIN_NS (10 ms) while busy: skip auxiliary ring drain.
    CLOCK.with(|c| c.set(5_000_000));
    fw.run_housekeeping(&mut done).unwrap();
    assert_eq!(fw.aux_drain_calls, 0);

    // Past AUX_DRAIN_NS while busy: drain once and advance timestamp.
    CLOCK.with(|c| c.set(11_000_000));
    fw.run_housekeeping(&mut done).unwrap();
    assert_eq!(fw.aux_drain_calls, 1);

    // Recovery pending bypasses throttle immediately.
    CLOCK.with(|c| c.set(12_000_000));
    fw.recovery.pending = true;
    fw.run_housekeeping(&mut done).unwrap();
    assert_eq!(fw.aux_drain_calls, 2);

    // Idle bypasses throttle immediately.
    CLOCK.with(|c| c.set(13_000_000));
    fw.recovery.pending = false;
    fw.engine.flights.clear();
    fw.run_housekeeping(&mut done).unwrap();
    assert_eq!(fw.aux_drain_calls, 3);
}

#[test]
fn ta_only_event_does_not_spin_or_arm_short_repoll() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x100,
    });
    fw.engine.fs_target = 0x100;
    // Simulate draining a TA (0x70) stage event with no FS (0x71) event and FS stamp unreached.
    fw.drain_stage_delta = 1;
    fw.drain_fs_delta = 0;

    let mut retry_tail = false;
    let mut deferred = false;
    let mut done = vec![];
    let active = fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap();
    assert!(active);
    assert!(!retry_tail);
    assert!(!fw.engine.short_repoll);
}

#[test]
fn fs_event_without_settled_receipt_still_polls_tail_and_sets_short_repoll() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x100,
    });
    fw.engine.fs_target = 0x100;
    fw.drain_stage_delta = 1;
    fw.drain_fs_delta = 1;

    let mut retry_tail = false;
    let mut deferred = false;
    let mut done = vec![];
    let active = fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap();
    assert!(active);
    assert!(retry_tail);

    retry_tail = false;
    let active = fw
        .poll_step(true, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap();
    assert!(active);
    assert!(!retry_tail);
    assert!(fw.engine.short_repoll);
}

#[test]
fn concurrent_compute_completion_behind_pending_render_wakes_submit_retire() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x200,
    });
    fw.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw.engine.fs_target = 0x200;
    fw.engine.target[0] = 0x100;
    // Render still running, compute reached its stamp.
    fw.graph.render.fw_stamp[1].set(0x100);
    fw.graph.compute[0].fw_stamp.set(0x100);
    assert!(fw.oldest_stamp_reached());
    assert_eq!(fw.graph.compute[0].fw_stamp.reads.get(), 1);
    assert_eq!(fw.graph.render.fw_stamp[1].reads.get(), 0);
    assert_eq!(fw.oldest_terminal_events(), Some((Some(0), Some(0))));

    // Pipe 1 reaching its stamp behind a pending pipe 0 flight also wakes inline retirement.
    let mut fw2 = Firmware::new();
    fw2.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x200,
        fs_target: 0,
    });
    fw2.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 1,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw2.engine.target[0] = 0x200;
    fw2.engine.target[1] = 0x100;
    fw2.graph.compute[0].fw_stamp.set(0x100);
    fw2.graph.compute[1].fw_stamp.set(0x100);
    assert!(fw2.oldest_stamp_reached());
    assert_eq!(fw2.graph.compute[0].fw_stamp.reads.get(), 1);
    assert_eq!(fw2.graph.compute[1].fw_stamp.reads.get(), 1);
    assert_eq!(fw2.graph.render.fw_stamp[1].reads.get(), 0);
}

#[test]
fn oldest_stamp_reached_skips_inactive_class_wc_reads() {
    // Compute-only workload never reads render fw_stamp or idle compute pipe 1.
    let mut fc = Firmware::new();
    fc.engine.flights.push(Flight {
        computes: 1,
        compute_pipe: 0,
        renders: 0,
        target: 0x200,
        fs_target: 0,
    });
    fc.engine.target[0] = 0x200;
    fc.graph.compute[0].fw_stamp.set(0x100);
    assert!(!fc.oldest_stamp_reached());
    assert_eq!(fc.graph.compute[0].fw_stamp.reads.get(), 1);
    assert_eq!(fc.graph.compute[1].fw_stamp.reads.get(), 0);
    assert_eq!(fc.graph.render.fw_stamp[0].reads.get(), 0);
    assert_eq!(fc.graph.render.fw_stamp[1].reads.get(), 0);

    // Render-only workload never reads compute fw_stamp.
    let mut fr = Firmware::new();
    fr.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x200,
    });
    fr.engine.fs_target = 0x200;
    fr.graph.render.fw_stamp[1].set(0x100);
    assert!(!fr.oldest_stamp_reached());
    assert_eq!(fr.graph.compute[0].fw_stamp.reads.get(), 0);
    assert_eq!(fr.graph.compute[1].fw_stamp.reads.get(), 0);
    assert_eq!(fr.graph.render.fw_stamp[0].reads.get(), 0);
    assert_eq!(fr.graph.render.fw_stamp[1].reads.get(), 1);
}

#[test]
fn poll_step_skips_publish_when_pending_blocked_and_nothing_retired() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 1,
        target: 0,
        fs_target: 0x100,
    });
    fw.engine.pending.push(Arc::new(Job { id: 2 }));
    fw.engine.fs_target = 0x100;
    fw.drain_stage_delta = 1;
    let mut retry_tail = false;
    let mut deferred = false;
    let mut done = vec![];
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 0);

    // Waking via scrub_woke triggers publish_checked once.
    fw.slots.scrub_woke.store(true, Ordering::Release);
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 1);

    // Waking via chan_wait triggers publish_checked.
    fw.engine.chan_wait = true;
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 2);
    fw.engine.chan_wait = false;

    // Waking via unfreeze triggers publish_checked.
    fw.engine.frozen = true;
    fw.recovery.pending = true;
    fw.unfreeze_on_hang_check = true;
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 3);

    // Waking via external publish_wake (e.g. VM close or unstarted queue cancel) triggers
    // publish_checked once while flights remain active, then skips again on the next busy poll.
    let shared = Shared::new();
    fw.engine.shared = Some(shared.clone());
    shared.publish_wake.store(true, Ordering::Release);
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 4);
    assert!(!shared.publish_wake.load(Ordering::Acquire));
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 4);

    // Waking via retirement triggers publish_checked (and returns false since flights drained).
    fw.retire_on_call = true;
    assert!(!fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 5);
}

#[test]
fn pending_slot_wait_arms_short_repoll_without_breaking_ta_only_or_frozen() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 11,
        target: 0,
        fs_target: 0x100,
    });
    fw.engine.fs_target = 0x100;
    let mut retry_tail = false;
    let mut deferred = false;
    let mut done = vec![];

    // No pending work on a timer poll -> keep normal resnap_us interval.
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!fw.engine.short_repoll);

    // Pending job NOT blocked on slot headroom (e.g. external fence / cross-VM / InitBM) ->
    // keep normal resnap_us interval without arming short_repoll.
    fw.engine.pending.push(Arc::new(Job { id: 2 }));
    fw.engine.slot_wait = false;
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!fw.engine.short_repoll);

    // Pending job waiting on slot headroom on a non-event poll -> arm short_repoll.
    fw.engine.slot_wait = true;
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(fw.engine.short_repoll);

    // TA-only EP 0x20 event still suppresses short_repoll.
    fw.drain_stage_delta = 1;
    fw.drain_fs_delta = 0;
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!fw.engine.short_repoll);
    fw.drain_stage_delta = 0;

    // Frozen engine suppresses short_repoll even with pending work.
    fw.engine.frozen = true;
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!fw.engine.short_repoll);
}

#[test]
fn held_wait_spins_stage_events_and_repolls_soon_until_a_retirement() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 2,
        target: 0,
        fs_target: 0x200,
    });
    fw.engine.fs_target = 0x200;
    fw.engine.pending.push(Arc::new(Job { id: 2 }));
    fw.drain_stage_delta = 1;
    let (mut retry_tail, mut deferred, mut done) = (false, false, vec![]);
    // Nothing held: a stage-only event is non-terminal, with no tail spin and no short repoll.
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!retry_tail);
    assert!(!fw.engine.short_repoll);
    // A ready job held behind its own VM's flights spins the receipt tail on the same event...
    fw.engine.held_wait = true;
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(retry_tail);
    // ...and a timer pass repolls at repoll_us while the hold lasts, without publishing.
    retry_tail = false;
    fw.drain_stage_delta = 0;
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(fw.engine.short_repoll);
    assert_eq!(fw.publish_checked_calls, 0);
    // The retirement that can release it reaches publish_checked().
    fw.retire_on_call = true;
    let _ = fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap();
    assert_eq!(fw.publish_checked_calls, 1);
}

#[test]
fn ta_only_irq_wakes_ta_wave_wait_with_hysteresis_and_short_repoll() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
        compute_pipe: 0,
        renders: 4,
        target: 0,
        fs_target: 0x400,
    });
    fw.engine.fs_target = 0x400;
    fw.engine.pending.push(Arc::new(Job { id: 2 }));
    fw.engine.ta_wave_wait = true;
    fw.ta_pending = 4;
    fw.drain_stage_delta = 1;
    fw.drain_fs_delta = 0;

    let mut retry_tail = false;
    let mut deferred = false;
    let mut done = vec![];
    // TA still full (4 > TA_WAVE_REFILL=2): no publish_checked wakeup yet, but ta_wave_wait
    // keeps non_terminal_only false so a 0x70 IRQ retries tail.
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 0);
    assert!(retry_tail);

    // TA drops to 3 (> TA_WAVE_REFILL=2): hysteresis avoids 1-pass stub drip, while
    // tail == 0 arms short_repoll because ta_wave_wait is active.
    fw.ta_pending = 3;
    retry_tail = false;
    assert!(fw
        .poll_step(true, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 0);
    assert!(!retry_tail);
    assert!(fw.engine.short_repoll);

    // TA drains to 2 (<= cap - TA_WAVE_REFILL_HEADROOM): immediately wakes publish_checked().
    fw.ta_pending = 2;
    assert!(fw
        .poll_step(true, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 1);
    assert!(fw.engine.short_repoll);

    // Light TA wave cap (8): stays paused at 7 (> 8 - 2), wakes at 6 (<= 6).
    fw.ta_wave_cap = hw::sizes::TA_WAVE_PASSES_LIGHT;
    fw.ta_pending = 7;
    assert!(fw
        .poll_step(true, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 1);
    fw.ta_pending = 6;
    assert!(fw
        .poll_step(true, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 2);

    // Once ta_wave_wait clears, a TA-only 0x70 IRQ stays non_terminal_only (no spin/short_repoll).
    fw.engine.ta_wave_wait = false;
    retry_tail = false;
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert!(!retry_tail);
    assert!(!fw.engine.short_repoll);
}

#[test]
fn submit_locked_repolls_soon_when_its_publication_leaves_a_paced_job() {
    // publish_checked() leaves a ready job on TA wave or slot headroom: SUBMIT arms the same
    // short repoll poll_step() arms for it, unless the engine is frozen.
    CLOCK.with(|c| c.set(100_000));
    for (slot_wait, ta_wave_wait, held_wait, frozen, soon) in [
        (false, false, false, false, 0),
        (true, false, false, false, 1),
        (false, true, false, false, 1),
        (false, false, true, false, 1),
        (false, true, false, true, 0),
    ] {
        let shared = Shared::new();
        let mut fw = Firmware::new();
        fw.last_housekeeping_ns = 100_000;
        fw.engine.flights.push(Flight {
            computes: 0,
            compute_pipe: 0,
            renders: 1,
            target: 0,
            fs_target: 0x100,
        });
        fw.engine.fs_target = 0x100;
        fw.engine.frozen = frozen;
        fw.publish_waits = Some((slot_wait, ta_wave_wait, held_wait));
        let mut done = vec![];
        shared
            .submit_locked(&mut fw, Arc::new(Job { id: 2 }), &mut done)
            .unwrap();
        assert_eq!(fw.retire_calls, 0);
        assert_eq!(fw.publish_checked_calls, 1);
        assert_eq!(shared.rearmed.get(), 1);
        assert_eq!(shared.rearm_soon.get(), soon);
        assert_eq!(shared.poll_now_calls.get(), 0);
    }
}

#[test]
fn submit_locked_and_poll_step_micro_coalesce_busy_horizons() {
    CLOCK.with(|c| c.set(100_000));
    let shared = Shared::new();
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 2,
        compute_pipe: 0,
        renders: 0,
        target: 0x200,
        fs_target: 0,
    });
    fw.engine.target[0] = 0x200;
    fw.coalesce_us = Some(18);
    fw.engine.coalesce_deadline_ns = 118_000;

    // submit_locked skips publish_checked and arms rearm_after(18) when defer_busy_coalesce fires.
    let mut done = vec![];
    shared
        .submit_locked(&mut fw, Arc::new(Job { id: 3 }), &mut done)
        .unwrap();
    assert_eq!(fw.publish_checked_calls, 0);
    assert_eq!(shared.rearm_after_calls.get(), 1);
    assert_eq!(shared.last_rearm_us.get(), 18);
    assert_eq!(shared.rearmed.get(), 0);

    // An early non-retiring event poll before the deadline does not flush prematurely.
    CLOCK.with(|c| c.set(105_000));
    let (mut retry_tail, mut deferred) = (false, false);
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 0);

    // Reaching the deadline on an event poll flushes immediately.
    CLOCK.with(|c| c.set(118_000));
    assert!(fw
        .poll_step(true, EVENT_RETIRE_POLLS, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 1);
    assert_eq!(fw.engine.coalesce_deadline_ns, 0);

    // A timer poll (!evented) with an active coalesce deadline also flushes immediately.
    fw.engine.coalesce_deadline_ns = 136_000;
    CLOCK.with(|c| c.set(125_000));
    assert!(fw
        .poll_step(false, 0, &mut retry_tail, &mut deferred, &mut done)
        .unwrap());
    assert_eq!(fw.publish_checked_calls, 2);
    assert_eq!(fw.engine.coalesce_deadline_ns, 0);
}

mod boot {
    pub fn eproto() -> super::Error {
        super::Error(-71)
    }
}

mod event_drain_test {
    use super::{Error, Result};
    use std::cell::{Cell, RefCell};

    const EIO: Error = Error(-5);
    const STATE_READ: usize = 0;
    const STATE_WRITE: usize = 0x40;
    const RING_SLOTS: u32 = 128;
    const RECORD_WORDS: usize = 18;
    const COMPUTE_PIPES: usize = 16;
    const STAGE_COUNT: usize = 2 + COMPUTE_PIPES;
    mod compute {
        pub const EVENT: u32 = 15;
    }
    const COMPUTE_EVENT_MASK: u32 = ((1u32 << COMPUTE_PIPES) - 1) << compute::EVENT;
    mod rings {
        pub const EVENT_RECORD: usize = 0x48;
    }
    mod bindings {
        pub unsafe fn ktime_get() -> i64 {
            12345
        }
    }
    mod device {
        pub struct Device;
    }
    macro_rules! dev_warn { ($dev:expr, $($arg:tt)*) => {{ let _ = ($dev, format_args!($($arg)*)); }}; }
    macro_rules! dev_err { ($dev:expr, $($arg:tt)*) => {{ let _ = ($dev, format_args!($($arg)*)); }}; }
    fn rmb() {}
    fn wmb() {}
    fn mb() {}

    struct Mem {
        words: RefCell<Vec<u32>>,
        reads: Cell<u32>,
        writes: Cell<u32>,
        fail_at_offset: Cell<Option<usize>>,
    }
    impl Mem {
        fn new(n_words: usize) -> Self {
            Self {
                words: RefCell::new(vec![0u32; n_words]),
                reads: Cell::new(0),
                writes: Cell::new(0),
                fail_at_offset: Cell::new(None),
            }
        }
        fn r32(&self, off: usize) -> Result<u32> {
            self.reads.set(self.reads.get() + 1);
            if self.fail_at_offset.get() == Some(off) {
                return Err(EIO);
            }
            Ok(self.words.borrow()[off / 4])
        }
        fn w32(&self, off: usize, val: u32) -> Result {
            self.writes.set(self.writes.get() + 1);
            self.words.borrow_mut()[off / 4] = val;
            Ok(())
        }
    }

    struct Ring {
        state: Mem,
        ring: Mem,
    }
    impl Ring {
        fn new() -> Self {
            Self {
                state: Mem::new(0x40),
                ring: Mem::new(RING_SLOTS as usize * RECORD_WORDS),
            }
        }
        fn put_record(&self, slot: u32, words: &[u32]) {
            let mut buf = self.ring.words.borrow_mut();
            let base = slot as usize * RECORD_WORDS;
            for (i, &w) in words.iter().enumerate() {
                buf[base + i] = w;
            }
        }
        fn set_indices(&self, read: u32, write: u32) {
            let mut st = self.state.words.borrow_mut();
            st[STATE_READ / 4] = read;
            st[STATE_WRITE / 4] = write;
        }
        fn read_idx(&self) -> u32 {
            self.state.words.borrow()[STATE_READ / 4]
        }
    }

    #[derive(Clone, Copy)]
    struct ErrorEvent {
        kind: u32,
        slot: i32,
        raw: [u32; RECORD_WORDS],
    }

    #[derive(Default)]
    struct Events {
        stage_events: u32,
        ta_events: u32,
        fs_events: u32,
        compute_events: u32,
        event_ns: [u64; STAGE_COUNT],
        errors: u32,
        channel_errors: u32,
        recovery_started_ns: Option<u64>,
        last_error: Option<ErrorEvent>,
        last_statistics: Option<[u32; RECORD_WORDS]>,
        recovery_statistics: u32,
    }

    impl Events {
        fn indices(ring: &Ring) -> Result<(u32, u32)> {
            let read = ring.state.r32(STATE_READ)?;
            let producer = ring.state.r32(STATE_WRITE)?;
            if read >= RING_SLOTS || producer >= RING_SLOTS {
                return Err(EIO);
            }
            rmb();
            Ok((read, producer))
        }
        fn words(ring: &Ring, slot: u32) -> Result<[u32; RECORD_WORDS]> {
            let base = slot as usize * rings::EVENT_RECORD;
            let mut out = [0u32; RECORD_WORDS];
            for (i, word) in out.iter_mut().enumerate() {
                *word = ring.ring.r32(base + i * 4)?;
            }
            Ok(out)
        }
        __EVENT_DRAIN__
        __EVENT_DRAIN_STATS__
    }

    #[test]
    fn event_drain_batches_state_read_and_minimizes_wc_reads() {
        let ring = Ring::new();
        // 5 consecutive type-1 stage events: TA (0x70 -> word 4 bit 16), FS (0x71 -> word 4 bit 17),
        // Compute pipe 0 (15 -> word 1 bit 15), Compute pipe 1 (16 -> word 1 bit 16),
        // and Compute pipe 7 (22 -> word 1 bit 22, event_ns[9]).
        let mut r0 = [0u32; RECORD_WORDS];
        r0[0] = 1;
        r0[4] = 1 << 16;
        let mut r1 = [0u32; RECORD_WORDS];
        r1[0] = 1;
        r1[4] = 1 << 17;
        let mut r2 = [0u32; RECORD_WORDS];
        r2[0] = 1;
        r2[1] = 1 << 15;
        let mut r3 = [0u32; RECORD_WORDS];
        r3[0] = 1;
        r3[1] = 1 << 16;
        let mut r4 = [0u32; RECORD_WORDS];
        r4[0] = 1;
        r4[1] = 1 << 22;
        ring.put_record(0, &r0);
        ring.put_record(1, &r1);
        ring.put_record(2, &r2);
        ring.put_record(3, &r3);
        ring.put_record(4, &r4);
        ring.set_indices(0, 5);

        let mut ev = Events::default();
        ev.drain(&device::Device, &ring).unwrap();
        assert_eq!(ev.event_ns[0], 12345);
        assert_eq!(ev.event_ns[1], 12345);
        assert_eq!(ev.event_ns[2], 12345);
        assert_eq!(ev.event_ns[3], 12345);
        assert_eq!(ev.event_ns[9], 12345);
        assert_eq!(ev.fs_events, 1);
        assert_eq!(ev.compute_events, 3);
        assert_eq!(ev.stage_events, 5);
        assert_eq!(ring.read_idx(), 5);
        // Exactly ONE STATE_READ write for all 5 records!
        assert_eq!(ring.state.writes.get(), 1);
        // Exactly 3 WC reads per type-1 record (kind, word 1, word 4) = 15 reads (not 90)!
        assert_eq!(ring.ring.reads.get(), 15);
    }

    #[test]
    fn event_drain_flushes_progress_on_mid_burst_error() {
        let ring = Ring::new();
        let mut r0 = [0u32; RECORD_WORDS];
        r0[0] = 1;
        r0[4] = 1 << 17;
        ring.put_record(0, &r0);
        ring.set_indices(0, 2);
        // Fail reading record 1's kind word.
        ring.ring.fail_at_offset.set(Some(rings::EVENT_RECORD));
        let mut ev = Events::default();
        assert_eq!(ev.drain(&device::Device, &ring), Err(EIO));
        // Record 0 progress must still have been flushed to STATE_READ = 1.
        assert_eq!(ring.read_idx(), 1);
        assert_eq!(ring.state.writes.get(), 1);
        assert_eq!(ev.fs_events, 1);
    }

    #[test]
    fn stats_drain_batches_state_read_and_reads_single_word_for_non_recovery() {
        let ring = Ring::new();
        let mut r0 = [0u32; RECORD_WORDS];
        r0[0] = 0x10;
        let mut r1 = [0u32; RECORD_WORDS];
        r1[0] = 0x11;
        let mut r2 = [0u32; RECORD_WORDS];
        r2[0] = 0x16;
        ring.put_record(0, &r0);
        ring.put_record(1, &r1);
        ring.put_record(2, &r2);
        ring.set_indices(0, 3);

        let mut ev = Events::default();
        ev.drain_statistics(&device::Device, &ring).unwrap();
        assert_eq!(ev.recovery_statistics, 1);
        assert!(ev.last_statistics.is_some());
        assert_eq!(ring.read_idx(), 3);
        assert_eq!(ring.state.writes.get(), 1);
        // 1 read each for r0, r1 + 1 (kind) + 18 (full words) for r2 = 21 reads (vs 54).
        assert_eq!(ring.ring.reads.get(), 21);
    }
}

mod resnap_sync_test {
    use std::sync::{
        Arc, Barrier, Mutex,
        atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
    };

    const BACKUP_REARM_NS: u64 = 1_000_000;
    static CLOCK: AtomicU64 = AtomicU64::new(0);
    fn now_ns() -> u64 {
        CLOCK.load(Ordering::Acquire)
    }

    mod params {
        pub fn resnap_us() -> u32 {
            100
        }
        pub fn repoll_us() -> u32 {
            25
        }
    }
    #[allow(non_upper_case_globals)]
    mod module_parameters {
        pub struct Param(u32);
        impl Param {
            pub fn value(&self) -> u32 {
                self.0
            }
        }
        pub static g16_repoll_us: Param = Param(25);
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct Delta(i64);
    impl Delta {
        fn from_micros(us: i64) -> Self {
            Self(us)
        }
    }

    struct Handle {
        drops: Arc<AtomicU32>,
    }
    impl Drop for Handle {
        fn drop(&mut self) {
            self.drops.fetch_add(1, Ordering::AcqRel);
        }
    }

    struct Lock<T>(Mutex<T>);
    impl<T> Lock<T> {
        fn new(v: T) -> Self {
            Self(Mutex::new(v))
        }
        fn lock(&self) -> std::sync::MutexGuard<'_, T> {
            self.0.lock().unwrap()
        }
    }

    struct Shared {
        sleeping: AtomicBool,
        ewma_flight_us: AtomicU32,
        resnap_deadline_ns: AtomicU64,
        backup_deadline_ns: AtomicU64,
        resnap_handle: Lock<Option<Handle>>,
        starts: AtomicU32,
        drops: Arc<AtomicU32>,
        mod_polls: AtomicU32,
    }

    impl Shared {
        fn new() -> Arc<Self> {
            Arc::new(Self {
                sleeping: AtomicBool::new(false),
                ewma_flight_us: AtomicU32::new(0),
                resnap_deadline_ns: AtomicU64::new(0),
                backup_deadline_ns: AtomicU64::new(0),
                resnap_handle: Lock::new(None),
                starts: AtomicU32::new(0),
                drops: Arc::new(AtomicU32::new(0)),
                mod_polls: AtomicU32::new(0),
            })
        }
        fn start(self: Arc<Self>, _delta: Delta) -> Handle {
            self.starts.fetch_add(1, Ordering::AcqRel);
            Handle {
                drops: self.drops.clone(),
            }
        }
        fn mod_poll(self: &Arc<Self>, _delay: u64) {
            self.mod_polls.fetch_add(1, Ordering::AcqRel);
        }

        __CANCEL_RESNAP__
        __IDLE_RESNAP__
        __REARM_US__
        __REARM__
        __REARM_AFTER__
        __NOTE_FLIGHT_US__
    }

    #[test]
    fn idle_resnap_avoids_churn_for_short_armed_timer_while_cancelling_long_or_stale() {
        CLOCK.store(100_000, Ordering::Release);
        let shared = Shared::new();
        // Unarmed idle: no-op without dropping anything.
        shared.idle_resnap();
        assert_eq!(shared.drops.load(Ordering::Acquire), 0);

        // Short armed timer (<= resnap_us = 100 us): preserved across brief d=1 idle gap.
        shared.rearm(false);
        assert_eq!(shared.starts.load(Ordering::Acquire), 1);
        CLOCK.store(120_000, Ordering::Release);
        shared.idle_resnap();
        assert_eq!(shared.drops.load(Ordering::Acquire), 0);
        assert!(shared.resnap_handle.lock().is_some());

        // Next submission arriving within window reuses the armed timer without restart.
        shared.rearm(false);
        assert_eq!(shared.starts.load(Ordering::Acquire), 1);
        assert_eq!(shared.drops.load(Ordering::Acquire), 0);

        // Overdue or > resnap_us timer falls back to unconditional cancel_resnap().
        CLOCK.store(200_000, Ordering::Release);
        shared.idle_resnap();
        assert_eq!(shared.drops.load(Ordering::Acquire), 1);
        assert!(shared.resnap_handle.lock().is_none());
        assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 0);
    }

    #[test]
    fn note_flight_us_decays_rapidly_downward_for_burst_transitions() {
        let shared = Shared::new();
        shared.note_flight_us(260);
        assert_eq!(shared.ewma_flight_us.load(Ordering::Relaxed), 260);
        // Three 20 us flights decay rapidly below 2 * resnap_us (200 -> 140 -> 80 -> 50).
        shared.note_flight_us(20);
        assert_eq!(shared.ewma_flight_us.load(Ordering::Relaxed), 140);
        shared.note_flight_us(20);
        assert_eq!(shared.ewma_flight_us.load(Ordering::Relaxed), 80);
        shared.note_flight_us(20);
        assert_eq!(shared.ewma_flight_us.load(Ordering::Relaxed), 50);
        assert_eq!(shared.rearm_us(false), 25);
    }

    #[test]
    fn adaptive_rearm_us_scales_with_short_flight_ewma() {
        let shared = Shared::new();
        assert_eq!(shared.rearm_us(false), 100);
        assert_eq!(shared.rearm_us(true), 25);

        // Very short 30 us flights clamp to repoll_us (25 us).
        shared.ewma_flight_us.store(30, Ordering::Relaxed);
        assert_eq!(shared.rearm_us(false), 25);

        // 100 us flights halve to 50 us.
        shared.ewma_flight_us.store(100, Ordering::Relaxed);
        assert_eq!(shared.rearm_us(false), 50);

        // Heavy 250+ us flights stay at full resnap_us (100 us).
        shared.ewma_flight_us.store(250, Ordering::Relaxed);
        assert_eq!(shared.rearm_us(false), 100);
    }

    #[test]
    fn resnap_rearm_coalesces_and_cancel_disarms_unconditionally() {
        CLOCK.store(100_000, Ordering::Release);
        let shared = Shared::new();
        shared.rearm(false);
        assert_eq!(shared.starts.load(Ordering::Acquire), 1);
        assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 200_000);
        assert!(shared.resnap_handle.lock().is_some());

        // Same-window rearm coalesces without cancelling/restarting the handle.
        CLOCK.store(102_000, Ordering::Release);
        shared.rearm(false);
        assert_eq!(shared.starts.load(Ordering::Acquire), 1);
        assert_eq!(shared.drops.load(Ordering::Acquire), 0);

        // Sooner repoll (>5 us earlier) replaces the handle cleanly.
        shared.rearm(true);
        assert_eq!(shared.starts.load(Ordering::Acquire), 2);
        assert_eq!(shared.drops.load(Ordering::Acquire), 1);
        assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 127_000);

        // Coalesce rearm_after(18) pulls in a 25 us timer (>5 us earlier) and coalesces within 5 us.
        shared.rearm_after(18);
        assert_eq!(shared.starts.load(Ordering::Acquire), 3);
        assert_eq!(shared.drops.load(Ordering::Acquire), 2);
        assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 120_000);
        CLOCK.store(106_000, Ordering::Release);
        shared.rearm_after(14);
        assert_eq!(shared.starts.load(Ordering::Acquire), 3);
        assert_eq!(shared.drops.load(Ordering::Acquire), 2);

        // cancel_resnap unconditionally clears deadline and drops handle.
        shared.cancel_resnap();
        assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 0);
        assert!(shared.resnap_handle.lock().is_none());
        assert_eq!(shared.drops.load(Ordering::Acquire), 3);
    }

    #[test]
    fn sleep_cancel_resnap_never_leaves_handle_armed_under_concurrent_rearm() {
        CLOCK.store(1_000_000, Ordering::Release);
        for _ in 0..200 {
            let shared = Shared::new();
            shared.rearm(false);
            let start = Arc::new(Barrier::new(2));
            let s1 = shared.clone();
            let b1 = start.clone();
            let t = std::thread::spawn(move || {
                b1.wait();
                for _ in 0..32 {
                    s1.rearm(true);
                }
            });
            start.wait();
            // Exact sleep_down() + join_workers() sequence: sleeping = true, then cancel_resnap().
            shared.sleeping.store(true, Ordering::Release);
            shared.cancel_resnap();
            t.join().unwrap();
            assert!(
                shared.resnap_handle.lock().is_none(),
                "rearm leaked an hrtimer handle past sleep_down() + cancel_resnap()"
            );
            assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 0);
        }
    }
}

#[path = "__USAGE_PATH__"]
mod usage;

#[test]
fn per_file_usage_union_deduplicates_pipelined_and_dual_pipe_siblings() {
    let client = Arc::new(usage::Client::new());
    const HZ: u32 = 24_000_000;
    // Two pipelined renders whose TA and FS intervals overlap across flights.
    client.add_union(usage::Engine::Vertex, 24_000, 48_000, HZ);
    client.add_union(usage::Engine::Fragment, 40_000, 88_000, HZ);
    client.add_union(usage::Engine::Vertex, 36_000, 72_000, HZ);
    client.add_union(usage::Engine::Fragment, 64_000, 120_000, HZ);
    // Two sibling queues on the same DRM file overlapping across dual CDM pipes.
    client.add_union(usage::Engine::Compute, 100_000, 124_000, HZ);
    client.add_union(usage::Engine::Compute, 106_000, 130_000, HZ);
    let [v_ns, f_ns, c_ns] = client.snapshot();
    // Vertex union: [24k, 72k] = 48k ticks = 2_000_000 ns (not 60k ticks = 2.5 ms).
    assert_eq!(v_ns, 2_000_000);
    // Fragment union: [40k, 120k] = 80k ticks = 3_333_333 ns (distinct from Vertex).
    assert_eq!(f_ns, 3_333_333);
    assert_ne!(v_ns, f_ns);
    // Compute union: [100k, 130k] = 30k ticks = 1_250_000 ns (not 48k ticks = 2.0 ms).
    assert_eq!(c_ns, 1_250_000);
}

mod poll_placement_test {
    use std::sync::{
        Arc, Mutex,
        atomic::{AtomicU32, Ordering},
    };

    const FAST_POLL_WORK: u64 = 1;
    const UNBOUND_POLL_WORK: u64 = 4;

    #[derive(Default)]
    struct Queue {
        calls: Mutex<Vec<(Option<u32>, u64)>>,
    }
    impl Queue {
        fn enqueue<P, const ID: u64>(&self, _item: P) -> Result<(), ()> {
            self.calls.lock().unwrap().push((None, ID));
            Ok(())
        }
        fn enqueue_on<P, const ID: u64>(&self, cpu: u32, _item: P) -> Result<(), ()> {
            self.calls.lock().unwrap().push((Some(cpu), ID));
            Ok(())
        }
        fn take(&self) -> Vec<(Option<u32>, u64)> {
            std::mem::take(&mut *self.calls.lock().unwrap())
        }
    }

    struct Shared {
        active_flights: AtomicU32,
        submitter: AtomicU32,
        events: Queue,
        events_unbound: Queue,
    }
    impl Shared {
        fn new() -> Arc<Self> {
            Arc::new(Self {
                active_flights: AtomicU32::new(0),
                submitter: AtomicU32::new(u32::MAX),
                events: Queue::default(),
                events_unbound: Queue::default(),
            })
        }
        fn submitter_cpu(&self) -> Option<u32> {
            let v = self.submitter.load(Ordering::Relaxed);
            (v != u32::MAX).then_some(v)
        }
        __POLL_NOW__
    }

    #[test]
    fn poll_now_pins_single_flight_and_routes_pipelined_flights_unbound() {
        let shared = Shared::new();
        shared.submitter.store(3, Ordering::Relaxed);

        // d=0 and d=1 pin FAST_POLL_WORK onto the submitting CPU.
        shared.active_flights.store(0, Ordering::Release);
        shared.poll_now();
        assert_eq!(shared.events.take(), vec![(Some(3), FAST_POLL_WORK)]);
        assert!(shared.events_unbound.take().is_empty());

        shared.active_flights.store(1, Ordering::Release);
        shared.poll_now();
        assert_eq!(shared.events.take(), vec![(Some(3), FAST_POLL_WORK)]);
        assert!(shared.events_unbound.take().is_empty());

        // Without a recorded submitter CPU, d=1 falls back to events.enqueue().
        shared.submitter.store(u32::MAX, Ordering::Relaxed);
        shared.poll_now();
        assert_eq!(shared.events.take(), vec![(None, FAST_POLL_WORK)]);
        assert!(shared.events_unbound.take().is_empty());

        // d>=2 routes UNBOUND_POLL_WORK onto events_unbound even with a valid submitter CPU.
        shared.submitter.store(3, Ordering::Relaxed);
        for depth in [2u32, 4, 16] {
            shared.active_flights.store(depth, Ordering::Release);
            shared.poll_now();
            assert!(shared.events.take().is_empty());
            assert_eq!(
                shared.events_unbound.take(),
                vec![(None, UNBOUND_POLL_WORK)]
            );
        }
    }
}
'''

harness = harness.replace(
    '__USAGE_PATH__', str(kernel_root() / 'drivers/gpu/drm/asahi/usage.rs')
)

for token, decl in {
    '__SUBMIT_LOCKED__': 'fn submit_locked(',
    '__RECLAIM_CLOSED_VM_SLOTS__': 'pub(crate) fn reclaim_closed_vm_slots(',
    '__POLL_STEP__': 'pub(crate) fn poll_step(',
    '__RUN_HOUSEKEEPING__': 'pub(crate) fn run_housekeeping(',
    '__OLDEST_TERMINAL_EVENTS__': 'fn oldest_terminal_events(',
    '__TA_WAVE_READY__': 'fn ta_wave_ready(',
    '__OLDEST_STAMP_REACHED__': 'fn oldest_stamp_reached(',
    '__CANCEL_RESNAP__': 'pub(crate) fn cancel_resnap(',
    '__IDLE_RESNAP__': 'pub(crate) fn idle_resnap(',
    '__REARM_US__': 'pub(crate) fn rearm_us(',
    '__REARM__': 'pub(crate) fn rearm(',
    '__REARM_AFTER__': 'pub(crate) fn rearm_after(',
    '__POLL_NOW__': 'pub(crate) fn poll_now(',
}.items():
    harness = harness.replace(token, item(decl).replace('super::hw::', 'hw::'))
harness = harness.replace('__NOTE_FLIGHT_US__', item('pub(crate) fn note_flight_us(', mod_source))
for token, decl in {
    '__EVENT_DRAIN__': 'pub(crate) fn drain(',
    '__EVENT_DRAIN_STATS__': 'pub(crate) fn drain_statistics(',
}.items():
    harness = harness.replace(token, item(decl, event_source))

unit = output / 'poll-retire.rs'
unit.write_text(harness)
log = ''
for command in [
    rustc() + ['--edition=2021', '--test', str(unit), '-o', str(output / 'poll-retire')],
    [str(output / 'poll-retire'), '--nocapture'],
]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (output / 'poll-retire.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)
(output / 'poll-retire.json').write_text(
    json.dumps(
        {
            'passed': True,
            'tests': 25,
            'cpu_only': True,
            'hardware_execution': False,
            'scope': 'Verbatim production submit_locked inline retirement and poll_step TA-only non-spin logic.',
            'source_sha256': sha(production),
        },
        indent=2,
    )
    + '\n'
)
