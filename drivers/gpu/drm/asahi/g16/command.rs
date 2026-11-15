// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! SUBMIT parsing and validation into engine commands.
//!
//! Uses the shared Mesa UAPI and the qualified M4 HAL200 subset: render resource
//! specifications retain all four high words; shader streams remain Mesa-owned.
//! The complete stream is validated before timestamps, VM spans or scheduler state
//! are touched. Unsupported extension fields are rejected rather than ignored.
//! Render/compute attachments are retained with the immutable command; the whole
//! VM job guard protects indirect and bindless accesses through retirement.
//! Exact historical prefixes become immutable per-command dependencies. Ray-query state
//! remains disabled.

use core::mem::{offset_of, size_of};
use core::ops::Range;
use kernel::{prelude::*, uapi, xarray};

use super::{gate, hw};
use crate::{file, mmu, util::AnyBitPattern, util::Reader};

/// Largest command buffer.
pub(crate) const MAX_CMDBUF: usize = 4 << 20;
/// Most input plus output syncs of one SUBMIT.
pub(crate) const MAX_SYNCS: usize = 4096;
/// Hardware commands per SUBMIT.
pub(crate) const MAX_COMMANDS: u32 = crate::file::MAX_COMMANDS_PER_SUBMISSION;

const HEADER_SIZE: usize = size_of::<uapi::drm_asahi_cmd_header>();
const ATTACHMENT_SIZE: usize = size_of::<uapi::drm_asahi_attachment>();
/// A render payload with RSRC_SPEC_HI carries every high word.
const RSRC_SPEC_HI_MIN: usize =
    offset_of!(uapi::drm_asahi_cmd_render, bg_partial_rsrc_spec_hi) + size_of::<u32>();
const NONE: u16 = uapi::DRM_ASAHI_BARRIER_NONE as u16;

const VERTEX_SCRATCH: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_VERTEX_SCRATCH;
const PROCESS_EMPTY_TILES: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES;
const RSRC_SPEC_HI: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_RSRC_SPEC_HI;
const DBIAS_IS_INT: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_DBIAS_IS_INT;
const FRAGMENT_BARRIERS: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS;

const CMD_RENDER: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER;
const CMD_COMPUTE: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE;
const SET_VERTEX_ATTACHMENTS: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_VERTEX_ATTACHMENTS;
const SET_FRAGMENT_ATTACHMENTS: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS;
const SET_COMPUTE_ATTACHMENTS: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_ATTACHMENTS;

const MAX_DIMENSION: u16 = 16384;
const MAX_LAYERS: u16 = 2048;
const MAX_TILE_BUFFER: u32 = 32768;
/// Size of a queue's USC window.
pub(crate) const USC_WINDOW: u64 = 1 << 32;

/// Engine index of render commands (VDM) in the accepted counts.
pub(crate) const RENDER: usize = 0;
/// Engine index of compute commands (CDM).
pub(crate) const COMPUTE: usize = 1;

/// Exact queue-local command ordinal and the prefixes this command must consume.
/// Ordinals do not wrap with firmware stamps. NONE excludes the current submission's
/// implicit prefix; a nonzero historical prior still applies independently of NONE.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Order {
    pub(crate) engine: usize,
    pub(crate) sequence: u64,
    pub(crate) wait_through: [Option<u64>; 2],
}

impl Order {
    pub(crate) fn new(
        base: [u64; 2],
        preceding: [u16; 2],
        barriers: [u16; 2],
        prior: [u64; 2],
        engine: usize,
    ) -> Result<Self> {
        if engine >= 2 {
            return Err(EINVAL);
        }
        let mut sequences = [0; 2];
        let mut wait_through = [None; 2];
        for index in 0..2 {
            // Earlier commands in this ioctl are not historical commands.
            if prior[index] > base[index] {
                return Err(EINVAL);
            }
            sequences[index] = base[index]
                .checked_add(u64::from(preceding[index]))
                .and_then(|value| value.checked_add(1))
                .ok_or(EOVERFLOW)?;
            if barriers[index] != NONE {
                if barriers[index] > preceding[index] {
                    return Err(EINVAL);
                }
                wait_through[index] = Some(
                    base[index]
                        .checked_add(u64::from(barriers[index]))
                        .ok_or(EOVERFLOW)?,
                );
            }
            if prior[index] != 0 {
                wait_through[index] =
                    Some(wait_through[index].map_or(prior[index], |last| last.max(prior[index])));
            }
        }
        Ok(Self {
            engine,
            sequence: sequences[engine],
            wait_through,
        })
    }

