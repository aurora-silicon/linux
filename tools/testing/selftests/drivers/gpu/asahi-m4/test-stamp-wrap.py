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
use logic::{Retirement, ABORT_GRACE_NS};
mod encode { __STAMP_SEED__ __STAMP__ }
mod hw { pub mod sizes { pub const COMPUTE_PIPES: usize = 8; } }
struct Object{cells:BTreeMap<usize,u64>,va:u64,unstable:bool,reads:Cell<u32>,syncs:Cell<u32>}
impl Default for Object{fn default()->Self{Self{cells:BTreeMap::new(),va:0x1234,unstable:false,reads:Cell::new(0),syncs:Cell::new(0)}}}
impl Object {
    fn sync_for_cpu(&self,_off:usize,_len:usize)->Result{self.syncs.set(self.syncs.get()+1);Ok(())}
    fn r32(&self,off:usize)->Result<u32>{self.reads.set(self.reads.get()+1);Ok(*self.cells.get(&off).unwrap_or(&0) as u32)}
    fn r64(&self,off:usize)->Result<u64>{self.reads.set(self.reads.get()+1);if self.unstable{return Err(EAGAIN)}Ok(*self.cells.get(&off).unwrap_or(&0))}
    fn set(&mut self,off:usize,v:u64){self.cells.insert(off,v);}
    fn reset_reads(&self){self.reads.set(0);self.syncs.set(0);}
}
#[derive(Clone,Copy)]enum Channel{Compute,Ta,Fs}
mod notifier_page {pub const TA_STAMP:usize=8;pub const FS_STAMP:usize=12;pub const FS_RECORD:usize=32;}
mod notifier {pub const CONTEXT:usize=16;}
mod manager {pub const COUNTER:usize=0x4c;pub const COMPLETED:usize=0x54;}
#[derive(Default)]struct CGraph{fw_stamp:Object,qstate:Object,qinfo:Object,notifier:Object,user_stamp:Object}
struct RGraph{fw_stamp:[Object;2],qstate:[Object;2],qinfo:[Object;2],notifier:Object}
struct Graph{compute:[CGraph;hw::sizes::COMPUTE_PIPES],render:RGraph}
struct Vm{slot:u32}struct Job{vm:Vm,resources:Resources}
struct Resources{compute:Vec<CUnit>,passes:Vec<RUnit>,pools:Pools}
impl Resources{fn compute(&self)->Result<&Self>{Ok(self)}fn render(&self)->Result<&Self>{Ok(self)}fn compute_unit(&self,k:usize)->Result<&CUnit>{self.compute.get(k).ok_or(EINVAL)}fn render_pass(&self,k:usize)->Result<&RUnit>{self.passes.get(k).ok_or(EINVAL)}fn pb_retired(&self,k:usize)->Result<bool>{self.pools.retired(k)}fn pb_retired_at(&self,k:usize,target:u64)->Result<bool>{self.pools.retired_at(k,target)}}
struct Claims{slot:[u32;16],slot_count:u32,pass:[u32;4]}
struct Flight{computes:u32,renders:u32,compute_pipe:u8,target:u32,ta_target:u32,fs_target:u32,job:Job,claims:Claims,head:u32,ta_head:u32,fs_head:u32,rec:[u32;3],chan_settled:bool,pb_slot:u32,uma_submitted:u64,csubmitted:[u64;16],recovery_suspect:bool,recovery_started_ns:u64,gate_passed_ns:Option<u64>,unsettled_polls:u32,doomed:bool}
struct Engine{flights:Vec<Flight>,target:[u32;hw::sizes::COMPUTE_PIPES],ta_target:u32,fs_target:u32,retired_ticks:[u64;4]}
fn stamp_reached(observed:u32,own:u32,tail:u32)->bool{logic::stamp_reached(observed,own,tail)}
impl Engine{fn oldest(&self)->Option<&Flight>{self.flights.first()}}
struct CUnit{counter:Object,descriptor:Object,its:Object,work:Object}
struct RUnit{scene:Object}
struct Scrub{compute:Vec<CUnit>,passes:Vec<RUnit>}
struct Slots{scrub:Scrub}
struct Pools{full:bool,completed:u64}
impl Pools{fn retired(&self,_k:usize)->Result<bool>{Ok(self.full)}fn retired_at(&self,_k:usize,target:u64)->Result<bool>{Ok(self.full && target!=0 && self.completed>=target)}}
struct TopChannels{compute_producer:u32}
impl TopChannels{fn producer(&self,ch:Channel)->u32{match ch{Channel::Compute=>self.compute_producer,_=>1}}}
struct Firmware{graph:Graph,engine:Engine,slots:Slots,pools:Pools,channels:TopChannels,queue_passed:bool,last_chan_passed:Cell<Option<bool>>}
thread_local! { static CLOCK: Cell<u64> = const { Cell::new(100_000_000) }; }
fn rmb(){}fn now_ns()->u64{CLOCK.with(|c|c.get())}
impl Firmware{
    fn queue_ring_receipt(&self,_state:&Object,_info:&Object,_head:u32)->Result<bool>{Ok(self.queue_passed)}
    fn queue_ring_passed(&self,_ch:Channel,_state:&Object,_info:&Object,_f:&Flight)->Result<bool>{Ok(self.queue_passed)}
    fn channel_receipt(&self,_ch:Channel,_rec:u32,passed:bool)->Result<bool>{self.last_chan_passed.set(Some(passed));Ok(self.queue_passed)}
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
    let mut target_arr=[0u32;hw::sizes::COMPUTE_PIPES];target_arr[0]=target;
    let mut f=Firmware{
        graph:Graph{compute:core::array::from_fn(|_|CGraph::default()),
            render:RGraph{fw_stamp:Default::default(),qstate:Default::default(),qinfo:Default::default(),notifier:Object::default()}},
        engine:Engine{flights:vec![Flight{computes:u32::from(!render),renders:u32::from(render),compute_pipe:0,target,ta_target:target,fs_target:target,job:Job{vm:Vm{slot:2},resources:Resources{compute:vec![CUnit{counter:Object::default(),descriptor:Object::default(),its:Object::default(),work:Object::default()}],passes:vec![RUnit{scene:Object::default()}],pools:Pools{full:true,completed:2}}},claims:Claims{slot:[0;16],slot_count:u32::from(!render),pass:[0;4]},head:1,ta_head:1,fs_head:1,rec:[1;3],chan_settled:false,pb_slot:0,uma_submitted:2,csubmitted:{let mut a=[0u64;16];if !render{a[0]=ordinal;}a},recovery_suspect:false,recovery_started_ns:0,gate_passed_ns:None,unsettled_polls:0,doomed:false}],target:target_arr,ta_target:target,fs_target:target,retired_ticks:[0;4]},
        slots:Slots{scrub:Scrub{compute:vec![CUnit{counter:Object::default(),descriptor:Object::default(),its:Object::default(),work:Object::default()}],passes:vec![RUnit{scene:Object::default()}]}},
        pools:Pools{full:true,completed:2},channels:TopChannels{compute_producer:1},queue_passed:true,last_chan_passed:Cell::new(None),
    };
    f.graph.compute[0].fw_stamp.set(0,u64::from(target));
    f.graph.compute[0].user_stamp.set(0,u64::from(target));
    f.engine.flights[0].job.resources.compute[0].counter.set(0,ordinal);
    f.engine.flights[0].job.resources.compute[0].its.set(0,100);f.engine.flights[0].job.resources.compute[0].its.set(8,101);
    f.engine.flights[0].job.resources.compute[0].descriptor.set(manager::COMPLETED,ordinal);
    f.engine.flights[0].job.resources.compute[0].descriptor.set(manager::COUNTER,0x1234);
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
            f.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(n-1)));
            assert_eq!(f.retired(),Ok(Retirement::Pending));
            f.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(n)));
            f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(n-1)));
            assert_eq!(f.retired(),Ok(Retirement::Pending));
            f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(n)));
            assert_eq!(f.graph.render.fw_stamp[0].reads.get(),0);
            assert_eq!(f.graph.render.fw_stamp[1].reads.get(),0);
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
        f.engine.flights.push(later);f.engine.target[0]=encode::stamp(n+1);
        // The command behind it finished too: the shared stamp and notifier cells name it.
        f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(n+1)));
        f.graph.compute[0].user_stamp.set(0,u64::from(encode::stamp(n+1)));
        assert_eq!(f.retired(),Ok(Retirement::Full));
        f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(n-1)));
        assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(n)));
        assert_eq!(f.retired(),Ok(Retirement::Full));
        f.queue_passed=false;assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.queue_passed=true;f.engine.flights[0].job.resources.compute[0].descriptor.set(manager::COMPLETED,n-1);
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
        f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(n-1)));
        assert_eq!(f.retired(),Ok(Retirement::Pending));
        f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(n)));
        f.graph.render.notifier.set(notifier_page::FS_RECORD+notifier::CONTEXT,2);
        assert_eq!(f.retired(),Ok(Retirement::Full));
        assert_eq!(f.graph.render.fw_stamp[0].reads.get(),0);
        assert_eq!(f.graph.render.fw_stamp[1].reads.get(),0);
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
    f.engine.target[0]=encode::stamp(20);
    f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(20)));
    f.graph.compute[0].user_stamp.set(0,u64::from(encode::stamp(20)));
    // Keep render FS stamp unreached so render at index 0 stays pending.
    f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(9)));
    assert_eq!(f.retired_flight(),Ok(Some((1,Retirement::Full))));
    f.engine.flights.remove(1);
    assert_eq!(f.retired_flight(),Ok(None));
    // Now finish render: render retires cleanly with exact single-class receipt.
    f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(10)));
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
    f2.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(29)));
    assert_eq!(f2.retired_flight(),Ok(Some((1,Retirement::Full))));
}
#[test]fn multi_compute_horizon_requires_every_claimed_unit_receipt(){
    let mut f=firmware(false,12);
    let mk_unit=|sub:u64,start:u64,end:u64|{
        let mut u=CUnit{counter:Object::default(),descriptor:Object::default(),its:Object::default(),work:Object::default()};
        u.counter.set(0,sub);
        u.its.set(0,start);u.its.set(8,end);
        u.descriptor.set(manager::COMPLETED,sub);
        u.descriptor.set(manager::COUNTER,0x1234);
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
    f.engine.flights[0].job.resources.compute[1].descriptor.set(manager::COMPLETED,10);
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    f.engine.flights[0].job.resources.compute[1].descriptor.set(manager::COMPLETED,11);
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
    assert_eq!(fr.graph.compute[0].fw_stamp.reads.get(),0);
    assert_eq!(fr.graph.compute[0].notifier.reads.get(),0);
    assert_eq!(fr.graph.compute[1].fw_stamp.reads.get(),0);
    assert_eq!(fr.graph.compute[1].notifier.reads.get(),0);
    // In-progress render (TA done, FS still running) fails fast on FS stamp without reading TA or scene.
    let mut fp=firmware(true,7);
    fp.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(6)));
    assert_eq!(fp.retired_flight(),Ok(None));
    assert_eq!(fp.graph.render.notifier.reads.get(),1);
    assert_eq!(fp.graph.render.fw_stamp[0].reads.get(),0);
    assert_eq!(fp.graph.render.fw_stamp[1].reads.get(),0);
    assert_eq!(fp.engine.flights[0].job.resources.passes[0].scene.reads.get(),0);
    // Unsettled notifier context skips scene WC timestamp reads.
    fp.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(7)));
    fp.graph.render.notifier.set(notifier::CONTEXT,99);
    assert_eq!(fp.retired_flight(),Ok(None));
    assert_eq!(fp.engine.flights[0].job.resources.passes[0].scene.reads.get(),0);
    // Mixed flight pending is polled once, not twice.
    let mut fm=firmware(false,9);
    fm.engine.flights[0].renders=1;
    fm.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(8)));
    assert_eq!(fm.retired_flight(),Ok(None));
    assert_eq!(fm.graph.compute[0].fw_stamp.reads.get(),1);
    assert_eq!(fm.graph.render.notifier.reads.get(),0);
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
    fmix.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(29)));
    let (mut check_c,mut check_r)=([true;hw::sizes::COMPUTE_PIPES],true);
    assert_eq!(fmix.retired_flight_classes(&mut check_c,&mut check_r),Ok(Some((1,Retirement::Full))));
    assert!(!check_c[0] && !check_c[1] && check_r);
    fmix.engine.flights.remove(1);
    assert_eq!(fmix.retired_flight_classes(&mut check_c,&mut check_r),Ok(Some((1,Retirement::Full))));
    fmix.engine.flights.remove(1);
    assert_eq!(fmix.retired_flight_classes(&mut check_c,&mut check_r),Ok(None));
    assert_eq!(fmix.graph.compute[0].fw_stamp.reads.get(),1);
}
#[test]fn innocent_suspect_flight_does_not_false_abort_after_recovery_grace(){
    let mut f=firmware(true,15);
    f.engine.flights[0].recovery_suspect=true;
    f.engine.flights[0].recovery_started_ns=10_000_000;
    // Long after recovery started (>20 ms), flight finishes its stage/queue gates while scene
    // timestamps are briefly unsettled on the first poll.
    CLOCK.with(|c| c.set(100_000_000));
    f.engine.flights[0].job.resources.passes[0].scene.set(0x628,0);
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    assert_eq!(f.engine.flights[0].gate_passed_ns,Some(100_000_000));
    assert_eq!(f.engine.flights[0].unsettled_polls,1);
    // Receipts settle before ABORT_GRACE_NS from gate_passed_ns -> full normal retirement.
    CLOCK.with(|c| c.set(100_500_000));
    f.engine.flights[0].job.resources.passes[0].scene.set(0x628,103);
    assert_eq!(f.retired(),Ok(Retirement::Full));
    assert!(!f.engine.flights[0].doomed);
}
#[test]fn truly_aborted_suspect_requires_two_polls_and_grace_from_gate_passed(){
    let mut f=firmware(false,22);
    f.engine.flights[0].recovery_suspect=true;
    f.engine.flights[0].recovery_started_ns=10_000_000;
    f.engine.flights[0].job.resources.compute[0].its.set(8,0);
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    // Second poll before ABORT_GRACE_NS stays pending.
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS - 1));
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    // Once both >=2 polls and >=ABORT_GRACE_NS elapsed from gate_passed_ns, abort and doom.
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS));
    assert_eq!(f.retired(),Ok(Retirement::Aborted));
    assert!(f.engine.flights[0].doomed);
}
#[test]fn suspect_render_with_passed_queues_and_stale_fs_notifier_aborts_after_grace(){
    // Single suspect render: both stage stamps and queue receipts passed, FS notifier still stale.
    let mut f=firmware(true,23);
    f.engine.flights[0].recovery_suspect=true;
    f.engine.flights[0].recovery_started_ns=10_000_000;
    f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(22)));
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    assert_eq!(f.engine.flights[0].gate_passed_ns,Some(100_000_000));
    assert_eq!(f.engine.flights[0].unsettled_polls,1);
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS - 1));
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS));
    assert_eq!(f.retired(),Ok(Retirement::Aborted));
    assert!(f.engine.flights[0].doomed);

    // Same suspect render when queues have NOT passed stays Pending and never starts grace timer.
    let mut q=firmware(true,24);
    q.engine.flights[0].recovery_suspect=true;
    q.engine.flights[0].recovery_started_ns=10_000_000;
    q.queue_passed=false;
    q.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(23)));
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(q.retired(),Ok(Retirement::Pending));
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS * 2));
    assert_eq!(q.retired(),Ok(Retirement::Pending));
    assert!(!q.engine.flights[0].doomed);
    assert_eq!(q.engine.flights[0].gate_passed_ns,None);
    assert_eq!(q.engine.flights[0].unsettled_polls,0);

    // Innocent suspect render whose stale FS notifier settles before ABORT_GRACE_NS retires Full.
    let mut s=firmware(true,25);
    s.engine.flights[0].recovery_suspect=true;
    s.engine.flights[0].recovery_started_ns=10_000_000;
    s.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(24)));
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(s.retired(),Ok(Retirement::Pending));
    assert_eq!(s.engine.flights[0].gate_passed_ns,Some(100_000_000));
    CLOCK.with(|c| c.set(100_500_000));
    s.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(25)));
    assert_eq!(s.retired(),Ok(Retirement::Full));
    assert!(!s.engine.flights[0].doomed);

    // Pipelined suspect render (render_behind=true) with passed queues and stale FS notifier aborts.
    let mut b=firmware(true,26);
    b.engine.flights[0].recovery_suspect=true;
    b.engine.flights[0].recovery_started_ns=10_000_000;
    let mut later=firmware(true,27).engine.flights.remove(0);
    later.uma_submitted=4;
    b.engine.flights.push(later);
    b.engine.ta_target=encode::stamp(27);
    b.engine.fs_target=encode::stamp(27);
    b.graph.render.fw_stamp[0].set(0,u64::from(encode::stamp(26)));
    b.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(26)));
    b.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(26)));
    b.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(25)));
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(b.retired(),Ok(Retirement::Pending));
    assert_eq!(b.engine.flights[0].gate_passed_ns,Some(100_000_000));
    assert!(b.graph.render.fw_stamp[1].syncs.get()>=1);
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS));
    assert_eq!(b.retired(),Ok(Retirement::Aborted));
    assert!(b.engine.flights[0].doomed);
}
#[test]fn suspect_render_with_unreached_fs_stage_and_stale_notifier_stays_pending(){
    // Exact (!render_behind) suspect render: queues passed, BOTH FS stage and FS notifier are
    // one target behind -> must stay Pending across 2 polls 20 ms apart without arming abort grace.
    let mut f=firmware(true,28);
    f.engine.flights[0].recovery_suspect=true;
    f.engine.flights[0].recovery_started_ns=10_000_000;
    f.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(27)));
    f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(27)));
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    assert_eq!(f.engine.flights[0].gate_passed_ns,None);
    assert_eq!(f.engine.flights[0].unsettled_polls,0);
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS));
    assert_eq!(f.retired(),Ok(Retirement::Pending));
    assert!(!f.engine.flights[0].doomed);
    assert_eq!(f.engine.flights[0].gate_passed_ns,None);
    assert_eq!(f.engine.flights[0].unsettled_polls,0);
    assert!(f.graph.render.fw_stamp[1].syncs.get()>=2);

    // Pipelined (render_behind=true) counterpart: queues passed, BOTH FS stage and FS notifier
    // one target behind -> must stay Pending across 2 polls 20 ms apart.
    let mut b=firmware(true,29);
    b.engine.flights[0].recovery_suspect=true;
    b.engine.flights[0].recovery_started_ns=10_000_000;
    let mut later=firmware(true,30).engine.flights.remove(0);
    later.uma_submitted=4;
    b.engine.flights.push(later);
    b.engine.ta_target=encode::stamp(30);
    b.engine.fs_target=encode::stamp(30);
    b.graph.render.fw_stamp[0].set(0,u64::from(encode::stamp(29)));
    b.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(28)));
    b.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(encode::stamp(29)));
    b.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(encode::stamp(28)));
    CLOCK.with(|c| c.set(100_000_000));
    assert_eq!(b.retired(),Ok(Retirement::Pending));
    assert_eq!(b.engine.flights[0].gate_passed_ns,None);
    CLOCK.with(|c| c.set(100_000_000 + ABORT_GRACE_NS));
    assert_eq!(b.retired(),Ok(Retirement::Pending));
    assert!(!b.engine.flights[0].doomed);
    assert_eq!(b.engine.flights[0].gate_passed_ns,None);
    assert_eq!(b.engine.flights[0].unsettled_polls,0);
    assert!(b.graph.render.fw_stamp[1].syncs.get()>=2);

    // Innocent suspect render whose uncached TA/FS notifiers already settled retires Full
    // immediately without reading or syncing fw_stamp even if DRAM fw_stamp lags behind ASC cache.
    let mut ok=firmware(true,31);
    ok.engine.flights[0].recovery_suspect=true;
    ok.engine.flights[0].recovery_started_ns=10_000_000;
    ok.graph.render.fw_stamp[0].set(0,u64::from(encode::stamp(30)));
    ok.graph.render.fw_stamp[1].set(0,u64::from(encode::stamp(30)));
    assert_eq!(ok.retired(),Ok(Retirement::Full));
    assert!(!ok.engine.flights[0].doomed);
    assert_eq!(ok.graph.render.fw_stamp[0].syncs.get(),0);
    assert_eq!(ok.graph.render.fw_stamp[1].syncs.get(),0);
}
#[test]fn same_tick_stage_timestamps_retire_full_while_zero_or_reversed_stay_pending(){
    // 24 MHz (41.67 ns) stage timestamps can legitimately record end == start on tiny draws
    // or dispatches; accept end >= start while still rejecting 0 or reversed intervals.
    let mut r=firmware(true,50);
    r.engine.flights[0].job.resources.passes[0].scene.set(0x600,100);
    r.engine.flights[0].job.resources.passes[0].scene.set(0x608,100);
    r.engine.flights[0].job.resources.passes[0].scene.set(0x620,200);
    r.engine.flights[0].job.resources.passes[0].scene.set(0x628,200);
    assert_eq!(r.retired(),Ok(Retirement::Full));
    assert_eq!(r.engine.retired_ticks,[100,100,200,200]);

    r.engine.flights[0].job.resources.passes[0].scene.set(0x608,0);
    assert_eq!(r.retired(),Ok(Retirement::Pending));
    r.engine.flights[0].job.resources.passes[0].scene.set(0x608,99);
    assert_eq!(r.retired(),Ok(Retirement::Pending));
    r.engine.flights[0].job.resources.passes[0].scene.set(0x608,100);
    r.engine.flights[0].job.resources.passes[0].scene.set(0x628,199);
    assert_eq!(r.retired(),Ok(Retirement::Pending));

    let mut c=firmware(false,51);
    c.engine.flights[0].job.resources.compute[0].its.set(0,300);
    c.engine.flights[0].job.resources.compute[0].its.set(8,300);
    assert_eq!(c.retired(),Ok(Retirement::Full));
    assert_eq!(c.engine.retired_ticks,[300,300,0,0]);

    c.engine.flights[0].job.resources.compute[0].its.set(8,0);
    assert_eq!(c.retired(),Ok(Retirement::Pending));
    c.engine.flights[0].job.resources.compute[0].its.set(8,299);
    assert_eq!(c.retired(),Ok(Retirement::Pending));
}
#[test]fn cross_vm_render_hang_recovery_isolates_innocent_client_flight(){
    // VM A (slot 0, ASID 1) hangs/aborts on render n=30 while innocent VM B (slot 1, ASID 2)
    // has a co-resident render n=31 with real TA/FS ticks and independent allocator slot
    // completion. VM A aborts; VM B retires Full without blame or error.
    CLOCK.with(|c|c.set(100_000_000));
    let mut f=firmware(true,30);
    f.engine.flights[0].job.vm.slot=1;
    f.engine.flights[0].pb_slot=0;
    f.engine.flights[0].doomed=true;
    f.engine.flights[0].recovery_suspect=true;
    f.engine.flights[0].recovery_started_ns=90_000_000;
    f.engine.flights[0].job.resources.passes[0].scene=Object::default();

    let mut innocent=firmware(true,31).engine.flights.remove(0);
    innocent.job.vm.slot=2;
    innocent.pb_slot=1;
    innocent.uma_submitted=2;
    innocent.doomed=false;
    innocent.recovery_suspect=true;
    innocent.recovery_started_ns=90_000_000;
    innocent.job.resources.pools=Pools{full:true,completed:2};
    innocent.job.resources.passes[0].scene=Object::default();
    innocent.job.resources.passes[0].scene.set(0x600,300);
    innocent.job.resources.passes[0].scene.set(0x608,320);
    innocent.job.resources.passes[0].scene.set(0x620,400);
    innocent.job.resources.passes[0].scene.set(0x628,430);
    f.engine.flights.push(innocent);

    let t31=encode::stamp(31);
    f.engine.ta_target=t31;
    f.engine.fs_target=t31;
    for stage in 0..2 { f.graph.render.fw_stamp[stage].set(0,u64::from(t31)); }
    f.graph.render.notifier.set(notifier_page::TA_STAMP,u64::from(t31));
    f.graph.render.notifier.set(notifier_page::FS_STAMP,u64::from(t31));
    f.graph.render.notifier.set(notifier::CONTEXT,2);
    f.graph.render.notifier.set(notifier_page::FS_RECORD+notifier::CONTEXT,2);

    // First unsettled poll records gate_passed_ns on hanging VM A; after ABORT_GRACE_NS, VM A aborts.
    assert_eq!(f.retired_flight(),Ok(None));
    CLOCK.with(|c|c.set(100_000_000 + ABORT_GRACE_NS));
    let (idx_a,ret_a)=f.retired_flight().unwrap().unwrap();
    assert_eq!((idx_a,ret_a),(0,Retirement::Aborted));
    assert!(ret_a.job_failed(f.engine.flights[0].doomed));
    f.engine.flights.remove(0);

    // Innocent VM B on slot 1 proves TA/FS execution via hardware ticks and retires Full.
    let (idx_b,ret_b)=f.retired_flight().unwrap().unwrap();
    assert_eq!((idx_b,ret_b),(0,Retirement::Full));
    assert!(!f.engine.flights[0].doomed);
    assert!(!ret_b.job_failed(f.engine.flights[0].doomed));
}
#[test]fn two_compute_pipes_retire_out_of_order_independently(){
    // Pipe 0 flight at idx 0 (pending, rec=1) + Pipe 1 flight at idx 1 (complete, rec=2,
    // shared Channel::Compute producer=2): pipe 1 retires Full ahead of pipe 0, and once
    // pipe 0 settles afterwards it still uses passed channel_receipt because rec (1) != producer (2).
    let mut f=firmware(false,10);
    f.engine.flights[0].compute_pipe=0;
    f.engine.flights[0].rec[2]=1;
    let mut c1=firmware(false,11).engine.flights.remove(0);
    c1.compute_pipe=1;
    c1.rec[2]=2;
    f.engine.flights.push(c1);
    f.engine.target[0]=encode::stamp(10);f.engine.target[1]=encode::stamp(11);
    f.channels.compute_producer=2;
    f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(9)));
    f.graph.compute[1].fw_stamp.set(0,u64::from(encode::stamp(11)));
    f.graph.compute[1].user_stamp.set(0,u64::from(encode::stamp(11)));
    assert_eq!(f.retired_flight(),Ok(Some((1,Retirement::Full))));
    assert_eq!(f.last_chan_passed.get(),Some(true));
    f.engine.flights.remove(1);
    assert_eq!(f.retired_flight(),Ok(None));
    // Now finish pipe 0: rec[2]=1 != producer=2 still sets chan_behind=true.
    f.last_chan_passed.set(None);
    f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(10)));
    assert_eq!(f.retired_flight(),Ok(Some((0,Retirement::Full))));
    assert_eq!(f.last_chan_passed.get(),Some(true));
}
#[test]fn two_pipe_cross_vm_compute_hang_recovery_isolates_innocent_pipe(){
    // Pipe 0 VM A (ASID 1) hangs/aborts while co-resident Pipe 1 VM B (ASID 2) proves CDM
    // execution via hardware ITS ticks and retires Full immediately without blame or false abort.
    CLOCK.with(|c|c.set(100_000_000));
    let mut f=firmware(false,40);
    f.engine.flights[0].compute_pipe=0;
    f.engine.flights[0].job.vm.slot=1;
    f.engine.flights[0].rec[2]=1;
    f.engine.flights[0].doomed=true;
    f.engine.flights[0].recovery_suspect=true;
    f.engine.flights[0].recovery_started_ns=90_000_000;
    f.engine.flights[0].job.resources.compute[0].its=Object::default();

    let mut innocent=firmware(false,41).engine.flights.remove(0);
    innocent.compute_pipe=1;
    innocent.job.vm.slot=2;
    innocent.rec[2]=2;
    innocent.doomed=false;
    innocent.recovery_suspect=true;
    innocent.recovery_started_ns=90_000_000;
    innocent.job.resources.compute[0].its.set(0,500);
    innocent.job.resources.compute[0].its.set(8,530);
    f.engine.flights.push(innocent);

    f.engine.target[0]=encode::stamp(40);f.engine.target[1]=encode::stamp(41);
    f.channels.compute_producer=2;
    f.graph.compute[0].fw_stamp.set(0,u64::from(encode::stamp(40)));
    f.graph.compute[0].user_stamp.set(0,u64::from(encode::stamp(40)));
    f.graph.compute[1].fw_stamp.set(0,u64::from(encode::stamp(41)));
    f.graph.compute[1].user_stamp.set(0,u64::from(encode::stamp(41)));

    // Within grace: pipe 0 records gate_passed_ns while innocent pipe 1 retires Full immediately.
    let (idx_b,ret_b)=f.retired_flight().unwrap().unwrap();
    assert_eq!((idx_b,ret_b),(1,Retirement::Full));
    assert!(!f.engine.flights[1].doomed);
    assert!(!ret_b.job_failed(f.engine.flights[1].doomed));
    f.engine.flights.remove(1);
    assert_eq!(f.retired_flight(),Ok(None));

    // Past ABORT_GRACE_NS from gate_passed_ns: hanging pipe 0 aborts cleanly.
    CLOCK.with(|c|c.set(100_000_000 + ABORT_GRACE_NS));
    let (idx_a,ret_a)=f.retired_flight().unwrap().unwrap();
    assert_eq!((idx_a,ret_a),(0,Retirement::Aborted));
    assert!(ret_a.job_failed(f.engine.flights[0].doomed));
}

