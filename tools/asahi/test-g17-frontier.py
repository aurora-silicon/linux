#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production G17 status predicate and frontier pruning on a host."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/gpu/drm/asahi/g17/queue/mod.rs').read_text()
fences = (root / 'drivers/gpu/drm/asahi/g17/fence.rs').read_text()


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


helper = block(fences, 'pub(crate) fn completion_status(')
# Model the C fields with UnsafeCell; access the same naturally aligned storage.
helper = helper.replace('core::ptr::addr_of_mut!((*raw).flags).cast()', '(*raw).flags.get()')
helper = helper.replace('core::ptr::addr_of!((*raw).error)', '(*raw).error.get()')
program = r'''
#![allow(dead_code)]
use std::{cell::UnsafeCell,sync::{Arc,atomic::{AtomicUsize,Ordering}}};
#[repr(C)] struct RawFence { flags:UnsafeCell<usize>, error:UnsafeCell<i32>, timestamp:UnsafeCell<u64> }
// Writes precede the signal bit; data is immutable once signalled.
unsafe impl Sync for RawFence {}
#[derive(Clone)] struct Fence(Arc<RawFence>);
impl Fence {
 fn raw(&self)->*mut RawFence{Arc::as_ptr(&self.0).cast_mut()}
 fn pending(error:i32)->Self{Self(Arc::new(RawFence{flags:UnsafeCell::new(6),error:UnsafeCell::new(error),timestamp:UnsafeCell::new(0)}))}
 fn signal(&self,error:i32,timestamp:u64){unsafe{
  self.0.error.get().write(error);self.0.timestamp.get().write(timestamp);
  AtomicUsize::from_ptr(self.0.flags.get()).fetch_or(8,Ordering::Release);
 }}
}
mod bindings {#[allow(non_upper_case_globals)]pub const dma_fence_flag_bits_DMA_FENCE_FLAG_SIGNALED_BIT:u32=3;}
mod fence {use super::*;
''' + helper + r'''
}
struct Fences {ready:Fence,completed:Fence}
struct KVec<T>(Vec<T>);
impl<T>KVec<T>{fn retain(&mut self,f:impl FnMut(&mut T)->bool){self.0.retain_mut(f)}}
impl<T>std::ops::Deref for KVec<T>{type Target=[T];fn deref(&self)->&[T]{&self.0}}
struct Queue {frontiers:[KVec<(u64,Fences)>;2],failures:[Option<(u64,Fence)>;2]}
impl Queue {
''' + block(source, '    fn prune_frontier(') + r'''
}
fn entry(sequence:u64,status:Option<i32>)->(u64,Fences){
 let f=Fence::pending(0);if let Some(status)=status{f.signal(status,100+sequence);}
 (sequence,Fences{ready:f.clone(),completed:f})
}
fn queue(entries:Vec<(u64,Fences)>)->Queue{Queue{frontiers:[KVec(entries),KVec(vec![])],failures:[None,None]}}
fn sequences(q:&Queue,index:usize)->Vec<u64>{q.frontiers[index].iter().map(|e|e.0).collect()}
#[test]fn pending_error_and_other_flags_do_not_mean_completion(){
 let f=Fence::pending(-5);assert_eq!(fence::completion_status(&f),0);
 f.signal(-5,123);assert_eq!(fence::completion_status(&f),-5);
 let ok=Fence::pending(0);ok.signal(0,321);assert_eq!(fence::completion_status(&ok),1);
}
#[test]fn delayed_output_is_not_retired_when_scheduler_or_hardware_is_done(){
 let mut q=queue(vec![entry(1,None)]);
 let ready=Fence::pending(0);ready.signal(0,123);q.frontiers[0].0[0].1.ready=ready;
 q.prune_frontier(0);assert_eq!(sequences(&q,0),vec![1]);
 q.frontiers[0].0[0].1.completed.signal(0,456);
 q.prune_frontier(0);assert!(q.frontiers[0].is_empty());
}
#[test]fn later_completion_does_not_subsume_pending_or_failed_earlier_jobs(){
 let mut q=queue(vec![entry(1,None),entry(2,Some(-125)),entry(3,Some(0)),entry(4,None)]);
 q.prune_frontier(0);assert_eq!(sequences(&q,0),vec![1,4]);assert_eq!(q.failures[0].as_ref().unwrap().0,2);
 q.frontiers[0].0[0].1.completed.signal(-5,222);
 q.prune_frontier(0);assert_eq!(sequences(&q,0),vec![4]);assert_eq!(q.failures[0].as_ref().unwrap().0,1);
 assert_eq!(fence::completion_status(&q.failures[0].as_ref().unwrap().1),-5);
}
#[test]fn each_engine_keeps_its_earliest_error_witness(){
 let mut q=queue(vec![entry(1,Some(-125)),entry(2,Some(-5)),entry(3,None)]);
 q.frontiers[1]=KVec(vec![entry(1,None),entry(2,Some(-12)),entry(3,Some(-5))]);
 q.prune_frontier(0);q.prune_frontier(1);
 assert_eq!(sequences(&q,0),vec![3]);assert_eq!(sequences(&q,1),vec![1]);
 assert_eq!(q.failures[0].as_ref().unwrap().0,1);assert_eq!(q.failures[1].as_ref().unwrap().0,2);
}
#[test]fn prefixes_still_observe_success_pending_and_failed_status(){
 let fs=[entry(1,Some(0)),entry(2,None),entry(3,Some(-125))];
 let checked:Vec<_>=fs.iter().filter(|(_,f)|fence::completion_status(&f.completed)<=0).map(|e|e.0).collect();
 assert_eq!(checked,vec![2,3]);
}
#[test]fn acquired_signal_publishes_error_and_timestamp(){
 for _ in 0..1000 {
  let f=Fence::pending(0);let producer=f.clone();
  let t=std::thread::spawn(move||producer.signal(-125,0x123456789));
  let status=loop{let s=fence::completion_status(&f);if s!=0{break s}std::hint::spin_loop()};
  assert_eq!(status,-125);assert_eq!(unsafe{f.0.timestamp.get().read()},0x123456789);t.join().unwrap();
 }
}
#[test]fn ten_thousand_prunes_preserve_all_live_jobs_then_collect_every_terminal(){
 let mut q=queue((1..=256).map(|i|entry(i,None)).collect());
 for _ in 0..10000{q.prune_frontier(0);assert_eq!(q.frontiers[0].len(),256);}
 for (sequence,f) in q.frontiers[0].iter(){f.completed.signal(if sequence%17==0{-125}else{0},*sequence);}
 q.prune_frontier(0);assert!(q.frontiers[0].is_empty());assert_eq!(q.failures[0].as_ref().unwrap().0,17);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'frontier.rs'
    binary = Path(directory) / 'frontier'
    path.write_text(program)
    subprocess.run(['rustc', '--edition=2021', '-O', '--test', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary), '--test-threads=1'], check=True, timeout=60)
