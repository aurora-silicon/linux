#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the copied production progress sampler across native zero wrap."""
from common import kernel_root, output_dir, rustc, sha
import subprocess,json
root=kernel_root();out=output_dir();backend=root/'drivers/gpu/drm/asahi/g16'
source=(backend/'poll.rs').read_text();engine=(backend/'engine.rs').read_text();recovery_src=(backend/'recovery.rs').read_text()
def function(text,name):
    marker=text.index('fn '+name+'(');start=text.rfind('\n',0,marker)+1;at=text.index('{',marker);depth,end=1,at+1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
option='progress: [Option<u32>; sizes::COMPUTE_PIPES + 2]' in engine
harness=r'''
#![allow(dead_code)]
use std::cell::Cell;
#[path="__LOGIC__"]mod logic;
use logic::stamp_reached;
#[derive(Copy,Clone,Debug,PartialEq,Eq)]struct Error(i32);
const EIO:Error=Error(5);const ETIMEDOUT:Error=Error(110);
type Result<T=()>=core::result::Result<T,Error>;
mod hw{pub mod sizes{pub const COMPUTE_PIPES:usize=8;}}
mod command{pub const RENDER:usize=0;pub const COMPUTE:usize=1;}
mod params{
    pub fn recovery()->bool{true}
    pub fn deadline_ms()->u32{1000}
    pub fn compute_deadline_ms()->u32{5000}
}
thread_local!{static CLOCK:Cell<u64>=const{Cell::new(0)};}
fn now_ns()->u64{CLOCK.with(|c|c.get())}
fn set_now_ns(v:u64){CLOCK.with(|c|c.set(v));}
macro_rules! dev_warn{($($tt:tt)*)=>{};}
macro_rules! dev_err{($($tt:tt)*)=>{};}
struct Object(Cell<u32>);impl Object{fn r32(&self,_off:usize)->core::result::Result<u32,()>{Ok(self.0.get())}}
struct Compute{fw_stamp:Object}struct Render{fw_stamp:[Object;2]}struct Graph{compute:[Compute;hw::sizes::COMPUTE_PIPES],render:Render}
struct Job{long:bool}
impl Job{fn long(&self)->bool{self.long}}
struct Flight{job:Job,computes:u32,compute_pipe:u8,renders:u32,target:u32,ta_target:u32,fs_target:u32,published_ns:u64}
struct Engine{flights:Vec<Flight>,target:[u32;hw::sizes::COMPUTE_PIPES],ta_target:u32,fs_target:u32,progress_flight:[u64;hw::sizes::COMPUTE_PIPES+1],progress:[__TYPE__;hw::sizes::COMPUTE_PIPES+2]}
impl Engine{fn oldest(&self)->Option<&Flight>{self.flights.first()}}
struct Recovery{deadlines:logic::Deadlines,compute_deadlines:[logic::Deadlines;hw::sizes::COMPUTE_PIPES],warning_deadline_ns:[u64;2],compute_warning_deadline_ns:[u64;hw::sizes::COMPUTE_PIPES],long_commands:u64,receipt_fault:bool,failed:Option<Error>}
struct Firmware{graph:Graph,engine:Engine,recovery:Recovery,render_renewed:u32,compute_renewed:u32,pipe_renewed:[u32;hw::sizes::COMPUTE_PIPES],fail_stops:Vec<Error>}
impl Firmware{
    fn dev(&self){}
    fn transport_error(&self)->Option<Error>{None}
    fn recovery_pending(&self)->Result<bool>{Ok(false)}
    fn recover(&mut self)->Result{Ok(())}
    fn fail_stop(&mut self,error:Error,_done:&mut ()){self.recovery.failed=Some(error);self.fail_stops.push(error);}
    fn renew_render_deadline(&mut self){
        let now=now_ns();
        let renewed=logic::Deadlines::renew(now,params::deadline_ms(),0);
        self.recovery.deadlines.any=renewed.any;
        self.recovery.deadlines.long_any=renewed.long_any;
        self.recovery.warning_deadline_ns[command::RENDER]=now.saturating_add(5_000_000_000);
        self.render_renewed+=1;
    }
    fn renew_compute_pipe_deadline(&mut self,pipe:usize){
        let pipe=pipe.min(hw::sizes::COMPUTE_PIPES-1);
        let now=now_ns();
        let renewed=logic::Deadlines::renew(now,0,params::compute_deadline_ms());
        self.recovery.compute_deadlines[pipe].compute=renewed.compute;
        self.recovery.compute_deadlines[pipe].long_compute=renewed.long_compute;
        self.recovery.compute_warning_deadline_ns[pipe]=now.saturating_add(5_000_000_000);
        self.pipe_renewed[pipe]+=1;
        self.compute_renewed+=1;
    }
    fn renew_compute_deadline(&mut self){
        for pipe in 0..hw::sizes::COMPUTE_PIPES{
            self.renew_compute_pipe_deadline(pipe);
        }
    }
    __PROGRESS__
    __CHECK_HANGS__
}
type Completions=();
fn firmware(render:bool,target:u32,previous:u32)->Firmware{
    set_now_ns(1_000_000_000);
    let mut t=[0u32;hw::sizes::COMPUTE_PIPES];t[0]=target;
    let mut f=Firmware{graph:Graph{compute:core::array::from_fn(|_|Compute{fw_stamp:Object(Cell::new(previous))}),render:Render{fw_stamp:[Object(Cell::new(previous)),Object(Cell::new(previous))]}},
        engine:Engine{flights:vec![Flight{job:Job{long:false},computes:u32::from(!render),compute_pipe:0,renders:u32::from(render),target,ta_target:target,fs_target:target,published_ns:1}],target:t,ta_target:target,fs_target:target,progress_flight:[0;hw::sizes::COMPUTE_PIPES+1],progress:[__ZERO__;hw::sizes::COMPUTE_PIPES+2]},
        recovery:Recovery{deadlines:logic::Deadlines::EXPIRED,compute_deadlines:[logic::Deadlines::EXPIRED;hw::sizes::COMPUTE_PIPES],warning_deadline_ns:[u64::MAX;2],compute_warning_deadline_ns:[u64::MAX;hw::sizes::COMPUTE_PIPES],long_commands:0,receipt_fault:false,failed:None},
        render_renewed:0,compute_renewed:0,pipe_renewed:[0;hw::sizes::COMPUTE_PIPES],fail_stops:vec![]};
    f.renew_render_deadline();f.renew_compute_deadline();f.render_renewed=0;f.compute_renewed=0;f.pipe_renewed=[0;hw::sizes::COMPUTE_PIPES];f
}
#[test]fn pending_native_zero_and_reached_are_distinct_progress_states(){
    let mut f=firmware(false,0x100,0);f.progress();assert_eq!(f.compute_renewed,0);f.progress();assert_eq!(f.compute_renewed,0);
    f.graph.compute[0].fw_stamp.0.set(0x100);f.progress();assert_eq!(f.compute_renewed,1,"completion following real wire zero renews deadline before retirement check");
    assert_eq!(f.render_renewed,0);
    f.progress();assert_eq!(f.compute_renewed,1,"repeated completed stamp is not new progress");
}
#[test]fn paired_ta_fs_zero_each_renews_once_without_lending_other_engine_progress(){
    let mut f=firmware(true,0x100,0);f.progress();
    f.graph.compute[0].fw_stamp.0.set(0x100);f.progress();assert_eq!(f.render_renewed,0);assert_eq!(f.compute_renewed,0);
    f.graph.render.fw_stamp[0].0.set(0x100);f.progress();assert_eq!(f.render_renewed,1);assert_eq!(f.compute_renewed,0);
    f.graph.render.fw_stamp[1].0.set(0x100);f.progress();assert_eq!(f.render_renewed,2);assert_eq!(f.compute_renewed,0);
    f.progress();assert_eq!(f.render_renewed,2);
}
#[test]fn before_at_and_after_zero_completions_have_the_same_progress_contract(){
    for target in [0x7fffff00u32,0x80000000,0xffffff00,0,0x100]{
        let mut f=firmware(false,target,target.wrapping_sub(0x100));f.progress();assert_eq!(f.compute_renewed,0);
        f.graph.compute[0].fw_stamp.0.set(target);f.progress();assert_eq!(f.compute_renewed,1);
        f.engine.flights[0].published_ns=2;f.progress();assert_eq!(f.compute_renewed,1,"new flight first sample is not forward progress");
    }
}
#[test]fn concurrent_compute_and_render_renew_deadlines_independently(){
    let mut f=firmware(true,0x200,0x100);
    f.engine.flights.push(Flight{job:Job{long:false},computes:1,compute_pipe:0,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target[0]=0x300;
    f.progress();assert_eq!((f.render_renewed,f.compute_renewed),(0,0));
    f.graph.compute[0].fw_stamp.0.set(0x300);f.progress();assert_eq!((f.render_renewed,f.compute_renewed),(0,1));
    f.graph.render.fw_stamp[0].0.set(0x200);f.progress();assert_eq!((f.render_renewed,f.compute_renewed),(1,1));
}
#[test]fn two_concurrent_compute_pipes_sample_progress_independently(){
    let mut f=firmware(false,0x200,0x100);
    f.engine.flights.push(Flight{job:Job{long:false},computes:1,compute_pipe:1,renders:0,target:0x200,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target[0]=0x200;f.engine.target[1]=0x200;
    f.progress();assert_eq!(f.compute_renewed,0);
    // Pipe 1 advances while pipe 0 stays pending -> renews compute deadline once.
    f.graph.compute[1].fw_stamp.0.set(0x200);
    f.progress();assert_eq!(f.compute_renewed,1);
    f.progress();assert_eq!(f.compute_renewed,1);
    // Pipe 0 advances independently -> renews compute deadline a second time.
    f.graph.compute[0].fw_stamp.0.set(0x200);
    f.progress();assert_eq!(f.compute_renewed,2);
}
#[test]fn stuck_render_times_out_while_compute_keeps_advancing_and_retiring(){
    let mut f=firmware(true,0x200,0x100);
    f.engine.flights.push(Flight{job:Job{long:false},computes:1,compute_pipe:0,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target[0]=0x300;
    let mut done=();
    assert_eq!(f.check_hangs(&mut done),Ok(()));
    // Compute keeps advancing and retiring every 400 ms across 1.2 s while render stays stuck at 0x100.
    for step in 1u64..=3 {
        set_now_ns(1_000_000_000 + step * 400_000_000);
        let next_target = 0x300 + (step as u32) * 0x100;
        f.graph.compute[0].fw_stamp.0.set(next_target - 0x100);
        f.engine.flights[1].target = next_target;
        f.engine.flights[1].published_ns = 2 + step;
        f.engine.target[0] = next_target;
        f.renew_compute_deadline();
        let res = f.check_hangs(&mut done);
        if step < 3 {
            assert_eq!(res, Ok(()), "render deadline (1s) not yet expired at step {step}");
            assert_eq!(f.render_renewed, 0, "compute progress/retirement must never renew render deadline");
        } else {
            assert_eq!(res, Err(ETIMEDOUT), "stuck render must fail-stop at 1.2s despite active compute");
            assert_eq!(f.fail_stops, [ETIMEDOUT]);
        }
    }
}
#[test]fn stuck_compute_times_out_while_render_keeps_advancing_and_retiring(){
    let mut f=firmware(false,0x200,0x100);
    f.engine.flights.push(Flight{job:Job{long:false},computes:0,compute_pipe:0,renders:1,target:0,ta_target:0x300,fs_target:0x300,published_ns:2});
    f.engine.ta_target=0x300;f.engine.fs_target=0x300;
    let mut done=();
    assert_eq!(f.check_hangs(&mut done),Ok(()));
    // Render keeps advancing and retiring every 2 s across 6 s while compute stays stuck at 0x100.
    for step in 1u64..=3 {
        set_now_ns(1_000_000_000 + step * 2_000_000_000);
        let next_target = 0x300 + (step as u32) * 0x100;
        f.graph.render.fw_stamp[0].0.set(next_target - 0x100);
        f.graph.render.fw_stamp[1].0.set(next_target - 0x100);
        f.engine.flights[1].ta_target = next_target;
        f.engine.flights[1].fs_target = next_target;
        f.engine.flights[1].published_ns = 2 + step;
        f.engine.ta_target = next_target;
        f.engine.fs_target = next_target;
        f.renew_render_deadline();
        let res = f.check_hangs(&mut done);
        if step < 3 {
            assert_eq!(res, Ok(()), "compute deadline (5s) not yet expired at step {step}");
            assert_eq!(f.compute_renewed, 0, "render progress/retirement must never renew compute deadline");
        } else {
            assert_eq!(res, Err(ETIMEDOUT), "stuck compute must fail-stop at 6s despite active render");
            assert_eq!(f.fail_stops, [ETIMEDOUT]);
        }
    }
}
#[test]fn stuck_compute_pipe_times_out_while_other_compute_pipe_advances_and_retires(){
    // Pipe 0 has a normal flight stuck at 0x100 (5s deadline); pipe 1 runs a long job that
    // keeps advancing and retiring every 2s across 6s. Pipe 1 progress/retirement must never
    // renew pipe 0's deadline or grant pipe 0 the 10s long-job allowance.
    let mut f=firmware(false,0x200,0x100);
    f.engine.flights.push(Flight{job:Job{long:true},computes:1,compute_pipe:1,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target[1]=0x300;
    let mut done=();
    assert_eq!(f.check_hangs(&mut done),Ok(()));
    for step in 1u64..=3 {
        set_now_ns(1_000_000_000 + step * 2_000_000_000);
        let next_target = 0x300 + (step as u32) * 0x100;
        f.graph.compute[1].fw_stamp.0.set(next_target - 0x100);
        f.engine.flights[1].target = next_target;
        f.engine.flights[1].published_ns = 2 + step;
        f.engine.target[1] = next_target;
        f.renew_compute_pipe_deadline(1);
        let res = f.check_hangs(&mut done);
        if step < 3 {
            assert_eq!(res, Ok(()), "pipe 0 deadline (5s) not yet expired at step {step}");
            assert_eq!(f.pipe_renewed[0], 0, "pipe 1 activity must never renew pipe 0 deadline");
            assert!(f.pipe_renewed[1] > 0);
        } else {
            assert_eq!(res, Err(ETIMEDOUT), "stuck pipe 0 must fail-stop at 6s despite active long pipe 1");
            assert_eq!(f.fail_stops, [ETIMEDOUT]);
        }
    }
}
#[test]fn younger_long_job_on_same_compute_pipe_does_not_extend_stalled_normal_head(){
    // Pipe 0 has a normal flight at index 0 (5s deadline) and a younger long flight at index 1
    // on the same pipe (10s allowance). Only the oldest active flight on a pipe selects its
    // deadline, so the stalled normal head still times out at 6s instead of inheriting 10s.
    let mut f=firmware(false,0x200,0x100);
    f.engine.flights.push(Flight{job:Job{long:true},computes:1,compute_pipe:0,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target[0]=0x300;
    let mut done=();
    set_now_ns(1_000_000_000 + 6_000_000_000);
    assert_eq!(f.check_hangs(&mut done), Err(ETIMEDOUT));
    assert_eq!(f.fail_stops, [ETIMEDOUT]);
    // Conversely, when the oldest flight on pipe 0 IS a long job and a younger normal job is
    // queued behind it on the same pipe, pipe 0 keeps its 10s long-job allowance at 6s.
    let mut fl=firmware(false,0x200,0x100);
    fl.engine.flights[0].job.long=true;
    fl.engine.flights.push(Flight{job:Job{long:false},computes:1,compute_pipe:0,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    fl.engine.target[0]=0x300;
    set_now_ns(1_000_000_000 + 6_000_000_000);
    assert_eq!(fl.check_hangs(&mut done), Ok(()));
    assert!(fl.fail_stops.is_empty());
}
'''.replace('__LOGIC__',str(backend/'recovery/logic.rs')).replace('__PROGRESS__',function(source,'progress').replace('super::hw::','hw::')).replace('__CHECK_HANGS__',function(recovery_src,'check_hangs').replace('super::command::','command::').replace('super::hw::','hw::')).replace('__TYPE__','Option<u32>' if option else 'u32').replace('__ZERO__','None' if option else '0')
unit=out/'progress-wrap.rs';unit.write_text(harness);log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'progress-wrap')],[str(out/'progress-wrap'),'--nocapture']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);log+=result.stdout;print(result.stdout,end='');(out/'progress-wrap.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'progress-wrap.json').write_text(json.dumps({'passed':True,'cpu_only':True,'tests':11,'scope':'Copied production progress and modular comparison logic; mapped-register reads and deadline renewal mocked. No GPU or real deadline expiration.','source_sha256':{name:sha(backend/name)for name in ['engine.rs','poll.rs','recovery/logic.rs']}},indent=2)+'\n')
