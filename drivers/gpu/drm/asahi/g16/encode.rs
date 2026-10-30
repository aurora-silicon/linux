// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 25G83 HAL200 work-image and microsequence encoders.
//!
//! Pure byte builders over retained firmware/client addresses. Allocation,
//! address-range admission, owner leases, cache visibility and publication are
//! performed by the engine; these routines never mutate live allocator state.

use kernel::{prelude::*, uapi};

use super::fw::graph::manager;
use super::fw::work::{self, barrier, compute, fs, initbm, scene, ta, REG_RECORD};
use super::hw;

/// Stores `v` little-endian at `off`. Every offset is a literal inside a fixed-size image; an
/// out-of-range store is dropped rather than panicking.
#[inline(always)]
fn put(b: &mut [u8], off: usize, v: &[u8]) {
    if let Some(dst) = b.get_mut(off..off + v.len()) {
        dst.copy_from_slice(v);
    }
}

#[inline(always)]
fn put8(b: &mut [u8], off: usize, v: u8) {
    put(b, off, &[v]);
}

#[inline(always)]
fn put16(b: &mut [u8], off: usize, v: u16) {
    put(b, off, &v.to_le_bytes());
}

#[inline(always)]
fn put32(b: &mut [u8], off: usize, v: u32) {
    put(b, off, &v.to_le_bytes());
}

#[inline(always)]
fn put64(b: &mut [u8], off: usize, v: u64) {
    put(b, off, &v.to_le_bytes());
}

/// `DIV_ROUND_UP`.
const fn div_round_up(n: u64, d: u64) -> u64 {
    n.div_ceil(d)
}

/// `round_up` / `ALIGN` to a power of two, wrapping on overflow (no panic).
const fn align(n: u64, a: u64) -> u64 {
    n.wrapping_add(a - 1) & !(a - 1)
}

/// Native M4 stage target: a 32-bit addition of 0x100, including zero on wrap.
/// Logical host ordinals remain monotonic; zero is not an unpublished sentinel.
pub(crate) const fn stamp_seed(_seed: u32, n: u64) -> u32 {
    (n as u32).wrapping_mul(0x100)
}

/// M4 stage target, independent of the diagnostic engine identifier.
pub(crate) const fn stamp(n: u64) -> u32 {
    stamp_seed(0, n)
}

/// 32-bit compact pointer of a GVA in the compact window.
pub(crate) const fn compact(gva: u64) -> u64 {
    gva.wrapping_sub(0x1000000000)
}

#[path = "encode/compute.rs"]
mod compute_encoder;
#[allow(unused_imports)] // Pure encoder utilities are exercised by the host ABI harness.
pub(crate) use compute_encoder::{
    encode_compute, init_compute_pool, preserve_compute_descriptor, ComputeEncodeError, ComputeRefs,
};

/// Tiling geometry and register words of a render (accelerator count 2).
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub(crate) struct Geometry {
    pub(crate) tx: u32,
    pub(crate) ty: u32,
    pub(crate) bx: u32,
    pub(crate) by: u32,
    pub(crate) sx: u32,
    pub(crate) sy: u32,
    pub(crate) pixels: u32,
    pub(crate) screen: u32,
    pub(crate) blocks: u32,
    pub(crate) tiles: u32,
    pub(crate) region_stride: u32,
    pub(crate) tpc_stride: u32,
    pub(crate) isp_blocks: u32,
    pub(crate) utile_config: u32,
    pub(crate) layer_mode: u32,
    pub(crate) tilebuffer_control: u32,
    pub(crate) tile_mode: u32,
    pub(crate) isp_control: u32,
    pub(crate) e: u64,
    pub(crate) x_blocks: u64,
    pub(crate) y_blocks: u64,
    pub(crate) tilemap_bytes: u64,
    pub(crate) tpc_bytes: u64,
    pub(crate) layer_metadata: u64,
    pub(crate) native_tiles: u64,
}

