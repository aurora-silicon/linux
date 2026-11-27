#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the G16 VM registry's reservation with host mocks and check how live VMs are shared.

The production Registry::reserve and Registry::release (g16/vm.rs) and the process share rule
(queue_share.rs) are copied from the tree and built with rustc against mocks of the kernel
mutex, vector, scope guard and current process. A process may open the device many times, so
the live VMs must be shared per process: one process with many files must leave room for the
others, and giving VMs back must make room again.

--mutations also runs the checks against each mutant below and expects every one to fail:
the registry as it was before the process share, the share taken but never given back, and a
process alone allowed to take every VM slot (the reserve it must leave for later clients).
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ASAHI = ROOT / 'drivers/gpu/drm/asahi'


def item(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def before_share():
    """The reservation as it was before VMs were shared per process."""
    return r'''fn reserve(&self, file_id: u64) -> Result<Reservation<'_>> {
        self.prune();
        let limit = if module_parameters::robust_isolation.value() != 0 { 1 } else { hw::sizes::MAX_VMS };
        self.live
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |live| {
                (live < limit).then_some(live + 1)
            })
            .map_err(|_| ENOSPC)?;
        let rollback = kernel::types::ScopeGuard::new(|| {
            self.live.fetch_sub(1, Ordering::AcqRel);
        });
        {
            let mut files = self.files.lock();
            if let Some((_, count)) = files.iter_mut().find(|(id, _)| *id == file_id) {
                if *count >= MAX_VMS_PER_FILE {
                    return Err(ENOSPC);
                }
                *count += 1;
            } else {
                files.push((file_id, 1), GFP_NOWAIT)?;
            }
        }
        rollback.dismiss();
        Ok(Reservation(Some(self), file_id, 0))
    }'''


def fixture(mutation):
    vm = (ASAHI / 'g16/vm.rs').read_text()
    share = (ASAHI / 'queue_share.rs').read_text()
    reserve = item(vm, 'fn reserve(&self, file_id: u64)')
    release = item(vm, 'fn release(&self, file_id: u64')
    if mutation == 'before-share':
        reserve = before_share()
    elif mutation == 'lone-takes-all':
        pass
    elif mutation == 'never-give-back':
        old = 'self.processes.lock().give_back(process)'
        if old not in release:
            raise RuntimeError('mutation site changed: never-give-back')
        release = release.replace(old, 'None::<Process>')
    per_file = next(line for line in vm.splitlines() if line.startswith('const MAX_VMS_PER_FILE'))
    max_vms = next(line.strip() for line in (ASAHI / 'g16/hw.rs').read_text().splitlines()
                   if 'const MAX_VMS:' in line).replace('pub(crate) ', '')
    policy = '\n'.join(item(share, signature) for signature in (
        'fn reserve(max: u32)', 'fn may_take(', 'fn may_take_held(', 'fn take_with(',
        'fn give_back(processes'))
    if mutation == 'lone-takes-all':
        old = 'held < max / 2 || max.saturating_sub'
        if old not in policy:
            raise RuntimeError('mutation site changed: lone-takes-all')
        policy = policy.replace(old, 'held < max / 2 || held == total || max.saturating_sub', 1)
    shares = item(share, 'impl Shares {')
    return r'''
#![allow(dead_code, unused_imports, non_upper_case_globals)]
use std::cell::{Cell, RefCell};
use std::sync::atomic::{AtomicU32, Ordering};
type Error = i32;
type Result<T = ()> = std::result::Result<T, Error>;
const ENOSPC: Error = -28;
const EBUSY: Error = -16;
const ENOMEM: Error = -12;
#[derive(Clone, Copy)] struct Flags;
const GFP_NOWAIT: Flags = Flags;
const GFP_KERNEL: Flags = Flags;
mod kernel {
    pub mod alloc { pub type Flags = super::super::Flags; }
    pub mod types {
        pub struct ScopeGuard<F: FnOnce()>(Option<F>);
        impl<F: FnOnce()> ScopeGuard<F> {
            pub fn new(f: F) -> Self { Self(Some(f)) }
            pub fn dismiss(mut self) { self.0 = None; }
        }
        impl<F: FnOnce()> Drop for ScopeGuard<F> {
            fn drop(&mut self) { if let Some(f) = self.0.take() { f() } }
        }
    }
}
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    const fn new() -> Self { Self(Vec::new()) }
    fn push(&mut self, v: T, _: Flags) -> Result { self.0.push(v); Ok(()) }
    fn remove(&mut self, i: usize) -> Result<T> { Ok(self.0.remove(i)) }
    fn swap_remove(&mut self, i: usize) -> T { self.0.swap_remove(i) }
}
impl<T> std::ops::Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> std::ops::DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }
struct Mutex<T>(RefCell<T>);
impl<T> Mutex<T> { fn lock(&self) -> std::cell::RefMut<'_, T> { self.0.borrow_mut() } }
struct ARef<T>(T);
struct Mm;
type Pid = i32;
mod module_parameters {
    pub struct P; impl P { pub fn value(&self) -> u32 { 0 } }
    pub static robust_isolation: P = P;
}
mod hw { pub mod sizes { pub ''' + max_vms + r''' } }
thread_local! { static CURRENT: Cell<usize> = Cell::new(1); }
fn current_name() -> (Pid, [u8; 16]) { (CURRENT.with(|c| c.get()) as Pid, [0; 16]) }
fn current_key() -> (usize, Option<ARef<Mm>>) { (CURRENT.with(|c| c.get()), None) }
struct Process { key: usize, _mm: Option<ARef<Mm>>, held: u32, tgid: Pid, comm: [u8; 16] }
pub(crate) struct Shares(KVec<Process>);
''' + policy + '\n' + shares + '\n' + per_file + r'''
struct Registry { live: AtomicU32, files: Mutex<KVec<(u64, u32)>>, processes: Mutex<Shares> }
struct Reservation<'a>(Option<&'a Registry>, u64, usize);
impl Drop for Reservation<'_> {
    fn drop(&mut self) { if let Some(registry) = self.0 { registry.release(self.1, self.2); } }
}
impl Registry {
    fn prune(&self) {}
''' + reserve + '\n' + release + r'''
}

fn as_process(key: usize) { CURRENT.with(|c| c.set(key)); }

fn main() {
    let max = hw::sizes::MAX_VMS;
    let registry = Registry { live: AtomicU32::new(0), files: Mutex(RefCell::new(KVec::new())),
        processes: Mutex(RefCell::new(Shares::new())) };
    // A compositor (process 1) holds one VM.
    as_process(1);
    let compositor = registry.reserve(100).expect("compositor VM");
    // Process 2 opens the device again and again, creating every VM each file allows.
    as_process(2);
    let mut taken = Vec::new();
    for file in 200..(200 + max as u64) {
        let before = taken.len();
        for _ in 0..MAX_VMS_PER_FILE {
            match registry.reserve(file) {
                Ok(r) => taken.push(r),
                Err(e) => { assert_eq!(e, ENOSPC); break; }
            }
        }
        if taken.len() == before { break; }
    }
    let held = taken.len() as u32;
    println!("process 2 holds {} of {} VMs", held, max);
    // A process may hold half the device's VMs whatever the others hold.
    assert!(held >= max / 2, "a process may hold half the device's VMs");
    // But it leaves room for the others: a newly started process and the compositor get VMs.
    as_process(3);
    let late = registry.reserve(300).expect("FAIL line 1: a newly started process gets no VM");
    as_process(1);
    let second = registry.reserve(100).expect("FAIL line 2: the compositor gets no second VM");
    assert!(registry.live.load(Ordering::Acquire) <= max);
    drop(late);
    drop(second);
    // A VM given back can be created again by the same process.
    as_process(2);
    drop(taken.pop());
    taken.push(registry.reserve(9999).expect("FAIL line 3: a given back VM cannot be created again"));
    drop(taken);
    drop(compositor);
    // Everything given back: a process alone still leaves the reserve, since a VM keeps its
    // slot until it is closed, and a process started after it gets a VM.
    as_process(4);
    let mut all = Vec::new();
    for file in 0..(max as u64 + 8) {
        for _ in 0..MAX_VMS_PER_FILE {
            if let Ok(r) = registry.reserve(1000 + file) { all.push(r) }
        }
    }
    assert_eq!(all.len() as u32, max - reserve(max),
               "FAIL line 4: a process alone takes the reserve (or less than the rest)");
    as_process(5);
    let after = registry.reserve(5000).expect("FAIL line 6: a process started later gets no VM");
    drop(after);
    drop(all);
    assert_eq!(registry.live.load(Ordering::Acquire), 0);
    assert!(registry.processes.lock().0.is_empty(), "FAIL line 5: a process share outlives its VMs");
    println!("ok");
}
'''


def run(mutation, work):
    src = Path(work) / f'{mutation or "control"}.rs'
    exe = Path(work) / f'{mutation or "control"}'
    src.write_text(fixture(mutation))
    build = subprocess.run(['rustc', '--edition=2021', '-O', '-C', 'overflow-checks=on',
                            '-o', str(exe), str(src)], capture_output=True, text=True)
    if build.returncode:
        raise RuntimeError(f'{mutation or "control"} does not compile:\n{build.stderr}')
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='g16-vm-share-') as work:
        code, output = run(None, work)
        print(output.strip())
        if code:
            print('FAIL control')
            return 1
        print('PASS control')
        if not args.mutations:
            return 0
        killed = 0
        names = ('before-share', 'never-give-back', 'lone-takes-all')
        for name in names:
            code, output = run(name, work)
            if code and 'panicked at' in output:
                killed += 1
                print(f'KILLED {name}: ' + next(line for line in output.splitlines() if 'panicked' in line or 'FAIL' in line))
            else:
                print(f'SURVIVED {name}:\n{output}')
        print(f'{killed}/{len(names)} mutants killed')
        return 0 if killed == len(names) else 1


if __name__ == '__main__':
    raise SystemExit(main())
