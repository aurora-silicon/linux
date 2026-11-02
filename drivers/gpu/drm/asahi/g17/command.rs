// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Decode a submission and validate its user resources before publishing any work.

use super::fw::{
    compute::{IntersectionTables, ScratchRequest},
    kick::WriteRange,
    m5,
    render::{RenderPass, UscProgram, ZlsSurface},
    render_rt::RayQueryStage,
};
use crate::util::{AnyBitPattern, Reader};
use core::ops::Range;
use kernel::{prelude::*, uapi};

const _: () = assert!(uapi::drm_asahi_compute_flags_DRM_ASAHI_COMPUTE_AS_BUILD == 1 << 2);

const MAX_ATTACHMENTS: usize = crate::microseq::MAX_ATTACHMENTS;
const USC_WINDOW_SIZE: u64 = 1 << 32;
const MAX_SAMPLERS: u32 = 1024;
const RENDER_BASE_SIZE: usize = core::mem::offset_of!(uapi::drm_asahi_cmd_render, prior_cdm) + size_of::<u64>();
const COMPUTE_BASE_SIZE: usize = core::mem::offset_of!(uapi::drm_asahi_cmd_compute, prior_cdm) + size_of::<u64>();
/// The timestamp-copy arrays of the common command layout, which G17P does not implement.
const RENDER_TIMESTAMP_COPIES: Range<usize> = core::mem::offset_of!(uapi::drm_asahi_cmd_render, ts_frag_end_copies)
    ..core::mem::offset_of!(uapi::drm_asahi_cmd_render, prior_vdm);
const COMPUTE_TIMESTAMP_COPIES: Range<usize> = core::mem::offset_of!(uapi::drm_asahi_cmd_compute, ts_end_copies)
    ..core::mem::offset_of!(uapi::drm_asahi_cmd_compute, scs_layout);
const SAMPLER_SIZE: u64 = 8;
const PAGE_SIZE: u64 = 0x4000;
const ZLS_METADATA_STRIDE: u64 = 0x80;
const MAX_DIMENSION: u16 = 16384;
const MAX_LAYERS: u16 = 2048;
const MAX_TILE_BUFFER: u64 = 32768;
const PROCESS_EMPTY: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES;
const RSRC_SPEC_HI: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_RSRC_SPEC_HI;
const DBIAS_INT: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_DBIAS_IS_INT;
const FEW_PRIMITIVES: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FEW_PRIMITIVES;
const FRAGMENT_BARRIERS: u32 = uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS;
const RENDER_FLAGS: u32 =
    PROCESS_EMPTY | RSRC_SPEC_HI | DBIAS_INT | FEW_PRIMITIVES | FRAGMENT_BARRIERS;

/// Attachment settings persist until replaced, including by an empty list.
#[derive(Clone, Copy)]
pub(crate) struct Attachments {
    ranges: [WriteRange; MAX_ATTACHMENTS],
    count: usize,
}

impl Attachments {
    const EMPTY: Self = Self {
        ranges: [WriteRange { va: 0, size: 0 }; MAX_ATTACHMENTS],
        count: 0,
    };

    pub(crate) fn as_slice(&self) -> &[WriteRange] {
        &self.ranges[..self.count]
    }

    fn parse(bytes: &[u8]) -> Result<Self> {
        let size = size_of::<uapi::drm_asahi_attachment>();
        if bytes.len() % size != 0 || bytes.len() / size > MAX_ATTACHMENTS {
            return Err(EINVAL);
        }
        let mut list = Self::EMPTY;
        let mut reader = Reader::new(bytes);
        for range in &mut list.ranges[..bytes.len() / size] {
            let item: uapi::drm_asahi_attachment = reader.read()?;
            if item.flags != 0 || item.pad != 0 {
                return Err(EINVAL);
            }
            *range = WriteRange {
                va: item.pointer,
                size: item.size,
            };
            list.count += 1;
        }
        Ok(list)
    }

    fn validate(&self, space: &impl AddressSpace) -> Result {
        for range in self.as_slice() {
            require_range(space, range.va, range.size, Access::Write)?;
        }
        Ok(())
    }
}