impl Geometry {
    /// Rejects geometry that cannot fit the retained render storage. Admission and the
    /// final publication check use the same byte counts; this must run before any command
    /// in a userspace job can reach firmware.
    pub(crate) fn validate_capacity(&self, tpc: u64, tilemap: u64) -> Result {
        if self.tpc_bytes > tpc || self.tilemap_bytes > tilemap {
            return Err(E2BIG);
        }
        Ok(())
    }

    /// The geometry of render `u` with layer metadata at `heap`. The render was validated
    /// (width, height, layers >= 1, samples 1, 2 or 4, utiles 16 or 32).
    pub(crate) fn new(u: &uapi::drm_asahi_cmd_render, heap: u64) -> Self {
        let (w, h, l) = (
            u32::from(u.width_px),
            u32::from(u.height_px),
            u32::from(u.layers),
        );
        let empty =
            u.flags & uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES != 0;
        let log2s = u32::from(u.samples).max(1).ilog2();
        let mut g = Geometry::default();

        g.tx = div_round_up(u64::from(w), 32) as u32;
        g.ty = div_round_up(u64::from(h), 32) as u32;
        g.bx = align(div_round_up(u64::from(g.tx), 4), 4) as u32;
        g.by = align(div_round_up(u64::from(g.ty), 4), 4) as u32;
        g.sx = g.bx * if u.utile_width_px == 16 { 2 } else { 1 };
        g.sy = g.by * if u.utile_height_px == 16 { 2 } else { 1 };
        g.e = (4 * u64::from(g.sx)) * (4 * u64::from(g.sy));
        g.pixels = w.wrapping_sub(1) | h.wrapping_sub(1) << 16;
        g.screen = g.tx.wrapping_sub(1) | g.ty.wrapping_sub(1) << 12;
        g.x_blocks = u64::from(g.bx) << 10 | u64::from(g.bx) << 18 | (3 * u64::from(g.bx));
        g.y_blocks = u64::from(g.by) << 18 | u64::from(g.by) << 10 | (3 * u64::from(g.by));
        g.blocks = g.bx * g.by;
        g.tiles = g.tx * g.ty;
        g.region_stride = (5 * g.e / 64) as u32;
        g.tpc_stride = (g.e / 8) as u32;
        g.isp_blocks = ((g.sx & 0x3fd) << 16) | (g.sy & 0x3fd);
        let utile_w = if u.utile_width_px == 32 {
            0x2000
        } else {
            0x1000
        };
        let utile_h = if u.utile_height_px == 32 {
            0x8000
        } else {
            0x4000
        };
        g.utile_config = utile_w | utile_h | log2s;
        g.tilemap_bytes = align(5 * g.e * u64::from(l), 0x1000);
        g.tpc_bytes = g.e * 8 * u64::from(l);
        g.layer_metadata = heap + if l > 1 { 0x200 } else { 0 };
        g.layer_mode = if l == 1 {
            0x8000
        } else {
            0xc000 | (if empty { 0x2000 } else { 0x1000 }) | l.wrapping_sub(1)
        };
        g.tilebuffer_control = div_round_up(
            u64::from(u.sample_size_B)
                * u64::from(u.utile_width_px)
                * u64::from(u.utile_height_px)
                * u64::from(u.samples),
            2048,
        ) as u32
            | log2s << 17;
        g.tile_mode = 0x280 | u32::from(l > 1) | if empty { 1 << 16 } else { 0 };
        g.isp_control = 0xc000 | (u.flags & (1 << 18));
        g.native_tiles = 0x3717fu64
            | u64::from(l > 1) << 32
            | u64::from(g.tx.wrapping_sub(1)) << 44
            | u64::from(g.ty.wrapping_sub(1)) << 53
            | 137438953472u64
            | u64::from(g.utile_config & 0xf000) << 28;
        g
    }
}

