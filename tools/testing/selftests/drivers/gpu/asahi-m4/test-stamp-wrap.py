#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run native wrapped stamps through production encoding and full receipt decisions."""
from common import kernel_root, output_dir, rustc, sha
import json
import subprocess

root = kernel_root()
out = output_dir()
backend = root / 'drivers/gpu/drm/asahi/g16'

def function(text, name):
    marker = text.index('fn ' + name + '(')
    start = text.rfind('\n', 0, marker) + 1
    at = text.index('{', marker)
    depth, end = 1, at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

harness = r'''
#![allow(dead_code)]
use std::collections::BTreeMap;
#[derive(Copy,Clone,Debug,PartialEq,Eq)]struct Error(i32);
const EINVAL:Error=Error(-22);const EAGAIN:Error=Error(-11);type Result<T=()> = std::result::Result<T,Error>;
#[path="__LOGIC__"]mod logic;
use logic::Retirement;
mod encode { __STAMP_SEED__ __STAMP__ }
struct Object{cells:BTreeMap<usize,u64>,va:u64,unstable:bool}
impl Default for Object{fn default()->Self{Self{cells:BTreeMap::new(),va:0x1234,unstable:false}}}
impl Object {
    fn r32(&self,off:usize)->Result<u32>{Ok(*self.cells.get(&off).unwrap_or(&0) as u32)}
    fn r64(&self,off:usize)->Result<u64>{if self.unstable{return Err(EAGAIN)}Ok(*self.cells.get(&off).unwrap_or(&0))}
    fn set(&mut self,off:usize,v:u64){self.cells.insert(off,v);}
}
#[derive(Clone,Copy)]enum Channel{Compute,Ta,Fs}
mod notifier_page {pub const COMPUTE_STAMP:usize=0;pub const COMPUTE_CONTEXT:usize=4;pub const TA_STAMP:usize=8;pub const FS_STAMP:usize=12;pub const FS_RECORD:usize=32;}
mod notifier {pub const CONTEXT:usize=16;}
mod compute {pub const DESCRIPTOR_COMPLETED:usize=0;pub const DESCRIPTOR_COUNTER:usize=8;}
struct CGraph{fw_stamp:Object,qstate:Object,qinfo:Object,notifier:Object}
struct RGraph{fw_stamp:[Object;2],qstate:[Object;2],qinfo:[Object;2],notifier:Object}
struct Graph{compute:CGraph,render:RGraph}
struct Vm{slot:u32}struct Job{vm:Vm,resources:Resources}
struct Resources{compute:Vec<CUnit>,passes:Vec<RUnit>,pools:Pools}
impl Resources{fn compute(&self)->Result<&Self>{Ok(self)}fn render(&self)->Result<&Self>{Ok(self)}}
struct Claims{slot:[u32;1],pass:[u32;1]}
struct Flight{computes:u32,renders:u32,target:u32,ta_target:u32,fs_target:u32,job:Job,claims:Claims,head:u32,ta_head:u32,fs_head:u32,rec:[u32;3],recovery_suspect:bool,recovery_started_ns:u64,doomed:bool}
struct Engine{flights:Vec<Flight>}
impl Engine{fn oldest(&self)->Option<&Flight>{self.flights.first()}}
struct CUnit{counter:Object,its:Object,work:Object}
struct RUnit{scene:Object}
struct Scrub{compute:Vec<CUnit>,passes:Vec<RUnit>}
struct Slots{scrub:Scrub}
struct Pools{full:bool}
impl Pools{fn retired(&self,_k:usize)->Result<bool>{Ok(self.full)}}
struct Firmware{graph:Graph,engine:Engine,slots:Slots,pools:Pools,queue_passed:bool}
fn rmb(){}fn now_ns()->u64{100_000_000}
impl Firmware{
    fn queue_receipt(&self,_ch:Channel,_state:&Object,_info:&Object,_head:u32,_rec:u32)->Result<bool>{Ok(self.queue_passed)}
    __RETIRED__
    __RETIRED_SNAPSHOT__
}
fn firmware(render:bool,ordinal:u64)->Firmware{
    let target=encode::stamp(ordinal);
    let mut f=Firmware{
        graph:Graph{compute:CGraph{fw_stamp:Object::default(),qstate:Object::default(),qinfo:Object::default(),notifier:Object::default()},
            render:RGraph{fw_stamp:Default::default(),qstate:Default::default(),qinfo:Default::default(),notifier:Object::default()}},
        engine:Engine{flights:vec![Flight{computes:u32::from(!render),renders:u32::from(render),target,ta_target:target,fs_target:target,job:Job{vm:Vm{slot:2},resources:Resources{compute:vec![CUnit{counter:Object::default(),its:Object::default(),work:Object::default()}],passes:vec![RUnit{scene:Object::default()}],pools:Pools{full:true}}},claims:Claims{slot:[0],pass:[0]},head:1,ta_head:1,fs_head:1,rec:[1;3],recovery_suspect:false,recovery_started_ns:0,doomed:false}]},
        slots:Slots{scrub:Scrub{compute:vec![CUnit{counter:Object::default(),its:Object::default(),work:Object::default()}],passes:vec![RUnit{scene:Object::default()}]}},
        pools:Pools{full:true},queue_passed:true,
    };
    f.graph.compute.fw_stamp.set(0,u64::from(target));
    f.graph.compute.notifier.set(notifier_page::COMPUTE_STAMP,u64::from(target));
    f.graph.compute.notifier.set(notifier_page::COMPUTE_CONTEXT,2);
    f.engine.flights[0].job.resources.compute[0].counter.set(0,ordinal);
    f.engine.flights[0].job.resources.compute[0].its.set(0,100);f.engine.flights[0].job.resources.compute[0].its.set(8,101);
    f.engine.flights[0].job.resources.compute[0].work.set(compute::DESCRIPTOR_COMPLETED,ordinal);
    f.engine.flights[0].job.resources.compute[0].work.set(compute::DESCRIPTOR_COUNTER,0x1234);
    for stage in 0..2{f.graph.render.fw_stamp[stage].set(0,u64::from(target));}
    f.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(target));
    f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(target));
    f.graph.render.notifier.set(notifier::CONTEXT,2);
    f.graph.render.notifier.set(notifier_page::FS_RECORD+notifier::CONTEXT,2);
    for (off,time) in [(0x600,100),(0x608,101),(0x620,102),(0x628,103)]{f.engine.flights[0].job.resources.passes[0].scene.set(off,time);}
    f
}
#[test]fn native_stamps_cross_both_top_bits_and_zero_without_host_ordinal_reuse(){
    for n in [0x3fffff,0x400000,0x7fffff,0x800000,0xbfffff,0xc00000,0xffffff,0x1000000,0x1000001] {
        assert_eq!(encode::stamp(n),((n as u32).wrapping_mul(0x100)));
        assert_eq!(logic::next_ordinal(n),Some(n+1));
        assert!(!logic::stamp_reached(encode::stamp(n-1),encode::stamp(n),encode::stamp(n)));
        assert!(logic::stamp_reached(encode::stamp(n),encode::stamp(n),encode::stamp(n)));
    }
}
#[test]fn wrapped_compute_and_paired_ta_fs_require_all_normal_receipt_evidence(){
    for render in [false,true]{for n in [0x7fffff,0x800000,0xffffff,0x1000000,0x1000001]{
        let mut f=firmware(render,n);
        assert_eq!(f.retired(),Ok(Retirement::Full));
        f.queue_passed=false;assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.queue_passed=true;
        if render {
            f.graph.render.fw_stamp[0].set(0,u64::from(encode::stamp(n-1)));
            assert_eq!(f.retired(),Ok(Retirement::Pending));
            f.graph.render.fw_stamp[0].set(0,u64::from(encode::stamp(n)));
            f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(n-1)));
            assert_eq!(f.retired(),Ok(Retirement::Pending));
            f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(n)));
            f.engine.flights[0].job.resources.pools.full=false;assert_eq!(f.retired(),Ok(Retirement::Pending));
        }else{
            f.engine.flights[0].job.resources.compute[0].its.set(8,0);
            assert_eq!(f.retired(),Ok(Retirement::Pending));
        }
    }}
}
#[test]fn changing_packed_receipt_remains_pending_and_stable_retry_can_retire(){
    let mut f=firmware(false,1);f.engine.flights[0].job.resources.compute[0].its.unstable=true;
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    f.engine.flights[0].job.resources.compute[0].its.unstable=false;
    assert_eq!(f.retired(),Ok(Retirement::Full));
}

'''.replace('__LOGIC__', str(backend/'recovery/logic.rs'))
for token, path, name in [('__STAMP_SEED__','encode.rs','stamp_seed'),('__STAMP__','encode.rs','stamp'),('__RETIRED__','poll.rs','retired'),('__RETIRED_SNAPSHOT__','poll.rs','retired_snapshot')]:
    harness=harness.replace(token,function((backend/path).read_text(),name))
unit=out/'stamp-wrap.rs';unit.write_text(harness)
log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'stamp-wrap')],[str(out/'stamp-wrap'),'--nocapture']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    log+=result.stdout;print(result.stdout,end='');(out/'stamp-wrap.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'stamp-wrap.json').write_text(json.dumps({'passed':True,'cpu_only':True,'scope':'Production stamp builders, modular comparisons and full receipt decisions; queue/DMA/MMIO transport mocked. Tests around top-bit/zero wrap and partial paired TA/FS receipts. Firmware behavior not hardware-qualified.','source_sha256':{name:sha(backend/name)for name in ['encode.rs','recovery/logic.rs','poll.rs']}},indent=2)+'\n')
