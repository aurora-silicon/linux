#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Actual shared Rust destructor/admission code under mock hardware fault injection.

Uses std locks/references as kernel ownership stand-ins. No hardware or integration claim.
"""
from common import kernel_root, output_dir, rustc, frozen_bindings

from pathlib import Path
import subprocess,json
ROOT=kernel_root();OUT=output_dir()
def body(source,marker):
 a=source.index(marker);b=source.index('{',a);level=1;i=b+1
 while level:
  if source[i]=='{':level+=1
  elif source[i]=='}':level-=1
  i+=1
 return source[a:i]
mm=(ROOT/'drivers/gpu/drm/asahi/mmu.rs').read_text();pt=(ROOT/'drivers/gpu/drm/asahi/pgtable.rs').read_text()
pre=r'''
#![allow(dead_code,unused_variables)]
use std::{cell::RefCell,rc::Rc,mem::{ManuallyDrop,size_of},ops::{Deref,DerefMut,Range},num::NonZeroUsize,sync::{Mutex as StdMutex,atomic::{AtomicBool as StdAtomicBool,AtomicU8,AtomicU32,AtomicU64,AtomicUsize,Ordering,fence}}};
type Result<T=()> = std::result::Result<T,i32>;const EIO:i32=5;const ETIMEDOUT:i32=110;const EOVERFLOW:i32=75;
static TRIP_LOAD:AtomicUsize=AtomicUsize::new(0);
struct AtomicBool(StdAtomicBool);impl AtomicBool{fn new(v:bool)->Self{Self(StdAtomicBool::new(v))}fn store(&self,v:bool,o:Ordering){self.0.store(v,o);}fn load(&self,o:Ordering)->bool{let n=TRIP_LOAD.load(Ordering::Relaxed);if n!=0&&TRIP_LOAD.fetch_sub(1,Ordering::Relaxed)==1{self.0.store(true,Ordering::Release);}self.0.load(o)}}
const UAT_NUM_CTX:usize=64;const UAT_USER_CTX_START:usize=1;const TTBR_VALID:u64=1;const TTBR_ASID_SHIFT:usize=48;const UAT_PGSZ:usize=0x4000;
static NODE_DROPS:AtomicUsize=AtomicUsize::new(0);static MUTATIONS:AtomicUsize=AtomicUsize::new(0);static ROOT_FREES:AtomicUsize=AtomicUsize::new(0);static TREE_FREES:AtomicUsize=AtomicUsize::new(0);static FWCTL:AtomicUsize=AtomicUsize::new(0);static LEAF_CLEARS:AtomicUsize=AtomicUsize::new(0);static BARRIERS:AtomicUsize=AtomicUsize::new(0);
macro_rules! pr_err{($($t:tt)*)=>{()};}macro_rules! mod_pr_debug{($($t:tt)*)=>{()};}macro_rules! mod_dev_dbg{($($t:tt)*)=>{()};}macro_rules! dev_err{($($t:tt)*)=>{()};}
struct MutexBackend;struct Mutex<T>(StdMutex<T>);impl<T>Mutex<T>{fn new(v:T)->Self{Self(StdMutex::new(v))}fn lock(&self)->Guard<'_,T,MutexBackend>{Guard(self.0.lock().unwrap(),std::marker::PhantomData)}}
struct Guard<'a,T,M>(std::sync::MutexGuard<'a,T>,std::marker::PhantomData<M>);impl<T,M>Deref for Guard<'_,T,M>{type Target=T;fn deref(&self)->&T{&self.0}}impl<T,M>DerefMut for Guard<'_,T,M>{fn deref_mut(&mut self)->&mut T{&mut self.0}}
struct Monotonic;struct Delta;impl Delta{fn from_millis(n:u64)->u64{n}}struct Instant<T>{n:std::cell::Cell<u64>,p:std::marker::PhantomData<T>}impl<T>Instant<T>{fn now()->Self{Self{n:std::cell::Cell::new(0),p:std::marker::PhantomData}}fn elapsed(&self)->u64{let n=self.n.get();self.n.set(n+1);n}}
mod pgtable{pub fn clean_poc(_p:*const u8,_n:usize){super::BARRIERS.fetch_add(1,super::Ordering::Relaxed);}pub fn poc_barrier(){super::BARRIERS.fetch_add(1,super::Ordering::Relaxed);}}
mod mem{pub fn sync(){super::BARRIERS.fetch_add(1,super::Ordering::Relaxed);}pub fn tlbi_asid(_:u8){}}
#[derive(Clone,Copy,PartialEq,Eq)]enum UatFirmware{Handoff,PublishedRoots}
struct Handoff{lock_ap:AtomicU8,lock_fw:AtomicU8,turn:AtomicU32,cur_slot:AtomicU32}impl Handoff{fn new()->Self{Self{lock_ap:AtomicU8::new(0),lock_fw:AtomicU8::new(0),turn:AtomicU32::new(0),cur_slot:AtomicU32::new(0xffff)}}
'''
handoff='\n'.join(body(mm,m) for m in ['fn lock(&self) {\n        self.lock_ap', 'fn unlock(&self) {\n        self.turn', 'fn current_slot('])
mid=r'''
}
struct Quirks{handoff_lock:bool,clean_tables:bool}
struct SlotTTBS{ttb0:AtomicU64,ttb1:AtomicU64}struct UatShared{handoff:Handoff,ttbs:[SlotTTBS;64],map_kernel_to_user:bool,kernel_ttb1:u64,binding_generations:[u64;64],binding_published:[bool;64],owned_user_roots:[u64;64],quirks:Quirks}
impl UatShared{fn handoff(&self)->&Handoff{&self.handoff}fn ttbs(&self)->&[SlotTTBS;64]{&self.ttbs}}
struct UatInner{firmware:UatFirmware,m4_adopted:bool,failed:AtomicBool,shared:Mutex<UatShared>}
'''
mid=mid.replace('impl UatShared{','impl UatShared{'+''.join(body(mm,m) for m in ['fn handoff_lock(', 'fn handoff_unlock(', 'fn publish_slot(']))
raii='#[path="'+str(ROOT/'drivers/gpu/drm/asahi/mmu/ppl.rs')+'"]mod ppl;\n'+'\n'.join(body(mm,m) for m in [
 'struct M4Transport<', 'impl ppl::Memory for M4Transport', 'enum HandoffGuard<',
 'impl HandoffGuard<', 'impl Drop for HandoffGuard<'
])
inner='impl UatInner{\n'+'\n'.join(body(mm,m) for m in ['fn lock(&self) -> Guard', 'fn check_m4(', 'fn lock_handoff(', 'fn lock_m4_upper('])+'\n}\n'
models=r'''
fn uat(m4:bool,firmware:UatFirmware)->Rc<UatInner>{Rc::new(UatInner{firmware,m4_adopted:m4,failed:AtomicBool::new(false),shared:Mutex::new(UatShared{handoff:Handoff::new(),ttbs:std::array::from_fn(|_|SlotTTBS{ttb0:AtomicU64::new(0),ttb1:AtomicU64::new(0)}),map_kernel_to_user:!m4,kernel_ttb1:0x800000000,binding_generations:[0;64],binding_published:[false;64],owned_user_roots:[0;64],quirks:Quirks{handoff_lock:true,clean_tables:false}})})}
#[derive(Clone,Copy)]struct Prot{cached:bool}impl Prot{fn is_cached_noncoherent(&self)->bool{self.cached}}
struct Mutation;impl Drop for Mutation{fn drop(&mut self){MUTATIONS.fetch_sub(1,Ordering::Relaxed);}}
struct MapTable{fail:bool}impl MapTable{fn unmap_pages(&mut self,_r:Range<u64>)->Result{LEAF_CLEARS.fetch_add(1,Ordering::Relaxed);if self.fail{Err(EIO)}else{Ok(())}}}
struct OwnerState{dev:(),is_kernel:bool,page_table:MapTable}impl OwnerState{fn mapping_mutation(&mut self)->Mutation{MUTATIONS.fetch_add(1,Ordering::Relaxed);Mutation}fn slot(&self)->Option<u32>{Some(0)}fn tlbi_range(&self,_a:u8,_v:u64,_s:usize){}fn tlbi_contexts_naming_root(&self,_v:u64,_s:usize){}}
struct Owner(RefCell<OwnerState>);impl Owner{fn lock_inner(&self)->std::cell::RefMut<'_,OwnerState>{self.0.borrow_mut()}}
struct PinBo;struct KernelMappingInner{owner:Rc<Owner>,uat_inner:Rc<UatInner>,prot:Prot,mapped_size:usize,_bo:Rc<PinBo>}
struct Node{inner:KernelMappingInner,va:u64}impl Deref for Node{type Target=KernelMappingInner;fn deref(&self)->&Self::Target{&self.inner}}impl Node{fn start(&self)->u64{self.va}}
impl Drop for Node{fn drop(&mut self){assert!(self.inner.owner.0.try_borrow_mut().is_ok(),"node freed under owner lock");assert_eq!(MUTATIONS.load(Ordering::Relaxed),0,"node freed before mutation guard");NODE_DROPS.fetch_add(1,Ordering::Relaxed);}}
struct KernelMapping(ManuallyDrop<Node>);impl KernelMapping{fn iova(&self)->u64{self.0.start()}fn size(&self)->usize{self.0.mapped_size}fn iova_range(&self)->Range<u64>{self.iova()..self.iova()+self.size()as u64}fn remap_uncached_and_flush(&mut self){FWCTL.fetch_add(1,Ordering::Relaxed);}}
'''
map_drop=body(mm,'impl Drop for KernelMapping {')
# The actual PageTable Drop chooses whether to visit/free physical pages; the mock records those effects.
pages=r'''
struct Page;impl Page{unsafe fn from_phys(_p:u64){ROOT_FREES.fetch_add(1,Ordering::Relaxed);}}
struct UatPageTable{retained:bool,ttb_owned:bool,ttb:u64,va_range:Range<u64>}
impl UatPageTable{fn new()->Self{Self{retained:false,ttb_owned:true,ttb:0x804000000,va_range:0..0x4000}}fn with_pages<F>(&mut self,_r:Range<u64>,_a:bool,_f:bool,mut f:F)->Result where F:FnMut(u64,&[AtomicU64])->Result{TREE_FREES.fetch_add(1,Ordering::Relaxed);f(0,&[AtomicU64::new(0)])}
'''+body(pt,'pub(crate) fn retain(')+'}\n'+body(pt,'impl Drop for UatPageTable {')
bindings=r'''
#[derive(Clone,Copy)]struct Token(u32);impl Token{fn last_slot(&self)->u32{self.0}}
struct Slot;impl Slot{fn changed(&self)->bool{true}fn slot(&self)->u32{3}fn token(&self)->Token{Token(3)}}
struct Allocator;impl Allocator{fn set_limit(&self,_l:Option<NonZeroUsize>){}fn get(&self,_t:Option<Token>)->Result<Slot>{Ok(Slot)}}
struct VmBinding{active_users:usize,bind_token:Option<Token>,binding:Option<Slot>,generation:u8,ttb:u64}
#[derive(Clone)]struct Vm{id:u64,binding:Rc<Mutex<VmBinding>>}
struct VmBind(Vm,u32);
struct Uat{inner:Rc<UatInner>,slots:Allocator}
mod module_parameters{pub struct Param;impl Param{pub fn value(&self)->u32{0}}pub static robust_isolation:Param=Param;}
'''
helpers='\n'.join(body(mm,m) for m in ['const fn binding_wire_generation(', 'const fn application_ttb1('])
bind='impl Uat{'+body(mm,'pub(crate) fn bind(&self, vm: &Vm)')+'}\n'
vm=r'''
struct VmInner{id:u64,binding:Rc<Mutex<VmBinding>>,uat_inner:Rc<UatInner>,page_table:UatPageTable}
impl VmInner{fn ttb(&self)->u64{self.page_table.ttb}}
'''+body(mm,'impl Drop for VmInner {')
main=r'''
fn reset(){TRIP_LOAD.store(0,Ordering::Relaxed);for c in [&NODE_DROPS,&MUTATIONS,&ROOT_FREES,&TREE_FREES,&FWCTL,&LEAF_CLEARS,&BARRIERS]{c.store(0,Ordering::Relaxed);}}
fn mapping(u:Rc<UatInner>,cached:bool,fail_unmap:bool)->(KernelMapping,Rc<PinBo>){let bo=Rc::new(PinBo);let n=Node{va:0xfffffc2000000000,inner:KernelMappingInner{owner:Rc::new(Owner(RefCell::new(OwnerState{dev:(),is_kernel:true,page_table:MapTable{fail:fail_unmap}}))),uat_inner:u,prot:Prot{cached},mapped_size:0x4000,_bo:bo.clone()}};(KernelMapping(ManuallyDrop::new(n)),bo)}
fn mapping_tests(){
 for (m4,firmware,cached,node_free,fwctl) in [(false,UatFirmware::Handoff,false,1,0),(false,UatFirmware::Handoff,true,1,1),(false,UatFirmware::PublishedRoots,true,1,0),(true,UatFirmware::Handoff,false,1,0),(true,UatFirmware::Handoff,true,0,0)]{reset();let u=uat(m4,firmware);let (m,bo)=mapping(u.clone(),cached,false);drop(m);assert_eq!(NODE_DROPS.load(Ordering::Relaxed),node_free);assert_eq!(FWCTL.load(Ordering::Relaxed),fwctl);assert_eq!(Rc::strong_count(&bo),if node_free==0{2}else{1});assert_eq!(u.failed.load(Ordering::Acquire),m4&&cached);assert_eq!(u.shared.lock().handoff.lock_ap.load(Ordering::Relaxed),0);}
 for turn in [0,1]{reset();let u=uat(true,UatFirmware::Handoff);{let s=u.shared.lock();s.handoff.lock_fw.store(1,Ordering::Relaxed);s.handoff.turn.store(turn,Ordering::Relaxed);}let (m,bo)=mapping(u.clone(),false,false);drop(m);assert_eq!(NODE_DROPS.load(Ordering::Relaxed),0);assert_eq!(LEAF_CLEARS.load(Ordering::Relaxed),0);assert_eq!(Rc::strong_count(&bo),2);assert!(u.failed.load(Ordering::Acquire));assert_eq!(u.check_m4(),Err(EIO));assert_eq!(u.shared.lock().handoff.lock_ap.load(Ordering::Relaxed),0);assert!(BARRIERS.load(Ordering::Relaxed)>=2);}
 reset();let u=uat(true,UatFirmware::Handoff);let (m,bo)=mapping(u.clone(),false,true);drop(m);assert_eq!(LEAF_CLEARS.load(Ordering::Relaxed),1);assert_eq!(NODE_DROPS.load(Ordering::Relaxed),0);assert_eq!(Rc::strong_count(&bo),2);assert!(u.failed.load(Ordering::Acquire));assert_eq!(u.shared.lock().handoff.lock_ap.load(Ordering::Relaxed),0);assert_eq!(MUTATIONS.load(Ordering::Relaxed),0);
 reset();let u=uat(true,UatFirmware::Handoff);u.failed.store(true,Ordering::Release);let(m,bo)=mapping(u.clone(),false,false);drop(m);assert_eq!(NODE_DROPS.load(Ordering::Relaxed),0);assert_eq!(LEAF_CLEARS.load(Ordering::Relaxed),0);assert_eq!(Rc::strong_count(&bo),2);
}
fn fail_stop_race_tests(){
 // Fault becomes visible after the pre-lock check, while this caller waits for shared lock.
 reset();let u=uat(true,UatFirmware::Handoff);TRIP_LOAD.store(2,Ordering::Relaxed);let(m,bo)=mapping(u.clone(),false,false);drop(m);assert_eq!(NODE_DROPS.load(Ordering::Relaxed),0);assert_eq!(LEAF_CLEARS.load(Ordering::Relaxed),0);assert_eq!(Rc::strong_count(&bo),2);assert!(u.failed.load(Ordering::Acquire));assert_eq!(u.shared.lock().handoff.lock_ap.load(Ordering::Relaxed),0);
 // bind must recheck after both the VM binding mutex and global UAT mutex acquisitions.
 for trip in [2,3]{reset();let u=uat(true,UatFirmware::Handoff);let backend=Uat{inner:u.clone(),slots:Allocator};let vm=Vm{id:9,binding:new_binding()};TRIP_LOAD.store(trip,Ordering::Relaxed);assert!(backend.bind(&vm).is_err());let state=u.shared.lock();assert_eq!(state.ttbs[4].ttb0.load(Ordering::Relaxed),0);assert_eq!(state.binding_generations[4],0);assert_eq!(state.handoff.lock_ap.load(Ordering::Relaxed),0);assert!(u.failed.load(Ordering::Acquire));}
}
fn page_tests(){for retain in [false,true]{reset();let mut p=UatPageTable::new();if retain{p.retain();}drop(p);assert_eq!(TREE_FREES.load(Ordering::Relaxed),usize::from(!retain));assert_eq!(ROOT_FREES.load(Ordering::Relaxed),usize::from(!retain));}}
fn new_binding()->Rc<Mutex<VmBinding>>{Rc::new(Mutex::new(VmBinding{active_users:0,bind_token:None,binding:None,generation:0,ttb:0x804000000}))}
fn bind_tests(){for m4 in [false,true]{for blocked in [false,true]{if blocked&&!m4{continue;}reset();let u=uat(m4,UatFirmware::Handoff);{let shared=u.shared.lock();shared.handoff.lock_fw.store(u8::from(blocked),Ordering::Relaxed);}let backend=Uat{inner:u.clone(),slots:Allocator};let vm=Vm{id:9,binding:new_binding()};let result=backend.bind(&vm);let s=u.shared.lock();if blocked{assert!(result.is_err());assert_eq!(s.ttbs[4].ttb0.load(Ordering::Relaxed),0);assert_eq!(s.binding_generations[4],0);assert_eq!(s.owned_user_roots[4],0);assert_eq!(vm.binding.lock().generation,0);assert!(u.failed.load(Ordering::Acquire));}else{let bound=result.unwrap();assert_eq!(bound.1,4);assert_eq!(s.ttbs[4].ttb0.load(Ordering::Relaxed),0x804000001|(4<<48));assert_eq!(s.ttbs[4].ttb1.load(Ordering::Relaxed),if m4{0}else{0x800000001|(4<<48)});assert_eq!(vm.binding.lock().generation,1);}assert_eq!(s.handoff.lock_ap.load(Ordering::Relaxed),0);}}
 // A previously faulted M4 is refused before any new lock or TTBAT generation mutation.
 let u=uat(true,UatFirmware::Handoff);u.failed.store(true,Ordering::Release);let backend=Uat{inner:u.clone(),slots:Allocator};assert!(backend.bind(&Vm{id:9,binding:new_binding()}).is_err());assert_eq!(u.shared.lock().binding_generations[4],0);
}
fn generation_churn_test(){
 let u=uat(true,UatFirmware::Handoff);let backend=Uat{inner:u.clone(),slots:Allocator};
 let published=backend.bind(&Vm{id:1,binding:new_binding()}).unwrap();assert_eq!(published.0.binding.lock().generation,1);
 u.shared.lock().binding_published[4]=true;
 for n in 0..512{let empty=backend.bind(&Vm{id:2+n,binding:new_binding()}).unwrap();assert_eq!(empty.0.binding.lock().generation,2);}
 u.shared.lock().binding_published[4]=true;
 let next=backend.bind(&Vm{id:999,binding:new_binding()}).unwrap();assert_eq!(next.0.binding.lock().generation,3);
}
fn vm_drop_tests(){for (m4,blocked,pre_failed) in [(false,false,false),(true,false,false),(true,true,false),(true,false,true)]{reset();let u=uat(m4,UatFirmware::Handoff);let binding=new_binding();binding.lock().bind_token=Some(Token(3));let low=0x804000001|(4<<48);{let mut s=u.shared.lock();s.ttbs[4].ttb0.store(low,Ordering::Release);s.owned_user_roots[4]=low;s.handoff.lock_fw.store(u8::from(blocked),Ordering::Release);}u.failed.store(pre_failed,Ordering::Release);drop(VmInner{id:9,binding,uat_inner:u.clone(),page_table:UatPageTable::new()});let uncertain=blocked||pre_failed;assert_eq!(ROOT_FREES.load(Ordering::Relaxed),usize::from(!uncertain));assert_eq!(TREE_FREES.load(Ordering::Relaxed),usize::from(!uncertain));let s=u.shared.lock();assert_eq!(s.ttbs[4].ttb0.load(Ordering::Relaxed),if uncertain{low}else{0});assert_eq!(s.owned_user_roots[4],if uncertain{low}else{0});assert_eq!(s.handoff.lock_ap.load(Ordering::Relaxed),0);assert_eq!(u.failed.load(Ordering::Acquire),uncertain);}}
fn main(){generation_churn_test();mapping_tests();page_tests();bind_tests();vm_drop_tests();fail_stop_race_tests();println!("Shared Rust fault injection passed: G15/Neo normal and cached frees exactly once; M4 cached/timeout/partial-unmap retains node+backing+VA; guards released before free; M4 bind timeout changes no TTBAT/generation; VM-drop timeout retains physical tables/root; faulted M4 admission rejects before and after lock acquisition (mock hardware)");}
'''
source=pre+handoff+mid+raii+inner+models+map_drop+pages+bindings+helpers+bind+vm+main
(OUT/'check-mmu-retention.rs').write_text(source)
subprocess.run(rustc() + ['--edition','2021',str(OUT/'check-mmu-retention.rs'),'-o',str(OUT/'check-mmu-retention')],check=True)
r=subprocess.run([str(OUT/'check-mmu-retention')],capture_output=True,text=True)
print(r.stdout,end='');print(r.stderr,end='');(OUT/'mmu-retention-tests.log').write_text(r.stdout+r.stderr)
(OUT/'mmu-retention-tests.json').write_text(json.dumps({'status':'pass'if r.returncode==0 else'fail','coverage':'Actual shared Rust Drop/bind/PPL methods with std ownership and mocked hardware; no hardware qualification','output':r.stdout},indent=2)+'\n')
# Mandatory entry-point fault checks remain before any new object/root allocation.
new_vm=body(mm,'pub(crate) fn new_vm(&self, id: u64, kernel_range: Range<u64>)')
assert new_vm.index('self.inner.check_m4()?')<new_vm.index('Vm::new(')
assert 'ManuallyDrop::new(node)' in mm and mm.count('Ok(KernelMapping(ManuallyDrop::new(node)))')==3
raise SystemExit(r.returncode)
