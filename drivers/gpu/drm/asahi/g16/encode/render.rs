// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 25G83 HAL200 render wire records. Inputs are frozen, retained addresses;
//! these builders neither allocate nor publish queue/notifier state.

use super::*;

/// Persistent operands of a paired TA/FS submission.
#[derive(Copy, Clone, Default)]
pub(crate) struct RenderRefs {
    pub(crate) notifier: u64,
    #[allow(dead_code)]
    pub(crate) legacy_empty: u64,
    pub(crate) qinfo: [u64; 2],
    /// Actual notifier stamp cells and runtime stage cells, respectively.
    pub(crate) user_stamp: [u64; 2],
    pub(crate) fw_stamp: [u64; 2],
    /// Both entries name the one retained M4 UMA descriptor.
    pub(crate) uma_mgr: [u64; 2],
    /// Firmware aliases of the two GPU-writable auxiliary records; never manager metadata.
    pub(crate) uma_aux: [u64; 2],
    pub(crate) uma_gpu: u64,
    pub(crate) pb_mgr: u64,
    /// GPU VA of the scene list, not its firmware alias.
    pub(crate) scene_list: u64,
    #[allow(dead_code)]
    pub(crate) mgr_misc: u64,
    #[allow(dead_code)]
    pub(crate) pb_init_blocks: u32,
    pub(crate) parameter_id: u32,
    pub(crate) generation: u8,
    /// Final activity record interiors: TA +4, FS +8.
    pub(crate) activity: [u64; 2],
    /// Frozen 64-bit UMA counters; TA is the even member, FS the preceding odd.
    pub(crate) counter: [u64; 2],
    pub(crate) uma_submitted: u64,
    pub(crate) uuid: u32,
    pub(crate) usc_base: u64,
}

/// Addresses retained for one render pass.
#[derive(Copy, Clone, Default)]
pub(crate) struct PassRefs {
    pub(crate) ta_work: u64,
    pub(crate) ta_work_gva: u64,
    pub(crate) fs_work: u64,
    pub(crate) fs_work_gva: u64,
    pub(crate) ta_seq: u64,
    pub(crate) fs_seq: u64,
    pub(crate) scene: u64,
    pub(crate) tpc_gva: u64,
    pub(crate) tpc_fw: u64,
    pub(crate) tilemap_gva: u64,
    /// Scene metadata GPU VA, including layer metadata at +0x200.
    pub(crate) heap_gva: u64,
    /// Scratch GPU VA; scratch_fw is a same-backing firmware alias.
    pub(crate) scene_user_gva: u64,
    pub(crate) scratch_fw: u64,
    pub(crate) pre_gva: [u64; 3],
    #[allow(dead_code)]
    pub(crate) fs_scratch_gva: u64,
    pub(crate) aux_gva: u64,
    #[allow(dead_code)]
    pub(crate) its: u64,
    #[allow(dead_code)]
    pub(crate) ta_scratch: u64,
}

fn metadata(w: &mut [u8], off: usize, r: &RenderRefs, engine: usize, target: u32) {
    put64(w, off, r.user_stamp[engine]);
    put64(w, off + 8, r.fw_stamp[engine]);
    put32(w, off + 0x10, target);
    put32(w, off + 0x14, 0x70 + engine as u32);
    put32(w, off + 0x1c, 1);
    put32(w, off + 0x20, r.uuid);
    // Wire sequence stays zero; host reservations are 64-bit notifier state.
}

fn inline_uma(q: &mut [u8], off: usize, r: &RenderRefs, engine: usize) {
    put64(q, off, r.uma_mgr[0]);
    put64(
        q,
        off + 0x18,
        r.uma_gpu + if engine == 0 { 0x100 } else { 0x200 },
    );
    put64(q, off + 0x28, r.counter[engine]);
    put32(q, off + 0x30, 0x70 + engine as u32);
    put8(q, off + 0x34, engine as u8);
}

