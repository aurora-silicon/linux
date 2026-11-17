// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 application path: one decoded Mesa render command -> G18 job inputs.
//!
//! This is the DRM counterpart of `g18_render::g18_capture_parameters`.
//! Where the capture helper inverts Apple's native gears/colour descriptors,
//! this module starts from the UAPI `drm_asahi_cmd_render` that Mesa's G18G
//! (M5-path) Gallium driver submits and produces:
//!
//! - `G17pRenderParameters` with every user field translated by the shared
//!   `g17_uapi::translate_render_command` (the M5-proven parser and range
//!   checks) and the G18 adaptations below;
//! - `G18RenderControls` from `g18_render::g18_default_controls`;
//! - every job-owned field left at zero (see [`job_owned_fields_clear`]) for
//!   the job (`G18RenderJobSink`) to allocate and fill.
//!
//! G18 adaptations, each against a named source:
//! - VDM stream: the UAPI carries the full GART-alias VA; `g18_render` takes
//!   the native compact view (the render-register notes, "Kernel-owned inputs"),
//!   exactly Mesa's `agx_m5_vdm_compact` (`agx_m5_va.h:34-41`).
//! - aux_fb_flags 0x30c000: indexed multi-draw, the gears-g1 value
//!   (`g18_render.rs:490-494`), where M5 used 0x10c000 (`m5_drm.rs:251`).
//! - Main-pass tile flags 0xc00000 (native +8d2/+8d3): present in all three
//!   G18 captures (`g18_render.rs:489`, tile_config 0xc10280), dropped by the
//!   partial helper (`g18_render.rs:396`). The UAPI cannot express them.
//! - Single-layer ZLS array strides are zeroed, as on M5 (`m5_drm.rs:238-241`).
//! - Uncompressed Z16 uses the shared range-checked ZLS format and preserves
//!   ISP bit 18. AGXMetalG18G selects that bit for Depth16Unorm at
//!   0x2196383e8..0x219638400 (the indirect DBIAS/PBE notes, section 2).
//! - msaa163, only with `g18_admit_msaa4=1`: the native 4x render set
//!   (`is_msaa4_command`) is admitted with the native sample control
//!   0x40000; every other multisampled command keeps its refusal.
//! - Native682 layered Z32/S8 uses cacheline or page array strides and
//!   independently ranged metadata. Single-layer compressed stencil retains
//!   its previous inert behavior.
//!   `isp_bgobjvals` 0x300 is re-asserted by the register builder; its low
//!   byte carries the application's stencil clear value.
//!
//! Pure: no allocation, MMIO, firmware or VM access. The job boundary at the
//! end of the file is the only kernel-typed item (a host parity
//! check compiles it against a stub `kernel` crate).

use crate::{g18::fw::render_params::G17pRenderParameters, g18::uapi::command as g17_uapi, g18::fw::render as g18_render, g18::fw::vm_layout as m5_vm};

/// Largest framebuffer side the target layouts and merge coefficients encode.
/// Whether a target fits is the job's own target plan
/// (`g18_render_assembly::target_plan`), which admission also requires: its
/// TPC slot is 0x20000 bytes, which the conformance FBO surface (64 x
/// GL_MAX_RENDERBUFFER_SIZE = 64x16384, 4x128 macrotiles) fills exactly.
pub(crate) const G18_MAX_RENDER_DIMENSION: u32 = 16384;
/// Indexed multi-draw auxiliary framebuffer flags (gears-g1 capture).
pub(crate) const G18_AUX_FB_FLAGS: u64 = 0x30c000;
/// Native main-pass tile flags (+8d2/+8d3), witnessed in colour-a/b and gears-g1.
pub(crate) const G18_MAIN_TILE_FLAGS: u64 = 0xc0_0000;
/// One-sample PPP multisample control; the only value `g18_render` admits.
pub(crate) const G18_MULTISAMPLE_CONTROL: u64 = 0x88;

