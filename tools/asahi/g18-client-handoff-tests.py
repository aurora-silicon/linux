#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the G18 client hand-over of the application render and compute owners against a model.

The harness extracts from drivers/gpu/drm/asahi/g18 ApplicationJob and ApplicationJob::prepare()
(runtime/graphics_job_application.rs), G18ParameterBuffer::application_aliases() and
clear_client_data() (queue/graphics_application.rs), G18UmaPoolMemory::application_aliases()
(buffer/pool_application.rs), compute_application_aliases() and clear_scratch_for()
(buffer/pool.rs) and replacement_shared_compute() (buffer/pool_shared_compute.rs), and compiles
them with host rustc against a model of the GEM objects and their GPU mappings. The model's GPU
work of a VM writes the VM's id through each of its writable views of the PB blocks, scene
scratch, discard page, render targets and pool scratch blocks.

What it shows:
- when the render job goes to another VM, no live view of that VM reaches a byte another VM
  wrote, PB blocks added by growth included, and the previous VM's views are gone first;
- the same VM again, and a target regrowth of the same VM, clear nothing;
- the client view of the command graph is read-only;
- the compute pool's scratch blocks are cleared when another VM is given them, also when a
  replacement pool retains the old scratch objects, and not for the same VM;
- source checks: both owners refuse a job that has not retired before any of this, the session
  aliases the render pool only after prepare(), and the compute attempt drops the previous
  aliases before it aliases the pool.

--mutations runs every mutant, including the code at c5e25e0925351 (before the fix), and
requires each to fail.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G18 = 'drivers/gpu/drm/asahi/g18/'
PREPARE = G18 + 'runtime/graphics_job_application.rs'
PB = G18 + 'queue/graphics_application.rs'
POOL = G18 + 'buffer/pool.rs'
POOL_APP = G18 + 'buffer/pool_application.rs'
POOL_SHARED = G18 + 'buffer/pool_shared_compute.rs'
SESSION = G18 + 'boot/session.rs'
COMPUTE = G18 + 'initdata/application_compute.rs'
BASE = 'c5e25e0925351'


def item(source, signature, optional=False):
    if optional and signature not in source:
        return ''
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


