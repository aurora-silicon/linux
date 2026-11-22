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
use std::cell::Cell;
use std::collections::BTreeMap;
#[derive(Copy,Clone,Debug,PartialEq,Eq)]struct Error(i32);
const EINVAL:Error=Error(-22);const EAGAIN:Error=Error(-11);type Result<T=()> = std::result::Result<T,Error>;
#[path="__LOGIC__"]mod logic;
use logic::Retirement;
mod encode { __STAMP_SEED__ __STAMP__ }
struct Object{cells:BTreeMap<usize,u64>,va:u64,unstable:bool,reads:Cell<u32>}
impl Default for Object{fn default()->Self{Self{cells:BTreeMap::new(),va:0x1234,unstable:false,reads:Cell::new(0)}}}
impl Object {
    fn r32(&self,off:usize)->Result<u32>{self.reads.set(self.reads.get()+1);Ok(*self.cells.get(&off).unwrap_or(&0) as u32)}
    fn r64(&self,off:usize)->Result<u64>{self.reads.set(self.reads.get()+1);if self.unstable{return Err(EAGAIN)}Ok(*self.cells.get(&off).unwrap_or(&0))}
    fn set(&mut self,off:usize,v:u64){self.cells.insert(off,v);}
    fn reset_reads(&self){self.reads.set(0);}
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
impl Resources{fn compute(&self)->Result<&Self>{Ok(self)}fn render(&self)->Result<&Self>{Ok(self)}fn compute_unit(&self,k:usize)->Result<&CUnit>{self.compute.get(k).ok_or(EINVAL)}fn render_pass(&self,k:usize)->Result<&RUnit>{self.passes.get(k).ok_or(EINVAL)}fn pb_retired(&self,k:usize)->Result<bool>{self.pools.retired(k)}fn pb_retired_at(&self,k:usize,target:u64)->Result<bool>{self.pools.retired_at(k,target)}}
struct Claims{slot:[u32;16],slot_count:u32,pass:[u32;1]}
struct Flight{computes:u32,renders:u32,target:u32,ta_target:u32,fs_target:u32,job:Job,claims:Claims,head:u32,ta_head:u32,fs_head:u32,rec:[u32;3],pb_slot:u32,uma_submitted:u64,csubmitted:[u64;16],recovery_suspect:bool,recovery_started_ns:u64,doomed:bool}
struct Engine{flights:Vec<Flight>,target:u32,ta_target:u32,fs_target:u32,retired_ticks:[u64;4]}
fn stamp_reached(observed:u32,own:u32,tail:u32)->bool{logic::stamp_reached(observed,own,tail)}
impl Engine{fn oldest(&self)->Option<&Flight>{self.flights.first()}}
struct CUnit{counter:Object,its:Object,work:Object}
struct RUnit{scene:Object}
struct Scrub{compute:Vec<CUnit>,passes:Vec<RUnit>}
struct Slots{scrub:Scrub}
struct Pools{full:bool,completed:u64}
impl Pools{fn retired(&self,_k:usize)->Result<bool>{Ok(self.full)}fn retired_at(&self,_k:usize,target:u64)->Result<bool>{Ok(self.full && target!=0 && self.completed>=target)}}
struct Firmware{graph:Graph,engine:Engine,slots:Slots,pools:Pools,queue_passed:bool}
fn rmb(){}fn now_ns()->u64{100_000_000}
impl Firmware{
    fn queue_receipt(&self,_ch:Channel,_state:&Object,_info:&Object,_head:u32,_rec:u32)->Result<bool>{Ok(self.queue_passed)}
    fn queue_passed(&self,_ch:Channel,_state:&Object,_info:&Object,_f:&Flight)->Result<bool>{Ok(self.queue_passed)}
    // The ITS-pair ticks (25G83, and the first choice on 26B123): the receipt tests set them.
    fn compute_ticks(&self,unit:&CUnit)->Result<(u64,u64)>{Ok((unit.its.r64(0)?,unit.its.r64(8)?))}
    __RETIRED__
    __RETIRED_AT__
    __RETIRED_FLIGHT__
    __RETIRED_FLIGHT_CLASSES__
    __RETIRED_SNAPSHOT__
    __RETIRED_SNAPSHOT_AT__
}
fn firmware(render:bool,ordinal:u64)->Firmware{
    let target=encode::stamp(ordinal);
    let mut f=Firmware{
        graph:Graph{compute:CGraph{fw_stamp:Object::default(),qstate:Object::default(),qinfo:Object::default(),notifier:Object::default()},
            render:RGraph{fw_stamp:Default::default(),qstate:Default::default(),qinfo:Default::default(),notifier:Object::default()}},
        engine:Engine{flights:vec![Flight{computes:u32::from(!render),renders:u32::from(render),target,ta_target:target,fs_target:target,job:Job{vm:Vm{slot:2},resources:Resources{compute:vec![CUnit{counter:Object::default(),its:Object::default(),work:Object::default()}],passes:vec![RUnit{scene:Object::default()}],pools:Pools{full:true,completed:2}}},claims:Claims{slot:[0;16],slot_count:u32::from(!render),pass:[0]},head:1,ta_head:1,fs_head:1,rec:[1;3],pb_slot:0,uma_submitted:2,csubmitted:{let mut a=[0u64;16];if !render{a[0]=ordinal;}a},recovery_suspect:false,recovery_started_ns:0,doomed:false}],target,ta_target:target,fs_target:target,retired_ticks:[0;4]},
        slots:Slots{scrub:Scrub{compute:vec![CUnit{counter:Object::default(),its:Object::default(),work:Object::default()}],passes:vec![RUnit{scene:Object::default()}]}},
        pools:Pools{full:true,completed:2},queue_passed:true,
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
#[test]fn queued_compute_flight_retires_on_passed_receipts_in_order(){
    for n in [0x7fffff,0x800000,0xffffff,0x1000000,0x1000001]{
        let mut f=firmware(false,n);
        let later=firmware(false,n+1).engine.flights.remove(0);
        f.engine.flights.push(later);f.engine.target=encode::stamp(n+1);
        // The command behind it finished too: the shared stamp and notifier cells name it.
        f.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(n+1)));
        f.graph.compute.notifier.set(notifier_page::COMPUTE_STAMP,u64::from(encode::stamp(n+1)));
        f.graph.compute.notifier.set(notifier_page::COMPUTE_CONTEXT,9);
        assert_eq!(f.retired(),Ok(Retirement::Full));
        f.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(n-1)));
        assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(n)));
        assert_eq!(f.retired(),Ok(Retirement::Full));
        f.queue_passed=false;assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.queue_passed=true;f.engine.flights[0].job.resources.compute[0].work.set(compute::DESCRIPTOR_COMPLETED,n-1);
        assert_eq!(f.retired(),Ok(Retirement::Pending));
    }
}
#[test]fn queued_render_flight_retires_on_passed_receipts_in_order(){
    for n in [0x7fffff,0x800000,0xffffff,0x1000000,0x1000001]{
        let mut f=firmware(true,n);
        let mut later=firmware(true,n+1).engine.flights.remove(0);
        later.uma_submitted=4;
        f.engine.flights.push(later);
        f.engine.ta_target=encode::stamp(n+1);
        f.engine.fs_target=encode::stamp(n+1);
        // The render behind it finished too: shared TA/FS stamps and notifier moved on.
        for stage in 0..2 { f.graph.render.fw_stamp[stage].set(0,u64::from(encode::stamp(n+1))); }
        f.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(n+1)));
        f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(n+1)));
        f.graph.render.notifier.set(notifier::CONTEXT,9);
        f.graph.render.notifier.set(notifier_page::FS_RECORD+notifier::CONTEXT,9);
        f.engine.flights[0].job.resources.pools.completed=4;
        assert_eq!(f.retired(),Ok(Retirement::Full));
        // Unreached FS stamp keeps it pending.
        f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(n-1)));
        assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(n)));
        assert_eq!(f.retired(),Ok(Retirement::Full));
        // Unreached UMA completion keeps it pending.
        f.engine.flights[0].job.resources.pools.completed=1;
        assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.engine.flights[0].job.resources.pools.completed=2;
        assert_eq!(f.retired(),Ok(Retirement::Full));
        // Unpassed TA/FS queue keeps it pending.
        f.queue_passed=false;
        assert_eq!(f.retired(),Ok(Retirement::Pending));
    }
}
#[test]fn changing_packed_receipt_remains_pending_and_stable_retry_can_retire(){
    let mut f=firmware(false,1);f.engine.flights[0].job.resources.compute[0].its.unstable=true;
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    f.engine.flights[0].job.resources.compute[0].its.unstable=false;
    assert_eq!(f.retired(),Ok(Retirement::Full));
    assert_eq!(f.engine.retired_ticks,[100,101,0,0]);
    let mut r=firmware(true,1);
    assert_eq!(r.retired(),Ok(Retirement::Full));
    assert_eq!(r.engine.retired_ticks,[100,101,102,103]);
}