fn timestamps(
    q: &mut [u8],
    off: usize,
    work: u64,
    queue: u64,
    user: bool,
    uuid: u32,
    engine: usize,
) {
    let (control, pair, auxiliary, context, user_pair) = if engine == 0 {
        (0x8c0, 0x8c8, 0x928, 0x83c, 0x8d8)
    } else {
        (0xc08, 0xc10, 0xc70, 0xb84, 0xc20)
    };
    for end in [false, true] {
        let base = off + if end { 0x50 } else { 0 };
        put32(q, base, if end { 3 } else { 0x80000003 });
        put64(q, base + 4, work + control);
        put64(q, base + 0xc, work + pair);
        put64(q, base + 0x14, work + pair + if end { 8 } else { 0 });
        put64(q, base + 0x1c, queue);
        put64(q, base + 0x24, if user { work + user_pair } else { 0 });
        put64(q, base + 0x2c, work + auxiliary);
        put64(q, base + 0x34, work + context);
        put64(q, base + 0x44, u64::from(uuid));
    }
    put32(q, off + 0x4c, 1);
}

fn internal_pair(w: &mut [u8], p: &PassRefs, engine: usize, ts: &[u64; 4]) {
    let off = if engine == 0 { 0x8c8 } else { 0xc10 };
    let result = p.scene + 0x600 + 0x20 * engine as u64;
    put64(w, off, result);
    put64(w, off + 8, result + 8);
    put64(w, off + 0x10, ts[2 * engine]);
    put64(w, off + 0x18, ts[2 * engine + 1]);
}