/// Clustered tiling layout from `base`: 64 segments of 5E bytes, then M2 (0xc00), M3 (0xa00),
/// M4 ((Q + 1) x 512 for Q = pages of the owned PB / 0x1800, rounded up) and the 128-aligned
/// 0x80-byte M1.
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub(crate) struct ClusterLayout {
    pub(crate) ct: u64,
    pub(crate) m1: u64,
    pub(crate) m2: u64,
    pub(crate) m3: u64,
    pub(crate) m4: u64,
    pub(crate) bytes: u64,
}

impl ClusterLayout {
    #[allow(dead_code)]
    pub(crate) fn new(geo: &Geometry, base: u64) -> Self {
        let q = div_round_up(div_round_up(hw::client::PB_HEAP_SIZE, 0x8000), 0x1800);
        let ct = base;
        let m2 = base + align(64 * 5 * geo.e, 32);
        let m3 = m2 + 0xc00;
        let m4 = m3 + 0xa00;
        let m1 = base + align(m4 - base + (q + 1) * 8 * 64, 128);
        ClusterLayout {
            ct,
            m1,
            m2,
            m3,
            m4,
            bytes: m1 + 0x80 - base,
        }
    }
}

/// Clustered tiling eligibility: one layer, one sample, 32x32 utiles, unless the submission
/// asked for single-cluster vertex work. The engine then also needs a clustering buffer that
/// holds [`ClusterLayout::bytes`].
#[allow(dead_code)]
pub(crate) fn cluster_eligible(_u: &uapi::drm_asahi_cmd_render, _geo: &Geometry) -> bool {
    // The M4 25G83 native contract uses the single-cluster register table.
    false
}

/// Program address.
pub(crate) fn prog_addr(usc_base: u64, p: &uapi::drm_asahi_bg_eot) -> u64 {
    usc_base.wrapping_add(u64::from(p.usc & !63))
}

/// Program resource word, 64-bit with RSRC_SPEC_HI.
pub(crate) fn prog_rsrc(
    u: &uapi::drm_asahi_cmd_render,
    p: &uapi::drm_asahi_bg_eot,
    hi: u32,
) -> u64 {
    if u.flags & uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_RSRC_SPEC_HI != 0 {
        u64::from(hi) << 32 | u64::from(p.rsrc_spec)
    } else {
        u64::from(p.rsrc_spec)
    }
}

/// Register records.
pub(crate) fn regs(dst: &mut [u8], sel: &[u32], val: &[u64]) {
    for (i, (s, v)) in sel.iter().zip(val.iter()).enumerate() {
        put32(dst, REG_RECORD * i, *s);
        put64(dst, REG_RECORD * i + 4, *v);
    }
}

/// A barrier record waiting until the stamp at `stamp_va` reaches `wait`:
/// `event` names the producing event (0x70 TA, 0x71 FS, 15 compute), `own` is the waiter's stamp.
pub(crate) fn barrier(
    stamp_va: u64,
    wait: u32,
    event: u32,
    own: u32,
    _uuid: u32,
    ty: work::BarrierType,
) -> [u8; barrier::SIZE] {
    let mut x = [0u8; barrier::SIZE];

    put32(&mut x, 0x0, work::op::BARRIER);
    put64(&mut x, barrier::STAMP_VA, stamp_va);
    put64(&mut x, barrier::STAMP_VA2, stamp_va);
    put32(&mut x, barrier::WAIT, wait);
    put32(&mut x, barrier::EVENT, event);
    put32(&mut x, barrier::OWN, own);
    put32(&mut x, barrier::TYPE, ty as u32);
    x
}

#[path = "encode/render.rs"]
mod render_encoder;
#[allow(unused_imports)] // Includes private-tail variants rejected by the shared Mesa UAPI.
pub(crate) use render_encoder::{
    append_fs_timestamp_copies, encode_fs, encode_ta, fill_scene, initbm, render_attachments,
    scene, scene_render, PassRefs, RenderRefs,
};
