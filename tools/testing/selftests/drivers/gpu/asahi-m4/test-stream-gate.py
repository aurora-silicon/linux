#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the production M4 Rust stream gate against a mapped mock VM."""
from common import kernel_root, output_dir, rustc, frozen_bindings

import hashlib
import json
import pathlib
import subprocess
import tempfile

ROOT = kernel_root()
SOURCE = ROOT / 'drivers/gpu/drm/asahi/g16/gate.rs'
OUT = output_dir()
harness = r'''
#![allow(dead_code, unused_variables)]
extern crate core;
macro_rules! pr_warn { ($($x:tt)*) => {{}} }
#[macro_export]
macro_rules! mock_current { () => { crate::kernel::task::CurrentTask }; }
extern crate self as kernel;
pub use crate::mock_current as current;
pub mod task {
    use std::cell::Cell;
    std::thread_local! {
        static PENDING: Cell<bool> = const { Cell::new(false) };
        static SCHEDULED: Cell<usize> = const { Cell::new(0) };
    }
    pub struct CurrentTask;
    impl CurrentTask { pub fn signal_pending(&self) -> bool { PENDING.with(Cell::get) } }
    pub fn pending(value: bool) { PENDING.with(|p| p.set(value)); }
    pub fn might_sleep() { SCHEDULED.with(|p| p.set(p.get() + 1)); }
    pub fn scheduled() -> usize { SCHEDULED.with(Cell::get) }
}
pub mod prelude {
    pub type Error = i32;
    pub type Result<T = ()> = core::result::Result<T, Error>;
    pub const EINVAL: Error = -22;
    pub const EFAULT: Error = -14;
    pub const EACCES: Error = -13;
    pub const ERESTARTSYS: Error = -512;
}
mod mmu {
    use crate::kernel::prelude::*;
    pub const UAT_PGSZ: usize = 0x4000;
    pub const UAT_PGMSK: usize = UAT_PGSZ - 1;
    pub struct Region { pub base: u64, pub bytes: Vec<u8>, pub readable: bool }
    #[derive(Default)]
    pub struct Vm {
        pub regions: Vec<Region>,
        pub epoch: std::cell::Cell<Option<u64>>,
        pub lock_calls: std::cell::Cell<usize>,
        pub read_calls: std::cell::Cell<usize>,
        pub last_read_len: std::cell::Cell<usize>,
    }
    pub struct VmReadGuard<'a>(&'a Vm);
    impl VmReadGuard<'_> {
        pub fn epoch(&self) -> Option<u64> {
            self.0.epoch.get()
        }
        pub fn covers_range(&mut self, address: u64, size: u64, _read: bool, _write: bool) -> bool {
            address
                .checked_add(size)
                .is_some_and(|end| self.0.contains(address, end))
        }
        pub fn read_user(&mut self, va: u64, out: &mut [u8]) -> Result {
            self.0.read_user(va, out)
        }
    }
    impl Vm {
        pub fn region(&mut self, base: u64, bytes: Vec<u8>) {
            self.regions.push(Region { base, bytes, readable: true });
        }
        pub fn contains(&self, va: u64, end: u64) -> bool {
            self.regions.iter().any(|r| r.readable && va >= r.base
                && end >= va && end <= r.base + r.bytes.len() as u64)
        }
        pub fn read_lock(&self) -> Result<VmReadGuard<'_>> {
            self.lock_calls.set(self.lock_calls.get() + 1);
            Ok(VmReadGuard(self))
        }
        pub fn read_user(&self, va: u64, out: &mut [u8]) -> Result {
            self.read_calls.set(self.read_calls.get() + 1);
            self.last_read_len.set(out.len());
            let end = va.checked_add(out.len() as u64).ok_or(EFAULT)?;
            for r in &self.regions {
                if r.readable && va >= r.base && end <= r.base + r.bytes.len() as u64 {
                    let at = (va - r.base) as usize;
                    out.copy_from_slice(&r.bytes[at..at + out.len()]);
                    return Ok(());
                }
            }
            Err(EFAULT)
        }
    }
}
mod g16 {
    pub mod params { pub fn stream_gate() -> bool { true } }
    pub mod command {
        use crate::{mmu, kernel::prelude::*};
        use std::cell::Cell;
        pub const USC_WINDOW: u64 = 1 << 32;
        #[derive(Default)]
        pub struct SpanProof {
            epoch: Option<u64>,
            ranges: Vec<(u64, u64, u8)>,
        }
        impl SpanProof {
            pub fn new() -> Self { Self::default() }
            pub fn covers_aligned(&self, epoch: Option<u64>, first: u64, size: u64, access: u8) -> bool {
                self.epoch.is_some()
                    && self.epoch == epoch
                    && self.ranges.iter().any(|&(s, l, a)| {
                        (a & access) == access && first >= s && first + size <= s + l
                    })
            }
            pub fn record_one(&mut self, epoch: Option<u64>, first: u64, size: u64, access: u8) {
                if epoch.is_none() {
                    self.invalidate();
                    return;
                }
                if self.epoch != epoch {
                    self.epoch = epoch;
                    self.ranges.clear();
                }
                self.ranges.push((first, size, access));
            }
            pub fn invalidate(&mut self) {
                self.epoch = None;
                self.ranges.clear();
            }
        }
        std::thread_local! {
            static COVER_CALLS: Cell<usize> = const { Cell::new(0) };
        }
        pub fn reset_cover_calls() { COVER_CALLS.with(|c| c.set(0)); }
        pub fn cover_calls() -> usize { COVER_CALLS.with(Cell::get) }
        pub fn cover_locked(
            guard: &mut mmu::VmReadGuard<'_>,
            epoch: Option<u64>,
            mut proof: Option<&mut SpanProof>,
            start: u64,
            end: u64,
            read: bool,
            write: bool,
        ) -> Result {
            if end <= start || start < 0x4000 || end > (1u64 << 42) - 0x8000 {
                return Err(EINVAL);
            }
            let first = start & !(mmu::UAT_PGMSK as u64);
            let last = (end + mmu::UAT_PGMSK as u64) & !(mmu::UAT_PGMSK as u64);
            let size = last - first;
            let access = u8::from(read) | (u8::from(write) << 1);
            if start < 0x7100000000 && end > 0x7000000000
                || start < 0x1000010000 && end > 0x1000000000 {
                return Err(EACCES);
            }
            if proof.as_deref().is_some_and(|p| p.covers_aligned(epoch, first, size, access)) {
                return Ok(());
            }
            COVER_CALLS.with(|c| c.set(c.get() + 1));
            if guard.covers_range(start, end - start, read, write) {
                if let Some(p) = proof.as_deref_mut() {
                    p.record_one(epoch, first, size, access);
                }
                Ok(())
            } else {
                if let Some(p) = proof.as_deref_mut() {
                    p.invalidate();
                }
                Err(EFAULT)
            }
        }
    }
    #[path = "PRODUCTION_SOURCE"] pub mod gate;
}
const BASE: u64 = 0x200000000;
const USC: u64 = 0x2000000000;
fn bytes(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}
fn launch(mode: u32, program: u64, indirect: u64) -> Vec<u32> {
    let mut w = vec![0x00080000 | (mode << 27), ((program >> 38) as u32) << 22,
        (program >> 6) as u32, 0x40000000 | ((program >> 40) & 0xff) as u32];
    if mode == 0 { w.extend([1, 2, 3, 4, 5, 6]); }
    else {
        w.extend([(indirect >> 32) as u32, indirect as u32]);
        if mode == 1 { w.extend([4, 5, 6]); }
    }
    w
}
fn vm_for(words: &[u32]) -> mmu::Vm {
    let mut vm = mmu::Vm::default();
    let mut stream = bytes(words); stream.resize((stream.len() + 0x3fff).max(0x4000) & !0x3fff, 0);
    vm.region(BASE, stream); vm.region(USC, vec![0; 0x4000]);
    vm.region(0x400000000, vec![0; 0x4000]); vm
}
fn cdm(vm: &mmu::Vm, usc: u64, base: u64, end: u64) -> kernel::prelude::Result {
    let mut guard = None;
    let mut epoch = None;
    g16::gate::cdm_locked(vm, &mut guard, &mut epoch, usc, base, end, None)
}
fn run(words: &[u32]) -> bool {
    cdm(&vm_for(words), USC, BASE, BASE + words.len() as u64 * 4).is_ok()
}
#[test]
fn direct_modes_and_program_split() {
    for program in [USC, USC + 64, USC + 0x3fc0] {
        let mut words = launch(0, program, 0); words.push(0x40000000);
        assert!(run(&words));
    }
    // Cross word1/word2's 38-bit split with a separate USC window.
    for usc in [0x3f00000000u64, 0x4000000000, 0x8100000000, 0x30000000000] {
        let mut words = launch(0, usc + 0x80, 0); words.push(0x40000000);
        let mut vm = vm_for(&words); vm.region(usc, vec![0; 0x4000]);
        assert!(cdm(&vm, usc, BASE, BASE + words.len() as u64 * 4).is_ok());
    }
}
#[test]
fn indirect_modes() {
    for mode in [1, 2] {
        let mut words = launch(mode, USC + 0x80, 0x400000000);
        words.push(0x40000000); assert!(run(&words));
        words[5] |= 1; assert!(!run(&words));
    }
    let mut words = launch(2, USC, 0x400003ff0); words.push(0x40000000);
    assert!(!run(&words)); // Whole six-word grid must fit, not only its first12 bytes.
}
#[test]
fn barriers_padding_and_links() {
    for barrier in [0x60000160, 0x60001560, 0x600201fe, 0x600215fe,
        0x60200160, 0x7fe01560] {
        assert!(run(&[barrier, 0x40000000]));
    }
    assert!(run(&[0xa0000000, 0xa0000007, 0x40000000]));
    let target = 0x600000000u64;
    let words = [0x20000000 | (target >> 32) as u32, target as u32];
    let mut vm = vm_for(&words);
    let mut linked = bytes(&[0x60000160, 0x40000000]); linked.resize(0x4000, 0);
    vm.region(target, linked);
    assert!(cdm(&vm, USC, BASE, BASE + 8).is_ok());
    assert!(!run(&[0x30000006, 0])); // Call/return form is not qualified.
}
#[test]
fn truncation_and_unknown_records_fail_closed() {
    for mode in [0, 1, 2] {
        let words = launch(mode, USC, 0x400000000);
        for count in 1..words.len() {
            assert!(!run(&words[..count]), "mode{mode} count{count}");
        }
        assert!(!run(&words)); // No terminate before the declared root end.
    }
    for word in [0x60000502, 0x60024102, 0xa0000008, 0x80000000,
        0xc0000000, 0xe0000000, 0x18080000] {
        assert!(!run(&[word, 0x40000000]), "{word:08x}");
    }
    assert!(!run(&[0x20000000]));
    assert!(!run(&[]));
}
#[test]
fn programs_permissions_and_sizes() {
    let mut words = launch(0, USC - 64, 0); words.push(0x40000000);
    assert!(!run(&words));
    let mut words = launch(0, USC + (1 << 32), 0); words.push(0x40000000);
    assert!(!run(&words));
    let mut words = launch(0, USC, 0); words[7] = 0x800; words.push(0x40000000);
    assert!(!run(&words));
    let mut words = launch(0, USC, 0); words.push(0x40000000);
    let mut vm = vm_for(&words); vm.regions[1].readable = false;
    assert!(cdm(&vm, USC, BASE, BASE + 44).is_err());
    let target = 0x7000000000u64;
    let words = [0x20000000 | (target >> 32) as u32, target as u32];
    let mut vm = vm_for(&words); vm.region(target, vec![0; 0x4000]);
    assert!(cdm(&vm, USC, BASE, BASE + 8).is_err());
}
#[test]
fn cycles_are_rejected() {
    let a = BASE; let b = BASE + 0x100;
    let words = [0x20000000 | (b >> 32) as u32, b as u32];
    let mut vm = vm_for(&words);
    vm.regions[0].bytes[0x100..0x108].copy_from_slice(&bytes(&[0x20000000 | (a >> 32) as u32, a as u32]));
    assert!(cdm(&vm, USC, BASE, BASE + 8).is_err());
    assert!(!run(&[0x20000000 | (a >> 32) as u32, a as u32]));
    // A loop beyond the old record budget must also be detected.
    let mut words = vec![0x60000160; 20000];
    words.extend([0x20000000 | (a >> 32) as u32, a as u32]);
    assert!(!run(&words));
}
#[test]
fn long_streams_are_fully_checked() {
    let mut words = Vec::new();
    for _ in 0..20000 { words.extend(launch(0, USC, 0)); }
    words.push(0x40000000);
    let scheduled = kernel::task::scheduled();
    assert!(run(&words));
    assert!(kernel::task::scheduled() > scheduled + 1);
    words.pop();
    assert!(!run(&words)); // Full walk still requires a terminator.
    words.push(0xe0000000); words.push(0x40000000);
    assert!(!run(&words)); // A bad suffix after 20k launches is not admitted.
    for count in [8191, 8192, 8193, 32768] {
        let mut words = vec![0x60000160; count]; words.push(0x40000000);
        assert!(run(&words));
    }
}
#[test]
fn long_link_chains_and_cycles() {
    let first = 0x600000000u64;
    let root = [0x20000000 | (first >> 32) as u32, first as u32];
    let mut vm = vm_for(&root);
    for index in 0..128u64 {
        let address = first + index * 0x10000;
        let target = address + 0x10000;
        let words = if index == 127 { vec![0x40000000] }
                    else { vec![0x60000160, 0x20000000 | (target >> 32) as u32, target as u32] };
        let mut data = bytes(&words); data.resize(0x4000, 0);
        vm.region(address, data);
    }
    assert!(cdm(&vm, USC, BASE, BASE + 8).is_ok());
    let tail = vm.regions.last_mut().unwrap();
    tail.bytes[..8].copy_from_slice(&bytes(&root));
    assert!(cdm(&vm, USC, BASE, BASE + 8).is_err());
}
#[test]
fn cached_window_reuses_page_coverage_without_rechecking_every_record() {
    let mut words = Vec::new();
    for _ in 0..5 {
        words.extend(launch(0, USC, 0));
    }
    words.push(0x40000000);
    // 5 launches * 40B + 4B terminate = 204 bytes, all within one 256-byte window.
    g16::command::reset_cover_calls();
    assert!(run(&words));
    // Expect 1 cover() call for the 256B stream window + 1 for the cached USC program page.
    assert_eq!(g16::command::cover_calls(), 2);
}
#[test]
fn pending_signal_interrupts_walk() {
    let words = [0x60000160, 0x40000000];
    let vm = vm_for(&words);
    kernel::task::pending(true);
    let result = cdm(&vm, USC, BASE, BASE + 8);
    kernel::task::pending(false);
    assert_eq!(result, Err(kernel::prelude::ERESTARTSYS));
    assert!(cdm(&vm, USC, BASE, BASE + 8).is_ok());
}
fn vdm_check(vm: &mmu::Vm, vas: &[u64]) -> kernel::prelude::Result {
    let mut guard = vm.read_lock()?;
    g16::gate::vdm_batch_locked(&mut guard, vas)
}
#[test]
fn vdm_m4_root_barrier() {
    for word in [0x21000960, 0x21000968] {
        assert!(vdm_check(&vm_for(&[word]), &[BASE]).is_ok());
    }
    for word in [0, 0x21000964, 0x40000000] {
        assert!(vdm_check(&vm_for(&[word]), &[BASE]).is_err());
    }
}
#[test]
fn vdm_batch_shares_lock_caches_window_and_rejects_bad_root() {
    let mut vm = mmu::Vm::default();
    let mut page0 = vec![0u8; 0x4000];
    let mut page1 = vec![0u8; 0x4000];
    let root = 0x21000960u32.to_le_bytes();
    for off in [0usize, 0x20, 0x40, 0x80, 0x3ffc] {
        page0[off..off + 4].copy_from_slice(&root);
    }
    page1[0x10..0x14].copy_from_slice(&root);
    vm.region(BASE, page0);
    vm.region(BASE + 0x4000, page1);

    // Empty batch performs no reads.
    assert!(vdm_check(&vm, &[]).is_ok());
    assert_eq!(vm.read_calls.get(), 0);

    // Four adjacent VDM roots within 256 bytes share one lock and one read_user.
    vm.lock_calls.set(0);
    assert!(vdm_check(&vm, &[BASE, BASE + 0x20, BASE + 0x40, BASE + 0x80]).is_ok());
    assert_eq!(vm.lock_calls.get(), 1);
    assert_eq!(vm.read_calls.get(), 1);

    // A bad root inside the same cached 256-byte window is still rejected.
    vm.regions[0].bytes[0x80..0x84].copy_from_slice(&0xdeadbeefu32.to_le_bytes());
    assert_eq!(
        vdm_check(&vm, &[BASE, BASE + 0x20, BASE + 0x80]),
        Err(kernel::prelude::EINVAL)
    );
    vm.regions[0].bytes[0x80..0x84].copy_from_slice(&root);

    // Cross-page and end-of-page roots share one lock and never over-read past UAT_PGSZ.
    vm.lock_calls.set(0);
    vm.read_calls.set(0);
    assert!(vdm_check(&vm, &[BASE, BASE + 0x3ffc, BASE + 0x4010]).is_ok());
    assert_eq!(vm.lock_calls.get(), 1);
    assert_eq!(vm.read_calls.get(), 3);
}
#[test]
fn cdm_batches_vm_read_lock_and_releases_on_resched() {
    let target = 0x600000000u64;
    let mut root = Vec::new();
    root.extend(launch(0, USC, 0));
    root.extend(launch(1, USC + 0x80, 0x400000000));
    root.extend(launch(2, USC + 0x100, 0x400000040));
    root.extend([0x20000000 | (target >> 32) as u32, target as u32]);
    let mut vm = vm_for(&root);
    let mut linked_words = launch(0, USC + 0x4000, 0);
    linked_words.push(0x40000000);
    let mut linked = bytes(&linked_words);
    linked.resize(0x4000, 0);
    vm.region(target, linked);
    vm.region(USC + 0x4000, vec![0; 0x4000]);

    // Multi-launch mode 0/1/2 + linked segment acquires the VM read lock only once.
    assert!(cdm(&vm, USC, BASE, BASE + root.len() as u64 * 4).is_ok());
    assert_eq!(vm.lock_calls.get(), 1);

    // Streams longer than CDM_RESCHED_RECORDS (1024) drop and re-acquire the lock on each resched.
    let mut long = vec![0x60000160; 2500];
    long.push(0x40000000);
    let long_vm = vm_for(&long);
    let sched_before = kernel::task::scheduled();
    assert!(cdm(&long_vm, USC, BASE, BASE + long.len() as u64 * 4).is_ok());
    let sched_delta = kernel::task::scheduled() - sched_before;
    assert_eq!(sched_delta, 3);
    assert_eq!(long_vm.lock_calls.get(), sched_delta);
}
#[test]
fn single_vdm_and_short_cdm_root_minimize_poc_read_len() {
    let vm = vm_for(&[0x21000960]);
    assert!(vdm_check(&vm, &[BASE]).is_ok());
    assert_eq!(vm.lock_calls.get(), 1);
    assert_eq!(vm.read_calls.get(), 1);
    assert_eq!(vm.last_read_len.get(), 4);

    let mut words = launch(0, USC, 0);
    words.push(0x40000000);
    let cdm_vm = vm_for(&words);
    assert!(cdm(&cdm_vm, USC, BASE, BASE + words.len() as u64 * 4).is_ok());
    assert_eq!(cdm_vm.lock_calls.get(), 1);
    assert_eq!(cdm_vm.read_calls.get(), 1);
    assert_eq!(cdm_vm.last_read_len.get(), words.len() * 4);
}
#[test]
fn cdm_locked_shares_held_guard_and_hits_span_proof() {
    let mut words = launch(1, USC + 0x80, 0x400000000);
    words.push(0x40000000);
    let vm = vm_for(&words);
    vm.epoch.set(Some(7));
    let mut proof = g16::command::SpanProof::new();

    // Caller holds one VmReadGuard across two cdm_locked walks; first pass populates proof,
    // second pass hits proof with zero additional cover_locked walks and still 1 lock_calls.
    let mut guard = Some(vm.read_lock().unwrap());
    let mut epoch = guard.as_ref().and_then(|g| g.epoch());
    g16::command::reset_cover_calls();
    assert!(g16::gate::cdm_locked(
        &vm,
        &mut guard,
        &mut epoch,
        USC,
        BASE,
        BASE + words.len() as u64 * 4,
        Some(&mut proof),
    )
    .is_ok());
    assert_eq!(vm.lock_calls.get(), 1);
    assert_eq!(g16::command::cover_calls(), 3);

    assert!(g16::gate::cdm_locked(
        &vm,
        &mut guard,
        &mut epoch,
        USC,
        BASE,
        BASE + words.len() as u64 * 4,
        Some(&mut proof),
    )
    .is_ok());
    drop(guard);
    assert_eq!(vm.lock_calls.get(), 1);
    assert_eq!(g16::command::cover_calls(), 3);

    // Epoch bump invalidates the cached proof and forces cover_locked to re-verify spans.
    vm.epoch.set(Some(8));
    let mut guard = Some(vm.read_lock().unwrap());
    let mut epoch = guard.as_ref().and_then(|g| g.epoch());
    assert!(g16::gate::cdm_locked(
        &vm,
        &mut guard,
        &mut epoch,
        USC,
        BASE,
        BASE + words.len() as u64 * 4,
        Some(&mut proof),
    )
    .is_ok());
    assert_eq!(vm.lock_calls.get(), 2);
    assert_eq!(g16::command::cover_calls(), 6);
}
'''

with tempfile.TemporaryDirectory(prefix='m4-rust-gate-') as directory:
    directory = pathlib.Path(directory)
    test = directory / 'test.rs'
    test.write_text(harness.replace('PRODUCTION_SOURCE', str(SOURCE)))
    binary = directory / 'test'
    subprocess.run(rustc() + [ '--edition=2021', '--test', str(test), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--nocapture'], check=True, text=True, capture_output=True, timeout=60)
    print(result.stdout, end='')
    (OUT / 'stream-gate-host.log').write_text(result.stdout + result.stderr)
    (OUT / 'stream-gate-host.json').write_text(json.dumps({
        'source': str(SOURCE), 'sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        'result': 'pass', 'test_groups': 12,
        'mesa_sources': ['src/asahi/genxml/agx3_cmdstream.h', 'src/asahi/lib/agx3_cdm_decode_core.c',
                         'src/asahi/lib/agx_vdm_barrier_core.c'],
        'limitations': 'Host VM model validates parser, addresses, cycles and interruption; hardware and kernel cache behavior require target tests.'
    }, indent=2) + '\n')
