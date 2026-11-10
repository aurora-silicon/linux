#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Actual shared Rust TTBAT/generation helpers against the working C M4 contract."""
from pathlib import Path
import subprocess,json
from common import kernel_root, output_dir, rustc, unpack_fixtures
ROOT=kernel_root();OUT=output_dir()
uat=(ROOT/'drivers/gpu/drm/asahi/g16/uat.rs').read_text()
def body(source,marker):
 start=source.index(marker);i=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[start:i]
unpack_fixtures(OUT)
mmu=(ROOT/'drivers/gpu/drm/asahi/mmu.rs').read_text()
helpers='\n'.join(body(mmu,x) for x in ['const fn binding_wire_generation(', 'const fn application_ttb1('])
r='''#![allow(dead_code)]\nconst TTBR_VALID:u64=1;const TTBR_ASID_SHIFT:usize=48;\n'''+helpers+'''
fn main(){let expected=std::fs::read(std::env::args().nth(1).unwrap()).unwrap();let words:Vec<u64>=expected.chunks_exact(8).map(|b|u64::from_le_bytes(b.try_into().unwrap())).collect();let epochs=[0,1,254,255,256,257,u64::MAX];let mut i=0;for asid in 1usize..64{for epoch in epochs{let pa=0x804000000+asid as u64*0x4000;assert_eq!(pa|1|(asid as u64)<<48,words[i]);assert_eq!(binding_wire_generation(epoch)as u64,words[i+1]);assert_eq!(application_ttb1(pa,asid,false),words[i+2]);assert_eq!(application_ttb1(pa,asid,true),pa|1|(asid as u64)<<48);i+=3;}}assert_eq!(i,words.len());println!("441 M4 TTBAT/epoch C golden cases pass; all 63 application upper words zero; G15 shared upper words preserved");}
'''

# Exercise the actual root lookup and invalidation dispatch even after the bind
# token is gone. M4 TTBAT roots omit the ASID tag used by published-root firmware.
lookup=body(mmu,'fn contexts_naming_root(&self, ttb: u64, adopted: bool)')
invalidate=body(mmu,'fn tlbi_contexts_naming_root(&self, iova: u64, size: usize)')
extra=r"""
use core::sync::atomic::{AtomicU64,Ordering};
use core::cell::Cell;
#[derive(PartialEq)]enum UatFirmware{Handoff,PublishedRoots}
struct SlotTTBS{ttb0:AtomicU64}
struct Shared{slots:[SlotTTBS;64]}
impl Shared{fn ttbs(&self)->&[SlotTTBS]{&self.slots} __LOOKUP__}
struct Inner{firmware:UatFirmware,m4_adopted:bool,shared:Shared}
impl Inner{fn lock(&self)->&Shared{&self.shared}}
struct Owner{uat_inner:Inner,root:u64,invalidated:Cell<u64>}
impl Owner{
 fn ttb(&self)->u64{self.root}
 fn tlbi_context_mask(&self,_iova:u64,_size:usize,mask:u64){self.invalidated.set(mask)}
 __INVALIDATE__
}
mod mem{pub fn sync(){}}
__TAGGED_ROOT__
fn check_root_invalidation(){
 let root=0x804000000;
 let owner=Owner{uat_inner:Inner{firmware:UatFirmware::Handoff,m4_adopted:true,
  shared:Shared{slots:core::array::from_fn(|_|SlotTTBS{ttb0:AtomicU64::new(0)})}},root,invalidated:Cell::new(0)};
 for slot in [0,1,7,63]{owner.uat_inner.shared.slots[slot].ttb0.store(tagged_root(root,slot),Ordering::Relaxed);}
 owner.uat_inner.shared.slots[2].ttb0.store((root+0x4000)|1,Ordering::Relaxed);
 owner.tlbi_contexts_naming_root(0x7000000000,0x4000);
 assert_eq!(owner.invalidated.get(),1|2|128|(1u64<<63));
 // Closing a bind token does not clear its TTBAT root. The lookup still finds
 // that hardware context; replacing its root removes it from this mask.
 owner.uat_inner.shared.slots[1].ttb0.store((root+0x8000)|1,Ordering::Relaxed);
 owner.tlbi_contexts_naming_root(0x7000000000,0x4000);
 assert_eq!(owner.invalidated.get(),1|128|(1u64<<63));
 let mut owner=owner;owner.uat_inner.m4_adopted=false;
 owner.invalidated.set(0);owner.tlbi_contexts_naming_root(0,0x4000);
 assert_eq!(owner.invalidated.get(),0,"legacy handoff keeps its bind-slot invalidation");
 owner.uat_inner.firmware=UatFirmware::PublishedRoots;
 for slot in 0..64 {owner.uat_inner.shared.slots[slot].ttb0.store(0,Ordering::Relaxed);}
 for slot in [1,7,63]{owner.uat_inner.shared.slots[slot].ttb0.store(tagged_root(root,slot),Ordering::Relaxed);}
 owner.tlbi_contexts_naming_root(0,0x4000);assert_eq!(owner.invalidated.get(),2|128|(1u64<<63));
 println!("M4 context-0 and tagged user roots, context0, retired bind slots and tagged-root invalidation masks pass");
}
"""
extra=extra.replace('__LOOKUP__',lookup).replace('__INVALIDATE__',invalidate).replace('__TAGGED_ROOT__',body(mmu,'const fn tagged_root('))
r=r.replace('fn main(){','fn main(){check_root_invalidation();',1)+extra

