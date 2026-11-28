// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G14 firmware render command and parameter buffer encoders.
//!
//! The TA and fragment data masters consume distinct records. The render runtime owns both
//! records and the scene until both queues retire. Each logical queue owns its parameter
//! buffer, including the firmware-written page and block cursors, across submissions.

use crate::alloc::Allocator as _;
use crate::fw::types::GpuArray;
use crate::gpu::QueueInfo;
use crate::m3::agx_attachments::Attachments;
use crate::m3::agx_uapi::UapiRenderCommand;
use crate::{buffer, fw};
use core::mem::offset_of;
use kernel::{
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
};

type Rv = fw::vertex::raw::RunVertexG14V13_5<'static>;
type Rv13 = fw::vertex::raw::RunVertexG13V13_5<'static>;
type Rf = fw::fragment::raw::RunFragmentG14V13_5<'static>;
type Rf13 = fw::fragment::raw::RunFragmentG13V13_5<'static>;
type Meta = fw::job::raw::JobMeta;

const TILECTL_DISABLE_CLUSTERING: u32 = 1 << 0;

pub(crate) const INITBUF_AT: usize = 0xf00;
pub(crate) const OWN_BARRIER_AT: usize = 0xf40;
pub(crate) const OWN_BARRIER_SIZE: usize = 0x40;

/// G14 stamp dependency queued on FS immediately before RunFragment.
pub(crate) fn encode_own_barrier(
    record: &mut [u8],
    ta: &QueueInfo,
    ta_target: u32,
    fs_target: u32,
) {
    put32(record, 0, 4);
    put64(record, 0x04, ta.fw_stamp);
    put64(record, 0x0c, ta.fw_stamp);
    put32(record, 0x14, ta_target);
    put32(record, 0x20, ta.slot);
    put32(record, 0x24, fs_target);
}

struct Binding {
    info: GpuArray<u8>,
    slot: u32,
    blocks: u32,
    initialized: bool,
}

pub(crate) struct QueueRender {
    graph: Arc<crate::gpu::GpuManagerG14V13_5>,
    buffer: buffer::BufferG14V13_5,
    ualloc: Arc<Mutex<crate::alloc::DefaultAllocator>>,
    binding: Pin<KBox<Mutex<Option<Binding>>>>,
}

pub(crate) struct Prep {
    scene: buffer::SceneG14V13_5,
    aux_fb: GpuArray<u8>,
    tile: buffer::TileInfo,
    scene27: GpuArray<u8>,
    info_va: u64,
    usc: u64,
    gpu_gen: crate::hw::GpuGen,
    tiling_control: u32,
    core_masks: [u32; 2],
    pub(crate) buffer_op: Option<BufferOp>,
}

#[derive(Clone, Copy)]
pub(crate) enum BufferOp {
    Rebuild,
    Grow,
}

impl Prep {
    pub(crate) fn slot(&self) -> u32 {
        self.scene.slot()
    }

    /// Clears the tiler's TPC storage after a firmware recovery stopped the tiler.
    /// Zeroes the scene's TPC after a firmware recovery stopped its tiler.
    ///
    /// # Safety
    ///
    /// As [`crate::buffer::Scene::clear_tpc`].
    pub(crate) unsafe fn clear_tpc(&self) -> Result {
        // SAFETY: forwarded from the caller.
        unsafe { self.scene.clear_tpc(self.tile.tpc_size) }
    }
}

fn to_cmdbuf(r: &UapiRenderCommand) -> kernel::uapi::drm_asahi_cmd_render {
    // SAFETY: the UAPI struct is plain data; all-zero is a valid value.
    let mut c: kernel::uapi::drm_asahi_cmd_render = unsafe { core::mem::zeroed() };
    // This path has one cluster. The shared tiler only needs dimensions and sample geometry.
    c.isp_zls_pixels = r.depth_dimensions;
    c.width_px = r.width;
    c.height_px = r.height;
    c.layers = r.layers;
    c.utile_width_px = r.utile_width;
    c.utile_height_px = r.utile_height;
    c.samples = r.samples;
    c.sample_size_B = r.sample_size;
    c.ppp_ctrl = r.ppp_control;
    c.ppp_multisamplectl = r.multisample_control;
    // The tiling parameters carry the vertex helper's configuration, as in the 13.5 command.
    c.vertex_helper.cfg = r.vertex_helper.config;
    c
}

impl QueueRender {
    pub(crate) fn new(
        graph: Arc<crate::gpu::GpuManagerG14V13_5>,
        ualloc: Arc<Mutex<crate::alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<crate::alloc::DefaultAllocator>>,
    ) -> Result<Self> {
        let buffer = graph.new_render_buffer(ualloc.clone(), ualloc_priv)?;
        Ok(Self {
            graph,
            buffer,
            ualloc,
            binding: KBox::pin_init(new_mutex!(None), GFP_KERNEL)?,
        })
    }

