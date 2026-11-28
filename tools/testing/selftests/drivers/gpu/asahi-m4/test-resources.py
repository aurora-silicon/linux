#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the actual M4 resource allocators against a CPU-only allocation recorder."""
from common import kernel_root, output_dir, rustc
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
root = kernel_root()
out = args.output or output_dir()
out.mkdir(parents=True, exist_ok=True)
def item(text, declaration):
    start = text.index(declaration)
    at = text.index('{', start)
    depth = 1
    end = at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def module(text, name):
    return item(text, 'pub(crate) mod ' + name + ' {')

hw = (root/'drivers/gpu/drm/asahi/g16/hw.rs').read_text()
production = root/'drivers/gpu/drm/asahi/g16'
graph_source = (production/'graph.rs').read_text()
# Qualified J713 noncached read/write leaf attributes, pinned independently
# of the production allocator and page-table implementation.
private_rw = 0x00c0000000000c8b
source = r'''
#![allow(dead_code, unused_imports, private_interfaces)]
extern crate self as kernel;
#[derive(Copy,Clone,Debug,Eq,PartialEq)] pub struct Error(i32);
pub const EINVAL:Error=Error(22); pub const EAGAIN:Error=Error(11); pub const E2BIG:Error=Error(7);
pub const EBUSY:Error=Error(16); pub const EOVERFLOW:Error=Error(75); pub const EIO:Error=Error(5);
pub const ENOMEM:Error=Error(12);
pub const ETIMEDOUT:Error=Error(110);pub const ERESTARTSYS:Error=Error(512);
pub const GFP_KERNEL:u32=0;
pub type Result<T=()> = std::result::Result<T,Error>;
macro_rules! init { ($($tt:tt)*) => { $($tt)* }; }
pub mod prelude {pub use crate::{Result,KVec,KBox,GFP_KERNEL,EINVAL,EAGAIN,E2BIG,EBUSY,EOVERFLOW,EIO,ENOMEM,ETIMEDOUT,ERESTARTSYS};}
std::thread_local!{pub static FAIL_KBOX_UNINIT:std::cell::Cell<bool>=const{std::cell::Cell::new(false)};}
pub struct KBox<T>(Box<T>);
impl<T>KBox<T>{
    pub fn new(x:T,_:u32)->Result<Self>{Ok(Self(Box::new(x)))}
    pub fn init(x:T,_:u32)->Result<Self>{Ok(Self(Box::new(x)))}
    pub fn new_uninit(_:u32)->Result<KBox<core::mem::MaybeUninit<T>>>{
        if FAIL_KBOX_UNINIT.with(|f|f.replace(false)){return Err(ENOMEM);}
        Ok(KBox(Box::new(core::mem::MaybeUninit::uninit())))
    }
    pub fn write(mut b:KBox<core::mem::MaybeUninit<T>>,x:T)->Self{
        b.0.write(x);
        // SAFETY: `write` just initialized `b.0`.
        Self(unsafe{Box::from_raw(Box::into_raw(b.0).cast::<T>())})
    }
}
impl<T>std::ops::Deref for KBox<T>{type Target=T;fn deref(&self)->&T{&self.0}}
impl<T>std::ops::DerefMut for KBox<T>{fn deref_mut(&mut self)->&mut T{&mut self.0}}
pub struct KVec<T>(Vec<T>);
impl<T>KVec<T>{pub fn new()->Self{Self(Vec::new())} pub fn with_capacity(n:usize,_:u32)->Result<Self>{Ok(Self(Vec::with_capacity(n)))} pub fn push(&mut self,x:T,_:u32)->Result{self.0.push(x);Ok(())}}
impl<T>std::ops::Deref for KVec<T>{type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T>std::ops::DerefMut for KVec<T>{fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
pub mod time {pub fn msecs_to_jiffies(n:u32)->u64{n as u64}}
pub mod sync {
    pub struct Mutex<T>(std::sync::Mutex<T>);
    pub struct Guard<'a,T>(Option<std::sync::MutexGuard<'a,T>>);
    impl<T>Mutex<T>{pub fn new(v:T)->Self{Self(std::sync::Mutex::new(v))}pub fn lock(&self)->Guard<'_,T>{Guard(Some(self.0.lock().unwrap()))}}
    impl<T>std::ops::Deref for Guard<'_,T>{type Target=T;fn deref(&self)->&T{self.0.as_ref().unwrap()}}
    impl<T>std::ops::DerefMut for Guard<'_,T>{fn deref_mut(&mut self)->&mut T{self.0.as_mut().unwrap()}}
    std::thread_local!{pub static FORCED_WAIT:std::cell::Cell<u8>=const{std::cell::Cell::new(0)};}
    pub enum CondVarTimeoutResult{Woken{jiffies:u64},Timeout,Signal{jiffies:u64}}
    pub struct CondVar(std::sync::Condvar);
    impl CondVar{pub fn new()->Self{Self(std::sync::Condvar::new())}pub fn notify_all(&self){self.0.notify_all()}
      pub fn wait_interruptible_timeout<T>(&self,g:&mut Guard<'_,T>,ms:u64)->CondVarTimeoutResult{
        match FORCED_WAIT.with(|v|v.replace(0)){1=>return CondVarTimeoutResult::Timeout,2=>return CondVarTimeoutResult::Signal{jiffies:ms},_=>{}}
        let start=std::time::Instant::now();let(v,t)=self.0.wait_timeout(g.0.take().unwrap(),std::time::Duration::from_millis(ms)).unwrap();g.0=Some(v);
        if t.timed_out(){CondVarTimeoutResult::Timeout}else{CondVarTimeoutResult::Woken{jiffies:ms.saturating_sub(start.elapsed().as_millis() as u64)}}
      }
    }

    pub struct Arc<T>{inner:std::sync::Arc<T>}
    impl<T>Arc<T>{pub fn new(v:T,_:u32)->crate::Result<Self>{Ok(Self{inner:std::sync::Arc::new(v)})}pub fn strong_count(v:&Self)->usize{std::sync::Arc::strong_count(&v.inner)}pub fn ptr_eq(a:&Self,b:&Self)->bool{std::sync::Arc::ptr_eq(&a.inner,&b.inner)}}
    impl<T>Clone for Arc<T>{fn clone(&self)->Self{Self{inner:self.inner.clone()}}}
    impl<T>std::ops::Deref for Arc<T>{type Target=T;fn deref(&self)->&T{&self.inner}}
    pub mod aref {pub struct ARef<T>(T);impl From<&crate::device::Device>for ARef<crate::device::Device>{fn from(v:&crate::device::Device)->Self{Self(v.clone())}}}
}
pub mod mmu {
    pub struct VmBind{pub live:std::sync::Arc<std::sync::atomic::AtomicU32>,pub released:std::sync::Arc<std::sync::atomic::AtomicBool>}
    impl Drop for VmBind{fn drop(&mut self){use std::sync::atomic::Ordering;assert_eq!(self.live.load(Ordering::Acquire),1);self.released.store(true,Ordering::Release);}}
}
pub mod cleanup {pub struct Deferred<T>(T);impl<T>Deferred<T>{pub fn new(v:T)->crate::Result<Self>{Ok(Self(v))}}impl<T>std::ops::Deref for Deferred<T>{type Target=T;fn deref(&self)->&T{&self.0}}}
pub mod device {#[derive(Clone)]pub struct Device;impl AsRef<Device> for Device {fn as_ref(&self)->&Device{self}}}
pub mod driver {pub struct AsahiDrmDevice(pub crate::device::Device);impl AsRef<crate::device::Device>for AsahiDrmDevice{fn as_ref(&self)->&crate::device::Device{&self.0}}}
pub mod g16 {
    pub mod hw {__HW__}
    pub mod fw {
        #[path="__ROOT__/fw/buffer.rs"]pub mod buffer;
        #[path="__ROOT__/fw/graph.rs"]pub mod graph;
        #[path="__ROOT__/fw/work.rs"]pub mod work;
    }
    pub mod object {
        use crate::*;use crate::sync::Arc;
        #[derive(Copy,Clone,Debug,Eq,PartialEq)]pub enum Class{Shared,Fw,FwCached,SharedCached,FwCoherent,Mmio}
        pub enum Placement{Anywhere}
        pub struct Request{pub size:usize,pub class:Class,pub placement:Placement,pub offset:usize,pub client:Option<(u64,u64)>,pub lower:Option<u64>,pub cpu:bool}
        pub struct FwObject{pub class:Class,pub client_leaf:Option<u64>,pub va:u64,pub gva:Option<u64>,pub lower_gva:Option<u64>,pub lower_leaf:Option<u64>,pub size:usize,pub mapped:usize,pub offset:usize,pub data:std::sync::Arc<std::sync::Mutex<Vec<u8>>>,pub fills:std::sync::Mutex<Vec<(usize,usize)>>,pub fail_fill:std::sync::atomic::AtomicBool,pub write_bytes:std::sync::atomic::AtomicUsize}
        impl FwObject{
            pub fn view(o:&Arc<Self>,offset:usize,size:usize)->Result<Arc<Self>>{Arc::new(Self{class:o.class,client_leaf:None,va:o.va+offset as u64,gva:None,lower_gva:None,lower_leaf:None,size,mapped:o.mapped,offset:o.offset+offset,data:o.data.clone(),fills:std::sync::Mutex::new(Vec::new()),fail_fill:std::sync::atomic::AtomicBool::new(false),write_bytes:std::sync::atomic::AtomicUsize::new(0)},0)}
            pub fn write(&self,o:usize,b:&[u8])->Result{self.write_bytes.fetch_add(b.len(),std::sync::atomic::Ordering::Relaxed);let mut d=self.data.lock().unwrap();d[self.offset+o..self.offset+o+b.len()].copy_from_slice(b);Ok(())}
            pub fn fill(&self,o:usize,v:u8,n:usize)->Result{if self.fail_fill.swap(false,std::sync::atomic::Ordering::Relaxed){return Err(EIO);}self.fills.lock().unwrap().push((o,n));let mut d=self.data.lock().unwrap();d[self.offset+o..self.offset+o+n].fill(v);Ok(())}
            pub fn w32(&self,o:usize,v:u32)->Result{self.write(o,&v.to_le_bytes())}
            pub fn w64(&self,o:usize,v:u64)->Result{self.write(o,&v.to_le_bytes())}
            pub fn r32(&self,o:usize)->Result<u32>{let d=self.data.lock().unwrap();Ok(u32::from_le_bytes(d[self.offset+o..self.offset+o+4].try_into().unwrap()))}
            pub fn snapshot(&self)->Vec<u8>{self.data.lock().unwrap().clone()}
        }
        pub struct FwLease(pub crate::cleanup::Deferred<Objects>);
        impl FwLease {
            pub fn new(mut objects: Objects) -> Result<Arc<Self>> {
                objects.finish_private()?;
                Arc::new(Self(crate::cleanup::Deferred::new(objects)?), 0)
            }
        }
        pub struct Factory;
        impl Factory{
            pub fn objects(&self,_:&super::vm::MockVm)->Objects{Objects::new()}
            pub fn fw_objects(&self)->Objects{Objects::new()}
            pub fn persistent_objects(&self)->Objects{Objects::persistent()}
            pub fn persistent_objects_probe(&self)->Objects{Objects::persistent()}
        }
        pub static NEXT_VA: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0xfffffc2001000000);
        std::thread_local!{pub static ALL_OBJECTS:std::cell::RefCell<Vec<Arc<FwObject>>>=const{std::cell::RefCell::new(Vec::new())};}
        pub struct Window {
            pub fail_once: std::sync::atomic::AtomicBool,
            pub calls: std::sync::atomic::AtomicUsize,
            pub ranges: std::sync::Mutex<Vec<core::ops::Range<u64>>>,
        }
        impl Window {
            pub fn new() -> Self {
                Self {
                    fail_once: std::sync::atomic::AtomicBool::new(false),
                    calls: std::sync::atomic::AtomicUsize::new(0),
                    ranges: std::sync::Mutex::new(Vec::new()),
                }
            }
            pub fn sync_range(&self, _uat: &(), range: core::ops::Range<u64>) -> Result {
                self.calls.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                self.ranges.lock().unwrap().push(range);
                if self.fail_once.swap(false, std::sync::atomic::Ordering::AcqRel) {
                    return Err(EIO);
                }
                Ok(())
            }
        }
        pub struct Private {
            pub pending: Option<core::ops::Range<u64>>,
        }
        pub struct Objects{pub objects:Vec<Arc<FwObject>>,pub persistent:bool,pub uat:(),pub window:Arc<crate::sync::Mutex<Window>>,pub private:Option<Private>}
        // Device-lifetime render and compute command records, per test thread (the production
        // tables are resources::RenderCmds and resources::ComputeCmds): one never-dropped
        // allocator owns them.
        std::thread_local!{
            pub static PERSISTENT:std::cell::RefCell<Option<Objects>>=const{std::cell::RefCell::new(None)};
            pub static PASS_CMDS:std::cell::RefCell<Vec<(u64,crate::g16::slots::PassCmds)>>=const{std::cell::RefCell::new(Vec::new())};
            pub static UNIT_CMDS:std::cell::RefCell<Vec<(u64,crate::g16::slots::UnitCmds)>>=const{std::cell::RefCell::new(Vec::new())};
        }
        pub fn test_cmds(k:u64)->crate::g16::slots::PassCmds{
            if let Some(c)=PASS_CMDS.with(|p|p.borrow().iter().find(|e|e.0==k).map(|e|e.1.clone())){return c;}
            let c=PERSISTENT.with(|p|{let mut p=p.borrow_mut();crate::g16::slots::PassCmds::new(p.get_or_insert_with(Objects::persistent),k).unwrap()});
            PASS_CMDS.with(|p|p.borrow_mut().push((k,c.clone())));c
        }
        pub fn test_unit_cmds(k:u64)->crate::g16::slots::UnitCmds{
            if let Some(c)=UNIT_CMDS.with(|p|p.borrow().iter().find(|e|e.0==k).map(|e|e.1.clone())){return c;}
            let c=PERSISTENT.with(|p|{let mut p=p.borrow_mut();crate::g16::slots::UnitCmds::new(p.get_or_insert_with(Objects::persistent),k).unwrap()});
            UNIT_CMDS.with(|p|p.borrow_mut().push((k,c.clone())));c
        }
        impl Objects{
            pub fn new()->Self{Self::with_window(false,Arc::new(crate::sync::Mutex::new(Window::new()),0).unwrap())}
            pub fn persistent()->Self{Self::with_window(true,Arc::new(crate::sync::Mutex::new(Window::new()),0).unwrap())}
            pub fn with_window(persistent:bool,window:Arc<crate::sync::Mutex<Window>>)->Self{Self{objects:vec![],persistent,uat:(),window,private:Some(Private{pending:None})}}
            __FINISH_PRIVATE__
            pub fn alloc_dynamic_client(&mut self,size:usize)->Result<Arc<FwObject>>{self.alloc(Request{size,class:Class::Shared,placement:Placement::Anywhere,offset:0,client:Some((0x7040000000,super::hw::pte::LEAF_NC)),lower:None,cpu:true})}
            pub fn alloc(&mut self,r:Request)->Result<Arc<FwObject>>{if r.class==Class::FwCoherent&&(!self.persistent||r.client.is_some()){return Err(EINVAL);}let bytes=(r.size+r.offset+0x3fff)&!0x3fff;let va=NEXT_VA.fetch_add(bytes as u64+0x4000,std::sync::atomic::Ordering::Relaxed);let x=Arc::new(FwObject{class:r.class,client_leaf:r.client.map(|c|c.1),va:va+r.offset as u64,gva:r.client.map(|c|c.0+r.offset as u64),lower_gva:r.lower.map(|g|g+r.offset as u64),lower_leaf:r.lower.map(|_|super::hw::pte::LOWER),size:r.size,mapped:bytes,offset:r.offset,data:std::sync::Arc::new(std::sync::Mutex::new(if r.cpu{vec![0;bytes]}else{vec![]})),fills:std::sync::Mutex::new(Vec::new()),fail_fill:std::sync::atomic::AtomicBool::new(false),write_bytes:std::sync::atomic::AtomicUsize::new(0)},0)?;if let Some(p)=self.private.as_mut(){let end=va+bytes as u64;p.pending=Some(match p.pending.take(){Some(range)=>range.start.min(va)..range.end.max(end),None=>va..end});}self.objects.push(x.clone());ALL_OBJECTS.with(|a|a.borrow_mut().push(x.clone()));Ok(x)}
        }
    }
    pub mod graph {
        use crate::*;use crate::sync::Arc;use super::object::*;
        use super::fw::graph::{self, job_list, notifier_page, qinfo, qstate, sched_ctx};
        use super::hw::{pte, sizes, tuning};
        #[derive(Copy, Clone)]
        pub(crate) struct FwAbi;
        impl FwAbi {
            pub(crate) fn qinfo(self) -> &'static graph::qinfo::Layout {
                &graph::qinfo::V25G83
            }
        }
        __CLIENT__
        __CMD_CLASSES__
        __ALLOC__
        __ALLOC_WORK__
        __GRAPH_TYPES__
    }
    pub mod encode {
        pub struct Geometry{pub tpc_bytes:u64,pub tilemap_bytes:u64}
        pub struct PassRefs{pub ta_work:u64,pub ta_work_gva:u64,pub fs_work:u64,pub fs_work_gva:u64,pub ta_seq:u64,pub fs_seq:u64,pub scene:u64,pub tpc_gva:u64,pub tilemap_gva:u64,pub heap_gva:u64,pub scene_user_gva:u64,pub scene_list:u64,pub pre_gva:[u64;3],pub fs_scratch_gva:u64,pub aux_gva:u64,pub its:u64,pub ta_scratch:u64,pub tpc_fw:u64,pub scratch_fw:u64}
    }
    pub mod vm {
        use crate::{mmu,sync::Arc};
        use core::sync::atomic::{AtomicBool,Ordering};
        use super::resources::reservations::Cache;
        pub struct MockVm {
            closed: AtomicBool,
            unpublished: AtomicBool,
            cache: Option<Arc<Cache>>,
            _live: Option<Arc<LiveToken>>,
        }
        impl MockVm{
            pub fn new() -> Self {
                Self {
                    closed: AtomicBool::new(false),
                    unpublished: AtomicBool::new(false),
                    cache: None,
                    _live: None,
                }
            }
            pub fn tracked(cache: Arc<Cache>) -> (Self, std::sync::Arc<AtomicBool>) {
                let live = std::sync::Arc::new(std::sync::atomic::AtomicU32::new(1));
                let released = std::sync::Arc::new(AtomicBool::new(false));
                let registry = Arc::new(Registry { live: live.clone() }, 0).unwrap();
                let bind = mmu::VmBind { live, released: released.clone() };
                let token = Arc::new(LiveToken { registry, bind: Some(bind), file_id: 0, process: 0 }, 0).unwrap();
                (Self { cache: Some(cache), _live: Some(token), ..Self::new() }, released)
            }
            pub fn is_closed(&self)->bool{self.closed.load(Ordering::Acquire)}
            pub fn close(&self){self.closed.store(true,Ordering::Release);}
            // Accepted renders still unpublished (the VM cache's standby reservations).
            pub fn set_unpublished(&self,v:bool){self.unpublished.store(v,Ordering::Release);}
        }
        pub struct VmState{pub id:u64,pub slot:u32,pub generation:u8,pub vm:MockVm}
        impl VmState {
            pub fn renders_unpublished(&self) -> bool {
                self.vm.unpublished.load(Ordering::Acquire)
                    || self.vm.cache.as_ref().is_some_and(|cache| cache.renders_unpublished())
            }
        }
        struct Registry{live:std::sync::Arc<std::sync::atomic::AtomicU32>}impl Registry{fn release(&self,_file:u64,_process:usize){self.live.fetch_sub(1,Ordering::AcqRel);}}
        __LIVE_TOKEN__
        __LIVE_DROP__
        #[test]fn accounting_outlives_deferred_arenas_and_releases_binding_first(){
            use std::sync::atomic::{AtomicU32,AtomicBool};
            let count=std::sync::Arc::new(AtomicU32::new(1));let released=std::sync::Arc::new(AtomicBool::new(false));
            let registry=Arc::new(Registry{live:count.clone()},0).unwrap();
            let token=Arc::new(LiveToken{registry,file_id:0,process:0,bind:Some(mmu::VmBind{live:count.clone(),released:released.clone()})},0).unwrap();
            let deferred_arena=token.clone();drop(token);
            assert_eq!(count.load(Ordering::Acquire),1);assert!(!released.load(Ordering::Acquire));
            drop(deferred_arena);assert!(released.load(Ordering::Acquire));assert_eq!(count.load(Ordering::Acquire),0);
        }
    }
    pub mod resources {
        use core::sync::atomic::{AtomicU32,AtomicU64,Ordering};use crate::*;
        pub struct PassDirty<'a>{pub tpc:&'a AtomicU64,pub tilemap:&'a AtomicU64,pub tilemap_last:&'a AtomicU64,pub tilemap_clean:&'a AtomicU64,pub heap:&'a AtomicU64}
        pub struct ComputeArena{pub compute:KVec<super::slots::ComputeSlot>,pub render:Option<crate::sync::Arc<RenderArena>>,pub hw_slots:Option<[usize;super::hw::sizes::SCS_COMPUTE_UNITS]>}
        // A job's compute units; here one arena.
        pub type Prepared=ComputeArena;
        #[derive(Clone)]
        pub enum PassOwnerArena{Base(crate::sync::Arc<RenderArena>)}
        pub struct PoolExtArena;
        #[derive(Clone)]
        pub enum PbOwnerArena{Base(crate::sync::Arc<RenderArena>),Ext(crate::sync::Arc<RenderArena>,crate::sync::Arc<PoolExtArena>)}
        impl PassOwnerArena{
            pub fn pass_and_dirty(&self,k:usize)->Option<(&super::slots::PassSlot,PassDirty<'_>)>{
                match self{Self::Base(a)=>Some((a.passes.get(k)?,PassDirty{tpc:a.tpc_dirty.get(k)?,tilemap:a.tilemap_dirty.get(k)?,tilemap_last:a.tilemap_last.get(k)?,tilemap_clean:a.tilemap_clean.get(k)?,heap:a.heap_dirty.get(k)?}))}
            }
        }
        std::thread_local!{
            pub static COMPUTE_OWNER_ARENA:std::cell::RefCell<
                Option<crate::sync::Arc<ComputeArena>>,
            >=const{std::cell::RefCell::new(None)};
            pub static COMPUTE_OWNER_ARENAS:std::cell::RefCell<
                Option<Vec<crate::sync::Arc<ComputeArena>>>,
            >=const{std::cell::RefCell::new(None)};
        }
        impl ComputeArena{
            pub fn compute_units(&self)->Result<usize>{Ok(self.compute.len())}
            pub fn compute_slot_mask(&self)->Result<u32>{
                if let Some(slots)=self.hw_slots {
                    let mut mask=0u32;
                    for slot in slots { mask |= 1u32 << slot; }
                    return Ok(mask);
                }
                let n=self.compute.len().min(super::hw::sizes::COMPUTE_UNITS);
                Ok(if n>=32 { u32::MAX } else { (1u32 << n) - 1 })
            }
            pub fn compute_unit(&self,k:usize)->Result<&super::slots::ComputeSlot>{
                if let Some(slots)=self.hw_slots {
                    let local=slots.iter().position(|&s|s==k).ok_or(EINVAL)?;
                    return self.compute.get(local).ok_or(EINVAL);
                }
                self.compute.get(k).ok_or(EINVAL)
            }
            pub fn compute_arena(&self,k:usize)->Option<crate::sync::Arc<ComputeArena>>{
                let idx=if let Some(slots)=self.hw_slots {
                    slots.iter().position(|&s|s==k)?
                } else { k };
                COMPUTE_OWNER_ARENAS
                    .with(|a|a.borrow().as_ref().and_then(|v|v.get(idx).cloned()))
                    .or_else(||COMPUTE_OWNER_ARENA.with(|a|a.borrow().clone()))
            }
            pub fn render_arena(&self)->Option<crate::sync::Arc<RenderArena>>{self.render.clone()}
            pub fn render_passes(&self)->Result<usize>{self.render.as_deref().map(|r|r.passes.len()).ok_or(EINVAL)}
            pub fn render_pass(&self,k:usize)->Result<&super::slots::PassSlot>{self.render.as_deref().and_then(|r|r.passes.get(k)).ok_or(EINVAL)}
            pub fn render_pass_dirty(&self,k:usize)->Result<PassDirty<'_>>{let r=self.render.as_deref().ok_or(EINVAL)?;if k>=r.passes.len(){return Err(EINVAL);}Ok(PassDirty{tpc:&r.tpc_dirty[k],tilemap:&r.tilemap_dirty[k],tilemap_last:&r.tilemap_last[k],tilemap_clean:&r.tilemap_clean[k],heap:&r.heap_dirty[k]})}
            pub fn set_pass_dirty(&self,k:usize,tpc:u64,tilemap:u64,heap:u64)->Result{let p=self.render_pass(k)?;if tpc>p.tpc.size as u64||tilemap>p.tilemap.size as u64||heap>p.heap.size as u64{return Err(E2BIG);}let d=self.render_pass_dirty(k)?;d.tpc.fetch_max(tpc,Ordering::Relaxed);d.tilemap.fetch_max(tilemap,Ordering::Relaxed);d.tilemap_last.store(tilemap,Ordering::Relaxed);d.tilemap_clean.store(0,Ordering::Relaxed);d.heap.fetch_max(heap,Ordering::Relaxed);Ok(())}
            pub fn pass_owner_arena(&self,_k:usize)->Option<PassOwnerArena>{self.render.as_ref().map(|r|PassOwnerArena::Base(r.clone()))}
        }
        pub struct RenderArena{pub passes:KVec<super::slots::PassSlot>,pub tpc_dirty:[AtomicU64;super::hw::sizes::RENDER_SLOTS],pub tilemap_dirty:[AtomicU64;super::hw::sizes::RENDER_SLOTS],pub tilemap_last:[AtomicU64;super::hw::sizes::RENDER_SLOTS],pub tilemap_clean:[AtomicU64;super::hw::sizes::RENDER_SLOTS],pub heap_dirty:[AtomicU64;super::hw::sizes::RENDER_SLOTS]}
        impl RenderArena{ __SET_DIRTY__ }
        pub(crate) const BASE_COMPUTE_UNITS: usize = 2;
        pub(crate) const BASE_RENDER_SLOTS: usize = super::hw::sizes::BASE_RENDER_SLOTS;
        const BASE_PB_SLOTS: usize = super::hw::sizes::BASE_PB_SLOTS;
        __RENDER_BURST__

        pub(crate) mod reservations {
            use super::{ComputeArena as ComputeUnitSet, ComputeArena as ComputeUnitTable, PoolExtArena, RenderArena};
            use crate::{
                device::Device,
                driver::AsahiDrmDevice,
                g16::{budget::Budget, object::Factory, pools::{PbEpoch, Pools}, slots::{ComputeWaiters, Slots}, vm::{MockVm, VmState}},
                sync::{Arc, CondVar, Mutex},
                KVec,
            };
            use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

            #[derive(Default)]
            pub(crate) struct Shared {
                closed_vm_reclaim: AtomicBool,
                publish_wake: AtomicBool,
                started: AtomicBool,
                sleeping: AtomicBool,
                poll_queued: AtomicBool,
            }
            impl Shared {
                __WAKE_PUBLISH__
                __WAKE_CLOSED_VM_RECLAIM__

                fn poll_now(&self) {
                    self.poll_queued.store(true, Ordering::Release);
                }
            }

            __STANDBY__
            __SPARE_EPOCHS__
            struct RenderExtArena;
            struct PbHeapExtArena;
            const EXT_RENDER_TIERS: [(); 0] = [];
            const PB_HEAP_EXT_TIERS: [(); 0] = [];
            const EXT_PB_SLOTS: usize = 1;

            pub(crate) struct Cache {
                standby: Mutex<Standby>,
                standby_available: CondVar,
                closed: AtomicBool,
                active_render_passes: AtomicU32,
                shared: Arc<Shared>,
                active_compute_cmds: AtomicU32,
            }
            impl Cache {
                __RELEASE_RESERVED__
                __RELEASE_COMPUTE_CMDS__
                __RELEASE_RENDER_PASSES__
                __RENDERS_UNPUBLISHED__
            }

            __PREPARED__
            __DROP_PREPARED__
            impl Prepared {
                __SET_COMPUTE_WAITER__
                __RELEASE_COMPUTE_WAITER__
                __RELEASE_ACTIVE_COMPUTES__
                __COMMIT_RENDER__
                __DISCARD_UNPUBLISHED__
            }

            struct Engine {
                shared: Option<Arc<Shared>>,
            }
            struct Firmware {
                engine: Engine,
                pools: Pools,
                slots: Slots,
            }
            impl Firmware {
                __RECLAIM_CLOSED_VM_SLOTS__

                fn idle_poll(&mut self) {
                    let shared = self.engine.shared.as_ref().unwrap();
                    assert!(shared.poll_queued.swap(false, Ordering::AcqRel), "poll must be queued");
                    self.reclaim_closed_vm_slots();
                }
            }

            fn displaced_vm(credits: u32) -> (
                Firmware, Arc<Cache>, Arc<VmState>, Arc<PbEpoch>, std::sync::Arc<AtomicBool>, Prepared,
            ) {
                let shared = Arc::new(Shared::default(), 0).unwrap();
                shared.started.store(true, Ordering::Release);
                let cache = Arc::new(Cache {
                    standby: Mutex::new(Standby { epochs: KVec::new(), reserved: credits, waiters: 0 }),
                    standby_available: CondVar::new(),
                    closed: AtomicBool::new(false),
                    active_render_passes: AtomicU32::new(credits),
                    shared: shared.clone(),
                    active_compute_cmds: AtomicU32::new(0),
                }, 0).unwrap();
                let (vm, released) = MockVm::tracked(cache.clone());
                let vm = Arc::new(VmState { id: 1, slot: 1, generation: 1, vm }, 0).unwrap();
                let replacement = crate::resources::vm(2);
                let mut pools = Pools::new(&AsahiDrmDevice(Device));
                let budget = Budget::new().unwrap();
                for _ in 0..credits {
                    let credit = Budget::reserve(&budget, 1).unwrap().take().unwrap();
                    cache.standby.lock().epochs.push(PbEpoch::new(&Factory, &vm, credit).unwrap(), 0).unwrap();
                }
                let epoch = exposed_epoch(&budget, &vm, 0);
                let newer = exposed_epoch(&budget, &replacement, 0);
                for (owner, backing) in [(&vm, &epoch), (&replacement, &newer)] {
                    let binding = pools.bind(0, false);
                    pools.hold_alloc(0, owner, None, None).unwrap();
                    pools.publish_flight(0);
                    pools.retire_flight(0, backing, binding, true).unwrap();
                }
                let firmware = Firmware {
                    engine: Engine { shared: Some(shared) },
                    pools,
                    slots: Slots::new().unwrap(),
                };
                let prepared = Prepared {
                    active_computes: AtomicU32::new(0),
                    active_renders: AtomicU32::new(credits),
                    unpublished_renders: AtomicU32::new(credits),
                    compute_waiter_mask: AtomicU32::new(0),
                    compute_waiters: None,
                    cache: Some(cache.clone()),
                    compute: None,
                    compute_scs: None,
                    render: None,
                    render_scs: false,
                    render_ext: [],
                    pb_heap_ext: [],
                    pb_ext: [Some(Arc::new(PoolExtArena, 0).unwrap())],
                };
                (firmware, cache, vm, epoch, released, prepared)
            }

            fn exposed_epoch(budget: &Arc<Budget>, vm: &Arc<VmState>, slot: usize) -> Arc<PbEpoch> {
                let credit = Budget::reserve(budget, 1).unwrap().take().unwrap();
                let e = PbEpoch::new(&Factory, vm, credit).unwrap();
                e.prepare(slot, crate::g16::hw::client::pb_heap(0), 512).unwrap();
                e.expose();
                e
            }

            #[test]
            fn release_active_computes_is_idempotent_before_drop() {
                let (_fw, cache, _vm, _epoch, _released, mut job) = displaced_vm(0);
                cache.active_compute_cmds.store(3, Ordering::Release);
                job.active_computes.store(3, Ordering::Release);
                job.release_active_computes();
                assert_eq!(cache.active_compute_cmds.load(Ordering::Acquire), 0);
                // Second call and subsequent Drop must not underflow active_compute_cmds.
                job.release_active_computes();
                drop(job);
                assert_eq!(cache.active_compute_cmds.load(Ordering::Acquire), 0);
            }

            #[test]
            fn release_compute_waiter_is_idempotent_before_drop() {
                let (fw, _cache, _vm, _epoch, _released, mut job) = displaced_vm(0);
                let mask = (1u32 << 16) | (1u32 << 17);
                let waiters = fw.slots.admit_compute_mask(mask).unwrap().unwrap();
                job.set_compute_waiter(mask, waiters);
                job.release_compute_waiter();
                // Second call and subsequent Drop must not double-release or corrupt waiter state.
                job.release_compute_waiter();
                drop(job);
                assert_eq!(fw.slots.compute_stranding_mask_for_pipe(2, u32::MAX), 0);
            }

            #[test]
            fn cancel_last_unpublished_render_wakes_idle_reclaim_and_releases_asid() {
                let (mut fw, cache, vm, epoch, released, job) = displaced_vm(1);
                vm.vm.close();
                cache.closed.store(true, Ordering::Release);
                cache.shared.wake_closed_vm_reclaim();
                fw.idle_poll();
                assert!(cache.shared.closed_vm_reclaim.load(Ordering::Acquire));
                assert_eq!(Arc::strong_count(&epoch), 2, "vm_last still holds the epoch");
                assert!(!cache.shared.poll_queued.load(Ordering::Acquire));
                drop(epoch);
                drop(vm);
                assert!(!released.load(Ordering::Acquire));

                drop(job);
                assert_eq!(cache.standby.lock().reserved, 0);
                fw.idle_poll();
                assert!(!cache.shared.closed_vm_reclaim.load(Ordering::Acquire));
                assert!(released.load(Ordering::Acquire), "idle poll released the ASID and last PB epoch");
            }

            #[test]
            fn passed_abort_returns_unpublished_credits_and_preserves_quarantine() {
                let (mut fw, cache, vm, old_epoch, released, job) = displaced_vm(3);
                let aborted = cache.standby.lock().epochs.pop().unwrap();
                aborted.prepare(1, crate::g16::hw::client::pb_heap(0), 512).unwrap();
                aborted.expose();
                fw.pools.bind(1, false);
                fw.pools.hold_alloc(1, &vm, None, None).unwrap();
                fw.pools.publish_flight(1);
                job.commit_render(1);
                vm.vm.close();
                cache.closed.store(true, Ordering::Release);
                cache.shared.wake_closed_vm_reclaim();
                fw.idle_poll();
                assert_eq!(Arc::strong_count(&old_epoch), 2);
                assert_eq!(cache.standby.lock().reserved, 2);

                fw.pools.quarantine_flight(1, Some(aborted.clone()));
                let retained = Arc::new(job, 0).unwrap();
                retained.discard_unpublished_renders();
                retained.discard_unpublished_renders();
                let reserved = cache.standby.lock().reserved;
                assert_eq!(reserved, 0);
                assert_eq!(cache.active_render_passes.load(Ordering::Acquire), 1);
                fw.idle_poll();
                assert_eq!(Arc::strong_count(&old_epoch), 1, "vm_last released the displaced epoch");
                assert_eq!(fw.pools.quarantined_mask(), 1 << 1);
                assert_eq!(Arc::strong_count(&aborted), 2);
                assert!(retained.pb_ext[0].is_some(), "retained job still owns its arena");
                drop(old_epoch);
                drop(aborted);
                drop(vm);
                assert!(!released.load(Ordering::Acquire), "quarantine pins the published ASID");
                drop(retained);
                assert_eq!(cache.active_render_passes.load(Ordering::Acquire), 0);
            }
        }
    }
    pub mod boot {pub fn eproto()->crate::Error{crate::Error(71)}}
    pub mod engine {
        use core::sync::atomic::{AtomicU32, Ordering};
        pub static WMB_CALLS: AtomicU32 = AtomicU32::new(0);
        pub fn wmb() {
            WMB_CALLS.fetch_add(1, Ordering::SeqCst);
            core::sync::atomic::fence(Ordering::SeqCst);
        }
    }
    // Arenas here have the two base units.
    pub mod params {pub fn compute_units()->usize{2}pub fn pb_heap_blocks()->u32{2048}pub fn pb_posted_blocks(heap:u32)->u32{heap}}
    pub struct Events{pub grow:crate::KVec<[u32;18]>}
    pub struct Firmware{pub pools:pools::Pools,pub events:Events}
    pub mod budget{__BUDGET__}
    #[path="__ROOT__/rt.rs"]pub mod rt;
    #[path="__ROOT__/slots.rs"]pub mod slots;
    #[path="__ROOT__/pools.rs"]pub mod pools;
}
#[cfg(test)]mod resources {
    use super::*;use g16::{object::Objects,slots::{Slots,Claims},pools::PoolSet};
    struct Arenas{compute:g16::resources::ComputeArena,render:g16::resources::RenderArena}
    fn render_arena(passes:KVec<g16::slots::PassSlot>)->g16::resources::RenderArena{
        g16::resources::RenderArena{
            passes,
            tpc_dirty:[const{std::sync::atomic::AtomicU64::new(0)};g16::hw::sizes::RENDER_SLOTS],
            tilemap_dirty:[const{std::sync::atomic::AtomicU64::new(0)};g16::hw::sizes::RENDER_SLOTS],
            tilemap_last:[const{std::sync::atomic::AtomicU64::new(0)};g16::hw::sizes::RENDER_SLOTS],
            tilemap_clean:[const{std::sync::atomic::AtomicU64::new(0)};g16::hw::sizes::RENDER_SLOTS],
            heap_dirty:[const{std::sync::atomic::AtomicU64::new(0)};g16::hw::sizes::RENDER_SLOTS],
        }
    }
    fn find_va(va:u64)->sync::Arc<g16::object::FwObject>{
        g16::object::ALL_OBJECTS.with(|a|a.borrow().iter().find(|obj|obj.va==va).unwrap().clone())
    }
    fn arenas(o:&mut Objects)->Arenas{
        let mut compute=KVec::new();let mut passes=KVec::new();
        let f=g16::object::Factory;
        for k in 0..2{compute.push(g16::slots::ComputeSlot::new(o,&f,k,&crate::g16::object::test_unit_cmds(k)).unwrap(),0).unwrap();passes.push(g16::slots::PassSlot::new(o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap(),0).unwrap();}
        Arenas{compute:g16::resources::ComputeArena{compute,render:None,hw_slots:None},render:render_arena(passes)}
    }
    fn retire_render(s:&mut Slots,k:usize){let mut c=Claims::new();c.pass[0]=k as u32;c.pass_mask=1<<k;s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();}
    fn drain_scrubs(batch:g16::slots::ScrubBatch)->bool{let mut q=g16::slots::ScrubQueue::new();q.push_batch(batch);let mut wake=false;while let Some(mut item)=q.pop(){wake|=item.run_one();}wake}
    #[test]fn render_reuse_clears_only_previous_write_spans(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        s.claim_pass(&arena.render,0).unwrap();
        let p=arena.render.passes[0].tpc.clone();let m=arena.render.passes[0].tilemap.clone();
        assert_eq!(*p.fills.lock().unwrap(),[(0,0)]);assert_eq!(*m.fills.lock().unwrap(),[(0,0)]);
        arena.render.set_dirty(0,0x8000,0x5000).unwrap();p.fill(0,0xa5,0x8000).unwrap();m.fill(0,0x5a,0x5000).unwrap();
        p.write(0x8000,&[0xf1]).unwrap();m.write(0x5000,&[0xf2]).unwrap();
        retire_render(&mut s,0);s.claim_pass(&arena.render,1).unwrap();retire_render(&mut s,1);s.claim_pass(&arena.render,0).unwrap();
        assert!(p.snapshot()[..0x8000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x5000].iter().all(|&b|b==0));
        assert_eq!(p.snapshot()[0x8000],0xf1);assert_eq!(m.snapshot()[0x5000],0xf2);
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x8000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x5000)));
        arena.render.set_dirty(0,0x2000,0x1000).unwrap();p.fill(0,0x45,0x2000).unwrap();m.fill(0,0x67,0x1000).unwrap();
        retire_render(&mut s,0);s.claim_pass(&arena.render,1).unwrap();retire_render(&mut s,1);s.claim_pass(&arena.render,0).unwrap();
        assert!(p.snapshot()[..0x8000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x5000].iter().all(|&b|b==0));
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x2000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x1000)));
        assert_eq!((s.stats.tpc_clear_bytes,s.stats.tilemap_clear_bytes,s.stats.pass_scrubs),(0xa000,0x6000,5));
    }
    #[test]fn cancelled_render_preparation_keeps_dirty_union(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);s.claim_pass(&arena.render,0).unwrap();
        let p=arena.render.passes[0].tpc.clone();let m=arena.render.passes[0].tilemap.clone();
        arena.render.set_dirty(0,0x1000,0x3000).unwrap();arena.render.set_dirty(0,0x2000,0x1000).unwrap();
        p.fill(0,0x91,0x2000).unwrap();m.fill(0,0x92,0x3000).unwrap();
        let mut c=Claims::new();c.pass_mask=1;s.release(&mut c);s.claim_pass(&arena.render,0).unwrap();
        assert!(p.snapshot()[..0x2000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x3000].iter().all(|&b|b==0));
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x2000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x3000)));
    }
    #[test]fn failed_scrub_preserves_dirty_bounds_for_retry(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);s.claim_pass(&arena.render,0).unwrap();
        let p=arena.render.passes[0].tpc.clone();let m=arena.render.passes[0].tilemap.clone();
        arena.render.set_dirty(0,0x1000,0x2000).unwrap();p.fill(0,0x31,0x1000).unwrap();m.fill(0,0x32,0x2000).unwrap();
        retire_render(&mut s,0);s.claim_pass(&arena.render,1).unwrap();retire_render(&mut s,1);
        m.fail_fill.store(true,std::sync::atomic::Ordering::Relaxed);assert_eq!(s.claim_pass(&arena.render,0),Err(EIO));
        s.claim_pass(&arena.render,0).unwrap();assert!(p.snapshot()[..0x1000].iter().all(|&b|b==0));assert!(m.snapshot()[..0x2000].iter().all(|&b|b==0));
        assert_eq!(p.fills.lock().unwrap().last(),Some(&(0,0x1000)));assert_eq!(m.fills.lock().unwrap().last(),Some(&(0,0x2000)));
        assert_eq!((s.stats.tpc_clear_bytes,s.stats.tilemap_clear_bytes,s.stats.pass_scrubs),(0x2000,0x2000,3));
    }
    #[test]fn dirty_bounds_reject_overflow_without_poisoning_next_claim(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);s.claim_pass(&arena.render,0).unwrap();
        assert_eq!(arena.render.set_dirty(0,u64::MAX,0),Err(E2BIG));assert_eq!(arena.render.set_dirty(0,0,u64::MAX),Err(E2BIG));assert_eq!(arena.render.set_dirty(2,0,0),Err(EINVAL));
        let mut c=Claims::new();c.pass_mask=1;s.release(&mut c);s.claim_pass(&arena.render,0).unwrap();
        assert_eq!(arena.render.passes[0].tpc.fills.lock().unwrap().last(),Some(&(0,0)));assert_eq!(arena.render.passes[0].tilemap.fills.lock().unwrap().last(),Some(&(0,0)));
    }
    #[test]fn production_allocations_are_aligned_disjoint_and_in_mesa_hole(){
        let mut o=Objects::new();let slots=Slots::new().unwrap();let arena=arenas(&mut o);
        let pools=PoolSet::new(&mut o,&g16::object::Factory).unwrap();
        let mut aliases=Vec::new();
        for a in &o.objects{if let Some(g)=a.gva{assert_eq!(a.client_leaf,Some(__PRIVATE_RW__),"private GPU alias must match qualified J713 NC RW PTE");let base=g-a.offset as u64;assert_eq!(base&0x3fff,0);assert_eq!(a.mapped&0x3fff,0);assert!(base>=0x7000000000);assert!(base+a.mapped as u64<=0x7100000000);aliases.push((base,base+a.mapped as u64));}}
        aliases.push((0x1000000000,0x1000010000)); // Per-VM context store.
        aliases.sort();for a in aliases.windows(2){assert!(a[0].1<=a[1].0,"overlap {:x?} {:x?}",a[0],a[1]);}
        for (k,s) in arena.compute.compute.iter().enumerate(){assert_eq!(s.work.lower_gva.unwrap()&0x3fff,0);assert_eq!(s.work.size,0x2000);assert_eq!(s.pool.size,0x2000000);assert_eq!(s.pool_state.size,0x20000);assert_eq!(s.page_list.size,0x4000);assert_eq!(s.context_store.size,0x40000);assert_eq!(s.pool_state.r32(0).unwrap()as u64 |((s.pool_state.r32(4).unwrap()as u64)<<32),s.pool.gva.unwrap());assert_eq!(pools.allocator_refs_for(k).unwrap().parameter_id,k as u32);}
        // One PB heap per VM, shared by both render allocators.
        assert_eq!(aliases.len(),42);
    }
    #[test]fn firmware_cached_render_records_are_device_lifetime_and_never_owned_by_vm_arenas(){
        use g16::object::{Class,Placement,Request};
        let req=|class,client:Option<(u64,u64)>,lower:Option<u64>|Request{size:0x40,class,placement:Placement::Anywhere,offset:0,client,lower,cpu:true};
        // A per-VM arena cannot map a firmware-cached record (its teardown would unmap it), and
        // a device-lifetime allocator cannot give one a client or lower alias.
        let mut vm=Objects::new();
        assert!(matches!(vm.alloc(req(Class::FwCoherent,None,None)),Err(EINVAL)));
        let mut dev=Objects::persistent();
        assert!(matches!(dev.alloc(req(Class::FwCoherent,Some((0x7040000000,g16::hw::pte::LEAF_NC)),None)),Err(EINVAL)));
        assert!(dev.alloc(req(Class::FwCoherent,None,Some(0x70f1000000))).is_ok());
        // Two VMs' pass slot 3 share index 3's records; neither VM arena owns any of them.
        let (mut a,mut b)=(Objects::new(),Objects::new());
        let pa=g16::slots::PassSlot::new(&mut a,3,1,&crate::g16::object::test_cmds(3)).unwrap();
        let pb=g16::slots::PassSlot::new(&mut b,3,1,&crate::g16::object::test_cmds(3)).unwrap();
        let records=|p:&g16::slots::PassSlot|[p.ta_seq.clone(),p.fs_seq.clone(),p.own_barrier.clone(),p.prev_barrier.clone(),p.ta_fs_barrier.clone(),p.fs_dep.clone(),p.compute_barrier.clone(),p.fs_compute_barrier.clone(),p.ta_work.clone(),p.fs_work.clone()];
        let (ra,rb)=(records(&pa),records(&pb));
        for (i,(x,y)) in ra.iter().zip(rb.iter()).enumerate(){
            assert!(crate::sync::Arc::ptr_eq(x,y));
            assert_eq!(x.class,Class::FwCoherent);
            assert_eq!((x.gva,x.client_leaf),(None,None));
            // Only the work images have the (uncached) kernel GPU task lower alias.
            assert_eq!(x.lower_gva.is_some(),i>=8);
        }
        assert!(ra[8..].iter().all(|o|o.lower_leaf==Some(g16::hw::pte::LOWER)));
        assert_eq!((pa.refs().ta_work_gva,pa.refs().fs_work_gva),(pb.refs().ta_work_gva,pb.refs().fs_work_gva));
        assert_eq!((pa.refs().ta_work,pa.refs().ta_work_gva),(ra[8].va,ra[8].lower_gva.unwrap()));
        assert!(a.objects.iter().chain(b.objects.iter()).all(|o|o.class!=Class::FwCoherent));
        assert!(a.objects.iter().chain(b.objects.iter()).all(|o|ra.iter().all(|r|!crate::sync::Arc::ptr_eq(o,r))));
        let pc=g16::slots::PassSlot::new(&mut b,4,1,&crate::g16::object::test_cmds(4)).unwrap();
        assert!(!crate::sync::Arc::ptr_eq(&pc.ta_seq,&pa.ta_seq)&&!crate::sync::Arc::ptr_eq(&pc.own_barrier,&pa.own_barrier));
        // Microsequences keep whole pages; the barrier records are disjoint 0x40-aligned
        // records of one page; the encoders name the shared records.
        assert_eq!((pa.ta_seq.va&0x3fff,pa.fs_seq.va&0x3fff),(0,0));
        let page=ra[2].va&!0x3fff;
        let mut spans:Vec<(u64,u64)>=ra[2..8].iter().map(|o|(o.va,o.va+o.size as u64)).collect();
        spans.sort();
        assert!(spans.windows(2).all(|w|w[0].1<=w[1].0));
        assert!(ra[2..8].iter().all(|o|o.va&!0x3fff==page&&o.va%0x40==0&&o.va+o.size as u64<=page+0x4000));
        assert_eq!((pa.refs().ta_seq,pa.refs().fs_seq),(pb.refs().ta_seq,pb.refs().fs_seq));
        assert_eq!((pa.refs().ta_seq,pa.refs().fs_seq),(pa.ta_seq.va,pa.fs_seq.va));
        // Closing VM a drops its arena and pass slot: the device table still owns and hands
        // out the same records, mapped by its never-dropped allocator.
        drop(pa);drop(a);
        let again=crate::g16::object::test_cmds(3);
        assert!(crate::sync::Arc::ptr_eq(&again.ta_seq,&rb[0])&&crate::sync::Arc::ptr_eq(&again.fs_dep,&rb[5]));
        crate::g16::object::PERSISTENT.with(|p|{
            let p=p.borrow();let owned=&p.as_ref().unwrap().objects;
            assert!(owned.iter().any(|o|crate::sync::Arc::ptr_eq(o,&rb[0])));
            assert!(owned.iter().any(|o|crate::sync::Arc::ptr_eq(o,&rb[1])));
            assert!(owned.iter().any(|o|crate::sync::Arc::ptr_eq(o,&rb[8])));
            assert!(owned.iter().any(|o|crate::sync::Arc::ptr_eq(o,&rb[9])));
            assert!(owned.iter().all(|o|o.class==Class::FwCoherent));
        });
        drop(pb);drop(pc);drop(b);
    }
    #[test]fn firmware_cached_compute_records_and_work_rings_are_device_lifetime_and_never_owned_by_vm_arenas(){
        use g16::graph::{ComputeGraph, RenderGraph};
        use g16::object::{Class, Placement, Request};
        use g16::slots::{ComputeSlot, Placement as SlotPlacement, UnitCmds};
        let mut vm_a = Objects::new();
        let mut vm_b = Objects::new();
        let f = g16::object::Factory;
        // Per-VM non-persistent allocator refuses UnitCmds (Class::FwCoherent).
        assert!(matches!(UnitCmds::new(&mut vm_a, 3), Err(EINVAL)));
        // Normal VM unit 3 and an SCS VM local unit 0 bound to hardware slot 3 share slot 3's
        // device-lifetime work + seq + barrier records; neither VM arena owns any FwCoherent object.
        let ca = ComputeSlot::new(&mut vm_a, &f, 3, &g16::object::test_unit_cmds(3)).unwrap();
        let cb = ComputeSlot::new_in(
            &mut vm_b,
            &f,
            0,
            3,
            SlotPlacement::Scs { seeds: 8192 },
            &g16::object::test_unit_cmds(3),
        )
        .unwrap();
        let ra = [ca.work.clone(), ca.seq.clone(), ca.barrier.clone()];
        let rb = [cb.work.clone(), cb.seq.clone(), cb.barrier.clone()];
        for (idx, (x, y)) in ra.iter().zip(rb.iter()).enumerate() {
            assert!(crate::sync::Arc::ptr_eq(x, y));
            assert_eq!(x.class, Class::FwCoherent);
            assert_eq!((x.gva, x.client_leaf), (None, None));
            assert_eq!(x.lower_gva.is_some(), idx == 0);
            assert_eq!(x.va & 0x3fff, 0);
        }
        assert_ne!(ca.work.va, ca.seq.va);
        assert_ne!(ca.seq.va, ca.barrier.va);
        assert_eq!(ca.descriptor.class, Class::Fw);
        assert_eq!(cb.descriptor.class, Class::Fw);
        assert_ne!(ca.descriptor.va, cb.descriptor.va);
        assert!(vm_a.objects.iter().chain(vm_b.objects.iter()).all(|o| o.class != Class::FwCoherent));
        assert!(vm_a
            .objects
            .iter()
            .chain(vm_b.objects.iter())
            .all(|o| ra.iter().all(|r| !crate::sync::Arc::ptr_eq(o, r))));
        // Distinct hardware slot gets distinct UnitCmds.
        let cc = ComputeSlot::new(&mut vm_b, &f, 4, &g16::object::test_unit_cmds(4)).unwrap();
        assert!(!crate::sync::Arc::ptr_eq(&cc.work, &ca.work));
        assert!(!crate::sync::Arc::ptr_eq(&cc.seq, &ca.seq));
        assert!(!crate::sync::Arc::ptr_eq(&cc.barrier, &ca.barrier));
        // Closing VM a leaves slot 3's UnitCmds owned by the device-lifetime allocator.
        drop(ca);
        drop(vm_a);
        let again = g16::object::test_unit_cmds(3);
        assert!(crate::sync::Arc::ptr_eq(&again.work, &rb[0]));
        assert!(crate::sync::Arc::ptr_eq(&again.seq, &rb[1]));
        assert!(crate::sync::Arc::ptr_eq(&again.barrier, &rb[2]));
        g16::object::PERSISTENT.with(|p| {
            let p = p.borrow();
            let owned = &p.as_ref().unwrap().objects;
            assert!(owned.iter().any(|o| crate::sync::Arc::ptr_eq(o, &rb[0])));
            assert!(owned.iter().any(|o| crate::sync::Arc::ptr_eq(o, &rb[1])));
            assert!(owned.iter().any(|o| crate::sync::Arc::ptr_eq(o, &rb[2])));
            assert!(owned.iter().all(|o| o.class == Class::FwCoherent));
        });
        // Work rings require a device-lifetime persistent allocator and never live in session o.
        let mut session = Objects::new();
        let mut non_persistent = Objects::new();
        let mut rings = Objects::persistent();
        let ctx = session
            .alloc(Request {
                size: 0x40,
                class: Class::Fw,
                placement: Placement::Anywhere,
                offset: 0,
                client: None,
                lower: None,
                cpu: true,
            })
            .unwrap();
        assert!(matches!(
            ComputeGraph::new(&mut session, &mut non_persistent, ctx.clone()),
            Err(EINVAL)
        ));
        assert!(matches!(
            RenderGraph::new(&mut session, &mut non_persistent, ctx.clone()),
            Err(EINVAL)
        ));
        let cg0 = ComputeGraph::new(&mut session, &mut rings, ctx.clone()).unwrap();
        let cg1 = ComputeGraph::new(&mut session, &mut rings, ctx.clone()).unwrap();
        let rg = RenderGraph::new(&mut session, &mut rings, ctx).unwrap();
        let all_rings = [&cg0.ring, &cg1.ring, &rg.ring[0], &rg.ring[1]];
        assert_eq!(rings.objects.len(), 4);
        for ring in all_rings {
            assert_eq!(ring.class, Class::FwCoherent);
            assert_eq!((ring.gva, ring.client_leaf, ring.lower_gva), (None, None, None));
            assert!(rings.objects.iter().any(|o| crate::sync::Arc::ptr_eq(o, ring)));
            assert!(session.objects.iter().all(|o| !crate::sync::Arc::ptr_eq(o, ring)));
        }
        drop(cb);
        drop(cc);
        drop(vm_b);
    }
    #[test]fn work_has_kernel_lower_alias_without_application_alias(){
        let mut o=Objects::new();let slots=Slots::new().unwrap();let arena=arenas(&mut o);
        assert!(arena.compute.compute.iter().all(|s|s.work.class==g16::object::Class::FwCoherent));
        assert!(arena.render.passes.iter().all(|s|s.ta_work.class==g16::object::Class::FwCoherent&&s.fs_work.class==g16::object::Class::FwCoherent));
        let work=arena.compute.compute.iter().map(|s|&s.work)
            .chain(arena.render.passes.iter().flat_map(|s|[&s.ta_work,&s.fs_work]));
        for object in work {
            assert_eq!(object.gva,None,"work must not enter the application alias inventory");
            assert_eq!(object.client_leaf,None);
            assert!(object.lower_gva.is_some());
            assert_eq!(object.lower_leaf,Some(0x00c0000000000c8b));
            assert!(object.class==g16::object::Class::Shared||object.class==g16::object::Class::FwCoherent);
            assert!(object.va>=0xfffffc2000000000);
            let lower=object.lower_gva.unwrap();
            for client in &o.objects {
                if let Some(gva)=client.gva {
                    assert!(lower<gva-client.offset as u64 || lower>=gva-client.offset as u64+client.mapped as u64,
                        "a second application alias exposes retained work backing");
                }
            }
        }
        for s in arena.compute.compute.iter() {
            for object in [&s.preempt,&s.pool,&s.pool_state,&s.page_list,&s.context_store,&s.aux] {
                assert_eq!(object.client_leaf,Some(0x00c0000000000c8b));
                assert!(object.gva.is_some());
            }
        }
        for s in arena.render.passes.iter() {
            for object in [&s.tpc,&s.tilemap,&s.heap,&s.scene_user,&s.scene_list,&s.fs_scratch,&s.aux] {
                assert_eq!(object.client_leaf,Some(0x00c0000000000c8b));
                assert!(object.gva.is_some());
            }
            let refs=s.refs();
            assert_eq!(refs.ta_work_gva,s.ta_work.lower_gva.unwrap());
            assert_eq!(refs.fs_work_gva,s.fs_work.lower_gva.unwrap());
            assert_eq!(refs.scene_list,s.scene_list.gva.unwrap());
        }
    }
    #[test]fn allocator_metadata_is_firmware_only_and_shared_state_keeps_real_backing(){
        let mut o=Objects::new();let pools=PoolSet::new(&mut o,&g16::object::Factory).unwrap();
        let read64=|obj:&g16::object::FwObject,at:usize|->u64{obj.r32(at).unwrap()as u64|((obj.r32(at+4).unwrap()as u64)<<32)};
        for k in 0..2 {
            let refs=pools.allocator_refs_for(k).unwrap();
            let descriptor=find_va(refs.uma_mgr[0]);
            let auxiliary=o.objects.iter().find(|obj|obj.va+0x100==refs.uma_aux[0]).unwrap();
            assert_eq!(descriptor.class,g16::object::Class::Fw);
            assert_eq!(descriptor.gva,None);assert_eq!(descriptor.client_leaf,None);
            assert_eq!(descriptor.size,g16::fw::graph::manager::SIZE);
            assert_eq!(auxiliary.class,g16::object::Class::Shared);
            assert_eq!(auxiliary.client_leaf,Some(__PRIVATE_RW__));
            assert_eq!(auxiliary.gva,Some(refs.uma_gpu));
            assert_eq!(refs.uma_aux,[auxiliary.va+0x100,auxiliary.va+0x200]);
            assert!(!std::ptr::eq(&descriptor.data,&auxiliary.data));
            // Shared auxiliary updates cannot change allocator pointers/counters.
            let metadata=descriptor.snapshot();
            {
                // The recorder allocates CPU-visible bytes only for cpu=true.
                // Materialize this GPU-only backing to model writes through its
                // writable client alias without requesting a CPU map in production.
                let mut gpu_bytes=auxiliary.data.lock().unwrap();
                gpu_bytes.resize(auxiliary.mapped,0);
                gpu_bytes[0x100..0x108].copy_from_slice(&0xabcdef0123456789u64.to_le_bytes());
                gpu_bytes[0x200..0x208].copy_from_slice(&0x76543210fedcba98u64.to_le_bytes());
            }
            assert_eq!(metadata,descriptor.snapshot());
            // Freelist and pool-table pointers still select writable client mappings.
            let state_va=read64(&descriptor,g16::fw::graph::manager::FREELIST);
            let list_va=read64(&descriptor,g16::fw::graph::manager::POOL_TABLE);
            for va in [list_va,state_va] {
                let shared=o.objects.iter().find(|obj|obj.gva==Some(va)).unwrap();
                assert_eq!(shared.class,g16::object::Class::Shared);
                assert_eq!(shared.client_leaf,Some(__PRIVATE_RW__));
            }
            let state=o.objects.iter().find(|obj|obj.gva==Some(state_va)).unwrap();
            let pool_va=read64(state,0);
            let pool=o.objects.iter().find(|obj|obj.gva==Some(pool_va)).unwrap();
            assert_eq!(pool.class,g16::object::Class::Shared);
            assert_eq!(pool.client_leaf,Some(__PRIVATE_RW__));
        }
    }
    #[test]fn reuse_preserves_pool_lists_counter_and_requires_full_displacement(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        arena.compute.compute[0].pool_state.w64(8,0xdeadbeef).unwrap();
        arena.compute.compute[0].page_list.w64(0x1000,0xaabbccdd).unwrap();
        let a=arena.compute.compute[0].pool_state.snapshot();let b=arena.compute.compute[0].page_list.snapshot();
        let mut c=Claims::new();assert_eq!(s.claim_compute(&arena.compute,&mut c,0).unwrap(),Some(0));s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.free_compute(),1);assert_eq!(s.claim_compute(&arena.compute,&mut c,0).unwrap(),Some(1));s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        arena.compute.compute[0].counter.w64(0,91).unwrap();
        assert_eq!(s.claim_compute(&arena.compute,&mut c,0).unwrap(),Some(0));
        assert_eq!(a,arena.compute.compute[0].pool_state.snapshot());assert_eq!(b,arena.compute.compute[0].page_list.snapshot());assert_eq!(arena.compute.compute[0].counter.r32(0).unwrap(),91);
        s.publish(&mut c).unwrap();s.quarantine(&mut c);assert!(s.compute_exhausted());
    }
    #[test]fn production_render_descriptors_preserve_firmware_bytes_and_exact_counter(){
        let mut o=Objects::new();let mut pools=PoolSet::new(&mut o,&g16::object::Factory).unwrap();
        let a=pools.prepare(0,1).unwrap();assert!(a.init_buffer);assert_eq!(a.uma_submitted,2);
        assert!(!pools.retired(0).unwrap());pools.publish_batch(0,2,1).unwrap();
        assert!(pools.publish_batch(0,2,1).is_err());assert!(pools.prepare(0,2).is_err());
        let d=find_va(a.uma_mgr[0]);
        d.w64(0x54,2).unwrap();d.w64(0x60,0x5a5aa5a5).unwrap();
        let original=d.snapshot();assert!(pools.retired(0).unwrap());
        let next=pools.prepare(0,2).unwrap();assert!(next.init_buffer);assert_eq!(next.uma_submitted,4);
        assert_eq!(original,d.snapshot());assert_eq!(pools.prepare(1,3).unwrap().parameter_id,1);
    }

    #[test]fn cross_vm_evidence_counts_only_noted_publication(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let a=sync::Arc::new(
            g16::vm::VmState{id:100,slot:1,generation:1,vm:g16::vm::MockVm::new()},0,
        ).unwrap();
        let b=sync::Arc::new(
            g16::vm::VmState{id:101,slot:2,generation:1,vm:g16::vm::MockVm::new()},0,
        ).unwrap();
        let c=sync::Arc::new(
            g16::vm::VmState{id:102,slot:3,generation:9,vm:g16::vm::MockVm::new()},0,
        ).unwrap();
        let mut claims=Claims::new();
        for vm in [&a,&b]{s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,vm).unwrap();s.publish(&mut claims).unwrap();s.note_published(&claims);s.retire(&mut claims).unwrap();}
        assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        assert_eq!(s.claim_compute(&arena.compute,&mut claims,0).unwrap(),Some(0));
        s.hold_vm(&claims,&c).unwrap();assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        s.publish(&mut claims).unwrap();assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
        let proof=s.stats.last_retarget[0];
        assert_eq!((proof.unit,proof.old_vm,proof.new_vm),(0,100,102));
        assert_eq!((proof.old_asid,proof.new_asid,proof.old_generation,proof.new_generation),(1,3,1,9));
        s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
        s.retire(&mut claims).unwrap();
        s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,&a).unwrap();
        s.release(&mut claims);s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
    }

    pub(super) fn vm(id:u64)->sync::Arc<g16::vm::VmState>{
        sync::Arc::new(
            g16::vm::VmState{id,slot:id as u32,generation:1,vm:g16::vm::MockVm::new()},0,
        ).unwrap()
    }
    #[test]fn previous_vm_lease_survives_replacement_publication_until_full_retirement(){
        let mut o=Objects::new();let arena=arenas(&mut o);let mut s=Slots::new().unwrap();
        let a=vm(1);let b=vm(2);let c=vm(3);let mut claims=Claims::new();
        for owner in [&a,&b]{s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,owner).unwrap();s.publish(&mut claims).unwrap();s.retire(&mut claims).unwrap();}
        assert_eq!(sync::Arc::strong_count(&a),2);
        assert_eq!(s.claim_compute(&arena.compute,&mut claims,0).unwrap(),Some(0));
        s.hold_vm(&claims,&c).unwrap();s.publish(&mut claims).unwrap();s.note_published(&claims);
        assert_eq!(sync::Arc::strong_count(&a),2,"publication cannot release old ASID/backing");
        assert_eq!(sync::Arc::strong_count(&c),2,"candidate must be held too");
        s.retire(&mut claims).unwrap();
        assert_eq!(sync::Arc::strong_count(&a),1);assert_eq!(sync::Arc::strong_count(&c),2);
    }
    #[test]fn cancel_releases_candidate_but_quarantine_preserves_both_vm_leases(){
        let mut o=Objects::new();let arena=arenas(&mut o);let mut s=Slots::new().unwrap();
        let a=vm(1);let b=vm(2);let c=vm(3);let mut claims=Claims::new();
        for owner in [&a,&b]{s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,owner).unwrap();s.publish(&mut claims).unwrap();s.retire(&mut claims).unwrap();}
        s.claim_compute(&arena.compute,&mut claims,0).unwrap();s.hold_vm(&claims,&c).unwrap();s.release(&mut claims);
        assert_eq!(sync::Arc::strong_count(&a),2);assert_eq!(sync::Arc::strong_count(&c),1);
        s.claim_compute(&arena.compute,&mut claims,0).unwrap();s.hold_vm(&claims,&c).unwrap();
        s.quarantine(&mut claims);s.release(&mut claims);
        assert_eq!(sync::Arc::strong_count(&a),2);assert_eq!(sync::Arc::strong_count(&c),2);
    }
    #[test]fn pb_installations_are_fresh_and_exposed_records_cannot_be_reinitialized(){
        let vm=vm(1);let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let a=g16::pools::PbEpoch::new(&factory,&vm,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
        a.prepare(0,0x70c0000000,2048).unwrap();a.check_assignment(0).unwrap();a.expose();
        a.manager.w64(0xb0,0x123456789abcdef0).unwrap();let saved=a.manager.snapshot();
        assert_eq!(a.prepare(0,0x70c0000000,2048),Err(EINVAL));assert_eq!(saved,a.manager.snapshot());
        let b=g16::pools::PbEpoch::new(&factory,&vm,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();b.prepare(0,0x70c0000000,2048).unwrap();
        assert!(!std::ptr::eq(&a.manager.data,&b.manager.data));
        assert_eq!(b.manager.class,g16::object::Class::Fw);assert_eq!(b.manager.gva,None);
        let mut slots=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        slots.retire_binding(0,&a).unwrap();assert_eq!(sync::Arc::strong_count(&a),2);
        b.check_assignment(0).unwrap();b.expose();assert_eq!(sync::Arc::strong_count(&a),2);
        slots.retire_binding(0,&b).unwrap();assert_eq!(sync::Arc::strong_count(&a),1);
    }
    #[test]fn pb_reuse_needs_the_same_binding_vm_and_a_completed_render_and_holds_nothing(){
        let one=vm(1);let two=vm(2);let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let a=g16::pools::PbEpoch::new(&factory,&one,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
        a.prepare(0,0x70c0000000,2048).unwrap();a.check_assignment(0).unwrap();a.expose();
        let mut p=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let n=p.bind(0,false);p.retire_binding(0,&a).unwrap();p.retire_epoch(0,n,true);
        assert_eq!(sync::Arc::strong_count(&a),2,"only the owner lease");
        let r=p.reusable(0,&one).unwrap();assert!(sync::Arc::ptr_eq(&r,&a));drop(r);
        assert_eq!(sync::Arc::strong_count(&a),2);
        assert!(p.reusable(0,&two).is_none(),"another VM");assert!(p.reusable(1,&one).is_none(),"another slot");
        assert_eq!(p.bind(0,true),n);p.retire_epoch(0,n,true);assert!(p.reusable(0,&one).is_some());
        p.retire_epoch(0,n,false);assert!(p.reusable(0,&one).is_none(),"failed render");
        p.retire_epoch(0,n,true);p.forget(0);assert!(p.reusable(0,&one).is_none(),"aborted render");
        let m=p.bind(0,false);assert_ne!(m,n);p.retire_epoch(0,n,true);
        assert!(p.reusable(0,&one).is_none(),"a stale binding after a fresh one");
        p.retire_epoch(0,m,true);assert!(p.reusable(0,&one).is_some());
        p.invalidate();assert!(p.reusable(0,&one).is_none(),"recovery");
        p.retire_epoch(0,m,true);assert!(p.reusable(0,&one).is_none(),"a render retiring after recovery");
        assert_eq!(p.bind(0,true),m.wrapping_add(1));
        assert_eq!(sync::Arc::strong_count(&a),2);
    }

    #[test]fn standby_pool_reuses_epochs_at_depth_16_and_drains_on_close(){
        // Models Cache's Standby invariant: epochs.len() >= reserved while open or draining.
        let one=vm(1);let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let mut epochs:Vec<sync::Arc<g16::pools::PbEpoch>>=Vec::new();
        let mut reserved:u32=0;
        let mut allocs:u32=0;
        let prepare=|n:u32,epochs:&mut Vec<sync::Arc<g16::pools::PbEpoch>>,reserved:&mut u32,allocs:&mut u32|{
            let deficit=reserved.checked_add(n).unwrap().saturating_sub(epochs.len() as u32);
            if deficit!=0 {
                let mut res=g16::budget::Budget::reserve(&budget,deficit).unwrap();
                for _ in 0..deficit {
                    epochs.push(g16::pools::PbEpoch::new(&factory,&one,res.take().unwrap()).unwrap());
                    *allocs+=1;
                }
            }
            *reserved+=n;
            assert!(epochs.len()>=*reserved as usize);
        };
        // Ramp to depth 16: allocates 16 standby epochs once.
        for _ in 0..16 { prepare(1,&mut epochs,&mut reserved,&mut allocs); }
        assert_eq!((allocs,epochs.len(),reserved),(16,16,16));
        // Steady-state d=16 with PB reuse: 64 more submits allocate zero new epochs.
        for _ in 0..64 {
            reserved-=1; // previous render committed with reused PB
            prepare(1,&mut epochs,&mut reserved,&mut allocs);
        }
        assert_eq!((allocs,epochs.len(),reserved),(16,16,16));
        // One fresh InitBM consumes 1 standby epoch and commits 1 reservation; next submit refills 1.
        let fresh=epochs.pop().unwrap();
        assert!(fresh.unexposed());
        fresh.prepare(0,0x70c0000000,2048).unwrap();
        fresh.expose();
        reserved-=1;
        assert_eq!((epochs.len(),reserved),(15,15));
        prepare(1,&mut epochs,&mut reserved,&mut allocs);
        assert_eq!((allocs,epochs.len(),reserved),(17,16,16));
        // Unpublished rollback returns the unexposed epoch without losing capacity.
        let rolled=epochs.pop().unwrap();
        assert!(rolled.unexposed());
        epochs.push(rolled);
        assert_eq!(epochs.len(),16);
        // Closing trims surplus immediately while preserving reserved epochs until jobs finish.
        reserved=2;
        epochs.truncate(reserved as usize);
        assert_eq!(epochs.len(),2);
        reserved=0;
        epochs.truncate(reserved as usize);
        assert!(epochs.is_empty());
        drop(fresh);
        assert_eq!(sync::Arc::strong_count(&one),1);
    }

    #[test]fn compute_same_vm_reuse_skips_bulk_scrub_and_cross_vm_or_recovery_full_scrubs(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let a=vm(1);let b=vm(2);let mut c=Claims::new();
        // Fresh zero-allocated per-VM units skip private WC scratch fills on first use while
        // still scrubbing shared command records (`work`, `seq`), clearing `its`, and requiring
        // full work writes.
        for _ in 0..2 {
            let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
            assert!(s.compute_needs_full_work(k));
            assert_eq!(arena.compute.compute[k].context_store.fills.lock().unwrap().len(),0);
            assert_eq!(arena.compute.compute[k].its.fills.lock().unwrap().len(),1);
            assert_eq!(arena.compute.compute[k].seq.fills.lock().unwrap().len(),1);
            s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        }
        // Same-VM clean reuse of unit 0 clears ONLY its (16 B), skipping context_store/aux/preempt/seq.
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(!s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].its.fills.lock().unwrap().len(),2);
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),0);
        assert_eq!(arena.compute.compute[0].preempt.fills.lock().unwrap().len(),0);
        assert_eq!(arena.compute.compute[0].aux.fills.lock().unwrap().len(),0);
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        // Defense-in-depth: reusing the same unit object for a different VM forces a full private scrub.
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(b.id)).unwrap().unwrap();
        assert_eq!(k,1);
        assert!(s.compute_needs_full_work(1));
        assert_eq!(arena.compute.compute[1].context_store.fills.lock().unwrap().len(),1);
        assert_eq!(arena.compute.compute[1].preempt.fills.lock().unwrap().len(),1);
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        // Firmware recovery forces full scrub even for same VM.
        s.invalidate_scrub();
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),1);
        // Cancelled preparation forces full scrub on next claim.
        s.release(&mut c);
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),2);
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
    }

    #[test]fn pass_same_vm_reuse_skips_bulk_scrub_and_preserves_tpc_dirty_for_full_scrub(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let a=vm(1);let b=vm(2);
        let retire=|s:&mut Slots,k:usize,owner:&sync::Arc<g16::vm::VmState>|{
            let mut c=Claims::new();c.pass[0]=k as u32;c.pass_mask=1<<k;
            s.hold_vm(&c,owner).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        };
        // First use of freshly zero-allocated slots 0 and 1 skips private WC fills while clearing
        // shared command records and `its`.
        s.claim_pass_for(&arena.render,0,Some(a.id)).unwrap();
        assert!(s.render_needs_full_work(0));
        arena.render.set_dirty(0,0x8000,0x4000).unwrap();
        retire(&mut s,0,&a);
        s.claim_pass_for(&arena.render,1,Some(a.id)).unwrap();
        retire(&mut s,1,&a);
        let p0=&arena.render.passes[0];
        assert_eq!(p0.aux.fills.lock().unwrap().len(),0);
        assert_eq!(p0.scene.fills.lock().unwrap().len(),0);
        assert_eq!(p0.tpc.fills.lock().unwrap().len(),0);
        assert_eq!(p0.its.fills.lock().unwrap().len(),1);
        assert_eq!(p0.ta_work.fills.lock().unwrap().len(),1);
        // Same-VM clean reuse of slot 0: clears tilemap and active heap(0x200) only; skips aux/tpc/scratch/scene/its/pre.
        s.claim_pass_for(&arena.render,0,Some(a.id)).unwrap();
        assert!(!s.render_needs_full_work(0));
        assert_eq!(p0.aux.fills.lock().unwrap().len(),0);
        assert_eq!(p0.scene.fills.lock().unwrap().len(),0);
        assert_eq!(p0.ta_scratch.fills.lock().unwrap().len(),0);
        assert_eq!(p0.fs_scratch.fills.lock().unwrap().len(),0);
        assert_eq!(p0.scene_user.fills.lock().unwrap().len(),0);
        assert_eq!(p0.scene_list.fills.lock().unwrap().len(),0);
        assert_eq!(p0.tpc.fills.lock().unwrap().len(),0);
        assert_eq!(p0.its.fills.lock().unwrap().len(),1);
        assert_eq!(p0.pre[0].fills.lock().unwrap().len(),0);
        assert_eq!(p0.pre[1].fills.lock().unwrap().len(),0);
        assert_eq!(p0.pre[2].fills.lock().unwrap().len(),0);
        assert_eq!(p0.tilemap.fills.lock().unwrap().last(),Some(&(0,0x4000)));
        assert_eq!(p0.heap.fills.lock().unwrap().last(),Some(&(0,0x200)));
        // Preserved tpc_dirty high-water mark is still cleared when reusing the same slot object
        // for a different VM (defense-in-depth) or after recovery.
        arena.render.set_dirty(0,0x2000,0x1000).unwrap();
        retire(&mut s,0,&a);
        s.claim_pass_for(&arena.render,1,Some(a.id)).unwrap();
        retire(&mut s,1,&a);
        s.claim_pass_for(&arena.render,0,Some(b.id)).unwrap();
        assert_eq!(p0.aux.fills.lock().unwrap().len(),1);
        assert_eq!(p0.scene.fills.lock().unwrap().len(),1);
        assert_eq!(p0.scene_list.fills.lock().unwrap().len(),1);
        assert_eq!(p0.its.fills.lock().unwrap().len(),2);
        assert_eq!(p0.pre[0].fills.lock().unwrap().len(),1);
        assert_eq!(p0.tpc.fills.lock().unwrap().last(),Some(&(0,0x8000)));
        assert_eq!(p0.tilemap.fills.lock().unwrap().last(),Some(&(0,0x1000)));
        assert_eq!(p0.heap.fills.lock().unwrap().last(),Some(&(0,0x4000)));
    }

    #[test]fn forty_eight_render_passes_and_thirty_two_compute_units_are_disjoint_in_mesa_hole(){
        assert_eq!(g16::hw::sizes::RENDER_SLOTS, 48);
        assert_eq!(g16::hw::sizes::COMPUTE_UNITS, 32);
        assert_eq!(g16::hw::sizes::COMPUTE_PIPES, 16);
        assert_eq!(g16::hw::sizes::PIPE1_COMPUTE_START, 2);
        let mut o=Objects::new();
        let pools=PoolSet::new(&mut o,&g16::object::Factory).unwrap();
        for idx in 0..g16::hw::sizes::EXT_PB_SLOTS {
            let local=(g16::hw::sizes::BASE_PB_SLOTS+idx) as u64;
            let hw_slot=(g16::hw::sizes::WARM_PB_SLOTS+idx) as u32;
            let ext=g16::pools::Allocator::new_at(&mut o,&g16::object::Factory,local,hw_slot,pools.heap(),g16::slots::Placement::Base).unwrap();
            assert_eq!(ext.refs().unwrap().parameter_id,hw_slot);
        }
        for k in 0..g16::hw::sizes::COMPUTE_UNITS as u64 {
            g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,k,&crate::g16::object::test_unit_cmds(k)).unwrap();
        }
        let mut scene_lists=Vec::new();
        let count_client=|objs:&Objects|objs.objects.iter().filter(|a|a.gva.is_some()).count();
        for k in 0..g16::hw::sizes::RENDER_SLOTS as u64 {
            let pass=g16::slots::PassSlot::new(&mut o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap();
            let va=pass.refs().scene_list;
            assert_ne!(va,0);
            assert!(!scene_lists.contains(&va),"each pass slot must own a distinct scene_list");
            scene_lists.push(va);
        }
        // Every demand-grown extension tier stays strictly within MAX_CLIENT (512) per batch.
        for &(start, end) in &g16::resources::EXT_RENDER_TIERS {
            let mut ot = Objects::new();
            for k in start..end {
                g16::slots::PassSlot::new(&mut ot, k as u64, 1, &crate::g16::object::test_cmds((k as u64) as u64)).unwrap();
            }
            let tier_maps = count_client(&ot);
            assert!(
                tier_maps <= g16::hw::sizes::MAX_CLIENT,
                "tier {start}..{end} client maps {tier_maps} > MAX_CLIENT"
            );
        }
        let mut aliases=Vec::new();
        for a in &o.objects {
            if let Some(g)=a.gva {
                let base=g-a.offset as u64;
                assert_eq!(base&0x3fff,0);
                assert!(base>=0x7000000000 && base+a.mapped as u64<=0x7100000000);
                // Must stay disjoint from the dynamic client range 0x7040000000..0x7080000000.
                assert!(base+a.mapped as u64<=0x7040000000 || base>=0x7080000000,
                    "overlap with dynamic client range: {:#x}..{:#x}", base, base+a.mapped as u64);
                aliases.push((base,base+a.mapped as u64));
            }
        }
        aliases.sort();
        for w in aliases.windows(2) {
            assert!(w[0].1<=w[1].0,"overlap {:x?} {:x?}",w[0],w[1]);
        }
    }

    #[test]fn decoupled_pb_allocator_supports_same_vm_pipelining_cross_vm_displacement_and_abort(){
        let mut o=Objects::new();let pools=PoolSet::new(&mut o,&g16::object::Factory).unwrap();
        let vm1=vm(1);let vm2=vm(2);let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let e1=g16::pools::PbEpoch::new(&factory,&vm1,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
        e1.prepare(0,0x70c0000000,2048).unwrap();e1.check_assignment(0).unwrap();e1.expose();
        let e2=g16::pools::PbEpoch::new(&factory,&vm2,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
        e2.prepare(1,0x70c0000000,2048).unwrap();e2.check_assignment(1).unwrap();e2.expose();
        let mut p=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        // Priming render on slot 0 (VM1).
        assert_eq!(p.pick_slot(&vm1,false),Some(0));
        let b1=p.bind(0,false);
        let a1=pools.prepare_in_flight(0,1,false).unwrap();
        assert_eq!(a1.uma_submitted,2);
        pools.publish_batch(0,2,1).unwrap();p.publish_flight(0);
        let d0=find_va(a1.uma_mgr[0]);
        d0.w64(0x54,2).unwrap();
        p.retire_flight(0,&e1,b1,true).unwrap();
        // Same-VM pipelining: 3 concurrent renders share slot 0 with cumulative uma_submitted (4, 6, 8).
        assert!(p.can_pipeline(&vm1));
        assert!(!p.can_pipeline(&vm2));
        for draw in 2..=4u64 {
            assert_eq!(p.pick_slot(&vm1,draw>2),Some(0));
            let refs=pools.prepare_in_flight(0,draw,draw>2).unwrap();
            assert_eq!(refs.uma_submitted,draw*2);
            pools.publish_batch(0,refs.uma_submitted,1).unwrap();
            p.publish_flight(0);
        }
        assert!(!pools.retired_at(0,4).unwrap());
        d0.w64(0x54,4).unwrap();
        assert!(pools.retired_at(0,4).unwrap());
        assert!(!pools.retired_at(0,6).unwrap());
        p.retire_flight(0,&e1,b1,true).unwrap();
        d0.w64(0x54,6).unwrap();
        assert!(pools.retired_at(0,6).unwrap());
        p.retire_flight(0,&e1,b1,true).unwrap();
        d0.w64(0x54,8).unwrap();
        assert!(pools.retired(0).unwrap());
        p.retire_flight(0,&e1,b1,true).unwrap();
        // Cross-VM switch drains and switches to slot 1 (displacing slot 0 upon clean retirement).
        assert_eq!(p.pick_slot(&vm2,false),Some(1));
        let b2=p.bind(1,false);
        p.publish_flight(1);
        p.retire_flight(1,&e2,b2,true).unwrap();
        assert!(!p.exhausted());
        assert_eq!(p.needed_slots(),g16::hw::sizes::BASE_PB_SLOTS);
        // Normal cross-VM switch back to VM1 reuses displaced slot 0 rather than claiming slot 2.
        assert_eq!(p.pick_slot(&vm1,false),Some(0));
    }

    #[test]fn render_allocators_use_distinct_uma_slots_within_firmware_window(){
        let mut o=Objects::new();
        let f=g16::object::Factory;
        let pools=PoolSet::new_with_slots(&mut o,&f,[7,8]).unwrap();
        assert_eq!(pools.unit(0).unwrap().id(),7);
        assert_eq!(pools.unit(1).unwrap().id(),8);
        let scs=pools.scs(&mut o,&f,g16::hw::sizes::FREELIST_SEED + 512).unwrap();
        assert_eq!(scs.unit(0).unwrap().id(),7);
        assert_eq!(scs.unit(1).unwrap().id(),8);
        assert_eq!(
            g16::pools::Allocator::new_at(
                &mut o,
                &f,
                g16::hw::sizes::MAX_VM_PB_SLOTS as u64,
                0,
                pools.heap(),
                g16::slots::Placement::Base
            ).err(),
            Some(EINVAL)
        );
        assert_eq!(
            g16::pools::Allocator::new_at(
                &mut o,
                &f,
                0,
                g16::hw::sizes::PB_SLOTS as u32,
                pools.heap(),
                g16::slots::Placement::Base
            ).err(),
            Some(EINVAL)
        );
        let mut uma_slots=Vec::new();
        for k in 0..g16::hw::sizes::PB_SLOTS {
            let local=(k % g16::hw::sizes::MAX_VM_PB_SLOTS) as u64;
            let alloc=g16::pools::Allocator::new_at(&mut o,&f,local,k as u32,pools.heap(),g16::slots::Placement::Base).unwrap();
            let refs=alloc.prepare_in_flight(1,false).unwrap();
            assert_eq!(refs.parameter_id,k as u32);
            let desc=find_va(refs.uma_mgr[0]);
            let slot_id=desc.r32(8).unwrap();
            let expected=g16::hw::engine::RENDER_UMA_SLOT+k as u32;
            assert_eq!(slot_id,expected);
            assert!(slot_id>=33 && slot_id<=64);
            assert!(!uma_slots.contains(&slot_id));
            uma_slots.push(slot_id);
            // Tampering with the descriptor UMA slot ID is rejected before publication.
            desc.write(8,&(expected+1).to_le_bytes()).unwrap();
            assert_eq!(alloc.prepare_in_flight(1,false).err(),Some(EINVAL));
        }
    }

    #[test]fn multi_active_pools_allow_warm_cross_vm_reuse_and_gate_cold_initbm(){
        let vm1=vm(1);let vm2=vm(2);let vm3=vm(3);
        let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let mk_epoch=|owner:&sync::Arc<g16::vm::VmState>,slot:usize|{
            let e=g16::pools::PbEpoch::new(&factory,owner,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
            e.prepare(slot,0x70c0000000,2048).unwrap();e.check_assignment(slot).unwrap();e.expose();
            e
        };
        let mut p=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        // Prime VM1 on slot 0 and VM2 on slot 1 (drained InitBM).
        let e1=mk_epoch(&vm1,0);
        assert_eq!(p.pick_slot_within(&vm1,false,2),Some(0));
        let b1=p.bind(0,false);p.publish_flight(0);p.retire_flight(0,&e1,b1,true).unwrap();
        let e2=mk_epoch(&vm2,1);
        assert_eq!(p.pick_slot_within(&vm2,false,2),Some(1));
        let b2=p.bind(1,false);p.publish_flight(1);p.retire_flight(1,&e2,b2,true).unwrap();
        // VM1 starts a render on warm slot 0 (no InitBM).
        assert_eq!(p.pick_slot_within(&vm1,false,2),Some(0));
        assert!(p.reusable(0,&vm1).is_some());
        let b1=p.bind(0,true);p.publish_flight(0);
        assert_eq!(p.slot_inflight(0),1);
        assert_eq!(p.slot_inflight(1),0);
        // While VM1 is live on slot 0 (behind=true):
        // - Warm VM2 pipelines onto its own slot 1 without InitBM or drain.
        // - Cold VM3 has no warm slot and returns None so the engine drains before InitBM.
        assert!(p.can_pipeline_within(&vm2,2));
        assert_eq!(p.pick_slot_within(&vm2,true,2),Some(1));
        assert!(!p.can_pipeline_within(&vm3,2));
        assert_eq!(p.pick_slot_within(&vm3,true,2),None);
        // VM2 publishes on slot 1 concurrently with VM1 on slot 0.
        let b2=p.bind(1,true);p.publish_flight(1);
        assert_eq!(p.slot_inflight(0),1);
        assert_eq!(p.slot_inflight(1),1);
        // Neither in-flight slot is displaced while live.
        assert!(!p.exhausted());
        // Both retire cleanly in order.
        p.retire_flight(0,&e1,b1,true).unwrap();
        assert_eq!(p.slot_inflight(0),0);
        assert_eq!(p.slot_inflight(1),1);
        p.retire_flight(1,&e2,b2,true).unwrap();
        assert_eq!(p.slot_inflight(1),0);
        // Recovery invalidation clears warm reuse for BOTH slots so both VMs drain for InitBM.
        p.invalidate();
        assert!(!p.can_pipeline_within(&vm1,2));
        assert!(!p.can_pipeline_within(&vm2,2));
    }

    #[test]fn render_hang_after_cross_vm_use_recovers_via_lazy_pb_slot_and_displaces_survivor(){
        let vm1=vm(1);let vm2=vm(2);let vm3=vm(3);let vm4=vm(4);
        let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let mk_epoch=|owner:&sync::Arc<g16::vm::VmState>,slot:usize|{
            let e=g16::pools::PbEpoch::new(&factory,owner,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
            e.prepare(slot,0x70c0000000,2048).unwrap();e.check_assignment(slot).unwrap();e.expose();
            e
        };
        let mut p=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        // Exact live cross-VM sequence: VM1 on slot 0 -> VM2 on slot 1 -> VM1 on slot 0.
        let e1=mk_epoch(&vm1,0);
        assert_eq!(p.pick_slot_within(&vm1,false,2),Some(0));
        let b=p.bind(0,false);p.publish_flight(0);p.retire_flight(0,&e1,b,true).unwrap();
        let e2=mk_epoch(&vm2,1);
        assert_eq!(p.pick_slot_within(&vm2,false,2),Some(1));
        let b=p.bind(1,false);p.publish_flight(1);p.retire_flight(1,&e2,b,true).unwrap();
        let e1b=mk_epoch(&vm1,0);
        assert_eq!(p.pick_slot_within(&vm1,false,2),Some(0));
        let b=p.bind(0,false);p.publish_flight(0);p.retire_flight(0,&e1b,b,true).unwrap();
        // VM3 switches to slot 1 with a fresh epoch and hangs: recovery invalidates epochs
        // and quarantines slot 1 while retaining BOTH the prior owner (e2) and fresh epoch (e3).
        let e3=mk_epoch(&vm3,1);
        assert_eq!(p.pick_slot_within(&vm3,false,2),Some(1));
        let _=p.bind(1,false);p.publish_flight(1);
        p.invalidate();
        p.quarantine_flight(1,Some(e3.clone()));
        // e2 is slot 1's prior owner and VM2's last retired epoch.
        assert_eq!(sync::Arc::strong_count(&e2),3,"prior slot owner epoch must stay mapped");
        assert_eq!(sync::Arc::strong_count(&e3),2,"aborted fresh epoch must stay mapped");
        // Render class must NOT wedge: slot 2 is fresh and needed_slots advances to 3.
        assert!(!p.exhausted());
        assert_eq!(p.needed_slots(),3);
        // Pre-hang job with only 2 prepared PB slots sees per-job exhaustion (no EAGAIN spin),
        // while a post-hang job with 3 prepared PB slots claims recovery slot 2.
        assert!(p.exhausted_for_within(&vm4,2));
        assert!(!p.exhausted_for_within(&vm4,3));
        assert_eq!(p.pick_slot_within(&vm4,false,3),Some(2));
        let e4=mk_epoch(&vm4,2);
        let b=p.bind(2,false);p.publish_flight(2);p.retire_flight(2,&e4,b,true).unwrap();
        // Clean retirement on slot 2 displaces surviving slot 0 back to reusable!
        assert!(!p.exhausted_for_within(&vm1,3));
        assert_eq!(p.pick_slot_within(&vm1,false,3),Some(0));
        let e1c=mk_epoch(&vm1,0);
        let b=p.bind(0,false);p.publish_flight(0);p.retire_flight(0,&e1c,b,true).unwrap();
        // Subsequent cross-VM render reuses displaced slot 2 (does not consume slot 3).
        assert_eq!(p.pick_slot_within(&vm2,false,3),Some(2));
        // An unrelated compute hang calling invalidate() does not wedge render either.
        p.invalidate();
        assert!(!p.exhausted());
        assert_eq!(p.pick_slot_within(&vm2,false,3),Some(2));
    }

    #[test]fn pb_recovery_slots_survive_four_render_hangs_and_exhaust_cleanly_at_limit(){
        let vm1=vm(1);let vm2=vm(2);let vm3=vm(3);
        let factory=g16::object::Factory;let budget=g16::budget::Budget::new().unwrap();
        let mk_epoch=|owner:&sync::Arc<g16::vm::VmState>,slot:usize|{
            let e=g16::pools::PbEpoch::new(&factory,owner,g16::budget::Budget::reserve(&budget,1).unwrap().take().unwrap()).unwrap();
            e.prepare(slot,0x70c0000000,2048).unwrap();e.check_assignment(slot).unwrap();e.expose();
            e
        };
        let warm=g16::hw::sizes::WARM_PB_SLOTS;
        let ext=g16::hw::sizes::EXT_PB_SLOTS;
        let total=g16::hw::sizes::PB_SLOTS;
        let mut p=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        // A VM bound to [0, 1] with all 4 recovery spares (16..20) survives 4 hangs cleanly
        // and keeps 2 healthy slots (18, 19) for cross-VM switching.
        let vm_mask:u32=(1u32<<0)|(1u32<<1)|(((1u32<<ext)-1)<<warm);
        for (i,expected) in [0usize,1,warm,warm+1].iter().enumerate() {
            let slot=p.pick_slot_in_mask_blocks_mode(&vm1,false,vm_mask,0,0,false).unwrap();
            assert_eq!(slot,*expected);
            p.publish_flight(slot);
            p.invalidate();
            p.quarantine_flight(slot,None);
            assert_ne!(p.quarantined_mask() & (1u32<<slot), 0);
            assert!(!p.exhausted_for_mask_mode(&vm1,vm_mask,0,false));
            assert_eq!(p.needed_slots(),(2+i+1).min(total));
        }
        // Cross-VM switching still works cleanly across surviving spares 18 and 19.
        let s_a=warm+2;let s_b=warm+3;
        assert_eq!(p.pick_slot_in_mask_blocks_mode(&vm1,false,vm_mask,0,0,false),Some(s_a));
        let ea=mk_epoch(&vm1,s_a);
        let b=p.bind(s_a,false);p.publish_flight(s_a);p.retire_flight(s_a,&ea,b,true).unwrap();
        assert_eq!(p.pick_slot_in_mask_blocks_mode(&vm2,false,vm_mask,0,0,false),Some(s_b));
        let eb=mk_epoch(&vm2,s_b);
        let b=p.bind(s_b,false);p.publish_flight(s_b);p.retire_flight(s_b,&eb,b,true).unwrap();
        assert_eq!(p.pick_slot_in_mask_blocks_mode(&vm1,false,vm_mask,0,0,false),Some(s_a));
        // 5th hang quarantines s_a: only s_b survives (reuse-only for VM2; VM1/VM3 exhausted).
        p.publish_flight(s_a);
        p.quarantine_flight(s_a,None);
        assert!(!p.exhausted_for_mask_mode(&vm2,vm_mask,0,false));
        assert_eq!(p.pick_slot_in_mask_blocks_mode(&vm2,false,vm_mask,0,0,false),Some(s_b));
        assert!(p.exhausted_for_mask_mode(&vm1,vm_mask,0,false));
        assert!(p.exhausted_for_mask_mode(&vm3,vm_mask,0,false));
        p.invalidate();
        assert!(p.exhausted_for_mask_mode(&vm2,vm_mask,0,false));

        // Quarantine all but the last global slot leaving slot (PB_SLOTS - 1) fresh:
        // VM2 claims and reuses it while other VMs report exhaustion until recovery.
        let mut q=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        for _ in 0..(total - 1) {
            let slot=q.pick_slot(&vm1,false).unwrap();
            q.publish_flight(slot);
            q.quarantine_flight(slot,None);
            assert!(!q.exhausted());
        }
        let last=total-1;
        assert_eq!(q.pick_slot(&vm2,false),Some(last));
        let qlast=mk_epoch(&vm2,last);
        let qb=q.bind(last,false);
        q.publish_flight(last);
        assert!(!q.exhausted_for(&vm3)); // in-flight: waits for drain first
        q.retire_flight(last,&qlast,qb,true).unwrap();
        assert!(!q.exhausted());
        assert!(!q.exhausted_for(&vm2));
        assert_eq!(q.pick_slot(&vm2,false),Some(last));
        assert!(q.exhausted_for(&vm1));
        assert!(q.exhausted_for(&vm3));
        // Recovery invalidates the last surviving slot's epoch -> global pool exhaustion.
        q.invalidate();
        assert!(q.exhausted());
        assert!(q.exhausted_for(&vm2));
    }

    #[test]fn recovery_invalidate_scrub_survives_in_flight_retirement(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let a=vm(1);
        // Prime unit 0 and unit 1 so unit 0 is clean for same-VM reuse.
        let mut c0=Claims::new();
        assert_eq!(s.claim_compute_for(&arena.compute,&mut c0,Some(a.id)).unwrap(),Some(0));
        s.hold_vm(&c0,&a).unwrap();s.publish(&mut c0).unwrap();
        // Recovery fires while unit 0 is still in flight.
        s.invalidate_scrub();
        s.retire(&mut c0).unwrap();
        // Displace unit 0 via unit 1 (published after recovery -> clean).
        let mut c1=Claims::new();
        assert_eq!(s.claim_compute_for(&arena.compute,&mut c1,Some(a.id)).unwrap(),Some(1));
        s.hold_vm(&c1,&a).unwrap();s.publish(&mut c1).unwrap();s.retire(&mut c1).unwrap();
        // Next claim of unit 0 by the same VM must still perform a full scrub!
        let k=s.claim_compute_for(&arena.compute,&mut c0,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),1);
        s.hold_vm(&c0,&a).unwrap();s.publish(&mut c0).unwrap();s.retire(&mut c0).unwrap();
        // Same invariant for render pass slots in flight across recovery.
        let mut r0=Claims::new();r0.pass[0]=0;r0.pass_mask=1;
        s.claim_pass_for(&arena.render,0,Some(a.id)).unwrap();
        s.hold_vm(&r0,&a).unwrap();s.publish(&mut r0).unwrap();
        s.invalidate_scrub();
        s.retire(&mut r0).unwrap();
        let mut r1=Claims::new();r1.pass[0]=1;r1.pass_mask=2;
        s.claim_pass_for(&arena.render,1,Some(a.id)).unwrap();
        s.hold_vm(&r1,&a).unwrap();s.publish(&mut r1).unwrap();s.retire(&mut r1).unwrap();
        s.claim_pass_for(&arena.render,0,Some(a.id)).unwrap();
        assert_eq!(arena.render.passes[0].scene.fills.lock().unwrap().len(),1);
    }

    #[test]fn distinct_vm_cross_vm_retarget_skips_private_wc_scrub_and_preserves_dirty_triggers(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let a_arenas=arenas(&mut o);let b_arenas=arenas(&mut o);
        let ra=sync::Arc::new(a_arenas.render,0).unwrap();
        let rb=sync::Arc::new(b_arenas.render,0).unwrap();
        let prep_a=g16::resources::Prepared{compute:a_arenas.compute.compute,render:Some(ra.clone()),hw_slots:None};
        let prep_b=g16::resources::Prepared{compute:b_arenas.compute.compute,render:Some(rb.clone()),hw_slots:None};
        let a=vm(1);let b=vm(2);
        // --- Compute: distinct per-VM ComputeSlot units skip 288 KiB private WC fills across
        // clean cross-VM retargets while keeping compute_needs_full_work = true. ---
        let mut c=Claims::new();
        assert_eq!(s.claim_compute_for(&prep_a,&mut c,Some(a.id)).unwrap(),Some(0));
        assert!(s.compute_needs_full_work(0));
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_a,&mut c,Some(a.id)).unwrap(),Some(1));
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        // VM B retargets unit 0 on its own private ComputeSlot: full_work = true, 0 private WC fills!
        let seq_before=prep_b.compute[0].seq.fills.lock().unwrap().len();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(0));
        assert!(s.compute_needs_full_work(0));
        assert_eq!(prep_b.compute[0].context_store.fills.lock().unwrap().len(),0);
        assert_eq!(prep_b.compute[0].aux.fills.lock().unwrap().len(),0);
        assert_eq!(prep_b.compute[0].preempt.fills.lock().unwrap().len(),0);
        assert_eq!(prep_b.compute[0].seq.fills.lock().unwrap().len(),seq_before);
        assert_eq!(prep_b.compute[0].its.fills.lock().unwrap().len(),1);
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        // Displace unit 0 via unit 1 (VM B), then VM A returns to unit 0: still 0 private WC fills!
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(1));
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_a,&mut c,Some(a.id)).unwrap(),Some(0));
        assert!(s.compute_needs_full_work(0));
        assert_eq!(prep_a.compute[0].context_store.fills.lock().unwrap().len(),0);
        assert_eq!(prep_a.compute[0].aux.fills.lock().unwrap().len(),0);
        assert_eq!(prep_a.compute[0].preempt.fills.lock().unwrap().len(),0);
        // Cancelled preparation on VM A marks VM A's private unit 0 dirty even if VM B runs cleanly
        // on hardware slot 0 in between.
        s.release(&mut c);
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(0));
        assert_eq!(prep_b.compute[0].context_store.fills.lock().unwrap().len(),0);
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(1));
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_a,&mut c,Some(a.id)).unwrap(),Some(0));
        assert_eq!(prep_a.compute[0].context_store.fills.lock().unwrap().len(),1);
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        // Affected error (`mark_dirty`) on VM A survives an intervening clean VM B flight.
        let mut md=Claims::new();md.slot[0]=0;md.slot_count=1;
        s.mark_dirty(&md);
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(1));
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(0));
        assert_eq!(prep_b.compute[0].context_store.fills.lock().unwrap().len(),0);
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(1));
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_a,&mut c,Some(a.id)).unwrap(),Some(0));
        assert_eq!(prep_a.compute[0].context_store.fills.lock().unwrap().len(),2);
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        // Recovery (`invalidate_scrub`) forces private WC scrub on VM A even after VM B clears
        // `scrub_full[0]` via a clean retirement!
        s.invalidate_scrub();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(1));
        assert_eq!(prep_b.compute[1].context_store.fills.lock().unwrap().len(),1);
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(0));
        assert_eq!(prep_b.compute[0].context_store.fills.lock().unwrap().len(),1);
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_b,&mut c,Some(b.id)).unwrap(),Some(1));
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_a,&mut c,Some(a.id)).unwrap(),Some(0));
        assert_eq!(prep_a.compute[0].context_store.fills.lock().unwrap().len(),3);
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();

        // --- Render: distinct per-VM PassSlots skip 420 KiB private WC fills across clean
        // cross-VM retargets while keeping render_needs_full_work = true and scrubbing each VM's
        // own tilemap/heap delta (never trusting the prior VM's `pre_cleaned` bit). ---
        let pub_ret=|s:&mut Slots,k:usize,vm:&sync::Arc<g16::vm::VmState>,prep:&g16::resources::Prepared|{
            let mut cr=Claims::new();cr.pass[0]=k as u32;cr.pass_mask=1u64<<k;
            s.hold_vm(&cr,vm).unwrap();s.hold_arenas(&cr,prep);s.publish(&mut cr).unwrap();s.retire(&mut cr).unwrap();
        };
        s.claim_pass_in(&prep_a,0,Some(a.id),0x4000).unwrap();
        prep_a.set_pass_dirty(0,0x2000,0x4000,0x200).unwrap();
        ra.passes[0].tilemap.fill(0,0xaa,0x4000).unwrap();
        pub_ret(&mut s,0,&a,&prep_a);
        s.claim_pass_in(&prep_a,1,Some(a.id),0x1000).unwrap();
        pub_ret(&mut s,1,&a,&prep_a);
        // VM B retargets pass slot 0 cleanly on its own PassSlot: 0 private bulk WC fills,
        // full_work = true, its cleared, and 0 tilemap/heap fills (fresh).
        s.claim_pass_in(&prep_b,0,Some(b.id),0x4000).unwrap();
        assert!(s.render_needs_full_work(0));
        assert_eq!(rb.passes[0].aux.fills.lock().unwrap().len(),0);
        assert_eq!(rb.passes[0].scene.fills.lock().unwrap().len(),0);
        assert_eq!(rb.passes[0].ta_scratch.fills.lock().unwrap().len(),0);
        assert_eq!(rb.passes[0].pre[0].fills.lock().unwrap().len(),0);
        assert_eq!(rb.passes[0].tilemap.fills.lock().unwrap().len(),0);
        assert_eq!(rb.passes[0].its.fills.lock().unwrap().len(),1);
        prep_b.set_pass_dirty(0,0x1000,0x2000,0x200).unwrap();
        rb.passes[0].tilemap.fill(0,0xbb,0x2000).unwrap();
        pub_ret(&mut s,0,&b,&prep_b);
        // Displace slot 0 with slot 1, then pre-scrub VM B's slot 0 off-lock (`clean_mask` bit 0 set).
        s.claim_pass_in(&prep_b,1,Some(b.id),0x1000).unwrap();
        pub_ret(&mut s,1,&b,&prep_b);
        s.stage_displaced_pass_scrubs();
        let _=drain_scrubs(s.take_staged_scrubs());
        // VM A returns to slot 0: MUST ignore VM B's `clean_mask` and zero VM A's own 0x4000
        // tilemap + 0x200 heap while STILL skipping aux/scene/scratch/pre/tpc!
        s.claim_pass_in(&prep_a,0,Some(a.id),0x4000).unwrap();
        assert!(s.render_needs_full_work(0));
        assert_eq!(ra.passes[0].aux.fills.lock().unwrap().len(),0);
        assert_eq!(ra.passes[0].scene.fills.lock().unwrap().len(),0);
        assert_eq!(ra.passes[0].tpc.fills.lock().unwrap().len(),0);
        assert_eq!(ra.passes[0].tilemap.fills.lock().unwrap().last(),Some(&(0,0x4000)));
        assert_eq!(ra.passes[0].heap.fills.lock().unwrap().last(),Some(&(0,0x200)));
        assert!(ra.passes[0].tilemap.snapshot()[..0x4000].iter().all(|&x|x==0));
        pub_ret(&mut s,0,&a,&prep_a);
        // Recovery invalidates VM B's pass slot 0 even if VM A cleans hardware slot 0 first.
        s.invalidate_scrub();
        s.claim_pass_in(&prep_a,1,Some(a.id),0x1000).unwrap();
        pub_ret(&mut s,1,&a,&prep_a);
        s.claim_pass_in(&prep_a,0,Some(a.id),0x1000).unwrap();
        assert_eq!(ra.passes[0].aux.fills.lock().unwrap().len(),1);
        pub_ret(&mut s,0,&a,&prep_a);
        s.claim_pass_in(&prep_a,1,Some(a.id),0x1000).unwrap();
        pub_ret(&mut s,1,&a,&prep_a);
        s.claim_pass_in(&prep_b,0,Some(b.id),0x2000).unwrap();
        assert_eq!(rb.passes[0].aux.fills.lock().unwrap().len(),1);
        assert_eq!(rb.passes[0].scene.fills.lock().unwrap().len(),1);
    }

    #[test]fn pipelined_render_pass_slots_keep_forty_seven_in_flight(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let mut passes=KVec::new();
        for k in 0..g16::hw::sizes::RENDER_SLOTS as u64 {
            let pass=g16::slots::PassSlot::new(&mut o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap();
            if k < g16::hw::sizes::BASE_RENDER_SLOTS as u64 {
                assert_eq!(pass.tpc.size as u64, g16::hw::client::TPC_SIZE);
                assert_eq!(pass.tilemap.size as u64, g16::hw::client::TILEMAP_SIZE);
            } else {
                assert_eq!(pass.tpc.size as u64, g16::hw::client::TPC_SLOT_SIZE);
                assert_eq!(pass.tilemap.size as u64, g16::hw::client::TILEMAP_SLOT_SIZE);
            }
            passes.push(pass,0).unwrap();
        }
        let arena=render_arena(passes);
        let a=vm(1);
        let b=vm(2);
        let mut live:Vec<Claims>=Vec::new();
        // Publish 47 concurrent passes in flight (slots 0..47), covering 4x11-pass and 6x7-pass
        // multi-pass bursts and exercising 64-bit pass_mask bits above bit 31.
        for k in 0..47usize {
            let mut c=Claims::new();
            c.pass[0]=k as u32;c.pass_mask=1u64<<k;
            s.claim_pass_for(&arena,k,Some(a.id)).unwrap();
            assert!(s.render_needs_full_work(k));
            s.hold_vm(&c,&a).unwrap();
            s.publish(&mut c).unwrap();
            live.push(c);
        }
        assert_eq!(s.render_published(),47);
        assert!(s.pass_available(48)); // slot 47 is still fresh
        assert!(!s.pass_available(47));
        // Retire 0 and 1 -> slot 0 is displaced and reusable while 45 others remain in flight.
        s.retire(&mut live[0]).unwrap();
        assert!(!s.pass_available(47)); // slot 0 not displaced yet
        s.retire(&mut live[1]).unwrap();
        assert!(s.pass_available(47)); // slot 0 displaced by slot 1!
        // Claim and publish bit 47 (slot 47) explicitly to prove zero 64-bit mask overflow.
        let mut c47=Claims::new();
        c47.pass[0]=47;c47.pass_count=1;c47.pass_mask=1u64<<47;
        s.claim_pass_for(&arena,47,Some(a.id)).unwrap();
        s.hold_vm(&c47,&a).unwrap();
        s.publish(&mut c47).unwrap();
        // Clean same-VM reuse of displaced slot 0 does not need full work write.
        s.claim_pass_for(&arena,0,Some(a.id)).unwrap();
        assert!(!s.render_needs_full_work(0));
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.release(&mut c0);
        // Cancelled preparation or cross-VM switch forces full work write again.
        s.claim_pass_for(&arena,0,Some(b.id)).unwrap();
        assert!(s.render_needs_full_work(0));
        s.release(&mut c0);
        for c in live.iter_mut().skip(2) {
            s.retire(c).unwrap();
        }
        s.retire(&mut c47).unwrap();
        assert_eq!(s.render_published(),0);
    }

    #[test]fn offlock_scrub_cleans_displaced_slot_and_skips_sync_scrub_on_same_vm_claim(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let render=sync::Arc::new(arena.render,0).unwrap();
        let prep=g16::resources::Prepared{compute:arena.compute.compute,render:Some(render.clone()),hw_slots:None};
        let a=vm(1);
        let geo=g16::encode::Geometry{tpc_bytes:0x2000,tilemap_bytes:0x4000};
        // Publish and retire slot 0: not displaced yet, so nothing is staged.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.claim_pass_in(&prep,0,Some(a.id),0x4000).unwrap();
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();
        render.set_dirty(0,0x2000,0x4000).unwrap();
        render.passes[0].tilemap.fill(0,0xab,0x4000).unwrap();
        render.passes[0].heap.fill(0,0xcd,0x200).unwrap();
        let its_before=render.passes[0].its.fills.lock().unwrap().len();
        let pre0_before=render.passes[0].pre[0].fills.lock().unwrap().len();
        s.retire(&mut c0).unwrap();
        s.stage_displaced_pass_scrubs();
        assert!(!s.scrubbing_active());
        assert!(!drain_scrubs(s.take_staged_scrubs()));
        // Publish and retire slot 1: displaces slot 0, staging slot 0 for off-lock scrub.
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();
        s.retire(&mut c1).unwrap();
        s.stage_displaced_pass_scrubs();
        assert!(s.scrubbing_active());
        // While slot 0 scrubs off-lock it cannot be picked or claimed, and waiter is recorded.
        assert!(!s.pass_available(2));
        assert_eq!(s.pick_pass_in(&prep,&geo,0),None);
        assert_eq!(s.claim_pass_in(&prep,0,Some(a.id),0x4000),Err(EBUSY));
        let batch=s.take_staged_scrubs();
        assert!(drain_scrubs(batch));
        assert!(!s.scrubbing_active());
        assert!(render.passes[0].tilemap.snapshot()[..0x4000].iter().all(|&b|b==0));
        assert!(render.passes[0].heap.snapshot()[..0x200].iter().all(|&b|b==0));
        assert_eq!(render.passes[0].its.fills.lock().unwrap().len(),its_before);
        assert_eq!(render.passes[0].pre[0].fills.lock().unwrap().len(),pre0_before);
        assert_eq!(render.tilemap_dirty[0].load(std::sync::atomic::Ordering::Acquire),0);
        assert_eq!(render.heap_dirty[0].load(std::sync::atomic::Ordering::Acquire),0);
        // Next same-VM claim picks pre-cleaned slot 0 and performs zero WC fills under lock.
        let fills_before=render.passes[0].tilemap.fills.lock().unwrap().len();
        let heap_before=render.passes[0].heap.fills.lock().unwrap().len();
        assert_eq!(s.pick_pass_in(&prep,&geo,0),Some(0));
        s.claim_pass_in(&prep,0,Some(a.id),0x4000).unwrap();
        assert!(!s.render_needs_full_work(0));
        assert_eq!(render.passes[0].tilemap.fills.lock().unwrap().len(),fills_before);
        assert_eq!(render.passes[0].heap.fills.lock().unwrap().len(),heap_before);
        assert_eq!(render.passes[0].its.fills.lock().unwrap().len(),its_before);
        assert_eq!(render.passes[0].pre[0].fills.lock().unwrap().len(),pre0_before);
    }

    #[test]fn stage_displaced_pass_scrubs_prioritizes_pass_cursor_order(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let mut passes=KVec::new();
        for k in 0..20 {
            passes.push(g16::slots::PassSlot::new(&mut o, k as u64, 1, &crate::g16::object::test_cmds((k as u64) as u64)).unwrap(), 0).unwrap();
        }
        let render=sync::Arc::new(render_arena(passes), 0).unwrap();
        let prep=g16::resources::Prepared{compute:KVec::new(),render:Some(render.clone()),hw_slots:None};
        let a=vm(1);
        // Publish and retire slots 0..20 in order so slots 0..19 are displaced dirty and
        // slot 19 is the undisplaced newest retired slot.
        for k in 0..20 {
            let mut c=Claims::new();c.pass[0]=k as u32;c.pass_count=1;c.pass_mask=1u64<<k;
            s.claim_pass_in(&prep,k,Some(a.id),0x2000).unwrap();
            s.hold_render_pass(k,&a,&prep).unwrap();
            s.publish(&mut c).unwrap();
            render.set_dirty(k,0x1000,0x2000).unwrap();
            render.passes[k].tilemap.fill(0,0x33,0x2000).unwrap();
            s.retire(&mut c).unwrap();
        }
        // Place pass_cursor at 11: the next pick_pass_in_for scan starts at 11, so the 8-slot
        // async scrub batch must prioritize displaced slots 11..19 instead of 0..8.
        s.pass_cursor = 11;
        s.stage_displaced_pass_scrubs();
        assert!(s.scrubbing_active());
        let _ = drain_scrubs(s.take_staged_scrubs());
        assert!(!s.scrubbing_active());
        for k in 11..19 {
            assert_eq!(render.tilemap_dirty[k].load(std::sync::atomic::Ordering::Acquire), 0, "cursor-upcoming slot {k} must be pre-scrubbed");
        }
        for k in 0..8 {
            assert_eq!(render.tilemap_dirty[k].load(std::sync::atomic::Ordering::Acquire), 0x2000, "distant slot {k} waits for later batch");
        }
        // Remaining displaced slots stay pending and scrub cleanly on subsequent calls.
        s.stage_displaced_pass_scrubs();
        assert!(s.scrubbing_active());
        let _ = drain_scrubs(s.take_staged_scrubs());
        for k in 0..8 {
            assert_eq!(render.tilemap_dirty[k].load(std::sync::atomic::Ordering::Acquire), 0, "remaining slot {k} scrubbed in next batch");
        }
    }

    #[test]fn offlock_scrub_falls_back_on_error_drop_recovery_and_cross_vm(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let render=sync::Arc::new(arena.render,0).unwrap();
        let prep=g16::resources::Prepared{compute:arena.compute.compute,render:Some(render.clone()),hw_slots:None};
        let a=vm(1);let b=vm(2);
        // Prime and displace slot 0.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();
        render.set_dirty(0,0x1000,0x2000).unwrap();
        render.passes[0].tilemap.fill(0,0x55,0x2000).unwrap();
        s.retire(&mut c0).unwrap();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();
        s.retire(&mut c1).unwrap();
        // Dropped batch clears scrubbing bit without marking clean.
        s.stage_displaced_pass_scrubs();
        assert!(s.scrubbing_active());
        drop(s.take_staged_scrubs());
        assert!(!s.scrubbing_active());
        // Failed off-lock scrub preserves tilemap_dirty and falls back synchronously.
        s.stage_displaced_pass_scrubs();
        render.passes[0].tilemap.fail_fill.store(true,std::sync::atomic::Ordering::Relaxed);
        assert!(!drain_scrubs(s.take_staged_scrubs()));
        assert_eq!(render.tilemap_dirty[0].load(std::sync::atomic::Ordering::Acquire),0x2000);
        let fills_before=render.passes[0].tilemap.fills.lock().unwrap().len();
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        assert!(render.passes[0].tilemap.fills.lock().unwrap().len()>fills_before);
        assert!(render.passes[0].tilemap.snapshot()[..0x2000].iter().all(|&b|b==0));
        // Cancel, re-displace, pre-scrub clean, then invalidate_scrub() or cross-VM forces full scrub.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();s.retire(&mut c0).unwrap();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();s.retire(&mut c1).unwrap();
        s.stage_displaced_pass_scrubs();
        let _=drain_scrubs(s.take_staged_scrubs());
        s.invalidate_scrub();
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        assert!(s.render_needs_full_work(0));
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();s.retire(&mut c0).unwrap();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();s.retire(&mut c1).unwrap();
        s.stage_displaced_pass_scrubs();
        let _=drain_scrubs(s.take_staged_scrubs());
        s.claim_pass_in(&prep,0,Some(b.id),0x2000).unwrap();
        assert!(s.render_needs_full_work(0));
    }
    #[test]fn post_publish_scrub_staging_does_not_block_immediate_slot_reuse(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let render=sync::Arc::new(arena.render,0).unwrap();
        let prep=g16::resources::Prepared{compute:arena.compute.compute,render:Some(render.clone()),hw_slots:None};
        let a=vm(1);
        let geo=g16::encode::Geometry{tpc_bytes:0x1000,tilemap_bytes:0x2000};
        // Prime slot 0, then displace it by retiring slot 1; record retirement without staging yet.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();
        render.set_dirty(0,0x1000,0x2000).unwrap();
        render.passes[0].tilemap.fill(0,0x77,0x2000).unwrap();
        s.retire(&mut c0).unwrap();
        s.note_render_retired();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();
        s.retire(&mut c1).unwrap();
        s.note_render_retired();
        // Immediate publication in the same lock hold sees slot 0 available and NOT scrubbing.
        assert!(!s.scrubbing_active());
        assert!(s.pass_available(2));
        assert_eq!(s.pick_pass_in(&prep,&geo,0),Some(0));
        // Disallowed staging (fail-stop / frozen) stages nothing and clears pending flag.
        assert!(!drain_scrubs(s.stage_and_take_scrubs(false)));
        assert!(!s.scrubbing_active());
        // Allowed staging after publication stages idle displaced slot 0 and scrubs it off-lock.
        s.note_render_retired();
        let batch=s.stage_and_take_scrubs(true);
        assert!(s.scrubbing_active());
        assert!(!drain_scrubs(batch));
        assert!(!s.scrubbing_active());
        assert!(render.passes[0].tilemap.snapshot()[..0x2000].iter().all(|&b|b==0));
        // Calling stage_and_take_scrubs again without a new render retirement is a no-op.
        assert!(!drain_scrubs(s.stage_and_take_scrubs(true)));
        assert!(!s.scrubbing_active());
    }
    #[test]fn multi_unit_compute_horizon_claims_publish_retire_and_release(){
        use crate::g16::{hw, slots::ComputeSlot, resources::ComputeArena, vm};
        use crate::sync::Arc;
        let mut o=Objects::new();
        let mut units=KVec::new();
        for k in 0..hw::sizes::COMPUTE_UNITS {
            units.push(ComputeSlot::new(&mut o,&g16::object::Factory,k as u64,&crate::g16::object::test_unit_cmds(k as u64)).unwrap(),0).unwrap();
        }
        let prep=ComputeArena{compute:units,render:None,hw_slots:None};
        let a=Arc::new(
            vm::VmState{id:1,slot:1,generation:1,vm:vm::MockVm::new()},0,
        ).unwrap();
        let mut s=Slots::new().unwrap();
        assert_eq!(s.free_compute_in(hw::sizes::COMPUTE_UNITS),hw::sizes::COMPUTE_UNITS as u32);
        // Claim 4 units into one Claims for a compute horizon, publish, and retire in order.
        let mut claims=Claims::new();
        for i in 0..4 {
            let k=s.claim_compute_for(&prep,&mut claims,Some(a.id)).unwrap().expect("free unit");
            assert_eq!(k,i);
            assert_eq!(claims.slot_count,(i+1) as u32);
            assert_eq!(claims.slot[i],k as u32);
        }
        assert_eq!(s.free_compute_in(hw::sizes::COMPUTE_UNITS),(hw::sizes::COMPUTE_UNITS-4) as u32);
        s.hold_vm(&claims,&a).unwrap();
        s.hold_arenas(&claims,&prep);
        s.publish(&mut claims).unwrap();
        assert_eq!(s.compute_published(),4);
        s.retire(&mut claims).unwrap();
        assert_eq!(s.compute_published(),0);
        // Unit 3 (the newest retired) holds the displacement slot; 15 units remain free.
        assert_eq!(s.free_compute_in(hw::sizes::COMPUTE_UNITS),(hw::sizes::COMPUTE_UNITS-1) as u32);
        // Claiming all remaining free units then releasing restores availability cleanly.
        let mut batch=Claims::new();
        while s.claim_compute_for(&prep,&mut batch,Some(a.id)).unwrap().is_some() {}
        assert_eq!(batch.slot_count as usize,hw::sizes::COMPUTE_UNITS-1);
        assert_eq!(s.free_compute_in(hw::sizes::COMPUTE_UNITS),0);
        s.release(&mut batch);
        assert_eq!(s.free_compute_in(hw::sizes::COMPUTE_UNITS),(hw::sizes::COMPUTE_UNITS-1) as u32);
    }

    #[test]fn multi_pass_render_batch_claims_ordered_retirement_and_publish_batch(){
        use crate::g16::pools::PoolSet;
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let mut passes=KVec::new();
        for k in 0..g16::hw::sizes::RENDER_SLOTS as u64 {
            passes.push(g16::slots::PassSlot::new(&mut o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap(),0).unwrap();
        }
        let render=sync::Arc::new(render_arena(passes),0).unwrap();
        let prep=g16::resources::Prepared{compute:arena.compute.compute,render:Some(render),hw_slots:None};
        let a=vm(1);
        // Wrapped submission order [46, 47, 0]: retiring in order leaves slot 0 as last_retired
        // while high slots 46 and 47 (bits above 31) are displaced and reusable immediately.
        let order=[46usize,47,0];
        let mut c=Claims::new();
        for (i,&k) in order.iter().enumerate() {
            s.claim_pass_in(&prep,k,Some(a.id),0x1000).unwrap();
            c.pass[i]=k as u32;
            c.pass_count+=1;
            c.pass_mask|=1u64<<k;
            s.pass_busy|=1u64<<k;
        }
        // Mismatched pass_mask or duplicate slot is rejected by hold_vm.
        let mut bad=c;
        bad.pass_mask^=1u64<<1;
        assert_eq!(s.hold_vm(&bad,&a),Err(EINVAL));
        let mut dup=c;
        dup.pass[2]=46;
        assert_eq!(s.hold_vm(&dup,&a),Err(EINVAL));

        s.hold_vm(&c,&a).unwrap();
        s.hold_arenas(&c,&prep);
        s.publish(&mut c).unwrap();
        assert_eq!(s.render_published(),3);
        s.retire(&mut c).unwrap();
        assert_eq!(s.render_published(),0);
        assert_eq!(c.pass_count,0);
        assert_eq!(c.pass_mask,0);
        assert_eq!(s.pass_busy,0);
        // Slots 46 and 47 were displaced by slot 0 (newest), so 46 & 47 are claimable while 0 is held.
        assert_eq!(s.claim_pass_in(&prep,0,Some(a.id),0x1000),Err(EBUSY));
        s.claim_pass_in(&prep,46,Some(a.id),0x1000).unwrap();
        s.claim_pass_in(&prep,47,Some(a.id),0x1000).unwrap();
        let mut cancel=Claims::new();
        cancel.pass[0]=46;cancel.pass[1]=47;cancel.pass_count=2;cancel.pass_mask=(1u64<<46)|(1u64<<47);
        s.pass_busy|=cancel.pass_mask;
        s.release(&mut cancel);
        assert_eq!(cancel.pass_count,0);
        assert_eq!(s.pass_busy,0);
        s.claim_pass_in(&prep,46,Some(a.id),0x1000).unwrap();
        let mut cancel2=Claims::new();
        cancel2.pass[0]=46;cancel2.pass_count=1;cancel2.pass_mask=1u64<<46;
        s.release(&mut cancel2);

        // PoolSet::publish_batch advances submitted by 2 * renders and rejects invalid deltas.
        let pools=PoolSet::new(&mut o,&g16::object::Factory).unwrap();
        assert_eq!(pools.publish_batch(0,0,0),Err(EINVAL));
        assert_eq!(pools.publish_batch(0,4,3),Err(EINVAL));
        assert_eq!(pools.publish_batch(0,6,3),Ok(()));
        assert_eq!(pools.publish_batch(0,10,3),Err(EINVAL));
        assert_eq!(pools.publish_batch(0,12,3),Ok(()));
        assert!(!pools.retired_at(0,6).unwrap());
    }

    #[test]fn per_unit_hold_and_verify_held_match_batch_and_reject_unheld_slots(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let mut passes=KVec::new();
        for k in 0..g16::hw::sizes::RENDER_SLOTS as u64 {
            passes.push(g16::slots::PassSlot::new(&mut o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap(),0).unwrap();
        }
        let render=sync::Arc::new(render_arena(passes),0).unwrap();
        let prep=g16::resources::Prepared{compute:arena.compute.compute,render:Some(render),hw_slots:None};
        let a=vm(1);let b=vm(2);

        // Per-unit compute hold_compute_slot + verify_held + note_published.
        let mut cc=Claims::new();
        let k0=s.claim_compute_for(&prep,&mut cc,Some(a.id)).unwrap().unwrap();
        assert_eq!(s.verify_held(&cc),Err(EINVAL));
        assert_eq!(s.hold_compute_slot(g16::hw::sizes::COMPUTE_UNITS,&a,&prep),Err(EINVAL));
        s.hold_compute_slot(k0,&a,&prep).unwrap();
        s.verify_held(&cc).unwrap();
        s.publish(&mut cc).unwrap();
        s.note_published(&cc);
        s.retire(&mut cc).unwrap();

        // High render pass slots (46, 47) held individually via hold_render_pass.
        let mut cr=Claims::new();
        for (i,&k) in [46usize,47].iter().enumerate() {
            s.claim_pass_in(&prep,k,Some(a.id),0x1000).unwrap();
            cr.pass[i]=k as u32;
            cr.pass_count+=1;
            cr.pass_mask|=1u64<<k;
            s.pass_busy|=1u64<<k;
            assert_eq!(s.verify_held(&cr),Err(EINVAL));
            s.hold_render_pass(k,&a,&prep).unwrap();
            s.verify_held(&cr).unwrap();
        }
        assert_eq!(s.hold_render_pass(g16::hw::sizes::RENDER_SLOTS,&a,&prep),Err(EINVAL));
        let mut bad=cr;
        bad.pass_mask^=1u64<<1;
        assert_eq!(s.verify_held(&bad),Err(EINVAL));
        let mut dup=cr;
        dup.pass[1]=46;
        assert_eq!(s.verify_held(&dup),Err(EINVAL));
        s.publish(&mut cr).unwrap();
        s.note_published(&cr);
        s.retire(&mut cr).unwrap();

        // Cross-VM reuse of displaced slot 46 records retarget via bit-set note_published.
        let mut cr2=Claims::new();
        s.claim_pass_in(&prep,46,Some(b.id),0x1000).unwrap();
        cr2.pass[0]=46;cr2.pass_count=1;cr2.pass_mask=1u64<<46;s.pass_busy|=1u64<<46;
        s.hold_render_pass(46,&b,&prep).unwrap();
        s.verify_held(&cr2).unwrap();
        s.publish(&mut cr2).unwrap();
        s.note_published(&cr2);
        assert_eq!(s.stats.cross_vm_retargets[1],1);
        assert_eq!(s.stats.last_retarget[1].unit,46);
        assert_eq!(s.stats.last_retarget[1].old_vm,1);
        assert_eq!(s.stats.last_retarget[1].new_vm,2);
        s.retire(&mut cr2).unwrap();
    }

    #[test]fn render_ext_tiers_grow_on_demand_and_keep_shallow_clients_bounded(){
        use crate::g16::resources::{BASE_RENDER_SLOTS, EXT_RENDER_TIERS, target_render_slots};
        let tier0_end = EXT_RENDER_TIERS[0].1;
        let tier1_end = EXT_RENDER_TIERS[1].1;
        let tier2_end = EXT_RENDER_TIERS[2].1;
        let full_end = EXT_RENDER_TIERS[3].1;
        assert_eq!((BASE_RENDER_SLOTS, tier0_end, tier1_end, tier2_end, full_end), (2, 8, 18, 32, 48));

        // Steady 60 fps (16.67 ms) and 120 fps (8.33 ms) single-pass depth-1 clients
        // finish each frame before the next arrives (active_passes == 0) and stay on
        // the two base pass slots without allocating any RenderExtArena tier.
        for period_ns in [16_666_667u64, 8_333_333u64] {
            let mut last = 0u64;
            let mut burst = 0u32;
            for step in 1u64..=10 {
                assert_eq!(
                    target_render_slots(&mut last, &mut burst, step * period_ns, 1, 0, false, BASE_RENDER_SLOTS),
                    BASE_RENDER_SLOTS
                );
                assert_eq!(burst, 0);
            }
        }

        // A 2-pass compositor or shallow depth-2 overlap grows ONLY to Tier 0 (8 slots),
        // never allocating Tier 1 or Tier 2 until live concurrency actually requires them.
        let mut last = 0u64;
        let mut burst = 0u32;
        assert_eq!(
            target_render_slots(&mut last, &mut burst, 100_000_000, 2, 0, false, BASE_RENDER_SLOTS),
            tier0_end
        );
        // Back-to-back 2-pass frames with 2 active passes (need = 2 + 2 + 1 = 5 <= 8) stay on Tier 0.
        assert_eq!(
            target_render_slots(&mut last, &mut burst, 108_333_333, 2, 2, false, tier0_end),
            tier0_end
        );

        // Heavy multi-ms pipelined renders (5 ms and 16.6 ms apart) overlap an already-active
        // pass and grow progressively: Tier 0 -> Tier 1 -> Tier 2 only as depth climbs.
        let mut last = 0u64;
        let mut burst = 0u32;
        let mut backed = BASE_RENDER_SLOTS;
        backed = target_render_slots(&mut last, &mut burst, 1_000_000, 1, 0, false, backed);
        assert_eq!(backed, BASE_RENDER_SLOTS);
        backed = target_render_slots(&mut last, &mut burst, 6_000_000, 1, 1, false, backed);
        assert_eq!(backed, tier0_end);
        backed = target_render_slots(&mut last, &mut burst, 22_600_000, 1, 6, false, backed);
        assert_eq!(backed, tier0_end); // 6 + 1 + 1 == 8 fits in Tier 0
        backed = target_render_slots(&mut last, &mut burst, 39_200_000, 1, 7, false, backed);
        assert_eq!(backed, tier1_end); // 7 + 1 + 1 == 9 grows to Tier 1 (18 slots)
        backed = target_render_slots(&mut last, &mut burst, 55_800_000, 1, 16, false, backed);
        assert_eq!(backed, tier1_end); // 16 + 1 + 1 == 18 fits in Tier 1
        backed = target_render_slots(&mut last, &mut burst, 72_400_000, 1, 17, false, backed);
        assert_eq!(backed, tier2_end); // 17 + 1 + 1 == 19 grows to Tier 2 (32 slots)
        backed = target_render_slots(&mut last, &mut burst, 89_000_000, 1, 30, false, backed);
        assert_eq!(backed, tier2_end); // 30 + 1 + 1 == 32 fits in Tier 2
        backed = target_render_slots(&mut last, &mut burst, 105_600_000, 1, 31, false, backed);
        assert_eq!(backed, full_end); // 31 + 1 + 1 == 33 grows to Tier 3 (48 slots)

        // Sub-2 ms back-to-back single-pass bursts grow only to Tier 0 after 3 consecutive gaps.
        let mut last = 0u64;
        let mut burst = 0u32;
        assert_eq!(target_render_slots(&mut last, &mut burst, 1_000_000, 1, 0, false, BASE_RENDER_SLOTS), BASE_RENDER_SLOTS);
        assert_eq!(target_render_slots(&mut last, &mut burst, 2_000_000, 1, 0, false, BASE_RENDER_SLOTS), BASE_RENDER_SLOTS);
        assert_eq!(target_render_slots(&mut last, &mut burst, 3_000_000, 1, 0, false, BASE_RENDER_SLOTS), BASE_RENDER_SLOTS);
        assert_eq!(target_render_slots(&mut last, &mut burst, 4_000_000, 1, 0, false, BASE_RENDER_SLOTS), tier0_end);
        assert_eq!(burst, 3);
        assert_eq!(target_render_slots(&mut last, &mut burst, 5_000_000, 1, 0, false, tier0_end), tier0_end);

        // Multi-pass pipelined workloads stop at Tier 2 (32 slots) for 20-31 pass demand and
        // grow into Tier 3 (48 slots) only when live demand exceeds 32 slots:
        // 11-pass render.postfx: depth 2 (23) -> Tier 2, depth 3 (34) -> Tier 3;
        // 7-pass compress.bw array: depth 3 (22) -> Tier 2, depth 5 (36) -> Tier 3;
        // 4-pass render.shadow: depth 5 (21) -> Tier 2, depth 8 (33) -> Tier 3.
        let mut last = 0u64;
        let mut burst = 0u32;
        assert_eq!(target_render_slots(&mut last, &mut burst, 10_000_000, 11, 0, false, BASE_RENDER_SLOTS), tier1_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 15_000_000, 11, 11, false, tier1_end), tier2_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 18_000_000, 11, 22, false, tier2_end), full_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 20_000_000, 7, 7, false, tier1_end), tier1_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 25_000_000, 7, 14, false, tier1_end), tier2_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 28_000_000, 7, 28, false, tier2_end), full_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 30_000_000, 4, 12, false, tier1_end), tier1_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 35_000_000, 4, 16, false, tier1_end), tier2_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 38_000_000, 4, 28, false, tier2_end), full_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 40_000_000, 8, 0, false, BASE_RENDER_SLOTS), tier1_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 50_000_000, 20, 0, false, BASE_RENDER_SLOTS), tier2_end);
        assert_eq!(target_render_slots(&mut last, &mut burst, 60_000_000, 32, 0, false, BASE_RENDER_SLOTS), full_end);

        // Verify exact per-slot mapped backing bytes for Base vs Ext slots.
        let mut o_base = Objects::new();
        g16::slots::PassSlot::new(&mut o_base, 0, 1, &crate::g16::object::test_cmds((0) as u64)).unwrap();
        let base_slot_bytes: usize = o_base.objects.iter().map(|obj| obj.mapped).sum();
        // Microsequences and barrier records are the device-lifetime records of the index.
        assert_eq!(base_slot_bytes, (16 * 1024 + 8 * 1024 + 448) * 1024);
        let mut o_ext = Objects::new();
        g16::slots::PassSlot::new(&mut o_ext, 2, 1, &crate::g16::object::test_cmds((2) as u64)).unwrap();
        let ext_slot_bytes: usize = o_ext.objects.iter().map(|obj| obj.mapped).sum();
        assert_eq!(ext_slot_bytes, (2 * 1024 + 1024 + 448) * 1024);
        // Each index's device-lifetime records: two work image pages, two microsequence pages
        // and one barrier page.
        let indices = crate::g16::object::PASS_CMDS.with(|p| p.borrow().len());
        let device_bytes: usize = crate::g16::object::PERSISTENT
            .with(|p| p.borrow().as_ref().unwrap().objects.iter().map(|obj| obj.mapped).sum());
        assert_eq!((indices, device_bytes), (2, 2 * 5 * 16 * 1024));
        // Cumulative extension backing across the 4 demand-grown tiers:
        // Tier 0 (6 ext): 20.625 MiB, Tier 1 (16 ext): 55.00 MiB,
        // Tier 2 (30 ext): 103.125 MiB, Tier 3 (46 ext): 158.125 MiB.
        assert_eq!((tier0_end - BASE_RENDER_SLOTS) * ext_slot_bytes, 21_120 * 1024);
        assert_eq!((tier1_end - BASE_RENDER_SLOTS) * ext_slot_bytes, 56_320 * 1024);
        assert_eq!((tier2_end - BASE_RENDER_SLOTS) * ext_slot_bytes, 105_600 * 1024);
        assert_eq!((full_end - BASE_RENDER_SLOTS) * ext_slot_bytes, 161_920 * 1024);
    }

    #[test]fn base_compute_quarantine_expands_fresh_vm_and_recovers_displacement(){
        use crate::g16::resources::{
            compute_home_start, pick_compute_units, target_compute_units, usable_compute_units,
        };
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let mut units=KVec::new();
        for k in 0..4u64 {
            units.push(g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,k,&crate::g16::object::test_unit_cmds(k)).unwrap(),0).unwrap();
        }
        let prep_full=g16::resources::ComputeArena{compute:units,render:None,hw_slots:None};
        let a=vm(1);let b=vm(2);
        // Retire base unit 0, then hang and quarantine base unit 1 (or vice versa):
        // unit 0 is now retired-undisplaced and unit 1 is quarantined -> base [0..2] exhausted.
        let mut c=Claims::new();
        assert_eq!(s.claim_compute_for(&prep_full,&mut c,Some(a.id)).unwrap(),Some(0));
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.claim_compute_for(&prep_full,&mut c,Some(a.id)).unwrap(),Some(1));
        s.hold_vm(&c,&a).unwrap();s.publish(&mut c).unwrap();s.quarantine(&mut c);
        assert!(s.base_compute_quarantined());
        assert!(s.compute_exhausted_within(2));
        assert!(!s.compute_exhausted_within(4));
        // Fresh single-command pipe-0 VM skips quarantined unit 1 and selects healthy [0, 2]
        // (matching dynamic pipe0_exclusive_mask), while an existing [0, 1] VM replaces unit 1
        // with unit 2 immediately without allocating all 30 extension units.
        let home = compute_home_start(0, g16::hw::sizes::COMPUTE_UNITS);
        assert_eq!(home, 0);
        let q = s.quarantined_compute_mask();
        let p0 = s.pipe0_exclusive_mask();
        let fresh_mask = pick_compute_units(home, 0, q, p0, 0, 2, g16::hw::sizes::COMPUTE_UNITS);
        assert_eq!(fresh_mask, (1u32 << 0) | (1u32 << 2));
        let usable = usable_compute_units(home, 0b11, q, p0, g16::hw::sizes::COMPUTE_UNITS);
        assert_eq!(usable, 1);
        let target = target_compute_units(0, 1, usable, 1, g16::hw::sizes::COMPUTE_UNITS);
        assert_eq!(target, 2);
        let add = pick_compute_units(home, 0b11, q, p0, 0, target, g16::hw::sizes::COMPUTE_UNITS);
        assert_eq!(add, 1u32 << 2);
        // Expanded fresh VM claims unit 2, retires cleanly, and displaces base unit 0 back to reusable.
        assert_eq!(s.claim_compute_for(&prep_full,&mut c,Some(b.id)).unwrap(),Some(2));
        s.hold_vm(&c,&b).unwrap();s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        assert!(!s.compute_exhausted_within(2));
        assert!(s.compute_available(2));
        // A base-only (2-unit) VM can now claim displaced base unit 0 again.
        let mut base_units=KVec::new();
        for k in 0..2u64 {
            base_units.push(g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,k,&crate::g16::object::test_unit_cmds(k)).unwrap(),0).unwrap();
        }
        let prep_base=g16::resources::ComputeArena{compute:base_units,render:None,hw_slots:None};
        assert_eq!(s.claim_compute_for(&prep_base,&mut c,Some(b.id)).unwrap(),Some(0));
        s.release(&mut c);
    }

    #[test]fn sparse_compute_alloc_disjoint_multi_vm_and_quarantine_promotion(){
        use crate::g16::resources::{
            compute_home_start, pick_compute_units, target_compute_units, usable_compute_units,
            BASE_COMPUTE_UNITS, COMPUTE_TOTAL_UNIT_BUDGET,
        };
        let max_u = g16::hw::sizes::COMPUTE_UNITS;
        let p0 = (1u32 << g16::hw::sizes::PIPE1_COMPUTE_START) - 1;
        // 16 concurrent depth-1 VMs (compute.light procs d16): pairwise-disjoint 2-unit pairs,
        // 32 total units (~1.03 GiB instead of 16 * 32 = 512 units / 16.5 GiB).
        let mut union = 0u32;
        let mut total_d1 = 0u32;
        for seq in 0..16usize {
            let home = compute_home_start(seq, max_u);
            let target = target_compute_units(0, 1, 0, 16, max_u);
            assert_eq!(target, BASE_COMPUTE_UNITS);
            let mask = pick_compute_units(home, 0, 0, p0, 0, target, max_u);
            assert_eq!(mask.count_ones() as usize, BASE_COMPUTE_UNITS);
            assert_eq!(union & mask, 0, "VM {seq} collided with earlier VM pair");
            if seq == 0 {
                assert_eq!(mask, p0);
            } else {
                assert_eq!(mask & p0, 0, "secondary VM {seq} must stay outside pipe0_exclusive");
            }
            union |= mask;
            total_d1 += mask.count_ones();
        }
        assert_eq!(union, u32::MAX);
        assert_eq!(total_d1 as usize, max_u);
        // 16 concurrent deep-pipelined VMs (16 x d16 worst case) stay within 64 units (~2.06 GiB).
        let mut total_deep16 = 0usize;
        for seq in 0..16usize {
            let home = compute_home_start(seq, max_u);
            let base = pick_compute_units(home, 0, 0, p0, 0, BASE_COMPUTE_UNITS, max_u);
            let usable = usable_compute_units(home, base, 0, p0, max_u);
            let target = target_compute_units(15, 1, usable, 16, max_u);
            assert_eq!(target, 4);
            let add = pick_compute_units(home, base, 0, p0, 0, target, max_u);
            let full = base | add;
            assert_eq!(full.count_ones() as usize, 4);
            total_deep16 += full.count_ones() as usize;
        }
        assert!(total_deep16 <= COMPUTE_TOTAL_UNIT_BUDGET);
        // Power-of-two concurrency (2, 4, 8 VMs) stays pairwise-disjoint even when grown to
        // full per-VM hardware share (16, 8, 4 units respectively).
        for (n_vms, each) in [(2usize, 16usize), (4, 8), (8, 4)] {
            let mut seen = 0u32;
            for seq in 0..n_vms {
                let home = compute_home_start(seq, max_u);
                let mask = pick_compute_units(home, 0, 0, p0, 0, each, max_u);
                assert_eq!(mask.count_ones() as usize, each);
                assert_eq!(seen & mask, 0, "{n_vms} VMs overlap at seq={seq}");
                seen |= mask;
            }
            assert_eq!(seen, u32::MAX);
        }
        // Single VM scales on demand with 2x headroom so single-pipe d16 keeps >= 17 reusable
        // units even when 15 other idle pipes hold retired tails: d1=2, d4=8, d16=32 (21 with 3 VMs).
        assert_eq!(target_compute_units(0, 1, 2, 1, max_u), 2);
        assert_eq!(target_compute_units(3, 1, 2, 1, max_u), 8);
        assert_eq!(target_compute_units(15, 1, 8, 1, max_u), 32);
        assert_eq!(target_compute_units(15, 1, 8, 3, max_u), 21);
        // Secondary VM self-heals if one of its units is promoted into pipe0_exclusive_mask.
        let home_u2 = compute_home_start(8, max_u);
        assert_eq!(home_u2, 2);
        let init_u2 = pick_compute_units(home_u2, 0, 0, p0, 0, 2, max_u);
        assert_eq!(init_u2, (1u32 << 2) | (1u32 << 3));
        let q_u0 = 1u32 << 0;
        let p0_promoted = (1u32 << 1) | (1u32 << 2);
        let usable_after = usable_compute_units(home_u2, init_u2, q_u0, p0_promoted, max_u);
        assert_eq!(usable_after, 1);
        let target_after = target_compute_units(0, 1, usable_after, 16, max_u);
        assert_eq!(target_after, 2);
        let add_after = pick_compute_units(home_u2, init_u2, q_u0, p0_promoted, 0, target_after, max_u);
        assert_eq!(add_after, 1u32 << 4);
        assert_eq!(
            usable_compute_units(home_u2, init_u2 | add_after, q_u0, p0_promoted, max_u),
            2
        );
        // Fresh VM skips a stranded pair where both units are pinned tails, and an existing VM
        // whose owned pair became stranded adds one immediately available non-tail unit.
        let stranded_pair = (1u32 << 16) | (1u32 << 17);
        let fresh_skip = pick_compute_units(16, 0, 0, p0, stranded_pair, 2, max_u);
        assert_eq!(fresh_skip, (1u32 << 18) | (1u32 << 19));
        let heal_add = pick_compute_units(16, stranded_pair, 0, p0, stranded_pair, 2, max_u);
        assert_eq!(heal_add.count_ones(), 1);
        assert_eq!(heal_add & stranded_pair, 0);
        assert_eq!(heal_add & p0, 0);
        // Sole active compute VM resets the home cursor back to pipe 0 ([0, 1]) after multi-VM
        // traffic (procs d16 + procs d8) so clight d16 starts on pipe 0 with all 16 units.
        let state = std::sync::atomic::AtomicU64::new(24);
        assert_eq!(g16::resources::reserve_compute_vm_home(&state), (1, 0));
        assert_eq!(state.load(std::sync::atomic::Ordering::Acquire), (1u64 << 32) | 1);
        assert_eq!(compute_home_start(0, max_u), 0);
        assert_eq!(g16::resources::reserve_compute_vm_home(&state), (2, 1));
        assert_eq!(compute_home_start(1, max_u), 16);
    }

    #[test]fn concurrent_cold_start_compute_vms_reserve_disjoint_home_pairs_and_rollback(){
        use std::sync::{Arc as StdArc, Barrier};
        use g16::resources::{
            ComputeVmReservation, active_compute_vm_count, compute_home_start,
            release_compute_vm_slot, reserve_compute_vm_home,
        };

        let max_u = g16::hw::sizes::COMPUTE_UNITS;

        // 1. Sixteen VMs racing from idle (cursor = 24 left by earlier multi-proc traffic) before
        // any ComputeUnitTable::with_added() finishes must reserve distinct home sequences 0..16
        // across all 16 pairs without collapsing onto pipe 0 or resetting next_slot back to 1.
        let shared = StdArc::new(std::sync::atomic::AtomicU64::new(24));
        let barrier = StdArc::new(Barrier::new(16));
        let mut handles = Vec::with_capacity(16);
        for _ in 0..16 {
            let st = shared.clone();
            let bar = barrier.clone();
            handles.push(std::thread::spawn(move || {
                bar.wait();
                let (active, seq) = reserve_compute_vm_home(&st);
                (active, seq, compute_home_start(seq, max_u))
            }));
        }
        let mut seqs = Vec::with_capacity(16);
        let mut homes = Vec::with_capacity(16);
        for h in handles {
            let (active, seq, home) = h.join().unwrap();
            assert!((1..=16).contains(&active));
            seqs.push(seq);
            homes.push(home);
        }
        seqs.sort_unstable();
        homes.sort_unstable();
        assert_eq!(seqs, (0..16).collect::<Vec<_>>());
        assert_eq!(homes, (0..16).map(|p| p * 2).collect::<Vec<_>>());
        assert_eq!(active_compute_vm_count(&shared), 16);
        assert_eq!(shared.load(std::sync::atomic::Ordering::Acquire) as u32, 16);
        for _ in 0..16 {
            release_compute_vm_slot(&shared);
        }
        assert_eq!(active_compute_vm_count(&shared), 0);
        assert_eq!(shared.load(std::sync::atomic::Ordering::Acquire) as u32, 16);
        // Next sole VM after idle resets cursor cleanly back to pipe 0 (seq = 0, cursor = 1).
        assert_eq!(reserve_compute_vm_home(&shared), (1, 0));
        assert_eq!(shared.load(std::sync::atomic::Ordering::Acquire), (1u64 << 32) | 1);
        release_compute_vm_slot(&shared);
        assert_eq!(active_compute_vm_count(&shared), 0);

        // 2. Rollback on allocation failure: dropping an uncommitted ComputeVmReservation
        // decrements active_vms back to 0 so the next VM sees active == 0 -> seq = 0, and a
        // mid-burst failure decrements active_vms without clobbering the cursor or live VMs.
        let state = crate::sync::Arc::new(std::sync::atomic::AtomicU64::new(24), 0).unwrap();
        {
            let (res_fail, active, seq) = ComputeVmReservation::acquire(&state);
            assert_eq!((active, seq), (1, 0));
            assert_eq!(active_compute_vm_count(&state), 1);
            drop(res_fail);
        }
        assert_eq!(active_compute_vm_count(&state), 0);
        let (res_a, active_a, seq_a) = ComputeVmReservation::acquire(&state);
        assert_eq!((active_a, seq_a), (1, 0));
        let counter_a = res_a.commit().unwrap();
        {
            let (res_b_fail, active_b, seq_b) = ComputeVmReservation::acquire(&state);
            assert_eq!((active_b, seq_b), (2, 1));
            assert_eq!(active_compute_vm_count(&state), 2);
            drop(res_b_fail);
        }
        assert_eq!(active_compute_vm_count(&state), 1);
        assert_eq!(state.load(std::sync::atomic::Ordering::Acquire) as u32, 2);
        let (res_c, active_c, seq_c) = ComputeVmReservation::acquire(&state);
        assert_eq!((active_c, seq_c), (2, 2));
        let counter_c = res_c.commit().unwrap();
        release_compute_vm_slot(&counter_a);
        release_compute_vm_slot(&counter_c);
        assert_eq!(active_compute_vm_count(&state), 0);

        // 3. Low-32 cursor wrap must not carry into or borrow from high-32 active_vms.
        let wrap = std::sync::atomic::AtomicU64::new((1u64 << 32) | u64::from(u32::MAX));
        assert_eq!(reserve_compute_vm_home(&wrap), (2, u32::MAX as usize));
        assert_eq!(wrap.load(std::sync::atomic::Ordering::Acquire), 2u64 << 32);
        release_compute_vm_slot(&wrap);
        assert_eq!(wrap.load(std::sync::atomic::Ordering::Acquire), 1u64 << 32);
    }

    #[test]fn sparse_compute_admission_and_queued_waiter_prevent_pair_borrow_stranding(){
        use g16::resources::{ComputeArena, pick_compute_units_for};
        use g16::slots::ComputeSlot;

        let mut o = Objects::new();
        let max_u = g16::hw::sizes::COMPUTE_UNITS;
        let p0 = (1u32 << g16::hw::sizes::PIPE1_COMPUTE_START) - 1;
        let mut full_units = KVec::new();
        for k in 0..max_u as u64 {
            full_units
                .push(
                    ComputeSlot::new(&mut o, &g16::object::Factory, k, &crate::g16::object::test_unit_cmds(k))
                        .unwrap(),
                    0,
                )
                .unwrap();
        }
        let prep_b = ComputeArena { compute: full_units, render: None, hw_slots: None };
        let make_pair = |o: &mut Objects, slots_pair: [usize; 2]| -> ComputeArena {
            let mut u = KVec::new();
            for &hw in &slots_pair {
                u.push(
                    ComputeSlot::new(o, &g16::object::Factory, hw as u64, &crate::g16::object::test_unit_cmds(hw as u64))
                        .unwrap(),
                    0,
                )
                .unwrap();
            }
            ComputeArena { compute: u, render: None, hw_slots: Some(slots_pair) }
        };
        let vm_a = vm(501);
        let vm_b = vm(502);
        let mask_a = (1u32 << 16) | (1u32 << 17);

        // Window 1 (prepare -> submit TOCTOU race):
        // Pipe 1 retires unit 16; VM A prepares {16, 17} while 17 is non-tail. Before A reaches
        // admission, VM B on pipe 2 != 1 borrows unit 17. Verify admission rejects {16, 17} both
        // while 17 is Published and after 17 retires, while compute_exhausted_in_mask stays false
        // during Published, and A cleanly re-prepares {16, 17, 18} to admit without ECANCELED.
        let mut s = Slots::new().unwrap();
        let pair_a = make_pair(&mut o, [16, 17]);
        let mut c1 = Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&pair_a, &mut c1, Some(vm_b.id), 1).unwrap(), Some(16));
        s.hold_vm(&c1, &vm_b).unwrap();
        s.publish(&mut c1).unwrap();
        s.retire(&mut c1).unwrap();
        // VM A prepares {16, 17} locklessly against tails={16}.
        let prep_mask = pick_compute_units_for(16, 0, 0, p0, s.compute_tail_mask(), 2, max_u, false);
        assert_eq!(prep_mask, mask_a);
        // Race: VM B claims & publishes 17 on pipe 2 before A calls admit_compute_mask.
        let mut c2 = Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&pair_a, &mut c2, Some(vm_b.id), 2).unwrap(), Some(17));
        s.hold_vm(&c2, &vm_b).unwrap();
        s.publish(&mut c2).unwrap();
        // Even with 17 Published on pipe 2 (and 16 Retired on pipe 1), compute_exhausted_in_mask
        // must stay false while any pipe has Published work, while admission rejects {16, 17}.
        assert!(!s.compute_exhausted_in_mask(mask_a));
        assert!(s.admit_compute_mask(mask_a).unwrap().is_none());
        // After 17 retires on pipe 2, {16, 17} is stranded (both undisplaced tails on p1 != p2).
        s.retire(&mut c2).unwrap();
        assert!(s.compute_exhausted_in_mask(mask_a));
        assert!(s.admit_compute_mask(mask_a).unwrap().is_none());
        // Retry with fresh compute_tail_mask + force_add_non_tail adds non-tail unit 18 and admits.
        let rescue = pick_compute_units_for(16, mask_a, 0, p0, s.compute_tail_mask(), 2, max_u, true);
        assert_eq!(rescue.count_ones(), 1);
        assert_eq!(rescue & s.compute_tail_mask(), 0);
        let rescued_mask = mask_a | rescue;
        assert!(!s.compute_exhausted_in_mask(rescued_mask));
        let waiters = s.admit_compute_mask(rescued_mask).unwrap().expect("rescued mask must admit");
        waiters.release(rescued_mask);

        // Window 2 (queued-on-input-fence / pending race against pair_borrow):
        // Pipe 1 retires unit 16; sparse VM A ({16, 17}) is admitted and waits on an input fence.
        // Multi-unit VM B (u32::MAX) on pipe 2 != 1 exhausts ALL pair_safe units on pipe 2 and
        // attempts to fall through to pair_borrow unit 17: claim_compute_for_pipe MUST refuse 17,
        // B must still make forward progress once its own pipe-2 flight retires, and A must later
        // claim 17 on pipe 1 with zero ECANCELED!
        let mut s2 = Slots::new().unwrap();
        let mut init1 = Claims::new();
        assert_eq!(s2.claim_compute_for_pipe(&pair_a, &mut init1, Some(vm_b.id), 1).unwrap(), Some(16));
        s2.hold_vm(&init1, &vm_b).unwrap();
        s2.publish(&mut init1).unwrap();
        s2.retire(&mut init1).unwrap();
        let waiters_a = s2.admit_compute_mask(mask_a).unwrap().expect("A must admit on safe pipe 1");
        assert_ne!(s2.compute_stranding_mask_for_pipe(2, u32::MAX) & (1u32 << 17), 0);
        assert_eq!(s2.compute_stranding_mask_for_pipe(1, mask_a) & (1u32 << 17), 0);
        // Drain every secondary unit except 16 (p1 tail) and 17 (guarded by waiter A) onto pipe 2.
        let mut b_flights: Vec<Claims> = Vec::new();
        loop {
            let mut cb = Claims::new();
            match s2.claim_compute_for_pipe(&prep_b, &mut cb, Some(vm_b.id), 2).unwrap() {
                Some(k) => {
                    assert_ne!(k, 17, "pipe 2 must NEVER pair_borrow unit 17 while waiter A needs {{16, 17}}");
                    s2.hold_vm(&cb, &vm_b).unwrap();
                    s2.publish(&mut cb).unwrap();
                    b_flights.push(cb);
                }
                None => break,
            }
        }
        // Exactly 28 secondary units (2..32 minus 16 and 17) were claimed on pipe 2; 17 stayed free!
        assert_eq!(b_flights.len(), 28);
        assert!(!s2.compute_available_in_mask_for_pipe(u32::MAX, 2));
        assert_eq!(s2.free_compute_in_mask_for_pipe(u32::MAX, 2), 0);
        // B retires its two oldest pipe-2 flights so the first is displaced and B keeps moving.
        s2.retire(&mut b_flights[0]).unwrap();
        s2.retire(&mut b_flights[1]).unwrap();
        let mut b_next = Claims::new();
        let b_reused = s2
            .claim_compute_for_pipe(&prep_b, &mut b_next, Some(vm_b.id), 2)
            .unwrap()
            .expect("B must make forward progress on pipe 2 after retiring its own flights");
        assert_ne!(b_reused, 17);
        s2.release(&mut b_next);
        for cb in &mut b_flights[2..] {
            s2.retire(cb).unwrap();
        }
        // Now A wakes from its input fence: never exhausted, claims 17 on pipe 1, publishes & retires.
        assert!(!s2.compute_exhausted_in_mask(mask_a));
        assert!(s2.compute_pipe_safe_for_mask(mask_a, 1));
        let mut ca = Claims::new();
        assert_eq!(s2.claim_compute_for_pipe(&pair_a, &mut ca, Some(vm_a.id), 1).unwrap(), Some(17));
        s2.hold_vm(&ca, &vm_a).unwrap();
        s2.publish(&mut ca).unwrap();
        waiters_a.release(mask_a);
        s2.retire(&mut ca).unwrap();
        assert!(!s2.compute_exhausted_in_mask(mask_a));

        // Post-quarantine non-adjacent mask {16, 19} (17 & 18 quarantined, 16 = pipe 1 tail):
        // unit 19 is not 16 ^ 1, yet pipe 2 must still be blocked from stranding waiter {16, 19}.
        let mut s3 = Slots::new().unwrap();
        let pair_1718 = make_pair(&mut o, [17, 18]);
        for _ in 0..2 {
            let mut cq = Claims::new();
            assert!(s3.claim_compute_for_pipe(&pair_1718, &mut cq, Some(vm_b.id), 3).unwrap().is_some());
            s3.hold_vm(&cq, &vm_b).unwrap();
            s3.publish(&mut cq).unwrap();
            s3.quarantine(&mut cq);
        }
        let pair_1619 = make_pair(&mut o, [16, 19]);
        let mask_1619 = (1u32 << 16) | (1u32 << 19);
        let mut cp1 = Claims::new();
        let tail_u = s3.claim_compute_for_pipe(&pair_1619, &mut cp1, Some(vm_b.id), 1).unwrap().unwrap();
        let other_u = if tail_u == 16 { 19 } else { 16 };
        s3.hold_vm(&cp1, &vm_b).unwrap();
        s3.publish(&mut cp1).unwrap();
        s3.retire(&mut cp1).unwrap();
        let w1619 = s3.admit_compute_mask(mask_1619).unwrap().expect("{16, 19} must admit on pipe 1");
        assert_ne!(s3.compute_stranding_mask_for_pipe(2, mask_1619) & (1u32 << other_u), 0);
        let mut cp2 = Claims::new();
        assert_eq!(s3.claim_compute_for_pipe(&pair_1619, &mut cp2, Some(vm_b.id), 2).unwrap(), None);
        w1619.release(mask_1619);
        assert_eq!(s3.claim_compute_for_pipe(&pair_1619, &mut cp2, Some(vm_b.id), 2).unwrap(), Some(other_u));
        s3.release(&mut cp2);
    }

    #[test]fn two_pipe_compute_out_of_order_retire_and_base_unit_isolation(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let p1_start = g16::hw::sizes::PIPE1_COMPUTE_START;
        let mut units=KVec::new();
        for k in 0..(p1_start + 2) as u64 {
            units.push(g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,k,&crate::g16::object::test_unit_cmds(k)).unwrap(),0).unwrap();
        }
        let prep=g16::resources::ComputeArena{compute:units,render:None,hw_slots:None};
        let a=vm(1);let b=vm(2);
        // Pipe 0 claims unit 0; pipe 1 stays out of 0..PIPE1_COMPUTE_START and claims PIPE1_COMPUTE_START.
        let mut c0=Claims::new();
        let mut c1=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep,&mut c0,Some(a.id),0).unwrap(),Some(0));
        assert_eq!(c0.compute_pipe,0);
        s.hold_vm(&c0,&a).unwrap();s.publish(&mut c0).unwrap();
        assert_eq!(s.claim_compute_for_pipe(&prep,&mut c1,Some(a.id),1).unwrap(),Some(p1_start));
        assert_eq!(c1.compute_pipe,1);
        s.hold_vm(&c1,&a).unwrap();s.publish(&mut c1).unwrap();
        // Pipe 1 finishes ahead of still-running pipe 0: retire must succeed (no EINVAL).
        assert_eq!(s.retire(&mut c1),Ok(()));
        // Second pipe-1 flight claims PIPE1_COMPUTE_START + 1 and retires, displacing PIPE1_COMPUTE_START.
        let mut c1b=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep,&mut c1b,Some(a.id),1).unwrap(),Some(p1_start + 1));
        s.hold_vm(&c1b,&a).unwrap();s.publish(&mut c1b).unwrap();
        assert_eq!(s.retire(&mut c1b),Ok(()));
        assert!(s.compute_available_for_pipe(p1_start + 2,1));
        // Now pipe 0's unit 0 retires: pipe 1's newer retirement must NOT mark unit 0 reusable,
        // while base unit 1 remains fresh so a 2-unit base-only VM is never exhausted!
        assert_eq!(s.retire(&mut c0),Ok(()));
        assert!(!s.compute_exhausted_within(2));
        assert!(s.compute_available(2));
        // Pipe 0 claims base unit 1 (not undisplaced unit 0), publishes, and retires -> unit 0 displaced!
        let mut c0b=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep,&mut c0b,Some(b.id),0).unwrap(),Some(1));
        s.hold_vm(&c0b,&b).unwrap();s.publish(&mut c0b).unwrap();
        assert_eq!(s.retire(&mut c0b),Ok(()));
        assert!(!s.compute_exhausted_within(2));
        assert!(s.compute_available(2));
        // A 2-unit base-only VM can claim displaced base unit 0 on pipe 0, while pipe 1
        // reclaims displaced extension unit PIPE1_COMPUTE_START without touching 0..PIPE1_COMPUTE_START.
        let mut base_units=KVec::new();
        for k in 0..2u64 {
            base_units.push(g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,k,&crate::g16::object::test_unit_cmds(k)).unwrap(),0).unwrap();
        }
        let prep_base=g16::resources::ComputeArena{compute:base_units,render:None,hw_slots:None};
        let mut c0c=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep_base,&mut c0c,Some(b.id),0).unwrap(),Some(0));
        let mut c1c=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep,&mut c1c,Some(b.id),1).unwrap(),Some(p1_start));
        s.release(&mut c0c);
        s.release(&mut c1c);
    }

    #[test]fn scs_compute_and_base_pb_route_around_quarantined_slots(){
        use crate::g16::resources::{pick_base_pb_slots, pick_scs_compute_slots};
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let p1_start = g16::hw::sizes::PIPE1_COMPUTE_START;
        let mut full_units=KVec::new();
        for k in 0..g16::hw::sizes::COMPUTE_UNITS as u64 {
            full_units.push(g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,k,&crate::g16::object::test_unit_cmds(k)).unwrap(),0).unwrap();
        }
        let prep_full=g16::resources::ComputeArena{compute:full_units,render:None,hw_slots:None};
        let bad=vm(1);let fresh=vm(2);let other=vm(3);
        // Exact dcp102b sequence: a prior process hangs and permanently quarantines compute unit 0.
        let mut c=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep_full,&mut c,Some(bad.id),0).unwrap(),Some(0));
        s.hold_vm(&c,&bad).unwrap();s.publish(&mut c).unwrap();s.quarantine(&mut c);
        assert_eq!(s.quarantined_compute_mask(), 1u32 << 0);
        // Pipe 1 runs concurrent non-SCS compute outside dynamic pipe0_exclusive_mask() ([1, 2]),
        // so it claims unit 3 and never holds an undisplaced lease on the fresh SCS VM's [1, 2].
        let _ = p1_start;
        assert_eq!(s.pipe0_exclusive_mask(), (1u32 << 1) | (1u32 << 2));
        let mut c_p1=Claims::new();
        assert_eq!(s.claim_compute_for_pipe(&prep_full,&mut c_p1,Some(other.id),1).unwrap(),Some(3));
        s.hold_vm(&c_p1,&other).unwrap();s.publish(&mut c_p1).unwrap();s.retire(&mut c_p1).unwrap();
        // Brand-new SCS compute VM skips quarantined unit 0 and binds [1, 2] (mask 0b110), matching
        // dynamic pipe0_exclusive_mask() / scs_compute_mask().
        assert_eq!(s.scs_compute_mask(), (1u32 << 1) | (1u32 << 2));
        let scs_slots = pick_scs_compute_slots(
            s.scs_compute_mask(),
            s.quarantined_compute_mask(),
            g16::hw::sizes::COMPUTE_UNITS,
        );
        assert_eq!(scs_slots, [1, 2]);
        let mut scs_units=KVec::new();
        for (local, &hw_slot) in scs_slots.iter().enumerate() {
            scs_units.push(g16::slots::ComputeSlot::new(&mut o,&g16::object::Factory,local as u64,&crate::g16::object::test_unit_cmds(hw_slot as u64)).unwrap(),0).unwrap();
        }
        let prep_scs=g16::resources::ComputeArena{compute:scs_units,render:None,hw_slots:Some(scs_slots)};
        let scs_mask = prep_scs.compute_slot_mask().unwrap();
        assert_eq!(scs_mask, (1u32 << 1) | (1u32 << 2));
        // Execute two back-to-back SCS compute commands (e.g. rt.scene p_rt + p_comp) repeatedly:
        // must never hit compute_exhausted_in_mask and must ping-pong cleanly across [1, 2].
        for expected in [1usize, 2, 1, 2] {
            assert!(!s.compute_exhausted_in_mask(scs_mask));
            assert!(s.compute_available_in_mask(scs_mask));
            let mut cs=Claims::new();
            assert_eq!(s.claim_compute_for_pipe(&prep_scs,&mut cs,Some(fresh.id),0).unwrap(),Some(expected));
            s.hold_vm(&cs,&fresh).unwrap();s.publish(&mut cs).unwrap();s.retire(&mut cs).unwrap();
        }
        let make_scs_arena = |o: &mut Objects, slots: [usize; 2]| {
            let mut units = KVec::new();
            for (local, &hw_slot) in slots.iter().enumerate() {
                units
                    .push(
                        g16::slots::ComputeSlot::new(
                            o,
                            &g16::object::Factory,
                            local as u64,
                            &crate::g16::object::test_unit_cmds(hw_slot as u64),
                        )
                        .unwrap(),
                        0,
                    )
                    .unwrap();
            }
            g16::resources::ComputeArena {
                compute: units,
                render: None,
                hw_slots: Some(slots),
            }
        };
        // Ordering A: secondary pipe 1 retires onto unit 2 BEFORE pipe 0 quarantines unit 0.
        // Reservation must skip secondary-pinned unit 2 and promote unit 3 -> [1, 3].
        let mut s_ord_a = Slots::new().unwrap();
        let mut c_p1a = Claims::new();
        assert_eq!(s_ord_a.claim_compute_for_pipe(&prep_full, &mut c_p1a, Some(other.id), 1).unwrap(), Some(2));
        s_ord_a.hold_vm(&c_p1a, &other).unwrap(); s_ord_a.publish(&mut c_p1a).unwrap(); s_ord_a.retire(&mut c_p1a).unwrap();
        let mut c_bada = Claims::new();
        assert_eq!(s_ord_a.claim_compute_for_pipe(&prep_full, &mut c_bada, Some(bad.id), 0).unwrap(), Some(0));
        s_ord_a.hold_vm(&c_bada, &bad).unwrap(); s_ord_a.publish(&mut c_bada).unwrap(); s_ord_a.quarantine(&mut c_bada);
        assert_eq!(s_ord_a.pipe0_exclusive_mask(), (1u32 << 1) | (1u32 << 3));
        assert_eq!(s_ord_a.scs_compute_mask(), (1u32 << 1) | (1u32 << 3));
        let scs_a = pick_scs_compute_slots(
            s_ord_a.scs_compute_mask(),
            s_ord_a.quarantined_compute_mask(),
            g16::hw::sizes::COMPUTE_UNITS,
        );
        assert_eq!(scs_a, [1, 3]);
        let prep_scs_a = make_scs_arena(&mut o, scs_a);
        let mask_a = prep_scs_a.compute_slot_mask().unwrap();
        for expected in [1usize, 3, 1, 3] {
            assert!(!s_ord_a.compute_exhausted_in_mask(mask_a));
            assert!(s_ord_a.compute_available_in_mask(mask_a));
            let mut cs = Claims::new();
            assert_eq!(s_ord_a.claim_compute_for_pipe(&prep_scs_a, &mut cs, Some(fresh.id), 0).unwrap(), Some(expected));
            s_ord_a.hold_vm(&cs, &fresh).unwrap(); s_ord_a.publish(&mut cs).unwrap(); s_ord_a.retire(&mut cs).unwrap();
        }
        // Ordering C (monotonicity): even after pipe 1 later displaces unit 2, [1, 3] stays
        // monotonically reserved so secondary pipes cannot steal unit 3 from the already-bound SCS VM.
        let mut c_disp = Claims::new();
        let u_disp = s_ord_a.claim_compute_for_pipe(&prep_full, &mut c_disp, Some(other.id), 1).unwrap().unwrap();
        assert_ne!(u_disp, 1);
        assert_ne!(u_disp, 3);
        s_ord_a.hold_vm(&c_disp, &other).unwrap(); s_ord_a.publish(&mut c_disp).unwrap(); s_ord_a.retire(&mut c_disp).unwrap();
        assert_eq!(s_ord_a.pipe0_exclusive_mask(), (1u32 << 1) | (1u32 << 3));
        let mut c_p2 = Claims::new();
        let u_p2 = s_ord_a.claim_compute_for_pipe(&prep_full, &mut c_p2, Some(other.id), 2).unwrap().unwrap();
        assert_ne!(u_p2, 1);
        assert_ne!(u_p2, 3);
        s_ord_a.hold_vm(&c_p2, &other).unwrap(); s_ord_a.publish(&mut c_p2).unwrap(); s_ord_a.retire(&mut c_p2).unwrap();
        for expected in [1usize, 3] {
            assert!(!s_ord_a.compute_exhausted_in_mask(mask_a));
            let mut cs = Claims::new();
            assert_eq!(s_ord_a.claim_compute_for_pipe(&prep_scs_a, &mut cs, Some(fresh.id), 0).unwrap(), Some(expected));
            s_ord_a.hold_vm(&cs, &fresh).unwrap(); s_ord_a.publish(&mut cs).unwrap(); s_ord_a.retire(&mut cs).unwrap();
        }
        // Ordering B: secondary pipe 1 is STILL IN-FLIGHT on unit 2 when pipe 0 quarantines unit 0,
        // and retires unit 2 afterwards. Reservation must still skip in-flight unit 2 -> [1, 3].
        let mut s_ord_b = Slots::new().unwrap();
        let mut c_p1b = Claims::new();
        assert_eq!(s_ord_b.claim_compute_for_pipe(&prep_full, &mut c_p1b, Some(other.id), 1).unwrap(), Some(2));
        s_ord_b.hold_vm(&c_p1b, &other).unwrap(); s_ord_b.publish(&mut c_p1b).unwrap();
        let mut c_badb = Claims::new();
        assert_eq!(s_ord_b.claim_compute_for_pipe(&prep_full, &mut c_badb, Some(bad.id), 0).unwrap(), Some(0));
        s_ord_b.hold_vm(&c_badb, &bad).unwrap(); s_ord_b.publish(&mut c_badb).unwrap(); s_ord_b.quarantine(&mut c_badb);
        assert_eq!(s_ord_b.pipe0_exclusive_mask(), (1u32 << 1) | (1u32 << 3));
        assert_eq!(s_ord_b.scs_compute_mask(), (1u32 << 1) | (1u32 << 3));
        // Secondary pipe 1 finishes and retires unit 2 afterwards, pinning unit 2 on pipe 1.
        s_ord_b.retire(&mut c_p1b).unwrap();
        let scs_b = pick_scs_compute_slots(
            s_ord_b.scs_compute_mask(),
            s_ord_b.quarantined_compute_mask(),
            g16::hw::sizes::COMPUTE_UNITS,
        );
        assert_eq!(scs_b, [1, 3]);
        let prep_scs_b = make_scs_arena(&mut o, scs_b);
        let mask_b = prep_scs_b.compute_slot_mask().unwrap();
        for expected in [1usize, 3, 1, 3] {
            assert!(!s_ord_b.compute_exhausted_in_mask(mask_b));
            assert!(s_ord_b.compute_available_in_mask(mask_b));
            let mut cs = Claims::new();
            assert_eq!(s_ord_b.claim_compute_for_pipe(&prep_scs_b, &mut cs, Some(fresh.id), 0).unwrap(), Some(expected));
            s_ord_b.hold_vm(&cs, &fresh).unwrap(); s_ord_b.publish(&mut cs).unwrap(); s_ord_b.retire(&mut cs).unwrap();
        }
        // Ordering D (total 15-pipe saturation + Tier-2 fallback + double-crash self-heal):
        // Every secondary pipe 1..16 holds 1 retired + 1 in-flight unit (all 30 secondary units
        // pinned) when pipe 0 quarantines unit 0. Tier-2 promotes a non-tail unit (2) whose pipe
        // has a newer in-flight publication (17); retiring 17 displaces 2 cleanly, while a second
        // crash quarantining 17 instead self-heals scs_compute_mask() onto another clean unit.
        let mut s_sat = Slots::new().unwrap();
        let mut sat_inflight = Vec::new();
        for p in 1..g16::hw::sizes::COMPUTE_PIPES {
            let mut c_ret = Claims::new();
            let _ = s_sat.claim_compute_for_pipe(&prep_full, &mut c_ret, Some(other.id), p).unwrap().unwrap();
            s_sat.hold_vm(&c_ret, &other).unwrap(); s_sat.publish(&mut c_ret).unwrap(); s_sat.retire(&mut c_ret).unwrap();
        }
        for p in 1..g16::hw::sizes::COMPUTE_PIPES {
            let mut c_pub = Claims::new();
            let _ = s_sat.claim_compute_for_pipe(&prep_full, &mut c_pub, Some(other.id), p).unwrap().unwrap();
            s_sat.hold_vm(&c_pub, &other).unwrap(); s_sat.publish(&mut c_pub).unwrap();
            sat_inflight.push(c_pub);
        }
        let mut c_bads = Claims::new();
        assert_eq!(s_sat.claim_compute_for_pipe(&prep_full, &mut c_bads, Some(bad.id), 0).unwrap(), Some(0));
        s_sat.hold_vm(&c_bads, &bad).unwrap(); s_sat.publish(&mut c_bads).unwrap(); s_sat.quarantine(&mut c_bads);
        let scs_sat = pick_scs_compute_slots(
            s_sat.scs_compute_mask(),
            s_sat.quarantined_compute_mask(),
            g16::hw::sizes::COMPUTE_UNITS,
        );
        assert_eq!(scs_sat[0], 1);
        let promoted = scs_sat[1];
        assert!(promoted >= 2 && promoted < 17, "Tier 2 must pick a non-tail retired unit, got {promoted}");
        // Retiring pipe 2..16's in-flight units frees units 3..17, while quarantining pipe 1's
        // in-flight unit (which sat ahead of `promoted` = 2) pins 2 and triggers self-healing
        // promotion of a clean unit into scs_compute_mask().
        for c_pub in sat_inflight.iter_mut().skip(1) {
            s_sat.retire(c_pub).unwrap();
        }
        s_sat.quarantine(&mut sat_inflight[0]);
        let healed = pick_scs_compute_slots(
            s_sat.scs_compute_mask(),
            s_sat.quarantined_compute_mask(),
            g16::hw::sizes::COMPUTE_UNITS,
        );
        assert_eq!(healed[0], 1);
        assert_ne!(healed[1], promoted);
        let prep_healed = make_scs_arena(&mut o, healed);
        let mask_healed = prep_healed.compute_slot_mask().unwrap();
        for _ in 0..4 {
            assert!(!s_sat.compute_exhausted_in_mask(mask_healed));
            let mut cs = Claims::new();
            assert!(s_sat.claim_compute_for_pipe(&prep_healed, &mut cs, Some(fresh.id), 0).unwrap().is_some());
            s_sat.hold_vm(&cs, &fresh).unwrap(); s_sat.publish(&mut cs).unwrap(); s_sat.retire(&mut cs).unwrap();
        }
        // Ordering E: multiple sequential quarantines across secondary-pipe retired leases on
        // units 2, 4, 5, 6 slide scs_compute_mask() cleanly across [1, 3] -> [3, 7] -> [3, 8].
        // Pin exact units {2, 4, 5, 6} onto secondary pipes 1..=4 (pair-safe full-mask claiming
        // normally keeps pipes on disjoint even pairs so {4, 5} would not split across pipes).
        let mut s_multi = Slots::new().unwrap();
        for (p, target_u) in [(1usize, 2usize), (2, 3), (3, 4), (4, 5), (2, 6)] {
            let pin_arena = make_scs_arena(&mut o, [target_u, target_u]);
            let mut cp = Claims::new();
            assert_eq!(s_multi.claim_compute_for_pipe(&pin_arena, &mut cp, Some(other.id), p).unwrap(), Some(target_u));
            s_multi.hold_vm(&cp, &other).unwrap(); s_multi.publish(&mut cp).unwrap(); s_multi.retire(&mut cp).unwrap();
        }
        // Pair-safe full-mask claiming keeps distinct secondary pipes on disjoint pairs (pipe 1 -> 2,
        // pipe 2 -> 4, skipping 3 because 2 is pipe 1's tail) before borrowing a partner only when
        // no pair-safe unit remains.
        let mut s_pair = Slots::new().unwrap();
        let mut cp_a = Claims::new();
        assert_eq!(s_pair.claim_compute_for_pipe(&prep_full, &mut cp_a, Some(other.id), 1).unwrap(), Some(2));
        s_pair.hold_vm(&cp_a, &other).unwrap(); s_pair.publish(&mut cp_a).unwrap(); s_pair.retire(&mut cp_a).unwrap();
        let mut cp_b = Claims::new();
        assert_eq!(s_pair.claim_compute_for_pipe(&prep_full, &mut cp_b, Some(other.id), 2).unwrap(), Some(4));
        s_pair.release(&mut cp_b);
        let pair_23 = make_scs_arena(&mut o, [2, 3]);
        let mut cp_borrow = Claims::new();
        assert_eq!(s_pair.claim_compute_for_pipe(&pair_23, &mut cp_borrow, Some(other.id), 2).unwrap(), Some(3));
        s_pair.release(&mut cp_borrow);
        for (q_unit, expected_pair) in [(0usize, [1usize, 3]), (1, [3, 7]), (7, [3, 8])] {
            let mut cq = Claims::new();
            assert_eq!(s_multi.claim_compute_for_pipe(&prep_full, &mut cq, Some(bad.id), 0).unwrap(), Some(q_unit));
            s_multi.hold_vm(&cq, &bad).unwrap(); s_multi.publish(&mut cq).unwrap(); s_multi.quarantine(&mut cq);
            let pair = pick_scs_compute_slots(
                s_multi.scs_compute_mask(),
                s_multi.quarantined_compute_mask(),
                g16::hw::sizes::COMPUTE_UNITS,
            );
            assert_eq!(pair, expected_pair);
        }
        let prep_multi = make_scs_arena(&mut o, [3, 8]);
        let mask_multi = prep_multi.compute_slot_mask().unwrap();
        for _ in 0..4 {
            assert!(!s_multi.compute_exhausted_in_mask(mask_multi));
            let mut cs = Claims::new();
            assert!(s_multi.claim_compute_for_pipe(&prep_multi, &mut cs, Some(fresh.id), 0).unwrap().is_some());
            s_multi.hold_vm(&cs, &fresh).unwrap(); s_multi.publish(&mut cs).unwrap(); s_multi.retire(&mut cs).unwrap();
        }
        // Up to 28 quarantines slide pick_scs_compute_slots cleanly across COMPUTE_UNITS = 32.
        assert_eq!(pick_scs_compute_slots(0, 0b001111, g16::hw::sizes::COMPUTE_UNITS), [4, 5]);
        assert_eq!(pick_scs_compute_slots(0, 0b111111, g16::hw::sizes::COMPUTE_UNITS), [6, 7]);
        assert_eq!(
            pick_scs_compute_slots((1u32 << 1) | (1u32 << 3), 1u32 << 0, g16::hw::sizes::COMPUTE_UNITS),
            [1, 3]
        );
        // 16 concurrent compute VMs across all 16 pipes with 32 compute units: 0 cross-VM evictions,
        // 0 slot exhaustion, each pipe keeps 2 dedicated units (1 retired undisplaced + 1 claimable).
        let mut s16 = Slots::new().unwrap();
        let vms16: Vec<_> = (100u64..116).map(vm).collect();
        for round in 0..4usize {
            let mut claims = Vec::new();
            for p in 0..g16::hw::sizes::COMPUTE_PIPES {
                let mut cp = Claims::new();
                let u = s16
                    .claim_compute_for_pipe(&prep_full, &mut cp, Some(vms16[p].id), p)
                    .unwrap()
                    .expect("16 concurrent pipes must each claim a unit with 32 units");
                if p == 0 {
                    assert!(u < 2, "pipe 0 prefers pipe0_exclusive_mask [0, 1], got {u}");
                } else {
                    assert!(u >= 2, "secondary pipe {p} must stay clear of [0, 1], got {u}");
                }
                s16.hold_vm(&cp, &vms16[p]).unwrap();
                s16.publish(&mut cp).unwrap();
                claims.push(cp);
            }
            for cp in &mut claims {
                s16.retire(cp).unwrap();
            }
            if round >= 1 {
                assert_eq!(s16.stats.cross_vm_retargets[0], 0);
            }
        }
        // Twin fix: pick_base_pb_slots skips quarantined warm PB slots and falls back to EXT_PB_SLOTS.
        assert_eq!(pick_base_pb_slots(0, 0), [0, 1]);
        assert_eq!(pick_base_pb_slots(3, 1u32 << 3), [4, 5]);
        assert_eq!(pick_base_pb_slots(3, (1u32 << 3) | (1u32 << 4)), [5, 6]);
        let all_warm_but_one = ((1u32 << g16::hw::sizes::WARM_PB_SLOTS) - 1) & !(1u32 << 7);
        assert_eq!(pick_base_pb_slots(0, all_warm_but_one), [7, g16::hw::sizes::WARM_PB_SLOTS]);
        // Confirm Pools::exhausted_for_mask_mode stays false for render_scs with picked healthy slots.
        let mut pools = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let bad_vm = vm(10);
        let good_vm = vm(11);
        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let ep = g16::pools::PbEpoch::new(
            &factory,
            &bad_vm,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        )
        .unwrap();
        ep.prepare(3, 0x70c0000000, 2048).unwrap();
        ep.expose();
        pools.publish_flight(3);
        pools.quarantine_flight(3, Some(ep));
        let picked = pick_base_pb_slots(3, pools.quarantined_mask());
        assert_eq!(picked, [4, 5]);
        let pb_mask = (1u32 << picked[0]) | (1u32 << picked[1]);
        assert!(!pools.exhausted_for_mask_mode(&good_vm, pb_mask, picked[0], true));
    }

    #[test]fn render_only_pb_routing_without_quarantine_matches_v29_for_any_boot_history(){
        use crate::g16::resources::pick_base_pb_slots;
        let warm = g16::hw::sizes::WARM_PB_SLOTS;
        // v29 (4310e041) gave a new render arena [start % WARM_PB_SLOTS, home + 1]. With nothing
        // quarantined the quarantine-aware picker must be that exact pair for every start.
        for start in 0..(4 * warm) {
            assert_eq!(pick_base_pb_slots(start, 0), [start % warm, (start + 1) % warm]);
        }
        // render.light [procs] d=16 (dcp103a, dcp103b) makes WARM_PB_SLOTS render-only VMs. Earlier
        // work in the same boot only moves `next_pb_slot`: whatever the start, every warm slot is
        // the home of exactly one of them and the alternate of exactly one other.
        for start in [0usize, 1, 7, 15, 16, 39, 1000] {
            let mut home = [0u32; 32];
            let mut alt = [0u32; 32];
            for vm in 0..warm {
                let [h, a] = pick_base_pb_slots(start + vm, 0);
                assert_ne!(h, a);
                home[h] += 1;
                alt[a] += 1;
            }
            assert!(home[..warm].iter().all(|&n| n == 1), "start {start}: {home:?}");
            assert!(alt[..warm].iter().all(|&n| n == 1), "start {start}: {alt:?}");
            assert!(home[warm..].iter().chain(&alt[warm..]).all(|&n| n == 0));
        }
    }

    #[test]fn base_render_quarantine_expands_fresh_vm_and_recovers_displacement(){
        use crate::g16::resources::{BASE_RENDER_SLOTS, EXT_RENDER_TIERS, target_render_slots};
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let mut passes=KVec::new();
        for k in 0..4u64 {
            passes.push(g16::slots::PassSlot::new(&mut o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap(),0).unwrap();
        }
        let render=sync::Arc::new(render_arena(passes),0).unwrap();
        let prep=g16::resources::Prepared{compute:KVec::new(),render:Some(render),hw_slots:None};
        let a=vm(1);let b=vm(2);
        let geo=g16::encode::Geometry{tpc_bytes:0x1000,tilemap_bytes:0x1000};
        // Retire base pass slot 0, then hang and quarantine base pass slot 1 -> base [0..2] exhausted.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_count=1;c0.pass_mask=1<<0;
        s.claim_pass_in(&prep,0,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c0,&a).unwrap();s.publish(&mut c0).unwrap();s.retire(&mut c0).unwrap();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_count=1;c1.pass_mask=1<<1;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.publish(&mut c1).unwrap();s.quarantine(&mut c1);
        assert!(s.base_render_quarantined());
        assert!(s.render_exhausted_within(2));
        assert!(!s.render_exhausted_within(4));
        // Fresh single-pass VM expands Tier 0 RenderExtArena immediately because a base pass slot is quarantined.
        let mut last=0u64;let mut burst=0u32;
        assert_eq!(
            target_render_slots(&mut last,&mut burst,10_000_000,1,0,s.base_render_quarantined(),BASE_RENDER_SLOTS),
            EXT_RENDER_TIERS[0].1
        );
        // Expanded VM picks and retires pass slot 2, displacing base pass slot 0 back to reusable.
        let k=s.pick_pass_in(&prep,&geo,0).unwrap();
        assert_eq!(k,2);
        let mut c2=Claims::new();c2.pass[0]=k as u32;c2.pass_count=1;c2.pass_mask=1<<k;
        s.claim_pass_in(&prep,k,Some(b.id),0x1000).unwrap();
        s.hold_vm(&c2,&b).unwrap();s.publish(&mut c2).unwrap();s.retire(&mut c2).unwrap();
        assert!(!s.render_exhausted_within(2));
    }

    #[test]fn async_scrub_queue_and_recovery_generation_guard(){
        use crate::g16::slots::ScrubQueue;
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let render=sync::Arc::new(arena.render,0).unwrap();
        let prep=g16::resources::Prepared{compute:arena.compute.compute,render:Some(render.clone()),hw_slots:None};
        let a=vm(1);
        let geo=g16::encode::Geometry{tpc_bytes:0x1000,tilemap_bytes:0x2000};
        // Prime and displace slot 0.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();
        render.set_dirty(0,0x1000,0x2000).unwrap();
        render.passes[0].tilemap.fill(0,0x66,0x2000).unwrap();
        s.retire(&mut c0).unwrap();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();
        s.retire(&mut c1).unwrap();

        // Hand staged scrub off via ScrubQueue; waiter is woken on completion and woke flag is set.
        s.stage_displaced_pass_scrubs();
        let batch=s.take_staged_scrubs();
        assert!(!batch.is_empty());
        let mut q=ScrubQueue::new();
        assert!(q.push_batch(batch));
        assert!(s.scrubbing_active());
        assert_eq!(s.pick_pass_in(&prep,&geo,0),None);
        let mut item=q.pop().expect("queued scrub");
        assert!(q.pop().is_none());
        let wmb_before=g16::engine::WMB_CALLS.load(std::sync::atomic::Ordering::SeqCst);
        assert!(item.run_one());
        assert!(g16::engine::WMB_CALLS.load(std::sync::atomic::Ordering::SeqCst)>wmb_before);
        assert!(!s.scrubbing_active());
        assert!(s.take_scrub_woke());
        assert!(!s.take_scrub_woke());
        assert_eq!(s.pick_pass_in(&prep,&geo,0),Some(0));
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        assert!(!s.render_needs_full_work(0));

        // Re-displace slot 0, stage scrub, then simulate recovery invalidate_scrub() BEFORE
        // the async scrub runs: it must clear scrubbing without marking clean, so next claim
        // performs a full scrub.
        let mut c0=Claims::new();c0.pass[0]=0;c0.pass_mask=1;
        s.hold_vm(&c0,&a).unwrap();s.hold_arenas(&c0,&prep);s.publish(&mut c0).unwrap();s.retire(&mut c0).unwrap();
        let mut c1=Claims::new();c1.pass[0]=1;c1.pass_mask=2;
        s.claim_pass_in(&prep,1,Some(a.id),0x1000).unwrap();
        s.hold_vm(&c1,&a).unwrap();s.hold_arenas(&c1,&prep);s.publish(&mut c1).unwrap();s.retire(&mut c1).unwrap();
        s.stage_displaced_pass_scrubs();
        assert!(q.push_batch(s.take_staged_scrubs()));
        assert!(s.scrubbing_active());
        s.invalidate_scrub();
        let mut stale=q.pop().expect("stale scrub");
        assert!(!stale.run_one());
        assert!(!s.scrubbing_active());
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        assert!(s.render_needs_full_work(0));
    }

    #[test]fn lazy_tilemap_prefix_scrub_and_classed_slot_selection_avoid_overscrub(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let mut passes=KVec::new();
        for k in 0..3u64 {
            passes.push(g16::slots::PassSlot::new(&mut o, k, 1, &crate::g16::object::test_cmds((k) as u64)).unwrap(),0).unwrap();
        }
        let render=sync::Arc::new(render_arena(passes),0).unwrap();
        let prep=g16::resources::Prepared{compute:KVec::new(),render:Some(render.clone()),hw_slots:None};
        let a=vm(1);let b=vm(2);
        let pub_retire=|s:&mut Slots,k:usize,vm:&sync::Arc<g16::vm::VmState>|{
            let mut c=Claims::new();c.pass[0]=k as u32;c.pass_mask=1u64<<k;
            s.hold_vm(&c,vm).unwrap();s.hold_arenas(&c,&prep);s.publish(&mut c).unwrap();s.retire(&mut c).unwrap();
        };
        // Slot 0 runs a large pass (0x10000), then a small pass (0x2000) -> dirty=0x10000, last=0x2000.
        s.claim_pass_in(&prep,0,Some(a.id),0x10000).unwrap();
        prep.set_pass_dirty(0,0x2000,0x10000,0x200).unwrap();
        render.passes[0].tilemap.fill(0,0x55,0x10000).unwrap();
        pub_retire(&mut s,0,&a);
        s.claim_pass_in(&prep,1,Some(a.id),0x2000).unwrap();
        prep.set_pass_dirty(1,0x1000,0x2000,0x200).unwrap();
        pub_retire(&mut s,1,&a);
        // Synchronous small claim on slot 0 only zeroes 0..0x2000 and leaves 0x2000..0x10000 dirty.
        s.claim_pass_in(&prep,0,Some(a.id),0x2000).unwrap();
        assert_eq!(render.passes[0].tilemap.fills.lock().unwrap().last(),Some(&(0,0x2000)));
        assert_eq!(render.passes[0].tilemap.snapshot()[0x2000],0x55);
        prep.set_pass_dirty(0,0x1000,0x2000,0x1000).unwrap(); // multi-layer heap=0x1000
        render.passes[0].tilemap.fill(0,0x66,0x2000).unwrap();
        render.passes[0].heap.fill(0,0x77,0x1000).unwrap();
        pub_retire(&mut s,0,&a);
        // Slot 2 retires to displace slot 0; both slot 0 (partial) and slot 1 (fully clean) scrub off-lock.
        s.claim_pass_in(&prep,2,Some(a.id),0x2000).unwrap();
        prep.set_pass_dirty(2,0x1000,0x2000,0x200).unwrap();
        pub_retire(&mut s,2,&a);
        s.stage_displaced_pass_scrubs();
        let _=drain_scrubs(s.take_staged_scrubs());
        // Slot 0 off-lock scrubbed ONLY 0..0x2000 tilemap and 0..0x1000 heap; high-water 0x10000 preserved!
        assert_eq!(render.tilemap_dirty[0].load(std::sync::atomic::Ordering::Acquire),0x10000);
        assert_eq!(render.tilemap_clean[0].load(std::sync::atomic::Ordering::Acquire),0x2000);
        assert_eq!(render.heap_dirty[0].load(std::sync::atomic::Ordering::Acquire),0);
        assert!(render.passes[0].tilemap.snapshot()[..0x2000].iter().all(|&x|x==0));
        assert_eq!(render.passes[0].tilemap.snapshot()[0x2000],0x55);
        assert!(render.passes[0].heap.snapshot()[..0x1000].iter().all(|&x|x==0));
        // Slot 1 off-lock scrubbed fully (dirty=0, clean=0).
        assert_eq!(render.tilemap_dirty[1].load(std::sync::atomic::Ordering::Acquire),0);
        assert_eq!(render.tilemap_clean[1].load(std::sync::atomic::Ordering::Acquire),0);
        // Cursor starts at 0 (after slot 2): small pass (0x2000) picks slot 0 (exact clean prefix),
        // while large pass (0x8000) prefers slot 1 (zero sync delta) over partial-clean slot 0!
        let small=g16::encode::Geometry{tpc_bytes:0x1000,tilemap_bytes:0x2000};
        let large=g16::encode::Geometry{tpc_bytes:0x1000,tilemap_bytes:0x8000};
        assert_eq!(s.pick_pass_in(&prep,&small,0),Some(0));
        assert_eq!(s.pick_pass_in(&prep,&large,0),Some(1));
        // Claiming partial-clean slot 0 for a medium pass (0x6000) zeroes ONLY tail delta (0x2000, 0x4000)
        // and performs zero heap fills.
        let heap_before=render.passes[0].heap.fills.lock().unwrap().len();
        s.claim_pass_in(&prep,0,Some(a.id),0x6000).unwrap();
        assert_eq!(render.passes[0].tilemap.fills.lock().unwrap().last(),Some(&(0x2000,0x4000)));
        assert_eq!(render.passes[0].heap.fills.lock().unwrap().len(),heap_before);
        assert!(render.passes[0].tilemap.snapshot()[..0x6000].iter().all(|&x|x==0));
        assert_eq!(render.passes[0].tilemap.snapshot()[0x6000],0x55);
        pub_retire(&mut s,0,&a);
        // Cross-VM switch on slot 0 still full-scrubs the preserved high-water mark (0x10000) + full heap (0x4000).
        s.claim_pass_in(&prep,1,Some(a.id),0x2000).unwrap();
        pub_retire(&mut s,1,&a);
        s.claim_pass_in(&prep,0,Some(b.id),0x2000).unwrap();
        assert_eq!(render.passes[0].tilemap.fills.lock().unwrap().last(),Some(&(0,0x10000)));
        assert_eq!(render.passes[0].heap.fills.lock().unwrap().last(),Some(&(0,0x4000)));
        assert!(render.passes[0].tilemap.snapshot()[..0x10000].iter().all(|&x|x==0));
    }
    #[test]fn queue_graph_objects_cache_only_firmware_owned_records(){
        use g16::object::Class;
        use g16::graph::{ComputeGraph, FwAbi, RenderGraph, cmd_fw, cmd_shared, graph_fw, qinfo_cached};
        assert_eq!(graph_fw(), Class::FwCached);
        assert!(qinfo_cached());
        assert_eq!(cmd_fw(), Class::Fw);
        assert_eq!(cmd_shared(), Class::Shared);
        let mut o = Objects::new();
        let mut rings = Objects::persistent();
        let ctx = o.alloc(g16::object::Request {
            size: 0x40,
            class: Class::Fw,
            placement: g16::object::Placement::Anywhere,
            offset: 0,
            client: None,
            lower: None,
            cpu: true,
        }).unwrap();
        let c = ComputeGraph::new(&mut o, &mut rings, ctx.clone()).unwrap();
        let c1 = ComputeGraph::new(&mut o, &mut rings, ctx.clone()).unwrap();
        let r = RenderGraph::new(&mut o, &mut rings, ctx).unwrap();
        c.init(FwAbi, true).unwrap();
        c1.init(FwAbi, false).unwrap();
        r.init(FwAbi).unwrap();
        assert_eq!(c.sched_ctx.va, c1.sched_ctx.va);
        assert_ne!(c.qinfo.va, c1.qinfo.va);
        assert_ne!(c.fw_stamp.va, c1.fw_stamp.va);
        assert_eq!(c1.qinfo.class, Class::FwCached);
        assert_eq!(c1.qstate.class, Class::Fw);
        assert_eq!(c1.ring.class, Class::FwCoherent);
        assert_eq!(c1.fw_stamp.class, Class::Shared);
        assert_eq!(c.qinfo.class, Class::FwCached);
        assert_eq!(c.qscratch.class, Class::FwCached);
        assert_eq!(c.job_list.class, Class::FwCached);
        assert_eq!(c.sched_ctx.class, Class::Fw);
        assert_eq!(c.qstate.class, Class::Fw);
        assert_eq!(c.notifier.class, Class::FwCached);
        assert_eq!(c.threshold.class, Class::Fw);
        assert_eq!(c.ring.class, Class::FwCoherent);
        assert_eq!(c.user_stamp.class, Class::Shared);
        assert_eq!(c.fw_stamp.class, Class::Shared);
        for stage in 0..2 {
            assert_eq!(r.qinfo[stage].class, Class::FwCached);
            assert_eq!(r.qscratch[stage].class, Class::FwCached);
            assert_eq!(r.job_list[stage].class, Class::FwCached);
            assert_eq!(r.qstate[stage].class, Class::Fw);
            assert_eq!(r.ring[stage].class, Class::FwCoherent);
            assert_eq!(r.user_stamp[stage].class, Class::Shared);
            assert_eq!(r.fw_stamp[stage].class, Class::Shared);
        }
        assert_eq!(r.sched_ctx.class, Class::Fw);
        assert_eq!(r.notifier.class, Class::Fw);
        assert_eq!(r.threshold.class, Class::Fw);
        assert_eq!(r.legacy_empty.class, Class::Shared);
        assert_eq!(r.registration_barrier.class, Class::Fw);
    }

    #[test]fn pb_heap_growth_rotates_to_expanded_epoch_and_falls_back_under_quarantine(){
        use g16::fw::buffer;
        use g16::hw::client;
        use g16::pools::{PbEpoch, Pools, alloc_pb_heap_chunk};

        // Demand-grown PB heap chunks map contiguously right after the 64 MiB base heap
        // inside PB_HEAP_SIZE (256 MiB) and reject out-of-range or zero-sized chunks.
        let mut o = Objects::new();
        let tier0 = alloc_pb_heap_chunk(&mut o, 512, 512).unwrap();
        let tier1 = alloc_pb_heap_chunk(&mut o, 1024, 1024).unwrap();
        assert_eq!(tier0.gva, Some(client::pb_heap(0) + 512 * buffer::BLOCK_BYTES));
        assert_eq!(tier0.size, 512 * buffer::BLOCK_BYTES as usize);
        assert_eq!(tier1.gva, Some(client::pb_heap(0) + 1024 * buffer::BLOCK_BYTES));
        assert_eq!(tier1.size, 1024 * buffer::BLOCK_BYTES as usize);
        assert!(alloc_pb_heap_chunk(&mut o, 512, 0).is_err());
        assert!(alloc_pb_heap_chunk(&mut o, 1024, 1025).is_err());

        // Start VM1 with a 512-block (64 MiB) warm epoch on slot 0.
        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let vm1 = vm(1);
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let e512 = PbEpoch::new(&factory, &vm1, g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap()).unwrap();
        e512.prepare(0, client::pb_heap(0), 512).unwrap();
        assert_eq!(e512.heap_blocks(), 512);
        e512.expose();
        let b0 = p.bind(0, false);
        p.publish_flight(0);
        p.retire_flight(0, &e512, b0, true).unwrap();

        // At 512 blocks VM1 pipelines cleanly on slot 0.
        assert!(p.can_pipeline_within_blocks(&vm1, 2, 512));
        assert_eq!(p.pick_slot_within_blocks(&vm1, true, 2, 512), Some(0));

        // Once VM1 expands its PB heap to 1024 blocks (128 MiB), slot 0's 512-block epoch
        // refuses pipelining so the engine drains once and rotates onto displaced slot 1.
        assert!(!p.can_pipeline_within_blocks(&vm1, 2, 1024));
        assert_eq!(p.pick_slot_within_blocks(&vm1, true, 2, 1024), None);
        assert_eq!(p.pick_slot_within_blocks(&vm1, false, 2, 1024), Some(1));
        assert!(p.reusable_for_within(0, &vm1, 2, 1024).is_none());

        let e1024 = PbEpoch::new(&factory, &vm1, g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap()).unwrap();
        e1024.prepare(1, client::pb_heap(0), 1024).unwrap();
        assert_eq!(e1024.heap_blocks(), 1024);
        e1024.expose();
        let b1 = p.bind(1, false);
        p.publish_flight(1);
        p.retire_flight(1, &e1024, b1, true).unwrap();

        // Slot 1 now pipelines at 1024 blocks; expanding to 2048 rotates cleanly back to displaced slot 0.
        assert!(p.can_pipeline_within_blocks(&vm1, 2, 1024));
        assert_eq!(p.pick_slot_within_blocks(&vm1, true, 2, 1024), Some(1));
        assert!(!p.can_pipeline_within_blocks(&vm1, 2, 2048));
        assert_eq!(p.pick_slot_within_blocks(&vm1, false, 2, 2048), Some(0));

        // Quarantine edge case: if slot 0 is quarantined so no displaced slot remains in 0..2,
        // VM1 safely falls back to reusing its warm 1024-block slot 1 instead of deadlocking.
        p.quarantine_flight(0, None);
        assert!(p.can_pipeline_within_blocks(&vm1, 2, 2048));
        assert_eq!(p.pick_slot_within_blocks(&vm1, true, 2, 2048), Some(1));
        assert!(p.reusable_for_within(1, &vm1, 2, 2048).is_some());
    }

    #[test]fn vm_affine_compute_claiming_keeps_four_interleaved_vms_warm(){
        let mut o = Objects::new();
        let mut s = Slots::new().unwrap();
        let mut units = KVec::new();
        for k in 0..g16::hw::sizes::COMPUTE_UNITS as u64 {
            units.push(g16::slots::ComputeSlot::new(&mut o, &g16::object::Factory, k, &crate::g16::object::test_unit_cmds(k)).unwrap(), 0).unwrap();
        }
        let prep = g16::resources::ComputeArena { compute: units, render: None, hw_slots: None };
        let vms = [vm(1), vm(2), vm(3), vm(4)];
        // Round 0 warms one unit per VM (4 units out of 24); VM2's retirement displaces VM1's
        // unit immediately so from round 1 onward every sequential interleaved claim is warm.
        for v in &vms {
            let mut c = Claims::new();
            let k = s.claim_compute_for(&prep, &mut c, Some(v.id)).unwrap().unwrap();
            assert!(s.compute_needs_full_work(k));
            s.hold_vm(&c, v).unwrap();
            s.publish(&mut c).unwrap();
            s.note_published(&c);
            s.retire(&mut c).unwrap();
        }
        assert_eq!(s.stats.cross_vm_retargets[0], 0);
        for _round in 0..4 {
            for v in &vms {
                let mut c = Claims::new();
                let k = s.claim_compute_for(&prep, &mut c, Some(v.id)).unwrap().unwrap();
                assert!(!s.compute_needs_full_work(k));
                s.hold_vm(&c, v).unwrap();
                s.publish(&mut c).unwrap();
                s.note_published(&c);
                s.retire(&mut c).unwrap();
            }
        }
        assert_eq!(s.stats.cross_vm_retargets[0], 0);
        // Pipelined depth-2 per VM across 4 concurrent VMs warms one additional unit per VM
        // from the unowned pool and then reuses warm VM-owned units with zero cross-VM scrubs.
        let mut live: Vec<Claims> = Vec::new();
        for (idx, v) in vms.iter().enumerate() {
            let mut c = Claims::new();
            let k = s.claim_compute_for(&prep, &mut c, Some(v.id)).unwrap().unwrap();
            if idx < 3 {
                assert!(!s.compute_needs_full_work(k));
            }
            s.hold_vm(&c, v).unwrap();
            s.publish(&mut c).unwrap();
            s.note_published(&c);
            live.push(c);
        }
        for _round in 0..4 {
            for (idx, v) in vms.iter().enumerate() {
                let mut c = Claims::new();
                let k = s.claim_compute_for(&prep, &mut c, Some(v.id)).unwrap().unwrap();
                if _round > 0 {
                    assert!(!s.compute_needs_full_work(k));
                }
                s.hold_vm(&c, v).unwrap();
                s.publish(&mut c).unwrap();
                s.note_published(&c);
                let mut old = std::mem::replace(&mut live[idx], c);
                s.retire(&mut old).unwrap();
            }
        }
        for mut c in live {
            s.retire(&mut c).unwrap();
        }
        assert_eq!(s.stats.cross_vm_retargets[0], 0);
    }

    #[test]fn warm_pb_pool_keeps_sixteen_concurrent_vms_warm_and_handles_overflow(){
        let warm = g16::hw::sizes::WARM_PB_SLOTS;
        assert_eq!(warm, 16);
        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let mk_epoch = |owner: &sync::Arc<g16::vm::VmState>, slot: usize| {
            let e = g16::pools::PbEpoch::new(
                &factory,
                owner,
                g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
            ).unwrap();
            e.prepare(slot, g16::hw::client::pb_heap(0), 512).unwrap();
            e.check_assignment(slot).unwrap();
            e.expose();
            e
        };
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let vms: Vec<_> = (0..20usize).map(|i| vm(100 + i as u64)).collect();
        let mask_for = |i: usize| -> (u32, usize) {
            let home = i % warm;
            let alt = (home + 1) % warm;
            ((1u32 << home) | (1u32 << alt), home)
        };
        // Prime 16 VMs arriving in scrambled (reverse) order: each claims its own home slot!
        let mut epochs = Vec::new();
        for i in (0..warm).rev() {
            let (mask, home) = mask_for(i);
            let slot = p.pick_slot_in_mask_blocks_mode(&vms[i], false, mask, home, 512, false).unwrap();
            assert_eq!(slot, home, "VM {i} must claim its home slot {home} despite reverse arrival");
            let e = mk_epoch(&vms[i], slot);
            let b = p.bind(slot, false);
            p.publish_flight(slot);
            p.retire_flight(slot, &e, b, true).unwrap();
            epochs.push((i, e));
        }
        // All 16 VMs now pipeline concurrently (behind=true) on 16 disjoint warm slots with 0 InitBM.
        let mut binds = Vec::new();
        for &(i, ref _e) in &epochs {
            let (mask, home) = mask_for(i);
            assert!(p.can_pipeline_in_mask_blocks_mode(&vms[i], mask, home, 512, false));
            let slot = p.pick_slot_in_mask_blocks_mode(&vms[i], true, mask, home, 512, false).unwrap();
            assert_eq!(slot, home);
            assert!(p.reusable_for_mask_mode(slot, &vms[i], mask, 512, false).is_some());
            let b = p.bind(slot, true);
            p.publish_flight(slot);
            binds.push((i, slot, b));
        }
        for (idx, (i, slot, b)) in binds.into_iter().enumerate() {
            assert_eq!(p.slot_inflight(slot), 1);
            p.retire_flight(slot, &epochs[idx].1, b, true).unwrap();
            assert_eq!(p.slot_inflight(slot), 0);
            let _ = i;
        }
        // Overflow d=20: VMs 16..20 share warm slots [0..4] cleanly on drained displaced rotation.
        for i in warm..20 {
            let (mask, home) = mask_for(i);
            assert!(!p.can_pipeline_in_mask_blocks_mode(&vms[i], mask, home, 512, false));
            let slot = p.pick_slot_in_mask_blocks_mode(&vms[i], false, mask, home, 512, false).unwrap();
            assert_ne!(mask & (1u32 << slot), 0);
            let e = mk_epoch(&vms[i], slot);
            let b = p.bind(slot, false);
            p.publish_flight(slot);
            p.retire_flight(slot, &e, b, true).unwrap();
        }
    }

    #[test]fn vm_affine_render_pass_claiming_keeps_sixteen_vms_warm_and_scales_tier1(){
        use g16::resources::{BASE_RENDER_SLOTS, EXT_RENDER_TIERS, target_render_slots_for};
        // Steady 60 Hz depth-1 VM stays at BASE_RENDER_SLOTS even when 16 render VMs are active.
        let (mut last, mut burst) = (0u64, 0u32);
        let (eff, single) = target_render_slots_for(&mut last, &mut burst, 10_000_000, 1, 0, false, BASE_RENDER_SLOTS, BASE_RENDER_SLOTS, 16);
        assert_eq!((eff, single), (BASE_RENDER_SLOTS, BASE_RENDER_SLOTS));
        // Bursting VM with 4 active render VMs stays at Tier 0 (8 slots).
        for step in 1..=3u64 {
            let (e, s) = target_render_slots_for(&mut last, &mut burst, 10_000_000 + step * 500_000, 1, 0, false, BASE_RENDER_SLOTS, BASE_RENDER_SLOTS, 4);
            if step < 3 {
                assert_eq!((e, s), (BASE_RENDER_SLOTS, BASE_RENDER_SLOTS));
            } else {
                assert_eq!((e, s), (EXT_RENDER_TIERS[0].1, EXT_RENDER_TIERS[0].1));
            }
        }
        // Once active_render_vms reaches 8 or 16, a bursting VM expands effective target to Tier 1
        // (18 slots) while keeping single_vm_target at Tier 0 (8) so PB_HEAP_EXT_TIERS[1] stays off.
        let (eff8, single8) = target_render_slots_for(&mut last, &mut burst, 12_000_000, 1, 0, false, EXT_RENDER_TIERS[0].1, EXT_RENDER_TIERS[0].1, 8);
        assert_eq!((eff8, single8), (EXT_RENDER_TIERS[1].1, EXT_RENDER_TIERS[0].1));
        let (eff16, single16) = target_render_slots_for(&mut last, &mut burst, 12_500_000, 1, 0, false, eff8, single8, 16);
        assert_eq!((eff16, single16), (EXT_RENDER_TIERS[1].1, EXT_RENDER_TIERS[0].1));
        // Heavy single-VM multi-pass (8 passes) still advances single_vm_target to Tier 1.
        let (eff_heavy, single_heavy) = target_render_slots_for(&mut last, &mut burst, 13_000_000, 8, 0, false, eff16, single16, 16);
        assert_eq!((eff_heavy, single_heavy), (EXT_RENDER_TIERS[1].1, EXT_RENDER_TIERS[1].1));

        // 16 concurrent VMs across 18 pass slots stay on 16 disjoint warm pass slots with 0 cross-VM retargets.
        let mut o = Objects::new();
        let mut s = Slots::new().unwrap();
        let mut passes = KVec::new();
        for k in 0..EXT_RENDER_TIERS[1].1 {
            passes.push(g16::slots::PassSlot::new(&mut o, k as u64, 1, &crate::g16::object::test_cmds((k as u64) as u64)).unwrap(), 0).unwrap();
        }
        let render = sync::Arc::new(render_arena(passes), 0).unwrap();
        let prep = g16::resources::ComputeArena {
            compute: KVec::new(),
            render: Some(render),
            hw_slots: None,
        };
        let geo = g16::encode::Geometry { tpc_bytes: 0x1000, tilemap_bytes: 0x2000 };
        let vms: Vec<_> = (0..16u64).map(|i| vm(200 + i)).collect();
        let mut vm_slots = [usize::MAX; 16];
        for (i, v) in vms.iter().enumerate() {
            let k = s.pick_pass_in_for(&prep, &geo, 0, Some(v.id)).unwrap();
            s.claim_pass_in(&prep, k, Some(v.id), geo.tilemap_bytes).unwrap();
            assert!(s.render_needs_full_work(k));
            let mut c = Claims::new();
            c.pass[0] = k as u32; c.pass_count = 1; c.pass_mask = 1u64 << k;
            s.hold_render_pass(k, v, &prep).unwrap();
            s.publish(&mut c).unwrap();
            s.retire(&mut c).unwrap();
            vm_slots[i] = k;
        }
        assert_eq!(s.stats.cross_vm_retargets[1], 0);
        // Subsequent interleaved rounds across all 16 VMs hit their own warm pass slots every time!
        for _round in 0..4 {
            for (i, v) in vms.iter().enumerate() {
                let k = s.pick_pass_in_for(&prep, &geo, 0, Some(v.id)).unwrap();
                assert_eq!(k, vm_slots[i], "VM {i} must stay on its warm pass slot");
                s.claim_pass_in(&prep, k, Some(v.id), geo.tilemap_bytes).unwrap();
                assert!(!s.render_needs_full_work(k));
                let mut c = Claims::new();
                c.pass[0] = k as u32; c.pass_count = 1; c.pass_mask = 1u64 << k;
                s.hold_render_pass(k, v, &prep).unwrap();
                s.publish(&mut c).unwrap();
                s.retire(&mut c).unwrap();
            }
        }
        assert_eq!(s.stats.cross_vm_retargets[1], 0);
    }

    #[test]fn pb_epoch_prepare_seeds_active_prefix_and_clears_shrink_tail(){
        use g16::fw::buffer;
        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let v = vm(300);
        g16::object::ALL_OBJECTS.with(|a| a.borrow_mut().clear());
        let e = g16::pools::PbEpoch::new(
            &factory,
            &v,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        ).unwrap();
        let (page_obj, block_obj) = g16::object::ALL_OBJECTS.with(|a| {
            let objs = a.borrow();
            (
                objs.iter().find(|o| o.gva.is_some() && o.size == buffer::PAGE_LIST_SIZE).unwrap().clone(),
                objs.iter().find(|o| o.gva.is_none() && o.size == buffer::BLOCK_LIST_SIZE).unwrap().clone(),
            )
        });
        let heap = g16::hw::client::pb_heap(0);
        // First prepare for 512 blocks writes ONLY 512*4*4 = 8192 page bytes and 512*8 = 4096 block bytes.
        e.prepare(0, heap, 512).unwrap();
        assert_eq!(page_obj.write_bytes.load(std::sync::atomic::Ordering::Relaxed), 512 * 4 * 4);
        assert_eq!(block_obj.write_bytes.load(std::sync::atomic::Ordering::Relaxed), 512 * 8);
        assert_eq!(page_obj.r32(512 * 4 * 4).unwrap(), 0);
        assert_eq!(block_obj.r32(512 * 8).unwrap(), 0);
        // Grow unpublished epoch to 1024 blocks, then shrink retry back to 512 blocks:
        // tail [512..1024] must be zeroed cleanly.
        e.prepare(1, heap, 1024).unwrap();
        assert_ne!(page_obj.r32(512 * 4 * 4).unwrap(), 0);
        assert_ne!(block_obj.r32(512 * 8).unwrap(), 0);
        e.prepare(2, heap, 512).unwrap();
        assert_eq!(page_obj.r32(512 * 4 * 4).unwrap(), 0);
        assert_eq!(block_obj.r32(512 * 8).unwrap(), 0);
        assert_eq!(page_obj.r32(1023 * 4 * 4).unwrap(), 0);
        assert_eq!(block_obj.r32(1023 * 8).unwrap(), 0);
    }

    #[test]fn closed_vms_reclaim_displaced_pb_render_and_compute_slots(){
        use g16::hw::sizes::{COMPUTE_UNITS, WARM_PB_SLOTS};
        use g16::resources::{COMPUTE_OWNER_ARENA, ComputeArena, PbOwnerArena};
        use g16::slots::{ComputeSlot, PassSlot};

        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let mut s = Slots::new().unwrap();
        let mut o = Objects::new();

        let mut compute_slots = KVec::new();
        for k in 0..COMPUTE_UNITS {
            compute_slots.push(ComputeSlot::new(&mut o, &g16::object::Factory, k as u64, &crate::g16::object::test_unit_cmds(k as u64)).unwrap(), 0).unwrap();
        }
        let mut passes = KVec::new();
        for k in 0..18 {
            passes.push(PassSlot::new(&mut o, k as u64, 1, &crate::g16::object::test_cmds((k as u64) as u64)).unwrap(), 0).unwrap();
        }
        let shared_prep_compute = ComputeArena { compute: compute_slots, render: None, hw_slots: None };
        let geo = g16::encode::Geometry { tpc_bytes: 0x1000, tilemap_bytes: 0x2000 };

        let mut vms = Vec::new();
        let mut epochs = Vec::new();
        let mut pb_arenas = Vec::new();
        let mut pass_arenas = Vec::new();
        let mut comp_arenas = Vec::new();

        // Bind and retire 16 concurrent VMs across 16 PB slots, 16 render passes, and 16 compute units.
        for i in 0..WARM_PB_SLOTS {
            let v = vm(400 + i as u64);
            let pb_arena = sync::Arc::new(render_arena(KVec::new()), 0).unwrap();
            let mut r_passes = KVec::new();
            for k in 0..18 {
                r_passes.push(PassSlot::new(&mut o, k as u64, 1, &crate::g16::object::test_cmds((k as u64) as u64)).unwrap(), 0).unwrap();
            }
            let r_arena = sync::Arc::new(render_arena(r_passes), 0).unwrap();
            let r_prep = ComputeArena { compute: KVec::new(), render: Some(r_arena.clone()), hw_slots: None };
            let c_arena = sync::Arc::new(
                ComputeArena { compute: KVec::new(), render: None, hw_slots: None }, 0,
            ).unwrap();

            // 1) PB warm slot i
            let mask = (1u32 << i) | (1u32 << ((i + 1) % WARM_PB_SLOTS));
            let pb_slot = p
                .pick_slot_in_mask_blocks_mode(&v, false, mask, i, 512, false)
                .unwrap();
            assert_eq!(pb_slot, i);
            let e = g16::pools::PbEpoch::new(
                &factory,
                &v,
                g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
            ).unwrap();
            e.prepare(pb_slot, g16::hw::client::pb_heap(0), 512).unwrap();
            e.expose();
            let b = p.bind(pb_slot, false);
            p.hold_alloc(pb_slot, &v, Some(PbOwnerArena::Base(pb_arena.clone())), None).unwrap();
            p.publish_flight(pb_slot);
            p.retire_flight(pb_slot, &e, b, true).unwrap();

            // 2) Render pass slot i
            let pk = s.pick_pass_in_for(&r_prep, &geo, 0, Some(v.id)).unwrap();
            s.claim_pass_in(&r_prep, pk, Some(v.id), geo.tilemap_bytes).unwrap();
            let mut rc = Claims::new();
            rc.pass[0] = pk as u32;
            rc.pass_count = 1;
            rc.pass_mask = 1u64 << pk;
            s.hold_render_pass(pk, &v, &r_prep).unwrap();
            s.publish(&mut rc).unwrap();
            s.retire(&mut rc).unwrap();

            // 3) Compute unit i
            let mut cc = Claims::new();
            let ck = s
                .claim_compute_for(&shared_prep_compute, &mut cc, Some(v.id))
                .unwrap()
                .unwrap();
            COMPUTE_OWNER_ARENA.with(|a| *a.borrow_mut() = Some(c_arena.clone()));
            s.hold_compute_slot(ck, &v, &shared_prep_compute).unwrap();
            COMPUTE_OWNER_ARENA.with(|a| *a.borrow_mut() = None);
            s.publish(&mut cc).unwrap();
            s.retire(&mut cc).unwrap();

            vms.push(v);
            epochs.push(e);
            pb_arenas.push(pb_arena);
            pass_arenas.push(r_arena);
            comp_arenas.push(c_arena);
        }

        // Live VMs stay pinned and reusable across reclaim_closed().
        assert!(!p.reclaim_closed());
        assert!(!s.reclaim_closed());
        for i in 0..WARM_PB_SLOTS {
            assert!(sync::Arc::strong_count(&pb_arenas[i]) > 1);
            assert!(sync::Arc::strong_count(&pass_arenas[i]) > 1);
            assert!(sync::Arc::strong_count(&comp_arenas[i]) > 1);
            assert!(p.reusable(i, &vms[i]).is_some());
        }

        // Close all 16 VMs: displaced slots 0..15 reclaim immediately; undisplaced slot 15 stays.
        for v in &vms {
            v.vm.close();
        }
        assert!(p.reclaim_closed(), "undisplaced PB slot 15 remains");
        assert!(s.reclaim_closed(), "undisplaced render/compute slot 15 remain");
        // Displaced pass scrubs must skip closed VMs.
        s.stage_displaced_pass_scrubs();
        assert!(!s.scrubbing_active());

        for i in 0..(WARM_PB_SLOTS - 1) {
            assert_eq!(sync::Arc::strong_count(&pb_arenas[i]), 1, "PB arena {i} reclaimed");
            // pb_mgr_table[i] still names this epoch's manager: it stays until replaced.
            assert_eq!(sync::Arc::strong_count(&epochs[i]), 2, "PB epoch {i} still bound");
            assert_eq!(sync::Arc::strong_count(&pass_arenas[i]), 1, "pass arena {i} reclaimed");
            assert_eq!(sync::Arc::strong_count(&comp_arenas[i]), 1, "compute arena {i} reclaimed");
            // Drop our local epoch ref to confirm VmState itself has only our Vec ref left.
            assert_eq!(sync::Arc::strong_count(&vms[i]), 2);
        }
        // Final undisplaced slot 15 stays protected until displaced.
        let last = WARM_PB_SLOTS - 1;
        assert_eq!(sync::Arc::strong_count(&pb_arenas[last]), 2);
        assert_eq!(sync::Arc::strong_count(&pass_arenas[last]), 2);
        assert_eq!(sync::Arc::strong_count(&comp_arenas[last]), 2);

        // Live VM 17 runs 1 render + 1 compute, displacing slot 15 everywhere.
        let live = vm(500);
        let live_r_arena = sync::Arc::new(render_arena(passes), 0).unwrap();
        let live_r_prep = ComputeArena { compute: KVec::new(), render: Some(live_r_arena.clone()), hw_slots: None };
        let live_slot = p
            .pick_slot_in_mask_blocks_mode(&live, false, 0b11, 0, 512, false)
            .unwrap();
        assert_eq!(live_slot, 0);
        let live_e = g16::pools::PbEpoch::new(
            &factory,
            &live,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        ).unwrap();
        live_e.prepare(live_slot, g16::hw::client::pb_heap(0), 512).unwrap();
        live_e.expose();
        let lb = p.bind(live_slot, false);
        p.hold_alloc(live_slot, &live, Some(PbOwnerArena::Base(live_r_arena.clone())), None).unwrap();
        p.publish_flight(live_slot);
        p.retire_flight(live_slot, &live_e, lb, true).unwrap();

        let lpk = s.pick_pass_in_for(&live_r_prep, &geo, 0, Some(live.id)).unwrap();
        s.claim_pass_in(&live_r_prep, lpk, Some(live.id), geo.tilemap_bytes).unwrap();
        let mut lrc = Claims::new();
        lrc.pass[0] = lpk as u32;
        lrc.pass_count = 1;
        lrc.pass_mask = 1u64 << lpk;
        s.hold_render_pass(lpk, &live, &live_r_prep).unwrap();
        s.publish(&mut lrc).unwrap();
        s.retire(&mut lrc).unwrap();

        let mut lcc = Claims::new();
        let lck = s
            .claim_compute_for(&shared_prep_compute, &mut lcc, Some(live.id))
            .unwrap()
            .unwrap();
        s.hold_compute_slot(lck, &live, &shared_prep_compute).unwrap();
        s.publish(&mut lcc).unwrap();
        s.retire(&mut lcc).unwrap();

        // Now slot 15 is displaced -> reclaim_closed() evicts it and returns false (0 remaining).
        assert!(!p.reclaim_closed());
        assert!(!s.reclaim_closed());
        assert_eq!(sync::Arc::strong_count(&pb_arenas[last]), 1);
        assert_eq!(sync::Arc::strong_count(&pass_arenas[last]), 1);
        assert_eq!(sync::Arc::strong_count(&comp_arenas[last]), 1);
        // VM 17's epoch replaced slot 0's; the other slots keep their closed owners' epochs.
        assert_eq!(sync::Arc::strong_count(&epochs[0]), 1, "replaced PB epoch 0 released");
        for i in 1..WARM_PB_SLOTS {
            assert_eq!(sync::Arc::strong_count(&epochs[i]), 2, "PB epoch {i} still bound");
        }
        // Each epoch is released once a newer epoch retires on its own slot.
        for i in 1..WARM_PB_SLOTS {
            let next = vm(520 + i as u64);
            let e = g16::pools::PbEpoch::new(
                &factory,
                &next,
                g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
            ).unwrap();
            e.prepare(i, g16::hw::client::pb_heap(0), 512).unwrap();
            e.expose();
            let b = p.bind(i, false);
            p.hold_alloc(i, &next, None, None).unwrap();
            p.publish_flight(i);
            p.retire_flight(i, &e, b, true).unwrap();
            assert_eq!(sync::Arc::strong_count(&epochs[i]), 1, "replaced PB epoch {i} released");
        }
        assert!(!p.reclaim_closed());
        drop(epochs);
        for (i, v) in vms.iter().enumerate() {
            assert_eq!(sync::Arc::strong_count(v), 1, "closed VM {i} fully released");
        }

        // Active off-lock scrub on a closed VM's displaced pass slot waits for scrub completion.
        let mut s2 = Slots::new().unwrap();
        let mut sc_passes = KVec::new();
        for k in 0..2 {
            sc_passes.push(PassSlot::new(&mut o, k as u64, 1, &crate::g16::object::test_cmds((k as u64) as u64)).unwrap(), 0).unwrap();
        }
        let sc_arena = sync::Arc::new(render_arena(sc_passes), 0).unwrap();
        let sc_prep = ComputeArena { compute: KVec::new(), render: Some(sc_arena.clone()), hw_slots: None };
        let va = vm(600);
        let vb = vm(601);
        for (k, owner) in [(0usize, &va), (1usize, &vb)] {
            s2.claim_pass_in(&sc_prep, k, Some(owner.id), 0x2000).unwrap();
            sc_prep.set_pass_dirty(k, 0x1000, 0x2000, 0x200).unwrap();
            let mut c = Claims::new();
            c.pass[0] = k as u32;
            c.pass_count = 1;
            c.pass_mask = 1u64 << k;
            s2.hold_render_pass(k, owner, &sc_prep).unwrap();
            s2.publish(&mut c).unwrap();
            s2.retire(&mut c).unwrap();
        }
        // Stage off-lock scrub on displaced slot 0 while va is still open, then close va.
        s2.stage_displaced_pass_scrubs();
        assert!(s2.scrubbing_active());
        let batch = s2.take_staged_scrubs();
        va.vm.close();
        assert!(s2.reclaim_closed(), "must not evict slot 0 mid-scrub");
        assert_eq!(sync::Arc::strong_count(&va), 2);
        // Running the scrub wakes the reclaim waiter, then reclaim_closed() evicts slot 0.
        assert!(drain_scrubs(batch));
        assert!(s2.take_scrub_woke());
        assert!(!s2.reclaim_closed());
        assert_eq!(sync::Arc::strong_count(&va), 1);

        // Quarantined closed-VM slots stay pinned forever AND do not keep reclaim_closed() armed.
        let vq = vm(700);
        let mut pq = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let eq = g16::pools::PbEpoch::new(
            &factory,
            &vq,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        ).unwrap();
        pq.hold_alloc(0, &vq, Some(PbOwnerArena::Base(sc_arena.clone())), None).unwrap();
        pq.publish_flight(0);
        pq.quarantine_flight(0, Some(eq.clone()));
        let mut cq = Claims::new();
        let qk = s2.claim_compute_for(&shared_prep_compute, &mut cq, Some(vq.id)).unwrap().unwrap();
        s2.hold_compute_slot(qk, &vq, &shared_prep_compute).unwrap();
        s2.publish(&mut cq).unwrap();
        s2.quarantine(&mut cq);
        vq.vm.close();
        assert!(!pq.reclaim_closed(), "quarantined PB slot must not spin reclaim");
        assert!(!s2.reclaim_closed(), "quarantined compute slot must not spin reclaim");
        assert!(sync::Arc::strong_count(&vq) > 1, "quarantined leases stay retained");
    }

    #[test]fn dual_pipe_closed_compute_vms_pin_only_one_unit_per_pipe(){
        use g16::hw::sizes::COMPUTE_UNITS;
        use g16::resources::{COMPUTE_OWNER_ARENAS, ComputeArena};
        use g16::slots::ComputeSlot;

        let mut o = Objects::new();
        let mut s = Slots::new().unwrap();
        let mut compute_slots = KVec::new();
        for k in 0..COMPUTE_UNITS {
            compute_slots.push(ComputeSlot::new(&mut o, &g16::object::Factory, k as u64, &crate::g16::object::test_unit_cmds(k as u64)).unwrap(), 0).unwrap();
        }
        let prep = ComputeArena { compute: compute_slots, render: None, hw_slots: None };

        // Simulate 8 compute VMs, each owning 24 independent per-unit ComputeArenas
        // (matching production ComputeUnitSet), running across BOTH CDM pipes (0 and 1).
        let mut vms = Vec::new();
        let mut vm_unit_arenas: Vec<Vec<sync::Arc<ComputeArena>>> = Vec::new();
        for i in 0..8usize {
            let v = vm(800 + i as u64);
            let mut per_unit = Vec::with_capacity(COMPUTE_UNITS);
            for _ in 0..COMPUTE_UNITS {
                per_unit.push(
                    sync::Arc::new(
                        ComputeArena { compute: KVec::new(), render: None, hw_slots: None },
                        0,
                    )
                    .unwrap(),
                );
            }
            COMPUTE_OWNER_ARENAS.with(|a| *a.borrow_mut() = Some(per_unit.clone()));
            for pipe in [0usize, 1usize] {
                let mut c = Claims::new();
                let k = s.claim_compute_for_pipe(&prep, &mut c, Some(v.id), pipe).unwrap().unwrap();
                s.hold_compute_slot(k, &v, &prep).unwrap();
                s.publish(&mut c).unwrap();
                s.retire(&mut c).unwrap();
            }
            COMPUTE_OWNER_ARENAS.with(|a| *a.borrow_mut() = None);
            vms.push(v);
            vm_unit_arenas.push(per_unit);
        }

        // Close all 8 VMs and reclaim displaced leases:
        // - Pipe 0 and pipe 1 each retain 1 undisplaced last-retired unit -> reusable == 22.
        // - Out of 8 * 24 = 192 per-unit arenas, ONLY those 2 undisplaced units remain pinned
        //   (strong_count == 2); all 190 other units (including the other 23 units of each
        //   last-retired VM) drop immediately (strong_count == 1).
        for v in &vms {
            v.vm.close();
        }
        assert!(s.reclaim_closed(), "two undisplaced dual-pipe units remain");
        assert_eq!(s.lifetime_counts()[0].reusable, (COMPUTE_UNITS - 2) as u32);
        let pinned_units: usize = vm_unit_arenas
            .iter()
            .flat_map(|units| units.iter())
            .filter(|arena| sync::Arc::strong_count(arena) > 1)
            .count();
        assert_eq!(pinned_units, 2, "must pin exactly 1 unit per CDM pipe, never a whole VM set");
        for (i, units) in vm_unit_arenas.iter().enumerate() {
            let vm_pinned = units.iter().filter(|a| sync::Arc::strong_count(a) > 1).count();
            if i == 7 {
                assert_eq!(vm_pinned, 2, "last VM holds only its 2 undisplaced units");
            } else {
                assert_eq!(vm_pinned, 0, "displaced closed VM {i} holds 0 units");
                assert_eq!(sync::Arc::strong_count(&vms[i]), 1, "closed VM {i} fully released");
            }
        }

        // Displace pipe 0 and pipe 1 with a live VM -> all 8 closed VMs and 192 units release.
        let live = vm(900);
        for pipe in [0usize, 1usize] {
            let mut c = Claims::new();
            let k = s.claim_compute_for_pipe(&prep, &mut c, Some(live.id), pipe).unwrap().unwrap();
            s.hold_compute_slot(k, &live, &prep).unwrap();
            s.publish(&mut c).unwrap();
            s.retire(&mut c).unwrap();
        }
        assert!(!s.reclaim_closed(), "both dual-pipe closed leases displaced and reclaimed");
        let pinned_after: usize = vm_unit_arenas
            .iter()
            .flat_map(|units| units.iter())
            .filter(|arena| sync::Arc::strong_count(arena) > 1)
            .count();
        assert_eq!(pinned_after, 0);
        for (i, v) in vms.iter().enumerate() {
            assert_eq!(sync::Arc::strong_count(v), 1, "closed VM {i} fully released after displacement");
        }
    }

    #[test]fn closed_vm_reclaim_retains_firmware_pb_and_uma_leases_until_overwritten(){
        use g16::hw::sizes::{COMPUTE_UNITS, WARM_PB_SLOTS};
        use g16::resources::{COMPUTE_OWNER_ARENA, ComputeArena, PbOwnerArena};
        use g16::slots::ComputeSlot;

        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let mut s = Slots::new().unwrap();
        let mut o = Objects::new();

        // VM1 primes PB/UMA slot 0 and compute unit 0; VM2 displaces both via slot 1 / unit 1.
        let vm1 = vm(1001);
        let vm2 = vm(1002);
        let vm3 = vm(1003);

        let pools1 = PoolSet::new_with_slots(&mut o, &factory, [0, 1]).unwrap();
        let uma1 = pools1.unit(0).unwrap().uma_lease();
        let r_arena1 = sync::Arc::new(render_arena(KVec::new()), 0).unwrap();
        let e1 = g16::pools::PbEpoch::new(
            &factory,
            &vm1,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        ).unwrap();
        let pb_lease1 = e1.records_lease();
        e1.prepare(0, g16::hw::client::pb_heap(0), 512).unwrap();
        e1.expose();
        let b1 = p.bind(0, false);
        p.hold_alloc(0, &vm1, Some(PbOwnerArena::Base(r_arena1.clone())), Some(uma1.clone())).unwrap();
        p.publish_flight(0);
        p.retire_flight(0, &e1, b1, true).unwrap();

        let pools2 = PoolSet::new_with_slots(&mut o, &factory, [1, 2]).unwrap();
        let uma2 = pools2.unit(0).unwrap().uma_lease();
        let r_arena2 = sync::Arc::new(render_arena(KVec::new()), 0).unwrap();
        let e2 = g16::pools::PbEpoch::new(
            &factory,
            &vm2,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        ).unwrap();
        e2.prepare(1, g16::hw::client::pb_heap(0), 512).unwrap();
        e2.expose();
        let b2 = p.bind(1, false);
        p.hold_alloc(1, &vm2, Some(PbOwnerArena::Base(r_arena2.clone())), Some(uma2.clone())).unwrap();
        p.publish_flight(1);
        p.retire_flight(1, &e2, b2, true).unwrap();

        // Compute unit 0 owned by VM1, displaced by VM2 on unit 1.
        let mut units1 = KVec::new();
        for k in 0..COMPUTE_UNITS {
            units1.push(ComputeSlot::new(&mut o, &factory, k as u64, &crate::g16::object::test_unit_cmds(k as u64)).unwrap(), 0).unwrap();
        }
        let c_uma1 = units1[0].uma_lease();
        let prep1 = ComputeArena { compute: units1, render: None, hw_slots: None };
        let c_arena1 = sync::Arc::new(
            ComputeArena { compute: KVec::new(), render: None, hw_slots: None },
            0,
        )
        .unwrap();
        let mut cc1 = Claims::new();
        assert_eq!(s.claim_compute_for(&prep1, &mut cc1, Some(vm1.id)).unwrap(), Some(0));
        COMPUTE_OWNER_ARENA.with(|a| *a.borrow_mut() = Some(c_arena1.clone()));
        s.hold_compute_slot(0, &vm1, &prep1).unwrap();
        COMPUTE_OWNER_ARENA.with(|a| *a.borrow_mut() = None);
        s.publish(&mut cc1).unwrap();
        s.retire(&mut cc1).unwrap();

        let mut units2 = KVec::new();
        for k in 0..COMPUTE_UNITS {
            units2.push(ComputeSlot::new(&mut o, &factory, k as u64, &crate::g16::object::test_unit_cmds(k as u64)).unwrap(), 0).unwrap();
        }
        let prep2 = ComputeArena { compute: units2, render: None, hw_slots: None };
        let mut cc2 = Claims::new();
        assert_eq!(s.claim_compute_for(&prep2, &mut cc2, Some(vm2.id)).unwrap(), Some(1));
        s.hold_compute_slot(1, &vm2, &prep2).unwrap();
        s.publish(&mut cc2).unwrap();
        s.retire(&mut cc2).unwrap();

        let mut units3 = KVec::new();
        units3.push(ComputeSlot::new(&mut o, &factory, 0, &crate::g16::object::test_unit_cmds(0)).unwrap(), 0).unwrap();
        let c_uma3 = units3[0].uma_lease();
        let prep3 = ComputeArena { compute: units3, render: None, hw_slots: None };

        // Drop local arena/epoch/pool handles for VM1 and close VM1.
        drop(e1);
        drop(pools1);
        drop(prep1);
        vm1.vm.close();

        // Reclaim evicts VM1's RenderArena and ComputeArena immediately and retains its
        // firmware PB & UMA leases until overwritten. PB slot 0's epoch keeps VM1's VmState
        // (TTBAT slot) and page list until a newer epoch retires on slot 0.
        assert!(!p.reclaim_closed());
        assert!(!s.reclaim_closed());
        assert_eq!(sync::Arc::strong_count(&vm1), 2, "VM1 TTBAT slot stays with its PB epoch");
        assert_eq!(sync::Arc::strong_count(&r_arena1), 1, "VM1 RenderArena/PB heap must release");
        assert_eq!(sync::Arc::strong_count(&c_arena1), 1, "VM1 ComputeArena/32MiB pool must release");
        assert_eq!(sync::Arc::strong_count(&pb_lease1), 3, "pb_mgr_table[0] lease held by epoch + retained_pb");
        assert_eq!(sync::Arc::strong_count(&uma1), 2, "render uma_mgr_table[33] lease must stay mapped");
        assert_eq!(sync::Arc::strong_count(&c_uma1), 2, "compute uma_mgr_table[1] lease must stay mapped");

        // Cancelling an unpublished replacement on slot 0 / unit 0 must NOT release VM1's leases.
        let pools3 = PoolSet::new_with_slots(&mut o, &factory, [0, 2]).unwrap();
        let uma3 = pools3.unit(0).unwrap().uma_lease();
        let r_arena3 = sync::Arc::new(render_arena(KVec::new()), 0).unwrap();
        p.hold_alloc(0, &vm3, Some(PbOwnerArena::Base(r_arena3.clone())), Some(uma3.clone())).unwrap();
        p.cancel_alloc(0);
        assert_eq!(sync::Arc::strong_count(&uma1), 2);
        assert_eq!(sync::Arc::strong_count(&uma3), 2);
        assert_eq!(sync::Arc::strong_count(&vm1), 2);

        let mut cc3_cancel = Claims::new();
        assert_eq!(s.claim_compute_for(&prep3, &mut cc3_cancel, Some(vm3.id)).unwrap(), Some(0));
        s.hold_compute_slot(0, &vm3, &prep3).unwrap();
        s.release(&mut cc3_cancel);
        assert_eq!(sync::Arc::strong_count(&c_uma1), 2);

        // Once VM3 retires on exact hardware slot 0 / unit 0, firmware tables are overwritten and
        // VM1's old FwLeases drop cleanly while VM3's leases become retained.
        let e3 = g16::pools::PbEpoch::new(
            &factory,
            &vm3,
            g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
        ).unwrap();
        let pb_lease3 = e3.records_lease();
        e3.prepare(0, g16::hw::client::pb_heap(0), 512).unwrap();
        e3.expose();
        let b3 = p.bind(0, false);
        p.hold_alloc(0, &vm3, Some(PbOwnerArena::Base(r_arena3)), Some(uma3.clone())).unwrap();
        p.publish_flight(0);
        p.retire_flight(0, &e3, b3, true).unwrap();
        assert_eq!(sync::Arc::strong_count(&pb_lease1), 1, "overwritten pb_mgr_table[0] lease dropped");
        assert_eq!(sync::Arc::strong_count(&vm1), 1, "replaced PB epoch releases VM1's TTBAT slot");
        assert_eq!(sync::Arc::strong_count(&uma1), 1, "overwritten render uma_mgr_table[33] lease dropped");
        assert_eq!(sync::Arc::strong_count(&pb_lease3), 3, "new PB lease held by epoch + retained_pb");
        assert_eq!(sync::Arc::strong_count(&uma3), 3, "new render UMA lease held by pool + retained_uma");

        let mut cc3 = Claims::new();
        assert_eq!(s.claim_compute_for(&prep3, &mut cc3, Some(vm3.id)).unwrap(), Some(0));
        s.hold_compute_slot(0, &vm3, &prep3).unwrap();
        s.publish(&mut cc3).unwrap();
        s.retire(&mut cc3).unwrap();
        assert_eq!(sync::Arc::strong_count(&c_uma1), 1, "overwritten compute uma_mgr_table[1] lease dropped");
        assert_eq!(sync::Arc::strong_count(&c_uma3), 3, "new compute UMA lease held by unit + retained_uma");
        let _ = WARM_PB_SLOTS;
    }


    // A closed VM's PB slot 3 is displaced by slot 4 and reclaimed. pb_mgr_table[3] still names
    // its epoch's manager; gPM_SP(0) wrote that manager's page list (DYNAMIC, 0x704000cb40)
    // through ctx 46 after the epoch was gone: "write unmapped level 0 unit 0xe0 ctx 46".
    #[test]fn closed_vm_pb_epoch_keeps_page_list_and_context_until_slot_replacement_retires(){
        use g16::resources::PbOwnerArena;

        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let epoch = |v: &sync::Arc<g16::vm::VmState>, k: usize| {
            let e = g16::pools::PbEpoch::new(
                &factory,
                v,
                g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
            ).unwrap();
            e.prepare(k, g16::hw::client::pb_heap(0), 512).unwrap();
            e.expose();
            e
        };

        // VM X (ctx 46) runs and retires on PB slot 3; VM Y then retires on slot 4.
        let x = vm(46);
        let x_arena = sync::Arc::new(render_arena(KVec::new()), 0).unwrap();
        let ex = epoch(&x, 3);
        let x_records = ex.records_lease();
        let bx = p.bind(3, false);
        p.hold_alloc(3, &x, Some(PbOwnerArena::Base(x_arena.clone())), None).unwrap();
        p.publish_flight(3);
        p.retire_flight(3, &ex, bx, true).unwrap();
        let y = vm(47);
        let ey = epoch(&y, 4);
        let by = p.bind(4, false);
        p.hold_alloc(4, &y, None, None).unwrap();
        p.publish_flight(4);
        p.retire_flight(4, &ey, by, true).unwrap();

        // X closes. Its arena and PB heap go, but not the epoch firmware still names.
        x.vm.close();
        assert!(!p.reclaim_closed(), "a retained owner epoch alone must not keep reclaim armed");
        assert!(!p.reclaim_closed());
        assert_eq!(sync::Arc::strong_count(&x_arena), 1, "closed VM's render arena reclaimed");
        assert_eq!(sync::Arc::strong_count(&ex), 2, "slot 3 epoch (page list) reclaimed early");
        assert_eq!(sync::Arc::strong_count(&x), 2, "ctx 46 released while its lists are named");
        assert!(sync::Arc::strong_count(&x_records) >= 2);
        assert!(p.reusable(3, &x).is_none());

        // A new VM's InitBM epoch rebinds slot 3. The old binding stays intact while that
        // render is in flight and goes only with its full retirement.
        let z = vm(48);
        assert_eq!(p.pick_slot_in_mask_blocks_mode(&z, false, 0b11000, 3, 512, false), Some(3));
        let ez = epoch(&z, 3);
        let bz = p.bind(3, false);
        p.hold_alloc(3, &z, None, None).unwrap();
        p.publish_flight(3);
        assert!(p.reclaim_closed(), "rebinding render still in flight");
        assert_eq!(sync::Arc::strong_count(&ex), 2);
        assert_eq!(sync::Arc::strong_count(&x), 2);
        p.retire_flight(3, &ez, bz, true).unwrap();
        assert_eq!(sync::Arc::strong_count(&ex), 1, "replaced epoch released");
        drop(ex);
        assert_eq!(sync::Arc::strong_count(&x), 1, "closed VM released after replacement");
        assert_eq!(sync::Arc::strong_count(&x_records), 1);
        assert!(!p.reclaim_closed());
    }

    // Live cross-VM steal of a warm PB slot (display manager active: kwin, its helpers and 16
    // clients over 16 warm slots). VM A's epoch owns slot 3. VM B's fresh epoch rebinds slot 3
    // and retires, replacing owners[3] while A is still open. A's next render is a cold InitBM
    // on slot 4, the only flight; gPM_SP(0) then writes the old epoch's page list through A's
    // context: "write unmapped level 0 unit 0xe0 ctx 62 ... flights=1 live_vm=Some(108)".
    #[test]fn live_vm_displaced_pb_epoch_keeps_page_list_until_its_next_render_retires(){
        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let epoch = |v: &sync::Arc<g16::vm::VmState>, k: usize| {
            let e = g16::pools::PbEpoch::new(
                &factory,
                v,
                g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
            ).unwrap();
            e.prepare(k, g16::hw::client::pb_heap(0), 512).unwrap();
            e.expose();
            e
        };
        let run = |p: &mut g16::pools::Pools, v: &sync::Arc<g16::vm::VmState>, k: usize,
                   e: &sync::Arc<g16::pools::PbEpoch>, completed: bool| {
            let b = p.bind(k, false);
            p.hold_alloc(k, v, None, None).unwrap();
            p.publish_flight(k);
            p.retire_flight(k, e, b, completed).unwrap();
        };

        let a = vm(62);
        let b = vm(5);
        let ea1 = epoch(&a, 3);
        run(&mut p, &a, 3, &ea1, true);
        let eb = epoch(&b, 3);
        run(&mut p, &b, 3, &eb, true);
        assert!(p.reusable(3, &a).is_none(), "slot 3 is bound to B's epoch");
        assert_eq!(sync::Arc::strong_count(&ea1), 2, "A's page list unmapped while A is open");
        assert_eq!(p.stats.kept, 1);

        // A's next render binds slot 4 with InitBM. The old list stays while it is in flight
        // and after a failed retirement, which need not have run the InitBM.
        let ea2 = epoch(&a, 4);
        let b2 = p.bind(4, false);
        p.hold_alloc(4, &a, None, None).unwrap();
        p.publish_flight(4);
        assert_eq!(sync::Arc::strong_count(&ea1), 2);
        p.retire_flight(4, &ea2, b2, false).unwrap();
        assert_eq!(sync::Arc::strong_count(&ea1), 2, "a failed render released the old list");
        let ea3 = epoch(&a, 4);
        run(&mut p, &a, 4, &ea3, true);
        assert_eq!(sync::Arc::strong_count(&ea1), 1, "released after A's newer render retired");
        assert_eq!(sync::Arc::strong_count(&ea3), 3, "slot 4 owner and A's last epoch");
        assert_eq!(sync::Arc::strong_count(&eb), 3, "slot 3 owner and B's last epoch");
        // Warm reuse of the same epoch changes nothing.
        run(&mut p, &b, 3, &eb, true);
        assert_eq!(sync::Arc::strong_count(&eb), 3);
        assert_eq!(p.stats.kept, 1);
        // Open VMs never keep reclaim armed.
        assert!(!p.take_closed_vm_last());
        assert!(!p.reclaim_closed());
    }

    // A closed VM keeps its last epoch while a render it accepted waits for publication or
    // retirement, and loses it once none does, so its context and lists are not pinned.
    #[test]fn closed_vm_last_pb_epoch_waits_for_its_accepted_renders(){
        let factory = g16::object::Factory;
        let budget = g16::budget::Budget::new().unwrap();
        let mut p = g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        let epoch = |v: &sync::Arc<g16::vm::VmState>, k: usize| {
            let e = g16::pools::PbEpoch::new(
                &factory,
                v,
                g16::budget::Budget::reserve(&budget, 1).unwrap().take().unwrap(),
            ).unwrap();
            e.prepare(k, g16::hw::client::pb_heap(0), 512).unwrap();
            e.expose();
            e
        };
        let run = |p: &mut g16::pools::Pools, v: &sync::Arc<g16::vm::VmState>, k: usize,
                   e: &sync::Arc<g16::pools::PbEpoch>| {
            let b = p.bind(k, false);
            p.hold_alloc(k, v, None, None).unwrap();
            p.publish_flight(k);
            p.retire_flight(k, e, b, true).unwrap();
        };

        let a = vm(20);
        let b = vm(21);
        let ea1 = epoch(&a, 3);
        run(&mut p, &a, 3, &ea1);
        let eb = epoch(&b, 3);
        run(&mut p, &b, 3, &eb);
        assert_eq!(sync::Arc::strong_count(&ea1), 2);

        // A closes with an accepted render not yet published.
        a.vm.set_unpublished(true);
        a.vm.close();
        assert!(p.reclaim_closed(), "accepted render of the closed VM still unpublished");
        assert_eq!(sync::Arc::strong_count(&ea1), 2);
        // It publishes a fresh InitBM epoch on slot 4.
        a.vm.set_unpublished(false);
        let ea2 = epoch(&a, 4);
        let b2 = p.bind(4, false);
        p.hold_alloc(4, &a, None, None).unwrap();
        p.publish_flight(4);
        assert!(p.reclaim_closed(), "render of the closed VM in flight");
        assert_eq!(sync::Arc::strong_count(&ea1), 2);
        p.retire_flight(4, &ea2, b2, true).unwrap();
        assert_eq!(sync::Arc::strong_count(&ea1), 1, "older epoch released after the InitBM");
        assert!(p.take_closed_vm_last(), "a closed VM's entry asks for another reclaim pass");
        assert!(!p.take_closed_vm_last());
        // B retires on slot 5, so slot 4 is displaced; nothing of A is pending any more.
        let eb5 = epoch(&b, 5);
        run(&mut p, &b, 5, &eb5);
        assert!(!p.reclaim_closed());
        assert_eq!(sync::Arc::strong_count(&ea2), 2, "only slot 4's owner keeps A's last epoch");
        drop(ea1);
        drop(ea2);
        // Slot 4's owner holds A until a newer epoch retires there.
        assert_eq!(sync::Arc::strong_count(&a), 2);
        let eb4 = epoch(&b, 4);
        run(&mut p, &b, 4, &eb4);
        assert_eq!(sync::Arc::strong_count(&a), 1, "closed VM fully released");
        assert!(!p.reclaim_closed());
    }

    mod compute_cmds_retry {
        use crate::*;
        use crate::sync::{Arc, Mutex};
        use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
        use crate::g16::object::{FwObject, Objects, Window};
        use crate::g16::slots::UnitCmds;
        pub(super) mod hw {
            pub(crate) use crate::g16::hw::sizes;
        }
        pub struct Factory {
            window: Arc<Mutex<Window>>,
            persistent_calls: AtomicUsize,
        }
        impl Factory {
            pub fn persistent_objects(&self) -> Objects {
                self.persistent_calls.fetch_add(1, Ordering::Relaxed);
                Objects::with_window(true, self.window.clone())
            }
        }
        __PERSISTENT_UNIT_CMDS__
        __COMPUTE_CMDS_STRUCT__
        impl ComputeCmds {
            pub(crate) fn new() -> Result<Arc<Self>> {
                Arc::new(
                    Self {
                        sets: Mutex::new([const { None }; hw::sizes::COMPUTE_UNITS]),
                    },
                    GFP_KERNEL,
                )
            }
            __COMPUTE_CMDS_GET__
        }
        #[test]
        fn failed_finish_private_retries_sync_and_failed_kbox_skips_persistent_objects() {
            let window = Arc::new(Mutex::new(Window::new()), GFP_KERNEL).unwrap();
            window.lock().fail_once.store(true, Ordering::Release);
            let factory = Factory {
                window: window.clone(),
                persistent_calls: AtomicUsize::new(0),
            };
            let cmds = ComputeCmds::new().unwrap();

            // KBox::new_uninit OOM must fail before calling factory.persistent_objects().
            crate::FAIL_KBOX_UNINIT.with(|f| f.set(true));
            assert_eq!(cmds.get(&factory, 0).err(), Some(ENOMEM));
            assert_eq!(factory.persistent_calls.load(Ordering::Relaxed), 0);

            // First real call allocates work/seq/barrier (3 objects) and fails sync_range once.
            let before = crate::g16::object::ALL_OBJECTS.with(|a| a.borrow().len());
            assert_eq!(cmds.get(&factory, 0).err(), Some(EIO));
            assert_eq!(factory.persistent_calls.load(Ordering::Relaxed), 1);
            assert_eq!(crate::g16::object::ALL_OBJECTS.with(|a| a.borrow().len()) - before, 3);
            assert_eq!(window.lock().calls.load(Ordering::Relaxed), 1);

            // Retry must NOT re-allocate work/seq/barrier AND MUST retry sync_range on the
            // preserved pending range before marking finished = true.
            let first = cmds.get(&factory, 0).unwrap();
            assert_eq!(factory.persistent_calls.load(Ordering::Relaxed), 1);
            assert_eq!(crate::g16::object::ALL_OBJECTS.with(|a| a.borrow().len()) - before, 3);
            assert_eq!(window.lock().calls.load(Ordering::Relaxed), 2);
            {
                let w = window.lock();
                let ranges = w.ranges.lock().unwrap();
                assert_eq!(ranges.len(), 2);
                assert_eq!(ranges[0], ranges[1]);
            }

            // Third call hits finished fast-path without calling sync_range again.
            let second = cmds.get(&factory, 0).unwrap();
            assert_eq!(window.lock().calls.load(Ordering::Relaxed), 2);
            assert!(Arc::ptr_eq(&first.work, &second.work));
            assert!(Arc::ptr_eq(&first.seq, &second.seq));
            assert!(Arc::ptr_eq(&first.barrier, &second.barrier));
        }
    }

}
'''
poll_source = (production/'poll.rs').read_text()
mod_source = (production/'mod.rs').read_text()
sleep_source = (production/'sleep.rs').read_text()
assert 'scrubs.run()' not in poll_source, 'poll.rs must hand scrubs off asynchronously via enqueue_scrubs'
assert 'while let Some(mut item) = self.scrub_queue.lock().pop()' not in poll_source, 'run_scrubs must drop scrub_queue spinlock before run_one()'
assert 'self.enqueue_scrubs(scrubs)' in poll_source, 'poll.rs must enqueue scrubs asynchronously'
assert 'OwnedQueue::new_unbound_reclaim(c_str!("aurora_g16_scrub"), false)' in mod_source
assert 'self.scrub_wq.drain();' in sleep_source, 'sleep.rs::join_workers must drain scrub_wq'
assert 'closed_vm_reclaim' in mod_source
assert 'self.shared.wake_closed_vm_reclaim();' in item(mod_source, 'fn vm_closed(')
assert 'reclaim_closed_vm_slots' in poll_source
assert 'job.resources.discard_unpublished_renders();' in item(poll_source, 'if p.is_pending()')
lifetime_source = (production.parent / 'mmu' / 'lifetime.rs').read_text()
is_closed_fn = item(lifetime_source, 'pub(crate) fn is_closed(')
assert '.lock()' not in is_closed_fn, 'Vm::is_closed() must be lock-free'
assert 'lifetime.closed.load(Ordering::Acquire)' in is_closed_fn
unmap_fn = item(lifetime_source, 'pub(crate) fn unmap_user_ranges(')
assert 'lifetime.closed.store(true, Ordering::Release)' in unmap_fn
budget=(production/'budget.rs').read_text()
budget=budget.replace('new_condvar, new_mutex,','').replace('#[pin_data]','').replace('    #[pin]\n','')
constructor=item(budget,'pub(crate) fn new()')
budget=budget.replace(constructor, 'pub(crate) fn new()->Result<Arc<Self>>{Arc::new(Self{used:Mutex::new(0),available:CondVar::new()},GFP_KERNEL)}')
budget += r'''
#[test]fn blocked_reservation_wakes_only_after_deferred_credit_release(){
    let b=Budget::new().unwrap();let all=Budget::reserve(&b,LIMIT).unwrap();
    let other=b.clone();let(tx,rx)=std::sync::mpsc::channel();
    let thread=std::thread::spawn(move||{let permit=Budget::reserve(&other,1).unwrap();tx.send(()).unwrap();drop(permit);});
    assert!(rx.recv_timeout(std::time::Duration::from_millis(20)).is_err());
    drop(all);rx.recv_timeout(std::time::Duration::from_secs(1)).unwrap();thread.join().unwrap();
    assert_eq!(*b.used.lock(),0);
}
'''
budget += r'''
#[test]fn administrative_timeout_and_signal_do_not_spend_credits_or_poison_reuse(){
    let b=Budget::new().unwrap();let full=Budget::reserve(&b,LIMIT).unwrap();
    crate::sync::FORCED_WAIT.with(|v|v.set(1));assert!(matches!(Budget::reserve(&b,1),Err(EBUSY)));
    crate::sync::FORCED_WAIT.with(|v|v.set(2));assert!(matches!(Budget::reserve(&b,1),Err(ERESTARTSYS)));
    assert_eq!(*b.used.lock(),LIMIT);drop(full);
    assert!(Budget::reserve(&b,LIMIT).is_ok());assert_eq!(*b.used.lock(),0);
}
'''
source=source.replace('__BUDGET__',budget.replace('//!','//'))
source = source.replace('__LIVE_TOKEN__' ,item((production/'vm.rs').read_text(),'pub(crate) struct LiveToken')).replace('__LIVE_DROP__',item((production/'vm.rs').read_text(),'impl Drop for LiveToken'))
resources_source = (production/'resources.rs').read_text()
for placeholder, source_text, declaration in (
    ('__WAKE_PUBLISH__', mod_source, 'pub(crate) fn wake_publish('),
    ('__WAKE_CLOSED_VM_RECLAIM__', mod_source, 'pub(crate) fn wake_closed_vm_reclaim('),
    ('__STANDBY__', resources_source, 'struct Standby {'),
    ('__RELEASE_RESERVED__', resources_source, 'fn release_reserved('),
    ('__RELEASE_COMPUTE_CMDS__', resources_source, 'fn release_compute_cmds('),
    ('__RELEASE_RENDER_PASSES__', resources_source, 'fn release_render_passes('),
    ('__RENDERS_UNPUBLISHED__', resources_source, 'pub(crate) fn renders_unpublished('),
    ('__PREPARED__', resources_source, 'pub(crate) struct Prepared {'),
    ('__DROP_PREPARED__', resources_source, 'impl Drop for Prepared'),
    ('__SET_COMPUTE_WAITER__', resources_source, 'pub(crate) fn set_compute_waiter('),
    ('__RELEASE_COMPUTE_WAITER__', resources_source, 'pub(crate) fn release_compute_waiter('),
    ('__RELEASE_ACTIVE_COMPUTES__', resources_source, 'pub(crate) fn release_active_computes('),
    ('__COMMIT_RENDER__', resources_source, 'pub(crate) fn commit_render('),
    ('__DISCARD_UNPUBLISHED__', resources_source, 'pub(crate) fn discard_unpublished_renders('),
    ('__RECLAIM_CLOSED_VM_SLOTS__', poll_source, 'pub(crate) fn reclaim_closed_vm_slots('),
):
    method = item(source_text, declaration)
    # The host Arc wrapper uses Deref without the kernel's arbitrary self types.
    source = source.replace(placeholder, method.replace('self: &Arc<Self>', '&self'))
