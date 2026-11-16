#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the actual ownership transitions with a host-side secret drop observer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
state = (root / 'drivers/soc/apple/key_result.rs').read_text()
source = r'''
#![allow(dead_code)]
extern crate self as kernel;
pub mod prelude {pub(crate) use crate::*;}
#[derive(Clone,Copy,Debug,PartialEq,Eq)] struct Error(i32);
type Result<T=()> = std::result::Result<T,Error>;
const ERESTARTSYS:Error=Error(-512); const ERESTARTNOINTR:Error=Error(-513);
const ERESTARTNOHAND:Error=Error(-514); const ERESTART_RESTARTBLOCK:Error=Error(-516);
const EINTR:Error=Error(-4); const EBUSY:Error=Error(-16); const EIO:Error=Error(-5);
const ETIMEDOUT:Error=Error(-110); const EALREADY:Error=Error(-114);
const ENOENT:Error=Error(-2); const EAGAIN:Error=Error(-11); const ENODATA:Error=Error(-61);
const ECANCELED:Error=Error(-125); const EACCES:Error=Error(-13);
use std::sync::{Arc,Mutex,atomic::{AtomicUsize,Ordering::Relaxed}};
#[derive(Debug)] struct Secret(Arc<AtomicUsize>);
impl Drop for Secret {fn drop(&mut self){self.0.fetch_add(1,Relaxed);}}
mod key_request {pub(crate) type Output=super::Secret;}
mod delivery {
'''+state+r'''
}
#[test]fn terminal_errors_survive_expiry_and_late_completion(){
 for error in [ECANCELED,EACCES,EIO] {
  let mut state=delivery::State::Pending;
  if error==ECANCELED {state.cancel(error).unwrap();} else {assert!(state.complete(Err(error),true));}
  state.expire(); assert_eq!(state.claim().unwrap_err(),error);
  assert!(!state.complete(Err(ETIMEDOUT),false)); assert_eq!(state.claim().unwrap_err(),error);
 }
}
#[test]fn cancellation_and_expiry_destroy_unclaimed_plaintext(){
 for expire in [false,true] {
  let drops=Arc::new(AtomicUsize::new(0));let mut state=delivery::State::Pending;
  assert!(state.complete(Ok(Secret(drops.clone())),false));
  if expire {state.expire();} else {state.cancel(ECANCELED).unwrap();}
  assert_eq!(drops.load(Relaxed),1);
  assert_eq!(state.claim().unwrap_err(),if expire{ETIMEDOUT}else{ECANCELED});
 }
}
#[test]fn cancel_versus_claim_has_one_linearization_point(){
 for _ in 0..100 {
  let drops=Arc::new(AtomicUsize::new(0));let mut initial=delivery::State::Pending;
  initial.complete(Ok(Secret(drops.clone())),false);
  let state=Arc::new(Mutex::new(initial));let other=state.clone();
  let cancel=std::thread::spawn(move||other.lock().unwrap().cancel(ECANCELED));
  let claim=state.lock().unwrap().claim();let canceled=cancel.join().unwrap();
  match claim {Ok(secret)=>{assert_eq!(canceled,Err(EALREADY));drop(secret);},
   Err(error)=>{assert_eq!(error,ECANCELED);assert_eq!(canceled,Ok(()));}}
  assert_eq!(drops.load(Relaxed),1);
 }
}
#[test]fn claimed_output_does_not_hold_a_delivery_lock(){
 let drops=Arc::new(AtomicUsize::new(0));let mut state=delivery::State::Pending;
 state.complete(Ok(Secret(drops.clone())),false);let output=state.claim().unwrap();
 assert_eq!(state.cancel(ECANCELED),Err(EALREADY));state.expire();
 assert_eq!(state.claim().unwrap_err(),ENODATA);drop(output);assert_eq!(drops.load(Relaxed),1);
}
'''

# Exercise the actual completion methods at the worker-retirement boundary too.
request = (root / 'drivers/soc/apple/key_request.rs').read_text()
def method(name):
    start = request.rfind('\n', 0, request.index('fn ' + name + '(')) + 1
    brace = request.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (request[end] == '{') - (request[end] == '}')
        end += 1
    return request[start:end]

source += r"""
mod shim {pub fn monotonic_ns()->u64{0}}
mod completion {
use super::*;
use crate::delivery::State;
use crate::key_request::Output;
use std::sync::{atomic::{AtomicBool,AtomicU64,Ordering::{Acquire,Release}},Condvar};
struct Mutex<T>(std::sync::Mutex<T>);
impl<T>Mutex<T>{fn lock(&self)->std::sync::MutexGuard<'_,T>{self.0.lock().unwrap()}}
struct Event {sequence:AtomicU64,lock:std::sync::Mutex<()>,changed:Condvar}
impl Event {
 fn new()->Self{Self{sequence:AtomicU64::new(0),lock:std::sync::Mutex::new(()),changed:Condvar::new()}}
 fn sequence(&self)->u64{self.sequence.load(Acquire)}
 fn wake(&self){let _lock=self.lock.lock().unwrap();self.sequence.fetch_add(1,Release);self.changed.notify_all();}
 fn wait(&self,sequence:u64,_:u64)->Result{
  let mut lock=self.lock.lock().unwrap();
  while self.sequence.load(Acquire)==sequence{lock=self.changed.wait(lock).unwrap();}Ok(())}
}
struct Completion {phase:Mutex<State>,changed:Event,deadline_ns:u64,retired:AtomicBool}
impl Completion {
 fn new()->Arc<Self>{Arc::new(Self{phase:Mutex(std::sync::Mutex::new(State::Pending)),
  changed:Event::new(),deadline_ns:u64::MAX,retired:AtomicBool::new(false)})}
""" + '\n'.join(method(name) for name in ('complete','retire','cancel','claim','wait_ready')) + r"""
}
#[test]fn success_waits_for_capture_release_without_consuming_output(){
 let done=Completion::new();let drops=Arc::new(AtomicUsize::new(0));
 done.complete(Ok(Secret(drops.clone())));assert!(matches!(done.claim(),Err(EAGAIN)));
 let other=done.clone();let (tx,rx)=std::sync::mpsc::channel();
 let waiter=std::thread::spawn(move||{other.wait_ready().unwrap();tx.send(other.claim()).unwrap();});
 assert!(rx.recv_timeout(std::time::Duration::from_millis(20)).is_err());
 done.retire();drop(rx.recv_timeout(std::time::Duration::from_secs(1)).unwrap().unwrap());
 waiter.join().unwrap();assert_eq!(drops.load(Relaxed),1);assert!(matches!(done.claim(),Err(ENODATA)));
}
#[test]fn cancellation_does_not_wait_for_worker_retirement_or_revive_output(){
 let done=Completion::new();let drops=Arc::new(AtomicUsize::new(0));
 done.complete(Ok(Secret(drops.clone())));done.cancel(ECANCELED).unwrap();done.wait_ready().unwrap();
 assert!(matches!(done.claim(),Err(ECANCELED)));assert_eq!(drops.load(Relaxed),1);
 done.retire();assert!(matches!(done.claim(),Err(ECANCELED)));
}
}
"""

with tempfile.TemporaryDirectory(prefix='sep-context-lifecycle-') as name:
    directory = Path(name)
    (directory / 'test.rs').write_text(source)
    subprocess.run(['rustc', '--edition=2021', '--test', str(directory / 'test.rs'),
                    '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test'), '--test-threads=1'], check=True)