MODEL = r'''
#![allow(dead_code, unused_variables, unused_mut, unused_imports)]
use std::cell::RefCell;
use std::ops::Range;
use std::rc::Rc;
use std::sync::atomic::{fence, Ordering};
type Result<T = ()> = std::result::Result<T, i32>;
const EBUSY: i32 = 16; const EINVAL: i32 = 22; const EOVERFLOW: i32 = 75; const EFAULT: i32 = 14;
const ENOENT: i32 = 2;
#[derive(Clone, Copy)] struct Flags;
const GFP_KERNEL: Flags = Flags;
type KBox<T> = Box<T>;
struct KVec<T>(Vec<T>);
impl<T> KVec<T> {
    fn new() -> Self { KVec(Vec::new()) }
    fn push(&mut self, v: T, _: Flags) -> Result { self.0.push(v); Ok(()) }
    fn clear(&mut self) { self.0.clear() }
    fn len(&self) -> usize { self.0.len() }
    fn is_empty(&self) -> bool { self.0.is_empty() }
    fn iter(&self) -> std::slice::Iter<'_, T> { self.0.iter() }
    fn iter_mut(&mut self) -> std::slice::IterMut<'_, T> { self.0.iter_mut() }
}
impl<T> IntoIterator for KVec<T> {
    type Item = T; type IntoIter = std::vec::IntoIter<T>;
    fn into_iter(self) -> Self::IntoIter { self.0.into_iter() }
}
#[macro_export]
macro_rules! g18_kick_info { ($($t:tt)*) => {}; }
struct AsahiDrmDevice;
impl AsahiDrmDevice { fn as_ref(&self) -> &Self { self } }

type Data = Rc<RefCell<Vec<u8>>>;
struct Live { key: usize, vm: u64, addr: u64, prot: mmu::Prot, data: Data }
thread_local! {
    static LIVE: RefCell<Vec<Live>> = RefCell::new(Vec::new());
    static NEXT: RefCell<usize> = RefCell::new(0);
    static CLEARS: RefCell<usize> = RefCell::new(0);
}
mod mmu {
    use super::*;
    #[derive(Clone, Copy, PartialEq, Eq, Debug)] pub enum Prot { Rw, Ro }
    pub const PROT_GPU_SHARED_RW: Prot = Prot::Rw;
    pub const PROT_GPU_SHARED_RO: Prot = Prot::Ro;
    #[derive(Clone)] pub struct Vm { pub id: u64 }
    impl Vm {
        pub fn id(&self) -> u64 { self.id }
        pub fn retain_t8140_job(&self) -> Result<T8140VmJobGuard> { Ok(T8140VmJobGuard) }
        pub fn translate_iova(&self, a: u64) -> Result<u64> { Ok(a) }
    }
    pub struct T8140VmJobGuard;
    pub struct Uat { pub lower: Vm }
    impl Uat { pub fn kernel_lower_vm(&self) -> &Vm { &self.lower } }
    pub struct KernelMapping { key: usize }
    impl KernelMapping {
        pub fn new(vm: &Vm, addr: u64, prot: Prot, data: &Data) -> Self {
            let key = NEXT.with(|n| { let mut n = n.borrow_mut(); *n += 1; *n });
            LIVE.with(|l| {
                let mut l = l.borrow_mut();
                assert!(!l.iter().any(|m| m.vm == vm.id && m.addr == addr),
                    "VA {:#x} mapped twice in VM {}: the previous views were not dropped", addr, vm.id);
                l.push(Live { key, vm: vm.id, addr, prot, data: data.clone() });
            });
            KernelMapping { key }
        }
    }
    impl Drop for KernelMapping {
        fn drop(&mut self) { LIVE.with(|l| l.borrow_mut().retain(|m| m.key != self.key)); }
    }
}
struct MappedObject { data: Data, iova: u64, logical_size: usize }
impl MappedObject {
    fn new(iova: u64, size: usize) -> Self {
        MappedObject { data: Rc::new(RefCell::new(vec![0; size])), iova, logical_size: size }
    }
    fn shared(&self, iova: u64) -> Self { MappedObject { data: self.data.clone(), iova, logical_size: self.logical_size } }
    fn iova(&self) -> u64 { self.iova }
    fn with_bytes_mut<R>(&mut self, f: impl FnOnce(&mut [u8]) -> Result<R>) -> Result<R> {
        let mut bytes = self.data.borrow_mut();
        let all_zero_before = bytes.iter().all(|&b| b == 0);
        let r = f(&mut bytes[..]);
        // Render targets are cleared every frame; count the hand-over clears only.
        let target = (va::GRAPHICS_TARGET_OFFSET..va::GRAPHICS_TARGET_OFFSET + 0x30000).contains(&self.iova);
        if !target && !all_zero_before && bytes.iter().all(|&b| b == 0) { CLEARS.with(|c| *c.borrow_mut() += 1); }
        r
    }
    fn map_alias_at(&mut self, vm: &mmu::Vm, address: u64, prot: mmu::Prot) -> Result<mmu::KernelMapping> {
        Ok(mmu::KernelMapping::new(vm, address, prot, &self.data))
    }
    fn regrow_wc_at(&mut self, _: &AsahiDrmDevice, _: &mmu::Vm, _: u64, size: usize) -> Result {
        if size > self.logical_size { *self.data.borrow_mut() = vec![0; size]; self.logical_size = size; }
        Ok(())
    }
}
mod g18 {
    pub mod address_map { pub const COMPACT_BIAS: u64 = 0x10_0000_0000; }
    pub mod uapi { pub mod graphics_scratch {
        #[derive(Clone, Copy)] pub struct G18RenderScratchSelection;
    } }
}
mod va {
    pub use crate::g18::address_map::COMPACT_BIAS;
    pub const GRAPHICS_JOB_LOW: u64 = 0x70_1200_0000;
    pub const GRAPHICS_TARGET_OFFSET: u64 = 0x0100_0000;
}
#[derive(Clone, Copy, Default)]
struct Params {
    cycle: u64, record_index: u64, ta_hardware_buffer_id: u32, usc_flist_hardware_buffer_id: u32,
    scissor_array: u64, depth_bias_array: u64, depth_buffer: u64, depth_aux_buffer: u64,
    load_pipeline: u64, store_pipeline: u64, partial_load_pipeline: u64, partial_store_pipeline: u64,
    target: usize,
}
#[derive(Clone, Copy, Default)] struct Controls;
struct RenderTarget { width: u32, height: u32 }
struct G18ApplicationRender {
    parameters: Params, controls: Controls,
    scratch: Option<crate::g18::uapi::graphics_scratch::G18RenderScratchSelection>,
    target: RenderTarget, vdm_base: u64,
}
mod assembly {
    pub struct Plan { pub offsets: [u64; 3], pub sizes: [usize; 3] }
    pub fn target_plan(p: &super::Params) -> Option<Plan> {
        Some(Plan { offsets: [0, 0x10000, 0x20000], sizes: [p.target; 3] })
    }
    pub fn apply_target_plan(_: &mut super::Params, _: &Plan) {}
    pub const TARGET_NAMES: [&str; 3] = ["a", "b", "c"];
}
mod regs { pub fn initialize_g18_deflake(_: &mut [u8], _: u32, _: u32) -> Option<()> { Some(()) } }
#[derive(Clone, Copy)] struct Frame { generation: u32 }
impl Frame { fn new(generation: u32) -> Option<Self> { (generation != 0).then_some(Frame { generation }) } }
#[derive(Clone, Copy)] struct WalkKey;
struct Program { vm: mmu::Vm }
impl Program {
    fn matches_vm(&self, vm: &mmu::Vm) -> bool { self.vm.id == vm.id }
    fn rebind_retired(&mut self, vm: &mmu::Vm) -> Result { self.vm = vm.clone(); Ok(()) }
}
struct G18PbCommandFields { pb_slot: u32 }
#[derive(Clone, Copy)]
struct G18PbAddresses { page_list_low: u64, page_list_row: u64, metrics_low: u64, scene_scratch_low: u64, discard_low: u64 }
const PB_BLOCKS_LOW: u64 = 0x70_2000_0000;
const PB_BLOCK: u64 = 0x80000;
struct G18ParameterBuffer {
    page_list: MappedObject, metrics: MappedObject, scene_scratch: MappedObject,
    discard: MappedObject, blocks: KVec<MappedObject>, addresses: G18PbAddresses,
}
impl G18ParameterBuffer {
    fn application_scene(&mut self, _: Option<&[u8; 0x80]>) -> Result<[u8; 0x80]> { Ok([0; 0x80]) }
    fn scene_cycle(&self) -> u64 { 0 }
    fn metrics_record(&self) -> u64 { 0 }
    fn command_fields(&self) -> G18PbCommandFields { G18PbCommandFields { pb_slot: 0 } }
@@PB@@
}
struct G18GraphicsJob {
    program: Program, targets: KVec<MappedObject>, aliases: KVec<mmu::KernelMapping>,
    graph: MappedObject, parameter_buffer: G18ParameterBuffer, parameters: Params,
    controls: Controls, signals: MappedObject, encoded: Option<()>,
}
impl G18GraphicsJob { fn context_id(&self) -> u8 { 3 } }
@@JOB@@
impl ApplicationJob {
    fn encode_application(&mut self, _: &AsahiDrmDevice, _: Frame) -> Result { Ok(()) }
    fn retired(&mut self) { self.retired = true; self.guard = None; }
@@PREPARE@@
}
const DIRECTORY_BASE: u64 = 0x70_0200_0000;
const BLOCK_LIST_BASE: u64 = 0x70_0400_0000;
const BLOCK_DATA_BASE: u64 = 0x70_0500_0000;
const BLOCK_DATA_STRIDE: u64 = 0x20_0000;
const G18_COMPUTE_BLOCK_BYTES: u64 = 0x20_0000;
struct Layout;
impl Layout { fn page_offset(&self, i: u32) -> Option<u64> { Some(u64::from(i) * 0x4000) } }
#[derive(Clone, Copy)] struct G18ComputePlan;
struct G18UmaPoolMemory {
    process_aliases: KVec<mmu::KernelMapping>, scratch: KVec<MappedObject>, scratch_plan: Option<()>,
    directory: KVec<MappedObject>, block_list: MappedObject, layout: Layout, encoded: bool,
    descriptor_loaded: bool, render: bool, low_bias: u64, hardware_slot: u32, client_vm: Option<u64>,
}
impl G18UmaPoolMemory {
    fn model(render: bool, low_bias: u64, blocks: usize) -> Self {
        let mut scratch = KVec::new();
        for i in 0..blocks { let _ = scratch.push(MappedObject::new(0x1000 * i as u64, 64), GFP_KERNEL); }
        let mut directory = KVec::new();
        let _ = directory.push(MappedObject::new(0, 64), GFP_KERNEL);
        G18UmaPoolMemory { process_aliases: KVec::new(), scratch, scratch_plan: Some(()), directory,
            block_list: MappedObject::new(0, 64), layout: Layout, encoded: true, descriptor_loaded: true,
            render, low_bias, hardware_slot: 1, client_vm: None }
    }
    // As the production constructor: a fresh pool has no client.
    fn new_layout(_: &AsahiDrmDevice, _: &mmu::Uat, _: Range<u64>, _: u32, _: bool, _: bool,
        low_bias: u64, _: u32, _: u64) -> Result<Self> {
        let mut pool = Self::model(false, low_bias, 0); pool.scratch_plan = None; pool.encoded = false;
        pool.descriptor_loaded = false; Ok(pool)
    }
    // The retained scratch objects are the same memory, plus one fresh block.
    fn prepare_shared_compute_retaining(&mut self, _: &AsahiDrmDevice, _: &mmu::Uat, _: Range<u64>,
        _: G18ComputePlan, retained: Option<&Self>) -> Result {
        for (i, old) in retained.unwrap().scratch.iter().enumerate() {
            self.scratch.push(old.shared(0x10_0000 + 0x1000 * i as u64), GFP_KERNEL)?;
        }
        self.scratch.push(MappedObject::new(0x20_0000, 64), GFP_KERNEL)?;
        self.scratch_plan = Some(()); Ok(())
    }
    fn alias_into_process(&mut self, _: &AsahiDrmDevice, _: &mmu::Uat, vm: &mmu::Vm) -> Result {
        let mut aliases = KVec::new();
        for (i, block) in self.scratch.iter_mut().enumerate() {
            aliases.push(block.map_alias_at(vm, BLOCK_DATA_BASE + self.low_bias + i as u64 * 0x30_0000,
                mmu::PROT_GPU_SHARED_RW)?, GFP_KERNEL)?;
        }
        self.process_aliases = aliases; Ok(())
    }
    fn audit_render_mappings(&self, _: &mmu::Uat, _: &mmu::Vm) -> Result<usize> { Ok(0) }
@@POOL@@
}

/// Kernel-only addresses: kept by design (they name the PB's and pool's own objects).
fn kept(addr: u64) -> bool {
    let bias = va::COMPACT_BIAS;
    [0x70_3000_0000u64, 0x70_3100_0000, DIRECTORY_BASE, BLOCK_LIST_BASE, va::GRAPHICS_JOB_LOW]
        .iter().any(|&a| addr == a || addr == a + bias)
}
/// The GPU work of `vm` writes its id through each writable view it has.
fn gpu_work(vm: u64) {
    LIVE.with(|l| for m in l.borrow().iter() {
        if m.vm == vm && m.prot == mmu::Prot::Rw && !kept(m.addr) { m.data.borrow_mut().fill(vm as u8); }
    });
}
/// No live view of `vm` may reach a byte another VM wrote.
fn exposed(vm: u64, what: &str) {
    LIVE.with(|l| for m in l.borrow().iter() {
        if m.vm == vm {
            let data = m.data.borrow();
            assert!(data.iter().all(|&b| b == 0 || b == vm as u8),
                "{}: VM {} maps {:#x}, which holds VM {}'s data", what, vm, m.addr,
                data.iter().find(|&&b| b != 0 && b != vm as u8).unwrap());
        }
    });
}
fn holds(object: &MappedObject, vm: u64) -> bool { object.data.borrow().iter().all(|&b| b == vm as u8) }
fn clears() -> usize { CLEARS.with(|c| *c.borrow()) }

fn new_job() -> Box<ApplicationJob> {
    let mut blocks = KVec::new();
    for i in 0..2u64 { let _ = blocks.push(MappedObject::new(PB_BLOCKS_LOW + i * PB_BLOCK, 64), GFP_KERNEL); }
    let pb = G18ParameterBuffer {
        page_list: MappedObject::new(0x70_3000_0000, 64), metrics: MappedObject::new(0x70_3100_0000, 64),
        scene_scratch: MappedObject::new(0x70_3200_0000, 64), discard: MappedObject::new(0x70_3300_0000, 64),
        blocks, addresses: G18PbAddresses { page_list_low: 0x70_3000_0000, page_list_row: 0x70_3000_0000 + va::COMPACT_BIAS,
            metrics_low: 0x70_3100_0000, scene_scratch_low: 0x70_3200_0000, discard_low: 0x70_3300_0000 },
    };
    let mut targets = KVec::new();
    for offset in [0u64, 0x10000, 0x20000] {
        let _ = targets.push(MappedObject::new(va::GRAPHICS_TARGET_OFFSET + offset, 64), GFP_KERNEL);
    }
    let job = G18GraphicsJob { program: Program { vm: mmu::Vm { id: 100 } }, targets, aliases: KVec::new(),
        graph: MappedObject::new(0xffff_0000, 64), parameter_buffer: pb, parameters: Params::default(),
        controls: Controls, signals: MappedObject::new(0, 0xc020), encoded: None };
    Box::new(ApplicationJob { job: Box::new(job), storage: mmu::Vm { id: 100 }, user_aliases: KVec::new(),
        pool_aliases: KVec::new(), guard: None, scene: [0; 0x80], generation: 0, retired: true,
        client_frames: 0, scratch: None, walked: None })
}
fn render(target: usize) -> G18ApplicationRender {
    G18ApplicationRender { parameters: Params { target, ..Params::default() }, controls: Controls,
        scratch: None, target: RenderTarget { width: 8, height: 8 }, vdm_base: 0 }
}
/// submit_application: prepare, the render pool once per remap, the frame, retirement.
fn frame(job: &mut ApplicationJob, pool: &mut G18UmaPoolMemory, uat: &mmu::Uat, vm: u64, target: usize) {
    let vm = mmu::Vm { id: vm };
    job.prepare(&AsahiDrmDevice, uat, &vm, &render(target)).unwrap();
    if job.pool_aliases.is_empty() { job.pool_aliases = pool.application_aliases(uat, &vm).unwrap(); }
    exposed(vm.id, "render hand-over");
    LIVE.with(|l| for m in l.borrow().iter() {
        if m.vm == vm.id && (m.addr == va::GRAPHICS_JOB_LOW) {
            assert_eq!(m.prot, mmu::Prot::Ro, "the client view of the command graph must be read-only");
        }
    });
    gpu_work(vm.id);
    job.retired();
}

fn main() {
    let uat = mmu::Uat { lower: mmu::Vm { id: 0 } };
    let mut job = new_job();
    let mut pool = G18UmaPoolMemory::model(true, 0, 3);
    frame(&mut job, &mut pool, &uat, 1, 64);
    // A firmware TVB grow request backs a block for VM 1, aliased with its other PB views.
    let mut grown = MappedObject::new(PB_BLOCKS_LOW + 2 * PB_BLOCK, 64);
    for at in [grown.iova(), grown.iova() + va::COMPACT_BIAS] {
        let alias = grown.map_alias_at(&mmu::Vm { id: 1 }, at, mmu::PROT_GPU_SHARED_RW).unwrap();
        job.user_aliases.push(alias, GFP_KERNEL).unwrap();
    }
    job.job.parameter_buffer.blocks.push(grown, GFP_KERNEL).unwrap();
    gpu_work(1);
    let before = clears();
    frame(&mut job, &mut pool, &uat, 1, 64);
    frame(&mut job, &mut pool, &uat, 1, 128);
    assert_eq!(clears(), before, "the same VM again (also with regrown targets) must not clear the PB or pool");
    assert!(job.job.parameter_buffer.blocks.iter().all(|b| holds(b, 1)) && pool.scratch.iter().all(|b| holds(b, 1)),
        "the same VM's storage must be kept");
    for vm in [2, 2, 1, 3, 2] { frame(&mut job, &mut pool, &uat, vm, 128); }
    println!("PASS render: 8 frames over 3 VMs, PB growth, target regrowth; no VM sees another's PB blocks, scene scratch, discard page or pool scratch; graph view read-only; same VM clears nothing");

    let mut compute = G18UmaPoolMemory::model(false, 0x4000_0000, 2);
    let mut owner = |pool: &mut G18UmaPoolMemory, vm: u64| {
        let aliases = pool.compute_application_aliases(&AsahiDrmDevice, &uat, &mmu::Vm { id: vm }).unwrap();
        exposed(vm, "compute hand-over");
        gpu_work(vm);
        aliases // the attempt keeps them until the next submission clears job.aliases
    };
    let a = owner(&mut compute, 1); drop(a);
    let before = clears();
    let a = owner(&mut compute, 1); drop(a);
    assert_eq!(clears(), before, "the same VM's compute again must not clear the pool");
    assert!(compute.scratch.iter().all(|b| holds(b, 1)));
    let a = owner(&mut compute, 2); drop(a);
    let mut replacement = compute.replacement_shared_compute(&AsahiDrmDevice, &uat, 0..1, G18ComputePlan).unwrap();
    let a = owner(&mut replacement, 1); drop(a);
    let a = owner(&mut replacement, 1); drop(a);
    let a = owner(&mut replacement, 3); drop(a);
    println!("PASS compute: 6 submissions over 3 VMs and a replacement pool retaining the scratch objects; no VM sees another's scratch; same VM clears nothing");
}
'''


