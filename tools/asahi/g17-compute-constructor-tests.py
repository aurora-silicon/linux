#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute both production shared-constructor waits and flag cleanup.

Allocations, mappings, device locks and firmware are mocked. Real method bodies
must check the original admission after every wake and after off-lock creation.
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
    preparation = (ROOT / 'preparation.rs').read_text()
    admission = (item(preparation, "pub(super) struct Admission<'a>") + '\n' +
                 item(preparation, "impl<'a> Admission<'a>")).replace('pub(super) ', '').replace('super::now_ns()', 'now_ns()').replace('super::timeout::', '')
    methods = '\n'.join(item(runtime, sig) for sig in ['    fn shared_compute(', '    fn compute_scheduler('])
    methods = methods.replace('super::object::Allocator', 'Allocator').replace('super::preparation::Lease', 'Lease').replace('super::initdata::', '')
    return r'''
#![allow(dead_code)]
use std::cell::Cell;
use std::sync::{Mutex,MutexGuard,atomic::{AtomicBool,AtomicU64,Ordering}};
type Result<T=()> = std::result::Result<T,i32>;
const EIO:i32=-5;const EAGAIN:i32=-11;const ENODEV:i32=-19;const EBUSY:i32=-16;const ECANCELED:i32=-125;
const RECOVERY_WAIT_MS:u32=26_000;
static NOW:AtomicU64=AtomicU64::new(0);static CANCEL:AtomicBool=AtomicBool::new(false);static ALLOCATE:AtomicBool=AtomicBool::new(false);
fn now_ns()->u64{NOW.load(Ordering::SeqCst)}
mod kernel{pub mod time{pub fn msecs_to_jiffies(ms:u32)->u32{ms}}}
''' + admission + r'''
struct Gate;struct Lease;
impl Gate{fn enter_until(&self,a:&Admission<'_>)->Result<Lease>{a.check()?;Ok(Lease)}}
impl Lease{fn is_current(&self)->bool{true}}
struct State(Mutex<Option<Box<Firmware>>>);
impl State{fn lock(&self)->MutexGuard<'_,Option<Box<Firmware>>>{self.0.lock().unwrap()}}
struct Changed;
impl Changed{
 fn wait_timeout<T>(&self,_:&mut MutexGuard<'_,T>,_:u32){NOW.fetch_add(1_000_000,Ordering::SeqCst);assert!(now_ns()<100_000_000,"constructor wait exceeded original budget");}
 fn notify_all(&self){}
}
struct Shared{preparations:Gate,state:State,changed:Changed,crashed:AtomicBool}
impl Shared{fn drm(&self)->Result<()>{Ok(())} fn note_changed(&self){self.changed.notify_all();}}
struct Context;impl Context{fn vm(&self)->&(){&()}}
struct Backend{shared:Shared,context:Context}
struct Allocator<'a>{dev:&'a (),uat:&'a ()}
struct ComputeShared;struct ComputeScheduler;
fn allocate(){if ALLOCATE.load(Ordering::SeqCst){NOW.fetch_add(25_000_000,Ordering::SeqCst);}}
impl ComputeShared{fn new(_:&Allocator<'_>)->Result<Self>{allocate();Ok(Self)}}
impl ComputeScheduler{fn new(_:&Allocator<'_>)->Result<Self>{allocate();Ok(Self)}}
struct Init{shared:bool,scheduler:bool}
impl Init{
 fn has_compute_shared(&self)->bool{self.shared}
 fn has_compute_scheduler(&self)->bool{self.scheduler}
 fn cache_compute_vm(&self,_:&())->Result{Ok(())}
 fn install_compute_shared(&mut self,_:ComputeShared)->Result{self.shared=true;Ok(())}
 fn install_compute_scheduler(&mut self,_:ComputeScheduler)->Result{self.scheduler=true;Ok(())}
}
struct Queues{compute_building:bool,compute_scheduler_building:bool}
struct Firmware{init:Init,uat:(),queues:Queues}
impl Backend{
''' + methods + r'''
}
fn main(){
 for scheduler in [false,true]{for cancel in [false,true]{for building in [false,true]{
  NOW.store(0,Ordering::SeqCst);CANCEL.store(cancel,Ordering::SeqCst);ALLOCATE.store(!building,Ordering::SeqCst);
  let b=Backend{context:Context,shared:Shared{preparations:Gate,changed:Changed,crashed:AtomicBool::new(false),state:State(Mutex::new(Some(Box::new(Firmware{uat:(),init:Init{shared:false,scheduler:false},queues:Queues{compute_building:building,compute_scheduler_building:building}}))))}};
  let check=||{if CANCEL.load(Ordering::SeqCst)&&now_ns()>=10_000_000{Err(ECANCELED)}else{Ok(())}};
  let admission=Admission::new(20_000_000,&check);
  let alloc=Allocator{dev:&(),uat:&()};
  let r=if scheduler{b.compute_scheduler(&alloc,&Lease,&admission)}else{b.shared_compute(&admission)};
  assert_eq!(r,Err(if cancel{ECANCELED}else{EBUSY}),"constructor must propagate deadline/cancel");
  assert_eq!(now_ns(),if building{if cancel{10_000_000}else{20_000_000}}else{25_000_000});
  let state=b.shared.state.lock();let fw=state.as_ref().unwrap();
  assert!(!fw.init.shared&&!fw.init.scheduler,"expired constructor must not install");
  if !building{assert!(!fw.queues.compute_building&&!fw.queues.compute_scheduler_building,"failed constructor must clear builder flag");}
 }}}
 println!("PASS 8 production shared/scheduler constructor waits: expiry, cancellation, off-lock-build expiry, no installation and flag cleanup");
}
'''


def main():
    source = fixture()
    mutants = [
        ('fixed', source, ''),
        ('ignore-constructor-wakes', source.replace('                    admission.check()?;\n                    let firmware', '                    let firmware').replace('                admission.check()?;\n                let firmware', '                let firmware').replace('admission.slice_ms()?', '10'), 'constructor wait exceeded'),
        ('ignore-build-expiry', source.replace('if let Err(error) = admission.check() {', 'if let Err(error) = Ok::<(),i32>(()) {'), 'constructor must propagate'),
    ]
    with tempfile.TemporaryDirectory(prefix='g17-compute-constructors-') as tmp:
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
