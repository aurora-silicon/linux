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

The runtime's hold rule is compiled with the real owner close (release_queue,
release_render), the real scheduler-release teardown (close, releasing,
take_consumed) and the admission state: a closed owner's idle pair is held
from its close, before admission releases the slot, until its release is
consumed and the pair parked. On Neo such a pair (its context waiting for its
compute) refused every client given its slot, each of which lost its device
after its two-second publication bound. Quarantined and owed pairs are held
by the same rule, a lent pair keeps its bit across a resync (and is held when
its owner closes meanwhile), and a parked or missing pair is never held. Each release mutation drops one part and must fail.
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
        'const fn new(', 'fn owns_live(', 'fn has_live_owner(', 'fn exhausted(', 'fn owner_quarantine(', 'fn owner_released(', 'fn refusal(', 'fn advance(',
        'fn set_owner(', 'fn take_idle(', 'fn acquire(', 'fn acquire_for(', 'fn live_owners(', 'fn live_siblings(',
        'fn reclaimable_siblings_in(', 'fn reclaimable_siblings(', 'fn promotes_reclaimable_sibling(',
        'fn can_lease_slot(', 'fn unavailable_released(', 'fn total_pair_demand(', 'fn pair_demand(',
        'fn claim_sibling(', 'fn can_reassign_unused_spare(', 'fn reassign_unused_spare(',
        'fn cancel_empty_slot(', 'fn can_pick(', 'fn can_pick_for(',
        'fn wide(', 'fn mark_wide(', 'fn can_pick_render(', 'fn can_pick_render_for(',
        'fn pick(', 'fn pick_for(', 'fn usable(', 'fn join(', 'fn join_for(',
        'fn release(', 'fn set_held(', 'fn retained(', 'fn used('])
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
    state.claim_sibling(1, cfg::RENDER_PAIRS_MAX, 0);
    assert_eq!(state.owners[5], 1005, "a sibling never comes from a held slot");
    // With pair capacity to spare a sibling is claimed, but from the next
    // released slot: slot 5 is the oldest release and stays held.
    let mut spare = crowded(&[5, 9, 2]);
    assert!(!spare.set_held(5, true));
    spare.owners[9] = 1;
    spare.released &= !bit(9);
    spare.render_capacity = SLOTS;
    spare.claim_sibling(1, cfg::RENDER_PAIRS_MAX, 0);
    assert_eq!(spare.owners[5], 1005, "a sibling never comes from a held slot");
    assert_eq!(spare.owners[2], 1, "a sibling comes from a released, unheld slot");
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
    // The closed owner itself is refused too: the kernel may have closed it for a client whose
    // early publication still asks (`try_render_early`). Retaking its idle pair would clear
    // `released`, and nothing would release it again.
    assert!(state.owner_released(7), "a released owner takes no pair back");
    assert!(!state.owner_released(8), "an owner that took a released pair is not closed");
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
    'released-owner-retakes': ('.any(|slot| self.owners[slot] == owner && self.released & (1u64 << slot) != 0)',
                               '.any(|_| false)'),
    'held-blocks-owner': ('&& (self.quarantined | self.kill_hold) & (1u64 << slot) == 0\n            && self.refs[slot] < self.render_depth',
                          '&& (self.quarantined | self.kill_hold | self.held) & (1u64 << slot) == 0\n            && self.refs[slot] < self.render_depth'),
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


