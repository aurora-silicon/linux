// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8103 (G13G) with GPU firmware 27.2: render passes (tiler then fragment).
//!
//! The 27.2 tiler and fragment commands keep the 13.5 G13G parameter blocks (vertex job
//! parameters and tiling parameters, fragment job parameters 1 to 3) and the 13.5 tile buffer
//! (`crate::buffer`), with new command headers, tails and micro-sequence operations. The blocks
//! are built from the 13.5 structures, so their layout is the one `queue/render.rs` uses.

use super::*;
use crate::fw::types::*;
use crate::m3::agx_attachments::Attachments;
use crate::m3::agx_uapi::UapiRenderCommand;
use crate::{alloc::Allocator, buffer};

type Buffer = buffer::BufferG13V27_2;
type Scene = buffer::SceneG13V27_2;

/// Render slots, reused in order once the firmware has unlinked both commands of a slot.
pub(crate) const J313_RENDER_SLOTS: usize = 8;

const TA_BYTES: usize = 0x800;
const FRAG_BYTES: usize = 0xc00;
const SEQ_BYTES: usize = 0x400;
const INIT_BYTES: usize = 0x40;
const BARRIER_BYTES: usize = 0x40;
const TRACK_BYTES: usize = 0x100;
const CURSOR_BYTES: usize = 0x40;
const STAMP_BYTES: usize = 0x80;

const TA_SEQ_SIZE: u32 = 0x300;
const FRAG_SEQ_SIZE: u32 = 0x340;

const RENDER_VERTEX_SCRATCH: u32 = 1 << 0;
const RENDER_PROCESS_EMPTY_TILES: u32 = 1 << 1;
const RENDER_NO_VERTEX_CLUSTERING: u32 = 1 << 2;
const RENDER_DBIAS_IS_INT: u32 = 1 << 18;
const TILECTL_DISABLE_CLUSTERING: u32 = 1 << 0;

fn put32(b: &mut [u8], at: usize, v: u32) { b[at..at + 4].copy_from_slice(&v.to_le_bytes()); }
fn put64(b: &mut [u8], at: usize, v: u64) { b[at..at + 8].copy_from_slice(&v.to_le_bytes()); }
fn put_struct<T>(b: &mut [u8], at: usize, size: usize, v: &T) -> Result {
    if core::mem::size_of::<T>() != size { return Err(EINVAL); }
    // SAFETY: `T` is a plain `repr(C)` firmware structure of `size` bytes.
    let bytes = unsafe { core::slice::from_raw_parts((v as *const T).cast::<u8>(), size) };
    b[at..at + size].copy_from_slice(bytes);
    Ok(())
}

/// Per client queue: the 13.5 tile buffer and the queue's identity.
pub(crate) struct J313RenderContext {
    pub(crate) buffer: Buffer,
    ualloc: Arc<Mutex<crate::alloc::DefaultAllocator>>,
    id: u32,
    /// The client address space the shared page list arena is mapped into for this context.
    arena_vm: crate::mmu::Vm,
}

impl J313RenderContext {
    pub(crate) fn new(graph: &GpuManagerG13V13_5, mgr: &buffer::BufferManagerG13V27_2, id: u32,
        ualloc: Arc<Mutex<crate::alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<crate::alloc::DefaultAllocator>>) -> Result<Self> {
        let (vm, range, prot) = ualloc_priv.lock().placement();
        buffer::arena_acquire(&vm, range, prot)?;
        let mut alloc = GpuManager::alloc(graph);
        let buffer = match Buffer::new(graph, &mut alloc, ualloc.clone(), ualloc_priv, mgr) {
            Ok(b) => b,
            Err(e) => { buffer::arena_release(&vm); return Err(e); }
        };
        Ok(Self { buffer, ualloc, id, arena_vm: vm })
    }
}

impl Drop for J313RenderContext {
    fn drop(&mut self) {
        let (count, done, ta_done, slot) = self.buffer.counters();
        if count != done {
            pr_info!("J313 render: buffer of queue {} ends with counter {} != completed {} (TA {}, slot {}); its slot is not evicted\n",
                     self.id, count, done, ta_done, slot);
        }
        buffer::arena_release(&self.arena_vm);
    }
}

/// Rejects render commands the 13.5 parameter blocks cannot express.
pub(crate) fn j313_validate_render(r: &UapiRenderCommand) -> Result {
    if r.flags & !(RENDER_VERTEX_SCRATCH | RENDER_PROCESS_EMPTY_TILES | RENDER_NO_VERTEX_CLUSTERING
        | RENDER_DBIAS_IS_INT) != 0 { return Err(EINVAL); }
    if r.width == 0 || r.height == 0 || r.width > 16384 || r.height > 16384
        || r.layers == 0 || r.layers > 2048 || !matches!(r.samples, 1 | 2 | 4)
        || !matches!((r.utile_width, r.utile_height), (32, 32) | (32, 16) | (16, 16)) {
        return Err(EINVAL);
    }
    Ok(())
}

