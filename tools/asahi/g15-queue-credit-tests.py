#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run actual native logical-queue credit/new/drop bodies on host services.

Scheduler/VM/allocation services are models. This tests host capacity and Rust
field-drop ordering, not hardware execution-state reservation or DRM scheduling.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def extract(text, marker):
    start = text.index(marker); opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


CODE = r'''
#![allow(dead_code)]
use std::{sync::{Arc as StdArc,Mutex,Barrier,OnceLock,atomic::{AtomicBool,AtomicI32,AtomicU32,AtomicU64,Ordering}},ops::Deref,cell::Cell};
#[derive(Copy,Clone,Debug,PartialEq,Eq)]struct Error(i32);
type Result<T=()> = std::result::Result<T,Error>;
const EIO:Error=Error(-5);const EINVAL:Error=Error(-22);const ENOMEM:Error=Error(-12);const GFP_KERNEL:()=();
thread_local!{static FAIL:Cell<u32>=const{Cell::new(0)};static RACE_FIRST:Cell<bool>=const{Cell::new(false)};}
static RACE_GATE:OnceLock<Barrier>=OnceLock::new();
trait RacyCounter{fn racy_reserve(&self)->std::result::Result<u32,u32>;}
impl RacyCounter for StdArc<AtomicU32>{fn racy_reserve(&self)->std::result::Result<u32,u32>{
 let count=self.load(Ordering::Acquire);
 RACE_FIRST.with(|flag|if flag.replace(false){RACE_GATE.get_or_init(||Barrier::new(16)).wait();});
 if count<hw::sizes::MAX_QUEUES{self.store(count+1,Ordering::Release);Ok(count)}else{Err(count)}
}}
fn failure(n:u32)->bool{FAIL.with(|f|f.get()==n)}
struct Arc<T>(StdArc<T>);impl<T> Clone for Arc<T>{fn clone(&self)->Self{Self(self.0.clone())}}
impl<T> Deref for Arc<T>{type Target=T;fn deref(&self)->&T{&self.0}}
impl<T> Arc<T>{fn wrap(v:T)->Self{Self(StdArc::new(v))}fn new(v:T,_:())->Result<Self>{if failure(4){Err(ENOMEM)}else{Ok(Self::wrap(v))}}}
struct KBox<T>(Box<T>);impl<T>KBox<T>{fn new(v:T,_:())->Result<Self>{if failure(5){Err(ENOMEM)}else{Ok(Self(Box::new(v)))}}}
struct Trace{counter:StdArc<AtomicU32>,drains:Mutex<Vec<u32>>,prunes:AtomicU32}
struct Device{trace:StdArc<Trace>}
struct Shared{logical_queues:StdArc<AtomicU32>,crashed:AtomicBool,dev:Device}
struct Status(AtomicI32);impl Status{fn get(&self)->i32{self.0.load(Ordering::SeqCst)}}
struct VmState{status:Status,trace:StdArc<Trace>}impl VmState{fn prune_registry(&self){self.trace.prunes.fetch_add(1,Ordering::SeqCst);}}
mod mmu{use super::*;pub struct Vm{pub state:Arc<VmState>}}
mod vm{use super::*;pub fn lookup(_:&Gpu,vm:&mmu::Vm)->Result<Arc<VmState>>{if failure(1){Err(ENOMEM)}else{Ok(vm.state.clone())}}}
mod hw{pub const USER_RANGE:std::ops::Range<u64>=0x100000000..0x2000000000;pub mod sizes{__LIMIT__}}
mod uapi{pub const drm_asahi_priority_DRM_ASAHI_PRIORITY_REALTIME:u32=3;}
const USC_WINDOW:u64=0x100000000;
struct Gpu{shared:Arc<Shared>}
struct Scheduler{trace:StdArc<Trace>}
impl Scheduler{fn new(dev:&Device)->Result<Self>{if failure(3){Err(ENOMEM)}else{Ok(Self{trace:dev.trace.clone()})}}}
impl Drop for Scheduler{fn drop(&mut self){let n=self.trace.counter.load(Ordering::SeqCst);assert!(n>0,"scheduler must drain before credit release");self.trace.drains.lock().unwrap().push(n);}}
struct Timeline;impl Timeline{fn new()->Result<Arc<Self>>{if failure(2){Err(ENOMEM)}else{Ok(Arc::wrap(Self))}}}
__STATE__
__CREDIT__
__CREDIT_IMPL__
__CREDIT_DROP__
__QUEUE__
__QUEUE_DROP__
impl Queue{__NEW__}
fn setup()->Gpu{
 let counter=StdArc::new(AtomicU32::new(0));let trace=StdArc::new(Trace{counter:counter.clone(),drains:Mutex::new(Vec::new()),prunes:AtomicU32::new(0)});
 Gpu{shared:Arc::wrap(Shared{logical_queues:counter,crashed:AtomicBool::new(false),dev:Device{trace}})}
}
fn vm(gpu:&Gpu,status:i32)->mmu::Vm{mmu::Vm{state:Arc::wrap(VmState{status:Status(AtomicI32::new(status)),trace:gpu.shared.dev.trace.clone()})}}
fn new(gpu:&Gpu,status:i32,priority:u32,base:u64)->Result<KBox<Queue>>{KBox::new(Queue::new(gpu,vm(gpu,status),priority,base)?,GFP_KERNEL)}
fn count(gpu:&Gpu)->u32{gpu.shared.logical_queues.load(Ordering::SeqCst)}
fn main(){
 // One device shared across different VM objects/files: exactly58 live queues.
 let gpu=setup();let mut queues=Vec::new();for _ in 0..hw::sizes::MAX_QUEUES{queues.push(new(&gpu,0,2,0x100000000).expect("live logical queue below cap"));}
 assert_eq!(count(&gpu),58,"reported logical capacity must be reserved");
 assert!(matches!(new(&gpu,0,2,0x100000000),Err(ENOMEM)),"59th device queue must refuse");
 assert_eq!(count(&gpu),58);drop(queues);
 assert_eq!(count(&gpu),0,"release returns all logical credits");
 assert_eq!(*gpu.shared.dev.trace.drains.lock().unwrap(),(1..=58).rev().collect::<Vec<_>>(),"every scheduler drains before its capacity returns");
 assert!(new(&gpu,0,2,0x100000000).is_ok(),"capacity reusable after drain");
 assert_eq!(count(&gpu),0);
 // Failure at lookup/timeline/scheduler/state/outerKBox must never leak a credit.
 for point in 1..=5{
  let gpu=setup();FAIL.with(|f|f.set(point));assert!(new(&gpu,0,2,0x100000000).is_err());FAIL.with(|f|f.set(0));
  assert_eq!(count(&gpu),0,"allocation failure must return logical credit");
  let drains=gpu.shared.dev.trace.drains.lock().unwrap().clone();
  assert_eq!(drains,if point>=4{vec![1]}else{vec![]},"allocation rollback drains before credit release");
  let retry=new(&gpu,0,2,0x100000000).expect("allocation failure must leave reusable capacity");assert_eq!(count(&gpu),1);drop(retry);assert_eq!(count(&gpu),0);
 }
 // Existing GPU/VM/range refusals do not consume capacity.
 for (status,priority,base) in [(-5,2,0x100000000),(0,4,0x100000000),(0,2,1),(0,2,0x2000000000)]{
  let gpu=setup();assert!(new(&gpu,status,priority,base).is_err());assert_eq!(count(&gpu),0);
 }
 let gpu=setup();gpu.shared.crashed.store(true,Ordering::SeqCst);assert!(new(&gpu,0,2,0x100000000).is_err());assert_eq!(count(&gpu),0);
 // CAS contention:128 acquisitions across16threads, held until all have attempted.
 let gpu=setup();let ready=StdArc::new(Barrier::new(17));let release=StdArc::new(Barrier::new(17));let successes=StdArc::new(AtomicU32::new(0));let mut threads=Vec::new();
 for _ in 0..16{let shared=gpu.shared.clone();let ready=ready.clone();let release=release.clone();let successes=successes.clone();threads.push(std::thread::spawn(move||{
  RACE_FIRST.with(|flag|flag.set(true));let mut held=Vec::new();for _ in 0..8{if let Ok(credit)=QueueCredit::acquire(&shared){successes.fetch_add(1,Ordering::SeqCst);held.push(credit);}}
  ready.wait();release.wait();drop(held);
 }));}
 ready.wait();assert_eq!(successes.load(Ordering::SeqCst),58,"concurrent reservations respect shared cap");assert_eq!(count(&gpu),58,"concurrent reservations respect shared cap");release.wait();for t in threads{t.join().unwrap();}assert_eq!(count(&gpu),0,"concurrent releases return capacity");
 // An already-corrupt/saturated counter must not wrap into an admitted credit.
 gpu.shared.logical_queues.store(u32::MAX,Ordering::SeqCst);assert!(matches!(QueueCredit::acquire(&gpu.shared),Err(ENOMEM)));assert_eq!(count(&gpu),u32::MAX);
 println!("PASS actual Queue::new/field order/credit:58/59,5 allocation failures,admission negatives,16-thread CAS and saturation");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    queue = (ROOT / "drivers/gpu/drm/asahi/g15/queue.rs").read_text()
    hardware = (ROOT / "drivers/gpu/drm/asahi/g15/hw.rs").read_text()
    limit = next(line.strip() for line in hardware.splitlines() if "const MAX_QUEUES:" in line)
    replacements = {"__LIMIT__": limit, "__STATE__": extract(queue, "pub(crate) struct QueueState {"),
        "__CREDIT__": extract(queue, "struct QueueCredit {"), "__CREDIT_IMPL__": extract(queue, "impl QueueCredit {"),
        "__CREDIT_DROP__": extract(queue, "impl Drop for QueueCredit {"), "__QUEUE__": extract(queue, "pub(crate) struct Queue {"),
        "__QUEUE_DROP__": extract(queue, "impl Drop for Queue {"), "__NEW__": extract(queue, "    pub(crate) fn new(gpu: &Gpu,")}
    code = CODE
    for old, new in replacements.items(): code = code.replace(old, new)
    variants = {"control": code}; expected = {}
    if args.mutations:
        changes = {
            "cap-not-enforced": ("count < hw::sizes::MAX_QUEUES", "count < u32::MAX", "59th device queue must refuse"),
            "counter-not-reserved": ("then(|| count + 1)", "then(|| count)", "reported logical capacity must be reserved"),
            "credit-not-released": ("let previous = self.shared.logical_queues.fetch_sub(1, Ordering::AcqRel);\n        debug_assert!(previous != 0);", "let _ = &self.shared;", "release returns all logical credits"),
            "release-before-drain": ("pub(crate) struct Queue {\n", "pub(crate) struct Queue {\n    _credit: QueueCredit,\n", "scheduler must drain before credit release"),
            "nonatomic-reservation": (".fetch_update(Ordering::AcqRel, Ordering::Acquire, |count| {\n                (count < hw::sizes::MAX_QUEUES).then(|| count + 1)\n            })", ".racy_reserve()", "concurrent reservations respect shared cap"),
            "advertised-cap-off-by-one": ("count < hw::sizes::MAX_QUEUES", "count <= hw::sizes::MAX_QUEUES", "59th device queue must refuse"),
        }
        for name, (old, new, message) in changes.items():
            if code.count(old) != 1: raise RuntimeError(f"{name}: mutation anchor count {code.count(old)}")
            variant = code.replace(old, new)
            if name == "release-before-drain":
                old_tail = "    // Last field: all preceding fields (especially scheduler) drop before capacity returns.\n    _credit: QueueCredit,\n"
                if variant.count(old_tail) != 1: raise RuntimeError("missing tail credit")
                variant = variant.replace(old_tail, "")
            variants[name] = variant; expected[name] = message
    variants["control-after"] = code
    receipt = {"scope": __doc__, "sources": {"queue.rs": hashlib.sha256(queue.encode()).hexdigest(),
        "hw.rs": hashlib.sha256(hardware.encode()).hexdigest()}, "variants": {}}
    for name, source in variants.items():
        path=args.output/(name+".rs");path.write_text(source);binary=args.output/name
        built=subprocess.run([os.environ.get("RUSTC","rustc"),"--edition=2021",str(path),"-o",str(binary)],capture_output=True,text=True)
        (args.output/(name+".build.log")).write_text(built.stdout+built.stderr)
        if built.returncode: raise RuntimeError(f"{name}: NOBUILD, not a runtime kill: {built.stderr}")
        ran=subprocess.run([str(binary.resolve())],capture_output=True,text=True,timeout=30)
        receipt["variants"][name]={"exit":ran.returncode,"stdout":ran.stdout,"stderr":ran.stderr,"intended_failure":name in expected and expected[name] in ran.stderr}
        if name.startswith("control") and ran.returncode:raise RuntimeError(ran.stdout+ran.stderr)
        if name in expected and (ran.returncode==0 or expected[name] not in ran.stderr):raise RuntimeError(f"{name}: wrong runtime failure: {ran.stderr}")
    (args.output/"result.json").write_text(json.dumps(receipt,indent=2)+"\n");print(json.dumps(receipt,indent=2))


if __name__ == "__main__": main()