    /// A same-engine command cannot jump a hole in its queue's completed frontier.
    /// Cross-engine historical dependencies end at the named prefix, even while later
    /// commands in that older submission remain pending.
    pub(crate) fn ready(&self, completed: [u64; 2]) -> bool {
        completed[self.engine].checked_add(1) == Some(self.sequence)
            && self
                .wait_through
                .iter()
                .zip(completed)
                .all(|(prefix, observed)| prefix.is_none_or(|last| observed >= last))
    }
}

/// A validated render command.
pub(crate) struct RenderCommand {
    /// The payload, zero-extended to the full UAPI structure.
    pub(crate) cmd: uapi::drm_asahi_cmd_render,
    /// Retained timestamp destinations, published after full command retirement.
    pub(crate) timestamp_outputs: crate::timestamp::Destinations,
    /// Explicit hardware attachments in effect for each render engine.
    pub(crate) vertex_attachments: KVec<uapi::drm_asahi_attachment>,
    pub(crate) fragment_attachments: KVec<uapi::drm_asahi_attachment>,
}

/// A validated compute command.
pub(crate) struct ComputeCommand {
    pub(crate) cmd: uapi::drm_asahi_cmd_compute,
    /// The compute attachments in effect (validated writable).
    pub(crate) attachments: KVec<uapi::drm_asahi_attachment>,
    /// Retained timestamp destinations, published after full command retirement.
    pub(crate) timestamp_outputs: crate::timestamp::Destinations,
}

/// A validated command.
pub(crate) enum Command {
    Render(KBox<RenderCommand>),
    Compute(KBox<ComputeCommand>),
}

/// A validated SUBMIT.
pub(crate) struct Submission {
    /// The commands, in submission order.
    pub(crate) commands: KVec<Command>,
    /// Exact per-command queue-local ordinals, allocated before scheduler acceptance.
    pub(crate) orders: KVec<Order>,
    /// Render and compute commands, for the queue's accepted-command accounting.
    pub(crate) counts: [u64; 2],
}

/// What the per-command checks need.
pub(crate) struct Context<'a> {
    /// The submitting VM.
    pub(crate) vm: &'a mmu::Vm,
    /// The queue's USC window base.
    pub(crate) usc_base: u64,
}

/// A stream whose timestamps and ordinals are resolved ([`prepare`]), before its spans are
/// checked ([`Prepared::validate`]).
pub(crate) struct Prepared<'a> {
    bytes: &'a [u8],
    /// CPU views and their backing resolved before scheduler acceptance.
    resolved: KVec<crate::timestamp::Destinations>,
    orders: KVec<Order>,
    counts: [u64; 2],
}

/// One command of the stream.
struct Entry<'a> {
    kind: u32,
    barriers: [u16; 2],
    payload: &'a [u8],
}

/// Iterates the commands of a structurally valid stream.
struct Stream<'a> {
    bytes: &'a [u8],
}

impl<'a> Iterator for Stream<'a> {
    type Item = Result<Entry<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.bytes.is_empty() {
            return None;
        }
        let entry = (|| {
            if self.bytes.len() < HEADER_SIZE {
                return Err(EINVAL);
            }
            let header: uapi::drm_asahi_cmd_header = Reader::new(self.bytes).read()?;
            let size = header.size as usize;
            let payload = self
                .bytes
                .get(HEADER_SIZE..HEADER_SIZE + size)
                .ok_or(EINVAL)?;
            if size % 8 != 0 {
                return Err(EINVAL);
            }
            self.bytes = &self.bytes[HEADER_SIZE + size..];
            Ok(Entry {
                kind: header.cmd_type.into(),
                barriers: [header.vdm_barrier, header.cdm_barrier],
                payload,
            })
        })();
        if entry.is_err() {
            self.bytes = &[];
        }
        Some(entry)
    }
}

fn stream(bytes: &[u8]) -> Stream<'_> {
    Stream { bytes }
}

