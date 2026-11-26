#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host test: compute channels given up under the device mutex never leak a reservation.

A logical compute queue holds up to `COMPUTE_CHANNELS_MAX` channels. When an
unpublished compute packet fails, `Backend::rollback_unpublished_compute()`
rolls back every channel of the owner, and before primary pool growth
`Registry::drain_compute_siblings()` releases every idle sibling. Both take
queues out of the registry under the device mutex. Each taken queue keeps an
empty registry entry, and so its QID, until it is dropped off-lock and its
reservation is cancelled (`finish_uninstalled_compute()`). An entry left empty
for good locks its owner out: `reserve_compute_channel()` refuses that owner
with `EINVAL`.

This test compiles, against host stubs, the production code from
drivers/gpu/drm/asahi/g17/runtime.rs (`Registry::drain_compute_siblings()`,
`Registry::rollback_unpublished_compute()`, `Registry::reserve_compute_channel()`,
`Registry::reserved()`, `Registry::cancel_compute()`,
`Backend::finish_uninstalled_compute()`, `Backend::finish_retired_compute()`)
and from runtime/backend.rs (`Backend::rollback_unpublished_compute()`). It
checks, for an owner with all `COMPUTE_CHANNELS_MAX` channels, that:

- nothing a channel gives up (queue or displaced binding) is dropped while the
  device mutex is held;
- every cancelled or drained queue has its reservation cancelled, so the owner
  can reserve channels again;
- this also holds when a later channel fails its rollback or is still busy in
  the drain (`EAGAIN`), and when finishing one queue fails;
- an owner with more channels than places is refused before any extra channel
  changes.

Each mutation drops one part of that and must fail an assertion.
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


def line(source, prefix):
    found = [text for text in source.splitlines() if text.startswith(prefix)]
    assert len(found) == 1, prefix
    return found[0]


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


