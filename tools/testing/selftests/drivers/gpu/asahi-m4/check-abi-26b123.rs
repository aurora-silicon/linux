// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(dead_code,non_camel_case_types,non_snake_case,non_upper_case_globals,unused_imports)]
// Encodes the same InitData, work items and queue info for the 25G83 and 26B123 firmware
// ABIs with the production encoders, checks the layout catalog, and writes both encodings
// for test-abi-26b123.py's independent offset oracle.
extern crate self as kernel;
pub mod prelude { pub type Result<T=()> = core::result::Result<T,i32>; pub const E2BIG:i32=7; }
mod uapi {include!("public-uapi.rs");}
mod hw {
    pub enum GpuVariant {G}
    pub mod engine {pub const COMPUTE_SEED:u32=0x100;}
    pub mod client {pub const PB_HEAP_SIZE:u64=0x10000000;}
    pub(crate) use super::backend_hw::FwAbi;
}
mod fwid {
@FWID_BUILD@
}
struct Config {
    chip_id:u32,gpu_variant:hw::GpuVariant,gpu_revision:u32,uat_ias:u32,uat_oas:u32,
    num_clusters:u32,num_cores_per_cluster:u32,
    hw_ids:&'static[backend_hw::HwId],firmware:&'static[backend_hw::FirmwareBuild],
}
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/hw.rs"]
mod backend_hw;
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/fw/mod.rs"]
mod fw;
#[path = "@KERNEL@/drivers/gpu/drm/asahi/g16/encode.rs"]
mod encode;

use backend_hw::{FwAbi, LoaderTable, PplMagic, FW_25G83, FW_26B123, PPL_MAGIC, UPPER};
use fw::initdata::{self as wire, id};
use fw::work::{compute, fs, ta};

const HOST: u64 = wire::HOST_START;

/// An eleven-row (J773g-sized) table so the encoder fills every performance row.
fn input() -> wire::Input {
    let mut p = wire::Performance {
        count: 11,
        frequency: [0; 11],
        auxiliary: [0; 11],
        voltage: [0; 11],
        sram: [0; 11],
    };
    for i in 0..11 {
        p.frequency[i] = if i == 0 { 0 } else { 200 + 100 * i as u32 };
        p.auxiliary[i] = if i == 0 { 0 } else { 190 + 100 * i as u32 };
        p.voltage[i] = if i == 0 { 100 } else { 600 + 40 * i as u32 };
        p.sram[i] = 800 + 30 * i as u32;
    }
    wire::Input {
        performance: p,
        va: core::array::from_fn(|i| HOST + 0x100000 + i as u64 * 0x100000),
        channel_state: core::array::from_fn(|i| HOST + 0x4000000 + i as u64 * 0x10000),
        channel_ring: core::array::from_fn(|i| HOST + 0x4004000 + i as u64 * 0x10000),
        loader_va: core::array::from_fn(|i| {
            if wire::LOADERS[i].length != 0 {
                HOST + 0x5000000 + i as u64 * 0x100000 + (wire::LOADERS[i].pa & 0x3fff)
            } else {
                0
            }
        }),
        scratch: HOST + 0xa000000,
        lower_uma: wire::BOOTSTRAP_START + 0x200000,
        lower_parameter: wire::BOOTSTRAP_START + 0x240000,
        ktrace_mask: 0x12345678,
        progress_recovery: 1,
    }
}

fn initdata(layout: &wire::Layout, ceiling: u32) -> Result<Vec<u8>, ()> {
    let mut buffers: [Vec<u8>; wire::OBJECT_COUNT] =
        core::array::from_fn(|i| vec![0xa5u8; layout.object_size[i]]);
    let mut slices = buffers.each_mut().map(|b| b.as_mut_slice());
    wire::encode_for(layout, &mut slices, &input(), ceiling)?;
    Ok(buffers.into_iter().flatten().collect())
}

fn object<'a>(image: &'a [u8], layout: &wire::Layout, object: usize) -> &'a [u8] {
    let start: usize = layout.object_size[..object].iter().sum();
    &image[start..start + layout.object_size[object]]
}

fn r32(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn r64(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at + 8].try_into().unwrap()) }