// Commands stay inline in the submission plan, avoiding an allocation per render pass.
#[allow(clippy::large_enum_variant)]
pub(crate) enum Payload {
    /// With the render storage declarations in force (`DRM_ASAHI_SET_RENDER_RESOURCES`), if any.
    Render(uapi::drm_asahi_cmd_render, [Attachments; 2], [RayQueryStage; 2], Option<RenderDeclared>),
    /// With the storage declarations in force (`DRM_ASAHI_SET_COMPUTE_RESOURCES`), if any.
    Compute(uapi::drm_asahi_cmd_compute, Attachments, IntersectionTables, Option<m5::compute::Declared>),
}

pub(crate) struct Command {
    /// Render and compute prefix indices, before this command.
    pub(crate) barriers: [u16; 2],
    pub(crate) payload: Payload,
}

/// Parser over the single userspace snapshot taken by the submit ioctl.
/// Call `finish` after consuming it to reject submissions with no hardware work.
pub(crate) struct Parser<'a> {
    bytes: &'a [u8],
    counts: [u16; 2],
    attachments: [Attachments; 3],
    /// Whether this submission path takes compute storage declarations (the M5 one).
    resources: bool,
    declared: Option<m5::compute::Declared>,
    render_declared: Option<RenderDeclared>,
}

/// Render programs one SET_RENDER_RESOURCES record declares at most.
pub(crate) const MAX_RENDER_PROGRAMS: usize = 64;

/// A render pass's linked-program storage declarations (`DRM_ASAHI_SET_RENDER_RESOURCES`):
/// every vertex and fragment program of the pass, background and end-of-tile included, planned
/// together ([`m5::compute::Gpu::plan_render`]).
#[derive(Clone, Copy)]
pub(crate) struct RenderDeclared {
    programs: [uapi::drm_asahi_render_program; MAX_RENDER_PROGRAMS],
    count: usize,
}

impl RenderDeclared {
    /// Parses a payload of 40-byte records, which need not be eight-byte aligned. Stages and
    /// flags are checked by the plan.
    fn parse(bytes: &[u8]) -> Result<Self> {
        let size = size_of::<uapi::drm_asahi_render_program>();
        let count = bytes.len() / size;
        if bytes.len() % size != 0 || count > MAX_RENDER_PROGRAMS {
            return Err(EINVAL);
        }
        let mut declared = Self {
            programs: [uapi::drm_asahi_render_program::default(); MAX_RENDER_PROGRAMS],
            count,
        };
        let mut reader = Reader::new(bytes);
        for program in &mut declared.programs[..count] {
            *program = reader.read()?;
        }
        Ok(declared)
    }

    pub(crate) fn programs(&self) -> &[uapi::drm_asahi_render_program] {
        &self.programs[..self.count]
    }
}

impl<'a> Parser<'a> {
    pub(crate) fn new(bytes: &'a [u8], variant: &super::fw::Variant) -> Self {
        Self {
            bytes,
            counts: [0; 2],
            attachments: [Attachments::EMPTY; 3],
            resources: variant.submission == super::fw::variant::Submission::M5HostCommand,
            declared: None,
            render_declared: None,
        }
    }

