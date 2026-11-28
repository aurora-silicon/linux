#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host-check the G16G ray-tracing command set (RENDER_RAY_QUERY, BOUNDED_AS_BUILD).

Compiles the production g16/rt.rs, recovery/logic.rs and the TA/FS/compute encoders, plus the
command admission and engine conversion functions copied verbatim from command.rs and
engine.rs, against CPU-only stand-ins. No GPU, firmware or Mesa is involved.

Failure modes this guards against (written before the code):

 1. A non-RT command changes bytes: the engine's conversion of an empty ray state is not the
    disabled word set, or the disabled set differs from the words G16G always wrote (the frozen
    golden fixtures then cover the rest).
 2. RT words land in the wrong register record, the register counts change, a gated register
    (0x1a141, render 0x017d9) is written while its gate is off, or a ray-query compute item is
    not bracketed by 0x017e1 = 1 first and 0x017d9 = 1 last, or its tail is not 0x2c, as macOS
    27.2 writes them on the J773g; or a start command's SCS flag (StartCompute +0x40, StartTA
    +0x60, StartFS +0x78) differs from whether the item's register list enables SCS, which
    leaves a running ray-query dispatch exposed to the firmware's idle SCS teardown.
 3. The fragment window word loses bit 9; a render stage's tail word takes the auxiliary
    intersection word (the M4 Pro's render programs use the scratch tail).
 4. SCS sizing differs from the M4 Pro's formula ((pages + pages/256 + 8) x cores pages), is not
    rounded to whole 2-MiB rows, overflows silently, or admits a pool whose pages and freelist do
    not fit an SCS pool window (E2BIG); zero cores is accepted.
 5. A render whose scratch exceeds its queue's reservation is accepted, or one within it
    refused; an unmapped intersection pool or default program is accepted.
 6. The long allowance misses a ray/AS-build job, applies to plain work, re-enables a disabled
    (zero) deadline, or shortens a deadline already longer than 10 s.
 7. A compute flag other than AS_BUILD is accepted; scratch alone counts as ray traversal.
 8. The feature word loses a bit or gains another than RENDER_RAY_QUERY and BOUNDED_AS_BUILD.
 9. A UMA manager image or compute descriptor reports seeds that are not whole rows inside the
    freelist, a freelist (+0x1c) that does not hold its pool or is smaller than a base one, an
    SCS pool's freelist other than macOS's 0x80000 records, or row words (+0x48) other than 8
    per valid row.
10. Mesa's G16G ray-query commands (layout 0x0130000060010070 on 10 cores, compute and either or
    both render stages, and their QUEUE_CREATE reservations) are refused, which Mesa reports as
    VK_ERROR_DEVICE_LOST; the render pools a queue creates cannot take a queue that later
    reserves the other stage too; or a compute pool is created for fewer than two dispatches
    where a window holds two.
