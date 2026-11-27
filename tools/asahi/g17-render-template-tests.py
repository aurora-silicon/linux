#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host unit tests for G17P/V25G83 render descriptor template delta-patching
and 64-byte fragment status clearing.

Compiles the real firmware modules (`fw/kick.rs`, `fw/compute.rs`, `fw/render.rs`,
`fw/render_template.rs`) with `rustc --test` against a minimal `kernel` shim so
the exact production encoding logic executes byte-for-byte against the full
reference encoders across G17P and V25G83, SCS on/off, ring wraparound, MCache
growth/shrink/spill, key-change invalidation, mismatch rejection, and validation
errors. Also checks source guards in `queue/render/memory.rs` and `capacity.rs`.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / 'drivers/gpu/drm/asahi'
G17 = ROOT / 'g17'


def strip_kunit_blocks(src: str) -> str:
    """Remove `#[kunit_tests(...)] mod ... { ... }` blocks for standalone rustc."""
    out = []
    i = 0
    marker = '#[kunit_tests('
    while True:
        idx = src.find(marker, i)
        if idx < 0:
            out.append(src[i:])
            break
        out.append(src[i:idx])
        brace = src.index('{', idx)
        depth = 1
        pos = brace + 1
        while depth:
            ch = src[pos]
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
            pos += 1
        i = pos
    return ''.join(out)


def check_source_guards():
    memory_rs = (G17 / 'queue/render/memory.rs').read_text()
    assert 'FRAGMENT_STATUS_CLEAR_BYTES: usize = 64' in memory_rs, \
        'NeoGeneration::prepare must define 64-byte fragment status clear'
    assert 'self.fragment.bytes_mut()[..FRAGMENT_STATUS_CLEAR_BYTES].fill(0)' in memory_rs, \
        'NeoGeneration::prepare must clear only the first 64-byte cache line of fragment status'
    assert 'fw::render_template::encode(' in memory_rs, \
        'Memory::write_descriptors must use fw::render_template::encode'

    capacity_rs = (G17 / 'capacity.rs').read_text()
    assert 'g17_render_template full={} patched={} verified={} mismatched={} unmapped={}' in capacity_rs, \
        'capacity debugfs must report g17_render_template counters'