source = source.replace('__SPARE_EPOCHS__', next(
    line for line in resources_source.splitlines() if line.startswith('const SPARE_EPOCHS:')))
object_source = (production/'object.rs').read_text()
# A firmware-cached object with an uncached GPU lower alias is cleaned to the point of
# coherency after every host store; only that combination sets the flag.
stored_fn = item(object_source, 'fn stored(&self')
assert 'crate::pgtable::clean_poc(' in stored_fn and 'crate::pgtable::poc_barrier()' in stored_fn, 'FwObject::stored must clean and complete'
for store in ('pub(crate) fn write(', 'pub(crate) fn fill(', 'pub(crate) fn w32(', 'pub(crate) fn w64('):
    assert 'self.stored(' in item(object_source, store), store + ' must clean firmware-cached GPU-aliased stores'
assert object_source.count('let clean = req.class == Class::FwCoherent && req.lower.is_some();') == 1, 'only FwCoherent work with a lower alias cleans its stores'
assert 'pub(crate) struct ComputeUnitTable {' in resources_source, 'resources.rs must store sparse per-slot ComputeArenas'
assert 'pub(crate) struct ComputeUnitSet {' in resources_source, 'resources.rs must retain ComputeUnitSet for SCS compute units'
assert 'pub(crate) struct ComputeCmds {' in resources_source, 'resources.rs must retain device-lifetime ComputeCmds'
assert 'let cmds = factory.compute_cmds(hw_slot)?;' in resources_source, 'ComputeArena::new must fetch hardware-slot UnitCmds'
assert 'ComputeArena::new(factory, bind, vm, live, k, k, Placement::Base)' in resources_source, 'ComputeUnitTable::with_added must allocate one ComputeArena per sparse slot'
assert 'for (local, &hw_slot) in hw_slots.iter().enumerate()' in resources_source, 'ComputeUnitSet::new_scs must bind UnitCmds by hw_slot'
assert 'compute: Option<Arc<ComputeUnitTable>>' in resources_source, 'Prepared must carry sparse ComputeUnitTable so undisplaced slots pin only one unit'
assert 'self.compute.as_ref()?.arena(k)' in resources_source and 'scs.arena_for_hw(k)' in resources_source, 'Prepared::compute_arena must return the per-unit ComputeArena'
engine_source = (production/'engine.rs').read_text()
prefix_compute_fn = item(engine_source, 'fn prefix_compute(')
assert 'seq: s.seq.va,' in prefix_compute_fn, 'prefix_compute must point ComputeRefs::seq at firmware-cached s.seq'
assert 's.seq.write(0, q)?;' in prefix_compute_fn, 'prefix_compute must write encoded sequence into s.seq'
assert '&w[work::compute::BODY_ACTIVE_START..work::compute::SEQ_EMBEDDED]' in prefix_compute_fn, 'clean compute reuse must skip embedded sequence copy into WC work'
graphs_finish_fn = item(graph_source, 'pub(crate) fn finish(')
assert 'core::mem::ManuallyDrop::new(factory.persistent_objects_probe())' in graphs_finish_fn, 'Graphs::finish must keep probe-time FwCoherent ring allocator in ManuallyDrop'
assert 'ring_objects.finish_private()?;' in graphs_finish_fn, 'Graphs::finish must publish persistent ring mappings'
alloc_private_in_fn = item(object_source, 'fn alloc_private_in(')
assert 'pub(crate) fn persistent_objects_probe(&self) -> Objects' in object_source, 'Factory must expose persistent_objects_probe for probe-time work rings'
assert 'if self.probe {' in alloc_private_in_fn and 'self.dev.registration_guard().ok_or(ENODEV)?' in alloc_private_in_fn, 'alloc_private_in must gate as_bound() on self.probe and keep registration_guard() otherwise'
assert alloc_private_in_fn.index('UniqueArc::<FwObject>::new_uninit(GFP_KERNEL)?') < alloc_private_in_fn.index('self.uat.kernel_vm().map_in_range('), 'alloc_private_in must pre-allocate UniqueArc before mapping firmware aperture'
assert alloc_private_in_fn.index('self.uat.kernel_lower_vm().map_in_range(') < alloc_private_in_fn.index('self.uat.kernel_vm().map_in_range('), 'alloc_private_in must map uncached aliases before firmware aperture'
assert alloc_private_in_fn.index('self.uat.kernel_vm().map_in_range(') < alloc_private_in_fn.index('self.mappings.push_within_capacity(firmware)'), 'alloc_private_in must commit mappings infallibly after firmware map'
source = (source
    .replace('__FINISH_PRIVATE__', item(object_source, 'pub(crate) fn finish_private('))
    .replace('__PERSISTENT_UNIT_CMDS__', item(resources_source, 'struct PersistentUnitCmds {'))
    .replace('__COMPUTE_CMDS_STRUCT__', item(resources_source, 'pub(crate) struct ComputeCmds {').replace('#[pin]\n', '').replace('super::hw::', 'hw::'))
    .replace('__COMPUTE_CMDS_GET__', item(resources_source, 'pub(crate) fn get(&self, factory: &Factory, k: usize) -> Result<UnitCmds> {')))
