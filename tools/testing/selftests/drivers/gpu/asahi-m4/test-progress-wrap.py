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
option='progress: [Option<u32>; 3]' in engine
harness=r'''
#![allow(dead_code)]
use std::cell::Cell;
#[path="__LOGIC__"]mod logic;
use logic::stamp_reached;
#[derive(Copy,Clone,Debug,PartialEq,Eq)]struct Error(i32);
const EIO:Error=Error(5);const ETIMEDOUT:Error=Error(110);
type Result<T=()>=core::result::Result<T,Error>;
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
struct Compute{fw_stamp:Object}struct Render{fw_stamp:[Object;2]}struct Graph{compute:Compute,render:Render}
struct Flight{computes:u32,renders:u32,target:u32,ta_target:u32,fs_target:u32,published_ns:u64}
struct Engine{flights:Vec<Flight>,target:u32,ta_target:u32,fs_target:u32,progress_flight:[u64;2],progress:[__TYPE__;3]}
impl Engine{fn oldest(&self)->Option<&Flight>{self.flights.first()}}
struct Recovery{deadline_ns:u64,compute_deadline_ns:u64,warning_deadline_ns:[u64;2],long_commands:u64,receipt_fault:bool,failed:Option<Error>}
struct Firmware{graph:Graph,engine:Engine,recovery:Recovery,render_renewed:u32,compute_renewed:u32,fail_stops:Vec<Error>}
impl Firmware{
    fn dev(&self){}
    fn transport_error(&self)->Option<Error>{None}
    fn recovery_pending(&self)->Result<bool>{Ok(false)}
    fn recover(&mut self)->Result{Ok(())}
    fn fail_stop(&mut self,error:Error,_done:&mut ()){self.recovery.failed=Some(error);self.fail_stops.push(error);}
    fn renew_render_deadline(&mut self){
        let now=now_ns();
        self.recovery.deadline_ns=logic::renew(now,params::deadline_ms(),0).0;
        self.recovery.warning_deadline_ns[command::RENDER]=now.saturating_add(5_000_000_000);
        self.render_renewed+=1;
    }
    fn renew_compute_deadline(&mut self){
        let now=now_ns();
        self.recovery.compute_deadline_ns=logic::renew(now,0,params::compute_deadline_ms()).1;
        self.recovery.warning_deadline_ns[command::COMPUTE]=now.saturating_add(5_000_000_000);
        self.compute_renewed+=1;
    }
    __PROGRESS__
    __CHECK_HANGS__
}
type Completions=();
fn firmware(render:bool,target:u32,previous:u32)->Firmware{
    set_now_ns(1_000_000_000);
    let mut f=Firmware{graph:Graph{compute:Compute{fw_stamp:Object(Cell::new(previous))},render:Render{fw_stamp:[Object(Cell::new(previous)),Object(Cell::new(previous))]}},
        engine:Engine{flights:vec![Flight{computes:u32::from(!render),renders:u32::from(render),target,ta_target:target,fs_target:target,published_ns:1}],target,ta_target:target,fs_target:target,progress_flight:[0;2],progress:[__ZERO__;3]},
        recovery:Recovery{deadline_ns:u64::MAX,compute_deadline_ns:u64::MAX,warning_deadline_ns:[u64::MAX;2],long_commands:0,receipt_fault:false,failed:None},
        render_renewed:0,compute_renewed:0,fail_stops:vec![]};
    f.renew_render_deadline();f.renew_compute_deadline();f.render_renewed=0;f.compute_renewed=0;f
}
#[test]fn pending_native_zero_and_reached_are_distinct_progress_states(){
    let mut f=firmware(false,0x100,0);f.progress();assert_eq!(f.compute_renewed,0);f.progress();assert_eq!(f.compute_renewed,0);
    f.graph.compute.fw_stamp.0.set(0x100);f.progress();assert_eq!(f.compute_renewed,1,"completion following real wire zero renews deadline before retirement check");
    assert_eq!(f.render_renewed,0);
    f.progress();assert_eq!(f.compute_renewed,1,"repeated completed stamp is not new progress");
}
#[test]fn paired_ta_fs_zero_each_renews_once_without_lending_other_engine_progress(){
    let mut f=firmware(true,0x100,0);f.progress();
    f.graph.compute.fw_stamp.0.set(0x100);f.progress();assert_eq!(f.render_renewed,0);assert_eq!(f.compute_renewed,0);
    f.graph.render.fw_stamp[0].0.set(0x100);f.progress();assert_eq!(f.render_renewed,1);assert_eq!(f.compute_renewed,0);
    f.graph.render.fw_stamp[1].0.set(0x100);f.progress();assert_eq!(f.render_renewed,2);assert_eq!(f.compute_renewed,0);
    f.progress();assert_eq!(f.render_renewed,2);
}
#[test]fn before_at_and_after_zero_completions_have_the_same_progress_contract(){
    for target in [0x7fffff00u32,0x80000000,0xffffff00,0,0x100]{
        let mut f=firmware(false,target,target.wrapping_sub(0x100));f.progress();assert_eq!(f.compute_renewed,0);
        f.graph.compute.fw_stamp.0.set(target);f.progress();assert_eq!(f.compute_renewed,1);
        f.engine.flights[0].published_ns=2;f.progress();assert_eq!(f.compute_renewed,1,"new flight first sample is not forward progress");
    }
}
#[test]fn concurrent_compute_and_render_renew_deadlines_independently(){
    let mut f=firmware(true,0x200,0x100);
    f.engine.flights.push(Flight{computes:1,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target=0x300;
    f.progress();assert_eq!((f.render_renewed,f.compute_renewed),(0,0));
    f.graph.compute.fw_stamp.0.set(0x300);f.progress();assert_eq!((f.render_renewed,f.compute_renewed),(0,1));
    f.graph.render.fw_stamp[0].0.set(0x200);f.progress();assert_eq!((f.render_renewed,f.compute_renewed),(1,1));
}
#[test]fn stuck_render_times_out_while_compute_keeps_advancing_and_retiring(){
    let mut f=firmware(true,0x200,0x100);
    f.engine.flights.push(Flight{computes:1,renders:0,target:0x300,ta_target:0,fs_target:0,published_ns:2});
    f.engine.target=0x300;
    let mut done=();
    assert_eq!(f.check_hangs(&mut done),Ok(()));
    // Compute keeps advancing and retiring every 400 ms across 1.2 s while render stays stuck at 0x100.
    for step in 1u64..=3 {
        set_now_ns(1_000_000_000 + step * 400_000_000);
        let next_target = 0x300 + (step as u32) * 0x100;
        f.graph.compute.fw_stamp.0.set(next_target - 0x100);
        f.engine.flights[1].target = next_target;
        f.engine.flights[1].published_ns = 2 + step;
        f.engine.target = next_target;
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
    f.engine.flights.push(Flight{computes:0,renders:1,target:0,ta_target:0x300,fs_target:0x300,published_ns:2});
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
'''.replace('__LOGIC__',str(backend/'recovery/logic.rs')).replace('__PROGRESS__',function(source,'progress')).replace('__CHECK_HANGS__',function(recovery_src,'check_hangs').replace('super::command::','command::')).replace('__TYPE__','Option<u32>' if option else 'u32').replace('__ZERO__','None' if option else '0')
unit=out/'progress-wrap.rs';unit.write_text(harness);log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'progress-wrap')],[str(out/'progress-wrap'),'--nocapture']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);log+=result.stdout;print(result.stdout,end='');(out/'progress-wrap.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'progress-wrap.json').write_text(json.dumps({'passed':True,'cpu_only':True,'tests':11,'scope':'Copied production progress and modular comparison logic; mapped-register reads and deadline renewal mocked. No GPU or real deadline expiration.','source_sha256':{name:sha(backend/name)for name in ['engine.rs','poll.rs','recovery/logic.rs']}},indent=2)+'\n')
