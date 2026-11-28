#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""G17 compute work whose completion stamp never comes: retired, not failed.

The firmware can write a compute command's completion record and then never
store its completion stamp, for example while the rest of its ioctl does not
complete. The real compute queue retirement pass, the compute watchdog's
decision and timeout paths and the real timeout policy (timeout.rs) are
compiled against mock firmware state. A watchdog that finds its command
finished retires it on its record instead of failing its client; the commands
behind it then retire past it; every retired command keeps its place and its
descriptor until a stamp covers it; work that cannot retire that way is failed
as before. Every retired kick with an exact execution record, and only those,
feeds its channel's run of heavy kicks once (g17/pairs.rs), which gates sibling
compute fan-out on M5 Pro. --mutation removes one part of that and must fail.
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
            line = source.rfind('\n', 0, start - 1) + 1
            if not source[line:start].strip().startswith('#['):
                break
            start = line
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
    retirement = (G17 / 'queue/compute/retirement.rs').read_text()
    compute = (G17 / 'queue/compute.rs').read_text()
    control = (G17 / 'runtime/compute_control.rs').read_text()
    timeout = (G17 / 'timeout.rs').read_text()
    depth = re.search(r'pub\(crate\) const COMPUTE_DEPTH: usize = (\d+);',
                      (ROOT / 'hw/t8140.rs').read_text()).group(1)
    retired = '\n'.join([
        f'pub(crate) const IN_FLIGHT: usize = {depth};',
        item(retirement, 'pub(crate) struct Ticket {', True),
        item(retirement, 'pub(crate) struct Observation {', True),
        item(retirement, 'pub(crate) struct Cursors {', True),
        item(retirement, 'impl Cursors {'),
        item(retirement, 'pub(crate) const fn stamp_covers('),
        item(retirement, 'pub(crate) enum Recordless {', True),
        line(retirement, 'pub(crate) const STAMP_FALLBACK_NS:'),
        item(retirement, 'pub(crate) fn record_aged('),
        item(retirement, 'pub(crate) enum StampWait {', True),
        line(retirement, 'pub(crate) const STAMP_WAIT_EXPIRED:'),
        item(retirement, 'pub(crate) fn stamp_wait('),
        line(retirement, 'static STAMPLESS:'),
        item(retirement, 'pub(crate) fn note_stampless('),
        line(retirement, 'pub(crate) const CANCELLED_TICKS:'),
        item(retirement, 'pub(crate) fn unexecuted('),
        item(retirement, 'pub(crate) fn poll('),
    ])
    queue = '\n'.join(item(compute, signature) for signature in [
        'pub(crate) fn timeout_head(', 'fn front_finished(', 'fn selected(',
        'pub(crate) fn quarantine(', 'pub(crate) fn retire(', 'fn classify_unexecuted(',
        'pub(crate) fn owns_packet(', 'pub(crate) fn retired_held(',
        'pub(crate) fn expire_stamp_wait(', 'pub(crate) fn timeout(',
        'pub(crate) fn can_classify_timeout('])
    pairs = (G17 / 'pairs.rs').read_text()
    hz = re.search(r'pub\(crate\) const COMMAND_TIMESTAMP_HZ: u64 = ([0-9_]+);',
                   (ROOT / 'hw/t8140.rs').read_text()).group(1)
    gate = '\n'.join([f'pub(crate) const COMMAND_TIMESTAMP_HZ: u64 = {hz};'] + [
        re.search(r'(?:pub\(crate\) )?const ' + name + r': [^=]+= [^;]+;', pairs).group(0)
        for name in ['SIBLING_EXECUTION_NS', 'SIBLING_EXECUTION_TICKS', 'SIBLING_SUSTAINED_KICKS']] + [
        item(pairs, 'pub(crate) fn heavy_run('), item(pairs, 'pub(crate) fn sustained(')]
    ).replace('crate::hw::t8140::resources::', '')
    backend = '\n'.join([
        item(control, 'pub(super) fn defer_compute_timeout(').replace('pub(super) ', '', 1),
        item(control, 'fn retire_on_record('),
        item(control, 'pub(super) fn timeout_compute(').replace('pub(super) ', '', 1),
    ])
    return r'''
#![allow(warnings)]
use std::cell::{Cell, RefCell, RefMut};
use std::collections::VecDeque;
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(i32);
pub type Result<T = ()> = std::result::Result<T, Error>;
pub const EIO: Error = Error(-5);
pub const EAGAIN: Error = Error(-11);
pub const ENOMEM: Error = Error(-12);
pub const EINVAL: Error = Error(-22);
pub const ENODATA: Error = Error(-61);
pub const ETIMEDOUT: Error = Error(-110);

thread_local! { static NOW: Cell<u64> = Cell::new(1_000_000_000); }
pub fn now_ns() -> u64 { NOW.with(|now| now.get()) }
fn advance(ns: u64) { NOW.with(|now| now.set(now.get() + ns)); }

macro_rules! pr_info { ($($t:tt)*) => {}; }
macro_rules! dev_info { ($($t:tt)*) => {}; }
macro_rules! dev_warn { ($($t:tt)*) => {}; }

mod g17 {
    pub(crate) fn now_ns() -> u64 { crate::now_ns() }
    pub(crate) use crate::Firmware;
    pub(crate) mod timeout {
''' + timeout + r'''
    }
    pub(crate) mod recovery {
        pub(crate) fn note_retired() {}
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub(crate) enum Class { Spared, Guilty }
    }
    pub(crate) mod trace {
        pub(crate) const COMPUTE: u8 = 1;
        pub(crate) fn complete(_: u8, _: u8, _: u32, _: &std::sync::Arc<crate::Packet>, _: [u64; 2]) {}
    }
    pub(crate) mod job {
        pub(crate) trait Backend { fn fail_vm(&self, error: crate::Error); }
    }
    pub(crate) mod pairs {
''' + gate + r'''
    }
}
mod module_parameters {
    pub(crate) struct Parameter(pub(crate) u32);
    impl Parameter { pub(crate) fn value(&self) -> u32 { self.0 } }
    pub(crate) static compute_timeout_max_ms: Parameter = Parameter(30_000);
}
mod qos { pub(crate) struct Owner; }
mod retirement {
    use crate::{Result, EIO, ENODATA};
''' + retired + r'''
}
use g17::{job, recovery};
use retirement::{Cursors, Observation, Recordless, Ticket, IN_FLIGHT};

pub struct Flags;
pub const GFP_KERNEL: Flags = Flags;
pub struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn with_capacity(capacity: usize, _: Flags) -> Result<Self> { Ok(Self(Vec::with_capacity(capacity))) }
    fn push_within_capacity(&mut self, value: T) -> std::result::Result<(), T> { self.0.push(value); Ok(()) }
}
impl<T> Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }

pub struct VmStatus;
pub struct Completion {
    result: RefCell<Option<Result<[u64; 4]>>>,
    class: Cell<Option<recovery::Class>>,
    status: Arc<VmStatus>,
}
impl Completion {
    fn new() -> Self { Self { result: RefCell::new(None), class: Cell::new(None), status: Arc::new(VmStatus) } }
    /// The first result signals the fence; later ones change nothing.
    fn signal(&self, result: Result<[u64; 4]>) {
        let mut slot = self.result.borrow_mut();
        if slot.is_none() { *slot = Some(result); }
    }
    fn spared(&self) -> bool { self.class.get() == Some(recovery::Class::Spared) }
    fn guilty(&self) -> bool { self.class.get() == Some(recovery::Class::Guilty) }
    fn classify(&self, class: recovery::Class) { self.class.set(Some(class)); }
    fn status(&self) -> &Arc<VmStatus> { &self.status }
}
pub enum Deferred {
    Retired(Arc<Completion>, Result<[u64; 4]>),
    RetiredCompute(Arc<Completion>, [u64; 4], Option<[u64; 2]>),
    FailedOwned(Arc<Completion>, Error),
}
impl Deferred {
    fn finish(self) {
        match self {
            Self::Retired(completion, result) => completion.signal(result),
            Self::RetiredCompute(completion, ticks, _) => completion.signal(Ok(ticks)),
            Self::FailedOwned(completion, error) => completion.signal(Err(error)),
        }
    }
}
pub struct DeferredBatch(Vec<Deferred>);
impl DeferredBatch {
    fn worker() -> Result<Self> { Ok(Self(Vec::new())) }
    fn push(&mut self, event: Deferred) -> Result { self.0.push(event); Ok(()) }
    fn finish(&mut self) { for event in self.0.drain(..) { event.finish(); } }
}

pub struct Vm(u64);
impl Vm { fn id(&self) -> u64 { self.0 } }
pub struct Context { vm: Vm }
impl Context { fn vm(&self) -> &Vm { &self.vm } }
pub struct Order { sequence: u64 }
pub struct Packet { completion: Arc<Completion>, order: Order, context: Context }
impl Packet {
    fn check_firmware_producers(&self) -> Result { Ok(()) }
    fn within_allowance(&self, _: u64) -> bool { false }
}
pub struct WorkStateLease { drained: Cell<bool> }
impl WorkStateLease { fn drained(&self) -> Result<bool> { Ok(self.drained.get()) } }

/// The queue's stamp word: each read takes the next queued value, then keeps the last.
pub struct Graph { m5: bool, stamps: RefCell<VecDeque<u32>>, stamp: Cell<u32>, cursors: Cell<Cursors> }
impl Graph {
    fn stamp(&self) -> Result<u32> {
        if let Some(next) = self.stamps.borrow_mut().pop_front() { self.stamp.set(next); }
        Ok(self.stamp.get())
    }
    fn cursors(&self) -> Result<Cursors> { Ok(self.cursors.get()) }
}
pub struct Previous { publication: Option<(u64, u64)> }

''' + item(compute, 'struct Fifo<T> {') + '\n' + item(compute, 'impl<T> Fifo<T> {') + '\n' + item(compute, 'struct Active {') + r'''

pub struct Queue {
    active: Fifo<Active>,
    graph: Graph,
    quarantined: bool,
    quarantine_error: Option<Error>,
    failure_status_pending: bool,
    retirement_proved: bool,
    retired_by_teardown: bool,
    spared_quarantine: bool,
    spared_deferred: bool,
    awaiting_witness: bool,
    retire_pending: bool,
    last_end: u64,
    previous: Option<Previous>,
    submitted: u32,
    retirement_ready: bool,
    released: bool,
    owner: Option<u64>,
    released_at: u64,
    heavy_run: u8,
    terminal: Option<(Arc<VmStatus>, Error)>,
    fault_stamp: Option<u32>,
    timed_out_at: u64,
}
impl Queue {
    fn qid(&self) -> u8 { 28 }
    fn owner(&self) -> Option<u64> { self.owner }
    fn retire_configuration(&mut self, _: u64) -> Result { Ok(()) }
    fn valid_ticket(&self, _: &Active, _: &Ticket) -> Result { Ok(()) }
    fn qos_owner(&self) -> qos::Owner { qos::Owner }
    fn classify(&self, _: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {}
    fn clear_owner(&mut self, stamp_released_at: bool) {
        self.owner = None;
        if stamp_released_at { self.released_at = now_ns(); }
    }
''' + queue + r'''
}

pub struct QueueCell(RefCell<Queue>);
impl QueueCell { fn as_deref(&self) -> Option<RefMut<'_, Queue>> { Some(self.0.borrow_mut()) } }
pub struct Entry { queue: QueueCell }
pub struct Queues { compute: Vec<Option<Entry>>, render: Vec<g17::timeout::Head> }
impl Queues {
    fn compute(&self, owner: u64) -> Option<RefMut<'_, Queue>> {
        self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.owner() == Some(owner))
    }
    fn compute_mut(&mut self, owner: u64) -> Option<RefMut<'_, Queue>> { self.compute(owner) }
    fn compute_for_packet(&self, owner: u64, packet: &Arc<Packet>) -> Option<RefMut<'_, Queue>> {
        self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.owner() == Some(owner) && queue.owns_packet(packet))
    }
    fn compute_for_packet_mut(&mut self, owner: u64, packet: &Arc<Packet>) -> Option<RefMut<'_, Queue>> {
        self.compute_for_packet(owner, packet)
    }
    /// Other VMs' heads, as render pairs and other queues give them.
    fn render_timeout_heads(&self) -> impl Iterator<Item = g17::timeout::Head> + '_ {
        self.render.iter().copied()
    }
}
pub struct Init;
pub struct Firmware { queues: Queues, init: Init, epoch: u64 }
impl Firmware {
    /// The device's retirement pass over every compute queue.
    fn scan_compute(&mut self, masks: Option<[u64; 2]>, deferred: &mut DeferredBatch) -> Result {
        for entry in self.queues.compute.iter().flatten() {
            let mut queue = entry.queue.as_deref().ok_or(EIO)?;
            queue.retire(masks, self.epoch, 0, |_| Ok(()), &mut |event| deferred.push(event))?;
        }
        Ok(())
    }
}
pub struct State(RefCell<Option<Box<Firmware>>>);
impl State { fn lock(&self) -> RefMut<'_, Option<Box<Firmware>>> { self.0.borrow_mut() } }
pub struct Shared { state: State, dev: () }
pub struct Backend { owner: u64, shared: Shared, failed: Cell<Option<Error>> }
impl job::Backend for Backend { fn fail_vm(&self, error: Error) { self.failed.set(Some(error)); } }
impl Backend {
    /// No progress word: nothing is renewed for the context's own older node.
    fn behind_own_head(&self, _: &Arc<Packet>, _: &Init, _: u8) -> Option<(u32, u32)> { None }
''' + backend + r'''
}

#[cfg(test)]
mod owed {
    use super::*;
    use crate::g17::timeout::{Decision, Head, MAX_RENEWALS};
    use crate::retirement::STAMP_WAIT_EXPIRED;

    const OWNER: u64 = 7;
    const EPOCH: u64 = 1;
    const MS: u64 = 1_000_000;

    fn stamp_of(kick: u64) -> u32 { (kick as u32) << 8 }

    fn queue(m5: bool) -> Queue {
        Queue {
            active: Fifo::new().unwrap(),
            graph: Graph { m5, stamps: RefCell::new(VecDeque::new()), stamp: Cell::new(stamp_of(0x0f)),
                cursors: Cell::new(Cursors { producer: 4, consumer: 4, count: 64 }) },
            quarantined: false, quarantine_error: None, failure_status_pending: false,
            retirement_proved: false, retired_by_teardown: false, spared_quarantine: false,
            spared_deferred: false, awaiting_witness: false, retire_pending: false, last_end: 0,
            previous: None, submitted: 0, retirement_ready: true, released: false, owner: Some(OWNER),
            released_at: 0, heavy_run: 0, terminal: None, fault_stamp: None, timed_out_at: 0,
        }
    }

    /// Publishes the command of kick `kick`; `drained`: the rest of its ioctl completed.
    fn publish(queue: &mut Queue, kick: u64, drained: bool) -> Arc<Packet> {
        let packet = Arc::new(Packet { completion: Arc::new(Completion::new()),
            order: Order { sequence: kick }, context: Context { vm: Vm(70) } });
        let ticket = Ticket { owner: OWNER, submission: kick, context_id: 1, qid: 28,
            descriptor: 0x1000 + kick, work_node: 0x2000, kick, ordinal: kick as u32,
            item_producer: 4, item_count: 64, epoch: EPOCH };
        let active = Active { packet: packet.clone(), published: kick,
            work: Arc::new(WorkStateLease { drained: Cell::new(drained) }), ticket: Some(ticket),
            observed: None, observed_at: None, failure_deferred: false, qos_pending: false,
            spared_as_dropped: false, stamps_owed: false };
        assert!(queue.active.push(active).is_ok());
        queue.submitted += 1;
        queue.retirement_ready = false;
        packet
    }

    /// The scan latched `packet`'s completion record now.
    fn record(queue: &mut Queue, packet: &Arc<Packet>, timestamps: [u64; 2]) {
        let active = queue.active.iter_mut().find(|active| Arc::ptr_eq(&active.packet, packet)).unwrap();
        active.observed = Some(Observation { timestamps });
        active.observed_at = Some(now_ns());
    }

    /// Other VMs' heads are given as `others`.
    fn backend(queue: Queue, others: Vec<Head>) -> Backend {
        let firmware = Firmware {
            queues: Queues { compute: vec![Some(Entry { queue: QueueCell(RefCell::new(queue)) })],
                render: others },
            init: Init, epoch: EPOCH };
        Backend { owner: OWNER, shared: Shared { state: State(RefCell::new(Some(Box::new(firmware)))), dev: () },
            failed: Cell::new(None) }
    }

    fn with_firmware<R>(backend: &Backend, f: impl FnOnce(&mut Firmware) -> R) -> R {
        let mut state = backend.shared.state.lock();
        f((*state).as_deref_mut().unwrap())
    }

    fn with_queue<R>(backend: &Backend, f: impl FnOnce(&mut Queue) -> R) -> R {
        with_firmware(backend, |firmware| {
            let entry = firmware.queues.compute[0].as_ref().unwrap();
            let mut queue = entry.queue.as_deref().unwrap();
            f(&mut queue)
        })
    }

    /// A retirement pass, its completions signalled afterwards.
    fn scan(backend: &Backend) {
        let mut deferred = DeferredBatch::worker().unwrap();
        with_firmware(backend, |firmware| firmware.scan_compute(None, &mut deferred)).unwrap();
        deferred.finish();
    }

    fn result(packet: &Arc<Packet>) -> Option<Result<[u64; 4]>> {
        *packet.completion.result.borrow()
    }

    fn class(packet: &Arc<Packet>) -> Option<recovery::Class> {
        packet.completion.class.get()
    }

    /// Execution record ticks of a kick lasting `us` microseconds.
    fn ticks(us: u64) -> u64 { us * g17::pairs::COMMAND_TIMESTAMP_HZ / 1_000_000 }

    #[test]
    fn only_exact_execution_records_feed_the_heavy_run() {
        let heavy = ticks(g17::pairs::SIBLING_EXECUTION_NS / 1_000);
        let mut q = queue(true);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        let p3 = publish(&mut q, 0x12, true);
        record(&mut q, &p1, [1_000, 1_000 + heavy]);
        record(&mut q, &p2, [1_000 + heavy, 1_000 + 3 * heavy]);
        // p3 retires without a record: its synthesized envelope measures nothing.
        q.graph.stamp.set(stamp_of(0x12));
        let b = backend(q, vec![]);
        scan(&b);
        assert!(result(&p3).is_some_and(|result| result.is_ok()));
        with_queue(&b, |q| {
            assert_eq!(q.active.len, 0);
            assert_eq!(q.heavy_run, 2, "two exact heavy records, nothing from the recordless kick");
            assert!(g17::pairs::sustained(q.heavy_run));
        });
        // One short record ends the run.
        let p4 = with_queue(&b, |q| {
            let p4 = publish(q, 0x13, true);
            record(q, &p4, [9_000, 9_000 + ticks(3)]);
            q.graph.stamp.set(stamp_of(0x13));
            p4
        });
        scan(&b);
        assert!(result(&p4).is_some());
        with_queue(&b, |q| assert_eq!(q.heavy_run, 0, "a short kick closes the gate"));
    }

    #[test]
    fn a_kick_held_for_its_stamp_is_measured_once() {
        let heavy = ticks(g17::pairs::SIBLING_EXECUTION_NS / 1_000);
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 100 + heavy]);
        record(&mut q, &p2, [100 + heavy, 100 + 2 * heavy]);
        let b = backend(q, vec![]);
        // Late stamps: both retire on their records and stay held for their stamps.
        advance(60 * MS);
        scan(&b);
        assert!(result(&p1).is_some() && result(&p2).is_some());
        with_queue(&b, |q| assert_eq!((q.active.len, q.heavy_run), (2, 2)));
        with_queue(&b, |q| q.graph.stamp.set(stamp_of(0x11)));
        scan(&b);
        with_queue(&b, |q| assert_eq!((q.active.len, q.heavy_run), (0, 2),
            "releasing a held descriptor measures nothing again"));
    }

    #[test]
    fn a_finished_head_whose_stamp_never_comes_retires_and_the_queue_moves_on() {
        let mut q = queue(false);
        // The rest of the first command's ioctl never completes, so its stamp never comes.
        let p1 = publish(&mut q, 0x10, false);
        let p2 = publish(&mut q, 0x11, true);
        let p3 = publish(&mut q, 0x12, true);
        record(&mut q, &p1, [100, 200]);
        record(&mut q, &p2, [210, 300]);
        let b = backend(q, vec![]);
        for _ in 0..16 {
            advance(2_000 * MS);
            scan(&b);
        }
        for p in [&p1, &p2, &p3] {
            assert_eq!(result(p), None, "the stamp wait restarts while the ioctl runs");
        }
        // Its watchdog gives up (job.rs): the head finished, so it retires on its record.
        assert_eq!(b.defer_compute_timeout(&p1, 0, 0), Decision::Retire,
            "a finished head is retired, not failed");
        assert_eq!(result(&p1), Some(Ok([100, 200, 0, 0])), "its client gets its result");
        assert_eq!(result(&p2), Some(Ok([210, 300, 0, 0])),
            "the next command retires past it in the same pass");
        assert_eq!(result(&p3), None, "a command without a record still runs");
        assert_eq!(b.failed.get(), None);
        with_queue(&b, |q| {
            assert!(!q.quarantined);
            assert_eq!(q.active.len, 3, "retired commands keep their place until a stamp covers them");
            assert_eq!(q.submitted, 3, "and their descriptors");
            assert!(q.retired_held(&p1) && q.retired_held(&p2) && !q.retired_held(&p3));
            assert_eq!(q.timeout_head().map(|head| (head.published, head.finished)), Some((0x12, false)),
                "the head is the command still running");
            // The last command finishes with its stamp, which covers the ones before it.
            record(q, &p3, [310, 400]);
            q.graph.stamp.set(stamp_of(0x12));
        });
        scan(&b);
        assert_eq!(result(&p3), Some(Ok([310, 400, 0, 0])));
        with_queue(&b, |q| {
            assert_eq!((q.active.len, q.submitted), (0, 0), "the stamp releases every held descriptor");
            assert!(q.retirement_ready);
        });
    }

    #[test]
    fn retired_commands_keep_their_descriptors_until_the_stamp_covers_them() {
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 200]);
        record(&mut q, &p2, [210, 300]);
        let b = backend(q, vec![]);
        advance(60 * MS);
        // Both stamps are late past the fallback bound: one pass retires both.
        scan(&b);
        assert_eq!(result(&p1), Some(Ok([100, 200, 0, 0])));
        assert_eq!(result(&p2), Some(Ok([210, 300, 0, 0])), "the pass steps past the first");
        for _ in 0..4 {
            with_queue(&b, |q| {
                assert_eq!((q.active.len, q.submitted), (2, 2), "nothing is released without the stamp");
                assert!(q.retired_held(&p1) && q.retired_held(&p2));
                assert!(q.retire_pending, "owed stamps are polled for");
                assert_eq!(q.timeout_head(), None, "retired commands are no head");
            });
            advance(1_000 * MS);
            scan(&b);
        }
        with_queue(&b, |q| q.graph.stamp.set(stamp_of(0x10)));
        scan(&b);
        with_queue(&b, |q| {
            assert_eq!((q.active.len, q.submitted), (1, 1), "a stamp releases only what it covers");
            assert!(!q.owns_packet(&p1) && q.retired_held(&p2));
            q.graph.stamp.set(stamp_of(0x11));
        });
        scan(&b);
        with_queue(&b, |q| assert_eq!((q.active.len, q.submitted), (0, 0)));
        assert_eq!(result(&p2), Some(Ok([210, 300, 0, 0])), "it was reported once");
    }

    #[test]
    fn work_that_cannot_retire_on_its_record_is_failed_as_before() {
        // No record: the oldest unfinished work is its own.
        let mut q = queue(false);
        let p = publish(&mut q, 0x10, true);
        let b = backend(q, vec![]);
        assert_eq!(b.defer_compute_timeout(&p, 0, 0), Decision::Own);
        // Behind another VM's older failed work: deferred for the grace, then its own.
        let failed = Head { vm: 5, published: 0x01, finished: false, failed: true, excused: false };
        let mut q = queue(false);
        let p = publish(&mut q, 0x10, true);
        let b = backend(q, vec![failed]);
        for renewals in 0..MAX_RENEWALS {
            assert_eq!(b.defer_compute_timeout(&p, renewals, 0), Decision::Defer);
        }
        assert_eq!(b.defer_compute_timeout(&p, MAX_RENEWALS, 0), Decision::Own);
        assert_eq!(result(&p), None);
        // A record whose ring entries the firmware has not consumed does not retire.
        let mut q = queue(false);
        let p = publish(&mut q, 0x10, false);
        record(&mut q, &p, [100, 200]);
        q.graph.cursors.set(Cursors { producer: 4, consumer: 2, count: 64 });
        let b = backend(q, vec![failed]);
        advance(100 * MS);
        assert_eq!(b.defer_compute_timeout(&p, 0, 0), Decision::Own,
            "a head that does not retire on its record is failed as its own");
        assert_eq!(result(&p), None);
        // Its timeout then fails it and its VM.
        assert_eq!(b.timeout_compute(&p), Ok(()));
        assert_eq!(result(&p), Some(Err(ETIMEDOUT)));
        assert_eq!(b.failed.get(), Some(ETIMEDOUT));
    }

    #[test]
    fn a_watchdog_behind_retired_commands_weighs_its_own_command() {
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 200]);
        // Another VM's older live work is ahead of the second command.
        let live = Head { vm: 5, published: 0x05, finished: false, failed: false, excused: false };
        let b = backend(q, vec![live]);
        advance(60 * MS);
        scan(&b);
        assert_eq!(result(&p1), Some(Ok([100, 200, 0, 0])));
        assert_eq!(b.defer_compute_timeout(&p2, 0, 0), Decision::Defer,
            "the second command is deferred behind the live blocker");
        assert_eq!(b.defer_compute_timeout(&p2, MAX_RENEWALS, 0), Decision::Spare);
        assert_eq!(result(&p2), None);
    }

    #[test]
    fn a_command_that_retires_in_the_timeout_pass_keeps_its_result() {
        let mut q = queue(false);
        let p = publish(&mut q, 0x10, true);
        record(&mut q, &p, [100, 200]);
        let b = backend(q, vec![]);
        // The fallback bound passed and no pass ran since.
        advance(60 * MS);
        assert_eq!(b.timeout_compute(&p), Ok(()));
        assert_eq!(result(&p), Some(Ok([100, 200, 0, 0])));
        assert_eq!(b.failed.get(), None, "its VM is not failed");
        with_queue(&b, |q| assert!(!q.quarantined));
        // Unfinished work still fails, and its VM.
        let mut q = queue(false);
        let p = publish(&mut q, 0x10, true);
        let b = backend(q, vec![]);
        assert_eq!(b.timeout_compute(&p), Ok(()));
        assert_eq!(result(&p), Some(Err(ETIMEDOUT)));
        assert_eq!(b.failed.get(), Some(ETIMEDOUT));
    }

    #[test]
    fn a_command_retired_behind_held_ones_leaves_after_them() {
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 200]);
        let b = backend(q, vec![]);
        advance(60 * MS);
        scan(&b);
        // The second command's stamp lands during the next pass, after the first one's was read.
        with_queue(&b, |q| {
            record(q, &p2, [210, 300]);
            q.graph.stamps.borrow_mut().extend([stamp_of(0x0f), stamp_of(0x11)]);
        });
        scan(&b);
        assert_eq!(result(&p2), Some(Ok([210, 300, 0, 0])));
        with_queue(&b, |q| {
            assert_eq!((q.active.len, q.submitted), (2, 2), "it waits behind the held command");
            assert!(q.retired_held(&p1) && q.retired_held(&p2));
        });
        scan(&b);
        with_queue(&b, |q| assert_eq!((q.active.len, q.submitted), (0, 0)));
    }

    #[test]
    fn a_command_behind_held_ones_that_crossed_a_recovery_waits_for_its_stamp() {
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 200]);
        let b = backend(q, vec![]);
        advance(60 * MS);
        scan(&b);
        // A recovery since: the second command comes back with the restart's cancellation record.
        with_firmware(&b, |firmware| firmware.epoch = EPOCH + 1);
        with_queue(&b, |q| record(q, &p2, [300, 301]));
        advance(60 * MS);
        scan(&b);
        assert_eq!(result(&p2), None);
        assert_eq!(class(&p2), None);
        with_queue(&b, |q| assert!(!q.quarantined, "behind held commands only a stamp retires it"));
        // Once the first command is released it is the front, settled as before.
        with_queue(&b, |q| q.graph.stamp.set(stamp_of(0x10)));
        scan(&b);
        assert_eq!(class(&p2), Some(recovery::Class::Spared), "a recovery dropped it");
        assert_eq!(class(&p1), None);
        with_queue(&b, |q| assert!(q.quarantined));
    }

    #[test]
    fn a_failed_command_behind_held_ones_leaves_their_results_alone() {
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 200]);
        let b = backend(q, vec![]);
        advance(60 * MS);
        scan(&b);
        // A zero-duration record: the firmware's report of the command's own failure.
        with_queue(&b, |q| record(q, &p2, [400, 400]));
        scan(&b);
        assert_eq!(class(&p2), Some(recovery::Class::Guilty));
        assert_eq!(result(&p2), Some(Err(ENODATA)));
        assert_eq!(class(&p1), None, "a held command ran and keeps its class");
        assert_eq!(result(&p1), Some(Ok([100, 200, 0, 0])));
        with_queue(&b, |q| assert!(q.quarantined));
    }

    #[test]
    fn after_a_rebind_its_first_publication_stays_the_one_that_retired_first() {
        let mut q = queue(false);
        q.previous = Some(Previous { publication: Some((0x10, EPOCH)) });
        let p1 = publish(&mut q, 0x10, true);
        let p2 = publish(&mut q, 0x11, true);
        record(&mut q, &p1, [100, 200]);
        record(&mut q, &p2, [210, 300]);
        let b = backend(q, vec![]);
        advance(60 * MS);
        scan(&b);
        assert_eq!(result(&p2), Some(Ok([210, 300, 0, 0])));
        with_queue(&b, |q| {
            assert!(!q.quarantined, "a command behind the held first publication retires");
            assert!(q.previous.is_some(), "the previous binding stays until the stamp");
            q.graph.stamp.set(stamp_of(0x11));
        });
        scan(&b);
        with_queue(&b, |q| assert!(q.previous.is_none() && q.active.len == 0));
        // Another command retiring first still fails the queue.
        let mut q = queue(false);
        q.previous = Some(Previous { publication: Some((0x99, EPOCH)) });
        let p = publish(&mut q, 0x10, true);
        record(&mut q, &p, [100, 200]);
        let b = backend(q, vec![]);
        advance(60 * MS);
        scan(&b);
        with_queue(&b, |q| assert!(q.quarantined));
    }

    #[test]
    fn only_the_recorded_oldest_command_has_its_stamp_wait_ended() {
        let mut q = queue(false);
        let p1 = publish(&mut q, 0x10, false);
        let p2 = publish(&mut q, 0x11, false);
        record(&mut q, &p2, [210, 300]);
        assert!(!q.expire_stamp_wait(&p1), "no record yet");
        assert!(!q.expire_stamp_wait(&p2), "not the oldest command still to retire");
        record(&mut q, &p1, [100, 200]);
        assert!(q.expire_stamp_wait(&p1));
        assert_eq!(q.active.get(0).unwrap().observed_at, Some(STAMP_WAIT_EXPIRED));
        assert_eq!(q.active.get(1).unwrap().observed_at, Some(now_ns()));
        // M5: the record can precede the command's writes.
        let mut m5 = queue(true);
        let p = publish(&mut m5, 0x10, false);
        record(&mut m5, &p, [100, 200]);
        assert!(!m5.expire_stamp_wait(&p));
        let mut failed = queue(false);
        let p = publish(&mut failed, 0x10, false);
        record(&mut failed, &p, [100, 200]);
        failed.quarantined = true;
        assert!(!failed.expire_stamp_wait(&p));
    }
}
'''