"""
from common import kernel_root, output_dir, rustc, flat_bindings
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import tempfile

ROOT = kernel_root()
OUT = output_dir()
BACKEND = ROOT / 'drivers/gpu/drm/asahi/g16'
FLAT = flat_bindings()


def function(text, name):
    marker = text.index('fn ' + name + '(')
    start = text.rfind('\n', 0, marker) + 1
    at = text.index('{', marker)
    depth, end = 1, at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def block(text, declaration):
    start = text.index(declaration)
    at = text.index('{', start)
    depth, end = 1, at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


command = (BACKEND / 'command.rs').read_text()
engine = (BACKEND / 'engine.rs').read_text()
admission = '\n'.join(function(command, name) for name in
                      ['check_intersection', 'check_render_ray', 'check_compute_ray'])
reserve = function((BACKEND / 'queue.rs').read_text(), 'reserve_render_scs')
conversion = '\n'.join(function(engine, name) for name in ['compute_rt', 'stage_rt'])
# The client address map, which bounds SCS pools (SCS_POOL_WINDOW).
client = block((BACKEND / 'hw.rs').read_text(), 'pub(crate) mod client {')
long = block(command, 'impl Command {')
for text in ('admission', 'conversion', 'long'):
    globals()[text] = globals()[text].replace('super::rt::', 'rt::')

# The feature word: the bits params.rs adds, from the real header, plus the shared
# VM_STATUS and VM_STATUS_MIRROR of file.rs.
params = (BACKEND / 'params.rs').read_text()
statement = params[params.index('params.features |='):]
statement = statement[:statement.index(';')]
names = re.findall(r'drm_asahi_feature_(DRM_ASAHI_FEATURE_\w+)', statement)
with tempfile.TemporaryDirectory(prefix='m4-rt-features-') as name:
    directory = pathlib.Path(name)
    source = directory / 'features.c'
    header = ROOT / 'include/uapi/drm/asahi_drm.h'
    source.write_text('#include <stdio.h>\n#include ' + json.dumps(str(header)) + '\nint main(void){printf("%#llx\\n",'
                      '(unsigned long long)(' + ' | '.join(names + ['DRM_ASAHI_FEATURE_VM_STATUS',
                      'DRM_ASAHI_FEATURE_VM_STATUS_MIRROR']) + '));return 0;}\n')
    binary = directory / 'features'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Werror', '-D__user=',
                   '-idirafter', str(header.parents[1]), str(source), '-o', str(binary)], check=True)
    features = int(subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout, 16)
assert features == 0x4001fc02, hex(features)
assert sorted(names) == sorted(['DRM_ASAHI_FEATURE_PER_COMMAND_INPUT_SYNCS', 'DRM_ASAHI_FEATURE_EXACT_PRIOR_BARRIERS',
                                'DRM_ASAHI_FEATURE_FRAGMENT_BARRIERS', 'DRM_ASAHI_FEATURE_COMPUTE_NO_QUERY_ACCESS',
                                'DRM_ASAHI_FEATURE_FEW_PRIMITIVES', 'DRM_ASAHI_FEATURE_RENDER_RAY_QUERY',
                                'DRM_ASAHI_FEATURE_BOUNDED_AS_BUILD']), names
print(f'feature word {features:#x} (RENDER_RAY_QUERY | BOUNDED_AS_BUILD added to G16G features)')

harness = r'''
#![allow(dead_code, non_camel_case_types, non_snake_case, non_upper_case_globals, unused_imports)]
extern crate self as kernel;
pub mod prelude {
    pub type Result<T = ()> = core::result::Result<T, i32>;
    pub const EINVAL: i32 = -22; pub const E2BIG: i32 = -7; pub const EOVERFLOW: i32 = -75;
}
use prelude::*;
const ENOENT: i32 = -2;
mod uapi { include!("@FLAT@"); }
mod hw {
    pub mod engine { pub const COMPUTE_SEED: u32 = 0x100; }
    pub mod sizes { pub const BASE_RENDER_SLOTS: usize = 2; }
    @CLIENT@
}
mod fw {
    #[path = "@BACKEND@/fw/graph.rs"] pub mod graph;
    #[path = "@BACKEND@/fw/work.rs"] pub mod work;
}
#[path = "@BACKEND@/encode.rs"] mod encode;
#[path = "@BACKEND@/rt.rs"] mod rt;
#[path = "@BACKEND@/recovery/logic.rs"] mod logic;
use encode::{ComputeRt, StageRt};
use fw::work::{compute, fs, ta};

mod mmu { pub struct Vm { pub mapped: Vec<(u64, u64)> } }
/// Readable mappings only; the production span rules have their own tests.
fn cover(vm: &mmu::Vm, start: u64, end: u64, _read: bool, _write: bool) -> Result {
    if vm.mapped.iter().any(|&(a, b)| a <= start && end <= b) { Ok(()) } else { Err(ENOENT) }
}
struct SpanBatch;
impl SpanBatch {
    fn push(&mut self, vm: &mmu::Vm, start: u64, end: u64, read: bool, write: bool) -> Result {
        cover(vm, start, end, read, write)
    }
}
struct Context<'a> { vm: &'a mmu::Vm, usc_base: u64, active_cores: u32, render_scs: [u16; 2] }
mod queue_test {
    use super::*;
    struct Config { supports_render_scs: bool }
    struct Queue { cfg: Config, accepted: [u64; 2], active_cores: u32, render_scs: [u16; 2] }
    const EBUSY: i32 = -16;
    struct Error;
    impl Error { fn from_errno(code: i32) -> i32 { code } }
    impl Queue { @RESERVE@ }

    #[test]
    fn reserve_scratch() {
        let mut q = Queue {
            cfg: Config { supports_render_scs: false },
            accepted: [0; 2], active_cores: 10, render_scs: [0; 2],
        };
        let (layout, pages) = LAYOUTS[0];
        assert_eq!(q.reserve_render_scs([0, 0]), Ok(()));
        for request in [[layout, 0], [0, layout]] {
            assert_eq!(q.reserve_render_scs(request), Err(-95));
            assert_eq!(q.render_scs, [0, 0]);
        }
        q.cfg.supports_render_scs = true;
        assert_eq!(q.reserve_render_scs([layout, 0]), Ok(()));
        assert_eq!(q.render_scs, [pages, 0]);
        q.accepted = [1, 0];
        assert_eq!(q.reserve_render_scs([0, 0]), Err(EBUSY));
        assert_eq!(q.render_scs, [pages, 0]);
    }
}
pub mod bindings { pub const EOPNOTSUPP: u32 = 95; }
@ADMISSION@
@CONVERSION@
type KBox<T> = Box<T>;
struct RenderCommand { ray_query: [rt::RayQueryStage; 2], scs: rt::ScsSeeds }
struct ComputeCommand { ray: rt::ComputeRay, scs: rt::ScsSeeds, as_build: bool }
enum Command { Render(KBox<RenderCommand>), Compute(KBox<ComputeCommand>) }
@LONG@

// Mesa's three T1 layouts: depth 112, 96 cliques, driver 40, 88 and 304 bytes.
const LAYOUTS: [(u64, u16); 3] = [(0x0028000060000070, 791), (0x0058000060010070, 827), (0x0130000060010070, 989)];
const POOL: u64 = 0x10000;
fn tables() -> rt::IntersectionTables {
    rt::IntersectionTables::new(POOL, 0x4000, 1, 112, 0x10010020000, 0x16000000000).unwrap()
}
fn ray(layout: u64) -> rt::ComputeRay {
    rt::ComputeRay::new(layout, 0, 0, POOL, 0x4000, 1, 112, 0x10010020000, 0x16000000000).unwrap()
}
fn stage(layout: u64) -> rt::RayQueryStage {
    rt::RayQueryStage { intersection: tables(), scratch: rt::ScratchRequest::new(layout).unwrap() }
}
fn records(w: &[u8], at: usize, count: usize) -> Vec<(u32, u64)> {
    (0..count).map(|i| {
        let r = &w[at + i * 12..at + i * 12 + 12];
        (u32::from_le_bytes(r[..4].try_into().unwrap()), u64::from_le_bytes(r[4..].try_into().unwrap()))
    }).collect()
}
fn u16_at(w: &[u8], at: usize) -> usize { u16::from_le_bytes([w[at], w[at + 1]]) as usize }
fn u32_at(w: &[u8], at: usize) -> u32 { u32::from_le_bytes(w[at..at + 4].try_into().unwrap()) }

#[test]
fn empty_ray_state_converts_to_the_words_g16g_always_wrote() {
    assert_eq!(compute_rt(&rt::ComputeRay::NONE, 8192), ComputeRt::DISABLED);
    assert_eq!(stage_rt(&rt::RayQueryStage::NONE), StageRt::DISABLED);
    // Payload-free layouts and table-less commands are the same empty state.
    let quiet = rt::ComputeRay::new(0x0000_0000_6000_0070, 0, 0, 0, 0, 0, 0, 0, 0).unwrap();
    assert_eq!(compute_rt(&quiet, 8192), ComputeRt::DISABLED);
    assert_eq!(StageRt::DISABLED.window | 1 << 9, 0xff0200);
    assert!(!encode::compute_encoder_gate());
}

fn compute_refs() -> encode::ComputeRefs {
    let f = 0xfffffc2000000000u64; let g = 0x1000000000u64;
    encode::ComputeRefs {
        notifier: f + 0x600400, qinfo: f + 0x200000, user_stamp: f + 0x600648, fw_stamp: f + 0x600640,
        work: f + 0x100000, work_gva: g + 0x100000, seq: f + 0x100900, descriptor_fw: 0, preempt_gva: g + 0x200000,
        its: f + 0x1008c0, stats_fw: f + 0x300000, auxiliary_fw: f + 0x400000, auxiliary_gva: g + 0x400000,
        context_store_gva: g + 0x500000, counter_fw: f + 0x500000, pool_state_gva: g + 0x600000,
        page_list_gva: g + 0x700000, usc_base: 1 << 40, record_fw: f + 0x700000, binding_generation: 1,
        uma_slot: 1, event: compute::EVENT, command_id: 1, compute_index: 0, work_818: 0, target_stamp: 0x100, submitted: 1,
    }
}

fn encode_compute(layout: &compute::Layout, rt_words: &ComputeRt) -> core::result::Result<[u8; compute::WORK_SIZE], encode::ComputeEncodeError> {
    let c = uapi::drm_asahi_cmd_compute { cdm_ctrl_stream_base: 0x1000300000, cdm_ctrl_stream_end: 0x1000300200, ..Default::default() };
    let mut w = [0xa5; compute::WORK_SIZE]; let mut q = [0xa5; compute::SEQ_SIZE];
    encode::encode_compute_rt(layout, &compute_refs(), &c, &[], &[0; 2], 1, 1, 1, rt_words, &mut w, &mut q)?;
    Ok(w)
}

#[test]
fn compute_rt_words_land_in_the_25f84_register_slots_only() {
    for layout in [&compute::V25G83, &compute::V26B123] {
        let plain = encode_compute(layout, &ComputeRt::DISABLED).unwrap();
        assert_eq!(u16_at(&plain, 0x728), 25);
        let plain_regs = records(&plain, compute::REGS, 25);
        let d = compute::DESCRIPTOR;
        // A base unit's descriptor: 16384 records, 8192 seeds, 16 rows x 8.
        assert_eq!((u32_at(&plain, d + 0x1c), u32_at(&plain, d + 0x24), u32_at(&plain, d + 0x2c), u32_at(&plain, d + 0x48)),
            (16384, 8192, 8192, 128));
        for (layout_word, pages) in LAYOUTS {
            let words = compute_rt(&ray(layout_word), 28672);
            let w = encode_compute(layout, &words).unwrap();
            // macOS 27.2 on the J773g: 0x017e1 = 1 first, 0x017d9 = 1 last.
            assert_eq!((u16_at(&w, 0x728), u16_at(&w, 0x72a)), (27, 27 * 12));
            let got = records(&w, compute::REGS, 27);
            assert_eq!((got[0], got[26]), ((0x017e1, 1), (0x017d9, 1)));
            assert!(got.iter().all(|&(sel, _)| sel != 0x1a141), "gated register");
            let expect = |sel: u32| got.iter().find(|r| r.0 == sel).unwrap().1;
            let mib = u64::from(pages).div_ceil(256);
            assert_eq!(expect(0x101d1), u64::from(pages) | mib << 16);
            assert_eq!((expect(0xd479), expect(0x1a0e9), expect(0x107a1)), (0x2c, 112, 1 << 24 | 0x60 << 16));
            assert_eq!([expect(0x1a089), expect(0x1a091), expect(0x1a059), expect(0x1a061), expect(0x1a0b9), expect(0x1a0c1)],
                [POOL + 128, 0xc0003800, 0x6f00000000, 0x3feff, 0x10010020000, 0x16000000000]);
            // An SCS pool's descriptor: the 0x80000-record freelist, 56 rows x 8.
            assert_eq!((u32_at(&w, d + 0x1c), u32_at(&w, d + 0x24), u32_at(&w, d + 0x2c), u32_at(&w, d + 0x48)),
                (0x80000, 28672, 28672, 448));
            // Between the two records the list is the plain one, only the ten RT words differ.
            let rt_sels = [0x1a089, 0x1a091, 0x1a059, 0x1a061, 0x1a0b9, 0x1a0c1, 0x101d1, 0xd479, 0x1a0e9, 0x107a1];
            for (&(sel, value), &(plain_sel, plain_value)) in got[1..26].iter().zip(plain_regs.iter()) {
                assert_eq!(sel, plain_sel);
                assert!(value == plain_value || rt_sels.contains(&sel), "{sel:#x}");
            }
            // The embedded StartCompute marks the slot as an SCS user, as macOS 27.2 does on
            // every J773g compute item that opens with 0x017e1 (and on no other).
            let flag = compute::SEQ_EMBEDDED + compute::START_SCS;
            assert_eq!((u32_at(&plain, flag), u32_at(&w, flag)), (0, 1));
            // Nothing else moves: the register list, its count, the pool's descriptor words and
            // the start command's SCS flag.
            let regs = compute::REGS..compute::REGS + 27 * 12;
            let pool_words = [0x1c, 0x24, 0x2c, 0x48];
            for (i, (a, b)) in plain.iter().zip(w.iter()).enumerate() {
                if a != b {
                    assert!(regs.contains(&i) || (0x728..0x72c).contains(&i) || (flag..flag + 4).contains(&i)
                        || pool_words.iter().any(|&o| (d + o..d + o + 4).contains(&i)), "byte {i:#x}");
                }
            }
        }
        // Seeds are whole rows within the freelist, which is never smaller than a base one.
        for seeds in [0, 8191, 8704 + 64, 16896] {
            let words = ComputeRt { seeds, ..ComputeRt::DISABLED };
            assert!(encode_compute(layout, &words).is_err(), "seeds {seeds}");
        }
        assert!(encode_compute(layout, &ComputeRt { seeds: 8704, ..ComputeRt::DISABLED }).is_ok());
        assert!(encode_compute(layout, &ComputeRt { seeds: 8192, records: 8192, ..ComputeRt::DISABLED }).is_err());
        // The pool's size moves only its seed counts and row words.
        let small = encode_compute(layout, &compute_rt(&ray(LAYOUTS[2].0), 18432)).unwrap();
        let large = encode_compute(layout, &compute_rt(&ray(LAYOUTS[2].0), rt::MAX_SCS_SEEDS)).unwrap();
        assert_eq!((u32_at(&small, d + 0x1c), u32_at(&small, d + 0x2c), u32_at(&small, d + 0x48)), (0x80000, 18432, 288));
        assert_eq!((u32_at(&large, d + 0x1c), u32_at(&large, d + 0x2c), u32_at(&large, d + 0x48)), (0x80000, 31744, 496));
        for (i, (a, b)) in small.iter().zip(large.iter()).enumerate() {
            if a != b { assert!([0x24, 0x2c, 0x48].iter().any(|&o| (d + o..d + o + 4).contains(&i)), "byte {i:#x}"); }
        }
    }
}

fn render_refs() -> (encode::RenderRefs, encode::PassRefs, uapi::drm_asahi_cmd_render) {
    let f = 0xfffffc2000000000u64; let g = 0x1000000000u64;
    let mut u = uapi::drm_asahi_cmd_render::default();
    u.width_px = 64; u.height_px = 64; u.layers = 1; u.samples = 1; u.utile_width_px = 32; u.utile_height_px = 32;
    u.sample_size_B = 4; u.vdm_ctrl_stream_base = g + 0xa00000; u.isp_scissor_base = g + 0xb00000;
    let r = encode::RenderRefs {
        notifier: f + 0x800000, qinfo: [f + 0xf00000, f + 0x1000000], user_stamp: [f + 0x800620, f + 0x800630],
        fw_stamp: [f + 0x4000000, f + 0x4004000], uma_mgr: [f + 0x300000; 2], uma_aux: [f + 0x300100, f + 0x300200],
        uma_gpu: g + 0x6000000, pb_mgr: f + 0x3000000, scene_list: g + 0x500000, parameter_id: 1, generation: 1,
        activity: [f + 0x900004, f + 0xa00008], counter: [2, 1], uma_submitted: 2, uuid: 0x123400, usc_base: 1 << 40,
        ..Default::default()
    };
    let p = encode::PassRefs {
        ta_work: f + 0xb00000, ta_work_gva: g + 0x7000000, fs_work: f + 0xc00000, fs_work_gva: g + 0x8000000,
        ta_seq: f + 0xd00000, fs_seq: f + 0xe00000, scene: f + 0x100000, tpc_gva: g + 0x200000, tpc_fw: f + 0x3200000,
        tilemap_gva: g + 0x100000, heap_gva: g + 0x300000, scene_user_gva: g + 0x900000, scratch_fw: f + 0x200000,
        pre_gva: [g + 0x600000, g + 0x700000, g + 0x800000], aux_gva: g + 0xb00000, ..Default::default()
    };
    (r, p, u)
}

#[test]
fn render_stage_words_land_in_the_ta_and_fs_register_slots_only() {
    let (r, p, u) = render_refs();
    let geo = encode::Geometry::new(&u, p.heap_gva);
    let ta_rt = |words: &StageRt, layout: &ta::Layout| {
        let mut w = [0xa5; ta::WORK_SIZE]; let mut q = [0xa5; ta::SEQ_SIZE];
        encode::encode_ta_rt(layout, &r, &p, &u, &[0; 4], 1, 0x100, &geo, None, words, &mut w, &mut q); (w, q)
    };
    let fs_rt = |words: &StageRt, layout: &fs::Layout| {
        let mut w = [0xa5; fs::WORK_SIZE]; let mut q = [0xa5u8; fs::SEQ_SIZE];
        encode::encode_fs_rt(layout, &r, &p, &u, &[0; 4], 1, 1 << 40, 1, 0x100, &geo, words, &mut w, &mut q); (w, q)
    };
    for (tl, fl) in [(&ta::V25G83, &fs::V25G83), (&ta::V26B123, &fs::V26B123)] {
        let (ta_plain, taq_plain) = ta_rt(&StageRt::DISABLED, tl);
        let (fs_plain, fsq_plain) = fs_rt(&StageRt::DISABLED, fl);
        for (layout_word, pages) in LAYOUTS {
            let mut s = stage(layout_word);
            // The auxiliary word is validated, not written: the tail stays the scratch tail.
            let mut wire = [0u8; 48];
            wire[0..4].copy_from_slice(&16u32.to_le_bytes());
            wire[8..16].copy_from_slice(&POOL.to_le_bytes()); wire[16..24].copy_from_slice(&0x4000u64.to_le_bytes());
            wire[24..28].copy_from_slice(&1u32.to_le_bytes()); wire[28..32].copy_from_slice(&112u32.to_le_bytes());
            wire[32..40].copy_from_slice(&0x10010020000u64.to_le_bytes()); wire[40..48].copy_from_slice(&0x16000000000u64.to_le_bytes());
            s.intersection = rt::IntersectionTables::parse(&wire).unwrap();
            let words = stage_rt(&s);
            let size = u64::from(pages) | u64::from(pages).div_ceil(256) << 16;
            let window = 1 << 24 | 0x60 << 16;
            let ift = [POOL + 128, 0xc0003800, 0x6f00000000, 0x3feff, 0x10010020000, 0x16000000000];
            let (w, q) = ta_rt(&words, tl);
            // StartTA marks the slot as an SCS user; nothing else in the sequence moves.
            assert_eq!((u32_at(&taq_plain, ta::START_SCS), u32_at(&q, ta::START_SCS)), (0, 1));
            for (i, (a, b)) in taq_plain.iter().zip(q.iter()).enumerate() {
                assert!(a == b || (ta::START_SCS..ta::START_SCS + 4).contains(&i), "TA seq byte {i:#x}");
            }
            assert_eq!((u16_at(&w, 0x748), u16_at(&w, 0x74a)), (68, 68 * 12));
            let ta_plain_regs = records(&ta_plain, ta::REGS, 66);
            let got = records(&w, ta::REGS, 68);
            assert_eq!((got[0], got[67]), ((0x017e1, 1), (0x017d9, 1)));
            let value = |got: &Vec<(u32, u64)>, sel: u32| got.iter().find(|r| r.0 == sel).unwrap().1;
            let ta_rt_sels = [0x1a099, 0x1a0a1, 0x1a069, 0x1a071, 0x1a0c9, 0x1a0d1, 0x101c9, 0xd471, 0x1a0f1, 0x10799];
            assert_eq!([0x1a099, 0x1a0a1, 0x1a069, 0x1a071, 0x1a0c9, 0x1a0d1].map(|s| value(&got, s)), ift);
            assert_eq!([0x101c9, 0xd471, 0x1a0f1, 0x10799].map(|s| value(&got, s)), [size, 0x2c, 112, window]);
            for (&(sel, val), &(psel, pval)) in got[1..67].iter().zip(ta_plain_regs.iter()) {
                assert_eq!(sel, psel);
                assert!(val == pval || ta_rt_sels.contains(&sel), "TA {sel:#x}");
            }
            for (i, (a, b)) in ta_plain.iter().zip(w.iter()).enumerate() {
                if a != b {
                    assert!((ta::REGS..ta::REGS + 68 * 12).contains(&i) || (0x748..0x74c).contains(&i), "TA byte {i:#x}");
                }
            }
            let (w, q) = fs_rt(&words, fl);
            // StartFS marks the slot as an SCS user; nothing else in the sequence moves.
            assert_eq!((u32_at(&fsq_plain, fs::START_SCS), u32_at(&q, fs::START_SCS)), (0, 1));
            for (i, (a, b)) in fsq_plain.iter().zip(q.iter()).enumerate() {
                assert!(a == b || (fs::START_SCS..fs::START_SCS + 4).contains(&i), "FS seq byte {i:#x}");
            }
            assert_eq!((u16_at(&w, 0x788), u16_at(&w, 0x78a)), (84, 84 * 12));
            let fs_plain_regs = records(&fs_plain, fs::REGS, 82);
            let got = records(&w, fs::REGS, 84);
            assert_eq!((got[0].0, got[1], got[83]), (0x1739, (0x017e1, 1), (0x017d9, 1)));
            let fs_rt_sels = [0x1a0a9, 0x1a0b1, 0x1a079, 0x1a081, 0x1a0d9, 0x1a0e1, 0x101c1, 0xd469, 0x1a0f9, 0x10791];
            assert_eq!([0x1a0a9, 0x1a0b1, 0x1a079, 0x1a081, 0x1a0d9, 0x1a0e1].map(|s| value(&got, s)), ift);
            assert_eq!([0x101c1, 0xd469, 0x1a0f9, 0x10791].map(|s| value(&got, s)), [size, 0x2c, 112, window | 1 << 9]);
            assert_eq!(got[0], fs_plain_regs[0]);
            for (&(sel, val), &(psel, pval)) in got[2..83].iter().zip(fs_plain_regs[1..].iter()) {
                assert_eq!(sel, psel);
                assert!(val == pval || fs_rt_sels.contains(&sel), "FS {sel:#x}");
            }
            for (i, (a, b)) in fs_plain.iter().zip(w.iter()).enumerate() {
                if a != b {
                    assert!((fs::REGS..fs::REGS + 84 * 12).contains(&i) || (0x788..0x78c).contains(&i), "FS byte {i:#x}");
                }
            }
        }
    }
}

#[test]
fn scratch_requests_follow_the_gen1_section_model() {
    for (layout, pages) in LAYOUTS {
        let s = rt::ScratchRequest::new(layout).unwrap();
        assert_eq!(s.pages(), pages);
        assert_eq!((s.tail_word(), s.stack_word(), s.window_word(), s.driver_word()), (0x2c, 112, 0x1600000, 0));
        // Both compute and render stages use macOS 27.2's J773g 0x2c tail reservation.
        assert_eq!(s.compute_tail_word(), 0x2c);
    }
    assert_eq!(rt::ScratchRequest::NONE.compute_tail_word(), 0);
    // Payload-free: no scratch even with depth and cliques.
    assert_eq!(rt::ScratchRequest::new(0x0000_0000_ff00_ffff).unwrap(), rt::ScratchRequest::NONE);
    // The smallest request is Apple's 89-page floor plus the guard page.
    assert_eq!(rt::ScratchRequest::new(1 << 32).unwrap().pages(), 89);
    // More than 2^16 pages, and a stack word that overflows, are refused.
    assert_eq!(rt::ScratchRequest::new(0xffff_0000_ff00_ffff), Err(EINVAL));
    assert_eq!(rt::ScratchRequest::new(0x0000_ffff_ff00_0000), Err(EINVAL));
}

#[test]
fn scs_sizing_is_the_m4_pro_formula_in_whole_rows() {
    // Per core: pages + ceil(pages / 256) + 8.
    assert_eq!(rt::extra_pages(10, 791), Ok(10 * (791 + 4 + 8)));
    assert_eq!(rt::extra_pages(10, 256), Ok(10 * (256 + 1 + 8)));
    assert_eq!(rt::extra_pages(10, 257), Ok(10 * (257 + 2 + 8)));
    assert_eq!(rt::extra_pages(10, 0), Ok(0));
    assert_eq!(rt::extra_pages(0, 791), Err(EINVAL));
    assert_eq!(rt::required_seeds(10, &[]), Ok(8192));
    assert_eq!(rt::required_seeds(10, &[0, 0]), Ok(8192));
    // Mesa's three T1 layouts on a 10-core M4: base 8192 + 10 x (pages + pages/256 + 8).
    assert_eq!(rt::required_seeds(10, &[791]), Ok(16384));
    assert_eq!(rt::required_seeds(10, &[827]), Ok(16896));
    assert_eq!(rt::required_seeds(10, &[989]), Ok(18432));
    assert_eq!(rt::required_seeds(8, &[827]), Ok(15360));
    assert_eq!(rt::required_seeds(1, &[89]), Ok(8704));
    // Both stages of one render add up.
    assert_eq!(rt::required_seeds(10, &[89, 89]), Ok(8192 + 2048));
    assert_eq!(rt::required_seeds(10, &[791, 89]), Ok(17408));
    assert_eq!(rt::required_seeds(10, &[989, 989]), Ok(28672));
    // Freelists: a base pool keeps 16384 records over 0x20000 bytes; an SCS pool gets the
    // 0x80000 records over 4 MiB that macOS 27.2 writes on the J773g.
    assert_eq!((rt::freelist_records(8192), rt::freelist_bytes(8192)), (16384, 0x20000));
    for seeds in [8704, 16384, 28672, rt::MAX_SCS_SEEDS] {
        assert_eq!((rt::freelist_records(seeds), rt::freelist_bytes(seeds)), (0x80000, 0x400000));
    }
    // The largest pool: 62 2-MiB rows, whose 124 MiB and 4-MiB freelist fill a 128 MiB window
    // (63 rows and their freelist would not).
    assert_eq!(rt::MAX_SCS_SEEDS, 62 * 512);
    assert!(rt::window_bytes(rt::MAX_SCS_SEEDS) <= hw::client::SCS_POOL_WINDOW);
    assert!(rt::window_bytes(rt::MAX_SCS_SEEDS + 512) > hw::client::SCS_POOL_WINDOW);
    assert_eq!(rt::required_seeds(10, &[2337]), Ok(31744));
    assert_eq!(rt::required_seeds(10, &[2338]), Err(E2BIG));
    assert_eq!(rt::required_seeds(10, &[1164, 1164]), Ok(31744));
    assert_eq!(rt::required_seeds(10, &[1165, 1165]), Err(E2BIG));
    // Pools are created for two stages at the larger request, or the request if two do not fit.
    assert_eq!(rt::pool_seeds(10, [791, 0]), Ok(24576));
    assert_eq!(rt::pool_seeds(10, [827, 0]), Ok(25088));
    assert_eq!(rt::pool_seeds(10, [0, 989]), rt::required_seeds(10, &[989, 989]));
    assert_eq!(rt::pool_seeds(10, [2000, 0]), rt::required_seeds(10, &[2000]));
    assert_eq!(rt::pool_seeds(10, [2338, 0]), Err(E2BIG));
    assert_eq!(rt::pool_seeds(10, [0, 0]), Ok(8192));
    // Extremes: refused, never wrapped (overflow checks are on).
    assert_eq!(rt::required_seeds(u32::MAX, &[u16::MAX, u16::MAX]), Err(E2BIG));
    assert_eq!(rt::required_seeds(u32::MAX, &[u16::MAX; 64]), Err(E2BIG));
    assert_eq!(rt::required_seeds(0, &[1]), Err(EINVAL));
    for cores in 1..=10 { for pages in [89u16, 200, 500, 791, 1000] {
        if let Ok(seeds) = rt::required_seeds(cores, &[pages]) {
            assert_eq!(seeds % rt::SEEDS_PER_ROW, 0);
            assert!(u64::from(seeds) >= 8192 + rt::extra_pages(cores, pages).unwrap() && seeds <= rt::MAX_SCS_SEEDS);
        }
    }}
}

#[test]
fn ray_and_as_build_admission() {
    let vm = mmu::Vm { mapped: vec![(POOL, POOL + 0x4000), (0x10010020000, 0x10010020040)] };
    let context = Context { vm: &vm, usc_base: 0, active_cores: 10, render_scs: [791, 0] };
    let scs = |needed, pool| rt::ScsSeeds { needed, pool };
    let mut b = SpanBatch;
    // Admission returns the pool the command needs, and the one created for it: compute for two
    // dispatches.
    assert_eq!(check_compute_ray(&mut b, &context, &ray(LAYOUTS[0].0)), Ok(scs(16384, 24576)));
    assert_eq!(check_compute_ray(&mut b, &context, &ray(LAYOUTS[1].0)), Ok(scs(16896, 25088)));
    // 4 KiB of driver bytes per thread: 3833 pages per core, more than a window holds.
    let huge = 0x1000_0000_6000_0070;
    assert_eq!(rt::ScratchRequest::new(huge).unwrap().pages(), 3833);
    assert_eq!(check_compute_ray(&mut b, &context, &ray(huge)), Err(E2BIG));
    let unmapped = mmu::Vm { mapped: vec![(POOL, POOL + 0x3000), (0x10010020000, 0x10010020040)] };
    let narrow = Context { vm: &unmapped, ..context };
    assert_eq!(check_compute_ray(&mut b, &narrow, &ray(LAYOUTS[0].0)), Err(ENOENT));
    let no_program = mmu::Vm { mapped: vec![(POOL, POOL + 0x4000)] };
    assert_eq!(check_compute_ray(&mut b, &Context { vm: &no_program, ..context }, &ray(LAYOUTS[0].0)), Err(ENOENT));
    let tables_only = rt::ComputeRay::new(0, 0, 0, POOL, 0x4000, 1, 112, 0, 0).unwrap();
    assert_eq!(check_compute_ray(&mut b, &Context { vm: &no_program, ..context }, &tables_only), Ok(rt::ScsSeeds::NONE));
    // Renders: within the queue's reservation, either stage; beyond it EINVAL. The pool created
    // for them holds two stages of the reservation's larger one.
    let none = rt::RayQueryStage::NONE;
    let small = stage(1 << 32 | 0x6000_0000);
    assert_eq!(check_render_ray(&mut b, &context, &[stage(LAYOUTS[0].0), none]), Ok(scs(16384, 24576)));
    assert_eq!(check_render_ray(&mut b, &context, &[none, small]), Ok(scs(9216, 24576)));
    assert_eq!(check_render_ray(&mut b, &Context { render_scs: [0, 0], ..context }, &[none, small]), Err(EINVAL));
    assert_eq!(check_render_ray(&mut b, &Context { render_scs: [89, 89], ..context }, &[small, small]), Ok(scs(10240, 10240)));
    assert_eq!(check_render_ray(&mut b, &context, &[stage(LAYOUTS[1].0), none]), Err(EINVAL));
    assert_eq!(check_render_ray(&mut b, &Context { render_scs: [3833, 0], ..context }, &[stage(huge), none]), Err(E2BIG));
    assert_eq!(check_render_ray(&mut b, &Context { vm: &unmapped, ..context }, &[stage(LAYOUTS[0].0), none]), Err(ENOENT));
    assert_eq!(check_render_ray(&mut b, &Context { render_scs: [0, 0], ..context }, &[none, none]), Ok(rt::ScsSeeds::NONE));
    // Flags: AS_BUILD alone.
    assert_eq!(rt::as_build(0), Some(false)); assert_eq!(rt::as_build(4), Some(true));
    for flag in [1, 2, 8, 1 << 16, 1 << 17, 1 << 20, 5, 6] { assert_eq!(rt::as_build(flag), None); }
}

// The commands Mesa's G16G ray-query path (ASAHI_MESA_DEBUG=g16grt, mesa 3ff3d359) submits on a
// 10-core M4: hk_gen1_ray_query_scs_layout() is 0x0130000060010070 with the candidate profile,
// the intersection pool is one 16-KiB IFT (first 1, count 112), the default program
// 0x10010020000 with control 0x16000000000. dEQP-VK.ray_query.* compute, chit/ahit and
// dynamic-indexing cases submit the compute shape; .vert/.frag/.tesc cases reserve the render
// stage at QUEUE_CREATE (hk_queue_ensure_drm) and submit a render with it. Before SCS pools were
// sized from the request, every one of them was refused with E2BIG, which Mesa reports as
// VK_ERROR_DEVICE_LOST at vkQueueSubmit.
#[test]
fn mesa_g16grt_ray_query_commands_are_admitted() {
    let mesa = LAYOUTS[2].0;
    let vm = mmu::Vm { mapped: vec![(POOL, POOL + 0x4000), (0x10010020000, 0x10010020040)] };
    let pages = rt::ScratchRequest::new(mesa).unwrap().pages();
    assert_eq!(pages, 989);
    // QUEUE_CREATE render_scs_layout: the vertex stage, the fragment stage, or both. The VM's
    // SCS render pools a queue creates hold both stages, so a queue Mesa replaces to add the
    // other stage still fits them.
    for reservation in [[pages, 0], [0, pages], [pages, pages]] {
        assert!(rt::required_seeds(10, &reservation).is_ok(), "QUEUE_CREATE {reservation:?}");
        assert_eq!(rt::pool_seeds(10, reservation), Ok(28672), "{reservation:?}");
    }
    let context = Context { vm: &vm, usc_base: 0, active_cores: 10, render_scs: [pages, pages] };
    let scs = |needed, pool| rt::ScsSeeds { needed, pool };
    let mut b = SpanBatch;
    // Compute: 18432 for one dispatch, a 56-row pool for two.
    assert_eq!(check_compute_ray(&mut b, &context, &ray(mesa)), Ok(scs(18432, 28672)));
    let none = rt::RayQueryStage::NONE;
    for (stages, needed) in [([stage(mesa), none], 18432), ([none, stage(mesa)], 18432), ([stage(mesa), stage(mesa)], 28672)] {
        assert_eq!(check_render_ray(&mut b, &context, &stages), Ok(scs(needed, 28672)));
        // A queue that reserved one stage creates pools that take both.
        let one = Context { render_scs: [pages, 0], ..context };
        assert_eq!(check_render_ray(&mut b, &one, &[stage(mesa), none]).map(|s| s.pool), Ok(28672));
    }
    // Their pools fit an SCS window, freelist included.
    assert_eq!(rt::freelist_records(28672), 0x80000);
    assert!(rt::window_bytes(28672) <= hw::client::SCS_POOL_WINDOW);
}

#[test]
fn long_allowance_follows_g17_select_per_job() {
    let render = |stages: [rt::RayQueryStage; 2]| Command::Render(Box::new(RenderCommand { ray_query: stages, scs: rt::ScsSeeds::NONE }));
    let compute = |ray: rt::ComputeRay, as_build: bool| Command::Compute(Box::new(ComputeCommand { ray, scs: rt::ScsSeeds::NONE, as_build }));
    let none = rt::RayQueryStage::NONE;
    assert!(render([stage(LAYOUTS[0].0), none]).long() && render([none, stage(LAYOUTS[0].0)]).long());
    assert!(!render([none, none]).long() && render([none, none]).scs() == rt::ScsSeeds::NONE);
    assert!(compute(ray(LAYOUTS[0].0), false).long() && compute(rt::ComputeRay::NONE, true).long());
    assert!(!compute(rt::ComputeRay::NONE, false).long());
    // Scratch alone is not traversal (as on the M4 Pro), but it needs an SCS pool; tables
    // alone do not.
    let scratch = rt::ComputeRay::new(LAYOUTS[0].0, 0, 0, 0, 0, 0, 0, 0, 0).unwrap();
    let vm = mmu::Vm { mapped: vec![(POOL, POOL + 0x4000)] };
    let context = Context { vm: &vm, usc_base: 0, active_cores: 10, render_scs: [0, 0] };
    let admitted = rt::ScsSeeds { needed: 16384, pool: 24576 };
    let mut b = SpanBatch;
    assert!(!compute(scratch, false).long() && check_compute_ray(&mut b, &context, &scratch) == Ok(admitted));
    let tables = rt::ComputeRay::new(0, 0, 0, POOL, 0x4000, 1, 112, 0, 0).unwrap();
    assert_eq!(check_compute_ray(&mut b, &context, &tables), Ok(rt::ScsSeeds::NONE));
    // A command's SCS pool is what admission recorded.
    let command = Command::Compute(Box::new(ComputeCommand { ray: scratch, scs: admitted, as_build: false }));
    assert_eq!((command.scs(), compute(rt::ComputeRay::NONE, false).scs()), (admitted, rt::ScsSeeds::NONE));
    // Deadlines: at least ten seconds for a long job, never re-enabling a disabled one.
    assert_eq!(logic::long_deadline_ms(5000), 10_000);
    assert_eq!(logic::long_deadline_ms(10_000), 10_000);
    assert_eq!(logic::long_deadline_ms(30_000), 30_000);
    assert_eq!(logic::long_deadline_ms(0), 0);
    assert_eq!(logic::long_deadline_ms(1), 10_000);
    let now = 1_000_000_000;
    let d = logic::Deadlines::renew(now, 5000, 5000);
    assert_eq!((d.select(false, false), d.select(true, false)), (now + 5_000_000_000, now + 5_000_000_000));
    assert_eq!((d.select(false, true), d.select(true, true)), (now + 10_000_000_000, now + 10_000_000_000));
    let d = logic::Deadlines::renew(now, 5000, 0);
    assert_eq!((d.select(true, false), d.select(true, true)), (u64::MAX, u64::MAX));
    assert_eq!(d.select(false, true), now + 10_000_000_000);
    let d = logic::Deadlines::renew(u64::MAX - 1, 20_000, 50);
    assert_eq!((d.select(false, true), d.select(true, false), d.select(true, true)), (u64::MAX, u64::MAX, u64::MAX));
    let d = logic::Deadlines::renew(now, 50, 50);
    assert_eq!((d.select(false, false), d.select(true, true)), (now + 100_000_000, now + 10_000_000_000));
    assert_eq!(logic::Deadlines::EXPIRED.select(true, true), logic::EXPIRED);
}
'''
encode_source = (BACKEND / 'encode.rs').read_text()
# The compute encoder's gates, visible to this harness only through a tiny accessor.
gates = '\npub(crate) fn compute_encoder_gate() -> bool { compute_encoder::COMPUTE_SCS_DRIVER_WORD }\n' \
        'pub(crate) fn render_encoder_gate() -> bool { render_encoder::RENDER_SCS_ENABLE }\n'
with tempfile.TemporaryDirectory(prefix='m4-rust-rt-') as name:
    directory = pathlib.Path(name)
    staged = directory / 'g16'
    staged.mkdir()
    # Stage the encoder with the gate accessors appended; the other modules come from the tree.
    (staged / 'encode').mkdir()
    for part in ['compute.rs', 'render.rs']:
        (staged / 'encode' / part).write_text((BACKEND / 'encode' / part).read_text())
    (staged / 'encode.rs').write_text(encode_source + gates)
    source = harness
    for key, value in {'@FLAT@': str(FLAT), '@ADMISSION@': admission, '@CONVERSION@': conversion,
                       '@RESERVE@': reserve, '@LONG@': long, '@CLIENT@': client}.items():
        source = source.replace(key, value)
    source = source.replace('#[path = "@BACKEND@/encode.rs"]', '#[path = "' + str(staged / 'encode.rs') + '"]')
    source = source.replace('@BACKEND@', str(BACKEND))
    source += '\n#[test] fn render_gate_is_on() { assert!(encode::render_encoder_gate()); }\n'
    test = directory / 'test.rs'
    test.write_text(source)
    (OUT / 'ray-tracing-host.rs').write_text(source)
    binary = directory / 'test'
    subprocess.run(rustc() + ['--edition=2021', '-C', 'overflow-checks=yes', '--test', str(test), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), '--nocapture'], text=True, capture_output=True)
    print(result.stdout + result.stderr, end='')
    (OUT / 'ray-tracing-host.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    tests = int(re.search(r'test result: ok\. (\d+) passed;', result.stdout)[1])
(OUT / 'ray-tracing-host.json').write_text(json.dumps({
    'result': 'pass', 'tests': tests, 'feature_word': hex(features),
    'scope': 'Production rt.rs, recovery/logic.rs and TA/FS/compute encoders, with command.rs admission and '
             'engine.rs conversion copied verbatim; VM spans mocked. No GPU, firmware or Mesa.',
    'source_sha256': {name: hashlib.sha256((BACKEND / name).read_bytes()).hexdigest() for name in
                      ['rt.rs', 'recovery/logic.rs', 'command.rs', 'engine.rs', 'params.rs', 'encode.rs', 'hw.rs',
                       'encode/compute.rs', 'encode/render.rs']},
}, indent=2) + '\n')