/// Tiling parameters, as `queue/render.rs` computes them, for a single vertex cluster.
fn tiling(r: &UapiRenderCommand) -> Result<buffer::TileInfo> {
    let (width, height, layers) = (u32::from(r.width), u32::from(r.height), u32::from(r.layers));
    let (tile_width, tile_height) = (32u32, 32u32);
    let (utile_width, utile_height) = (u32::from(r.utile_width), u32::from(r.utile_height));
    let utiles_per_tile_x = tile_width / utile_width;
    let utiles_per_tile_y = tile_height / utile_height;
    let utiles_per_tile = utiles_per_tile_x * utiles_per_tile_y;
    let tiles_x = width.div_ceil(tile_width);
    let tiles_y = height.div_ceil(tile_height);
    let tiles = tiles_x * tiles_y;
    let mtiles = 16u32;
    let tiles_per_mtile_x = crate::util::align(tiles_x.div_ceil(4), 4);
    let tiles_per_mtile_y = crate::util::align(tiles_y.div_ceil(4), 4);
    let tiles_per_mtile = tiles_per_mtile_x * tiles_per_mtile_y;
    let rgn_size = crate::util::align(5 * tiles_per_mtile * utiles_per_tile, 4) / 4;
    let tilemap_size = (4 * rgn_size * mtiles) as usize * layers as usize;
    let tpc_mtile_stride = 8 * utiles_per_tile * tiles_per_mtile / 4;
    let tpc_size = (4 * tpc_mtile_stride * mtiles) as usize * layers as usize;
    let min_tvb_blocks = crate::util::align(tiles.div_ceil(128), 8);
    Ok(buffer::TileInfo {
        tiles_x, tiles_y, tiles, utile_width, utile_height,
        tiles_per_mtile_x, tiles_per_mtile_y,
        utiles_per_mtile_x: tiles_per_mtile_x * utiles_per_tile_x,
        utiles_per_mtile_y: tiles_per_mtile_y * utiles_per_tile_y,
        tilemap_size, tpc_size,
        meta1_layer_stride: 0,
        meta1_blocks: 0,
        layermeta_size: if layers > 1 { 0x100 } else { 0 },
        min_tvb_blocks: min_tvb_blocks as usize,
        params: fw::vertex::raw::TilingParameters {
            rgn_size,
            unk_4: 0x88,
            ppp_ctrl: r.ppp_control,
            x_max: (width - 1) as u16,
            y_max: (height - 1) as u16,
            te_screen: ((tiles_y - 1) << 12) | (tiles_x - 1),
            te_mtile1: (3 * tiles_per_mtile_x) | (2 * tiles_per_mtile_x) << 9 | tiles_per_mtile_x << 18,
            te_mtile2: (3 * tiles_per_mtile_y) | (2 * tiles_per_mtile_y) << 9 | tiles_per_mtile_y << 18,
            tiles_per_mtile,
            tpc_stride: tpc_mtile_stride,
            unk_24: 0x100,
            unk_28: if layers > 1 { 0xe000 | (layers - 1) } else { 0x8000 },
            helper_cfg: r.vertex_helper.config,
            __pad: Default::default(),
        },
    })
}

/// What a render pass needs from the runtime.
pub(crate) struct J313RenderInputs<'a> {
    pub(crate) command: &'a UapiRenderCommand,
    pub(crate) usc: u64,
    pub(crate) attachments: &'a [Attachments; 2],
    pub(crate) vm_slot: u32,
    pub(crate) rings: [&'a J313Ring; 2],
    /// Stamp values the tiler and fragment commands signal.
    pub(crate) values: [u32; 2],
    /// Per-queue command counters.
    pub(crate) sequence: [u64; 2],
    /// Tiler and fragment statistics objects.
    pub(crate) stats: [u64; 2],
}

/// One built render pass. The scene and the auxiliary buffer stay until the fragment command
/// has retired; the slot's commands until the firmware has unlinked them.
pub(crate) struct J313RenderJob {
    pub(crate) slot: usize,
    pub(crate) init: Option<u64>,
    /// Barrier ahead of the fragment command: waits for the tiler's stamp.
    pub(crate) barrier: u64,
    pub(crate) tiler: u64,
    pub(crate) fragment: u64,
    /// Why the buffer setup was emitted: (rebind, grown, auto-grown), buffer slot, blocks.
    pub(crate) why: (bool, bool, bool, u32, u32),
    _scene: Scene,
    _aux_fb: fw::types::GpuArray<u8>,
}

pub(crate) struct J313RenderPool {
    ta: fw::types::GpuArray<u8>,
    frag: fw::types::GpuArray<u8>,
    seqs: fw::types::GpuArray<u8>,
    init: fw::types::GpuArray<u8>,
    barriers: fw::types::GpuArray<u8>,
    tracking: fw::types::GpuArray<u8>,
    cursors: fw::types::GpuArray<u8>,
    stamps: fw::types::GpuArray<u8>,
}

impl J313RenderPool {
    pub(crate) fn new(alloc: &mut KernelAllocators) -> Result<Self> {
        let n = J313_RENDER_SLOTS;
        let ta = alloc.private.array_empty::<u8>(n * TA_BYTES)?;
        let frag = alloc.private.array_empty::<u8>(n * FRAG_BYTES)?;
        let seqs = alloc.private.array_empty::<u8>(n * 2 * SEQ_BYTES)?;
        let init = alloc.private.array_empty::<u8>(n * INIT_BYTES)?;
        let barriers = alloc.private.array_empty::<u8>(n * BARRIER_BYTES)?;
        let mut tracking = alloc.private.array_empty::<u8>(n * 2 * TRACK_BYTES)?;
        let cursors = alloc.shared.array_empty::<u8>(n * 2 * CURSOR_BYTES)?;
        let stamps = alloc.shared.array_empty::<u8>(n * STAMP_BYTES)?;
        let base = cursors.gpu_va().get();
        for (i, chunk) in tracking.as_mut_slice().chunks_mut(TRACK_BYTES).enumerate() {
            chunk[..8].copy_from_slice(&(base + (CURSOR_BYTES * i) as u64).to_le_bytes());
        }
        crate::pgtable::clean_poc(tracking.as_slice().as_ptr(), tracking.as_slice().len());
        crate::pgtable::poc_barrier();
        Ok(Self { ta, frag, seqs, init, barriers, tracking, cursors, stamps })
    }

