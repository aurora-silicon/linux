#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute production M4 publication/error isolation with a CPU-only transport recorder."""
from common import kernel_root, output_dir, rustc, sha
import json
import re
import subprocess

production = kernel_root() / 'drivers/gpu/drm/asahi/g16/engine.rs'
source = production.read_text()
params_src = (production.parent / 'params.rs').read_text()
queue_src = (production.parent / 'queue.rs').read_text()
job_src = (production.parent / 'job.rs').read_text()
mod_src = (production.parent / 'mod.rs').read_text()
hw_src = (production.parent / 'hw.rs').read_text()
gpu_src = (production.parent.parent / 'gpu.rs').read_text()
output = output_dir()


def item_in(src, declaration):
    start = src.index(declaration)
    at = src.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]


def item(declaration):
    return item_in(source, declaration)


# Verify Queue struct declares `_credit: QueueCredit` after `scheduler: Scheduler`.
queue_struct = item_in(queue_src, 'pub(crate) struct Queue {')
sched_pos = queue_struct.index('scheduler: Scheduler')
credit_pos = queue_struct.index('_credit: QueueCredit')
assert sched_pos < credit_pos, 'QueueCredit must drop after Scheduler'
assert queue_struct[credit_pos:].strip().endswith('}'), '_credit must be last Queue field'
gpu_limits_fn = item_in(mod_src, 'fn queue_limits(&self) -> Option<uapi::drm_asahi_queue_limits>')
assert 'Some(params::queue_limits())' in gpu_limits_fn, gpu_limits_fn
assert 'DRM_ASAHI_FEATURE_INDEPENDENT_QUEUES' in gpu_src
max_queues_line = next(line for line in hw_src.splitlines() if 'const MAX_QUEUES:' in line)
flights_line = next(line for line in hw_src.splitlines() if 'const FLIGHTS:' in line)
heavy_limits = [line.strip() for line in hw_src.splitlines() if 'const COMPUTE_HEAVY_' in line]
assert len(heavy_limits) == 2, heavy_limits
assert 'const COMPUTE_SLOTS: usize = COMPUTE_UNITS;' in hw_src
assert 'const HORIZON_JOBS: u32 = COMPUTE_UNITS as u32;' in hw_src
prefix_render_fn = item('fn prefix_render(')
prefix_compute_fn = item('fn prefix_compute(')
can_queue_behind_fn = item('fn can_queue_behind_with_ta(')
assert 'let render_bar = cross_engine_barrier_type(producer_initbm, init_buffer);' in prefix_render_fn
assert (
    prefix_render_fn.index('let render_bar = cross_engine_barrier_type(producer_initbm, init_buffer);')
    < prefix_render_fn.index('if plan.ta_prev_wait')
), 'render_bar must be hoisted ahead of barrier emission'
assert prefix_render_fn.count('render_bar,') == 5, prefix_render_fn
assert 'p.prev_barrier.write(0, &prev)?;' in prefix_render_fn
# The TA->TA barrier is posted exactly when the plan asks for it and the TA ring's order
# does not already imply it, decided after every InitBM producer is known.
assert 'if plan.ta_prev_wait && !ta_ordered {' in prefix_render_fn
assert (
    prefix_render_fn.index('let producer_initbm = pf.initbm')
    < prefix_render_fn.index('let ta_ordered = ta_ring_orders_prev(cross_vm_prev, producer_initbm);')
    < prefix_render_fn.index('if plan.ta_prev_wait && !ta_ordered {')
    < prefix_render_fn.index('p.prev_barrier.write(0, &prev)?;')
), 'prev_barrier must follow the ring-order decision'
# Barrier stats are staged on `Prefix` and committed only on TA producer publication.
assert 'stats.ta_prev' not in prefix_render_fn and 'stats.ta_order' not in prefix_render_fn
assert 'pf.ta_order += 1;' in prefix_render_fn and 'pf.ta_prev += 1;' in prefix_render_fn
# The VM mark waits for visibility: an unpublished prefix never marks its VM.
assert 'note_msaa_render' not in prefix_render_fn
assert 'f.msaa |= cmd.is_multisampled();' in prefix_render_fn
assert 'p.ta_fs_barrier.write(0, &ta_fs)?;' in prefix_render_fn
assert 'p.fs_dep.write(0, &dep)?;' in prefix_render_fn
assert 'p.compute_barrier.write(off, &cbar)?;' in prefix_render_fn
assert 'p.fs_compute_barrier.write(off, &fcbar)?;' in prefix_render_fn
c2ta_slice = prefix_render_fn[
    prefix_render_fn.index('plan.ta_compute_wait'):prefix_render_fn.index('p.compute_barrier.write')
]
c2fs_slice = prefix_render_fn[
    prefix_render_fn.index('plan.fs_compute_wait'):prefix_render_fn.index('p.fs_compute_barrier.write')
]
fs2c_slice = prefix_compute_fn[
    prefix_compute_fn.index('wait_render'):prefix_compute_fn.index('s.barrier.write')
]
assert 'work::compute::EVENTS[pipe]' in prefix_compute_fn
assert 'work::compute::EVENTS[p_idx]' in c2ta_slice
assert 'work::compute::EVENTS[p_idx]' in c2fs_slice
assert 'render_bar,' in c2ta_slice and 'BarrierType::Hold' not in c2ta_slice
assert 'render_bar,' in c2fs_slice and 'BarrierType::Hold' not in c2fs_slice
assert 'BarrierType::Stamp,' in prefix_render_fn[prefix_render_fn.index('let own = encode::barrier'):prefix_render_fn.index('p.own_barrier.write')]
assert 'cross_engine_barrier_type(producer_initbm, false)' in fs2c_slice
assert 'bar_type,' in fs2c_slice and 'BarrierType::Hold' not in fs2c_slice
assert 'let waits_for_timestamps = c.query_access' in can_queue_behind_fn
assert 'DRM_ASAHI_FEATURE_RENDER_STAGE_PREFIXES_V2' in params_src
publish_prefix_body_fn = item('fn publish_prefix_body(')
assert 'self.graph.render.threshold.w32(0, prefix.draw as u32)?;' in publish_prefix_body_fn
_visible = publish_prefix_body_fn.index('*visible = true;')
assert publish_prefix_body_fn.count('job.vm.note_msaa_render();') == 1
assert publish_prefix_body_fn.index('job.vm.note_msaa_render();') > _visible
# Both render fold paths check each member before claiming its pass slot.
_same = publish_prefix_body_fn.index('while more_renders > 0 && end < job.progress.count()')
_horizon = publish_prefix_body_fn.index('// Cross-job render horizon')
assert publish_prefix_body_fn.count('self.msaa_member_allowed(&job.vm, r2)') == 2
_m1 = publish_prefix_body_fn.index('self.msaa_member_allowed(&job.vm, r2)', _same)
assert _m1 < publish_prefix_body_fn.index('pick_pass_in_for(', _same) < _horizon
_m2 = publish_prefix_body_fn.index('self.msaa_member_allowed(&job.vm, r2)', _horizon)
assert _m2 < publish_prefix_body_fn.index('pick_pass_in_for(', _horizon)
assert 'let msaa_conflict = !self.msaa_member_allowed(&job.vm, r);' in can_queue_behind_fn
assert 'self.graph.render.threshold.w32(8, prefix.draw as u32)?;' in publish_prefix_body_fn
assert 'earliest_unfinished_ta_draw' not in source and 'rearm_ta_threshold' not in source
assert 'let pipe_fold_cap = if heavy_compute && c_waits == [None; sizes::COMPUTE_PIPES]' in publish_prefix_body_fn, 'pipe_fold_cap must only apply to heavy barrier-free compute'


