#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the copied production progress sampler across native zero wrap."""
from common import kernel_root, output_dir, rustc, sha
import subprocess,json
root=kernel_root();out=output_dir();backend=root/'drivers/gpu/drm/asahi/g16'
source=(backend/'poll.rs').read_text();engine=(backend/'engine.rs').read_text()
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
struct Object(Cell<u32>);impl Object{fn r32(&self,_off:usize)->Result<u32,()>{Ok(self.0.get())}}
struct Compute{fw_stamp:Object}struct Render{fw_stamp:[Object;2]}struct Graph{compute:Compute,render:Render}
struct Flight{computes:u32,renders:u32,target:u32,ta_target:u32,fs_target:u32,published_ns:u64}
struct Engine{flight:Flight,target:u32,ta_target:u32,fs_target:u32,progress_flight:u64,progress:[__TYPE__;3]}
impl Engine{fn oldest(&self)->Option<&Flight>{Some(&self.flight)}}
struct Firmware{graph:Graph,engine:Engine,renewed:u32}
impl Firmware{fn renew_deadlines(&mut self){self.renewed+=1;} __PROGRESS__ }
fn firmware(render:bool,target:u32,previous:u32)->Firmware{
    Firmware{graph:Graph{compute:Compute{fw_stamp:Object(Cell::new(previous))},render:Render{fw_stamp:[Object(Cell::new(previous)),Object(Cell::new(previous))]}},
        engine:Engine{flight:Flight{computes:u32::from(!render),renders:u32::from(render),target,ta_target:target,fs_target:target,published_ns:1},target,ta_target:target,fs_target:target,progress_flight:0,progress:[__ZERO__;3]},renewed:0}
}
#[test]fn pending_native_zero_and_reached_are_distinct_progress_states(){
    let mut f=firmware(false,0x100,0);f.progress();assert_eq!(f.renewed,0);f.progress();assert_eq!(f.renewed,0);
    f.graph.compute.fw_stamp.0.set(0x100);f.progress();assert_eq!(f.renewed,1,"completion following real wire zero renews deadline before retirement check");
    f.progress();assert_eq!(f.renewed,1,"repeated completed stamp is not new progress");
}
#[test]fn paired_ta_fs_zero_each_renews_once_without_lending_other_engine_progress(){
    let mut f=firmware(true,0x100,0);f.progress();
    f.graph.compute.fw_stamp.0.set(0x100);f.progress();assert_eq!(f.renewed,0);
    f.graph.render.fw_stamp[0].0.set(0x100);f.progress();assert_eq!(f.renewed,1);
    f.graph.render.fw_stamp[1].0.set(0x100);f.progress();assert_eq!(f.renewed,2);f.progress();assert_eq!(f.renewed,2);
}
#[test]fn before_at_and_after_zero_completions_have_the_same_progress_contract(){
    for target in [0x7fffff00u32,0x80000000,0xffffff00,0,0x100]{
        let mut f=firmware(false,target,target.wrapping_sub(0x100));f.progress();assert_eq!(f.renewed,0);
        f.graph.compute.fw_stamp.0.set(target);f.progress();assert_eq!(f.renewed,1);
        f.engine.flight.published_ns=2;f.progress();assert_eq!(f.renewed,1,"new flight first sample is not forward progress");
    }
}
'''.replace('__LOGIC__',str(backend/'recovery/logic.rs')).replace('__PROGRESS__',function(source,'progress')).replace('__TYPE__','Option<u32>' if option else 'u32').replace('__ZERO__','None' if option else '0')
unit=out/'progress-wrap.rs';unit.write_text(harness);log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'progress-wrap')],[str(out/'progress-wrap'),'--nocapture']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);log+=result.stdout;print(result.stdout,end='');(out/'progress-wrap.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'progress-wrap.json').write_text(json.dumps({'passed':True,'cpu_only':True,'tests':11,'scope':'Copied production progress and modular comparison logic; mapped-register reads and deadline renewal mocked. No GPU or real deadline expiration.','source_sha256':{name:sha(backend/name)for name in ['engine.rs','poll.rs','recovery/logic.rs']}},indent=2)+'\n')
