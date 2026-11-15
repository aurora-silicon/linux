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

def function(text, name):
    marker = text.index('fn ' + name + '(')
    start = text.rfind('\n', 0, marker) + 1
    first = text.index('{', marker)
    depth = 1
    at = first + 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[start:at]

queue_methods = '\n'.join(function(queue_source, name) for name in ['cancel', 'check'])
methods = '\n'.join(function(source, name) for name in ['latch_failure', 'complete', 'finish', 'start', 'check_command_dependencies', 'command_retired', 'note_full_retirement', 'mark_terminal_retired'])
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
const EOVERFLOW:Error=Error(-75); const NONE:u16=u16::MAX;
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
mod command { pub(crate) use super::Order; }
struct QueueState { completed:[AtomicU64;2],cancelled_at:[AtomicU64;2] }
impl QueueState { QUEUE_METHODS }
struct Job { id:u64,vm:Vm,_guard:Guard,progress:Progress,started:AtomicBool,accepted:AtomicBool,fence:Fence,commands:Vec<()>,inputs:Vec<InputFence>,orders:Vec<Order>,queue:Arc<QueueState>,full_retired:AtomicU32,terminal_retired:AtomicBool }
struct Shared { crashed:AtomicBool,submitted:Mutex<Vec<u64>>,submit_result:Result }
impl Shared {
    fn new()->Arc<Self> {
        Arc::new(Self{crashed:AtomicBool::new(false),submitted:Mutex::new(vec![]),submit_result:Ok(())})
    }
    fn submit(&self,job:Arc<Job>)->Result {
        self.submitted.lock().unwrap().push(job.id); self.submit_result
    }
}
impl Job {
    METHODS
}
fn job(id:u64,status:Arc<Status>)->Arc<Job> {
    job_with_inputs(id,status,vec![])
}
fn job_with_inputs(id:u64,status:Arc<Status>,inputs:Vec<InputFence>)->Arc<Job> {
    let orders=vec![Order::new([0,0],[0,0],[NONE;2],[0;2],1).unwrap(),Order::new([0,0],[0,1],[NONE;2],[0;2],1).unwrap()];
    job_with_orders(id,status,inputs,orders,Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2]}))
}
fn job_with_orders(id:u64,status:Arc<Status>,inputs:Vec<InputFence>,orders:Vec<Order>,queue:Arc<QueueState>)->Arc<Job> {
    Arc::new(Job{id,vm:Vm{status:status.clone()},_guard:Guard(AtomicBool::new(false)),
        progress:Progress{signaled:AtomicBool::new(false),sent:AtomicU32::new(0),next:AtomicU32::new(0),pending:AtomicBool::new(false)},started:AtomicBool::new(false),accepted:AtomicBool::new(true),
        fence:Fence{vm:status,signals:Mutex::new(vec![])},commands:vec![();orders.len()],inputs,orders,queue,
        full_retired:AtomicU32::new(0),terminal_retired:AtomicBool::new(false)})
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
    let queue=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2]});
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
    let queue=Arc::new(QueueState{completed:[const{AtomicU64::new(0)};2],cancelled_at:[const{AtomicU64::new(0)};2]});
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
    let shared=Arc::new(Shared{crashed:AtomicBool::new(false),submitted:Mutex::new(vec![]),submit_result:Err(ENOMEM)});
    failed.start(&shared);
    assert_eq!(failed.fence.signals.lock().unwrap()[0],(Err(ENOMEM),-12));
    assert_eq!(failed.queue.check(&failed.orders[0]),Err(ECANCELED));assert_eq!(status.get(),0);
    assert_eq!(failed.queue.completed[1].load(Ordering::Acquire),0);
    let healthy=job(2,status.clone());let shared=Shared::new();healthy.start(&shared);
    assert_eq!(*shared.submitted.lock().unwrap(),vec![2]);assert_eq!(status.get(),0);
}
#[test]
fn full_receipts_are_consecutive_bounded_and_once() {
    let queued=job(1,Arc::new(Status::new()));
    assert_eq!(queued.note_full_retirement(0,1),Err(EINVAL));
    queued.progress.sent.store(2,Ordering::Release);
    assert_eq!(queued.note_full_retirement(0,0),Err(EINVAL));
    assert_eq!(queued.note_full_retirement(1,1),Err(EIO));
    queued.note_full_retirement(0,1).unwrap();
    assert_eq!(queued.note_full_retirement(0,1),Err(EIO));
    assert_eq!(queued.note_full_retirement(1,2),Err(EINVAL));
    assert_eq!(queued.note_full_retirement(u32::MAX,1),Err(EOVERFLOW));
    queued.note_full_retirement(1,1).unwrap();
}
'''.replace('QUEUE_METHODS', queue_methods).replace('ORDER_SOURCE', order_source).replace('FENCE_METHODS', fence_methods).replace('METHODS', methods.replace('super::fence::','fence::'))

with tempfile.TemporaryDirectory(prefix='m4-rust-job-') as directory:
    directory=pathlib.Path(directory); test=directory/'test.rs'; test.write_text(harness)
    binary=directory/'test'
    subprocess.run(rustc() + ['--edition=2021','--test',str(test),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary),'--nocapture'],check=True,text=True,capture_output=True)
    print(result.stdout,end='')
    (OUT/'job-failure-host.log').write_text(result.stdout+result.stderr)
    (OUT/'job-failure-host.json').write_text(json.dumps({
        'source':str(SOURCE),'sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'result':'pass','test_groups':23,
        'limitations':'Production Job methods with host VM/fence mocks; actual DRM scheduler and firmware publication gates require kernel and execution integration validation.'
    },indent=2)+'\n')