#[test]fn concurrent_compute_and_render_retire_independently_by_class(){
    // Render at index 0 (pending) + Compute at index 1 (complete): compute retires ahead of render.
    let mut f=firmware(true,10);
    let c=firmware(false,20).engine.flights.remove(0);
    f.engine.flights.push(c);
    f.engine.target=encode::stamp(20);
    f.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(20)));
    f.graph.compute.notifier.set(notifier_page::COMPUTE_STAMP,u64::from(encode::stamp(20)));
    f.graph.compute.notifier.set(notifier_page::COMPUTE_CONTEXT,2);
    // Keep render FS stamp unreached so render at index 0 stays pending.
    f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(9)));
    assert_eq!(f.retired_flight(),Ok(Some((1,Retirement::Full))));
    f.engine.flights.remove(1);
    assert_eq!(f.retired_flight(),Ok(None));
    // Now finish render: render retires cleanly with exact single-class receipt.
    f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(10)));
    assert_eq!(f.retired_flight(),Ok(Some((0,Retirement::Full))));
    // Reverse order: Compute at index 0 (pending) + Render at index 1 (complete).
    let mut f2=firmware(false,30);
    let r=firmware(true,40).engine.flights.remove(0);
    f2.engine.flights.push(r);
    f2.engine.ta_target=encode::stamp(40);
    f2.engine.fs_target=encode::stamp(40);
    for stage in 0..2 { f2.graph.render.fw_stamp[stage].set(0,u64::from(encode::stamp(40))); }
    f2.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(40)));
    f2.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(40)));
    f2.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(29)));
    assert_eq!(f2.retired_flight(),Ok(Some((1,Retirement::Full))));
}
#[test]fn multi_compute_horizon_requires_every_claimed_unit_receipt(){
    let mut f=firmware(false,12);
    let mk_unit=|sub:u64,start:u64,end:u64|{
        let mut u=CUnit{counter:Object::default(),its:Object::default(),work:Object::default()};
        u.counter.set(0,sub);
        u.its.set(0,start);u.its.set(8,end);
        u.work.set(compute::DESCRIPTOR_COMPLETED,sub);
        u.work.set(compute::DESCRIPTOR_COUNTER,0x1234);
        u
    };
    let fl=&mut f.engine.flights[0];
    fl.computes=3;
    fl.claims.slot_count=3;
    fl.claims.slot[0]=0;fl.claims.slot[1]=1;fl.claims.slot[2]=2;
    fl.csubmitted[0]=10;fl.csubmitted[1]=11;fl.csubmitted[2]=12;
    fl.job.resources.compute=vec![mk_unit(10,100,110),mk_unit(11,110,120),mk_unit(12,120,130)];
    assert_eq!(f.retired(),Ok(Retirement::Full));
    // Missing interior unit completion keeps the whole batched flight pending.
    f.engine.flights[0].job.resources.compute[1].work.set(compute::DESCRIPTOR_COMPLETED,10);
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    f.engine.flights[0].job.resources.compute[1].work.set(compute::DESCRIPTOR_COMPLETED,11);
    // Missing tail unit ticks keeps the whole batched flight pending.
    f.engine.flights[0].job.resources.compute[2].its.set(8,0);
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    f.engine.flights[0].job.resources.compute[2].its.set(8,130);
    assert_eq!(f.retired(),Ok(Retirement::Full));
}
#[test]fn inactive_class_and_unreached_stage_skip_wc_reads(){
    // Compute-only retirement performs zero render WC reads.
    let mut fc=firmware(false,5);
    assert_eq!(fc.retired_flight(),Ok(Some((0,Retirement::Full))));
    assert_eq!(fc.graph.render.fw_stamp[0].reads.get(),0);
    assert_eq!(fc.graph.render.fw_stamp[1].reads.get(),0);
    assert_eq!(fc.graph.render.notifier.reads.get(),0);
    // Render-only retirement performs zero compute WC reads.
    let mut fr=firmware(true,5);
    assert_eq!(fr.retired_flight(),Ok(Some((0,Retirement::Full))));
    assert_eq!(fr.graph.compute.fw_stamp.reads.get(),0);
    assert_eq!(fr.graph.compute.notifier.reads.get(),0);
    // In-progress render (TA done, FS still running) fails fast on FS stamp without reading TA or scene.
    let mut fp=firmware(true,7);
    fp.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(6)));
    assert_eq!(fp.retired_flight(),Ok(None));
    assert_eq!(fp.graph.render.fw_stamp[1].reads.get(),1);
    assert_eq!(fp.graph.render.fw_stamp[0].reads.get(),0);
    assert_eq!(fp.engine.flights[0].job.resources.passes[0].scene.reads.get(),0);
    // Unsettled notifier skips scene WC timestamp reads.
    fp.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(7)));
    fp.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(6)));
    assert_eq!(fp.retired_flight(),Ok(None));
    assert_eq!(fp.engine.flights[0].job.resources.passes[0].scene.reads.get(),0);
    // Mixed flight pending is polled once, not twice.
    let mut fm=firmware(false,9);
    fm.engine.flights[0].renders=1;
    fm.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(8)));
    assert_eq!(fm.retired_flight(),Ok(None));
    assert_eq!(fm.graph.compute.fw_stamp.reads.get(),1);
    assert_eq!(fm.graph.render.fw_stamp[1].reads.get(),0);
    // Pending compute class is not re-polled while retiring a burst of render flights.
    let mut fmix=firmware(false,30);
    let r1=firmware(true,40).engine.flights.remove(0);
    let r2=firmware(true,41).engine.flights.remove(0);
    fmix.engine.flights.push(r1);
    fmix.engine.flights.push(r2);
    fmix.engine.ta_target=encode::stamp(41);
    fmix.engine.fs_target=encode::stamp(41);
    for stage in 0..2 { fmix.graph.render.fw_stamp[stage].set(0,u64::from(encode::stamp(41))); }
    fmix.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(41)));
    fmix.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(41)));
    fmix.graph.compute.fw_stamp.set(0,u64::from(encode::stamp(29)));
    let (mut check_c,mut check_r)=(true,true);
    assert_eq!(fmix.retired_flight_classes(&mut check_c,&mut check_r),Ok(Some((1,Retirement::Full))));
    assert!(!check_c && check_r);
    fmix.engine.flights.remove(1);
    assert_eq!(fmix.retired_flight_classes(&mut check_c,&mut check_r),Ok(Some((1,Retirement::Full))));
    fmix.engine.flights.remove(1);
    assert_eq!(fmix.retired_flight_classes(&mut check_c,&mut check_r),Ok(None));
    assert_eq!(fmix.graph.compute.fw_stamp.reads.get(),1);
}