fn render(out: &std::path::Path) {
    let f = 0xfffffc2000000000u64;
    let g = 0x1000000000u64;
    let mut u = uapi::drm_asahi_cmd_render::default();
    u.width_px = 1919; u.height_px = 1079; u.layers = 3; u.samples = 2;
    u.utile_width_px = 16; u.utile_height_px = 32; u.sample_size_B = 4;
    u.flags = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_RSRC_SPEC_HI;
    u.vdm_ctrl_stream_base = g + 0xa00000;
    u.depth.base = g + 0xe00000; u.stencil.base = g + 0xf00000;
    u.bg.usc = 0x10000; u.eot.usc = 0x20000;
    let r = encode::RenderRefs {
        notifier: f + 0x800000, qinfo: [f + 0xf00000, f + 0x1000000],
        user_stamp: [f + 0x800620, f + 0x800630], fw_stamp: [f + 0x4000000, f + 0x4004000],
        uma_mgr: [f + 0x300000, f + 0x300000], uma_aux: [f + 0x300100, f + 0x300200],
        uma_gpu: g + 0x6000000, pb_mgr: f + 0x3000000, scene_list: g + 0x500000,
        parameter_id: 3, generation: 254, activity: [f + 0x900004, f + 0xa00008],
        counter: [4, 3], uma_submitted: 4, uuid: 0x123401, usc_base: 1 << 40,
        ..Default::default()
    };
    let p = encode::PassRefs {
        ta_work: f + 0xb00000, ta_work_gva: g + 0x7000000, fs_work: f + 0xc00000,
        fs_work_gva: g + 0x8000000, ta_seq: f + 0xd00000, fs_seq: f + 0xe00000,
        scene: f + 0x100000, tpc_gva: g + 0x200000, tpc_fw: f + 0x3200000,
        tilemap_gva: g + 0x100000, heap_gva: g + 0x300000, scene_user_gva: g + 0x900000,
        scratch_fw: f + 0x200000, pre_gva: [g + 0x600000, g + 0x700000, g + 0x800000],
        aux_gva: g + 0xb00000, ..Default::default()
    };
    let geo = encode::Geometry::new(&u, p.heap_gva);
    let ts = [f + 0xffc000100, 0, 0, f + 0xffc000118];
    let mut images = Vec::new();
    for (ta_layout, fs_layout) in [(&ta::V25G83, &fs::V25G83), (&ta::V26B123, &fs::V26B123)] {
        let mut tw = [0xa5; ta::WORK_SIZE]; let mut tq = [0xa5; ta::SEQ_SIZE];
        let mut fw_ = [0xa5; fs::WORK_SIZE]; let mut fq = [0xa5; fs::SEQ_SIZE];
        encode::encode_ta_for(ta_layout, &r, &p, &u, &ts, 7, 0x300, &geo, None, &mut tw, &mut tq);
        encode::encode_fs_for(fs_layout, &r, &p, &u, &ts, 7, 1 << 40, 1, 0x300, &geo, &mut fw_, &mut fq);
        images.push((tw, tq, fw_, fq));
    }
    // The 25G83 entry points are the 25G83 layouts.
    let (mut tw, mut tq) = ([0; ta::WORK_SIZE], [0; ta::SEQ_SIZE]);
    let (mut fw_, mut fq) = ([0; fs::WORK_SIZE], [0; fs::SEQ_SIZE]);
    encode::encode_ta(&r, &p, &u, &ts, 7, 0x300, &geo, None, &mut tw, &mut tq);
    encode::encode_fs(&r, &p, &u, &ts, 7, 1 << 40, 1, 0x300, &geo, &mut fw_, &mut fq);
    assert!(tw == images[0].0 && tq == images[0].1 && fw_ == images[0].2 && fq == images[0].3);
    let (ref tw, ref tq, ref fw_, ref fq) = images[1];
    assert_eq!(r64(tw, 0x917), 254, "TA generation is a u64 at 0x917");
    assert_eq!(r64(tq, ta::FINALIZE + 0x85), p.ta_work + 0x91f);
    for record in [ta::TS_START, ta::TS_END] {
        assert_eq!(r64(tq, record + 0x2c), p.ta_work + 0x92f);
    }
    assert_eq!(r64(fw_, 0xc5f), 254, "FS generation is a u64 at 0xc5f");
    assert_eq!((fw_[0xc84], fw_[0xc8b]), (0, 1), "FS flag moved to 0xc8b");
    assert_eq!(r64(fq, fs::FINALIZE + 0xa5), p.fs_work + 0xc67);
    for record in [fs::TS_START, fs::TS_END] {
        assert_eq!(r64(fq, record + 0x2c), p.fs_work + 0xc77);
    }
    // The 26B123 timestamp op stores two u64s after the completion target: host-free.
    assert!(tw[0x927..0x937].iter().all(|&b| b == 0));
    assert!(fw_[0xc6f..0xc7f].iter().all(|&b| b == 0));
    for (name, (tw, tq, fw_, fq)) in ["25g83", "26b123"].iter().zip(&images) {
        let mut all = tw.to_vec(); all.extend(tq); all.extend(fw_); all.extend(fq);
        std::fs::write(out.join(format!("render-{name}.bin")), all).unwrap();
    }
}