mod queue_proof {
    use super::{Channel, Object, Result, EINVAL};
    use super::logic::ring_passed;
    pub(crate) mod object {
        pub(crate) type FwObject = super::Object;
    }
    pub(crate) mod hw {
        __HW_CLIENT__
        __HW_ENGINE__
        __HW_TUNING__
        __HW_SIZES__
    }
    pub(crate) mod fw {
        pub(crate) mod channels {
            pub(crate) const RING_SLOTS: u32 = 256;
        }
    }
    pub(crate) mod graph {
        #[derive(Copy, Clone, Debug, PartialEq, Eq)]
        pub(crate) enum Class { Fw, FwCached }
        __GRAPH_FW__
        __QINFO_CACHED__
    }
    mod qstate {
        pub(crate) const READ: usize = 0x10;
        pub(crate) const WRITE: usize = 0x14;
    }
    struct QInfoLayout { busy: usize }
    struct Abi;
    impl Abi { fn qinfo(&self) -> QInfoLayout { QInfoLayout { busy: 0x30 } } }
    struct Init { abi: Abi }
    struct Pipe { read: u32, completion: u32, write: u32 }
    impl Pipe {
        fn read(&self) -> Result<u32> { Ok(self.read) }
        fn completion(&self) -> Result<u32> { Ok(self.completion) }
        fn write(&self) -> Result<u32> { Ok(self.write) }
    }
    struct Channels { compute: Pipe, ta: Pipe, fs: Pipe }
    impl Channels {
        fn channel(&self, ch: Channel) -> &Pipe {
            match ch { Channel::Compute => &self.compute, Channel::Ta => &self.ta, Channel::Fs => &self.fs }
        }
        fn producer(&self, ch: Channel) -> u32 { self.channel(ch).write }
    }
    struct Engine { head: [u32; hw::sizes::COMPUTE_PIPES], ta_head: u32, fs_head: u32, flights: Vec<Flight> }
    struct Flight { computes: u32, compute_pipe: u8, c_start: u32, head: u32, ta_start: u32, ta_head: u32, fs_start: u32, fs_head: u32, rec: [u32; 3], chan_settled: bool }
    struct Firmware { init: Init, channels: Channels, engine: Engine }
    mod inner {
        use super::*;
        impl Firmware {
            __QUEUE_RING_RECEIPT__
            __QUEUE_RING_PASSED__
            __CHANNEL_RECEIPT__
            __LATCH_COMPUTE_CHANNEL_RECEIPTS__
            __QUEUE_RECEIPT__
            __QUEUE_PASSED__
        }
        #[test]
        fn cached_qinfo_skips_info_wc_reads_and_authenticates_on_nc_qstate_and_pipe() {
            assert!(!hw::tuning::FW_CACHED);
            assert!(graph::qinfo_cached());
            let mut fw = Firmware {
                init: Init { abi: Abi },
                channels: Channels {
                    compute: Pipe { read: 5, completion: 5, write: 5 },
                    ta: Pipe { read: 7, completion: 7, write: 7 },
                    fs: Pipe { read: 9, completion: 9, write: 9 },
                },
                engine: Engine { head: { let mut h = [0u32; hw::sizes::COMPUTE_PIPES]; h[0] = 10; h[1] = 12; h }, ta_head: 20, fs_head: 30, flights: vec![] },
            };
            let mut state = Object::default();
            let mut info = Object::default();
            // Deliberately poison qinfo with stale consumer cursors and nonzero busy flag.
            info.set(0x20, 0);
            info.set(0x24, 0);
            info.set(0x28, 0);
            info.set(0x30, 1);
            state.set(qstate::READ, 10);
            state.set(qstate::WRITE, 10);
            assert_eq!(fw.queue_receipt(Channel::Compute, &state, &info, 10, 5), Ok(true));
            assert_eq!(info.reads.get(), 0);
            // Lagging NC qstate::READ or qstate::WRITE rejects receipt.
            state.set(qstate::READ, 9);
            assert_eq!(fw.queue_receipt(Channel::Compute, &state, &info, 10, 5), Ok(false));
            state.set(qstate::READ, 10);
            state.set(qstate::WRITE, 11);
            assert_eq!(fw.queue_receipt(Channel::Compute, &state, &info, 10, 5), Ok(false));
            state.set(qstate::WRITE, 10);
            // Lagging submit pipe completion rejects receipt.
            fw.channels.compute.completion = 4;
            assert_eq!(fw.queue_receipt(Channel::Compute, &state, &info, 10, 5), Ok(false));
            fw.channels.compute.completion = 5;
            assert_eq!(fw.queue_receipt(Channel::Compute, &state, &info, 256, 5), Err(EINVAL));
            assert_eq!(info.reads.get(), 0);

            // Split ring + channel receipts for 2-pipe OOO compute: pipe 1's own ring is exact
            // (head=12) while shared Channel::Compute producer moved on to 6 (rec=5 -> passed=true).
            fw.channels.compute = Pipe { read: 6, completion: 6, write: 6 };
            state.set(qstate::READ, 12);
            state.set(qstate::WRITE, 12);
            assert_eq!(fw.queue_ring_receipt(&state, &info, 12), Ok(true));
            assert_eq!(fw.channel_receipt(Channel::Compute, 5, false), Ok(false));
            assert_eq!(fw.channel_receipt(Channel::Compute, 5, true), Ok(true));
            let f_p1 = Flight { computes: 1, compute_pipe: 1, c_start: 10, head: 11, ta_start: 0, ta_head: 1, fs_start: 0, fs_head: 1, rec: [1, 1, 5], chan_settled: false };
            assert_eq!(fw.queue_ring_passed(Channel::Compute, &state, &info, &f_p1), Ok(true));
            state.set(qstate::READ, 10);
            assert_eq!(fw.queue_ring_passed(Channel::Compute, &state, &info, &f_p1), Ok(false));
            assert_eq!(info.reads.get(), 0);

            // queue_passed with queued work behind flight (including wrap-around) never reads qinfo.
            fw.engine.ta_head = 2;
            fw.channels.ta = Pipe { read: 2, completion: 2, write: 3 };
            let f = Flight { computes: 0, compute_pipe: 0, c_start: 0, head: 10, ta_start: 254, ta_head: 0, fs_start: 0, fs_head: 1, rec: [1, 1, 5], chan_settled: false };
            state.set(qstate::READ, 1);
            assert_eq!(fw.queue_passed(Channel::Ta, &state, &info, &f), Ok(true));
            assert_eq!(info.reads.get(), 0);
            // Unpassed NC qstate::READ rejects queue_passed.
            state.set(qstate::READ, 255);
            assert_eq!(fw.queue_passed(Channel::Ta, &state, &info, &f), Ok(false));
            state.set(qstate::READ, 1);
            // Unpassed pipe completion rejects queue_passed.
            fw.channels.ta.completion = 0;
            assert_eq!(fw.queue_passed(Channel::Ta, &state, &info, &f), Ok(false));
            assert_eq!(info.reads.get(), 0);
        }
        #[test]
        fn cross_pipe_channel_wrap_latches_receipt_before_producer_laps_flight() {
            // Pipe 0 submits compute record 1 while pipe 1 submits and retires 255 records,
            // stopping at producer=0, read=0, completion=0. Raw ring_passed(0, 0, 1, 0, 256)
            // would be false forever, but latch_compute_channel_receipts proves record 1 before
            // the 256-entry submit channel wraps.
            let mut fw = Firmware {
                init: Init { abi: Abi },
                channels: Channels {
                    compute: Pipe { read: 1, completion: 1, write: 1 },
                    ta: Pipe { read: 0, completion: 0, write: 0 },
                    fs: Pipe { read: 0, completion: 0, write: 0 },
                },
                engine: Engine {
                    head: [0u32; hw::sizes::COMPUTE_PIPES],
                    ta_head: 0,
                    fs_head: 0,
                    flights: vec![Flight {
                        computes: 1,
                        compute_pipe: 0,
                        c_start: 0,
                        head: 1,
                        ta_start: 0,
                        ta_head: 0,
                        fs_start: 0,
                        fs_head: 0,
                        rec: [0, 0, 1],
                        chan_settled: false,
                    }],
                },
            };
            // While firmware has not completed record 1 yet, latch stays false.
            fw.channels.compute = Pipe { read: 1, completion: 0, write: 2 };
            fw.latch_compute_channel_receipts().unwrap();
            assert!(!fw.engine.flights[0].chan_settled);
            // Out-of-range firmware cursors (>= RING_SLOTS, e.g. 257 which wraps mod 256 to 1)
            // must be rejected before modular comparison in both passed receipts and latching.
            for (read, completion) in [(257, 257), (257, 2), (2, 257)] {
                fw.channels.compute = Pipe { read, completion, write: 2 };
                assert_eq!(fw.channel_receipt(Channel::Compute, 1, true), Ok(false));
                fw.latch_compute_channel_receipts().unwrap();
                assert!(!fw.engine.flights[0].chan_settled);
            }
            // Once read and completion pass record 1 before wrap, latch becomes true.
            fw.channels.compute = Pipe { read: 2, completion: 2, write: 2 };
            fw.latch_compute_channel_receipts().unwrap();
            assert!(fw.engine.flights[0].chan_settled);
            // Even after pipe 1 submits 254 more records and stops at producer=0 (where raw
            // channel_receipt(Compute, 1, true) returns false), the latched proof persists.
            fw.channels.compute = Pipe { read: 0, completion: 0, write: 0 };
            assert_eq!(fw.channel_receipt(Channel::Compute, 1, true), Ok(false));
            assert!(fw.engine.flights[0].chan_settled);
        }
    }
}

