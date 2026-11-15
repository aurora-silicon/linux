#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2026 Eryk Wieliczko
"""Compile the production submission boundary and observe output-sync side effects."""
from common import kernel_root, output_dir, rustc
import subprocess
s=(kernel_root()/'drivers/gpu/drm/asahi/g16/queue.rs').read_text();a=s.index('    fn submit(',s.index('impl crate::queue::Queue for Queue'));b=s.index('{',a);i=b+1;n=1
while n:n+=(s[i]=='{')-(s[i]=='}');i+=1
body=s[a:i]
r=r'''
#![allow(dead_code,unused_variables)]
use std::pin::Pin;use std::sync::atomic::{AtomicU32,Ordering};
type Result<T=()> = std::result::Result<T,i32>;const ERESTARTSYS:i32=512;const EBUSY:i32=16;
type KVec<T>=Vec<T>;type KBox<T>=Box<T>;
mod xarray{pub struct XArray<T>(pub T);}
mod file{pub struct SyncItem;pub struct Object;}
mod driver{pub struct AsahiRegData<'a>(pub &'a ());}
static ACCEPTED:AtomicU32=AtomicU32::new(0);static REFUSED:AtomicU32=AtomicU32::new(0);
struct Syncs;impl Syncs{fn new(_v:Vec<file::SyncItem>,_n:usize)->Self{Self}fn accept(&self,_f:&u32){ACCEPTED.fetch_add(1,Ordering::SeqCst);}fn refuse(&self,_t:&(),error:i32){assert_ne!(error,ERESTARTSYS);REFUSED.fetch_add(1,Ordering::SeqCst);}}
struct Status;impl Status{fn record_if_device_loss(&self,_e:i32){}}
struct Vm{status:Status}struct Queue{vm:Vm,timeline:(),next:Result<u32>}
impl Queue{fn submit_job(&self,_id:u64,_syncs:&Syncs,_count:usize,_raw:&[u8],_objects:Pin<&xarray::XArray<KBox<file::Object>>>)->Result<u32>{self.next}
__SUBMIT__
}
#[test]fn restart_has_no_output_fence_and_retry_accepts_once(){
 let mut q=Queue{vm:Vm{status:Status},timeline:(),next:Err(ERESTARTSYS)};
 let data=driver::AsahiRegData(&());let objects=xarray::XArray(Box::new(file::Object));
 assert_eq!(q.submit(&data,1,vec![file::SyncItem],0,&[],Pin::new(&objects)),Err(ERESTARTSYS));
 assert_eq!(REFUSED.load(Ordering::SeqCst),0);assert_eq!(ACCEPTED.load(Ordering::SeqCst),0);
 q.next=Ok(42);q.submit(&data,1,vec![file::SyncItem],0,&[],Pin::new(&objects)).unwrap();
 assert_eq!(REFUSED.load(Ordering::SeqCst),0);assert_eq!(ACCEPTED.load(Ordering::SeqCst),1);
 q.next=Err(EBUSY);assert_eq!(q.submit(&data,2,vec![file::SyncItem],0,&[],Pin::new(&objects)),Err(EBUSY));
 assert_eq!(REFUSED.load(Ordering::SeqCst),1);assert_eq!(ACCEPTED.load(Ordering::SeqCst),1);
}
'''.replace('__SUBMIT__',body)
out=output_dir();p=out/'submit-restart.rs';p.write_text(r);subprocess.run(rustc()+['--edition=2021','--test',str(p),'-o',str(out/'submit-restart')],check=True);subprocess.run([str(out/'submit-restart')],check=True)