fn compute_work(out: &std::path::Path) {
    let f = 0xfffffc2000000000u64;
    let g = 0x1000000000u64;
    let r = encode::ComputeRefs {
        notifier: f + 0x600400, qinfo: f + 0x200000, user_stamp: f + 0x600648,
        fw_stamp: f + 0x600640, work: f + 0x100000, work_gva: g + 0x100000, seq: f + 0x100900,
        preempt_gva: g + 0x200000, its: f + 0x500040, stats_fw: f + 0x300000,
        auxiliary_fw: f + 0x400000, auxiliary_gva: g + 0x400000,
        context_store_gva: g + 0x500000, counter_fw: f + 0x500000,
        pool_state_gva: g + 0x600000, page_list_gva: g + 0x700000, usc_base: 1 << 40,
        record_fw: f + 0x700000, binding_generation: 200, uma_slot: 3, command_id: 9,
        compute_index: 8, work_818: 0, target_stamp: 0x900, submitted: 5,
    };
    let c = uapi::drm_asahi_cmd_compute {
        cdm_ctrl_stream_base: g + 0x300000, cdm_ctrl_stream_end: g + 0x300200,
        ..Default::default()
    };
    let att = [uapi::drm_asahi_attachment { pointer: g + 0x50000000, size: 137, ..Default::default() }];
    let ts = [0xfffffc2ffc000100, 0];
    let mut images = Vec::new();
    for layout in [&compute::V25G83, &compute::V26B123] {
        let mut w = [0xa5; compute::WORK_SIZE]; let mut q = [0xa5; compute::SEQ_SIZE];
        encode::encode_compute_for(layout, &r, &c, &att, &ts, 1, 1, 1, &mut w, &mut q).unwrap();
        images.push((w, q));
    }
    let (mut w, mut q) = ([0; compute::WORK_SIZE], [0; compute::SEQ_SIZE]);
    encode::encode_compute(&r, &c, &att, &ts, 1, 1, 1, &mut w, &mut q).unwrap();
    assert!(w == images[0].0 && q == images[0].1);
    let (ref w, ref q) = images[1];
    assert_eq!(r64(w, 0x88b), 200, "compute generation is a u64 at 0x88b");
    assert_eq!(r64(q, 0x1a8), r.work + 0x8b3);
    assert_eq!(r64(q, compute::FINALIZE + 0x69), r.work + 0x8b3);
    assert_eq!(r64(q, compute::FINALIZE + 0x71), r.work + 0x893);
    for record in [compute::TS_START, compute::TS_END] {
        assert_eq!(r64(q, record + 0x2c), r.work + 0x8a3);
    }
    assert!(w[0x89b..0x8ab].iter().all(|&b| b == 0));
    assert!(w[0x8e0..compute::SEQ_EMBEDDED].iter().all(|&b| b == 0), "beyond the cached prefix");
    assert_eq!(&w[compute::SEQ_EMBEDDED..compute::SEQ_EMBEDDED + compute::SEQ_SIZE], &q[..]);
    for (name, (w, q)) in ["25g83", "26b123"].iter().zip(&images) {
        let mut all = w.to_vec(); all.extend(q);
        std::fs::write(out.join(format!("compute-{name}.bin")), all).unwrap();
    }
}