    /// Writes a barrier that is already satisfied (waits for `wait` on a stamp the queue has
    /// reached) into the unused command storage of retired `slot`, for queue `which` (0 tiler,
    /// 1 fragment). Publishing it makes the firmware process the queue, which unlinks retired
    /// commands from its tracking lists.
    pub(crate) fn nop_barrier(&mut self, slot: usize, which: usize, stamp: u64, wait: u32, event: u32) -> Result<u64> {
        if slot >= J313_RENDER_SLOTS || which > 1 { return Err(EINVAL); }
        let (area, bytes) = if which == 0 { (&mut self.init, INIT_BYTES) } else { (&mut self.barriers, BARRIER_BYTES) };
        let va = area.gpu_va().get() + (slot * bytes) as u64;
        let b = &mut area.as_mut_slice()[slot * bytes..(slot + 1) * bytes];
        b.fill(0);
        put32(b, 0, 4);
        put64(b, 4, stamp);
        put64(b, 0xc, stamp);
        put32(b, 0x14, wait);
        put32(b, 0x20, event);
        put32(b, 0x24, wait);
        crate::pgtable::clean_poc(b.as_ptr(), b.len());
        crate::pgtable::poc_barrier();
        Ok(va)
    }

    /// Whether the firmware still holds either command of `slot` in a queue's tracking list.
    pub(crate) fn registered(&self, slot: usize) -> Result<bool> {
        for k in [2 * slot, 2 * slot + 1] {
            crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[k * TRACK_BYTES..].as_ptr(), TRACK_BYTES);
            crate::pgtable::poc_barrier();
            if self.tracking.shared_index(k * TRACK_BYTES + 0x94)?.load(Ordering::Acquire) != 0 {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// Retirement of `slot`'s tiler (`stage` 0) or fragment (1) command: advance its cursor to
    /// the value the firmware recorded, so its retirement scan unlinks the command.
    pub(crate) fn retire(&self, slot: usize, stage: usize) -> Result<u32> {
        let k = 2 * slot + stage;
        crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[k * TRACK_BYTES..].as_ptr(), TRACK_BYTES);
        crate::pgtable::poc_barrier();
        let expected = self.tracking.shared_index(k * TRACK_BYTES + 0xc)?.load(Ordering::Acquire);
        self.cursors.shared_index(k * CURSOR_BYTES)?.store(expected, Ordering::Release);
        crate::pgtable::clean_poc(self.cursors.as_slice()[k * CURSOR_BYTES..].as_ptr(), CURSOR_BYTES);
        crate::pgtable::poc_barrier();
        Ok(expected)
    }

    /// Logs `slot`'s tracking words and the heads of its commands.
    pub(crate) fn dump(&self, dev: &kernel::device::Device, slot: usize) {
        let w = |a: &fw::types::GpuArray<u8>, at: usize| a.shared_index(at).map(|x| x.load(Ordering::Acquire)).unwrap_or(u32::MAX);
        for stage in 0..2 {
            let k = 2 * slot + stage;
            crate::pgtable::clean_invalidate_poc(self.tracking.as_slice()[k * TRACK_BYTES..].as_ptr(), TRACK_BYTES);
            crate::pgtable::poc_barrier();
            let t: [u32; 0x32] = core::array::from_fn(|i| w(&self.tracking, k * TRACK_BYTES + 4 * i));
            dev_info!(dev, "J313 render slot {} stage {} cursor={:#x} tracking={:x?}\n",
                      slot, stage, w(&self.cursors, k * CURSOR_BYTES), t);
        }
        let s: [u32; 0x20] = core::array::from_fn(|i| w(&self.stamps, slot * STAMP_BYTES + 4 * i));
        dev_info!(dev, "J313 render slot {} stamps={:x?}\n", slot, s);
    }

    /// Builds `slot`'s tiler and fragment commands (and a buffer initialization when the tile
    /// buffer was rebound or grown) for one render pass.
    pub(crate) fn build(&mut self, graph: &GpuManagerG13V13_5, alloc: &mut KernelAllocators,
        ctx: &J313RenderContext, slot: usize, i: &J313RenderInputs<'_>) -> Result<J313RenderJob> {
        if slot >= J313_RENDER_SLOTS { return Err(EINVAL); }
        let r = i.command;
        j313_validate_render(r)?;
        let tile_info = tiling(r)?;
        let buffer = &ctx.buffer;
        // A tile buffer that grows after it has been used leaves state behind that faults the
        // next client: size it once (t8112_g13_min_blocks) and let overflowing passes use
        // partial renders instead.
        let autogrown = if crate::module_parameters::t8112_g13_autogrow.value() { buffer.auto_grow()? } else { false };
        let grown = buffer.ensure_blocks(tile_info.min_tvb_blocks
            .max(crate::module_parameters::t8112_g13_min_blocks.value() as usize))?;
        let scene = buffer.new_scene(alloc, &tile_info)?;
        let aux_fb = ctx.ualloc.lock().array_empty_tagged(0x8000, b"AXFB")?;
        // The slot's descriptor entry may still describe another buffer (the last one a setup
        // command committed): make it this buffer's, as the pass starts.
        buffer.load_pb_desc(scene.slot());

        let ta_va = self.ta.gpu_va().get() + (slot * TA_BYTES) as u64;
        let frag_va = self.frag.gpu_va().get() + (slot * FRAG_BYTES) as u64;
        let ta_seq_va = self.seqs.gpu_va().get() + (2 * slot * SEQ_BYTES) as u64;
        let frag_seq_va = ta_seq_va + SEQ_BYTES as u64;
        let init_va = self.init.gpu_va().get() + (slot * INIT_BYTES) as u64;
        let track_va = |stage: usize| self.tracking.gpu_va().get() + ((2 * slot + stage) * TRACK_BYTES) as u64;
        let (ta_track, frag_track) = (track_va(0), track_va(1));
        let stamps_va = self.stamps.gpu_va().get() + (slot * STAMP_BYTES) as u64;
        // Stamps area: tiler start/end, fragment start/end, then the two 16-byte areas the
        // finalize handlers clear.
        let (ta_latch, frag_latch) = (stamps_va + 0x20, stamps_va + 0x30);

        // Fresh tracking state and stamps for this use of the slot.
        for k in [2 * slot, 2 * slot + 1] {
            let t = &mut self.tracking.as_mut_slice()[k * TRACK_BYTES..(k + 1) * TRACK_BYTES];
            t[8..].fill(0);
            crate::pgtable::clean_poc(t.as_ptr(), t.len());
        }
        let st = &mut self.stamps.as_mut_slice()[slot * STAMP_BYTES..(slot + 1) * STAMP_BYTES];
        st.fill(0);
        crate::pgtable::clean_poc(st.as_ptr(), st.len());

        let [ta_ring, frag_ring] = i.rings;
        let (ta_ev, frag_ev) = (&ta_ring.event, &frag_ring.event);
        let vm_slot = i.vm_slot;
        let buffer_slot = scene.slot();
        let info_va: u64 = scene.buffer_pointer().into();
        let scene_va: u64 = scene.gpu_pointer().into();
        let kbuf_va: u64 = scene.kernel_buffer_pointer().into();
        let uuid = 0u64;

        // Shared values (queue/render.rs).
        let samples_log2 = match r.samples { 1 => 0, 2 => 1, 4 => 2, _ => return Err(EINVAL) };
        let utile_config = ((tile_info.utile_width / 16) << 12) | ((tile_info.utile_height / 16) << 14) | samples_log2;
        let mut tile_config: u64 = 0x280;
        if r.layers > 1 { tile_config |= 1; }
        if r.flags & RENDER_PROCESS_EMPTY_TILES != 0 { tile_config |= 0x10000; }
        let utile_bytes = u32::from(r.sample_size) * (u32::from(r.utile_width) * u32::from(r.utile_height) << samples_log2);
        let blocks_per_utile = utile_bytes.div_ceil(2048);
        let tiling_control = graph.get_cfg().render.tiling_control | TILECTL_DISABLE_CLUSTERING;
        let core_masks = graph.core_masks_packed();
        let aux_fb_info = fw::fragment::raw::AuxFBInfoG13V13_5 {
            isp_ctl: 0xc000u32 | (r.flags & RENDER_DBIAS_IS_INT),
            unk2: 0,
            width: u32::from(r.width),
            height: u32::from(r.height),
            unk3: U64(0x100000),
        };
        let rsrc = |p: crate::m3::agx_uapi::UapiProgram| p.resource_spec as u32;

        // Tiler command.
        let ta = &mut self.ta.as_mut_slice()[slot * TA_BYTES..(slot + 1) * TA_BYTES];
        ta.fill(0);
        put32(ta, 0, 0);
        put64(ta, 4, i.sequence[0]);
        put32(ta, 0xc, vm_slot);
        put64(ta, 0x10, ta_track);
        put32(ta, 0x18, buffer_slot);
        put64(ta, 0x20, info_va);
        put64(ta, 0x28, scene_va);
        put64(ta, 0x30, kbuf_va);
        let jp1 = fw::vertex::raw::JobParameters1G13V13_5 {
            unk_0: U64(0x200),
            unk_8: f32!(1e-20),
            unk_c: f32!(1e-20),
            tvb_tilemap: scene.tvb_tilemap_pointer(),
            tvb_cluster_tilemaps: scene.cluster_tilemaps_pointer(),
            tpc: scene.tpc_pointer(),
            tvb_heapmeta: scene.tvb_heapmeta_pointer().or(0x8000_0000_0000_0000),
            iogpu_unk_54: U64(0x3a0012006b0003),
            iogpu_unk_56: U64(1),
            tvb_cluster_meta1: scene.meta_1_pointer()
                .map(|x| x.or(u64::from(tile_info.meta1_layer_stride) << 50)),
            utile_config,
            unk_4c: 0,
            ppp_multisamplectl: U64(r.multisample_control),
            tvb_layermeta: scene.tvb_layermeta_pointer(),
            tvb_cluster_layermeta: scene.tvb_cluster_layermeta_pointer(),
            core_mask: Array::new([*core_masks.first().unwrap_or(&0), *core_masks.get(1).unwrap_or(&0)]),
            preempt_buf1: scene.preempt_buf_1_pointer(),
            preempt_buf2: scene.preempt_buf_2_pointer(),
            unk_80: U64(0x1),
            preempt_buf3: scene.preempt_buf_3_pointer().or(0x4_0000_0000_0000),
            vdm_ctrl_stream_base: U64(r.vdm_base),
            tvb_cluster_meta2: scene.meta_2_pointer(),
            tvb_cluster_meta3: scene.meta_3_pointer(),
            tiling_control,
            unk_ac: 0,
            unk_b0: Default::default(),
            usc_exec_base_ta: U64(i.usc),
            tvb_cluster_meta4: scene.meta_4_pointer().map(|x| x.or(0x3000_0000_0000_0000)),
            unk_f0: U64(0x1c + u64::from(crate::util::align(tile_info.meta1_blocks, 4))),
            unk_f8: U64(0x8c60),
            helper_program: r.vertex_helper.binary,
            unk_104: 0,
            helper_arg: U64(r.vertex_helper.data),
            unk_110: Default::default(),
            unk_118: 0x1c,
            __pad: Default::default(),
        };
        put_struct(ta, 0x38, 0x384, &jp1)?;
        put_struct(ta, 0x3bc, 0xa0, &tile_info.params)?;
        put64(ta, 0x45c, scene.tpc_pointer().into());
        put64(ta, 0x464, tile_info.tpc_size as u64);
        put64(ta, 0x46c, ta_seq_va);
        put32(ta, 0x474, TA_SEQ_SIZE);
        put32(ta, 0x478, frag_ev.slot());
        put32(ta, 0x47c, i.values[1]);
        put64(ta, 0x4a4, scene.preempt_buf_1_pointer().into());
        // Encoder parameters (13.5 layout and place).
        put32(ta, 0x544, u32::MAX);
        put64(ta, 0x548, r.sampler_heap);
        put32(ta, 0x550, u32::from(r.sampler_count));
        put32(ta, 0x554, u32::from(r.sampler_count) + 1);
        // Job metadata: completion stamp.
        put64(ta, 0x584, u64::from(ta_ev.stamp_pointer()));
        put64(ta, 0x58c, u64::from(ta_ev.fw_stamp_pointer()));
        put32(ta, 0x594, i.values[0]);
        // Queue slot: the firmware registers the queue under this byte when it links the
        // command and wakes the slot named by the submission entry.
        ta[0x598] = u8::try_from(ta_ev.slot()).map_err(|_| EINVAL)?;
        put32(ta, 0x59c, ta_ev.slot());
        put64(ta, 0x5a4, uuid);
        put32(ta, 0x5ac, i.sequence[0] as u32);
        put64(ta, 0x5d4, stamps_va);
        put64(ta, 0x5dc, stamps_va + 8);
        put64(ta, 0x63c, ta_latch);
        put32(ta, 0x644, 1);
        crate::pgtable::clean_poc(ta.as_ptr(), ta.len());

        // Tiler micro-sequence.
        let (ta_attach, frag_attach) = (&i.attachments[0], &i.attachments[1]);
        let seq = &mut self.seqs.as_mut_slice()[2 * slot * SEQ_BYTES..(2 * slot + 1) * SEQ_BYTES];
        seq.fill(0);
        put32(seq, 0, 5);
        for (at, v) in [(0x04, ta_va + 0x3bc), (0x0c, ta_va + 0x38), (0x14, info_va), (0x1c, scene_va),
                        (0x24, i.stats[0] + 4), (0x2c, ta_ring.va()), (0x34, ta_va + 0x568),
                        (0x5c, ta_va + 0x480), (0x64, ta_va + 0x5b4), (0x74, uuid),
                        (0x188, i.sequence[0])] {
            put64(seq, at, v);
        }
        for (at, v) in [(0x3c, vm_slot), (0x40, 1), (0x44, buffer_slot), (0x48, buffer_slot),
                        (0x50, i.sequence[0] as u32), (0x54, ta_ev.slot())] {
            put32(seq, at, v);
        }
        seq[0x80..0x80 + crate::m3::agx_attachments::SIZE].copy_from_slice(&ta_attach.0);
        Self::timestamps(seq, 0x19c, 0x1ec, ta_va, ta_ring.va(), [0x5cc, 0x5d4, 0x5e4, 0x61c, 0x538], uuid);
        seq[0x190] = u8::try_from(ta_ev.slot()).map_err(|_| EINVAL)?;
        put32(seq, 0x1e8, 0x100);
        let fin = 0x238;
        put32(seq, fin, 6);
        for (at, v) in [(4, scene_va), (0xc, info_va), (0x14, i.stats[0] + 4), (0x1c, ta_ring.va()),
                        (0x24, ta_va + 0x568), (0x34, ta_va + 0x480), (0x40, uuid),
                        (0x48, u64::from(ta_ev.fw_stamp_pointer()))] {
            put64(seq, fin + at, v);
        }
        put32(seq, fin + 0x2c, vm_slot);
        put32(seq, fin + 0x50, i.values[0]);
        put32(seq, fin + 0x78, (-(fin as i32)) as u32);
        seq[fin + 0x7c] = u8::from(ta_attach.count() > 0);
        put64(seq, fin + 0x85, ta_latch);
        put64(seq, fin + 0x8d, ta_va + 0x60c);
        put32(seq, fin + 0x9c, 0x4000_0002);
        crate::pgtable::clean_poc(seq.as_ptr(), seq.len());

        // Fragment command.
        let frag = &mut self.frag.as_mut_slice()[slot * FRAG_BYTES..(slot + 1) * FRAG_BYTES];
        frag.fill(0);
        put32(frag, 0, 1);
        put64(frag, 4, i.sequence[1]);
        put32(frag, 0xc, vm_slot);
        put64(frag, 0x14, frag_seq_va);
        put32(frag, 0x1c, FRAG_SEQ_SIZE);
        put64(frag, 0x20, frag_track);
        put64(frag, 0x28, info_va);
        put64(frag, 0x30, scene_va);
        put64(frag, 0x38, kbuf_va);
        put64(frag, 0x40, scene.tvb_tilemap_pointer().into());
        put64(frag, 0x48, r.multisample_control);
        put32(frag, 0x50, u32::from(r.samples));
        frag[0x54..0x56].copy_from_slice(&(tile_info.tiles_per_mtile_y as u16).to_le_bytes());
        frag[0x56..0x58].copy_from_slice(&(tile_info.tiles_per_mtile_x as u16).to_le_bytes());
        put32(frag, 0x68, r.merge_upper_x);
        put32(frag, 0x6c, r.merge_upper_y);
        put64(frag, 0x78, u64::from(tile_info.tiles));
        let fjp1 = fw::fragment::raw::JobParameters1G13V13_5 {
            utile_config,
            unk_4: 0,
            bg: fw::fragment::raw::BackgroundProgram {
                rsrc_spec: U64(u64::from(rsrc(r.background))),
                address: U64(u64::from(r.background.usc)),
            },
            ppp_multisamplectl: U64(r.multisample_control),
            isp_scissor_base: U64(r.scissor_base),
            isp_dbias_base: U64(r.depth_bias_base),
            isp_oclqry_base: U64(r.occlusion_query_base),
            aux_fb_info,
            isp_zls_pixels: U64(u64::from(r.depth_dimensions)),
            zls_ctrl: U64(r.zls_control),
            z_load: U64(r.depth.base),
            z_store: U64(r.depth.base),
            s_load: U64(r.stencil.base),
            s_store: U64(r.stencil.base),
            z_load_stride: U64(u64::from(r.depth.stride)),
            z_store_stride: U64(u64::from(r.depth.stride)),
            s_load_stride: U64(u64::from(r.stencil.stride)),
            s_store_stride: U64(u64::from(r.stencil.stride)),
            z_load_comp: U64(r.depth.compression_base),
            z_load_comp_stride: U64(u64::from(r.depth.compression_stride)),
            z_store_comp: U64(r.depth.compression_base),
            z_store_comp_stride: U64(u64::from(r.depth.compression_stride)),
            s_load_comp: U64(r.stencil.compression_base),
            s_load_comp_stride: U64(u64::from(r.stencil.compression_stride)),
            s_store_comp: U64(r.stencil.compression_base),
            s_store_comp_stride: U64(u64::from(r.stencil.compression_stride)),
            tvb_tilemap: scene.tvb_tilemap_pointer(),
            tvb_layermeta: scene.tvb_layermeta_pointer(),
            mtile_stride_dwords: U64((4 * u64::from(tile_info.params.rgn_size)) << 24),
            tvb_heapmeta: scene.tvb_heapmeta_pointer(),
            tile_config: U64(tile_config),
            aux_fb: aux_fb.gpu_pointer(),
            unk_108: Default::default(),
            usc_exec_base_isp: U64(i.usc),
            unk_140: U64(0x8c60),
            helper_program: r.fragment_helper.binary,
            unk_14c: 0,
            helper_arg: U64(r.fragment_helper.data),
            unk_158: U64(0x1c),
            unk_160: U64(0),
            __pad: Default::default(),
        };
        put_struct(frag, 0x80, 0x348, &fjp1)?;
        let fjp2 = fw::fragment::raw::JobParameters2 {
            eot_rsrc_spec: rsrc(r.end_of_tile),
            eot_usc: r.end_of_tile.usc,
            unk_8: 0,
            unk_c: 0,
            isp_merge_upper_x: F32::from_bits(r.merge_upper_x),
            isp_merge_upper_y: F32::from_bits(r.merge_upper_y),
            unk_18: U64(0),
            utiles_per_mtile_y: tile_info.utiles_per_mtile_y as u16,
            utiles_per_mtile_x: tile_info.utiles_per_mtile_x as u16,
            unk_24: 0,
            tile_counts: ((tile_info.tiles_y - 1) << 12) | (tile_info.tiles_x - 1),
            tib_blocks: blocks_per_utile,
            isp_bgobjdepth: r.depth_clear,
            isp_bgobjvals: r.stencil_clear | 0x400,
            unk_38: 0,
            unk_3c: 1,
            helper_cfg: r.fragment_helper.config,
            __pad: Default::default(),
        };
        put_struct(frag, 0x3c8, 0xf0, &fjp2)?;
        let fjp3 = fw::fragment::raw::JobParameters3G13V13_5 {
            isp_dbias_base: fw::fragment::raw::ArrayAddr { ptr: U64(r.depth_bias_base), unk_padding: U64(0) },
            isp_scissor_base: fw::fragment::raw::ArrayAddr { ptr: U64(r.scissor_base), unk_padding: U64(0) },
            isp_oclqry_base: U64(r.occlusion_query_base),
            unk_118: U64(0),
            unk_120: Default::default(),
            unk_partial_bg: fw::fragment::raw::BackgroundProgram {
                rsrc_spec: U64(u64::from(rsrc(r.partial_background))),
                address: U64(u64::from(r.partial_background.usc)),
            },
            unk_258: U64(0),
            unk_260: U64(0),
            unk_268: U64(0),
            unk_270: U64(0),
            partial_bg: fw::fragment::raw::BackgroundProgram {
                rsrc_spec: U64(u64::from(rsrc(r.partial_background))),
                address: U64(u64::from(r.partial_background.usc)),
            },
            zls_ctrl: U64(r.zls_control),
            unk_290: U64(0),
            z_load: U64(r.depth.base),
            z_partial_stride: U64(u64::from(r.depth.stride)),
            z_partial_comp_stride: U64(u64::from(r.depth.compression_stride)),
            z_store: U64(r.depth.base),
            z_partial: U64(r.depth.base),
            z_partial_comp: U64(r.depth.compression_base),
            s_load: U64(r.stencil.base),
            s_partial_stride: U64(u64::from(r.stencil.stride)),
            s_partial_comp_stride: U64(u64::from(r.stencil.compression_stride)),
            s_store: U64(r.stencil.base),
            s_partial: U64(r.stencil.base),
            s_partial_comp: U64(r.stencil.compression_base),
            unk_2f8: Default::default(),
            tib_blocks: blocks_per_utile,
            unk_30c: 0,
            aux_fb_info,
            tile_config: U64(tile_config),
            unk_328_padding: Default::default(),
            unk_partial_eot: fw::fragment::raw::EotProgram::new(rsrc(r.partial_end_of_tile), r.partial_end_of_tile.usc),
            partial_eot: fw::fragment::raw::EotProgram::new(rsrc(r.partial_end_of_tile), r.partial_end_of_tile.usc),
            isp_bgobjdepth: r.depth_clear,
            isp_bgobjvals: r.stencil_clear,
            sample_size: u32::from(r.sample_size),
            unk_37c: 0,
            unk_380: U64(0),
            unk_388: U64(0),
            unk_390_0: U64(0),
            isp_zls_pixels: U64(u64::from(r.depth_dimensions)),
        };
        put_struct(frag, 0x4b8, 0x2b8, &fjp3)?;
        // Encoder parameters (13.5 layout and place), then the 13.5 flags that follow them.
        put32(frag, 0x8a8, u32::MAX);
        put64(frag, 0x8ac, r.sampler_heap);
        put32(frag, 0x8b4, u32::from(r.sampler_count));
        put32(frag, 0x8b8, u32::from(r.sampler_count) + 1);
        // 27.2 moves the 13.5 tail flags by 0x14 (0x8bc..0x8cb is an optional buffer
        // descriptor, left empty): process empty tiles, then no-clear for the final run.
        put32(frag, 0x8d0, u32::from(r.flags & RENDER_PROCESS_EMPTY_TILES != 0));
        put32(frag, 0x8d4, 1);
        put64(frag, 0x8e8, u64::from(frag_ev.stamp_pointer()));
        put64(frag, 0x8f0, u64::from(frag_ev.fw_stamp_pointer()));
        put32(frag, 0x8f8, i.values[1]);
        frag[0x8fc] = u8::try_from(frag_ev.slot()).map_err(|_| EINVAL)?;
        put32(frag, 0x900, frag_ev.slot());
        put64(frag, 0x908, uuid);
        put32(frag, 0x910, i.sequence[1] as u32);
        put64(frag, 0x938, stamps_va + 0x10);
        put64(frag, 0x940, stamps_va + 0x18);
        put64(frag, 0x9a0, frag_latch);
        put32(frag, 0x9a8, 1);
        crate::pgtable::clean_poc(frag.as_ptr(), frag.len());

        // Fragment micro-sequence.
        let seq = &mut self.seqs.as_mut_slice()[(2 * slot + 1) * SEQ_BYTES..(2 * slot + 2) * SEQ_BYTES];
        seq.fill(0);
        put32(seq, 0, 7);
        for (at, v) in [(0x04, frag_va + 0x3c8), (0x0c, frag_va + 0x80), (0x14, scene_va),
                        (0x1c, i.stats[1] + 8), (0x24, frag_va + 0x888), (0x2c, scene_va + 0x3c),
                        (0x34, frag_va + 0x8d8), (0x3c, frag_va + 0x8dc), (0x44, frag_ring.va()),
                        (0x4c, frag_va), (0x74, frag_va + 0x770), (0x7c, frag_va + 0x918),
                        (0x98, uuid), (0x1a8, i.sequence[1])] {
            put64(seq, at, v);
        }
        for (at, v) in [(0x54, vm_slot), (0x58, 1), (0x5c, buffer_slot), (0x60, buffer_slot),
                        (0x68, i.sequence[1] as u32), (0x6c, frag_ev.slot())] {
            put32(seq, at, v);
        }
        seq[0xa0..0xa0 + crate::m3::agx_attachments::SIZE].copy_from_slice(&frag_attach.0);
        Self::timestamps(seq, 0x1b4, 0x204, frag_va, frag_ring.va(), [0x930, 0x938, 0x948, 0x980, 0x89c], uuid);
        seq[0x1b0] = u8::try_from(frag_ev.slot()).map_err(|_| EINVAL)?;
        put32(seq, 0x200, 0x10000);
        let fin = 0x250;
        put32(seq, fin, 8);
        for (at, v) in [(4, uuid), (0xc, u64::from(frag_ev.fw_stamp_pointer())), (0x1c, scene_va),
                        (0x24, info_va), (0x30, i.stats[1] + 8), (0x38, frag_va + 0x8d8),
                        (0x40, frag_va + 0x8dc), (0x48, frag_va + 0x888), (0x50, frag_ring.va()),
                        (0x58, frag_va), (0x64, frag_va + 0x770)] {
            put64(seq, fin + at, v);
        }
        put32(seq, fin + 0x14, i.values[1]);
        put32(seq, fin + 0x2c, 1);
        put32(seq, fin + 0x60, vm_slot);
        put32(seq, fin + 0x98, (-(fin as i32)) as u32);
        seq[fin + 0x9c] = u8::from(frag_attach.count() > 0);
        put64(seq, fin + 0xa5, frag_latch);
        seq[fin + 0xb5] = u8::try_from(frag_ev.slot()).map_err(|_| EINVAL)?;
        put64(seq, fin + 0xad, frag_va + 0x970);
        put32(seq, fin + 0xbc, 0x4000_0002);
        crate::pgtable::clean_poc(seq.as_ptr(), seq.len());

        // Buffer initialization ahead of the tiler command, as 13.5 does.
        let why = (scene.rebind(), grown, autogrown, buffer_slot, buffer.block_count());
        // The hardware tiles with the descriptor the last setup command committed, whatever
        // slot the pass names: commit this buffer's before every pass.
        let every = crate::module_parameters::t8112_g13_init_every.value();
        if every && !(scene.rebind() || grown || autogrown) {
            buffer.recommit_pb();
        }
        let init = if every || scene.rebind() || grown || autogrown {
            let b = &mut self.init.as_mut_slice()[slot * INIT_BYTES..(slot + 1) * INIT_BYTES];
            b.fill(0);
            put32(b, 0, 6);
            put32(b, 4, vm_slot);
            put32(b, 8, buffer_slot);
            put32(b, 0x10, if crate::module_parameters::t8112_g13_init_blocks.value() { buffer.block_count() } else { 0 });
            put64(b, 0x14, info_va);
            put32(b, 0x1c, i.values[0]);
            crate::pgtable::clean_poc(b.as_ptr(), b.len());
            Some(init_va)
        } else {
            None
        };
        // Tiler-to-fragment barrier (extended layout). The fragment command is queued with the
        // tiler so that a partial render, which runs it while the tiler is still going, finds it.
        let barrier_va = self.barriers.gpu_va().get() + (slot * BARRIER_BYTES) as u64;
        {
            let b = &mut self.barriers.as_mut_slice()[slot * BARRIER_BYTES..(slot + 1) * BARRIER_BYTES];
            b.fill(0);
            put32(b, 0, 4);
            put64(b, 4, u64::from(ta_ev.fw_stamp_pointer()));
            put64(b, 0xc, u64::from(ta_ev.fw_stamp_pointer()));
            put32(b, 0x14, i.values[0]);
            put32(b, 0x20, ta_ev.slot());
            put32(b, 0x24, i.values[1]);
            crate::pgtable::clean_poc(b.as_ptr(), b.len());
        }
        crate::pgtable::poc_barrier();
        buffer.increment();
        Ok(J313RenderJob { slot, init, barrier: barrier_va, why, tiler: ta_va, fragment: frag_va, _scene: scene, _aux_fb: aux_fb })
    }

    /// The start and end timestamp operations of a stage (kext `encodeTimeStamp` layout):
    /// `offsets` are the command's time, pair, user-pair, completion and encoder words.
    fn timestamps(seq: &mut [u8], start: usize, end: usize, cmd: u64, queue: u64, offsets: [u64; 5], uuid: u64) {
        let [time, pair, user, c0, c1] = offsets;
        for (at, op, selected) in [(start, 0x8000_0003u32, pair), (end, 3, pair + 8)] {
            put32(seq, at, op);
            for (field, v) in [(4, cmd + time), (0xc, cmd + pair), (0x14, cmd + selected), (0x1c, queue),
                               (0x24, cmd + user), (0x2c, cmd + c0), (0x34, cmd + c1), (0x44, uuid)] {
                put64(seq, at + field, v);
            }
        }
    }
}

/// The tiler and fragment queues (each with its own completion stamp) and the render slots.
pub(crate) fn j313_render_resources(graph: &GpuManagerG13V13_5) -> Result<([J313Ring; 2], J313RenderPool)> {
    let ta_event = graph.event_manager.try_get_polled()?;
    let frag_event = graph.event_manager.try_get_polled()?;
    let mut alloc = GpuManager::alloc(graph);
    let rings = [J313Ring::new(&mut alloc, ta_event, 0)?, J313Ring::new(&mut alloc, frag_event, 1)?];
    let pool = J313RenderPool::new(&mut alloc)?;
    Ok((rings, pool))
}
