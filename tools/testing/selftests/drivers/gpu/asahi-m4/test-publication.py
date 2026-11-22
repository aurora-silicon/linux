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
#[derive(Copy,Clone,Debug,PartialEq,Eq)]enum Channel { Ta = 0, Fs = 1, Compute = 2 }
#[derive(Copy,Clone)]struct ComputeCommand { query_access: bool }
#[derive(Copy,Clone)]enum Command { Compute(ComputeCommand), Render(()) }
struct Status(AtomicU32);impl Status{fn get(&self)->u32{self.0.load(Ordering::Relaxed)}}
struct Vm { status:Status, slot:u32 }
struct Queue { turn:AtomicU64, priority:u32, completed:[AtomicU64;2] }
struct Progress { sent:AtomicU32,count:u32,pending:AtomicBool }
impl Progress {
    fn sent(&self)->u32{self.sent.load(Ordering::Relaxed)}
    fn count(&self)->u32{self.count}
    fn set_pending(&self,v:bool){self.pending.store(v,Ordering::Relaxed)}
}
struct Resources;impl Resources{fn compute_units(&self)->Result<usize>{Ok(2)}fn render_passes(&self)->Result<usize>{Ok(8)}}
struct Job { id:u32,vm:Arc<Vm>,queue:Queue,progress:Progress,commands:Vec<Command>,dependency:Result,terminal_marked:AtomicBool,resources:Resources }
impl Job {
    fn check_command_dependencies(&self,_index:u32)->Result{self.dependency}
    fn mark_terminal_retired(&self)->Result{assert!(!self.progress.pending.load(Ordering::Relaxed));self.terminal_marked.store(true,Ordering::Relaxed);Ok(())}
}
fn command(job:&Job,k:u32)->Option<&Command>{job.commands.get(k as usize)}
type Completions=Vec<(u32,Result)>;
fn complete(done:&mut Completions,job:&Arc<Job>,result:Result){done.push((job.id,result));}
#[derive(Default)]struct Stats{hold:[u64;3],grouped:u64}
struct Claims;
struct Flight{job:Arc<Job>,claims:Claims,group:Vec<Arc<Job>>,doomed:bool,recovery_suspect:bool,initbm:bool}
impl Flight{fn new(job:Arc<Job>)->Self{Self{job,claims:Claims,group:vec![],doomed:false,recovery_suspect:false,initbm:false}}}
struct Engine{ordinal:u64,draw:u64,pending:Vec<Arc<Job>>,flights:Vec<Flight>,frozen:bool,turn:u64,stats:Stats,chan_wait:bool}
impl Engine {
    __PICK__
    __UNPEND__
}
struct Checks;impl Checks{fn check(&self)->Result{Ok(())}fn health(&self)->Result{Ok(())}}
struct Shared{sleeping:AtomicBool}
struct Session{shared:Shared,rings:Vec<Channel>,ring_fail:bool,checks:AtomicU32,transports:AtomicU32}
impl Session{
    fn check(&self)->Result{self.checks.fetch_add(1,Ordering::Relaxed);Ok(())}
    fn transport(&self)->Result{self.transports.fetch_add(1,Ordering::Relaxed);Ok(())}
    fn ring(&mut self,channel:Channel)->Result{
        if self.ring_fail{return Err(EIO);}
        self.rings.push(channel);
        Ok(())
    }
}
struct Recovery{error:Option<Error>,publication_wait_deadline:Option<u64>}impl Recovery{fn failed(&self)->Option<Error>{self.error}}
struct Slots{compute_exhausted:bool,render_exhausted:bool,releases:u32,quarantines:u32}
impl Slots{
    fn compute_exhausted(&self)->bool{self.compute_exhausted}
    fn render_exhausted(&self)->bool{self.render_exhausted}
    fn compute_exhausted_within(&self,_units:usize)->bool{self.compute_exhausted}
    fn render_exhausted_within(&self,_passes:usize)->bool{self.render_exhausted}
    fn release(&mut self,_claims:&mut Claims){self.releases+=1;}
    fn quarantine(&mut self,_claims:&mut Claims){self.quarantines+=1;}
}
struct Pools{exhausted:bool,exhausted_vm:Option<u32>,block_pipeline_vm:Option<u32>}
impl Pools{
    fn exhausted(&self)->bool{self.exhausted}
    fn exhausted_for(&self,vm:&Vm)->bool{self.exhausted_vm==Some(vm.slot)}
    fn can_pipeline(&self,vm:&Arc<Vm>)->bool{self.block_pipeline_vm!=Some(vm.slot)}
}
mod encode {
    pub struct ComputeRefs{pub invalid:bool}
    // Fault injection replaces the descriptor reader, while the actual production
    // containment branch below decides whether the unit becomes claimable again.
    pub fn preserve_compute_descriptor(refs:&ComputeRefs,_old:&[u8;160],_before:u64,_work:&mut[u8;8192])->Result<(),()>{if refs.invalid{Err(())}else{Ok(())}}
}
mod hw { pub mod sizes { pub const FLIGHTS:usize=32; } }
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
    session:Session,objects:Checks,recovery:Recovery,engine:Engine,slots:Slots,pools:Pools,power:Power,
    unavailable:Vec<(usize,Error)>,stages:VecDeque<Stage>,retained:Vec<u32>,attempted:Vec<u32>,fail_stops:u32,
    allow_behind:bool,block_render_behind:bool,block_compute_behind:bool,
}
impl Firmware {
    fn mark_class_unavailable(&mut self,class:usize,error:Error){self.unavailable.push((class,error));}
    __NEEDS_ENGINE_DRAIN__
    fn can_queue_behind(&self,job:&Arc<Job>)->bool{
        if !self.allow_behind{return false;}
        if self.needs_engine_drain(job){return false;}
        if self.block_render_behind&&matches!(command(job,job.progress.sent()),Some(Command::Render(()))){return false;}
        if self.block_compute_behind&&matches!(command(job,job.progress.sent()),Some(Command::Compute(_))){return false;}
        true
    }
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
    fn publish_prefix_into(&mut self,job:&Arc<Job>,_flight:&mut Flight,visible:&mut bool,pending_rings:&mut u8)->Result{
        self.attempted.push(job.id);
        let stage=self.stages.pop_front().expect("unexpected hardware publication");
        *visible=stage.visible;
        if stage.result.is_ok(){
            assert!(stage.visible);
            let idx=job.progress.sent.fetch_add(1,Ordering::Relaxed);
            let mut fl=Flight::new(job.clone());
            match command(job,idx){
                Some(Command::Render(()))=>{
                    *pending_rings|=(1<<(Channel::Fs as u8))|(1<<(Channel::Ta as u8));
                    fl.initbm=!self.pools.can_pipeline(&job.vm);
                }
                _=>{*pending_rings|=1<<(Channel::Compute as u8);}
            }
            self.engine.flights.push(fl);
        }
        stage.result
    }
    __FLUSH_DOORBELLS__
    __PUBLISH_PREFIX__
    __PUBLISH__
    __PUBLISH_CHECKED__
}
fn mk_vm(slot:u32)->Arc<Vm>{Arc::new(Vm{status:Status(AtomicU32::new(0)),slot})}
fn job_on_vm(id:u32,vm:Arc<Vm>,render:bool,sent:u32,dependency:Result)->Arc<Job>{
    Arc::new(Job{id,vm,
        queue:Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]},
        progress:Progress{sent:AtomicU32::new(sent),count:sent+1,pending:AtomicBool::new(true)},
        commands:vec![if render{Command::Render(())}else{Command::Compute(ComputeCommand{query_access:true})};sent as usize+1],dependency,terminal_marked:AtomicBool::new(false),resources:Resources})
}
fn job(id:u32,render:bool,sent:u32,dependency:Result)->Arc<Job>{
    job_on_vm(id,mk_vm(id),render,sent,dependency)
}
fn firmware(jobs:Vec<Arc<Job>>,stages:Vec<(Result,bool)>)->Firmware{
    Firmware{session:Session{shared:Shared{sleeping:AtomicBool::new(false)},rings:vec![],ring_fail:false,checks:AtomicU32::new(0),transports:AtomicU32::new(0)},objects:Checks,recovery:Recovery{error:None,publication_wait_deadline:None},
        engine:Engine{ordinal:0,draw:0,pending:jobs,flights:vec![],frozen:false,turn:0,stats:Stats::default(),chan_wait:false},
        slots:Slots{compute_exhausted:false,render_exhausted:false,releases:0,quarantines:0},pools:Pools{exhausted:false,exhausted_vm:None,block_pipeline_vm:None},power:Power::default(),
        unavailable:vec![],stages:stages.into_iter().map(|(result,visible)|Stage{result,visible}).collect(),
        retained:vec![],attempted:vec![],fail_stops:0,allow_behind:false,block_render_behind:false,block_compute_behind:false}
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
    #[test]fn exhausted_render_pools_cancels_render_and_allows_compute(){
        let mut f=firmware(vec![job(1,true,0,Ok(())),job(2,false,0,Ok(()))],vec![(Ok(()),true)]);f.pools.exhausted=true;
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));assert_eq!(done,[(1,Err(ECANCELED))]);
        assert_eq!(f.unavailable,[(command::RENDER,EIO)]);assert_eq!(f.attempted,[2]);assert_eq!(f.fail_stops,0);
    }
    #[test]fn per_vm_exhausted_render_pools_cancels_only_blocked_vm_without_stalling_compute(){
        let mut f=firmware(vec![job(1,true,0,Ok(())),job(2,false,0,Ok(()))],vec![(Ok(()),true)]);
        f.pools.exhausted_vm=Some(1);
        let mut done=vec![];assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(done,[(1,Err(ECANCELED))]);
        assert!(f.unavailable.is_empty());
        assert!(!f.engine.chan_wait);
        assert_eq!(f.recovery.publication_wait_deadline,None);
        assert_eq!(f.attempted,[2]);assert_eq!(f.fail_stops,0);
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
    #[test]fn batched_compute_publications_coalesce_to_one_doorbell(){
        let vm1=mk_vm(1);
        let mut f=firmware(
            vec![job_on_vm(1,vm1.clone(),false,0,Ok(())),job_on_vm(2,vm1.clone(),false,0,Ok(())),job_on_vm(3,vm1,false,0,Ok(()))],
            vec![(Ok(()),true),(Ok(()),true),(Ok(()),true)],
        );
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(f.attempted,[1,2,3]);
        assert_eq!(f.session.rings,[Channel::Compute]);
        assert_eq!(f.engine.stats.grouped,2);
    }
    #[test]fn batched_render_publications_coalesce_in_fs_then_ta_order(){
        let vm1=mk_vm(1);
        let mut f=firmware(
            vec![job_on_vm(1,vm1.clone(),true,0,Ok(())),job_on_vm(2,vm1,true,0,Ok(()))],
            vec![(Ok(()),true),(Ok(()),true)],
        );
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(f.attempted,[1,2]);
        assert_eq!(f.session.rings,[Channel::Fs,Channel::Ta]);
        assert_eq!(f.engine.stats.grouped,1);
    }
    #[test]fn batched_doorbell_flushes_after_later_deferral_and_fail_stops_on_ring_error(){
        let vm1=mk_vm(1);
        let mut f=firmware(
            vec![job_on_vm(1,vm1.clone(),false,0,Ok(())),job_on_vm(2,vm1,false,0,Ok(()))],
            vec![(Ok(()),true),(Err(EAGAIN),false)],
        );
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(f.attempted,[1,2]);
        assert_eq!(f.session.rings,[Channel::Compute]);
        assert_eq!(f.fail_stops,0);

        let mut f=firmware(vec![job(1,false,0,Ok(()))],vec![(Ok(()),true)]);
        f.session.ring_fail=true;
        let mut done=vec![];
        assert_eq!(f.publish(&mut done),Err(EIO));
        assert_eq!(f.fail_stops,1);
    }
    #[test]fn alt_class_fallback_prefers_higher_priority_smaller_value(){
        let vm1=mk_vm(1);
        let mk_prio=|id:u32,render:bool,priority:u32|Arc::new(Job{
            id,vm:vm1.clone(),
            queue:Queue{turn:AtomicU64::new(0),priority,completed:[AtomicU64::new(0),AtomicU64::new(0)]},
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(())}else{Command::Compute(ComputeCommand{query_access:false})}],
            dependency:Ok(()),terminal_marked:AtomicBool::new(false),resources:Resources,
        });
        // Head render (prio 2) is blocked behind active render flight; alt-class fallback
        // must select prio 2 (MEDIUM) compute ahead of prio 3 (LOW) compute.
        let r_head=mk_prio(1,true,2);
        let c_low=mk_prio(2,false,3);
        let c_med=mk_prio(3,false,2);
        let mut f=firmware(vec![r_head.clone(),c_low,c_med],vec![(Ok(()),true),(Ok(()),true)]);
        f.engine.flights.push(Flight::new(r_head));
        f.allow_behind=true;
        f.block_render_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[3,2]);
        assert_eq!(f.engine.pending.len(),1);
    }
    #[test]fn alt_bypass_does_not_starve_same_vm_initbm_render(){
        let vm1=mk_vm(1);
        let c_active=job_on_vm(1,vm1.clone(),false,0,Ok(()));
        let r_initbm=job_on_vm(2,vm1.clone(),true,0,Ok(()));
        let c_pending=job_on_vm(3,vm1.clone(),false,0,Ok(()));
        c_pending.queue.turn.store(5,Ordering::Relaxed);
        let mut f=firmware(vec![r_initbm,c_pending],vec![(Ok(()),true),(Ok(()),true)]);
        f.engine.flights.push(Flight::new(c_active));
        f.allow_behind=true;
        f.pools.block_pipeline_vm=Some(vm1.slot);
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert!(f.attempted.is_empty());
        assert_eq!(f.engine.stats.hold[0],1);
        assert_eq!(f.engine.pending.len(),2);
        f.engine.flights.clear();
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[2]);
        assert_eq!(f.engine.pending.len(),1);
        f.engine.flights.clear();
        f.pools.block_pipeline_vm=None;
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[2,3]);
    }
    #[test]fn alt_bypass_does_not_starve_cross_vm(){
        let vm1=mk_vm(1);
        let vm2=mk_vm(2);
        let vm1_active=job_on_vm(1,vm1.clone(),false,0,Ok(()));
        let vm2_head=job_on_vm(2,vm2,false,0,Ok(()));
        let vm1_pending=job_on_vm(3,vm1,false,0,Ok(()));
        vm1_pending.queue.turn.store(5,Ordering::Relaxed);
        let mut f=firmware(vec![vm2_head,vm1_pending],vec![(Ok(()),true)]);
        f.engine.flights.push(Flight::new(vm1_active));
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert!(f.attempted.is_empty());
        assert_eq!(f.engine.stats.hold[0],1);
        assert_eq!(f.engine.pending.len(),2);
        f.engine.flights.clear();
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[2]);
    }
    #[test]fn alt_bypass_still_allows_same_vm_class_cap_overlap(){
        let vm1=mk_vm(1);
        let c_active=job_on_vm(1,vm1.clone(),false,0,Ok(()));
        let c_head=job_on_vm(2,vm1.clone(),false,0,Ok(()));
        let r_pipelined=job_on_vm(3,vm1,true,0,Ok(()));
        r_pipelined.queue.turn.store(5,Ordering::Relaxed);
        let mut f=firmware(vec![c_head,r_pipelined],vec![(Ok(()),true)]);
        f.engine.flights.push(Flight::new(c_active));
        f.allow_behind=true;
        f.block_compute_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[3]);
        assert_eq!(f.engine.pending.len(),1);
    }
    #[test]fn deferred_eagain_does_not_burn_queue_turn(){
        let j1=job(1,false,0,Ok(()));
        let j2=job(2,false,0,Ok(()));
        j1.queue.turn.store(1,Ordering::Relaxed);
        j2.queue.turn.store(2,Ordering::Relaxed);
        let mut f=firmware(vec![j1.clone(),j2.clone()],vec![(Err(EAGAIN),false),(Ok(()),true)]);
        f.engine.turn=10;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[1]);
        assert_eq!(j1.queue.turn.load(Ordering::Relaxed),1);
        assert_eq!(f.engine.turn,10);
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[1,1]);
        assert_eq!(j1.queue.turn.load(Ordering::Relaxed),11);
        assert_eq!(f.engine.turn,11);
    }

}
#[derive(Copy,Clone)]struct Order{sequence:u64,wait_through:[Option<u64>;2]}
struct QueueBehindJob{id:u32,vm:Arc<Vm>,queue:Arc<Queue>,progress:Progress,commands:Vec<Command>,order:Order,extra_orders:Vec<Order>,dependency:Result,resources:Resources}
impl QueueBehindJob{
    fn check_command_dependencies(&self,_index:u32)->Result{self.dependency}
    fn command_order(&self,index:u32)->Option<Order>{
        if self.extra_orders.is_empty(){Some(self.order)}else{self.extra_orders.get(index as usize).copied()}
    }
}
struct QueueBehindFlight{job:Arc<QueueBehindJob>,first:usize,count:usize,computes:u32,renders:u32,target:u32,cstamp:[u32;16],fs_target:u32,initbm:bool,has_timestamps:bool,doomed:bool,recovery_suspect:bool}
struct QueueBehindEngine{ordinal:u64,draw:u64,flights:Vec<QueueBehindFlight>}
struct QueueBehindSlots{compute_exhausted:bool,render_exhausted:bool,compute_available:bool,pass_available:bool}
impl QueueBehindSlots{
    fn compute_exhausted(&self)->bool{self.compute_exhausted}
    fn render_exhausted(&self)->bool{self.render_exhausted}
    fn compute_available(&self,_n:usize)->bool{self.compute_available}
    fn pass_available(&self,_n:usize)->bool{self.pass_available}
}
struct QueueBehindPools{exhausted:bool,can_pipeline:bool}
impl QueueBehindPools{
    fn exhausted(&self)->bool{self.exhausted}
    fn can_pipeline(&self,_vm:&Arc<Vm>)->bool{self.can_pipeline}
}
struct QueueBehindHost{engine:QueueBehindEngine,slots:QueueBehindSlots,pools:QueueBehindPools}
mod queue_behind_scope {
    use super::*;
    type Job = QueueBehindJob;
    fn command(job:&Job,k:u32)->Option<&Command>{job.commands.get(k as usize)}
    mod params { pub fn compute_flights()->usize{16} pub fn render_flights()->usize{15} }
    impl QueueBehindHost {
        __CAN_QUEUE_BEHIND__
        __CROSS_ENGINE_WAIT__
    }
    #[test]fn timed_compute_queues_behind_same_vm_and_coexists_with_render(){
        let vm1=Arc::new(Vm{status:Status(AtomicU32::new(0)),slot:1});
        let vm2=Arc::new(Vm{status:Status(AtomicU32::new(0)),slot:2});
        let q1=Arc::new(Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]});
        let mk=|id:u32,vm:Arc<Vm>,queue:Arc<Queue>,render:bool|Arc::new(QueueBehindJob{
            id,vm,queue,progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(())}else{Command::Compute(ComputeCommand{query_access:true})}],
            order:Order{sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
        });
        let j1=mk(1,vm1.clone(),q1.clone(),false);
        let j2=mk(2,vm1.clone(),q1.clone(),false);
        let j_other_vm=mk(3,vm2.clone(),q1.clone(),false);
        let j_render=mk(4,vm1.clone(),q1.clone(),true);
        let mut p=QueueBehindHost{
            engine:QueueBehindEngine{ordinal:1,draw:1,flights:vec![QueueBehindFlight{job:j1,first:0,count:1,computes:1,renders:0,target:0x100,cstamp:{let mut a=[0;16];a[0]=0x100;a},fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true},
        };
        assert!(p.can_queue_behind(&j2));
        assert!(!p.can_queue_behind(&j_other_vm));
        assert!(p.can_queue_behind(&j_render));
        p.engine.flights[0].doomed=true;
        assert!(!p.can_queue_behind(&j2));
        p.engine.flights[0].doomed=false;
        p.engine.flights[0].renders=1;
        p.engine.flights[0].computes=0;
        assert!(p.can_queue_behind(&j2));
        p.engine.flights[0].initbm=true;
        assert!(!p.can_queue_behind(&j2));
        p.engine.flights[0].initbm=false;
        p.engine.flights[0].renders=0;
        p.engine.flights[0].computes=1;
        p.slots.compute_available=false;
        assert!(!p.can_queue_behind(&j2));
    }
    #[test]fn compute_query_access_waits_for_timed_predecessor_unless_no_query_access(){
        let vm1=Arc::new(Vm{status:Status(AtomicU32::new(0)),slot:1});
        let q1=Arc::new(Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]});
        let q2=Arc::new(Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]});
        let mk=|id:u32,queue:Arc<Queue>,query_access:bool,wait_vdm:Option<u64>,wait_cdm:Option<u64>|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue,
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access})],
            order:Order{sequence:u64::from(id),wait_through:[wait_vdm,wait_cdm]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
        });
        let timed=mk(1,q1.clone(),true,None,None);
        let query_copy=mk(2,q1.clone(),true,None,Some(1));
        let no_query=mk(2,q1.clone(),false,None,Some(1));
        let no_barrier=mk(2,q1.clone(),true,None,None);
        let other_queue=mk(2,q2.clone(),true,None,Some(1));
        let p=QueueBehindHost{
            engine:QueueBehindEngine{ordinal:1,draw:1,flights:vec![QueueBehindFlight{job:timed,first:0,count:1,computes:1,renders:0,target:0x100,cstamp:{let mut a=[0;16];a[0]=0x100;a},fs_target:0,initbm:false,has_timestamps:true,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true},
        };
        assert!(!p.can_queue_behind(&query_copy));
        assert!(p.can_queue_behind(&no_query));
        assert!(p.can_queue_behind(&no_barrier));
        assert!(p.can_queue_behind(&other_queue));
        // Cross-engine render timestamp predecessor also blocks query_access compute.
        let timed_render=Arc::new(QueueBehindJob{
            id:1,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(())],
            order:Order{sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
        });
        let pr=QueueBehindHost{
            engine:QueueBehindEngine{ordinal:1,draw:1,flights:vec![QueueBehindFlight{job:timed_render,first:0,count:1,computes:0,renders:1,target:0,cstamp:[0;16],fs_target:0x200,initbm:false,has_timestamps:true,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true},
        };
        assert!(!pr.can_queue_behind(&mk(2,q1.clone(),true,Some(1),None)));
        assert!(pr.can_queue_behind(&mk(2,q1.clone(),false,Some(1),None)));
        assert!(pr.can_queue_behind(&mk(2,q1.clone(),true,None,None)));
    }
    #[test]fn render_queues_behind_same_vm_and_coexists_with_compute_while_blocking_initbm(){
        let vm1=Arc::new(Vm{status:Status(AtomicU32::new(0)),slot:1});
        let vm2=Arc::new(Vm{status:Status(AtomicU32::new(0)),slot:2});
        let q1=Arc::new(Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]});
        let mk=|id:u32,vm:Arc<Vm>,render:bool|Arc::new(QueueBehindJob{
            id,vm,queue:q1.clone(),progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(())}else{Command::Compute(ComputeCommand{query_access:true})}],
            order:Order{sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
        });
        let r1=mk(1,vm1.clone(),true);
        let r2=mk(2,vm1.clone(),true);
        let r_other_vm=mk(3,vm2.clone(),true);
        let c1=mk(4,vm1.clone(),false);
        let mut p=QueueBehindHost{
            engine:QueueBehindEngine{ordinal:1,draw:1,flights:vec![QueueBehindFlight{job:r1,first:0,count:1,computes:0,renders:1,target:0,cstamp:[0;16],fs_target:0x200,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true},
        };
        assert!(p.can_queue_behind(&r2));
        assert!(!p.can_queue_behind(&r_other_vm));
        assert!(p.can_queue_behind(&c1));
        p.engine.flights[0].initbm=true;
        assert!(!p.can_queue_behind(&r2));
        assert!(!p.can_queue_behind(&c1));
        p.engine.flights[0].initbm=false;
        p.pools.can_pipeline=false;
        assert!(!p.can_queue_behind(&r2));
        assert!(p.can_queue_behind(&c1));
        p.pools.can_pipeline=true;
        p.slots.pass_available=false;
        assert!(!p.can_queue_behind(&r2));
        p.slots.pass_available=true;
        p.engine.flights[0].recovery_suspect=true;
        assert!(!p.can_queue_behind(&r2));
        assert!(!p.can_queue_behind(&c1));
    }
    #[test]fn cross_engine_wait_resolves_barriers_and_skips_no_barrier_or_retired(){
        let vm1=Arc::new(Vm{status:Status(AtomicU32::new(0)),slot:1});
        let q1=Arc::new(Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]});
        let q2=Arc::new(Queue{turn:AtomicU64::new(0),priority:0,completed:[AtomicU64::new(0),AtomicU64::new(0)]});
        let mk=|id:u32,queue:Arc<Queue>,render:bool,seq:u64,wait:[Option<u64>;2]|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue,
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(())}else{Command::Compute(ComputeCommand{query_access:false})}],
            order:Order{sequence:seq,wait_through:wait},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
        });
        let r1=mk(1,q1.clone(),true,1,[None,None]);
        let c1=mk(2,q1.clone(),false,1,[Some(1),None]);
        let c_nobarrier=mk(3,q1.clone(),false,2,[None,None]);
        let r2_after_c1=mk(4,q1.clone(),true,2,[Some(1),Some(1)]);
        let c_other_q=mk(5,q2.clone(),false,1,[None,None]);
        let p=QueueBehindHost{
            engine:QueueBehindEngine{ordinal:1,draw:1,flights:vec![
                QueueBehindFlight{job:r1,first:0,count:1,computes:0,renders:1,target:0,cstamp:[0;16],fs_target:0x500,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false},
                QueueBehindFlight{job:c1.clone(),first:0,count:1,computes:1,renders:0,target:0x700,cstamp:{let mut a=[0;16];a[0]=0x700;a},fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false},
            ]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true},
        };
        // Same-queue FS -> CDM barrier resolves render's fs_target; CDM -> TA resolves compute's target.
        assert_eq!(p.cross_engine_wait(&c1,0,command::RENDER),Ok(Some(0x500)));
        assert_eq!(p.cross_engine_wait(&r2_after_c1,0,command::COMPUTE),Ok(Some(0x700)));
        // No-barrier / independent-queue submits emit no cross-engine barrier.
        assert_eq!(p.cross_engine_wait(&c_nobarrier,0,command::RENDER),Ok(None));
        assert_eq!(p.cross_engine_wait(&c_other_q,0,command::RENDER),Ok(None));
        // Already-retired cross-engine prefix emits no barrier.
        q1.completed[command::RENDER].store(1,Ordering::Release);
        assert_eq!(p.cross_engine_wait(&c1,0,command::RENDER),Ok(None));
        // Multi-command same-job compute flight resolves non-first member's exact cstamp
        // and defers with EAGAIN if an unretired predecessor is missing.
        let multi_c=Arc::new(QueueBehindJob{
            id:6,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:3,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false});3],
            order:Order{sequence:1,wait_through:[None,None]},
            extra_orders:vec![
                Order{sequence:1,wait_through:[None,None]},
                Order{sequence:2,wait_through:[None,None]},
                Order{sequence:3,wait_through:[None,None]},
            ],
            dependency:Ok(()),resources:Resources,
        });
        let r_wait_c2=mk(7,q1.clone(),true,3,[None,Some(2)]);
        let r_wait_c3=mk(8,q1.clone(),true,4,[None,Some(3)]);
        let r_wait_missing=mk(9,q1.clone(),true,5,[None,Some(99)]);
        let mut cstamp=[0u32;16];
        cstamp[0]=0x100;cstamp[1]=0x200;cstamp[2]=0x300;
        let pm=QueueBehindHost{
            engine:QueueBehindEngine{ordinal:3,draw:1,flights:vec![
                QueueBehindFlight{job:multi_c,first:0,count:3,computes:3,renders:0,target:0x300,cstamp,fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false},
            ]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true},
        };
        assert_eq!(pm.cross_engine_wait(&r_wait_c2,0,command::COMPUTE),Ok(Some(0x200)));
        assert_eq!(pm.cross_engine_wait(&r_wait_c3,0,command::COMPUTE),Ok(Some(0x300)));
        assert_eq!(pm.cross_engine_wait(&r_wait_missing,0,command::COMPUTE),Err(EAGAIN));
    }
    #[test]fn publish_checked_uses_transport_without_repeating_session_check(){
        let mut fw=firmware(vec![job(1,false,0,Ok(()))],vec![(Ok(()),true)]);
        let mut done=vec![];
        fw.publish_checked(&mut done).unwrap();
        assert_eq!(fw.session.checks.load(Ordering::Relaxed),0);
        assert_eq!(fw.session.transports.load(Ordering::Relaxed),1);
        let mut fw2=firmware(vec![job(2,false,0,Ok(()))],vec![(Ok(()),true)]);
        fw2.publish(&mut done).unwrap();
        assert_eq!(fw2.session.checks.load(Ordering::Relaxed),1);
        assert_eq!(fw2.session.transports.load(Ordering::Relaxed),1);
    }
}
'''
harness = harness.replace('__LOGIC__', str(production.parent / 'recovery/logic.rs'))
for token, declaration in {
    '__PUBLICATION_ERROR__': 'enum PublicationError {',
    '__PICK__': 'fn pick(&self)',
    '__UNPEND__': 'fn unpend(&mut self',
    '__FLUSH_DOORBELLS__': 'fn flush_doorbells(',
    '__PUBLISH_PREFIX__': 'fn publish_prefix(',
    '__PUBLISH__': 'pub(crate) fn publish(&mut self',
    '__PUBLISH_CHECKED__': 'pub(crate) fn publish_checked(&mut self',
    '__NEEDS_ENGINE_DRAIN__': 'fn needs_engine_drain(',
    '__DESCRIPTOR_CONTAINMENT__': 'if encode::preserve_compute_descriptor(',
    '__PUBLISH_PRODUCER__': 'fn publish_producer(',
    '__CAN_QUEUE_BEHIND__': 'fn can_queue_behind(',
    '__CROSS_ENGINE_WAIT__': 'fn cross_engine_wait(',
}.items():
    harness = harness.replace(token, item(declaration).replace("super::command::", "crate::command::").replace("super::recovery::", "crate::recovery::").replace("super::hw::", "crate::hw::"))
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
    'passed': True, 'tests': 24, 'cpu_only': True, 'hardware_execution': False,
    'scope': 'Verbatim production publication wrapper, selection, error isolation and descriptor-error containment branch; wire encoder, descriptor failure injection and transport replaced by CPU recorders.',
    'source_sha256': sha(production),
}, indent=2)+'\n')