def build(read):
    prepare_src = read(PREPARE)
    pb_src = read(PB)
    pool_src = read(POOL)
    job = item(prepare_src, 'pub(crate) struct ApplicationJob {')
    prepare = item(prepare_src, 'pub(crate) fn prepare(')
    pb = (item(pb_src, 'pub(crate) fn application_aliases(&mut self, vm: &mmu::Vm)') + '\n'
          + item(pb_src, 'pub(crate) fn clear_client_data(', optional=True))
    pool = (item(read(POOL_APP), 'pub(crate) fn application_aliases(&mut self, uat') + '\n'
            + item(pool_src, 'pub(crate) fn compute_application_aliases(') + '\n'
            + item(pool_src, 'fn clear_scratch_for(', optional=True) + '\n'
            + item(read(POOL_SHARED), 'pub(crate) fn replacement_shared_compute('))
    return (MODEL.replace('@@JOB@@', job).replace('@@PREPARE@@', prepare)
            .replace('@@PB@@', pb).replace('@@POOL@@', pool))


def sub(old, new):
    def apply(code):
        assert old in code, 'mutation anchor missing: ' + old
        return code.replace(old, new, 1)
    return apply


MUTATIONS = {
    'pb-clear-dropped': sub('if new_client && self.generation!=0 {self.job.parameter_buffer.clear_client_data()?;}', ''),
    'pb-clear-every-remap': sub('if new_client && self.generation!=0 {', 'if self.generation!=0 {'),
    'pb-grown-blocks-kept': sub('for block in self.blocks.iter_mut() {\n            clear(block)', 'for block in self.blocks.iter_mut().take(2) {\n            clear(block)'),
    'pb-scene-scratch-kept': sub('clear(&mut self.scene_scratch)?;', ''),
    'pb-discard-kept': sub('clear(&mut self.discard)?;', ''),
    'graph-view-writable': sub('va::GRAPHICS_JOB_LOW,\n                mmu::PROT_GPU_SHARED_RO)', 'va::GRAPHICS_JOB_LOW,\n                mmu::PROT_GPU_SHARED_RW)'),
    'render-pool-clear-dropped': sub('{return Err(EINVAL);}\n        self.clear_scratch_for(vm)?;', '{return Err(EINVAL);}'),
    'compute-pool-clear-dropped': sub('        self.clear_scratch_for(vm)?;\n        self.alias_into_process(', '        self.alias_into_process('),
    'pool-clear-every-alias': sub('self.client_vm.is_some_and(|id| id != vm.id())', 'self.client_vm.is_some()'),
    'replacement-forgets-client': sub('pool.client_vm = self.client_vm;', ''),
}


