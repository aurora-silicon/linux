#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2026 Eryk Wieliczko
"""Exercise production registry close and quota accounting without a GPU."""
from common import kernel_root, output_dir, rustc
from pathlib import Path
import subprocess
root=kernel_root();out=output_dir();s=(root/'drivers/gpu/drm/asahi/g16/vm.rs').read_text()
def item(marker):
 a=s.index(marker);b=s.index('{',a);i=b+1;n=1
 while n:n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
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
struct Handle;impl Handle{fn is_closed(&self)->bool{false}}
struct VmState{id:u64,vm:Handle,drops:Arc<AtomicU32>}impl Drop for VmState{fn drop(&mut self){self.drops.fetch_add(1,Ordering::AcqRel);}}
__REGISTRY__
struct Reservation<'a>(Option<&'a Registry>,u64);
__RESERVATION_DROP__
impl Registry{
 __RESERVE__
 __RELEASE__
 __CLOSE__
 __PRUNE__
 fn new_test()->Self{Self{live:AtomicU32::new(0),vms:Mutex(StdMutex::new(KVec(Vec::with_capacity(63)))),files:Mutex(StdMutex::new(KVec(Vec::with_capacity(63))))}}
}
#[test]fn pending_and_live_file_quota_returns_only_on_release(){
 let r=Registry::new_test();let mut owners:Vec<_>=(0..8).map(|_|r.reserve(7).unwrap()).collect();
 assert!(matches!(r.reserve(7),Err(ENOSPC)));let other=r.reserve(8).unwrap();
 drop(owners.pop());owners.push(r.reserve(7).unwrap());assert!(matches!(r.reserve(7),Err(ENOSPC)));
 drop((owners,other));assert_eq!(r.live.load(Ordering::Acquire),0);assert!(r.files.lock().is_empty());
}
#[test]fn device_quota_is_still_bounded_across_files(){
 let r=Registry::new_test();let owners:Vec<_>=(0..63).map(|n|r.reserve(n/8).unwrap()).collect();
 assert!(matches!(r.reserve(100),Err(ENOSPC)));drop(owners);assert_eq!(r.live.load(Ordering::Acquire),0);
}
#[test]fn close_releases_registry_owner_without_another_vm_create(){
 let r=Registry::new_test();let drops=Arc::new(AtomicU32::new(0));
 let state=Arc::new(VmState{id:1,vm:Handle,drops:drops.clone()});r.vms.lock().push(state.clone(),0).unwrap();
 r.close(1);assert!(r.vms.lock().is_empty());assert_eq!(drops.load(Ordering::Acquire),0);
 drop(state);assert_eq!(drops.load(Ordering::Acquire),1);
 r.vms.lock().push(Arc::new(VmState{id:2,vm:Handle,drops:drops.clone()}),0).unwrap();
 r.close(2);assert_eq!(drops.load(Ordering::Acquire),2);
}
'''
for tag,marker in [('REGISTRY','pub(crate) struct Registry'),('RESERVATION_DROP',"impl Drop for Reservation<'_>"),('RESERVE','fn reserve(&self, file_id:'),('RELEASE','fn release(&self, file_id:'),('CLOSE','pub(crate) fn close(&self, id:'),('PRUNE','fn prune(&self)')]:
 source=source.replace('__'+tag+'__',item(marker).replace('    #[pin]\n',''))
source=source.replace('struct Reservation<',s[s.index('const MAX_VMS_PER_FILE:'):s.index(';',s.index('const MAX_VMS_PER_FILE:'))+1]+'\nstruct Reservation<',1)
p=out/'registry.rs';p.write_text(source)
subprocess.run(rustc()+['--edition=2021','--test',str(p),'-o',str(out/'registry')],check=True)
subprocess.run([str(out/'registry')],check=True)
