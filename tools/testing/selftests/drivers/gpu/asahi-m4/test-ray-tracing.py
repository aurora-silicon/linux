#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host-check the G16G ray-query command set (RENDER_RAY_QUERY).

Compiles the production g16/rt.rs, recovery/logic.rs and the TA/FS/compute encoders, plus the
command admission and engine conversion functions copied verbatim from command.rs and
engine.rs, against CPU-only stand-ins. No GPU, firmware or Mesa is involved.

Failure modes this guards against (written before the code):

 1. A non-RT command changes bytes: the engine's conversion of an empty ray state is not the
    disabled word set, or the disabled set differs from the words G16G always wrote (the frozen
    golden fixtures then cover the rest).
 2. RT words land in the wrong register record, the register counts change, or a gated
    register (0x017d9, 0x1a141) is written while its gate is off.
 3. The fragment window word loses bit 9; a render stage's tail word takes the auxiliary
    intersection word (the M4 Pro's render programs use the scratch tail).
 4. SCS sizing differs from the M4 Pro's formula ((pages + pages/256 + 8) x cores pages), is not
    rounded to whole 2-MiB rows, overflows silently, or admits a pool larger than the freelist
    (E2BIG); zero cores is accepted.
 5. A render whose scratch exceeds its queue's reservation is accepted, or one within it
    refused; an unmapped intersection pool or default program is accepted.
 6. The long allowance misses a ray job, applies to plain work, re-enables a disabled
    (zero) deadline, or shortens a deadline already longer than 10 s.
 7. Scratch alone counts as ray traversal.
 8. The feature word loses a bit or gains another than RENDER_RAY_QUERY.
 9. A UMA manager image or compute descriptor reports seeds that are not whole rows inside the
    freelist.
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
conversion = '\n'.join(function(engine, name) for name in ['compute_rt', 'stage_rt'])
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
assert features == 0x9c02, hex(features)
assert sorted(names) == sorted(['DRM_ASAHI_FEATURE_PER_COMMAND_INPUT_SYNCS', 'DRM_ASAHI_FEATURE_EXACT_PRIOR_BARRIERS',
                                'DRM_ASAHI_FEATURE_RENDER_RAY_QUERY']), names
print(f'feature word {features:#x} (RENDER_RAY_QUERY added to 0x1c02)')

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
    pub mod client { pub const PB_HEAP_SIZE: u64 = 0x10000000; }
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
struct Context<'a> { vm: &'a mmu::Vm, usc_base: u64, active_cores: u32, render_scs: [u16; 2] }
@ADMISSION@
@CONVERSION@
type KBox<T> = Box<T>;
struct RenderCommand { ray_query: [rt::RayQueryStage; 2] }
struct ComputeCommand { ray: rt::ComputeRay }
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
    assert!(!encode::compute_encoder_gates().0 && !encode::compute_encoder_gates().1);
}

