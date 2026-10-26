#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production pool wait against a deterministic clock/condition variable.

Every pair remains busy. Fake wakes report an unchanged jiffy budget, including
unrelated wake storms. This tests elapsed time and cancellation, not firmware.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def item(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def fixture():
    source = (ROOT / 'queue/admission.rs').read_text()
    return r'''
#![allow(dead_code)]
use std::sync::{Arc, Mutex, atomic::{AtomicU64, AtomicUsize, Ordering}};
type Result<T=()> = std::result::Result<T,i32>;
const EBUSY:i32=-16;const EIO:i32=-5;const EINVAL:i32=-22;
const ERESTARTSYS:i32=-512;const ECANCELED:i32=-125;
static NOW:AtomicU64=AtomicU64::new(0);
static WAITS:AtomicUsize=AtomicUsize::new(0);
static MODE:AtomicUsize=AtomicUsize::new(0);
mod g17 {pub fn now_ns()->u64 {crate::NOW.load(crate::Ordering::SeqCst)}}
mod kernel {pub mod time {pub fn msecs_to_jiffies(ms:u32)->u64 {u64::from(ms)}}}
mod hazard {pub const WAIT_MS:u32=1000;}
const QUARANTINE_WAIT_MS:u32=1000;
#[derive(Clone,Copy,PartialEq)] enum Plan {Any,Conflict}
#[derive(Clone,Copy)] struct Request<'a>(&'a ());
struct State {owners:[u64;2],quarantined:u64,spared:u64,next_ticket:u64,waiters:usize,conflict:bool,ready:bool}
impl State {
 fn exhausted(&self)->bool {false}
 fn refusal(&self,_:u64)->Result {Ok(())}
 fn plan(&self,_:u64,_:Option<Request<'_>>,_:Option<u64>)->Plan {if self.conflict {Plan::Conflict}else{Plan::Any}}
 fn pick(&mut self,_:u64,_:Plan)->Option<u8> {if self.ready{Some(1)}else{None}}
 fn record(&mut self,_:u64,slot:Option<u8>,_:u64,_:Request<'_>)->Option<usize> {if slot.is_none(){self.waiters+=1;}Some(0)}
 fn forget_waiter(&mut self,t:Option<u64>){if t.is_some(){self.waiters-=1;}}
}
struct Lock(Mutex<State>);
impl Lock {fn lock(&self)->std::sync::MutexGuard<'_,State>{self.0.lock().unwrap()}}
enum CondVarTimeoutResult {Signal{jiffies:u64},Timeout,Woken{jiffies:u64}}
struct CondVar;
impl CondVar {
 fn wait_interruptible_timeout(&self,_:&mut std::sync::MutexGuard<'_,State>,slice:u64)->CondVarTimeoutResult {
  assert!(WAITS.fetch_add(1,Ordering::SeqCst)<100_000,"wait did not terminate");
  assert!(slice<=10,"cancellation observation must be bounded");
  match MODE.load(Ordering::SeqCst) {
   0=>{NOW.fetch_add(slice*1_000_000,Ordering::SeqCst);CondVarTimeoutResult::Timeout},
   1=>{NOW.fetch_add(100_000,Ordering::SeqCst);CondVarTimeoutResult::Woken{jiffies:u64::MAX}},
   _=>CondVarTimeoutResult::Signal{jiffies:slice},
  }
 }
}
struct Pool {state:Lock,released:CondVar}
struct RenderLease {pool:Arc<Pool>,slot:u8,record:Option<usize>}
''' + item(source, 'fn wait_slice_ms(') + '\nimpl Pool {\n' + item(source, 'pub(crate) fn acquire_render(') + r'''
}
fn new_pool(conflict:bool,quarantined:u64)->Arc<Pool> {
 Arc::new(Pool{state:Lock(Mutex::new(State{owners:[0,1],quarantined,spared:0,next_ticket:1,waiters:0,conflict,ready:false})),released:CondVar})
}
fn reset(mode:usize){NOW.store(0,Ordering::SeqCst);WAITS.store(0,Ordering::SeqCst);MODE.store(mode,Ordering::SeqCst);}
fn main(){
 for mode in 0..2 {
  for conflict in [false,true] {
   reset(mode);let pool=new_pool(conflict,0);
   let result=pool.acquire_render(1,Some(Request(&())),2_000_000_000,||Ok(()));
   assert!(matches!(result,Err(EBUSY)),"busy wait must expire");
   assert!(NOW.load(Ordering::SeqCst)<=2_010_000_000,"absolute deadline extended by wakes");
   assert_eq!(pool.state.lock().waiters,0,"expired waiter must be removed");
   reset(mode);let pool=new_pool(conflict,0);
   let result=pool.acquire_render(1,Some(Request(&())),2_000_000_000,||if g17::now_ns()>=5_000_000 {Err(ECANCELED)}else{Ok(())});
   assert!(matches!(result,Err(ECANCELED)),"cancel must interrupt a busy wait");
   assert!(NOW.load(Ordering::SeqCst)<=15_000_000,"cancel observation bound");
   assert_eq!(pool.state.lock().waiters,0,"cancelled waiter must be removed");
  }
  reset(mode);let pool=new_pool(false,2);
  assert!(matches!(pool.acquire_render(1,None,2_000_000_000,||Ok(())),Err(EIO)));
  assert!(NOW.load(Ordering::SeqCst)<=1_010_000_000,"quarantine budget extended");
  reset(mode);let pool=new_pool(false,2);pool.state.lock().spared=2;
  assert!(matches!(pool.acquire_render(1,None,2_000_000_000,||Ok(())),Err(EBUSY)));
  assert!(NOW.load(Ordering::SeqCst)<=1_010_000_000,"spared quarantine budget extended");
 }
 reset(2);let pool=new_pool(true,0);
 assert!(matches!(pool.acquire_render(1,Some(Request(&())),2_000_000_000,||Ok(())),Err(ERESTARTSYS)));
 assert_eq!(pool.state.lock().waiters,0);
 // A previous outer retry already spent the budget. Even available capacity
 // cannot restart it or install a lease after cancellation.
 reset(0);NOW.store(2_000_000_000,Ordering::SeqCst);let pool=new_pool(false,0);pool.state.lock().ready=true;
 assert!(matches!(pool.acquire_render(1,None,2_000_000_000,||Ok(())),Err(EBUSY)));
 assert!(matches!(pool.acquire_render(1,None,u64::MAX,||Err(ECANCELED)),Err(ECANCELED)));
 assert_eq!(WAITS.load(Ordering::SeqCst),0);
 println!("PASS production admission: all-pairs-busy/no-progress and 20,000 unrelated wakes expire at absolute deadline; no-event cancellation bounded; conflict/quarantine/signal cleanup preserved");
}
'''


def main():
    source = fixture()
    caller = (ROOT / 'runtime/render/submit.rs').read_text()
    assert 'Request::of(packet), deadline_ns, || {' in caller
    assert 'packet.cancel_requested() || self.closed.load(Ordering::Acquire)' in caller
    cases = [('fixed', source),
             ('renew-deadline-on-wake', source.replace('wait_slice_ms(deadline_ns, now)?', 'wait_slice_ms(now.saturating_add(2_000_000_000), now)?')),
             ('ignore-cancel', source.replace('check()?;', ''))]
    with tempfile.TemporaryDirectory(prefix='g17-admission-deadline-') as tmp:
        for name, code in cases:
            path = Path(tmp) / (name + '.rs')
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            if name == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0, name
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