// drm_asahi_render_flags (include/uapi/drm/asahi_drm.h:859-895).
const RENDER_PROCESS_EMPTY_TILES: u32 = 1 << 1;
const RENDER_RSRC_SPEC_HI: u32 = 1 << 4;
const RENDER_DBIAS_IS_INT: u32 = 1 << 18;
const G18_RENDER_FLAGS: u32 =
    RENDER_PROCESS_EMPTY_TILES | RENDER_RSRC_SPEC_HI | RENDER_DBIAS_IS_INT;
// ZLS control: stencil load (14) / store (18), stencil compression (4, 8),
// depth load (15) / store (19), depth format (25-26: 0 = Z32F, 2 = Z16).
const ZLS_STENCIL_TRANSFER: u64 = (1 << 14) | (1 << 18);
const ZLS_STENCIL_COMPRESSION: u64 = (1 << 4) | (1 << 8);
const ZLS_DEPTH_TRANSFER: u64 = (1 << 15) | (1 << 19);
const ZLS_DEPTH_FORMAT: u64 = 3 << 25;
/// Upper ISP_BGOBJVALS bits every Mesa backend seeds (`agx_pipe.c:1333`).
const ISP_BGOBJVALS_BASE: u32 = 0x300;
/// Width of the compact VDM view (`agx_m5_va.h:37`, `g17_render.rs:88`).
const VDM_COMPACT_WINDOW: u64 = 1 << 39;

/// Why a render is outside what the G18 job can do yet. `Invalid` is a
/// malformed or unmapped command (EINVAL); every other variant is a valid
/// command outside the admitted envelope (ENOTSUPP).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G18ApplicationRefusal {
    Invalid(g17_uapi::UapiTranslateError),
    Flags(u32),
    /// Vertex or fragment scratch helper program (spilling shaders).
    HelperProgram,
    Dimensions { width: u16, height: u16 },
    Layers(u16),
    Samples(u8),
    Utile(u8, u8),
    MultisampleControl(u64),
    Samplers { heap: u64, count: u16 },
    StencilTransfer(u64),
    StencilClear(u32),
    Depth(g17_uapi::UapiTranslateError),
    DepthFormat(u64),
    VdmCompact(u64),
}

impl G18ApplicationRefusal {
    /// True when the command itself is malformed rather than out of envelope.
    pub(crate) fn is_invalid(&self) -> bool {
        matches!(self, Self::Invalid(_))
    }
}

/// Render-target geometry the job must plan and allocate for this command.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18TargetRequest {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) layers: u16,
    pub(crate) utile: (u8, u8),
    pub(crate) samples: u8,
}

/// Everything the job receives for one admitted render.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18ApplicationRender {
    /// User fields translated; every job-owned field is zero.
    pub(crate) parameters: G17pRenderParameters,
    /// `g18_default_controls(&parameters)`; job-owned control fields are zero.
    pub(crate) controls: g18_render::G18RenderControls,
    pub(crate) target: G18TargetRequest,
    /// VDM stream as submitted (full VA in the submitting VM). The compact
    /// form is `parameters.encoder`; the job needs both views (risk #1).
    pub(crate) vdm_base: u64,
    /// Attachment ranges Mesa declared (render targets and PBE images).
    pub(crate) vertex_attachments: g17_uapi::UapiAttachmentList,
    pub(crate) fragment_attachments: g17_uapi::UapiAttachmentList,
    /// An inert compressed stencil plane was removed (see module doc).
    pub(crate) stencil_stripped: bool,
    /// UAPI render flags: only PROCESS_EMPTY_TILES and RSRC_SPEC_HI remain.
    pub(crate) flags: u32,
    /// TA/3D private-memory selection derived from the render's
    /// `drm_asahi_g18_render_resources` suffix by the adapter; `None` keeps
    /// the fixed first-use policy (`g18_render_policy::resource_pairs`).
    pub(crate) scratch: Option<crate::g18::uapi::graphics_scratch::G18RenderScratchSelection>,
}

