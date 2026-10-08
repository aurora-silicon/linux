#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute production publication/error/packet methods with host driver doubles.

Run from any directory. --revision accepts an immutable Git commit. The driver
double exposes live WRITE/counters and injects fragment/timestamp errors; the
preparation loop, publication result, pending/active handling, guard release and
fence ownership are extracted from the requested production bytes, not modeled.
--negative-controls requires the pre-fix and three unsafe mutations to fail.
No device access or kernel build is performed.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = 'drivers/gpu/drm/asahi/m3_runtime.rs'
SUBMIT = 'drivers/gpu/drm/asahi/m3_submit.rs'
PREFIX = '1c0f80535227d65203741b4b7698dad4fe2ae529'

def source(path, revision):
    if revision:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', f'{revision}:{path}'], text=True)
    return (ROOT / path).read_text()

def method(text, name):
    start = text.index('fn ' + name + '(')
    opening = text.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[start:cursor]

STUBS = r'''
#![allow(dead_code, unused_variables, unreachable_patterns)]
use std::{cell::{Cell, RefCell, RefMut}, rc::Rc, sync::{Arc, atomic::{AtomicBool, Ordering}}, ops::{Deref,DerefMut}, marker::PhantomData, pin::Pin};
#[derive(Clone, Copy, PartialEq, Eq, Debug)] struct Error(i32);
impl Error { fn to_errno(self)->i32 {self.0} }
type Result<T=()> = std::result::Result<T,Error>;
const ENOMEM:Error=Error(12); const EIO:Error=Error(5); const EINVAL:Error=Error(22);
const ENOTSUPP:Error=Error(95); const EBUSY:Error=Error(16); const EOVERFLOW:Error=Error(75);
const GFP_KERNEL:()=();
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
 fn new()->Self {Self(Vec::new())}
 fn with_capacity(n:usize,_:())->Result<Self> {Ok(Self(Vec::with_capacity(n)))}
 fn reserve(&mut self,n:usize,_:())->Result {self.0.reserve(n);Ok(())}
 fn push(&mut self,t:T,_:())->Result {self.0.push(t);Ok(())}
 fn remove(&mut self,i:usize)->Result<T> {if i<self.len(){Ok(self.0.remove(i))}else{Err(EIO)}}
 fn drain_all(&mut self)->std::vec::Drain<'_,T> {self.0.drain(..)}
 fn retain(&mut self,f:impl FnMut(&T)->bool) {self.0.retain(f)}
}
impl<T> Deref for KVec<T>{type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T> DerefMut for KVec<T>{fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
impl<T> IntoIterator for KVec<T>{type Item=T;type IntoIter=std::vec::IntoIter<T>;fn into_iter(self)->Self::IntoIter{self.0.into_iter()}}
struct Lock<T>(RefCell<T>); impl<T> Lock<T>{fn lock(&self)->RefMut<'_,T>{self.0.borrow_mut()}}
struct Guard(Rc<Cell<usize>>);impl Drop for Guard{fn drop(&mut self){self.0.set(self.0.get()-1)}}
#[derive(Clone)] struct Vm;impl Vm {fn status(&self)->Result<Status>{Ok(Status)}}
struct Status;impl Status{fn record(&self,_:i32){}}
struct Completion{signals:Cell<usize>,error:Cell<Option<Error>>}
impl Completion{fn set_error(&self,e:Error){self.error.set(Some(e))}fn signal(&self){self.signals.set(self.signals.get()+1)}}
mod agx_memory{pub fn publish(){}}
mod policy{pub const SLOTS:usize=16;pub fn packet_retired(p:bool,a:bool)->bool{!p&&!a}}
mod m3_pass_layout{pub const SLOTS:usize=16;}
mod m3_params {pub enum G15Debug{} }
mod m3_submit {
 use super::*;
 #[derive(Clone,Copy)] pub enum Command {Render{command:u32,usc:u64},Compute(u32)}
 pub struct Packet{pub vm:Vm,pub commands:Vec<Command>,pub wide_visibility:Vec<bool>,pub timestamp_failure:bool,pub finish_claimed:AtomicBool,pub vm_job:Lock<Option<Guard>>,pub completion:Completion}
 impl Packet{
 pub fn timestamp_addresses(&self,_:usize)->[u64;2]{[0;2]}
 pub fn render_timestamp_addresses(&self,_:usize)->[[u64;2];2]{[[u64::from(self.timestamp_failure),0],[0,0]]}
 PACKET_METHODS
 }
}
struct Monotonic;struct Instant<T>(PhantomData<T>);
impl<T> Instant<T>{fn now()->Self{Self(PhantomData)}fn elapsed(&self)->Elapsed{Elapsed}}
struct Elapsed;impl Elapsed{fn as_nanos(&self)->i64{0}}
fn physical_counter()->u64{0}
mod debug{pub enum DebugFlags{M3PassTiming,M3SubmitSummary}pub fn debug_enabled(_:DebugFlags)->bool{false}}
mod t8122_start{use super::*;pub fn is_t8122(_: &Soc)->bool{false}pub fn job_setup_failed_verdict(_: &Drm,_:Error){}}
struct Soc{registers:(),clusters:usize}
struct Device;impl Device{fn soc(&self)->&Soc{static S:Soc=Soc{registers:(),clusters:1}; &S}}
struct Drm;impl Drm{fn as_ref(&self)->&Self{self}}
struct Uat;
struct Health{failed:Cell<bool>,pending:Cell<bool>}
impl Health{fn set_gpu_pending(&self,b:bool){self.pending.set(b)}}
struct State{health:Health}impl State{fn healthy(&self)->bool{!self.health.failed.get()}}
struct Config{completed_events:u64}impl Config{
 fn stats_region(&self)->Result<()>{Ok(())}
 fn submit_queue(&mut self,_:usize,_:u64,_:u16,_:usize,_:bool)->Result{Ok(())}
 fn render_pb(&mut self)->Result{Ok(())}
}
struct Transport;impl Transport{fn send_message(self:Pin<&mut Self>,_:u64,_:u64)->Result{Ok(())}}
// Backing-memory double: a live firmware consumer can observe WRITE immediately.
// Fragment error occurs on the second append; timestamp error occurs after WRITE.
struct Render{writes:u32,ta_write:u32,counter:u32,draw:u64,count:usize,base:usize,checkpoint:(u32,u32,u64),aborts:usize,resets:Vec<usize>,append_calls:usize}
impl Render{
 fn new(_: &Drm,_:&Uat,_:&Vm,_:(),_:usize,_:())->Result<Self>{Ok(Self::empty())}
 fn empty()->Self{Self{writes:0,ta_write:0,counter:0,draw:0,count:0,base:0,checkpoint:(0,0,0),aborts:0,resets:Vec::new(),append_calls:0}}
 fn begin_batch(&mut self,_:&Drm,_:&Uat,_:&Vm,_:&[m3_submit::Command],base:usize,_:bool)->Result{
  self.checkpoint=(self.writes,self.counter,self.draw);self.base=base;self.count=0;Ok(())
 }
 fn append(&mut self,command:u32,usc:u64)->Result{
  self.append_calls+=1;self.resets.push(self.base+self.count);self.counter+=1;self.draw+=1;self.count+=1;
  // Failed fragment encoding follows mutation of shared counters/pass storage.
  usc.checked_add(u64::from(command)).ok_or(EOVERFLOW)?;
  self.ta_write+=1;
  if command==2{return Err(EIO)} // A first pipe WRITE was exposed, second failed.
  self.writes+=1;Ok(())
 }
 fn set_user_timestamps(&mut self,a:[[u64;2];2])->Result{if a[0][0]!=0{Err(EINVAL)}else{Ok(())}}
 fn abort_batch(&mut self)->Result{self.aborts+=1;(self.writes,self.counter,self.draw)=self.checkpoint;Ok(())}
 fn first(&self)->bool{false}fn queues(&self)->[u64;2]{[0;2]}fn heads(&self)->[u16;2]{[0;2]}
 fn batch(&self)->(usize,usize,u64){(self.base,self.count,self.draw)}
}
mod m3_render{pub(crate) use super::Render;}
struct Compute;impl Compute{
 fn new(_: &Drm,_:&Uat,_:&Vm,_:(),_:u32,_:())->Result<Self>{Ok(Self)}
 fn replay(&mut self,_:&Uat,_:&Vm,_:u32)->Result{Ok(())}
 fn append(&mut self,_:u32)->Result{Ok(())}fn prepare_completion(&mut self,_:bool)->Result{Ok(())}
 fn set_user_timestamps(&mut self,_:[u64;2])->Result{Ok(())}fn set_attachments(&mut self,_:&())->Result{Ok(())}
 fn abort_batch(&mut self)->Result{Ok(())}fn queue(&self)->u64{0}fn head(&self)->u16{0}fn first(&self)->bool{false}
}
mod m3_compute{pub(crate) use super::Compute;}
mod m3_compute_storage{pub fn coalesce_stamp_flush(_:bool,_:[u64;2],_:bool)->bool{false}}
enum NativeJob{Render(Render),Compute(Compute)}
struct Batch{entries:KVec<(Arc<m3_submit::Packet>,usize)>,index:usize,kind:usize,first:m3_submit::Command,preparation_ns:i64,previous_events:u64,expected_events:u64,start:Instant<Monotonic>,measure:bool,doorbell_counter_ns:i64,polling_ns:i64,polls:u64,stamp_ns:i64,first_event_ns:i64,events_ns:i64,base:usize,last_draw:u64,publication_tick:u64}
struct Inner{pending:KVec<(Arc<m3_submit::Packet>,usize)>,active:KVec<Batch>,packets:KVec<Arc<m3_submit::Packet>>,jobs:KVec<NativeJob>,fault_captured:bool,gpu_pending:bool,state:State,config:Config,device:Device,drm:Drm,uat:Uat,transport:Transport,t8122_verdict:bool}
impl Inner{
 fn capture_fault(&mut self,_:Error){self.fault_captured=true;self.state.health.failed.set(true)}
 fn fail(&mut self,_:usize,_:&Vm,e:Error){self.capture_fault(e)}
 fn pipeline_slot(&mut self)->Result<Option<(usize,usize)>>{Ok(Some((if self.active.is_empty(){0}else{1},16)))}
 fn next_batch(&self,_:usize)->Result<KVec<(Arc<m3_submit::Packet>,usize)>>{
  let mut out=KVec::new();let(p,start)=&self.pending[0];for i in *start..p.commands.len(){out.push((p.clone(),i),())?}Ok(out)
 }
 INNER_METHODS
}
fn packet(count:usize,bad_second:bool,bad_timestamp:bool,guards:&Rc<Cell<usize>>)->Arc<m3_submit::Packet>{
 guards.set(guards.get()+1);
 Arc::new(m3_submit::Packet{vm:Vm,commands:(0..count).map(|i|m3_submit::Command::Render{command:1,usc:if bad_second&&i==1{u64::MAX}else{0}}).collect(),wide_visibility:vec![false;count],timestamp_failure:bad_timestamp,finish_claimed:AtomicBool::new(false),vm_job:Lock(RefCell::new(Some(Guard(guards.clone())))),completion:Completion{signals:Cell::new(0),error:Cell::new(None)}})
}
fn inner()->Inner{let mut jobs=KVec::new();jobs.push(NativeJob::Render(Render::empty()),()).unwrap();Inner{pending:KVec::new(),active:KVec::new(),packets:KVec::new(),jobs,fault_captured:false,gpu_pending:false,state:State{health:Health{failed:Cell::new(false),pending:Cell::new(false)}},config:Config{completed_events:0},device:Device,drm:Drm,uat:Uat,transport:Transport,t8122_verdict:false}}
fn render(i:&Inner)->&Render{let NativeJob::Render(r)=&i.jobs[0]else{panic!()};r}
fn seed(i:&mut Inner,guards:&Rc<Cell<usize>>)->Arc<m3_submit::Packet>{
 let p=packet(1,false,false,guards);let mut e=KVec::new();e.push((p.clone(),0),()).unwrap();let b=i.publish(&e,0,false).unwrap();i.active.push(b,()).unwrap();p
}
#[test]fn second_fragment_failure_retains_live_writes_guards_and_slots(){failure(true,false)}
#[test]fn late_timestamp_failure_retains_live_writes_guards_and_slots(){failure(false,true)}
#[test]fn first_append_shared_counter_failure_is_quarantined(){early_failure(false)}
#[test]fn first_append_partial_pipe_write_is_quarantined(){early_failure(true)}
fn early_failure(pipe:bool){
 let guards=Rc::new(Cell::new(0));let mut i=inner();let old=seed(&mut i,&guards);
 let mut bad=packet(1,false,false,&guards);
 Arc::get_mut(&mut bad).unwrap().commands[0]=m3_submit::Command::Render{command:if pipe{2}else{1},usc:if pipe{0}else{u64::MAX}};
 let queued=packet(1,false,false,&guards);i.pending.push((bad.clone(),0),()).unwrap();i.pending.push((queued.clone(),0),()).unwrap();
 assert!(i.publish_next());assert!(i.fault_captured);assert_eq!(render(&i).aborts,0);
 assert_eq!(render(&i).counter,2);assert_eq!(render(&i).writes,1);assert_eq!(render(&i).ta_write,if pipe{2}else{1});
 assert_eq!(render(&i).append_calls,2);assert_eq!(guards.get(),3);
 assert!(old.vm_job.lock().is_some());assert!(bad.vm_job.lock().is_some());assert!(i.gpu_pending);
 let before=render(&i).resets.clone();assert!(!i.publish_next());assert_eq!(render(&i).resets,before);
}
fn failure(fragment:bool,timestamp:bool){
 let guards=Rc::new(Cell::new(0));let mut i=inner();let old=seed(&mut i,&guards);
 let bad=packet(2,fragment,timestamp,&guards);let queued=packet(1,false,false,&guards);
 i.pending.push((bad.clone(),0),()).unwrap();i.pending.push((queued.clone(),0),()).unwrap();
 assert!(i.publish_next());assert!(i.fault_captured);assert!(!i.state.healthy());assert!(i.gpu_pending);
 assert_eq!(render(&i).aborts,0,"live WRITE must not rewind");
 assert_eq!(render(&i).writes,2,"older pass plus first appended pass stay published");
 assert_eq!(render(&i).counter,if fragment{3}else{2},"shared counters cannot rewind");
 assert_eq!(render(&i).append_calls,if fragment{3}else{2},"no next packet publication");
 assert_eq!(guards.get(),3,"error fences must not release any DMA guard");
 for p in [&old,&bad,&queued]{assert_eq!(p.completion.signals.get(),1);assert!(p.vm_job.lock().is_some())}
 assert!(i.packets.iter().any(|p|Arc::ptr_eq(p,&bad)));assert!(i.active.is_empty());
 let before=render(&i).resets.clone();assert!(!i.publish_next());assert_eq!(render(&i).resets,before);
 assert_eq!(i.packets.len(),2,"both potentially published packet owners remain quarantined");
}
#[test]fn successful_overlap_can_retire_owners(){
 let guards=Rc::new(Cell::new(0));let mut i=inner();let old=seed(&mut i,&guards);let p=packet(2,false,false,&guards);
 i.pending.push((p.clone(),0),()).unwrap();assert!(i.publish_next());assert_eq!(i.active.len(),2);
 assert_eq!(render(&i).writes,3);assert_eq!(guards.get(),2);assert!(i.state.healthy());
 let first=i.active.remove(0).unwrap();i.retire_packets(&first.entries);assert_eq!(guards.get(),1);
 let second=i.active.remove(0).unwrap();i.retire_packets(&second.entries);assert_eq!(guards.get(),0);
 assert_eq!(old.completion.signals.get(),1);assert_eq!(p.completion.signals.get(),1);
}
#[test]fn single_flight_error_aborts_and_releases_unpublished_guard(){
 let guards=Rc::new(Cell::new(0));let mut i=inner();let bad=packet(2,true,false,&guards);
 i.pending.push((bad.clone(),0),()).unwrap();assert!(i.publish_next());assert!(i.state.healthy());
 assert_eq!(render(&i).aborts,1);assert_eq!(render(&i).writes,0);assert_eq!(render(&i).counter,0);
 assert_eq!(guards.get(),0);assert!(i.active.is_empty());assert_eq!(bad.completion.signals.get(),1);
}
'''

