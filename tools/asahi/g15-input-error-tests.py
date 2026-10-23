#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Actual G15 job dependency completion and G17 status/policy bodies on host services.

DMA fence lookup, engine submission, queue-fence ordering and kernel mutexes are
models. No kernel scheduling, GPU access or firmware execution is exercised.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def extract(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PRELUDE = r'''
#![allow(dead_code)]
use std::sync::{Arc,Mutex as StdMutex,MutexGuard,atomic::{AtomicBool,AtomicI32,AtomicU32,Ordering}};
#[derive(Copy,Clone,Debug,PartialEq,Eq)] struct Error(i32);
impl Error { fn from_errno(n:i32)->Self {assert!(n<0);Self(n)} fn to_errno(self)->i32{self.0} }
type Result<T=()> = std::result::Result<T,Error>;
const EIO:Error=Error(-5);const ENODEV:Error=Error(-19);const ETIMEDOUT:Error=Error(-110);
const ECANCELED:Error=Error(-125);const EINVAL:Error=Error(-22);const ENOMEM:Error=Error(-12);
macro_rules! pr_warn { ($($arg:tt)*)=>{{ let _=format_args!($($arg)*); }} }
struct Mutex<T>(StdMutex<T>);impl<T> Mutex<T>{fn lock(&self)->MutexGuard<'_,T>{self.0.lock().unwrap()}}
struct Mirror{word:*mut i32}
struct VmStatus{error:AtomicI32,reported_error:AtomicI32,mirror:Mutex<Option<Mirror>>}
impl VmStatus {
 fn new(word:*mut i32)->Self{Self{error:AtomicI32::new(0),reported_error:AtomicI32::new(0),mirror:Mutex(StdMutex::new(Some(Mirror{word})))}}
__STATUS__
}
mod g17 {use super::*;__POLICY__}
mod bindings {pub unsafe fn dma_fence_get_status(p:*const i32)->i32{unsafe{*p}}}
struct Fence(i32);impl Fence{fn raw(&self)->*const i32{&self.0}}
struct Vm{status:VmStatus}
struct Progress{signaled:AtomicBool}
struct Guard{quarantines:AtomicU32}impl Guard{fn quarantine(&self){self.quarantines.fetch_add(1,Ordering::SeqCst);}}
struct JobFence{vm:Arc<Vm>,signals:AtomicU32,result:AtomicI32,saw_sticky:AtomicI32,saw_report:AtomicI32,saw_mirror:AtomicI32}
impl JobFence{
 fn signal(&self,result:Result){
  self.saw_sticky.store(self.vm.status.get(),Ordering::SeqCst);
  self.saw_report.store(self.vm.status.reported_error(),Ordering::SeqCst);
  let mirror=self.vm.status.mirror.lock();
  self.saw_mirror.store(unsafe{AtomicI32::from_ptr(mirror.as_ref().unwrap().word)}.load(Ordering::SeqCst),Ordering::SeqCst);
  self.result.store(result.err().map_or(0,|e|e.to_errno()),Ordering::SeqCst);
  self.signals.fetch_add(1,Ordering::SeqCst);
 }
}
struct Shared{crashed:AtomicBool,submits:AtomicU32,submit_result:Result}
impl Shared{fn submit(&self,_:Arc<Job>)->Result{self.submits.fetch_add(1,Ordering::SeqCst);self.submit_result}}
struct Job{started:AtomicBool,vm:Arc<Vm>,progress:Progress,_guard:Guard,fence:JobFence}
impl Job {__JOB__}
fn make_job(own:Option<Error>)->(Arc<Job>,Box<AtomicI32>){
 let mut mirror=Box::new(AtomicI32::new(0));let vm=Arc::new(Vm{status:VmStatus::new((&mut *mirror as *mut AtomicI32).cast())});
 if let Some(error)=own{vm.status.record(error);}
 let job=Arc::new(Job{started:AtomicBool::new(false),vm:vm.clone(),progress:Progress{signaled:AtomicBool::new(false)},_guard:Guard{quarantines:AtomicU32::new(0)},fence:JobFence{vm,signals:AtomicU32::new(0),result:AtomicI32::new(0),saw_sticky:AtomicI32::new(0),saw_report:AtomicI32::new(0),saw_mirror:AtomicI32::new(0)}});
 (job,mirror)
}
fn shared(crashed:bool,result:Result)->Arc<Shared>{Arc::new(Shared{crashed:AtomicBool::new(crashed),submits:AtomicU32::new(0),submit_result:result})}
fn n(n:&AtomicI32)->i32{n.load(Ordering::SeqCst)}
fn main(){
 let (unrelated,other_mirror)=make_job(None);
 for input in [ECANCELED,EIO,ENODEV,ETIMEDOUT,EINVAL,ENOMEM]{
  let (job,mirror)=make_job(None);let device=shared(false,Ok(()));
  job.start(&device,&[Fence(1),Fence(input.to_errno()),Fence(-1)]);
  let expected=g17::inherited_error(input).to_errno();
  assert_eq!(device.submits.load(Ordering::SeqCst),0,"errored input must never submit");
  assert_eq!(job.fence.signals.load(Ordering::SeqCst),1,"dependency completion must signal once");
  assert_eq!(n(&job.fence.result),expected,"inherited error mapping");
  assert_eq!(n(&job.fence.saw_report),expected,"report must precede fence signal");
  assert_eq!(n(&job.fence.saw_mirror),expected,"mirror must precede fence signal");
  assert_eq!(n(&job.fence.saw_sticky),0,"healthy dependency consumer admission stays open");
  assert_eq!(job.vm.status.get(),0,"healthy dependency consumer admission stays open");
  assert_eq!(mirror.load(Ordering::SeqCst),expected);
  assert_eq!(job._guard.quarantines.load(Ordering::SeqCst),0,"unpublished job must not quarantine");
  job.finish_dependency(EIO);job.complete(Err(EIO));
  assert_eq!(job.fence.signals.load(Ordering::SeqCst),1,"dependency completion must signal once");
  assert_eq!(job.vm.status.get(),0,"duplicate completion must not poison VM");
  // A later job on the same VM is admitted despite the reported client error.
  let later=Arc::new(Job{started:AtomicBool::new(false),vm:job.vm.clone(),progress:Progress{signaled:AtomicBool::new(false)},_guard:Guard{quarantines:AtomicU32::new(0)},fence:JobFence{vm:job.vm.clone(),signals:AtomicU32::new(0),result:AtomicI32::new(0),saw_sticky:AtomicI32::new(0),saw_report:AtomicI32::new(0),saw_mirror:AtomicI32::new(0)}});
  later.start(&device,&[Fence(1)]);assert_eq!(device.submits.load(Ordering::SeqCst),1,"successful dependencies after report must submit");
 }
 for own in [EIO,EINVAL]{for input in [ECANCELED,EIO,ENODEV,ETIMEDOUT,EINVAL]{
  let(job,_mirror)=make_job(Some(own));let device=shared(false,Ok(()));job.start(&device,&[Fence(input.to_errno())]);
  assert_eq!(device.submits.load(Ordering::SeqCst),0);assert_eq!(n(&job.fence.result),input.to_errno(),"own-failed VM must retain original dependency errno");
  assert_eq!(job.vm.status.get(),own.to_errno(),"own-failed VM sticky error must remain");
  assert_eq!(n(&job.fence.saw_report),own.to_errno());assert_eq!(n(&job.fence.saw_mirror),own.to_errno());
  assert_eq!(job._guard.quarantines.load(Ordering::SeqCst),0);
 }}
 for inputs in [vec![],vec![Fence(1)],vec![Fence(1),Fence(1)]]{
  let(job,_mirror)=make_job(None);let device=shared(false,Ok(()));job.start(&device,&inputs);
  assert_eq!(device.submits.load(Ordering::SeqCst),1,"successful dependencies must submit");assert_eq!(job.fence.signals.load(Ordering::SeqCst),0);assert_eq!(job.vm.status.get(),0);
 }
 // Preserve device-crash and submit-refusal sticky semantics.
 for (crashed,result) in [(true,Ok(())),(false,Err(ENODEV)),(false,Err(EINVAL))]{
  let(job,_mirror)=make_job(None);let device=shared(crashed,result);job.start(&device,&[Fence(1)]);
  let expected=if crashed||result==Err(ENODEV){EIO}else{EINVAL};assert_eq!(job.vm.status.get(),expected.to_errno());assert_eq!(n(&job.fence.saw_mirror),expected.to_errno());
 }
 // An engine-owned fault still quarantines before completion.
 let(job,_mirror)=make_job(None);job.complete(Err(EIO));assert_eq!(job._guard.quarantines.load(Ordering::SeqCst),1);assert_eq!(job.vm.status.get(),EIO.to_errno());
 assert_eq!(unrelated.vm.status.get(),0,"unrelated VM untouched");assert_eq!(unrelated.vm.status.reported_error(),0);assert_eq!(other_mirror.load(Ordering::SeqCst),0);assert_eq!(unrelated.fence.signals.load(Ordering::SeqCst),0);
 println!("PASS: actual Job::start/completion and VmStatus/policy: healthy/failed VMs, mirror ordering, one-shot completion, successful input, device faults and isolation");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--ref", help="Read job.rs at a public git revision for a removed-fix control")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    job_path = "drivers/gpu/drm/asahi/g15/job.rs"
    job = (subprocess.check_output(["git", "show", args.ref + ":" + job_path], cwd=ROOT, text=True)
           if args.ref else (ROOT / job_path).read_text())
    current = (ROOT / job_path).read_text()
    status = (ROOT / "drivers/gpu/drm/asahi/g17/status.rs").read_text()
    policy = (ROOT / "drivers/gpu/drm/asahi/g17/dependency.rs").read_text()
    methods = [extract(job, "    pub(crate) fn complete("), extract(job, "    fn finish("),
               extract(job, "    fn start(self: &Arc<Self>")]
    # The old control needs the current helper for the duplicate-completion test;
    # it still runs its actual unmodified start, which reproduces the lost input error.
    methods.append(extract(job if "fn finish_dependency(" in job else current, "    fn finish_dependency("))
    bodies = "\n".join(methods)
    code = PRELUDE.replace("__JOB__", bodies).replace("__STATUS__", "\n".join(
        extract(status, marker) for marker in ["    pub(crate) fn get(", "    pub(crate) fn record(",
        "    pub(crate) fn reported_error(", "    pub(crate) fn report_failure("])).replace(
        "__POLICY__", extract(policy, "pub(crate) fn inherited_error("))
    variants = {"control": code}
    expected = {}
    if args.mutations:
        changes = {
            "healthy-errors-submit": ("if let Some(status) = failed {", "if let Some(status) = failed.filter(|_| self.vm.status.get() != 0) {", "errored input must never submit"),
            "foreign-device-error-unmapped": ("crate::g17::inherited_error(error)", "error", "inherited error mapping"),
            "dependency-records-sticky": ("self.finish_dependency(crate::g17::inherited_error(error));", "self.finish(Err(crate::g17::inherited_error(error)), false);", "healthy dependency consumer admission stays open"),
            "report-after-signal": ("self.vm.status.report_failure(error);\n        self.fence.signal(Err(error));", "self.fence.signal(Err(error));\n        self.vm.status.report_failure(error);", "report must precede fence signal"),
            "dependency-quarantines": ("    fn finish_dependency(&self, error: Error) {", "    fn finish_dependency(&self, error: Error) {\n        self._guard.quarantine();", "unpublished job must not quarantine"),
            "own-error-remapped": ("self.finish(Err(error), false);\n            } else", "self.finish(Err(crate::g17::inherited_error(error)), false);\n            } else", "own-failed VM must retain original dependency errno"),
            "success-refused": ("if let Some(status) = failed {", "if let Some(status) = failed.or(Some(-125)) {", "successful dependencies after report must submit"),
            "dependency-completes-twice": ("    fn finish_dependency(&self, error: Error) {\n        if self.progress.signaled.swap(true, Ordering::AcqRel) {\n            return;\n        }", "    fn finish_dependency(&self, error: Error) {", "dependency completion must signal once"),
            "mirror-not-published": ("unsafe { AtomicI32::from_ptr(mirror.word) }.store(error, Ordering::SeqCst);", "let _ = mirror;", "mirror must precede fence signal"),
        }
        for name, (old, new, message) in changes.items():
            if code.count(old) != 1:
                raise RuntimeError(f"{name}: unique mutation anchor count {code.count(old)}")
            variants[name] = code.replace(old, new)
            expected[name] = message
    variants["control-after"] = code
    receipt = {"scope": __doc__, "ref": args.ref, "sources": {name: hashlib.sha256(text.encode()).hexdigest()
        for name, text in [(job_path, job), ("g17/status.rs", status), ("g17/dependency.rs", policy)]}, "variants": {}}
    for name, source in variants.items():
        path = args.output / (name + ".rs"); path.write_text(source)
        binary = args.output / name
        built = subprocess.run([os.environ.get("RUSTC", "rustc"), "--edition=2021", str(path), "-o", str(binary)], capture_output=True, text=True)
        (args.output / (name + ".build.log")).write_text(built.stdout + built.stderr)
        if built.returncode:
            raise RuntimeError(f"{name}: NOBUILD, not a runtime kill: {built.stderr}")
        ran = subprocess.run([str(binary.resolve())], capture_output=True, text=True, timeout=30)
        receipt["variants"][name] = {"exit": ran.returncode, "stdout": ran.stdout, "stderr": ran.stderr,
            "intended_failure": name in expected and expected[name] in ran.stderr}
        if name.startswith("control") and not args.ref and ran.returncode:
            raise RuntimeError(ran.stdout + ran.stderr)
        if name in expected and (ran.returncode == 0 or expected[name] not in ran.stderr):
            raise RuntimeError(f"{name}: did not die at intended runtime assertion: {ran.stderr}")
    (args.output / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if args.ref:
        for name in ["control", "control-after"]:
            result = receipt["variants"][name]
            if result["exit"] == 0 or "errored input must never submit" not in result["stderr"]:
                raise RuntimeError("removed-fix control did not reproduce the intended lost-input failure")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