/// User pointers the G18 3D command carries as native descriptor copies
/// (render-cmd `G183dCommandFields`), because the G18 fragment program drops
/// their registers 0x15101/0x15109 (`m5_render.rs:306`). The slots are the
/// ones G17P fills from the same parameters (`g17_render.rs:1381-1383`); the
/// gears-g1 native command carries its depth-bias and scissor arrays there.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18DescriptorPointers {
    /// d04e0 -> 3D +0x1d20: ISP depth-bias array.
    pub(crate) d04e0: u64,
    /// d04e8 -> 3D +0x1d30: ISP scissor array.
    pub(crate) d04e8: u64,
    /// d04f0 -> 3D +0x1d40 / register 0x15311: aligned occlusion query heap.
    pub(crate) d04f0: u64,
}

impl G18ApplicationRender {
    /// The job must copy these into its 3D command fields; the register
    /// programs built from `parameters` do not contain them.
    pub(crate) fn descriptor_pointers(&self) -> G18DescriptorPointers {
        G18DescriptorPointers {
            d04e0: self.parameters.depth_bias_array,
            d04e8: self.parameters.scissor_array,
            d04f0: self.parameters.occlusion_query_base,
        }
    }
}

/// Whether every job-owned field is still zero. The translation leaves them
/// zero; the job must assign each before building registers:
/// - parameters: context_base, tilemap, layermeta, heapmeta, tpc,
///   deflake_1/2/3, ta_status, fragment_status, aux_fb, cycle, record_index,
///   lifecycle, fragment_lifecycle, ta_hardware_buffer_id,
///   usc_flist_hardware_buffer_id, parameter_buffer and the four timestamps
///   (which `g18_render` requires to stay zero);
/// - controls: resource_word, work_key and usc_statistics of both stages,
///   and the optional TA cluster fields (zero means absent).
///
/// Sources: the render-register notes, "Kernel-owned inputs", the H1 job
/// (`g18_graphics_job.rs:61-74`, `g18_render_assembly.rs:34-47`) and its
/// policy (`g18_render_policy.rs:25-28`).
pub(crate) fn job_owned_fields_clear(
    p: &G17pRenderParameters,
    c: &g18_render::G18RenderControls,
) -> bool {
    let parameters = [p.context_base, p.tilemap, p.layermeta, p.heapmeta, p.tpc,
        p.deflake_1, p.deflake_2, p.deflake_3, p.ta_status, p.fragment_status,
        p.aux_fb, p.cycle, p.record_index, p.lifecycle, p.fragment_lifecycle,
        p.parameter_buffer, p.ta_timestamp_start, p.ta_timestamp_end,
        p.fragment_timestamp_start, p.fragment_timestamp_end];
    let controls = [c.ta.resource_word, c.fragment.resource_word, c.ta.work_key,
        c.fragment.work_key, c.ta.usc_statistics, c.fragment.usc_statistics,
        c.ta_cluster_tilemap, c.ta_cluster_layermeta];
    parameters.iter().chain(controls.iter()).all(|&v| v == 0)
        && p.ta_hardware_buffer_id == 0 && p.usc_flist_hardware_buffer_id == 0
        && c.ta_cluster_meta == [0; 4] && c.ta_cluster_control == [0; 3]
}

/// Internal state handed to `translate_render_command`: the M5 reference
/// profile's role (`m5_drm.rs:234`) with every job-owned field zeroed and
/// G18 policy applied. `translate_render_command` overwrites all user fields.
pub(crate) fn g18_application_base() -> G17pRenderParameters {
    let mut p = G17pRenderParameters::source_rsrc8();
    p.context_base = 0;
    p.tilemap = 0;
    p.layermeta = 0;
    p.heapmeta = 0;
    p.tpc = 0;
    p.deflake_1 = 0;
    p.deflake_2 = 0;
    p.deflake_3 = 0;
    p.ta_status = 0;
    p.fragment_status = 0;
    p.aux_fb = 0;
    p.cycle = 0;
    p.record_index = 0;
    p.lifecycle = 0;
    p.fragment_lifecycle = 0;
    p.ta_hardware_buffer_id = 0;
    p.usc_flist_hardware_buffer_id = 0;
    p.parameter_buffer = 0;
    p.gpc_perf_state_map = 0;
    p.gpc_perf_state_map_low = 0;
    p.gpc_perf_state_control = 0;
    p.ta_timestamp_start = 0;
    p.ta_timestamp_end = 0;
    p.fragment_timestamp_start = 0;
    p.fragment_timestamp_end = 0;
    // G18 policy. aux_fb_page_count keeps the value every M5/G18 capture
    // carries (0x100000, `g17_render.rs:421`).
    p.aux_fb_flags = G18_AUX_FB_FLAGS;
    p.samples = 1;
    p.absolute_pointers = false;
    p.bg_resource_prefix = 0;
    p.native_pm_bytes = false;
    p.native_ta_registers = 0;
    p
}