    /// Called with the render data masters idle, so making a fresh binding cannot free memory
    /// which a previous flight still owns.
    pub(crate) fn prepare(&self, r: &UapiRenderCommand, usc: u64) -> Result<Prep> {
        let clusters =
            <crate::gpu::GpuManagerG14V13_5 as crate::gpu::GpuManager>::get_dyncfg(&self.graph)
                .id
                .num_clusters;
        let tile = crate::queue::QueueInnerG14V13_5::get_tiling_params(&to_cmdbuf(r), clusters)?;
        self.buffer.ensure_parameter_blocks(tile.min_tvb_blocks)?;
        let mut alloc =
            <crate::gpu::GpuManagerG14V13_5 as crate::gpu::GpuManager>::alloc(&self.graph);
        let scene = self.buffer.new_scene(&mut alloc, &tile)?;
        let mut scene27 = alloc.shared.array_empty::<u8>(0x48)?;
        let aux_fb = self.ualloc.lock().array_empty_tagged(0x8000, b"AXFB")?;
        let (user_buffer, stats) = scene.render_addresses();
        put64(scene27.as_mut_slice(), 0x18, user_buffer);
        put64(scene27.as_mut_slice(), 0x30, stats);
        crate::pgtable::clean_poc(scene27.as_slice().as_ptr(), scene27.len());

        let mut guard = self.binding.lock();
        if guard
            .as_ref()
            .is_none_or(|binding| binding.slot != scene.slot())
            || scene.rebind()
        {
            // Remember a failed rebind too. The slot allocator may return the same token on retry.
            *guard = None;
            let mut info = alloc.shared.array_empty::<u8>(0x90)?;
            let p = self.buffer.parameter_info();
            let record = info.as_mut_slice();
            put32(record, 0x08, scene.slot());
            put64(record, 0x18, p.page_list);
            put64(record, 0x20, p.page_list);
            put32(record, 0x28, p.page_list_size);
            put32(record, 0x30, p.max_blocks);
            put64(record, 0x3c, p.block_list);
            put64(record, 0x44, p.block_control);
            put32(record, 0x4c, u32::MAX);
            put32(record, 0x50, p.block_size);
            put64(record, 0x5c, p.counter);
            put32(record, 0x74, p.max_pages);
            put32(record, 0x78, p.max_linear_pages);
            crate::pgtable::clean_poc(record.as_ptr(), record.len());
            *guard = Some(Binding {
                info,
                slot: scene.slot(),
                blocks: 0,
                initialized: false,
            });
        }
        let binding = guard.as_ref().ok_or(EIO)?;
        let buffer_op = if !binding.initialized {
            Some(BufferOp::Rebuild)
        } else if self.buffer.block_count() > binding.blocks {
            Some(BufferOp::Grow)
        } else {
            None
        };
        let cfg = <crate::gpu::GpuManagerG14V13_5 as crate::gpu::GpuManager>::get_cfg(&self.graph);
        let dyncfg =
            <crate::gpu::GpuManagerG14V13_5 as crate::gpu::GpuManager>::get_dyncfg(&self.graph);
        let core_masks = [
            *dyncfg.id.core_masks_packed.first().unwrap_or(&0),
            *dyncfg.id.core_masks_packed.get(1).unwrap_or(&0),
        ];
        Ok(Prep {
            scene,
            aux_fb,
            tile,
            scene27,
            info_va: binding.info.gpu_va().get(),
            usc,
            gpu_gen: cfg.gpu_gen,
            tiling_control: cfg.render.tiling_control | TILECTL_DISABLE_CLUSTERING,
            core_masks,
            buffer_op,
        })
    }

    pub(crate) fn published(&self) {
        if let Some(binding) = Option::as_mut(&mut *self.binding.lock()) {
            binding.initialized = true;
            binding.blocks = self.buffer.block_count();
            self.buffer.increment_parameter_counter();
        }
    }

    /// A running TA can request more backing. The firmware computes the page list itself.
    pub(crate) fn grow(&self) -> Result<bool> {
        let mut guard = self.binding.lock();
        let binding = Option::as_mut(&mut *guard).ok_or(EIO)?;
        let count = self.buffer.block_count() as usize;
        let grown = self.buffer.ensure_parameter_blocks(count + 1)?;
        binding.blocks = self.buffer.block_count();
        Ok(grown)
    }
}

fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

/// Inputs both records share.
pub(crate) struct Common {
    pub(crate) cmd_va: u64,
    pub(crate) stream_va: u64,
    pub(crate) tracking: u64,
    pub(crate) timestamps: u64,
    pub(crate) context: u32,
    pub(crate) sequence: u64,
    pub(crate) stamp_value: u32,
}

/// Header shared by every 27.2 record (as compute): type, sequence, context, tracking.
fn header(b: &mut [u8], kind: u32, c: &Common) {
    put32(b, 0, kind);
    put64(b, 4, c.sequence);
    put32(b, 0xc, c.context);
    put64(b, 0x10, c.tracking);
}

/// Write the destinations used by both internal timestamp paths. `user` names the command's
/// pair of user destinations, or is 0 ([`crate::g14_v27_2_abi::user_timestamp_pointer`]).
fn timestamp_op(
    s: &mut [u8],
    at: usize,
    start: bool,
    q: &QueueInfo,
    command_time: u64,
    ts_pointers: u64,
    internal0: u64,
    internal1: u64,
    user: u64,
) {
    put32(s, at, if start { 0x8000_0003 } else { 3 });
    put64(s, at + 0x04, command_time);
    put64(s, at + 0x0c, ts_pointers);
    put64(
        s,
        at + 0x14,
        if start { ts_pointers } else { ts_pointers + 8 },
    );
    put64(s, at + 0x1c, q.queue);
    put64(s, at + 0x24, user);
    put64(s, at + 0x2c, internal0);
    put64(s, at + 0x34, internal1);
}

