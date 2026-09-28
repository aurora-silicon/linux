#!/usr/bin/env python3
"""Exercise actual kernel snapshot serialization and reserved fault storage.

Offline only. Mock the allocator and owned-memory readers; compile the real
constructor and record method so size/overflow/rollback checks stay production code.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=(Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g16_fault.rs'))
a = p.parse_args()
s = a.source.read_text()
methods = s[s.index('    pub(crate) fn new('):s.index('    pub(crate) fn buffer(')]
harness = r'''
use std::{ops::{Deref,DerefMut},sync::atomic::{AtomicBool,AtomicU64,Ordering}};
type Result<T=()> = std::result::Result<T,i32>;
const EINVAL:i32=-22;const EOVERFLOW:i32=-75;const E2BIG:i32=-7;
#[derive(Clone,Copy,PartialEq,Debug)]enum Flags{Kernel,Nowait}
const GFP_KERNEL:Flags=Flags::Kernel;const GFP_NOWAIT:Flags=Flags::Nowait;
static FAIL_ALLOC:AtomicBool=AtomicBool::new(false);
struct KVVec<T>(Vec<T>);
impl KVVec<u8>{
 fn with_capacity(len:usize,flags:Flags)->Result<Self>{
  assert_eq!(flags,GFP_KERNEL,"reserve must precede the fault path");
  if FAIL_ALLOC.swap(false,Ordering::SeqCst){return Err(-12)}
  Ok(Self(Vec::with_capacity(len)))
 }
 fn resize(&mut self,len:usize,value:u8,flags:Flags)->Result{
  if len>self.0.capacity(){
   assert_eq!(flags,GFP_KERNEL,"fault recording tried to grow reserved storage");
   if FAIL_ALLOC.swap(false,Ordering::SeqCst){return Err(-12)}
  }
  self.0.resize(len,value);Ok(())
 }
 fn truncate(&mut self,len:usize){self.0.truncate(len)}
}
impl<T>Deref for KVVec<T>{type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T>DerefMut for KVVec<T>{fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
struct Monotonic;
impl Monotonic{fn ktime_get()->u64{static T:AtomicU64=AtomicU64::new(1);T.fetch_add(1,Ordering::SeqCst)}}
struct Dump {bytes:KVVec<u8>,count:u32,limit:usize}
impl Dump {
/*METHODS*/
}
fn main(){
 FAIL_ALLOC.store(true,Ordering::SeqCst);
 assert!(matches!(Dump::with_format(9,b"M4FWD001",108),Err(-12)));
 let mut d=Dump::with_format(9,b"M4FWD001",108).unwrap();
 let capacity=d.bytes.capacity();let address=d.bytes.as_ptr();
 let constructed=u64::from_le_bytes(d.bytes[24..32].try_into().unwrap());
 d.begin(11);
 assert_eq!(u64::from_le_bytes(d.bytes[16..24].try_into().unwrap()),11);
 assert!(u64::from_le_bytes(d.bytes[24..32].try_into().unwrap())>constructed);
 assert_eq!(d.record("oversize",0,5, |_|panic!("oversize read")),Err(E2BIG));
 assert_eq!(d.record("overflow",0,usize::MAX, |_|panic!("overflow read")),Err(EOVERFLOW));
 assert_eq!(d.record("",0,4, |_|panic!("empty name read")),Err(EINVAL));
 assert_eq!(d.bytes.len(),40);assert_eq!(d.count,0);
 // Allocation is now unavailable, as it can be after a GPU fault. Every
 // bounded append must still work from the reserve, without consuming this
 // injected allocation failure or entering a blocking allocator.
 FAIL_ALLOC.store(true,Ordering::SeqCst);
 assert_eq!(d.record("bad-read",0,4, |out|{out[0]=255;Err(-5)}),Err(-5));
 assert_eq!(d.bytes.len(),40);assert_eq!(d.count,0);
 d.record("good",0x1234,4,|out|{out.copy_from_slice(&[1,2,3,4]);Ok(())}).unwrap();
 assert_eq!(d.bytes.len(),108);assert_eq!(d.count,1);
 assert_eq!(&d.bytes[40..72],b"good\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
 assert_eq!(&d.bytes[104..108],&[1,2,3,4]);
 assert_eq!(d.bytes.capacity(),capacity);assert_eq!(d.bytes.as_ptr(),address);
 assert!(FAIL_ALLOC.swap(false,Ordering::SeqCst),"append attempted allocation");
 assert_eq!(d.record("full",0,1, |_|panic!("full read")),Err(E2BIG));
 d.count=u32::MAX;
 assert_eq!(d.record("count-overflow",0,1, |_|panic!("counter overflow")),Err(EOVERFLOW));
 let m4=Dump::new(1).unwrap();let m3=Dump::new_m3().unwrap();
 assert_eq!(m4.limit,16*1024*1024);assert_eq!(m3.limit,2*1024*1024);
 println!("PASS reserve failure, no-allocation fault append, fresh capture time, bounds and read rollback");
}
'''.replace('/*METHODS*/',methods)
with tempfile.TemporaryDirectory(prefix='fault-bounds-') as tmp:
    source=Path(tmp)/'test.rs';source.write_text(harness)
    binary=Path(tmp)/'test'
    subprocess.run(['rustc','--edition=2021',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