/// Mesa's compact VDM view (`agx_m5_vdm_compact`).
pub(crate) fn g18_vdm_compact(vdm_base: u64) -> Option<u64> {
    let offset = vdm_base.checked_sub(m5_vm::DRIVER_BASE)?;
    if vdm_base & 3 != 0 || offset >= VDM_COMPACT_WINDOW {
        return None;
    }
    Some(offset)
}

/// msaa163: whether `command` is exactly the native 4x render set (see
/// `g18_render::G18_MSAA4_UTILE`): four samples, 32x16 utiles, sample
/// positions 0xeaa26e26. Native331 adds sixteen-byte samples
/// to the eight-byte profile, retaining its geometry. Native315 adds
/// D32/S8 attachments to that render geometry. Gallium currently allocates
/// them uncompressed; compressed multisample metadata remains unadmitted.
fn is_multisample_command(command: &g17_uapi::UapiRenderCommand) -> bool {
    let (utile, positions) = match command.samples {
        2 => (g18_render::G18_MSAA2_UTILE, g18_render::G18_MSAA2_MULTISAMPLE_CONTROL),
        4 => (g18_render::G18_MSAA4_UTILE, g18_render::G18_MSAA4_MULTISAMPLE_CONTROL),
        _ => return false,
    };
    (command.utile_width, command.utile_height) == utile
        && command.multisample_control == positions
        && matches!(command.sample_size,
            g18_render::G18_MSAA4_SAMPLE_SIZE | g18_render::G18_MSAA_WIDE_SAMPLE_SIZE
                | g18_render::G18_MSAA_MRT_SAMPLE_SIZE)
        && (command.layers == 1 ||
            ((2..=2048).contains(&command.layers)
                && matches!(command.sample_size,
                    g18_render::G18_MSAA4_SAMPLE_SIZE | g18_render::G18_MSAA_WIDE_SAMPLE_SIZE)
                && command.depth.base == 0 && command.stencil.base == 0))
        && command.depth.compression_base == 0
        && command.stencil.compression_base == 0
}

/// Complete single-level D16/D32/S8 storage for the admitted one/four-sample
/// render. AIL's tile contains 16 KiB: multiplying bytes per pixel by samples
/// shrinks its logical-pixel dimensions, rather than multiplying the rounded
/// one-sample allocation (which is wrong across a tile boundary).
fn uncompressed_plane_bytes(width: u64, height: u64, bytes_per_sample: u64,
                            samples: u8) -> u64 {
    let block_bytes = bytes_per_sample * u64::from(samples);
    let (tile_width, tile_height) = match block_bytes {
        1 => (128, 128),
        2 => (128, 64),
        4 => (64, 64),
        8 => (64, 32),
        16 => (32, 32),
        _ => return 0, // Only admitted D16/D32/S8 at 1x/4x reaches this helper.
    };
    if width >= tile_width && height >= tile_height {
        width.div_ceil(tile_width) * height.div_ceil(tile_height) * 0x4000
    } else {
        (width.next_power_of_two() * height.next_power_of_two() * block_bytes)
            .div_ceil(128) * 128
    }
}