pub(crate) fn encode_buffer_op(rec: &mut [u8], c: &Common, p: &Prep, op: BufferOp) {
    put32(
        rec,
        0,
        match op {
            BufferOp::Rebuild => 6,
            BufferOp::Grow => 5,
        },
    );
    put32(rec, 4, c.context);
    put32(rec, 8, p.slot());
    // Rebuild is a range operation with zero count. Grow uses the published block producer.
    let info = match op {
        BufferOp::Rebuild => 0x14,
        BufferOp::Grow => 0x0c,
    };
    put64(rec, info, p.info_va);
    put32(rec, info + 8, c.stamp_value);
}

/// Encodes the tiler command and its stream. The fragment stamp is its wakeup target.
#[allow(clippy::too_many_arguments)]
pub(crate) fn encode_vertex(
    cmd: &mut [u8],
    stream: &mut [u8],
    c: &Common,
    q: &QueueInfo,
    vertex_state: u64,
    r: &UapiRenderCommand,
    p: &Prep,
    frag_stamp: u32,
    frag_slot: u32,
    timestamps: [u64; 2],
    attachments: &Attachments,
) -> Result {
    let jp2 = |field: usize| 0x488 + field - offset_of!(Rv, job_params2);
    header(cmd, 0, c);
    let scene = &p.scene;
    let (info_va, scene_va) = (p.info_va, p.scene27.gpu_va().get());
    // StartVertex and FinalizeVertex point past the state header word.
    let vertex_state = vertex_state + 4;
    let samples_log2 = match r.samples {
        1 => 0,
        2 => 1,
        4 => 2,
        _ => return Err(EINVAL),
    };
    let utile_config =
        ((p.tile.utile_width / 16) << 12) | ((p.tile.utile_height / 16) << 14) | samples_log2;
    put32(cmd, 0x18, scene.slot());
    put64(cmd, 0x20, info_va);
    put64(cmd, 0x28, scene_va);
    put64(cmd, 0x30, scene.kernel_buffer_pointer().into());
    // JobParameters1 at 0x38, 13.5 G13 or G14 internal layout.
    macro_rules! write_vtx_jp1 {
        ($R:ty) => {{
            let jp1 = |field: usize| 0x38 + field - offset_of!($R, job_params1);
            put64(cmd, jp1(offset_of!($R, job_params1.unk_0)), 0x200);
            put32(cmd, jp1(offset_of!($R, job_params1.unk_8)), 0x1e3c_e508);
            put32(cmd, jp1(offset_of!($R, job_params1.unk_c)), 0x1e3c_e508);
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.tvb_tilemap)),
                scene.tvb_tilemap_pointer().into(),
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.tpc)),
                scene.tpc_pointer().into(),
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.tvb_heapmeta)),
                scene
                    .tvb_heapmeta_pointer()
                    .or(0x8000_0000_0000_0000)
                    .into(),
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.iogpu_unk_54)),
                0x3a_0012_006b_0003,
            );
            put64(cmd, jp1(offset_of!($R, job_params1.iogpu_unk_56)), 1);
            put32(
                cmd,
                jp1(offset_of!($R, job_params1.utile_config)),
                utile_config,
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.ppp_multisamplectl)),
                r.multisample_control,
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.tvb_layermeta)),
                scene.tvb_layermeta_pointer().into(),
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.preempt_buf1)),
                scene.preempt_buf_1_pointer().into(),
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.preempt_buf2)),
                scene.preempt_buf_2_pointer().into(),
            );
            put64(cmd, jp1(offset_of!($R, job_params1.unk_80)), 1);
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.preempt_buf3)),
                scene.preempt_buf_3_pointer().or(0x4_0000_0000_0000).into(),
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.vdm_ctrl_stream_base)),
                r.vdm_base,
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.usc_exec_base_ta)),
                p.usc,
            );
            put64(cmd, jp1(offset_of!($R, job_params1.unk_f8)), 0x8c60);
            put32(
                cmd,
                jp1(offset_of!($R, job_params1.helper_program)),
                r.vertex_helper.binary,
            );
            put64(
                cmd,
                jp1(offset_of!($R, job_params1.helper_arg)),
                r.vertex_helper.data,
            );
            put32(cmd, jp1(offset_of!($R, job_params1.unk_118)), 0x1c);
        }};
    }
    if p.gpu_gen == crate::hw::GpuGen::G13 {
        write_vtx_jp1!(Rv13);
        let jp1 = |field: usize| 0x38 + field - offset_of!(Rv13, job_params1);
        let mask_off = jp1(offset_of!(Rv13, job_params1.core_mask));
        put32(cmd, mask_off, p.core_masks[0]);
        put32(cmd, mask_off + 4, p.core_masks[1]);
        put32(
            cmd,
            jp1(offset_of!(Rv13, job_params1.tiling_control)),
            p.tiling_control,
        );
        put64(
            cmd,
            jp1(offset_of!(Rv13, job_params1.unk_f0)),
            0x1c + u64::from(crate::util::align(p.tile.meta1_blocks, 4)),
        );
    } else {
        write_vtx_jp1!(Rv);
    }
    // Tiling parameters at 0x3bc (13.5 block), + the new word at +0x30.
    {
        let at = 0x3bc;
        let n = core::mem::size_of::<fw::vertex::raw::TilingParameters>();
        // SAFETY: `TilingParameters` is a `repr(C)` plain-data struct of `n` bytes.
        let src = unsafe {
            core::slice::from_raw_parts(core::ptr::from_ref(&p.tile.params).cast::<u8>(), n)
        };
        cmd[at..at + n].copy_from_slice(src);
        put32(cmd, at + 0x30, scene.slot());
    }
    put64(cmd, 0x45c, scene.tpc_pointer().into());
    put64(cmd, 0x464, p.tile.tpc_size as u64);
    let mseq = 0x46c;
    const STREAM: u32 = 0x300;
    put64(cmd, mseq, c.stream_va);
    put32(cmd, mseq + 8, STREAM);
    put32(cmd, 0x478, frag_slot);
    put32(cmd, 0x47c, frag_stamp);
    put64(
        cmd,
        jp2(offset_of!(Rv, job_params2.preempt_buf1)),
        scene.preempt_buf_1_pointer().into(),
    );
    put64(cmd, 0x558, r.sampler_heap);
    put32(cmd, 0x560, u32::from(r.sampler_count));
    put32(cmd, 0x564, u32::from(r.sampler_count));
    // Allow a synchronous parameter-buffer request.
    put32(cmd, 0x570, 1);
    // Vertex shader scratch flag: 13.5 uses_scratch is 8 bytes past sync_grow (0x570 -> 0x578).
    const _: () = assert!(offset_of!(Rv, uses_scratch) - offset_of!(Rv, sync_grow) == 8);
    put32(cmd, 0x578, u32::from(r.flags & 1 != 0));
    // Job meta at 0x580.
    put64(cmd, 0x584, q.stamp);
    put64(cmd, 0x58c, q.fw_stamp);
    put32(cmd, 0x594, c.stamp_value);
    put32(cmd, 0x598, q.slot);
    // flush_stamps: the firmware's per-queue completion walk stops at a command whose meta
    // leaves this clear, so the host stamp is never written and later commands never retire.
    put32(cmd, 0x5a0, 1);
    // The event-control index is independent of the completion stamp slot.
    put64(cmd, 0x5a4, 0); // uuid (compute leaves it 0)
    let command_time = c.cmd_va + 0x5cc;
    let ts = c.cmd_va + 0x5d4;
    put64(cmd, 0x5d4, c.timestamps);
    put64(cmd, 0x5dc, c.timestamps + 8);
    put64(cmd, 0x5e4, timestamps[0]);
    put64(cmd, 0x5ec, timestamps[1]);
    let user = crate::g14_v27_2_abi::user_timestamp_pointer(c.cmd_va + 0x5e4, timestamps);
    let internal0 = c.cmd_va + 0x61c;
    let internal1 = c.cmd_va + 0x538;
    let flag = c.cmd_va + 0x568;
    let unk_pointee = c.cmd_va + 0x480;
    let unk_buf_0 = c.cmd_va + 0x5b4;
    let tiling = c.cmd_va + 0x3bc;
    let jp1_va = c.cmd_va + 0x38;

    // Stream: StartVertex (0x19c), Timestamp, wait for the vertex pipe, Timestamp,
    // FinalizeVertex (0x9c), end.
    let s = stream;
    put32(s, 0, 5);
    put64(s, 0x04, tiling);
    put64(s, 0x0c, jp1_va);
    put64(s, 0x14, info_va);
    put64(s, 0x1c, scene_va);
    put64(s, 0x24, vertex_state);
    put64(s, 0x2c, q.queue);
    put64(s, 0x34, flag);
    put32(s, 0x3c, c.context);
    put32(s, 0x40, 1);
    put32(s, 0x48, scene.slot());
    // Match the command metadata: the default event-control index is zero.
    put64(s, 0x5c, unk_pointee);
    put64(s, 0x64, unk_buf_0);
    s[0x80..0x180].copy_from_slice(&attachments.0[..0x100]);
    put32(s, 0x180, u32::from(attachments.count()));
    put64(s, 0x188, c.sequence);
    put32(s, 0x190, q.slot);
    put32(s, 0x194, 1); // The tiler command uses the single-cluster layout.
    timestamp_op(
        s,
        0x19c,
        true,
        q,
        command_time,
        ts,
        internal0,
        internal1,
        user,
    );
    put32(s, 0x1e8, 0x0000_0100);
    timestamp_op(
        s,
        0x1ec,
        false,
        q,
        command_time,
        ts,
        internal0,
        internal1,
        user,
    );
    let fin = 0x238;
    put32(s, fin, 6);
    put64(s, fin + 0x04, scene_va);
    put64(s, fin + 0x0c, info_va);
    put64(s, fin + 0x14, vertex_state);
    put64(s, fin + 0x1c, q.queue);
    put64(s, fin + 0x24, flag);
    put32(s, fin + 0x2c, c.context);
    put64(s, fin + 0x34, unk_pointee);
    put64(s, fin + 0x48, q.fw_stamp);
    put32(s, fin + 0x50, c.stamp_value);
    put32(s, fin + 0x78, (-(fin as i32)) as u32);
    put64(s, fin + 0x8d, c.cmd_va + 0x60c);
    put32(s, fin + 0x9c, 0x4000_0002);
    debug_assert!(fin + 0xa0 <= STREAM as usize);
    Ok(())
}

