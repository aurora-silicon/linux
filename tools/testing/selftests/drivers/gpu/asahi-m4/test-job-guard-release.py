#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Force the final production completion reference to drop during fence signalling."""
from common import kernel_root, output_dir, rustc, sha
import subprocess,json
root=kernel_root();out=output_dir();job=(root/'drivers/gpu/drm/asahi/g16/job.rs').read_text();cleanup=(root/'drivers/gpu/drm/asahi/cleanup.rs').read_text()
link=job[job.index('struct CompletionLink {'):job.index('/// A job as the engine sees it.')]
completion=job[job.index('pub(crate) struct Completions {'):job.index("/// The scheduler's view of a job.")]
deferred=cleanup[cleanup.index("pub(crate) struct Deferred<T:"):]
wrapped='_guard: crate::cleanup::Deferred<' in job
mapping='struct MappingGuard {' in job
mapping_source=job[job.index('struct MappingGuard {'):job.index('/// A completion link allocated')] if mapping else ''
if mapping:
    start=job.index('let guard = MappingGuard {');end=job.index(';',start)
    mapping_value=job[start:end].replace('let guard = ', '', 1)
else:
    mapping_value='guard'
harness=r'''
#![allow(dead_code,non_camel_case_types,private_interfaces)]
use core::{ops::{Deref,DerefMut},pin::Pin,ptr,sync::atomic::{AtomicPtr,AtomicBool,AtomicUsize,Ordering}};
use std::sync::{Arc,Mutex as StdMutex};
type Result<T=()> = std::result::Result<T,()>;const GFP_KERNEL:u32=0;
struct Mutex<T>(StdMutex<T>);impl<T> Mutex<T>{fn new(v:T)->Self{Self(StdMutex::new(v))}fn lock(&self)->std::sync::MutexGuard<'_,T>{self.0.lock().unwrap()}}
static SIGNALLING:AtomicBool=AtomicBool::new(false);
struct KBox<T>(Box<T>);impl<T>KBox<T>{fn pin_init(value:T,_flags:u32)->Result<Pin<Self>>{Ok(unsafe{Pin::new_unchecked(Self(Box::new(value)))})}}
impl<T>Deref for KBox<T>{type Target=T;fn deref(&self)->&T{&self.0}}
impl<T>DerefMut for KBox<T>{fn deref_mut(&mut self)->&mut T{&mut self.0}}
struct Work<T>(core::marker::PhantomData<fn()->T>);
struct Item<T:Send+'static>{value:T,work:Work<Self>}
macro_rules! new_work {($($arg:tt)*)=>{Work(core::marker::PhantomData)}}
macro_rules! pin_init {(Item{$value:ident,work <- $work:expr,})=>{Item{value:$value,work:$work}}}
struct Queue{pending:Mutex<Vec<Box<dyn Send>>>}
impl Queue{
    unsafe fn from_raw<'a>(p:*mut bindings::workqueue_struct)->&'a Self{&*p}
    fn enqueue<T:Send+'static>(&self,item:Pin<KBox<Item<T>>>){
        // Mock transport has no self-referential work fields. Erase ownership into a
        // pre-reserved queue slot; actual Deferred destructor is included verbatim below.
        let boxed=unsafe{Pin::into_inner_unchecked(item)}.0;
        let mut pending=self.pending.lock();assert!(pending.len()<pending.capacity());pending.push(boxed);
    }
    fn drain(&self){let values=core::mem::take(&mut*self.pending.lock());drop(values);}
}
mod bindings{pub type workqueue_struct=super::Queue;}
static QUEUE:AtomicPtr<bindings::workqueue_struct>=AtomicPtr::new(ptr::null_mut());
__DEFERRED__
__LINK__
struct VmState{alive:Arc<AtomicBool>}
// Adversarial scheduling: model separately deferred VmState cleanup running as
// soon as its last Arc disappears, before the guard's worker gets CPU time.
impl Drop for VmState{fn drop(&mut self){self.alive.store(false,Ordering::Release);}}
struct Guard{dropped:Arc<AtomicUsize>,lease_alive:Arc<AtomicBool>}
impl Drop for Guard{fn drop(&mut self){assert!(!SIGNALLING.load(Ordering::Acquire),"VmJobGuard destruction entered mapping drain while signalling fence");assert!(self.lease_alive.load(Ordering::Acquire),"ASID binding lease disappeared before mapping drain");self.dropped.fetch_add(1,Ordering::AcqRel);}}
mod mmu{pub(super) type VmJobGuard=super::Guard;}
__MAPPING_SOURCE__
type JobGuard=__GUARD_TYPE__;
struct Job{completion:Mutex<CompletionLink>,vm:Arc<VmState>,_guard:JobGuard,scheduler_owner:Mutex<Option<Arc<Job>>>}
impl Job{
    fn new(dropped:Arc<AtomicUsize>,lease_alive:Arc<AtomicBool>)->Arc<Self>{
        let vm=Arc::new(VmState{alive:lease_alive.clone()});
        let guard=Guard{dropped,lease_alive};let guard=__MAPPING_VALUE__;
        let job=Arc::new(Self{completion:Mutex::new(CompletionLink::new()),vm,_guard:__GUARD_VALUE__,scheduler_owner:Mutex::new(None)});
        *job.scheduler_owner.lock()=Some(job.clone());job
    }
    fn complete(&self,_result:Result){
        // Force the scheduler-free race before complete_all drops its final Arc.
        // This models the callback's legal lifetime ordering, not the DRM internals.
        drop(self.scheduler_owner.lock().take());
    }
}
__COMPLETIONS__
#[test]fn final_job_guard_release_is_queued_until_after_signalling(){
    let queue=Box::leak(Box::new(Queue{pending:Mutex::new(Vec::with_capacity(16))}));QUEUE.store(queue,Ordering::Release);
    let dropped=Arc::new(AtomicUsize::new(0));let lease_alive=Arc::new(AtomicBool::new(true));let job=Job::new(dropped.clone(),lease_alive.clone());
    let mut completed=Completions::new();completed.push(&job,Ok(()));drop(job);
    SIGNALLING.store(true,Ordering::Release);complete_all(completed);
    assert_eq!(dropped.load(Ordering::Acquire),0,"mapping destruction must remain deferred");
    assert!(lease_alive.load(Ordering::Acquire),"last outer Job reference released the ASID lease before deferred mapping drain");
    SIGNALLING.store(false,Ordering::Release);queue.drain();assert_eq!(dropped.load(Ordering::Acquire),1);
    assert!(!lease_alive.load(Ordering::Acquire),"binding lease releases after mapping drain");
}
'''.replace('__DEFERRED__',deferred).replace('__LINK__',link).replace('__COMPLETIONS__',completion).replace('__MAPPING_SOURCE__',mapping_source).replace('__MAPPING_VALUE__',mapping_value).replace('__GUARD_TYPE__', 'Deferred<MappingGuard>' if mapping else ('Deferred<Guard>' if wrapped else 'Guard')).replace('__GUARD_VALUE__','Deferred::new(guard).unwrap()' if wrapped else 'guard')
unit=out/'job-guard-release.rs';unit.write_text(harness);log=''
for command in [rustc()+['--edition=2021','--test',str(unit),'-o',str(out/'job-guard-release')],[str(out/'job-guard-release'),'--nocapture','--test-threads=1']]:
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);log+=result.stdout;print(result.stdout,end='');(out/'job-guard-release.log').write_text(log)
    if result.returncode:raise SystemExit(result.returncode)
(out/'job-guard-release.json').write_text(json.dumps({'passed':True,'cpu_only':True,'tests':1,'scope':'Copied production Completions and cleanup::Deferred Drop/Deref/new with Job guard field selected from copied production source; kernel enqueue and scheduler release mocked. Forces final Arc release under signalling scope; no real mapping or lockdep execution.','source_sha256':{'job.rs':sha(root/'drivers/gpu/drm/asahi/g16/job.rs'),'cleanup.rs':sha(root/'drivers/gpu/drm/asahi/cleanup.rs')}},indent=2)+'\n')