# Each restores old behaviour or drops one part of the rule.
MUTATIONS = {
    'own-finished': [('_ if own.finished => Decision::Retire,', '_ if own.finished => Decision::Own,')],
    'no-retire-glue': [('verdict.decision = self.retire_on_record(firmware, packet, &mut deferred)?;', '')],
    'retire-unchecked': [('if !firmware\n            .queues\n            .compute_for_packet(self.owner, packet)\n'
                          '            .is_some_and(|queue| queue.retired_held(packet))\n        {',
                          'if false {')],
    'no-expiry': [('if observed_at == Some(STAMP_WAIT_EXPIRED) {', 'if false {')],
    'no-step-past': [('Ok(Some(_)) | Err(EAGAIN) if owed => at += 1,', 'Ok(Some(_)) | Err(EAGAIN) if owed => break,'),
                     ('                        at += 1;\n                        continue;\n', '                        break;\n')],
    'pop-owed': [('return if retirement::stamp_covers(stamp, ticket.kick) {', 'return if true {')],
    'head-includes-owed': [('let head = self.active.iter().find(|active| !active.stamps_owed);',
                            'let head = self.active.front();')],
    'held-stamped-pops': [('if stampless || at != 0 {', 'if stampless {')],
    'crossed-behind-owed': [('                    if at != 0 {\n                        // Only its stamp retires it now',
                             '                    if false {\n                        // Only its stamp retires it now')],
    'owed-classified': [('if active.stamps_owed || completion.spared() || completion.guilty() {',
                         'if completion.spared() || completion.guilty() {')],
    'rebind-check-entry': [('self.previous.as_ref().is_some_and(|p| p.publication != first)',
                            'self.previous.as_ref().is_some_and(|p| p.publication != Some((front.packet.order.sequence, ticket.epoch)))')],
    'timeout-fails-retired': [('if !queue.owns_packet(packet) || queue.retired_held(packet) {',
                               'if !queue.owns_packet(packet) {')],
    'no-owed-poll': [('        if at != 0 {\n            // Owed stamps come without a completion event',
                      '        if false {\n            // Owed stamps come without a completion event')],
    # A recordless retirement's synthesized envelope counts as an execution.
    'count-recordless': [('let execution = front.observed.map(|record| record.timestamps);',
                          'let execution = Some(timestamps);')],
    # Releasing a held descriptor measures the kick again.
    'count-owed-twice': [('                    // The stamp now covers the held command: release its descriptor.\n',
                          '                    // The stamp now covers the held command: release its descriptor.\n'
                          '                    self.heavy_run = crate::g17::pairs::heavy_run(self.heavy_run, 0, u64::MAX);\n')],
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = ap.parse_args()
    code = fixture()
    for old, new in MUTATIONS.get(args.mutation, []):
        assert code.count(old) == 1, (args.mutation, old)
        code = code.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='g17-compute-owed-') as tmp:
        path = Path(tmp) / 'owed.rs'
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
