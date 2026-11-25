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
const STAGE_PREFIXES: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES;
const COMPUTE_NO_QUERY_ACCESS: u32 =
    uapi::drm_asahi_compute_flags_DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS as u32;

const RENDER_STAGE_PREFIXES_OFF: usize =
    offset_of!(uapi::drm_asahi_cmd_render_m5, stage_prefixes);
const RENDER_M5_SIZE: usize = size_of::<uapi::drm_asahi_cmd_render_m5>();

const _: () = {
    assert!(RENDER_STAGE_PREFIXES_OFF == size_of::<uapi::drm_asahi_cmd_render>());
    assert!(size_of::<uapi::drm_asahi_cmd_render>() == 448);
    assert!(RENDER_M5_SIZE == 480);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, ta_cdm_barrier) == 0);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, reserved0) == 2);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, reserved1) == 4);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, ta_prior_cdm) == 8);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, ta_vdm_barrier) == 16);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, reserved2) == 18);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, reserved3) == 20);
    assert!(offset_of!(uapi::drm_asahi_render_stage_prefixes, ta_prior_vdm) == 24);
    assert!(size_of::<uapi::drm_asahi_render_stage_prefixes>() == 32);
};

const CMD_RENDER: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER;
const CMD_COMPUTE: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE;
const SET_VERTEX_ATTACHMENTS: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_VERTEX_ATTACHMENTS;
const SET_FRAGMENT_ATTACHMENTS: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS;
const SET_COMPUTE_ATTACHMENTS: u32 = uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_ATTACHMENTS;

const MAX_DIMENSION: u16 = 16384;
const MAX_LAYERS: u16 = 2048;
const MAX_TILE_BUFFER: u32 = 32768;
const ZLS_METADATA_STRIDE: u64 = 0x80;
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
    pub(crate) ta_wait_through: [Option<u64>; 2],
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
            ta_wait_through: wait_through,
        })
    }

    /// Narrows the tiling half's prefixes to `tiling` (`DRM_ASAHI_RENDER_STAGE_PREFIXES`),
    /// requiring each stage prefix to stay within the command's full fragment prefix.
    pub(crate) fn narrow_tiling(&mut self, tiling: &Self) -> Result {
        for index in 0..2 {
            if tiling.wait_through[index].unwrap_or(0) > self.wait_through[index].unwrap_or(0) {
                return Err(EINVAL);
            }
        }
        self.ta_wait_through = tiling.wait_through;
        Ok(())
    }

    /// A same-engine command cannot jump a hole in its queue's published frontier.
    /// Cross-engine dependencies may also follow published predecessors via hardware
    /// ring barriers (FS -> CDM and CDM -> TA) while same-VM work pipelines; cross-VM
    /// switches, InitBM and timestamp query accesses still drain to completion.
    pub(crate) fn ready(&self, completed: [u64; 2], published: [u64; 2]) -> bool {
        let frontier = [
            published[RENDER].max(completed[RENDER]),
            published[COMPUTE].max(completed[COMPUTE]),
        ];
        frontier[self.engine].checked_add(1) == Some(self.sequence)
            && self
                .wait_through
                .iter()
                .zip(frontier)
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
    /// False when userspace asserts this dispatch neither copies nor resets timestamp queries.
    pub(crate) query_access: bool,
}

/// A validated command.
pub(crate) enum Command {
    Render(KBox<RenderCommand>),
    Compute(KBox<ComputeCommand>),
}

/// Inline storage for the common 1-2 command submission, spilling to a heap `KVec` above 2.
pub(crate) enum Commands {
    Empty,
    One([Command; 1]),
    Two([Command; 2]),
    Heap(KVec<Command>),
}

impl Commands {
    fn new() -> Self {
        Self::Empty
    }

    fn push(&mut self, command: Command) -> Result {
        match self {
            Self::Heap(vec) => vec.push(command, GFP_KERNEL)?,
            Self::Two(_) => {
                // Allocate before moving inline entries so OOM leaves `self` intact.
                let mut vec = KVec::with_capacity(4, GFP_KERNEL)?;
                if let Self::Two([a, b]) = core::mem::replace(self, Self::Empty) {
                    let _ = vec.push_within_capacity(a);
                    let _ = vec.push_within_capacity(b);
                    let _ = vec.push_within_capacity(command);
                }
                *self = Self::Heap(vec);
            }
            Self::One(_) => {
                if let Self::One([a]) = core::mem::replace(self, Self::Empty) {
                    *self = Self::Two([a, command]);
                }
            }
            Self::Empty => *self = Self::One([command]),
        }
        Ok(())
    }
}