/// Decodes a payload: short ones are zero-filled, bytes past the known structure must be zero.
fn payload<T: AnyBitPattern>(bytes: &[u8]) -> Result<T> {
    if bytes
        .get(size_of::<T>()..)
        .is_some_and(|tail| tail.iter().any(|&byte| byte != 0))
    {
        return Err(EINVAL);
    }
    Reader::new(bytes).read_up_to(bytes.len())
}

/// Whether `[start, start + size)` is a nonempty range inside `range`.
fn inside(start: u64, size: u64, range: &Range<u64>) -> bool {
    size != 0 && start >= range.start && start < range.end && size <= range.end - start
}

fn helper_is_empty(helper: &uapi::drm_asahi_helper_program) -> bool {
    helper.binary == 0 && helper.cfg == 0 && helper.data == 0
}

/// M4 helper-program execution is not implemented. Scratch requests carry real
/// helper operands and cannot be accepted by dropping the bit or the operands.
fn check_render_helpers(cmd: &uapi::drm_asahi_cmd_render) -> Result {
    if cmd.flags & VERTEX_SCRATCH != 0
        || !helper_is_empty(&cmd.vertex_helper)
        || !helper_is_empty(&cmd.fragment_helper)
    {
        return Err(Error::from_errno(-(kernel::bindings::EOPNOTSUPP as i32)));
    }
    Ok(())
}

/// Reject geometry which cannot fit any retained pass before a submission is accepted.
/// The encoder owns the sizing formula; admission and publication use the same calculation.
fn check_geometry(cmd: &uapi::drm_asahi_cmd_render) -> Result {
    super::encode::Geometry::new(cmd, 0)
        .validate_capacity(hw::client::TPC_SIZE, hw::client::TILEMAP_SIZE)
}

