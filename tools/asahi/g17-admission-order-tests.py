#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""The production ioctl sizing callback must not allocate physical channels.

A mock physical allocator records demand while reverse timeline dependencies
remain unsignalled. Restoring the eager callback must fail this same control.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/g17'


def method(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    code = r'''
#![allow(dead_code)]
use std::sync::atomic::{AtomicUsize,AtomicBool,Ordering};
type Result<T=()> = std::result::Result<T,i32>;
const ECANCELED:i32=-125;const EIO:i32=-5;const EFAULT:i32=-14;const EINVAL:i32=-22;
mod g17 {
pub mod fw {
 pub mod compute_pool {
  pub fn required_blocks(_:usize,cores:u32,pages:u16)->crate::Result<usize> {
   if cores==0 {Err(-22)} else {Ok(21+usize::from(pages))}
  }
 }
 pub mod m5 { pub mod pool { #[derive(Clone,Copy,Debug,PartialEq)] pub struct PoolGeometry(pub u32); } }
}
pub mod freelist {
 #[derive(Debug,PartialEq)]
 pub enum ComputeCapacity { Blocks(usize), M5(Option<super::fw::m5::pool::PoolGeometry>) }
}
pub mod render_resources { #[derive(Clone,Copy)] pub struct Plan { pub geometry: super::fw::m5::pool::PoolGeometry } }
pub mod command {
 pub struct ScratchRequest(pub u16);
 impl ScratchRequest { pub fn pages(&self)->u16{self.0} }
 pub enum Validated {
  Render,
  Compute { plan: Option<super::render_resources::Plan>, scratch: ScratchRequest },
 }
}
pub mod runtime {
 use super::*;
 use crate::{Result,ECANCELED,EIO,EFAULT,EINVAL};
 use std::sync::atomic::{AtomicUsize,AtomicBool,Ordering};
 pub struct Status(pub i32);
 impl Status {fn get(&self)->i32{self.0}}
 pub struct Context {pub status:Status,pub m5:bool}
 impl Context {fn status(&self)->&Status{&self.status} fn is_current(&self)->bool{true} fn is_m5(&self)->bool{self.m5}}
 pub struct Shared {pub compute_blocks:usize,pub active_cores:u32,pub crashed:AtomicBool}
 pub struct Admission;
 pub struct Backend {pub shared:Shared,pub physical:AtomicUsize,pub closed:AtomicBool,pub context:Context}
 impl Backend {
  pub fn ensure_compute(&self,_:freelist::ComputeCapacity,_:&Admission)->Result {self.physical.fetch_add(1,Ordering::SeqCst);Ok(())}
''' + method('runtime.rs', 'pub(super) fn compute_capacity(').replace('pub(super) ', 'pub ', 1) + '\n' + method('runtime.rs', 'fn admission_open(').replace('fn ', 'pub fn ', 1) + r'''
 }
 pub trait Api {fn ensure_compute(&self,command:&command::Validated)->Result;}
 impl Api for Backend {
''' + method('runtime/backend.rs', 'fn ensure_compute(') + r'''
 }
}
}
use g17::{command::{ScratchRequest,Validated},freelist::ComputeCapacity,fw::m5::pool::PoolGeometry,render_resources::Plan,runtime::*};
fn compute(pages:u16)->Validated {Validated::Compute{plan:None,scratch:ScratchRequest(pages)}}
fn backend(cores:u32,m5:bool)->Backend {
 Backend{shared:Shared{compute_blocks:21,active_cores:cores,crashed:AtomicBool::new(false)},physical:AtomicUsize::new(0),closed:AtomicBool::new(false),context:Context{status:Status(0),m5}}
}
fn main(){
 let backend=backend(10,false);
 // Four clients each enqueue nine queues in reverse dependency order. The
 // final host signal cannot be submitted if ioctl admission blocks on channels.
 for _client in 0..4 {for _queue in (0..9).rev() {<Backend as Api>::ensure_compute(&backend,&compute(0)).unwrap();}}
 assert_eq!(backend.physical.load(Ordering::SeqCst),0,"unsignalled work must not pin physical capacity");
 assert_eq!(backend.compute_capacity(&compute(3)),Ok(ComputeCapacity::Blocks(24)));
 Backend::ensure_compute(&backend,backend.compute_capacity(&compute(0)).unwrap(),&Admission).unwrap();
 assert_eq!(backend.physical.load(Ordering::SeqCst),1);
 let invalid=self::backend(0,false);
 assert_eq!(<Backend as Api>::ensure_compute(&invalid,&compute(0)),Err(-22));
 assert_eq!(invalid.physical.load(Ordering::SeqCst),0);
 assert_eq!(<Backend as Api>::ensure_compute(&backend,&Validated::Render),Err(EINVAL));
 // M5 sizes a compute pool by its plan's geometry; that, too, admits nothing physical.
 let m5=self::backend(0,true);
 let planned=Validated::Compute{plan:Some(Plan{geometry:PoolGeometry(7)}),scratch:ScratchRequest(0)};
 for _ in 0..36 {<Backend as Api>::ensure_compute(&m5,&planned).unwrap();}
 assert_eq!(m5.compute_capacity(&planned),Ok(ComputeCapacity::M5(Some(PoolGeometry(7)))));
 assert_eq!(m5.physical.load(Ordering::SeqCst),0,"unsignalled work must not pin physical capacity");
 for _ in 0..512 {
  backend.closed.store(true,Ordering::Release);
  assert_eq!(backend.admission_open().and_then(|()| Backend::ensure_compute(&backend,ComputeCapacity::Blocks(21),&Admission)),Err(ECANCELED),"closed constructor must not install");
  assert_eq!(backend.physical.load(Ordering::SeqCst),1);
 }
 println!("PASS closed builders cannot install after logical close; production ioctl sizing validates 36 reverse-order submissions without physical admission, on G17P and M5; invalid sizing consumes no channel");
}
'''
    fixed = 'self.compute_capacity(command).map(|_| ())'
    assert code.count(fixed) == 1
    with tempfile.TemporaryDirectory(prefix='g17-admission-order-') as tmp:
        for name, source in [('fixed', code), ('eager-physical-admission', code.replace(fixed, 'self.compute_capacity(command).and_then(|required| Backend::ensure_compute(self, required, &Admission))')), ('ignore-logical-close', code.replace('self.closed.load(Ordering::Acquire)', 'false'))]:
            path = Path(tmp) / (name + '.rs')
            binary = path.with_suffix('')
            path.write_text(source)
            subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'fixed':
                assert result.returncode == 0, result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and ('must not pin physical capacity' if name == 'eager-physical-admission' else 'closed constructor must not install') in result.stderr, result.stderr
                print('PASS negative control:', name)


if __name__ == '__main__':
    main()
