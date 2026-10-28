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
const ECANCELED:i32=-125;const EIO:i32=-5;const EFAULT:i32=-14;
mod g17 {pub mod fw {pub mod compute_pool {
 pub fn required_blocks(_:usize,cores:u32,pages:u16)->crate::Result<usize> {
  if cores==0 {Err(-22)} else {Ok(21+usize::from(pages))}
 }
}}}
struct Status(i32);
impl Status {fn get(&self)->i32{self.0}}
struct Context {status:Status}
impl Context {fn status(&self)->&Status{&self.status} fn is_current(&self)->bool{true}}
struct Shared {compute_blocks:usize,active_cores:u32,crashed:AtomicBool}
struct Backend {shared:Shared,physical:AtomicUsize,closed:AtomicBool,context:Context}
impl Backend {
 fn ensure_compute(&self,_:u16)->Result {self.physical.fetch_add(1,Ordering::SeqCst);Ok(())}
''' + method('runtime.rs', 'fn admission_open(') + r'''
}
trait Api {fn ensure_compute(&self,scs_pages:u16)->Result;}
impl Api for Backend {
''' + method('runtime/backend.rs', 'fn ensure_compute(') + r'''
}
fn main(){
 let backend=Backend{shared:Shared{compute_blocks:21,active_cores:10,crashed:AtomicBool::new(false)},physical:AtomicUsize::new(0),closed:AtomicBool::new(false),context:Context{status:Status(0)}};
 // Four clients each enqueue nine queues in reverse dependency order. The
 // final host signal cannot be submitted if ioctl admission blocks on channels.
 for _client in 0..4 {for _queue in (0..9).rev() {<Backend as Api>::ensure_compute(&backend,0).unwrap();}}
 assert_eq!(backend.physical.load(Ordering::SeqCst),0,"unsignalled work must not pin physical capacity");
 Backend::ensure_compute(&backend,0).unwrap();
 assert_eq!(backend.physical.load(Ordering::SeqCst),1);
 let invalid=Backend{shared:Shared{compute_blocks:21,active_cores:0,crashed:AtomicBool::new(false)},physical:AtomicUsize::new(0),closed:AtomicBool::new(false),context:Context{status:Status(0)}};
 assert_eq!(<Backend as Api>::ensure_compute(&invalid,0),Err(-22));
 assert_eq!(invalid.physical.load(Ordering::SeqCst),0);
 for _ in 0..512 {
  backend.closed.store(true,Ordering::Release);
  assert_eq!(backend.admission_open().and_then(|()| Backend::ensure_compute(&backend,0)),Err(ECANCELED),"closed constructor must not install");
  assert_eq!(backend.physical.load(Ordering::SeqCst),1);
 }
 println!("PASS closed builders cannot install after logical close; production ioctl sizing validates 36 reverse-order submissions without physical admission; invalid sizing consumes no channel");
}
'''
    fixed = 'crate::g17::fw::compute_pool::required_blocks(self.shared.compute_blocks, self.shared.active_cores, scs_pages)\n            .map(|_| ())'
    assert code.count(fixed) == 1
    with tempfile.TemporaryDirectory(prefix='g17-admission-order-') as tmp:
        for name, source in [('fixed', code), ('eager-physical-admission', code.replace(fixed, 'Backend::ensure_compute(self, scs_pages)')), ('ignore-logical-close', code.replace('self.closed.load(Ordering::Acquire)', 'false'))]:
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