/// Structural validation of a whole command stream, before anything is looked up.
pub(crate) fn check_stream(bytes: &[u8], early_tiling: bool) -> Result {
    if bytes.is_empty() || bytes.len() > MAX_CMDBUF || bytes.len() % 8 != 0 {
        return Err(EINVAL);
    }
    let range = hw::USER_RANGE;
    let mut counts = [0u32; 2];
    for entry in stream(bytes) {
        let entry = entry?;
        match entry.kind {
            SET_VERTEX_ATTACHMENTS | SET_FRAGMENT_ATTACHMENTS | SET_COMPUTE_ATTACHMENTS => {
                if entry.barriers != [NONE; 2]
                    || entry.payload.len() % ATTACHMENT_SIZE != 0
                    || entry.payload.len() / ATTACHMENT_SIZE > hw::sizes::MAX_ATTACHMENTS
                {
                    return Err(EINVAL);
                }
                for bytes in entry.payload.chunks_exact(ATTACHMENT_SIZE) {
                    let attachment: uapi::drm_asahi_attachment = Reader::new(bytes).read()?;
                    if attachment.pad != 0
                        || attachment.flags != 0
                        || !inside(attachment.pointer, attachment.size, &range)
                    {
                        return Err(EINVAL);
                    }
                }
                continue;
            }
            _ => {}
        }
        if (entry.barriers[RENDER] != NONE && u32::from(entry.barriers[RENDER]) > counts[RENDER])
            || (entry.barriers[COMPUTE] != NONE
                && u32::from(entry.barriers[COMPUTE]) > counts[COMPUTE])
        {
            return Err(EINVAL);
        }
        match entry.kind {
            CMD_RENDER => {
                let packet: uapi::drm_asahi_cmd_render = payload(entry.payload)?;
                let cmd = &packet;
                // Ray-query extensions belong to G17; reject every nonzero byte
                // beyond the common historical-prefix fields on G16.
                let end = offset_of!(uapi::drm_asahi_cmd_render, prior_cdm) + size_of::<u64>();
                if entry.payload.get(end..).is_some_and(|tail| tail.iter().any(|&b| b != 0)) {
                    return Err(EINVAL);
                }
                check_render_helpers(cmd)?;
                // M4 HAL200 resource words carry all four high halves.
                let allowed = PROCESS_EMPTY_TILES
                    | DBIAS_IS_INT
                    | RSRC_SPEC_HI
                    | if early_tiling { FRAGMENT_BARRIERS } else { 0 };
                if cmd.flags & !allowed != 0
                    || !(1..=MAX_DIMENSION).contains(&cmd.width_px)
                    || !(1..=MAX_DIMENSION).contains(&cmd.height_px)
                    || !(1..=MAX_LAYERS).contains(&cmd.layers)
                    || !matches!(cmd.samples, 1 | 2 | 4)
                {
                    return Err(EINVAL);
                }
                check_geometry(cmd)?;
                // Timestamp copies are reserved for another runtime. No copy
                // feature is advertised, and zero handles do not waive MBZ offsets.
                if cmd
                    .ts_frag_end_copies
                    .iter()
                    .any(|ts| ts.handle != 0 || ts.offset != 0)
                {
                    return Err(EINVAL);
                }
                // `payload` rejects nonzero ray-query bytes beyond the prior
                // extension. RENDER_RAY_QUERY is never advertised on G16.
                // Never discard unflagged high words, nor accept the flag without them.
                if cmd.flags & RSRC_SPEC_HI != 0 && entry.payload.len() < RSRC_SPEC_HI_MIN {
                    return Err(EINVAL);
                }
                if cmd.flags & RSRC_SPEC_HI == 0
                    && (cmd.bg_eot_rsrc_spec_hi != 0
                        || cmd.bg_eot_partial_rsrc_spec_hi != 0
                        || cmd.bg_rsrc_spec_hi != 0
                        || cmd.bg_partial_rsrc_spec_hi != 0)
                {
                    return Err(EINVAL);
                }
                counts[RENDER] += 1;
            }
            CMD_COMPUTE => {
                let packet: uapi::drm_asahi_cmd_compute = payload(entry.payload)?;
                let cmd = &packet;
                if cmd.flags != 0
                    || cmd.cdm_ctrl_stream_end <= cmd.cdm_ctrl_stream_base
                    || !inside(
                        cmd.cdm_ctrl_stream_base,
                        cmd.cdm_ctrl_stream_end - cmd.cdm_ctrl_stream_base,
                        &range,
                    )
                {
                    return Err(EINVAL);
                }
                // The G17P scratch layout and intersection tables: never on G16.
                if cmd
                    .ts_end_copies
                    .iter()
                    .any(|ts| ts.handle != 0 || ts.offset != 0)
                    || packet.scs_layout != 0
                    || packet.cdm_auxiliary_allocation != 0
                    || packet.reserved != 0
                    || packet.intersection_pool != 0
                    || packet.intersection_pool_bytes != 0
                    || packet.intersection_first != 0
                    || packet.intersection_count != 0
                    || packet.intersection_default_program != 0
                    || packet.intersection_default_control != 0
                {
                    return Err(EINVAL);
                }
                counts[COMPUTE] += 1;
            }
            _ => return Err(EINVAL),
        }
        if counts[RENDER] + counts[COMPUTE] > MAX_COMMANDS {
            return Err(EINVAL);
        }
    }
    if counts == [0; 2] {
        return Err(EINVAL);
    }
    Ok(())
}

/// Checks that `[start, end)` (rounded out to pages) is user memory mapped with the requested
/// GPU access: a driver range ENOENT, outside the user range EINVAL, an unmapped page ENOENT, a
/// page without the access EACCES.
pub(crate) fn cover(vm: &mmu::Vm, start: u64, end: u64, read: bool, write: bool) -> Result {
    let mask = mmu::UAT_PGMSK as u64;
    if end <= start {
        return Err(EINVAL);
    }
    let first = start & !mask;
    let last = end.checked_add(mask).ok_or(EINVAL)? & !mask;
    // Every G16 driver mapping of a user VM is a client alias inside the fixed reserved ranges
    // (`vm::ClientAlias::check`), so testing those two ranges finds every driver range without
    // walking the alias mappings under the VM's lifetime lock on each span.
    if super::vm::reserved_ranges()
        .iter()
        .any(|reserved| first < reserved.end && reserved.start < last)
    {
        return Err(ENOENT);
    }
    let range = hw::USER_RANGE;
    if first < range.start || last > range.end {
        return Err(EINVAL);
    }
    if vm.covers_range(first, last - first, read, write) {
        Ok(())
    } else if vm.covers_range(first, last - first, false, false) {
        Err(EACCES)
    } else {
        Err(ENOENT)
    }
}

