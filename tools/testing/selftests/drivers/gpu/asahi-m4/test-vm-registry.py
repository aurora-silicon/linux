#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2026 Eryk Wieliczko
"""Exercise production registry close and quota accounting without a GPU."""
from common import kernel_root, output_dir, rustc
from pathlib import Path
import subprocess
root=kernel_root();out=output_dir();s=(root/'drivers/gpu/drm/asahi/g16/vm.rs').read_text()
qs=(root/'drivers/gpu/drm/asahi/queue_share.rs').read_text()
def item(marker,text=None):
 text=s if text is None else text
 a=text.index(marker);b=text.index('{',a);i=b+1;n=1
 while n:n+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[a:i]
source=r'''
#![allow(dead_code,unused_variables,non_upper_case_globals)]
extern crate self as kernel;
use std::sync::{Arc,Mutex as StdMutex,atomic::{AtomicU32,Ordering}};
type Result<T=()> = std::result::Result<T,u32>;const ENOSPC:u32=28;const GFP_NOWAIT:u32=0;
struct Mutex<T>(StdMutex<T>);impl<T>Mutex<T>{fn lock(&self)->std::sync::MutexGuard<'_,T>{self.0.lock().unwrap()}}
struct KVec<T>(Vec<T>);impl<T>KVec<T>{fn push(&mut self,v:T,_f:u32)->Result{self.0.push(v);Ok(())}fn remove(&mut self,i:usize)->Result<T>{Ok(self.0.remove(i))}}
impl<T>std::ops::Deref for KVec<T>{type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T>std::ops::DerefMut for KVec<T>{fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
mod hw{pub mod sizes{pub const MAX_VMS:u32=63;}}
mod module_parameters{pub struct Param;impl Param{pub fn value(&self)->u32{0}}pub static robust_isolation:Param=Param;}
mod types{pub struct ScopeGuard<F:FnOnce()>(Option<F>);impl<F:FnOnce()>ScopeGuard<F>{pub fn new(f:F)->Self{Self(Some(f))}pub fn dismiss(mut self){self.0.take();}}impl<F:FnOnce()>Drop for ScopeGuard<F>{fn drop(&mut self){if let Some(f)=self.0.take(){f()}}}}
const EBUSY:u32=16;
// The VM share, by queue_share.rs's own rule; the current process is set per test.
mod queue_share{
 use super::*;
 thread_local!{pub static CURRENT:std::cell::Cell<usize>=std::cell::Cell::new(1);}
 __QS_RESERVE__
 __QS_MAY_TAKE_HELD__
 pub struct Shares(Vec<(usize,u32)>);
 impl Shares{
  pub const fn new()->Self{Self(Vec::new())}
  pub fn take_current(&mut self,max:u32,_f:u32)->Result<usize>{
   let key=CURRENT.with(|c|c.get());let total=self.0.iter().map(|p|p.1).sum::<u32>();
   let held=self.0.iter().find(|p|p.0==key).map_or(0,|p|p.1);
   if !may_take_held(held,total,max){return Err(EBUSY);}
   match self.0.iter_mut().find(|p|p.0==key){Some(p)=>p.1+=1,None=>self.0.push((key,1))}
   Ok(key)
  }
  pub fn give_back(&mut self,key:usize)->Option<usize>{
   let i=self.0.iter().position(|p|p.0==key)?;self.0[i].1-=1;
   (self.0[i].1==0).then(||self.0.swap_remove(i).0)
  }
  pub fn held(&self)->u32{self.0.iter().map(|p|p.1).sum()}
 }
}
fn as_process(key:usize){queue_share::CURRENT.with(|c|c.set(key));}
struct Handle(std::sync::atomic::AtomicBool);impl Handle{fn open()->Self{Self(std::sync::atomic::AtomicBool::new(false))}fn mark_closed(&self){self.0.store(true,Ordering::Release);}fn is_closed(&self)->bool{self.0.load(Ordering::Acquire)}}
struct Cache{closes:AtomicU32,spares:StdMutex<Vec<Arc<VmState>>>}impl Cache{fn new()->Self{Self{closes:AtomicU32::new(0),spares:StdMutex::new(Vec::new())}}fn close(&self){self.closes.fetch_add(1,Ordering::AcqRel);let spares=std::mem::take(&mut *self.spares.lock().unwrap());drop(spares);}}
struct VmState{id:u64,vm:Handle,drops:Arc<AtomicU32>,resources:Option<Arc<Cache>>}impl Drop for VmState{fn drop(&mut self){self.drops.fetch_add(1,Ordering::AcqRel);}}
__REGISTRY__
struct Reservation<'a>(Option<&'a Registry>,u64,usize);
__RESERVATION_DROP__
impl Registry{
 __RESERVE__
 __RELEASE__
 __CLOSE__
 __PRUNE__
 fn new_test()->Self{Self{live:AtomicU32::new(0),vms:Mutex(StdMutex::new(KVec(Vec::with_capacity(63)))),files:Mutex(StdMutex::new(KVec(Vec::with_capacity(63)))),processes:Mutex(StdMutex::new(queue_share::Shares::new()))}}
}
#[test]fn pending_and_live_file_quota_returns_only_on_release(){
 let r=Registry::new_test();let mut owners:Vec<_>=(0..8).map(|_|r.reserve(7).unwrap()).collect();
 assert!(matches!(r.reserve(7),Err(ENOSPC)));let other=r.reserve(8).unwrap();
 drop(owners.pop());owners.push(r.reserve(7).unwrap());assert!(matches!(r.reserve(7),Err(ENOSPC)));
 drop((owners,other));assert_eq!(r.live.load(Ordering::Acquire),0);assert!(r.files.lock().is_empty());
}
#[test]fn device_quota_is_still_bounded_across_files(){
 // Eight processes, one file each: the device limit still holds across them.
 let r=Registry::new_test();let owners:Vec<_>=(0..63u64).map(|n|{as_process((n/8+1) as usize);r.reserve(n/8).unwrap()}).collect();
 as_process(100);assert!(matches!(r.reserve(100),Err(ENOSPC)));drop(owners);assert_eq!(r.live.load(Ordering::Acquire),0);
 assert_eq!(r.processes.lock().held(),0);
}
#[test]fn a_process_alone_leaves_vm_slots_for_the_compositor(){
 // One process across eight files stops at 56 of 63; another process still gets a slot.
 let r=Registry::new_test();as_process(1);
 let owners:Vec<_>=(0..56).map(|n|r.reserve(n/7).unwrap()).collect();
 assert!(matches!(r.reserve(9),Err(ENOSPC)));assert!(r.files.lock().iter().all(|(id,_)|*id!=9));
 as_process(2);let compositor=r.reserve(50).unwrap();
 drop(owners);drop(compositor);assert_eq!(r.live.load(Ordering::Acquire),0);
 assert!(r.files.lock().is_empty());assert_eq!(r.processes.lock().held(),0);
}
#[test]fn close_releases_registry_owner_without_another_vm_create(){
 let r=Registry::new_test();let drops=Arc::new(AtomicU32::new(0));
 let cache=Arc::new(Cache::new());
 let state=Arc::new(VmState{id:1,vm:Handle::open(),drops:drops.clone(),resources:Some(cache.clone())});r.vms.lock().push(state.clone(),0).unwrap();
 r.close(1);assert!(r.vms.lock().is_empty());assert_eq!(drops.load(Ordering::Acquire),0);
 // The client's close lets the VM's cache drop its arenas while other owners hold the VM.
 assert_eq!(cache.closes.load(Ordering::Acquire),1);
 drop(state);assert_eq!(drops.load(Ordering::Acquire),1);
 r.vms.lock().push(Arc::new(VmState{id:2,vm:Handle::open(),drops:drops.clone(),resources:None}),0).unwrap();
 r.close(2);assert_eq!(drops.load(Ordering::Acquire),2);
}
#[test]fn prune_closes_cache_and_breaks_spare_epoch_cycle(){
 let r=Registry::new_test();let drops=Arc::new(AtomicU32::new(0));
 let cache=Arc::new(Cache::new());
 let state=Arc::new(VmState{id:1,vm:Handle::open(),drops:drops.clone(),resources:Some(cache.clone())});
 // Simulate a spare PB epoch inside Cache holding a strong reference back to VmState.
 cache.spares.lock().unwrap().push(state.clone());
 state.vm.mark_closed();
 r.vms.lock().push(state,0).unwrap();
 r.prune();
 assert!(r.vms.lock().is_empty());
 assert_eq!(cache.closes.load(Ordering::Acquire),1);
 assert_eq!(drops.load(Ordering::Acquire),1);
}
'''
for tag,marker in [('REGISTRY','pub(crate) struct Registry'),('RESERVATION_DROP',"impl Drop for Reservation<'_>"),('RESERVE','fn reserve(&self, file_id:'),('RELEASE','fn release(&self, file_id:'),('CLOSE','pub(crate) fn close(&self, id:'),('PRUNE','fn prune(&self)')]:
 source=source.replace('__'+tag+'__',item(marker).replace('    #[pin]\n',''))
source=source.replace('__QS_RESERVE__','pub '+item('fn reserve(max: u32)',qs)).replace('__QS_MAY_TAKE_HELD__','pub '+item('fn may_take_held(',qs))
source=source.replace('struct Reservation<',s[s.index('const MAX_VMS_PER_FILE:'):s.index(';',s.index('const MAX_VMS_PER_FILE:'))+1]+'\nstruct Reservation<',1)
p=out/'registry.rs';p.write_text(source)
subprocess.run(rustc()+['--edition=2021','--test',str(p),'-o',str(out/'registry')],check=True)
subprocess.run([str(out/'registry')],check=True)