/// Native682: bit0 selects 16-KiB units; zero selects 128-byte units.
/// Zero therefore represents a real 128-byte layer, not an absent stride.
/// Validate the complete final layer, not just its first mapped byte.
fn validate_layered_zls_plane<A: g17_uapi::GpuAddressSpace>(
    plane: g17_uapi::UapiZlsBuffer, width: u64, height: u64,
    bytes_per_sample: u64, layers: u16, address_space: &A,
) -> Result<(), g17_uapi::UapiTranslateError> {
    use g17_uapi::{GpuAccess, UapiTranslateError as E};
    if plane.base == 0 {
        return if plane.compression_base == 0 && plane.stride == 0
            && plane.compression_stride == 0 { Ok(()) } else { Err(E::ZlsFields) };
    }
    if !matches!(plane.stride & 0x3fff, 0 | 1) || plane.base & 0x7f != 0 {
        return Err(E::ZlsFields);
    }
    let unit = if plane.stride & 1 != 0 { 0x4000 } else { 0x80 };
    let stride = ((u64::from(plane.stride) >> 14) + 1) * unit;
    let data_bytes = uncompressed_plane_bytes(width, height, bytes_per_sample, 1);
    if data_bytes == 0 || stride < data_bytes {
        return Err(E::ZlsFields);
    }
    let extent = stride.checked_mul(u64::from(layers - 1))
        .and_then(|n| n.checked_add(data_bytes)).ok_or(E::AddressRange)?;
    if plane.base.checked_add(extent).is_none()
        || !address_space.covers(plane.base, extent, GpuAccess::ReadWrite) {
        return Err(E::AddressRange);
    }
    if plane.compression_base != 0 {
        if plane.compression_base & 0x7f != 0 || plane.compression_stride & 0x3fff != 0 {
            return Err(E::ZlsFields);
        }
        let stride = ((u64::from(plane.compression_stride) >> 14) + 1) * 0x80;
        let metadata_bytes = (width.next_power_of_two().div_ceil(16)
            * height.next_power_of_two().div_ceil(16) * 8).div_ceil(128) * 128;
        if stride < metadata_bytes {
            return Err(E::ZlsFields);
        }
        let extent = stride.checked_mul(u64::from(layers - 1))
            .and_then(|n| n.checked_add(metadata_bytes)).ok_or(E::AddressRange)?;
        if plane.compression_base.checked_add(extent).is_none()
            || !address_space.covers(plane.compression_base, extent, GpuAccess::ReadWrite) {
            return Err(E::AddressRange);
        }
    } else if plane.compression_stride != 0 {
        return Err(E::ZlsFields);
    }
    Ok(())
}