/// Attachment spans, 128-byte granular, actually mapped with `write` or read access.
fn check_attachments(vm: &mmu::Vm, list: &[uapi::drm_asahi_attachment], write: bool) -> Result {
    for attachment in list {
        let end = attachment
            .pointer
            .checked_add(attachment.size)
            .ok_or(EINVAL)?;
        if end > u64::MAX - 127 {
            return Err(EINVAL);
        }
        let first = attachment.pointer & !127;
        let last = (end + 127) & !127;
        if (last - first) / 128 > u64::from(u32::MAX) {
            return Err(EINVAL);
        }
        cover(vm, first, last, true, write)?;
    }
    Ok(())
}

/// Parses the attachment list of a SET_*_ATTACHMENTS command.
fn attachment_list(bytes: &[u8]) -> Result<KVec<uapi::drm_asahi_attachment>> {
    let mut list = KVec::with_capacity(bytes.len() / ATTACHMENT_SIZE, GFP_KERNEL)?;
    for bytes in bytes.chunks_exact(ATTACHMENT_SIZE) {
        list.push(Reader::new(bytes).read()?, GFP_KERNEL)?;
    }
    Ok(list)
}

fn copy_list(lists: &[&[uapi::drm_asahi_attachment]]) -> Result<KVec<uapi::drm_asahi_attachment>> {
    let mut copy = KVec::with_capacity(lists.iter().map(|list| list.len()).sum(), GFP_KERNEL)?;
    for list in lists {
        for attachment in list.iter() {
            copy.push(*attachment, GFP_KERNEL)?;
        }
    }
    Ok(copy)
}

/// Whether a GVA can be named by a 32-bit compact pointer.
fn compact(gva: u64) -> bool {
    (0x1000000000..0x9000000000).contains(&gva)
}

/// Validate uncompressed Z/S register state and coverage through each layer base.
/// This is a minimum span check, not a complete plane-extent validation: the UAPI
/// does not describe the complete ZLS surface layout. The encoded stride is a
/// register word, not a byte distance (the C HAL200 contract).
fn check_zls_plane(
    context: &Context<'_>,
    cmd: &uapi::drm_asahi_cmd_render,
    stencil: bool,
) -> Result {
    let (z, mask) = if stencil {
        (&cmd.stencil, (1u64 << 3) | (1 << 7) | (1 << 14) | (1 << 18))
    } else {
        (
            &cmd.depth,
            (1u64 << 1) | (1 << 5) | (1 << 15) | (1 << 19) | (3 << 25),
        )
    };
    if (z.base == 0 && (z.stride != 0 || cmd.zls_ctrl & mask != 0))
        || (z.base != 0 && cmd.layers > 1 && z.stride == 0)
        || (z.stride != 0 && z.stride & 0x3fff != 1)
        || z.base & 3 != 0
    {
        return Err(EINVAL);
    }
    if z.base != 0 {
        let distance = (u64::from(z.stride >> 14) + 1) * 0x4000;
        let minimum = distance
            .checked_mul(u64::from(cmd.layers - 1))
            .and_then(|bytes| bytes.checked_add(1))
            .ok_or(EINVAL)?;
        cover(
            context.vm,
            z.base,
            z.base.checked_add(minimum).ok_or(EINVAL)?,
            true,
            true,
        )?;
    }
    if cmd.depth.base == 0
        && cmd.stencil.base == 0
        && (cmd.zls_ctrl != 0 || cmd.isp_zls_pixels != 0)
    {
        return Err(EINVAL);
    }
    Ok(())
}