def release_fixture():
    source = (ROOT / 'queue/admission.rs').read_text()
    teardown = (ROOT / 'teardown.rs').read_text()
    render = (ROOT / 'runtime/render.rs').read_text()
    render_teardown = (ROOT / 'runtime/render/teardown.rs').read_text()
    backend = (ROOT / 'runtime/backend.rs').read_text()
    pair = (ROOT / 'queue/render.rs').read_text()
    render_exit = (ROOT / 'runtime/render/exit.rs').read_text()
    hw = (ROOT.parent / 'hw/t8140.rs').read_text()
    constants = '\n'.join(re.search(r'pub\(crate\) const ' + name + r': [^;]+;', hw).group(0)
                          for name in ['RENDER_SLOTS', 'RENDER_PAIRS_MAX', 'RENDER_DEPTH'])
    pairs = (ROOT / 'pairs.rs').read_text()
    at = pairs.index('const SIBLING_BACKLOG')
    backlog = pairs[at:pairs.index('\n}\n', pairs.index('fn sibling_backlog', at)) + 3]
    state = '\n'.join(item(source, name) for name in [
        'const fn new(', 'fn owns_live(', 'fn has_live_owner(', 'fn exhausted(', 'fn owner_quarantine(', 'fn refusal(', 'fn advance(',
        'fn set_owner(', 'fn take_idle(', 'fn acquire(', 'fn acquire_for(', 'fn live_owners(', 'fn live_siblings(',
        'fn reclaimable_siblings_in(', 'fn reclaimable_siblings(', 'fn promotes_reclaimable_sibling(',
        'fn can_lease_slot(', 'fn unavailable_released(', 'fn total_pair_demand(', 'fn pair_demand(',
        'fn claim_sibling(', 'fn usable(', 'fn can_reassign_unused_spare(', 'fn reassign_unused_spare(',
        'fn release_unused_sibling(', 'fn hold_for_exit_kill(', 'fn set_held(', 'fn sync_held(', 'fn retained(', 'fn used('])
    pool = '\n'.join(item(source, name) for name in [
        'pub(crate) fn can_reassign_unused_spare(', 'pub(crate) fn reassign_unused_spare(',
        'pub(crate) fn release_unused_sibling(', 'pub(crate) fn owns_live_slot(',
        'pub(crate) fn is_wide(', 'pub(crate) fn hold_render_slots_for_exit_kill(',
        'pub(crate) fn set_held(', 'pub(crate) fn sync_held(', 'pub(crate) fn release_owner('])
    pending = '\n'.join(item(teardown, name) for name in [
        'pub(crate) fn new(', 'pub(crate) fn releasing(', 'fn find(', 'fn insert(',
        'pub(crate) fn close(', 'pub(crate) fn take_consumed('])
    runtime = '\n'.join(item(render, name) for name in [
        'pub(super) fn reclaim_unused_render_spare(',
        'pub(super) fn hold_render_slot(', 'pub(super) fn sync_render_holds(',
        'fn render_slot_held(']).replace('pub(super) fn', 'pub fn').replace('KBox::into_inner(pair).park()', '(*pair).park()')
    render_state = '\n'.join(item(render_exit, name) for name in [
        'pub(in crate::g17::runtime) fn exit_scope(',
        'pub(in crate::g17::runtime) fn slot_owner(',
        'pub(in crate::g17::runtime) fn qids_for_slot(']).replace('pub(in crate::g17::runtime) fn', 'pub fn')
    firmware = '\n'.join('pub ' + item(render, name) for name in ['fn with_render<R>(', 'fn with_render_at<R>('])
    return r"""
#![allow(dead_code, unused_macros, unused_imports, unused_variables)]
use std::ops::{Deref, DerefMut};
use std::sync::{Arc, Mutex, MutexGuard, atomic::{AtomicBool, AtomicUsize, Ordering::SeqCst}};
type Result<T = ()> = std::result::Result<T, i32>;
const ENOSPC: i32 = -28;
const EIO: i32 = -5;
const EBUSY: i32 = -16;
const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
#[derive(Clone, Copy)]
struct Flags;
const GFP_KERNEL: Flags = Flags;
/// The next `KVec::push` fails as an allocation would.
static FAIL_PUSH: AtomicBool = AtomicBool::new(false);
/// The kernel vector's fallible push and remove.
#[derive(Clone)]
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    const fn new() -> Self { KVec(Vec::new()) }
    fn with_capacity(capacity: usize, _: Flags) -> Result<Self> { Ok(KVec(Vec::with_capacity(capacity))) }
    fn push(&mut self, value: T, _: Flags) -> Result {
        if FAIL_PUSH.swap(false, SeqCst) {
            return Err(ENOMEM);
        }
        self.0.push(value);
        Ok(())
    }
    fn remove(&mut self, index: usize) -> std::result::Result<T, ()> {
        if index < self.0.len() { Ok(self.0.remove(index)) } else { Err(()) }
    }
}
impl<T> Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }
type KBox<T> = Box<T>;
mod g17 {
    pub mod trace { pub fn teardown(_: u8, _: u64, _: u64, _: u64) {} }
    pub mod capacity {
        pub enum Site { RenderSlots }
        pub fn refused(_: Site) -> i32 { crate::ENOSPC }
    }
    pub mod pairs {
        use std::sync::atomic::{AtomicU8, Ordering::Relaxed};
""" + backlog + r"""
    }
    pub mod context {
        pub struct Context { pub published: bool, pub scheduler: u64, pub closed: std::sync::atomic::AtomicBool }
        impl Context {
            pub fn is_published(&self) -> bool { self.published }
            pub fn scheduler_va(&self) -> u64 { self.scheduler }
            pub fn note_closed(&self, _generation: u64) {
                self.closed.store(true, std::sync::atomic::Ordering::Release);
            }
            pub fn is_closed(&self) -> bool { self.closed.load(std::sync::atomic::Ordering::Acquire) }
            pub fn note_open_client(&self, _open: bool) {}
        }
    }
    pub mod teardown {
        use crate::{KVec, GFP_KERNEL, Result, EIO, EBUSY, EINVAL};
        use std::sync::Arc;
        use super::context::Context;
        type Error = i32;
        /// The release at control-ring index `target` is consumed once the consumer reached it.
        fn control_consumed(consumer: u32, _producer: u32, target: u32) -> Result<bool> {
            Ok(consumer >= target)
        }
""" + re.search(r'const CAPACITY: usize = [^;]+;', teardown).group(0) + r"""
        #[derive(Clone, Copy, PartialEq, Eq)]
""" + item(teardown, 'enum Phase {') + '\n' + item(teardown, 'struct Entry {') + r"""
        #[derive(Clone, Copy)]
""" + item(teardown, 'struct RenderKill {') + r"""
        #[derive(Clone, Copy)]
""" + item(teardown, 'pub(crate) enum Failure {') + '\n' + item(teardown, 'pub(crate) trait Host {') + '\n' \
        + item(teardown, 'pub(crate) struct Pending {') + r"""
        impl Pending {
""" + pending + r"""
            /// The release of `context` was published at control-ring index `target`.
            pub fn published(&mut self, context: &Arc<Context>, target: u32) {
                let index = self.find(context).unwrap();
                self.entries[index].phase = Phase::Released(target);
            }
        }
    }
    pub mod runtime {
        use crate::{cfg, KBox, Pool, Result, EBUSY, EIO};
        use super::{context::Context, teardown::Pending};
        use std::sync::{Arc, Mutex, MutexGuard, atomic::{AtomicBool, Ordering}};
        pub const RENDER_SLOTS: usize = cfg::RENDER_SLOTS;
        pub fn now_ns() -> u64 { 1 }
        pub struct Active { pub stamps_owed: bool }
        pub struct Pair {
            pub quarantined: bool,
            pub active: Vec<Active>,
            pub context: Arc<Context>,
            pub owner: Option<u64>,
        }
        impl Pair {
""" + item(pair, 'pub(crate) fn unreusable_work(') + r"""
            pub fn context(&self) -> &Arc<Context> { &self.context }
            pub fn release_owner(&mut self) { self.owner = None; }
            pub fn detachable(&self) -> Result<bool> { Ok(!self.quarantined && self.active.is_empty()) }
            pub fn reusable(&self) -> Result<bool> { Ok(self.owner.is_none() && self.detachable()?) }
            pub fn can_yield_unused(&self) -> Result<bool> { Ok(false) }
            pub fn buffer_id(&self) -> u8 { 0 }
            pub fn park(self) -> ((), ()) { ((), ()) }
        }
        #[derive(Clone, Copy)]
        pub struct Qid(pub u8);
        impl Qid { pub fn qid(self) -> u8 { self.0 } }
        pub struct Qids;
        impl Qids {
            pub fn can_cancel_unpublished(&self, _: Qid) -> bool { false }
            pub fn cancel_unpublished(&mut self, _: Qid) -> Result { Ok(()) }
        }
        pub struct Buffers;
        impl Buffers { pub fn unreserve_owner(&mut self, _: u8) -> Result { Ok(()) } }
        pub struct UnusedRenderSpare { _entry: Option<Entry>, _binding: Option<()> }
        pub struct Reservation { pub owner: u64, pub ids: [Qid; 2] }
        pub struct Entry {
            pub reservation: Reservation,
            pub context: Option<Arc<Context>>,
            pub pair: Option<KBox<Pair>>,
            pub pool: Option<()>,
            pub parked: Option<()>,
            pub preparing: Option<()>,
            pub constructing: bool,
            pub growing: bool,
            pub pending_owner: Option<u64>,
            pub pending_owner_closed: bool,
            pub pending_error: Option<()>,
            pub pending_timeout: bool,
            pub closed: bool,
        }
        pub struct RenderState { pub entries: Vec<Option<Entry>> }
        impl RenderState {
            pub fn collect_retired(&mut self, _: usize) -> Result { Ok(()) }
""" + render_state + r"""
        }
        pub struct Registry {
            pub admission: Arc<Pool>,
            pub render: RenderState,
            pub teardown: Pending,
            pub qids: Qids,
            pub buffers: Buffers,
        }
        impl Registry {
            pub fn release_compute(&mut self, _: u64) -> Result { Ok(()) }
            pub fn note_build_room(&self) {}
""" + item(render_teardown, 'pub(in crate::g17::runtime) fn release_render(') + '\n' + runtime + r"""
        }
""" + item(render, 'fn render_hold(') + r"""
        pub struct Recovery;
        impl Recovery { pub fn generation(&self) -> u64 { 0 } }
        pub struct Firmware { pub queues: Registry, pub recovery: Recovery }
        pub struct Host<'a> {
            firmware: &'a mut Firmware,
            registration_powered: bool,
            m5_registration: Option<()>,
            outer_cursors: Option<(u8, [u32; 3])>,
            outer_producer_used: bool,
        }
        impl Host<'_> { fn release_registration(&mut self) {} }
        impl Firmware {
""" + firmware + r"""
        }
        pub mod backend {
            use super::*;
            pub enum Engine { Compute, Render }
            pub struct Lock<T>(pub Mutex<T>);
            impl<T> Lock<T> { pub fn lock(&self) -> MutexGuard<'_, T> { self.0.lock().unwrap() } }
            pub struct Channels;
            impl Channels { pub fn notify(&self) {} }
            pub struct Shared { pub state: Lock<Option<KBox<Firmware>>>, pub channels: Channels }
            impl Shared { pub fn queue_events(&self) {} }
            pub struct Backend {
                pub closed: AtomicBool,
                pub shared: Arc<Shared>,
                pub context: Arc<Context>,
                pub owner: u64,
            }
            impl Backend {
                fn hold_claim(&self, _: Option<()>) {}
                fn release_outer(&self, _: Engine) {}
                pub """ + item(backend, 'fn release_queue(') + r"""
            }
        }
    }
}
mod cfg {
""" + constants + r"""
}
#[derive(Clone)]
struct InFlight;
#[derive(Clone)]
""" + item(source, 'struct State {') + '\nimpl State {\n' + re.search(r'    const MASK: u64 = .*;', source).group(0) + '\n' + state + r"""
}
struct Lock(Mutex<State>);
impl Lock { fn lock(&self) -> MutexGuard<'_, State> { self.0.lock().unwrap() } }
/// `race`: a new owner that asks admission for a slot at every wake, as a publication
/// waiting for capacity does, without the device mutex.
struct Pool { state: Lock, wakes: AtomicUsize, race: Mutex<Option<u64>>, raced: Mutex<Vec<Option<u8>>> }
impl Pool {
    fn returned(&self) {
        self.wakes.fetch_add(1, SeqCst);
        let race = *self.race.lock().unwrap();
        if let Some(owner) = race {
            let got = self.state.lock().clone().acquire(owner, 1);
            self.raced.lock().unwrap().push(got);
        }
    }
""" + pool + r"""
}

use g17::context::Context;
use g17::runtime::{Active, Buffers, Entry, Firmware, Pair, Qid, Qids, Recovery, Registry, RenderState, Reservation};
use g17::runtime::backend::{Backend, Channels, Lock as DeviceLock, Shared};
const SLOTS: usize = cfg::RENDER_SLOTS;
const NEW: u64 = 5000;
fn bit(slot: usize) -> u64 { 1u64 << slot }
fn owner(slot: usize) -> u64 { 1000 + slot as u64 }
fn context(scheduler: u64) -> Arc<Context> {
    Arc::new(Context { published: true, scheduler, closed: std::sync::atomic::AtomicBool::new(false) })
}
/// Every slot is retained by an open owner and no new graph fits; `pairs` are installed.
fn device(pairs: &[(usize, &Arc<Context>)]) -> Arc<Shared> {
    let mut state = State::new();
    state.build_room = false;
    for slot in 1..SLOTS {
        state.owners[slot] = owner(slot);
        state.epochs[slot] = 100 + slot as u64;
    }
    let admission = Arc::new(Pool {
        state: Lock(Mutex::new(state)),
        wakes: AtomicUsize::new(0),
        race: Mutex::new(None),
        raced: Mutex::new(Vec::new()),
    });
    let mut entries: Vec<Option<Entry>> = (0..SLOTS).map(|_| None).collect();
    for (slot, context) in pairs {
        entries[*slot] = Some(Entry {
            reservation: Reservation { owner: owner(*slot), ids: [Qid((*slot * 2) as u8), Qid((*slot * 2 + 1) as u8)] },
            context: Some((*context).clone()),
            pair: Some(Box::new(Pair {
                quarantined: false,
                active: Vec::new(),
                context: (*context).clone(),
                owner: Some(owner(*slot)),
            })),
            pool: Some(()),
            parked: None,
            preparing: None,
            constructing: false,
            growing: false,
            pending_owner: None,
            pending_owner_closed: false,
            pending_error: None,
            pending_timeout: false,
            closed: false,
        });
    }
    let queues = Registry {
        admission,
        render: RenderState { entries },
        teardown: g17::teardown::Pending::new().unwrap(),
        qids: Qids,
        buffers: Buffers,
    };
    Arc::new(Shared { state: DeviceLock(Mutex::new(Some(Box::new(Firmware { queues, recovery: Recovery })))), channels: Channels })
}
fn backend(shared: &Arc<Shared>, slot: usize, context: &Arc<Context>) -> Backend {
    Backend { closed: AtomicBool::new(false), shared: shared.clone(), context: context.clone(), owner: owner(slot) }
}
/// Under the device mutex.
fn with<R>(shared: &Shared, f: impl FnOnce(&mut Firmware) -> R) -> R {
    let mut state = shared.state.lock();
    f(state.as_deref_mut().unwrap())
}
fn admission(shared: &Shared) -> State { with(shared, |firmware| firmware.queues.admission.state.lock().clone()) }
fn wakes(shared: &Shared) -> usize { with(shared, |firmware| firmware.queues.admission.wakes.load(SeqCst)) }
/// The teardown's detach of a consumed release: it parks the context's pairs.
struct Parker<'a> { render: &'a mut RenderState }
impl g17::teardown::Host for Parker<'_> {
    fn render_idle(&mut self, _: &Arc<Context>) -> Result<bool> { Ok(true) }
    fn compute_idle(&self, _: &Arc<Context>) -> bool { true }
    fn flists_released(&self, _: &Arc<Context>) -> bool { true }
    fn control_backpressured(&self) -> bool { false }
    fn publish_release(&mut self, _: u64, _: [u8; 4]) -> Result<u32> { Ok(0) }
    fn notify(&mut self) -> Result { Ok(()) }
    fn detach(&mut self, context: &Arc<Context>) -> Result {
        for entry in self.render.entries.iter_mut().flatten() {
            if entry.pair.as_ref().is_some_and(|pair| Arc::ptr_eq(pair.context(), context)) {
                entry.pair = None;
                entry.parked = Some(());
            }
        }
        Ok(())
    }
    fn report(&mut self, _: &Arc<Context>, _: g17::teardown::Failure) {}
}
/// The firmware consumed the release of `context`, published at `target`; the pass then
/// re-derives the holds, as service_teardown does after taking consumed releases.
fn consume(shared: &Shared, context: &Arc<Context>, target: u32) {
    with(shared, |firmware| {
        firmware.queues.teardown.published(context, target);
        let early = firmware.queues.teardown.take_consumed(target - 1, target + 1, &mut Parker { render: &mut firmware.queues.render });
        assert!(early.unwrap().is_none(), "a release is not taken before it is consumed");
        let taken = firmware.queues.teardown.take_consumed(target, target + 1, &mut Parker { render: &mut firmware.queues.render });
        assert!(taken.unwrap().is_some_and(|taken| Arc::ptr_eq(&taken, context)));
        assert!(!firmware.queues.teardown.releasing(context));
        firmware.queues.sync_render_holds();
    });
}

/// The Neo churn victims: a closed client's idle pair whose scheduler release cannot be
/// published (its compute is not idle) refused every client admission gave its slot.
fn closed_owner_release() {
    let (stuck, healthy, growing) = (context(0x30), context(0x31), context(0x33));
    let shared = device(&[(30, &stuck), (31, &healthy), (33, &growing)]);
    // Slot 30 was used longest ago: a new owner would take it first.
    with(&shared, |firmware| {
        let mut state = firmware.queues.admission.state.lock();
        state.epochs[30] = 1;
        state.epochs[31] = 2;
    });
    with(&shared, |firmware| *firmware.queues.admission.race.lock().unwrap() = Some(NEW));
    // Its owner closes while its last render still runs.
    with(&shared, |firmware| {
        let entry = firmware.queues.render.entries[30].as_mut().unwrap();
        entry.pair.as_mut().unwrap().active.push(Active { stamps_owed: false });
    });
    backend(&shared, 30, &stuck).release_queue().unwrap();
    let raced = with(&shared, |firmware| firmware.queues.admission.raced.lock().unwrap().clone());
    assert!(!raced.is_empty() && !raced.contains(&Some(30)),
            "a closing owner's pair is held before admission releases its slot: {raced:?}");
    with(&shared, |firmware| {
        assert!(firmware.queues.teardown.releasing(&stuck));
        let entry = firmware.queues.render.entries[30].as_ref().unwrap();
        assert!(entry.closed && entry.pair.as_ref().unwrap().owner.is_none());
    });
    let state = admission(&shared);
    assert_ne!(state.released & bit(30), 0);
    assert_ne!(state.held & bit(30), 0, "a closed owner's pair is held while its release is pending");
    assert_eq!(state.refusal(NEW), Err(ENOSPC), "a new owner facing only such a slot is refused for capacity");
    assert_eq!(state.clone().acquire(NEW, 1), None, "a pair whose release is pending goes to no other owner");
    // Its last render retires: the pair has no work left, but its release is still pending.
    with(&shared, |firmware| firmware.with_render(30, |pair, _| {
        pair.active.clear();
        Ok(())
    })).unwrap();
    assert_ne!(admission(&shared).held & bit(30), 0, "a retirement pass after the close keeps the hold");
    with(&shared, |firmware| firmware.queues.sync_render_holds());
    assert_ne!(admission(&shared).held & bit(30), 0, "a resync keeps the hold while the release is pending");
    // A client closes while its idle pair is lent to a growth, which takes no lease.
    let lent = with(&shared, |firmware| {
        let entry = firmware.queues.render.entries[33].as_mut().unwrap();
        entry.growing = true;
        entry.pair.take()
    });
    backend(&shared, 33, &growing).release_queue().unwrap();
    let raced = with(&shared, |firmware| firmware.queues.admission.raced.lock().unwrap().clone());
    assert!(!raced.contains(&Some(33)), "a closing owner's lent pair is held before admission releases its slot: {raced:?}");
    assert_ne!(admission(&shared).held & bit(33), 0, "a closing owner's lent pair is held while its release is pending");
    with(&shared, |firmware| {
        let entry = firmware.queues.render.entries[33].as_mut().unwrap();
        entry.pair = lent;
        entry.growing = false;
        firmware.queues.hold_render_slot(33);
    });
    assert_ne!(admission(&shared).held & bit(33), 0, "and stays held once the growth returns it");
    // Another client closes; its release is published and consumed, and its pair parked.
    backend(&shared, 31, &healthy).release_queue().unwrap();
    assert_ne!(admission(&shared).held & bit(31), 0);
    let before = wakes(&shared);
    consume(&shared, &healthy, 7);
    let state = admission(&shared);
    assert_eq!(state.held & bit(31), 0, "a consumed release returns its parked pair's slot");
    assert!(wakes(&shared) > before, "and wakes the publications waiting for render capacity");
    assert_eq!(state.clone().acquire(NEW, 1), Some(31), "a new owner takes the parked pair, not the one whose release is pending");
    // A queue the kernel closed for its client (`Firmware::close_failed_owners`) is closed by that
    // client later: its released scheduler gets no second teardown entry, its slot no new hold.
    backend(&shared, 31, &healthy).release_queue().unwrap();
    with(&shared, |firmware| assert!(!firmware.queues.teardown.releasing(&healthy),
                                     "a second close of a released context arms no second scheduler release"));
    assert_eq!(admission(&shared).held & bit(31), 0, "and holds its parked pair's slot no more");
    assert_ne!(state.held & bit(30), 0);
    // Once the stuck release is consumed too, its slot returns, as the older one.
    consume(&shared, &stuck, 9);
    let state = admission(&shared);
    assert_eq!(state.held & (bit(30) | bit(31)), 0);
    assert_ne!(state.held & bit(33), 0);
    assert_eq!(state.clone().acquire(NEW, 1), Some(30), "a slot whose release was consumed goes to a new owner again");
    let raced = with(&shared, |firmware| firmware.queues.admission.raced.lock().unwrap().clone());
    assert!(!raced.iter().take(2).any(|got| *got == Some(30)));
}

/// A close that cannot reserve its teardown entry does not count as the first close: the
/// next close (the client's, or `Firmware::close_failed_owners` again) does all of it.
fn failed_close_is_redone() {
    let failed = context(0x14);
    let shared = device(&[(14, &failed)]);
    FAIL_PUSH.store(true, SeqCst);
    assert_eq!(backend(&shared, 14, &failed).release_queue(), Err(ENOMEM));
    assert!(!failed.is_closed(), "a close that failed to reserve its teardown entry is not noted");
    assert_eq!(admission(&shared).released & bit(14), 0, "and releases nothing");
    backend(&shared, 14, &failed).release_queue().unwrap();
    assert!(failed.is_closed());
    with(&shared, |firmware| assert!(firmware.queues.teardown.releasing(&failed),
                                     "the next close reserves the teardown entry"));
    assert_ne!(admission(&shared).released & bit(14), 0, "and releases the owner");
}

/// Quarantined and owed pairs are held by the same rule; a lent pair keeps its bit.
fn work_and_lent_pairs() {
    let (open, other) = (context(0x12), context(0x20));
    let shared = device(&[(12, &open), (20, &other)]);
    with(&shared, |firmware| firmware.with_render(12, |pair, _| {
        pair.quarantined = true;
        Ok(())
    })).unwrap();
    let state = admission(&shared);
    assert_ne!(state.held & bit(12), 0, "a quarantined pair is held");
    assert_eq!(state.clone().acquire(owner(12), 1), Some(12), "its own open owner keeps its held pair");
    with(&shared, |firmware| firmware.with_render(12, |pair, _| {
        pair.quarantined = false;
        pair.active.push(Active { stamps_owed: true });
        Ok(())
    })).unwrap();
    assert_ne!(admission(&shared).held & bit(12), 0, "a pair whose stamps are owed is held");
    let before = wakes(&shared);
    with(&shared, |firmware| firmware.with_render(12, |pair, _| {
        pair.active.clear();
        Ok(())
    })).unwrap();
    assert_eq!(admission(&shared).held & bit(12), 0, "a drained pair of an open owner is not held");
    assert_eq!(wakes(&shared), before + 1, "and its clear wakes the waiters");
    // A pair lent out keeps its hold across a resync, and its return sets it again.
    with(&shared, |firmware| firmware.with_render(20, |pair, _| {
        pair.quarantined = true;
        Ok(())
    })).unwrap();
    with(&shared, |firmware| {
        let entry = firmware.queues.render.entries[20].as_mut().unwrap();
        let lent = entry.pair.take();
        entry.preparing = Some(());
        firmware.queues.sync_render_holds();
        assert_ne!(firmware.queues.admission.state.lock().held & bit(20), 0, "a lent pair keeps its hold across a resync");
        let entry = firmware.queues.render.entries[20].as_mut().unwrap();
        entry.pair = lent;
        entry.preparing = None;
        firmware.queues.hold_render_slot(20);
        assert_ne!(firmware.queues.admission.state.lock().held & bit(20), 0);
    });
    // A slot without a pair is never held.
    with(&shared, |firmware| {
        firmware.queues.admission.set_held(40, true);
        firmware.queues.sync_render_holds();
    });
    assert_eq!(admission(&shared).held, bit(20), "only the quarantined pair stays held");
}

/// Detaching an idle published sibling in `reclaim_unused_render_spare` must keep
/// `admission.owners[slot]` synced with `entry.reservation.owner`/`entry.context` in
/// `released` state so `exit_scope` + `hold_render_slots_for_exit_kill` still succeed if
/// the prior owner exits before the requester binds the slot.
fn spare_detach_keeps_exit_kill_ownership() {
    let old_ctx = context(0x40);
    let shared = device(&[(1, &old_ctx), (2, &old_ctx)]);
    let old_owner = owner(1);
    with(&shared, |firmware| {
        let mut state = firmware.queues.admission.state.lock();
        state.owners[2] = old_owner;
        state.refs[1] = 1;
        state.active |= bit(1);
        state.published |= bit(1) | bit(2);
        let entry2 = firmware.queues.render.entries[2].as_mut().unwrap();
        entry2.reservation.owner = old_owner;
        entry2.pair.as_mut().unwrap().owner = Some(old_owner);
    });
    let spare = with(&shared, |firmware| {
        firmware
            .queues
            .reclaim_unused_render_spare(Some(NEW), false)
            .unwrap()
    });
    assert!(spare.is_some());
    drop(spare);
    let state = admission(&shared);
    assert_eq!(state.owners[2], old_owner);
    assert_ne!(state.released & bit(2), 0);
    assert_eq!(state.published & bit(2), 0);
    with(&shared, |firmware| {
        {
            let mut state = firmware.queues.admission.state.lock();
            state.refs[1] = 0;
            state.active &= !bit(1);
        }
        let (own, _, slots) = firmware.queues.render.exit_scope(&old_ctx);
        assert_ne!(slots & bit(2), 0);
        assert_eq!(firmware.queues.render.slot_owner(2), Some(old_owner));
        let qids = own;
        let hold = (0..SLOTS as u8)
            .filter(|slot| slots & bit(usize::from(*slot)) != 0
                && firmware.queues.render.qids_for_slot(*slot) & qids != 0)
            .fold(0u64, |acc, slot| acc | bit(usize::from(slot)));
        firmware.queues.admission.release_owner(old_owner);
        assert!(firmware.queues.admission.hold_render_slots_for_exit_kill(old_owner, hold, 0));
    });
}

fn main() {
    closed_owner_release();
    failed_close_is_redone();
    work_and_lent_pairs();
    spare_detach_keeps_exit_kill_ownership();
    println!("PASS release holds: a closed owner's idle pair is held from its close, before admission releases its slot, through retirement passes and resyncs, until its scheduler release is consumed and the pair parked; no new owner is given it meanwhile; quarantined and owed pairs are held by the same rule and their own owner keeps them; a lent pair keeps its bit and is held when its owner closes meanwhile; a missing or parked pair is never held; detaching a published spare preserves exit-kill ownership");
}
"""