fn compute_refs() -> encode::ComputeRefs {
    let f = 0xfffffc2000000000u64; let g = 0x1000000000u64;
    encode::ComputeRefs {
        notifier: f + 0x600400, qinfo: f + 0x200000, user_stamp: f + 0x600648, fw_stamp: f + 0x600640,
        work: f + 0x100000, work_gva: g + 0x100000, seq: f + 0x100900, preempt_gva: g + 0x200000,
        its: f + 0x1008c0, stats_fw: f + 0x300000, auxiliary_fw: f + 0x400000, auxiliary_gva: g + 0x400000,
        context_store_gva: g + 0x500000, counter_fw: f + 0x500000, pool_state_gva: g + 0x600000,
        page_list_gva: g + 0x700000, usc_base: 1 << 40, record_fw: f + 0x700000, binding_generation: 1,
        uma_slot: 1, command_id: 1, compute_index: 0, work_818: 0, target_stamp: 0x100, submitted: 1,
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
        for (layout_word, pages) in LAYOUTS {
            let words = compute_rt(&ray(layout_word), rt::SCS_SEEDS);
            let w = encode_compute(layout, &words).unwrap();
            assert_eq!((u16_at(&w, 0x728), u16_at(&w, 0x72a)), (25, 25 * 12));
            let got = records(&w, compute::REGS, 25);
            assert!(got.iter().all(|&(sel, _)| sel != 0x017d9 && sel != 0x1a141), "gated registers");
            let expect = |sel: u32| got.iter().find(|r| r.0 == sel).unwrap().1;
            let mib = u64::from(pages).div_ceil(256);
            assert_eq!(expect(0x101d1), u64::from(pages) | mib << 16);
            assert_eq!((expect(0xd479), expect(0x1a0e9), expect(0x107a1)), (8, 112, 1 << 24 | 0x60 << 16));
            assert_eq!([expect(0x1a089), expect(0x1a091), expect(0x1a059), expect(0x1a061), expect(0x1a0b9), expect(0x1a0c1)],
                [POOL + 128, 0xc0003800, 0x6f00000000, 0x3feff, 0x10010020000, 0x16000000000]);
            let d = compute::DESCRIPTOR;
            assert_eq!((u32_at(&w, d + 0x24), u32_at(&w, d + 0x2c), u32_at(&w, d + 0x1c)), (16384, 16384, 16384));
            // Nothing else moves: only the ten RT words and the two seed counts differ.
            let mut differ = Vec::new();
            for (i, (a, b)) in plain.iter().zip(w.iter()).enumerate() { if a != b { differ.push(i); } }
            let rt_records: Vec<usize> = (9..19).collect();
            assert!(differ.iter().all(|&i| {
                let record = i.checked_sub(compute::REGS).map(|o| (o / 12, o % 12));
                matches!(record, Some((r, o)) if rt_records.contains(&r) && o >= 4)
                    || (d + 0x24..d + 0x28).contains(&i) || (d + 0x2c..d + 0x30).contains(&i)
            }), "unexpected bytes {:x?}", differ);
        }
        // Seeds are whole rows within the freelist.
        for seeds in [0, 8191, 8704 + 64, 16896] {
            let words = ComputeRt { seeds, ..ComputeRt::DISABLED };
            assert!(encode_compute(layout, &words).is_err(), "seeds {seeds}");
        }
        assert!(encode_compute(layout, &ComputeRt { seeds: 8704, ..ComputeRt::DISABLED }).is_ok());
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
            assert_eq!(q, taq_plain);
            assert_eq!(u16_at(&w, 0x748), 66);
            let got = records(&w, ta::REGS, 66);
            assert!(got.iter().all(|&(sel, _)| sel != 0x017d9));
            let value = |got: &Vec<(u32, u64)>, sel: u32| got.iter().find(|r| r.0 == sel).unwrap().1;
            assert_eq!([0x1a099, 0x1a0a1, 0x1a069, 0x1a071, 0x1a0c9, 0x1a0d1].map(|s| value(&got, s)), ift);
            assert_eq!([0x101c9, 0xd471, 0x1a0f1, 0x10799].map(|s| value(&got, s)), [size, 8, 112, window]);
            for (i, (a, b)) in ta_plain.iter().zip(w.iter()).enumerate() {
                if a != b { let o = i.checked_sub(ta::REGS).expect("TA header byte"); assert!((46..56).contains(&(o / 12)) && o % 12 >= 4, "TA byte {i:#x}"); }
            }
            let (w, q) = fs_rt(&words, fl);
            assert_eq!(q, fsq_plain);
            assert_eq!(u16_at(&w, 0x788), 82);
            let got = records(&w, fs::REGS, 82);
            assert!(got.iter().all(|&(sel, _)| sel != 0x017d9));
            assert_eq!([0x1a0a9, 0x1a0b1, 0x1a079, 0x1a081, 0x1a0d9, 0x1a0e1].map(|s| value(&got, s)), ift);
            assert_eq!([0x101c1, 0xd469, 0x1a0f9, 0x10791].map(|s| value(&got, s)), [size, 8, 112, window | 1 << 9]);
            for (i, (a, b)) in fs_plain.iter().zip(w.iter()).enumerate() {
                if a != b { let o = i.checked_sub(fs::REGS).expect("FS header byte"); assert!((o / 12 == 17 || (65..74).contains(&(o / 12))) && o % 12 >= 4, "FS byte {i:#x}"); }
            }
        }
    }
}

#[test]
fn scratch_requests_follow_the_gen1_section_model() {
    for (layout, pages) in LAYOUTS {
        let s = rt::ScratchRequest::new(layout).unwrap();
        assert_eq!(s.pages(), pages);
        assert_eq!((s.tail_word(), s.stack_word(), s.window_word(), s.driver_word()), (8, 112, 0x1600000, 0));
    }
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
    // Mesa's smallest T1 layout fits a 10-core M4's SCS pool exactly; the larger two do not.
    assert_eq!(rt::required_seeds(10, &[791]), Ok(16384));
    assert_eq!(rt::required_seeds(10, &[827]), Err(E2BIG));
    assert_eq!(rt::required_seeds(10, &[989]), Err(E2BIG));
    assert_eq!(rt::required_seeds(8, &[827]), Ok(15360));
    assert_eq!(rt::required_seeds(1, &[89]), Ok(8704));
    // Both stages of one render add up.
    assert_eq!(rt::required_seeds(10, &[89, 89]), Ok(8192 + 2048));
    assert_eq!(rt::required_seeds(10, &[791, 89]), Err(E2BIG));
    // Extremes: refused, never wrapped (overflow checks are on).
    assert_eq!(rt::required_seeds(u32::MAX, &[u16::MAX, u16::MAX]), Err(E2BIG));
    assert_eq!(rt::required_seeds(u32::MAX, &[u16::MAX; 64]), Err(E2BIG));
    assert_eq!(rt::required_seeds(0, &[1]), Err(EINVAL));
    for cores in 1..=10 { for pages in [89u16, 200, 500, 791, 1000] {
        if let Ok(seeds) = rt::required_seeds(cores, &[pages]) {
            assert_eq!(seeds % rt::SEEDS_PER_ROW, 0);
            assert!(u64::from(seeds) >= 8192 + rt::extra_pages(cores, pages).unwrap() && seeds <= rt::SCS_SEEDS);
        }
    }}
}

