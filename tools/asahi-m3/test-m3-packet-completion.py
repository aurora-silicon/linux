#!/usr/bin/env python3
"""Run the real M3 packet finisher with observable host-side fence/VM owners.

No GPU access. Exercise late callbacks and competing completion/cancel threads;
verify one signal, immutable error, and no release of unretired DMA ownership.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=(Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi/m3_submit.rs'))
a = p.parse_args()
source = a.source.read_text()
start = source.index('    fn finish(')
end = source.index('\n}\npub(crate) struct Job', start)
callback = source[start:end]
start = source.index('    fn timed_out(')
end = source.index('    fn cancel(', start)
timeout = source[start:end]
harness = r'''
use std::sync::{Arc, Barrier, Mutex as StdMutex};
use std::ops::{Deref,DerefMut};
const ENODEV:Error=Error(-19); const EIO:Error=Error(-5); const ETIMEDOUT:Error=Error(-110);
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};
#[derive(Clone,Copy)] struct Error(i32);
impl Error { fn to_errno(self)->i32 {self.0} }
type Result = std::result::Result<(),Error>;
struct Mutex<T>(StdMutex<T>);
impl<T> Mutex<T> { fn lock(&self)->std::sync::MutexGuard<'_,T> {self.0.lock().unwrap()} }
struct Status(AtomicI32);
impl Status { fn record(&self,e:i32){let _=self.0.compare_exchange(0,e,Ordering::SeqCst,Ordering::SeqCst);} }
struct Vm(Status);
impl Vm {fn status(&self)->&Status{&self.0}}
struct Fence {error:AtomicI32, signals:AtomicUsize}
impl Fence {
 fn set_error(&self,e:Error){assert_eq!(self.signals.load(Ordering::SeqCst),0,"error after signal");self.error.store(e.0,Ordering::SeqCst);}
 fn signal(&self){assert_eq!(self.signals.fetch_add(1,Ordering::SeqCst),0,"double signal");}
}
struct DmaOwner(Arc<AtomicUsize>);
impl Drop for DmaOwner {fn drop(&mut self){self.0.fetch_add(1,Ordering::SeqCst);}}
struct Packet {vm:Vm, completion:Fence,finish_claimed:AtomicBool,vm_job:Mutex<Option<DmaOwner>>}
mod g16_memory {pub fn publish(){}}
impl Packet {
/*FINISH*/
 fn new()->(Arc<Self>,Arc<AtomicUsize>){
  let drops=Arc::new(AtomicUsize::new(0));
  (Arc::new(Self{vm:Vm(Status(AtomicI32::new(0))),completion:Fence{error:AtomicI32::new(0),signals:AtomicUsize::new(0)},finish_claimed:AtomicBool::new(false),vm_job:Mutex(StdMutex::new(Some(DmaOwner(drops.clone()))))}),drops)
 }
}
fn verify(p:&Packet,drops:&AtomicUsize){
 let error=p.completion.error.load(Ordering::SeqCst);
 assert_eq!(p.completion.signals.load(Ordering::SeqCst),1);
 assert_eq!(p.vm.status().0.load(Ordering::SeqCst),error);
 assert_eq!(drops.load(Ordering::SeqCst),usize::from(error==0));
 assert_eq!(p.vm_job.lock().is_some(),error!=0,"failure is not retirement");
}

struct Health(bool);
impl Health {fn healthy(&self)->bool{self.0}fn mark_failed(&self){panic!("diagnostic-only path");}}
struct Runtime {health:Health}
impl Runtime {fn health(&self)->&Health{&self.health}}
struct Job {shared:Mutex<Option<Runtime>>,packet:Arc<Packet>}
mod m3_params {pub fn timeout_nohang()->bool{true}}
mod sched {
 use super::*;
 #[derive(Debug,PartialEq)]pub enum Status{NoHang,NoDevice}
 pub struct Job<T>(pub T);
 impl<T>Deref for Job<T>{type Target=T;fn deref(&self)->&T{&self.0}}
 impl<T>DerefMut for Job<T>{fn deref_mut(&mut self)->&mut T{&mut self.0}}
}
impl Job {
/*TIMEOUT*/
}
fn main(){
 for healthy in [None,Some(true),Some(false)]{
  let (p,d)=Packet::new();
  let mut job=sched::Job(Job{shared:Mutex(StdMutex::new(healthy.map(|healthy|Runtime{health:Health(healthy)}))),packet:p.clone()});
  let status=Job::timed_out(&mut job);
  if healthy==Some(true) {
   assert_eq!(status,sched::Status::NoHang);assert_eq!(p.completion.signals.load(Ordering::SeqCst),0);
   p.finish(Ok(()));verify(&p,&d);
   assert_eq!(Job::timed_out(&mut job),sched::Status::NoHang);verify(&p,&d);
  }else{assert_eq!(status,sched::Status::NoDevice);verify(&p,&d);assert_eq!(p.completion.error.load(Ordering::SeqCst),if healthy.is_none(){-19}else{-5});}
 }
 println!("PASS healthy pending/retired, failed and removed scheduler timeout states");
 let (p,d)=Packet::new();p.finish(Ok(()));p.finish(Err(Error(-125)));verify(&p,&d);
 assert_eq!(p.completion.error.load(Ordering::SeqCst),0);println!("PASS cancellation after retirement");
 let (p,d)=Packet::new();p.finish(Err(Error(-5)));p.finish(Err(Error(-110)));p.finish(Ok(()));verify(&p,&d);
 assert_eq!(p.completion.error.load(Ordering::SeqCst),-5);println!("PASS first error and DMA ownership preserved");
 for _ in 0..200 {
  let (p,d)=Packet::new();let barrier=Arc::new(Barrier::new(8));
  std::thread::scope(|scope| {for i in 0..8 {let p=p.clone();let b=barrier.clone();scope.spawn(move||{b.wait();p.finish(if i==0 {Ok(())}else{Err(Error(-i))});});}});
  verify(&p,&d);
 }
 println!("PASS 200 concurrent completion/cancellation races");
}
'''.replace('/*FINISH*/', callback).replace('/*TIMEOUT*/', timeout)
with tempfile.TemporaryDirectory(prefix='m3-packet-') as directory:
    path = Path(directory)/'test.rs'
    path.write_text(harness)
    binary = Path(directory)/'test'
    subprocess.run(['rustc', '--edition=2021', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