impl core::ops::Deref for Commands {
    type Target = [Command];

    fn deref(&self) -> &[Command] {
        match self {
            Self::Empty => &[],
            Self::One(items) => items,
            Self::Two(items) => items,
            Self::Heap(vec) => vec,
        }
    }
}

impl core::ops::DerefMut for Commands {
    fn deref_mut(&mut self) -> &mut [Command] {
        match self {
            Self::Empty => &mut [],
            Self::One(items) => items,
            Self::Two(items) => items,
            Self::Heap(vec) => vec,
        }
    }
}

/// Inline storage for the common 1-2 command ordinals, spilling to a heap `KVec` above 2.
pub(crate) enum Orders {
    Empty,
    One([Order; 1]),
    Two([Order; 2]),
    Heap(KVec<Order>),
}

impl Orders {
    fn new() -> Self {
        Self::Empty
    }

    fn push(&mut self, order: Order) -> Result {
        match self {
            Self::Heap(vec) => vec.push(order, GFP_KERNEL)?,
            Self::Two([a, b]) => {
                let (a, b) = (*a, *b);
                let mut vec = KVec::with_capacity(4, GFP_KERNEL)?;
                let _ = vec.push_within_capacity(a);
                let _ = vec.push_within_capacity(b);
                let _ = vec.push_within_capacity(order);
                *self = Self::Heap(vec);
            }
            Self::One([a]) => *self = Self::Two([*a, order]),
            Self::Empty => *self = Self::One([order]),
        }
        Ok(())
    }
}

impl core::ops::Deref for Orders {
    type Target = [Order];

    fn deref(&self) -> &[Order] {
        match self {
            Self::Empty => &[],
            Self::One(items) => items,
            Self::Two(items) => items,
            Self::Heap(vec) => vec,
        }
    }
}

/// A validated SUBMIT.
pub(crate) struct Submission {
    /// The commands, in submission order.
    pub(crate) commands: Commands,
    /// Exact per-command queue-local ordinals, allocated before scheduler acceptance.
    pub(crate) orders: Orders,
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

/// A stream whose timestamps and ordinals are resolved ([`ParsedStream::prepare`]), before its
/// spans are checked ([`Prepared::validate`]).
pub(crate) struct Prepared {
    commands: Commands,
    orders: Orders,
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

/// Decodes the 32-byte `drm_asahi_render_stage_prefixes` tail from a 480-byte
/// `drm_asahi_cmd_render_m5` payload without adding a duplicate `AnyBitPattern` impl.
fn decode_stage_prefixes(tail: &[u8]) -> Result<uapi::drm_asahi_render_stage_prefixes> {
    let bytes: &[u8; 32] = tail.try_into().map_err(|_| EINVAL)?;
    let u16_at = |o: usize| u16::from_le_bytes([bytes[o], bytes[o + 1]]);
    let u32_at = |o: usize| {
        u32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]])
    };
    let u64_at = |o: usize| {
        u64::from_le_bytes([
            bytes[o],
            bytes[o + 1],
            bytes[o + 2],
            bytes[o + 3],
            bytes[o + 4],
            bytes[o + 5],
            bytes[o + 6],
            bytes[o + 7],
        ])
    };
    Ok(uapi::drm_asahi_render_stage_prefixes {
        ta_cdm_barrier: u16_at(0),
        reserved0: u16_at(2),
        reserved1: u32_at(4),
        ta_prior_cdm: u64_at(8),
        ta_vdm_barrier: u16_at(16),
        reserved2: u16_at(18),
        reserved3: u32_at(20),
        ta_prior_vdm: u64_at(24),
    })
}

