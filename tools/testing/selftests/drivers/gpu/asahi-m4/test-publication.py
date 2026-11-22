#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute production M4 publication/error isolation with a CPU-only transport recorder."""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

production = kernel_root() / 'drivers/gpu/drm/asahi/g16/engine.rs'
source = production.read_text()
output = output_dir()


def item(declaration):
    start = source.index(declaration)
    at = source.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#![allow(dead_code, unused_variables)]
use std::sync::{Arc, atomic::{AtomicU32,AtomicU64,AtomicBool,Ordering}};
use std::collections::VecDeque;
#[derive(Copy,Clone,Debug,PartialEq,Eq)] struct Error(i32);
const ETIMEDOUT:Error=Error(110);const ECANCELED:Error=Error(125);const EOVERFLOW:Error=Error(75);const EIO:Error=Error(5);const EAGAIN:Error=Error(11);const E2BIG:Error=Error(7);
type Result<T=()> = std::result::Result<T,Error>;
macro_rules! dev_err {($($argument:tt)*)=>{()};}
__PUBLICATION_ERROR__
#[derive(Copy,Clone)]enum Command { Compute(()), Render(()) }
struct Status(AtomicU32);impl Status{fn get(&self)->u32{self.0.load(Ordering::Relaxed)}}
struct Vm { status:Status, slot:u32 }
struct Queue { turn:AtomicU64, priority:u32 }
struct Progress { sent:AtomicU32,count:u32,pending:AtomicBool }
impl Progress {
    fn sent(&self)->u32{self.sent.load(Ordering::Relaxed)}
    fn count(&self)->u32{self.count}
    fn set_pending(&self,v:bool){self.pending.store(v,Ordering::Relaxed)}
}
struct Job { id:u32,vm:Vm,queue:Queue,progress:Progress,commands:Vec<Command>,dependency:Result,terminal_marked:AtomicBool }
impl Job {
    fn check_command_dependencies(&self,_index:u32)->Result{self.dependency}
    fn mark_terminal_retired(&self)->Result{assert!(!self.progress.pending.load(Ordering::Relaxed));self.terminal_marked.store(true,Ordering::Relaxed);Ok(())}
}
fn command(job:&Job,k:u32)->Option<&Command>{job.commands.get(k as usize)}
type Completions=Vec<(u32,Result)>;
fn complete(done:&mut Completions,job:&Arc<Job>,result:Result){done.push((job.id,result));}
#[derive(Default)]struct Stats{hold:[u64;3]}
struct Claims;
struct Flight{claims:Claims,group:Vec<Arc<Job>>}
impl Flight{fn new(_job:Arc<Job>)->Self{Self{claims:Claims,group:vec![]}}}
struct Engine{ordinal:u64,draw:u64,pending:Vec<Arc<Job>>,flights:Vec<Flight>,frozen:bool,turn:u64,stats:Stats,chan_wait:bool}
impl Engine {
    __PICK__
    __UNPEND__
}
struct Checks;impl Checks{fn check(&self)->Result{Ok(())}fn health(&self)->Result{Ok(())}}
struct Shared{sleeping:AtomicBool}
struct Session{shared:Shared}impl Session{fn check(&self)->Result{Ok(())}}
struct Recovery{error:Option<Error>,publication_wait_deadline:Option<u64>}impl Recovery{fn failed(&self)->Option<Error>{self.error}}
struct Slots{compute_exhausted:bool,render_exhausted:bool,releases:u32,quarantines:u32}
impl Slots{
    fn compute_exhausted(&self)->bool{self.compute_exhausted}
    fn render_exhausted(&self)->bool{self.render_exhausted}
    fn release(&mut self,_claims:&mut Claims){self.releases+=1;}
    fn quarantine(&mut self,_claims:&mut Claims){self.quarantines+=1;}
}
mod encode {
    pub struct ComputeRefs{pub invalid:bool}
    // Fault injection replaces the descriptor reader, while the actual production
    // containment branch below decides whether the unit becomes claimable again.
    pub fn preserve_compute_descriptor(refs:&ComputeRefs,_old:&[u8;160],_before:u64,_work:&mut[u8;8192])->Result<(),()>{if refs.invalid{Err(())}else{Ok(())}}
}
mod command {pub const RENDER:usize=0;pub const COMPUTE:usize=1;}
thread_local!{static CLOCK:std::cell::Cell<u64>=const{std::cell::Cell::new(0)};}
fn now_ns()->u64{CLOCK.with(|clock|clock.get())}
mod params{pub fn deadline_ms()->u32{100}pub fn compute_deadline_ms()->u32{100}}
mod recovery { #[path="__LOGIC__"]pub mod logic; }
mod boundary {
    use super::{Result,EIO};
    mod object {
        use super::{Result,EIO};use std::cell::Cell;
        pub struct FwObject{pub producer:Cell<u32>,pub refuse:bool}
        impl FwObject{pub fn w32(&self,offset:usize,head:u32)->Result{assert_eq!(offset,0x40);if self.refuse{return Err(EIO);}self.producer.set(head);Ok(())}}
    }
    mod engine {
        use super::Result;
        mod qstate{pub const WRITE:usize=0x40;}
        __PUBLISH_PRODUCER__
        #[cfg(test)]mod tests{
            use super::*;use super::super::object::FwObject;use std::cell::Cell;
            #[test]fn publication_counter_tracks_successful_producer_stores_only(){
                let queue=FwObject{producer:Cell::new(0),refuse:false};let mut count=0;
                publish_producer(&queue,17,&mut count).unwrap();assert_eq!((queue.producer.get(),count),(17,1));
                publish_producer(&queue,18,&mut count).unwrap();assert_eq!((queue.producer.get(),count),(18,2));
                let refused=FwObject{producer:Cell::new(5),refuse:true};assert!(publish_producer(&refused,6,&mut count).is_err());assert_eq!((refused.producer.get(),count),(5,2));
            }
            #[test]fn publication_counter_is_monotonic_at_saturation(){
                let queue=FwObject{producer:Cell::new(0),refuse:false};let mut count=u64::MAX;
                publish_producer(&queue,1,&mut count).unwrap();assert_eq!(count,u64::MAX);assert_eq!(queue.producer.get(),1);
            }
        }
    }
}
struct Stage{result:Result,visible:bool}
// Record the CLPC work boundary without running the power governor or transport.
#[derive(Default)]struct Power{work:Vec<bool>}
impl Power{fn report_work(&mut self,in_flight:bool){self.work.push(in_flight);}}
struct Firmware{
    session:Session,objects:Checks,recovery:Recovery,engine:Engine,slots:Slots,power:Power,
    unavailable:Vec<(usize,Error)>,stages:VecDeque<Stage>,retained:Vec<u32>,attempted:Vec<u32>,fail_stops:u32,
}
impl Firmware {
    fn mark_class_unavailable(&mut self,class:usize,error:Error){self.unavailable.push((class,error));}
    fn service_power(&self)->Result{Ok(())}
    fn drain_rings(&mut self)->Result{Ok(())}
    fn check_hangs(&mut self,_done:&mut Completions)->Result{Ok(())}
    fn retain(&mut self,job:&Arc<Job>){self.retained.push(job.id);}
    fn fail_stop(&mut self,error:Error,_done:&mut Completions){self.recovery.error=Some(error);self.fail_stops+=1;}
    fn restore_descriptor(&mut self,invalid:bool)->Result{
        let refs=encode::ComputeRefs{invalid};let descriptor=[0;160];let before=1;let mut work=[0;8192];let w=&mut work;
        let mut flight=Flight::new(job(99,false,0,Ok(())));let f=&mut flight;
        __DESCRIPTOR_CONTAINMENT__
        Ok(())
    }
    // The wire encoder/transport is replaced only at this boundary. The production wrapper,
    // dependency selection and error isolation execute verbatim above and below it.
    fn publish_prefix_into(&mut self,job:&Arc<Job>,_flight:&mut Flight,visible:&mut bool)->Result{
        self.attempted.push(job.id);
        let stage=self.stages.pop_front().expect("unexpected hardware publication");
        *visible=stage.visible;
        if stage.result.is_ok(){
            assert!(stage.visible);job.progress.sent.fetch_add(1,Ordering::Relaxed);
            self.engine.flights.push(Flight::new(job.clone()));
        }
        stage.result
    }
    __PUBLISH_PREFIX__
    __PUBLISH__
}
fn job(id:u32,render:bool,sent:u32,dependency:Result)->Arc<Job>{
    Arc::new(Job{id,vm:Vm{status:Status(AtomicU32::new(0)),slot:id},
        queue:Queue{turn:AtomicU64::new(0),priority:0},
        progress:Progress{sent:AtomicU32::new(sent),count:sent+1,pending:AtomicBool::new(true)},
        commands:vec![if render{Command::Render(())}else{Command::Compute(())};sent as usize+1],dependency,terminal_marked:AtomicBool::new(false)})
}
fn firmware(jobs:Vec<Arc<Job>>,stages:Vec<(Result,bool)>)->Firmware{
    Firmware{session:Session{shared:Shared{sleeping:AtomicBool::new(false)}},objects:Checks,recovery:Recovery{error:None,publication_wait_deadline:None},
        engine:Engine{ordinal:0,draw:0,pending:jobs,flights:vec![],frozen:false,turn:0,stats:Stats::default(),chan_wait:false},
        slots:Slots{compute_exhausted:false,render_exhausted:false,releases:0,quarantines:0},power:Power::default(),
        unavailable:vec![],stages:stages.into_iter().map(|(result,visible)|Stage{result,visible}).collect(),
        retained:vec![],attempted:vec![],fail_stops:0}
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn vm_failure_between_selection_and_preparation_is_job_local(){
        let mut f=firmware(vec![job(1,false,0,Ok(())),job(2,true,0,Ok(()))],vec![(Err(EIO),false),(Ok(()),true)]);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(done,[(1,Err(EIO))]);assert_eq!(f.attempted,[1,2]);assert_eq!(f.fail_stops,0);assert!(f.retained.is_empty());assert_eq!(f.slots.releases,1);
        assert_eq!(f.power.work,[true]);
    }
    #[test]fn ordinary_capacity_failure_after_retired_command_does_not_stop_other_jobs(){
        let mut f=firmware(vec![job(1,true,1,Ok(())),job(2,false,0,Ok(()))],vec![(Err(E2BIG),false),(Ok(()),true)]);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(done,[(1,Err(E2BIG))]);assert_eq!(f.fail_stops,0);assert_eq!(f.attempted,[1,2]);
    }
    #[test]fn every_errno_after_visibility_is_uncertain(){
        for error in [EIO,E2BIG,EAGAIN]{
            let first=job(1,false,0,Ok(()));let mut f=firmware(vec![first.clone(),job(2,true,0,Ok(()))],vec![(Err(error),true)]);
            let mut done=vec![];assert_eq!(f.publish(&mut done),Err(EIO));assert_eq!(done,[(1,Err(EIO))]);assert_eq!(f.fail_stops,1);assert_eq!(f.retained,[1]);assert_eq!(f.slots.releases,0);assert_eq!(f.attempted,[1]);
            assert!(!first.terminal_marked.load(Ordering::Relaxed));
        }
    }
    #[test]fn exhausted_compute_class_still_allows_render(){
        let mut f=firmware(vec![job(1,false,1,Ok(())),job(2,true,0,Ok(()))],vec![(Ok(()),true)]);f.slots.compute_exhausted=true;
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(done,[(1,Err(ECANCELED))]);assert_eq!(f.attempted,[2]);assert_eq!(f.fail_stops,0);
    }
    #[test]fn exhausted_render_class_still_allows_compute(){
        let mut f=firmware(vec![job(1,true,0,Ok(())),job(2,false,0,Ok(()))],vec![(Ok(()),true)]);f.slots.render_exhausted=true;
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(done,[(1,Err(ECANCELED))]);assert_eq!(f.attempted,[2]);assert_eq!(f.fail_stops,0);
    }
    #[test]fn logical_u64_overflow_is_explicit_and_class_local(){
        let mut f=firmware(vec![job(1,false,0,Ok(())),job(2,true,0,Ok(()))],vec![(Ok(()),true)]);
        f.engine.ordinal=u64::MAX;
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(done,[(1,Err(ECANCELED))]);assert_eq!(f.unavailable,[(command::COMPUTE,EOVERFLOW)]);
        assert_eq!(f.attempted,[2]);assert_eq!(f.fail_stops,0);
    }
    #[test]fn failed_vm_fence_waits_for_its_prior_live_command(){
        let waiting=job(1,false,1,Ok(()));waiting.vm.status.0.store(5,Ordering::Relaxed);
        let mut f=firmware(vec![waiting.clone()],vec![]);f.engine.flights.push(Flight::new(waiting.clone()));
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));
        assert!(done.is_empty());assert!(f.attempted.is_empty());assert!(!waiting.terminal_marked.load(Ordering::Relaxed));
        f.engine.flights.clear();assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(done,[(1,Err(EIO))]);assert!(waiting.terminal_marked.load(Ordering::Relaxed));
        assert_eq!(f.power.work,[true,false]);
    }
    #[test]fn pending_exact_predecessor_does_not_block_ready_job(){
        let mut f=firmware(vec![job(1,false,0,Err(EAGAIN)),job(2,true,0,Ok(()))],vec![(Ok(()),true)]);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert!(done.is_empty());assert_eq!(f.attempted,[2]);assert_eq!(f.engine.pending.len(),1);
    }
    #[test]fn failed_input_errno_is_job_local_before_any_claim(){
        let mut f=firmware(vec![job(1,false,0,Err(EIO)),job(2,true,0,Ok(()))],vec![(Ok(()),true)]);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(done,[(1,Err(EIO))]);assert_eq!(f.attempted,[2]);assert_eq!(f.slots.releases,0);assert_eq!(f.fail_stops,0);
    }
    #[test]fn no_second_publication_precedes_a_wire_receipt(){
        let mut f=firmware(vec![job(1,false,0,Ok(())),job(2,true,0,Ok(()))],vec![(Ok(()),true)]);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(f.attempted,[1]);assert_eq!(f.engine.pending.len(),1);assert_eq!(f.engine.stats.hold[0],1);
    }
    #[test]fn corrupt_retained_descriptor_is_quarantined_before_unpublished_release(){
        let mut f=firmware(vec![],vec![]);assert_eq!(f.restore_descriptor(false),Ok(()));assert_eq!(f.slots.quarantines,0);
        assert_eq!(f.restore_descriptor(true),Err(EIO));assert_eq!(f.slots.quarantines,1);assert_eq!(f.slots.releases,0);
    }
    #[test]fn idle_deferred_publication_rearms_and_obeys_absolute_deadline(){
        CLOCK.with(|clock|clock.set(100));
        let mut f=firmware(vec![job(1,false,0,Ok(()))],vec![(Err(EAGAIN),false),(Err(EAGAIN),false)]);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));
        assert!(f.engine.chan_wait && f.engine.flights.is_empty());
        assert_eq!(f.power.work,[false]);
        let deadline=f.recovery.publication_wait_deadline.unwrap();
        CLOCK.with(|clock|clock.set(deadline+1));
        assert_eq!(f.publish(&mut done),Err(ETIMEDOUT));
        assert_eq!(f.recovery.publication_wait_deadline,Some(deadline));
    }
    #[test]fn nothing_is_published_during_a_system_sleep(){
        let mut f=firmware(vec![job(1,false,0,Ok(()))],vec![(Ok(()),true)]);f.session.shared.sleeping.store(true,Ordering::Relaxed);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));
        assert!(done.is_empty());assert!(f.attempted.is_empty());assert_eq!(f.engine.pending.len(),1);assert!(f.power.work.is_empty());
        f.session.shared.sleeping.store(false,Ordering::Relaxed);assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(f.attempted,[1]);
    }

}
'''
harness = harness.replace('__LOGIC__', str(production.parent / 'recovery/logic.rs'))
for token, declaration in {
    '__PUBLICATION_ERROR__': 'enum PublicationError {',
    '__PICK__': 'fn pick(&self)',
    '__UNPEND__': 'fn unpend(&mut self',
    '__PUBLISH_PREFIX__': 'fn publish_prefix(',
    '__PUBLISH__': 'pub(crate) fn publish(&mut self',
    '__DESCRIPTOR_CONTAINMENT__': 'if encode::preserve_compute_descriptor(',
    '__PUBLISH_PRODUCER__': 'fn publish_producer(',
}.items():
    harness = harness.replace(token, item(declaration).replace("super::command::", "crate::command::").replace("super::recovery::", "crate::recovery::"))
unit = output / 'publication.rs'
unit.write_text(harness)
log = ''
for command in [rustc() + ['--edition=2021', '--test', str(unit), '-o', str(output/'publication')],
                [str(output/'publication'), '--nocapture']]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (output/'publication.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)
(output/'publication.json').write_text(json.dumps({
    'passed': True, 'tests': 21, 'cpu_only': True, 'hardware_execution': False,
    'scope': 'Verbatim production publication wrapper, selection, error isolation and descriptor-error containment branch; wire encoder, descriptor failure injection and transport replaced by CPU recorders.',
    'source_sha256': sha(production),
}, indent=2)+'\n')