# Each drops one part of the runtime's hold rule.
RELEASE_MUTATIONS = {
    'spare-detach-reassigns': ('let claimed = if detach {\n                    self.admission\n                        .release_unused_sibling(slot as u8, expected, true)\n                } else {\n                    self.admission\n                        .reassign_unused_spare(slot as u8, expected, owner)\n                };',
                               'let claimed = self.admission.reassign_unused_spare(slot as u8, expected, owner);'),
    'hold-ignores-release': ('pair.unreusable_work() || teardown.releasing(pair.context())', 'pair.unreusable_work()'),
    'hold-ignores-work': ('pair.unreusable_work() || teardown.releasing(pair.context())', 'teardown.releasing(pair.context())'),
    'close-unheld': ('firmware.queues.sync_render_holds();\n            firmware.queues.admission.release_owner(self.owner);',
                     'firmware.queues.admission.release_owner(self.owner);'),
    # The client's close after the kernel's repeats the firmware side.
    'close-twice': ('if !self.context.is_closed() {', 'if true {'),
    # A close that fails to reserve its teardown entry still counts as the first.
    'latch-before-close': ('if !self.context.is_closed() {',
                           'if !self.context.is_closed() && { self.context.note_closed(firmware.recovery.generation()); true } {'),
    'hold-after-release': ('firmware.queues.sync_render_holds();\n            firmware.queues.admission.release_owner(self.owner);',
                           'firmware.queues.admission.release_owner(self.owner);\n            firmware.queues.sync_render_holds();'),
    'put-back-own-work': ('self.queues.hold_render_slot(slot);\n        result',
                          'let held = self.queues.render.entries[slot].as_ref().and_then(|entry| entry.pair.as_ref())'
                          '.is_some_and(|pair| pair.unreusable_work());\n        self.queues.admission.set_held(slot, held);\n        result'),
    'sync-skips-lent': ('.is_some_and(|context| self.teardown.releasing(context))\n                .then_some(true),',
                        '.is_some_and(|context| self.teardown.releasing(context))\n                .then_some(true).or(Some(false)),'),
    'lent-close-unheld': ('.is_some_and(|context| self.teardown.releasing(context))\n                .then_some(true),',
                          '.is_some_and(|context| self.teardown.releasing(context))\n                .then_some(true).and(None),'),
    'parked-stays-held': ('            None => Some(false),\n        }', '            None => None,\n        }'),
    'sync-clobbers': ('self.held = (self.held & !slots) | (held & slots);', 'self.held = held & Self::MASK;'),
    'sync-no-wake': ('let cleared = self.state.lock().sync_held(slots, held);\n        if cleared {',
                     'let cleared = self.state.lock().sync_held(slots, held);\n        if false {'),
}


def run_fixture(code, name):
    with tempfile.TemporaryDirectory(prefix='g17-admission-' + name + '-') as tmp:
        path = Path(tmp) / (name + '.rs')
        binary = path.with_suffix('')
        path.write_text(code)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
        # A killed mutant exits with the failed control's panic on stderr.
        result = subprocess.run([str(binary)])
        if result.returncode:
            raise SystemExit(result.returncode)


def release_holds(mutation):
    code = release_fixture()
    if mutation:
        old, new = RELEASE_MUTATIONS[mutation]
        assert code.count(old) == 1, mutation
        code = code.replace(old, new)
    run_fixture(code, 'release')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--mutation', choices=sorted(HELD_MUTATIONS.keys() | RELEASE_MUTATIONS.keys()))
    args = ap.parse_args()
    if args.mutation is None:
        compute_order()
        held_slots(None)
        release_holds(None)
    elif args.mutation in HELD_MUTATIONS:
        held_slots(args.mutation)
    else:
        release_holds(args.mutation)


if __name__ == '__main__':
    main()
