#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run production compute admission and gate waits with a controlled clock.

Firmware/allocation/locking boundaries are mocks. Binding theft is the review's
legal interleaving: every successful build loses its idle binding before publish.
The oracle measures one total budget, including nested waits, rather than a
fresh timeout per attempt. Mutants must fail at the named assertion.
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
    packet = item(runtime, '    fn compute_packet(')
    ensure = item(runtime, '    fn ensure_compute(')
    for old, new in [('super::job::Packet', 'Packet'),
                     ('super::command::Validated', 'Validated'),
                     ('super::recovery::Memory', 'Memory'),
                     ('super::fw::compute_pool::required_blocks', 'required_blocks')]:
        packet = packet.replace(old, new)
        ensure = ensure.replace(old, new)
    admission = (item(preparation, "pub(super) struct Admission<'a>") + '\n' +
                 item(preparation, "impl<'a> Admission<'a>")).replace('pub(super) ', '').replace('super::now_ns()', 'now_ns()')
    gate = item(preparation, 'pub(super) fn enter_until(').replace('pub(super) ', '')
    return r'''
#![allow(dead_code,unused_variables,irrefutable_let_patterns)]
use std::sync::{Arc,Mutex,MutexGuard,atomic::{AtomicBool,AtomicU64,AtomicUsize,Ordering}};
type Result<T=()> = std::result::Result<T,i32>;
const EINVAL:i32=-22;const ECANCELED:i32=-125;const EIO:i32=-5;const ENODEV:i32=-19;const EBUSY:i32=-16;const EAGAIN:i32=-11;
const RECOVERY_SETTLE_POLLS:usize=200;const WORK_RING_WAIT_NS:u64=2_000_000_000;
static NOW:AtomicU64=AtomicU64::new(0);static CALLS:AtomicUsize=AtomicUsize::new(0);
static MODE:AtomicUsize=AtomicUsize::new(0);static CANCEL:AtomicU64=AtomicU64::new(u64::MAX);
static STATUS:AtomicU64=AtomicU64::new(u64::MAX);static CLOSED:AtomicBool=AtomicBool::new(false);
fn now_ns()->u64{NOW.load(Ordering::SeqCst)}
fn advance(ns:u64){NOW.fetch_add(ns,Ordering::SeqCst);assert!(now_ns()<=6_000_000_000,"admission budget exceeded");}
mod kernel{pub mod time{
 pub fn msecs_to_jiffies(ms:u32)->u32{ms}
 pub struct Delta(pub u64);impl Delta{pub fn from_millis(ms:u64)->Self{Self(ms*1_000_000)}pub fn from_micros(us:i64)->Self{Self(us as u64*1_000)}}
 pub mod delay{pub fn fsleep(d:super::Delta){crate::advance(d.0);}}
}}
''' + admission + r'''
struct Wait;
impl Wait{fn lock(&self){}}
struct Changed;
impl Changed{fn wait_timeout(&self,_:&mut (),_:u32){advance(1_000_000);assert!(now_ns()<=WORK_RING_WAIT_NS,"gate wake storm must remain bounded");}}
struct Gate{wait:Wait,changed:Changed}
struct Lease;
impl Lease{fn is_current(&self)->bool{true}}
impl Gate{
 fn try_enter(self:&Arc<Self>)->Result<Lease>{if CLOSED.load(Ordering::SeqCst){Err(EAGAIN)}else{Ok(Lease)}}
''' + gate + r'''
}
struct Scratch;impl Scratch{fn pages(&self)->u16{0}}
enum Validated{Compute{scratch:Scratch}}
struct Status;impl Status{fn get(&self)->i32{if now_ns()>=STATUS.load(Ordering::SeqCst){-5}else{0}}}
mod g17{pub mod job{#[derive(Clone,Copy,Debug,PartialEq)]pub enum Engine{Render,Compute}}}
// A work-state node is taken per engine: compute admission must ask for a compute one.
struct Work;impl Work{fn acquire(&self,engine:g17::job::Engine)->Result<u32>{assert_eq!(engine,g17::job::Engine::Compute,"compute admission takes a compute work state");if MODE.load(Ordering::SeqCst)==4{Err(EBUSY)}else{Ok(1)}}}
struct Completion;impl Completion{fn status(&self)->Status{Status}fn work_state(&self)->Result<Work>{Ok(Work)}}
struct Packet{command:Validated,completion:Completion}
impl Packet{fn check_dependencies(&self)->Result{Ok(())}fn cancel_requested(&self)->bool{now_ns()>=CANCEL.load(Ordering::SeqCst)}fn is_published(&self)->bool{false}}
struct DeferredBatch;impl DeferredBatch{fn publication()->Result<Self>{Ok(Self)}fn finish(&mut self){}}
struct Recovery;impl Recovery{fn pending(&self)->bool{MODE.load(Ordering::SeqCst)==1}}
struct Init;impl Init{fn compute_vm_cached(&self,_:&())->bool{true}}
struct Context;impl Context{fn vm(&self)->&(){&()}}
struct Memory;impl Memory{fn recovery_state(_:&Init)->Result<u32>{Ok(0)}}
struct Queues;
impl Queues{
 fn compute_bound(&self,_:u64,_:&Context,_:usize)->Result<bool>{Ok(MODE.load(Ordering::SeqCst)!=0)}
 fn compute(&self,_:u64)->Option<()>{if MODE.load(Ordering::SeqCst)==0{None}else{Some(())}}
 fn render_prefix(&self,_:u64,_:&Arc<Packet>)->Result<Vec<()>>{Ok(vec![])}
 fn release_compute_witness(&self,_:&Init,_:&(),_:u64,_:&mut DeferredBatch)->Result<bool>{Ok(MODE.load(Ordering::SeqCst)==2)}
}
struct Firmware{recovery:Recovery,init:Init,queues:Queues,port:()}
impl Firmware{fn replay_compute(&mut self,_:&mut DeferredBatch)->Result{Ok(())}fn publish_compute(&mut self,_:u64,_:Arc<Packet>,_:&[()],_:&mut DeferredBatch)->Result{Err(EAGAIN)}}
struct State(Mutex<Option<Box<Firmware>>>);impl State{fn lock(&self)->MutexGuard<'_,Option<Box<Firmware>>>{self.0.lock().unwrap()}}
struct Available;impl Available{fn observe(&self)->u64{now_ns()}}
struct Shared{preparations:Arc<Gate>,state:State,crashed:AtomicBool,active_cores:u32,progress:Available}impl Shared{fn queue_events(&self){}}
struct ComputeDemand;impl ComputeDemand{fn new(_:&Shared)->Self{Self}}
struct Backend{shared:Shared,owner:u64,context:Context}
fn required_blocks(_:u32,_:u16)->Result<usize>{Ok(1)}
impl Backend{
 fn compute_direct(&self,_:&Arc<Packet>,_:usize,_:&mut DeferredBatch)->Option<Result>{None}
 fn sources_published(&self,_:&Arc<Packet>)->Result<bool>{Ok(true)}
 fn remember_direct(&self,_:&Queues){}
 fn wait_release(&self,_:&Available,_:u64,admission:&Admission<'_>)->Result{
  let _=admission.slice_ms()?;
  advance(1_000_000);
  admission.check()
 }
 fn admission_open(&self)->Result{Ok(())}
 // The real ensure_compute forwards the original context into this allocation
 // boundary. Each success loses its binding before the publication mutex.
 fn build_compute(&self,_:usize,admission:&Admission<'_>)->Result{
  admission.check()?;CALLS.fetch_add(1,Ordering::SeqCst);advance(500_000_000);Ok(())
 }
''' + ensure + '\n' + packet + r'''
}
fn main(){
 let b=Backend{owner:1,context:Context,shared:Shared{active_cores:1,progress:Available,preparations:Arc::new(Gate{wait:Wait,changed:Changed}),state:State(Mutex::new(Some(Box::new(Firmware{recovery:Recovery,init:Init,queues:Queues,port:()})))),crashed:AtomicBool::new(false)}};
 let p=Arc::new(Packet{command:Validated::Compute{scratch:Scratch},completion:Completion});
 for mode in 0..4 {
  MODE.store(mode,Ordering::SeqCst);NOW.store(0,Ordering::SeqCst);CALLS.store(0,Ordering::SeqCst);
  let r=b.compute_packet(&p,&mut DeferredBatch);
  assert_eq!(r,Err(EBUSY),"one admission budget covers binding theft/recovery/publication");
  assert!(now_ns()<=WORK_RING_WAIT_NS+10_000_000,"admission budget exceeded");
  if mode==0{assert_eq!(CALLS.load(Ordering::SeqCst),4);assert_eq!(now_ns(),WORK_RING_WAIT_NS);}
  println!("PASS mode={} result={:?} calls={} elapsed_ms={}",mode,r,CALLS.load(Ordering::SeqCst),now_ns()/1_000_000);
 }
 // A context without a free work state waits for one under the admission
 // deadline instead of failing the packet at once.
 MODE.store(4,Ordering::SeqCst);NOW.store(0,Ordering::SeqCst);CALLS.store(0,Ordering::SeqCst);
 let r=b.compute_packet(&p,&mut DeferredBatch);
 assert_eq!(r,Err(EBUSY),"a busy work state ends at the admission deadline");
 assert!(now_ns()>=WORK_RING_WAIT_NS&&now_ns()<=WORK_RING_WAIT_NS+10_000_000,"a busy work state waits for the admission deadline");
 println!("PASS busy work state waits elapsed_ms={}",now_ns()/1_000_000);
 for status in [false,true] {
  MODE.store(2,Ordering::SeqCst);NOW.store(0,Ordering::SeqCst);CALLS.store(0,Ordering::SeqCst);
  if status{STATUS.store(10_000_000,Ordering::SeqCst);}else{CANCEL.store(10_000_000,Ordering::SeqCst);}
  let r=b.compute_packet(&p,&mut DeferredBatch);
  assert_eq!(r,Err(if status{EIO}else{ECANCELED}),"nested unavailable-capacity wait must check packet");
  assert_eq!(now_ns(),10_000_000,"nested unavailable-capacity cancellation/status must be prompt");assert_eq!(CALLS.load(Ordering::SeqCst),0);
  STATUS.store(u64::MAX,Ordering::SeqCst);CANCEL.store(u64::MAX,Ordering::SeqCst);
 }
 let check=||Ok(());NOW.store(WORK_RING_WAIT_NS,Ordering::SeqCst);
 let admission=Admission::new(WORK_RING_WAIT_NS,&check);
 assert!(matches!(b.shared.preparations.enter_until(&admission),Err(EBUSY)),"spent budget before available gate");
 assert_eq!(b.ensure_compute(0,&admission),Err(EBUSY),"spent budget before nested admission");
 for cancel in [false,true] {
  NOW.store(0,Ordering::SeqCst);CLOSED.store(true,Ordering::SeqCst);
  let check=||{if cancel&&now_ns()>=10_000_000{Err(ECANCELED)}else{Ok(())}};
  let admission=Admission::new(20_000_000,&check);
  assert!(matches!(b.shared.preparations.enter_until(&admission),Err(e) if e==if cancel{ECANCELED}else{EBUSY}),"gate wake storm must check original deadline and cancellation");
  assert_eq!(now_ns(),if cancel{10_000_000}else{20_000_000});
 }
 println!("PASS spent budgets, nested capacity cancellation/status, gate recovery wake storms");
}
'''