/// The envelope checks that need only the raw command. Order matters for
/// diagnostics only; every check is independent. `msaa4` admits exactly the
/// native 4x set (module parameter `g18_admit_msaa4`); refusal classes are
/// unchanged for everything else.
fn admit_envelope(command: &g17_uapi::UapiRenderCommand, msaa4: bool)
    -> Result<(), G18ApplicationRefusal> {
    use G18ApplicationRefusal as R;
    if command.flags & !G18_RENDER_FLAGS != 0 {
        return Err(R::Flags(command.flags));
    }
    let no_helper = g17_uapi::UapiHelperProgram { binary: 0, config: 0, data: 0 };
    if command.vertex_helper != no_helper || command.fragment_helper != no_helper {
        return Err(R::HelperProgram);
    }
    if !(1..=G18_MAX_RENDER_DIMENSION).contains(&u32::from(command.width))
        || !(1..=G18_MAX_RENDER_DIMENSION).contains(&u32::from(command.height))
    {
        return Err(R::Dimensions { width: command.width, height: command.height });
    }
    // Native598 proves layered colour target allocation and all five
    // register lists. Native618/639 add layered four-sample colour through
    // sixteen bytes per sample. Native682 adds one-sample layered Z32/S8;
    // each plane and its metadata receive complete array range checks below.
    // Native1060 also verifies uncompressed D16 arrays, including48 layers.
    // Shared format validation below continues to reject compressed D16.
    if !(1..=2048).contains(&command.layers)
        || (command.layers > 1 && ((command.samples != 1
            && !(msaa4 && is_multisample_command(command)))
            || ((command.depth.base != 0 || command.stencil.base != 0)
                && (command.samples != 1
                    || !matches!(command.zls_control & ZLS_DEPTH_FORMAT, 0 | 0x0400_0000))))) {
        return Err(R::Layers(command.layers));
    }
    if !(msaa4 && is_multisample_command(command)) {
        if command.samples != 1 {
            return Err(R::Samples(command.samples));
        }
        if (command.utile_width, command.utile_height) != (32, 32) {
            return Err(R::Utile(command.utile_width, command.utile_height));
        }
        if command.multisample_control != G18_MULTISAMPLE_CONTROL {
            return Err(R::MultisampleControl(command.multisample_control));
        }
    }
    if command.sampler_heap != 0 || command.sampler_count != 0 {
        return Err(R::Samplers { heap: command.sampler_heap, count: command.sampler_count });
    }
    if command.occlusion_query_base & 0xf != 0 {
        return Err(R::Invalid(g17_uapi::UapiTranslateError::AddressAlignment));
    }
    if command.zls_control & ZLS_STENCIL_TRANSFER != 0
        && (command.stencil.base == 0
            || (command.stencil.compression_base != 0 && command.layers == 1)) {
        return Err(R::StencilTransfer(command.zls_control));
    }
    if command.stencil_clear & !(ISP_BGOBJVALS_BASE | 0xff) != 0 {
        return Err(R::StencilClear(command.stencil_clear));
    }
    Ok(())
}

/// Translate one render command for the G18 job. `address_space` must answer
/// for the submitting VM; `window` is the queue's validated USC window.
/// Multisampled commands are refused (`g18_admit_msaa4=0` behaviour). The
/// adapter calls [`translate_g18_render_admitting`]; this form is kept for
/// the host parity tools (a host parity check).
#[allow(dead_code)]
#[inline(never)]
pub(crate) fn translate_g18_render<A: g17_uapi::GpuAddressSpace>(
    command: &g17_uapi::UapiRenderCommand,
    vertex_attachments: g17_uapi::UapiAttachmentList,
    fragment_attachments: g17_uapi::UapiAttachmentList,
    window: g17_uapi::QueueUscWindow,
    address_space: &A,
) -> Result<G18ApplicationRender, G18ApplicationRefusal> {
    translate_g18_render_admitting(command, vertex_attachments, fragment_attachments,
        window, address_space, false)
}