'''.replace('__LOGIC__', str(backend/'recovery/logic.rs'))
for token, path, name in [('__STAMP_SEED__','encode.rs','stamp_seed'),('__STAMP__','encode.rs','stamp'),('__RETIRED__','poll.rs','retired'),('__RETIRED_AT__','poll.rs','retired_at'),('__RETIRED_FLIGHT__','poll.rs','retired_flight'),('__RETIRED_FLIGHT_CLASSES__','poll.rs','retired_flight_classes'),('__RETIRED_SNAPSHOT__','poll.rs','retired_snapshot'),('__RETIRED_SNAPSHOT_AT__','poll.rs','retired_snapshot_at'),('__QUEUE_RING_RECEIPT__','poll.rs','queue_ring_receipt'),('__QUEUE_RING_PASSED__','poll.rs','queue_ring_passed'),('__CHANNEL_RECEIPT__','poll.rs','channel_receipt'),('__LATCH_COMPUTE_CHANNEL_RECEIPTS__','poll.rs','latch_compute_channel_receipts'),('__QUEUE_RECEIPT__','poll.rs','queue_receipt'),('__QUEUE_PASSED__','poll.rs','queue_passed'),('__GRAPH_FW__','graph.rs','graph_fw'),('__QINFO_CACHED__','graph.rs','qinfo_cached')]:
    body = function((backend/path).read_text(),name)
    if token in ('__RETIRED_FLIGHT__', '__RETIRED_FLIGHT_CLASSES__', '__RETIRED_SNAPSHOT_AT__'):
        body = body.replace('super::hw::', 'hw::')
    harness=harness.replace(token, body)
hw_text=(backend/'hw.rs').read_text()
def mod_block(text, name):
    start=text.index('pub(crate) mod '+name+' {')
    at=text.index('{',start)
    depth,end=1,at+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]
harness=(harness
    .replace('__HW_CLIENT__',mod_block(hw_text,'client'))
    .replace('__HW_ENGINE__',mod_block(hw_text,'engine'))
    .replace('__HW_TUNING__',mod_block(hw_text,'tuning'))
    .replace('__HW_SIZES__',mod_block(hw_text,'sizes')))
unit=out/'stamp-wrap.rs';unit.write_text(harness)
log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'stamp-wrap')],[str(out/'stamp-wrap'),'--nocapture']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    log+=result.stdout;print(result.stdout,end='');(out/'stamp-wrap.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'stamp-wrap.json').write_text(json.dumps({'passed':True,'cpu_only':True,'scope':'Production stamp builders, modular comparisons and full receipt decisions; queue/DMA/MMIO transport mocked. Tests around top-bit/zero wrap and partial paired TA/FS receipts. Firmware behavior not hardware-qualified.','source_sha256':{name:sha(backend/name)for name in ['encode.rs','recovery/logic.rs','poll.rs']}},indent=2)+'\n')