/// Per-command render admission.
fn check_render(context: &Context<'_>, cmd: &uapi::drm_asahi_cmd_render) -> Result {
    check_render_helpers(cmd)?;
    let vm = context.vm;
    let allowed = PROCESS_EMPTY_TILES
        | RSRC_SPEC_HI
        | DBIAS_IS_INT
        | if super::params::early_tiling() {
            FRAGMENT_BARRIERS
        } else {
            0
        };
    let tile_buffer = u32::from(cmd.sample_size_B)
        * u32::from(cmd.utile_width_px)
        * u32::from(cmd.utile_height_px)
        * u32::from(cmd.samples);
    if cmd.flags & !allowed != 0
        || !(1..=MAX_DIMENSION).contains(&cmd.width_px)
        || !(1..=MAX_DIMENSION).contains(&cmd.height_px)
        || !(1..=MAX_LAYERS).contains(&cmd.layers)
        || !matches!(cmd.samples, 1 | 2 | 4)
        || !matches!(cmd.utile_width_px, 16 | 32)
        || !matches!(cmd.utile_height_px, 16 | 32)
        || tile_buffer > MAX_TILE_BUFFER
        || cmd.sampler_heap != 0
        || cmd.sampler_count != 0
        || cmd.ppp_ctrl & !0x203 != 0
        || cmd.zls_ctrl & !0x00000000060cc0aa != 0
        || cmd.isp_zls_pixels & 0xc0000000 != 0
        || !matches!((cmd.zls_ctrl >> 25) & 3, 0 | 2)
        || ((cmd.zls_ctrl >> 1) & 1) != ((cmd.zls_ctrl >> 5) & 1)
        || ((cmd.zls_ctrl >> 3) & 1) != ((cmd.zls_ctrl >> 7) & 1)
    {
        return Err(EINVAL);
    }
    if cmd.stencil.comp_base != 0
        || cmd.stencil.comp_stride != 0
        || cmd.depth.comp_base != 0
        || cmd.depth.comp_stride != 0
    {
        return Err(Error::from_errno(-(kernel::bindings::EOPNOTSUPP as i32)));
    }
    // VDM: compact-addressable and readable; scissor: four-byte aligned.
    let vdm = cmd.vdm_ctrl_stream_base;
    let scissor = cmd.isp_scissor_base;
    if vdm & 3 != 0 || !compact(vdm) || scissor == 0 || scissor & 3 != 0 {
        return Err(EINVAL);
    }
    cover(vm, vdm, vdm + 4, true, false)?;
    gate::vdm(vm, vdm)?;
    cover(
        vm,
        scissor,
        scissor.checked_add(4).ok_or(EINVAL)?,
        true,
        false,
    )?;
    if cmd.isp_dbias_base != 0 {
        let base = cmd.isp_dbias_base;
        if base & 3 != 0 {
            return Err(EINVAL);
        }
        cover(vm, base, base.checked_add(4).ok_or(EINVAL)?, true, false)?;
    } else if cmd.flags & DBIAS_IS_INT != 0 {
        return Err(EINVAL);
    }
    if cmd.isp_oclqry_base != 0 {
        if cmd.isp_oclqry_base & 15 != 0 {
            return Err(EINVAL);
        }
        let base = cmd.isp_oclqry_base;
        cover(vm, base, base.checked_add(8).ok_or(EINVAL)?, true, true)?;
    }
    for program in [&cmd.bg, &cmd.eot, &cmd.partial_bg, &cmd.partial_eot] {
        if program.usc == 0 {
            continue;
        }
        let address = context
            .usc_base
            .checked_add(u64::from(program.usc & !63))
            .ok_or(EINVAL)?;
        cover(
            vm,
            address,
            address.checked_add(64).ok_or(EINVAL)?,
            true,
            false,
        )?;
    }
    check_zls_plane(context, cmd, false)?;
    check_zls_plane(context, cmd, true)?;
    Ok(())
}

/// Per-command compute admission.
fn check_compute(
    context: &Context<'_>,
    cmd: &uapi::drm_asahi_cmd_compute,
    attachments: &[uapi::drm_asahi_attachment],
) -> Result {
    let vm = context.vm;
    let (base, end) = (cmd.cdm_ctrl_stream_base, cmd.cdm_ctrl_stream_end);
    if cmd.flags != 0
        || cmd.sampler_count != 0
        || cmd.sampler_heap != 0
        || !helper_is_empty(&cmd.helper)
        || base & 3 != 0
        || end & 3 != 0
        || end <= base
        || end - base > (1 << 31)
    {
        return Err(EINVAL);
    }
    cover(vm, base, end, true, false)?;
    gate::cdm(vm, context.usc_base, base, end)?;
    check_attachments(vm, attachments, true)
}