compute_tiers_start = resources_source.index('pub(crate) const COMPUTE_UNIT_TIERS:')
compute_tiers_end = resources_source.index('];\n', compute_tiers_start) + 3
ext_tiers_start = resources_source.index('pub(crate) const EXT_RENDER_TIERS:')
ext_tiers_end = resources_source.index('];\n', ext_tiers_start) + 3
render_burst_lines = (
    resources_source[compute_tiers_start:compute_tiers_end]
    + '\n'
    + next(
        line for line in resources_source.splitlines()
        if line.startswith('pub(crate) const COMPUTE_TOTAL_UNIT_BUDGET:')
    )
    + '\n'
    + resources_source[ext_tiers_start:ext_tiers_end]
    + '\n'
    + '\n'.join(
        line for line in resources_source.splitlines()
        if line.startswith(('const BURST_SUBMITS:', 'const BURST_GAP_NS:', 'const RENDER_BURST_COMMANDS:'))
    )
    + '\npub(crate) ' + item(resources_source, 'fn pick_scs_compute_slots(')
    + '\npub(crate) ' + item(resources_source, 'fn pick_base_pb_slots(')
    + '\npub(crate) ' + item(resources_source, 'fn reserve_compute_vm_home(')
    + '\npub(crate) ' + item(resources_source, 'fn active_compute_vm_count(')
    + '\npub(crate) ' + item(resources_source, 'fn release_compute_vm_slot(')
    + '\npub(crate) ' + item(resources_source, 'struct ComputeVmReservation {').replace('Arc<', 'crate::sync::Arc<')
    + '\n' + item(resources_source, 'impl ComputeVmReservation {').replace('Arc<', 'crate::sync::Arc<').replace('fn acquire(', 'pub(crate) fn acquire(').replace('fn commit(', 'pub(crate) fn commit(')
    + '\n' + item(resources_source, 'impl Drop for ComputeVmReservation {')
    + '\npub(crate) ' + item(resources_source, 'fn compute_home_start(')
    + '\npub(crate) ' + item(resources_source, 'fn usable_compute_units(')
    + '\npub(crate) ' + item(resources_source, 'fn target_compute_units(')
    + '\npub(crate) ' + item(resources_source, 'fn pick_compute_units_for(')
    + '\npub(crate) ' + item(resources_source, 'fn pick_compute_units(')
    + '\npub(crate) ' + item(resources_source, 'fn target_render_slots(')
    + '\npub(crate) ' + item(resources_source, 'fn target_render_slots_for(')
)
source = source.replace('__CMD_CLASSES__', '\n'.join(item(graph_source, decl) for decl in ('pub(crate) const fn graph_fw(', 'pub(crate) const fn qinfo_cached(', 'pub(crate) const fn cmd_fw(', 'pub(crate) const fn cmd_shared('))).replace('__SET_DIRTY__',item(resources_source,'pub(crate) fn set_dirty(')).replace('__RENDER_BURST__', render_burst_lines)
graph_types = '\n'.join(
    item(graph_source, decl)
    for decl in (
        'pub(crate) struct ComputeGraph {',
        'struct QueueSession {',
        'impl QueueSession {',
        'struct QueueObjects {',
        'impl QueueObjects {',
        'fn init_queue(',
        'fn init_notifier(',
        'struct ComputeGraphSession {',
        'impl ComputeGraphSession {',
        'impl ComputeGraph {',
        'pub(crate) struct RenderGraph {',
        'struct RenderGraphSession {',
        'impl RenderGraphSession {',
        'impl RenderGraph {',
    )
).replace('fn new(', 'pub(crate) fn new(').replace('fn init(', 'pub(crate) fn init(')
source = source.replace('__GRAPH_TYPES__', graph_types)
source = source.replace('__ALLOC__',item(graph_source,'pub(crate) fn alloc(')).replace('__ALLOC_WORK__',item(graph_source,'pub(crate) fn alloc_work('))
source = source.replace('__HW__','\n'.join([module(hw,n) for n in ('client','sizes','engine','tuning','pte')] + [l for l in hw.splitlines() if l.startswith('pub(crate) const PAGE: u64')])).replace('__ROOT__',str(production)).replace('__PRIVATE_RW__',hex(private_rw)+'u64').replace('__CLIENT__', '#[derive(Copy,Clone)]\n'+item(graph_source, 'pub(crate) struct Client {')+'\n'+item(graph_source, 'impl Client {'))
(out/'check-resources.rs').write_text(source)
log = ''
for command in [
    rustc() + [ '--edition=2021', '--test', str(out/'check-resources.rs'), '-o', str(out/'check-resources')],
    [str(out/'check-resources'), '--nocapture'],
]:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log += result.stdout
    print(result.stdout, end='')
    (out/'resource-host.log').write_text(log)
    if result.returncode:
        raise SystemExit(result.returncode)
files = ['graph.rs', 'slots.rs', 'pools.rs', 'pools/lifetime.rs', 'fw/buffer.rs', 'fw/graph.rs', 'fw/work.rs', 'hw.rs', 'resources.rs', 'budget.rs', 'mod.rs', 'poll.rs', 'vm.rs']
test_count = int(next(line.split()[3] for line in log.splitlines() if line.startswith('test result: ok.')))
(out/'resource-host.json').write_text(json.dumps({
    'result': 'pass', 'tests': test_count,
    'scope': 'Actual production allocation, initialization and lifetime code against a CPU-only backing recorder; no hardware qualification.',
    'private_rw_pte': hex(private_rw),
    'private_rw_reference': 'Qualified J713 NC read/write leaf attributes',
    'source': {f: hashlib.sha256((production/f).read_bytes()).hexdigest() for f in files},
}, indent=2) + '\n')
