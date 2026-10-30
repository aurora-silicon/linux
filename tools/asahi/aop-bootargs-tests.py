#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production bootarg patching with synthetic MMIO."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HARNESS = r'''
#![allow(dead_code)]
use std::{cell::{Cell,RefCell},marker::PhantomData,mem,ops::{Deref,DerefMut}};
type Result<T=()> = std::result::Result<T,i32>;
const EINVAL:i32=-22; const EIO:i32=-5; const GFP_KERNEL:u32=0;
const AOP_MMIO_SIZE:usize=128; const BOOTARGS_OFFSET:usize=16; const BOOTARGS_SIZE:usize=20;
struct KVec<T>(Vec<T>);
impl<T:Clone> KVec<T>{fn from_elem(v:T,n:usize,_:u32)->Result<Self>{Ok(Self(vec![v;n]))}}
impl<T> Deref for KVec<T>{type Target=Vec<T>;fn deref(&self)->&Vec<T>{&self.0}}
impl<T> DerefMut for KVec<T>{fn deref_mut(&mut self)->&mut Vec<T>{&mut self.0}}
struct Region<const N:usize>;
struct Device{bytes:RefCell<Vec<u8>>,writes:RefCell<Vec<usize>>,fail:Cell<bool>}
struct Mmio<'a,T>{dev:&'a Device,_p:PhantomData<T>}
impl<'a,T> Mmio<'a,T>{
 fn size(&self)->usize{self.dev.bytes.borrow().len()}
 fn read32(&self,at:usize)->u32{self.try_read32(at).unwrap()}
 fn try_read32(&self,at:usize)->Result<u32>{assert_eq!(at%4,0);Ok(u32::from_le_bytes(self.dev.bytes.borrow()[at..at+4].try_into().unwrap()))}
 fn try_write32(&self,v:u32,at:usize)->Result{assert_eq!(at%4,0);self.dev.writes.borrow_mut().push(at);if !self.dev.fail.get(){self.dev.bytes.borrow_mut()[at..at+4].copy_from_slice(&v.to_le_bytes());}Ok(())}
 fn try_cast<U>(&self)->Result<Mmio<'a,U>>{Ok(Mmio{dev:self.dev,_p:PhantomData})}
}
struct IoSlice<'a>{dev:&'a Device,start:usize,end:usize}
impl IoSlice<'_>{
 fn copy_to_slice(&self,v:&mut[u8]){v.copy_from_slice(&self.dev.bytes.borrow()[self.start..self.end]);}
 fn copy_from_slice(&self,v:&[u8]){self.dev.bytes.borrow_mut()[self.start..self.end].copy_from_slice(v);}
}
macro_rules! io_project {($io:ident,[try: $start:ident .. $($end:tt)*])=>{IoSlice{dev:$io.dev,start:$start,end:$($end)*}};}
struct Aop;
impl Aop { METHOD }
fn fixture(offset:usize,table:&[u8])->Device{
 let mut bytes=vec![0xa5;AOP_MMIO_SIZE];bytes[16..20].copy_from_slice(&(offset as u32).to_le_bytes());bytes[20..24].copy_from_slice(&(table.len() as u32).to_le_bytes());
 if offset+table.len()<=bytes.len(){bytes[offset..offset+table.len()].copy_from_slice(table);}
 Device{bytes:RefCell::new(bytes),writes:RefCell::new(Vec::new()),fail:Cell::new(false)}
}
fn patch(dev:&Device,word:bool)->Result{Aop.patch_bootargs(&Mmio{dev,_p:PhantomData::<Region<AOP_MMIO_SIZE>>},&[(1,0x12345678),(2,0x87654321)],word)}
fn main(){
 let table:[u8;24]=[1,0,0,0,4,0,0,0,0,0,0,0,2,0,0,0,4,0,0,0,0,0,0,0];
 for offset in 32..36 {for word in [false,true] {
  let d=fixture(offset,&table);let mut expected=d.bytes.borrow().clone();expected[offset+8..offset+12].copy_from_slice(&0x12345678u32.to_le_bytes());expected[offset+20..offset+24].copy_from_slice(&0x87654321u32.to_le_bytes());
  assert_eq!(patch(&d,word),Ok(()));assert_eq!(*d.bytes.borrow(),expected,"adjacent bytes changed");
  assert!(d.writes.borrow().iter().all(|&x|x%4==0&&x>=offset&!3&&x+4<=(offset+table.len()+3)&!3));
  if word {assert!(!d.writes.borrow().is_empty());}else{assert!(d.writes.borrow().is_empty());}
  let writes=d.writes.borrow().len();assert_eq!(patch(&d,word),Ok(()));assert_eq!(d.writes.borrow().len(),writes,"unchanged words rewritten");
 }}
 let mut malformed=table;malformed[16..20].copy_from_slice(&9u32.to_le_bytes());
 for word in [false,true]{let d=fixture(35,&malformed);let before=d.bytes.borrow().clone();assert_eq!(patch(&d,word),Err(EINVAL));assert_eq!(*d.bytes.borrow(),before);assert!(d.writes.borrow().is_empty());}
 for tail in 1..8 {let d=fixture(35,&table[..12+tail]);assert_eq!(patch(&d,true),Err(EINVAL));assert!(d.writes.borrow().is_empty());}
 let d=fixture(35,&table);d.fail.set(true);assert_eq!(patch(&d,true),Err(EIO),"readback error ignored");
 assert_eq!(patch(&fixture(35,&[]),true),Err(EINVAL));assert_eq!(patch(&fixture(35,&[]),false),Ok(()));
 assert_eq!(patch(&fixture(120,&table),true),Err(EINVAL));
 println!("PASS aligned/unaligned byte and word access, unchanged writes, malformed tables, bounds and failed readback");
}
'''


def method(source):
    start = source.index('    fn patch_bootargs(')
    at = source.index('{', start)
    depth = 1
    end = at + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'drivers/soc/apple/aop.rs')
    args = parser.parse_args()
    production = method(args.source.read_text())
    macros = ''.join('macro_rules! ' + name + ' {($($t:tt)*)=>{()};}\n' for name in ['dev_info', 'dev_err'] if name + '!' in production)
    code = HARNESS.replace('METHOD', production).replace('struct Aop;', macros + 'struct Aop;')
    with tempfile.TemporaryDirectory(prefix='aop-bootargs-') as tmp:
        p = Path(tmp) / 'test.rs'
        binary = p.with_suffix('')
        p.write_text(code)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(p), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        bad = code.replace('if aop_mmio.try_read32(address)? != value {', 'if false {')
        assert bad != code
        p.write_text(bad)
        subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(p), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert result.returncode != 0 and 'readback error ignored' in result.stderr
        print('PASS negative control: missing write verification')


if __name__ == '__main__':
    main()
