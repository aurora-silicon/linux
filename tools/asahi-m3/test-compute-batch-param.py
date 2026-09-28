#!/usr/bin/env python3
"""Exercise the production compute-batch parser, setter and getter on the host."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/gpu/drm/asahi/m3_params.rs').read_text()
storage = (root / 'drivers/gpu/drm/asahi/m3_compute_storage.rs').read_text()
slots = re.search(r'const SLOTS\s*:\s*usize\s*=\s*(\d+)', storage).group(1)

def function(name):
    start = re.search(r'(?m)^(?:pub\(crate\) )?(?:unsafe )?fn ' + name + r'\(', source).start()
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

harness = r'''
#![allow(non_camel_case_types, non_upper_case_globals)]
use std::ffi::{c_char,c_int,c_void,CString};
use std::sync::atomic::{AtomicU64,Ordering};
mod kernel { pub mod bindings {
 pub struct Args {pub arg:*mut std::ffi::c_void}
 pub struct kernel_param {pub __bindgen_anon_1:Args}
}}
struct Error;const EINVAL:Error=Error;
impl Error {fn to_errno(&self)->i32{-22}}
mod m3_compute_storage {pub const SLOTS:usize=/*SLOTS*/;}
mod module_parameters {
 pub struct Param;
 pub static m3_compute_batch_size:Param=Param;
 impl Param {pub fn value(&self)->&'static u32{&5}}
}
static M3_COMPUTE_BATCH_OVERRIDE:AtomicU64=AtomicU64::new(0);
/*FUNCTIONS*/
fn main(){
 let kp=kernel::bindings::kernel_param{__bindgen_anon_1:kernel::bindings::Args{
  arg:std::ptr::addr_of!(M3_COMPUTE_BATCH_OVERRIDE) as *mut c_void}};
 let write=|text:&str|unsafe {set_param(CString::new(text).unwrap().as_ptr(),&kp,parse_compute_batch_override)};
 assert_eq!(compute_batch_size(),5);
 for n in 1..=m3_compute_storage::SLOTS {
  assert_eq!(write(&n.to_string()),0);assert_eq!(compute_batch_size(),n);
 }
 assert_eq!(write(" 0x10\n"),0);assert_eq!(compute_batch_size(),16);
 for bad in ["", "-1", "17", "0x11", "0x", "1 2", "1garbage", "18446744073709551616"] {
  assert_eq!(write(bad),-22,"{bad}");assert_eq!(compute_batch_size(),16);
 }
 assert_eq!(unsafe{set_param(std::ptr::null(),&kp,parse_compute_batch_override)},-22);
 assert_eq!(unsafe{set_param([255u8,0].as_ptr().cast(),&kp,parse_compute_batch_override)},-22);
 assert_eq!(compute_batch_size(),16);
 let packet_snapshot=compute_batch_size();assert_eq!(write("1"),0);
 assert_eq!(packet_snapshot,16);assert_eq!(compute_batch_size(),1);
 assert_eq!(write("0"),0);assert_eq!(compute_batch_size(),5);
 println!("PASS actual parser/setter/getter: bounds, invalid-write retention, null/UTF-8 rejection, packet snapshot and boot fallback");
}
'''.replace('/*SLOTS*/', slots).replace('/*FUNCTIONS*/', '\n'.join(function(n) for n in
    ('parse_u64', 'set_param', 'parse_compute_batch_override', 'compute_batch_size')))
with tempfile.TemporaryDirectory(prefix='m3-compute-param-') as temp:
    path = Path(temp) / 'test.rs'
    path.write_text(harness)
    binary = path.with_suffix('')
    subprocess.run(['rustc', '--edition=2021', '-D', 'warnings', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
