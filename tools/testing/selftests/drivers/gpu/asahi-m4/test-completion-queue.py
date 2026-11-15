#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host-test the actual allocation-free Rust completion-link implementation."""
from common import kernel_root, output_dir, rustc, frozen_bindings

import hashlib
import json
import pathlib
import subprocess
import tempfile

SOURCE = kernel_root() / 'drivers/gpu/drm/asahi/g16/job.rs'
OUT = output_dir()
source = SOURCE.read_text()
link = source[source.index('struct CompletionLink {'):source.index('/// A job as the engine sees it.')]
queue = source[source.index('pub(crate) struct Completions {'):source.index('/// The scheduler\'s view of a job.')]
harness = r'''
#![allow(dead_code)]
use std::sync::{Arc,Mutex as StdMutex};
use core::sync::atomic::{AtomicBool,AtomicUsize,Ordering};
use core::cell::Cell;
use core::ops::{Deref,DerefMut};
use std::alloc::{GlobalAlloc,Layout,System};
static FAIL_ALLOCATIONS:AtomicBool=AtomicBool::new(false);
static ALLOCATIONS:AtomicUsize=AtomicUsize::new(0);
static DEVICE_LOCKED:AtomicBool=AtomicBool::new(false);
struct Allocator;
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self,layout:Layout)->*mut u8 {
        if FAIL_ALLOCATIONS.load(Ordering::Relaxed) { core::ptr::null_mut() }
        else { ALLOCATIONS.fetch_add(1,Ordering::Relaxed); System.alloc(layout) }
    }
    unsafe fn dealloc(&self,ptr:*mut u8,layout:Layout) { System.dealloc(ptr,layout) }
    unsafe fn realloc(&self,ptr:*mut u8,layout:Layout,new:usize)->*mut u8 {
        if FAIL_ALLOCATIONS.load(Ordering::Relaxed) { core::ptr::null_mut() }
        else { ALLOCATIONS.fetch_add(1,Ordering::Relaxed); System.realloc(ptr,layout,new) }
    }
}
#[global_allocator]
static ALLOCATOR:Allocator=Allocator;
thread_local! { static HELD:Cell<u32>=const { Cell::new(0) }; }
struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> {
    fn new(value:T)->Self { Self(StdMutex::new(value)) }
    fn lock(&self)->Guard<'_,T> {
        HELD.with(|held| { assert_eq!(held.get(),0,"nested completion lock"); held.set(1); });
        Guard(self.0.lock().unwrap())
    }
}
struct Guard<'a,T>(std::sync::MutexGuard<'a,T>);
impl<T> Deref for Guard<'_,T> { type Target=T; fn deref(&self)->&T { &self.0 } }
impl<T> DerefMut for Guard<'_,T> { fn deref_mut(&mut self)->&mut T { &mut self.0 } }
impl<T> Drop for Guard<'_,T> { fn drop(&mut self) { HELD.with(|held|held.set(0)); } }
type Result<T=()> = core::result::Result<T,i32>;
LINK
struct Job { id:u64,completion:Mutex<CompletionLink>,log:Arc<StdMutex<Vec<(u64,Result)>>> }
impl Job {
    fn new(id:u64,log:Arc<StdMutex<Vec<(u64,Result)>>>)->Arc<Self> {
        Arc::new(Self{id,completion:Mutex::new(CompletionLink::new()),log})
    }
    fn complete(&self,result:Result) {
        assert!(!DEVICE_LOCKED.load(Ordering::Acquire),"fence callback under device lock");
        HELD.with(|held|assert_eq!(held.get(),0,"callback under completion lock"));
        // Reenter the job's own link lock just as a fence callback may reenter driver code.
        drop(self.completion.lock());
        self.log.lock().unwrap().push((self.id,result));
    }
}
QUEUE
#[test]
fn fifo_first_completion_wins_and_no_callbacks_during_push() {
    let log=Arc::new(StdMutex::new(Vec::with_capacity(3)));
    let jobs:Vec<_>=(0..3).map(|id|Job::new(id,log.clone())).collect();
    let mut done=Completions::new(); DEVICE_LOCKED.store(true,Ordering::Release);
    done.push(&jobs[2],Err(-5)); done.push(&jobs[0],Ok(())); done.push(&jobs[1],Err(-19));
    done.push(&jobs[2],Ok(())); assert!(log.lock().unwrap().is_empty());
    DEVICE_LOCKED.store(false,Ordering::Release); complete_all(done);
    assert_eq!(*log.lock().unwrap(),vec![(2,Err(-5)),(0,Ok(())),(1,Err(-19))]);
    for job in jobs { assert_eq!(Arc::strong_count(&job),1); }
}
#[test]
fn one_job_cannot_enter_two_lists_or_be_requeued_after_drain() {
    let log=Arc::new(StdMutex::new(Vec::with_capacity(1))); let job=Job::new(42,log.clone());
    let mut first=Completions::new(); let mut second=Completions::new();
    first.push(&job,Ok(())); second.push(&job,Err(-5));
    assert!(second.pop().is_none()); complete_all(first);
    second.push(&job,Err(-5)); assert!(second.pop().is_none());
    assert_eq!(*log.lock().unwrap(),vec![(42,Ok(()))]);
    assert_eq!(Arc::strong_count(&job),1);
}
#[test]
fn retirement_and_completion_work_with_allocator_forced_to_fail() {
    let log=Arc::new(StdMutex::new(Vec::with_capacity(200)));
    let jobs:Vec<_>=(0..200).map(|id|Job::new(id,log.clone())).collect();
    let mut done=Completions::new(); let before=ALLOCATIONS.load(Ordering::Relaxed);
    FAIL_ALLOCATIONS.store(true,Ordering::SeqCst);
    for job in &jobs { done.push(job,Ok(())); }
    complete_all(done);
    FAIL_ALLOCATIONS.store(false,Ordering::SeqCst);
    let after=ALLOCATIONS.load(Ordering::Relaxed);
    assert_eq!(before,after); assert_eq!(log.lock().unwrap().len(),200);
    for job in jobs { assert_eq!(Arc::strong_count(&job),1); }
}
#[test]
fn concurrent_lists_claim_a_job_exactly_once() {
    let log=Arc::new(StdMutex::new(Vec::with_capacity(1))); let job=Job::new(7,log.clone());
    let barrier=Arc::new(std::sync::Barrier::new(8));
    let threads:Vec<_>=(0..8).map(|_| { let job=job.clone(); let barrier=barrier.clone();
        std::thread::spawn(move|| { let mut done=Completions::new(); barrier.wait();
            done.push(&job,Ok(())); let queued=done.head.is_some(); complete_all(done); queued })
    }).collect();
    let claimed=threads.into_iter().filter_map(|thread|thread.join().ok()).filter(|&queued|queued).count();
    assert_eq!(claimed,1); assert_eq!(log.lock().unwrap().len(),1);
    assert_eq!(Arc::strong_count(&job),1);
}
#[test]
fn empty_drain_and_repeated_fifo_batches_leave_no_chain() {
    complete_all(Completions::new());
    let log=Arc::new(StdMutex::new(Vec::with_capacity(256)));
    let jobs:Vec<_>=(0..256).map(|id|Job::new(id,log.clone())).collect();
    for group in jobs.chunks(16) { let mut done=Completions::new();
        for job in group { done.push(job,Ok(())); }
        complete_all(done);
    }
    let log=log.lock().unwrap();
    assert!(log.iter().enumerate().all(|(index,(id,result))|*id==index as u64 && result.is_ok()));
    assert!(jobs.iter().all(|job|Arc::strong_count(job)==1 && job.completion.lock().next.is_none()));
}
'''.replace('LINK',link).replace('QUEUE',queue)
with tempfile.TemporaryDirectory(prefix='m4-rust-completions-') as directory:
    directory=pathlib.Path(directory); test=directory/'test.rs'; test.write_text(harness)
    binary=directory/'test'
    subprocess.run(rustc() + ['--edition=2021','--test',str(test),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary),'--test-threads=1','--nocapture'],check=True,text=True,capture_output=True)
    print(result.stdout,end='')
    (OUT/'completion-queue-host.log').write_text(result.stdout+result.stderr)
    (OUT/'completion-queue-host.json').write_text(json.dumps({
        'source':str(SOURCE),'sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'result':'pass','test_groups':5,
        'limitations':'Actual queue/link implementation against host mutex/Arc/job callbacks; kernel lockdep and DRM integration are checked by module build and target validation.'
    },indent=2)+'\n')
