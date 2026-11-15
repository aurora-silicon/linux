#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production pool-release admission and worker wake predicates.

Firmware and allocation boundaries are host stubs. This is not GPU execution.
Both original failure modes must fail the same state-transition assertions.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def fixture():
    runtime = (ROOT / 'runtime.rs').read_text()
    compute = (ROOT / 'queue/compute.rs').read_text()
    events = (ROOT / 'runtime/events.rs').read_text()
    predicate = '.any(|queue| queue.retirement_polling() || queue.pool_release_pending())'
    assert events.count(predicate) == 1
    methods = '\n'.join(item(runtime, name) for name in [
        'fn compute_admission_error(', 'pub(super) fn reserve_compute(']).replace('pub(super) ', '').replace('queue: ComputeSlot::new()?', 'queue: None')
    pending = item(compute, 'pub(crate) fn pool_release_pending(') + '\n' + \
        item(compute, 'pub(crate) fn release_settling(')
    return r'''
#![allow(dead_code)]
type Error = i32;
type Result<T = ()> = std::result::Result<T, Error>;
const EINVAL: Error = -22;
const ENOSPC: Error = -28;
const EAGAIN: Error = -11;
const FIRST_COMPUTE_QID: u8 = 4;
mod g17 { pub mod capacity {
    pub enum Site { ComputeSlots }
    pub fn refused(_: Site) -> i32 { -28 }
}}
#[derive(Clone, Copy, PartialEq, Eq)]
enum DataMaster { Compute }
#[derive(Clone, Copy)]
enum PoolPhase { Populated, ReleaseSent(u32), Vacant }
struct Active { len: usize }
mod compute {
    use super::*;
    pub(super) struct Queue {
        pub owner: Option<u64>,
        pub pool_phase: PoolPhase,
        pub retire_pending: bool,
        pub released: bool,
        pub quarantined: bool,
        pub active: Active,
    }
    impl Queue {
        pub(super) fn owner(&self) -> Option<u64> { self.owner }
        pub(super) fn retirement_polling(&self) -> bool { self.retire_pending }
PENDING
    }
RELEASE_PENDING
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Id { qid: u8 }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ComputeReservation { slot: usize, id: Id, owner: u64 }
type ComputeSlot = Option<Box<compute::Queue>>;
struct ComputeEntry { reservation: ComputeReservation, queue: ComputeSlot }
struct Admission; impl Admission {fn queues(&self)->u32{4}}
struct QueueIds { free: bool, fail: Option<Error> }
impl QueueIds {
    fn reserve_for(&mut self, _: u64, _: DataMaster, preferred: Option<u8>, _: usize) -> Result<Id> {
        if let Some(error) = self.fail { return Err(error); }
        if !self.free { return Err(ENOSPC); }
        self.free = false;
        Ok(Id { qid: preferred.unwrap() })
    }
}
struct Identities { next: u64 }
impl Identities {
    fn reserve(&mut self, count: u64) -> Result<std::ops::Range<u64>> {
        let first = self.next; self.next += count;
        Ok(first..self.next)
    }
}
struct Registry {
    compute: Vec<Option<ComputeEntry>>,
    identities: Identities,
    qids: QueueIds,
    admission: Admission,
}
impl Registry {
    // Physical-QID capacity notification does not change this pool-release fixture.
    fn note_build_room(&self) {}
METHODS
    fn poll(&self) -> bool {
        self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref())PREDICATE
    }
}
fn state(phase: PoolPhase, free: bool) -> Registry {
    let mut entries = Vec::new();
    for slot in 0..4 {
        entries.push(Some(ComputeEntry {
            reservation: ComputeReservation { slot, id: Id { qid: slot as u8 }, owner: slot as u64 + 1 },
            queue: Some(Box::new(compute::Queue { owner: None, pool_phase: phase, retire_pending: false, released: true, quarantined: false, active: Active { len: 0 } })),
        }));
    }
    entries.push(None);
    Registry { compute: entries, identities: Identities { next: 100 }, qids: QueueIds { free, fail: None }, admission: Admission }
}
fn main() {
    // Captured failure: four idle released queues, full physical namespace,
    // control-release cursor still awaiting observation, no retirement event.
    let mut registry = state(PoolPhase::ReleaseSent(3), false);
    assert!(registry.poll(), "release needs a worker wake without new GPU events");
    for _ in 0..8 {
        assert_eq!(registry.reserve_compute(200), Err(EAGAIN), "pending release is temporary admission backpressure");
        assert!(registry.compute[4].is_none(), "failed admission must not install a reservation");
        assert!(!registry.qids.free);
    }
    // Only the firmware witness may move the retained graph to vacant.
    for entry in registry.compute.iter_mut().flatten() {
        entry.queue.as_mut().unwrap().pool_phase = PoolPhase::Vacant;
    }
    assert!(!registry.poll(), "settled idle queues must stop polling");
    assert_eq!(registry.compute_admission_error(ENOSPC), EAGAIN);
    for phase in [PoolPhase::Populated, PoolPhase::Vacant] {
        let mut registry = state(phase, false);
        assert!(!registry.poll());
        assert_eq!(registry.reserve_compute(200), Err(EAGAIN), "shared channel capacity waits for retirement");
    }
    // Pending releases do not throttle spare physical capacity or hide errors.
    let mut registry = state(PoolPhase::ReleaseSent(u32::MAX), true);
    let reservation = registry.reserve_compute(200).unwrap();
    assert_eq!(reservation.slot, 4);
    assert_eq!(reservation.id.qid, FIRST_COMPUTE_QID);
    assert!(registry.compute[4].is_some());
    for error in [EINVAL, -5, -12, EAGAIN] {
        let mut registry = state(PoolPhase::ReleaseSent(3), false);
        registry.qids.fail = Some(error);
        assert_eq!(registry.reserve_compute(200), Err(error));
    }
    let mut registry = state(PoolPhase::Populated, false);
    registry.compute[0].as_mut().unwrap().queue.as_mut().unwrap().retire_pending = true;
    assert!(registry.poll(), "existing command-retirement polling remains enabled");
    // An exited owner's command waiting for its stamp settles without a new event.
    assert_eq!(registry.reserve_compute(200), Err(EAGAIN), "a settling released queue is temporary backpressure");
    // A closed owner retains its identity until its final command retires.
    registry.compute[0].as_mut().unwrap().queue.as_mut().unwrap().owner = Some(1);
    registry.compute[0].as_mut().unwrap().queue.as_mut().unwrap().active.len = 1;
    assert_eq!(registry.reserve_compute(200), Err(EAGAIN), "closed owner still has a command");
    // An off-lock graph constructor also returns capacity or installs a queue.
    let mut building = state(PoolPhase::Populated, false);
    building.compute[0].as_mut().unwrap().queue = None;
    assert_eq!(building.reserve_compute(200), Err(EAGAIN), "constructor can return capacity");
    building.compute.pop();
    assert_eq!(building.reserve_compute(200), Err(EAGAIN), "full registry must also wait for a constructor");
    // A command still owned by a live client is not a release.
    registry.compute[0].as_mut().unwrap().queue.as_mut().unwrap().released = false;
    assert_eq!(registry.reserve_compute(200), Err(EAGAIN), "an owned retiring queue can yield its channel after its witness");
    println!("PASS captured four-queue transition, settled wake removal, stamp-wait settling, full/free identity capacity and error preservation");
}
'''.replace('RELEASE_PENDING', item(compute, 'fn release_pending(')).replace('PENDING', pending).replace('METHODS', methods).replace('PREDICATE', predicate)


def main():
    code = fixture()
    with tempfile.TemporaryDirectory(prefix='g17-pool-release-') as directory:
        root = Path(directory)
        for mutation in ['fixed', 'no-wake', 'terminal-admission']:
            current = code
            if mutation == 'no-wake':
                current = current.replace('queue.retirement_polling() || queue.pool_release_pending()', 'queue.retirement_polling()')
            elif mutation == 'terminal-admission':
                current = current.replace('.map_err(|error| self.compute_admission_error(error))?', '?')
            if mutation != 'fixed':
                assert current != code, f'vacuous mutation: {mutation}'
            path = root / (mutation + '.rs')
            path.write_text(current)
            binary = path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if mutation == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                expected = {
                    'no-wake': 'release needs a worker wake',
                    'terminal-admission': 'pending release is temporary',
                }[mutation]
                assert result.returncode != 0 and expected in result.stderr, result.stderr
                print('PASS negative control:', mutation)


if __name__ == '__main__':
    main()