harness = r'''
#![allow(dead_code, unused_variables)]
#[path = "__USAGE__"]
mod usage;
use std::sync::{Arc, atomic::{AtomicU32,AtomicU64,AtomicBool,Ordering}};
use std::collections::VecDeque;
#[derive(Copy,Clone,Debug,PartialEq,Eq)] struct Error(i32);
const ETIMEDOUT:Error=Error(110);const ECANCELED:Error=Error(125);const EOVERFLOW:Error=Error(75);const EIO:Error=Error(5);const EAGAIN:Error=Error(11);const E2BIG:Error=Error(7);
type Result<T=()> = std::result::Result<T,Error>;
macro_rules! dev_err {($($argument:tt)*)=>{()};}
__PUBLICATION_ERROR__
#[derive(Copy,Clone,Debug,PartialEq,Eq)]enum Channel { Ta = 0, Fs = 1, Compute = 2 }
#[derive(Copy,Clone)]struct ComputeCommand { query_access: bool, rt: bool }
impl ComputeCommand { fn is_rt(&self) -> bool { self.rt } }
#[derive(Copy,Clone)]struct RenderCommand { heavy_ta: bool, msaa: bool }
impl RenderCommand { fn is_heavy_ta(&self) -> bool { self.heavy_ta } fn is_multisampled(&self) -> bool { self.msaa } }
const R_LIGHT: RenderCommand = RenderCommand { heavy_ta: false, msaa: false };
const R_HEAVY: RenderCommand = RenderCommand { heavy_ta: true, msaa: false };
const R_MSAA: RenderCommand = RenderCommand { heavy_ta: false, msaa: true };
#[derive(Copy,Clone)]enum Command { Compute(ComputeCommand), Render(RenderCommand) }
struct Status(AtomicU32);impl Status{fn get(&self)->u32{self.0.load(Ordering::Relaxed)}}
struct Vm { id:u64, status:Status, slot:u32, msaa:AtomicBool }
impl Vm { fn renders_msaa(&self)->bool{self.msaa.load(Ordering::Relaxed)} fn note_msaa_render(&self){self.msaa.store(true,Ordering::Relaxed)} }
struct Queue { turn:AtomicU64, priority:u32, completed:[AtomicU64;2], compute_ooo:AtomicU64, compute_ewma_us:AtomicU32 }
impl Queue {
    fn new(turn:u64, priority:u32)->Arc<Self>{
        Arc::new(Self{turn:AtomicU64::new(turn),priority,completed:[AtomicU64::new(0),AtomicU64::new(0)],compute_ooo:AtomicU64::new(0),compute_ewma_us:AtomicU32::new(0)})
    }
    __NOTE_COMPUTE_US__
    fn is_compute_retired(&self, seq:u64)->bool{
        let c=self.completed[command::COMPUTE].load(Ordering::Acquire);
        if seq<=c { return true; }
        let off=seq-c-1;
        if off==0 || off>64 { return false; }
        (self.compute_ooo.load(Ordering::Acquire) & (1u64 << (off-1))) != 0
    }
}
struct Progress { sent:AtomicU32,count:u32,pending:AtomicBool }
impl Progress {
    fn sent(&self)->u32{self.sent.load(Ordering::Relaxed)}
    fn count(&self)->u32{self.count}
    fn set_pending(&self,v:bool){self.pending.store(v,Ordering::Relaxed)}
}
struct Resources;impl Resources{fn compute_units(&self)->Result<usize>{Ok(24)}fn compute_slot_mask(&self)->Result<u32>{Ok((1u32 << 24) - 1)}fn compute_scs(&self)->bool{false}fn render_passes(&self)->Result<usize>{Ok(8)}fn render_scs(&self)->bool{false}fn pb_slots(&self)->Result<usize>{Ok(2)}fn pb_slot_mask(&self)->Result<(u32,usize)>{Ok((0b11,0))}fn pb_heap_blocks(&self)->Result<u32>{Ok(512)}}
struct Job { id:u32,vm:Arc<Vm>,queue:Arc<Queue>,progress:Progress,commands:Vec<Command>,dependency:Result,terminal_marked:AtomicBool,resources:Resources }
impl Job {
    fn check_command_dependencies(&self,_index:u32)->Result{self.dependency}
    fn mark_terminal_retired(&self)->Result{assert!(!self.progress.pending.load(Ordering::Relaxed));self.terminal_marked.store(true,Ordering::Relaxed);Ok(())}
}
fn command(job:&Job,k:u32)->Option<&Command>{job.commands.get(k as usize)}
type Completions=Vec<(u32,Result)>;
fn complete(done:&mut Completions,job:&Arc<Job>,result:Result){done.push((job.id,result));}
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
__HOLD_CAUSE__
__HOLD_CAUSE_IMPL__
#[derive(Default)]struct Stats{hold:[u64;3],hold_causes:[u64;HoldCause::COUNT],grouped:u64}
struct Claims;
struct Flight{job:Arc<Job>,claims:Claims,group:Vec<Arc<Job>>,computes:u32,renders:u32,compute_pipe:u8,rt_compute:bool,doomed:bool,recovery_suspect:bool,initbm:bool,heavy_render:bool}
impl Flight{fn new(job:Arc<Job>)->Self{Self{job,claims:Claims,group:vec![],computes:0,renders:0,compute_pipe:0,rt_compute:false,doomed:false,recovery_suspect:false,initbm:false,heavy_render:false}}}
__SCS_COMPUTE_CONFLICT__
__MSAA_RENDER_CONFLICT__
struct Engine{ordinal:u64,draw:u64,pending:Vec<Arc<Job>>,flights:Vec<Flight>,frozen:bool,turn:u64,stats:Stats,chan_wait:bool,slot_wait:bool,ta_wave_wait:bool,held_wait:bool,handoff_avoid_pipes:u32,coalesce_deadline_ns:u64}
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
    fn compute_exhausted_within(&self,_units:usize)->bool{self.compute_exhausted}fn compute_exhausted_in_mask(&self,_mask:u32)->bool{self.compute_exhausted}
    fn render_exhausted_within(&self,_passes:usize)->bool{self.render_exhausted}
    fn release(&mut self,_claims:&mut Claims){self.releases+=1;}
    fn quarantine(&mut self,_claims:&mut Claims){self.quarantines+=1;}
}
struct Pools{exhausted:bool,exhausted_vm:Option<u32>,block_pipeline_vm:Option<u32>}
impl Pools{
    fn exhausted(&self)->bool{self.exhausted}
    fn exhausted_for(&self,vm:&Vm)->bool{self.exhausted_vm==Some(vm.slot)}
    fn exhausted_for_within(&self,vm:&Vm,_limit:usize)->bool{self.exhausted_for(vm)}
    fn exhausted_for_within_mode(&self,vm:&Vm,limit:usize,_scs:bool)->bool{self.exhausted_for_within(vm,limit)}
    fn exhausted_for_mask_mode(&self,vm:&Vm,_mask:u32,_home:usize,_scs:bool)->bool{self.exhausted_for(vm)}
    fn can_pipeline(&self,vm:&Arc<Vm>)->bool{self.block_pipeline_vm!=Some(vm.slot)}
    fn can_pipeline_within(&self,vm:&Arc<Vm>,_limit:usize)->bool{self.can_pipeline(vm)}
    fn can_pipeline_within_blocks(&self,vm:&Arc<Vm>,limit:usize,_min_blocks:u32)->bool{self.can_pipeline_within(vm,limit)}
    fn can_pipeline_within_blocks_mode(&self,vm:&Arc<Vm>,limit:usize,min_blocks:u32,_scs:bool)->bool{self.can_pipeline_within_blocks(vm,limit,min_blocks)}
    fn can_pipeline_in_mask_blocks_mode(&self,vm:&Arc<Vm>,_mask:u32,_home:usize,_min_blocks:u32,_scs:bool)->bool{self.can_pipeline(vm)}
}
mod encode {
    pub struct ComputeRefs{pub invalid:bool}
    // Fault injection replaces the descriptor reader, while the actual production
    // containment branch below decides whether the unit becomes claimable again.
    pub fn validate_compute_descriptor(refs:&ComputeRefs,_old:&[u8;160],_before:u64)->Result<(),()>{if refs.invalid{Err(())}else{Ok(())}}
}
mod hw { pub mod sizes { pub const FLIGHTS:usize=32; pub const TA_WAVE_PASSES:usize=4; pub const TA_WAVE_PASSES_LIGHT:usize=8; pub const TA_LIGHT_TILEMAP_BYTES:u64=64*1024; pub const TA_WAVE_REFILL_HEADROOM:usize=2; pub const COMPUTE_PIPES:usize=8; pub const MAX_VM_COMPUTE_PIPES:usize=2; pub const COMPUTE_UNITS:usize=24; pub const PIPE1_COMPUTE_START:usize=6; pub const COMPUTE_SLOTS:usize=24; pub const RENDER_SLOTS:usize=48; pub const COALESCE_MAX_CMDS:usize=6; pub const COALESCE_WINDOW_US:u32=18; pub const COALESCE_WINDOW_NS:u64=18_000; __HEAVY_LIMITS__ } }
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
    ta_pending_calls:std::cell::Cell<u32>,
}
impl Firmware {
    fn mark_class_unavailable(&mut self,class:usize,error:Error){self.unavailable.push((class,error));}
    fn sync_quarantine_hints(&self){}
    fn compute_handoff_pipe(&self,_job:&Arc<Job>)->Option<usize>{None}
    __NEEDS_ENGINE_DRAIN__
    __RENDER_CAN_PIPELINE__
    __HOLD_CAUSE_FN__
    __NOTE_HOLD__
    __TA_WAVE_CAP_FOR__
    __ACTIVE_TA_WAVE_CAP__
    fn blocked_on_slot_headroom(&self,job:&Arc<Job>)->bool{
        if self.engine.flights.is_empty()||self.needs_engine_drain(job){return false;}
        if job.check_command_dependencies(job.progress.sent()).is_err(){return false;}
        (self.block_render_behind&&matches!(command(job,job.progress.sent()),Some(Command::Render(_))))
            ||(self.block_compute_behind&&matches!(command(job,job.progress.sent()),Some(Command::Compute(_))))
    }
    fn ta_pending_passes(&self)->usize{
        self.ta_pending_calls.set(self.ta_pending_calls.get()+1);
        self.engine.flights.iter().map(|f|f.renders as usize).sum()
    }
    fn can_queue_behind(&self,job:&Arc<Job>)->bool{
        let mut ta_pending=None;
        self.can_queue_behind_with_ta(job,&mut ta_pending)
    }
    fn can_queue_behind_with_ta(&self,job:&Arc<Job>,ta_pending:&mut Option<usize>)->bool{
        if !self.allow_behind{return false;}
        if self.needs_engine_drain(job){return false;}
        if let Some(Command::Render(r))=command(job,job.progress.sent()){
            let cap=self.ta_wave_cap_for(r.is_heavy_ta());
            if self.block_render_behind||*ta_pending.get_or_insert_with(||self.ta_pending_passes())>=cap{return false;}
        }
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
                Some(Command::Render(r))=>{
                    *pending_rings|=(1<<(Channel::Fs as u8))|(1<<(Channel::Ta as u8));
                    fl.renders=1;
                    fl.heavy_render=r.is_heavy_ta();
                    fl.initbm=!self.pools.can_pipeline(&job.vm);
                }
                _=>{*pending_rings|=1<<(Channel::Compute as u8);fl.computes=1;}
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
fn mk_vm(slot:u32)->Arc<Vm>{Arc::new(Vm{id:u64::from(slot),status:Status(AtomicU32::new(0)),slot,msaa:AtomicBool::new(false)})}
fn job_on_vm_mode(id:u32,vm:Arc<Vm>,render:bool,heavy:bool,sent:u32,dependency:Result)->Arc<Job>{
    Arc::new(Job{id,vm,
        queue:Queue::new(0,0),
        progress:Progress{sent:AtomicU32::new(sent),count:sent+1,pending:AtomicBool::new(true)},
        commands:vec![if render{Command::Render(RenderCommand{heavy_ta:heavy,msaa:false})}else{Command::Compute(ComputeCommand{query_access:true,rt:false})};sent as usize+1],dependency,terminal_marked:AtomicBool::new(false),resources:Resources})
}
fn job_on_vm(id:u32,vm:Arc<Vm>,render:bool,sent:u32,dependency:Result)->Arc<Job>{
    job_on_vm_mode(id,vm,render,false,sent,dependency)
}
fn job(id:u32,render:bool,sent:u32,dependency:Result)->Arc<Job>{
    job_on_vm(id,mk_vm(id),render,sent,dependency)
}
fn firmware(jobs:Vec<Arc<Job>>,stages:Vec<(Result,bool)>)->Firmware{
    Firmware{session:Session{shared:Shared{sleeping:AtomicBool::new(false)},rings:vec![],ring_fail:false,checks:AtomicU32::new(0),transports:AtomicU32::new(0)},objects:Checks,recovery:Recovery{error:None,publication_wait_deadline:None},
        engine:Engine{ordinal:0,draw:0,pending:jobs,flights:vec![],frozen:false,turn:0,stats:Stats::default(),chan_wait:false,slot_wait:false,ta_wave_wait:false,held_wait:false,handoff_avoid_pipes:0,coalesce_deadline_ns:0},
        slots:Slots{compute_exhausted:false,render_exhausted:false,releases:0,quarantines:0},pools:Pools{exhausted:false,exhausted_vm:None,block_pipeline_vm:None},power:Power::default(),
        unavailable:vec![],stages:stages.into_iter().map(|(result,visible)|Stage{result,visible}).collect(),
        retained:vec![],attempted:vec![],fail_stops:0,allow_behind:false,block_render_behind:false,block_compute_behind:false,
        ta_pending_calls:std::cell::Cell::new(0)}
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
            queue:Queue::new(0,priority),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:false,rt:false})}],
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
    #[test]fn hold_causes_split_hold0_by_the_gate_that_stopped_publication(){
        let vm1=mk_vm(1);
        let vm2=mk_vm(2);
        // One held publication pass per case: (in-flight job and kind, its flags, held job,
        // host knobs) -> exactly one hold[0] of the expected cause.
        let run=|flight:Arc<Job>,renders:u32,initbm:bool,doomed:bool,held:Arc<Job>,
                 allow_behind:bool,block_pipeline:Option<u32>,block_render:bool,expect:HoldCause,
                 held_wait:bool|{
            let mut f=firmware(vec![held],vec![]);
            let mut fl=Flight::new(flight);
            if renders==0{fl.computes=1;}else{fl.renders=renders;}
            fl.initbm=initbm;
            fl.doomed=doomed;
            f.engine.flights.push(fl);
            f.allow_behind=allow_behind;
            f.pools.block_pipeline_vm=block_pipeline;
            f.block_render_behind=block_render;
            let mut done=vec![];
            assert_eq!(f.publish_checked(&mut done),Ok(()));
            assert!(f.attempted.is_empty(),"{:?}",expect);
            assert_eq!(f.engine.stats.hold[0],1,"{:?}",expect);
            let mut want=[0u64;HoldCause::COUNT];
            want[expect as usize]=1;
            assert_eq!(f.engine.stats.hold_causes,want,"{:?}",expect);
            // Only a hold behind the job's own VM's flights asks for the short repoll.
            assert_eq!(f.engine.held_wait,held_wait,"{:?}",expect);
        };
        let r=|id:u32,vm:&Arc<Vm>,dep:Result|job_on_vm(id,vm.clone(),true,0,dep);
        // A warm same-VM render whose allocator cannot pipeline waits for an empty engine.
        run(r(1,&vm1,Ok(())),1,false,false,r(2,&vm1,Ok(())),true,Some(vm1.slot),false,HoldCause::DrainPb,true);
        // A cold InitBM flight drains first.
        run(r(1,&vm1,Ok(())),1,true,false,r(2,&vm1,Ok(())),true,None,false,HoldCause::DrainInitbm,true);
        // Another VM's compute in flight: no cross-VM render/compute overlap.
        run(job_on_vm(1,vm2.clone(),false,0,Ok(())),0,false,false,r(2,&vm1,Ok(())),true,None,false,HoldCause::DrainCross,false);
        // A doomed flight drains first.
        run(r(1,&vm1,Ok(())),1,false,true,r(2,&vm1,Ok(())),true,None,false,HoldCause::DrainFailed,false);
        // A full light TA wave (8 renders) pauses the next light render.
        run(r(1,&vm1,Ok(())),8,false,false,r(2,&vm1,Ok(())),true,None,false,HoldCause::TaWave,true);
        // Pass, flight or unit headroom.
        run(r(1,&vm1,Ok(())),1,false,false,r(2,&vm1,Ok(())),true,None,true,HoldCause::Slot,true);
        // A failed dependency (pick() skips jobs still waiting for one).
        run(r(1,&vm1,Ok(())),1,false,false,r(2,&vm1,Err(ECANCELED)),false,None,false,HoldCause::Dependency,false);
        // Any other refusal.
        run(r(1,&vm1,Ok(())),1,false,false,r(2,&vm1,Ok(())),false,None,false,HoldCause::Other,true);
        // Behind another VM's render (warm cross-VM renders overlap): counted, but no short repoll.
        run(r(1,&vm2,Ok(())),1,false,false,r(2,&vm1,Ok(())),false,None,false,HoldCause::Other,false);
        assert_eq!(HoldCause::NAMES.len(),HoldCause::COUNT);
        assert_eq!(HoldCause::Other as usize,HoldCause::COUNT-1);
    }
    #[test]fn alt_bypass_does_not_starve_cold_cross_vm_initbm_while_warm_overlaps(){
        let vm1=mk_vm(1);
        let vm2=mk_vm(2);
        let vm1_active=job_on_vm(1,vm1.clone(),false,0,Ok(()));
        let vm2_cold_render=job_on_vm(2,vm2.clone(),true,0,Ok(()));
        let vm1_pending=job_on_vm(3,vm1.clone(),false,0,Ok(()));
        vm1_pending.queue.turn.store(5,Ordering::Relaxed);
        let mut f=firmware(vec![vm2_cold_render,vm1_pending],vec![(Ok(()),true),(Ok(()),true)]);
        f.engine.flights.push(Flight::new(vm1_active));
        f.allow_behind=true;
        // Cold cross-VM render (requires InitBM) must drain without starvation.
        f.pools.block_pipeline_vm=Some(vm2.slot);
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert!(f.attempted.is_empty());
        assert_eq!(f.engine.stats.hold[0],1);
        assert_eq!(f.engine.pending.len(),2);
        f.engine.flights.clear();
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        // Once drained, VM2's InitBM render publishes (fl.initbm=true blocks vm1_pending behind it).
        assert_eq!(f.attempted,[2]);
        assert_eq!(f.engine.pending.len(),1);

        // Warm cross-VM render queues directly behind another VM's active render with zero hold[0].
        let vm1_r=job_on_vm(10,vm1.clone(),true,0,Ok(()));
        let vm2_warm_r=job_on_vm(11,vm2.clone(),true,0,Ok(()));
        let mut fw=firmware(vec![vm2_warm_r],vec![(Ok(()),true)]);
        let mut fl=Flight::new(vm1_r);
        fl.renders=1;
        fw.engine.flights.push(fl);
        fw.allow_behind=true;
        assert_eq!(fw.publish_checked(&mut done),Ok(()));
        assert_eq!(fw.attempted,[11]);
        assert_eq!(fw.engine.stats.hold[0],0);

        // Cross-VM non-SCS compute queues behind another VM's active compute on a disjoint pipe.
        let vm1_c=job_on_vm(12,vm1.clone(),false,0,Ok(()));
        let vm2_c=job_on_vm(13,vm2.clone(),false,0,Ok(()));
        let mut fc=firmware(vec![vm2_c],vec![(Ok(()),true)]);
        let mut flc=Flight::new(vm1_c);
        flc.computes=1;
        fc.engine.flights.push(flc);
        fc.allow_behind=true;
        assert_eq!(fc.publish_checked(&mut done),Ok(()));
        assert_eq!(fc.attempted,[13]);
        assert_eq!(fc.engine.stats.hold[0],0);

        // Cross-VM compute drains behind another VM's active render (no cross-VM R<->C overlap).
        let vm1_r2=job_on_vm(14,vm1.clone(),true,0,Ok(()));
        let vm2_c2=job_on_vm(15,vm2.clone(),false,0,Ok(()));
        let mut fcr=firmware(vec![vm2_c2],vec![(Ok(()),true)]);
        let mut flr=Flight::new(vm1_r2);
        flr.renders=1;
        fcr.engine.flights.push(flr);
        fcr.allow_behind=true;
        assert_eq!(fcr.publish_checked(&mut done),Ok(()));
        assert!(fcr.attempted.is_empty());
        assert_eq!(fcr.engine.stats.hold[0],1);

        // Symmetrically, warm cross-VM render drains behind another VM's active compute.
        let vm1_c2=job_on_vm(16,vm1,false,0,Ok(()));
        let vm2_r2=job_on_vm(17,vm2,true,0,Ok(()));
        let mut frc=firmware(vec![vm2_r2],vec![(Ok(()),true)]);
        let mut flc2=Flight::new(vm1_c2);
        flc2.computes=1;
        frc.engine.flights.push(flc2);
        frc.allow_behind=true;
        assert_eq!(frc.publish_checked(&mut done),Ok(()));
        assert!(frc.attempted.is_empty());
        assert_eq!(frc.engine.stats.hold[0],1);
    }
    #[test]fn alt_bypass_preserves_per_queue_fifo_order(){
        let vm1=mk_vm(1);
        let q1=Queue::new(0,0);
        let q2=Queue::new(1,0);
        let mk_q=|id:u32,q:Arc<Queue>,render:bool|Arc::new(Job{
            id,vm:vm1.clone(),queue:q,
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:false,rt:false})}],
            dependency:Ok(()),terminal_marked:AtomicBool::new(false),resources:Resources,
        });
        // q1 has blocked render job 1 followed by compute job 2 on the SAME queue q1;
        // q2 has ready compute job 3. alt-bypass must NOT jump q1's job 2 ahead of job 1,
        // and must select q2's job 3 instead.
        let q1_r1=mk_q(1,q1.clone(),true);
        let q1_c2=mk_q(2,q1,false);
        let q2_c3=mk_q(3,q2,false);
        let mut f=firmware(vec![q1_r1.clone(),q1_c2,q2_c3],vec![(Ok(()),true)]);
        f.engine.flights.push(Flight::new(q1_r1));
        f.allow_behind=true;
        f.block_render_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[3]);
        assert_eq!(f.engine.pending.len(),2);
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
    #[test]fn mixed_compute_and_render_batch_rings_compute_before_fs_and_ta(){
        let vm1=mk_vm(1);
        let mut f=firmware(
            vec![job_on_vm(1,vm1.clone(),true,0,Ok(())),job_on_vm(2,vm1,false,0,Ok(()))],
            vec![(Ok(()),true),(Ok(()),true)],
        );
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish(&mut done),Ok(()));
        assert_eq!(f.attempted,[1,2]);
        assert_eq!(f.session.rings,[Channel::Compute,Channel::Fs,Channel::Ta]);
    }
    #[test]fn ta_wave_pacing_flushes_doorbell_at_four_passes_and_arms_ta_wave_wait(){
        // Heavy renders fill TA_WAVE_PASSES (4) and ring FS+TA immediately; pass 5 pauses.
        let vm1=mk_vm(1);
        let mut jobs:Vec<Arc<Job>>=(1..=5).map(|id|job_on_vm_mode(id,vm1.clone(),true,true,0,Ok(()))).collect();
        jobs.push(job_on_vm(6,vm1.clone(),false,0,Ok(())));
        let stages=vec![(Ok(()),true);5];
        let mut f=firmware(jobs,stages);
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[1,2,3,4,6]);
        assert_eq!(f.session.rings,[Channel::Fs,Channel::Ta,Channel::Compute]);
        assert!(f.engine.ta_wave_wait);
        assert!(!f.engine.slot_wait);
        assert_eq!(f.engine.pending.len(),1);

        // Light renders fill TA_WAVE_PASSES_LIGHT (8) and ring FS+TA immediately; pass 9 pauses.
        let mut light_jobs:Vec<Arc<Job>>=(1..=9).map(|id|job_on_vm_mode(id,vm1.clone(),true,false,0,Ok(()))).collect();
        light_jobs.push(job_on_vm(10,vm1,false,0,Ok(())));
        let mut fl=firmware(light_jobs,vec![(Ok(()),true);9]);
        fl.allow_behind=true;
        assert_eq!(fl.publish_checked(&mut done),Ok(()));
        assert_eq!(fl.attempted,[1,2,3,4,5,6,7,8,10]);
        assert_eq!(fl.session.rings,[Channel::Fs,Channel::Ta,Channel::Compute]);
        assert!(fl.engine.ta_wave_wait);
        assert!(!fl.engine.slot_wait);
        assert_eq!(fl.engine.pending.len(),1);
    }
    #[test]fn publish_checked_d64_blocked_single_queue_hoists_ta_pending_and_skips_same_queue_in_o1(){
        let vm1=mk_vm(1);
        let q1=Queue::new(0,0);
        let mk=|id:u32|Arc::new(Job{
            id,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            dependency:Ok(()),terminal_marked:AtomicBool::new(false),resources:Resources,
        });
        let pending:Vec<Arc<Job>>=(9..=68).map(mk).collect();
        let mut f=firmware(pending,vec![]);
        for id in 1..=8 {
            let mut fl=Flight::new(mk(id));
            fl.renders=1;
            f.engine.flights.push(fl);
        }
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert!(f.attempted.is_empty());
        assert!(f.engine.ta_wave_wait);
        assert_eq!(f.engine.pending.len(),60);
        // Hoisted ta_pending + O(1) same-queue skip reads TA stamp at most once across 60 jobs.
        assert_eq!(f.ta_pending_calls.get(),1);
    }
    #[test]fn same_queue_independent_compute_bypasses_ta_wave_paused_render_only(){
        let vm1=mk_vm(1);
        let q1=Queue::new(0,0);
        let mk=|id:u32,render:bool,query_access:bool,dep:Result|Arc::new(Job{
            id,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_HEAVY)}else{Command::Compute(ComputeCommand{query_access,rt:false})}],
            dependency:dep,terminal_marked:AtomicBool::new(false),resources:Resources,
        });
        // Seed 4 active heavy TA passes so TA wave pacing is active.
        let mut active_flights=Vec::new();
        for id in 1..=4 {
            let mut fl=Flight::new(mk(id,true,false,Ok(())));
            fl.renders=1;
            fl.heavy_render=true;
            active_flights.push(fl);
        }
        // On q1: render 9 is TA-wave paused; independent !query_access compute 10 bypasses it.
        let r9=mk(9,true,false,Ok(()));
        let c10=mk(10,false,false,Ok(()));
        let mut f=firmware(vec![r9,c10],vec![(Ok(()),true)]);
        f.engine.flights=active_flights;
        f.allow_behind=true;
        let mut done=vec![];
        assert_eq!(f.publish_checked(&mut done),Ok(()));
        assert_eq!(f.attempted,[10]);
        assert_eq!(f.engine.pending.len(),1);
        assert!(f.engine.ta_wave_wait);

        // query_access compute on the same queue must NOT bypass TA-wave-paused render.
        let mut f_q=firmware(vec![mk(11,true,false,Ok(())),mk(12,false,true,Ok(()))],vec![]);
        for id in 1..=4 {
            let mut fl=Flight::new(mk(id,true,false,Ok(())));
            fl.renders=1;
            f_q.engine.flights.push(fl);
        }
        f_q.allow_behind=true;
        assert_eq!(f_q.publish_checked(&mut done),Ok(()));
        assert!(f_q.attempted.is_empty());

        // Dependent compute (dependency Err(EAGAIN)) must NOT bypass TA-wave-paused render.
        let mut f_dep=firmware(vec![mk(13,true,false,Ok(())),mk(14,false,false,Err(EAGAIN))],vec![]);
        for id in 1..=4 {
            let mut fl=Flight::new(mk(id,true,false,Ok(())));
            fl.renders=1;
            f_dep.engine.flights.push(fl);
        }
        f_dep.allow_behind=true;
        assert_eq!(f_dep.publish_checked(&mut done),Ok(()));
        assert!(f_dep.attempted.is_empty());

        // Later compute must NEVER jump an earlier pending compute on the same queue.
        let mut f_cc=firmware(
            vec![mk(15,true,false,Ok(())),mk(16,false,true,Ok(())),mk(17,false,false,Ok(()))],
            vec![],
        );
        for id in 1..=4 {
            let mut fl=Flight::new(mk(id,true,false,Ok(())));
            fl.renders=1;
            f_cc.engine.flights.push(fl);
        }
        f_cc.allow_behind=true;
        f_cc.block_compute_behind=false;
        // Job 16 is query_access (cannot bypass render 15); job 17 sits behind compute 16 so it must also wait.
        assert_eq!(f_cc.publish_checked(&mut done),Ok(()));
        assert!(f_cc.attempted.is_empty());
    }

}
mod fence {
    pub const MAX_FW_WAITS: usize = 8;
    pub struct FwWait {
        pub queue: std::sync::Arc<crate::Queue>,
        pub last_ordinal: [Option<u64>; 2],
    }
}
#[derive(Copy,Clone)]struct Order{engine:usize,sequence:u64,wait_through:[Option<u64>;2]}
struct QueueBehindJob{id:u32,vm:Arc<Vm>,queue:Arc<Queue>,progress:Progress,commands:Vec<Command>,order:Order,extra_orders:Vec<Order>,dependency:Result,resources:Resources,fw_waits:Vec<fence::FwWait>}
impl QueueBehindJob{
    fn check_command_dependencies(&self,_index:u32)->Result{self.dependency}
    fn command_order(&self,index:u32)->Option<Order>{
        if !self.extra_orders.is_empty() {
            self.extra_orders.get(index as usize).copied()
        } else {
            Some(self.order)
        }
    }
}
struct QueueBehindStamp{val:std::cell::Cell<u32>,reads:std::cell::Cell<u32>}
impl QueueBehindStamp{
    fn new(v:u32)->Self{Self{val:std::cell::Cell::new(v),reads:std::cell::Cell::new(0)}}
    fn r32(&self,_off:usize)->Result<u32>{self.reads.set(self.reads.get()+1);Ok(self.val.get())}
}
struct QueueBehindRenderGraph{fw_stamp:[QueueBehindStamp;2]}
struct QueueBehindGraph{render:QueueBehindRenderGraph}
impl QueueBehindGraph{fn new()->Self{Self{render:QueueBehindRenderGraph{fw_stamp:[QueueBehindStamp::new(0),QueueBehindStamp::new(0)]}}}}
struct QueueBehindFlight{job:Arc<QueueBehindJob>,group:Vec<Arc<QueueBehindJob>>,first:usize,count:usize,computes:u32,renders:u32,compute_pipe:u8,rt_compute:bool,heavy_render:bool,published_ns:u64,target:u32,cstamp:[u32;16],r_ta_stamp:[u32;16],r_fs_stamp:[u32;16],fs_target:u32,initbm:bool,has_timestamps:bool,doomed:bool,recovery_suspect:bool}
struct QueueBehindEngine{ordinal:u64,draw:u64,ta_target:u32,next_compute_pipe:u8,pipe_last_vm:[Option<u64>;hw::sizes::COMPUTE_PIPES],pipe_last_used:[u64;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:u32,flights:Vec<QueueBehindFlight>}
struct QueueBehindSlots{compute_exhausted:bool,render_exhausted:bool,compute_available:bool,pass_available:bool}
impl QueueBehindSlots{
    fn compute_exhausted(&self)->bool{self.compute_exhausted}
    fn compute_exhausted_in_mask(&self,mask:u32)->bool{self.compute_exhausted||mask==0}
    fn render_exhausted(&self)->bool{self.render_exhausted}
    fn compute_available(&self,_n:usize)->bool{self.compute_available}
    fn compute_available_in_mask(&self,_mask:u32)->bool{self.compute_available}
    fn compute_available_in_mask_for_pipe(&self,mask:u32,pipe:usize)->bool{
        let allowed = if pipe == 0 { mask } else { mask & !((1u32 << hw::sizes::PIPE1_COMPUTE_START) - 1) };
        self.compute_available && allowed != 0
    }
    fn compute_available_for_pipe(&self,n:usize,pipe:usize)->bool{
        self.compute_available && (pipe == 0 || n > hw::sizes::PIPE1_COMPUTE_START)
    }
    fn pass_available(&self,_n:usize)->bool{self.pass_available}
}
mod resources { pub const BASE_COMPUTE_UNITS: usize = 2; }
struct QueueBehindPools{exhausted:bool,can_pipeline:bool,block_pipeline_vm:Option<u32>}
impl QueueBehindPools{
    fn exhausted(&self)->bool{self.exhausted}
    fn can_pipeline(&self,vm:&Arc<Vm>)->bool{self.can_pipeline&&self.block_pipeline_vm!=Some(vm.slot)}
    fn can_pipeline_within(&self,vm:&Arc<Vm>,_limit:usize)->bool{self.can_pipeline(vm)}
    fn can_pipeline_within_blocks(&self,vm:&Arc<Vm>,limit:usize,_min_blocks:u32)->bool{self.can_pipeline_within(vm,limit)}
    fn can_pipeline_within_blocks_mode(&self,vm:&Arc<Vm>,limit:usize,min_blocks:u32,_scs:bool)->bool{self.can_pipeline_within_blocks(vm,limit,min_blocks)}
    fn can_pipeline_in_mask_blocks_mode(&self,vm:&Arc<Vm>,_mask:u32,_home:usize,_min_blocks:u32,_scs:bool)->bool{self.can_pipeline(vm)}
}
struct QueueBehindHost{graph:QueueBehindGraph,engine:QueueBehindEngine,slots:QueueBehindSlots,pools:QueueBehindPools}
mod queue_behind_scope {
    use super::*;
    use crate::hw::sizes;
    type Job = QueueBehindJob;
    type Flight = QueueBehindFlight;
    fn command(job:&Job,k:u32)->Option<&Command>{job.commands.get(k as usize)}
    mod params { pub fn compute_flights()->usize{16} pub fn render_flights()->usize{47} }
    #[allow(non_upper_case_globals)]
    mod kernel {
        pub mod uapi {
            pub const drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS: u32 = 1 << 20;
            pub const drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES: u32 = 1 << 23;
        }
    }
    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    enum BarrierType {
        Stamp = 0,
        Hold = 1,
    }
    impl BarrierType {
        const fn from_stamp_only(stamp_only: bool) -> Self {
            if stamp_only { Self::Stamp } else { Self::Hold }
        }
    }
    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    __WAIT_TARGET__
    __CROSS_ENGINE_BARRIER_TYPE__
    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    __RENDER_BARRIER_PLAN__
    __PLAN_RENDER_BARRIERS__
    __TA_RING_ORDERS_PREV__
    __SCS_COMPUTE_CONFLICT__
    __MSAA_RENDER_CONFLICT__
    impl QueueBehindHost {
        __NEEDS_ENGINE_DRAIN__
        __RENDER_CAN_PIPELINE__
        __BLOCKED_ON_SLOT_HEADROOM__
        __TA_PENDING_PASSES__
        __TA_WAVE_CAP_FOR__
        __CAN_QUEUE_BEHIND__
        __CAN_QUEUE_BEHIND_WITH_TA__
        __MSAA_MEMBER_ALLOWED__
        __WAIT_STAMP_FOR_WITH__
        __WAIT_STAMP_FOR__
        __CROSS_ENGINE_WAIT__
        __WAIT_COMPUTE_STAMPS_FOR__
        __COMPUTE_QUEUE_IS_HEAVY__
        __COMPUTE_HANDOFF_PIPE__
        __HAS_COMPUTE_PIPE_FOR_VM__
        __CHOOSE_COMPUTE_PIPE__
    }
    #[test]fn timed_compute_queues_behind_same_vm_and_coexists_with_render(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let mk=|id:u32,vm:Arc<Vm>,queue:Arc<Queue>,render:bool|Arc::new(QueueBehindJob{
            id,vm,queue,progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:true,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let j1=mk(1,vm1.clone(),q1.clone(),false);
        let j2=mk(2,vm1.clone(),q1.clone(),false);
        let j_other_vm=mk(3,vm2.clone(),q1.clone(),false);
        let j_render=mk(4,vm1.clone(),q1.clone(),true);
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{job:j1,group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x100,cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        assert!(p.can_queue_behind(&j2));
        // Cross-VM non-SCS compute queues on a disjoint compute pipe without draining.
        assert!(p.can_queue_behind(&j_other_vm));
        assert!(!p.needs_engine_drain(&j_other_vm));
        assert!(p.can_queue_behind(&j_render));
        p.engine.flights[0].doomed=true;
        assert!(!p.can_queue_behind(&j2));
        assert!(!p.can_queue_behind(&j_other_vm));
        p.engine.flights[0].doomed=false;
        p.engine.flights[0].renders=1;
        p.engine.flights[0].computes=0;
        assert!(p.can_queue_behind(&j2));
        assert!(!p.can_queue_behind(&j_other_vm));
        assert!(p.needs_engine_drain(&j_other_vm));
        p.engine.flights[0].initbm=true;
        assert!(!p.can_queue_behind(&j2));
        assert!(!p.can_queue_behind(&j_other_vm));
        p.engine.flights[0].initbm=false;
        p.engine.flights[0].renders=0;
        p.engine.flights[0].computes=1;
        p.slots.compute_available=false;
        assert!(!p.can_queue_behind(&j2));
    }
    #[test]fn scs_compute_shares_the_compute_engine_only_with_scs_compute_of_its_vm(){
        // gpubench compute.long: a ray-query lane (vm1) beside a long-ALU lane (vm2). The
        // ray-query dispatch was reported complete with its results unwritten whenever the
        // other lane's compute ran with it, and its stores then faulted in its closed VM.
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let q2=Queue::new(0,0);
        let mk=|id:u32,vm:Arc<Vm>,queue:Arc<Queue>,rt:bool|Arc::new(QueueBehindJob{
            id,vm,queue,progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt})],
            order:Order{engine:command::COMPUTE,sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let flight=|job:Arc<QueueBehindJob>,rt_compute:bool,compute_pipe:u8|QueueBehindFlight{job,group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe,rt_compute,heavy_render:false,published_ns:0,target:0x100,cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false};
        let rq=mk(1,vm1.clone(),q1.clone(),true);
        let rq_next=mk(2,vm1.clone(),q1.clone(),true);
        let rq_post=mk(3,vm1.clone(),q1.clone(),false);
        let alu=mk(4,vm2.clone(),q2.clone(),false);
        let rq_vm2=mk(5,vm2.clone(),q2.clone(),true);
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![flight(rq.clone(),true,0)]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Behind an SCS flight only SCS compute of its own VM queues, so one ray-query
        // queue keeps pipelining.
        assert!(p.can_queue_behind(&rq_next));
        assert!(!p.needs_engine_drain(&rq_next));
        for j in [&alu,&rq_post,&rq_vm2] {
            assert!(!p.can_queue_behind(j));
            assert!(p.needs_engine_drain(j));
        }
        // SCS compute does not start beside non-SCS compute on any pipe; non-SCS cross-VM
        // compute runs concurrently on disjoint compute pipes without draining.
        for pipe in [0u8,1] {
            p.engine.flights=vec![flight(alu.clone(),false,pipe)];
            assert!(!p.can_queue_behind(&rq));
            assert!(p.needs_engine_drain(&rq));
            assert!(p.can_queue_behind(&rq_post));
            assert!(!p.needs_engine_drain(&rq_post));
            p.engine.flights=vec![flight(rq_post.clone(),false,pipe)];
            assert!(!p.can_queue_behind(&rq));
            assert!(p.needs_engine_drain(&rq));
        }
        // A render flight of its own VM does not hold it; once SCS compute is gone, the rest
        // overlaps as before.
        let mut render=flight(mk(6,vm1.clone(),q1.clone(),false),false,0);
        render.computes=0;
        render.renders=1;
        p.engine.flights=vec![render];
        assert!(!p.needs_engine_drain(&rq));
        assert!(p.can_queue_behind(&rq));
        p.engine.flights=vec![flight(rq_post.clone(),false,0)];
        assert!(p.can_queue_behind(&rq_post));
        assert!(!p.needs_engine_drain(&rq_post));
        assert!(p.can_queue_behind(&alu));
        assert!(!p.needs_engine_drain(&alu));
    }
    #[test]fn compute_query_access_waits_for_timed_predecessor_unless_no_query_access(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let q2=Queue::new(0,0);
        let mk=|id:u32,queue:Arc<Queue>,query_access:bool,wait_vdm:Option<u64>,wait_cdm:Option<u64>|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue,
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:u64::from(id),wait_through:[wait_vdm,wait_cdm]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let timed=mk(1,q1.clone(),true,None,None);
        let query_copy=mk(2,q1.clone(),true,None,Some(1));
        let no_query=mk(2,q1.clone(),false,None,Some(1));
        let no_barrier=mk(2,q1.clone(),true,None,None);
        let other_queue=mk(2,q2.clone(),true,None,Some(1));
        let p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{job:timed,group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x100,cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,has_timestamps:true,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        assert!(!p.can_queue_behind(&query_copy));
        assert!(p.can_queue_behind(&no_query));
        assert!(p.can_queue_behind(&no_barrier));
        assert!(p.can_queue_behind(&other_queue));
        // Cross-engine render timestamp predecessor also blocks query_access compute.
        let timed_render=Arc::new(QueueBehindJob{
            id:1,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let pr=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{job:timed_render,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x200;a},fs_target:0x200,initbm:false,has_timestamps:true,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        assert!(!pr.can_queue_behind(&mk(2,q1.clone(),true,Some(1),None)));
        assert!(pr.can_queue_behind(&mk(2,q1.clone(),false,Some(1),None)));
        assert!(pr.can_queue_behind(&mk(2,q1.clone(),true,None,None)));
    }
    #[test]fn render_queues_behind_same_vm_and_coexists_with_compute_while_blocking_initbm(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let mk=|id:u32,vm:Arc<Vm>,render:bool|Arc::new(QueueBehindJob{
            id,vm,queue:q1.clone(),progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:true,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r1=mk(1,vm1.clone(),true);
        let r2=mk(2,vm1.clone(),true);
        let r_other_vm=mk(3,vm2.clone(),true);
        let c1=mk(4,vm1.clone(),false);
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{job:r1,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x200;a},fs_target:0x200,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false}]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        assert!(p.can_queue_behind(&r2));
        // Warm cross-VM render queues behind healthy non-InitBM render; cold InitBM VM drains.
        assert!(p.can_queue_behind(&r_other_vm));
        p.pools.block_pipeline_vm=Some(vm2.slot);
        assert!(!p.can_queue_behind(&r_other_vm));
        assert!(p.needs_engine_drain(&r_other_vm));
        p.pools.block_pipeline_vm=None;
        assert!(p.can_queue_behind(&c1));
        p.engine.flights[0].initbm=true;
        assert!(!p.can_queue_behind(&r2));
        assert!(!p.can_queue_behind(&r_other_vm));
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
    #[test]fn cross_vm_render_compute_overlap_matrix(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let q2=Queue::new(0,0);
        let mk=|id:u32,vm:Arc<Vm>,queue:Arc<Queue>,render:bool|Arc::new(QueueBehindJob{
            id,vm,queue,progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:false,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let vm1_r=mk(1,vm1.clone(),q1.clone(),true);
        let vm1_c=mk(2,vm1.clone(),q1.clone(),false);
        let vm2_r=mk(3,vm2.clone(),q2.clone(),true);
        let vm2_c=mk(4,vm2.clone(),q2.clone(),false);
        let mk_flight=|job:Arc<QueueBehindJob>,c:u32,r:u32|QueueBehindFlight{
            job,group:vec![],first:0,count:1,computes:c,renders:r,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,
            target:if c!=0{0x100}else{0},cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x200;a},
            fs_target:if r!=0{0x200}else{0},initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
        };
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![mk_flight(vm1_r.clone(),0,1)]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Active VM1 Render: same-VM R & C allowed; cross-VM R allowed; cross-VM C blocked + drains.
        assert!(p.can_queue_behind(&vm1_r) && !p.needs_engine_drain(&vm1_r));
        assert!(p.can_queue_behind(&vm1_c) && !p.needs_engine_drain(&vm1_c));
        assert!(p.can_queue_behind(&vm2_r) && !p.needs_engine_drain(&vm2_r));
        assert!(!p.can_queue_behind(&vm2_c) && p.needs_engine_drain(&vm2_c));
        // Active VM1 Compute: same-VM R & C allowed; cross-VM non-SCS C allowed on a disjoint
        // pipe; cross-VM R blocked + drains.
        p.engine.flights=vec![mk_flight(vm1_c.clone(),1,0)];
        assert!(p.can_queue_behind(&vm1_r) && !p.needs_engine_drain(&vm1_r));
        assert!(p.can_queue_behind(&vm1_c) && !p.needs_engine_drain(&vm1_c));
        assert!(p.can_queue_behind(&vm2_c) && !p.needs_engine_drain(&vm2_c));
        assert!(!p.can_queue_behind(&vm2_r) && p.needs_engine_drain(&vm2_r));
        // Mixed active VM1 Render + VM1 Compute: both cross-VM R and cross-VM C blocked + drain.
        p.engine.flights=vec![mk_flight(vm1_r.clone(),0,1),mk_flight(vm1_c.clone(),1,0)];
        assert!(!p.can_queue_behind(&vm2_r) && p.needs_engine_drain(&vm2_r));
        assert!(!p.can_queue_behind(&vm2_c) && p.needs_engine_drain(&vm2_c));
        // Once a VM renders multisampled, none of its renders shares the TA/FS queues with
        // another VM's render, in either order; its own renders still queue behind each other.
        let mk_msaa=|id:u32,vm:Arc<Vm>,queue:Arc<Queue>|Arc::new(QueueBehindJob{
            id,vm,queue,progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_MSAA)],
            order:Order{engine:command::RENDER,sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let vm2_ms=mk_msaa(6,vm2.clone(),q2.clone());
        // Neither VM has rendered multisampled: only a multisampled render is held.
        p.engine.flights=vec![mk_flight(vm1_r.clone(),0,1)];
        assert!(p.can_queue_behind(&vm2_r) && !p.needs_engine_drain(&vm2_r));
        assert!(!p.can_queue_behind(&vm2_ms) && p.needs_engine_drain(&vm2_ms));
        // VM1 has rendered multisampled: VM2's single-sampled render is held behind any VM1
        // render flight, VM1's own renders are not.
        vm1.note_msaa_render();
        let vm1_ms=mk_msaa(5,vm1.clone(),q1.clone());
        assert!(p.can_queue_behind(&vm1_r) && !p.needs_engine_drain(&vm1_r));
        assert!(p.can_queue_behind(&vm1_ms) && !p.needs_engine_drain(&vm1_ms));
        assert!(!p.can_queue_behind(&vm2_r) && p.needs_engine_drain(&vm2_r));
        assert!(!p.can_queue_behind(&vm2_ms) && p.needs_engine_drain(&vm2_ms));
        // ...and VM1's single-sampled render is held behind any VM2 render flight.
        p.engine.flights=vec![mk_flight(vm2_r.clone(),0,1)];
        assert!(!p.can_queue_behind(&vm1_r) && p.needs_engine_drain(&vm1_r));
        assert!(p.can_queue_behind(&vm2_r) && !p.needs_engine_drain(&vm2_r));
        // Compute flights carry no render: the existing cross-VM compute rule applies.
        p.engine.flights=vec![mk_flight(vm2_c.clone(),1,0)];
        assert!(!p.can_queue_behind(&vm1_ms) && p.needs_engine_drain(&vm1_ms));
        assert!(p.can_queue_behind(&vm2_r) && !p.needs_engine_drain(&vm2_r));
        assert!(p.msaa_member_allowed(&vm1,&R_MSAA));
    }
    #[test]fn render_fold_members_obey_cross_vm_msaa_rule(){
        // The same-submit fold and the cross-job horizon fold ask msaa_member_allowed() for
        // every candidate member (source-order asserts above): a single-sampled lead of an
        // unmarked VM may be admitted beside another VM's render, but a multisampled member
        // after it splits the same-submit fold and declines the horizon fold.
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q2=Queue::new(0,0);
        let lead=Arc::new(QueueBehindJob{
            id:1,vm:vm1.clone(),queue:Queue::new(0,0),progress:Progress{sent:AtomicU32::new(0),count:2,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT),Command::Render(R_MSAA)],
            order:Order{engine:command::RENDER,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let other=Arc::new(QueueBehindJob{
            id:2,vm:vm2.clone(),queue:q2,progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:2,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let in_flight=QueueBehindFlight{
            job:other,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,
            target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x200;a},
            fs_target:0x200,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
        };
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![in_flight]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // The lead (command 0) is admitted; the multisampled member (command 1) is refused.
        assert!(p.can_queue_behind(&lead) && !p.needs_engine_drain(&lead));
        assert!(p.msaa_member_allowed(&vm1,&R_LIGHT));
        assert!(!p.msaa_member_allowed(&vm1,&R_MSAA));
        // Once VM2 is marked, no member of VM1 may join beside it.
        vm2.note_msaa_render();
        assert!(!p.msaa_member_allowed(&vm1,&R_LIGHT));
        assert!(!p.can_queue_behind(&lead) && p.needs_engine_drain(&lead));
        // With nothing of another VM in flight every member may join.
        p.engine.flights.clear();
        assert!(p.msaa_member_allowed(&vm1,&R_MSAA));
        assert!(!vm1.renders_msaa());
    }
    #[test]fn cross_engine_wait_resolves_barriers_and_skips_no_barrier_or_retired(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let q2=Queue::new(0,0);
        let mk=|id:u32,queue:Arc<Queue>,render:bool,seq:u64,wait:[Option<u64>;2]|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue,
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:false,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:seq,wait_through:wait},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r1=mk(1,q1.clone(),true,1,[None,None]);
        let c1=mk(2,q1.clone(),false,1,[Some(1),None]);
        let c_nobarrier=mk(3,q1.clone(),false,2,[None,None]);
        let r2_after_c1=mk(4,q1.clone(),true,2,[Some(1),Some(1)]);
        let c_other_q=mk(5,q2.clone(),false,1,[None,None]);
        let p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![
                QueueBehindFlight{job:r1,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x500;a},fs_target:0x500,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false},
                QueueBehindFlight{job:c1.clone(),group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x700,cstamp:{let mut a=[0;16];a[0]=0x700;a},r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false},
            ]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Same-queue FS -> CDM barrier resolves render's fs_target; CDM -> TA resolves compute's target.
        assert_eq!(p.cross_engine_wait(&c1,0,command::RENDER),Ok(Some(WaitTarget{stamp:0x500,initbm:false})));
        assert_eq!(p.cross_engine_wait(&r2_after_c1,0,command::COMPUTE),Ok(Some(WaitTarget{stamp:0x700,initbm:false})));
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
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt:false});3],
            order:Order{engine:command::COMPUTE,sequence:1,wait_through:[None,None]},
            extra_orders:vec![
                Order{engine:command::COMPUTE,sequence:1,wait_through:[None,None]},
                Order{engine:command::COMPUTE,sequence:2,wait_through:[None,None]},
                Order{engine:command::COMPUTE,sequence:3,wait_through:[None,None]},
            ],
            dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r_wait_c2=mk(7,q1.clone(),true,3,[None,Some(2)]);
        let r_wait_c3=mk(8,q1.clone(),true,4,[None,Some(3)]);
        let r_wait_missing=mk(9,q1.clone(),true,5,[None,Some(99)]);
        let mut cstamp=[0u32;16];
        cstamp[0]=0x100;cstamp[1]=0x200;cstamp[2]=0x300;
        let pm=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:3,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![
                QueueBehindFlight{job:multi_c,group:vec![],first:0,count:3,computes:3,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x300,cstamp,r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false},
            ]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        assert_eq!(pm.cross_engine_wait(&r_wait_c2,0,command::COMPUTE),Ok(Some(WaitTarget{stamp:0x200,initbm:false})));
        assert_eq!(pm.cross_engine_wait(&r_wait_c3,0,command::COMPUTE),Ok(Some(WaitTarget{stamp:0x300,initbm:false})));
        assert_eq!(pm.cross_engine_wait(&r_wait_missing,0,command::COMPUTE),Err(EAGAIN));
    }
    #[test]fn cross_engine_barrier_keeps_hold_when_producer_or_consumer_carries_initbm(){
        // Healthy same-VM non-InitBM handoffs downgrade to BarrierType::Stamp (type 0).
        assert_eq!(cross_engine_barrier_type(false, false), BarrierType::Stamp);
        // Any InitBM on either producer (including FS -> CDM) or consumer keeps BarrierType::Hold.
        assert_eq!(cross_engine_barrier_type(true, false), BarrierType::Hold);
        assert_eq!(cross_engine_barrier_type(false, true), BarrierType::Hold);
        assert_eq!(cross_engine_barrier_type(true, true), BarrierType::Hold);

        // Verify wait_stamp_for_with / cross_engine_wait propagate producer flight.initbm.
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let mk=|id:u32,render:bool,seq:u64,wait:[Option<u64>;2]|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:false,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:seq,wait_through:wait},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r_initbm=mk(1,true,1,[None,None]);
        let c_after_r=mk(2,false,1,[Some(1),None]);
        let r_after_r=mk(3,true,2,[Some(1),None]);
        let host=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{
                job:r_initbm,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,
                cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x900;a},fs_target:0x900,
                initbm:true,has_timestamps:false,doomed:false,recovery_suspect:false,
            }]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        let fs2c = host.cross_engine_wait(&c_after_r, 0, command::RENDER).unwrap().unwrap();
        assert_eq!(fs2c, WaitTarget { stamp: 0x900, initbm: true });
        assert_eq!(cross_engine_barrier_type(fs2c.initbm, false), BarrierType::Hold);
        let r2r = host.wait_stamp_for(&r_after_r, command::RENDER, Some(1)).unwrap().unwrap();
        assert_eq!(r2r, WaitTarget { stamp: 0x900, initbm: true });
        assert_eq!(cross_engine_barrier_type(r2r.initbm, false), BarrierType::Hold);
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
    #[test]fn multi_queue_concurrent_compute_and_render_on_same_vm(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q_comp=Queue::new(0,2);
        let q_rend=Queue::new(0,2);
        let c_job=Arc::new(QueueBehindJob{
            id:1,vm:vm1.clone(),queue:q_comp.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r_job=Arc::new(QueueBehindJob{
            id:2,vm:vm1.clone(),queue:q_rend.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:0,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{
                job:c_job.clone(),group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x100,cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,
                initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
            }]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Queue 1 render queues directly behind Queue 0 compute on the same VM with no cross-engine barrier.
        assert!(p.can_queue_behind(&r_job));
        assert_eq!(p.cross_engine_wait(&r_job,0,command::COMPUTE),Ok(None));
        // Symmetrically, Queue 0 compute queues directly behind Queue 1 render on the same VM.
        p.engine.flights[0]=QueueBehindFlight{
            job:r_job,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x200;a},fs_target:0x200,
            initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
        };
        assert!(p.can_queue_behind(&c_job));
        assert_eq!(p.cross_engine_wait(&c_job,0,command::RENDER),Ok(None));
    }
    #[test]fn cross_engine_wait_and_compute_flight_cap_account_for_batched_horizon_members(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,2);
        let mk=|id:u32,render:bool,seq:u64,wait:[Option<u64>;2]|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access:false,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:seq,wait_through:wait},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let c1=mk(1,false,1,[None,None]);
        let c2=mk(2,false,2,[None,None]);
        let c3=mk(3,false,3,[None,None]);
        let r_wait_c2=mk(4,true,1,[None,Some(2)]);
        let r_wait_c3=mk(5,true,2,[None,Some(3)]);
        let c4=mk(6,false,4,[None,None]);
        let mut cstamp=[0u32;16];
        cstamp[0]=0x100;cstamp[1]=0x200;cstamp[2]=0x300;
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:3,draw:0,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{
                job:c1,group:vec![c2,c3],first:0,count:1,computes:3,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x300,cstamp,r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,
                initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
            }]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        assert_eq!(p.cross_engine_wait(&r_wait_c2,0,command::COMPUTE),Ok(Some(WaitTarget{stamp:0x200,initbm:false})));
        assert_eq!(p.cross_engine_wait(&r_wait_c3,0,command::COMPUTE),Ok(Some(WaitTarget{stamp:0x300,initbm:false})));
        // Active computes sum flight.computes: 16 batched computes saturates compute_flights().
        assert!(p.can_queue_behind(&c4));
        p.engine.flights[0].computes=16;
        assert!(!p.can_queue_behind(&c4));
    }
    #[test]fn multi_pass_render_flight_wait_stamp_and_render_cap(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,2);
        let multi=Arc::new(QueueBehindJob{
            id:1,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:3,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT),Command::Render(R_LIGHT),Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:1,wait_through:[None,None]},
            extra_orders:vec![
                Order{engine:command::RENDER,sequence:1,wait_through:[None,None]},
                Order{engine:command::RENDER,sequence:2,wait_through:[Some(1),None]},
                Order{engine:command::RENDER,sequence:3,wait_through:[Some(2),None]},
            ],
            dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let next_r=Arc::new(QueueBehindJob{
            id:2,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:4,wait_through:[Some(3),None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let mut r_fs=[0u32;16];
        r_fs[0]=0x100;r_fs[1]=0x200;r_fs[2]=0x300;
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:0,draw:3,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{
                job:multi.clone(),group:vec![],first:0,count:3,computes:0,renders:3,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:r_fs,fs_target:0x300,
                initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
            }]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Lookup into published multi-pass render flight resolves interior and tail passes.
        assert_eq!(p.wait_stamp_for(&next_r,command::RENDER,Some(1)),Ok(Some(WaitTarget{stamp:0x100,initbm:false})));
        assert_eq!(p.wait_stamp_for(&next_r,command::RENDER,Some(2)),Ok(Some(WaitTarget{stamp:0x200,initbm:false})));
        assert_eq!(p.wait_stamp_for(&next_r,command::RENDER,Some(3)),Ok(Some(WaitTarget{stamp:0x300,initbm:false})));
        assert_eq!(p.wait_stamp_for(&next_r,command::RENDER,Some(4)),Err(EAGAIN));
        // Lookup against an in-progress same-batch flight resolves earlier pass before publish.
        let mut cur_fs=[0u32;16];
        cur_fs[0]=0x400;
        let cur=QueueBehindFlight{
            job:next_r.clone(),group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:cur_fs,fs_target:0x400,
            initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
        };
        assert_eq!(p.wait_stamp_for_with(&next_r,command::RENDER,Some(4),Some(&cur)),Ok(Some(WaitTarget{stamp:0x400,initbm:false})));
        // Cross-job render horizon flight resolves folded flight.group member stamps too.
        let r_fold_a=Arc::new(QueueBehindJob{
            id:10,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:5,wait_through:[Some(4),None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r_fold_b=Arc::new(QueueBehindJob{
            id:11,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:6,wait_through:[Some(5),None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let mut grp_fs=[0u32;16];
        grp_fs[0]=0x400;grp_fs[1]=0x500;grp_fs[2]=0x600;
        let cur_grp=QueueBehindFlight{
            job:next_r.clone(),group:vec![r_fold_a.clone(),r_fold_b.clone()],first:0,count:1,computes:0,renders:3,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:grp_fs,fs_target:0x600,
            initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
        };
        assert_eq!(p.wait_stamp_for_with(&r_fold_b,command::RENDER,Some(4),Some(&cur_grp)),Ok(Some(WaitTarget{stamp:0x400,initbm:false})));
        assert_eq!(p.wait_stamp_for_with(&r_fold_b,command::RENDER,Some(5),Some(&cur_grp)),Ok(Some(WaitTarget{stamp:0x500,initbm:false})));
        assert_eq!(p.wait_stamp_for_with(&r_fold_b,command::RENDER,Some(6),Some(&cur_grp)),Ok(Some(WaitTarget{stamp:0x600,initbm:false})));
        // Active renders sum flight.renders: 44 passes (4x11-pass postfx) still pipelines
        // behind once TA has finished, while 47 batched renders saturates render_flights().
        assert!(p.can_queue_behind(&next_r));
        p.engine.flights[0].renders=44;
        assert!(p.can_queue_behind(&next_r));
        p.engine.flights[0].renders=47;
        assert!(!p.can_queue_behind(&next_r));
    }
    #[test]fn ta_pending_passes_paces_unfinished_ta_waves_without_throttling_finished_ta(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,2);
        let mk=|id:u32,heavy:bool|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(RenderCommand{heavy_ta:heavy,msaa:false})],
            order:Order{engine:command::RENDER,sequence:u64::from(id),wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r_live=mk(1,false);
        let next_light=mk(2,false);
        let next_heavy=mk(3,true);
        let mut r_ta=[0u32;16];
        for i in 0..8 {
            r_ta[i]=(i as u32 + 1)*0x100;
        }
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:0,draw:8,ta_target:0x800,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{
                job:r_live,group:vec![],first:0,count:8,computes:0,renders:3,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],
                r_ta_stamp:r_ta,r_fs_stamp:r_ta,fs_target:0x800,
                initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
            }]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Below TA_WAVE_PASSES (3 < 4): zero WC fw_stamp reads.
        assert_eq!(p.ta_pending_passes(),3);
        assert_eq!(p.graph.render.fw_stamp[0].reads.get(),0);
        assert!(p.can_queue_behind(&next_light));
        assert!(p.can_queue_behind(&next_heavy));
        // 4 unfinished TA passes: heavy render pauses at 4, light render keeps pipelining up to 8.
        p.engine.flights[0].renders=4;
        assert_eq!(p.ta_pending_passes(),4);
        assert!(p.graph.render.fw_stamp[0].reads.get()>0);
        assert!(!p.can_queue_behind(&next_heavy));
        assert!(p.can_queue_behind(&next_light));
        // Heavy active flight also enforces conservative cap 4 on light followers.
        p.engine.flights[0].heavy_render=true;
        assert!(!p.can_queue_behind(&next_light));
        p.engine.flights[0].heavy_render=false;
        // 8 unfinished light TA passes pauses light renders until TA advances.
        p.engine.flights[0].renders=8;
        assert_eq!(p.ta_pending_passes(),8);
        assert!(!p.can_queue_behind(&next_light));
        // Once pass 0 finishes TA (obs=0x100) while FS is still running, ta_pending drops to 7.
        p.graph.render.fw_stamp[0].val.set(0x100);
        assert_eq!(p.ta_pending_passes(),7);
        assert!(p.can_queue_behind(&next_light));
        // Even with 16 active FS passes, finished TA (obs=0x800) leaves single-pass pipelining unthrottled.
        p.graph.render.fw_stamp[0].val.set(0x800);
        p.engine.flights[0].renders=16;
        assert_eq!(p.ta_pending_passes(),0);
        assert!(p.can_queue_behind(&next_light));
        assert!(p.can_queue_behind(&next_heavy));
    }
    #[test]fn slot_wait_arms_only_for_same_vm_slot_or_flight_headroom_blocks(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,0);
        let mk=|id:u32,vm:Arc<Vm>,render:bool,dep:Result,query_access:bool,wait:[Option<u64>;2]|Arc::new(QueueBehindJob{
            id,vm,queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![if render{Command::Render(R_LIGHT)}else{Command::Compute(ComputeCommand{query_access,rt:false})}],
            order:Order{engine:if render{command::RENDER}else{command::COMPUTE},sequence:u64::from(id),wait_through:wait},
            extra_orders:vec![],dependency:dep,resources:Resources,fw_waits:vec![],
        });
        let r_live=mk(1,vm1.clone(),true,Ok(()),false,[None,None]);
        let r_same=mk(2,vm1.clone(),true,Ok(()),false,[None,None]);
        let r_cross_vm=mk(3,vm2.clone(),true,Ok(()),false,[None,None]);
        let r_ext_fence=mk(4,vm1.clone(),true,Err(EAGAIN),false,[None,None]);
        let c_ts_wait=mk(5,vm1.clone(),false,Ok(()),true,[Some(1),None]);
        let mut p=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![QueueBehindFlight{
                job:r_live,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,cstamp:[0;16],
                r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x100;a},fs_target:0x100,
                initbm:false,has_timestamps:true,doomed:false,recovery_suspect:false,
            }]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Not blocked on slots when pass_available is true and render_flights has headroom.
        assert!(!p.blocked_on_slot_headroom(&r_same));
        // Waiting on CPU timestamp publish (query_access) is NOT a slot-headroom block.
        assert!(!p.can_queue_behind(&c_ts_wait));
        assert!(!p.blocked_on_slot_headroom(&c_ts_wait));
        // Render pass slot exhaustion IS a slot-headroom block for ready same-VM and warm cross-VM renders...
        p.slots.pass_available=false;
        assert!(p.blocked_on_slot_headroom(&r_same));
        assert!(p.blocked_on_slot_headroom(&r_cross_vm));
        // ...but NOT for cold InitBM cross-VM, external-fence, or live-InitBM jobs.
        p.pools.block_pipeline_vm=Some(vm2.slot);
        assert!(!p.blocked_on_slot_headroom(&r_cross_vm));
        p.pools.block_pipeline_vm=None;
        assert!(!p.blocked_on_slot_headroom(&r_ext_fence));
        p.pools.can_pipeline=false;
        assert!(!p.blocked_on_slot_headroom(&r_same));
        p.pools.can_pipeline=true;
        p.engine.flights[0].initbm=true;
        assert!(!p.blocked_on_slot_headroom(&r_same));
        p.engine.flights[0].initbm=false;
        // Render flight cap (47) also counts as a slot-headroom block.
        p.slots.pass_available=true;
        p.engine.flights[0].renders=47;
        assert!(p.blocked_on_slot_headroom(&r_same));

        // End-to-end publish_checked: cold InitBM cross-VM drain leaves slot_wait=false;
        // warm render slot block sets slot_wait=true.
        let v1=mk_vm(1);
        let v2=mk_vm(2);
        let mut fc=firmware(vec![job_on_vm(2,v2.clone(),true,0,Ok(()))],vec![]);
        fc.engine.flights.push(super::Flight::new(job_on_vm(1,v1.clone(),true,0,Ok(()))));
        fc.allow_behind=true;
        fc.pools.block_pipeline_vm=Some(v2.slot);
        let mut done=vec![];
        assert_eq!(fc.publish_checked(&mut done),Ok(()));
        assert!(!fc.engine.slot_wait);

        let mut fs=firmware(vec![job_on_vm(2,v1.clone(),true,0,Ok(()))],vec![]);
        fs.engine.flights.push(super::Flight::new(job_on_vm(1,v1,true,0,Ok(()))));
        fs.allow_behind=true;
        fs.block_render_behind=true;
        assert_eq!(fs.publish_checked(&mut done),Ok(()));
        assert!(fs.engine.slot_wait);
    }
    #[test]fn staged_render_barriers_split_ta_and_fs_compute_and_render_waits(){
        const FRAG: u32 = kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS;
        const STAGED: u32 = kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES;
        const NO_C1: [Option<u32>; sizes::COMPUTE_PIPES] = [None; sizes::COMPUTE_PIPES];
        const NO_C: [[Option<u32>; sizes::COMPUTE_PIPES]; 2] = [NO_C1; 2];
        let c8 = |a: Option<u32>, b: Option<u32>| -> [Option<u32>; sizes::COMPUTE_PIPES] {
            let mut arr = [None; sizes::COMPUTE_PIPES];
            arr[0] = a;
            arr[1] = b;
            arr
        };
        // 1. Cold InitBM / empty render pipe (`class_behind = false`): zero render barriers,
        //    compute->FS-only wait routes directly to fs_compute_barrier.
        assert_eq!(
            plan_render_barriers(STAGED, false, true, 0, None, None, [NO_C1, c8(Some(0x700), None)], false),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: false,
                ta_fs_wait: None,
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: c8(Some(0x700), None),
            }
        );
        // 2. Independent multi-pass render (`fs_wait_render = None`, e.g. compress.bw array /
        //    render.shadow or already-retired predecessor): overlaps early while keeping
        //    TA(N-1)->TA(N) `prev_barrier` on the shared PB/UMA epoch, with ZERO FS->TA and
        //    ZERO FS->FS barriers (only own_barrier on FS).
        for flags in [0u32, FRAG, STAGED] {
            assert_eq!(
                plan_render_barriers(flags, true, true, 0x200, None, None, NO_C, false),
                RenderBarrierPlan {
                    early: true,
                    ta_prev_wait: true,
                    ta_fs_wait: None,
                    fs_render_wait: None,
                    ta_compute_wait: NO_C1,
                    fs_compute_wait: NO_C1,
                }
            );
        }
        // 3. Same compute target on TA and FS: only TA waits (FS is ordered via own_barrier);
        //    shared-PB TA->TA `prev_barrier` is kept and dependent FS wait on 0x200 emits fs_dep.
        assert_eq!(
            plan_render_barriers(
                STAGED,
                true,
                true,
                0x200,
                Some(0x200),
                None,
                [c8(Some(0x700), None), c8(Some(0x700), None)],
                false,
            ),
            RenderBarrierPlan {
                early: true,
                ta_prev_wait: true,
                ta_fs_wait: None,
                fs_render_wait: Some(0x200),
                ta_compute_wait: c8(Some(0x700), None),
                fs_compute_wait: NO_C1,
            }
        );
        // 4. Different compute targets on TA (0x600) vs FS (0x700) with no FS render dep:
        //    TA waits on prev_ta and 0x600, FS waits on 0x700, zero FS->FS render barriers.
        assert_eq!(
            plan_render_barriers(
                STAGED,
                true,
                true,
                0x200,
                None,
                None,
                [c8(Some(0x600), None), c8(Some(0x700), None)],
                false,
            ),
            RenderBarrierPlan {
                early: true,
                ta_prev_wait: true,
                ta_fs_wait: None,
                fs_render_wait: None,
                ta_compute_wait: c8(Some(0x600), None),
                fs_compute_wait: c8(Some(0x700), None),
            }
        );
        // 4b. Two-pipe compute waits deduplicate per pipe independently: pipe 0 shared by TA+FS
        //     (0x600) stays on TA only, while pipe 1 FS-only wait (0x700) routes to FS pipe 1.
        assert_eq!(
            plan_render_barriers(
                STAGED,
                true,
                true,
                0x200,
                None,
                None,
                [c8(Some(0x600), None), c8(Some(0x600), Some(0x700))],
                false,
            ),
            RenderBarrierPlan {
                early: true,
                ta_prev_wait: true,
                ta_fs_wait: None,
                fs_render_wait: None,
                ta_compute_wait: c8(Some(0x600), None),
                fs_compute_wait: c8(None, Some(0x700)),
            }
        );
        // 5. Staged render whose TA waits on an older in-flight FS (0x100 != prev_fs 0x200)
        //    while FS depends on 0x200: TA waits on prev_ta and 0x100, FS waits on 0x200.
        assert_eq!(
            plan_render_barriers(
                STAGED,
                true,
                true,
                0x200,
                Some(0x200),
                Some(0x100),
                NO_C,
                false,
            ),
            RenderBarrierPlan {
                early: true,
                ta_prev_wait: true,
                ta_fs_wait: Some(0x100),
                fs_render_wait: Some(0x200),
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 6. Staged render where TA and FS wait on the same older in-flight FS (0x100):
        //    TA still waits on prev_ta for the shared PB, and TA wait dominates FS wait via
        //    own_barrier so fs_render_wait is omitted.
        assert_eq!(
            plan_render_barriers(
                STAGED,
                true,
                true,
                0x200,
                Some(0x100),
                Some(0x100),
                NO_C,
                false,
            ),
            RenderBarrierPlan {
                early: true,
                ta_prev_wait: true,
                ta_fs_wait: Some(0x100),
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 7. Staged render whose TA explicitly waits on the newest in-flight FS (0x200 == prev_fs):
        //    no early overlap; TA waits on 0x200 directly (which dominates prev_ta) and omits both
        //    prev_barrier and fs_dep.
        assert_eq!(
            plan_render_barriers(
                STAGED,
                true,
                true,
                0x200,
                Some(0x200),
                Some(0x200),
                NO_C,
                false,
            ),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: false,
                ta_fs_wait: Some(0x200),
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 8. Dependent FRAGMENT_BARRIERS render (e.g. render.postfx): TA runs early with
        //    shared-PB prev_barrier; FS waits on exact fs_wait_render (0x200).
        assert_eq!(
            plan_render_barriers(
                FRAG,
                true,
                true,
                0x200,
                Some(0x200),
                None,
                [c8(Some(0x700), None), c8(Some(0x700), None)],
                false,
            ),
            RenderBarrierPlan {
                early: true,
                ta_prev_wait: true,
                ta_fs_wait: None,
                fs_render_wait: Some(0x200),
                ta_compute_wait: c8(Some(0x700), None),
                fs_compute_wait: NO_C1,
            }
        );
        // 9. Legacy dependent render (flags = 0, fs_wait_render = Some(0x200)):
        //    TA waits on 0x200 (FS->TA, dominating prev_ta); prev_barrier and fs_dep omitted.
        assert_eq!(
            plan_render_barriers(0, true, true, 0x200, Some(0x200), None, NO_C, false),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: false,
                ta_fs_wait: Some(0x200),
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 10. Legacy dependent render waiting on an older FS (0x100 != prev_fs 0x200):
        //     TA still needs prev_barrier on the shared PB in addition to ta_fs_wait(0x100).
        assert_eq!(
            plan_render_barriers(0, true, true, 0x200, Some(0x100), None, NO_C, false),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: true,
                ta_fs_wait: Some(0x100),
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 11. Conservative `early_tiling = false` serializes TA behind prev_fs even when
        //     fs_wait_render is None, dominating prev_ta and omitting fs_dep.
        assert_eq!(
            plan_render_barriers(FRAG, true, false, 0x200, None, None, NO_C, false),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: false,
                ta_fs_wait: Some(0x200),
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 12. Cross-VM warm render behind another VM's render (`cross_vm_prev = true`, no
        //     own-queue render barrier): TA runs early behind prev_ta (overlapping VM A's FS)
        //     for all flag styles (legacy/FRAG/STAGED) while FS ALWAYS waits on prev_fs (0x200).
        for flags in [0u32, FRAG, STAGED] {
            assert_eq!(
                plan_render_barriers(flags, true, true, 0x200, None, None, NO_C, true),
                RenderBarrierPlan {
                    early: true,
                    ta_prev_wait: true,
                    ta_fs_wait: None,
                    fs_render_wait: Some(0x200),
                    ta_compute_wait: NO_C1,
                    fs_compute_wait: NO_C1,
                }
            );
        }
        // 13. Cross-VM legacy render that also has an own-queue barrier on an older FS (0x100):
        //     TA waits on own older FS (0x100) and prev_ta, while FS still waits on cross-VM
        //     prev_fs (0x200) so two VMs' FS/PB teardowns never overlap.
        assert_eq!(
            plan_render_barriers(0, true, true, 0x200, Some(0x100), None, NO_C, true),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: true,
                ta_fs_wait: Some(0x100),
                fs_render_wait: Some(0x200),
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
        // 14. Cross-VM with `early_tiling = false`: TA already waits on prev_fs, so fs_dep
        //     is dominated and omitted.
        assert_eq!(
            plan_render_barriers(STAGED, true, false, 0x200, None, None, NO_C, true),
            RenderBarrierPlan {
                early: false,
                ta_prev_wait: false,
                ta_fs_wait: Some(0x200),
                fs_render_wait: None,
                ta_compute_wait: NO_C1,
                fs_compute_wait: NO_C1,
            }
        );
    }
    #[test]fn ta_ring_order_replaces_only_the_same_vm_ta_barrier(){
        // A same-VM predecessor with no InitBM on either side: the plan's TA->TA wait is
        // left to the TA ring's order (case 2 above still plans it).
        assert!(ta_ring_orders_prev(false, false));
        // Another VM's predecessor, or an InitBM producer or consumer, keeps prev_barrier.
        assert!(!ta_ring_orders_prev(true, false));
        assert!(!ta_ring_orders_prev(false, true));
        assert!(!ta_ring_orders_prev(true, true));
    }
    #[test]fn two_pipe_concurrent_compute_dispatch_and_fw_timestamp_safety(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let vm2=Arc::new(Vm{id:2,status:Status(AtomicU32::new(0)),slot:2,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,2);
        let q2=Queue::new(0,2);
        let mk=|id:u32,vm:Arc<Vm>,queue:Arc<Queue>,seq:u64|Arc::new(QueueBehindJob{
            id,vm,queue,
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:seq,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let c1=mk(1,vm1.clone(),q1.clone(),1);
        let c2=mk(2,vm1.clone(),q1.clone(),2);
        let c_vm2=mk(3,vm2.clone(),q2.clone(),1);
        let q3=Queue::new(0,2);
        let mut host=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:0,draw:0,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Empty engine starts on pipe 0.
        let no_w = [None; sizes::COMPUTE_PIPES];
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, false), Some(0));
        host.engine.flights.push(QueueBehindFlight{
            job:c1.clone(),group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:1_000_000,target:0x100,
            cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,
            has_timestamps:false,doomed:false,recovery_suspect:false,
        });
        // Cold-start heavy probe: sole single-command compute flight becomes heavy at >= 140 us.
        assert!(!host.compute_queue_is_heavy(&q1, 1_139_999));
        assert!(host.compute_queue_is_heavy(&q1, 1_140_000));
        // Folded multi-command light flight does not trigger the cold-start probe.
        host.engine.flights[0].computes = 3;
        assert!(!host.compute_queue_is_heavy(&q1, 2_000_000));
        host.engine.flights[0].computes = 1;
        // The retired EWMA crosses over at the probe's 140 us, after the probe has expired.
        q1.note_compute_us(139);
        assert!(!host.compute_queue_is_heavy(&q1, 1_050_000));
        q1.note_compute_us(141);
        assert!(host.compute_queue_is_heavy(&q1, 1_050_000));
        q1.compute_ewma_us.store(0, Ordering::Relaxed);
        // The EWMA follows subsequent changes in the stream.
        q1.note_compute_us(30);
        assert!(!host.compute_queue_is_heavy(&q1, 1_050_000));
        q1.note_compute_us(300);
        assert!(host.compute_queue_is_heavy(&q1, 1_050_000));
        q1.note_compute_us(20);
        assert!(!host.compute_queue_is_heavy(&q1, 1_050_000));
        q1.compute_ewma_us.store(0, Ordering::Relaxed);
        // Light single-queue barrier-free compute stays affine to its active pipe (pipe 0),
        // while heavy single-queue compute and a second independent queue balance onto idle pipe 1.
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, false), Some(0));
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, true), Some(1));
        assert_eq!(host.choose_compute_pipe(&q2, &vm1, no_w, (1u32 << 24) - 1, false, false), Some(1));
        // Cross-VM compute leases its own disjoint pipe (never shares VM1's active pipe 0).
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, (1u32 << 24) - 1, false, false), Some(1));
        // Base-only VM2 (only units 0..2, cannot use secondary pipes) waits when pipe 0 is busy by VM1.
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, 0b11, false, false), None);
        assert!(!host.has_compute_pipe_for_vm(&vm2, 0b11, false));
        // RT/SCS compute on same VM stays pinned to pipe 0; other VM RT returns None while pipe 0 busy.
        assert_eq!(host.choose_compute_pipe(&q2, &vm1, no_w, (1u32 << 24) - 1, true, true), Some(0));
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, (1u32 << 24) - 1, true, true), None);
        // A 2-unit base-only or SCS same-VM stays pinned to pipe 0.
        assert_eq!(host.choose_compute_pipe(&q2, &vm1, no_w, 0b11, false, true), Some(0));
        assert_eq!(host.choose_compute_pipe(&q2, &vm1, no_w, 0b110, false, true), Some(0));
        assert!(host.can_queue_behind(&c2));
        // Add second concurrent flight on pipe 1 from VM2 (q2): VM1 stays on pipe 0 (or expands to
        // pipe 2 for heavy compute, capped at MAX_VM_COMPUTE_PIPES=2), VM2 stays on pipe 1.
        host.engine.flights.push(QueueBehindFlight{
            job:c_vm2.clone(),group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:1,rt_compute:false,heavy_render:false,published_ns:1_000_000,target:0x100,
            cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,
            has_timestamps:false,doomed:false,recovery_suspect:false,
        });
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, false), Some(0));
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, (1u32 << 24) - 1, false, false), Some(1));
        // Heavy VM1 picks idle pipe 2 (never VM2's active pipe 1).
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, true), Some(2));
        assert_eq!(host.choose_compute_pipe(&q3, &vm1, no_w, (1u32 << 24) - 1, false, false), Some(2));
        // Once VM1 has 2 active pipes (0 and 2), MAX_VM_COMPUTE_PIPES=2 keeps VM1 on {0, 2}.
        host.engine.flights.push(QueueBehindFlight{
            job:c2.clone(),group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:2,rt_compute:false,heavy_render:false,published_ns:1_000_000,target:0x100,
            cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,
            has_timestamps:false,doomed:false,recovery_suspect:false,
        });
        host.engine.next_compute_pipe = 2;
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, true), Some(2));
        host.engine.next_compute_pipe = 0;
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, true), Some(0));
        // Cross-VM non-SCS compute queues on disjoint pipes without draining.
        assert!(host.can_queue_behind(&c_vm2));
        assert!(host.can_queue_behind(&c2));
        // When all COMPUTE_PIPES are occupied by distinct VMs, a 9th VM has no free pipe.
        host.engine.flights.clear();
        for p in 0..sizes::COMPUTE_PIPES {
            let vp = mk_vm((p + 10) as u32);
            let qp = Queue::new(0, 2);
            let jp = mk((p + 10) as u32, vp, qp, 1);
            host.engine.flights.push(QueueBehindFlight{
                job:jp,group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:p as u8,rt_compute:false,heavy_render:false,published_ns:1_000_000,target:0x100,
                cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,
                has_timestamps:false,doomed:false,recovery_suspect:false,
            });
        }
        assert!(!host.can_queue_behind(&c_vm2));
        assert!(host.blocked_on_slot_headroom(&c_vm2));
        assert!(!host.needs_engine_drain(&c_vm2));
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, (1u32 << 24) - 1, false, false), None);
        // Starvation handoff: when all 8 pipes are occupied by other VMs, compute_handoff_pipe
        // picks one compatible pipe (0) to drain so the remaining 7 pipes keep dispatching while
        // pipe 0 drains for the waiting 9th VM.
        assert_eq!(host.compute_handoff_pipe(&c_vm2), Some(0));
        host.engine.handoff_avoid_pipes = 1u32 << 0;
        let vm10 = host.engine.flights[0].job.vm.clone();
        let q10 = host.engine.flights[0].job.queue.clone();
        let vm11 = host.engine.flights[1].job.vm.clone();
        let q11 = host.engine.flights[1].job.queue.clone();
        // Pipe 0 incumbent stops refilling pipe 0 while pipe 1 incumbent keeps dispatching on pipe 1.
        assert_eq!(host.choose_compute_pipe(&q10, &vm10, no_w, (1u32 << 24) - 1, false, false), None);
        assert_eq!(host.choose_compute_pipe(&q11, &vm11, no_w, (1u32 << 24) - 1, false, false), Some(1));
        // Base-only VM2 (only units 0..2, limited to pipe 0) waiting while extension VM1 refills
        // pipe 0: avoiding pipe 0 moves VM1 onto pipe 1 while pipe 0 drains, then VM2 gets pipe 0!
        host.engine.handoff_avoid_pipes = 0;
        host.engine.flights.clear();
        host.engine.flights.push(QueueBehindFlight{
            job:c1.clone(),group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:1_000_000,target:0x100,
            cstamp:[0x100;16],r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,initbm:false,
            has_timestamps:false,doomed:false,recovery_suspect:false,
        });
        // Base-only mask 0b11 cannot use pipes 1..7, so VM2 has no pipe while VM1 holds pipe 0.
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, 0b11, false, false), None);
        host.engine.handoff_avoid_pipes = 1u32 << 0;
        // Extension VM1 continues dispatching on secondary pipe 1 instead of refilling pipe 0.
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, (1u32 << 24) - 1, false, false), Some(1));
        // A base-only VM1 incumbent stops refilling pipe 0 so pipe 0 drains for handoff.
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, no_w, 0b11, false, false), None);
        assert!(!host.has_compute_pipe_for_vm(&vm1, 0b11, false));
        // Once pipe 0 drains (VM1 moved to pipe 1), clearing handoff_avoid_pipes gives VM2 pipe 0.
        host.engine.flights[0].compute_pipe = 1;
        host.engine.handoff_avoid_pipes = 0;
        assert_eq!(host.choose_compute_pipe(&q2, &vm2, no_w, 0b11, false, false), Some(0));
    }
    #[test]fn two_pipe_compute_barriers_reuse_single_pipe_and_serialize_across_both_pipes(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,2);
        let mk=|id:u32,seq:u64,wait_c:Option<u64>|Arc::new(QueueBehindJob{
            id,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:seq,wait_through:[None,wait_c]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let c1=mk(1,1,None);
        let c2=mk(2,2,None);
        let c3=mk(3,3,Some(1));
        let c4=mk(4,4,Some(2));
        let mut host=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:2,draw:0,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![
                QueueBehindFlight{
                    job:c1,group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x100,
                    cstamp:{let mut a=[0;16];a[0]=0x100;a},r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,
                    initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
                },
                QueueBehindFlight{
                    job:c2,group:vec![],first:0,count:1,computes:2,renders:0,compute_pipe:1,rt_compute:false,heavy_render:false,published_ns:0,target:0x200,
                    cstamp:{let mut a=[0;16];a[0]=0x200;a},r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,
                    initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
                },
            ]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Barrier-free compute resolves all None.
        let mut exp1 = [None; sizes::COMPUTE_PIPES];
        exp1[0] = Some(WaitTarget{stamp:0x100,initbm:false});
        let mut exp2 = exp1;
        exp2[1] = Some(WaitTarget{stamp:0x200,initbm:false});
        assert_eq!(host.wait_compute_stamps_for(&c3, None), Ok([None; sizes::COMPUTE_PIPES]));
        // Waiting on seq 1 (pipe 0 only) returns [Some(0x100), None..] and reuses pipe 0 (zero c2c barrier).
        let w1 = host.wait_compute_stamps_for(&c3, Some(1)).unwrap();
        assert_eq!(w1, exp1);
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, w1, (1u32 << 24) - 1, false, true), Some(0));
        // Waiting on seq 2 (both pipe 0 seq 1 and pipe 1 seq 2 <= 2) returns both stamps and
        // selects the least-loaded pipe (pipe 0 has 1 active vs pipe 1 has 2 -> picks pipe 0,
        // so prefix_compute emits one cross-pipe c2c barrier on pipe 1's 0x200).
        let w2 = host.wait_compute_stamps_for(&c4, Some(2)).unwrap();
        assert_eq!(w2, exp2);
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, w2, (1u32 << 24) - 1, false, false), Some(0));
        // Unpublished seq 3 returns Err(EAGAIN).
        assert_eq!(host.wait_compute_stamps_for(&c4, Some(3)), Err(EAGAIN));
        // If seq 2 retired out-of-order on pipe 1 while seq 1 is still active on pipe 0,
        // compute_ooo bit 0 marks seq 2 retired so wait_compute_stamps_for(Some(2)) succeeds
        // and still waits on pipe 0's active seq 1 (0x100)!
        host.engine.flights.remove(1);
        q1.compute_ooo.store(1u64 << 0, Ordering::Release);
        assert!(q1.is_compute_retired(2));
        let w2_ooo = host.wait_compute_stamps_for(&c4, Some(2)).unwrap();
        assert_eq!(w2_ooo, exp1);
        assert_eq!(host.choose_compute_pipe(&q1, &vm1, w2_ooo, (1u32 << 24) - 1, false, true), Some(0));
        // Once contiguous completed[COMPUTE] reaches 2, wait_compute_stamps_for returns all None.
        q1.completed[command::COMPUTE].store(2, Ordering::Release);
        q1.compute_ooo.store(0, Ordering::Release);
        assert_eq!(host.wait_compute_stamps_for(&c4, Some(2)), Ok([None; sizes::COMPUTE_PIPES]));
    }
    #[test]fn cross_queue_fw_waits_resolve_stamps_reuse_compute_pipe_and_guard_timestamps(){
        let vm1=Arc::new(Vm{id:1,status:Status(AtomicU32::new(0)),slot:1,msaa:AtomicBool::new(false)});
        let q1=Queue::new(0,2);
        let q2=Queue::new(0,2);
        let c1=Arc::new(QueueBehindJob{
            id:1,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let r1=Arc::new(QueueBehindJob{
            id:2,vm:vm1.clone(),queue:q1.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Render(R_LIGHT)],
            order:Order{engine:command::RENDER,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,fw_waits:vec![],
        });
        let c2_wait_q1=Arc::new(QueueBehindJob{
            id:3,vm:vm1.clone(),queue:q2.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:false,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:1,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
            fw_waits:vec![fence::FwWait{queue:q1.clone(),last_ordinal:[Some(1),Some(1)]}],
        });
        let c2_query_wait_q1=Arc::new(QueueBehindJob{
            id:4,vm:vm1.clone(),queue:q2.clone(),
            progress:Progress{sent:AtomicU32::new(0),count:1,pending:AtomicBool::new(true)},
            commands:vec![Command::Compute(ComputeCommand{query_access:true,rt:false})],
            order:Order{engine:command::COMPUTE,sequence:2,wait_through:[None,None]},
            extra_orders:vec![],dependency:Ok(()),resources:Resources,
            fw_waits:vec![fence::FwWait{queue:q1.clone(),last_ordinal:[None,Some(1)]}],
        });
        let host=QueueBehindHost{
            graph:QueueBehindGraph::new(),
            engine:QueueBehindEngine{ordinal:1,draw:1,ta_target:0,next_compute_pipe:0,pipe_last_vm:[None;hw::sizes::COMPUTE_PIPES],pipe_last_used:[0;hw::sizes::COMPUTE_PIPES],handoff_avoid_pipes:0,flights:vec![
                QueueBehindFlight{
                    job:c1,group:vec![],first:0,count:1,computes:1,renders:0,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0x100,
                    cstamp:{let mut a=[0;16];a[0]=0x100;a},r_ta_stamp:[0;16],r_fs_stamp:[0;16],fs_target:0,
                    initbm:false,has_timestamps:true,doomed:false,recovery_suspect:false,
                },
                QueueBehindFlight{
                    job:r1,group:vec![],first:0,count:1,computes:0,renders:1,compute_pipe:0,rt_compute:false,heavy_render:false,published_ns:0,target:0,
                    cstamp:[0;16],r_ta_stamp:[0;16],r_fs_stamp:{let mut a=[0;16];a[0]=0x200;a},fs_target:0x200,
                    initbm:false,has_timestamps:false,doomed:false,recovery_suspect:false,
                },
            ]},
            slots:QueueBehindSlots{compute_exhausted:false,render_exhausted:false,compute_available:true,pass_available:true},
            pools:QueueBehindPools{exhausted:false,can_pipeline:true,block_pipeline_vm:None},
        };
        // Cross-queue compute wait resolves pipe 0 stamp 0x100 and keeps q2 on pipe 0 (zero c2c).
        let mut exp_cw = [None; sizes::COMPUTE_PIPES];
        exp_cw[0] = Some(WaitTarget{stamp:0x100,initbm:false});
        let cw=host.wait_compute_stamps_for(&c2_wait_q1,None).unwrap();
        assert_eq!(cw,exp_cw);
        assert_eq!(host.choose_compute_pipe(&q2,&vm1,cw,(1u32<<24)-1,false,false),Some(0));
        // Cross-queue render wait resolves q1's FS stamp 0x200.
        assert_eq!(host.wait_stamp_for(&c2_wait_q1,command::RENDER,None),Ok(Some(WaitTarget{stamp:0x200,initbm:false})));
        // query_access compute waiting via fw_waits on a timestamped q1 flight must wait to retire;
        // non-query compute queues directly behind q1 in firmware.
        assert!(!host.can_queue_behind(&c2_query_wait_q1));
        assert!(host.can_queue_behind(&c2_wait_q1));
        // Once q1 retires, fw_waits resolves to no barriers.
        q1.completed[command::COMPUTE].store(1,Ordering::Release);
        q1.completed[command::RENDER].store(1,Ordering::Release);
        assert_eq!(host.wait_compute_stamps_for(&c2_wait_q1,None),Ok([None;sizes::COMPUTE_PIPES]));
        assert_eq!(host.wait_stamp_for(&c2_wait_q1,command::RENDER,None),Ok(None));
    }
}
#[allow(non_camel_case_types, deprecated)]
mod queue_limits_scope {
    use std::sync::Arc;
    #[derive(Copy, Clone, Debug, PartialEq, Eq)]
    struct Error(i32);
    const ENOSPC: Error = Error(28);
    type Result<T = ()> = std::result::Result<T, Error>;
    mod uapi {
        pub const DRM_ASAHI_FEATURE_INDEPENDENT_QUEUES: u64 = 1 << 2;
        #[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
        pub struct drm_asahi_queue_limits {
            pub max_queues: u32,
            pub max_in_flight_per_queue: u32,
            pub flags: u32,
            pub pad: u32,
        }
    }
    mod hw {
        pub mod sizes {
            __FLIGHTS_LINE__
            __MAX_QUEUES_LINE__
        }
    }
    mod params {
        use super::{hw, uapi};
        __QUEUE_LIMITS_FN__
    }
    struct Gpu;
    impl Gpu { __GPU_QUEUE_LIMITS_FN__ }
    __QUEUE_CREDIT_STRUCT__
    __QUEUE_CREDIT_IMPL__
    __QUEUE_CREDIT_DROP__
    #[test]
    fn queue_limits_and_independent_queues_feature_match_g16_bounds() {
        let gpu = Gpu;
        let limits = gpu.queue_limits().expect("G16 must expose queue_limits");
        assert_eq!(limits.max_queues, 58);
        assert_eq!(limits.max_in_flight_per_queue, 32);
        assert_eq!(limits.flags, 0);
        assert_eq!(limits.pad, 0);
        let mut features = 0u64;
        if gpu.queue_limits().is_some() {
            features |= uapi::DRM_ASAHI_FEATURE_INDEPENDENT_QUEUES;
        }
        assert_ne!(features & uapi::DRM_ASAHI_FEATURE_INDEPENDENT_QUEUES, 0);
    }
    #[test]
    fn queue_credit_enforces_per_file_max_queues_without_starving_other_clients() {
        let client_a = Arc::new(crate::usage::Client::new());
        let client_b = Arc::new(crate::usage::Client::new());
        let mut held_a = Vec::new();
        for i in 0..hw::sizes::MAX_QUEUES {
            held_a.push(QueueCredit::acquire(&client_a).expect("credit within MAX_QUEUES"));
            assert_eq!(client_a.queues(), i + 1);
        }
        assert_eq!(QueueCredit::acquire(&client_a).err(), Some(ENOSPC));
        assert_eq!(client_a.queues(), hw::sizes::MAX_QUEUES);
        // A second DRM file (e.g. a compositor) keeps its own queue budget.
        let compositor = QueueCredit::acquire(&client_b)
            .expect("client A hitting MAX_QUEUES must not starve client B");
        assert_eq!(client_b.queues(), 1);
        drop(held_a.pop());
        assert_eq!(client_a.queues(), hw::sizes::MAX_QUEUES - 1);
        let reused = QueueCredit::acquire(&client_a).expect("dropped credit reusable");
        assert_eq!(QueueCredit::acquire(&client_a).err(), Some(ENOSPC));
        drop(compositor);
        assert_eq!(client_b.queues(), 0);
        drop(reused);
        drop(held_a);
        assert_eq!(client_a.queues(), 0);
    }
}
mod compute_horizon_scope {
    use super::{Error, Result, EAGAIN, now_ns};
    const ENOENT: Error = Error(2);
    const ERANGE: Error = Error(34);
    use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
    mod command { pub const RENDER: usize = 0; pub const COMPUTE: usize = 1; }
    #[derive(Copy, Clone)]
    struct Order { engine: usize, sequence: u64, wait_through: [Option<u64>; 2] }
    impl Order {
        fn ready(&self, completed: [u64; 2], published: [u64; 2]) -> bool {
            let mut i = 0;
            while i < 2 {
                if let Some(last) = self.wait_through[i] {
                    if completed[i].max(published[i]) < last {
                        return false;
                    }
                }
                i += 1;
            }
            true
        }
    }
    #[derive(Copy, Clone)]
    enum Command { Compute(()), Render(()) }
    mod fence {
        use super::{Result, EAGAIN};
        pub fn check_dependencies(inputs: &[bool]) -> Result {
            if inputs.iter().all(|&ready| ready) { Ok(()) } else { Err(EAGAIN) }
        }
    }
    struct Status(AtomicU32);
    impl Status { fn get(&self) -> u32 { self.0.load(Ordering::Relaxed) } }
    struct Vm { status: Status }
    struct Queue {
        cancelled_compute: u64,
        completed: [AtomicU64; 2],
        published: [AtomicU64; 2],
    }
    impl Queue {
        fn check(&self, order: &Order) -> Result {
            if self.cancelled_compute != 0 && order.sequence >= self.cancelled_compute {
                Err(Error(125))
            } else {
                Ok(())
            }
        }
    }
    struct Progress { sent: u32, count: u32 }
    impl Progress {
        fn sent(&self) -> u32 { self.sent }
        fn count(&self) -> u32 { self.count }
    }
    struct Job {
        vm: Vm,
        queue: Queue,
        progress: Progress,
        commands: Vec<Command>,
        orders: Vec<Order>,
        inputs: Vec<bool>,
        fw_waits: Vec<()>,
        terminal_retired: AtomicBool,
    }
    impl Job {
        __CAN_FOLD_CMD__
        __CAN_FOLD_JOB__
        __CAN_FOLD_RENDER_CMD__
        __CAN_FOLD_RENDER_JOB__
    }
    #[test]
    fn compute_horizon_eligibility_rejects_barriers_renders_unready_fences_and_gaps() {
        let mk = |cmds: Vec<Command>, orders: Vec<Order>, inputs: Vec<bool>, cancelled: u64| Job {
            vm: Vm { status: Status(AtomicU32::new(0)) },
            queue: Queue {
                cancelled_compute: cancelled,
                completed: [AtomicU64::new(0), AtomicU64::new(0)],
                published: [AtomicU64::new(0), AtomicU64::new(0)],
            },
            progress: Progress { sent: 0, count: cmds.len() as u32 },
            commands: cmds,
            orders,
            inputs,
            fw_waits: vec![],
            terminal_retired: AtomicBool::new(false),
        };
        let ok_job = mk(
            vec![Command::Compute(()), Command::Compute(())],
            vec![
                Order { engine: command::COMPUTE, sequence: 2, wait_through: [None, None] },
                Order { engine: command::COMPUTE, sequence: 3, wait_through: [None, None] },
            ],
            vec![true],
            0,
        );
        assert!(ok_job.can_fold_horizon_cmd(0, 2));
        assert!(ok_job.can_fold_horizon_cmd(1, 3));
        assert!(!ok_job.can_fold_horizon_cmd(0, 5));
        assert!(ok_job.can_fold_horizon_job(2));
        assert!(!ok_job.can_fold_horizon_job(3));

        // Render barrier or compute barrier breaks the batch.
        let r_bar = mk(
            vec![Command::Compute(())],
            vec![Order { engine: command::COMPUTE, sequence: 2, wait_through: [Some(1), None] }],
            vec![],
            0,
        );
        assert!(!r_bar.can_fold_horizon_job(2));
        let c_bar = mk(
            vec![Command::Compute(())],
            vec![Order { engine: command::COMPUTE, sequence: 2, wait_through: [None, Some(1)] }],
            vec![],
            0,
        );
        assert!(!c_bar.can_fold_horizon_job(2));

        // Render interleaved breaks the batch.
        let mixed = mk(
            vec![Command::Compute(()), Command::Render(())],
            vec![
                Order { engine: command::COMPUTE, sequence: 2, wait_through: [None, None] },
                Order { engine: command::RENDER, sequence: 1, wait_through: [None, None] },
            ],
            vec![],
            0,
        );
        assert!(!mixed.can_fold_horizon_job(2));

        // Pending input fence or cancelled queue breaks the batch.
        let unready = mk(
            vec![Command::Compute(())],
            vec![Order { engine: command::COMPUTE, sequence: 2, wait_through: [None, None] }],
            vec![false],
            0,
        );
        assert!(!unready.can_fold_horizon_job(2));
        let cancelled = mk(
            vec![Command::Compute(())],
            vec![Order { engine: command::COMPUTE, sequence: 2, wait_through: [None, None] }],
            vec![],
            2,
        );
        assert!(!cancelled.can_fold_horizon_job(2));
        let mut with_fw_wait = mk(
            vec![Command::Compute(())],
            vec![Order { engine: command::COMPUTE, sequence: 2, wait_through: [None, None] }],
            vec![],
            0,
        );
        with_fw_wait.fw_waits.push(());
        assert!(!with_fw_wait.can_fold_horizon_cmd(0, 2));
        assert!(!with_fw_wait.can_fold_horizon_job(2));
    }

    #[test]
    fn can_fold_render_cmd_accepts_same_batch_render_dep_and_rejects_unready_compute() {
        let mk = |cmds: Vec<Command>, orders: Vec<Order>, inputs: Vec<bool>, pub_c: u64| Job {
            vm: Vm { status: Status(AtomicU32::new(0)) },
            queue: Queue {
                cancelled_compute: 0,
                completed: [AtomicU64::new(0), AtomicU64::new(0)],
                published: [AtomicU64::new(0), AtomicU64::new(pub_c)],
            },
            progress: Progress { sent: 0, count: cmds.len() as u32 },
            commands: cmds,
            orders,
            inputs,
            fw_waits: vec![],
            terminal_retired: AtomicBool::new(false),
        };
        let multi = mk(
            vec![Command::Render(()), Command::Render(()), Command::Render(())],
            vec![
                Order { engine: command::RENDER, sequence: 1, wait_through: [None, None] },
                Order { engine: command::RENDER, sequence: 2, wait_through: [Some(1), None] },
                Order { engine: command::RENDER, sequence: 3, wait_through: [Some(2), Some(5)] },
            ],
            vec![true],
            4,
        );
        // Pass 1 waits on pass 0 (not yet globally published): foldable at expected=2.
        assert!(multi.can_fold_render_cmd(1, 2));
        // Wrong sequence is rejected.
        assert!(!multi.can_fold_render_cmd(1, 3));
        // Pass 2 waits on unpublished compute 5 (published=4): rejected until compute 5 publishes.
        assert!(!multi.can_fold_render_cmd(2, 3));
        multi.queue.published[command::COMPUTE].store(5, Ordering::Release);
        assert!(multi.can_fold_render_cmd(2, 3));
        // Unready input fence blocks folding.
        let unready = mk(
            vec![Command::Render(()), Command::Render(())],
            vec![
                Order { engine: command::RENDER, sequence: 1, wait_through: [None, None] },
                Order { engine: command::RENDER, sequence: 2, wait_through: [Some(1), None] },
            ],
            vec![false],
            0,
        );
        assert!(!unready.can_fold_render_cmd(1, 2));
    }

    #[test]
    fn can_fold_render_horizon_job_accepts_render_only_jobs_and_rejects_mixed_or_unready() {
        let mk = |cmds: Vec<Command>, orders: Vec<Order>, inputs: Vec<bool>, pub_c: u64| Job {
            vm: Vm { status: Status(AtomicU32::new(0)) },
            queue: Queue {
                cancelled_compute: 0,
                completed: [AtomicU64::new(0), AtomicU64::new(0)],
                published: [AtomicU64::new(0), AtomicU64::new(pub_c)],
            },
            progress: Progress { sent: 0, count: cmds.len() as u32 },
            commands: cmds,
            orders,
            inputs,
            fw_waits: vec![],
            terminal_retired: AtomicBool::new(false),
        };
        // Two-pass render job waiting on primary flight pass 1 and its own pass 2.
        let r_job = mk(
            vec![Command::Render(()), Command::Render(())],
            vec![
                Order { engine: command::RENDER, sequence: 2, wait_through: [Some(1), None] },
                Order { engine: command::RENDER, sequence: 3, wait_through: [Some(2), Some(5)] },
            ],
            vec![true],
            4,
        );
        // Compute dep 5 not yet published (published=4) -> rejected.
        assert!(!r_job.can_fold_render_horizon_job(2));
        r_job.queue.published[command::COMPUTE].store(5, Ordering::Release);
        assert!(r_job.can_fold_render_horizon_job(2));
        // Wrong sequence is rejected.
        assert!(!r_job.can_fold_render_horizon_job(3));
        // Mixed compute+render job is rejected.
        let mixed = mk(
            vec![Command::Render(()), Command::Compute(())],
            vec![
                Order { engine: command::RENDER, sequence: 2, wait_through: [None, None] },
                Order { engine: command::COMPUTE, sequence: 1, wait_through: [None, None] },
            ],
            vec![true],
            5,
        );
        assert!(!mixed.can_fold_render_horizon_job(2));
        // Pending input fence is rejected.
        let unready = mk(
            vec![Command::Render(())],
            vec![Order { engine: command::RENDER, sequence: 2, wait_through: [Some(1), None] }],
            vec![false],
            5,
        );
        assert!(!unready.can_fold_render_horizon_job(2));
    }

    use crate::hw::sizes;
    mod recovery {
        pub(crate) mod logic {
            pub(crate) use crate::recovery::logic::{stamp_reached, RetiredStamps};
        }
    }
    mod poll {
        pub fn stamp_reached(obs: u32, own: u32, _tail: u32) -> bool { obs >= own }
    }
    mod work {
        pub mod ta { pub const TPC_BYTES: usize = 0; }
        pub mod compute {
            pub const fn pipe_of_event(event: u32) -> Option<usize> {
                if event >= 15 && event < 15 + super::super::sizes::COMPUTE_PIPES as u32 {
                    Some((event - 15) as usize)
                } else {
                    None
                }
            }
        }
    }
    fn wmb() {}
    struct PassBuf {
        size: usize,
        tpc_bytes: u64,
        cleared: std::cell::Cell<usize>,
    }
    impl PassBuf {
        fn r64(&self, _o: usize) -> Result<u64> { Ok(self.tpc_bytes) }
        fn fill(&self, _o: usize, _v: u8, n: usize) -> Result {
            self.cleared.set(self.cleared.get() + n);
            Ok(())
        }
    }
    struct Pass { ta_work: PassBuf, tpc: PassBuf }
    struct Resources { passes: Vec<Pass> }
    impl Resources {
        fn render_pass(&self, k: usize) -> Result<&Pass> {
            self.passes.get(k).ok_or(ENOENT)
        }
    }
    struct RecVm { slot: u32 }
    struct RecJob { vm: RecVm, resources: Resources }
    struct RecClaims { pass: [u32; 16] }
    struct RecFlight {
        job: RecJob,
        claims: RecClaims,
        computes: u32,
        renders: u32,
        compute_pipe: u8,
        target: u32,
        ta_target: u32,
        fs_target: u32,
        cstamp: [u32; 16],
        r_ta_stamp: [u32; 16],
        r_fs_stamp: [u32; 16],
        doomed: bool,
        recovery_suspect: bool,
        recovery_started_ns: u64,
        gate_passed_ns: Option<u64>,
        unsettled_polls: u32,
    }
    struct RecEngine { flights: Vec<RecFlight>, target: [u32; sizes::COMPUTE_PIPES], ta_target: u32, fs_target: u32 }
    struct RecState { last_retired: recovery::logic::RetiredStamps }
    struct RecFirmware { engine: RecEngine, recovery: RecState }
    fn st(ta: u32, fs: u32, c0: u32, c1: u32) -> [u32; 2 + sizes::COMPUTE_PIPES] {
        let mut s = [0u32; 2 + sizes::COMPUTE_PIPES];
        s[0] = ta;
        s[1] = fs;
        s[2] = c0;
        s[3] = c1;
        s
    }
    impl RecFirmware {
        __RECOVERY_PREPARE__
        __RECOVERY_PREPARE_SUSPECTS__
    }
    #[test]
    fn recovery_prepare_matches_interior_cstamp_in_batched_compute_horizon() {
        let mut cstamp = [0u32; 16];
        cstamp[0] = 0x100;
        cstamp[1] = 0x200;
        cstamp[2] = 0x300;
        let mut target = [0u32; sizes::COMPUTE_PIPES];
        target[0] = 0x300;
        let mut fw = RecFirmware {
            engine: RecEngine {
                flights: vec![RecFlight {
                    job: RecJob { vm: RecVm { slot: 1 }, resources: Resources { passes: vec![] } },
                    claims: RecClaims { pass: [0; 16] },
                    computes: 3,
                    renders: 0,
                    compute_pipe: 0,
                    target: 0x300,
                    ta_target: 0,
                    fs_target: 0,
                    cstamp,
                    r_ta_stamp: [0; 16],
                    r_fs_stamp: [0; 16],
                    doomed: false,
                    recovery_suspect: false,
                    recovery_started_ns: 0,
                    gate_passed_ns: None,
                    unsettled_polls: 0,
                }],
                target,
                ta_target: 0,
                fs_target: 0,
            },
            recovery: RecState { last_retired: recovery::logic::RetiredStamps::new() },
        };
        assert_eq!(fw.recovery_prepare(1, 15, 0x200, st(0, 0, 0x100, 0)), Ok(()));
        assert!(fw.engine.flights[0].doomed);
        assert!(fw.engine.flights[0].recovery_suspect);
        assert_eq!(fw.recovery_prepare(1, 15, 0x400, st(0, 0, 0x100, 0)), Err(ENOENT));
    }
    #[test]
    fn recovery_prepare_matches_interior_render_stamps_and_cleans_only_unfinished_tpc() {
        let mk_pass = |bytes: u64| Pass {
            ta_work: PassBuf { size: 0x1000, tpc_bytes: bytes, cleared: std::cell::Cell::new(0) },
            tpc: PassBuf { size: 0x10000, tpc_bytes: 0, cleared: std::cell::Cell::new(0) },
        };
        let mut r_ta = [0u32; 16];
        let mut r_fs = [0u32; 16];
        r_ta[0] = 0x100; r_ta[1] = 0x200; r_ta[2] = 0x300;
        r_fs[0] = 0x100; r_fs[1] = 0x200; r_fs[2] = 0x300;
        let mut pass = [0u32; 16];
        pass[0] = 0; pass[1] = 1; pass[2] = 2;
        let mut fw = RecFirmware {
            engine: RecEngine {
                flights: vec![RecFlight {
                    job: RecJob {
                        vm: RecVm { slot: 1 },
                        resources: Resources {
                            passes: vec![mk_pass(0x1000), mk_pass(0x2000), mk_pass(0x3000)],
                        },
                    },
                    claims: RecClaims { pass },
                    computes: 0,
                    renders: 3,
                    compute_pipe: 0,
                    target: 0,
                    ta_target: 0x300,
                    fs_target: 0x300,
                    cstamp: [0; 16],
                    r_ta_stamp: r_ta,
                    r_fs_stamp: r_fs,
                    doomed: false,
                    recovery_suspect: false,
                    recovery_started_ns: 0,
                    gate_passed_ns: None,
                    unsettled_polls: 0,
                }],
                target: [0; sizes::COMPUTE_PIPES],
                ta_target: 0x300,
                fs_target: 0x300,
            },
            recovery: RecState { last_retired: recovery::logic::RetiredStamps::new() },
        };
        // Pass 0 TA finished (stage[0] == 0x100); interior TA target 0x200 matches and cleans
        // TPC only for unfinished passes 1 and 2.
        assert_eq!(fw.recovery_prepare(9, 0x70, 0x200, st(0x100, 0, 0, 0)), Ok(()));
        assert!(fw.engine.flights[0].doomed);
        assert_eq!(fw.engine.flights[0].job.resources.passes[0].tpc.cleared.get(), 0);
        assert_eq!(fw.engine.flights[0].job.resources.passes[1].tpc.cleared.get(), 0x2000);
        assert_eq!(fw.engine.flights[0].job.resources.passes[2].tpc.cleared.get(), 0x3000);
        // Interior FS target 0x200 also matches; unrelated target 0x400 returns ENOENT.
        assert_eq!(fw.recovery_prepare(1, 0x71, 0x200, st(0x300, 0x100, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare(1, 0x71, 0x400, st(0x300, 0x100, 0, 0)), Err(ENOENT));
    }
    #[test]
    fn cross_vm_recovery_prepare_isolates_blame_to_faulting_vm() {
        let mut r_ta = [0u32; 16];
        let mut r_fs = [0u32; 16];
        r_ta[0] = 0x100; r_fs[0] = 0x100;
        let mut cstamp = [0u32; 16];
        cstamp[0] = 0x500;
        let mut target = [0u32; sizes::COMPUTE_PIPES];
        target[0] = 0x500;
        let mut fw = RecFirmware {
            engine: RecEngine {
                flights: vec![
                    RecFlight {
                        job: RecJob { vm: RecVm { slot: 1 }, resources: Resources { passes: vec![] } },
                        claims: RecClaims { pass: [0; 16] },
                        computes: 0,
                        renders: 1,
                        compute_pipe: 0,
                        target: 0,
                        ta_target: 0x100,
                        fs_target: 0x100,
                        cstamp: [0; 16],
                        r_ta_stamp: r_ta,
                        r_fs_stamp: r_fs,
                        doomed: false,
                        recovery_suspect: false,
                        recovery_started_ns: 0,
                        gate_passed_ns: None,
                        unsettled_polls: 0,
                    },
                    RecFlight {
                        job: RecJob { vm: RecVm { slot: 2 }, resources: Resources { passes: vec![] } },
                        claims: RecClaims { pass: [0; 16] },
                        computes: 1,
                        renders: 0,
                        compute_pipe: 0,
                        target: 0x500,
                        ta_target: 0,
                        fs_target: 0,
                        cstamp,
                        r_ta_stamp: [0; 16],
                        r_fs_stamp: [0; 16],
                        doomed: false,
                        recovery_suspect: false,
                        recovery_started_ns: 0,
                        gate_passed_ns: None,
                        unsettled_polls: 0,
                    },
                ],
                target,
                ta_target: 0x100,
                fs_target: 0x100,
            },
            recovery: RecState { last_retired: recovery::logic::RetiredStamps::new() },
        };
        // VM 1 (ASID 1) faults on FS 0x100 while VM 2 (ASID 2) has co-resident compute 0x500.
        assert_eq!(fw.recovery_prepare(1, 0x71, 0x100, st(0x100, 0, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare_suspects(st(0x100, 0, 0, 0), 0x71, Some(1)), Ok(()));
        assert!(fw.engine.flights[0].doomed);
        assert!(fw.engine.flights[0].recovery_suspect);
        // VM 2's co-resident compute is suspect-only (checked via hardware ITS ticks), NEVER doomed!
        assert!(!fw.engine.flights[1].doomed);
        assert!(fw.engine.flights[1].recovery_suspect);

        // Two co-resident cross-VM renders: VM 1 faults on FS 0x100 while VM 2 has live render 0x200.
        let mut r2_ta = [0u32; 16];
        let mut r2_fs = [0u32; 16];
        r2_ta[0] = 0x200; r2_fs[0] = 0x200;
        fw.engine.flights[0].doomed = false;
        fw.engine.flights[0].recovery_suspect = false;
        fw.engine.flights[1] = RecFlight {
            job: RecJob { vm: RecVm { slot: 2 }, resources: Resources { passes: vec![] } },
            claims: RecClaims { pass: [0; 16] },
            computes: 0,
            renders: 1,
            compute_pipe: 0,
            target: 0,
            ta_target: 0x200,
            fs_target: 0x200,
            cstamp: [0; 16],
            r_ta_stamp: r2_ta,
            r_fs_stamp: r2_fs,
            doomed: false,
            recovery_suspect: false,
            recovery_started_ns: 0,
            gate_passed_ns: None,
            unsettled_polls: 0,
        };
        fw.engine.ta_target = 0x200;
        fw.engine.fs_target = 0x200;
        assert_eq!(fw.recovery_prepare(1, 0x71, 0x100, st(0x200, 0, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare_suspects(st(0x200, 0, 0, 0), 0x71, Some(1)), Ok(()));
        assert!(fw.engine.flights[0].doomed);
        assert!(!fw.engine.flights[1].doomed);
        assert!(fw.engine.flights[1].recovery_suspect);
    }
    #[test]
    fn two_pipe_compute_recovery_isolates_faulting_pipe_and_event_targets() {
        // Pipe 0 (VM 1, event 15) and Pipe 1 (VM 2, event 16) both hold identical per-pipe
        // stamp 0x100. Faulting event 16 dooms ONLY pipe 1 (VM 2) while pipe 0 (VM 1) stays
        // un-doomed suspect-only.
        let mut cs = [0u32; 16];
        cs[0] = 0x100;
        let mut last_retired = recovery::logic::RetiredStamps::new();
        last_retired.push(0, 0x300);
        last_retired.push(1, 0x400);
        last_retired.push(sizes::COMPUTE_PIPES - 1, 0x700);
        let mut target = [0u32; sizes::COMPUTE_PIPES];
        target[0] = 0x100;
        target[1] = 0x100;
        let mut fw = RecFirmware {
            engine: RecEngine {
                flights: vec![
                    RecFlight {
                        job: RecJob { vm: RecVm { slot: 1 }, resources: Resources { passes: vec![] } },
                        claims: RecClaims { pass: [0; 16] },
                        computes: 1,
                        renders: 0,
                        compute_pipe: 0,
                        target: 0x100,
                        ta_target: 0,
                        fs_target: 0,
                        cstamp: cs,
                        r_ta_stamp: [0; 16],
                        r_fs_stamp: [0; 16],
                        doomed: false,
                        recovery_suspect: false,
                        recovery_started_ns: 0,
                        gate_passed_ns: None,
                        unsettled_polls: 0,
                    },
                    RecFlight {
                        job: RecJob { vm: RecVm { slot: 2 }, resources: Resources { passes: vec![] } },
                        claims: RecClaims { pass: [0; 16] },
                        computes: 1,
                        renders: 0,
                        compute_pipe: 1,
                        target: 0x100,
                        ta_target: 0,
                        fs_target: 0,
                        cstamp: cs,
                        r_ta_stamp: [0; 16],
                        r_fs_stamp: [0; 16],
                        doomed: false,
                        recovery_suspect: false,
                        recovery_started_ns: 0,
                        gate_passed_ns: None,
                        unsettled_polls: 0,
                    },
                ],
                target,
                ta_target: 0,
                fs_target: 0,
            },
            recovery: RecState { last_retired },
        };
        assert_eq!(fw.recovery_prepare(1, 16, 0x100, st(0, 0, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare_suspects(st(0, 0, 0, 0), 16, Some(2)), Ok(()));
        assert!(!fw.engine.flights[0].doomed);
        assert!(fw.engine.flights[0].recovery_suspect);
        assert!(fw.engine.flights[1].doomed);
        assert!(fw.engine.flights[1].recovery_suspect);

        // If pipe 0 already finished (stage[2] == 0x100) before recovery snapshot, pipe 0 is not
        // even marked recovery_suspect while pipe 1 is.
        fw.engine.flights[0].recovery_suspect = false;
        fw.engine.flights[1].recovery_suspect = false;
        fw.engine.flights[1].doomed = false;
        assert_eq!(fw.recovery_prepare_suspects(st(0, 0, 0x100, 0), 16, Some(2)), Ok(()));
        assert!(!fw.engine.flights[0].recovery_suspect);
        assert!(fw.engine.flights[1].recovery_suspect);
        assert!(fw.engine.flights[1].doomed);

        // last_retired_target distinguishes event 15 (slot 0), event 16 (slot 1), and event 22 (slot 7).
        assert_eq!(fw.recovery_prepare(1, 15, 0x300, st(0, 0, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare(1, 16, 0x400, st(0, 0, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare(1, 15 + sizes::COMPUTE_PIPES as u32 - 1, 0x700, st(0, 0, 0, 0)), Ok(()));
        assert_eq!(fw.recovery_prepare(1, 15, 0x400, st(0, 0, 0, 0)), Err(ENOENT));
        assert_eq!(fw.recovery_prepare(1, 16, 0x300, st(0, 0, 0, 0)), Err(ENOENT));
    }
}
mod busy_coalesce_scope {
    use super::{Arc, AtomicBool, AtomicU32, Command, ComputeCommand, Order, Ordering, Progress, Queue, Recovery, RenderCommand, Result, Status, Vm};
    use crate::hw::sizes;
    mod params {
        pub fn compute_flights() -> usize { 23 }
        pub fn render_flights() -> usize { 47 }
    }
    struct Resources { units: usize, scs: bool }
    impl Resources {
        fn compute_slot_mask(&self) -> Result<u32> {
            Ok(if self.units >= 32 { u32::MAX } else { (1u32 << self.units) - 1 })
        }
        fn compute_scs(&self) -> bool { self.scs }
        fn render_scs(&self) -> bool { self.scs }
    }
    struct Job {
        vm: Arc<Vm>,
        queue: Arc<Queue>,
        progress: Progress,
        commands: Vec<Command>,
        orders: Vec<Order>,
        fw_waits: Vec<()>,
        resources: Resources,
        is_long: bool,
        foldable: bool,
    }
    impl Job {
        fn long(&self) -> bool { self.is_long }
        fn command_order(&self, idx: u32) -> Option<Order> { self.orders.get(idx as usize).copied() }
        fn can_fold_horizon_cmd(&self, idx: u32, expected: u64) -> bool {
            self.foldable
                && self.orders.get(idx as usize).is_some_and(|o| o.engine == crate::command::COMPUTE && o.sequence == expected && o.wait_through == [None, None])
        }
        fn can_fold_horizon_job(&self, first: u64) -> bool {
            self.foldable
                && self.orders.iter().enumerate().all(|(i, o)| o.engine == crate::command::COMPUTE && o.sequence == first + i as u64 && o.wait_through == [None, None])
        }
        fn can_fold_render_cmd(&self, idx: u32, expected: u64) -> bool {
            self.foldable
                && self.orders.get(idx as usize).is_some_and(|o| o.engine == crate::command::RENDER && o.sequence == expected)
        }
        fn can_fold_render_horizon_job(&self, first: u64) -> bool {
            self.foldable
                && self.orders.iter().enumerate().all(|(i, o)| o.engine == crate::command::RENDER && o.sequence == first + i as u64)
        }
    }
    fn command(job: &Job, k: u32) -> Option<&Command> { job.commands.get(k as usize) }
    struct Flight {
        job: Arc<Job>,
        computes: u32,
        renders: u32,
        compute_pipe: u8,
        published_ns: u64,
        doomed: bool,
        recovery_suspect: bool,
        initbm: bool,
        heavy_render: bool,
    }
    struct Engine {
        flights: Vec<Flight>,
        pending: Vec<Arc<Job>>,
        frozen: bool,
        chan_wait: bool,
        slot_wait: bool,
        ta_wave_wait: bool,
        held_wait: bool,
        coalesce_deadline_ns: u64,
    }
    struct Slots { free_c: u32 }
    impl Slots {
        fn free_compute_in_mask_for_pipe(&self, _m: u32, _p: usize) -> u32 { self.free_c }
    }
    struct Host {
        recovery: Recovery,
        engine: Engine,
        slots: Slots,
        allow_behind: bool,
        ta_pending: usize,
    }
    impl Host {
        fn can_queue_behind(&self, _job: &Arc<Job>) -> bool { self.allow_behind }
        fn choose_compute_pipe(&self, _q: &Arc<Queue>, _vm: &Arc<Vm>, _w: [Option<()>; crate::hw::sizes::COMPUTE_PIPES], _m: u32, _rt: bool, _heavy: bool) -> Option<usize> { Some(0) }
        fn ta_pending_passes(&self) -> usize { self.ta_pending }
        __COMPUTE_QUEUE_IS_HEAVY__
        __TA_WAVE_CAP_FOR__
        __CAN_DEFER_BUSY_COALESCE__
        __DEFER_BUSY_COALESCE__
    }
    fn mk_job_mode(vm: &Arc<Vm>, q: &Arc<Queue>, render: bool, heavy: bool, seq: u64, wait: [Option<u64>; 2]) -> Arc<Job> {
        Arc::new(Job {
            vm: vm.clone(),
            queue: q.clone(),
            progress: Progress { sent: AtomicU32::new(0), count: 1, pending: AtomicBool::new(true) },
            commands: vec![if render { Command::Render(RenderCommand { heavy_ta: heavy, msaa: false }) } else { Command::Compute(ComputeCommand { query_access: false, rt: false }) }],
            orders: vec![Order { engine: if render { crate::command::RENDER } else { crate::command::COMPUTE }, sequence: seq, wait_through: wait }],
            fw_waits: vec![],
            resources: Resources { units: 24, scs: false },
            is_long: false,
            foldable: true,
        })
    }
    fn mk_job(vm: &Arc<Vm>, q: &Arc<Queue>, render: bool, seq: u64, wait: [Option<u64>; 2]) -> Arc<Job> {
        mk_job_mode(vm, q, render, false, seq, wait)
    }
    #[test]
    fn busy_coalesce_holds_deep_single_queue_bursts_and_never_delays_shallow_or_other_queues() {
        let vm1 = Arc::new(Vm { id: 1, status: Status(AtomicU32::new(0)), slot: 1, msaa: AtomicBool::new(false) });
        let q1 = Queue::new(0, 2);
        let q2 = Queue::new(0, 2);
        let c1 = mk_job(&vm1, &q1, false, 1, [None, None]);
        let mut h = Host {
            recovery: Recovery { error: None, publication_wait_deadline: None },
            engine: Engine {
                flights: vec![Flight { job: c1.clone(), computes: 1, renders: 0, compute_pipe: 0, published_ns: 90_000, doomed: false, recovery_suspect: false, initbm: false, heavy_render: false }],
                pending: vec![mk_job(&vm1, &q1, false, 2, [None, None])],
                frozen: false,
                chan_wait: false,
                slot_wait: false,
                ta_wave_wait: false,
                held_wait: false,
                coalesce_deadline_ns: 0,
            },
            slots: Slots { free_c: 20 },
            allow_behind: true,
            ta_pending: 0,
        };
        // d=2 (queue_active == 1 < 2): ZERO coalescing delay.
        assert_eq!(h.defer_busy_coalesce(100_000), None);

        // Once queue_active >= 2, barrier-free compute holds up to 18 us and accumulates up to K=6.
        h.engine.flights[0].computes = 2;
        h.engine.pending = vec![mk_job(&vm1, &q1, false, 3, [None, None])];
        assert_eq!(h.defer_busy_coalesce(100_000), Some(18));
        assert_eq!(h.engine.coalesce_deadline_ns, 118_000);
        // Second job arrives 5 us later -> remaining 13 us.
        h.engine.pending.push(mk_job(&vm1, &q1, false, 4, [None, None]));
        assert_eq!(h.defer_busy_coalesce(105_000), Some(13));
        // Reaching K=6 pending commands flushes immediately.
        for seq in 5..=8 {
            h.engine.pending.push(mk_job(&vm1, &q1, false, seq, [None, None]));
        }
        assert_eq!(h.defer_busy_coalesce(110_000), None);
        assert_eq!(h.engine.coalesce_deadline_ns, 0);

        // A job held behind the VM's own flights is never coalesced behind them.
        h.engine.pending = vec![mk_job(&vm1, &q1, false, 3, [None, None])];
        h.engine.held_wait = true;
        assert_eq!(h.defer_busy_coalesce(150_000), None);
        h.engine.held_wait = false;

        // Deadline expiry flushes even with 1 pending command.
        h.engine.pending = vec![mk_job(&vm1, &q1, false, 3, [None, None])];
        assert_eq!(h.defer_busy_coalesce(200_000), Some(18));
        assert_eq!(h.defer_busy_coalesce(218_000), None);
        assert_eq!(h.engine.coalesce_deadline_ns, 0);

        // Another queue arriving in pending flushes immediately (no cross-queue head-of-line delay).
        assert_eq!(h.defer_busy_coalesce(300_000), Some(18));
        h.engine.pending.push(mk_job(&vm1, &q2, false, 1, [None, None]));
        assert_eq!(h.defer_busy_coalesce(302_000), None);

        // Barriered compute flushes immediately (0 delay).
        h.engine.pending = vec![mk_job(&vm1, &q1, false, 3, [None, Some(2)])];
        assert_eq!(h.defer_busy_coalesce(400_000), None);

        // Heavy compute queue (EWMA >= 140 us) bypasses busy coalescing so 2-pipe fanout runs immediately.
        h.engine.pending = vec![mk_job(&vm1, &q1, false, 3, [None, None])];
        q1.note_compute_us(500);
        assert_eq!(h.defer_busy_coalesce(420_000), None);
        q1.compute_ewma_us.store(25, Ordering::Relaxed);
        assert_eq!(h.defer_busy_coalesce(420_000), Some(18));

        // Active flight from another queue bypasses busy coalescing immediately (multi-queue/async_cr).
        let r_other = mk_job(&vm1, &q2, true, 1, [None, None]);
        h.engine.flights.push(Flight { job: r_other, computes: 0, renders: 1, compute_pipe: 0, published_ns: 410_000, doomed: false, recovery_suspect: false, initbm: false, heavy_render: false });
        assert_eq!(h.defer_busy_coalesce(425_000), None);
        h.engine.flights.pop();

        // Light render with queue_active == 2 and ta_pending == 2 (wave_cap 8 -> ta_room 6)
        // coalesces up to K=6 passes before flushing.
        let r1 = mk_job(&vm1, &q1, true, 1, [None, None]);
        h.engine.flights = vec![Flight { job: r1, computes: 0, renders: 2, compute_pipe: 0, published_ns: 490_000, doomed: false, recovery_suspect: false, initbm: false, heavy_render: false }];
        h.ta_pending = 2;
        h.engine.pending = vec![mk_job(&vm1, &q1, true, 3, [Some(2), None])];
        assert_eq!(h.defer_busy_coalesce(500_000), Some(18));
        for seq in 4..=7 {
            h.engine.pending.push(mk_job(&vm1, &q1, true, seq, [Some(seq - 1), None]));
            assert_eq!(h.defer_busy_coalesce(502_000), Some(16));
        }
        h.engine.pending.push(mk_job(&vm1, &q1, true, 8, [Some(7), None]));
        assert_eq!(h.defer_busy_coalesce(505_000), None);

        // Heavy render keeps conservative wave_cap 4 (ta_room == 2): holds 1 pass, flushes at 2.
        h.engine.pending = vec![mk_job_mode(&vm1, &q1, true, true, 3, [Some(2), None])];
        assert_eq!(h.defer_busy_coalesce(600_000), Some(18));
        h.engine.pending.push(mk_job_mode(&vm1, &q1, true, true, 4, [Some(3), None]));
        assert_eq!(h.defer_busy_coalesce(605_000), None);
    }
}
// Production fold loops and publication prologue of publish_prefix_body, verbatim, around mock
// slots, waits and prefix_render: what they accept, refuse and mark is production behaviour.
mod msaa_fold_scope {
    use std::sync::{Arc, atomic::{AtomicBool, Ordering}};
    #[derive(Copy,Clone,Debug,PartialEq,Eq)] struct Error(i32);
    const EINVAL: Error = Error(22); const EAGAIN: Error = Error(11); const EIO: Error = Error(5);
    type Result<T = ()> = std::result::Result<T, Error>;
    #[derive(Copy,Clone)] struct Gfp; const GFP_NOWAIT: Gfp = Gfp;
    mod sizes {
        pub const QUEUE_CAP: u32 = 256; pub const TA_WAVE_PASSES: usize = 4; pub const TA_WAVE_PASSES_LIGHT: usize = 8;
        pub const HORIZON_JOBS: u32 = 8; pub const MAX_RENDER_RING_ENTRIES: usize = 5;
    }
    mod params { pub fn early_tiling() -> bool { true } }
    #[allow(non_upper_case_globals)]
    mod kernel { pub mod uapi { pub const drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES: u32 = 1 << 23; } }
    mod super_vm { pub(super) type VmState = super::Vm; }
    #[derive(Copy,Clone)] struct RenderCmd { flags: u32, samples: u16 }
    #[derive(Copy,Clone)] struct RenderCommand { cmd: RenderCmd, heavy_ta: bool }
    impl RenderCommand {
        fn is_heavy_ta(&self) -> bool { self.heavy_ta }
        __IS_MULTISAMPLED__
    }
    const LIGHT: RenderCommand = RenderCommand { cmd: RenderCmd { flags: 0, samples: 1 }, heavy_ta: false };
    const MSAA: RenderCommand = RenderCommand { cmd: RenderCmd { flags: 0, samples: 4 }, heavy_ta: false };
    #[derive(Copy,Clone)] enum Command { Render(RenderCommand), #[allow(dead_code)] Compute }
    struct Geometry;
    impl Geometry { fn new_for(_c: &RenderCmd, _heap: u64, _clusters: u32, _generation: u32) -> Self { Geometry } }
    struct Binding; impl Binding { fn mark_binding_published(&self) -> Result { Ok(()) } }
    struct Vm { id: u64, renders_msaa: AtomicBool, vm: Binding }
    impl Vm {
        __RENDERS_MSAA__
        __NOTE_MSAA_RENDER__
    }
    fn vm(id: u64) -> Arc<Vm> { Arc::new(Vm { id, renders_msaa: AtomicBool::new(false), vm: Binding }) }
    struct Queue;
    #[derive(Copy,Clone)] struct Order { sequence: u64, wait_through: [Option<u64>; 2], ta_wait_through: [Option<u64>; 2] }
    struct Progress { count: u32 } impl Progress { fn count(&self) -> u32 { self.count } }
    struct Resources;
    impl Resources {
        fn render_scs(&self) -> bool { false }
        fn pb_owner_arena(&self, _k: usize) -> u32 { 0 }
        fn pb_uma_lease(&self, _k: usize) -> u32 { 0 }
    }
    struct Job { id: u32, vm: Arc<Vm>, queue: Arc<Queue>, commands: Vec<Command>, first_seq: u64, progress: Progress, resources: Resources }
    impl Job {
        fn long(&self) -> bool { false }
        fn command_order(&self, k: u32) -> Option<Order> {
            Some(Order { sequence: self.first_seq + u64::from(k), wait_through: [None, None], ta_wait_through: [None, None] })
        }
        fn can_fold_render_cmd(&self, k: u32, seq: u64) -> bool { seq == self.first_seq + u64::from(k) }
        fn can_fold_render_horizon_job(&self, first: u64) -> bool { first == self.first_seq }
    }
    fn job(id: u32, vm: &Arc<Vm>, queue: &Arc<Queue>, first_seq: u64, commands: &[RenderCommand]) -> Arc<Job> {
        Arc::new(Job { id, vm: vm.clone(), queue: queue.clone(), commands: commands.iter().map(|r| Command::Render(*r)).collect(),
            first_seq, progress: Progress { count: commands.len() as u32 }, resources: Resources })
    }
    fn command(job: &Job, k: u32) -> Option<&Command> { job.commands.get(k as usize) }
    struct Claims { pass: [u32; 48], pass_count: u32, pass_mask: u64 }
    struct Group(Vec<Arc<Job>>);
    impl Group {
        fn len(&self) -> usize { self.0.len() }
        fn reserve(&mut self, n: usize, _g: Gfp) -> std::result::Result<(), ()> { self.0.reserve(n); Ok(()) }
        fn push_within_capacity(&mut self, j: Arc<Job>) -> std::result::Result<(), Arc<Job>> { self.0.push(j); Ok(()) }
    }
    struct Pb; impl Pb { fn check_assignment(&self, _k: usize) -> Result { Ok(()) } }
    struct Flight { job: Arc<Job>, renders: u32, claims: Claims, count: usize, group: Group, msaa: bool, pb: Option<Pb>, pb_slot: u32 }
    fn flight(job: &Arc<Job>, renders: u32) -> Flight {
        Flight { job: job.clone(), renders, claims: Claims { pass: [0; 48], pass_count: 1, pass_mask: 1 }, count: 1,
            group: Group(vec![]), msaa: false, pb: Some(Pb), pb_slot: 0 }
    }
    __MSAA_RENDER_CONFLICT__
    struct Prefix { draw: u64, ta_head: u32, fs_head: u32, renders: u32, ta_prev: u64, ta_order: u64 }
    struct Cfg { num_clusters: u32, gpu_generation: u32 }
    struct Slots { pass_busy: u64, publish_fails: bool }
    impl Slots {
        fn pick_pass_in_for(&self, _r: &Resources, _g: &Geometry, picked: u64, _vm: Option<u64>) -> Option<usize> {
            (0..48).find(|k| (self.pass_busy | picked) & (1u64 << k) == 0)
        }
        fn verify_held(&self, _c: &Claims) -> Result { Ok(()) }
        fn publish(&mut self, _c: &mut Claims) -> Result { if self.publish_fails { Err(EIO) } else { Ok(()) } }
    }
    struct Pools; impl Pools { fn hold_alloc(&mut self, _k: usize, _vm: &Arc<Vm>, _a: u32, _l: u32) -> Result { Ok(()) } }
    mod qstate { pub const WRITE: usize = 0; }
    struct QState { fail: bool }
    impl QState {
        fn w32(&self, _off: usize, _v: u32) -> Result {
            if self.fail { Err(EIO) } else { Ok(()) }
        }
    }
    struct RenderGraph { qstate: [QState; 2] }
    struct Graph { render: RenderGraph }
    #[derive(Default)]
    struct RenderStats { publications: [u64; 2], ta_prev: u64, ta_order: u64 }
    fn publish_producer(q: &QState, head: u32, count: &mut u64) -> Result {
        q.w32(qstate::WRITE, head)?;
        *count += 1;
        Ok(())
    }
    struct Engine { flights: Vec<Flight>, pending: Vec<Arc<Job>>, stats: RenderStats }
    struct Host { cfg: Cfg, slots: Slots, pools: Pools, graph: Graph, engine: Engine, prefixed: Vec<(u32, u32)>, recovery: bool }
    impl Host {
        __MSAA_MEMBER_ALLOWED__
        fn recovery_pending(&self) -> Result<bool> { Ok(self.recovery) }
        fn wait_compute_stamps_for(&self, _j: &Arc<Job>, _t: Option<u64>) -> Result { Ok(()) }
        fn wait_stamp_for_with(&self, _j: &Arc<Job>, _c: usize, _t: Option<u64>, _f: Option<&Flight>) -> Result { Ok(()) }
        #[allow(clippy::too_many_arguments)]
        fn prefix_render(&mut self, job: &Arc<Job>, _r: &Resources, k: u32, cmd: &RenderCommand, _g: Geometry,
            prefix: &mut Prefix, f: &mut Flight, _behind: bool, _class_behind: bool) -> Result {
            self.prefixed.push((job.id, k));
            prefix.renders += 1;
            prefix.draw += 1;
            f.msaa |= cmd.is_multisampled();
            Ok(())
        }
        /// The lead (command 0 of `job`) is already prefixed; returns the prefix's end command.
        #[allow(unused_assignments, unused_variables, unused_mut)]
        fn fold(&mut self, job: &Arc<Job>, f: &mut Flight, more: usize) -> Result<u32> {
            let index = 0u32;
            let mut end = 1u32;
            let mut prefix = Prefix { draw: 1, ta_head: 0, fs_head: 0, renders: 1, ta_prev: 0, ta_order: 0 };
            let (ta_read, fs_read) = (0u32, 0u32);
            let (behind, class_behind) = (true, true);
            let stage_prefix = kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES;
            let mut last_seq = job.command_order(index).map(|o| o.sequence);
            let ta_pending_before = 0usize;
            let mut wave_heavy = false;
            let mut more_renders = more;
            __SAME_SUBMIT_FOLD__
            __HORIZON_FOLD__
            f.renders = prefix.renders;
            Ok(end)
        }
        fn publish_prologue(&mut self, job: &Arc<Job>, f: &mut Flight, visible: &mut bool, fail_after: bool) -> Result {
            let render = true;
            __PUBLICATION_PROLOGUE__
            if fail_after { return Err(EIO); }
            Ok(())
        }
        fn publish_render_producers(&mut self, job: &Arc<Job>, f: &mut Flight, prefix: &Prefix, visible: &mut bool) -> Result {
            let render = true;
            __PUBLICATION_PROLOGUE__
            __RENDER_PRODUCER_PUBLICATION__
            Ok(())
        }
    }
    fn host(flights: Vec<Flight>, pending: Vec<Arc<Job>>) -> Host {
        Host {
            cfg: Cfg { num_clusters: 1, gpu_generation: 16 },
            slots: Slots { pass_busy: 1, publish_fails: false },
            pools: Pools,
            graph: Graph { render: RenderGraph { qstate: [QState { fail: false }, QState { fail: false }] } },
            engine: Engine { flights, pending, stats: RenderStats::default() },
            prefixed: vec![],
            recovery: false,
        }
    }
    #[test] fn same_submit_fold_splits_before_a_multisampled_member_beside_another_vm() {
        let (vm1, vm2, q1, q2) = (vm(1), vm(2), Arc::new(Queue), Arc::new(Queue));
        let lead = job(1, &vm1, &q1, 10, &[LIGHT, LIGHT, MSAA, LIGHT]);
        let other = job(2, &vm2, &q2, 1, &[LIGHT]);
        // Beside another VM's render: the single-sampled member joins, the prefix ends at the
        // multisampled one (command 2), which waits for its own publication.
        let mut h = host(vec![flight(&other, 1)], vec![]);
        let mut f = flight(&lead, 1);
        assert_eq!(h.fold(&lead, &mut f, 8), Ok(2));
        assert_eq!(h.prefixed, [(1, 1)]);
        assert!(!f.msaa);
        // Nothing of another VM in flight: the whole submit folds.
        let mut h = host(vec![], vec![]);
        let mut f = flight(&lead, 1);
        assert_eq!(h.fold(&lead, &mut f, 8), Ok(4));
        assert_eq!(h.prefixed, [(1, 1), (1, 2), (1, 3)]);
        assert!(f.msaa);
    }
    #[test] fn cross_job_fold_takes_a_whole_job_or_none_of_it_beside_another_vm() {
        let (vm1, vm2, q1, q2) = (vm(1), vm(2), Arc::new(Queue), Arc::new(Queue));
        let lead = job(1, &vm1, &q1, 10, &[LIGHT]);
        let other = job(2, &vm2, &q2, 1, &[LIGHT]);
        // A single-sampled job folds beside another VM's render.
        let light = job(3, &vm1, &q1, 11, &[LIGHT, LIGHT]);
        let mut h = host(vec![flight(&other, 1)], vec![lead.clone(), light.clone()]);
        let mut f = flight(&lead, 1);
        assert_eq!(h.fold(&lead, &mut f, 8), Ok(1));
        assert_eq!(h.prefixed, [(3, 0), (3, 1)]);
        assert_eq!(f.group.len(), 1);
        assert_eq!(f.renders, 3);
        // A job with a multisampled member is declined whole: none of its members is prefixed.
        let mixed = job(4, &vm1, &q1, 11, &[LIGHT, MSAA]);
        let mut h = host(vec![flight(&other, 1)], vec![lead.clone(), mixed.clone()]);
        let mut f = flight(&lead, 1);
        assert_eq!(h.fold(&lead, &mut f, 8), Ok(1));
        assert!(h.prefixed.is_empty());
        assert_eq!(f.group.len(), 0);
        assert_eq!(f.renders, 1);
        // Without another VM in flight the same job folds.
        let mut h = host(vec![], vec![lead.clone(), mixed.clone()]);
        let mut f = flight(&lead, 1);
        assert_eq!(h.fold(&lead, &mut f, 8), Ok(1));
        assert_eq!(h.prefixed, [(4, 0), (4, 1)]);
        assert!(f.msaa);
    }
    #[test] fn only_a_visible_multisampled_prefix_marks_its_vm() {
        let q = Arc::new(Queue);
        // Refused before visibility: unpublished, the VM stays unmarked.
        let v = vm(1);
        let j = job(1, &v, &q, 1, &[MSAA]);
        let mut h = host(vec![], vec![]);
        h.slots.publish_fails = true;
        let (mut f, mut visible) = (flight(&j, 1), false);
        f.msaa = true;
        assert_eq!(h.publish_prologue(&j, &mut f, &mut visible, false), Err(EIO));
        assert!(!visible && !v.renders_msaa());
        // Published: marked.
        let mut h = host(vec![], vec![]);
        let (mut f, mut visible) = (flight(&j, 1), false);
        f.msaa = true;
        assert_eq!(h.publish_prologue(&j, &mut f, &mut visible, false), Ok(()));
        assert!(visible && v.renders_msaa());
        // Visible, then failed (uncertain, may run): marked.
        let v2 = vm(2);
        let j2 = job(2, &v2, &q, 1, &[MSAA]);
        let mut h = host(vec![], vec![]);
        let (mut f, mut visible) = (flight(&j2, 1), false);
        f.msaa = true;
        assert_eq!(h.publish_prologue(&j2, &mut f, &mut visible, true), Err(EIO));
        assert!(visible && v2.renders_msaa());
        // A published single-sampled prefix does not mark.
        let v3 = vm(3);
        let j3 = job(3, &v3, &q, 1, &[LIGHT]);
        let mut h = host(vec![], vec![]);
        let (mut f, mut visible) = (flight(&j3, 1), false);
        assert_eq!(h.publish_prologue(&j3, &mut f, &mut visible, false), Ok(()));
        assert!(visible && !v3.renders_msaa());
        let _ = (EAGAIN, EINVAL);
    }
    #[test] fn ta_prev_barrier_stats_commit_only_on_ta_producer_publication() {
        let q = Arc::new(Queue);
        let v = vm(1);
        let j = job(1, &v, &q, 1, &[LIGHT]);
        let prefix = Prefix { draw: 1, ta_head: 4, fs_head: 5, renders: 1, ta_prev: 2, ta_order: 3 };
        // Rejected by recovery_pending (-EAGAIN) before visibility: staged counts are discarded.
        let mut h = host(vec![], vec![]);
        h.recovery = true;
        let (mut f, mut visible) = (flight(&j, 1), false);
        assert_eq!(h.publish_render_producers(&j, &mut f, &prefix, &mut visible), Err(EAGAIN));
        assert!(!visible && h.engine.stats.ta_prev == 0 && h.engine.stats.ta_order == 0);
        // TA producer publication fails: counts do not advance.
        let mut h = host(vec![], vec![]);
        h.graph.render.qstate[0].fail = true;
        let (mut f, mut visible) = (flight(&j, 1), false);
        assert_eq!(h.publish_render_producers(&j, &mut f, &prefix, &mut visible), Err(EIO));
        assert!(visible && h.engine.stats.ta_prev == 0 && h.engine.stats.ta_order == 0);
        // TA producer publishes cleanly: staged counts commit alongside publications[1].
        let mut h = host(vec![], vec![]);
        let (mut f, mut visible) = (flight(&j, 1), false);
        assert_eq!(h.publish_render_producers(&j, &mut f, &prefix, &mut visible), Ok(()));
        assert!(visible && h.engine.stats.publications[1] == 1);
        assert_eq!((h.engine.stats.ta_prev, h.engine.stats.ta_order), (2, 3));
    }
}

'''
harness = harness.replace('__LOGIC__', str(production.parent / 'recovery/logic.rs'))