def harness(runtime, submit):
    methods = '\n'.join(method(runtime, n) for n in
                       ['publish', 'publish_next', 'fail_front', 'fail_active',
                        'fail_pending', 'finish_all', 'packet_active', 'retire_packets'])
    packets = '\n'.join('pub(crate) ' + method(submit, n) for n in ['finish', 'release_guard', 'retired'])
    # Additional fields consumed by Compute's compile-only branch.
    return STUBS.replace('INNER_METHODS', methods).replace('PACKET_METHODS', packets).replace(
        'pub timestamp_failure:bool,', 'pub attachments:Vec<()>,pub timestamp_failure:bool,').replace(
        'timestamp_failure:bad_timestamp,', 'attachments:vec![();count],timestamp_failure:bad_timestamp,')

def run(runtime, submit, label, expect_pass):
    with tempfile.TemporaryDirectory(prefix='m3-publication-') as tmp:
        p = Path(tmp, 'controls.rs'); p.write_text(harness(runtime, submit))
        binary = Path(tmp, 'controls')
        build = subprocess.run(['rustc', '--edition=2021', '--test', str(p), '-o', str(binary)], capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stderr)
        result = subprocess.run([str(binary), '--test-threads=1'], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError(f'{label}: unexpected result\n{result.stdout}\n{result.stderr}')
        if not expect_pass:
            for name in ['second_fragment_failure_retains_live_writes_guards_and_slots',
                         'late_timestamp_failure_retains_live_writes_guards_and_slots']:
                if f'test {name} ... FAILED' not in result.stdout:
                    raise AssertionError(f'{label}: required overlap negative control did not fail')
        print(label, 'PASS' if expect_pass else 'REJECTED', flush=True)
        print(result.stdout, end='', flush=True)

def main():
    args = argparse.ArgumentParser()
    args.add_argument('--revision')
    args.add_argument('--negative-controls', action='store_true')
    opts = args.parse_args()
    runtime, submit = source(RUNTIME, opts.revision), source(SUBMIT, opts.revision)
    for path, data in [(RUNTIME, runtime), (SUBMIT, submit)]:
        print(path, hashlib.sha256(data.encode()).hexdigest(), flush=True)
    run(runtime, submit, 'production', True)
    if opts.negative_controls:
        run(source(RUNTIME, PREFIX), source(SUBMIT, PREFIX), 'pre-fix-1c0f', False)
        needle = 'if overlap {return Err((error,true));}'
        if runtime.count(needle) != 1:
            raise AssertionError('production overlap failure boundary changed; update mutation')
        run(runtime.replace(needle, 'if overlap {return Err((error,false));}'), submit,
            'unpublished-live-error', False)
        run(runtime.replace(needle, 'if overlap {j.abort_batch()?;return Err((error,true));}').replace(
            'j.abort_batch()?;return Err((error,true));', 'j.abort_batch().map_err(|e|(e,true))?;return Err((error,true));'), submit,
            'rewind-live-producer', False)
        run(runtime, submit.replace('self.completion.set_error(e);', 'self.completion.set_error(e);self.release_guard();'),
            'release-guard-on-error-fence', False)

if __name__ == '__main__':
    main()