/// Encode the 66-record single-cluster tiler work and its exact 0x300 sequence.
#[allow(clippy::too_many_arguments)]
pub(crate) fn encode_ta(
    r: &RenderRefs,
    p: &PassRefs,
    u: &uapi::drm_asahi_cmd_render,
    ts_va: &[u64; 4],
    ctx: u32,
    target: u32,
    geo: &Geometry,
    _cl: Option<&ClusterLayout>,
    w: &mut [u8; ta::WORK_SIZE],
    q: &mut [u8; ta::SEQ_SIZE],
) {
    w.fill(0);
    q.fill(0);
    let scratch = p.scene_user_gva;
    let scratch_word = 1
        | ((scratch.wrapping_add(if scratch & (1 << 42) != 0 {
            0
        } else {
            0x7000000000
        })) & 0x7ffffffffe)
        | ((scratch & (1 << 42)) >> 3);
    let val: [u64; 66] = [
        1,
        0x200,
        compact(p.tilemap_gva),
        compact(p.tilemap_gva),
        compact(p.tpc_gva),
        compact(p.heap_gva) | 1 << 63,
        compact(p.heap_gva) | 1 << 63,
        0x003a0012006b0003,
        1,
        u64::from(geo.utile_config),
        u.ppp_multisamplectl,
        compact(p.pre_gva[0]),
        compact(p.pre_gva[0]),
        compact(p.pre_gva[1]),
        compact(p.pre_gva[1]),
        1,
        compact(p.pre_gva[2]) | 1 << 50,
        0,
        compact(u.vdm_ctrl_stream_base) & !3,
        compact(geo.layer_metadata),
        compact(geo.layer_metadata),
        0,
        0,
        0,
        0,
        0,
        0,
        0x8860,
        u64::from(geo.region_stride),
        u64::from(geo.region_stride),
        0x88,
        u64::from(2 | (u.ppp_ctrl & 0x201)),
        u64::from(geo.pixels),
        u64::from(geo.screen),
        geo.x_blocks,
        geo.y_blocks,
        u64::from(geo.blocks),
        u64::from(geo.tpc_stride),
        0x100,
        u64::from(geo.layer_mode),
        0,
        u64::MAX,
        u64::MAX,
        0,
        0x1c,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        8,
        0xff0000,
        u64::from(r.parameter_id),
        compact(r.scene_list),
        compact(r.scene_list),
        scratch_word,
        0x6000400020,
        0x200000001,
        u64::from(r.uuid),
        u64::from(r.uuid),
        u64::from(r.uuid),
        // USC base is a shared queue property carried by the render refs.
        r.usc_base,
    ];
    regs(&mut w[ta::REGS..], &ta::SEL, &val);
    put64(w, 4, r.counter[0]);
    put32(w, 0xc, ctx);
    put64(w, 0x10, r.notifier);
    put32(w, 0x18, r.parameter_id);
    put64(w, 0x20, r.pb_mgr);
    put64(w, 0x28, p.scene);
    put64(w, 0x30, p.scene + 0x100);
    put64(w, 0x740, p.ta_work_gva + 0x40);
    put16(w, 0x748, 66);
    put16(w, 0x74a, 66 * 12);
    put64(w, 0x760, p.tpc_fw);
    put64(w, ta::TPC_BYTES, geo.tpc_bytes);
    put64(w, 0x770, p.ta_seq);
    put32(w, 0x778, ta::SEQ_SIZE as u32);
    put32(w, 0x77c, 0x71);
    put32(w, 0x780, target);
    put64(w, 0x7a8, compact(p.pre_gva[0]));
    metadata(w, 0x878, r, 0, target);
    internal_pair(w, p, 0, ts_va);
    put64(w, 0x8fe, r.uma_mgr[0]);
    put8(w, 0x906, 1);
    put64(w, 0x90f, r.uma_aux[0]);
    put8(w, 0x917, r.generation);

    let work = p.ta_work;
    put32(q, 0, 5);
    put64(q, 0x14, work + 0x40);
    put64(q, 0x1c, r.pb_mgr);
    put64(q, 0x24, p.scene);
    put64(q, 0x2c, r.activity[0]);
    put64(q, 0x34, r.qinfo[0]);
    put64(q, 0x3c, work + 0x85c);
    put32(q, 0x44, ctx);
    put32(q, 0x48, 1);
    put32(q, 0x50, r.parameter_id);
    put64(q, 0x64, work + 0x784);
    put64(q, 0x6c, work + 0x8a8);
    put32(q, 0x7c, r.uuid);
    // TA attachments are filled by render_attachments before publication.
    inline_uma(q, 0x190, r, 0);
    timestamps(
        q,
        0x1cc,
        work,
        r.qinfo[0],
        ts_va[0] != 0 || ts_va[1] != 0,
        r.uuid,
        0,
    );
    let f = 0x268;
    put32(q, f, 6);
    put64(q, f + 4, p.scene);
    put64(q, f + 0xc, r.pb_mgr);
    put64(q, f + 0x14, r.activity[0]);
    put64(q, f + 0x1c, r.qinfo[0]);
    put64(q, f + 0x24, work + 0x85c);
    put32(q, f + 0x2c, ctx);
    put64(q, f + 0x34, work + 0x784);
    put32(q, f + 0x40, r.uuid);
    put64(q, f + 0x48, r.fw_stamp[0]);
    put32(q, f + 0x50, target);
    put64(q, f + 0x78, p.ta_seq + 0x190);
    put32(q, f + 0x80, 0u32.wrapping_sub(f as u32));
    put64(q, f + 0x85, work + 0x918);
    put8(q, f + 0x8d, 1);
    put32(q, f + 0x94, 0x40000002);
}