def source_checks(read):
    prepare = item(read(PREPARE), 'pub(crate) fn prepare(')
    assert prepare.index('if !self.retired {return Err(EBUSY);}') < prepare.index('clear_client_data'), 'prepare: retired first'
    submit = item(read(SESSION), 'fn submit_application(')
    assert submit.index('job.prepare(') < submit.index('.application_aliases(vm)'), 'session: pool aliased after prepare'
    attempt = item(read(COMPUTE), 'fn prepare_application_compute_attempt(')
    retired = attempt.index('if !job.retired')
    dropped = attempt.index('job.aliases.clear();')
    aliased = attempt.index('pool.compute_application_aliases(')
    assert retired < dropped < aliased, 'compute: retired, previous aliases dropped, then the pool aliased'
    print('PASS source: retirement is checked first; previous views are dropped before the pools are aliased')


def compile_run(code, out, name, rustc):
    path = out / (name + '.rs')
    path.write_text(code)
    binary = out / name
    built = subprocess.run(rustc + ['--edition=2021', '-O', str(path), '-o', str(binary)],
                           capture_output=True, text=True)
    if built.returncode != 0:
        raise SystemExit(f'{name}: host build failed\n{built.stderr}')
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--output', type=Path, help='Work directory (default: a temporary one)')
    p.add_argument('--rustc', default=os.environ.get('HOSTRUSTC') or os.environ.get('RUSTC', 'rustc'))
    p.add_argument('--mutations', action='store_true',
                   help=f'Also require every mutant and the code at {BASE} to fail')
    a = p.parse_args()
    out = (a.output or Path(tempfile.mkdtemp(prefix='g18-client-handoff-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    rustc = [a.rustc]

    def tip(path):
        return (ROOT / path).read_text()

    def base(path):
        return subprocess.check_output(['git', 'show', f'{BASE}:{path}'], cwd=ROOT, text=True)

    source_checks(tip)
    result = compile_run(build(tip), out, 'fixed', rustc)
    if result.returncode != 0:
        sys.exit('FAIL fixed code\n' + result.stderr)
    print(result.stdout.strip())
    if not a.mutations:
        return
    unchecked = sub('assert_eq!(m.prot, mmu::Prot::Ro,', 'assert!(true || m.prot == mmu::Prot::Ro,')
    mutants = [(BASE, build(base)), (BASE + '-data-only', unchecked(build(base)))]
    for name, apply in MUTATIONS.items():
        code = build(tip)
        mutants.append((name, apply(code)))
    killed = 0
    for name, code in mutants:
        result = compile_run(code, out, 'mutant-' + name, rustc)
        if result.returncode == 0 or 'panicked at' not in result.stderr:
            sys.exit(f'FAIL mutant {name} survived\n{result.stdout}{result.stderr}')
        reason = next((l for l in result.stderr.splitlines() if l and 'panicked at' not in l and 'note:' not in l), '')
        print(f'PASS mutant killed: {name}: {reason.strip()}')
        killed += 1
    print(f'PASS {killed}/{len(mutants)} mutants killed')


if __name__ == '__main__':
    main()
