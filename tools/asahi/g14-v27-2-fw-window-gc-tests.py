#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host tests for G14 27.2 dynamic FirmwareWindow L2 mirroring and alloc.private reclamation.

Extracts and compiles the real production code from:
  - drivers/gpu/drm/asahi/mmu.rs (UAT_L2_SHIFT, FirmwareWindow, VmInner::map_node,
    VmInner::sync_firmware_window)
  - drivers/gpu/drm/asahi/buffer.rs (ParameterListPlacement::info_in_private)
  - drivers/gpu/drm/asahi/gpu.rs (MAX_FW_ALLOC_GARBAGE_*, GarbageAction, garbage_action)

and verifies both fixes for aurora-silicon/linux#3067 against a mock 3-level UAT page table,
non-snooping hardware UAT walker, PoC cache-maintenance/TLBI log, and render-flight lifecycle:

  1. Dynamic FirmwareWindow::sync_range mirrors newly allocated L2 tables across 32/64/96 MiB
     boundaries for alloc.gpu, alloc.shared, and multi-L2 straddling mappings with strict
     PoC-clean -> barrier -> store -> PoC-clean -> barrier -> TLBI ordering and atomic
     fail-closed rollback on corruption.
  2. G14 27.2 render-queue churn places the unused legacy buffer::Info wrapper in uncached
     alloc.shared (keep_garbage = false) instead of cached alloc.private (keep_garbage = true),
     so alloc.private stays at zero runtime garbage/leaks while alloc.shared/alloc.gpu reclaim
     memory strictly after RenderFlight retirement (no premature free / UAF while in flight).
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASAHI = ROOT / 'drivers/gpu/drm/asahi'

MUTATIONS = (
    'boot-only-mirror',
    'skip-l3-clean',
    'skip-l3-barrier',
    'skip-shared-l2-clean',
    'skip-tlbi',
    'skip-intact-check',
    'skip-existing-shared-check',
    'partial-publish-before-validate',
    'info-in-private-leak',
    'premature-free-in-flight',
)


def extract_item(source: str, marker: str) -> str:
    count = source.count(marker)
    assert count == 1, f'expected 1 occurrence of {marker!r}, found {count}'
    start = source.index(marker)
    open_brace = source.index('{', start)
    depth = 1
    pos = open_brace + 1
    in_str = False
    in_line_comment = False
    while depth > 0:
        ch = source[pos]
        nxt = source[pos + 1] if pos + 1 < len(source) else ''
        if in_line_comment:
            if ch == '\n':
                in_line_comment = False
        elif in_str:
            if ch == '\\':
                pos += 1
            elif ch == '"':
                in_str = False
        else:
            if ch == '/' and nxt == '/':
                in_line_comment = True
                pos += 1
            elif ch == '"':
                in_str = True
            elif ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
        pos += 1
    return source[start:pos]


def extract_line(source: str, prefix: str) -> str:
    matches = [line for line in source.splitlines() if line.strip().startswith(prefix)]
    assert len(matches) == 1, f'expected 1 line starting with {prefix!r}, found {len(matches)}'
    return matches[0]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    assert text.count(old) == 1, f'mutation {label}: expected 1 match for {old!r}, found {text.count(old)}'
    return text.replace(old, new, 1)