'''.replace('__LOGIC__', str(backend/'recovery/logic.rs'))
for token, path, name in [('__STAMP_SEED__','encode.rs','stamp_seed'),('__STAMP__','encode.rs','stamp'),('__RETIRED__','poll.rs','retired'),('__RETIRED_AT__','poll.rs','retired_at'),('__RETIRED_FLIGHT__','poll.rs','retired_flight'),('__RETIRED_FLIGHT_CLASSES__','poll.rs','retired_flight_classes'),('__RETIRED_SNAPSHOT__','poll.rs','retired_snapshot'),('__RETIRED_SNAPSHOT_AT__','poll.rs','retired_snapshot_at')]:
    harness=harness.replace(token,function((backend/path).read_text(),name))
unit=out/'stamp-wrap.rs';unit.write_text(harness)
log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'stamp-wrap')],[str(out/'stamp-wrap'),'--nocapture']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    log+=result.stdout;print(result.stdout,end='');(out/'stamp-wrap.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'stamp-wrap.json').write_text(json.dumps({'passed':True,'cpu_only':True,'scope':'Production stamp builders, modular comparisons and full receipt decisions; queue/DMA/MMIO transport mocked. Tests around top-bit/zero wrap and partial paired TA/FS receipts. Firmware behavior not hardware-qualified.','source_sha256':{name:sha(backend/name)for name in ['encode.rs','recovery/logic.rs','poll.rs']}},indent=2)+'\n')