def build_harness(mutation=None):
    runtime = (G17 / 'runtime.rs').read_text()
    backend = (G17 / 'runtime/backend.rs').read_text()
    max_line = line((G17 / 'pairs.rs').read_text(), 'pub(crate) const COMPUTE_CHANNELS_MAX: usize')

    aliases = '\n'.join([
        line(runtime, 'type Uninstalled ='),
        line(runtime, 'type PerChannel<T> ='),
    ]).replace('super::pairs::', '')
    drain = item(runtime, '    fn drain_compute_siblings(')
    rollback = item(runtime, '    fn rollback_unpublished_compute(\n')
    reserve = item(runtime, '    pub(super) fn reserve_compute_channel(')
    reserved = item(runtime, '    fn reserved(&mut self, reservation: ComputeReservation)')
    cancel = item(runtime, '    pub(super) fn cancel_compute(')
    finish_one = item(runtime, '    fn finish_uninstalled_compute(')
    finish_all = item(runtime, '    fn finish_retired_compute(')
    backend_rollback = item(backend, '    fn rollback_unpublished_compute(&self)')

    if mutation == 'queue-under-lock':
        # A cancelled queue is dropped where it is taken, as when a push into an empty KVec failed.
        rollback = replace_once(rollback,
                                '*place = entry.queue.take().map(|queue| (entry.reservation, queue));',
                                'drop(entry.queue.take());')
    elif mutation == 'binding-under-lock':
        rollback = replace_once(rollback, '*binding = replaced;', 'drop(replaced);')
    elif mutation == 'drain-under-lock':
        drain = replace_once(drain,
                             '*place = entry.queue.take().map(|queue| (entry.reservation, queue));',
                             'drop(entry.queue.take());')
    elif mutation == 'drain-local-on-eagain':
        # The old shape: collect locally and hand the queues over only on success.
        drain = replace_once(drain, 'retired: &mut PerChannel<Uninstalled>,', 'outer: &mut PerChannel<Uninstalled>,')
        drain = replace_once(drain, '        let mut channel = 0;\n',
                             '        let mut local: PerChannel<Uninstalled> = Default::default();\n'
                             '        let retired = &mut local;\n'
                             '        let mut channel = 0;\n')
        drain = drain[:drain.rindex('Ok(())')] + 'core::mem::swap(outer, retired);\n        Ok(())\n    }'
    elif mutation == 'finish-stops-at-error':
        finish_all = (
            '    fn finish_retired_compute(&self, retired: &mut PerChannel<Uninstalled>) -> Result {\n'
            '        for item in retired.iter_mut() {\n'
            '            self.finish_uninstalled_compute(item.take())?;\n'
            '        }\n'
            '        Ok(())\n'
            '    }')
    elif mutation == 'early-return-skips-finish':
        backend_rollback = replace_once(backend_rollback, 'let result = firmware.queues', 'let result: Result = Ok(()); firmware.queues')
        backend_rollback = replace_once(backend_rollback, '&mut retired, &mut bindings);', '&mut retired, &mut bindings)?;')
    elif mutation == 'overwrite-full':
        rollback = replace_once(rollback, 'bindings.get_mut(channel)', 'bindings.get_mut(channel % COMPUTE_CHANNELS_MAX)')
        rollback = replace_once(rollback, 'retired.get_mut(channel).filter(|place| place.is_none())',
                                'retired.get_mut(channel % COMPUTE_CHANNELS_MAX)')

    reserve = reserve.replace('crate::g17::capacity::', 'capacity::')
    reserve, cancel = (text.replace('pub(super) ', '') for text in (reserve, cancel))

    return r'''
#![allow(dead_code)]
use std::cell::{Cell, RefCell, RefMut};
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

type Error = i32;
type Result<T = ()> = core::result::Result<T, Error>;
type KBox<T> = Box<T>;
type KVec<T> = Vec<T>;
const EIO: Error = -5;
const ENODEV: Error = -19;
const EINVAL: Error = -22;
const EAGAIN: Error = -11;
const ENOSPC: Error = -28;
const FIRST_COMPUTE_QID: u8 = 8;
''' + max_line.replace('pub(crate) ', '') + r'''

thread_local! {
    static LOCKED: Cell<bool> = const { Cell::new(false) };
    static LOG: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}
fn locked() -> bool { LOCKED.with(Cell::get) }
fn log(text: String) { LOG.with(|log| log.borrow_mut().push(text)); }
fn take_log() -> Vec<String> { LOG.with(|log| core::mem::take(&mut *log.borrow_mut())) }

mod capacity {
    pub(super) enum Site { ComputeSlots }
    pub(super) fn refused(_: Site) -> i32 { super::ENOSPC }
}

#[derive(Clone, Copy, PartialEq, Debug)]
struct Id { qid: u8, generation: u32 }
impl Id { fn qid(&self) -> u8 { self.qid } }
#[derive(Clone, Copy)]
enum DataMaster { Compute }

struct Context;

/// A displaced binding: its aliases must be torn down off the device mutex.
struct Binding(u8);
impl Drop for Binding {
    fn drop(&mut self) {
        assert!(!locked(), "binding of qid {} dropped under the device mutex", self.0);
        log(format!("binding {}", self.0));
    }
}
mod compute { pub(super) use super::{Binding, Queue}; }

#[derive(Clone, Copy, PartialEq)]
enum Rollback { Cancel, Displace, Keep, Fail }

struct Queue {
    qid: u8,
    owner: Option<u64>,
    primary: bool,
    active: u32,
    rollback: Rollback,
    unpublished: bool,
}
impl Queue {
    fn owner(&self) -> Option<u64> { self.owner }
    fn is_primary(&self) -> bool { self.primary }
    fn active_len(&self) -> u32 { self.active }
    fn has_replays(&self) -> bool { false }
    fn configuring(&self) -> bool { false }
    fn release_owner(&mut self) -> Result<bool> {
        self.owner = None;
        Ok(self.unpublished)
    }
    fn rollback_unpublished(&mut self, owner: u64, _: &Arc<Context>) -> Result<(bool, Option<Binding>)> {
        assert_eq!(self.owner, Some(owner));
        log(format!("rollback {}", self.qid));
        match self.rollback {
            Rollback::Cancel => { self.owner = None; Ok((true, None)) }
            Rollback::Displace => Ok((false, Some(Binding(self.qid)))),
            Rollback::Keep => Ok((false, None)),
            Rollback::Fail => Err(EIO),
        }
    }
}
impl Drop for Queue {
    fn drop(&mut self) {
        assert!(!locked(), "queue {} dropped under the device mutex", self.qid);
        log(format!("drop {}", self.qid));
    }
}

/// Stands in for the cell-locked slot: a guard for the installed queue, or none.
struct ComputeSlot(RefCell<Option<KBox<Queue>>>);
impl ComputeSlot {
    fn new() -> Result<Self> { Ok(Self(RefCell::new(None))) }
    fn with(queue: Queue) -> Self { Self(RefCell::new(Some(Box::new(queue)))) }
    fn as_deref(&self) -> Option<RefMut<'_, Queue>> {
        RefMut::filter_map(self.0.borrow_mut(), |queue| queue.as_deref_mut()).ok()
    }
    fn as_deref_mut(&mut self) -> Option<RefMut<'_, Queue>> { self.as_deref() }
    fn is_some(&self) -> bool { self.0.borrow().is_some() }
    fn is_none(&self) -> bool { !self.is_some() }
    fn take(&mut self) -> Option<KBox<Queue>> { self.0.borrow_mut().take() }
}

#[derive(Clone, Copy)]
struct ComputeReservation { slot: usize, id: Id, owner: u64 }
struct ComputeEntry { reservation: ComputeReservation, queue: ComputeSlot }
''' + aliases + r'''

struct Identities(u64);
impl Identities {
    fn reserve(&mut self, count: u64) -> Result<core::ops::Range<u64>> {
        let start = self.0;
        self.0 += count;
        Ok(start..self.0)
    }
}
struct Qids { next: u8, cancelled: Vec<u8>, refuse: Option<u8> }
impl Qids {
    fn reserve_for(&mut self, _: u64, _: DataMaster, _: Option<u8>, _: usize) -> Result<Id> {
        self.next += 1;
        Ok(Id { qid: self.next, generation: 1 })
    }
    fn render_pair_fits(&self) -> bool { true }
    fn render_pair_capacity(&self) -> usize { 4 }
    fn cancel_unpublished(&mut self, id: Id) -> Result {
        if self.refuse == Some(id.qid) { return Err(EIO); }
        self.cancelled.push(id.qid);
        Ok(())
    }
}
struct Admission;
impl Admission {
    fn reserve_compute_with_budget<T>(
        &self, _: u64, _: u64, reserve: impl FnOnce(usize) -> Result<(T, bool, usize)>,
    ) -> Result<T> {
        reserve(0).map(|(id, _, _)| id)
    }
}

struct Registry {
    compute: KVec<Option<ComputeEntry>>,
    identities: Identities,
    qids: Qids,
    admission: Admission,
}
impl Registry {
    fn reclaimable_render_slots(&self) -> Result<(u64, u64)> { Ok((0, 0)) }
    fn compute_admission_error(&self, error: Error) -> Error {
        if error == ENOSPC { EAGAIN } else { error }
    }
    fn note_build_room(&mut self) {}
''' + '\n'.join([drain, rollback, reserve, reserved, cancel]) + r'''
}

struct Firmware { queues: Registry }
struct Guard<'a>(RefMut<'a, Option<KBox<Firmware>>>);
impl Deref for Guard<'_> {
    type Target = Option<KBox<Firmware>>;
    fn deref(&self) -> &Self::Target { &self.0 }
}
impl DerefMut for Guard<'_> {
    fn deref_mut(&mut self) -> &mut Self::Target { &mut self.0 }
}
impl Drop for Guard<'_> {
    fn drop(&mut self) { LOCKED.with(|locked| locked.set(false)); }
}
struct State(RefCell<Option<KBox<Firmware>>>);
impl State {
    fn lock(&self) -> Guard<'_> {
        assert!(!locked(), "device mutex taken twice");
        LOCKED.with(|locked| locked.set(true));
        Guard(self.0.borrow_mut())
    }
}
struct Lease;
impl Lease { fn is_current(&self) -> bool { true } }
struct Preparations;
impl Preparations { fn enter(&self, _: bool) -> Result<Lease> { Ok(Lease) } }
struct Channels(Cell<u32>);
impl Channels { fn notify(&self) { self.0.set(self.0.get() + 1); } }
struct Shared { preparations: Preparations, state: State, channels: Channels }
impl Shared { fn queue_events(&self) {} }
struct Backend { owner: u64, context: Arc<Context>, shared: Shared }
impl Backend {
''' + '\n'.join([finish_one, finish_all, backend_rollback]) + r'''
}

const OWNER: u64 = 1;

/// The owner's channels in slots 0..: primary first, then siblings; slot `free` stays empty.
fn backend(channels: &[(Rollback, u32, bool)]) -> Backend {
    let mut compute: KVec<Option<ComputeEntry>> = Vec::new();
    for (slot, &(rollback, active, unpublished)) in channels.iter().enumerate() {
        let qid = 10 + slot as u8;
        compute.push(Some(ComputeEntry {
            reservation: ComputeReservation { slot, id: Id { qid, generation: 1 }, owner: OWNER },
            queue: ComputeSlot::with(Queue {
                qid, owner: Some(OWNER), primary: slot == 0, active, rollback, unpublished,
            }),
        }));
    }
    for _ in 0..4 {
        compute.push(None);
    }
    let queues = Registry {
        compute,
        identities: Identities(100),
        qids: Qids { next: 40, cancelled: Vec::new(), refuse: None },
        admission: Admission,
    };
    Backend {
        owner: OWNER,
        context: Arc::new(Context),
        shared: Shared {
            preparations: Preparations,
            state: State(RefCell::new(Some(Box::new(Firmware { queues })))),
            channels: Channels(Cell::new(0)),
        },
    }
}

fn installed(b: &Backend) -> Vec<u8> {
    let state = b.shared.state.0.borrow();
    let queues = &state.as_ref().unwrap().queues;
    queues.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref().map(|queue| queue.qid)).collect()
}

fn empty_entries(b: &Backend) -> usize {
    let state = b.shared.state.0.borrow();
    state.as_ref().unwrap().queues.compute.iter().flatten().filter(|entry| entry.queue.is_none()).count()
}

fn cancelled(b: &Backend) -> Vec<u8> {
    b.shared.state.0.borrow().as_ref().unwrap().queues.qids.cancelled.clone()
}

/// The owner is not locked out: the production reservation grants it a channel again.
fn reserves_again(b: &Backend) {
    let mut state = b.shared.state.0.borrow_mut();
    let queues = &mut state.as_mut().unwrap().queues;
    let reservation = queues.reserve_compute_channel(OWNER, COMPUTE_CHANNELS_MAX as u8);
    assert!(reservation.is_ok(), "owner locked out of reserve_compute_channel: {:?}", reservation.err());
    queues.cancel_compute(reservation.unwrap()).unwrap();
}

fn drain(b: &Backend) -> Result {
    let mut retired: PerChannel<Uninstalled> = Default::default();
    let result = {
        let mut state = b.shared.state.lock();
        let firmware = state.as_deref_mut().unwrap();
        firmware.queues.drain_compute_siblings(OWNER, &mut retired)
    };
    let finished = b.finish_retired_compute(&mut retired);
    assert!(retired.iter().all(Option::is_none), "drained queue left unfinished");
    result.and(finished)
}

fn main() {
    use Rollback::*;
    assert_eq!(COMPUTE_CHANNELS_MAX, 3, "this test models three channels per owner");

    // 1. Rollback cancels all three channels: siblings first, every reservation cancelled.
    let b = backend(&[(Cancel, 0, false), (Cancel, 0, false), (Cancel, 0, false)]);
    b.rollback_unpublished_compute().unwrap();
    assert_eq!(take_log(), ["rollback 11", "rollback 12", "rollback 10", "drop 11", "drop 12", "drop 10"]);
    assert_eq!(cancelled(&b), [11, 12, 10]);
    assert!(installed(&b).is_empty() && empty_entries(&b) == 0, "rolled-back channel left in the registry");
    reserves_again(&b);

    // 2. Displaced bindings on all three channels are dropped off-lock; the queues stay.
    let b = backend(&[(Displace, 0, false), (Displace, 0, false), (Displace, 0, false)]);
    b.rollback_unpublished_compute().unwrap();
    assert_eq!(take_log(), ["rollback 11", "rollback 12", "rollback 10", "binding 11", "binding 12", "binding 10"]);
    assert_eq!(installed(&b), [10, 11, 12]);
    assert!(cancelled(&b).is_empty());

    // 3. A later channel fails its rollback: the channels taken before it are still finished.
    let b = backend(&[(Keep, 0, false), (Cancel, 0, false), (Fail, 0, false)]);
    assert_eq!(b.rollback_unpublished_compute(), Err(EIO));
    assert_eq!(take_log(), ["rollback 11", "rollback 12", "drop 11"]);
    assert_eq!(cancelled(&b), [11]);
    assert_eq!(installed(&b), [10, 12]);
    assert_eq!(empty_entries(&b), 0, "a failed rollback left an empty entry holding its QID");

    // 4. The drain releases both idle siblings and finishes both; the primary stays.
    let b = backend(&[(Keep, 0, true), (Keep, 0, true), (Keep, 0, true)]);
    drain(&b).unwrap();
    assert_eq!(take_log(), ["drop 11", "drop 12"]);
    assert_eq!(cancelled(&b), [11, 12]);
    assert_eq!(installed(&b), [10]);
    assert_eq!(empty_entries(&b), 0);
    reserves_again(&b);

    // 5. A busy later sibling stops the drain (EAGAIN), and the sibling taken before it is
    //    still finished, so the owner is not locked out.
    let b = backend(&[(Keep, 0, true), (Keep, 0, true), (Keep, 1, true)]);
    assert_eq!(drain(&b), Err(EAGAIN));
    assert_eq!(take_log(), ["drop 11"]);
    assert_eq!(cancelled(&b), [11]);
    assert_eq!(installed(&b), [10, 12]);
    assert_eq!(empty_entries(&b), 0, "EAGAIN left a drained sibling's entry empty");
    reserves_again(&b);

    // 6. Finishing goes on past a failed cancellation: the others' reservations still go.
    let b = backend(&[(Cancel, 0, false), (Cancel, 0, false), (Cancel, 0, false)]);
    b.shared.state.0.borrow_mut().as_mut().unwrap().queues.qids.refuse = Some(11);
    assert_eq!(b.rollback_unpublished_compute(), Err(EIO));
    assert_eq!(take_log(), ["rollback 11", "rollback 12", "rollback 10", "drop 11", "drop 12", "drop 10"]);
    assert_eq!(cancelled(&b), [12, 10], "a failed cancellation stopped the others");

    // 7. More channels than places (beyond the per-owner bound): refused before the extra
    //    channel changes; the three taken are finished and the extra one stays installed.
    let b = backend(&[(Cancel, 0, false), (Cancel, 0, false), (Cancel, 0, false), (Cancel, 0, false)]);
    assert_eq!(b.rollback_unpublished_compute(), Err(EIO));
    assert_eq!(take_log(), ["rollback 11", "rollback 12", "rollback 13", "drop 11", "drop 12", "drop 13"]);
    assert_eq!(cancelled(&b), [11, 12, 13]);
    assert_eq!(installed(&b), [10]);
    assert_eq!(empty_entries(&b), 0);

    println!("PASS g17-compute-retired: no reservation leaks across rollback and sibling drain");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=[
        'queue-under-lock', 'binding-under-lock', 'drain-under-lock', 'drain-local-on-eagain',
        'finish-stops-at-error', 'early-return-skips-finish', 'overwrite-full',
    ])
    args = parser.parse_args()
    source = build_harness(args.mutation)
    rustc = os.environ.get('HOSTRUSTC', 'rustc')
    with tempfile.TemporaryDirectory(prefix='g17-retired-') as tmp:
        rs = Path(tmp) / 'test.rs'
        exe = Path(tmp) / 'test'
        rs.write_text(source)
        build = subprocess.run([rustc, '--edition=2021', '-O', str(rs), '-o', str(exe)],
                               capture_output=True, text=True)
        if build.returncode != 0:
            sys.stderr.write(build.stderr)
            return build.returncode
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
