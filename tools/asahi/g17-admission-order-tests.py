#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""G17 admission order: compute sizing, and which render slot an owner is given.

The production ioctl sizing callback must not allocate physical channels. A mock
physical allocator records demand while reverse timeline dependencies remain
unsignalled. Restoring the eager callback must fail this same control.

The real render admission State and its slot choices are compiled with a held
slot: a pair that keeps work no other owner can bind past. No new or other
owner may be given it, its own open owner keeps using it, and it returns once
the hold clears. --mutation removes one part of that rule and must fail.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def method(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def compute_order():
    code = r'''
#![allow(dead_code)]
use std::sync::atomic::{AtomicUsize,AtomicBool,Ordering};
type Result<T=()> = std::result::Result<T,i32>;
const ECANCELED:i32=-125;const EIO:i32=-5;const EFAULT:i32=-14;const EINVAL:i32=-22;
mod g17 {
pub mod fw {
 pub mod compute_pool {
  pub fn required_blocks(_:usize,cores:u32,pages:u16)->crate::Result<usize> {
   if cores==0 {Err(-22)} else {Ok(21+usize::from(pages))}
  }
 }
 pub mod m5 { pub mod pool { #[derive(Clone,Copy,Debug,PartialEq)] pub struct PoolGeometry(pub u32); } }
}
pub mod freelist {
 #[derive(Debug,PartialEq)]
 pub enum ComputeCapacity { Blocks(usize), M5(Option<super::fw::m5::pool::PoolGeometry>) }
}
pub mod render_resources { #[derive(Clone,Copy)] pub struct Plan { pub geometry: super::fw::m5::pool::PoolGeometry } }
pub mod command {
 pub struct ScratchRequest(pub u16);
 impl ScratchRequest { pub fn pages(&self)->u16{self.0} }
 pub enum Validated {
  Render,
  Compute { plan: Option<super::render_resources::Plan>, scratch: ScratchRequest },
 }
}
pub mod runtime {
 use super::*;
 use crate::{Result,ECANCELED,EIO,EFAULT,EINVAL};
 use std::sync::atomic::{AtomicUsize,AtomicBool,Ordering};
 pub struct Status(pub i32);
 impl Status {fn get(&self)->i32{self.0}}
 pub struct Context {pub status:Status,pub m5:bool}
 impl Context {fn status(&self)->&Status{&self.status} fn is_current(&self)->bool{true} fn is_m5(&self)->bool{self.m5}}
 pub struct Shared {pub compute_blocks:usize,pub active_cores:u32,pub crashed:AtomicBool}
 pub struct Admission;
 pub struct Backend {pub shared:Shared,pub physical:AtomicUsize,pub closed:AtomicBool,pub context:Context}
 impl Backend {
  pub fn ensure_compute(&self,_:freelist::ComputeCapacity,_:&Admission)->Result {self.physical.fetch_add(1,Ordering::SeqCst);Ok(())}
''' + method('runtime.rs', 'pub(super) fn compute_capacity(').replace('pub(super) ', 'pub ', 1) + '\n' + method('runtime.rs', 'fn admission_open(').replace('fn ', 'pub fn ', 1) + r'''
 }
 pub trait Api {fn ensure_compute(&self,command:&command::Validated)->Result;}
 impl Api for Backend {
''' + method('runtime/backend.rs', 'fn ensure_compute(') + r'''
 }
}
}
use g17::{command::{ScratchRequest,Validated},freelist::ComputeCapacity,fw::m5::pool::PoolGeometry,render_resources::Plan,runtime::*};
fn compute(pages:u16)->Validated {Validated::Compute{plan:None,scratch:ScratchRequest(pages)}}
fn backend(cores:u32,m5:bool)->Backend {
 Backend{shared:Shared{compute_blocks:21,active_cores:cores,crashed:AtomicBool::new(false)},physical:AtomicUsize::new(0),closed:AtomicBool::new(false),context:Context{status:Status(0),m5}}
}
fn main(){
 let backend=backend(10,false);
 // Four clients each enqueue nine queues in reverse dependency order. The
 // final host signal cannot be submitted if ioctl admission blocks on channels.
 for _client in 0..4 {for _queue in (0..9).rev() {<Backend as Api>::ensure_compute(&backend,&compute(0)).unwrap();}}
 assert_eq!(backend.physical.load(Ordering::SeqCst),0,"unsignalled work must not pin physical capacity");
 assert_eq!(backend.compute_capacity(&compute(3)),Ok(ComputeCapacity::Blocks(24)));
 Backend::ensure_compute(&backend,backend.compute_capacity(&compute(0)).unwrap(),&Admission).unwrap();
 assert_eq!(backend.physical.load(Ordering::SeqCst),1);
 let invalid=self::backend(0,false);
 assert_eq!(<Backend as Api>::ensure_compute(&invalid,&compute(0)),Err(-22));
 assert_eq!(invalid.physical.load(Ordering::SeqCst),0);
 assert_eq!(<Backend as Api>::ensure_compute(&backend,&Validated::Render),Err(EINVAL));
 // M5 sizes a compute pool by its plan's geometry; that, too, admits nothing physical.
 let m5=self::backend(0,true);
 let planned=Validated::Compute{plan:Some(Plan{geometry:PoolGeometry(7)}),scratch:ScratchRequest(0)};
 for _ in 0..36 {<Backend as Api>::ensure_compute(&m5,&planned).unwrap();}
 assert_eq!(m5.compute_capacity(&planned),Ok(ComputeCapacity::M5(Some(PoolGeometry(7)))));
 assert_eq!(m5.physical.load(Ordering::SeqCst),0,"unsignalled work must not pin physical capacity");
 for _ in 0..512 {
  backend.closed.store(true,Ordering::Release);
  assert_eq!(backend.admission_open().and_then(|()| Backend::ensure_compute(&backend,ComputeCapacity::Blocks(21),&Admission)),Err(ECANCELED),"closed constructor must not install");
  assert_eq!(backend.physical.load(Ordering::SeqCst),1);
 }
 println!("PASS closed builders cannot install after logical close; production ioctl sizing validates 36 reverse-order submissions without physical admission, on G17P and M5; invalid sizing consumes no channel");
}
'''
    fixed = 'self.compute_capacity(command).map(|_| ())'
    assert code.count(fixed) == 1
    with tempfile.TemporaryDirectory(prefix='g17-admission-order-') as tmp:
        for name, source in [('fixed', code), ('eager-physical-admission', code.replace(fixed, 'self.compute_capacity(command).and_then(|required| Backend::ensure_compute(self, required, &Admission))')), ('ignore-logical-close', code.replace('self.closed.load(Ordering::Acquire)', 'false'))]:
            path = Path(tmp) / (name + '.rs')
            binary = path.with_suffix('')
            path.write_text(source)
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and ('must not pin physical capacity' if name == 'eager-physical-admission' else 'closed constructor must not install') in result.stderr, result.stderr
                print('PASS negative control:', name)



def item(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def held_fixture():
    source = (ROOT / 'queue/admission.rs').read_text()
    hw = (ROOT.parent / 'hw/t8140.rs').read_text()
    constants = '\n'.join(re.search(r'pub\(crate\) const ' + name + r': [^;]+;', hw).group(0)
                          for name in ['RENDER_SLOTS', 'RENDER_PAIRS_MAX', 'RENDER_DEPTH'])
    pairs = (ROOT / 'pairs.rs').read_text()
    at = pairs.index('const SIBLING_BACKLOG')
    backlog = pairs[at:pairs.index('\n}\n', pairs.index('fn sibling_backlog', at)) + 3]
    state = '\n'.join(item(source, name) for name in [
        'const fn new(', 'fn exhausted(', 'fn owner_quarantine(', 'fn refusal(', 'fn advance(',
        'fn take_idle(', 'fn acquire(', 'fn claim_sibling(', 'fn reassign_unused_spare(',
        'fn cancel_empty_slot(', 'fn can_pick(', 'fn pick(', 'fn usable(', 'fn join(', 'fn release(',
        'fn set_held(', 'fn retained(', 'fn used('])
    pool = '\n'.join(item(source, name) for name in [
        'pub(crate) fn set_held(', 'pub(crate) fn release_retained_owners_after_processor_stop(',
        'pub(crate) fn killed_pair_free('])
    return r'''
#![allow(dead_code, unused_macros)]
use std::sync::{Mutex, MutexGuard, atomic::{AtomicUsize, Ordering::SeqCst}};
type Result<T = ()> = std::result::Result<T, i32>;
const ENOSPC: i32 = -28;
const EIO: i32 = -5;
mod g17 {
    pub mod trace { pub fn teardown(_: u8, _: u64, _: u64, _: u64) {} }
    pub mod capacity {
        pub enum Site { RenderSlots }
        pub fn refused(_: Site) -> i32 { crate::ENOSPC }
    }
    pub mod pairs {
        use std::sync::atomic::{AtomicU8, Ordering::Relaxed};
''' + backlog + r'''
    }
}
mod cfg {
''' + constants + r'''
}
type KVec<T> = Vec<T>;
struct InFlight;
#[derive(Clone, Copy, PartialEq, Debug)]
enum Plan { Wait, Pair(u8), Join(u8), Any }
''' + item(source, 'struct State {') + '\nimpl State {\n' + re.search(r'    const MASK: u64 = .*;', source).group(0) + '\n' + state + r'''
}
struct Lock(Mutex<State>);
impl Lock { fn lock(&self) -> MutexGuard<'_, State> { self.0.lock().unwrap() } }
struct Pool { state: Lock, wakes: AtomicUsize }
impl Pool {
    fn returned(&self) { self.wakes.fetch_add(1, SeqCst); }
''' + pool + '\n}\n' + item(source, 'fn a_held_slot_goes_to_no_other_owner(') + r'''
const SLOTS: usize = cfg::RENDER_SLOTS;
fn bit(slot: usize) -> u64 { 1u64 << slot }
/// Every slot is retained by an open owner except `closed`, whose owners closed in this order.
fn crowded(closed: &[usize]) -> State {
    let mut state = State::new();
    // A full queue table: no new graph is built in an unowned slot.
    state.build_room = false;
    for slot in 1..SLOTS {
        state.owners[slot] = 1000 + slot as u64;
    }
    for (age, slot) in closed.iter().enumerate() {
        state.released |= bit(*slot);
        state.epochs[*slot] = 1 + age as u64;
    }
    state
}
fn pool(state: State) -> Pool { Pool { state: Lock(Mutex::new(state)), wakes: AtomicUsize::new(0) } }

fn new_owners() {
    // Slot 5 was released first: without the hold a new owner takes it before slot 9.
    let mut state = crowded(&[5, 9]);
    assert!(!state.set_held(5, true));
    assert_ne!(state.used() & bit(5), 0, "a held slot counts as used");
    assert_eq!(state.used() & bit(9), 0);
    assert_eq!(state.refusal(1), Ok(()));
    assert!(state.can_pick(1, Plan::Any));
    assert_eq!(state.acquire(1, 1), Some(9), "a held slot goes to no new owner");
    assert_eq!(state.refusal(2), Err(ENOSPC), "a new owner facing only held slots is refused for capacity");
    assert!(!state.can_pick(2, Plan::Any), "scheduler admission counts no held slot");
    assert_eq!(state.pick(2, 1, Plan::Any), None, "a held slot goes to no new owner");
    state.claim_sibling(1, cfg::RENDER_PAIRS_MAX);
    assert_eq!(state.owners[5], 1005, "a sibling never comes from a held slot");
    assert!(!state.reassign_unused_spare(5, Some(1005), Some(3)), "a held slot is no spare for another owner");
    assert!(!state.reassign_unused_spare(5, Some(1005), None), "a held slot is not freed for compute");
    assert_eq!(state.owners[5], 1005);
    state.killed |= bit(5);
    let pool = pool(state);
    assert!(!pool.killed_pair_free(), "a held killed pair is not free for a new owner");
    pool.set_held(5, false);
    assert_eq!(pool.wakes.load(SeqCst), 1, "clearing a hold wakes the waiters");
    assert!(pool.killed_pair_free());
    let mut state = pool.state.0.into_inner().unwrap();
    assert_eq!(state.used() & bit(5), 0);
    assert_eq!(state.refusal(2), Ok(()));
    assert_eq!(state.acquire(2, 1), Some(5), "a cleared slot goes to a new owner again");
}

fn own_owner() {
    // An open owner whose idle pair owes stamps keeps rendering on it.
    let mut state = crowded(&[]);
    state.owners[12] = 7;
    state.set_held(12, true);
    assert_eq!(state.refusal(7), Ok(()));
    assert!(state.can_pick(7, Plan::Any), "the owner's own held pair stays usable");
    assert!(state.can_pick(7, Plan::Pair(12)), "the owner's own held pair stays usable");
    assert_eq!(state.acquire(7, 1), Some(12), "the owner keeps its held pair");
    assert_eq!(state.acquire(7, 1), Some(12), "and stacks its renders there");
    assert_eq!(state.pick(7, 1, Plan::Pair(12)), Some(12));
    assert_eq!(state.pick(7, 1, Plan::Join(12)), Some(12));
    for _ in 0..4 {
        state.release(12);
    }
    assert_eq!(state.active, 0);
    // An open owner gives up a spare only while it keeps another usable pair; a held one is not.
    state.owners[13] = 7;
    assert!(!state.reassign_unused_spare(13, Some(7), Some(4)), "a held pair is not the usable pair an open owner keeps");
    state.refs[12] = 1;
    assert!(!state.cancel_empty_slot(12, 7), "a held slot is not given back as unowned");
    state.refs[12] = 0;
    // Once the owner closes, its held pair goes to no other owner.
    state.released |= bit(12) | bit(13);
    assert_eq!(state.acquire(8, 1), Some(13));
    assert_eq!(state.refusal(9), Err(ENOSPC));
    assert_eq!(state.acquire(9, 1), None, "a closed owner's held pair goes to no other owner");
}

fn holds_and_stop() {
    let pool = pool(crowded(&[5]));
    pool.set_held(5, true);
    pool.set_held(0, true);
    pool.set_held(SLOTS, true);
    pool.set_held(SLOTS + 7, true);
    assert_eq!(pool.state.lock().held, bit(5), "only a render slot is held");
    // Every pass that puts a healthy pair back clears its slot: no wake storm.
    pool.set_held(9, false);
    pool.set_held(9, false);
    assert_eq!(pool.wakes.load(SeqCst), 0, "clearing a slot that was not held wakes nobody");
    pool.set_held(12, true);
    pool.release_retained_owners_after_processor_stop();
    assert_eq!(pool.state.lock().held, 0, "a processor stop ends every hold");
}

fn main() {
    a_held_slot_goes_to_no_other_owner();
    new_owners();
    own_owner();
    holds_and_stop();
    println!("PASS held render slots: no new owner, sibling claim, spare reassignment, scheduler admission or killed-pair reuse is given one; it counts as used and refuses for capacity; its own open owner keeps its pair; a cleared hold wakes waiters and returns the slot; a processor stop ends every hold");
}
'''


# Each removes one part of the hold rule (or applies it to the owner's own pair).
HELD_MUTATIONS = {
    'no-held-free': ('& !self.kill_hold & !self.held;\n        let eligible', '& !self.kill_hold;\n        let eligible'),
    'no-held-sibling': ('& !self.killed & !self.held;', '& !self.killed;'),
    'no-held-refusal': ('& !self.kill_hold\n                    & !self.held\n', '& !self.kill_hold\n'),
    'no-held-pick': ('& !self.kill_hold & !self.held;\n                (0..cfg', '& !self.kill_hold;\n                (0..cfg'),
    'no-held-spare': ('(self.active | self.quarantined | self.kill_hold | self.held) & bit != 0',
                      '(self.active | self.quarantined | self.kill_hold) & bit != 0'),
    'no-held-other': ('(self.quarantined | self.kill_hold | self.held) & (1u64 << other) == 0',
                      '(self.quarantined | self.kill_hold) & (1u64 << other) == 0'),
    'no-held-cancel': ('(self.quarantined | self.kill_hold | self.held) & bit == 0',
                       '(self.quarantined | self.kill_hold) & bit == 0'),
    'no-held-killed': ('& !state.kill_hold & !state.held', '& !state.kill_hold'),
    'no-held-used': ('self.kill_hold | self.held | (self.retained()', 'self.kill_hold | (self.retained()'),
    'no-held-wake': ('if cleared {\n            self.returned();', 'if false {\n            self.returned();'),
    'wake-on-every-clear': ('was && !held', '!held'),
    'no-held-stop': ('state.held = 0;', ''),
    'held-blocks-owner': ('&& (self.quarantined | self.kill_hold) & (1u64 << slot) == 0\n            && self.refs[slot] < cfg::RENDER_DEPTH',
                          '&& (self.quarantined | self.kill_hold | self.held) & (1u64 << slot) == 0\n            && self.refs[slot] < cfg::RENDER_DEPTH'),
}


def held_slots(mutation):
    code = held_fixture()
    if mutation:
        old, new = HELD_MUTATIONS[mutation]
        assert code.count(old) == 1, mutation
        code = code.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='g17-admission-held-') as tmp:
        path = Path(tmp) / 'held.rs'
        binary = path.with_suffix('')
        path.write_text(code)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
        # A killed mutant exits with the failed control's panic on stderr.
        result = subprocess.run([str(binary)])
        if result.returncode:
            raise SystemExit(result.returncode)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(HELD_MUTATIONS))
    args = ap.parse_args()
    if args.mutation is None:
        compute_order()
    held_slots(args.mutation)


if __name__ == '__main__':
    main()
