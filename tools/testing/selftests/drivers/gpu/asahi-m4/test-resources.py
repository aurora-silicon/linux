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
pub const ETIMEDOUT:Error=Error(110);pub const ERESTARTSYS:Error=Error(512);
pub const GFP_KERNEL:u32=0;
pub type Result<T=()> = std::result::Result<T,Error>;
pub mod prelude {pub use crate::{Result,KVec,KBox,GFP_KERNEL,EINVAL,EAGAIN,E2BIG,EBUSY,EOVERFLOW,EIO,ETIMEDOUT,ERESTARTSYS};}
pub struct KBox<T>(Box<T>);
impl<T>KBox<T>{pub fn new(x:T,_:u32)->Result<Self>{Ok(Self(Box::new(x)))}}
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
        #[derive(Copy,Clone,Debug,Eq,PartialEq)]pub enum Class{Shared,Fw,FwCached,SharedCached,Mmio}
        pub enum Placement{Anywhere}
        pub struct Request{pub size:usize,pub class:Class,pub placement:Placement,pub offset:usize,pub client:Option<(u64,u64)>,pub lower:Option<u64>,pub cpu:bool}
        pub struct FwObject{pub class:Class,pub client_leaf:Option<u64>,pub va:u64,pub gva:Option<u64>,pub lower_gva:Option<u64>,pub lower_leaf:Option<u64>,pub size:usize,pub mapped:usize,pub offset:usize,pub data:std::sync::Arc<std::sync::Mutex<Vec<u8>>>,pub fills:std::sync::Mutex<Vec<(usize,usize)>>,pub fail_fill:std::sync::atomic::AtomicBool}
        impl FwObject{
            pub fn view(o:&Arc<Self>,offset:usize,size:usize)->Result<Arc<Self>>{Arc::new(Self{class:o.class,client_leaf:None,va:o.va+offset as u64,gva:None,lower_gva:None,lower_leaf:None,size,mapped:o.mapped,offset:o.offset+offset,data:o.data.clone(),fills:std::sync::Mutex::new(Vec::new()),fail_fill:std::sync::atomic::AtomicBool::new(false)},0)}
            pub fn write(&self,o:usize,b:&[u8])->Result{let mut d=self.data.lock().unwrap();d[self.offset+o..self.offset+o+b.len()].copy_from_slice(b);Ok(())}
            pub fn fill(&self,o:usize,v:u8,n:usize)->Result{if self.fail_fill.swap(false,std::sync::atomic::Ordering::Relaxed){return Err(EIO);}self.fills.lock().unwrap().push((o,n));let mut d=self.data.lock().unwrap();d[self.offset+o..self.offset+o+n].fill(v);Ok(())}
            pub fn w32(&self,o:usize,v:u32)->Result{self.write(o,&v.to_le_bytes())}
            pub fn w64(&self,o:usize,v:u64)->Result{self.write(o,&v.to_le_bytes())}
            pub fn r32(&self,o:usize)->Result<u32>{let d=self.data.lock().unwrap();Ok(u32::from_le_bytes(d[self.offset+o..self.offset+o+4].try_into().unwrap()))}
            pub fn snapshot(&self)->Vec<u8>{self.data.lock().unwrap().clone()}
        }
        pub struct Factory;
        impl Factory{pub fn objects(&self,_:&())->Objects{Objects::new()}}
        pub struct Objects{pub objects:Vec<Arc<FwObject>>,next:u64}
        impl Objects{
            pub fn new()->Self{Self{objects:vec![],next:0xfffffc2001000000}}
            pub fn finish_private(&mut self)->Result{Ok(())}
            pub fn alloc_dynamic_client(&mut self,size:usize)->Result<Arc<FwObject>>{self.alloc(Request{size,class:Class::Shared,placement:Placement::Anywhere,offset:0,client:Some((0x7040000000,super::hw::pte::LEAF_NC)),lower:None,cpu:true})}
            pub fn alloc(&mut self,r:Request)->Result<Arc<FwObject>>{let bytes=(r.size+r.offset+0x3fff)&!0x3fff;let va=self.next;self.next+=bytes as u64+0x4000;let x=Arc::new(FwObject{class:r.class,client_leaf:r.client.map(|c|c.1),va:va+r.offset as u64,gva:r.client.map(|c|c.0+r.offset as u64),lower_gva:r.lower.map(|g|g+r.offset as u64),lower_leaf:r.lower.map(|_|super::hw::pte::LOWER),size:r.size,mapped:bytes,offset:r.offset,data:std::sync::Arc::new(std::sync::Mutex::new(if r.cpu{vec![0;bytes]}else{vec![]})),fills:std::sync::Mutex::new(Vec::new()),fail_fill:std::sync::atomic::AtomicBool::new(false)},0)?;self.objects.push(x.clone());Ok(x)}
        }
    }
    pub mod graph {
        use crate::*;use crate::sync::Arc;use super::object::*;
        use super::hw::{pte,tuning};
        __CLIENT__
        __CMD_CLASSES__
        __ALLOC__
        __ALLOC_WORK__
    }
    pub mod encode {
        pub struct Geometry{pub tpc_bytes:u64,pub tilemap_bytes:u64}
        pub struct PassRefs{pub ta_work:u64,pub ta_work_gva:u64,pub fs_work:u64,pub fs_work_gva:u64,pub ta_seq:u64,pub fs_seq:u64,pub scene:u64,pub tpc_gva:u64,pub tilemap_gva:u64,pub heap_gva:u64,pub scene_user_gva:u64,pub scene_list:u64,pub pre_gva:[u64;3],pub fs_scratch_gva:u64,pub aux_gva:u64,pub its:u64,pub ta_scratch:u64,pub tpc_fw:u64,pub scratch_fw:u64}
    }
    pub mod vm {
        use crate::{mmu,sync::Arc};use core::sync::atomic::Ordering;
        pub struct VmState{pub id:u64,pub slot:u32,pub generation:u8,pub vm:()}
        struct Registry{live:std::sync::Arc<std::sync::atomic::AtomicU32>}impl Registry{fn release(&self,_file:u64){self.live.fetch_sub(1,Ordering::AcqRel);}}
        __LIVE_TOKEN__
        __LIVE_DROP__
        #[test]fn accounting_outlives_deferred_arenas_and_releases_binding_first(){
            use std::sync::atomic::{AtomicU32,AtomicBool};
            let count=std::sync::Arc::new(AtomicU32::new(1));let released=std::sync::Arc::new(AtomicBool::new(false));
            let registry=Arc::new(Registry{live:count.clone()},0).unwrap();
            let token=Arc::new(LiveToken{registry,file_id:0,bind:Some(mmu::VmBind{live:count.clone(),released:released.clone()})},0).unwrap();
            let deferred_arena=token.clone();drop(token);
            assert_eq!(count.load(Ordering::Acquire),1);assert!(!released.load(Ordering::Acquire));
            drop(deferred_arena);assert!(released.load(Ordering::Acquire));assert_eq!(count.load(Ordering::Acquire),0);
        }
    }
    pub mod resources {
        use core::sync::atomic::{AtomicU64,Ordering};use crate::*;
        pub struct ComputeArena{pub compute:KVec<super::slots::ComputeSlot>}
        // A job's compute units; here one arena.
        pub type Prepared=ComputeArena;
        #[derive(Clone)]
        pub enum PassOwnerArena{Base(crate::sync::Arc<RenderArena>)}
        impl ComputeArena{
            pub fn compute_units(&self)->Result<usize>{Ok(self.compute.len())}
            pub fn compute_unit(&self,k:usize)->Result<&super::slots::ComputeSlot>{self.compute.get(k).ok_or(EINVAL)}
            pub fn compute_arena(&self,_k:usize)->Option<crate::sync::Arc<ComputeArena>>{None}
            pub fn render_arena(&self)->Option<crate::sync::Arc<RenderArena>>{None}
            pub fn render_passes(&self)->Result<usize>{Err(EINVAL)}
            pub fn render_pass(&self,_k:usize)->Result<&super::slots::PassSlot>{Err(EINVAL)}
            pub fn render_pass_dirty(&self,_k:usize)->Result<(&AtomicU64,&AtomicU64)>{Err(EINVAL)}
            pub fn pass_owner_arena(&self,_k:usize)->Option<PassOwnerArena>{None}
        }
        pub struct RenderArena{pub passes:KVec<super::slots::PassSlot>,pub tpc_dirty:[AtomicU64;8],pub tilemap_dirty:[AtomicU64;8]}
        impl RenderArena{ __SET_DIRTY__ }
    }
    pub mod boot {pub fn eproto()->crate::Error{crate::Error(71)}}
    // Arenas here have the two base units.
    pub mod params {pub fn compute_units()->usize{2}pub fn pb_heap_blocks()->u32{2048}pub fn pb_posted_blocks(heap:u32)->u32{heap}}
    pub struct Events{pub grow:crate::KVec<[u32;18]>}
    pub struct Firmware{pub pools:pools::Pools,pub events:Events}
    pub mod budget{__BUDGET__}
    #[path="__ROOT__/slots.rs"]pub mod slots;
    #[path="__ROOT__/pools.rs"]pub mod pools;
}
#[cfg(test)]mod resources {
    use super::*;use g16::{object::Objects,slots::{Slots,Claims},pools::PoolSet};
    struct Arenas{compute:g16::resources::ComputeArena,render:g16::resources::RenderArena}
    fn arenas(o:&mut Objects)->Arenas{
        let mut compute=KVec::new();let mut passes=KVec::new();
        for k in 0..2{compute.push(g16::slots::ComputeSlot::new(o,k).unwrap(),0).unwrap();passes.push(g16::slots::PassSlot::new(o,k,1).unwrap(),0).unwrap();}
        Arenas{compute:g16::resources::ComputeArena{compute},render:g16::resources::RenderArena{passes,tpc_dirty:[const{std::sync::atomic::AtomicU64::new(0)};8],tilemap_dirty:[const{std::sync::atomic::AtomicU64::new(0)};8]}}
    }
    fn retire_render(s:&mut Slots,k:usize){let mut c=Claims::new();c.pass[0]=k as u32;c.pass_mask=1<<k;s.publish(&c).unwrap();s.retire(&mut c).unwrap();}
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
        let pools=PoolSet::new(&mut o).unwrap();
        let mut aliases=Vec::new();
        for a in &o.objects{if let Some(g)=a.gva{assert_eq!(a.client_leaf,Some(__PRIVATE_RW__),"private GPU alias must match qualified J713 NC RW PTE");let base=g-a.offset as u64;assert_eq!(base&0x3fff,0);assert_eq!(a.mapped&0x3fff,0);assert!(base>=0x7000000000);assert!(base+a.mapped as u64<=0x7100000000);aliases.push((base,base+a.mapped as u64));}}
        aliases.push((0x1000000000,0x1000010000)); // Per-VM context store.
        aliases.sort();for a in aliases.windows(2){assert!(a[0].1<=a[1].0,"overlap {:x?} {:x?}",a[0],a[1]);}
        for (k,s) in arena.compute.compute.iter().enumerate(){assert_eq!(s.work.lower_gva.unwrap()&0x3fff,0);assert_eq!(s.work.size,0x2000);assert_eq!(s.pool.size,0x2000000);assert_eq!(s.pool_state.size,0x20000);assert_eq!(s.page_list.size,0x4000);assert_eq!(s.context_store.size,0x40000);assert_eq!(s.pool_state.r32(0).unwrap()as u64 |((s.pool_state.r32(4).unwrap()as u64)<<32),s.pool.gva.unwrap());assert_eq!(pools.allocator_refs_for(k).unwrap().parameter_id,k as u32);}
        // One PB heap per VM, shared by both render allocators.
        assert_eq!(aliases.len(),42);
    }
    #[test]fn work_has_kernel_lower_alias_without_application_alias(){
        let mut o=Objects::new();let slots=Slots::new().unwrap();let arena=arenas(&mut o);
        let work=arena.compute.compute.iter().map(|s|&s.work)
            .chain(arena.render.passes.iter().flat_map(|s|[&s.ta_work,&s.fs_work]));
        for object in work {
            assert_eq!(object.gva,None,"work must not enter the application alias inventory");
            assert_eq!(object.client_leaf,None);
            assert!(object.lower_gva.is_some());
            assert_eq!(object.lower_leaf,Some(0x00c0000000000c8b));
            assert_eq!(object.class,g16::object::Class::Shared);
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
        let mut o=Objects::new();let pools=PoolSet::new(&mut o).unwrap();
        let read64=|obj:&g16::object::FwObject,at:usize|->u64{obj.r32(at).unwrap()as u64|((obj.r32(at+4).unwrap()as u64)<<32)};
        for k in 0..2 {
            let refs=pools.allocator_refs_for(k).unwrap();
            let descriptor=o.objects.iter().find(|obj|obj.va==refs.uma_mgr[0]).unwrap();
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
            let state_va=read64(descriptor,g16::fw::graph::manager::FREELIST);
            let list_va=read64(descriptor,g16::fw::graph::manager::POOL_TABLE);
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
        let mut c=Claims::new();assert_eq!(s.claim_compute(&arena.compute,&mut c,0).unwrap(),Some(0));s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        assert_eq!(s.free_compute(),1);assert_eq!(s.claim_compute(&arena.compute,&mut c,0).unwrap(),Some(1));s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        arena.compute.compute[0].counter.w64(0,91).unwrap();
        assert_eq!(s.claim_compute(&arena.compute,&mut c,0).unwrap(),Some(0));
        assert_eq!(a,arena.compute.compute[0].pool_state.snapshot());assert_eq!(b,arena.compute.compute[0].page_list.snapshot());assert_eq!(arena.compute.compute[0].counter.r32(0).unwrap(),91);
        s.publish(&c).unwrap();s.quarantine(&mut c);assert!(s.compute_exhausted());
    }
    #[test]fn production_render_descriptors_preserve_firmware_bytes_and_exact_counter(){
        let mut o=Objects::new();let mut pools=PoolSet::new(&mut o).unwrap();
        let a=pools.prepare(0,1).unwrap();assert!(a.init_buffer);assert_eq!(a.uma_submitted,2);
        assert!(!pools.retired(0).unwrap());pools.publish(0,2).unwrap();
        assert!(pools.publish(0,2).is_err());assert!(pools.prepare(0,2).is_err());
        let d=o.objects.iter().find(|o|o.va==a.uma_mgr[0]).unwrap();
        d.w64(0x54,2).unwrap();d.w64(0x60,0x5a5aa5a5).unwrap();
        let original=d.snapshot();assert!(pools.retired(0).unwrap());
        let next=pools.prepare(0,2).unwrap();assert!(next.init_buffer);assert_eq!(next.uma_submitted,4);
        assert_eq!(original,d.snapshot());assert_eq!(pools.prepare(1,3).unwrap().parameter_id,1);
    }

    #[test]fn cross_vm_evidence_counts_only_noted_publication(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let a=sync::Arc::new(g16::vm::VmState{id:100,slot:1,generation:1,vm:()},0).unwrap();
        let b=sync::Arc::new(g16::vm::VmState{id:101,slot:2,generation:1,vm:()},0).unwrap();
        let c=sync::Arc::new(g16::vm::VmState{id:102,slot:3,generation:9,vm:()},0).unwrap();
        let mut claims=Claims::new();
        for vm in [&a,&b]{s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,vm).unwrap();s.publish(&claims).unwrap();s.note_published(&claims);s.retire(&mut claims).unwrap();}
        assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        assert_eq!(s.claim_compute(&arena.compute,&mut claims,0).unwrap(),Some(0));
        s.hold_vm(&claims,&c).unwrap();assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        s.publish(&claims).unwrap();assert_eq!(s.stats.cross_vm_retargets,[0,0]);
        s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
        let proof=s.stats.last_retarget[0];
        assert_eq!((proof.unit,proof.old_vm,proof.new_vm),(0,100,102));
        assert_eq!((proof.old_asid,proof.new_asid,proof.old_generation,proof.new_generation),(1,3,1,9));
        s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
        s.retire(&mut claims).unwrap();
        s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,&a).unwrap();
        s.release(&mut claims);s.note_published(&claims);assert_eq!(s.stats.cross_vm_retargets,[1,0]);
    }

    fn vm(id:u64)->sync::Arc<g16::vm::VmState>{sync::Arc::new(g16::vm::VmState{id,slot:id as u32,generation:1,vm:()},0).unwrap()}
    #[test]fn previous_vm_lease_survives_replacement_publication_until_full_retirement(){
        let mut o=Objects::new();let arena=arenas(&mut o);let mut s=Slots::new().unwrap();
        let a=vm(1);let b=vm(2);let c=vm(3);let mut claims=Claims::new();
        for owner in [&a,&b]{s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,owner).unwrap();s.publish(&claims).unwrap();s.retire(&mut claims).unwrap();}
        assert_eq!(sync::Arc::strong_count(&a),2);
        assert_eq!(s.claim_compute(&arena.compute,&mut claims,0).unwrap(),Some(0));
        s.hold_vm(&claims,&c).unwrap();s.publish(&claims).unwrap();s.note_published(&claims);
        assert_eq!(sync::Arc::strong_count(&a),2,"publication cannot release old ASID/backing");
        assert_eq!(sync::Arc::strong_count(&c),2,"candidate must be held too");
        s.retire(&mut claims).unwrap();
        assert_eq!(sync::Arc::strong_count(&a),1);assert_eq!(sync::Arc::strong_count(&c),2);
    }
    #[test]fn cancel_releases_candidate_but_quarantine_preserves_both_vm_leases(){
        let mut o=Objects::new();let arena=arenas(&mut o);let mut s=Slots::new().unwrap();
        let a=vm(1);let b=vm(2);let c=vm(3);let mut claims=Claims::new();
        for owner in [&a,&b]{s.claim_compute(&arena.compute,&mut claims,0).unwrap().unwrap();s.hold_vm(&claims,owner).unwrap();s.publish(&claims).unwrap();s.retire(&mut claims).unwrap();}
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
        // First use of units 0 and 1 runs full scrub (context_store + aux + preempt + seq + its).
        for _ in 0..2 {
            let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
            assert_eq!(arena.compute.compute[k].context_store.fills.lock().unwrap().len(),1);
            assert_eq!(arena.compute.compute[k].its.fills.lock().unwrap().len(),1);
            s.hold_vm(&c,&a).unwrap();s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        }
        // Same-VM clean reuse of unit 0 clears ONLY its (16 B), skipping context_store/aux/preempt/seq.
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(!s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].its.fills.lock().unwrap().len(),2);
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),1);
        assert_eq!(arena.compute.compute[0].preempt.fills.lock().unwrap().len(),1);
        assert_eq!(arena.compute.compute[0].aux.fills.lock().unwrap().len(),1);
        s.hold_vm(&c,&a).unwrap();s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        // Cross-VM switch on unit 1 runs full scrub.
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(b.id)).unwrap().unwrap();
        assert_eq!(k,1);
        assert!(s.compute_needs_full_work(1));
        assert_eq!(arena.compute.compute[1].context_store.fills.lock().unwrap().len(),2);
        assert_eq!(arena.compute.compute[1].preempt.fills.lock().unwrap().len(),2);
        s.hold_vm(&c,&b).unwrap();s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        // Firmware recovery forces full scrub even for same VM.
        s.invalidate_scrub();
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),2);
        // Cancelled preparation forces full scrub on next claim.
        s.release(&mut c);
        let k=s.claim_compute_for(&arena.compute,&mut c,Some(a.id)).unwrap().unwrap();
        assert_eq!(k,0);
        assert!(s.compute_needs_full_work(0));
        assert_eq!(arena.compute.compute[0].context_store.fills.lock().unwrap().len(),3);
        s.hold_vm(&c,&a).unwrap();s.publish(&c).unwrap();s.retire(&mut c).unwrap();
    }

    #[test]fn pass_same_vm_reuse_skips_bulk_scrub_and_preserves_tpc_dirty_for_full_scrub(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();let arena=arenas(&mut o);
        let a=vm(1);let b=vm(2);
        let retire=|s:&mut Slots,k:usize,owner:&sync::Arc<g16::vm::VmState>|{
            let mut c=Claims::new();c.pass[0]=k as u32;c.pass_mask=1<<k;
            s.hold_vm(&c,owner).unwrap();s.publish(&c).unwrap();s.retire(&mut c).unwrap();
        };
        // First use of slots 0 and 1 runs full scrub.
        s.claim_pass_for(&arena.render,0,Some(a.id)).unwrap();
        arena.render.set_dirty(0,0x8000,0x4000).unwrap();
        retire(&mut s,0,&a);
        s.claim_pass_for(&arena.render,1,Some(a.id)).unwrap();
        retire(&mut s,1,&a);
        let p0=&arena.render.passes[0];
        assert_eq!(p0.aux.fills.lock().unwrap().len(),1);
        assert_eq!(p0.scene.fills.lock().unwrap().last(),Some(&(0,0x4000)));
        assert_eq!(p0.tpc.fills.lock().unwrap().len(),1);
        // Same-VM clean reuse of slot 0: clears tilemap/its/heap(0x1000)/pre headers only; skips aux/tpc/scratch/scene.
        s.claim_pass_for(&arena.render,0,Some(a.id)).unwrap();
        assert_eq!(p0.aux.fills.lock().unwrap().len(),1);
        assert_eq!(p0.scene.fills.lock().unwrap().len(),1);
        assert_eq!(p0.ta_scratch.fills.lock().unwrap().len(),1);
        assert_eq!(p0.fs_scratch.fills.lock().unwrap().len(),1);
        assert_eq!(p0.scene_user.fills.lock().unwrap().len(),1);
        assert_eq!(p0.scene_list.fills.lock().unwrap().len(),1);
        assert_eq!(p0.tpc.fills.lock().unwrap().len(),1);
        assert_eq!(p0.tilemap.fills.lock().unwrap().last(),Some(&(0,0x4000)));
        assert_eq!(p0.heap.fills.lock().unwrap().last(),Some(&(0,0x1000)));
        assert_eq!(p0.pre[0].fills.lock().unwrap().last(),Some(&(0,0x540)));
        // Preserved tpc_dirty high-water mark is still cleared on cross-VM switch.
        arena.render.set_dirty(0,0x2000,0x1000).unwrap();
        retire(&mut s,0,&a);
        s.claim_pass_for(&arena.render,1,Some(a.id)).unwrap();
        retire(&mut s,1,&a);
        s.claim_pass_for(&arena.render,0,Some(b.id)).unwrap();
        assert_eq!(p0.aux.fills.lock().unwrap().len(),2);
        assert_eq!(p0.scene.fills.lock().unwrap().len(),2);
        assert_eq!(p0.scene_list.fills.lock().unwrap().len(),2);
        assert_eq!(p0.tpc.fills.lock().unwrap().last(),Some(&(0,0x8000)));
        assert_eq!(p0.tilemap.fills.lock().unwrap().last(),Some(&(0,0x1000)));
        assert_eq!(p0.heap.fills.lock().unwrap().last(),Some(&(0,0x4000)));
    }

    #[test]fn eight_render_passes_and_sixteen_compute_units_are_disjoint_in_mesa_hole(){
        let mut o=Objects::new();
        let _pools=PoolSet::new(&mut o).unwrap();
        for k in 0..g16::hw::sizes::COMPUTE_UNITS as u64 {
            g16::slots::ComputeSlot::new(&mut o,k).unwrap();
        }
        let mut scene_lists=Vec::new();
        for k in 0..g16::hw::sizes::RENDER_SLOTS as u64 {
            let pass=g16::slots::PassSlot::new(&mut o,k,1).unwrap();
            let va=pass.refs().scene_list;
            assert_ne!(va,0);
            assert!(!scene_lists.contains(&va),"each pass slot must own a distinct scene_list");
            scene_lists.push(va);
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
        let mut o=Objects::new();let pools=PoolSet::new(&mut o).unwrap();
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
        pools.publish(0,2).unwrap();p.publish_flight(0);
        let d0=o.objects.iter().find(|obj|obj.va==a1.uma_mgr[0]).unwrap();
        d0.w64(0x54,2).unwrap();
        p.retire_flight(0,&e1,b1,true).unwrap();
        // Same-VM pipelining: 3 concurrent renders share slot 0 with cumulative uma_submitted (4, 6, 8).
        assert!(p.can_pipeline(&vm1));
        assert!(!p.can_pipeline(&vm2));
        for draw in 2..=4u64 {
            assert_eq!(p.pick_slot(&vm1,draw>2),Some(0));
            let refs=pools.prepare_in_flight(0,draw,draw>2).unwrap();
            assert_eq!(refs.uma_submitted,draw*2);
            pools.publish(0,refs.uma_submitted).unwrap();
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
        // Aborted render on slot 0 after slot 1 was previously retired leaves slot 1 undisplaced
        // and slot 0 quarantined -> global pool exhaustion.
        assert_eq!(p.pick_slot(&vm1,false),Some(0));
        p.publish_flight(0);
        p.quarantine_flight(0);
        assert!(p.reusable(0,&vm1).is_none());
        assert_eq!(p.pick_slot(&vm1,false),None);
        assert!(p.exhausted());
        assert!(p.exhausted_for(&vm1));

        // Quarantine slot 0 while slot 1 is still fresh: VM2 claims and reuses slot 1, while
        // other VMs report per-VM exhaustion without tripping global exhaustion until recovery.
        let vm3=vm(3);
        let mut q=g16::pools::Pools::new(&driver::AsahiDrmDevice(device::Device));
        assert_eq!(q.pick_slot(&vm1,false),Some(0));
        q.publish_flight(0);
        q.quarantine_flight(0);
        assert!(!q.exhausted());
        assert_eq!(q.pick_slot(&vm2,false),Some(1));
        let qb2=q.bind(1,false);
        q.publish_flight(1);
        assert!(!q.exhausted_for(&vm3)); // in-flight: waits for drain first
        q.retire_flight(1,&e2,qb2,true).unwrap();
        assert!(!q.exhausted());
        assert!(!q.exhausted_for(&vm2));
        assert_eq!(q.pick_slot(&vm2,false),Some(1));
        assert!(q.exhausted_for(&vm1));
        assert!(q.exhausted_for(&vm3));
        // Recovery invalidates the surviving slot's epoch -> global pool exhaustion.
        q.invalidate();
        assert!(q.exhausted());
        assert!(q.exhausted_for(&vm2));
    }

    #[test]fn pipelined_render_pass_slots_keep_seven_in_flight(){
        let mut o=Objects::new();let mut s=Slots::new().unwrap();
        let mut passes=KVec::new();
        for k in 0..g16::hw::sizes::RENDER_SLOTS as u64 {
            passes.push(g16::slots::PassSlot::new(&mut o,k,1).unwrap(),0).unwrap();
        }
        let arena=g16::resources::RenderArena{
            passes,
            tpc_dirty:[const{std::sync::atomic::AtomicU64::new(0)};8],
            tilemap_dirty:[const{std::sync::atomic::AtomicU64::new(0)};8],
        };
        let a=vm(1);
        let mut live:Vec<Claims>=Vec::new();
        // Publish 7 concurrent passes in flight (slots 0..7).
        for k in 0..7usize {
            let mut c=Claims::new();
            c.pass[0]=k as u32;c.pass_mask=1<<k;
            s.claim_pass_for(&arena,k,Some(a.id)).unwrap();
            s.hold_vm(&c,&a).unwrap();
            s.publish(&c).unwrap();
            live.push(c);
        }
        assert_eq!(s.render_published(),7);
        assert!(s.pass_available(8)); // slot 7 is still fresh
        // Retire 0 and 1 -> slot 0 is displaced and reusable while 5 others remain in flight.
        s.retire(&mut live[0]).unwrap();
        assert!(!s.pass_available(7)); // slot 0 not displaced yet
        s.retire(&mut live[1]).unwrap();
        assert!(s.pass_available(7)); // slot 0 displaced by slot 1!
        for c in live.iter_mut().skip(2) {
            s.retire(c).unwrap();
        }
        assert_eq!(s.render_published(),0);
    }

}
'''
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
source = source.replace('__CMD_CLASSES__',item(graph_source,'pub(crate) const fn cmd_fw(')+'\n'+item(graph_source,'pub(crate) const fn cmd_shared(')).replace('__SET_DIRTY__',item((production/'resources.rs').read_text(),'pub(crate) fn set_dirty('))
source = source.replace('__ALLOC__',item(graph_source,'pub(crate) fn alloc(')).replace('__ALLOC_WORK__',item(graph_source,'pub(crate) fn alloc_work('))
source = source.replace('__HW__','\n'.join(module(hw,n) for n in ('client','sizes','engine','tuning','pte'))).replace('__ROOT__',str(production)).replace('__PRIVATE_RW__',hex(private_rw)+'u64').replace('__CLIENT__', '#[derive(Copy,Clone)]\n'+item(graph_source, 'pub(crate) struct Client {')+'\n'+item(graph_source, 'impl Client {'))
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
files = ['graph.rs', 'slots.rs', 'pools.rs', 'pools/lifetime.rs', 'fw/buffer.rs', 'fw/graph.rs', 'fw/work.rs', 'hw.rs', 'resources.rs', 'budget.rs']
(out/'resource-host.json').write_text(json.dumps({
    'result': 'pass', 'tests': 26,
    'scope': 'Actual production allocation, initialization and lifetime code against a CPU-only backing recorder; no hardware qualification.',
    'private_rw_pte': hex(private_rw),
    'private_rw_reference': 'Qualified J713 NC read/write leaf attributes',
    'source': {f: hashlib.sha256((production/f).read_bytes()).hexdigest() for f in files},
}, indent=2) + '\n')