def build_and_run_rust_tests(mutation: str | None = None):
    fw_mod = (G17 / 'fw/mod.rs').read_text()
    fw_kick = strip_kunit_blocks((G17 / 'fw/kick.rs').read_text())
    fw_compute = strip_kunit_blocks((G17 / 'fw/compute.rs').read_text())
    fw_render = strip_kunit_blocks((G17 / 'fw/render.rs').read_text())
    fw_render_template = strip_kunit_blocks((G17 / 'fw/render_template.rs').read_text())

    fw_kick = fw_kick.replace('use kernel::prelude::*;', 'use crate::kernel::prelude::*;')
    fw_compute = fw_compute.replace('use kernel::prelude::*;', 'use crate::kernel::prelude::*;')
    fw_render = fw_render.replace('use kernel::prelude::*;', 'use crate::kernel::prelude::*;')
    fw_render = fw_render.replace('pub(crate) mod storage;', 'pub(crate) mod storage {}')
    fw_render_template = fw_render_template.replace('use kernel::prelude::*;', 'use crate::kernel::prelude::*;')

    if mutation == 'skip_render_mcache_clear':
        fw_render_template = fw_render_template.replace(
            'else if template.slot_mcache[slot] != 0 {',
            'else if false && template.slot_mcache[slot] != 0 {',
            1,
        )
    elif mutation == 'skip_ta_vdm_patch':
        fw_render_template = fw_render_template.replace(
            'ta.registers.entries[usize::from(ta_idx.vdm)].value = vdm;',
            '// mutated: omitted vdm patch',
            1,
        )
    elif mutation == 'skip_frag_eot_patch':
        fw_render_template = fw_render_template.replace(
            'frag.arrays[0].entries[usize::from(frag_idx.eot_va)].value = eot_va;',
            '// mutated: omitted eot_va patch',
            1,
        )
    elif mutation == 'skip_key_width_check':
        fw_render_template = fw_render_template.replace(
            'width,\n            height,',
            'width: _,\n            height,',
            1,
        )
        fw_render_template = fw_render_template.replace(
            'RenderPass {\n            width,',
            'RenderPass {\n            width: 0,',
            1,
        )

    fw_helpers = fw_mod[fw_mod.index('pub(crate) fn clear'):]

    harness = r'''
#![allow(dead_code, unused_imports, unused_variables, unused_parens)]

pub mod kernel {
    pub mod prelude {
        pub type Error = i32;
        pub type Result<T = ()> = core::result::Result<T, Error>;
        pub const EINVAL: Error = -22;
        pub const EOVERFLOW: Error = -75;
        pub const ENOMEM: Error = -12;
        pub const GFP_KERNEL: u32 = 0;

        pub unsafe trait Zeroable: Sized {}
        unsafe impl Zeroable for u8 {}
        unsafe impl Zeroable for u16 {}
        unsafe impl Zeroable for u32 {}
        unsafe impl Zeroable for u64 {}
        unsafe impl<T: Zeroable, const N: usize> Zeroable for [T; N] {}

        pub struct KBox<T>(pub Box<T>);
        impl<T> KBox<T> {
            pub fn new_uninit(_flags: u32) -> Result<KBox<core::mem::MaybeUninit<T>>> {
                Ok(KBox(Box::new(core::mem::MaybeUninit::uninit())))
            }
        }
        impl<T> KBox<core::mem::MaybeUninit<T>> {
            pub fn as_mut_ptr(&mut self) -> *mut T {
                self.0.as_mut_ptr()
            }
            pub unsafe fn assume_init(self) -> KBox<T> {
                let raw = Box::into_raw(self.0) as *mut T;
                KBox(unsafe { Box::from_raw(raw) })
            }
        }
        impl<T> core::ops::Deref for KBox<T> {
            type Target = T;
            fn deref(&self) -> &T { &self.0 }
        }
        impl<T> core::ops::DerefMut for KBox<T> {
            fn deref_mut(&mut self) -> &mut T { &mut self.0 }
        }

        #[macro_export]
        macro_rules! static_assert {
            ($cond:expr) => { const _: () = assert!($cond); };
        }
        pub use crate::static_assert;
    }
}

pub mod fw {
    use crate::kernel::prelude::*;

    pub(crate) mod queue {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(u32)]
        pub(crate) enum CommandTag {
            Tiling = 0x100,
            Fragment = 0x101,
            Compute = 0x102,
        }
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(u8)]
        pub(crate) enum DataMaster {
            Tiling = 0,
            Fragment = 1,
            Compute = 2,
        }
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub(crate) struct Policy(pub(crate) u8);
        impl Policy {
            pub(crate) const PRIORITIES: u8 = 4;
            pub(crate) fn kick_priority(self) -> u8 { self.0 }
        }
    }

    pub(crate) mod m5 {
        pub(crate) mod target {
            pub(crate) fn cluster_control(clusters: u32, pb_pages: u64) -> Option<[u64; 3]> {
                let difference = match clusters {
                    1 => return Some([1, 0, pb_pages]),
                    2 => 4u64,
                    4 => 5u64,
                    8 => 6u64,
                    _ => return None,
                };
                Some([0x40 | (difference << 8) | (difference << 19), 4, pb_pages])
            }
        }
    }

    pub(crate) mod render_rt {
        use super::compute::{IntersectionTables, ScratchRequest};
        #[derive(Clone, Copy, PartialEq, Eq)]
        pub(crate) struct RayQueryStage {
            pub(crate) intersection: IntersectionTables,
            pub(crate) scratch: ScratchRequest,
        }
    }

    #[path = "kick.rs"]
    pub(crate) mod kick;
    #[path = "compute.rs"]
    pub(crate) mod compute;
    #[path = "render.rs"]
    pub(crate) mod render;
    #[path = "render_template.rs"]
    pub(crate) mod render_template;

    include!("helpers.rs");
}

#[cfg(test)]
mod tests {
    use super::fw::compute::{IntersectionTables, ScratchRequest};
    use super::fw::kick::WriteRange;
    use super::fw::render::{
        Deflake, FragmentSlot, RegisterPrograms, RenderArgs, RenderPass, RenderResources,
        RenderStage, TaDescriptor, UscProgram,
    };
    use super::fw::render_rt::RayQueryStage;
    use super::fw::render_template::{encode, Path, TemplateSlot, RING_SLOTS};
    use super::fw::ObjectIds;
    use super::kernel::prelude::*;

    const USER_BASE: u64 = 0x10_0000_0000;

    fn bytes_of<T>(val: &T) -> &[u8] {
        unsafe {
            core::slice::from_raw_parts(core::ptr::from_ref(val).cast::<u8>(), core::mem::size_of::<T>())
        }
    }

    fn zero_box<T>() -> KBox<T> {
        let mut b = KBox::<T>::new_uninit(GFP_KERNEL).unwrap();
        unsafe {
            core::ptr::write_bytes(b.as_mut_ptr(), 0, 1);
            b.assume_init()
        }
    }

    fn base_render() -> (RenderPass, RenderResources) {
        let mut pass: RenderPass = unsafe { core::mem::zeroed() };
        let mut resources: RenderResources = unsafe { core::mem::zeroed() };
        pass.width = 1920;
        pass.height = 1080;
        pass.layers = 1;
        pass.utile_width = 32;
        pass.utile_height = 32;
        pass.sample_size = 4;
        pass.usc_base = USER_BASE + 0x20_0000;
        pass.bg = UscProgram { offset: 0x100, rsrc_spec: 0x11 };
        pass.partial_bg = UscProgram { offset: 0x200, rsrc_spec: 0x12 };
        pass.eot_offset = 0x300;
        pass.partial_eot_offset = 0x400;

        resources.context_id = 4;
        resources.context_generation = 2;
        resources.pb_slot = 1;
        resources.free_list_slot = 2;
        resources.free_list_control_va = 0xffff_f000_0001_0000;
        resources.pb_va = USER_BASE + 0x30_0000;
        resources.scene_end_va = USER_BASE + 0x41_0000;
        resources.tpc_va = USER_BASE + 0x53_0000;
        resources.tpc_size = 0x10000;
        resources.gtp_va = core::array::from_fn(|i| USER_BASE + 0x60_0000 + i as u64 * 0x1000);
        (pass, resources)
    }

    fn make_render_args<'a>(
        pass: &'a mut RenderPass,
        resources: &'a mut RenderResources,
        ordinal: u64,
        programs: RegisterPrograms,
        scs: bool,
    ) -> RenderArgs<'a> {
        let step = ordinal * 0x100;
        pass.vdm_va = USER_BASE + 0x10_0000 + step;
        pass.scissor_va = USER_BASE + 0x11_0000 + step;
        pass.depth_bias_va = USER_BASE + 0x12_0000 + step;
        pass.occlusion_query_va = USER_BASE + 0x13_0000 + step;
        pass.sampler_heap_va = if ordinal % 3 == 0 { 0 } else { USER_BASE + 0x14_0000 + step };
        pass.sampler_count = if ordinal % 3 == 0 { 0 } else { (ordinal as u32 % 16) + 1 };
        pass.depth_clear = 0x3f80_0000u32.wrapping_add(ordinal as u32);
        pass.stencil_clear = (ordinal as u32) & 0xff;
        pass.bg.offset = 0x100 + ((ordinal as u32 & 0x1f) << 4);
        pass.partial_bg.offset = 0x200 + ((ordinal as u32 & 0x1f) << 4);
        pass.eot_offset = 0x300 + ((ordinal as u32 & 0x1f) << 4);
        pass.partial_eot_offset = 0x400 + ((ordinal as u32 & 0x1f) << 4);

        resources.kick_record_va = 0xffff_f000_0002_0000 + step;
        resources.scene_va = USER_BASE + 0x40_0000 + step;
        resources.pm_scratch = USER_BASE + 0x42_0000 + step;
        resources.pm_metric = USER_BASE + 0x43_0000 + step;
        resources.tilemap_va = USER_BASE + 0x50_0000 + step;
        resources.layermeta_va = USER_BASE + 0x51_0000 + step;
        resources.heapmeta_va = USER_BASE + 0x52_0000 + step;
        resources.deflake_va = USER_BASE + 0x70_0000 + step;
        resources.ta_status_va = 0xffff_f000_0003_0000 + step;
        resources.fragment_status_va = 0xffff_f000_0004_0000 + step;
        resources.fragment_status_fw_va = 0xffff_f000_0004_0000 + step;
        resources.aux_fb_va = USER_BASE + 0x80_0000 + step;
        resources.timestamp_va = 0xffff_f000_0005_0000 + step;
        resources.timestamp_window = [0x1000 + ordinal, 0x2000 + ordinal, 0x3000 + ordinal, 0x4000 + ordinal];
        resources.ta_ids = ObjectIds {
            predecessor: (ordinal as u32).wrapping_add(10),
            current: (ordinal as u32).wrapping_add(11),
        };
        resources.fragment_ids = ObjectIds {
            predecessor: (ordinal as u32).wrapping_add(20),
            current: (ordinal as u32).wrapping_add(21),
        };

        let ray = RayQueryStage {
            intersection: IntersectionTables::NONE,
            scratch: ScratchRequest::new(if scs { (1 << 32) | (1 << 24) | 8 } else { 0 }).unwrap(),
        };
        RenderArgs {
            pass,
            resources,
            ray_query: [ray; 2],
            ordinal,
            pair_idle: ordinal % 4 == 0,
            ta: RenderStage {
                qid: 5,
                descriptor_va: USER_BASE + 0x90_0000 + (ordinal % RING_SLOTS as u64) * 0x1000,
                state_word: (ordinal as u32) & 0x3f,
                stamp_va: 0xffff_f000_0006_0000,
                aux_stamp_va: 0xffff_f000_0006_0008,
            },
            fragment: RenderStage {
                qid: 6,
                descriptor_va: USER_BASE + 0xa0_0000 + (ordinal % RING_SLOTS as u64) * 0x3000,
                state_word: (ordinal as u32).wrapping_add(3) & 0x3f,
                stamp_va: 0xffff_f000_0006_0010,
                aux_stamp_va: 0xffff_f000_0006_0018,
            },
            programs,
            ta_mode_1: false,
            clusters: 2,
            deflake: Deflake::FOUR_CLUSTERS,
        }
    }

    #[test]
    fn render_template_matches_full_across_programs_scs_wraparound_and_mcache() {
        let mut ta_ring: Vec<KBox<TaDescriptor>> = (0..RING_SLOTS)
            .map(|_| zero_box())
            .collect();
        let mut frag_ring: Vec<KBox<FragmentSlot>> = (0..RING_SLOTS)
            .map(|_| zero_box())
            .collect();
        let mut ref_ta: KBox<TaDescriptor> = zero_box();
        let mut ref_frag: KBox<FragmentSlot> = zero_box();
        let mut slot = TemplateSlot::new_box().unwrap();
        let (mut pass, mut resources) = base_render();

        for programs in [RegisterPrograms::G17P, RegisterPrograms::V25G83] {
            for scs in [false, true] {
                for i in 0u64..80 {
                    let ring_idx = (i as usize) % RING_SLOTS;
                    let args = make_render_args(&mut pass, &mut resources, i, programs, scs);
                    let mc = match i % 7 {
                        0 => 0,
                        1 => 4,
                        2 => 1,
                        3 => 0,
                        4 => 7,
                        5 => 10,
                        _ => 2,
                    };
                    let mut atts = [WriteRange { va: 0, size: 0 }; 10];
                    for (k, a) in atts[..mc].iter_mut().enumerate() {
                        *a = WriteRange {
                            va: USER_BASE + 0xc0_0000 + (i + k as u64) * 0x4000,
                            size: 0x4000,
                        };
                    }
                    let (mcache, path) = encode(
                        &mut ta_ring[ring_idx],
                        &mut frag_ring[ring_idx],
                        &args,
                        &atts[..mc],
                        4,
                        &mut slot,
                    )
                    .unwrap();
                    let expected = match i {
                        0 => Path::Full,
                        1 => Path::Verified,
                        _ => Path::Patched,
                    };
                    assert_eq!(path, expected, "unexpected path at i={} {:?} scs={}", i, programs, scs);

                    ref_ta.write(&args).unwrap();
                    ref_frag.write(&args).unwrap();
                    let ref_mcache = ref_frag
                        .set_mcache(args.fragment.descriptor_va, args.resources.aux_fb_va, &atts[..mc], 4)
                        .unwrap();
                    assert_eq!(mcache.is_some(), ref_mcache.is_some());
                    assert_eq!(bytes_of(&*ta_ring[ring_idx]), bytes_of(&*ref_ta),
                        "TA diff at i={} {:?} scs={}", i, programs, scs);
                    assert_eq!(
                        bytes_of(&frag_ring[ring_idx].descriptor),
                        bytes_of(&ref_frag.descriptor),
                        "Frag diff at i={} {:?} scs={} mc={}", i, programs, scs, mc
                    );
                }
            }
        }
    }

    #[test]
    fn render_template_key_change_and_validation_error_parity() {
        let mut ta: KBox<TaDescriptor> = zero_box();
        let mut frag: KBox<FragmentSlot> = zero_box();
        let mut ref_ta: KBox<TaDescriptor> = zero_box();
        let mut ref_frag: KBox<FragmentSlot> = zero_box();
        let mut slot = TemplateSlot::new_box().unwrap();
        let (mut pass, mut resources) = base_render();

        for i in 0..3 {
            let args = make_render_args(&mut pass, &mut resources, i, RegisterPrograms::G17P, false);
            let _ = encode(&mut ta, &mut frag, &args, &[], 4, &mut slot).unwrap();
        }
        pass.width = 1280;
        let args = make_render_args(&mut pass, &mut resources, 3, RegisterPrograms::G17P, false);
        assert_eq!(encode(&mut ta, &mut frag, &args, &[], 4, &mut slot).unwrap().1, Path::Full);
        let args = make_render_args(&mut pass, &mut resources, 4, RegisterPrograms::G17P, false);
        assert_eq!(encode(&mut ta, &mut frag, &args, &[], 4, &mut slot).unwrap().1, Path::Verified);

        let mut bad_pass = pass;
        let mut bad_res = resources;
        let _ = make_render_args(&mut bad_pass, &mut bad_res, 5, RegisterPrograms::G17P, false);
        bad_pass.vdm_va = USER_BASE - 0x1000;
        let mut dummy_pass = pass;
        let mut dummy_res = resources;
        let mut bad_args = make_render_args(&mut dummy_pass, &mut dummy_res, 5, RegisterPrograms::G17P, false);
        bad_args.pass = &bad_pass;
        bad_args.resources = &bad_res;
        assert!(encode(&mut ta, &mut frag, &bad_args, &[], 4, &mut slot).is_err());
        assert!(ref_ta.write(&bad_args).is_err());

        let args = make_render_args(&mut pass, &mut resources, 6, RegisterPrograms::G17P, false);
        let (_, p) = encode(&mut ta, &mut frag, &args, &[], 4, &mut slot).unwrap();
        assert_eq!(p, Path::Full);
        ref_ta.write(&args).unwrap();
        ref_frag.write(&args).unwrap();
        assert_eq!(bytes_of(&*ta), bytes_of(&*ref_ta));
        assert_eq!(bytes_of(&frag.descriptor), bytes_of(&ref_frag.descriptor));
    }
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        td = Path(tmp)
        fw_dir = td / 'fw'
        fw_dir.mkdir()
        (fw_dir / 'kick.rs').write_text(fw_kick)
        (fw_dir / 'compute.rs').write_text(fw_compute)
        (fw_dir / 'render.rs').write_text(fw_render)
        (fw_dir / 'render_template.rs').write_text(fw_render_template)
        (td / 'helpers.rs').write_text(fw_helpers)
        (td / 'lib.rs').write_text(harness)
        bin_path = td / 'tests'
        subprocess.run(
            ['rustc', '--edition=2021', '--test', str(td / 'lib.rs'), '-o', str(bin_path)],
            check=True,
        )
        res = subprocess.run([str(bin_path), '--nocapture'], capture_output=True, text=True)
        if res.returncode != 0:
            print(res.stdout)
            print(res.stderr)
            raise SystemExit(res.returncode)
        print(res.stdout.strip().splitlines()[-1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutation', default=None)
    args = ap.parse_args()
    check_source_guards()
    build_and_run_rust_tests(args.mutation)


if __name__ == '__main__':
    main()
