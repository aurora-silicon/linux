#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute production G16 inline submit retirement and EP 0x20 poll-step logic."""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

production = kernel_root() / 'drivers/gpu/drm/asahi/g16/poll.rs'
source = production.read_text()
output = output_dir()


def item(declaration):
    start = source.index(declaration)
    at = source.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#![allow(dead_code, unused_variables)]
use std::cell::Cell;
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct Error(i32);
type Result<T = ()> = std::result::Result<T, Error>;

const HOUSEKEEPING_NS: u64 = 1_000_000;
const EVENT_RETIRE_POLLS: u32 = 11;

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
    compute: ComputeGraph,
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
    renders: u32,
    target: u32,
    fs_target: u32,
}

struct Engine {
    shared: Option<Arc<Shared>>,
    flights: Vec<Flight>,
    pending: Vec<Arc<Job>>,
    target: u32,
    fs_target: u32,
    chan_wait: bool,
    short_repoll: bool,
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
    rearmed: Cell<u32>,
}
impl Shared {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            sleeping: AtomicBool::new(false),
            evented: AtomicBool::new(false),
            rearmed: Cell::new(0),
        })
    }
    fn rearm(&self, _soon: bool) {
        self.rearmed.set(self.rearmed.get() + 1);
    }
    fn fail_stop(&self, _e: Error) {}
    __SUBMIT_LOCKED__
}

#[derive(Default)]
struct Power {
    ramp: bool,
}
impl Power {
    fn needs_ramp(&self, _now: u64) -> bool {
        self.ramp
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
    last_housekeeping_ns: u64,
    // Test script knobs for retire_once / publish / housekeeping observation.
    retire_on_call: bool,
    drain_stage_delta: u32,
    drain_fs_delta: u32,
    drain_compute_delta: u32,
    retire_calls: u32,
    publish_full_calls: u32,
    publish_checked_calls: u32,
    housekeeping_calls: u32,
    power_calls: u32,
}

impl Firmware {
    fn new() -> Self {
        Self {
            session: Session,
            objects: Objects,
            graph: Graph {
                compute: ComputeGraph {
                    fw_stamp: Cell32::new(0),
                },
                render: RenderGraph {
                    fw_stamp: [Cell32::new(0), Cell32::new(0)],
                },
            },
            engine: Engine {
                shared: None,
                flights: vec![],
                pending: vec![],
                target: 0,
                fs_target: 0,
                chan_wait: false,
                short_repoll: false,
                frozen: false,
                stats: Stats::default(),
                prof: Prof::default(),
            },
            events: Events::default(),
            recovery: Recovery::default(),
            power: Power::default(),
            last_housekeeping_ns: 0,
            retire_on_call: false,
            drain_stage_delta: 0,
            drain_fs_delta: 0,
            drain_compute_delta: 0,
            retire_calls: 0,
            publish_full_calls: 0,
            publish_checked_calls: 0,
            housekeeping_calls: 0,
            power_calls: 0,
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
    fn answer_growth(&mut self) -> Result {
        Ok(())
    }
    fn check_hangs(&mut self, _done: &mut Completions) -> Result {
        Ok(())
    }
    fn fail_stop(&mut self, error: Error, _done: &mut Completions) {
        self.recovery.failed = Some(error);
    }
    fn run_housekeeping(&mut self, _done: &mut Completions) -> Result {
        self.housekeeping_calls += 1;
        self.last_housekeeping_ns = now_ns();
        Ok(())
    }
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
        self.publish_checked_calls += 1;
        Ok(())
    }

    __POLL_STEP__
    __OLDEST_TERMINAL_EVENTS__
    __OLDEST_STAMP_REACHED__
}

#[test]
fn submit_locked_retires_inline_on_reached_stamp_without_full_housekeeping() {
    CLOCK.with(|c| c.set(100_000));
    let shared = Shared::new();
    shared.evented.store(true, Ordering::Release);
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 1,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw.engine.target = 0x100;
    fw.graph.compute.fw_stamp.set(0x100);
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
}

#[test]
fn ta_only_event_does_not_spin_or_arm_short_repoll() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
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
        renders: 1,
        target: 0,
        fs_target: 0x200,
    });
    fw.engine.flights.push(Flight {
        computes: 1,
        renders: 0,
        target: 0x100,
        fs_target: 0,
    });
    fw.engine.fs_target = 0x200;
    fw.engine.target = 0x100;
    // Render still running, compute reached its stamp.
    fw.graph.render.fw_stamp[1].set(0x100);
    fw.graph.compute.fw_stamp.set(0x100);
    assert!(fw.oldest_stamp_reached());
    assert_eq!(fw.graph.compute.fw_stamp.reads.get(), 1);
    assert_eq!(fw.graph.render.fw_stamp[1].reads.get(), 0);
    assert_eq!(fw.oldest_terminal_events(), Some((Some(0), Some(0))));
}

#[test]
fn oldest_stamp_reached_skips_inactive_class_wc_reads() {
    // Compute-only workload never reads render fw_stamp.
    let mut fc = Firmware::new();
    fc.engine.flights.push(Flight {
        computes: 1,
        renders: 0,
        target: 0x200,
        fs_target: 0,
    });
    fc.engine.target = 0x200;
    fc.graph.compute.fw_stamp.set(0x100);
    assert!(!fc.oldest_stamp_reached());
    assert_eq!(fc.graph.compute.fw_stamp.reads.get(), 1);
    assert_eq!(fc.graph.render.fw_stamp[0].reads.get(), 0);
    assert_eq!(fc.graph.render.fw_stamp[1].reads.get(), 0);

    // Render-only workload never reads compute fw_stamp.
    let mut fr = Firmware::new();
    fr.engine.flights.push(Flight {
        computes: 0,
        renders: 1,
        target: 0,
        fs_target: 0x200,
    });
    fr.engine.fs_target = 0x200;
    fr.graph.render.fw_stamp[1].set(0x100);
    assert!(!fr.oldest_stamp_reached());
    assert_eq!(fr.graph.compute.fw_stamp.reads.get(), 0);
    assert_eq!(fr.graph.render.fw_stamp[0].reads.get(), 0);
    assert_eq!(fr.graph.render.fw_stamp[1].reads.get(), 1);
}
'''

for token, decl in {
    '__SUBMIT_LOCKED__': 'fn submit_locked(',
    '__POLL_STEP__': 'pub(crate) fn poll_step(',
    '__OLDEST_TERMINAL_EVENTS__': 'fn oldest_terminal_events(',
    '__OLDEST_STAMP_REACHED__': 'fn oldest_stamp_reached(',
}.items():
    harness = harness.replace(token, item(decl))

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
            'tests': 4,
            'cpu_only': True,
            'hardware_execution': False,
            'scope': 'Verbatim production submit_locked inline retirement and poll_step TA-only non-spin logic.',
            'source_sha256': sha(production),
        },
        indent=2,
    )
    + '\n'
)