/// [`translate_g18_render`], additionally admitting the native 4x render set
/// when `msaa4` is true (module parameter `g18_admit_msaa4=1`, msaa163).
#[inline(never)]
pub(crate) fn translate_g18_render_admitting<A: g17_uapi::GpuAddressSpace>(
    command: &g17_uapi::UapiRenderCommand,
    vertex_attachments: g17_uapi::UapiAttachmentList,
    fragment_attachments: g17_uapi::UapiAttachmentList,
    window: g17_uapi::QueueUscWindow,
    address_space: &A,
    msaa4: bool,
) -> Result<G18ApplicationRender, G18ApplicationRefusal> {
    use G18ApplicationRefusal as R;
    admit_envelope(command, msaa4)?;
    // Native G18 masks the low four base bits. The PPP slot is 15 bits
    // and addresses an 8-byte result, matching Gallium's 32768-entry heap.
    // Validate the complete read/modify/write range, not only slot zero:
    // a mapped first result does not make later indices safe.
    if command.occlusion_query_base != 0 {
        const QUERY_HEAP_BYTES: u64 = 32768 * 8;
        if command.occlusion_query_base.checked_add(QUERY_HEAP_BYTES).is_none()
            || !address_space.covers(command.occlusion_query_base, QUERY_HEAP_BYTES,
                                    g17_uapi::GpuAccess::ReadWrite) {
            return Err(R::Invalid(g17_uapi::UapiTranslateError::AddressRange));
        }
    }
    // Validate Z32F and uncompressed Z16, including the complete format-sized
    // backing. The shared validator refuses compressed Z16 metadata.
    // Retain the existing depth validation while admitting stencil's own
    // load/store bits. Its full uncompressed S8 range is checked separately.
    // Native321 confirms D16's UNORM16 integer clear at 1x and 4x.
    let depth16 = command.zls_control & ZLS_DEPTH_FORMAT == 0x0400_0000;
    if command.depth.base != 0 && depth16 && command.depth_clear > 0xffff {
        return Err(R::Depth(g17_uapi::UapiTranslateError::ZlsFields));
    }
    let mut depth_command = *command;
    depth_command.zls_control &= !ZLS_STENCIL_TRANSFER;
    // Retain the shared single-plane format/control validation. The G18
    // checks below independently validate every layer's data and metadata.
    depth_command.layers = 1;
    g17_uapi::validate_m5_reference_depth(&depth_command, address_space).map_err(R::Depth)?;
    if command.layers > 1 {
        // D16 uses two bytes per sample; Z32 uses four. Applying the Z32
        // extent to D16 rejects its native128-byte8x8 layer stride.
        let depth_bytes = if depth16 { 2 } else { 4 };
        for (plane, bytes_per_sample) in [(command.depth, depth_bytes), (command.stencil, 1)] {
            validate_layered_zls_plane(plane, u64::from(command.width),
                u64::from(command.height), bytes_per_sample, command.layers,
                address_space).map_err(R::Depth)?;
        }
    }
    if command.samples > 1 && command.depth.base != 0 {
        let bytes = uncompressed_plane_bytes(u64::from(command.width),
            u64::from(command.height), if depth16 { 2 } else { 4 }, command.samples);
        if bytes == 0 || command.depth.base.checked_add(bytes).is_none()
            || !address_space.covers(command.depth.base, bytes, g17_uapi::GpuAccess::ReadWrite) {
            return Err(R::Invalid(g17_uapi::UapiTranslateError::AddressRange));
        }
    }
    if command.stencil.base != 0 && command.stencil.compression_base == 0 {
        let bytes = uncompressed_plane_bytes(u64::from(command.width),
            u64::from(command.height), 1, command.samples);
        if bytes == 0 || command.stencil.base & 0x7f != 0
            || command.stencil.base.checked_add(bytes).is_none()
            || !address_space.covers(command.stencil.base, bytes, g17_uapi::GpuAccess::ReadWrite) {
            return Err(R::Invalid(g17_uapi::UapiTranslateError::AddressRange));
        }
    }
    if command.depth.base != 0
        && !matches!(command.zls_control & ZLS_DEPTH_FORMAT, 0 | 0x0400_0000) {
        return Err(R::DepthFormat(command.zls_control));
    }
    let translated = g17_uapi::translate_g18_render_command(*command, vertex_attachments,
        fragment_attachments, window, address_space,
        g17_uapi::RenderInternalState { parameters: g18_application_base() })
        .map_err(R::Invalid)?;
    let mut p = translated.parameters;

    p.encoder = g18_vdm_compact(command.vdm_base).ok_or(R::VdmCompact(command.vdm_base))?;
    // Native single-layer ZLS does not encode an array stride. Layered
    // Native682 retains all four already-validated data/metadata strides.
    if command.layers == 1 {
        p.depth_stride = 0;
        p.depth_aux_stride = 0;
        p.stencil_stride = 0;
        p.stencil_aux_stride = 0;
    }
    let stencil_stripped = p.stencil_aux_buffer != 0 && command.layers == 1;
    if stencil_stripped {
        p.stencil_buffer = 0;
        p.stencil_aux_buffer = 0;
        p.stencil_stride = 0;
        p.stencil_aux_stride = 0;
        p.depth_flags &= !ZLS_STENCIL_COMPRESSION;
        // Same derivation as translate_render_command, now without stencil.
        p.partial_depth_flags = p.depth_flags
            | if p.depth_buffer != 0 { ZLS_DEPTH_TRANSFER } else { 0 };
        if p.depth_buffer == 0 {
            // Neither plane remains: the M5 "no ZLS" envelope.
            p.depth_dimensions = 0;
        }
    }
    // ISP_BGOBJVALS: retain the S8 clear value and normalize the fixed bits.
    p.stencil_clear_value = command.stencil_clear & 0xff;
    // Native G18 passes the integer-depth selector to ISP register 0x15021.
    // Do not erase it while replacing the shared translator's base flags.
    p.aux_fb_flags = G18_AUX_FB_FLAGS | u64::from(command.flags & RENDER_DBIAS_IS_INT);
    p.tile_config |= G18_MAIN_TILE_FLAGS;

    // The job plans every target inside its fixed target window; refuse here
    // what it could not plan rather than fail the job.
    if crate::g18::fw::render_assembly::target_plan(&p).is_none() {
        return Err(R::Dimensions { width: command.width, height: command.height });
    }

    let mut controls = g18_render::g18_default_controls(&p);
    // The older UAPI translator's tib_blocks is storage size, whereas G18
    // +770 / 10051 is sample control: native run112 has zero at 1x and
    // 0x40000 at 4x. Keep storage accounting in p.tib_blocks for the
    // independent +4d0 metadata. msaa163: the admitted 4x render (utile
    // 32x16, utile_config 0x6002 and positions from the UAPI; p.samples
    // stays 1, see g18_render::G18_MSAA4_UTILE) takes the native 0x40000.
    let multisample_render =
        msaa4 && is_multisample_command(command) && g18_render::g18_multisample(&p);
    // Native464's 48-byte samples occupy two 32-byte partitions; native331's
    // 16-byte samples and the original eight-byte profile occupy one.
    let partitions = if command.sample_size == g18_render::G18_MSAA_MRT_SAMPLE_SIZE {
        2
    } else {
        1
    };
    controls.fragment_sample_control =
        if multisample_render { (u64::from(command.samples) * partitions) << 16 } else { 0 };
    Ok(G18ApplicationRender {
        parameters: p,
        controls,
        target: G18TargetRequest {
            width: p.width,
            height: p.height,
            layers: p.layers,
            utile: (p.utile_width, p.utile_height),
            samples: if multisample_render { command.samples } else { p.samples },
        },
        vdm_base: command.vdm_base,
        vertex_attachments: translated.vertex_attachments,
        fragment_attachments: translated.fragment_attachments,
        stencil_stripped,
        flags: command.flags,
        scratch: None,
    })
}