(OUT/'check-vm-words.rs').write_text(r)
subprocess.run(rustc()+['--edition','2021',str(OUT/'check-vm-words.rs'),'-o',str(OUT/'check-vm-words')],check=True)
p=subprocess.run([str(OUT/'check-vm-words'),str(OUT/'vm-words.bin')],check=True,text=True,capture_output=True)
assert 'new_adopted_with_user_upper(dev, geometry, &regions, false)' in uat
assert 'application_ttb1(uat_inner.kernel_ttb1, idx, uat_inner.map_kernel_to_user)' in mmu
assert 'binding.generation = binding_wire_generation(incarnation)' in mmu
print(p.stdout,end='');(OUT/'vm-words-tests.log').write_text(p.stdout)

# Compile the actual map transaction around a lock-checking PPL stand-in. The
# naming-root scan must never recursively acquire the mutex owned by this lease.
map_transaction = body(mmu, 'fn map_node(&mut self, node: &mm::Node<(), KernelMappingInner>, prot: Prot)')
lock_source = r'''
#![allow(dead_code, unused_variables)]
use std::{cell::Cell, rc::Rc, ops::{Range,Deref}};use std::sync::atomic::{AtomicBool,Ordering};
macro_rules! dev_err { ($($arg:tt)*) => {}; }
type Result<T=()> = std::result::Result<T,u32>;
const EOVERFLOW:u32=75;const EBUSY:u32=16;
#[derive(Clone,Copy)]struct Prot;
struct KernelMappingInner{mapped_size:usize}
mod mm {
    use super::*;
    pub struct Node<A,B>{pub start:u64,pub inner:B,pub marker:core::marker::PhantomData<A>}
    impl<A,B>Node<A,B>{pub fn start(&self)->u64{self.start}}
    impl<A,B>Deref for Node<A,B>{type Target=B;fn deref(&self)->&B{&self.inner}}
}
struct Uat{held:Cell<bool>,m4_adopted:bool,failed:AtomicBool}
struct Lease(Rc<Uat>);impl Drop for Lease{fn drop(&mut self){assert!(self.0.held.replace(false));}}
impl Uat{
    fn lock_m4_upper(self:&Rc<Self>,upper:bool)->Result<Lease>{assert!(upper);assert!(!self.held.replace(true),"recursive shared-UAT lock");Ok(Lease(self.clone()))}
}
struct Table{uat:Rc<Uat>,rolled_back:Cell<bool>,busy:bool}
impl Table{
    fn prepare_map(&mut self,_r:Range<u64>)->Result{assert!(!self.uat.held.get(),"allocation under PPL");Ok(())}
    fn discard_partial_map(&mut self,_r:Range<u64>)->Result{assert!(self.uat.held.get(),"rollback outside PPL");self.rolled_back.set(true);Ok(())}
    // Another mapping's live leaves in the range refuse the map before any store.
    fn ensure_unmapped(&mut self,_r:Range<u64>)->Result{assert!(self.uat.held.get(),"live-leaf check outside PPL");if self.busy{Err(EBUSY)}else{Ok(())}}
}
struct Owner{uat_inner:Rc<Uat>,page_table:Table,is_kernel:bool,fail:bool,invalidations:Cell<u32>,stores:Cell<u32>}
impl Owner{
    fn mapping_mutation(&mut self){}
    fn map_node_prepared(&mut self,_n:&mm::Node<(),KernelMappingInner>,_p:Prot)->Result{
        assert!(self.uat_inner.held.get(),"live leaf stores outside PPL");self.stores.set(self.stores.get()+1);if self.fail{Err(5)}else{Ok(())}
    }
    fn tlbi_contexts_naming_root(&self,_va:u64,_size:usize){assert!(!self.uat_inner.held.get(),"naming-root lookup recursively locks UatShared");self.invalidations.set(self.invalidations.get()+1);}
    fn finish_failed_map(&self,va:u64,size:usize){self.tlbi_contexts_naming_root(va,size);}
    __MAP_TRANSACTION__
}
#[test]fn success_and_rollback_release_ppl_before_root_lookup(){
    for fail in [false,true]{
        let uat=Rc::new(Uat{held:Cell::new(false),m4_adopted:true,failed:AtomicBool::new(false)});
        let mut owner=Owner{uat_inner:uat.clone(),page_table:Table{uat:uat.clone(),rolled_back:Cell::new(false),busy:false},is_kernel:true,fail,invalidations:Cell::new(0),stores:Cell::new(0)};
        let node=mm::Node{start:0xfffffc2000000000,inner:KernelMappingInner{mapped_size:0x4000},marker:core::marker::PhantomData};
        assert_eq!(owner.map_node(&node,Prot),if fail{Err(5)}else{Ok(())});
        assert!(!uat.held.get());assert_eq!(owner.page_table.rolled_back.get(),fail);assert_eq!(owner.invalidations.get(),1);
    }
}
#[test]fn a_live_range_is_refused_before_any_store_and_releases_ppl(){
    let uat=Rc::new(Uat{held:Cell::new(false),m4_adopted:true,failed:AtomicBool::new(false)});
    let mut owner=Owner{uat_inner:uat.clone(),page_table:Table{uat:uat.clone(),rolled_back:Cell::new(false),busy:true},is_kernel:true,fail:false,invalidations:Cell::new(0),stores:Cell::new(0)};
    let node=mm::Node{start:0xfffffc2000000000,inner:KernelMappingInner{mapped_size:0x4000},marker:core::marker::PhantomData};
    assert_eq!(owner.map_node(&node,Prot),Err(EBUSY));
    assert!(!uat.held.get());assert_eq!(owner.stores.get(),0);assert!(!owner.page_table.rolled_back.get());
    assert_eq!(owner.invalidations.get(),0);assert!(!uat.failed.load(Ordering::Acquire));
}
'''.replace('__MAP_TRANSACTION__',map_transaction)
lock_unit=OUT/'map-node-lock.rs';lock_unit.write_text(lock_source)
subprocess.run(rustc()+['--edition=2021','--test',str(lock_unit),'-o',str(OUT/'map-node-lock')],check=True)
subprocess.run([str(OUT/'map-node-lock')],check=True)
print('Production kernel-map success/rollback keeps leaf mutations under PPL and scans naming roots only after releasing its mutex')