def build_source(mutation: str | None) -> str:
    mmu_src = (ASAHI / 'mmu.rs').read_text()
    g14_src = (ASAHI / 'g14_v27_2.rs').read_text()
    buf_src = (ASAHI / 'buffer.rs').read_text()
    gpu_src = (ASAHI / 'gpu.rs').read_text()

    # Structural wiring checks on production files.
    assert 'graph.kernel_uat().attach_firmware_window(window);' in g14_src
    assert 'inner.sync_firmware_window(iova..end)' in mmu_src
    assert 'inner.sync_firmware_window(range.clone())?;' in mmu_src
    assert 'if list_placement.info_in_private()' in buf_src
    # RenderFlight owns scene (holding Buffer) and packet (holding QueueRender) until ta_done && fs_done.
    retire_render = extract_item(g14_src, 'fn retire_render(&self, state: &mut State)')
    assert 'if ta_done && fs_done {\n            let flight = state.rendering.take()' in retire_render

    l2_shift = extract_line(mmu_src, 'const UAT_L2_SHIFT:')
    magic_const = extract_line(mmu_src, 'const MAGIC_G14_V27_2:')
    fw_struct = extract_item(mmu_src, 'pub(crate) struct FirmwareWindow')
    fw_shared_entries = extract_item(mmu_src, 'fn shared_entries(')
    fw_shared = extract_item(mmu_src, 'fn shared(&self)')
    fw_context = extract_item(mmu_src, 'fn context(&self)')
    fw_empty = extract_item(mmu_src, 'fn empty(&self)')
    fw_mirror = extract_item(mmu_src, 'pub(crate) fn mirror(&mut self, uat: &Uat)')
    fw_install = extract_item(mmu_src, 'pub(crate) fn install(&self)')
    fw_intact = extract_item(mmu_src, 'pub(crate) fn intact(&self)')
    fw_sync_range = extract_item(mmu_src, 'pub(crate) fn sync_range(')

    map_node = extract_item(
        mmu_src,
        'fn map_node(&mut self, node: &mm::Node<(), KernelMappingInner>, prot: Prot)',
    )
    sync_fw_win = extract_item(mmu_src, 'fn sync_firmware_window(&mut self, range: Range<u64>)')

    placement_enum = extract_item(buf_src, 'pub(crate) enum ParameterListPlacement')
    placement_impl = extract_item(buf_src, 'impl ParameterListPlacement')

    max_bytes = extract_line(gpu_src, 'const MAX_FW_ALLOC_GARBAGE_BYTES:')
    max_objs = extract_line(gpu_src, 'const MAX_FW_ALLOC_GARBAGE_OBJECTS:')
    ga_enum = extract_item(gpu_src, 'enum GarbageAction')
    ga_fn = extract_item(gpu_src, 'fn garbage_action(')

    premature_free_mutant = 'false'

    if mutation == 'boot-only-mirror':
        map_node = replace_once(
            map_node,
            'let result = result.and_then(|()| self.sync_firmware_window(node.start()..end));',
            'let result = result;',
            mutation,
        )
    elif mutation == 'skip-l3-clean':
        fw_sync_range = replace_once(
            fw_sync_range,
            'crate::g15::uat::clean_page(e & mask)?;',
            'let _ = e & mask;',
            mutation,
        )
    elif mutation == 'skip-l3-barrier':
        fw_sync_range = replace_once(
            fw_sync_range,
            '// Complete PoC cleaning of every new L3 page before `shared_l2` names it.\n                    pgtable::poc_barrier();',
            '// Mutated: omitted barrier before shared_l2 store.',
            mutation,
        )
    elif mutation == 'skip-shared-l2-clean':
        fw_sync_range = replace_once(
            fw_sync_range,
            'pgtable::clean_poc(\n                core::ptr::from_ref(&shared[dirty_first]).cast::<u8>(),\n                count * size_of::<u64>(),\n            );',
            'let _ = (dirty_first, count);',
            mutation,
        )
    elif mutation == 'skip-tlbi':
        fw_sync_range = replace_once(
            fw_sync_range,
            'mem::tlbi_range(0, span_start as usize, span_len);',
            'let _ = (span_start, span_len);',
            mutation,
        )
    elif mutation == 'skip-intact-check':
        fw_sync_range = replace_once(
            fw_sync_range,
            'if self.lower_root == 0 || !self.intact() {\n            return Err(EIO);\n        }',
            'if self.lower_root == 0 {\n            return Err(EIO);\n        }',
            mutation,
        )
    elif mutation == 'skip-existing-shared-check':
        fw_sync_range = replace_once(
            fw_sync_range,
            'if e == prev {\n                            if cur_shared != e {\n                                return Err(EIO);\n                            }\n                            continue;\n                        }',
            'if e == prev {\n                            continue;\n                        }',
            mutation,
        )
    elif mutation == 'partial-publish-before-validate':
        fw_sync_range = replace_once(
            fw_sync_range,
            'crate::g15::uat::clean_page(e & mask)?;\n                        span = Some(match span {',
            'crate::g15::uat::clean_page(e & mask)?;\n                        shared[idx].store(e, Ordering::Release);\n                        entries[idx] = e;\n                        span = Some(match span {',
            mutation,
        )
    elif mutation == 'info-in-private-leak':
        placement_impl = replace_once(
            placement_impl,
            'Self::KernelGpu => false,',
            'Self::KernelGpu => true,',
            mutation,
        )
    elif mutation == 'premature-free-in-flight':
        premature_free_mutant = 'true'
    elif mutation is not None:
        raise ValueError(f'unknown mutation: {mutation}')

    return f'''
#![allow(dead_code, unused_variables, unused_imports, unused_mut)]

use core::mem::size_of;
use core::ops::{{Deref, Range}};
use core::sync::atomic::{{AtomicBool, AtomicU32, AtomicU64, AtomicU8, Ordering}};
use std::collections::{{BTreeMap, HashMap}};
use std::sync::{{Arc, Mutex}};

type PhysicalAddr = u64;
type Error = i32;
type Result<T = ()> = core::result::Result<T, Error>;
type KVec<T> = Vec<T>;

const EINVAL: Error = 22;
const EIO: Error = 5;
const EBUSY: Error = 16;
const EOVERFLOW: Error = 75;
const ENOSPC: Error = 28;

const UAT_PGBIT: usize = 14;
const UAT_PGSZ: usize = 1 << UAT_PGBIT;
const UAT_PGMSK: usize = UAT_PGSZ - 1;
const UAT_NPTE: usize = UAT_PGSZ / 8;
const UAT_TOP_LEVEL_SHIFT: usize = 36;
const IOVA_KERN_TOP_LEVEL_INDEX: u64 = 2;
const TTBR_VALID: u64 = 1;
const PA_MASK_42: u64 = ((1u64 << 42) - 1) & !(UAT_PGMSK as u64);
const KERNEL_START: u64 = 0xffffffa0_0000_0000;
const KERNEL_END: u64 = 0xffffffb0_0000_0000;
const KERNEL_RANGE: Range<u64> = KERNEL_START..KERNEL_END;
const IOVA_KERN_PRIV_START: u64 = 0xffffffa0_0000_0000;
const IOVA_KERN_SHARED_START: u64 = 0xffffffa8_0000_0000;
const IOVA_KERN_GPU_START: u64 = 0xffffffac_0000_0000;
const IOVA_KERN_MMIO_START: u64 = 0xffffffaf_0000_0000;

{magic_const}
{l2_shift}
{max_bytes}
{max_objs}

macro_rules! dev_err {{
    ($($t:tt)*) => {{}};
}}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Event {{
    CleanPage(u64),
    CleanPoc {{ addr: usize, len: usize }},
    PocBarrier,
    MemSync,
    TlbiRange {{ asid: u8, va: usize, len: usize }},
}}

/// Host RAM page backing for mock UAT tables (shadow root, shadow L2, L3 pages) plus
/// non-snooping PoC visibility tracking: `ram` holds what the CPU wrote; `dram` holds what
/// has been cleaned to PoC for the UAT hardware walker.
struct MockRam {{
    next_pa: u64,
    pages: BTreeMap<u64, Box<[AtomicU64; UAT_NPTE]>>,
    cleaned_pages: BTreeMap<u64, Vec<u64>>,
    pending_l3_cleans: Vec<u64>,
    shared_l2_dram: [u64; UAT_NPTE],
    shared_l2_cpu_base: usize,
    shared_l2_dirty_pending: bool,
    events: Vec<Event>,
}}

impl MockRam {{
    fn new() -> Self {{
        Self {{
            next_pa: 0x10_0000_0000,
            pages: BTreeMap::new(),
            cleaned_pages: BTreeMap::new(),
            pending_l3_cleans: Vec::new(),
            shared_l2_dram: [0; UAT_NPTE],
            shared_l2_cpu_base: 0,
            shared_l2_dirty_pending: false,
            events: Vec::new(),
        }}
    }}

    fn alloc_page(&mut self) -> u64 {{
        let pa = self.next_pa;
        self.next_pa += UAT_PGSZ as u64;
        self.pages.insert(pa, Box::new(core::array::from_fn(|_| AtomicU64::new(0))));
        self.cleaned_pages.insert(pa, vec![0u64; UAT_NPTE]);
        pa
    }}

    fn flush_cleaned_to_dram(&mut self, shared_l2_ptr: *const AtomicU64) {{
        for pa in self.pending_l3_cleans.drain(..) {{
            if let Some(page) = self.pages.get(&pa) {{
                let snap: Vec<u64> = page.iter().map(|a| a.load(Ordering::Acquire)).collect();
                self.cleaned_pages.insert(pa, snap);
            }}
        }}
        if self.shared_l2_dirty_pending && !shared_l2_ptr.is_null() {{
            let slice = unsafe {{ core::slice::from_raw_parts(shared_l2_ptr, UAT_NPTE) }};
            for (dst, src) in self.shared_l2_dram.iter_mut().zip(slice.iter()) {{
                *dst = src.load(Ordering::Acquire);
            }}
            self.shared_l2_dirty_pending = false;
        }}
    }}
}}

static RAM: Mutex<Option<MockRam>> = Mutex::new(None);
static SHARED_L2_PTR: AtomicU64 = AtomicU64::new(0);

fn with_ram<R>(f: impl FnOnce(&mut MockRam) -> R) -> R {{
    let mut guard = RAM.lock().unwrap();
    f(guard.as_mut().expect("RAM not initialized"))
}}

fn init_ram() {{
    SHARED_L2_PTR.store(0, Ordering::Release);
    *RAM.lock().unwrap() = Some(MockRam::new());
}}

mod pgtable {{
    use super::*;

    pub unsafe fn with_root_entries<R>(
        _reserved: Option<&()>,
        ttb: PhysicalAddr,
        count: usize,
        f: impl FnOnce(&[AtomicU64]) -> R,
    ) -> Result<R> {{
        if count > UAT_NPTE {{
            return Err(EINVAL);
        }}
        let ptr: *const AtomicU64 = with_ram(|ram| {{
            ram.pages
                .get(&ttb)
                .map(|b| b.as_ptr())
                .ok_or(EIO)
        }})?;
        let slice = unsafe {{ core::slice::from_raw_parts(ptr, count) }};
        Ok(f(slice))
    }}

    pub fn clean_poc(ptr: *const u8, len: usize) {{
        let addr = ptr as usize;
        with_ram(|ram| {{
            ram.events.push(Event::CleanPoc {{ addr, len }});
            let base = ram.shared_l2_cpu_base;
            if base != 0 && addr >= base && addr + len <= base + UAT_PGSZ {{
                ram.shared_l2_dirty_pending = true;
            }}
        }});
    }}

    pub fn poc_barrier() {{
        let ptr = SHARED_L2_PTR.load(Ordering::Acquire) as *const AtomicU64;
        with_ram(|ram| {{
            ram.events.push(Event::PocBarrier);
            ram.flush_cleaned_to_dram(ptr);
        }});
    }}
}}

mod g15 {{
    pub mod uat {{
        use super::super::*;

        pub fn clean_page(pa: u64) -> Result {{
            with_ram(|ram| {{
                if !ram.pages.contains_key(&pa) {{
                    return Err(EIO);
                }}
                ram.events.push(Event::CleanPage(pa));
                ram.pending_l3_cleans.push(pa);
                Ok(())
            }})
        }}

        pub fn clean_lower(_uat: &Uat) -> Result {{
            Ok(())
        }}
    }}
}}

mod mem {{
    use super::*;

    pub fn sync() {{
        with_ram(|ram| ram.events.push(Event::MemSync));
    }}

    pub fn tlbi_range(asid: u8, va: usize, len: usize) {{
        with_ram(|ram| ram.events.push(Event::TlbiRange {{ asid, va, len }}));
    }}
}}

struct MemMap {{
    buf: Box<[u8]>,
}}

impl MemMap {{
    fn new(size: usize) -> Self {{
        Self {{
            buf: vec![0u8; size].into_boxed_slice(),
        }}
    }}

    fn ptr(&self) -> *mut u8 {{
        self.buf.as_ptr() as *mut u8
    }}
}}

struct UatRegion {{
    base: PhysicalAddr,
    map: MemMap,
}}

#[repr(C)]
struct Handoff {{
    magic_ap: AtomicU64,
    magic_fw: AtomicU64,
    lock_ap: AtomicU8,
    lock_fw: AtomicU8,
    turn: AtomicU32,
    cur_slot: AtomicU32,
}}

#[repr(C)]
struct SlotTTBS {{
    ttb0: AtomicU64,
    ttb1: AtomicU64,
}}

struct Uat {{
    shadow_root: PhysicalAddr,
    lower_root: PhysicalAddr,
}}

impl Uat {{
    fn kernel_window_root(&self) -> PhysicalAddr {{
        self.shadow_root
    }}
    fn kernel_lower_root(&self) -> PhysicalAddr {{
        self.lower_root
    }}
}}

{fw_struct}

impl FirmwareWindow {{
    fn new_mock(pa_mask: u64) -> Self {{
        let handoff = UatRegion {{ base: 0x8_0000_0000, map: MemMap::new(UAT_PGSZ) }};
        let ttbs = UatRegion {{ base: 0x8_0000_4000, map: MemMap::new(UAT_PGSZ) }};
        let tables = UatRegion {{ base: 0x8_0000_8000, map: MemMap::new(UAT_PGSZ) }};
        let shared_l2 = UatRegion {{ base: 0x8_0000_c000, map: MemMap::new(UAT_PGSZ) }};
        // Firmware boot loader installs root[2] -> shared_l2 | 3.
        let root = unsafe {{ &*tables.map.ptr().cast::<[AtomicU64; 4]>() }};
        root[2].store(shared_l2.base | 3, Ordering::Release);
        let shared_ptr = shared_l2.map.ptr() as usize;
        SHARED_L2_PTR.store(shared_ptr as u64, Ordering::Release);
        with_ram(|ram| ram.shared_l2_cpu_base = shared_ptr);
        Self {{
            handoff,
            ttbs,
            tables,
            shared_l2,
            entries: vec![0u64; UAT_NPTE],
            lower_root: 0,
            pa_mask,
        }}
    }}

    {fw_shared_entries}
    {fw_shared}
    {fw_context}
    {fw_empty}
    {fw_mirror}
    {fw_install}
    {fw_intact}
    {fw_sync_range}
}}

#[derive(Clone, Copy)]
struct Prot(u64);

struct KernelMappingInner {{
    mapped_size: usize,
}}

mod mm {{
    use super::*;
    pub struct Node<A, B> {{
        pub start: u64,
        pub inner: B,
        pub _m: core::marker::PhantomData<A>,
    }}
    impl<A, B> Node<A, B> {{
        pub fn start(&self) -> u64 {{
            self.start
        }}
    }}
    impl<A, B> Deref for Node<A, B> {{
        type Target = B;
        fn deref(&self) -> &B {{
            &self.inner
        }}
    }}
}}

struct MockUatInner {{
    m4_adopted: bool,
    failed: AtomicBool,
}}

struct HandoffLease;

impl MockUatInner {{
    fn lock_m4_upper(&self, _upper: bool) -> Result<Option<HandoffLease>> {{
        Ok(None)
    }}
}}

struct MockPageTable {{
    ttb: PhysicalAddr,
    discarded: Vec<Range<u64>>,
}}

impl MockPageTable {{
    fn new() -> Self {{
        let ttb = with_ram(|ram| ram.alloc_page());
        Self {{
            ttb,
            discarded: Vec::new(),
        }}
    }}

    fn ttb(&self) -> PhysicalAddr {{
        self.ttb
    }}

    fn ensure_l2(&mut self) -> PhysicalAddr {{
        let root_pa = self.ttb;
        let cur = unsafe {{
            pgtable::with_root_entries(None, root_pa, 3, |w| w[2].load(Ordering::Acquire))
        }}
        .unwrap();
        if cur & 3 == 3 {{
            return cur & PA_MASK_42;
        }}
        let l2 = with_ram(|ram| ram.alloc_page());
        unsafe {{
            pgtable::with_root_entries(None, root_pa, 3, |w| {{
                w[2].store(l2 | 3, Ordering::Release);
            }})
        }}
        .unwrap();
        l2
    }}

    fn prepare_map(&mut self, range: Range<u64>) -> Result {{
        if range.is_empty() {{
            return Ok(());
        }}
        let l2 = self.ensure_l2();
        let first = ((range.start - KERNEL_START) >> UAT_L2_SHIFT) as usize;
        let last = (((range.end - 1) - KERNEL_START) >> UAT_L2_SHIFT) as usize;
        for idx in first..=last {{
            let cur = unsafe {{
                pgtable::with_root_entries(None, l2, idx + 1, |w| w[idx].load(Ordering::Acquire))
            }}?;
            if cur == 0 {{
                let l3 = with_ram(|ram| ram.alloc_page());
                unsafe {{
                    pgtable::with_root_entries(None, l2, idx + 1, |w| {{
                        w[idx].store(l3 | 3, Ordering::Release);
                    }})
                }}?;
            }}
        }}
        Ok(())
    }}

    fn ensure_unmapped(&mut self, range: Range<u64>) -> Result {{
        if range.is_empty() {{
            return Ok(());
        }}
        let l2 = self.ensure_l2();
        let mut va = range.start;
        while va < range.end {{
            let l2_idx = ((va - KERNEL_START) >> UAT_L2_SHIFT) as usize;
            let l3_idx = ((va >> UAT_PGBIT) & (UAT_NPTE as u64 - 1)) as usize;
            let l2e = unsafe {{
                pgtable::with_root_entries(None, l2, l2_idx + 1, |w| w[l2_idx].load(Ordering::Acquire))
            }}?;
            if l2e & 3 == 3 {{
                let l3 = l2e & PA_MASK_42;
                let leaf = unsafe {{
                    pgtable::with_root_entries(None, l3, l3_idx + 1, |w| w[l3_idx].load(Ordering::Acquire))
                }}?;
                if leaf != 0 {{
                    return Err(EBUSY);
                }}
            }}
            va += UAT_PGSZ as u64;
        }}
        Ok(())
    }}

    fn map_leaves(&mut self, range: Range<u64>, prot: Prot) -> Result {{
        let l2 = self.ensure_l2();
        let mut va = range.start;
        while va < range.end {{
            let l2_idx = ((va - KERNEL_START) >> UAT_L2_SHIFT) as usize;
            let l3_idx = ((va >> UAT_PGBIT) & (UAT_NPTE as u64 - 1)) as usize;
            let l2e = unsafe {{
                pgtable::with_root_entries(None, l2, l2_idx + 1, |w| w[l2_idx].load(Ordering::Acquire))
            }}?;
            assert_eq!(l2e & 3, 3);
            let l3 = l2e & PA_MASK_42;
            let leaf_pa = (0x20_0000_0000 + (va - KERNEL_START)) & PA_MASK_42;
            let pte = leaf_pa | prot.0 | 3;
            unsafe {{
                pgtable::with_root_entries(None, l3, l3_idx + 1, |w| {{
                    w[l3_idx].store(pte, Ordering::Release);
                }})
            }}?;
            // Simulate pgtable.rs clean_poc + poc_barrier for leaf writes into already-cleaned L3s.
            // Note: brand-new L3 tables that haven't been cleaned yet are NOT pushed to DRAM
            // unless FirmwareWindow::sync_range cleans them!
            with_ram(|ram| {{
                if let Some(dram_l3) = ram.cleaned_pages.get_mut(&l3) {{
                    if ram.shared_l2_dram[l2_idx] == l2e {{
                        dram_l3[l3_idx] = pte;
                    }}
                }}
            }});
            va += UAT_PGSZ as u64;
        }}
        Ok(())
    }}

    fn discard_partial_map(&mut self, range: Range<u64>) -> Result {{
        self.discarded.push(range.clone());
        let l2 = self.ensure_l2();
        let mut va = range.start;
        while va < range.end {{
            let l2_idx = ((va - KERNEL_START) >> UAT_L2_SHIFT) as usize;
            let l3_idx = ((va >> UAT_PGBIT) & (UAT_NPTE as u64 - 1)) as usize;
            let l2e = unsafe {{
                pgtable::with_root_entries(None, l2, l2_idx + 1, |w| w[l2_idx].load(Ordering::Acquire))
            }}?;
            if l2e & 3 == 3 {{
                let l3 = l2e & PA_MASK_42;
                unsafe {{
                    pgtable::with_root_entries(None, l3, l3_idx + 1, |w| {{
                        w[l3_idx].store(0, Ordering::Release);
                    }})
                }}?;
            }}
            va += UAT_PGSZ as u64;
        }}
        Ok(())
    }}
}}

struct Dev;
impl Dev {{
    fn as_ref(&self) -> &Self {{
        self
    }}
}}

struct VmInner {{
    dev: Dev,
    is_kernel: bool,
    va_range: Range<u64>,
    page_table: MockPageTable,
    uat_inner: Arc<MockUatInner>,
    firmware_window: Option<FirmwareWindow>,
}}

impl VmInner {{
    fn mapping_mutation(&mut self) {{}}

    fn map_node_prepared(&mut self, node: &mm::Node<(), KernelMappingInner>, prot: Prot) -> Result {{
        self.page_table
            .map_leaves(node.start()..node.start() + node.mapped_size as u64, prot)
    }}

    fn finish_failed_map(&self, _va: u64, _size: usize) {{}}

    fn tlbi_contexts_naming_root(&self, _va: u64, _size: usize) {{}}

    {map_node}

    {sync_fw_win}
}}

/// Simulates the non-snooping hardware UAT page table walker reading DRAM (`shared_l2_dram` ->
/// `cleaned_pages[l3][leaf_idx]`). Returns `Err(())` (UAT page fault!) if either the L2 entry
/// in DRAM `shared_l2` is missing/invalid or the L3 leaf in DRAM is uncleaned/zero.
fn hw_uat_translate(va: u64) -> core::result::Result<u64, &'static str> {{
    if va < KERNEL_START || va >= KERNEL_END {{
        return Err("outside kernel window");
    }}
    let l2_idx = ((va - KERNEL_START) >> UAT_L2_SHIFT) as usize;
    let l3_idx = ((va >> UAT_PGBIT) & (UAT_NPTE as u64 - 1)) as usize;
    with_ram(|ram| {{
        let l2e = ram.shared_l2_dram[l2_idx];
        if l2e & 3 != 3 {{
            return Err("UAT L2 fault: shared_l2 entry missing in DRAM");
        }}
        let l3_pa = l2e & PA_MASK_42;
        let l3 = ram.cleaned_pages.get(&l3_pa).ok_or("UAT L3 fault: missing page")?;
        let leaf = l3[l3_idx];
        if leaf & 3 != 3 {{
            return Err("UAT L3 fault: leaf missing or uncleaned in DRAM");
        }}
        Ok((leaf & PA_MASK_42) | (va & UAT_PGMSK as u64))
    }})
}}

fn map_block(vm: &mut VmInner, va: u64, size: usize) -> Result {{
    let node = mm::Node {{
        start: va,
        inner: KernelMappingInner {{ mapped_size: size }},
        _m: core::marker::PhantomData,
    }};
    vm.map_node(&node, Prot(0x400))
}}

fn boot_kernel_vm() -> VmInner {{
    init_ram();
    let lower_root = with_ram(|ram| ram.alloc_page());
    let mut vm = VmInner {{
        dev: Dev,
        is_kernel: true,
        va_range: KERNEL_RANGE,
        page_table: MockPageTable::new(),
        uat_inner: Arc::new(MockUatInner {{
            m4_adopted: false,
            failed: AtomicBool::new(false),
        }}),
        firmware_window: None,
    }};
    // Boot allocations: 16 KiB in alloc.gpu, 1 MiB in alloc.shared, 1 MiB in alloc.private,
    // and 1 MMIO table in the last 4 GiB (matching RootV27_2::build).
    map_block(&mut vm, IOVA_KERN_PRIV_START, 1024 * 1024).unwrap();
    map_block(&mut vm, IOVA_KERN_SHARED_START, 1024 * 1024).unwrap();
    map_block(&mut vm, IOVA_KERN_GPU_START, UAT_PGSZ).unwrap();
    map_block(&mut vm, IOVA_KERN_MMIO_START, UAT_PGSZ).unwrap();

    let uat = Uat {{
        shadow_root: vm.page_table.ttb(),
        lower_root,
    }};
    let mut window = FirmwareWindow::new_mock(PA_MASK_42);
    window.mirror(&uat).unwrap();
    window.install().unwrap();
    assert!(window.intact());
    vm.firmware_window = Some(window);
    with_ram(|ram| ram.events.clear());
    vm
}}

// === Bug 2 production code ===
#[derive(Copy, Clone)]
{placement_enum}
{placement_impl}
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
{ga_enum}
{ga_fn}

/// Faithful model of HeapAllocator (alloc.rs) tracking live allocations, garbage queue,
/// backing 1 MiB/64 KiB blocks, and VA reuse.
struct MockHeap {{
    base: u64,
    block_size: usize,
    keep_garbage: bool,
    top: u64,
    blocks: usize,
    free_list: Vec<(u64, usize)>,
    live: BTreeMap<u64, usize>,
    garbage: Vec<(u64, usize)>,
    garbage_bytes: usize,
    total_allocs: usize,
}}

impl MockHeap {{
    fn new(base: u64, block_size: usize, keep_garbage: bool) -> Self {{
        Self {{
            base,
            block_size,
            keep_garbage,
            top: base,
            blocks: 0,
            free_list: Vec::new(),
            live: BTreeMap::new(),
            garbage: Vec::new(),
            garbage_bytes: 0,
            total_allocs: 0,
        }}
    }}

    fn alloc(&mut self, size: usize) -> u64 {{
        self.total_allocs += 1;
        if let Some(pos) = self.free_list.iter().position(|&(_, s)| s >= size) {{
            let (va, s) = self.free_list.swap_remove(pos);
            assert!(self.live.insert(va, size).is_none(), "double-alloc UAF at {{va:#x}}");
            if s > size {{
                self.free_list.push((va + size as u64, s - size));
            }}
            return va;
        }}
        let va = self.top;
        let grow = self.block_size.max(size);
        let aligned = (grow + UAT_PGMSK) & !UAT_PGMSK;
        self.blocks += 1;
        let end = va + aligned as u64;
        self.top = end + UAT_PGSZ as u64; // guard page
        assert!(self.live.insert(va, size).is_none());
        if aligned > size {{
            self.free_list.push((va + size as u64, aligned - size));
        }}
        va
    }}

    fn return_range(&mut self, va: u64, size: usize) {{
        self.free_list.push((va, size));
        self.free_list.sort_by_key(|&(start, _)| start);
        let mut merged: Vec<(u64, usize)> = Vec::with_capacity(self.free_list.len());
        for (start, len) in self.free_list.drain(..) {{
            if let Some(last) = merged.last_mut() {{
                if last.0 + last.1 as u64 == start {{
                    last.1 += len;
                    continue;
                }}
            }}
            merged.push((start, len));
        }}
        self.free_list = merged;
    }}

    fn free(&mut self, va: u64) {{
        let size = self.live.remove(&va).expect("free of unknown VA");
        if self.keep_garbage {{
            self.garbage.push((va, size));
            self.garbage_bytes += size;
        }} else {{
            self.return_range(va, size);
        }}
    }}

    fn garbage(&self) -> (usize, usize) {{
        (self.garbage.len(), self.garbage_bytes)
    }}

    fn collect_garbage(&mut self, count: usize) {{
        let n = count.min(self.garbage.len());
        let drained: Vec<_> = self.garbage.drain(0..n).collect();
        for (va, size) in drained {{
            self.garbage_bytes -= size;
            self.return_range(va, size);
        }}
    }}
}}

struct KernelAllocs {{
    private: MockHeap,
    shared: MockHeap,
    gpu: MockHeap,
    gpu_ro: MockHeap,
    has_device_control: bool,
    held_warnings: usize,
    fw_cache_flushes: usize,
}}

impl KernelAllocs {{
    fn new(has_device_control: bool) -> Self {{
        Self {{
            private: MockHeap::new(IOVA_KERN_PRIV_START, 1024 * 1024, true),
            shared: MockHeap::new(IOVA_KERN_SHARED_START, 1024 * 1024, false),
            gpu: MockHeap::new(IOVA_KERN_GPU_START, 64 * 1024, false),
            gpu_ro: MockHeap::new(0xffffffa6_0000_0000, 1024 * 1024, true),
            has_device_control,
            held_warnings: 0,
            fw_cache_flushes: 0,
        }}
    }}

    /// Mirrors GpuManager::alloc() garbage handling (gpu.rs:1887-1925).
    fn check_gc(&mut self) -> GarbageAction {{
        let p = self.private.garbage();
        let ro = self.gpu_ro.garbage();
        let action = garbage_action(p, ro, self.has_device_control);
        match action {{
            GarbageAction::Keep => {{}}
            GarbageAction::Hold => self.held_warnings += 1,
            GarbageAction::Collect => {{
                self.fw_cache_flushes += 1;
                self.private.collect_garbage(p.0);
                self.gpu_ro.collect_garbage(ro.0);
            }}
        }}
        action
    }}
}}

struct MockBuffer {{
    placement: ParameterListPlacement,
    page_list: u64,
    block_list: u64,
    block_ctl: u64,
    counter: u64,
    info: u64,
    info_in_private: bool,
    kernel_buffer: u64,
    stats: u64,
}}

impl MockBuffer {{
    fn new(allocs: &mut KernelAllocs, placement: ParameterListPlacement) -> Self {{
        allocs.check_gc();
        let page_list = allocs.gpu.alloc(112 * 1024);
        let block_list = allocs.gpu.alloc(64 * 1024);
        let block_ctl = allocs.shared.alloc(0x80);
        let counter = allocs.shared.alloc(0x80);
        let info_in_private = placement.info_in_private();
        let info = if info_in_private {{
            allocs.private.alloc(0x100)
        }} else {{
            allocs.shared.alloc(0x100)
        }};
        let kernel_buffer = allocs.shared.alloc(0x80);
        let stats = allocs.shared.alloc(0x80);
        Self {{
            placement,
            page_list,
            block_list,
            block_ctl,
            counter,
            info,
            info_in_private,
            kernel_buffer,
            stats,
        }}
    }}

    fn destroy(self, allocs: &mut KernelAllocs) {{
        allocs.gpu.free(self.page_list);
        allocs.gpu.free(self.block_list);
        allocs.shared.free(self.block_ctl);
        allocs.shared.free(self.counter);
        if self.info_in_private {{
            allocs.private.free(self.info);
        }} else {{
            allocs.shared.free(self.info);
        }}
        allocs.shared.free(self.kernel_buffer);
        allocs.shared.free(self.stats);
    }}
}}

#[test]
fn t1_boot_mirror_and_attach_covers_initial_tables() {{
    let _vm = boot_kernel_vm();
    assert!(hw_uat_translate(IOVA_KERN_PRIV_START).is_ok());
    assert!(hw_uat_translate(IOVA_KERN_SHARED_START).is_ok());
    assert!(hw_uat_translate(IOVA_KERN_GPU_START).is_ok());
    assert!(hw_uat_translate(IOVA_KERN_MMIO_START).is_ok());
}}

#[test]
fn t2_alloc_gpu_growth_past_32_64_96_mib_boundaries() {{
    let mut vm = boot_kernel_vm();
    // Simulate 500 concurrent render queues consuming 208 KiB each in alloc.gpu:
    // PLST (112 KiB + 16 KiB guard = 128 KiB) + BLST (64 KiB + 16 KiB guard = 80 KiB).
    // Total ~104 MiB -> crosses 32 MiB, 64 MiB, and 96 MiB L2 boundaries!
    let mut top = IOVA_KERN_GPU_START + 2 * UAT_PGSZ as u64; // past boot 16 KiB + guard
    let mut vas = Vec::new();
    for _ in 0..500 {{
        let plst = top;
        map_block(&mut vm, plst, 112 * 1024).unwrap();
        top += 128 * 1024;
        let blst = top;
        map_block(&mut vm, blst, 64 * 1024).unwrap();
        top += 80 * 1024;
        vas.push((plst, blst));
    }}
    assert!(top > IOVA_KERN_GPU_START + 96 * 1024 * 1024);

    // Every single queue's PLST (first & last page) and BLST must translate via DRAM shared_l2!
    for &(plst, blst) in &vas {{
        hw_uat_translate(plst).expect("PLST start page UAT fault");
        hw_uat_translate(plst + 112 * 1024 - UAT_PGSZ as u64).expect("PLST end page UAT fault");
        hw_uat_translate(blst).expect("BLST start page UAT fault");
        hw_uat_translate(blst + 64 * 1024 - UAT_PGSZ as u64).expect("BLST end page UAT fault");
    }}

    // Verify exact PoC clean -> barrier -> shared_l2 clean -> barrier -> TLBI ordering
    // for the 3 new L2 slots (32 MiB, 64 MiB, 96 MiB) and zero redundant work on steady-state slots.
    let events = with_ram(|ram| ram.events.clone());
    let l3_cleans: Vec<usize> = events
        .iter()
        .enumerate()
        .filter_map(|(i, e)| matches!(e, Event::CleanPage(_)).then_some(i))
        .collect();
    assert_eq!(l3_cleans.len(), 3, "expected exactly 3 new L2 tables (32/64/96 MiB)");

    let tlbi_events: Vec<(usize, u8, usize, usize)> = events
        .iter()
        .enumerate()
        .filter_map(|(i, e)| match *e {{
            Event::TlbiRange {{ asid, va, len }} => Some((i, asid, va, len)),
            _ => None,
        }})
        .collect();
    assert_eq!(tlbi_events.len(), 3, "expected 1 TLBI per new L2 table");

    for k in 0..3 {{
        let c = l3_cleans[k];
        // CleanPage(l3) -> PocBarrier -> CleanPoc(shared_l2[idx], 8) -> PocBarrier -> MemSync -> TlbiRange -> MemSync
        assert_eq!(events[c + 1], Event::PocBarrier, "missing PoC barrier after L3 clean");
        assert!(
            matches!(events[c + 2], Event::CleanPoc {{ len: 8, .. }}),
            "missing shared_l2 entry PoC clean: {{:?}}",
            events[c + 2]
        );
        assert_eq!(events[c + 3], Event::PocBarrier, "missing PoC barrier after shared_l2 clean");
        assert_eq!(events[c + 4], Event::MemSync);
        let expected_va = (IOVA_KERN_GPU_START + ((k as u64 + 1) << UAT_L2_SHIFT)) as usize;
        assert_eq!(
            events[c + 5],
            Event::TlbiRange {{
                asid: 0,
                va: expected_va,
                len: 1 << UAT_L2_SHIFT,
            }}
        );
        assert_eq!(events[c + 6], Event::MemSync);
    }}
}}

#[test]
fn t3_alloc_shared_and_straddling_multi_l2_mapping() {{
    let mut vm = boot_kernel_vm();
    // Map a block straddling the 32 MiB boundary in alloc.shared (slot N already mirrored,
    // slot N+1 brand new).
    let straddle = IOVA_KERN_SHARED_START + (1 << UAT_L2_SHIFT) - UAT_PGSZ as u64;
    map_block(&mut vm, straddle, 2 * UAT_PGSZ).unwrap();
    hw_uat_translate(straddle).unwrap();
    hw_uat_translate(straddle + UAT_PGSZ as u64).unwrap();

    // Map a contiguous block spanning slots N+1 (mirrored), N+2 (new), and N+3 (new).
    with_ram(|ram| ram.events.clear());
    let multi_start = IOVA_KERN_SHARED_START + (2 << UAT_L2_SHIFT) - UAT_PGSZ as u64;
    let multi_size = (1 << UAT_L2_SHIFT) + 2 * UAT_PGSZ;
    map_block(&mut vm, multi_start, multi_size).unwrap();
    hw_uat_translate(multi_start).unwrap();
    hw_uat_translate(IOVA_KERN_SHARED_START + (2 << UAT_L2_SHIFT)).unwrap();
    hw_uat_translate(IOVA_KERN_SHARED_START + (3 << UAT_L2_SHIFT)).unwrap();

    // Verify 2 L3 cleans + single 16-byte shared_l2 clean + 64 MiB TLBI.
    let events = with_ram(|ram| ram.events.clone());
    assert_eq!(
        events.iter().filter(|e| matches!(e, Event::CleanPage(_))).count(),
        2
    );
    assert!(events.iter().any(|e| matches!(e, Event::CleanPoc {{ len: 16, .. }})));
    assert!(events.iter().any(|e| matches!(
        e,
        Event::TlbiRange {{
            asid: 0,
            len,
            ..
        }} if *len == (2 << UAT_L2_SHIFT)
    )));
}}

#[test]
fn t4_fail_closed_and_atomic_rollback_on_corruption() {{
    // (a) Corrupted firmware root[2] -> !intact() -> EIO, zero shared_l2 writes, leaves discarded.
    let mut vm = boot_kernel_vm();
    {{
        let win = vm.firmware_window.as_ref().unwrap();
        let root = unsafe {{ &*win.tables.map.ptr().cast::<[AtomicU64; 4]>() }};
        root[2].store(0, Ordering::Release);
    }}
    let va = IOVA_KERN_GPU_START + (1 << UAT_L2_SHIFT);
    assert_eq!(map_block(&mut vm, va, UAT_PGSZ), Err(EIO));
    assert_eq!(vm.page_table.discarded.len(), 1);
    assert!(hw_uat_translate(va).is_err());

    // (b) Unexpected non-zero shared_l2 entry on 2nd slot of a 2-slot new mapping:
    // Phase 1 must fail BEFORE Phase 2 writes the 1st slot (zero partial mutation!).
    let mut vm = boot_kernel_vm();
    let slot_a = ((IOVA_KERN_GPU_START - KERNEL_START) >> UAT_L2_SHIFT) as usize + 1;
    let slot_b = slot_a + 1;
    {{
        let win = vm.firmware_window.as_ref().unwrap();
        win.shared()[slot_b].store(0xdead_0000 | 3, Ordering::Release);
    }}
    let span_va = IOVA_KERN_GPU_START + (1 << UAT_L2_SHIFT);
    let span_len = (1 << UAT_L2_SHIFT) + UAT_PGSZ;
    assert_eq!(map_block(&mut vm, span_va, span_len), Err(EIO));
    let win = vm.firmware_window.as_ref().unwrap();
    assert_eq!(
        win.shared()[slot_a].load(Ordering::Acquire),
        0,
        "partial shared_l2 publication on slot_a despite slot_b validation failure"
    );
    assert_eq!(win.entries[slot_a], 0);
    assert_eq!(vm.page_table.discarded.len(), 1);

    // (c) Out-of-window range fails closed with EINVAL.
    let mut vm = boot_kernel_vm();
    let shadow_root = vm.page_table.ttb();
    assert_eq!(
        vm.firmware_window
            .as_mut()
            .unwrap()
            .sync_range(shadow_root, KERNEL_RANGE, 0x1000..0x5000),
        Err(EINVAL)
    );
    // Empty range is a safe no-op.
    assert_eq!(
        vm.firmware_window
            .as_mut()
            .unwrap()
            .sync_range(shadow_root, KERNEL_RANGE, IOVA_KERN_GPU_START..IOVA_KERN_GPU_START),
        Ok(false)
    );
}}

#[test]
fn t4b_existing_shadow_slot_missing_or_replaced_shared_descriptor_fails_closed() {{
    // (a) Existing mirrored slot whose `shared_l2` descriptor was cleared to 0 while
    // `intact()` remains true and `entries[slot] == shadow[slot]`: must fail closed with
    // EIO and discard the partial map rather than returning Ok(false).
    let mut vm = boot_kernel_vm();
    let slot_existing = ((IOVA_KERN_GPU_START - KERNEL_START) >> UAT_L2_SHIFT) as usize;
    let orig_desc = {{
        let win = vm.firmware_window.as_ref().unwrap();
        assert!(win.intact());
        assert_ne!(win.entries[slot_existing], 0);
        assert_eq!(
            win.shared()[slot_existing].load(Ordering::Acquire),
            win.entries[slot_existing]
        );
        win.shared()[slot_existing].store(0, Ordering::Release);
        win.entries[slot_existing]
    }};
    let va_same_slot = IOVA_KERN_GPU_START + 2 * UAT_PGSZ as u64;
    assert_eq!(map_block(&mut vm, va_same_slot, UAT_PGSZ), Err(EIO));
    assert_eq!(vm.page_table.discarded.len(), 1);
    assert_eq!(
        vm.firmware_window.as_ref().unwrap().shared()[slot_existing].load(Ordering::Acquire),
        0
    );

    // (b) Existing mirrored slot whose `shared_l2` descriptor was replaced with a different
    // valid descriptor: must also fail closed with EIO.
    {{
        let win = vm.firmware_window.as_ref().unwrap();
        win.shared()[slot_existing].store(0xdead_0000 | 3, Ordering::Release);
    }}
    let va_same_slot_2 = IOVA_KERN_GPU_START + 4 * UAT_PGSZ as u64;
    assert_eq!(map_block(&mut vm, va_same_slot_2, UAT_PGSZ), Err(EIO));
    assert_eq!(vm.page_table.discarded.len(), 2);

    // (c) Multi-slot straddling mapping covering a new slot followed by an existing slot
    // with a missing `shared_l2` descriptor: Phase 1 must reject before Phase 2 publishes
    // the new slot (zero partial mutation!).
    let mut vm = boot_kernel_vm();
    let slot_base = ((IOVA_KERN_GPU_START - KERNEL_START) >> UAT_L2_SHIFT) as usize;
    let slot_new = slot_base + 1;
    let slot_after = slot_base + 2;
    // Mirror `slot_after` first so `slot_new` stays unmirrored (0) while `slot_after` is
    // an existing mirrored slot.
    let va_after = IOVA_KERN_GPU_START + (2 << UAT_L2_SHIFT) + 2 * UAT_PGSZ as u64;
    map_block(&mut vm, va_after, UAT_PGSZ).unwrap();
    {{
        let win = vm.firmware_window.as_ref().unwrap();
        assert_eq!(win.entries[slot_new], 0);
        assert_ne!(win.entries[slot_after], 0);
        win.shared()[slot_after].store(0, Ordering::Release);
    }}
    let straddle_new_then_existing = IOVA_KERN_GPU_START + (2 << UAT_L2_SHIFT) - UAT_PGSZ as u64;
    assert_eq!(
        map_block(&mut vm, straddle_new_then_existing, 2 * UAT_PGSZ),
        Err(EIO)
    );
    let win = vm.firmware_window.as_ref().unwrap();
    assert_eq!(
        win.shared()[slot_new].load(Ordering::Acquire),
        0,
        "partial shared_l2 publication on slot_new despite missing shared descriptor on slot_after"
    );
    assert_eq!(win.entries[slot_new], 0);
    assert_eq!(vm.page_table.discarded.len(), 1);
    let _ = orig_desc;
}}

#[test]
fn t5_non_kernel_vm_skips_firmware_window() {{
    let mut vm = boot_kernel_vm();
    vm.is_kernel = false;
    let va = IOVA_KERN_GPU_START + (1 << UAT_L2_SHIFT);
    assert_eq!(vm.sync_firmware_window(va..va + UAT_PGSZ as u64), Ok(()));
    assert!(with_ram(|ram| ram.events.is_empty()));
}}

#[test]
fn t6_g14_v27_2_queue_churn_never_leaks_alloc_private_or_alloc_shared() {{
    let mut allocs = KernelAllocs::new(false);
    // Simulate boot-time static allocations in alloc.private (retained for life of boot).
    let _boot_root = allocs.private.alloc(0x8000);
    let boot_priv_blocks = allocs.private.blocks;
    let boot_priv_allocs = allocs.private.total_allocs;

    // 5,000 render queue create/destroy cycles (> 2x MAX_FW_ALLOC_GARBAGE_OBJECTS = 2048).
    for _ in 0..5000 {{
        let buf = MockBuffer::new(&mut allocs, ParameterListPlacement::KernelGpu);
        // Simulate render scene + scene27 + Binding::info in alloc.shared, then retire.
        let scene = allocs.shared.alloc(0x80);
        let scene27 = allocs.shared.alloc(0x80);
        let binding_info = allocs.shared.alloc(0x100);
        allocs.shared.free(scene);
        allocs.shared.free(scene27);
        allocs.shared.free(binding_info);
        buf.destroy(&mut allocs);
    }}
    assert_eq!(allocs.check_gc(), GarbageAction::Keep);
    assert_eq!(allocs.held_warnings, 0, "alloc.private tripped GarbageAction::Hold");
    assert_eq!(allocs.private.garbage(), (0, 0), "alloc.private accumulated garbage");
    assert_eq!(allocs.private.blocks, boot_priv_blocks, "alloc.private grew backing blocks");
    assert_eq!(allocs.private.total_allocs, boot_priv_allocs, "runtime touched alloc.private");
    assert_eq!(allocs.shared.blocks, 1, "alloc.shared failed to reuse freed nodes");
    assert_eq!(allocs.gpu.blocks, 2, "alloc.gpu failed to reuse freed PLST/BLST nodes");
}}

#[test]
fn t7_no_premature_free_while_render_flight_active() {{
    let mut allocs = KernelAllocs::new(false);
    let buf = MockBuffer::new(&mut allocs, ParameterListPlacement::KernelGpu);
    let inflight_vas = [
        buf.page_list,
        buf.block_list,
        buf.block_ctl,
        buf.counter,
        buf.info,
        buf.kernel_buffer,
        buf.stats,
    ];
    let scene = allocs.shared.alloc(0x80);
    let binding_info = allocs.shared.alloc(0x100);

    // Queue is destroyed by userspace while RenderFlight is STILL in flight:
    // Buffer + Binding::info must remain owned by RenderFlight until TA && FS both retire!
    let mut flight_buf = Some(buf);
    let mut flight_scene = Some(scene);
    let mut flight_binding = Some(binding_info);

    if {premature_free_mutant} {{
        if let Some(b) = flight_buf.take() {{
            b.destroy(&mut allocs);
        }}
    }}

    // While flight is active, allocate another queue and prove ZERO VA collision (no UAF!).
    let other = MockBuffer::new(&mut allocs, ParameterListPlacement::KernelGpu);
    for &va in &inflight_vas {{
        assert!(
            allocs.gpu.live.contains_key(&va) || allocs.shared.live.contains_key(&va),
            "in-flight buffer VA {{va:#x}} freed prematurely before retirement (UAF!)"
        );
        assert_ne!(other.info, va);
        assert_ne!(other.page_list, va);
        assert_ne!(other.block_list, va);
    }}
    other.destroy(&mut allocs);

    // Now TA + FS both retire -> RenderFlight drops -> queue's allocations are reclaimed.
    allocs.shared.free(flight_scene.take().unwrap());
    allocs.shared.free(flight_binding.take().unwrap());
    flight_buf.take().unwrap().destroy(&mut allocs);
    assert!(allocs.shared.live.is_empty());
    assert!(allocs.gpu.live.is_empty());
    assert_eq!(allocs.private.garbage(), (0, 0));

    // Subsequent queue reuses the reclaimed VAs cleanly.
    let reused = MockBuffer::new(&mut allocs, ParameterListPlacement::KernelGpu);
    assert_eq!(allocs.shared.blocks, 1);
    reused.destroy(&mut allocs);
}}

#[test]
fn t8_legacy_vm_private_preserves_private_placement_and_gc_flush() {{
    let mut allocs = KernelAllocs::new(true);
    for _ in 0..(MAX_FW_ALLOC_GARBAGE_OBJECTS + 16) {{
        let buf = MockBuffer::new(&mut allocs, ParameterListPlacement::VmPrivate);
        assert!(buf.info_in_private);
        buf.destroy(&mut allocs);
    }}
    allocs.check_gc();
    assert!(allocs.fw_cache_flushes >= 1, "legacy device-control path must flush & collect");
    assert!(allocs.private.garbage().0 <= MAX_FW_ALLOC_GARBAGE_OBJECTS);
}}
'''


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path)
    ap.add_argument('--mutation', choices=MUTATIONS)
    args = ap.parse_args()

    src = build_source(args.mutation)
    tmp_ctx = None if args.out else tempfile.TemporaryDirectory(prefix='g14-v27-2-fw-window-gc-')
    work = args.out if args.out else Path(tmp_ctx.name)
    work.mkdir(parents=True, exist_ok=True)
    try:
        rs = work / 'harness.rs'
        exe = work / 'harness'
        rs.write_text(src)
        rustc = shlex.split(os.environ.get('RUSTC', 'rustc'))
        subprocess.run(
            rustc + [
                '--edition=2021',
                '--test',
                '--cfg', 'CONFIG_DRM_ASAHI_G14_V27_2',
                str(rs),
                '-o', str(exe),
            ],
            check=True,
        )
        run = subprocess.run([str(exe), '--test-threads=1'], check=False)
        return run.returncode
    finally:
        if tmp_ctx is not None:
            tmp_ctx.cleanup()


if __name__ == '__main__':
    sys.exit(main())