/// Decodes a render command. With `DRM_ASAHI_RENDER_STAGE_PREFIXES`, the payload must be
/// the exact 480-byte `drm_asahi_cmd_render_m5` with zero reserved fields and without
/// `DRM_ASAHI_RENDER_FRAGMENT_BARRIERS`. Without the flag, any trailing bytes must be zero.
fn render_payload(
    bytes: &[u8],
) -> Result<(
    uapi::drm_asahi_cmd_render,
    Option<uapi::drm_asahi_render_stage_prefixes>,
)> {
    let head_len = bytes.len().min(RENDER_STAGE_PREFIXES_OFF);
    let cmd: uapi::drm_asahi_cmd_render = Reader::new(&bytes[..head_len]).read_up_to(head_len)?;
    let tail = bytes.get(RENDER_STAGE_PREFIXES_OFF..).unwrap_or(&[]);
    if cmd.flags & STAGE_PREFIXES != 0 {
        if bytes.len() != RENDER_M5_SIZE || cmd.flags & FRAGMENT_BARRIERS != 0 {
            return Err(EINVAL);
        }
        let prefixes = decode_stage_prefixes(tail)?;
        if prefixes.reserved0 != 0
            || prefixes.reserved1 != 0
            || prefixes.reserved2 != 0
            || prefixes.reserved3 != 0
        {
            return Err(EINVAL);
        }
        Ok((cmd, Some(prefixes)))
    } else {
        if tail.iter().any(|&byte| byte != 0) {
            return Err(EINVAL);
        }
        Ok((cmd, None))
    }
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

fn validate_attachments_header(entry: &Entry<'_>) -> Result {
    if entry.barriers != [NONE; 2]
        || entry.payload.len() % ATTACHMENT_SIZE != 0
        || entry.payload.len() / ATTACHMENT_SIZE > hw::sizes::MAX_ATTACHMENTS
    {
        return Err(EINVAL);
    }
    Ok(())
}

fn validate_attachment(attachment: &uapi::drm_asahi_attachment, range: &Range<u64>) -> Result {
    if attachment.pad != 0
        || attachment.flags != 0
        || !inside(attachment.pointer, attachment.size, range)
    {
        return Err(EINVAL);
    }
    Ok(())
}

fn validate_render_entry(
    entry: &Entry<'_>,
    counts: [u32; 2],
    early_tiling: bool,
) -> Result<(
    uapi::drm_asahi_cmd_render,
    Option<uapi::drm_asahi_render_stage_prefixes>,
)> {
    let (packet, prefixes) = render_payload(entry.payload)?;
    let cmd = &packet;
    // Ray-query extensions belong to G17; reject every nonzero byte
    // between the common historical-prefix fields and the stage-prefix tail.
    let end = offset_of!(uapi::drm_asahi_cmd_render, prior_cdm) + size_of::<u64>();
    let rq_end = entry.payload.len().min(RENDER_STAGE_PREFIXES_OFF);
    if entry
        .payload
        .get(end..rq_end)
        .is_some_and(|tail| tail.iter().any(|&b| b != 0))
    {
        return Err(EINVAL);
    }
    if let Some(prefixes) = prefixes {
        if (prefixes.ta_vdm_barrier != NONE && u32::from(prefixes.ta_vdm_barrier) > counts[RENDER])
            || (prefixes.ta_cdm_barrier != NONE
                && u32::from(prefixes.ta_cdm_barrier) > counts[COMPUTE])
        {
            return Err(EINVAL);
        }
    }
    check_render_helpers(cmd)?;
    // M4 HAL200 resource words carry all four high halves.
    let allowed = PROCESS_EMPTY_TILES
        | DBIAS_IS_INT
        | RSRC_SPEC_HI
        | if early_tiling {
            FRAGMENT_BARRIERS | STAGE_PREFIXES
        } else {
            0
        };
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
    Ok((packet, prefixes))
}

fn validate_compute_entry(
    entry: &Entry<'_>,
    range: &Range<u64>,
) -> Result<uapi::drm_asahi_cmd_compute> {
    let packet: uapi::drm_asahi_cmd_compute = payload(entry.payload)?;
    let cmd = &packet;
    if cmd.flags & !COMPUTE_NO_QUERY_ACCESS != 0
        || cmd.cdm_ctrl_stream_end <= cmd.cdm_ctrl_stream_base
        || !inside(
            cmd.cdm_ctrl_stream_base,
            cmd.cdm_ctrl_stream_end - cmd.cdm_ctrl_stream_base,
            range,
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
    Ok(packet)
}

/// Checks that `[start, end)` (rounded out to pages) lies inside the user address range and
/// outside every driver-reserved range, returning `(first_page, byte_size)`.
fn align_span(start: u64, end: u64) -> Result<(u64, u64)> {
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
    Ok((first, last - first))
}

/// Batches page-aligned GPUVM coverage checks under one `gpuvm.lock_inner()` hold while
/// preserving exact per-span `ENOENT` / `EACCES` / `EINVAL` error reporting on failure.
struct SpanBatch {
    ranges: [(u64, u64, u8); 48],
    len: usize,
}

impl SpanBatch {
    const fn new() -> Self {
        Self {
            ranges: [(0, 0, 0); 48],
            len: 0,
        }
    }

    fn push(&mut self, vm: &mmu::Vm, start: u64, end: u64, read: bool, write: bool) -> Result {
        let (first, size) = align_span(start, end)?;
        let access = u8::from(read) | (u8::from(write) << 1);
        if self.ranges[..self.len]
            .iter()
            .any(|&(a, s, acc)| a == first && s == size && (acc & access) == access)
        {
            return Ok(());
        }
        if self.len == self.ranges.len() {
            self.flush(vm)?;
        }
        self.ranges[self.len] = (first, size, access);
        self.len += 1;
        Ok(())
    }

    fn flush(&mut self, vm: &mmu::Vm) -> Result {
        let n = self.len;
        if n == 0 {
            return Ok(());
        }
        self.len = 0;
        if vm.covers_ranges_batch(&self.ranges[..n]).0 {
            return Ok(());
        }
        for &(first, size, access) in &self.ranges[..n] {
            let read = access & 1 != 0;
            let write = access & 2 != 0;
            if !vm.covers_range(first, size, read, write) {
                return if vm.covers_range(first, size, false, false) {
                    Err(EACCES)
                } else {
                    Err(ENOENT)
                };
            }
        }
        Ok(())
    }

    fn flush_locked(&mut self, guard: &mut mmu::VmReadGuard<'_>) -> Result {
        let n = self.len;
        if n == 0 {
            return Ok(());
        }
        self.len = 0;
        for &(first, size, access) in &self.ranges[..n] {
            let read = access & 1 != 0;
            let write = access & 2 != 0;
            if !guard.covers_range(first, size, read, write) {
                return if guard.covers_range(first, size, false, false) {
                    Err(EACCES)
                } else {
                    Err(ENOENT)
                };
            }
        }
        Ok(())
    }
}

/// Attachment spans, 128-byte granular, actually mapped with `write` or read access.
fn check_attachments(
    batch: &mut SpanBatch,
    vm: &mmu::Vm,
    list: &[uapi::drm_asahi_attachment],
    write: bool,
) -> Result {
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
        batch.push(vm, first, last, true, write)?;
    }
    Ok(())
}

/// Parses the attachment list of a SET_*_ATTACHMENTS command, validating every entry in one pass.
fn attachment_list_checked(
    bytes: &[u8],
    range: &Range<u64>,
) -> Result<KVec<uapi::drm_asahi_attachment>> {
    let mut list = KVec::with_capacity(bytes.len() / ATTACHMENT_SIZE, GFP_KERNEL)?;
    for bytes in bytes.chunks_exact(ATTACHMENT_SIZE) {
        let attachment: uapi::drm_asahi_attachment = Reader::new(bytes).read()?;
        validate_attachment(&attachment, range)?;
        list.push(attachment, GFP_KERNEL)?;
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

/// Checks that `[start, end)` (rounded out to pages) is user memory mapped with the requested
/// GPU access under an already-held [`mmu::VmReadGuard`].
pub(crate) fn cover_locked(
    guard: &mut mmu::VmReadGuard<'_>,
    start: u64,
    end: u64,
    read: bool,
    write: bool,
) -> Result {
    let (first, size) = align_span(start, end)?;
    if guard.covers_range(first, size, read, write) {
        Ok(())
    } else if guard.covers_range(first, size, false, false) {
        Err(EACCES)
    } else {
        Err(ENOENT)
    }
}

/// Whether a GVA can be named by a 32-bit compact pointer.
fn compact(gva: u64) -> bool {
    (0x1000000000..0x9000000000).contains(&gva)
}

/// Validate Z/S register state (including compressed metadata) and coverage through each layer
/// base. This is a minimum span check, not a complete plane-extent validation: the UAPI does
/// not describe the complete ZLS surface layout. The encoded stride is a register word, not a
/// byte distance (the C HAL200 contract).
fn check_zls_plane(
    batch: &mut SpanBatch,
    context: &Context<'_>,
    cmd: &uapi::drm_asahi_cmd_render,
    stencil: bool,
) -> Result {
    let (z, mask, comp_mask) = if stencil {
        (
            &cmd.stencil,
            (1u64 << 3) | (1 << 4) | (1 << 7) | (1 << 8) | (1 << 14) | (1 << 18),
            (1u64 << 4) | (1 << 8),
        )
    } else {
        (
            &cmd.depth,
            (1u64 << 1) | (1 << 2) | (1 << 5) | (1 << 6) | (1 << 15) | (1 << 19) | (3 << 25),
            (1u64 << 2) | (1 << 6),
        )
    };
    let compressed = cmd.zls_ctrl & comp_mask != 0;
    if (z.base == 0
        && (z.stride != 0
            || z.comp_base != 0
            || z.comp_stride != 0
            || cmd.zls_ctrl & mask != 0))
        || (z.base != 0 && cmd.layers > 1 && z.stride == 0)
        || (z.stride != 0 && z.stride & 0x3fff != 1)
        || z.base & 3 != 0
        || (z.comp_base == 0 && z.comp_stride != 0)
        || (z.comp_base != 0) != compressed
        || z.comp_base & 3 != 0
        || z.comp_stride & 0x3fff != 0
    {
        return Err(EINVAL);
    }
    if z.base != 0 {
        let distance = (u64::from(z.stride >> 14) + 1) * 0x4000;
        let minimum = distance
            .checked_mul(u64::from(cmd.layers - 1))
            .and_then(|bytes| bytes.checked_add(1))
            .ok_or(EINVAL)?;
        batch.push(
            context.vm,
            z.base,
            z.base.checked_add(minimum).ok_or(EINVAL)?,
            true,
            true,
        )?;
    }
    if z.comp_base != 0 {
        let distance = (u64::from(z.comp_stride >> 14) + 1) * ZLS_METADATA_STRIDE;
        let minimum = distance
            .checked_mul(u64::from(cmd.layers - 1))
            .and_then(|bytes| bytes.checked_add(1))
            .ok_or(EINVAL)?;
        batch.push(
            context.vm,
            z.comp_base,
            z.comp_base.checked_add(minimum).ok_or(EINVAL)?,
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

/// Per-command render admission. Pushes VM spans into `batch` so a multi-pass SUBMIT flushes
/// coverage once across all passes.
fn check_render(
    batch: &mut SpanBatch,
    context: &Context<'_>,
    cmd: &uapi::drm_asahi_cmd_render,
    vertex: &[uapi::drm_asahi_attachment],
    fragment: &[uapi::drm_asahi_attachment],
) -> Result {
    check_render_helpers(cmd)?;
    let vm = context.vm;
    let allowed = PROCESS_EMPTY_TILES
        | RSRC_SPEC_HI
        | DBIAS_IS_INT
        | if super::params::early_tiling() {
            FRAGMENT_BARRIERS | STAGE_PREFIXES
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
        || cmd.zls_ctrl & !0x00000000060cc1fe != 0
        || cmd.isp_zls_pixels & 0xc0000000 != 0
        || !matches!((cmd.zls_ctrl >> 25) & 3, 0 | 2)
        || ((cmd.zls_ctrl >> 1) & 1) != ((cmd.zls_ctrl >> 5) & 1)
        || ((cmd.zls_ctrl >> 2) & 1) != ((cmd.zls_ctrl >> 6) & 1)
        || ((cmd.zls_ctrl >> 3) & 1) != ((cmd.zls_ctrl >> 7) & 1)
        || ((cmd.zls_ctrl >> 4) & 1) != ((cmd.zls_ctrl >> 8) & 1)
    {
        return Err(EINVAL);
    }
    // VDM: compact-addressable and readable; scissor: four-byte aligned.
    let vdm = cmd.vdm_ctrl_stream_base;
    let scissor = cmd.isp_scissor_base;
    if vdm & 3 != 0 || !compact(vdm) || scissor == 0 || scissor & 3 != 0 {
        return Err(EINVAL);
    }
    batch.push(vm, vdm, vdm + 4, true, false)?;
    batch.push(
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
        batch.push(vm, base, base.checked_add(4).ok_or(EINVAL)?, true, false)?;
    } else if cmd.flags & DBIAS_IS_INT != 0 {
        return Err(EINVAL);
    }
    if cmd.isp_oclqry_base != 0 {
        if cmd.isp_oclqry_base & 15 != 0 {
            return Err(EINVAL);
        }
        let base = cmd.isp_oclqry_base;
        batch.push(vm, base, base.checked_add(8).ok_or(EINVAL)?, true, true)?;
    }
    for program in [&cmd.bg, &cmd.eot, &cmd.partial_bg, &cmd.partial_eot] {
        if program.usc == 0 {
            continue;
        }
        let address = context
            .usc_base
            .checked_add(u64::from(program.usc & !63))
            .ok_or(EINVAL)?;
        batch.push(
            vm,
            address,
            address.checked_add(64).ok_or(EINVAL)?,
            true,
            false,
        )?;
    }
    check_zls_plane(batch, context, cmd, false)?;
    check_zls_plane(batch, context, cmd, true)?;
    check_attachments(batch, vm, vertex, true)?;
    check_attachments(batch, vm, fragment, true)?;
    Ok(())
}

/// Per-command compute admission.
fn check_compute(
    batch: &mut SpanBatch,
    context: &Context<'_>,
    cmd: &uapi::drm_asahi_cmd_compute,
    attachments: &[uapi::drm_asahi_attachment],
) -> Result {
    let vm = context.vm;
    let (base, end) = (cmd.cdm_ctrl_stream_base, cmd.cdm_ctrl_stream_end);
    if cmd.flags & !COMPUTE_NO_QUERY_ACCESS != 0
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
    batch.push(vm, base, end, true, false)?;
    check_attachments(batch, vm, attachments, true)?;
    batch.flush(vm)?;
    gate::cdm(vm, context.usc_base, base, end)
}

/// Per-command barrier and stage-prefix metadata captured during single-pass parsing.
#[derive(Copy, Clone)]
struct OrderMeta {
    barriers: [u16; 2],
    prefixes: Option<uapi::drm_asahi_render_stage_prefixes>,
}

/// Inline storage for 1-2 command barrier metadata records, spilling to a heap `KVec` above 2.
enum OrderMetas {
    Empty,
    One([OrderMeta; 1]),
    Two([OrderMeta; 2]),
    Heap(KVec<OrderMeta>),
}

impl OrderMetas {
    fn new() -> Self {
        Self::Empty
    }

    fn push(&mut self, meta: OrderMeta) -> Result {
        match self {
            Self::Heap(vec) => vec.push(meta, GFP_KERNEL)?,
            Self::Two([a, b]) => {
                let (a, b) = (*a, *b);
                let mut vec = KVec::with_capacity(4, GFP_KERNEL)?;
                let _ = vec.push_within_capacity(a);
                let _ = vec.push_within_capacity(b);
                let _ = vec.push_within_capacity(meta);
                *self = Self::Heap(vec);
            }
            Self::One([a]) => *self = Self::Two([*a, meta]),
            Self::Empty => *self = Self::One([meta]),
        }
        Ok(())
    }
}

impl core::ops::Deref for OrderMetas {
    type Target = [OrderMeta];

    fn deref(&self) -> &[OrderMeta] {
        match self {
            Self::Empty => &[],
            Self::One(items) => items,
            Self::Two(items) => items,
            Self::Heap(vec) => vec,
        }
    }
}

/// Tracks the active attachment list during single-pass parsing so the first consuming command
/// takes ownership without copying, while later commands sharing the same list clone from it.
enum AttachmentState {
    Empty,
    Fresh(KVec<uapi::drm_asahi_attachment>),
    FromCommand(usize),
}

/// A structurally valid, decoded command stream before timestamp/ordinal resolution and VM checks.
pub(crate) struct ParsedStream {
    commands: Commands,
    meta: OrderMetas,
    counts: [u64; 2],
}

/// Structurally validates and decodes a command stream in a single pass, before anything is
/// looked up or device state is consulted.
pub(crate) fn parse_stream(bytes: &[u8], early_tiling: bool) -> Result<ParsedStream> {
    if bytes.is_empty() || bytes.len() > MAX_CMDBUF || bytes.len() % 8 != 0 {
        return Err(EINVAL);
    }
    let range = hw::USER_RANGE;
    let mut counts = [0u32; 2];
    let mut commands = Commands::new();
    let mut meta = OrderMetas::new();
    let mut vertex = AttachmentState::Empty;
    let mut fragment = AttachmentState::Empty;
    let mut compute = AttachmentState::Empty;

    for entry in stream(bytes) {
        let entry = entry?;
        match entry.kind {
            SET_VERTEX_ATTACHMENTS | SET_FRAGMENT_ATTACHMENTS | SET_COMPUTE_ATTACHMENTS => {
                validate_attachments_header(&entry)?;
                let list = attachment_list_checked(entry.payload, &range)?;
                let state = if list.is_empty() {
                    AttachmentState::Empty
                } else {
                    AttachmentState::Fresh(list)
                };
                match entry.kind {
                    SET_VERTEX_ATTACHMENTS => vertex = state,
                    SET_FRAGMENT_ATTACHMENTS => fragment = state,
                    SET_COMPUTE_ATTACHMENTS => compute = state,
                    _ => {}
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
        let next_idx = commands.len();
        match entry.kind {
            CMD_RENDER => {
                let (packet, prefixes) = validate_render_entry(&entry, counts, early_tiling)?;
                counts[RENDER] += 1;
                if counts[RENDER] + counts[COMPUTE] > MAX_COMMANDS {
                    return Err(EINVAL);
                }
                let take_render = |state: &mut AttachmentState, is_fragment: bool| {
                    match core::mem::replace(state, AttachmentState::Empty) {
                        AttachmentState::Empty => Ok(KVec::new()),
                        AttachmentState::Fresh(list) => {
                            *state = AttachmentState::FromCommand(next_idx);
                            Ok(list)
                        }
                        AttachmentState::FromCommand(prev_idx) => {
                            *state = AttachmentState::FromCommand(prev_idx);
                            match &commands[prev_idx] {
                                Command::Render(prev) => {
                                    let src = if is_fragment {
                                        &prev.fragment_attachments
                                    } else {
                                        &prev.vertex_attachments
                                    };
                                    copy_list(&[src])
                                }
                                Command::Compute(_) => Err(EINVAL),
                            }
                        }
                    }
                };
                let vertex_attachments = take_render(&mut vertex, false)?;
                let fragment_attachments = take_render(&mut fragment, true)?;
                commands.push(Command::Render(KBox::new(
                    RenderCommand {
                        cmd: packet,
                        timestamp_outputs: crate::timestamp::Destinations::empty(),
                        vertex_attachments,
                        fragment_attachments,
                    },
                    GFP_KERNEL,
                )?))?;
                meta.push(OrderMeta {
                    barriers: entry.barriers,
                    prefixes,
                })?;
            }
            CMD_COMPUTE => {
                let packet = validate_compute_entry(&entry, &range)?;
                counts[COMPUTE] += 1;
                if counts[RENDER] + counts[COMPUTE] > MAX_COMMANDS {
                    return Err(EINVAL);
                }
                let attachments = match core::mem::replace(&mut compute, AttachmentState::Empty) {
                    AttachmentState::Empty => KVec::new(),
                    AttachmentState::Fresh(list) => {
                        compute = AttachmentState::FromCommand(next_idx);
                        list
                    }
                    AttachmentState::FromCommand(prev_idx) => {
                        compute = AttachmentState::FromCommand(prev_idx);
                        match &commands[prev_idx] {
                            Command::Compute(prev) => copy_list(&[&prev.attachments])?,
                            Command::Render(_) => return Err(EINVAL),
                        }
                    }
                };
                commands.push(Command::Compute(KBox::new(
                    ComputeCommand {
                        cmd: packet,
                        attachments,
                        timestamp_outputs: crate::timestamp::Destinations::empty(),
                        query_access: packet.flags & COMPUTE_NO_QUERY_ACCESS == 0,
                    },
                    GFP_KERNEL,
                )?))?;
                meta.push(OrderMeta {
                    barriers: entry.barriers,
                    prefixes: None,
                })?;
            }
            _ => return Err(EINVAL),
        }
    }
    if counts == [0; 2] {
        return Err(EINVAL);
    }
    Ok(ParsedStream {
        commands,
        meta,
        counts: [u64::from(counts[RENDER]), u64::from(counts[COMPUTE])],
    })
}

impl ParsedStream {
    /// Resolves timestamps in place and computes exact historical-prefix dependencies for a queue
    /// that accepted `accepted` render and compute commands before this ioctl.
    pub(crate) fn prepare(
        mut self,
        accepted: [u64; 2],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result<Prepared> {
        let mut orders = Orders::new();
        let mut preceding = [0u16; 2];
        for (command, meta) in self.commands.iter_mut().zip(self.meta.iter()) {
            let (engine, prior) = match command {
                Command::Render(render) => {
                    render.timestamp_outputs = crate::timestamp::Destinations::resolve_aligned(
                        objects,
                        &[render.cmd.ts_vtx, render.cmd.ts_frag],
                    )?;
                    (RENDER, [render.cmd.prior_vdm, render.cmd.prior_cdm])
                }
                Command::Compute(compute) => {
                    compute.timestamp_outputs = crate::timestamp::Destinations::resolve_aligned(
                        objects,
                        &[compute.cmd.ts],
                    )?;
                    (COMPUTE, [compute.cmd.prior_vdm, compute.cmd.prior_cdm])
                }
            };
            let mut order = Order::new(accepted, preceding, meta.barriers, prior, engine)?;
            if let Some(prefixes) = meta.prefixes {
                let tiling = Order::new(
                    accepted,
                    preceding,
                    [prefixes.ta_vdm_barrier, prefixes.ta_cdm_barrier],
                    [prefixes.ta_prior_vdm, prefixes.ta_prior_cdm],
                    RENDER,
                )?;
                order.narrow_tiling(&tiling)?;
            }
            orders.push(order)?;
            preceding[engine] += 1;
        }
        Ok(Prepared {
            commands: self.commands,
            orders,
            counts: self.counts,
        })
    }
}

impl Prepared {
    /// Checks each command's spans, programs and stream gates, with a job reference on the VM
    /// already held so that the checked mappings are retained until the job retires.
    pub(crate) fn validate(self, context: &Context<'_>) -> Result<Submission> {
        let Prepared {
            commands,
            orders,
            counts,
        } = self;
        if commands.is_empty() {
            return Err(EINVAL);
        }
        let mut batch = SpanBatch::new();
        let mut vdm_vas = [0u64; MAX_COMMANDS as usize];
        let mut vdm_count = 0usize;
        for command in commands.iter() {
            match command {
                Command::Render(render) => {
                    check_render(
                        &mut batch,
                        context,
                        &render.cmd,
                        &render.vertex_attachments,
                        &render.fragment_attachments,
                    )?;
                    if vdm_count >= vdm_vas.len() {
                        return Err(EINVAL);
                    }
                    vdm_vas[vdm_count] = render.cmd.vdm_ctrl_stream_base;
                    vdm_count += 1;
                }
                Command::Compute(compute) => {
                    check_compute(&mut batch, context, &compute.cmd, &compute.attachments)?;
                }
            }
        }
        if vdm_count != 0 {
            let mut guard = context.vm.read_lock().map_err(|_| EFAULT)?;
            batch.flush_locked(&mut guard)?;
            gate::vdm_batch_locked(&mut guard, &vdm_vas[..vdm_count])?;
        } else {
            batch.flush(context.vm)?;
        }
        Ok(Submission {
            counts,
            commands,
            orders,
        })
    }
}
