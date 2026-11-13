#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production immediate GEM-close method with instrumented BO lifetimes.

Reported by Anees Iqbal (steelbrain): omacom/linux-aurora#458.
The external Vulkan/udmabuf reproducer remains the hardware test; this host
fixture checks the Rust caller's reservation/put/drain ordering and errors.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def method(source):
    start = source.index('    fn drop_mappings_now(')
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


FIXTURE = r'''
#![allow(dead_code)]
use std::{cell::RefCell, rc::Rc};
type Result<T = ()> = std::result::Result<T, i32>;
const ENOMEM: i32 = 12;
const EIO: i32 = 5;
macro_rules! mod_dev_dbg { ($($arg:tt)*) => {} }
mod gem { pub struct Object; }
#[derive(Default)] struct Prot;
struct VmOpResources { preallocated_gpuvas: [Option<()>; 3] }
struct StepContext;
impl StepContext {
    fn new(_: Option<()>, _: Prot, _: &mut VmOpResources) -> Result<Self> { Ok(Self) }
}
#[derive(Default)]
struct State {
    locked: bool,
    refs: usize,
    mapped: usize,
    queued: bool,
    released: bool,
    drains: usize,
    untracked: bool,
    present: bool,
    lock_error: Option<i32>,
    unmap_error: Option<i32>,
    events: Vec<&'static str>,
}
type Shared = Rc<RefCell<State>>;
struct Bo(Shared);
impl Drop for Bo {
    fn drop(&mut self) {
        let mut s = self.0.borrow_mut();
        assert!(!s.locked, "BO put while VM reservation held");
        s.events.push("put");
        s.refs -= 1;
        if s.refs == 0 { s.queued = true; }
    }
}
struct Unique(Shared);
impl Unique {
    fn find_bo(&mut self, _: &gem::Object) -> Option<Bo> {
        let mut s = self.0.borrow_mut();
        assert!(s.locked);
        if !s.present { return None; }
        s.refs += 1;
        Some(Bo(self.0.clone()))
    }
    fn bo_unmap(&mut self, _: &mut StepContext, _: &Bo) -> Result {
        let mut s = self.0.borrow_mut();
        assert!(s.locked);
        // EIO leaves one mapping; ENOMEM has removed all mappings before failing.
        let left = usize::from(s.unmap_error == Some(EIO));
        s.refs -= s.mapped - left;
        s.mapped = left;
        s.unmap_error.map_or(Ok(()), Err)
    }
}
struct Exec { unique_gpuvm: Unique }
impl Drop for Exec {
    fn drop(&mut self) {
        let mut s = self.unique_gpuvm.0.borrow_mut();
        assert!(s.locked);
        s.locked = false;
        s.events.push("unlock");
    }
}
struct GpuVm(Shared);
impl GpuVm {
    unsafe fn exec_lock_vm(&self, _: Option<()>, _: bool) -> Result<Exec> {
        let mut s = self.0.borrow_mut();
        if let Some(error) = s.lock_error { return Err(error); }
        assert!(!s.locked);
        s.locked = true;
        Ok(Exec { unique_gpuvm: Unique(self.0.clone()) })
    }
}
struct Vm { gpuvm: GpuVm }
impl Vm {
    fn enter_retirement_residency(&self) {}
    fn untrack_context_object(&self, _: &gem::Object) {
        self.gpuvm.0.borrow_mut().untracked = true;
    }
    fn untrack_shared_object(&self, _: &gem::Object) {}
    fn bo_deferred_cleanup(&self) {
        let mut s = self.gpuvm.0.borrow_mut();
        assert!(!s.locked, "drain under VM reservation");
        s.drains += 1;
        s.events.push("drain");
        if s.queued {
            assert_eq!(s.refs, 0);
            assert_eq!(s.mapped, 0, "free still-mapped backing");
            s.queued = false;
            s.released = true;
        }
    }
    fn drop_mappings_low_memory(&self, _: &gem::Object) -> Result {
        let mut s = self.gpuvm.0.borrow_mut();
        assert!(!s.locked);
        assert_eq!(s.refs, s.mapped, "temporary BO not released before fallback");
        assert!(!s.queued, "last indexed BO put not drained before fallback");
        s.events.push("fallback");
        s.refs -= s.mapped;
        s.mapped = 0;
        s.queued = s.present && !s.released;
        drop(s);
        self.bo_deferred_cleanup();
        Ok(())
    }
    __METHOD__
}
fn main() {
    for (present, lock_error, unmap_error) in [
        (true, None, None), (true, None, Some(EIO)),
        (true, None, Some(ENOMEM)), (false, None, None),
        (true, Some(ENOMEM), None), (true, Some(EIO), None),
    ] {
        let state = Rc::new(RefCell::new(State {
            present, lock_error, unmap_error, refs: usize::from(present) * 2,
            mapped: usize::from(present) * 2, ..State::default()
        }));
        let vm = Vm { gpuvm: GpuVm(state.clone()) };
        let result = vm.drop_mappings_now(&gem::Object);
        let s = state.borrow();
        assert!(!s.locked);
        if lock_error == Some(EIO) || unmap_error == Some(EIO) {
            assert_eq!(result, Err(EIO));
            assert!(!s.released, "failed unmap must retain live mapping");
        } else { assert_eq!(result, Ok(())); }
        if present && lock_error.is_none() {
            assert!(s.drains >= 1, "GEM close did not drain its final put");
            assert_eq!(&s.events[..3], &["unlock", "put", "drain"]);
            assert!(!s.queued);
            assert_eq!(s.released, unmap_error != Some(EIO));
            assert_eq!(s.untracked, unmap_error.is_none());
        }
        if lock_error == Some(ENOMEM) { assert!(s.released); }
        if !present { assert!(!s.released); }
    }
    println!("GEM close: success, partial error, ENOMEM and absent-BO paths passed");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--mutate', choices=['omit-drain', 'drain-before-put', 'drain-under-lock'])
    a = p.parse_args()
    source = method((ROOT / 'drivers/gpu/drm/asahi/mmu.rs').read_text())
    drain = '            self.bo_deferred_cleanup();\n'
    assert source.count(drain) == 1
    if a.mutate:
        source = source.replace(drain, '')
        if a.mutate != 'omit-drain':
            before = ('            core::mem::drop(bo);' if a.mutate == 'drain-before-put'
                      else '            core::mem::drop(vm_exec);')
            assert source.count(before) == 1
            source = source.replace(before, drain + before)
    a.out.mkdir(parents=True, exist_ok=True)
    code = a.out / 'gem-close.rs'
    binary = a.out / 'gem-close'
    code.write_text(FIXTURE.replace('__METHOD__', source))
    subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', '-Dwarnings',
                    str(code), '-o', str(binary)], check=True)
    run = subprocess.run([str(binary.resolve())])
    return run.returncode if run.returncode >= 0 else 128 - run.returncode


if __name__ == '__main__':
    sys.exit(main())