/// Encode the 82-record fragment work and base 0x380 sequence.
#[allow(clippy::too_many_arguments)]
pub(crate) fn encode_fs(
    r: &RenderRefs,
    p: &PassRefs,
    u: &uapi::drm_asahi_cmd_render,
    ts_va: &[u64; 4],
    ctx: u32,
    usc: u64,
    _n: u64,
    target: u32,
    geo: &Geometry,
    w: &mut [u8; fs::WORK_SIZE],
    q: &mut [u8],
) {
    w.fill(0);
    q.fill(0);
    let val: [u64; 82] = [
        1,
        u64::from(geo.utile_config),
        prog_rsrc(u, &u.eot, u.bg_eot_rsrc_spec_hi),
        prog_addr(usc, &u.eot),
        prog_rsrc(u, &u.bg, u.bg_rsrc_spec_hi),
        prog_addr(usc, &u.bg),
        u64::from(u.isp_merge_upper_x),
        u64::from(u.isp_merge_upper_y),
        0,
        0,
        0,
        0,
        u.ppp_multisamplectl,
        u64::from(geo.isp_blocks),
        u64::from(geo.isp_blocks),
        u64::from(geo.screen),
        0,
        0xff0200,
        p.heap_gva & !63,
        u.isp_scissor_base & !3,
        u.isp_dbias_base & !3,
        u64::from(geo.isp_control),
        u64::from(u.height_px) << 32 | u64::from(u.width_px),
        0x100000,
        u64::from(geo.tilebuffer_control),
        u64::from(u.isp_zls_pixels),
        u64::from(u.isp_bgobjdepth),
        u64::from(u.isp_bgobjvals),
        u.isp_oclqry_base & !15,
        u.zls_ctrl,
        0x04040404,
        0,
        u.depth.base,
        u.depth.base,
        u.stencil.base,
        u.stencil.base,
        0,
        0,
        0,
        0,
        u64::from(u.depth.stride),
        u64::from(u.depth.stride),
        u64::from(u.stencil.stride),
        u64::from(u.stencil.stride),
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        p.tilemap_gva,
        geo.layer_metadata,
        u64::from(geo.region_stride) << 26,
        u64::from(geo.tile_mode),
        0,
        0,
        0,
        0x8860,
        p.aux_gva,
        p.aux_gva,
        0x1c,
        0,
        geo.native_tiles,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        8,
        0x6000400020,
        0x200000001,
        u64::from(r.uuid),
        u64::from(r.uuid),
        u64::from(r.uuid),
        u64::from(r.parameter_id),
        compact(r.scene_list),
        usc,
    ];
    regs(&mut w[fs::REGS..], &fs::SEL, &val);
    put32(w, 0, 1);
    put64(w, 4, r.counter[1]);
    put32(w, 0xc, ctx);
    put64(w, 0x14, p.fs_seq);
    put32(w, 0x1c, fs::SEQ_SIZE as u32);
    put64(w, 0x20, r.notifier + 0x200);
    put64(w, 0x28, r.pb_mgr);
    put64(w, 0x30, p.scene);
    put64(w, 0x38, p.scene + 0x100);
    put64(w, 0x40, p.tilemap_gva);
    put32(w, 0x50, 1);
    put32(w, 0x54, geo.isp_blocks);
    put32(w, 0x60, geo.x_blocks as u32);
    put32(w, 0x64, geo.y_blocks as u32);
    put32(w, 0x68, geo.blocks);
    put32(w, 0x78, geo.tiles);
    put64(w, 0x780, p.fs_work_gva + 0x80);
    put16(w, 0x788, 82);
    put16(w, 0x78a, 82 * 12);
    put64(w, 0x7a0, u.isp_dbias_base);
    put64(w, 0x7b0, u.isp_scissor_base);
    put64(w, 0x7c0, u.isp_oclqry_base);
    put64(w, 0x8f8, prog_rsrc(u, &u.bg, u.bg_rsrc_spec_hi));
    put64(w, 0x900, prog_addr(usc, &u.bg));
    put64(
        w,
        0x928,
        prog_rsrc(u, &u.partial_bg, u.bg_partial_rsrc_spec_hi),
    );
    put64(w, 0x930, prog_addr(usc, &u.partial_bg));
    put64(w, 0x938, u.zls_ctrl);
    put64(w, 0x940, 0x04040404);
    put64(w, 0x948, u.depth.base);
    put64(w, 0x950, u64::from(u.depth.stride));
    put64(w, 0x960, u.depth.base);
    put64(w, 0x968, u.depth.base);
    put64(w, 0x978, u.stencil.base);
    put64(w, 0x980, u64::from(u.stencil.stride));
    put64(w, 0x990, u.stencil.base);
    put64(w, 0x998, u.stencil.base);
    put64(w, 0x9b8, u64::from(geo.tilebuffer_control));
    put64(w, 0x9c0, u64::from(geo.isp_control));
    put64(
        w,
        0x9c8,
        u64::from(u.height_px) << 32 | u64::from(u.width_px),
    );
    put64(w, 0x9d0, 0x100000);
    put64(w, 0x9d8, u64::from(geo.tile_mode));
    put64(w, 0x9f4, prog_rsrc(u, &u.eot, u.bg_eot_rsrc_spec_hi));
    put64(w, 0x9fc, prog_addr(usc, &u.eot));
    put64(
        w,
        0xa14,
        prog_rsrc(u, &u.partial_eot, u.bg_eot_partial_rsrc_spec_hi),
    );
    put64(w, 0xa1c, prog_addr(usc, &u.partial_eot));
    put64(
        w,
        0xa28,
        u64::from(u.isp_bgobjvals) << 32 | u64::from(u.isp_bgobjdepth),
    );
    put64(w, 0xa50, u64::from(u.isp_zls_pixels));
    metadata(w, 0xbc0, r, 1, target);
    internal_pair(w, p, 1, ts_va);
    put64(w, 0xc46, r.uma_mgr[0]);
    put8(w, 0xc4e, 1);
    put64(w, 0xc57, r.uma_aux[1]);
    put8(w, 0xc5f, r.generation);
    put8(w, 0xc84, 1);

    let work = p.fs_work;
    put32(q, 0, 7);
    put64(q, 0x14, work + 0x80);
    put64(q, 0x1c, p.scene);
    put64(q, 0x24, r.activity[1]);
    put64(q, 0x2c, work + 0xb70);
    put64(q, 0x34, p.scene + 0x54);
    put64(q, 0x3c, work + 0xbb0);
    put64(q, 0x44, work + 0xbb4);
    put64(q, 0x4c, r.qinfo[1]);
    put64(q, 0x54, work);
    put32(q, 0x5c, ctx);
    put32(q, 0x60, 1);
    put32(q, 0x68, r.parameter_id);
    put64(q, 0x7c, work + 0xa58);
    put64(q, 0x84, work + 0xbf0);
    put32(q, 0xa0, r.uuid);
    inline_uma(q, 0x1b0, r, 1);
    timestamps(
        q,
        0x1ec,
        work,
        r.qinfo[1],
        ts_va[2] != 0 || ts_va[3] != 0,
        r.uuid,
        1,
    );
    let f = 0x288;
    put32(q, f, 8);
    put32(q, f + 4, r.uuid);
    put64(q, f + 0xc, r.fw_stamp[1]);
    put32(q, f + 0x14, target);
    put64(q, f + 0x1c, p.scene);
    put64(q, f + 0x24, r.pb_mgr);
    put32(q, f + 0x2c, 1);
    put64(q, f + 0x30, r.activity[1]);
    put64(q, f + 0x38, work + 0xbb0);
    put64(q, f + 0x40, work + 0xbb4);
    put64(q, f + 0x48, work + 0xb70);
    put64(q, f + 0x50, r.qinfo[1]);
    put64(q, f + 0x58, work);
    put32(q, f + 0x60, ctx);
    put64(q, f + 0x64, work + 0xa58);
    put64(q, f + 0x98, p.fs_seq + 0x1b0);
    put32(q, f + 0xa0, 0u32.wrapping_sub(f as u32));
    put64(q, f + 0xa5, work + 0xc60);
    put32(q, f + 0xad, 1);
    put8(q, f + 0xb1, 1);
    put32(q, f + 0xb2, 0x71);
    put32(q, f + 0xbc, 0x40000002);
}