/// Stage timestamps a retired job can report, in ticks of the architectural
/// counter (`CNTPCT_EL0`, the `DRM_IOCTL_ASAHI_GET_TIME` source):
/// `[vertex start, vertex end, fragment start, fragment end]`.
#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct G18RenderRetirement {
    pub(crate) stage_ticks: Option<[u64; 4]>,
}

/// The job boundary. The conductor implements this over the G18 render
/// runtime (`g18_graphics_job.rs` and its queues/PB/UMA owners).
///
/// Contract for `submit_render`:
/// - calls are serialized by the adapter (one at a time per device);
/// - the job fills every field [`job_owned_fields_clear`] lists, allocates targets
///   for `render.target`, maps them into `vm` (both compact views while risk
///   #1 is open), builds registers from `render.parameters/controls`, copies
///   `render.descriptor_pointers()` into its 3D command fields, encodes,
///   publishes and waits;
/// - `Ok` only after both TA and 3D retired with no fault or recovery;
/// - any error after publication must record it in `vm.status()`, retain
///   every owner and make `G18DrmRuntime::is_crashed` true (M5 contract,
///   `m5_live_boot.rs:639-656`).
#[cfg(not(test))]
pub(crate) trait G18RenderJobSink: Send + Sync {
    fn submit_render(&self, vm: &crate::mmu::Vm, render: &G18ApplicationRender)
        -> kernel::error::Result<G18RenderRetirement>;
}