/// Resolves timestamps and exact historical-prefix dependencies of a valid stream
/// ([`check_stream`]) for a queue that accepted `accepted` render and compute commands before
/// this ioctl.
pub(crate) fn prepare<'a>(
    bytes: &'a [u8],
    accepted: [u64; 2],
    objects: Pin<&xarray::XArray<KBox<file::Object>>>,
) -> Result<Prepared<'a>> {
    let mut resolved = KVec::new();
    for entry in stream(bytes) {
        let entry = entry?;
        let outputs = match entry.kind {
            CMD_RENDER => {
                let packet: uapi::drm_asahi_cmd_render = payload(entry.payload)?;
                let cmd = &packet;
                crate::timestamp::Destinations::resolve_aligned(
                    objects,
                    &[cmd.ts_vtx, cmd.ts_frag],
                )?
            }
            CMD_COMPUTE => {
                let packet: uapi::drm_asahi_cmd_compute = payload(entry.payload)?;
                let cmd = &packet;
                crate::timestamp::Destinations::resolve_aligned(objects, &[cmd.ts])?
            }
            _ => continue,
        };
        resolved.push(outputs, GFP_KERNEL)?;
    }

    // Historical priors and header barriers.
    let mut preceding = [0u16; 2];
    let mut orders = KVec::with_capacity(resolved.len(), GFP_KERNEL)?;
    for entry in stream(bytes) {
        let entry = entry?;
        let (engine, prior) = match entry.kind {
            CMD_RENDER => {
                let packet: uapi::drm_asahi_cmd_render = payload(entry.payload)?;
                (RENDER, [packet.prior_vdm, packet.prior_cdm])
            }
            CMD_COMPUTE => {
                let packet: uapi::drm_asahi_cmd_compute = payload(entry.payload)?;
                (COMPUTE, [packet.prior_vdm, packet.prior_cdm])
            }
            _ => continue,
        };
        orders.push(
            Order::new(accepted, preceding, entry.barriers, prior, engine)?,
            GFP_KERNEL,
        )?;
        preceding[engine] += 1;
    }

    Ok(Prepared {
        bytes,
        resolved,
        orders,
        counts: [u64::from(preceding[RENDER]), u64::from(preceding[COMPUTE])],
    })
}

impl Prepared<'_> {
    /// Checks each command's spans, programs and stream gates, with a job reference on the VM
    /// already held so that the checked mappings are retained until the job retires.
    pub(crate) fn validate(self, context: &Context<'_>) -> Result<Submission> {
        let Prepared {
            bytes,
            resolved,
            orders,
            counts,
        } = self;
        let mut commands = KVec::with_capacity(resolved.len(), GFP_KERNEL)?;
        let mut vertex = KVec::new();
        let mut fragment = KVec::new();
        let mut compute = KVec::new();
        let mut resolved = resolved.into_iter();
        for entry in stream(bytes) {
            let entry = entry?;
            match entry.kind {
                SET_VERTEX_ATTACHMENTS => vertex = attachment_list(entry.payload)?,
                SET_FRAGMENT_ATTACHMENTS => fragment = attachment_list(entry.payload)?,
                SET_COMPUTE_ATTACHMENTS => compute = attachment_list(entry.payload)?,
                CMD_RENDER => {
                    let packet: uapi::drm_asahi_cmd_render = payload(entry.payload)?;
                    let cmd = &packet;
                    let outputs = resolved.next().ok_or(EINVAL)?;
                    check_render(context, cmd)?;
                    check_attachments(context.vm, &vertex, true)?;
                    check_attachments(context.vm, &fragment, true)?;
                    commands.push(
                        Command::Render(KBox::new(
                            RenderCommand {
                                cmd: *cmd,
                                timestamp_outputs: outputs,
                                vertex_attachments: copy_list(&[&vertex])?,
                                fragment_attachments: copy_list(&[&fragment])?,
                            },
                            GFP_KERNEL,
                        )?),
                        GFP_KERNEL,
                    )?;
                }
                CMD_COMPUTE => {
                    let packet: uapi::drm_asahi_cmd_compute = payload(entry.payload)?;
                    let cmd = &packet;
                    check_compute(context, cmd, &compute)?;
                    let outputs = resolved.next().ok_or(EINVAL)?;
                    commands.push(
                        Command::Compute(KBox::new(
                            ComputeCommand {
                                cmd: *cmd,
                                attachments: copy_list(&[&compute])?,
                                timestamp_outputs: outputs,
                            },
                            GFP_KERNEL,
                        )?),
                        GFP_KERNEL,
                    )?;
                }
                _ => return Err(EINVAL),
            }
        }
        if commands.is_empty() {
            return Err(EINVAL);
        }
        Ok(Submission {
            counts,
            commands,
            orders,
        })
    }
}