/// Complete attachment blocks include the count at the last word. The scheduler
/// has validated full retained ranges; alignment is for firmware cache lines.
pub(crate) fn render_attachments(q: &mut [u8], engine: usize, att: &[uapi::drm_asahi_attachment]) {
    let base = if engine == 0 { 0x88 } else { 0xa8 };
    for (i, a) in att.iter().enumerate().take(16) {
        let start = a.pointer & !127;
        let end = align(a.pointer.wrapping_add(a.size), 128);
        let off = base + 16 * i;
        put64(q, off, start);
        put32(q, off + 8, (end.wrapping_sub(start) / 128) as u32);
        put16(q, off + 0xc, 0x18);
        put16(q, off + 0xe, 1);
    }
    put32(q, base + 0x100, att.len().min(16) as u32);
    let f = if engine == 0 {
        ta::FINALIZE + 0x84
    } else {
        fs::FINALIZE + 0xa4
    };
    put8(q, f, u8::from(!att.is_empty()));
}

/// Base scene metadata in caller-owned storage. A full scene belongs to the
/// retained slot and should not be placed on the kernel stack.
pub(crate) fn fill_scene(r: &RenderRefs, p: &PassRefs, out: &mut [u8]) {
    let Some(out) = out.get_mut(..scene::SIZE) else {
        return;
    };
    out.fill(0);
    put64(out, 0, p.scene_user_gva);
    put64(out, 8, p.scratch_fw);
    put64(out, 0x28, compact(r.scene_list));
    put64(out, 0x40, p.scene + 0x200);
    put64(out, 0x500, r.uma_submitted);
}