fn queues(out: &std::path::Path) {
    let f = 0xfffffc2000000000u64;
    let args = (f + 0x100000, f + 0x200000, f + 0x400000, f + 0x300000, f + 0x500000);
    let v25 = fw::graph::queue_image_for(FwAbi::V25G83.qinfo(), args.0, args.1, args.2, args.3, args.4);
    assert_eq!(v25, fw::graph::queue_image(args.0, args.1, args.2, args.3, args.4));
    let v26 = fw::graph::queue_image_for(FwAbi::V26B123.qinfo(), args.0, args.1, args.2, args.3, args.4);
    assert_eq!(r64(&v26, 0x18), args.3 + 0x80, "scratch after the 0x80 header");
    assert_eq!((r32(&v26, 0x44), r32(&v26, 0x48)), (2, u32::MAX));
    assert_eq!(r64(&v26, 0x78), args.4, "aligned scheduling context");
    assert!(v26[0x80..].iter().all(|&b| b == 0));
    let mut all = v25.to_vec(); all.extend(v26);
    std::fs::write(out.join("queue.bin"), all).unwrap();
}

fn catalog() {
    // Handoff selection and what a 26B123 start still needs.
    assert!(FW_25G83.matches(1, &[UPPER, UPPER + 0x60000], &[1, 0]));
    assert!(FW_26B123.matches(2, &[UPPER, UPPER + 0x64000, UPPER + 0x134000], &[1, 0x10, 0]));
    assert!(!FW_25G83.matches(2, &[UPPER, UPPER + 0x64000, UPPER + 0x134000], &[1, 0x10, 0]));
    assert!(!FW_26B123.matches(2, &[UPPER, UPPER + 0x64000], &[1, 0x10]));
    assert_eq!((FW_25G83.abi, FW_26B123.abi), (FwAbi::V25G83, FwAbi::V26B123));
    assert_eq!(FW_25G83.unknown().count(), 0);
    // Nothing known to be missing refuses a 26B123 start any more.
    let unknown: Vec<_> = FW_26B123.unknown().collect();
    assert!(unknown.is_empty(), "{unknown:?}");
    assert_eq!(FW_25G83.loader_table, Some(LoaderTable::Host(0x1332f0)));
    assert_eq!(FW_26B123.loader_table, Some(LoaderTable::Firmware));
    assert_eq!(PPL_MAGIC, 0x4b1d000000000002);
    assert_eq!(FW_25G83.ppl_magic, Some(PplMagic { ap: PPL_MAGIC, firmware: PPL_MAGIC, answers: false }));
    assert_eq!(FW_26B123.ppl_magic, Some(PplMagic { ap: 0x4b1d000000000003, firmware: 0x4b1d000000000003, answers: true }));
    // The 26B123 firmware builds its own page-table tree: the boot loader builds no leaf.
    assert_eq!(FW_25G83.leaves.map(|l| l.len()), Some(5));
    assert_eq!(FW_26B123.leaves.map(|l| l.len()), Some(0));
    // Device control.
    let (a, b) = (FwAbi::V25G83.devctl(), FwAbi::V26B123.devctl());
    assert_eq!((a.import_globals, a.start, a.nop, a.grow_reply), (0x34, 0x0a, 0x1a, 7));
    assert_eq!((b.import_globals, b.start, b.nop, b.grow_reply), (0x35, 0x0a, 0x19, 7));
    // FirmwareData, selectors, register map.
    let (v25, v26) = (FwAbi::V25G83.initdata(), FwAbi::V26B123.initdata());
    let (d25, d26) = (&v25.firmware_data, &v26.firmware_data);
    assert_eq!((d25.fwctl, d25.generation, d25.phase, d25.scheduler, d25.snapshot), (0x4f88, 0x4fa0, 0x4fb0, 0x4fd4, 0x40b8));
    assert_eq!((d26.fwctl, d26.generation, d26.phase, d26.scheduler, d26.snapshot), (0x5168, 0x5180, 0x5190, 0x51b4, 0x4298));
    assert_eq!(d25.snapshot + d25.snapshot_entries * 8, 0x4eb8);
    assert_eq!(v25.pstate_selectors, [0x88, 0x8c, 0x98]);
    assert_eq!(v26.pstate_selectors, [0x8c, 0x90, 0x9c]);
    for (i, &at) in v25.pstate_selectors.iter().enumerate() {
        assert_eq!(wire::remap(v26.globals, at, 4), Ok(v26.pstate_selectors[i]));
    }
    assert_eq!((v25.loader_records(), v26.loader_records()), (42, 47));
    assert_eq!((v25.register_slots, v26.register_slots), (53, 60));
    assert_eq!((v25.power_config, v26.power_config), (0x469, 0x479));
    assert_eq!(v25.object_size, wire::OBJECT_SIZE);
    // Words whose 26B123 position is not known are refused rather than guessed.
    assert_eq!(wire::remap(v26.main, 0x25b8, 4), Err(()));
    assert_eq!(wire::remap(v26.main, 0xebc, 4), Err(()));
    assert_eq!(wire::remap(v26.main, 0x700, 4), Err(()), "the register map is slot-mapped");
    assert_eq!(wire::remap(v26.power_performance, 0x2e00, 4), Err(()));
    assert_eq!(wire::remap(v26.power_performance, 0x62c8, 4), Err(()));
    assert_eq!(wire::remap(v26.globals, 0x36, 4), Err(()), "a field may not straddle an insert");
    // A filled record this ABI has no slot for refuses the encoding.
    let mut no_slot = wire::Layout { name: "test", ..wire::V26B123 };
    no_slot.loader_slot[3] = None;
    assert!(initdata(&no_slot, 10).is_err());
    let mut past_map = wire::Layout { name: "test", ..wire::V26B123 };
    past_map.loader_slot[41] = Some(60);
    assert!(initdata(&past_map, 10).is_err());
}