vm_src = (production.parent / 'vm.rs').read_text()
command_src = (production.parent / 'command.rs').read_text()
_same_fold = item('while more_renders > 0 && end < job.progress.count() {')
_horizon_fold = item('if end == job.progress.count() && more_renders > 0 {')
_prologue_start = source.index('        if self.recovery_pending()? {\n            return Err(EAGAIN);\n        }\n        if render {\n            f.pb.as_ref()')
_prologue_end = source.index('        let publication = (|| -> Result {', _prologue_start)
_prologue = source[_prologue_start:_prologue_end]
assert '*visible = true;' in _prologue and 'job.vm.note_msaa_render();' in _prologue
_ta_pub_start = source.index('                publish_producer(\n                    &self.graph.render.qstate[0],', _prologue_end)
_ta_pub_end = source.index('            } else {', _ta_pub_start)
_ta_pub = source[_ta_pub_start:_ta_pub_end]
assert 'self.engine.stats.ta_prev += prefix.ta_prev;' in _ta_pub
assert 'self.engine.stats.ta_order += prefix.ta_order;' in _ta_pub
_scope_start = harness.index('mod msaa_fold_scope {')
_scope = harness[_scope_start:]
for token, text in {
    '__SAME_SUBMIT_FOLD__': _same_fold,
    '__HORIZON_FOLD__': _horizon_fold,
    '__PUBLICATION_PROLOGUE__': _prologue,
    '__RENDER_PRODUCER_PUBLICATION__': _ta_pub,
    '__IS_MULTISAMPLED__': item_in(command_src, 'pub(crate) fn is_multisampled('),
    '__RENDERS_MSAA__': item_in(vm_src, 'pub(crate) fn renders_msaa('),
    '__NOTE_MSAA_RENDER__': item_in(vm_src, 'pub(crate) fn note_msaa_render('),
    '__MSAA_RENDER_CONFLICT__': item('fn msaa_render_conflict('),
    '__MSAA_MEMBER_ALLOWED__': item('fn msaa_member_allowed('),
}.items():
    assert token in _scope, token
    _scope = _scope.replace(token, text
        .replace('super::recovery::', 'crate::recovery::')
        .replace('super::command::', 'crate::command::')
        .replace('super::vm::VmState', 'super_vm::VmState'))