/// Translate the legacy common fragment fields around the wider metadata and sampler tail.
const fn frag_at(offset: usize) -> usize {
    let meta = offset_of!(Rf, meta);
    let event_seq = meta + offset_of!(Meta, event_seq);
    if offset < FRAG_SAMPLERS13 {
        offset
    } else if offset < meta {
        offset + 0x10
    } else if offset < event_seq {
        offset + 0x14
    } else {
        offset + 0x18
    }
}

/// 13.5 offset of the sampler array (encoder_params), where the 27.2 +0x10 band starts.
const FRAG_SAMPLERS13: usize = 0x8ac;
/// 27.2 fragment stream: op 7 (0x1b4), Timestamp, WFI 0x00010000, Timestamp, op 8 (0xbc), end.
const FRAG_STREAM: u32 = 0x340;
/// Encodes the fragment command and its stream.
#[allow(clippy::too_many_arguments)]
pub(crate) fn encode_fragment(
    cmd: &mut [u8],
    stream: &mut [u8],
    c: &Common,
    q: &QueueInfo,
    fragment_state: u64,
    r: &UapiRenderCommand,
    p: &Prep,
    timestamps: [u64; 2],
    attachments: &Attachments,
) -> Result {
    let fragment_state = fragment_state + 4;
    // Anchors of the 13.5 layout this mapping assumes.
    if offset_of!(Rf, meta) != 0x8d0
        || offset_of!(Rf, encoder_params) + offset_of!(fw::job::raw::EncoderParams, sampler_array)
            != FRAG_SAMPLERS13
        || offset_of!(Rf, job_params1) != 0x80
        || offset_of!(Rf, job_params2) != 0x3c8
        || offset_of!(Rf, job_params3) != 0x4b8
        || cmd.len() < 0x9b0
        || stream.len() < FRAG_STREAM as usize
    {
        return Err(EINVAL);
    }
    let scene = &p.scene;
    let samples_log2 = match r.samples {
        1 => 0u32,
        2 => 1,
        4 => 2,
        _ => return Err(EINVAL),
    };
    let utile_config =
        ((p.tile.utile_width / 16) << 12) | ((p.tile.utile_height / 16) << 14) | samples_log2;
    let mut tile_config: u64 = 0x280;
    if r.layers > 1 {
        tile_config |= 1;
    }
    if r.flags & 2 != 0 {
        tile_config |= 0x10000;
    } // process empty tiles
    let pixels_per_utile = u32::from(r.utile_width) * u32::from(r.utile_height);
    let tib_blocks = (u32::from(r.sample_size) * (pixels_per_utile << samples_log2)).div_ceil(2048);
    let isp_ctl = 0xc000u32 | (r.flags & (1 << 18)); // depth bias is integer
    let rgn_size = u64::from(p.tile.params.rgn_size);
    let (info_va, scene_va) = (p.info_va, p.scene27.gpu_va().get());
    let va = |o: usize| c.cmd_va + o as u64;
    let w32 = |b: &mut [u8], o13: usize, v: u32| put32(b, frag_at(o13), v);
    let w64 = |b: &mut [u8], o13: usize, v: u64| put64(b, frag_at(o13), v);
    let w16 = |b: &mut [u8], o13: usize, v: u16| {
        let a = frag_at(o13);
        b[a..a + 2].copy_from_slice(&v.to_le_bytes());
    };

    // Header: 0x10 stays 0; microsequence at 0x14/0x1c; tracking slot at 0x20.
    put32(cmd, 0, 1);
    put64(cmd, 4, c.sequence);
    put32(cmd, 0xc, c.context);
    let mseq = 0x14;
    put64(cmd, mseq, c.stream_va);
    put32(cmd, mseq + 8, FRAG_STREAM);
    put64(cmd, 0x20, c.tracking);
    put64(cmd, 0x28, info_va);
    put64(cmd, 0x30, scene_va);
    w64(
        cmd,
        offset_of!(Rf, unk_buffer_buf),
        scene.kernel_buffer_pointer().into(),
    );
    w64(
        cmd,
        offset_of!(Rf, tvb_tilemap),
        scene.tvb_tilemap_pointer().into(),
    );
    w64(
        cmd,
        offset_of!(Rf, ppp_multisamplectl),
        r.multisample_control,
    );
    w32(cmd, offset_of!(Rf, samples), u32::from(r.samples));
    w16(
        cmd,
        offset_of!(Rf, tiles_per_mtile_y),
        p.tile.tiles_per_mtile_y as u16,
    );
    w16(
        cmd,
        offset_of!(Rf, tiles_per_mtile_x),
        p.tile.tiles_per_mtile_x as u16,
    );
    w32(cmd, offset_of!(Rf, isp_merge_upper_x), r.merge_upper_x);
    w32(cmd, offset_of!(Rf, isp_merge_upper_y), r.merge_upper_y);
    w64(cmd, offset_of!(Rf, tile_count), u64::from(p.tile.tiles));
    // Job parameters 1 (13.5 G13 or G14 internal layout).
    let g13 = p.gpu_gen == crate::hw::GpuGen::G13;
    macro_rules! write_frag_jp1 {
        ($R:ty) => {{
            w32(cmd, offset_of!($R, job_params1.utile_config), utile_config);
            w64(
                cmd,
                offset_of!($R, job_params1.bg.rsrc_spec),
                r.background.resource_spec,
            );
            w64(
                cmd,
                offset_of!($R, job_params1.bg.address),
                u64::from(r.background.usc),
            );
            w64(
                cmd,
                offset_of!($R, job_params1.ppp_multisamplectl),
                r.multisample_control,
            );
            w64(
                cmd,
                offset_of!($R, job_params1.isp_scissor_base),
                r.scissor_base,
            );
            w64(
                cmd,
                offset_of!($R, job_params1.isp_dbias_base),
                r.depth_bias_base,
            );
            w64(
                cmd,
                offset_of!($R, job_params1.isp_oclqry_base),
                r.occlusion_query_base,
            );
            for base in [
                offset_of!($R, job_params1.aux_fb_info),
                offset_of!($R, job_params3.aux_fb_info),
            ] {
                w32(cmd, base, isp_ctl);
                w32(cmd, base + 8, u32::from(r.width));
                w32(cmd, base + 0xc, u32::from(r.height));
                w64(cmd, base + 0x10, 0x10_0000);
            }
            w64(
                cmd,
                offset_of!($R, job_params1.isp_zls_pixels),
                u64::from(r.depth_dimensions),
            );
            w64(cmd, offset_of!($R, job_params1.zls_ctrl), r.zls_control);
            for (o, v) in [
                (offset_of!($R, job_params1.z_load), r.depth.base),
                (offset_of!($R, job_params1.z_store), r.depth.base),
                (offset_of!($R, job_params1.s_load), r.stencil.base),
                (offset_of!($R, job_params1.s_store), r.stencil.base),
                (
                    offset_of!($R, job_params1.z_load_stride),
                    u64::from(r.depth.stride),
                ),
                (
                    offset_of!($R, job_params1.z_store_stride),
                    u64::from(r.depth.stride),
                ),
                (
                    offset_of!($R, job_params1.s_load_stride),
                    u64::from(r.stencil.stride),
                ),
                (
                    offset_of!($R, job_params1.s_store_stride),
                    u64::from(r.stencil.stride),
                ),
                (
                    offset_of!($R, job_params1.z_load_comp),
                    r.depth.compression_base,
                ),
                (
                    offset_of!($R, job_params1.z_load_comp_stride),
                    u64::from(r.depth.compression_stride),
                ),
                (
                    offset_of!($R, job_params1.z_store_comp),
                    r.depth.compression_base,
                ),
                (
                    offset_of!($R, job_params1.z_store_comp_stride),
                    u64::from(r.depth.compression_stride),
                ),
                (
                    offset_of!($R, job_params1.s_load_comp),
                    r.stencil.compression_base,
                ),
                (
                    offset_of!($R, job_params1.s_load_comp_stride),
                    u64::from(r.stencil.compression_stride),
                ),
                (
                    offset_of!($R, job_params1.s_store_comp),
                    r.stencil.compression_base,
                ),
                (
                    offset_of!($R, job_params1.s_store_comp_stride),
                    u64::from(r.stencil.compression_stride),
                ),
                (
                    offset_of!($R, job_params1.tvb_tilemap),
                    scene.tvb_tilemap_pointer().into(),
                ),
                (
                    offset_of!($R, job_params1.tvb_layermeta),
                    scene.tvb_layermeta_pointer().into(),
                ),
                (
                    offset_of!($R, job_params1.mtile_stride_dwords),
                    (4 * rgn_size) << 24,
                ),
                (
                    offset_of!($R, job_params1.tvb_heapmeta),
                    scene.tvb_heapmeta_pointer().into(),
                ),
                (offset_of!($R, job_params1.tile_config), tile_config),
                (offset_of!($R, job_params1.aux_fb), p.aux_fb.gpu_va().get()),
                (offset_of!($R, job_params1.usc_exec_base_isp), p.usc),
                (offset_of!($R, job_params1.unk_140), 0x8c60),
                (
                    offset_of!($R, job_params1.helper_arg),
                    r.fragment_helper.data,
                ),
                (offset_of!($R, job_params1.unk_158), 0x1c),
            ] {
                w64(cmd, o, v);
            }
            w32(
                cmd,
                offset_of!($R, job_params1.helper_program),
                r.fragment_helper.binary,
            );
        }};
    }
    if g13 {
        write_frag_jp1!(Rf13);
    } else {
        write_frag_jp1!(Rf);
        w64(cmd, offset_of!(Rf, job_params1.unk_58_g14_0), 0x404_0404);
    }
    // Job parameters 2, + the new JP2+0x44 = buffer slot (cmd+0x40c).
    w32(
        cmd,
        offset_of!(Rf, job_params2.eot_rsrc_spec),
        r.end_of_tile.resource_spec as u32,
    );
    w32(cmd, offset_of!(Rf, job_params2.eot_usc), r.end_of_tile.usc);
    w32(
        cmd,
        offset_of!(Rf, job_params2.isp_merge_upper_x),
        r.merge_upper_x,
    );
    w32(
        cmd,
        offset_of!(Rf, job_params2.isp_merge_upper_y),
        r.merge_upper_y,
    );
    w16(
        cmd,
        offset_of!(Rf, job_params2.utiles_per_mtile_y),
        p.tile.utiles_per_mtile_y as u16,
    );
    w16(
        cmd,
        offset_of!(Rf, job_params2.utiles_per_mtile_x),
        p.tile.utiles_per_mtile_x as u16,
    );
    w32(
        cmd,
        offset_of!(Rf, job_params2.tile_counts),
        ((p.tile.tiles_y - 1) << 12) | (p.tile.tiles_x - 1),
    );
    w32(cmd, offset_of!(Rf, job_params2.tib_blocks), tib_blocks);
    w32(
        cmd,
        offset_of!(Rf, job_params2.isp_bgobjdepth),
        r.depth_clear,
    );
    w32(
        cmd,
        offset_of!(Rf, job_params2.isp_bgobjvals),
        if g13 { r.stencil_clear | 0x400 } else { r.stencil_clear },
    );
    w32(cmd, offset_of!(Rf, job_params2.unk_3c), 1);
    w32(
        cmd,
        offset_of!(Rf, job_params2.helper_cfg),
        r.fragment_helper.config,
    );
    put32(cmd, 0x40c, scene.slot());
    // Job parameters 3 (13.5 fill).
    for (o, v) in [
        (
            offset_of!(Rf, job_params3.isp_dbias_base.ptr),
            r.depth_bias_base,
        ),
        (
            offset_of!(Rf, job_params3.isp_scissor_base.ptr),
            r.scissor_base,
        ),
        (
            offset_of!(Rf, job_params3.isp_oclqry_base),
            r.occlusion_query_base,
        ),
        (
            offset_of!(Rf, job_params3.unk_partial_bg.rsrc_spec),
            r.partial_background.resource_spec,
        ),
        (
            offset_of!(Rf, job_params3.unk_partial_bg.address),
            u64::from(r.partial_background.usc),
        ),
        (
            offset_of!(Rf, job_params3.partial_bg.rsrc_spec),
            r.partial_background.resource_spec,
        ),
        (
            offset_of!(Rf, job_params3.partial_bg.address),
            u64::from(r.partial_background.usc),
        ),
        (offset_of!(Rf, job_params3.zls_ctrl), r.zls_control),
        (
            offset_of!(Rf, job_params3.unk_290),
            if g13 { 0 } else { 0x404_0404 },
        ),
        (offset_of!(Rf, job_params3.z_load), r.depth.base),
        (
            offset_of!(Rf, job_params3.z_partial_stride),
            u64::from(r.depth.stride),
        ),
        (
            offset_of!(Rf, job_params3.z_partial_comp_stride),
            u64::from(r.depth.compression_stride),
        ),
        (offset_of!(Rf, job_params3.z_store), r.depth.base),
        (offset_of!(Rf, job_params3.z_partial), r.depth.base),
        (
            offset_of!(Rf, job_params3.z_partial_comp),
            r.depth.compression_base,
        ),
        (offset_of!(Rf, job_params3.s_load), r.stencil.base),
        (
            offset_of!(Rf, job_params3.s_partial_stride),
            u64::from(r.stencil.stride),
        ),
        (
            offset_of!(Rf, job_params3.s_partial_comp_stride),
            u64::from(r.stencil.compression_stride),
        ),
        (offset_of!(Rf, job_params3.s_store), r.stencil.base),
        (offset_of!(Rf, job_params3.s_partial), r.stencil.base),
        (
            offset_of!(Rf, job_params3.s_partial_comp),
            r.stencil.compression_base,
        ),
        (offset_of!(Rf, job_params3.tile_config), tile_config),
        (
            offset_of!(Rf, job_params3.isp_zls_pixels),
            u64::from(r.depth_dimensions),
        ),
    ] {
        w64(cmd, o, v);
    }
    w32(cmd, offset_of!(Rf, job_params3.tib_blocks), tib_blocks);
    for base in [
        offset_of!(Rf, job_params3.unk_partial_eot),
        offset_of!(Rf, job_params3.partial_eot),
    ] {
        w32(
            cmd,
            base + offset_of!(fw::fragment::raw::EotProgram, rsrc_spec),
            r.partial_end_of_tile.resource_spec as u32,
        );
        w32(
            cmd,
            base + offset_of!(fw::fragment::raw::EotProgram, address),
            r.partial_end_of_tile.usc,
        );
    }
    w32(
        cmd,
        offset_of!(Rf, job_params3.isp_bgobjdepth),
        r.depth_clear,
    );
    w32(
        cmd,
        offset_of!(Rf, job_params3.isp_bgobjvals),
        r.stencil_clear,
    );
    w32(
        cmd,
        offset_of!(Rf, job_params3.sample_size),
        u32::from(r.sample_size),
    );
    put64(cmd, 0x8bc, r.sampler_heap);
    put32(cmd, 0x8c4, u32::from(r.sampler_count));
    put32(cmd, 0x8c8, u32::from(r.sampler_count));
    w32(cmd, offset_of!(Rf, encoder_params.unk_mask), u32::MAX);
    w32(
        cmd,
        offset_of!(Rf, process_empty_tiles),
        u32::from(r.flags & 2 != 0),
    );
    w32(cmd, offset_of!(Rf, no_clear_pipeline_textures), 1);
    // 0x8d8 unk_pointee, 0x8dc skip/done flag, 0x770, 0x888, 0x88c stay 0.
    let meta = offset_of!(Rf, meta);
    w64(cmd, meta + offset_of!(Meta, stamp), q.stamp);
    w64(cmd, meta + offset_of!(Meta, fw_stamp), q.fw_stamp);
    w32(cmd, meta + offset_of!(Meta, stamp_value), c.stamp_value);
    w32(cmd, meta + offset_of!(Meta, stamp_slot), q.slot);
    // evctl_index 0 (13.5 fixed), uuid (u64 at 0x908) 0, event_seq 0. flush_stamps (0x904) is
    // set, as for the tiler: without it the completion walk stops at this command.
    put32(cmd, 0x904, 1);
    w64(cmd, offset_of!(Rf, unk_buf_10), 1);
    w64(
        cmd,
        offset_of!(Rf, timestamp_pointers.start_addr),
        c.timestamps,
    );
    w64(
        cmd,
        offset_of!(Rf, timestamp_pointers.end_addr),
        c.timestamps + 8,
    );
    let command_time = va(frag_at(offset_of!(Rf, command_time)));
    let ts = va(frag_at(offset_of!(Rf, timestamp_pointers)));
    put64(cmd, 0x948, timestamps[0]);
    put64(cmd, 0x950, timestamps[1]);
    let user = crate::g14_v27_2_abi::user_timestamp_pointer(va(0x948), timestamps);
    let internal0 = va(0x980);
    let internal1 = va(0x89c);
    let (jp1, jp2) = (va(0x80), va(0x3c8));
    let busy = va(frag_at(offset_of!(Rf, busy_flag)));
    let unk_pointee = va(frag_at(offset_of!(Rf, unk_pointee)));
    let flag = va(frag_at(offset_of!(Rf, unk_v13_3)));
    let flag758 = va(frag_at(offset_of!(Rf, unk_758_flag)));
    let unk_buf_0 = va(frag_at(offset_of!(Rf, unk_buf_0)));
    if (busy, unk_pointee, flag, flag758, unk_buf_0, command_time)
        != (
            va(0x888),
            va(0x8d8),
            va(0x8dc),
            va(0x770),
            va(0x918),
            va(0x930),
        )
    {
        return Err(EINVAL);
    }

    // op 7 StartFragment (0x1b4).
    let s = stream;
    put32(s, 0, 7);
    put64(s, 0x04, jp2);
    put64(s, 0x0c, jp1);
    put64(s, 0x14, scene_va);
    put64(s, 0x1c, fragment_state);
    put64(s, 0x24, busy);
    put64(s, 0x2c, scene_va + 0x3c);
    put64(s, 0x34, unk_pointee);
    put64(s, 0x3c, flag);
    put64(s, 0x44, q.queue);
    put64(s, 0x4c, c.cmd_va);
    put32(s, 0x54, c.context);
    put32(s, 0x58, 1);
    put32(s, 0x60, scene.slot());
    put32(s, 0x64, 0); // sync_grow
    put32(s, 0x68, 0); // event_seq
    put32(s, 0x6c, 0); // evctl_index
    put64(s, 0x74, flag758);
    put64(s, 0x7c, unk_buf_0);
    put64(s, 0x98, 0); // uuid
    s[0xa0..0x1a0].copy_from_slice(&attachments.0[..0x100]);
    put32(s, 0x1a0, u32::from(attachments.count()));
    put64(s, 0x1a8, c.sequence);
    put32(s, 0x1b0, q.slot);
    timestamp_op(
        s,
        0x1b4,
        true,
        q,
        command_time,
        ts,
        internal0,
        internal1,
        user,
    );
    put32(s, 0x200, 0x0001_0000); // wait for the fragment pipe
    timestamp_op(
        s,
        0x204,
        false,
        q,
        command_time,
        ts,
        internal0,
        internal1,
        user,
    );
    // op 8 FinalizeFragment (0xbc).
    let fin = 0x250;
    put32(s, fin, 8);
    put64(s, fin + 0x04, 0); // uuid
    put64(s, fin + 0x0c, q.fw_stamp);
    put32(s, fin + 0x14, c.stamp_value);
    put64(s, fin + 0x1c, scene_va);
    put64(s, fin + 0x24, info_va);
    put32(s, fin + 0x2c, 1);
    put64(s, fin + 0x30, fragment_state);
    put64(s, fin + 0x38, unk_pointee);
    put64(s, fin + 0x40, flag);
    put64(s, fin + 0x48, busy);
    put64(s, fin + 0x50, q.queue);
    put64(s, fin + 0x58, c.cmd_va);
    put32(s, fin + 0x60, c.context);
    put64(s, fin + 0x64, flag758);
    put32(s, fin + 0x98, (-(fin as i32)) as u32);
    s[fin + 0x9c] = 0;
    put64(s, fin + 0xad, va(0x970));
    put32(s, fin + 0xb5, q.slot);
    put32(s, fin + 0xbc, 0x4000_0002);
    debug_assert!(fin + 0xc0 <= FRAG_STREAM as usize);
    Ok(())
}
