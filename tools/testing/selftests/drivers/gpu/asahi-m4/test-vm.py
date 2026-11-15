#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile actual Rust VM/UAT methods against mock MMIO and compare cold graph admission with C.

MMIO/cache/barrier effects are modeled; this is not hardware or end-to-end driver coverage.
"""
from common import FIXTURES, kernel_root, output_dir, rustc, unpack_fixtures
from pathlib import Path
import subprocess, json, re
ROOT = kernel_root()
OUT = output_dir()
def body(source, marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth = 1
    i = brace + 1
    while depth:
        if source[i] == '{': depth += 1
        elif source[i] == '}': depth -= 1
        i += 1
    return source[start:i]
uat = (ROOT/'drivers/gpu/drm/asahi/g16/uat.rs').read_text()
vm = (ROOT/'drivers/gpu/drm/asahi/g16/vm.rs').read_text()
pt = (ROOT/'drivers/gpu/drm/asahi/pgtable.rs').read_text()
methods = '\n'.join(body(uat, m) for m in [
 'fn cold_snapshot(', 'fn admit_cold(', 'fn admit_table(', 'pub(crate) fn ttbat_is_clear(',
 'pub(crate) fn bootstrap(',
 'fn verify_root(', 'fn shadow_l2(', 'pub(crate) fn sync_range(', 'fn sync_inner(',
])
prot_constants = pt[pt.index('const PTE_TYPE_BITS:'):pt.index('impl Prot {')]
prot_impl = body(pt, 'impl Prot {')
reserved = body(vm, 'pub(crate) fn reserved_ranges(')
pre = r'''
#![allow(dead_code)]
use std::{cell::RefCell, sync::{Mutex, atomic::{AtomicU64, Ordering}}, collections::HashMap, ops::Range};
type Result<T=()> = std::result::Result<T,i32>;
const EINVAL:i32=22; const EIO:i32=5; const EBUSY:i32=16; const ETIMEDOUT:i32=110;
const GFP_KERNEL:u32=0; const EAGAIN:i32=11;
struct KVec<T>(Vec<T>); impl<T>KVec<T>{fn with_capacity(n:usize,_g:u32)->Result<Self>{Ok(Self(Vec::with_capacity(n)))}fn push(&mut self,v:T,_g:u32)->Result{self.0.push(v);Ok(())}fn as_slice(&self)->&[T]{&self.0}}
const ENTRIES:usize=2048; const ROOT_ENTRIES:usize=64; const VALID:u64=3; const WINDOW_INDEX:usize=2;
mod hw { pub const PAGE:u64=0x4000; pub const TABLES_SIZE:u64=0x40000;
 pub const UPPER:u64=0xfffffc0000000000; pub const HOST:u64=0xfffffc2000000000;
 pub const HOST_SIZE:u64=0x1000000000; pub const PPL_MAGIC:u64=0x4b1d000000000002;
 pub const PA_MASK:u64=0x3ffffffc000; }
mod device { pub struct Device; }
// The common PPL lease is modeled here; test-ppl.py executes its actual guard with
// concurrency and timeout injection. This harness tests Window's graph operations.
mod mmu {
 pub struct HandoffGuard; impl Drop for HandoffGuard{fn drop(&mut self){super::note("ppl-unlock");}}
 pub struct Uat { pub lower:u64,pub shadow:u64 } impl Uat {
 pub fn kernel_lower_root(&self)->u64 {self.lower} pub fn kernel_window_root(&self)->u64 {self.shadow}
 pub fn lock_m4_handoff(&self)->super::Result<HandoffGuard>{super::note("ppl-lock");Ok(HandoffGuard)} } }
macro_rules! dev_info {($($tt:tt)*)=>{()};}
thread_local!{static TRACE:RefCell<Vec<String>>=const{RefCell::new(Vec::new())};}
fn note(s:impl Into<String>){TRACE.with(|t|t.borrow_mut().push(s.into()));}
fn take_trace()->Vec<String>{TRACE.with(|t|std::mem::take(&mut *t.borrow_mut()))}
struct Mapping { pa:u64,data:RefCell<Vec<u8>>,label:&'static str }
impl Mapping {fn new(pa:u64,len:usize,label:&'static str)->Self{Self{pa,data:RefCell::new(vec![0;len]),label}}
 fn io(&self)->&Self{self} fn phys(&self)->u64{self.pa}
 fn try_read8(&self,off:usize)->Result<u8>{self.data.borrow().get(off).copied().ok_or(EINVAL)}
 fn try_read32(&self,off:usize)->Result<u32>{let d=self.data.borrow();Ok(u32::from_le_bytes(d.get(off..off+4).ok_or(EINVAL)?.try_into().unwrap()))}
 fn try_read64(&self,off:usize)->Result<u64>{let d=self.data.borrow();Ok(u64::from_le_bytes(d.get(off..off+8).ok_or(EINVAL)?.try_into().unwrap()))}
 fn try_write8(&self,v:u8,off:usize)->Result{*self.data.borrow_mut().get_mut(off).ok_or(EINVAL)?=v;note(format!("{}:{off:x}={v:x}",self.label));Ok(())}
 fn try_write32(&self,v:u32,off:usize)->Result{self.data.borrow_mut().get_mut(off..off+4).ok_or(EINVAL)?.copy_from_slice(&v.to_le_bytes());note(format!("{}:{off:x}={v:x}",self.label));Ok(())}
 fn try_write64(&self,v:u64,off:usize)->Result{self.data.borrow_mut().get_mut(off..off+8).ok_or(EINVAL)?.copy_from_slice(&v.to_le_bytes());note(format!("{}:{off:x}={v:x}",self.label));Ok(())}
}
struct Monotonic; struct Delta; impl Delta{fn from_millis(ms:u64)->u64{ms}}
struct Instant<T>{n:std::cell::Cell<u64>,t:std::marker::PhantomData<T>}
impl<T> Instant<T>{fn now()->Self{Self{n:std::cell::Cell::new(0),t:std::marker::PhantomData}}fn elapsed(&self)->u64{let n=self.n.get();self.n.set(n+1);n}}
fn mb(){note("mb");} fn wmb(){note("wmb");} fn rmb(){}
mod mem{pub fn sync(){super::note("sync");}pub fn tlbi_range(asid:u8,va:usize,len:usize){super::note(format!("tlbi:{asid}:{va:x}:{len:x}"));}}
fn tlbi_all(){note("tlbi_all");} fn clean_lower(_: &mmu::Uat)->Result{note("clean_lower");Ok(())}
static RAM:std::sync::LazyLock<Mutex<HashMap<u64,Vec<AtomicU64>>>>=std::sync::LazyLock::new(||Mutex::new(HashMap::new()));
fn with_table<R>(pa:u64,count:usize,f:impl FnOnce(&[AtomicU64])->R)->Result<R>{let ram=RAM.lock().unwrap();let d=ram.get(&pa).ok_or(EIO)?;if count>d.len(){return Err(EINVAL)};Ok(f(&d[..count]))}
fn entry(pa:u64,index:usize)->Result<u64>{with_table(pa,index+1,|e|e[index].load(Ordering::Acquire))}
fn clean_entries(pa:u64,first:usize,count:usize)->Result{with_table(pa,first+count,|_|())?;note(format!("clean:{pa:x}:{first}:{count}"));Ok(())}
fn clean_page(pa:u64)->Result{clean_entries(pa,0,ENTRIES)}
struct Window{handoff:Mapping,ttbs:Mapping,root:Mapping,shared_l2:Mapping,image:Vec<u64>,snapshot:Vec<u64>,published:bool,firmware_root:[u64;2],text_pa:u64,data_pa:u64}
impl Window{fn blank()->Self{Self{handoff:Mapping::new(0x807000000,0x4000,"handoff"),ttbs:Mapping::new(0x808000000,0x4000,"ttbs"),root:Mapping::new(0x800000000,0x40000,"root"),shared_l2:Mapping::new(0x801000000,0x4000,"shared"),image:vec![0;2048],snapshot:vec![0;2048],published:false,firmware_root:[0;2],text_pa:0x802000000,data_pa:0x803000000}}
'''
post = r'''
}
fn fw_word(index:u64,w:&Window)->u64{let off=index*hw::PAGE;if off<0x60000{w.text_pa+off|if (0x50000..0x58000).contains(&off){0x483}else{0x4c3}}else{let d=off-0x60000;w.data_pa+d|if d<0xc000{0x00200000000004c3}else{0x0460000000000443}}}
fn seeded()->Window{let w=Window::blank();w.root.try_write64(w.root.pa+0x4000|0x403,0).unwrap();w.root.try_write64(w.root.pa+0x8000|0x403,0x4000).unwrap();for i in 0..103{w.root.try_write64(fw_word(i,&w),0x8000+i as usize*8).unwrap();}w}
fn mutation(id:usize)->Window{let w=seeded();match id{
 0=>{},1=>{w.root.try_write64(w.shared_l2.pa|3,16).unwrap();},
 2=>{w.root.try_write64(w.root.pa+0x40000|0x403,0).unwrap();},
 3=>{w.root.try_write64(w.root.pa|0x403,0).unwrap();},
 4=>{w.root.try_write64(w.root.pa+0x4000|3,0).unwrap();},
 5=>{w.root.try_write64(fw_word(0,&w)^0x4000,0x8000).unwrap();},
 6=>{w.root.try_write64(w.root.pa+0x4000|0x403,8).unwrap();},
 7=>{w.root.try_write64(w.shared_l2.pa+0x4000|3,16).unwrap();},
 8=>{w.root.try_write64(1,0x30000).unwrap();},
 9=>{w.shared_l2.try_write64(3,0).unwrap();},
 10=>{w.root.try_write64(0x3,0x8000+103*8).unwrap();},
 11=>{w.root.try_write64(w.root.pa+0xc000|0x403,0x4000+8).unwrap();},
 _=>unreachable!()};w}
fn assert_c_parity(w:&Window,id:usize,golden:&str,_out:&str){let mut b=w.root.data.borrow().clone();b.extend_from_slice(&w.shared_l2.data.borrow());let fixture=std::fs::read(format!("{golden}/vm-case-{id}.bin")).unwrap();assert_eq!(b,fixture,"Frozen C oracle input mismatch #{id}");let c_ok=ACCEPTED[id];let mut cloned=Window::blank();*cloned.root.data.borrow_mut()=w.root.data.borrow().clone();*cloned.shared_l2.data.borrow_mut()=w.shared_l2.data.borrow().clone();assert_eq!(cloned.admit_cold().is_ok(),c_ok,"C/Rust cold graph mismatch #{id}");}
fn root_admission_tests(){
 let mut w=Window::blank();assert!(w.admit_cold().is_ok());
 for off in [0usize,8]{w.handoff.try_write64(7,off).unwrap();assert_eq!(w.admit_cold(),Err(EBUSY));w.handoff.try_write64(hw::PPL_MAGIC,off).unwrap();assert!(w.admit_cold().is_ok());}
 for off in [0x10usize,0x11]{w.handoff.try_write8(1,off).unwrap();assert_eq!(w.admit_cold(),Err(EBUSY));w.handoff.try_write8(0,off).unwrap();}
 for i in 0..195{w.handoff.try_write64(1,0x20+i*8).unwrap();assert_eq!(w.admit_cold(),Err(EBUSY));w.handoff.try_write64(0,0x20+i*8).unwrap();}
 for i in 0..128{w.ttbs.try_write64(1,i*8).unwrap();assert_eq!(w.admit_cold(),Err(EBUSY));w.ttbs.try_write64(0,i*8).unwrap();}
}
fn publication_tests(){
 let uat=mmu::Uat{lower:0x804000000,shadow:0x805000000};let l2=0x805004000;let l3=0x805008000;
 {let mut ram=RAM.lock().unwrap();for pa in [uat.shadow,l2,l3]{ram.insert(pa,(0..2048).map(|_|AtomicU64::new(0)).collect());}ram[&uat.shadow][2].store(l2|3,Ordering::Release);ram[&l2][4].store(l3|3,Ordering::Release);ram[&l3][0].store(0x806000000|0x00e000000000040b,Ordering::Release);}
 for firmware_magic in [0,hw::PPL_MAGIC]{let mut w=Window::blank();w.handoff.try_write64(firmware_magic,8).unwrap();take_trace();w.sync_range(&uat,hw::HOST+0x8000000..hw::HOST+0x8004000).unwrap();assert_eq!(w.shared_l2.try_read64(4*8).unwrap(),0);assert!(take_trace().is_empty());
  w.bootstrap(&device::Device,&uat).unwrap();assert_eq!(w.handoff.try_read32(0x18).unwrap(),0xffff);assert_eq!(w.handoff.try_read8(0x638).unwrap(),u8::from(firmware_magic==0));assert_eq!(w.shared_l2.try_read64(4*8).unwrap(),l3|3);assert_eq!(w.ttbs.try_read64(0).unwrap(),uat.lower|1);assert_eq!(w.ttbs.try_read64(8).unwrap(),w.root.pa|1);let trace=take_trace();let c=trace.iter().position(|s|s==&format!("clean:{l3:x}:0:2048")).unwrap();let link=trace.iter().position(|s|s.starts_with("shared:20=")).unwrap();let ctx=trace.iter().position(|s|s.starts_with("ttbs:0=")).unwrap();assert!(c<link&&link<ctx);assert_eq!(w.bootstrap(&device::Device,&uat),Err(EBUSY));
  w.root.try_write64(w.root.pa+0x4000|0x403,0).unwrap();w.root.try_write64(w.root.pa+0x10000|0x00200000000004cb,8).unwrap();w.root.try_write64(w.shared_l2.pa|0x00200000000004cb,16).unwrap();w.verify_root().unwrap();w.root.try_write64(0,0).unwrap();assert_eq!(w.verify_root(),Err(EIO));w.root.try_write64(w.firmware_root[0],0).unwrap();
  take_trace();w.sync_range(&uat,hw::HOST+0x8000000..hw::HOST+0x8004000).unwrap();assert!(take_trace().iter().any(|s|s=="tlbi:0:fffffc2008000000:4000"));w.shared_l2.try_write64(l3+0x4000|3,32).unwrap();assert_eq!(w.sync_range(&uat,hw::HOST+0x8000000..hw::HOST+0x8004000),Err(EIO));assert_eq!(w.handoff.try_read8(0x10).unwrap(),0);
 }
}
fn prot_and_va_tests(){
 let user=[0x0080000000000c8bu64,0x00a0000000000c8b,0x00c0000000000c8b];for (i,p) in user.into_iter().enumerate(){let prot=Prot::from_pte(p);assert_eq!(prot.leaf_bits()|0x803,p);assert!(prot.allows_gpu(i!=1,i!=0));if i<2{assert!(!prot.allows_gpu(i==1,i==0));}}
 for p in [0x00c000000000044bu64,0x00e000000000040b,0x00c0000000000443,0x00e0000000000403,0x00c0000000000447,0x0080000000000447,0x00200000000004c7]{assert_eq!(Prot::from_pte(p).leaf_bits()|3,p);}
 let ranges=reserved_ranges();assert_eq!(ranges,[0x1000000000..0x1000010000,0x7000000000..0x7100000000]);assert_eq!(ranges[1].end-ranges[1].start,4u64<<30);
}
fn main(){let args:Vec<_>=std::env::args().collect();for id in 0..12{assert_c_parity(&mutation(id),id,&args[1],&args[2]);}root_admission_tests();publication_tests();prot_and_va_tests();println!("12 C/Rust firmware-graph parity fixtures; 195 flush, 128 TTBAT ownership rejections; stopped publication ordering; live link ownership/global TLBI; exact M4 leaf classes/VA reservations passed (mock hardware; PPL lease mocked and tested separately)");}
'''
harness = pre + methods + post + prot_constants + prot_impl + reserved
(OUT/'check-vm.rs').write_text(harness)

golden = OUT/'golden'
unpack_fixtures(golden)
fixtures = json.loads((FIXTURES/'vm-graph.json').read_text())
accepted = ','.join('true' if record['return_errno']==0 else 'false' for record in fixtures['cases'])
harness += '\nconst ACCEPTED:[bool;12]=['+accepted+'];\n'
(OUT/'check-vm.rs').write_text(harness)
subprocess.run(rustc()+['--edition','2021','--cfg','CONFIG_DRM_ASAHI_G16',str(OUT/'check-vm.rs'),'-o',str(OUT/'check-vm')],check=True)
r = subprocess.run([str(OUT/'check-vm'),str(golden),str(OUT)],text=True,capture_output=True)
print(r.stdout,end='');print(r.stderr,end='')
(OUT/'vm-host-tests.log').write_text(r.stdout+r.stderr)
raise SystemExit(r.returncode)