fn main() {
    let out = std::path::PathBuf::from(std::env::args().nth(1).unwrap());
    catalog();
    let v25 = initdata(&wire::V25G83, 10).unwrap();
    // The 25G83 entry point is the 25G83 layout.
    let mut buffers = wire::OBJECT_SIZE.map(|size| vec![0xa5u8; size]);
    let mut slices = buffers.each_mut().map(|b| b.as_mut_slice());
    wire::encode(&mut slices, &input(), 10).unwrap();
    assert_eq!(buffers.concat(), v25);
    let v26 = initdata(&wire::V26B123, 10).unwrap();
    let l = &wire::V26B123;
    assert_eq!(r64(object(&v26, l, id::ROOT), 0), 0x0c8a836f039204b8);
    let runtime = object(&v26, l, id::RUNTIME);
    assert_eq!(r64(runtime, 0x479), input().va[id::POWER_PERFORMANCE]);
    assert!(runtime[0x469..0x479].iter().all(|&b| b == 0));
    let data = object(&v26, l, id::FIRMWARE_DATA);
    assert_eq!((r64(data, 0x5168), r64(data, 0x5170), r32(data, 0x51b4)),
               (input().va[id::FWCTL], input().va[id::FWCTL] + 0x40, 0));
    let main = object(&v26, l, id::MAIN);
    assert_eq!(r64(main, 0xfa0), input().va[id::RETENTION]);
    assert_eq!(r64(main, 0xfa8), 0);
    // CRE (25G83 slot 28) is register-map slot 33 in 26B123.
    assert_eq!(r64(main, 0x640 + 33 * 0x28), 0x300d58000);
    assert!(main[0x640 + 28 * 0x28..0x640 + 29 * 0x28].iter().all(|&b| b == 0));
    let power = object(&v26, l, id::POWER_PERFORMANCE);
    assert_eq!(r64(power, 0x3b98), input().va[id::TEMPERATURE]);
    std::fs::write(out.join("initdata-25g83.bin"), &v25).unwrap();
    std::fs::write(out.join("initdata-26b123.bin"), &v26).unwrap();
    render(&out);
    compute_work(&out);
    queues(&out);
    println!("26B123 layout catalog, InitData ({} / {} bytes), TA/FS/compute work and queue info encode; 25G83 entry points equal the 25G83 layouts", v25.len(), v26.len());
}