    pub(crate) fn next(&mut self) -> Result<Option<Command>> {
        while !self.bytes.is_empty() {
            let mut reader = Reader::new(self.bytes);
            let header: uapi::drm_asahi_cmd_header = reader.read()?;
            let start = size_of::<uapi::drm_asahi_cmd_header>();
            let end = start + header.size as usize;
            let bytes = self.bytes.get(start..end).ok_or(EINVAL)?;
            self.bytes = &self.bytes[end..];
            let barriers = [header.vdm_barrier, header.cdm_barrier];
            match u32::from(header.cmd_type) {
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER
                | uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE => {
                    if barriers.iter().zip(self.counts).any(|(&barrier, count)| {
                        barrier != uapi::DRM_ASAHI_BARRIER_NONE as u16 && barrier > count
                    }) || u32::from(self.counts[0]) + u32::from(self.counts[1])
                        >= crate::file::MAX_COMMANDS_PER_SUBMISSION
                    {
                        return Err(EINVAL);
                    }
                    let payload = if u32::from(header.cmd_type)
                        == uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER
                    {
                        self.counts[0] += 1;
                        no_timestamp_copies(bytes, RENDER_TIMESTAMP_COPIES)?;
                        Payload::Render(
                            payload(&bytes[..bytes.len().min(RENDER_BASE_SIZE)])?,
                            [self.attachments[0], self.attachments[1]],
                            RayQueryStage::parse_pair(bytes.get(RENDER_BASE_SIZE..).unwrap_or(&[]))?,
                            self.render_declared,
                        )
                    } else {
                        self.counts[1] += 1;
                        no_timestamp_copies(bytes, COMPUTE_TIMESTAMP_COPIES)?;
                        Payload::Compute(
                            payload(&bytes[..bytes.len().min(COMPUTE_BASE_SIZE)])?,
                            self.attachments[2],
                            IntersectionTables::parse(bytes.get(COMPUTE_BASE_SIZE..).unwrap_or(&[]))?,
                            self.declared,
                        )
                    };
                    return Ok(Some(Command { barriers, payload }));
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_VERTEX_ATTACHMENTS
                | uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS
                | uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_ATTACHMENTS => {
                    if barriers != [uapi::DRM_ASAHI_BARRIER_NONE as u16; 2] {
                        return Err(EINVAL);
                    }
                    let slot = header.cmd_type as usize
                        - uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_VERTEX_ATTACHMENTS as usize;
                    self.attachments[slot] = Attachments::parse(bytes)?;
                }
                // A software record: it replaces the declarations of the commands that follow.
                uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_RESOURCES if self.resources => {
                    if barriers != [uapi::DRM_ASAHI_BARRIER_NONE as u16; 2] {
                        return Err(EINVAL);
                    }
                    self.declared = Some(declarations(bytes)?);
                }
                // Likewise for the renders that follow; it advances neither command index.
                uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_RENDER_RESOURCES if self.resources => {
                    if barriers != [uapi::DRM_ASAHI_BARRIER_NONE as u16; 2] {
                        return Err(EINVAL);
                    }
                    self.render_declared = Some(RenderDeclared::parse(bytes)?);
                }
                _ => return Err(EINVAL),
            }
        }
        Ok(None)
    }

    pub(crate) fn finish(&self) -> Result {
        if !self.bytes.is_empty() || self.counts == [0; 2] {
            return Err(EINVAL);
        }
        Ok(())
    }
}

/// Merges a `DRM_ASAHI_SET_COMPUTE_RESOURCES` payload, which need not be eight-byte aligned.
fn declarations(bytes: &[u8]) -> Result<m5::compute::Declared> {
    let size = size_of::<uapi::drm_asahi_compute_program>();
    if bytes.len() % size != 0 {
        return Err(EINVAL);
    }
    let mut declared = m5::compute::Declared::default();
    let mut reader = Reader::new(bytes);
    for _ in 0..bytes.len() / size {
        let program: uapi::drm_asahi_compute_program = reader.read()?;
        declared.include(&program)?;
    }
    Ok(declared)
}

/// Reject a command that asks for timestamp copies.
fn no_timestamp_copies(bytes: &[u8], copies: Range<usize>) -> Result {
    let end = copies.end.min(bytes.len());
    let start = copies.start.min(end);
    if bytes[start..end].iter().any(|&b| b != 0) {
        return Err(EINVAL);
    }
    Ok(())
}

fn payload<T: AnyBitPattern>(bytes: &[u8]) -> Result<T> {
    if bytes
        .get(size_of::<T>()..)
        .is_some_and(|tail| tail.iter().any(|&b| b != 0))
    {
        return Err(EINVAL);
    }
    Reader::new(bytes).read_up_to(bytes.len())
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Access {
    Read,
    Write,
    ReadWrite,
}

/// The caller retains the VM mappings while validating and executing the command.
pub(crate) trait AddressSpace {
    fn covers(&self, address: u64, size: u64, access: Access) -> bool;
}

fn require_range(space: &impl AddressSpace, address: u64, size: u64, access: Access) -> Result {
    if size == 0 || address.checked_add(size).is_none() || !space.covers(address, size, access) {
        return Err(EINVAL);
    }
    Ok(())
}

/// A validated four-GiB USC window owned by one queue.
pub(crate) struct UscWindow {
    base: u64,
}
impl UscWindow {
    pub(crate) fn new(base: u64, range: Range<u64>) -> Result<Self> {
        if base & (USC_WINDOW_SIZE - 1) != 0
            || base < range.start
            || base
                .checked_add(USC_WINDOW_SIZE)
                .is_none_or(|end| end > range.end)
        {
            return Err(EINVAL);
        }
        Ok(Self { base })
    }

    fn program(&self, space: &impl AddressSpace, tagged_offset: u32) -> Result {
        if tagged_offset == 0 {
            return Err(EINVAL);
        }
        require_range(
            space,
            self.base + u64::from(tagged_offset & !7),
            4,
            Access::Read,
        )
    }
}

fn sampler(space: &impl AddressSpace, heap: u64, count: u32) -> Result {
    if count > MAX_SAMPLERS || (heap == 0) != (count == 0) || heap & (SAMPLER_SIZE - 1) != 0 {
        return Err(EINVAL);
    }
    if count != 0 {
        require_range(space, heap, u64::from(count) * SAMPLER_SIZE, Access::Read)?;
    }
    Ok(())
}

fn helper_is_empty(helper: &uapi::drm_asahi_helper_program) -> bool {
    helper.binary == 0 && helper.cfg == 0 && helper.data == 0
}

fn surface(
    space: &impl AddressSpace,
    zls: &uapi::drm_asahi_zls_buffer,
    layers: u16,
) -> Result<ZlsSurface> {
    if (zls.base == 0 && (zls.comp_base != 0 || zls.stride != 0 || zls.comp_stride != 0))
        || (zls.comp_base == 0 && zls.comp_stride != 0)
        || (layers > 1 && zls.base != 0 && zls.stride == 0)
        || (zls.stride != 0 && u64::from(zls.stride) & (PAGE_SIZE - 1) != 1)
        || u64::from(zls.comp_stride) & (PAGE_SIZE - 1) != 0
    {
        return Err(EINVAL);
    }
    if zls.base != 0 {
        let stride = if zls.stride == 0 {
            0
        } else {
            (u64::from(zls.stride) / PAGE_SIZE + 1) * PAGE_SIZE
        };
        require_range(
            space,
            zls.base,
            stride * u64::from(layers - 1) + 1,
            Access::ReadWrite,
        )?;
    }
    if zls.comp_base != 0 {
        let stride = (u64::from(zls.comp_stride) / PAGE_SIZE + 1) * ZLS_METADATA_STRIDE;
        require_range(
            space,
            zls.comp_base,
            stride * u64::from(layers - 1) + 1,
            Access::ReadWrite,
        )?;
    }
    Ok(ZlsSurface {
        va: zls.base,
        compression_va: zls.comp_base,
        stride: zls.stride.into(),
        compression_stride: zls.comp_stride.into(),
    })
}

// As with Payload, retain the bounded attachment lists inside the submission plan.
#[allow(clippy::large_enum_variant)]
pub(crate) enum Validated {
    Render {
        long_job: bool,
        /// The plan of an M5 pass ([`m5::compute::Gpu::plan_render`]); `None` on the other
        /// submission paths.
        plan: Option<m5::compute::RenderPlan>,
        pass: RenderPass,
        ray_query: [RayQueryStage; 2],
        attachments: [Attachments; 2],
        timestamps: [uapi::drm_asahi_timestamps; 2],
    },
    Compute {
        long_job: bool,
        intersection: IntersectionTables,
        scratch: ScratchRequest,
        usc_base: u64,
        cdm_va: u64,
        cdm_end_va: u64,
        sampler_heap_va: u64,
        sampler_count: u32,
        timestamps: uapi::drm_asahi_timestamps,
        /// The planned resources of an M5 kick ([`m5::compute::Gpu::plan`]); `None` on the
        /// other submission paths.
        resources: Option<m5::compute::Resources>,
    },
}

impl Validated {
    pub(crate) fn long_job(&self) -> bool {
        match self {
            Self::Render { long_job, .. } | Self::Compute { long_job, .. } => *long_job,
        }
    }
    pub(crate) fn allow_long_job(&mut self) {
        match self {
            Self::Render { long_job, .. } | Self::Compute { long_job, .. } => *long_job = true,
        }
    }
}

impl Payload {
    pub(crate) fn prior(&self) -> [u64; 2] {
        match self {
            Self::Render(cmd, _, _, _) => [cmd.prior_vdm, cmd.prior_cdm],
            Self::Compute(cmd, _, _, _) => [cmd.prior_vdm, cmd.prior_cdm],
        }
    }

    pub(crate) fn validate(
        &self,
        window: &UscWindow,
        variant: &super::fw::Variant,
        m5: Option<&m5::compute::Gpu>,
        space: &impl AddressSpace,
    ) -> Result<Validated> {
        match self {
            Self::Render(cmd, attachments, ray_query, declared) => {
                // M5 render is new: name each refusal (the doorbell path keeps its silence).
                let bad = |what: &str| {
                    if m5.is_some() {
                        pr_warn!("G17 M5: render command refused: {}\n", what);
                    }
                    EINVAL
                };
                if cmd.flags & !RENDER_FLAGS != 0
                    || !(1..=MAX_DIMENSION).contains(&cmd.width_px)
                    || !(1..=MAX_DIMENSION).contains(&cmd.height_px)
                    || !(1..=MAX_LAYERS).contains(&cmd.layers)
                    || !matches!(
                        (cmd.utile_width_px, cmd.utile_height_px),
                        (16, 16) | (32, 16) | (32, 32)
                    )
                {
                    return Err(bad("flags, size or tile"));
                }
                // M5 Mesa offers 8x (S's validator admits it); the M5 subset decides below.
                let samples_log2 = match (cmd.samples, m5.is_some()) {
                    (1, _) => 0,
                    (2, _) => 1,
                    (4, _) => 2,
                    (8, true) => 3,
                    _ => return Err(bad("sample count")),
                };
                if u64::from(cmd.sample_size_B)
                    * u64::from(cmd.utile_width_px)
                    * u64::from(cmd.utile_height_px)
                    * u64::from(cmd.samples)
                    > MAX_TILE_BUFFER
                    || cmd.vdm_ctrl_stream_base == 0
                    || cmd.vdm_ctrl_stream_base & 3 != 0
                    || cmd.isp_scissor_base == 0
                    || cmd.isp_scissor_base & 7 != 0
                {
                    return Err(bad("tile buffer, VDM stream or scissor"));
                }
                require_range(space, cmd.vdm_ctrl_stream_base, 4, Access::Read).map_err(|_| bad("VDM stream mapping"))?;
                require_range(space, cmd.isp_scissor_base, 8, Access::Read).map_err(|_| bad("scissor mapping"))?;
                for (va, access) in [
                    (cmd.isp_dbias_base, Access::Read),
                    (cmd.isp_oclqry_base, Access::Write),
                ] {
                    if va != 0 {
                        if va & 7 != 0 {
                            return Err(bad("depth-bias or occlusion-query alignment"));
                        }
                        require_range(space, va, 8, access).map_err(|_| bad("depth-bias or occlusion-query mapping"))?;
                    }
                }
                let depth = surface(space, &cmd.depth, cmd.layers).map_err(|_| bad("depth surface"))?;
                let stencil = surface(space, &cmd.stencil, cmd.layers).map_err(|_| bad("stencil surface"))?;
                sampler(space, cmd.sampler_heap, cmd.sampler_count.into()).map_err(|_| bad("samplers"))?;
                if !helper_is_empty(&cmd.vertex_helper) || !helper_is_empty(&cmd.fragment_helper) {
                    return Err(bad("helper programs"));
                }
                for program in [&cmd.bg, &cmd.eot, &cmd.partial_bg, &cmd.partial_eot] {
                    window.program(space, program.usc).map_err(|_| bad("tile program"))?;
                }
                for list in attachments {
                    list.validate(space).map_err(|_| bad("attachments"))?;
                }
                let pass = RenderPass {
                    width: cmd.width_px.into(),
                    height: cmd.height_px.into(),
                    layers: cmd.layers,
                    utile_width: cmd.utile_width_px,
                    utile_height: cmd.utile_height_px,
                    samples_log2,
                    sample_size: cmd.sample_size_B,
                    process_empty_tiles: cmd.flags & PROCESS_EMPTY != 0,
                    msaa_control: cmd.ppp_multisamplectl,
                    ppp_control: cmd.ppp_ctrl.into(),
                    vdm_va: cmd.vdm_ctrl_stream_base,
                    scissor_va: cmd.isp_scissor_base,
                    depth_bias_va: cmd.isp_dbias_base,
                    depth_bias_is_int: cmd.flags & DBIAS_INT != 0,
                    few_primitives: cmd.flags & FEW_PRIMITIVES != 0 && variant.few_primitives,
                    fragment_barriers: cmd.flags & FRAGMENT_BARRIERS != 0,
                    occlusion_query_va: cmd.isp_oclqry_base,
                    sampler_heap_va: cmd.sampler_heap,
                    sampler_count: cmd.sampler_count.into(),
                    zls_control: cmd.zls_ctrl,
                    depth_dimensions: cmd.isp_zls_pixels.into(),
                    depth,
                    stencil,
                    depth_clear: cmd.isp_bgobjdepth,
                    stencil_clear: cmd.isp_bgobjvals,
                    merge_upper_x: cmd.isp_merge_upper_x,
                    merge_upper_y: cmd.isp_merge_upper_y,
                    usc_base: window.base,
                    bg: UscProgram {
                        offset: cmd.bg.usc,
                        rsrc_spec: u64::from(cmd.bg.rsrc_spec)
                            | (u64::from(cmd.bg_rsrc_spec_hi) << 32),
                    },
                    partial_bg: UscProgram {
                        offset: cmd.partial_bg.usc,
                        rsrc_spec: u64::from(cmd.partial_bg.rsrc_spec)
                            | (u64::from(cmd.bg_partial_rsrc_spec_hi) << 32),
                    },
                    eot_offset: cmd.eot.usc,
                    eot_rsrc_spec: u64::from(cmd.eot.rsrc_spec)
                        | (u64::from(cmd.bg_eot_rsrc_spec_hi) << 32),
                    partial_eot_offset: cmd.partial_eot.usc,
                    partial_eot_rsrc_spec: u64::from(cmd.partial_eot.rsrc_spec)
                        | (u64::from(cmd.bg_eot_partial_rsrc_spec_hi) << 32),
                    rsrc_spec_hi: cmd.flags & RSRC_SPEC_HI != 0,
                };
                for stage in ray_query {
                    if (stage.intersection.enabled() || stage.scratch.enabled())
                        && !variant.ray_traversal
                    {
                        return Err(refuse_ray_traversal());
                    }
                    if stage.intersection.enabled() {
                        require_range(space, stage.intersection.pool, stage.intersection.pool_bytes, Access::Read)
                            .map_err(|_| bad("intersection pool"))?;
                        let code = stage.intersection.default_program & 0x0000_ffff_ffff_ffff;
                        if code != 0 { require_range(space, code, 64, Access::Read).map_err(|_| bad("intersection program"))?; }
                    }
                }
                let plan = match m5 {
                    Some(gpu) => Some(m5_render(&pass, ray_query, gpu, declared)?),
                    None if declared.is_some() => return Err(bad("render declarations")),
                    None => None,
                };
                Ok(Validated::Render {
                    long_job: super::timeout::select(false,
                        ray_query.map(|stage| stage.intersection.enabled()), false),
                    plan,
                    pass,
                    ray_query: *ray_query,
                    attachments: *attachments,
                    timestamps: [cmd.ts_vtx, cmd.ts_frag],
                })
            }
            Self::Compute(cmd, attachments, intersection, declared) => {
                let as_build = super::timeout::as_build(cmd.flags).ok_or(EINVAL)?;
                if cmd.cdm_ctrl_stream_base & 3 != 0
                    || cmd.cdm_ctrl_stream_end & 3 != 0
                    || cmd.cdm_ctrl_stream_end <= cmd.cdm_ctrl_stream_base
                    || !helper_is_empty(&cmd.helper)
                {
                    return Err(EINVAL);
                }
                require_range(
                    space,
                    cmd.cdm_ctrl_stream_base,
                    cmd.cdm_ctrl_stream_end - cmd.cdm_ctrl_stream_base,
                    Access::Read,
                )?;
                sampler(space, cmd.sampler_heap, cmd.sampler_count)?;
                attachments.validate(space)?;
                let scratch = ScratchRequest::new(cmd.scs_layout)?;
                if (scratch.enabled() || intersection.enabled()) && !variant.ray_traversal {
                    return Err(refuse_ray_traversal());
                }
                if intersection.enabled() {
                    require_range(space, intersection.pool, intersection.pool_bytes, Access::Read)?;
                    let code = intersection.default_program & 0x0000_ffff_ffff_ffff;
                    if code != 0 { require_range(space, code, 64, Access::Read)?; }
                }
                let resources = match m5 {
                    Some(gpu) => Some(m5_compute(cmd, attachments, intersection, scratch, gpu, *declared)?),
                    None if declared.is_some() => return Err(EINVAL),
                    None => None,
                };
                Ok(Validated::Compute {
                    long_job: super::timeout::select(intersection.enabled(), [false; 2], as_build),
                    intersection: *intersection,
                    scratch,
                    usc_base: window.base,
                    cdm_va: cmd.cdm_ctrl_stream_base,
                    cdm_end_va: cmd.cdm_ctrl_stream_end - 4,
                    sampler_heap_va: cmd.sampler_heap,
                    sampler_count: cmd.sampler_count,
                    timestamps: cmd.ts,
                    resources,
                })
            }
        }
    }
}

/// The M5 subset of a compute command, after the common checks: what the M5 descriptor encodes
/// (an eight-byte aligned CDM stream, no samplers, scratch, intersection tables, flags, attachment
/// hints or timestamps), and the resources its declarations plan. The user's stream and shaders
/// stay user programs: only the first span is known mapped.
fn m5_compute(
    cmd: &uapi::drm_asahi_cmd_compute,
    attachments: &Attachments,
    intersection: &IntersectionTables,
    scratch: ScratchRequest,
    gpu: &m5::compute::Gpu,
    declared: Option<m5::compute::Declared>,
) -> Result<m5::compute::Resources> {
    if cmd.cdm_ctrl_stream_base & 7 != 0 {
        return Err(EINVAL);
    }
    if cmd.flags != 0
        || cmd.sampler_count != 0
        || cmd.sampler_heap != 0
        || scratch.enabled()
        || intersection.enabled()
        || attachments.count != 0
        || cmd.ts.start.handle != 0
        || cmd.ts.end.handle != 0
    {
        kernel::pr_warn_once!("G17 M5: compute feature outside the M5 subset refused\n");
        return Err(EOPNOTSUPP);
    }
    gpu.plan(declared.ok_or(EINVAL)?)
}

/// ZLS control bits (Mesa's genxml "ZLS Control"; S `m5_render_submission.rs`). Each aspect has a
/// load and a store, each with a tiling and a compression bit; depth also has a format.
mod zls {
    pub(super) const Z_LOAD: u64 = 1 << 15;
    pub(super) const S_LOAD: u64 = 1 << 14;
    pub(super) const Z_STORE: u64 = 1 << 19;
    pub(super) const S_STORE: u64 = 1 << 18;
    pub(super) const Z_TILING: u64 = 1 << 1 | 1 << 5;
    pub(super) const S_TILING: u64 = 1 << 3 | 1 << 7;
    pub(super) const Z_COMPRESS: u64 = 1 << 2 | 1 << 6;
    pub(super) const S_COMPRESS: u64 = 1 << 4 | 1 << 8;
    /// Depth format, bits 25:26: 0 is D32F, 2 is D16 (bit 26).
    pub(super) const Z_FORMAT: u64 = 3 << 25;
    pub(super) const Z_FORMAT_16: u64 = 1 << 26;
    pub(super) const DEPTH: u64 = Z_LOAD | Z_STORE | Z_TILING | Z_COMPRESS | Z_FORMAT;
    pub(super) const STENCIL: u64 = S_LOAD | S_STORE | S_TILING | S_COMPRESS;
}

/// The M5 depth/stencil forms of an admitted pass: what Honeykrisp's M5 render setup emits
/// (`agx_pack_zls_control` over D16, D32F and the two independent planes of D32F_S8 / D16_S8,
/// and S8), with or without the M5 lossless depth/stencil compression
/// (`hk_m5_depth_compression_eligible`). Every set bit is a known one of an attached aspect; a
/// compressed aspect has its metadata and compresses its load and its store alike, and a plain
/// one has none, because the partial phases load and store every attached aspect with the main
/// pass's control ([`super::fw::render`] `partial_zls_control`); one tiling for both
/// directions, for the same reason. The surfaces' mappings were checked by the common parser.
/// No ZLS resolve (bits 56 and 58): Honeykrisp does not emit it.
fn m5_zls(pass: &RenderPass) -> bool {
    let control = pass.zls_control;
    let aspect = |attached: &ZlsSurface, bits: u64, tiling: u64, compress: u64| {
        let used = control & bits;
        (attached.va != 0 || used == 0)
            && (used & tiling == 0 || used & tiling == tiling)
            && match used & compress {
                0 => attached.compression_va == 0,
                both if both == compress => attached.compression_va != 0,
                _ => false,
            }
    };
    control & !(zls::DEPTH | zls::STENCIL) == 0
        && control & zls::Z_FORMAT & !zls::Z_FORMAT_16 == 0
        && aspect(&pass.depth, zls::DEPTH, zls::Z_TILING, zls::Z_COMPRESS)
        && aspect(&pass.stencil, zls::STENCIL, zls::S_TILING, zls::S_COMPRESS)
        // A multisample pass keeps the plain layout: Honeykrisp compresses multisample depth only
        // for a kernel that advertises M5_DEPTH_MSAA_COMPRESSION, which this one does not.
        && (pass.samples_log2 == 0
            || (pass.depth.compression_va == 0 && pass.stencil.compression_va == 0))
}

/// The M5 subset of a render pass, after the common checks, and the plan of its declarations.
/// The admitted forms (widened by qualification): one layer; one sample, or 2, 4
/// or 8 (S's multisample form: at least 8 bytes per sample and at most 2048 samples per tile, so
/// 8x needs 16x16 tiles), with the tile's storage within the render plan's bound
/// ([`m5::compute::MAX_IMAGEBLOCK_BYTES`]); stored color, resolved or not (the resolve is the
/// end-of-tile program's); the depth/stencil forms of [`m5_zls`] with their load, clear and
/// store operations (and the firmware's partial store and reload of each attached aspect); few
/// or many primitives; no ray queries, scratch or helper programs. The fragment attachment list is
/// validated and not yet encoded: the M5 kick carries no cache-maintenance table. Other forms
/// are admitted as they qualify.
fn m5_render(
    pass: &RenderPass,
    ray_query: &[RayQueryStage; 2],
    gpu: &m5::compute::Gpu,
    declared: &Option<RenderDeclared>,
) -> Result<m5::compute::RenderPlan> {
    let tile = u32::from(pass.utile_width) * u32::from(pass.utile_height);
    let samples = 1u32 << pass.samples_log2;
    let storage = pass.layers == 1
        && pass.sample_size % 8 == 0
        && (pass.samples_log2 == 0 || (pass.sample_size >= 8 && tile * samples <= 2048))
        && u32::from(pass.sample_size) * tile * samples <= m5::compute::MAX_IMAGEBLOCK_BYTES;
    if !storage
        || !m5_zls(pass)
        || ray_query.iter().any(|stage| stage.intersection.enabled() || stage.scratch.enabled())
    {
        kernel::pr_warn_once!("G17 M5: render form outside the M5 subset refused\n");
        let attached = |surface: &ZlsSurface| match (surface.va != 0, surface.compression_va != 0) {
            (false, _) => "none",
            (true, false) => "plain",
            (true, true) => "compressed",
        };
        let (depth, stencil) = (attached(&pass.depth), attached(&pass.stencil));
        pr_warn!(
            "G17 M5: render form refused: {} layers, {} samples, {} B/sample, ZLS {:#x}, depth {}, stencil {}\n",
            pass.layers, samples, pass.sample_size, pass.zls_control, depth, stencil
        );
        return Err(EOPNOTSUPP);
    }
    let declared = declared.as_ref().ok_or_else(|| {
        pr_warn!("G17 M5: render command refused: no SET_RENDER_RESOURCES\n");
        EINVAL
    })?;
    gpu.plan_render(declared.programs()).inspect_err(|error| {
        pr_warn!("G17 M5: render command refused: plan of {} programs: {:?}\n", declared.programs().len(), error)
    })
}

/// Ray-traversal work on a firmware build that does not run it ([`super::fw::Variant`]).
fn refuse_ray_traversal() -> Error {
    kernel::pr_warn_once!("G17: ray-traversal work refused on this firmware\n");
    Error::from_errno(-(kernel::bindings::EOPNOTSUPP as i32))
}
