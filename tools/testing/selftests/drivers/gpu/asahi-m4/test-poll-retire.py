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
source = production.read_text()
output = output_dir()

assert 'const EVENT_RETIRE_POLLS: u32 = 11;' in source, 'EVENT_RETIRE_POLLS expected to be 11'
assert (
    'OwnedQueue::new_percpu_reclaim(c_str!("aurora_g16_events"), true)' in mod_source
), 'aurora_g16_events must use new_percpu_reclaim(..., true)'
assert (
    'pub(crate) usage: Arc<crate::usage::Client>' in queue_source
), 'QueueState::usage must reference the per-file usage::Client'
assert (
    '.add_union(engine, start, end, super::hw::COMMAND_TIMESTAMP_HZ as u32)' in source
), 'G16 poll retirement must deduplicate per-file intervals via Client::add_union'


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
    slot_wait: bool,
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
    publish_wake: AtomicBool,
    rearmed: Cell<u32>,
    idle_power_polls: Cell<u32>,
}
impl Shared {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            sleeping: AtomicBool::new(false),
            evented: AtomicBool::new(false),
            publish_wake: AtomicBool::new(false),
            rearmed: Cell::new(0),
            idle_power_polls: Cell::new(0),
        })
    }
    fn rearm(&self, _soon: bool) {
        self.rearmed.set(self.rearmed.get() + 1);
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
struct Slots {
    scrub_woke: AtomicBool,
}
impl Slots {
    fn take_scrub_woke(&self) -> bool {
        self.scrub_woke.swap(false, Ordering::AcqRel)
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
    slots: Slots,
    last_housekeeping_ns: u64,
    // Test script knobs for retire_once / publish / housekeeping observation.
    retire_on_call: bool,
    unfreeze_on_hang_check: bool,
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
                slot_wait: false,
                short_repoll: false,
                frozen: false,
                stats: Stats::default(),
                prof: Prof::default(),
            },
            events: Events::default(),
            recovery: Recovery::default(),
            power: Power::default(),
            slots: Slots::default(),
            last_housekeeping_ns: 0,
            retire_on_call: false,
            unfreeze_on_hang_check: false,
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
        if self.unfreeze_on_hang_check {
            self.engine.frozen = false;
            self.recovery.pending = false;
        }
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

#[test]
fn poll_step_skips_publish_when_pending_blocked_and_nothing_retired() {
    CLOCK.with(|c| c.set(100_000));
    let mut fw = Firmware::new();
    fw.engine.flights.push(Flight {
        computes: 0,
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
    const STAGE_EVENTS: [u32; 3] = [0x70, 0x71, 15];
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
        event_ns: [u64; 3],
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
        // 3 consecutive type-1 stage events: TA (0x70 -> word 4 bit 16), FS (0x71 -> word 4 bit 17), Compute (15 -> word 1 bit 15).
        let mut r0 = [0u32; RECORD_WORDS];
        r0[0] = 1;
        r0[4] = 1 << 16;
        let mut r1 = [0u32; RECORD_WORDS];
        r1[0] = 1;
        r1[4] = 1 << 17;
        let mut r2 = [0u32; RECORD_WORDS];
        r2[0] = 1;
        r2[1] = 1 << 15;
        ring.put_record(0, &r0);
        ring.put_record(1, &r1);
        ring.put_record(2, &r2);
        ring.set_indices(0, 3);

        let mut ev = Events::default();
        ev.drain(&device::Device, &ring).unwrap();
        assert_eq!(ev.event_ns[0], 12345);
        assert_eq!(ev.fs_events, 1);
        assert_eq!(ev.compute_events, 1);
        assert_eq!(ev.stage_events, 3);
        assert_eq!(ring.read_idx(), 3);
        // Exactly ONE STATE_READ write for all 3 records!
        assert_eq!(ring.state.writes.get(), 1);
        // Exactly 3 WC reads per type-1 record (kind, word 1, word 4) = 9 reads (not 54)!
        assert_eq!(ring.ring.reads.get(), 9);
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
        __REARM__
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

        // cancel_resnap unconditionally clears deadline and drops handle.
        shared.cancel_resnap();
        assert_eq!(shared.resnap_deadline_ns.load(Ordering::Acquire), 0);
        assert!(shared.resnap_handle.lock().is_none());
        assert_eq!(shared.drops.load(Ordering::Acquire), 2);
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
'''

harness = harness.replace(
    '__USAGE_PATH__', str(kernel_root() / 'drivers/gpu/drm/asahi/usage.rs')
)

for token, decl in {
    '__SUBMIT_LOCKED__': 'fn submit_locked(',
    '__POLL_STEP__': 'pub(crate) fn poll_step(',
    '__OLDEST_TERMINAL_EVENTS__': 'fn oldest_terminal_events(',
    '__OLDEST_STAMP_REACHED__': 'fn oldest_stamp_reached(',
    '__CANCEL_RESNAP__': 'pub(crate) fn cancel_resnap(',
    '__REARM__': 'pub(crate) fn rearm(',
}.items():
    harness = harness.replace(token, item(decl))
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
            'tests': 12,
            'cpu_only': True,
            'hardware_execution': False,
            'scope': 'Verbatim production submit_locked inline retirement and poll_step TA-only non-spin logic.',
            'source_sha256': sha(production),
        },
        indent=2,
    )
    + '\n'
)
