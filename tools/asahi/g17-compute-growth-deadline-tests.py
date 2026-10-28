#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute the complete production growth wait, abort and QID handback.

Firmware witnesses and allocations are mocks; cancellation/expiry must abort the
transaction and drop tentative aliases before a fresh identity can be returned.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def item(text, signature):
    start = text.index(signature)
    begin = text.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def fixture():
    runtime = (ROOT / 'runtime.rs').read_text()
    prep = (ROOT / 'preparation.rs').read_text()
    admission = (item(prep, "pub(super) struct Admission<'a>") + '\n' +
                 item(prep, "impl<'a> Admission<'a>")).replace('pub(super) ', '').replace('super::now_ns()', 'now_ns()').replace('super::timeout::', '')
    methods = '\n'.join(item(runtime, sig) for sig in ['    fn grow_compute(', '    fn finish_uninstalled_compute('])
    methods = methods.replace('super::object::Allocator', 'Allocator').replace('super::freelist::Pages', 'Pages')
    return r'''
#![allow(dead_code)]
use std::cell::Cell;
use std::sync::{Mutex,MutexGuard,atomic::{AtomicBool,AtomicU64,Ordering,fence}};
type Result<T=()> = std::result::Result<T,i32>;
type KBox<T> = Box<T>;
const EIO:i32=-5;const EAGAIN:i32=-11;const ENODEV:i32=-19;const EBUSY:i32=-16;const ECANCELED:i32=-125;
const RECOVERY_WAIT_MS:u32=26_000;
static NOW:AtomicU64=AtomicU64::new(0);static ALIAS:AtomicBool=AtomicBool::new(false);static PAGES:AtomicBool=AtomicBool::new(false);
static RESERVED:AtomicBool=AtomicBool::new(false);static ABORTED:AtomicBool=AtomicBool::new(false);
fn now_ns()->u64{NOW.load(Ordering::SeqCst)}
mod kernel{pub mod time{
 pub struct Delta(pub u64);impl Delta{pub fn from_millis(ms:u64)->Self{Self(ms)}}
 pub mod delay{pub fn fsleep(d:super::Delta){crate::NOW.fetch_add(d.0*1_000_000,crate::Ordering::SeqCst);assert!(crate::now_ns()<100_000_000,"growth admission exceeded original deadline");}}
}}
mod g17{pub const MSG_CONTROL_NOTIFY:u32=0;}
''' + admission + r'''
struct Gate;struct Lease;
impl Gate{fn enter_until(&self,a:&Admission<'_>)->Result<Lease>{a.check()?;Ok(Lease)}}
impl Lease{fn is_current(&self)->bool{true}}
struct State(Mutex<Option<Box<Firmware>>>);
impl State{fn lock(&self)->MutexGuard<'_,Option<Box<Firmware>>>{self.0.lock().unwrap()}}
struct Available;impl Available{fn observe(&self)->u64{now_ns()}fn notify(&self){}}
struct Shared{preparations:Gate,state:State,crashed:AtomicBool,progress:Available,channels:Available}
impl Shared{fn drm(&self)->Result<()>{Ok(())}fn queue_events(&self){}}
struct Context;impl Context{fn vm(&self)->&(){&()}}
struct Backend{shared:Shared,context:Context,owner:u64}
struct Allocator<'a>{dev:&'a (),uat:&'a ()}
struct Pages;struct Alias;
impl Pages{
 fn compute_sized(_:&Allocator<'_>,_:usize)->Result<Self>{PAGES.store(true,Ordering::SeqCst);Ok(Self)}
 fn map_client(&self,_:&())->Result<Alias>{ALIAS.store(true,Ordering::SeqCst);Ok(Alias)}
}
impl Drop for Pages{fn drop(&mut self){PAGES.store(false,Ordering::SeqCst);}}
impl Drop for Alias{fn drop(&mut self){ALIAS.store(false,Ordering::SeqCst);}}
mod compute{pub(crate) use crate::Queue;}
struct Queue{fresh:bool}
impl Queue{
 fn pool_fits(&self,_:usize)->bool{false}
 fn begin_growth(&mut self,_:&(),_:usize)->Result<bool>{Ok(false)}
 fn growth_token(&self)->Option<u64>{Some(7)}
 fn finish_growth(&mut self,_:&(),_:&mut Option<Pages>,_:&mut Option<Alias>,_:usize)->Result{Err(EAGAIN)}
 fn abort_growth(&mut self,t:u64)->Result<Option<bool>>{assert_eq!(t,7);ABORTED.store(true,Ordering::SeqCst);Ok(Some(self.fresh))}
}
impl Drop for Queue{fn drop(&mut self){if self.fresh{assert!(RESERVED.load(Ordering::SeqCst));assert!(!ALIAS.load(Ordering::SeqCst)&&!PAGES.load(Ordering::SeqCst),"growth aliases must precede fresh QID return");}}}
#[derive(Clone,Copy)]struct ComputeReservation;
struct Entry{reservation:ComputeReservation,queue:Option<Box<Queue>>}
struct Registry{compute:Vec<Option<Entry>>}
impl Registry{
 fn compute(&self,_:u64)->Option<&Queue>{self.compute[0].as_ref().unwrap().queue.as_deref()}
 fn compute_mut(&mut self,_:u64)->Option<&mut Queue>{self.compute[0].as_mut().unwrap().queue.as_deref_mut()}
 fn cancel_compute(&mut self,_:ComputeReservation)->Result{assert!(!ALIAS.load(Ordering::SeqCst)&&!PAGES.load(Ordering::SeqCst));RESERVED.store(false,Ordering::SeqCst);Ok(())}
}
struct Primary;impl Primary{fn notify(&self,_:u32)->Result{Ok(())}}
struct Firmware{queues:Registry,uat:(),init:(),primary:Primary}
impl Firmware{fn scan_compute(&mut self,_:Option<()>,_:&mut DeferredBatch)->Result{Ok(())}}
struct DeferredBatch;impl DeferredBatch{fn publication()->Result<Self>{Ok(Self)}fn finish(&mut self){}}
impl Backend{
 fn wait_release(&self,_:&Available,_:u64,admission:&Admission<'_>)->Result{
  let _=admission.slice_ms()?;
  kernel::time::delay::fsleep(kernel::time::Delta::from_millis(1));
  admission.check()
 }
''' + methods + r'''
}
fn main(){
 for fresh in [false,true]{for cancel in [false,true]{
  NOW.store(0,Ordering::SeqCst);ABORTED.store(false,Ordering::SeqCst);RESERVED.store(true,Ordering::SeqCst);
  let b=Backend{owner:1,context:Context,shared:Shared{preparations:Gate,progress:Available,channels:Available,crashed:AtomicBool::new(false),state:State(Mutex::new(Some(Box::new(Firmware{uat:(),init:(),primary:Primary,queues:Registry{compute:vec![Some(Entry{reservation:ComputeReservation,queue:Some(Box::new(Queue{fresh}))})]}}))))}};
  let check=||{if cancel&&now_ns()>=10_000_000{Err(ECANCELED)}else{Ok(())}};
  let admission=Admission::new(20_000_000,&check);
  assert_eq!(b.grow_compute(42,&admission),Err(if cancel{ECANCELED}else{EBUSY}),"growth must propagate cancel/deadline");
  assert_eq!(now_ns(),if cancel{10_000_000}else{20_000_000});
  assert!(ABORTED.load(Ordering::SeqCst));assert!(!ALIAS.load(Ordering::SeqCst)&&!PAGES.load(Ordering::SeqCst));
  assert_eq!(RESERVED.load(Ordering::SeqCst),!fresh,"only unpublished QIDs may return without firmware release");
 }}
 println!("PASS 4 complete production growth waits: deadline, cancellation, installed retention, fresh rollback and tentative-alias/QID order");
}
'''


def main():
    source = fixture()
    mutants = [('fixed', source, ''),
               ('ignore-shared-deadline', source.replace('if now_ns() >= self.deadline.get()', 'if false').replace('if remaining == 0 { return Err(EBUSY); }', ''), 'growth admission exceeded'),
               ('keep-growth-alias', source.replace('                drop(alias.take());', ''), 'growth aliases must precede')]
    with tempfile.TemporaryDirectory(prefix='g17-compute-growth-') as tmp:
        for name, code, expected in mutants:
            if name != 'fixed':
                assert code != source, f'vacuous mutation: {name}'
            path = Path(tmp) / (name + '.rs')
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '-Awarnings', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            if name == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and expected in result.stderr, result.stderr
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