/// Small scene header for direct writes into a zeroed retained firmware object.
#[allow(dead_code)]
pub(crate) fn scene(r: &RenderRefs, p: &PassRefs, _n: u64) -> [u8; 0x80] {
    let mut out = [0u8; 0x80];
    put64(&mut out, 0, p.scene_user_gva);
    put64(&mut out, 8, p.scratch_fw);
    put64(&mut out, 0x28, compact(r.scene_list));
    put64(&mut out, 0x40, p.scene + 0x200);
    out
}

/// Populate the scene-owned InitBM and own-TA wait before exposing either work.
pub(crate) fn scene_render(
    r: &RenderRefs,
    p: &PassRefs,
    ts: &[u64; 4],
    ctx: u32,
    target: u32,
    init: bool,
    out: &mut [u8],
) {
    if init {
        put(out, 0x300, &initbm(ctx, r.parameter_id, r.pb_mgr, target));
    }
    put(
        out,
        0x340,
        &barrier(
            r.fw_stamp[0],
            target,
            0x70,
            target,
            0,
            work::BarrierType::Stamp,
        ),
    );
    for (i, value) in ts.iter().enumerate() {
        put64(out, 0x610 + i / 2 * 0x20 + i % 2 * 8, *value);
    }
    let _ = p;
}

/// Fresh PB initialization. The zero +0xc/+0x10 request a firmware rebuild.
pub(crate) fn initbm(ctx: u32, parameter_id: u32, pb_mgr: u64, target: u32) -> [u8; initbm::SIZE] {
    let mut x = [0u8; initbm::SIZE];
    put32(&mut x, 0, 6);
    put32(&mut x, 4, ctx);
    put32(&mut x, 8, parameter_id);
    put64(&mut x, 0x14, pb_mgr);
    put32(&mut x, 0x1c, target);
    x
}

/// Additional API fragment-end copies. Returns None without touching output if
/// the fixed eight-copy grammar or retained sequence allocation cannot fit.
#[allow(dead_code)]
pub(crate) fn append_fs_timestamp_copies(
    w: &mut [u8; fs::WORK_SIZE],
    q: &mut [u8],
    p: &PassRefs,
    copies: &[u64; 8],
    scene: &mut [u8],
) -> Option<usize> {
    let count = copies.iter().filter(|&&va| va != 0).count();
    let length = fs::SEQ_SIZE + count * 0x4c;
    if q.len() < length || scene.len() < 0x780 {
        return None;
    }
    if count == 0 {
        return Some(length);
    }
    q.copy_within(0x288..fs::SEQ_SIZE, 0x288 + count * 0x4c);
    let mut record = [0u8; 0x4c];
    record.copy_from_slice(&q[0x23c..0x288]);
    let mut ordinal = 0;
    for (i, &va) in copies.iter().enumerate() {
        if va == 0 {
            continue;
        }
        put64(scene, 0x708 + i * 16, va);
        put64(&mut record, 0x24, p.scene + 0x700 + i as u64 * 16);
        put(q, 0x288 + ordinal * 0x4c, &record);
        ordinal += 1;
    }
    put32(
        q,
        0x328 + count * 0x4c,
        0u32.wrapping_sub((0x288 + count * 0x4c) as u32),
    );
    put32(w, 0x1c, length as u32);
    Some(length)
}
