#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production Job failure gates before deferred fence completion."""
from common import kernel_root, output_dir, rustc, frozen_bindings

import hashlib
import json
import pathlib
import subprocess
import tempfile

ROOT = kernel_root()
SOURCE = ROOT / 'drivers/gpu/drm/asahi/g16/job.rs'
OUT = output_dir()
source = SOURCE.read_text()
fence_source = (SOURCE.parent / 'fence.rs').read_text()
command_source = (SOURCE.parent / 'command.rs').read_text()
queue_source = (SOURCE.parent / 'queue.rs').read_text()
order_source = command_source[command_source.index('/// Exact queue-local command ordinal'):command_source.index('/// A validated render command.')]

def function_from(text, marker_text):
    marker = text.index(marker_text)
    start = text.rfind('\n', 0, marker) + 1
    first = text.index('{', marker)
    depth = 1
    at = first + 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[start:at]

def function(text, name):
    return function_from(text, 'fn ' + name + '(')

queue_methods = '\n'.join(function(queue_source, name) for name in ['cancel', 'check', 'cancelled_through', 'is_compute_retired'])
methods = '\n'.join(function(source, name) for name in ['latch_failure', 'complete', 'finish', 'is_started', 'try_start_inline', 'start', 'check_command_dependencies', 'command_retired', 'note_full_retirement', 'mark_terminal_retired', 'waits_on_queue'])
push_method = function_from(source, 'pub(crate) fn push(&mut self, job: Arc<Job>')
fence_methods = '\n'.join(function(fence_source, name) for name in ['dependency_status', 'check_dependencies', 'wait_dependencies'])
harness = r'''
#![allow(dead_code)]
extern crate self as kernel;
use std::sync::{Arc,Mutex,Condvar};
use core::sync::atomic::{AtomicBool,AtomicI32,AtomicI64,AtomicU32,AtomicU64,AtomicUsize,Ordering};
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
struct Error(i32);
impl Error { fn from_errno(error: i32)->Self { Self(error) } }
type Result<T=()> = core::result::Result<T,Error>;
const EIO:Error=Error(-5); const ENODEV:Error=Error(-19); const ECANCELED:Error=Error(-125);
const ETIMEDOUT:Error=Error(-110);const ENOMEM:Error=Error(-12);const EAGAIN:Error=Error(-11); const EINVAL:Error=Error(-22);
const EOVERFLOW:Error=Error(-75); const NONE:u16=u16::MAX; const RENDER:usize=0; const COMPUTE:usize=1;
ORDER_SOURCE
pub mod task { pub const MAX_SCHEDULE_TIMEOUT:i64=i64::MAX; }
struct InputState { status:AtomicI32,waits:AtomicUsize,interrupted:AtomicI64,gate:Mutex<()>,changed:Condvar }
#[derive(Clone)]struct InputFence(Arc<InputState>);
impl InputFence {
    fn new(status:i32)->Self { Self(Arc::new(InputState{status:AtomicI32::new(status),waits:AtomicUsize::new(0),interrupted:AtomicI64::new(0),gate:Mutex::new(()),changed:Condvar::new()})) }
    fn raw(&self)->*const InputState { Arc::as_ptr(&self.0) }
    fn signal(&self,status:i32) { let _gate=self.0.gate.lock().unwrap(); self.0.status.store(status,Ordering::Release); self.0.changed.notify_all(); }
}
pub mod bindings {
    use super::*;
    pub(super) unsafe fn dma_fence_get_status(fence:*const InputState)->i32 { (*fence).status.load(Ordering::Acquire) }
    pub(super) unsafe fn dma_fence_wait_timeout(fence:*const InputState,_interruptible:bool,_timeout:i64)->i64 {
        let fence=&*fence;
        fence.waits.fetch_add(1,Ordering::AcqRel);
        let interrupted=fence.interrupted.load(Ordering::Acquire); if interrupted!=0 { return interrupted; }
        let mut gate=fence.gate.lock().unwrap();
        while fence.status.load(Ordering::Acquire)==0 { gate=fence.changed.wait(gate).unwrap(); }
        1
    }
}
mod fence { use super::{InputFence as Fence,Result,Error,EAGAIN,ECANCELED}; FENCE_METHODS }
struct Status { sticky:AtomicI32,reported:AtomicI32 }
impl Status {
    fn new()->Self { Self{sticky:AtomicI32::new(0),reported:AtomicI32::new(0)} }
    fn get(&self)->i32 { self.sticky.load(Ordering::Acquire) }
    fn record(&self,error:Error) {
        let _=self.sticky.compare_exchange(0,error.0,Ordering::AcqRel,Ordering::Acquire);
        self.report_failure(error);
    }
    fn report_failure(&self,error:Error) {
        let _=self.reported.compare_exchange(0,error.0,Ordering::AcqRel,Ordering::Acquire);
    }
}
struct Vm { status:Arc<Status> }
struct Guard(AtomicBool);
impl Guard { fn quarantine(&self) { self.0.store(true,Ordering::Release); } }
struct Progress { signaled:AtomicBool,sent:AtomicU32,next:AtomicU32,pending:AtomicBool }
impl Progress {
    fn sent(&self)->u32 { self.sent.load(Ordering::Acquire) }
    fn next(&self)->u32 { self.next.load(Ordering::Acquire) }
    fn is_pending(&self)->bool { self.pending.load(Ordering::Acquire) }
}
struct Fence { vm:Arc<Status>,signals:Mutex<Vec<(Result,i32)>> }
impl Fence {
    fn signal(&self,result:Result) {
        let status=self.vm.reported.load(Ordering::Acquire);
        if result.is_err() { assert_ne!(status,0,"error fence before visible status"); }
        self.signals.lock().unwrap().push((result,status));
    }
}
mod command { pub(crate) use super::{Order, COMPUTE}; }
struct QueueState { completed:[AtomicU64;2],published:[AtomicU64;2],cancelled_at:[AtomicU64;2],compute_ooo:AtomicU64 }
impl QueueState { QUEUE_METHODS }
struct FenceOrigin { queue:Arc<QueueState>, last_ordinal:[Option<u64>;2], failed:AtomicBool }
impl FenceOrigin {
    fn new(queue:Arc<QueueState>, last_ordinal:[Option<u64>;2])->Arc<Self> { Arc::new(Self{queue,last_ordinal,failed:AtomicBool::new(false)}) }
    fn mark_failed(&self) { self.failed.store(true,Ordering::Release); }
    fn is_failed(&self)->bool { self.failed.load(Ordering::Acquire) }
}
struct Job { id:u64,vm:Vm,_guard:Guard,progress:Progress,started:AtomicBool,accepted:AtomicBool,fence:Fence,commands:Vec<()>,inputs:Vec<InputFence>,origin:Arc<FenceOrigin>,fw_waits:Vec<Arc<FenceOrigin>>,orders:Vec<Order>,queue:Arc<QueueState>,full_retired:AtomicU64,terminal_retired:AtomicBool }
struct Shared { crashed:AtomicBool,sleeping:AtomicBool,recovering:AtomicBool,contended:AtomicBool,submitted:Mutex<Vec<u64>>,submit_result:Result }
impl Shared {
    fn new()->Arc<Self> {
        Arc::new(Self{crashed:AtomicBool::new(false),sleeping:AtomicBool::new(false),recovering:AtomicBool::new(false),contended:AtomicBool::new(false),submitted:Mutex::new(vec![]),submit_result:Ok(())})
    }
    fn note_submitter(&self) {}
    fn wake_publish(&self) {}
    fn submit(&self,job:Arc<Job>)->Result {
        self.submitted.lock().unwrap().push(job.id); self.submit_result
    }
    fn try_submit_inline(&self,job:Arc<Job>)->Option<Result> {
        if self.contended.load(Ordering::Acquire) || self.sleeping.load(Ordering::Acquire) || self.recovering.load(Ordering::Acquire) { return None; }
        Some(self.submit(job))
    }
}
impl Job {
    METHODS
}
struct SchedJob { job:Arc<Job>, shared:Arc<Shared>, pending_sched:Arc<AtomicUsize>, counted:bool }
impl SchedJob {
    fn finish_pending(&mut self) {
        if self.counted {
            self.counted = false;
            self.pending_sched.fetch_sub(1, Ordering::AcqRel);
        }
    }
    fn run(&mut self) {
        self.job.start(&self.shared);
        self.finish_pending();
    }
}
impl Drop for SchedJob {
    fn drop(&mut self) {
        self.finish_pending();
    }
}
struct ArmedJob { job:SchedJob, pushed:Arc<Mutex<Vec<u64>>>, queue:Arc<Mutex<Vec<SchedJob>>> }
impl ArmedJob {
    fn push(self) {
        self.pushed.lock().unwrap().push(self.job.job.id);
        self.queue.lock().unwrap().push(self.job);
    }
}
struct PendingJob { job:SchedJob, deps:Vec<InputFence>, pushed:Arc<Mutex<Vec<u64>>>, queue:Arc<Mutex<Vec<SchedJob>>>, fail_dep:bool }
impl PendingJob {
    fn add_dependency(&mut self, fence:InputFence)->Result {
        if self.fail_dep { return Err(ENOMEM); }
        self.deps.push(fence);
        Ok(())
    }
    fn arm(self)->ArmedJob { ArmedJob { job:self.job, pushed:self.pushed, queue:self.queue } }
}
struct Entity { new_jobs:AtomicUsize, pushed:Arc<Mutex<Vec<u64>>>, queue:Arc<Mutex<Vec<SchedJob>>>, fail_new:bool, fail_dep:bool }
impl Entity {
    fn new()->Self { Self { new_jobs:AtomicUsize::new(0), pushed:Arc::new(Mutex::new(vec![])), queue:Arc::new(Mutex::new(vec![])), fail_new:false, fail_dep:false } }
    fn new_job(&mut self, _credits:u32, job:SchedJob)->Result<PendingJob> {
        self.new_jobs.fetch_add(1, Ordering::AcqRel);
        if self.fail_new { return Err(ENOMEM); }
        Ok(PendingJob { job, deps:vec![], pushed:self.pushed.clone(), queue:self.queue.clone(), fail_dep:self.fail_dep })
    }
    fn drain_run(&self) {
        let mut jobs = core::mem::take(&mut *self.queue.lock().unwrap());
        for mut job in jobs.drain(..) {
            job.run();
        }
    }
}
struct Scheduler { entity:Entity, pending_sched:Arc<AtomicUsize> }
impl Scheduler {
    fn new()->Self { Self { entity:Entity::new(), pending_sched:Arc::new(AtomicUsize::new(0)) } }
    PUSH_METHOD
}
fn job(id:u64,status:Arc<Status>)->Arc<Job> {
    job_with_inputs(id,status,vec![])
}
fn job_with_inputs(id:u64,status:Arc<Status>,inputs:Vec<InputFence>)->Arc<Job> {
    let orders=vec![Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap(),Order::new([0,0],[0,1],[NONE;2],[0;2],1).unwrap()];
    job_with_orders(id,status,inputs,orders,Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)}))
}
fn job_with_orders(id:u64,status:Arc<Status>,inputs:Vec<InputFence>,orders:Vec<Order>,queue:Arc<QueueState>)->Arc<Job> {
    let origin=FenceOrigin::new(queue.clone(),[None,Some(orders.len() as u64)]);
    Arc::new(Job{id,vm:Vm{status:status.clone()},_guard:Guard(AtomicBool::new(false)),
        progress:Progress{signaled:AtomicBool::new(false),sent:AtomicU32::new(0),next:AtomicU32::new(0),pending:AtomicBool::new(false)},started:AtomicBool::new(false),accepted:AtomicBool::new(true),
        fence:Fence{vm:status,signals:Mutex::new(vec![])},commands:vec![();orders.len()],inputs,origin,fw_waits:vec![],orders,queue,
        full_retired:AtomicU64::new(0),terminal_retired:AtomicBool::new(false)})
}
#[test]
fn a_failed_vm_is_rejected_without_an_input_error() {
    let status=Arc::new(Status::new()); status.record(EIO);
    let queued=job(1,status); let shared=Shared::new(); queued.start(&shared);
    assert!(shared.submitted.lock().unwrap().is_empty());
    assert_eq!(queued.fence.signals.lock().unwrap()[0],(Err(EIO),-5));
}
#[test]
fn hardware_error_is_latched_before_deferred_completions() {
    let status=Arc::new(Status::new()); let failed=job(1,status.clone()); let queued=job(2,status.clone());
    failed.latch_failure(EIO);
    assert_eq!(status.get(),-5); assert!(failed._guard.0.load(Ordering::Acquire));
    assert!(failed.fence.signals.lock().unwrap().is_empty());
    let shared=Shared::new(); queued.start(&shared);
    assert!(shared.submitted.lock().unwrap().is_empty());
    failed.complete(Err(EIO));
    assert_eq!(failed.fence.signals.lock().unwrap().len(),1);
}
#[test]
fn healthy_vm_can_still_submit() {
    let queued=job(1,Arc::new(Status::new())); let shared=Shared::new(); queued.start(&shared);
    assert_eq!(*shared.submitted.lock().unwrap(),vec![1]);
    assert!(queued.fence.signals.lock().unwrap().is_empty());
}
#[test]
fn cancellation_reports_without_sticky_quarantine() {
    let status=Arc::new(Status::new()); let queued=job(1,status.clone());
    queued.latch_failure(ECANCELED); queued.complete(Err(ECANCELED));
    assert_eq!(status.get(),0); assert_eq!(status.reported.load(Ordering::Acquire),-125);
    assert!(!queued._guard.0.load(Ordering::Acquire));
    assert_eq!(queued.fence.signals.lock().unwrap()[0],(Err(ECANCELED),-125));
}
#[test]
fn completion_is_once_and_does_not_replace_first_failure() {
    let status=Arc::new(Status::new()); let failed=job(1,status.clone());
    failed.latch_failure(ENODEV); failed.complete(Err(EIO)); failed.complete(Ok(()));
    assert_eq!(status.get(),-19); assert_eq!(failed.fence.signals.lock().unwrap().len(),1);
    assert_eq!(failed.fence.signals.lock().unwrap()[0].1,-19);
}
#[test]
fn crashed_gpu_also_refuses_a_healthy_vm() {
    let queued=job(1,Arc::new(Status::new())); let shared=Shared::new();
    shared.crashed.store(true,Ordering::Release); queued.start(&shared);
    assert!(shared.submitted.lock().unwrap().is_empty());
    assert_eq!(queued.fence.signals.lock().unwrap()[0],(Err(EIO),-5));
}
#[test]
fn input_error_from_a_different_vm_prevents_all_publication() {
    let input=InputFence::new(-5);
    let queued=job_with_inputs(1,Arc::new(Status::new()),vec![input]); let shared=Shared::new();
    queued.start(&shared);
    assert!(shared.submitted.lock().unwrap().is_empty());
    assert_eq!(queued.fence.signals.lock().unwrap()[0],(Err(ECANCELED),-125));
    assert_eq!(queued.vm.status.get(),0);
    assert!(!queued._guard.0.load(Ordering::Acquire),"never-published input failure needs no GPU quarantine");
}
#[test]
fn every_command_checks_every_retained_input() {
    let success=InputFence::new(1); let pending=InputFence::new(0);
    let queued=job_with_inputs(1,Arc::new(Status::new()),vec![success,pending.clone()]);
    for command in [0,1] { assert_eq!(queued.check_command_dependencies(command),Err(EAGAIN)); }
    pending.signal(1);
    assert_eq!(queued.check_command_dependencies(0),Ok(()));
    assert_eq!(queued.check_command_dependencies(1),Err(EAGAIN));
    queued.command_retired(0).unwrap();
    assert_eq!(queued.check_command_dependencies(1),Ok(()));
    assert_eq!(queued.check_command_dependencies(2),Err(EINVAL));
}
#[test]
fn no_command_ignores_a_failed_input_after_an_earlier_success() {
    let queued=job_with_inputs(1,Arc::new(Status::new()),vec![InputFence::new(1),InputFence::new(-19)]);
    for command in [0,1] { assert_eq!(queued.check_command_dependencies(command),Err(ECANCELED)); }
}
#[test]
fn pending_input_really_waits_outside_the_engine() {
    let input=InputFence::new(0);
    let queued=job_with_inputs(1,Arc::new(Status::new()),vec![input.clone()]); let shared=Shared::new();
    let runner={ let queued=queued.clone(); let shared=shared.clone(); std::thread::spawn(move||queued.start(&shared)) };
    while input.0.waits.load(Ordering::Acquire)==0 { std::thread::yield_now(); }
    assert!(shared.submitted.lock().unwrap().is_empty());
    input.signal(1); runner.join().unwrap();
    assert_eq!(*shared.submitted.lock().unwrap(),vec![1]);
    assert!(queued.check_command_dependencies(0).is_ok());
    queued.command_retired(0).unwrap();
    assert!(queued.check_command_dependencies(1).is_ok());
}
#[test]
fn interrupted_input_wait_never_reaches_firmware() {
    let input=InputFence::new(0); input.0.interrupted.store(-4,Ordering::Release);
    let queued=job_with_inputs(1,Arc::new(Status::new()),vec![input]); let shared=Shared::new();
    queued.start(&shared);
    assert!(shared.submitted.lock().unwrap().is_empty());
    assert_eq!(queued.fence.signals.lock().unwrap()[0],(Err(ECANCELED),-125));
    assert_eq!(queued.vm.status.get(),0);
}
#[test]
fn inherited_cancellation_closes_only_the_queue_so_later_ordinals_cannot_hang() {
    let queued=job_with_inputs(1,Arc::new(Status::new()),vec![InputFence::new(-125)]);
    let shared=Shared::new(); queued.start(&shared);
    assert_eq!(queued.vm.status.get(),0);
    assert_eq!(queued.queue.check(&queued.orders[0]),Err(ECANCELED));
    assert!(shared.submitted.lock().unwrap().is_empty());
    assert_eq!(queued.check_command_dependencies(1),Err(ECANCELED));
    assert!(!queued._guard.0.load(Ordering::Acquire));
}
#[test]
fn failed_producer_preserves_other_queues_and_does_not_bless_ordinal_holes() {
    for error in [-5,-19,-110,-12,-22,-125] {
        let status=Arc::new(Status::new());
        let failed=job_with_inputs(1,status.clone(),vec![InputFence::new(error)]);
        let later=job_with_orders(2,status.clone(),vec![],vec![
            Order::new([0,2],[0,0],[NONE;2],[0,2],1).unwrap()],failed.queue.clone());
        let independent=job(3,status.clone());
        let shared=Shared::new();
        failed.start(&shared);
        assert_eq!(status.get(),0,"producer errno must never fault the consumer VM");
        assert_eq!(failed.queue.completed[1].load(Ordering::Acquire),0);
        later.start(&shared);
        assert_eq!(later.check_command_dependencies(0),Err(ECANCELED));
        assert_eq!(later.fence.signals.lock().unwrap()[0].0,Err(ECANCELED));
        assert_eq!(failed.queue.completed[1].load(Ordering::Acquire),0,"no synthetic receipt across a cancelled ordinal");
        independent.start(&shared);
        assert_eq!(*shared.submitted.lock().unwrap(),vec![3]);
        assert_eq!(independent.check_command_dependencies(0),Ok(()));
        assert_eq!(status.get(),0);
        assert!(!failed._guard.0.load(Ordering::Acquire));
    }
}
#[test]
fn younger_failed_input_does_not_cancel_older_multi_command_job() {
    let status=Arc::new(Status::new());let older=job(1,status.clone());
    let younger=job_with_orders(2,status.clone(),vec![InputFence::new(-5)],vec![
        Order::new([0,2],[0,0],[NONE;2],[0;2],1).unwrap(),
        Order::new([0,2],[0,1],[NONE;2],[0;2],1).unwrap()],older.queue.clone());
    let shared=Shared::new();older.start(&shared);
    older.progress.sent.store(1,Ordering::Release);
    younger.start(&shared);
    assert_eq!(older.queue.cancelled_at[1].load(Ordering::Acquire),3);
    assert_eq!(status.get(),0);
    older.command_retired(0).unwrap();
    assert_eq!(older.check_command_dependencies(1),Ok(()));
    older.command_retired(1).unwrap();
    assert_eq!(older.queue.completed[1].load(Ordering::Acquire),2);
    assert_eq!(younger.check_command_dependencies(0),Err(ECANCELED));
    let newer=job_with_orders(3,status.clone(),vec![],vec![
        Order::new([0,4],[0,0],[NONE;2],[0;2],1).unwrap()],older.queue.clone());
    newer.start(&shared);
    assert_eq!(newer.check_command_dependencies(0),Err(ECANCELED));
    assert_eq!(older.queue.completed[1].load(Ordering::Acquire),2);
    let independent=job(4,status.clone());independent.start(&shared);
    assert_eq!(*shared.submitted.lock().unwrap(),vec![1,4]);
}
#[test]
fn cancellation_checks_both_command_ordinal_and_cross_engine_failed_prefix() {
    let queue=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let cutoff=Order::new([0,2],[0,0],[NONE;2],[0;2],1).unwrap();queue.cancel(&[cutoff],0);
    let earlier=Order::new([0,1],[0,0],[NONE;2],[0;2],1).unwrap();
    let independent_render=Order::new([0,3],[0,0],[NONE;2],[0;2],0).unwrap();
    let failed_prefix=Order::new([0,3],[0,0],[NONE;2],[0,3],0).unwrap();
    assert_eq!(queue.check(&earlier),Ok(()));
    assert_eq!(queue.check(&independent_render),Ok(()));
    assert_eq!(queue.check(&failed_prefix),Err(ECANCELED));
    let earlier_hole=Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap();queue.cancel(&[earlier_hole],0);
    queue.cancel(&[cutoff],0);assert_eq!(queue.cancelled_at[1].load(Ordering::Acquire),1);
    assert_eq!(queue.check(&earlier),Err(ECANCELED));
}
#[test]
fn exact_prefix_does_not_wait_for_the_aggregate_or_next_ordinal() {
    let queue=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let status=Arc::new(Status::new());
    let old=job_with_orders(1,status.clone(),vec![],vec![
        Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap(),
        Order::new([0,0],[0,1],[NONE;2],[0;2],1).unwrap()],queue.clone());
    let later=job_with_orders(2,status,vec![],vec![
        Order::new([0,2],[0,0],[NONE;2],[0,1],0).unwrap()],queue.clone());
    assert_eq!(later.check_command_dependencies(0),Err(EAGAIN));
    assert_eq!(queue.completed[1].load(Ordering::Acquire),0);
    old.command_retired(0).unwrap();
    assert_eq!(queue.completed[1].load(Ordering::Acquire),1);
    assert!(old.fence.signals.lock().unwrap().is_empty(),"older aggregate is still incomplete");
    assert_eq!(later.check_command_dependencies(0),Ok(()),"prefix1 must not wait for compute2");
    later.command_retired(0).unwrap();
    assert_eq!(queue.completed[0].load(Ordering::Acquire),1);
    assert!(old.fence.signals.lock().unwrap().is_empty());
    old.command_retired(1).unwrap();
    assert_eq!(queue.completed[1].load(Ordering::Acquire),2);
}
#[test]
fn exact_frontier_cannot_skip_or_double_retire_a_command() {
    let queued=job(1,Arc::new(Status::new()));
    assert_eq!(queued.command_retired(1),Err(EIO));
    assert_eq!(queued.queue.completed[1].load(Ordering::Acquire),0);
    queued.command_retired(0).unwrap();
    assert_eq!(queued.command_retired(0),Err(EIO));
    assert_eq!(queued.command_retired(2),Err(EINVAL));
    assert_eq!(queued.queue.completed[1].load(Ordering::Acquire),1);
}
#[test]
fn failed_command_leaves_an_unsatisfied_frontier_and_sticky_vm() {
    let queued=job(1,Arc::new(Status::new()));
    queued.latch_failure(EIO);
    assert_eq!(queued.queue.completed[1].load(Ordering::Acquire),0);
    assert_eq!(queued.vm.status.get(),-5);
    assert_eq!(queued.check_command_dependencies(1),Err(EIO));
}
#[test]
fn terminal_full_error_keeps_sticky_status_without_mapping_quarantine() {
    let queued=job(1,Arc::new(Status::new()));
    queued.progress.sent.store(2,Ordering::Release);
    queued.progress.next.store(1,Ordering::Release);
    queued.progress.pending.store(true,Ordering::Release);
    queued.note_full_retirement(0,1).unwrap();
    assert_eq!(queued.mark_terminal_retired(),Err(EIO));
    queued.note_full_retirement(1,1).unwrap();
    queued.progress.next.store(2,Ordering::Release);
    assert_eq!(queued.mark_terminal_retired(),Err(EIO),"future publication must first be removed");
    queued.progress.pending.store(false,Ordering::Release);
    queued.mark_terminal_retired().unwrap();
    assert_eq!(queued.check_command_dependencies(0),Err(EIO));
    queued.latch_failure(EIO); queued.complete(Err(EIO));
    assert_eq!(queued.vm.status.get(),-5);
    assert!(!queued._guard.0.load(Ordering::Acquire));
    assert_eq!(queued.fence.signals.lock().unwrap()[0],(Err(EIO),-5));
}
#[test]
fn aborted_earlier_owner_cannot_gain_terminal_full_proof() {
    let queued=job(1,Arc::new(Status::new()));
    queued.progress.sent.store(2,Ordering::Release);
    queued.progress.next.store(2,Ordering::Release);
    queued.note_full_retirement(0,1).unwrap();
    assert_eq!(queued.mark_terminal_retired(),Err(EIO));
    queued.latch_failure(EIO);
    assert!(queued._guard.0.load(Ordering::Acquire));
}
#[test]
fn never_published_refusal_needs_no_mapping_quarantine() {
    let queued=job(1,Arc::new(Status::new()));
    queued.mark_terminal_retired().unwrap();
    queued.latch_failure(EINVAL); queued.complete(Err(EINVAL));
    assert_eq!(queued.vm.status.get(),0);
    assert_eq!(queued.queue.check(&queued.orders[0]),Err(ECANCELED));
    assert!(!queued._guard.0.load(Ordering::Acquire));
}
#[test]
fn never_published_capacity_failure_closes_only_its_queue() {
    let status=Arc::new(Status::new());let failed=job(1,status.clone());
    let shared=Arc::new(Shared{crashed:AtomicBool::new(false),sleeping:AtomicBool::new(false),recovering:AtomicBool::new(false),contended:AtomicBool::new(false),submitted:Mutex::new(vec![]),submit_result:Err(ENOMEM)});
    failed.start(&shared);
    assert_eq!(failed.fence.signals.lock().unwrap()[0],(Err(ENOMEM),-12));
    assert_eq!(failed.queue.check(&failed.orders[0]),Err(ECANCELED));assert_eq!(status.get(),0);
    assert_eq!(failed.queue.completed[1].load(Ordering::Acquire),0);
    let healthy=job(2,status.clone());let shared=Shared::new();healthy.start(&shared);
    assert_eq!(*shared.submitted.lock().unwrap(),vec![2]);assert_eq!(status.get(),0);
}
#[test]
fn full_receipts_are_unique_bounded_and_allow_cross_engine_order() {
    let queued=job(1,Arc::new(Status::new()));
    assert_eq!(queued.note_full_retirement(0,1),Err(EINVAL));
    queued.progress.sent.store(2,Ordering::Release);
    assert_eq!(queued.note_full_retirement(0,0),Err(EINVAL));
    // Command 1 may retire before command 0 when compute and render run concurrently.
    queued.note_full_retirement(1,1).unwrap();
    assert_eq!(queued.note_full_retirement(1,1),Err(EIO));
    assert_eq!(queued.mark_terminal_retired(),Err(EIO));
    queued.note_full_retirement(0,1).unwrap();
    assert_eq!(queued.note_full_retirement(0,1),Err(EIO));
    assert_eq!(queued.note_full_retirement(1,2),Err(EINVAL));
    assert_eq!(queued.note_full_retirement(u32::MAX,1),Err(EOVERFLOW));
    queued.progress.next.store(2,Ordering::Release);
    queued.mark_terminal_retired().unwrap();
}
#[test]
fn inline_start_submits_once_and_falls_back_on_pending_input_or_contention() {
    let status=Arc::new(Status::new());
    let shared=Shared::new();
    // Ready job with signalled input submits inline; subsequent drm_sched start() is a no-op.
    let ready=job_with_inputs(10,status.clone(),vec![InputFence::new(1)]);
    assert!(ready.try_start_inline(&shared));
    assert!(ready.is_started());
    assert_eq!(*shared.submitted.lock().unwrap(),vec![10]);
    ready.start(&shared);
    assert_eq!(*shared.submitted.lock().unwrap(),vec![10]);
    // Unsignalled input defers to drm_sched without marking started.
    let pending=job_with_inputs(11,status.clone(),vec![InputFence::new(0)]);
    assert!(!pending.try_start_inline(&shared));
    assert!(!pending.is_started());
    assert_eq!(*shared.submitted.lock().unwrap(),vec![10]);
    // Recovering device defers to drm_sched without blocking or marking started.
    shared.recovering.store(true,Ordering::Release);
    let rec=job(13,status.clone());
    assert!(!rec.try_start_inline(&shared));
    assert!(!rec.is_started());
    shared.recovering.store(false,Ordering::Release);
    // Contended device mutex defers to drm_sched without blocking or marking started.
    shared.contended.store(true,Ordering::Release);
    let contended=job(12,status.clone());
    assert!(!contended.try_start_inline(&shared));
    assert!(!contended.is_started());
    assert_eq!(*shared.submitted.lock().unwrap(),vec![10]);
    shared.contended.store(false,Ordering::Release);
    contended.start(&shared);
    assert!(contended.is_started());
    assert_eq!(*shared.submitted.lock().unwrap(),vec![10,12]);
}
#[test]
fn scheduler_push_bypasses_drm_sched_on_inline_start_and_rolls_back_on_fallback_enomem() {
    let status=Arc::new(Status::new());
    let shared=Shared::new();
    let mut sched=Scheduler::new();
    // Ready jobs bypass entity.new_job() and armed.push() completely.
    let j1=job_with_inputs(1,status.clone(),vec![InputFence::new(1)]);
    j1.accepted.store(false,Ordering::Release);
    sched.push(j1.clone(),shared.clone()).unwrap();
    assert!(j1.is_started());
    assert!(j1.accepted.load(Ordering::Acquire));
    assert_eq!(sched.entity.new_jobs.load(Ordering::Acquire),0);
    assert!(sched.entity.pushed.lock().unwrap().is_empty());
    assert_eq!(*shared.submitted.lock().unwrap(),vec![1]);
    // Second ready job also bypasses drm_sched because j1 is already started.
    let j2=job(2,status.clone());
    sched.push(j2.clone(),shared.clone()).unwrap();
    assert!(j2.is_started());
    assert_eq!(sched.entity.new_jobs.load(Ordering::Acquire),0);
    assert_eq!(*shared.submitted.lock().unwrap(),vec![1,2]);
    // Unsignalled input falls back to drm_sched; subsequent job waits for predecessor start.
    let f3=InputFence::new(0);
    let j3=job_with_inputs(3,status.clone(),vec![f3.clone()]);
    sched.push(j3.clone(),shared.clone()).unwrap();
    assert!(!j3.is_started());
    assert_eq!(sched.entity.new_jobs.load(Ordering::Acquire),1);
    assert_eq!(*sched.entity.pushed.lock().unwrap(),vec![3]);
    let j4=job(4,status.clone());
    sched.push(j4.clone(),shared.clone()).unwrap();
    assert!(!j4.is_started());
    assert_eq!(sched.entity.new_jobs.load(Ordering::Acquire),2);
    assert_eq!(*sched.entity.pushed.lock().unwrap(),vec![3,4]);
    // Fallback ENOMEM rolls back accepted and pending_sched without poisoning queue ordering.
    f3.signal(1);
    sched.entity.drain_run();
    assert_eq!(sched.pending_sched.load(Ordering::Acquire),0);
    sched.entity.fail_new=true;
    let j5=job_with_inputs(5,status.clone(),vec![InputFence::new(0)]);
    j5.accepted.store(false,Ordering::Release);
    assert_eq!(sched.push(j5.clone(),shared.clone()),Err(ENOMEM));
    assert!(!j5.accepted.load(Ordering::Acquire));
    assert!(!j5.is_started());
    assert_eq!(sched.pending_sched.load(Ordering::Acquire),0);
    // Scheduler retains zero Arc<Job> references after inline or drm_sched completion.
    let w1=Arc::downgrade(&j1);
    let w3=Arc::downgrade(&j3);
    let w4=Arc::downgrade(&j4);
    drop((j1,j2,j3,j4,j5));
    assert!(w1.upgrade().is_none(), "inline job must not be retained by Scheduler");
    assert!(w3.upgrade().is_none(), "drm_sched job must not be retained by Scheduler after run");
    assert!(w4.upgrade().is_none(), "queued job must not be retained by Scheduler after run");
}
#[test]
fn compute_ooo_retirement_tracks_gaps_and_rejects_unpublished_or_duplicate() {
    let queue=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[AtomicU64::new(0),AtomicU64::new(4)],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let orders=vec![
        Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap(),
        Order::new([0,0],[0,1],[NONE;2],[0;2],1).unwrap(),
        Order::new([0,0],[0,2],[NONE;2],[0;2],1).unwrap(),
        Order::new([0,0],[0,3],[NONE;2],[0;2],1).unwrap(),
        Order::new([0,0],[0,4],[NONE;2],[0;2],1).unwrap(),
    ];
    let qjob=job_with_orders(1,Arc::new(Status::new()),vec![],orders,queue.clone());
    // Unpublished sequence 5 cannot retire OOO.
    assert_eq!(qjob.command_retired(4),Err(EIO));
    // Sequences 2 and 4 retire OOO before 1 and 3.
    qjob.command_retired(1).unwrap();
    qjob.command_retired(3).unwrap();
    assert_eq!(queue.completed[COMPUTE].load(Ordering::Acquire),0);
    assert!(!queue.is_compute_retired(1));
    assert!(queue.is_compute_retired(2));
    assert!(!queue.is_compute_retired(3));
    assert!(queue.is_compute_retired(4));
    // Duplicate OOO retirement is rejected.
    assert_eq!(qjob.command_retired(1),Err(EIO));
    assert_eq!(qjob.command_retired(3),Err(EIO));
    // Retiring 1 advances contiguous frontier to 2 and re-anchors bit 0 to seq 4!
    qjob.command_retired(0).unwrap();
    assert_eq!(queue.completed[COMPUTE].load(Ordering::Acquire),2);
    assert!(queue.is_compute_retired(1));
    assert!(queue.is_compute_retired(2));
    assert!(!queue.is_compute_retired(3));
    assert!(queue.is_compute_retired(4));
    assert_eq!(qjob.command_retired(3),Err(EIO));
    // Retiring 3 drains seq 4 as well, advancing contiguous frontier to 4 with clean bitmap.
    qjob.command_retired(2).unwrap();
    assert_eq!(queue.completed[COMPUTE].load(Ordering::Acquire),4);
    assert_eq!(queue.compute_ooo.load(Ordering::Acquire),0);
}
#[test]
fn fw_wait_defers_until_producer_queue_publishes_and_propagates_failure() {
    let q1=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let q2=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let status=Arc::new(Status::new());
    let p_orders=vec![Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap()];
    let c_orders=vec![Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap()];
    let prod=job_with_orders(1,status.clone(),vec![],p_orders,q1.clone());
    let cons=Arc::new(Job{
        id:2,vm:Vm{status:status.clone()},_guard:Guard(AtomicBool::new(false)),
        progress:Progress{signaled:AtomicBool::new(false),sent:AtomicU32::new(0),next:AtomicU32::new(0),pending:AtomicBool::new(false)},
        started:AtomicBool::new(false),accepted:AtomicBool::new(true),
        fence:Fence{vm:status,signals:Mutex::new(vec![])},commands:vec![()],inputs:vec![],
        origin:FenceOrigin::new(q2.clone(),[None,Some(1)]),
        fw_waits:vec![prod.origin.clone()],orders:c_orders,queue:q2.clone(),
        full_retired:AtomicU64::new(0),terminal_retired:AtomicBool::new(false),
    });
    assert!(cons.waits_on_queue(&q1));
    assert!(!cons.waits_on_queue(&q2));
    // Producer not yet published -> consumer defers with EAGAIN.
    assert_eq!(cons.check_command_dependencies(0),Err(EAGAIN));
    // Once producer publishes ordinal 1, consumer is ready for firmware handoff.
    q1.published[COMPUTE].store(1,Ordering::Release);
    assert_eq!(cons.check_command_dependencies(0),Ok(()));
    // Producer failure marks origin failed and cancels consumer with ECANCELED.
    prod.latch_failure(ECANCELED);
    assert!(prod.origin.is_failed());
    assert_eq!(cons.check_command_dependencies(0),Err(ECANCELED));
}
#[test]
fn fw_wait_cancels_when_producer_queue_cancels_ordinal() {
    let q1=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[AtomicU64::new(0),AtomicU64::new(1)],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let q2=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],published:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2],compute_ooo:AtomicU64::new(0)});
    let status=Arc::new(Status::new());
    let p_orders=vec![Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap()];
    let c_orders=vec![Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap()];
    let prod=job_with_orders(1,status.clone(),vec![],p_orders.clone(),q1.clone());
    let cons=Arc::new(Job{
        id:2,vm:Vm{status:status.clone()},_guard:Guard(AtomicBool::new(false)),
        progress:Progress{signaled:AtomicBool::new(false),sent:AtomicU32::new(0),next:AtomicU32::new(0),pending:AtomicBool::new(false)},
        started:AtomicBool::new(false),accepted:AtomicBool::new(true),
        fence:Fence{vm:status,signals:Mutex::new(vec![])},commands:vec![()],inputs:vec![],
        origin:FenceOrigin::new(q2.clone(),[None,Some(1)]),
        fw_waits:vec![prod.origin.clone()],orders:c_orders,queue:q2,
        full_retired:AtomicU64::new(0),terminal_retired:AtomicBool::new(false),
    });
    assert_eq!(cons.check_command_dependencies(0),Ok(()));
    q1.cancel(&p_orders,0);
    assert_eq!(cons.check_command_dependencies(0),Err(ECANCELED));
}
'''.replace('QUEUE_METHODS', queue_methods).replace('ORDER_SOURCE', order_source).replace('FENCE_METHODS', fence_methods).replace('METHODS', methods.replace('super::fence::','fence::').replace('super::command::','command::')).replace('PUSH_METHOD', push_method)

with tempfile.TemporaryDirectory(prefix='m4-rust-job-') as directory:
    directory=pathlib.Path(directory); test=directory/'test.rs'; test.write_text(harness)
    binary=directory/'test'
    subprocess.run(rustc() + ['--edition=2021','--test',str(test),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary),'--nocapture'],check=True,text=True,capture_output=True)
    print(result.stdout,end='')
    (OUT/'job-failure-host.log').write_text(result.stdout+result.stderr)
    (OUT/'job-failure-host.json').write_text(json.dumps({
        'source':str(SOURCE),'sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'result':'pass','test_groups':25,
        'limitations':'Production Job methods with host VM/fence mocks; actual DRM scheduler and firmware publication gates require kernel and execution integration validation.'
    },indent=2)+'\n')