harness = harness[:_scope_start] + _scope
harness = harness.replace('__USAGE__', str(production.parent.parent / 'usage.rs'))
harness = harness.replace('__FLIGHTS_LINE__', flights_line.strip())
harness = harness.replace('__MAX_QUEUES_LINE__', max_queues_line.strip())
harness = harness.replace('__HEAVY_LIMITS__', ' '.join(heavy_limits))
harness = harness.replace('__QUEUE_LIMITS_FN__', item_in(params_src, 'pub(crate) fn queue_limits()'))
harness = harness.replace('__GPU_QUEUE_LIMITS_FN__', gpu_limits_fn)
harness = harness.replace('__QUEUE_CREDIT_STRUCT__', item_in(queue_src, 'struct QueueCredit {'))
harness = harness.replace('__QUEUE_CREDIT_IMPL__', item_in(queue_src, 'impl QueueCredit {'))
harness = harness.replace('__QUEUE_CREDIT_DROP__', item_in(queue_src, 'impl Drop for QueueCredit {'))
harness = harness.replace('__NOTE_COMPUTE_US__', item_in(queue_src, 'pub(crate) fn note_compute_us('))
harness = harness.replace(
    '__CAN_FOLD_CMD__',
    item_in(job_src, 'pub(crate) fn can_fold_horizon_cmd(')
    .replace('super::command::', 'command::')
    .replace('super::fence::', 'fence::'),
)
harness = harness.replace(
    '__CAN_FOLD_JOB__',
    item_in(job_src, 'pub(crate) fn can_fold_horizon_job(')
    .replace('super::command::', 'command::')
    .replace('super::fence::', 'fence::'),
)
harness = harness.replace(
    '__CAN_FOLD_RENDER_CMD__',
    item_in(job_src, 'pub(crate) fn can_fold_render_cmd(')
    .replace('super::command::', 'command::')
    .replace('super::fence::', 'fence::'),
)
harness = harness.replace(
    '__CAN_FOLD_RENDER_JOB__',
    item_in(job_src, 'pub(crate) fn can_fold_render_horizon_job(')
    .replace('super::command::', 'command::')
    .replace('super::fence::', 'fence::'),
)
harness = harness.replace(
    '__RECOVERY_PREPARE__',
    item('pub(crate) fn recovery_prepare(')
    .replace('super::recovery::', 'recovery::')
    .replace('super::poll::', 'poll::'),
)
harness = harness.replace(
    '__RECOVERY_PREPARE_SUSPECTS__',
    item('pub(crate) fn recovery_prepare_suspects(')
    .replace('super::recovery::', 'recovery::'),
)
for token, declaration in {
    '__PUBLICATION_ERROR__': 'enum PublicationError {',
    '__PICK__': 'fn pick(&self)',
    '__UNPEND__': 'fn unpend(&mut self',
    '__FLUSH_DOORBELLS__': 'fn flush_doorbells(',
    '__PUBLISH_PREFIX__': 'fn publish_prefix(',
    '__PUBLISH__': 'pub(crate) fn publish(&mut self',
    '__PUBLISH_CHECKED__': 'pub(crate) fn publish_checked(&mut self',
    '__NEEDS_ENGINE_DRAIN__': 'fn needs_engine_drain(',
    '__RENDER_CAN_PIPELINE__': 'fn render_can_pipeline(',
    '__HOLD_CAUSE__': 'enum HoldCause {',
    '__HOLD_CAUSE_IMPL__': 'impl HoldCause {',
    '__HOLD_CAUSE_FN__': 'fn hold_cause(',
    '__NOTE_HOLD__': 'fn note_hold(',
    '__BLOCKED_ON_SLOT_HEADROOM__': 'fn blocked_on_slot_headroom(',
    '__DESCRIPTOR_CONTAINMENT__': 'if encode::validate_compute_descriptor(',
    '__PUBLISH_PRODUCER__': 'fn publish_producer(',
    '__TA_PENDING_PASSES__': 'pub(crate) fn ta_pending_passes(',
    '__TA_WAVE_CAP_FOR__': 'fn ta_wave_cap_for(',
    '__ACTIVE_TA_WAVE_CAP__': 'pub(crate) fn active_ta_wave_cap(',
    '__CAN_QUEUE_BEHIND__': 'fn can_queue_behind(',
    '__CAN_QUEUE_BEHIND_WITH_TA__': 'fn can_queue_behind_with_ta(',
    '__CAN_DEFER_BUSY_COALESCE__': 'fn can_defer_busy_coalesce(',
    '__DEFER_BUSY_COALESCE__': 'pub(crate) fn defer_busy_coalesce(',
    '__WAIT_STAMP_FOR_WITH__': 'fn wait_stamp_for_with(',
    '__WAIT_STAMP_FOR__': 'fn wait_stamp_for(',
    '__CROSS_ENGINE_WAIT__': 'fn cross_engine_wait(',
    '__WAIT_COMPUTE_STAMPS_FOR__': 'fn wait_compute_stamps_for(',
    '__COMPUTE_QUEUE_IS_HEAVY__': 'fn compute_queue_is_heavy(',
    '__COMPUTE_HANDOFF_PIPE__': 'fn compute_handoff_pipe(',
    '__HAS_COMPUTE_PIPE_FOR_VM__': 'fn has_compute_pipe_for_vm(',
    '__CHOOSE_COMPUTE_PIPE__': 'fn choose_compute_pipe(',
    '__SCS_COMPUTE_CONFLICT__': 'fn scs_compute_conflict(',
    '__MSAA_RENDER_CONFLICT__': 'fn msaa_render_conflict(',
    '__MSAA_MEMBER_ALLOWED__': 'fn msaa_member_allowed(',
    '__WAIT_TARGET__': 'struct WaitTarget {',
    '__CROSS_ENGINE_BARRIER_TYPE__': 'const fn cross_engine_barrier_type(',
    '__RENDER_BARRIER_PLAN__': 'struct RenderBarrierPlan {',
    '__PLAN_RENDER_BARRIERS__': 'fn plan_render_barriers(',
    '__TA_RING_ORDERS_PREV__': 'fn ta_ring_orders_prev(',
}.items():
    harness = harness.replace(token, item(declaration).replace("super::command::", "crate::command::").replace("super::recovery::", "crate::recovery::").replace("super::hw::", "crate::hw::").replace("super::resources::", "crate::resources::").replace("super::queue::QueueState", "crate::Queue").replace("super::vm::VmState", "crate::Vm").replace("super::fence::", "crate::fence::"))
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
passed_match = re.search(r'test result: ok\. (\d+) passed; 0 failed;', log)
assert passed_match, f'could not parse test count from:\n{log}'
(output/'publication.json').write_text(json.dumps({
    'passed': True, 'tests': int(passed_match.group(1)), 'cpu_only': True, 'hardware_execution': False,
    'scope': 'Verbatim production publication wrapper, selection, error isolation and descriptor-error containment branch; wire encoder, descriptor failure injection and transport replaced by CPU recorders.',
    'source_sha256': sha(production),
}, indent=2)+'\n')
