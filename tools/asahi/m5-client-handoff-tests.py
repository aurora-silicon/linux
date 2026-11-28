#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the production M5 render-job handoff with mock mappings.

M5GraphicsJob::rebind_vm, M5GraphicsMemory::rebind and both pools'
alias_into must clear the data storage the previous VM used (USC blocks,
parameter-buffer blocks, scene scratch, discard page, compute storage) before
another VM maps it, keep the allocator and parameter-manager state the
firmware refers to (USC directory and free list, page list, metrics), clear
nothing for a same-VM resubmission and nothing before full retirement.

--mutations also runs the unfixed source and each clear removed; every one
must be killed.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

LINUX = Path(__file__).resolve().parents[2]
RESOURCES = 'drivers/gpu/drm/asahi/m5/resources/'
FILES = {'usc': 'graphics_usc.rs', 'pb': 'parameter_buffer.rs',
         'memory': 'graphics_memory.rs', 'job': 'graphics_job.rs'}
BASE = 'c5e25e0925351'


def item(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace('pub(super) ', '')


def current():
    return {key: (LINUX / RESOURCES / name).read_text() for key, name in FILES.items()}


def original():
    return {key: subprocess.run(['git', '-C', str(LINUX), 'show', f'{BASE}:{RESOURCES}{name}'],
                                check=True, capture_output=True, text=True).stdout
            for key, name in FILES.items()}


PRELUDE = r'''
#![allow(warnings)]
use std::cell::RefCell;
use std::sync::atomic::{fence, Ordering};
type Error = i32;
type Result<T = ()> = core::result::Result<T, Error>;
const EINVAL: i32 = -22; const EOVERFLOW: i32 = -75; const ERANGE: i32 = -34;
const EFAULT: i32 = -14; const EBUSY: i32 = -16; const ENOTSUPP: i32 = -524;
const GFP_KERNEL: () = ();
macro_rules! dev_dbg { ($($t:tt)*) => {} }
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn new() -> Self { Self(Vec::new()) }
    fn push(&mut self, value: T, _: ()) -> Result { self.0.push(value); Ok(()) }
}
impl<T> core::ops::Deref for KVec<T> { type Target = Vec<T>; fn deref(&self) -> &Vec<T> { &self.0 } }
impl<T> core::ops::DerefMut for KVec<T> { fn deref_mut(&mut self) -> &mut Vec<T> { &mut self.0 } }
impl<T> IntoIterator for KVec<T> {
    type Item = T; type IntoIter = std::vec::IntoIter<T>;
    fn into_iter(self) -> Self::IntoIter { self.0.into_iter() }
}
mod mmu {
    pub type Prot = u32;
    pub const PROT_GPU_SHARED_RW: Prot = 3;
    pub const UAT_PGSZ: usize = 0x4000;
    pub struct KernelMapping(pub &'static str, pub u64);
    pub struct VmJobGuard(pub u64);
    #[derive(Clone)]
    pub struct Vm(pub u64);
    impl Vm {
        pub fn id(&self) -> u64 { self.0 }
        pub fn kernel_range(&self) -> core::ops::Range<u64> { 0x10_0000_0000..0x20_0000_0000 }
        pub fn retain_job(&self) -> super::Result<VmJobGuard> { Ok(VmJobGuard(self.0)) }
    }
    pub struct M5ExecutionContext { pub vm: Vm }
    impl M5ExecutionContext {
        pub fn rebind(&mut self, vm: &Vm) -> super::Result { self.vm = vm.clone(); Ok(()) }
        pub fn vm(&self) -> &Vm { &self.vm }
        pub fn context_id(&self) -> u8 { 3 }
        pub fn roots(&self) -> [u64; 2] { [0, 0] }
    }
}
mod m5 { pub mod vm_layout {
    pub fn compact_range(r: &core::ops::Range<u64>) -> Option<core::ops::Range<u64>> { Some(r.clone()) }
    pub fn auxiliary_range(r: &core::ops::Range<u64>) -> Option<core::ops::Range<u64>> { Some(r.clone()) }
} }
use m5::vm_layout;
struct AsahiDrmDevice;
impl AsahiDrmDevice { fn as_ref(&self) -> &Self { self } }
thread_local! { static MAPS: RefCell<Vec<(&'static str, u64, bool)>> = RefCell::new(Vec::new()); }
struct MappedObject { name: &'static str, bytes: Vec<u8>, iova: u64, logical_size: usize, home: u64 }
impl MappedObject {
    fn new(name: &'static str, iova: u64, logical_size: usize) -> Self {
        Self { name, bytes: vec![0; 256], iova, logical_size, home: 1 }
    }
    fn iova(&self) -> u64 { self.iova }
    fn with_bytes_mut<R>(&mut self, f: impl FnOnce(&mut [u8]) -> Result<R>) -> Result<R> { f(&mut self.bytes) }
    fn map_alias_at_with_guards(&mut self, vm: &mmu::Vm, address: u64, _: mmu::Prot, _: usize)
        -> Result<Option<mmu::KernelMapping>> {
        let zero = self.bytes.iter().all(|b| *b == 0);
        MAPS.with(|m| m.borrow_mut().push((self.name, vm.id(), zero)));
        if vm.id() == self.home { return if address == self.iova { Ok(None) } else { Err(EINVAL) }; }
        Ok(Some(mmu::KernelMapping(self.name, vm.id())))
    }
}
// graphics_usc.rs
const DIRECTORY_ENTRIES: usize = 0x80000;
const BLOCK_BYTES: usize = 0x20_0000;
const BLOCK_COUNT: usize = 19;
struct M5GraphicsUsc { directory: MappedObject, freelist: MappedObject, blocks: KVec<MappedObject> }
impl M5GraphicsUsc {
    fn validate_mappings(&self, _: &mmu::Vm) -> Result { Ok(()) }
@@USC@@
}
// parameter_buffer.rs
struct M5ParameterBuffer { blocks: [MappedObject; 3], page_list: MappedObject, metrics: MappedObject,
    scene_scratch: MappedObject, discard: MappedObject }
impl M5ParameterBuffer {
    fn validate(&self, _: &mmu::Vm) -> Result { Ok(()) }
@@PB@@
}
// graphics_memory.rs
struct M5GraphicsMemory { context: mmu::M5ExecutionContext, user_mappings: Option<mmu::VmJobGuard>,
    pages: KVec<MappedObject>, usc: M5GraphicsUsc, output_first: Option<usize> }
impl M5GraphicsMemory {
    fn vm(&self) -> &mmu::Vm { self.context.vm() }
@@MEMORY@@
}
// graphics_job.rs
struct M5GraphicsJob { program: M5GraphicsMemory, parameter_buffer: M5ParameterBuffer,
    compute_storage: MappedObject, vm_aliases: KVec<mmu::KernelMapping>, generation: u32,
    status: [u32; 2], stamps: [u32; 5] }
impl M5GraphicsJob {
    fn scheduler_status(&mut self) -> Result<[u32; 2]> { Ok(self.status) }
    fn progress(&mut self) -> Result<[u32; 5]> { Ok(self.stamps) }
@@OWNS@@
@@JOB@@
}
const DATA: [&str; 25] = ["usc0", "usc1", "usc2", "usc3", "usc4", "usc5", "usc6", "usc7", "usc8",
    "usc9", "usc10", "usc11", "usc12", "usc13", "usc14", "usc15", "usc16", "usc17", "usc18",
    "pb0", "pb1", "pb2", "scene", "discard", "compute"];
const KEPT: [&str; 4] = ["directory", "freelist", "page_list", "metrics"];
fn objects(job: &mut M5GraphicsJob) -> Vec<&mut MappedObject> {
    let usc = &mut job.program.usc;
    let pb = &mut job.parameter_buffer;
    let mut out: Vec<&mut MappedObject> = vec![&mut usc.directory, &mut usc.freelist];
    out.extend(usc.blocks.0.iter_mut());
    out.extend(pb.blocks.iter_mut());
    out.extend([&mut pb.page_list, &mut pb.metrics, &mut pb.scene_scratch, &mut pb.discard,
        &mut job.compute_storage]);
    out
}
fn fill(job: &mut M5GraphicsJob, value: u8) {
    for object in objects(job) { object.bytes.fill(value); }
}
fn snapshot(job: &mut M5GraphicsJob) -> Vec<(&'static str, Vec<u8>)> {
    objects(job).into_iter().map(|o| (o.name, o.bytes.clone())).collect()
}
fn handoff(job: &mut M5GraphicsJob, vm: u64, value: u8) {
    MAPS.with(|m| m.borrow_mut().clear());
    fill(job, value);
    job.rebind_vm(&AsahiDrmDevice, &mmu::Vm(vm)).unwrap();
    let maps = MAPS.with(|m| m.borrow().clone());
    for name in DATA.iter() {
        let entry = maps.iter().find(|(n, v, _)| n == name && *v == vm)
            .unwrap_or_else(|| panic!("{} not mapped into vm {}", name, vm));
        assert!(entry.2, "{} still holds the previous VM data when vm {} maps it", name, vm);
    }
    for object in objects(job) {
        if KEPT.contains(&object.name) {
            assert!(object.bytes.iter().all(|b| *b == value), "{} allocator state must be kept", object.name);
        }
    }
    assert!(job.owns_vm(&mmu::Vm(vm)));
}
fn main() {
    let mut iova = 0x10_0000_0000u64;
    let mut next = |size: usize| { let at = iova; iova += (size as u64 + 0xc000 + 0x7fff) & !0x7fff; at };
    let usc_blocks = (0..BLOCK_COUNT).map(|i| MappedObject::new(DATA[i], next(BLOCK_BYTES), BLOCK_BYTES)).collect();
    let usc = M5GraphicsUsc {
        directory: MappedObject::new("directory", next(DIRECTORY_ENTRIES * 8), DIRECTORY_ENTRIES * 8),
        freelist: MappedObject::new("freelist", next(0x4000), 0x4000), blocks: KVec(usc_blocks) };
    let pb = M5ParameterBuffer {
        blocks: [MappedObject::new("pb0", next(0x80000), 0x80000), MappedObject::new("pb1", next(0x80000), 0x80000),
            MappedObject::new("pb2", next(0x80000), 0x80000)],
        page_list: MappedObject::new("page_list", next(0x1c000), 0x1c000),
        metrics: MappedObject::new("metrics", next(0x4000), 0x4000),
        scene_scratch: MappedObject::new("scene", next(0x4000), 0x4000),
        discard: MappedObject::new("discard", next(0x8000), 0x8000) };
    let mut job = M5GraphicsJob {
        program: M5GraphicsMemory { context: mmu::M5ExecutionContext { vm: mmu::Vm(1) },
            user_mappings: None, pages: KVec::new(), usc, output_first: None },
        parameter_buffer: pb, compute_storage: MappedObject::new("compute", next(0x28000), 0x28000),
        vm_aliases: KVec::new(), generation: 7, status: [2, 0], stamps: [2, 0x700, 0, 0x700, 0] };
    // Client 1 (first owner) hands to client 2, which hands back, then to 3.
    handoff(&mut job, 2, 0xa5);
    assert_eq!(job.vm_aliases.len(), DATA.len() + KEPT.len(), "views of every pool object in vm 2");
    handoff(&mut job, 1, 0x5a);
    assert!(job.vm_aliases.is_empty(), "the first owner keeps its own mappings");
    handoff(&mut job, 3, 0x3c);
    // Same-VM resubmission: no clear.
    fill(&mut job, 0x77);
    let before = snapshot(&mut job);
    job.rebind_vm(&AsahiDrmDevice, &mmu::Vm(3)).unwrap();
    assert!(before == snapshot(&mut job), "same-VM resubmission must not clear");
    // Not retired: nothing is cleared while firmware may still use the pools.
    for (status, stamps) in [([1, 0], [2, 0x700, 0, 0x700, 0]), ([2, 0], [2, 0x600, 0, 0x700, 0]),
        ([2, 0], [2, 0x700, 0, 0x600, 0]), ([2, 1], [2, 0x700, 0, 0x700, 0])] {
        job.status = status; job.stamps = stamps;
        assert_eq!(job.rebind_vm(&AsahiDrmDevice, &mmu::Vm(4)).err(), Some(EBUSY));
        assert!(before == snapshot(&mut job), "busy job storage must be untouched");
    }
    println!("PASS 3 client handoffs clear 25 data objects before the next VM maps them; allocator state kept; same-VM and 4 unretired cases untouched");
}
'''


def fixture(src):
    return (PRELUDE.replace('@@USC@@', item(src['usc'], 'pub(super) fn alias_into('))
            .replace('@@PB@@', item(src['pb'], 'pub(super) fn alias_into('))
            .replace('@@MEMORY@@', item(src['memory'], 'pub(super) fn rebind('))
            .replace('@@OWNS@@', item(src['job'], 'pub(super) fn owns_vm('))
            .replace('@@JOB@@', item(src['job'], 'pub(super) fn rebind_vm(')))


def run(tmp, name, code):
    path = Path(tmp) / (name + '.rs')
    binary = path.with_suffix('')
    path.write_text(code)
    subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2021', str(path), '-o', str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def drop(src, key, text):
    assert text in src[key], (key, text)
    return {**src, key: src[key].replace(text, '')}


def main():
    src = current()
    mutations = [
        ('unfixed-' + BASE, original),
        ('no-usc-block-clear', lambda: drop(src, 'usc', 'block.with_bytes_mut(|out| { out.fill(0); Ok(()) })?;')),
        ('no-parameter-buffer-clear', lambda: drop(src, 'pb', 'object.with_bytes_mut(|out| { out.fill(0); Ok(()) })?;')),
        ('no-compute-storage-clear', lambda: drop(src, 'job',
            'self.compute_storage.with_bytes_mut(|out| { out.fill(0); Ok(()) })?;')),
    ]
    with tempfile.TemporaryDirectory(prefix='m5-client-handoff-') as tmp:
        result = run(tmp, 'fixed', fixture(src))
        assert result.returncode == 0, result.stderr
        print(result.stdout.strip())
        if '--mutations' not in sys.argv[1:]:
            return
        killed = 0
        for name, make in mutations:
            result = run(tmp, name.replace('-', '_'), fixture(make()))
            assert result.returncode != 0 and 'still holds the previous VM data' in result.stderr, \
                (name, result.stdout, result.stderr)
            killed += 1
            print('PASS mutant killed:', name)
        print(f'PASS {killed}/{len(mutations)} mutants killed')


if __name__ == '__main__':
    main()