def main():
    source = fixture()
    mutants = [
        ('fixed', source, ''),
        ('renew-on-binding-loss', source.replace('let admission = Admission::new(deadline, &check);\n        let progress = &self.shared.progress;\n        loop {', 'let progress = &self.shared.progress;\n        loop {\n            let admission = Admission::new(now_ns().saturating_add(WORK_RING_WAIT_NS), &check);'), 'admission budget exceeded'),
        ('ignore-deadline', source.replace('if now_ns() >= self.deadline', 'if false'), 'admission budget exceeded'),
        ('render-work-state', source.replace('acquire(crate::g17::job::Engine::Compute)', 'acquire(crate::g17::job::Engine::Render)'), 'compute admission takes a compute work state'),
        ('omit-gate-check', source.replace('            admission.check()?;\n            match self.try_enter()', '            match self.try_enter()').replace('admission.slice_ms()?', '10'), 'gate wake storm'),
    ]
    # Bind the old unbounded mutant with the review's eventual cancellation.
    mutants[1] = (mutants[1][0], mutants[1][1].replace('for mode in 0..4 {', 'CANCEL.store(10_000_000_000,Ordering::SeqCst);\n for mode in 0..4 {'), mutants[1][2])
    with tempfile.TemporaryDirectory(prefix='g17-compute-deadline-') as tmp:
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