#[test]
fn ray_admission() {
    let vm = mmu::Vm { mapped: vec![(POOL, POOL + 0x4000), (0x10010020000, 0x10010020040)] };
    let context = Context { vm: &vm, usc_base: 0, active_cores: 10, render_scs: [791, 0] };
    assert_eq!(check_compute_ray(&context, &ray(LAYOUTS[0].0)), Ok(()));
    assert_eq!(check_compute_ray(&context, &ray(LAYOUTS[1].0)), Err(E2BIG));
    let unmapped = mmu::Vm { mapped: vec![(POOL, POOL + 0x3000), (0x10010020000, 0x10010020040)] };
    let narrow = Context { vm: &unmapped, ..context };
    assert_eq!(check_compute_ray(&narrow, &ray(LAYOUTS[0].0)), Err(ENOENT));
    let no_program = mmu::Vm { mapped: vec![(POOL, POOL + 0x4000)] };
    assert_eq!(check_compute_ray(&Context { vm: &no_program, ..context }, &ray(LAYOUTS[0].0)), Err(ENOENT));
    let tables_only = rt::ComputeRay::new(0, 0, 0, POOL, 0x4000, 1, 112, 0, 0).unwrap();
    assert_eq!(check_compute_ray(&Context { vm: &no_program, ..context }, &tables_only), Ok(()));
    // Renders: within the queue's reservation, either stage; beyond it EINVAL.
    let none = rt::RayQueryStage::NONE;
    let small = stage(1 << 32 | 0x6000_0000);
    assert_eq!(check_render_ray(&context, &[stage(LAYOUTS[0].0), none]), Ok(()));
    assert_eq!(check_render_ray(&context, &[none, small]), Ok(()));
    assert_eq!(check_render_ray(&Context { render_scs: [0, 0], ..context }, &[none, small]), Err(EINVAL));
    assert_eq!(check_render_ray(&Context { render_scs: [89, 89], ..context }, &[small, small]), Ok(()));
    assert_eq!(check_render_ray(&context, &[stage(LAYOUTS[1].0), none]), Err(E2BIG));
    assert_eq!(check_render_ray(&Context { vm: &unmapped, ..context }, &[stage(LAYOUTS[0].0), none]), Err(ENOENT));
    assert_eq!(check_render_ray(&Context { render_scs: [0, 0], ..context }, &[none, none]), Ok(()));
}

#[test]
fn long_allowance_follows_g17_select_per_job() {
    let render = |stages: [rt::RayQueryStage; 2]| Command::Render(Box::new(RenderCommand { ray_query: stages }));
    let compute = |ray: rt::ComputeRay, _: bool| Command::Compute(Box::new(ComputeCommand { ray }));
    let none = rt::RayQueryStage::NONE;
    assert!(render([stage(LAYOUTS[0].0), none]).long() && render([none, stage(LAYOUTS[0].0)]).long());
    assert!(!render([none, none]).long() && !render([none, none]).scs());
    assert!(compute(ray(LAYOUTS[0].0), false).long());
    assert!(!compute(rt::ComputeRay::NONE, false).long());
    // Scratch alone is not traversal (as on the M4 Pro), but it needs an SCS pool.
    let scratch = rt::ComputeRay::new(LAYOUTS[0].0, 0, 0, 0, 0, 0, 0, 0, 0).unwrap();
    assert!(!compute(scratch, false).long() && compute(scratch, false).scs());
    assert!(!compute(rt::ComputeRay::new(0, 0, 0, POOL, 0x4000, 1, 112, 0, 0).unwrap(), false).scs());
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
gates = '\npub(crate) fn compute_encoder_gates() -> (bool, bool) { (compute_encoder::COMPUTE_SCS_ENABLE, compute_encoder::COMPUTE_SCS_DRIVER_WORD) }\n' \
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
                       '@LONG@': long}.items():
        source = source.replace(key, value)
    source = source.replace('#[path = "@BACKEND@/encode.rs"]', '#[path = "' + str(staged / 'encode.rs') + '"]')
    source = source.replace('@BACKEND@', str(BACKEND))
    source += '\n#[test] fn render_gate_is_off() { assert!(!encode::render_encoder_gate()); }\n'
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
                      ['rt.rs', 'recovery/logic.rs', 'command.rs', 'engine.rs', 'params.rs', 'encode.rs',
                       'encode/compute.rs', 'encode/render.rs']},
}, indent=2) + '\n')
