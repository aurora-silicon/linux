// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! Pure T8140/G17P render-command construction.
//!
//! The byte layout comes from m1n1
//! `916548424c00c72176326e3811badedce5714a99`. The source profile carries the
//! hardware-tested TA and fragment register recipes, the compiler-derived EOT
//! resource binding `0x8`, full-width load binding, and native-aligned BG/EOT
//! program pointers. Descriptor construction also writes the duplicated structural
//! pipeline fields. Queue construction returns ordered memory writes only;
//! allocation, cache maintenance, barriers, doorbells, MMIO, and completion
//! stay with the retained runtime.

const TA_REGISTER_COUNT: usize = 73;
const FRAGMENT_REGISTER_COUNT: usize = 89;

/// Both embedded register lists match the kext EXACTLY -- same writes, same
/// unique registers, same order -- verified against 2048-byte captures of the
/// native TA and 3D command records (macos-trace recs2048-gfx.txt):
///
///   TA  73 writes, 71 unique, list ends at +0x3cc
///   3D  89 writes, 86 unique, list ends at +0x4cc
///
/// A 1024-byte capture cuts the 3D list at exactly +0x400 and hides its last
/// 17 writes, which briefly looked like 14 registers "Metal never writes" --
/// they were entries 72..85. Assert the counts so a future truncated capture
/// cannot produce that conclusion again.
const _: () = assert!(TA_REGISTER_COUNT == 73);
const _: () = assert!(FRAGMENT_REGISTER_COUNT == 89);
const PARTIAL_STORE_REGISTER_COUNT: usize = 16;
const PARTIAL_RESUME_REGISTER_COUNT: usize = 23;
const PARTIAL_LOAD_REGISTER_COUNT: usize = 10;
const REGISTER_BYTES: usize = 12;

pub(crate) const TA_DESCRIPTOR_SIZE: usize = 0x9c0;
pub(crate) const FRAGMENT_DESCRIPTOR_SIZE: usize = 0x2240;
/// Native render-setup field copied through command descriptor +0xc00 to the
/// final 3D command tail at +0x2110. This is independent of SGX's GPC
/// performance-state map register.
const G17P_FRAGMENT_TAIL_SETUP_WORD: u32 = u32::MAX;
/// J700 shared-runtime flag snapshot copied by native 3D submit into type-1
/// command lanes +0x2124/+0x2128.
///
/// The kext resolves the accelerator's shared event-stamp/HWDSID internal
/// resource at +0x1a130, obtains its CPU mapping, reads byte bit 0 at mapping
/// +0x2ad/+0x2ae, and widens them before per-draw register generation. The
/// working Neo Metal snapshot contains 1/1.
const G17P_NEO_FRAGMENT_SHARED_RUNTIME_FLAG_SNAPSHOT: [u32; 2] = [1, 1];
pub(crate) const G17P_EOT_RESOURCE_SPEC: u64 = 0x8;
pub(crate) const G17P_LOAD_RESOURCE_SPEC: u64 = 0x0007_8000_0000_0040;
pub(crate) const G17P_SOURCE_BG_PIPELINE: u64 = 0x0000_0100_0199_0240;
pub(crate) const G17P_SOURCE_EOT_PIPELINE: u64 = 0x0000_0100_0199_0640;
/// Native `mcacheApertureBufferSetup` writes its packed write ranges here in
/// the 3D firmware command record. The KSM entry points at this table.
pub(crate) const G17P_FRAGMENT_MCACHE_OFFSET: usize = 0x6a0;
/// The tiler's MCache aperture table. `mcacheApertureBufferSetup` is called for
/// the TA with `ksmBufferInfo = recTA+0x60` and writes its entries at
/// `ksmBufferInfo + 0x600 + i*0x10`, i.e. `recTA + 0x660` -- the exact analogue
/// of the 3D's `rec3D + 0x6a0`.
pub(crate) const G17P_TILING_MCACHE_OFFSET: usize = 0x660;
pub(crate) const G17P_MCACHE_MAPPING_SIZE: usize = 0x10;
/// Temporary diagnostic mirror of the four Register Command Encoder programs.
/// Native keeps the complete programs in the 3D command itself: the first is
/// at command +0xa0 and the remaining phases follow at a 0x720-byte stride.
/// Each phase's +0x700 trailer points back at that same embedded phase and its
/// +0x708 word carries the entry count.  KSM must bind those complete embedded
/// structures, not this entries-only mirror.
pub(crate) const G17P_FRAGMENT_RCE_STORAGE_SIZE: usize = 0x4000;
pub(crate) const G17P_FRAGMENT_RCE_PROGRAM_OFFSET: u64 = 0xa0;
pub(crate) const G17P_FRAGMENT_RCE_PROGRAM_STRIDE: u64 = 0x720;

const TILE_WIDTH: u64 = 32;
const TILE_HEIGHT: u64 = 32;
const MACRO_TILES_X: u64 = 4;
const MACRO_TILES_Y: u64 = 4;
const REGION_ENTRY_SIZE: u64 = 5;
const EVENT_SELECTOR: u32 = 0x0e;
const EVENT_SUBTYPE_BASE: u32 = 0x0001_0000;
const EVENT_RECORD_SIZE: usize = 0x40;
const RING_SLOT_SIZE: usize = 0x18;
const QUEUE_WRITE_OFFSET: u64 = 0x40;
/// Base added by G17P's compact VDM control-stream register (0x1c880).
/// The register retains the legacy 40-bit {TTBR selector, 39-bit offset}
/// shape, so lower-root user addresses are encoded relative to this base.
const G17P_USER_BASE: u64 = 0x10_0000_0000;
const G17P_VDM_USER_WINDOW_SIZE: u64 = 1 << 39;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RenderBuildError {
    ZeroDimensions,
    LayerCountOutOfRange,
    UtileDimensions,
    AddressBeforeContext,
    UserAddressOutOfRange,
    VdmAddressOutOfRange,
    BufferTooSmall,
    ArithmeticOverflow,
    QueueGridOutOfRange,
    RingSlotOutOfRange,
    McacheAddressOutOfRange,
    McacheRangeEmpty,
    McacheSelectorOutOfRange,
}

impl RenderBuildError {
    /// Stable short name for one descriptor-build refusal. Callers flatten this
    /// enum to `EINVAL`, which erased the distinction between a geometry
    /// rejection and an address that sits below the render context base.
    pub(crate) fn name(&self) -> &'static str {
        match self {
            Self::ZeroDimensions => "zero-dimensions",
            Self::LayerCountOutOfRange => "layer-count-out-of-range",
            Self::UtileDimensions => "utile-dimensions",
            Self::AddressBeforeContext => "address-before-context",
            Self::UserAddressOutOfRange => "user-address-out-of-range",
            Self::VdmAddressOutOfRange => "vdm-address-out-of-range",
            Self::BufferTooSmall => "buffer-too-small",
            Self::ArithmeticOverflow => "arithmetic-overflow",
            Self::QueueGridOutOfRange => "queue-grid-out-of-range",
            Self::RingSlotOutOfRange => "ring-slot-out-of-range",
            Self::McacheAddressOutOfRange => "mcache-address-out-of-range",
            Self::McacheRangeEmpty => "mcache-range-empty",
            Self::McacheSelectorOutOfRange => "mcache-selector-out-of-range",
        }
    }
}

/// One resource-derived G17P MCache write-range mapping.
///
/// This is the exact input shape consumed by
/// `AGXAcceleratorG16::halMCacheApertureSetup`: address and size are in
/// 128-byte cache lines; `hwsid` is the allocated hardware DSID; `selector`
/// names the command's GART/UAT context; and `range_shift` is the native
/// write-range class (clamped to four by the kext).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PMcacheMapping {
    pub(crate) address: u64,
    pub(crate) size_units: u32,
    pub(crate) hwsid: u8,
    pub(crate) selector: u8,
    pub(crate) range_shift: u16,
}

/// Encode the two qwords written by the G17P kext at 3D descriptor
/// `+0x6a0 + index * 0x10`.
pub(crate) fn encode_g17p_mcache_mapping(
    mapping: G17PMcacheMapping,
) -> Result<[u8; G17P_MCACHE_MAPPING_SIZE], RenderBuildError> {
    if mapping.address & 0x7f != 0 || mapping.address >> 48 != 0 {
        return Err(RenderBuildError::McacheAddressOutOfRange);
    }
    if mapping.size_units == 0 {
        return Err(RenderBuildError::McacheRangeEmpty);
    }
    if mapping.selector > 0x3f {
        return Err(RenderBuildError::McacheSelectorOutOfRange);
    }

    let start = mapping.address >> 7;
    let end = start
        .checked_add(mapping.size_units as u64)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    // qword 1 reserves bits 41..46 for the range policy.
    if start >> 41 != 0 || end >> 41 != 0 {
        return Err(RenderBuildError::McacheAddressOutOfRange);
    }

    let q0 = start
        | ((mapping.selector as u64) << 41)
        | ((mapping.hwsid as u64) << 47)
        | (1u64 << 55);
    let range_shift = core::cmp::min(mapping.range_shift, 4) as u32;
    let q1 = end | (1u64 << 41) | (1u64 << (42 + range_shift));
    let mut out = [0u8; G17P_MCACHE_MAPPING_SIZE];
    out[..8].copy_from_slice(&q0.to_le_bytes());
    out[8..].copy_from_slice(&q1.to_le_bytes());
    Ok(out)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RegisterWrite {
    pub(crate) number: u32,
    pub(crate) value: u64,
}

impl RegisterWrite {
    const fn new(number: u32, value: u64) -> Self {
        Self { number, value }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17pRenderParameters {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) context_base: u64,
    /// Allocation epoch of the trusted execution-context lease, shared by TA/3D.
    pub(crate) context_generation: u8,
    pub(crate) tilemap: u64,
    /// Per-layer tiler metadata, which native places between the tilemap and
    /// the heap metadata. It is empty, and so equal to `heapmeta`, for a
    /// single-layer target.
    pub(crate) layermeta: u64,
    pub(crate) heapmeta: u64,
    pub(crate) tpc: u64,
    /// Geometry size of the owned TPC subregion, before capacity page rounding.
    pub(crate) tpc_size: u64,
    pub(crate) deflake_1: u64,
    pub(crate) deflake_2: u64,
    pub(crate) deflake_3: u64,
    pub(crate) encoder: u64,
    pub(crate) ta_status: u64,
    pub(crate) fragment_status: u64,
    pub(crate) store_pipeline_bind: u64,
    pub(crate) store_pipeline: u64,
    pub(crate) load_pipeline_bind: u64,
    pub(crate) load_pipeline: u64,
    pub(crate) partial_store_pipeline_bind: u64,
    pub(crate) partial_store_pipeline: u64,
    pub(crate) partial_load_pipeline_bind: u64,
    pub(crate) partial_load_pipeline: u64,
    pub(crate) scissor_array: u64,
    pub(crate) depth_bias_array: u64,
    pub(crate) aux_fb: u64,
    pub(crate) occlusion_query_base: u64,
    pub(crate) depth_buffer: u64,
    pub(crate) stencil_buffer: u64,
    pub(crate) depth_aux_buffer: u64,
    pub(crate) stencil_aux_buffer: u64,
    pub(crate) depth_stride: u64,
    pub(crate) stencil_stride: u64,
    pub(crate) depth_aux_stride: u64,
    pub(crate) stencil_aux_stride: u64,
    pub(crate) depth_clear_value_bits: u32,
    pub(crate) stencil_clear_value: u32,
    pub(crate) depth_flags: u64,
    /// ZLS control used by every firmware partial-store/load/resume phase.
    /// Unlike the pass-boundary control, an attached depth/stencil surface
    /// must be both loaded and stored across an internal split.
    pub(crate) partial_depth_flags: u64,
    pub(crate) depth_dimensions: u64,
    pub(crate) merge_upper_x_bits: u32,
    pub(crate) merge_upper_y_bits: u32,
    pub(crate) layers: u16,
    /// Sample count: 1, 2 or 4. Defaults to 1, so every existing parameter
    /// vector keeps its exact register program.
    pub(crate) samples: u8,
    pub(crate) utile_width: u8,
    pub(crate) utile_height: u8,
    pub(crate) utile_config: u64,
    pub(crate) multisample_control: u64,
    pub(crate) ppp_control: u64,
    pub(crate) tib_blocks: u64,
    pub(crate) tile_config: u64,
    pub(crate) aux_fb_flags: u64,
    pub(crate) aux_fb_page_count: u64,
    pub(crate) cycle: u64,
    pub(crate) record_index: u64,
    /// TA event correlation written to 0x1ca10/0x014a1/0x0a349.
    /// Native sources this independently from the fragment correlation.
    pub(crate) lifecycle: u64,
    /// Fragment event correlation written to 0x160e0/0x01499/0x0a341.
    pub(crate) fragment_lifecycle: u64,
    pub(crate) sampler_array: u64,
    pub(crate) sampler_count: u32,
    pub(crate) process_empty_tiles: bool,
    pub(crate) ta_timestamp_start: u64,
    pub(crate) ta_timestamp_end: u64,
    pub(crate) fragment_timestamp_start: u64,
    pub(crate) fragment_timestamp_end: u64,
    /// Write the VDM control-stream register (0x1c880) as a full GPU virtual
    /// address instead of the native user-process compact form.
    ///
    /// SCOPE CORRECTED against the macOS oracle. This flag once covered every
    /// register `ta_offset` feeds, justified from the legacy G14X path where
    /// all of them are whole pointers. A real Metal render pass on THIS chip
    /// says otherwise: the captured TA command record writes tilemap, TPC and
    /// heapmeta as small context-relative offsets (see `ta_offset`). The legacy
    /// encoding does not transfer to G17P, so those six registers are always
    /// relative now and this flag applies to the VDM stream alone. Native first
    /// lowers the task VA to a task-relative offset, maps the same backing at
    /// 0x1000000000 + offset in the GART window, and writes that offset here.
    /// Linux userspace supplies the full GART-alias VA; this kernel subtracts
    /// the architectural 0x1000000000 base. Hardware adds it back for compact
    /// pointers whose bit 39 is clear.
    pub(crate) absolute_pointers: bool,
    ///
    /// Legacy diagnostic OR-mask for the BG resource-specifier high 17 bits.
    /// Native calls this field PassControlMaskType and derives it from shader
    /// outputs, including extension of the highest active pass bit; it is not
    /// an address class or a universal constant. Main and partial shaders can
    /// legitimately differ. Default zero preserves the userspace mask;
    /// a nonzero value is an explicit diagnostic override only.
    pub(crate) bg_resource_prefix: u64,
    /// Historical opt-in diagnostic changing byte 2 of the USC private-memory
    /// budget qword at TA +0x93d. It must not override +0x94d: that byte is the
    /// allocation generation of the execution context, not a PM mode flag.
    /// The captured budget is not yet derived from our pool allocation.
    pub(crate) native_pm_bytes: bool,
    /// Hardware-buffer ID allocated for this TA command. Native allocates it
    /// from the accelerator's 0x7f-entry `AGXHardwareBufferIDManager`, stores
    /// it in the host-side `AGXTACommandDescriptor` at +0x1248/+0x928, and the
    /// register generator emits that value as register 0x1c830. Those two host
    /// offsets are not fields in this final 0x9c0-byte firmware command.
    pub(crate) ta_hardware_buffer_id: u32,
    /// HardwareBufferID of the retained USC private-memory FList shared by TA
    /// and 3D. Native allocates this from the USC manager (accelerator
    /// +0x133c8), independently of the TA parameter-buffer manager above, and
    /// emits the same ID in TA register 0x14308 and fragment register 0x14048.
    pub(crate) usc_flist_hardware_buffer_id: u32,
    /// Diagnostic capture-value bitmask retained for fields whose proper
    /// inputs are not modeled yet. In particular, the 0xa5a* values are
    /// assembled from workload-derived USC pool limits. Their captured values
    /// are not architectural literals and must not become defaults.
    pub(crate) native_ta_registers: u32,
    /// Retained GPU-ID diagnostic snapshot, not the fragment parameter-buffer
    /// owner. The old 0x1c838 use confused host descriptor +0x928 with a
    /// different object's transient raw-command +0x928 field.
    pub(crate) gpc_perf_state_map: u32,
    /// Low nibble of SGX 0xe01480, copied by native 3D submit to tail +0x210c.
    pub(crate) gpc_perf_state_map_low: u32,
    /// Bit 0 of SGX 0xe0141c, copied by native 3D submit to tail +0x2104.
    pub(crate) gpc_perf_state_control: u32,
    /// GPU address of the selected native 0x80 Scene-state entry, or 0 for
    /// none. The command-record binding is carried by `RenderDescriptorObjects`
    /// in both TA and 3D; this copy is retained for diagnostics.
    pub(crate) parameter_buffer: u64,
}

impl G17pRenderParameters {
    /// Apply the witnessed multisample register triple for a sample count.
    ///
    /// Captured constants, not derived: from Apple's own renders on this part
    /// (logs/m5-msaa-native-hv-20260915-101837) at one, two and four samples,
    /// with a same-mode control in a second oracle process establishing the
    /// noise floor. `multisample_control` is the sample POSITION table --
    /// 0x88 is the centre (8,8), 0x44cc is (4,4),(12,12), and 0xeaa26e26
    /// unpacks to (14,10),(10,2),(6,14),(2,6), the rotated-grid 4x pattern in
    /// 1/16 pixel units. `utile_config`'s low nibble is log2(samples);
    /// `tib_blocks` is samples << 16, zero at one sample.
    pub(crate) fn set_samples(&mut self, samples: u8) -> bool {
        let (multisample_control, utile_config, tib_blocks) = match samples {
            1 => (0x88, 0xa000, 0),
            2 => (0x44cc, 0xa001, 0x2_0000),
            4 => (0xeaa2_6e26, 0x6002, 0x4_0000),
            _ => return false,
        };
        self.samples = samples;
        self.multisample_control = multisample_control;
        self.utile_config = utile_config;
        self.tib_blocks = tib_blocks;
        true
    }

    pub(crate) const fn source_rsrc8() -> Self {
        Self {
            width: 64,
            height: 64,
            context_base: 0x0000_0010_0000_0000,
            context_generation: 1,
            tilemap: 0x0000_0010_001b_0000,
            layermeta: 0x0000_0010_001b_5000,
            heapmeta: 0x0000_0010_001b_5000,
            tpc: 0x0000_0010_0024_0000,
            tpc_size: 0x800,
            deflake_1: 0x0000_0010_0006_82a0,
            deflake_2: 0x0000_0010_0006_8020,
            deflake_3: 0x0000_0010_0006_8000,
            encoder: 0x0000_0010_0001_8000,
            ta_status: 0x0000_0010_0007_8000,
            fragment_status: 0x0000_0010_001a_8000,
            store_pipeline_bind: G17P_EOT_RESOURCE_SPEC,
            store_pipeline: G17P_SOURCE_EOT_PIPELINE,
            load_pipeline_bind: G17P_LOAD_RESOURCE_SPEC,
            load_pipeline: G17P_SOURCE_BG_PIPELINE,
            partial_store_pipeline_bind: 0,
            partial_store_pipeline: 0,
            partial_load_pipeline_bind: 0,
            partial_load_pipeline: 0,
            scissor_array: 0x0000_0100_019a_0000,
            depth_bias_array: 0x0000_0100_01af_8000,
            aux_fb: 0x0000_0100_01aa_8000,
            occlusion_query_base: 0,
            depth_buffer: 0,
            stencil_buffer: 0,
            depth_aux_buffer: 0,
            stencil_aux_buffer: 0,
            depth_stride: 0,
            stencil_stride: 0,
            depth_aux_stride: 0,
            stencil_aux_stride: 0,
            depth_clear_value_bits: 0x3f80_0000,
            stencil_clear_value: 0,
            depth_flags: 0,
            partial_depth_flags: 0,
            depth_dimensions: 0,
            merge_upper_x_bits: 0x3cdd_b3d9,
            merge_upper_y_bits: 0x3cdd_b3d9,
            layers: 1,
            samples: 1,
            utile_width: 32,
            utile_height: 32,
            utile_config: 0xa000,
            multisample_control: 0x88,
            ppp_control: 0x202,
            tib_blocks: 8,
            tile_config: 0x10280,
            aux_fb_flags: 0xc001,
            aux_fb_page_count: 0x10_0000,
            cycle: 0x17_8020,
            record_index: 0x8_0005,
            lifecycle: 0,
            fragment_lifecycle: 0,
            sampler_array: 0,
            sampler_count: 0,
            process_empty_tiles: true,
            ta_timestamp_start: 0xffff_fc20_0002_4c68,
            ta_timestamp_end: 0,
            fragment_timestamp_start: 0xffff_fc20_0002_4c68,
            fragment_timestamp_end: 0xffff_fc20_0002_4c70,
            absolute_pointers: false,
            bg_resource_prefix: 0,
            native_pm_bytes: false,
            ta_hardware_buffer_id: 0,
            usc_flist_hardware_buffer_id: 0,
            native_ta_registers: 0,
            gpc_perf_state_map: 0,
            gpc_perf_state_map_low: 0,
            gpc_perf_state_control: 0,
            parameter_buffer: 0,
        }
    }
}

#[derive(Debug, Copy, Clone)]
struct Geometry {
    tiles_x: u64,
    tiles_y: u64,
    size1: u64,
    size2: u64,
    size3: u64,
    x_blocks: u64,
    y_blocks: u64,
    screen: u64,
    pixels: u64,
    macro_size: u64,
}

const fn align(value: u64, alignment: u64) -> u64 {
    (value + alignment - 1) & !(alignment - 1)
}

fn geometry(parameters: &G17pRenderParameters) -> Result<Geometry, RenderBuildError> {
    if parameters.width == 0 || parameters.height == 0 {
        return Err(RenderBuildError::ZeroDimensions);
    }
    if parameters.layers == 0 || parameters.layers > 2048 {
        return Err(RenderBuildError::LayerCountOutOfRange);
    }

    let width = parameters.width as u64;
    let height = parameters.height as u64;
    let tiles_x = (width + TILE_WIDTH - 1) / TILE_WIDTH;
    let tiles_y = (height + TILE_HEIGHT - 1) / TILE_HEIGHT;
    let (utile_width, utile_height) = (parameters.utile_width, parameters.utile_height);
    if !matches!((utile_width, utile_height), (32, 32) | (32, 16) | (16, 16)) {
        return Err(RenderBuildError::UtileDimensions);
    }
    let utiles_per_tile_x = TILE_WIDTH / utile_width as u64;
    let utiles_per_tile_y = TILE_HEIGHT / utile_height as u64;
    // Four samples split the utile in Y. Measured against Apple's own
    // four-sample render: size1 0x14 -> 0x28, size3 0x20 -> 0x40 and
    // macro_size 0x40004 -> 0x40008, where only the y term moves. All three
    // follow from doubling this one term; two samples do not split.
    let utiles_per_tile_y = if parameters.samples == 4 {
        utiles_per_tile_y * 2
    } else {
        utiles_per_tile_y
    };
    let utiles_per_tile = utiles_per_tile_x * utiles_per_tile_y;
    let mtile_x1 = align((tiles_x + MACRO_TILES_X - 1) / MACRO_TILES_X, 4);
    let mtile_y1 = align((tiles_y + MACRO_TILES_Y - 1) / MACRO_TILES_Y, 4);
    let mtile_x2 = 2 * mtile_x1;
    let mtile_x3 = 3 * mtile_x1;
    let mtile_y2 = 2 * mtile_y1;
    let mtile_y3 = 3 * mtile_y1;
    let mtile_stride = mtile_x1 * mtile_y1;
    let size1 = (REGION_ENTRY_SIZE * mtile_stride * utiles_per_tile + 3) / 4;

    Ok(Geometry {
        tiles_x,
        tiles_y,
        size1,
        size2: mtile_stride,
        size3: 2 * mtile_stride * utiles_per_tile,
        x_blocks: mtile_x3 | (mtile_x2 << 9) | (mtile_x1 << 18),
        y_blocks: mtile_y3 | (mtile_y2 << 9) | (mtile_y1 << 18),
        screen: ((tiles_y - 1) << 12) | (tiles_x - 1),
        pixels: (width - 1) | ((height - 1) << 16),
        macro_size: mtile_y1 * utiles_per_tile_y | ((mtile_x1 * utiles_per_tile_x) << 16),
    })
}

/// Context-relative offset. ALWAYS relative -- a real Metal render pass on this
/// chip writes small offsets into these registers, not addresses.
///
/// Oracle: the TA command record captured from `mtlg.m` (macos-trace
/// `recs-gfx.txt`, dumped by `recs.d` at `requestChannelCommandTA`) carries an
/// embedded register list that decodes field-for-field against this function's
/// output -- same registers, same order, 12-byte entries of u32 number plus u64
/// value -- with these values:
///
///   0x01748 = 1                    (matches)
///   0x10141 = 0x200                (matches)
///   0x1c039 = 0x0119ba00           tilemap   <- OFFSET, not a VA
///   0x1c9c8 = 0x0119ba00           tilemap
///   0x1c0a1 = 0x01290000           tpc       <- OFFSET
///   0x1c031 = 0x800000000119ca00   heapmeta | bit63 (matches the form)
///   0x1c9c0 = 0x800000000119ca00   heapmeta
///   0x1c051 = 0x003a0012006b0003   (matches the constant exactly)
///
/// Writing full virtual addresses here -- which is what the legacy G14X path
/// does, and what an earlier version of this driver was changed to do -- points
/// the tiler's tilemap, TPC and heapmeta at values the firmware cannot use.
fn ta_resource_offset(
    parameters: &G17pRenderParameters,
    address: u64,
) -> Result<u64, RenderBuildError> {
    address
        .checked_sub(parameters.context_base)
        .ok_or(RenderBuildError::AddressBeforeContext)
}

/// Compact user/GART address consumed by the TA register and descriptor
/// fields. Native maps these objects in the architectural 0x1000000000 window
/// and publishes the offset, not the full address. Descriptor +0x7d6 is only
/// 32 bits wide, so reject aliases that cannot be represented consistently.
fn ta_user_offset(address: u64) -> Result<u64, RenderBuildError> {
    let offset = address
        .checked_sub(G17P_USER_BASE)
        .ok_or(RenderBuildError::UserAddressOutOfRange)?;

    if offset > u32::MAX as u64 {
        return Err(RenderBuildError::UserAddressOutOfRange);
    }

    Ok(offset)
}

fn vdm_user_offset(address: u64) -> Result<u64, RenderBuildError> {
    let offset = address
        .checked_sub(G17P_USER_BASE)
        .ok_or(RenderBuildError::VdmAddressOutOfRange)?;

    if offset >= G17P_VDM_USER_WINDOW_SIZE {
        return Err(RenderBuildError::VdmAddressOutOfRange);
    }

    Ok(offset)
}

pub(crate) fn build_ta_registers(
    p: &G17pRenderParameters,
) -> Result<[RegisterWrite; TA_REGISTER_COUNT], RenderBuildError> {
    build_ta_registers_for_work_key(p, render_work_key(1, 0))
}

fn build_ta_registers_for_work_key(
    p: &G17pRenderParameters,
    work_key: u64,
) -> Result<[RegisterWrite; TA_REGISTER_COUNT], RenderBuildError> {
    let g = geometry(p)?;
    let tilemap = ta_resource_offset(p, p.tilemap)?;
    let layermeta = ta_resource_offset(p, p.layermeta)?;
    let heapmeta = ta_resource_offset(p, p.heapmeta)?;
    let tpc = ta_resource_offset(p, p.tpc)?;
    let deflake_1 = ta_user_offset(p.deflake_1)?;
    let deflake_2 = ta_user_offset(p.deflake_2)?;
    let deflake_3 = ta_user_offset(p.deflake_3)?;
    // The VDM control stream is the one pointer the 192-byte capture window
    // does not reach (register 0x1c880 sits past the end of the dump), and it
    // is the one that is not relative to the driver-private `context_base`.
    // macOS first lowers the task VA to a task-relative offset, then maps the
    // backing at 0x1000000000 + offset in the GART window. Linux userspace gives
    // us that full alias VA, so this hardware encoder subtracts the GART base.
    // Bit 39 selects the upper UAT root; refuse an offset that would silently
    // change roots. Keep the full-address escape hatch diagnostic-only.
    let encoder = if p.absolute_pointers {
        p.encoder
    } else {
        vdm_user_offset(p.encoder)?
    };
    // generateTARegisterListG16Common: the layer count less one with bit 15,
    // plus bit 14 and a process-empty-tiles select for a layered target. The
    // pipe mirror 0x1c9e8 keeps only the layered bit and the count.
    let mut layer_mode = 0x8000 | (p.layers as u64 - 1);
    if p.layers > 1 {
        layer_mode |= 0x4000 | if p.process_empty_tiles { 0x2000 } else { 0x1000 };
    }

    Ok([
        RegisterWrite::new(0x01748, 1),
        // Native N=1 uses 0x200 and N=2 uses 0x0. The one-layer source profile
        // selects the N=1 value, matching descriptor byte +0x789 above.
        RegisterWrite::new(0x10141, 0x200),
        RegisterWrite::new(0x1c039, tilemap),
        RegisterWrite::new(0x1c9c8, tilemap),
        RegisterWrite::new(0x1c0a1, tpc),
        RegisterWrite::new(0x1c031, heapmeta | 0x8000_0000_0000_0000),
        RegisterWrite::new(0x1c9c0, heapmeta | 0x8000_0000_0000_0000),
        RegisterWrite::new(0x1c051, 0x003a_0012_006b_0003),
        RegisterWrite::new(0x1c061, 1),
        RegisterWrite::new(0x10149, p.utile_config),
        RegisterWrite::new(0x10139, p.multisample_control),
        RegisterWrite::new(0x10111, deflake_1),
        RegisterWrite::new(0x1c9b0, deflake_1),
        RegisterWrite::new(0x10119, deflake_2),
        RegisterWrite::new(0x1c9b8, deflake_2),
        RegisterWrite::new(0x1c958, 1),
        RegisterWrite::new(0x1c950, deflake_3 | 0x0004_0000_0000_0000),
        RegisterWrite::new(0x1c930, 0),
        RegisterWrite::new(0x1c880, encoder),
        RegisterWrite::new(0x1c079, layermeta),
        RegisterWrite::new(0x1c9d8, layermeta),
        RegisterWrite::new(0x10151, 0),
        RegisterWrite::new(0x1c199, 0),
        RegisterWrite::new(0x1c1a1, 0),
        RegisterWrite::new(0x1c1a9, 0),
        RegisterWrite::new(0x1c1b1, 0),
        RegisterWrite::new(0x1c1b9, 0),
        RegisterWrite::new(0x1c8f8, 0x8860),
        RegisterWrite::new(0x1c0b1, g.size1),
        RegisterWrite::new(0x1c850, g.size1),
        // Silicon holds 0x10131 at 0x88 at ONE, TWO and FOUR samples while
        // 0x10139 carries the real sample position table (all five captures in
        // logs/m5-msaa-native-hv-20260915-101837). Driving this from
        // multisample_control would write the position table here, which
        // Apple never does.
        RegisterWrite::new(0x10131, 0x88),
        RegisterWrite::new(0x10121, p.ppp_control),
        RegisterWrite::new(0x10129, g.pixels),
        RegisterWrite::new(0x101b9, g.screen),
        RegisterWrite::new(0x1c069, g.x_blocks),
        RegisterWrite::new(0x1c071, g.y_blocks),
        RegisterWrite::new(0x1c081, g.size2),
        RegisterWrite::new(0x1c0a9, g.size3),
        RegisterWrite::new(0x10171, 0x100),
        RegisterWrite::new(0x10169, layer_mode),
        RegisterWrite::new(0x0a309, 0),
        RegisterWrite::new(0x1c8e0, u64::MAX),
        RegisterWrite::new(0x1c8e8, u64::MAX),
        RegisterWrite::new(0x1c898, 0),
        RegisterWrite::new(0x101e1, 0x1c),
        RegisterWrite::new(0x1c9e8, layer_mode & 0x47ff),
        RegisterWrite::new(0x1a099, 0),
        RegisterWrite::new(0x1a0a1, 0),
        RegisterWrite::new(0x1a069, 0),
        RegisterWrite::new(0x1a071, 0),
        RegisterWrite::new(0x1a0c9, 0),
        RegisterWrite::new(0x1a0d1, 0),
        RegisterWrite::new(0x101c9, 0),
        RegisterWrite::new(0x0d471, 0),
        RegisterWrite::new(0x1a0f1, 8),
        RegisterWrite::new(0x10799, 0xff_0000),
        // Native sources this from AGXHardwareBufferIDManager. It is neither a
        // PBDesc ring index nor a capture-stable literal.
        RegisterWrite::new(0x1c830, p.ta_hardware_buffer_id as u64),
        RegisterWrite::new(0x1ca30, p.cycle),
        RegisterWrite::new(0x16c39, p.cycle),
        RegisterWrite::new(0x1c910, p.record_index),
        // Native derives the high word from the TA AGXUMADataRec PTC/DUPM
        // maximum at host descriptor +0x1488, copied through command +0x1158.
        // halUpdatePTCMaxCount computes max(0x60, min(pool_input / active_kicks,
        // 0x200)); Linux does not yet carry that pool input. Follow the kext's
        // exact zero-source fallback 0x60_0040_0020. Bit 1 retains one working
        // Metal trace's workload-derived 0xfe value as a diagnostic only.
        RegisterWrite::new(
            0x0a5a1,
            if p.native_ta_registers & 2 != 0 {
                0x0000_00fe_0040_0020
            } else {
                0x0000_0060_0040_0020
            },
        ),
        RegisterWrite::new(0x0d419, 0x0000_0002_0000_0001),
        RegisterWrite::new(0x1ca10, p.lifecycle),
        RegisterWrite::new(0x014a1, p.lifecycle),
        RegisterWrite::new(0x0a349, p.lifecycle),
        RegisterWrite::new(0x10209, work_key),
        RegisterWrite::new(0x1c9f0, work_key),
        RegisterWrite::new(0x14320, work_key),
        // AGXTACommandDescriptor::prepare receives the shared USC FList ID
        // through descriptor +0x1410. Optional GART/feature payloads are absent
        // in Linux, so the retained owner's ID is the complete value.
        RegisterWrite::new(0x14308, p.usc_flist_hardware_buffer_id as u64),
        RegisterWrite::new(0x14318, p.ta_status | 1),
        RegisterWrite::new(0x01740, 1),
        RegisterWrite::new(0x1c880, deflake_1),
        RegisterWrite::new(0x1c898, 1),
    ])
}

/// DIAGNOSTIC. Write native's value into TA-descriptor fields where ours differs
/// structurally at MATCHED 256x256 geometry (recs3.txt TA record, stride 0x9c0).
/// The tiler bins ~60 bytes of a 1.28 MB TVB with every input verified correct,
/// so these are what is left that we demonstrably send differently.
fn apply_g17p_ta_oracle(raw: &mut [u8]) {
    let mask = crate::g18::fw::config::TA_ORACLE;
    if mask == 0 {
        return;
    }
    if mask & 0x01 != 0 {
        put_u64(raw, 0x370, 0x500);
        put_u64(raw, 0x388, 0x500);
        put_u64(raw, 0x8d0, 0x500);
    }
    if mask & 0x02 != 0 {
        put_u64(raw, 0x018, 3);
    }
    if mask & 0x04 != 0 {
        put_u32(raw, 0x33c, 0xa9);
    }
    if mask & 0x08 != 0 {
        put_u64(raw, 0x7d8, 0x6);
        put_u32(raw, 0x798, 0xa);
    }
}

pub(crate) fn build_fragment_registers(
    p: &G17pRenderParameters,
) -> Result<[RegisterWrite; FRAGMENT_REGISTER_COUNT], RenderBuildError> {
    build_fragment_registers_for_work_key(p, render_work_key(1, 1))
}

/// As `build_fragment_registers`, plus the live entry count. The count is
/// always `FRAGMENT_REGISTER_COUNT`; the pair form is kept because the
/// descriptor's count word at +0x7a8 must agree with the slice actually
/// written, and having one source for both makes them impossible to desync.
pub(crate) fn build_fragment_registers_counted(
    p: &G17pRenderParameters,
) -> Result<([RegisterWrite; FRAGMENT_REGISTER_COUNT], usize), RenderBuildError> {
    Ok((
        build_fragment_registers_for_work_key(p, render_work_key(1, 1))?,
        FRAGMENT_REGISTER_COUNT,
    ))
}

fn build_fragment_registers_for_work_key(
    p: &G17pRenderParameters,
    work_key: u64,
) -> Result<[RegisterWrite; FRAGMENT_REGISTER_COUNT], RenderBuildError> {
    let g = geometry(p)?;
    let tile_state = 0x3717f
        | ((g.tiles_x - 1) << 44)
        | ((g.tiles_y - 1) << 53)
        | 0x20_0000_0000
        | (if p.layers > 1 { 0x1_0000_0000 } else { 0 })
        | ((p.utile_config & 0xf000) << 28);

    Ok([
        RegisterWrite::new(0x01739, 1),
        RegisterWrite::new(0x10009, p.utile_config),
        RegisterWrite::new(0x15379, p.store_pipeline_bind),
        RegisterWrite::new(0x15381, p.store_pipeline),
        RegisterWrite::new(0x15369, p.load_pipeline_bind),
        RegisterWrite::new(0x15371, p.load_pipeline),
        RegisterWrite::new(0x15131, p.merge_upper_x_bits as u64),
        RegisterWrite::new(0x15139, p.merge_upper_y_bits as u64),
        RegisterWrite::new(0x100a1, 0),
        RegisterWrite::new(0x15069, 0),
        RegisterWrite::new(0x15071, 0),
        RegisterWrite::new(0x16058, 0),
        RegisterWrite::new(0x10019, p.multisample_control),
        RegisterWrite::new(0x100b1, g.macro_size),
        RegisterWrite::new(0x16030, g.macro_size),
        RegisterWrite::new(0x100d9, g.screen),
        RegisterWrite::new(0x0a301, 0),
        RegisterWrite::new(0x10791, 0xff_0200),
        RegisterWrite::new(0x16098, p.heapmeta),
        RegisterWrite::new(0x15109, p.scissor_array),
        RegisterWrite::new(0x15101, p.depth_bias_array),
        RegisterWrite::new(0x15021, p.aux_fb_flags),
        RegisterWrite::new(0x15211, ((p.height as u64) << 32) | p.width as u64),
        RegisterWrite::new(0x15049, p.aux_fb_page_count),
        RegisterWrite::new(0x10051, p.tib_blocks),
        RegisterWrite::new(0x15321, p.depth_dimensions),
        RegisterWrite::new(0x15301, p.depth_clear_value_bits as u64),
        RegisterWrite::new(0x15309, (p.stencil_clear_value | 0x300) as u64),
        RegisterWrite::new(0x15311, p.occlusion_query_base),
        RegisterWrite::new(0x15319, p.depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
        RegisterWrite::new(0x15351, 0),
        RegisterWrite::new(0x15329, p.depth_buffer),
        RegisterWrite::new(0x15331, p.depth_buffer),
        RegisterWrite::new(0x15339, p.stencil_buffer),
        RegisterWrite::new(0x15341, p.stencil_buffer),
        RegisterWrite::new(0x15231, 0),
        RegisterWrite::new(0x15221, 0),
        RegisterWrite::new(0x15239, 0),
        RegisterWrite::new(0x15229, 0),
        RegisterWrite::new(0x15401, p.depth_stride),
        RegisterWrite::new(0x15421, p.depth_stride),
        RegisterWrite::new(0x15409, p.stencil_stride),
        RegisterWrite::new(0x15429, p.stencil_stride),
        RegisterWrite::new(0x153c1, p.depth_aux_buffer),
        RegisterWrite::new(0x15411, p.depth_aux_stride),
        RegisterWrite::new(0x153c9, p.depth_aux_buffer),
        RegisterWrite::new(0x15431, p.depth_aux_stride),
        RegisterWrite::new(0x153d1, p.stencil_aux_buffer),
        RegisterWrite::new(0x15419, p.stencil_aux_stride),
        RegisterWrite::new(0x153d9, p.stencil_aux_buffer),
        RegisterWrite::new(0x15439, p.stencil_aux_stride),
        RegisterWrite::new(0x16429, p.tilemap),
        RegisterWrite::new(0x16060, p.layermeta),
        RegisterWrite::new(0x16431, (4 * g.size1) << 24),
        RegisterWrite::new(0x10039, p.tile_config),
        RegisterWrite::new(0x16020, 0),
        RegisterWrite::new(0x16451, 0),
        RegisterWrite::new(0x15359, 0),
        RegisterWrite::new(0x100b8, 0x8860),
        RegisterWrite::new(0x16461, p.aux_fb),
        RegisterWrite::new(0x16090, p.aux_fb),
        RegisterWrite::new(0x101e9, 0x1c),
        RegisterWrite::new(0x160a8, 0),
        RegisterWrite::new(0x16068, tile_state),
        RegisterWrite::new(0x1a0a9, 0),
        RegisterWrite::new(0x1a0b1, 0),
        RegisterWrite::new(0x1a079, 0),
        RegisterWrite::new(0x1a081, 0),
        RegisterWrite::new(0x1a0d9, 0),
        RegisterWrite::new(0x1a0e1, 0),
        RegisterWrite::new(0x101c1, 0),
        RegisterWrite::new(0x0d469, 0),
        RegisterWrite::new(0x1a0f9, 8),
        // Fragment analog of TA 0x0a5a1. Native derives its high word from the
        // 3D AGXUMADataRec PTC/DUPM maximum at host descriptor +0xc00, copied
        // through command +0x818. Linux has no owned pool-size input yet, so
        // use the exact 0x60 zero-source fallback. Bit 3 retains one captured,
        // workload-derived 0xd5 value only as a diagnostic.
        RegisterWrite::new(
            0x0a5a9,
            if p.native_ta_registers & 8 != 0 {
                0x0000_00d5_0040_0020
            } else {
                0x0000_0060_0040_0020
            },
        ),
        RegisterWrite::new(0x0d429, 0x0000_0002_0000_0001),
        RegisterWrite::new(0x160e0, p.fragment_lifecycle),
        RegisterWrite::new(0x01499, p.fragment_lifecycle),
        RegisterWrite::new(0x0a341, p.fragment_lifecycle),
        // The fragment must use the same parameter-buffer owner as TA 1c830.
        // Native TA::prepare stores its allocated HWPB ID at host +928; the
        // same derived host descriptor is submitted through both channels.
        // generate3DRegisterList reads that HOST field at 8a5c7cc. Its wrapper
        // at 8a515d8 tail-calls with x1=raw command, x2=host descriptor: the
        // unused channel argument in the mangled common-function name is NOT
        // present in the optimized calling convention. Raw command +928 has
        // a different producer and must not be used to infer this register.
        RegisterWrite::new(0x1c838, p.ta_hardware_buffer_id as u64),
        RegisterWrite::new(0x1ca28, p.cycle),
        RegisterWrite::new(0x10211, work_key),
        RegisterWrite::new(0x10420, work_key),
        // AGX3DCommandDescriptor::prepare receives this ID from the USC pool's
        // HardwareBufferIDManager and generate3DRegisterList ORs it with an
        // optional GART payload and feature bit. Linux currently has neither
        // optional source, so publish the retained USC FList owner's exact ID.
        RegisterWrite::new(0x14048, p.usc_flist_hardware_buffer_id as u64),
        RegisterWrite::new(0x14080, p.fragment_status | 1),
        RegisterWrite::new(0x01731, 1),
        RegisterWrite::new(0x16020, 1),
        RegisterWrite::new(0x16020, 0),
        RegisterWrite::new(0x16068, 0x4_0000),
    ])
}

/// Size of one userspace global-ID group reserved by
/// `IOGPUDeviceUserClient::s_get_next_gid_group` on macOS.
pub(crate) const G17P_RENDER_GID_GROUP_STRIDE: u32 = 0x0100_0000;

/// Compose the predecessor/current IDs used by one native render pair.
///
/// macOS reserves a 0x01000000-ID range and gives its old global-counter value
/// to Metal. Metal returns that predecessor in render setup; the kernel then
/// allocates consecutive current IDs from the advanced global counter, 3D
/// first and TA second. The RCE programs and command tails both carry the
/// resulting `{predecessor,current}` pair. These are object IDs, not queue
/// submission ordinals.
pub(crate) const fn g17p_render_lifecycle_pair(
    predecessor: u32,
    fragment_current: u32,
) -> Option<(u64, u64)> {
    if predecessor == 0 || fragment_current == 0 {
        return None;
    }
    let tiling_current = match fragment_current.checked_add(1) {
        Some(value) if value != 0 => value,
        _ => return None,
    };
    let predecessor = (predecessor as u64) << 32;
    Some((
        predecessor | fragment_current as u64,
        predecessor | tiling_current as u64,
    ))
}

pub(crate) fn partial_store_registers(
    p: &G17pRenderParameters,
) -> [RegisterWrite; PARTIAL_STORE_REGISTER_COUNT] {
    [
        RegisterWrite::new(0x15379, p.partial_store_pipeline_bind),
        RegisterWrite::new(0x15381, p.partial_store_pipeline),
        RegisterWrite::new(0x10039, p.tile_config),
        RegisterWrite::new(0x15359, 0x20),
        RegisterWrite::new(0x15331, p.depth_buffer),
        RegisterWrite::new(0x153c9, p.depth_aux_buffer),
        RegisterWrite::new(0x15341, p.stencil_buffer),
        RegisterWrite::new(0x153d9, p.stencil_aux_buffer),
        RegisterWrite::new(0x15421, p.depth_stride),
        RegisterWrite::new(0x15431, p.depth_aux_stride),
        RegisterWrite::new(0x15429, p.stencil_stride),
        RegisterWrite::new(0x15439, p.stencil_aux_stride),
        RegisterWrite::new(0x15221, 0),
        RegisterWrite::new(0x15229, 0),
        RegisterWrite::new(0x15319, p.partial_depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
    ]
}

pub(crate) fn partial_resume_registers(
    p: &G17pRenderParameters,
) -> [RegisterWrite; PARTIAL_RESUME_REGISTER_COUNT] {
    [
        RegisterWrite::new(0x15379, p.partial_store_pipeline_bind),
        RegisterWrite::new(0x15381, p.partial_store_pipeline),
        RegisterWrite::new(0x15369, p.partial_load_pipeline_bind),
        RegisterWrite::new(0x15371, p.partial_load_pipeline),
        RegisterWrite::new(0x10039, p.tile_config & 0xffff),
        RegisterWrite::new(0x15359, 0x20),
        RegisterWrite::new(0x15331, p.depth_buffer),
        RegisterWrite::new(0x153c9, p.depth_aux_buffer),
        RegisterWrite::new(0x15341, p.stencil_buffer),
        RegisterWrite::new(0x153d9, p.stencil_aux_buffer),
        RegisterWrite::new(0x15421, p.depth_stride),
        RegisterWrite::new(0x15431, p.depth_aux_stride),
        RegisterWrite::new(0x15429, p.stencil_stride),
        RegisterWrite::new(0x15439, p.stencil_aux_stride),
        RegisterWrite::new(0x15221, 0),
        RegisterWrite::new(0x15229, 0),
        RegisterWrite::new(0x15309, (p.stencil_clear_value | 0x300) as u64),
        RegisterWrite::new(0x15329, p.depth_buffer),
        RegisterWrite::new(0x153c1, p.depth_aux_buffer),
        RegisterWrite::new(0x15339, p.stencil_buffer),
        RegisterWrite::new(0x153d1, p.stencil_aux_buffer),
        RegisterWrite::new(0x15319, p.partial_depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
    ]
}

pub(crate) fn partial_load_registers(
    p: &G17pRenderParameters,
) -> [RegisterWrite; PARTIAL_LOAD_REGISTER_COUNT] {
    [
        RegisterWrite::new(0x15369, p.partial_load_pipeline_bind),
        RegisterWrite::new(0x15371, p.partial_load_pipeline),
        RegisterWrite::new(0x10039, p.tile_config & 0xffff),
        RegisterWrite::new(0x15309, (p.stencil_clear_value | 0x300) as u64),
        RegisterWrite::new(0x15329, p.depth_buffer),
        RegisterWrite::new(0x153c1, p.depth_aux_buffer),
        RegisterWrite::new(0x15339, p.stencil_buffer),
        RegisterWrite::new(0x153d1, p.stencil_aux_buffer),
        RegisterWrite::new(0x15319, p.partial_depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
    ]
}

fn put_u8(raw: &mut [u8], offset: usize, value: u8) {
    raw[offset] = value;
}

#[cfg(test)]
fn put_u16(raw: &mut [u8], offset: usize, value: u16) {
    raw[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

// v91 WORD_STORES: aligned fields use one store (see g17_store).
#[cfg(not(test))]
#[inline(always)]
fn put_u16(raw: &mut [u8], offset: usize, value: u16) {
    crate::g18::fw::store::put_u16(raw, offset, value)
}

#[cfg(test)]
fn put_u32(raw: &mut [u8], offset: usize, value: u32) {
    raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

// v91 WORD_STORES: aligned fields use one store (see g17_store).
#[cfg(not(test))]
#[inline(always)]
fn put_u32(raw: &mut [u8], offset: usize, value: u32) {
    crate::g18::fw::store::put_u32(raw, offset, value)
}

#[cfg(test)]
fn put_u64(raw: &mut [u8], offset: usize, value: u64) {
    raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

// v91 WORD_STORES: aligned fields use one store (see g17_store).
#[cfg(not(test))]
#[inline(always)]
fn put_u64(raw: &mut [u8], offset: usize, value: u64) {
    crate::g18::fw::store::put_u64(raw, offset, value)
}

fn encode_registers(raw: &mut [u8], offset: usize, registers: &[RegisterWrite]) {
    for (index, register) in registers.iter().enumerate() {
        let entry = offset + index * REGISTER_BYTES;
        put_u32(raw, entry, register.number);
        put_u64(raw, entry + 4, register.value);
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RenderDescriptorKind {
    Tiling,
    Fragment,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RenderDescriptorObjects {
    pub(crate) record_a: u64,
    pub(crate) shared: u64,
    pub(crate) record_b: u64,
    pub(crate) zero: u64,
}

impl RenderDescriptorObjects {
    /// Select one 0x100-byte Pool-A record while leaving every other object
    /// pointer unchanged.
    pub(crate) fn with_pool_a_record_index(
        mut self,
        record_index: usize,
    ) -> Result<Self, RenderBuildError> {
        let offset = record_index
            .checked_mul(0x100)
            .ok_or(RenderBuildError::ArithmeticOverflow)?;
        self.record_a = self
            .record_a
            .checked_add(offset as u64)
            .ok_or(RenderBuildError::ArithmeticOverflow)?;
        Ok(self)
    }

    /// Replace the stage-local shared object without changing the other three
    /// descriptor pointers. Final native publishes keep TA +0x20 and Fragment
    /// +0x28 identical; callers must therefore apply any HWPB substitution to
    /// the shared object used by both descriptors, not to Fragment alone.
    pub(crate) const fn with_shared(mut self, shared: u64) -> Self {
        self.shared = shared;
        self
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RenderDescriptorMetadata {
    pub(crate) context_id: u32,
    pub(crate) submission_ordinal: u64,
    /// Monotonic state tag allocated by the command buffer's 36-entry state
    /// ring. This is neither the selected ring index nor any QID/HWID. Native
    /// retains the full word in the selected state object and places only its
    /// low six bits in the stage work key.
    pub(crate) command_buffer_state_word: u32,
    /// TA HardwareBufferID copied to firmware descriptor +0x18.
    ///
    /// Every working native descriptor corpus couples this field to register
    /// 0x1c830 exactly (for example 2/2, 4/4, and 5/5). The same allocator
    /// result is also PBState +0x0c and the descriptor-ring index consumed by
    /// `halLoadPBDesc`.
    pub(crate) ta_hardware_buffer_id: u32,
    pub(crate) submit_sequence: u64,
}

impl RenderDescriptorMetadata {
    pub(crate) const fn source(kind: RenderDescriptorKind) -> Self {
        Self {
            context_id: 1,
            submission_ordinal: 0,
            command_buffer_state_word: match kind {
                RenderDescriptorKind::Tiling => 0,
                RenderDescriptorKind::Fragment => 1,
            },
            ta_hardware_buffer_id: 0,
            submit_sequence: match kind {
                RenderDescriptorKind::Tiling => 1,
                RenderDescriptorKind::Fragment => 0,
            },
        }
    }
}

/// Native's TA/3D register-list producer couples the command-buffer state tag
/// to the descriptor's allocated application-GART context. The physical QID,
/// QoS HardwareBufferID, descriptor-array slot and 36-entry state-ring index
/// are independent namespaces and must not be substituted here.
const fn render_work_key(context_id: u32, command_buffer_state_word: u32) -> u64 {
    ((context_id as u64) << 8) | ((command_buffer_state_word & 0x3f) as u64)
}

/// The completion record must name the same generation as the rendered writes.
/// Native nextCommandBufferState stores the allocated word at EventControl +8;
/// generate3DRegisterList uses its low six bits in 0x10211/0x10420. B1 uses those
/// same bits in the cache-maintenance request before publishing the stamp.
/// Keep the full word here: physical slot ordinals and ring indices are not
/// substitutes for the command-buffer allocator's generation.
pub(crate) fn set_render_event_control_generation(
    event_control: &mut [u8],
    command_buffer_state_word: u32,
) -> Result<(), RenderBuildError> {
    let generation = event_control
        .get_mut(8..12)
        .ok_or(RenderBuildError::BufferTooSmall)?;
    generation.copy_from_slice(&command_buffer_state_word.to_le_bytes());
    Ok(())
}

fn descriptor_work_ordinal(ordinal: u64) -> u32 {
    (ordinal as u32).wrapping_add((ordinal / 2) as u32)
}

fn write_common_descriptor(
    kind: RenderDescriptorKind,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    registers: &[RegisterWrite],
    raw: &mut [u8],
) {
    let (selector, pointers, register_offset, pointer_gap) = match kind {
        RenderDescriptorKind::Tiling => (0u32, 0x10usize, 0x60usize, 8usize),
        RenderDescriptorKind::Fragment => (1u32, 0x20usize, 0xa0usize, 0usize),
    };
    put_u32(raw, 0, selector);
    put_u64(raw, 4, metadata.submit_sequence);
    put_u32(raw, 0x0c, metadata.context_id);

    let addresses = [
        objects.record_a,
        objects.shared,
        objects.record_b,
        objects.zero,
    ];
    let mut offset = pointers;
    for (index, address) in addresses.iter().enumerate() {
        put_u64(raw, offset, *address);
        offset += 8;
        if index == 0 {
            offset += pointer_gap;
        }
    }
    encode_registers(raw, register_offset, registers);

    let work = descriptor_work_ordinal(metadata.submission_ordinal);
    match kind {
        RenderDescriptorKind::Tiling => {
            put_u32(raw, 0x18, metadata.ta_hardware_buffer_id);
            put_u32(raw, 0x48, work);
        }
        RenderDescriptorKind::Fragment => {}
    }
}

pub(crate) fn build_ta_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    if raw.len() < TA_DESCRIPTOR_SIZE {
        return Err(RenderBuildError::BufferTooSmall);
    }
    let registers = build_ta_registers_for_work_key(
        parameters,
        render_work_key(metadata.context_id, metadata.command_buffer_state_word),
    )?;
    build_ta_descriptor_base(parameters, objects, metadata, &registers, raw)?;
    apply_g17p_ta_oracle(raw);
    Ok(())
}

pub(crate) fn build_ta_descriptor_base(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    registers: &[RegisterWrite],
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    if raw.len() < TA_DESCRIPTOR_SIZE {
        return Err(RenderBuildError::BufferTooSmall);
    }
    if registers.is_empty() || registers.len() > (0x660 - 0x60) / REGISTER_BYTES {
        return Err(RenderBuildError::BufferTooSmall);
    }
    raw[..TA_DESCRIPTOR_SIZE].fill(0);
    write_common_descriptor(
        RenderDescriptorKind::Tiling,
        objects,
        metadata,
        registers,
        raw,
    );

    // Like the fragment boundaries, these are generated for every native TA
    // command (25G83 88bdfc4/88be090/88be1a4), not only a queue's first kick.
    // The final three entries are 01740, 1c880, 1c898; firmware reads +38 and
    // +3a/+3c to select the corresponding embedded RCE boundaries.
    put_u16(raw, 0x38, (TA_REGISTER_COUNT - 2) as u16);
    put_u16(raw, 0x3a, TA_REGISTER_COUNT as u16);
    put_u16(raw, 0x3c, TA_REGISTER_COUNT as u16);

    let deflake = ta_user_offset(parameters.deflake_1)?;
    let sampler_max = if parameters.sampler_count == 0 {
        0
    } else {
        parameters.sampler_count + 1
    };
    put_u32(
        raw,
        0x0768,
        (((registers.len() * REGISTER_BYTES) as u32) << 16) | registers.len() as u32,
    );
    put_u64(raw, 0x0780, parameters.tpc);
    // Native copies RTMemory's geometry-derived TPC size beside its pointer
    // on every submission (25G83 88fd970..974), independent of queue lifetime.
    put_u64(raw, 0x0788, parameters.tpc_size);
    put_u32(raw, 0x07d6, deflake as u32);
    put_u32(raw, 0x0876, u32::MAX);
    put_u64(raw, 0x087a, parameters.sampler_array);
    put_u32(raw, 0x0882, parameters.sampler_count);
    put_u32(raw, 0x0886, sampler_max);
    // Present in 436/438 valid G17P TA records, including both exact 32x32
    // geometry matches. This belongs to the TA output/activation tail; leaving
    // it zero is a structural mismatch before the linked 3D job is generated.
    put_u32(raw, 0x0892, 1);
    put_u8(raw, 0x0932, 0x44);
    put_u8(raw, 0x093c, 1);
    put_u8(raw, 0x094d, parameters.context_generation);
    put_u64(raw, 0x08fe, parameters.ta_timestamp_start);
    put_u64(raw, 0x0906, parameters.ta_timestamp_end);
    // +0x090e/+0x0916 are caller-visible timestamp destinations. Firmware
    // writes them only inside its timestamp aperture, so they stay zero; the
    // completion publishes the pass envelope to userspace instead.
    // AGXTAChannelSKSM::submitBuffer mirrors the same dependency/current pair
    // consumed by the TA RCE program into the firmware command. These fields
    // are deliberately unaligned in the native 0x9c0-byte command layout.
    put_u32(raw, 0x086e, (parameters.lifecycle >> 32) as u32);
    put_u64(raw, 0x08ce, parameters.lifecycle as u32 as u64);
    Ok(())
}

/// Build the TA descriptor used by the proven native cold opening.
///
/// The first-work path applies its pool/lifecycle fields after the common
/// builder has generated the RCE boundaries required by every command.
pub(crate) fn build_cold_opening_ta_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    context_id: u32,
    command_buffer_state_word: u32,
    ta_hardware_buffer_id: u32,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    let mut metadata = RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling);
    metadata.context_id = context_id;
    metadata.command_buffer_state_word = command_buffer_state_word;
    metadata.ta_hardware_buffer_id = ta_hardware_buffer_id;
    build_ta_descriptor(parameters, objects, metadata, raw)?;
    put_u8(raw, 0x93e, 0xd0);
    put_u8(
        raw,
        0x93f,
        if parameters.native_pm_bytes {
            0x87
        } else {
            0x91
        },
    );
    Ok(())
}

fn put_embedded_registers(
    raw: &mut [u8],
    header_offset: usize,
    program_offset: usize,
    registers: &[RegisterWrite],
) {
    let bytes = registers.len() * REGISTER_BYTES;
    put_u32(
        raw,
        header_offset,
        ((bytes as u32) << 16) | registers.len() as u32,
    );
    encode_registers(raw, program_offset, registers);
}

pub(crate) struct G17pFragmentRegisters<'a> {
    pub(crate) main: &'a [RegisterWrite],
    pub(crate) store: &'a [RegisterWrite],
    pub(crate) resume: &'a [RegisterWrite],
    pub(crate) load: &'a [RegisterWrite],
}

pub(crate) fn build_fragment_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    if raw.len() < FRAGMENT_DESCRIPTOR_SIZE {
        return Err(RenderBuildError::BufferTooSmall);
    }
    let registers = build_fragment_registers_for_work_key(
        parameters,
        render_work_key(metadata.context_id, metadata.command_buffer_state_word),
    )?;
    let store = partial_store_registers(parameters);
    let resume = partial_resume_registers(parameters);
    let load = partial_load_registers(parameters);
    build_fragment_descriptor_base(
        parameters,
        objects,
        metadata,
        G17pFragmentRegisters {
            main: &registers,
            store: &store,
            resume: &resume,
            load: &load,
        },
        raw,
    )?;
    apply_g17p_fragment_oracle(raw);
    Ok(())
}

pub(crate) fn build_fragment_descriptor_base(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    programs: G17pFragmentRegisters<'_>,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    if raw.len() < FRAGMENT_DESCRIPTOR_SIZE {
        return Err(RenderBuildError::BufferTooSmall);
    }
    if programs.main.is_empty()
        || programs.main.len() > (0x6a0 - 0xa0) / REGISTER_BYTES
        || programs.store.len() > (0xec0 - 0x7c0) / REGISTER_BYTES
        || programs.resume.len() > (0x15e0 - 0xee0) / REGISTER_BYTES
        || programs.load.len() > (0x1d00 - 0x1600) / REGISTER_BYTES
    {
        return Err(RenderBuildError::BufferTooSmall);
    }
    raw[..FRAGMENT_DESCRIPTOR_SIZE].fill(0);
    let geometry = geometry(parameters)?;
    let register_count = programs.main.len();
    write_common_descriptor(
        RenderDescriptorKind::Fragment,
        objects,
        metadata,
        programs.main,
        raw,
    );

    put_u64(raw, 0x40, parameters.tilemap);
    put_u64(raw, 0x48, parameters.multisample_control);
    // Native copies the render target's sample count through host +0x8c8
    // into every fragment descriptor at +0x50 (25G83: 0x8839d80,
    // 0x8837af8..0x8837b08). It is not first-work metadata. The UAPI has
    // already encoded log2(samples) in UTILE_CONFIG bits [1:0].
    put_u32(raw, 0x50, 1u32 << (parameters.utile_config & 3));
    put_u32(raw, 0x54, geometry.macro_size as u32);
    put_u32(raw, 0x68, parameters.merge_upper_x_bits);
    put_u32(raw, 0x6c, parameters.merge_upper_y_bits);
    put_u64(raw, 0x78, geometry.tiles_x * geometry.tiles_y);

    // RCE boundaries belong to every command, including retained queue kicks.
    // Native emits these from the running register count (25G83 88c0740,
    // 88c07ac, 88c08c8, 88c0970); firmware consumes +80/+82/+84/+88 when
    // selecting an entry in this command's embedded program. The fixed recipe
    // ends with 01731=1, 16020=1, 16020=0, 16068=40000. Boundaries are after
    // 01731, after the first 16020, that same entry, and the complete program.
    // Keeping these only in the cold wrapper left ordinary restages at zero.
    put_u16(raw, 0x80, (register_count - 3) as u16);
    put_u16(raw, 0x82, (register_count - 2) as u16);
    put_u16(raw, 0x84, (register_count - 2) as u16);
    put_u16(raw, 0x88, register_count as u16);

    put_u32(
        raw,
        0x07a8,
        (((register_count * REGISTER_BYTES) as u32) << 16) | register_count as u32,
    );
    put_embedded_registers(raw, 0x0ec8, 0x07c0, programs.store);
    put_embedded_registers(raw, 0x15e8, 0x0ee0, programs.resume);
    put_embedded_registers(raw, 0x1d08, 0x1600, programs.load);

    put_u64(raw, 0x1d20, parameters.depth_bias_array);
    put_u64(raw, 0x1d30, parameters.scissor_array);
    put_u64(raw, 0x1d40, parameters.occlusion_query_base);
    put_u64(raw, 0x1e78, parameters.load_pipeline_bind);
    put_u64(raw, 0x1e80, parameters.load_pipeline);
    put_u64(raw, 0x1ea8, parameters.partial_load_pipeline_bind);
    put_u64(raw, 0x1eb0, parameters.partial_load_pipeline);
    put_u32(raw, 0x1ec0, 0x0404_0404);
    put_u64(raw, 0x1f38, parameters.tib_blocks);
    put_u64(raw, 0x1f40, parameters.aux_fb_flags);
    put_u32(raw, 0x1f48, parameters.width);
    put_u32(raw, 0x1f4c, parameters.height);
    put_u64(raw, 0x1f50, parameters.aux_fb_page_count);
    put_u64(raw, 0x1f58, parameters.tile_config);
    put_u32(raw, 0x1f78, parameters.store_pipeline_bind as u32);
    put_u64(raw, 0x1f7c, parameters.store_pipeline);
    put_u32(raw, 0x1f98, parameters.partial_store_pipeline_bind as u32);
    put_u64(raw, 0x1f9c, parameters.partial_store_pipeline);
    put_u32(raw, 0x1fa8, parameters.depth_clear_value_bits);
    put_u64(raw, 0x1fac, (parameters.tib_blocks << 33) | 0x300);
    // Native copies isMemorylessRender through the UMD render record +0x240,
    // parsed record +0x1b8, and host descriptor +0x8bf into this byte. It is
    // not a TIB-size predicate: native's memoryless policy also selects the
    // shortened SPM phase programs. The admitted render UAPI represents
    // memory-backed resources and has no memoryless-render policy or matching
    // SPM-program construction. Keep that policy clear even for >8 TIB
    // blocks; supporting memoryless renders requires modeling both together.
    // Empty-tile processing remains independent in register 0x10039 bit 16.
    put_u8(raw, 0x2100, 0);
    // Exact G17P tail copied by AGX3DChannelSKSM::submitBuffer. +0x2104 is the
    // accelerator control snapshot and +0x210c its low map nibble. +0x2110 is
    // a separate render-setup field: prepare copies descriptor +0xc00 to setup
    // +0x818, processRenderSetup carries it to +0x89c, and submitBuffer emits
    // it here. Working Metal commands carry the invariant all-ones value.
    put_u32(raw, 0x2104, parameters.gpc_perf_state_control);
    // AGX3DChannelSKSM::submitBuffer mirrors the same dependency/current pair
    // consumed by the fragment RCE program into the firmware command.
    put_u32(raw, 0x2108, (parameters.fragment_lifecycle >> 32) as u32);
    put_u32(raw, 0x210c, parameters.gpc_perf_state_map_low);
    put_u32(raw, 0x2110, G17P_FRAGMENT_TAIL_SETUP_WORD);
    // Native submitBuffer zeros +0x2114..+0x2123. Sampler state reaches the
    // GPU through its real register/program path, not these command-tail
    // lanes. The first two following flag lanes reproduce the working Neo
    // shared-runtime snapshot; the final two come from per-command descriptor
    // bytes and retain their constructor-zero values until those inputs are
    // modeled.
    put_u32(
        raw,
        0x2124,
        G17P_NEO_FRAGMENT_SHARED_RUNTIME_FLAG_SNAPSHOT[0],
    );
    put_u32(
        raw,
        0x2128,
        G17P_NEO_FRAGMENT_SHARED_RUNTIME_FLAG_SNAPSHOT[1],
    );
    put_u64(raw, 0x2168, parameters.fragment_lifecycle as u32 as u64);
    put_u64(raw, 0x2198, parameters.fragment_timestamp_start);
    put_u64(raw, 0x21a0, parameters.fragment_timestamp_end);
    // +0x21a8/+0x21b0: caller-visible destinations, zero as in the TA record.
    put_u8(raw, 0x21cc, 0x53);
    put_u8(raw, 0x21d6, 1);
    put_u8(raw, 0x21e7, parameters.context_generation);
    put_u8(raw, 0x2208, 1);
    put_u8(raw, 0x220c, 1);
    put_u8(raw, 0x222d, g17p_frg_hwdsid_mode());
    Ok(())
}

fn apply_g17p_fragment_oracle(raw: &mut [u8]) {
    // Neo diagnostic overrides stay outside the shared encoder so M5
    // register programs and geometry never inherit these substitutions.
    let oracle = crate::g18::fw::config::FRAG_ORACLE;
    if oracle & 0x01 != 0 {
        put_u64(raw, 0x158, 0x7007);
    }
    if oracle & 0x02 != 0 {
        put_u64(raw, 0x488, 1);
    }
    if oracle & 0x04 != 0 {
        // native 0x8000005300000000 against our 0x0000005300000000
        let mut cur = [0u8; 8];
        cur.copy_from_slice(&raw[0x21c8..0x21d0]);
        put_u64(raw, 0x21c8, u64::from_le_bytes(cur) | (1u64 << 63));
    }
    if oracle & 0x08 != 0 {
        put_u64(raw, 0x78, 0x40);
    }
    if oracle & 0x10 != 0 {
        put_u64(raw, 0x458, 3);
        put_u64(raw, 0x470, 0x500);
    }
    if oracle & 0x20 != 0 {
        put_u64(raw, 0x1f48, 0x0000_0100_0000_0100);
    }
    if oracle & 0x40 != 0 {
        // fields native leaves ZERO and we populate
        put_u64(raw, 0x1d0, 0);
        put_u64(raw, 0x1f0, 0);
        put_u64(raw, 0x1f8, 0);
    }
    if oracle & 0x80 != 0 {
        put_u32(raw, 0x1d8 + 4, 0x3f80_0000);
    }
}

/// FRG +0x222d mode byte: Linux has always written 1; all 160 native light
/// FRG captures hold 0. Diagnostic parameter `g17p_frg_hwdsid_clear`.
#[cfg(not(test))]
fn g17p_frg_hwdsid_mode() -> u8 {
    u8::from(crate::g18::params::g17p_frg_hwdsid_clear.value() == 0
        && !crate::g18::compat::timing_buffer::diag_knob(crate::g18::compat::timing_buffer::DIAG_FRG_222D_CLEAR))
}

#[cfg(test)]
fn g17p_frg_hwdsid_mode() -> u8 { 1 }

/// Build the fragment descriptor used by the proven native cold opening.
pub(crate) fn build_cold_opening_fragment_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    context_id: u32,
    command_buffer_state_word: u32,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    let mut metadata = RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment);
    metadata.context_id = context_id;
    metadata.command_buffer_state_word = command_buffer_state_word;
    build_fragment_descriptor(parameters, objects, metadata, raw)?;
    // +90 is staged with the channel's completion payload at publication.
    // A newly published native 3D queue carries 1 here; retained submissions
    // clear it. This lifecycle is independent of the outer-slot first-submit
    // byte and appears identically on cold QIDs 1 and 8 in both valid traces.
    put_u32(raw, 0x215c, 1);
    Ok(())
}

/// Stage the TA's link to the physical fragment command, not its ring slot.
///
/// Native TA submit copies host +0x1528 as a low-40-bit qword at +0x7a8 and
/// its bits 47:40 as two independent bytes at +0x7b0/+0x7b1 (8a99220..34).
/// Host +0xe60 supplies the full packed qword at +0x8c6 (8a99414..1c).
/// Its producer converts the TA channel timestamp, whereas +0x1528 converts
/// the fragment channel timestamp (8aa9e30..aaa05c). Both carry the 3D channel
/// payload. Preserve those distinct sources even when queue seeds coincide.
/// Refresh from the actual retained producer immediately before publication;
/// the payload counter wraps independently of the physical timestamp.
pub(crate) fn apply_g17p_ta_linked_completion_scratch(
    tiling_timestamp: u64,
    fragment_timestamp: u64,
    payload: u8,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    if tiling_timestamp > 0xff_ffff_ffff || fragment_timestamp > 0xff_ffff_ffff {
        return Err(RenderBuildError::ArithmeticOverflow);
    }
    if raw.len() < TA_DESCRIPTOR_SIZE {
        return Err(RenderBuildError::BufferTooSmall);
    }
    put_u64(raw, 0x07a8, fragment_timestamp);
    put_u8(raw, 0x07b0, payload);
    put_u8(raw, 0x07b1, payload);
    put_u64(raw, 0x08c6, tiling_timestamp | ((payload as u64) << 40));
    Ok(())
}

/// Apply the queue-local fields that change as the canonical three-item group
/// is refreshed in place. `descriptor_gpu_va` is the low VM address firmware
/// uses for the descriptor's self pointers.
pub(crate) fn apply_g17p_retained_descriptor_fields(
    kind: RenderDescriptorKind,
    descriptor_gpu_va: u64,
    runtime_control: u64,
    queue_id: u8,
    linked_fragment_queue_id: u8,
    submission_ordinal: u64,
    cold_opening: bool,
    native_pm_bytes: bool,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    let item_number = submission_ordinal
        .checked_add(1)
        .ok_or(RenderBuildError::ArithmeticOverflow)? & 0xffffffffff;
    // The native queue timestamp is 40 bits. Stamp-info carries its low
    // 24 bits shifted by eight, with ordinary u32 wrap as on M1/M2.
    let event_value = (item_number as u32).wrapping_shl(8);
    match kind {
        RenderDescriptorKind::Tiling => {
            if raw.len() < TA_DESCRIPTOR_SIZE {
                return Err(RenderBuildError::BufferTooSmall);
            }
            put_u64(
                raw,
                0x0760,
                descriptor_gpu_va
                    .checked_add(0x60)
                    .ok_or(RenderBuildError::ArithmeticOverflow)?,
            );
            // Native TA links directly to the physical 3D kick queue which
            // will consume this submission. This is queue allocator state,
            // not the type-1 fragment descriptor's distinct QID-0 RCE field.
            put_u32(raw, 0x079c, u32::from(linked_fragment_queue_id));
            put_u32(raw, 0x07a0, event_value);
            apply_g17p_ta_linked_completion_scratch(
                u64::from(item_number), u64::from(item_number), item_number as u8, raw,
            )?;
            // Native writes a u32 at +0x8b6 (8a993f4). +0x8b4/+0x8b5
            // belong to the preceding stamp pointer, not this event value.
            put_u32(raw, 0x08b6, event_value);
            // B1 indexes the registered scheduler-resource table with this
            // value while admitting the completed TA, then passes the full
            // word to FUN_15424(linked_3d_qid, ta_qid). It is the TA queue's
            // own physical QID, not a captured literal: two independent Metal
            // commands carry 7 here while +0x79c links QID 8, and 9 here while
            // +0x79c links QID 10. Leaving it zero made Linux consult the
            // unrelated QID-0 scheduler state at the TA-to-3D handoff.
            put_u32(
                raw,
                0x08ba,
                if crate::g18::fw::config::RENDER_TA_QID_FIELD != 0 {
                    u32::from(queue_id)
                } else {
                    0
                },
            );
            // Retain the existing Linux TA drain policy while its visibility
            // boundaries are audited. Native +0x8c2 is a pending IO-fence
            // boolean (host descriptor +0x48), not a priority/QID encoding.
            // Firmware ORs it into TSQ node +0x20 and clears it on a drain;
            // this TA flag does not guarantee a later fragment drain.
            // Keep this independent of the opt-in priority diagnostic.
            put_u32(raw, 0x08c2, 1);
            // Native's host +0x1288 source is a u32 at +0x8d6 (8a990b8),
            // immediately after the qword lifecycle field beginning +0x8ce.
            put_u32(raw, 0x08d6, submission_ordinal as u32);
            put_u64(raw, 0x0934, runtime_control);
            if cold_opening {
                put_u8(raw, 0x093e, 0xd0);
                put_u8(raw, 0x093f, if native_pm_bytes { 0x87 } else { 0x91 });
            }
        }
        RenderDescriptorKind::Fragment => {
            if raw.len() < FRAGMENT_DESCRIPTOR_SIZE {
                return Err(RenderBuildError::BufferTooSmall);
            }
            for (at, target) in [
                (0x07a0, 0x00a0),
                (0x0ec0, 0x07c0),
                (0x15e0, 0x0ee0),
                (0x1d00, 0x1600),
            ] {
                put_u64(
                    raw,
                    at,
                    descriptor_gpu_va
                        .checked_add(target)
                        .ok_or(RenderBuildError::ArithmeticOverflow)?,
                );
            }
            put_u32(raw, 0x2150, event_value);
            // Valid native queues prove +0x2154 is the physical 3D QID: it is
            // 1/3/8 for qids 1/3/8, respectively. The old literal only happened
            // to match the first captured qid.
            put_u32(raw, 0x2154, u32::from(queue_id));
            // Preserve the existing first-submission policy pending a
            // visibility-boundary audit. The captured cold/later values do
            // not establish ordinal semantics: native derives this flag
            // from pending IO-fences at host descriptor +0x48 (8837268..74,
            // 8837eb0). The TSQ ownership notes decode the producer.
            put_u32(raw, 0x215c, u32::from(submission_ordinal == 0));
            // +0x2160 is the packed 3D-channel SKSM completion scratch value:
            // the physical queue timestamp in bits [39:0] and the channel's
            // payload byte in bits [47:40]. G17PUserRenderStorage stages it
            // from the retained fragment producer immediately before publish.
            put_u32(raw, 0x2170, submission_ordinal as u32);
            put_u64(raw, 0x21ce, runtime_control);
            // Native HWDSID mask occupies +0x220d..+0x222c inclusive;
            // +0x222d is a separate mode flag. An unaligned u64 at +0x222c
            // reads 0x100 from that following flag, not from HWDSID bit 8.
            // Preserve the template's zero mask; aperture IDs alone do not
            // supply the explicit MDSID-list and accounting contract.
        }
    }
    Ok(())
}

/// Copy the four embedded 3D register-entry arrays into a diagnostic mirror.
///
/// The hardware-visible arrays remain descriptor-local.  Native's addresses at
/// descriptor +0x7a0/+0xec0/+0x15e0/+0x1d00 are the self pointers in each
/// embedded array's +0x700 trailer, not pointers to a second allocation.  This
/// mirror intentionally contains entries only and must never be bound to KSM.
pub(crate) fn stage_g17p_fragment_rce_programs(
    fragment: &mut [u8],
    rce_gpu_va: u64,
    rce: &mut [u8],
) -> Result<(), RenderBuildError> {
    if fragment.len() < FRAGMENT_DESCRIPTOR_SIZE
        || rce.len() < G17P_FRAGMENT_RCE_STORAGE_SIZE
    {
        return Err(RenderBuildError::BufferTooSmall);
    }

    let programs = [
        (0x07a0usize, 0x00a0usize, FRAGMENT_REGISTER_COUNT),
        (0x0ec0usize, 0x07c0usize, PARTIAL_STORE_REGISTER_COUNT),
        (0x15e0usize, 0x0ee0usize, PARTIAL_RESUME_REGISTER_COUNT),
        (0x1d00usize, 0x1600usize, PARTIAL_LOAD_REGISTER_COUNT),
    ];

    rce[..G17P_FRAGMENT_RCE_STORAGE_SIZE].fill(0);
    for (index, (_pointer_offset, source_offset, register_count)) in
        programs.into_iter().enumerate()
    {
        let bytes = register_count
            .checked_mul(REGISTER_BYTES)
            .ok_or(RenderBuildError::ArithmeticOverflow)?;
        let target_offset = index
            .checked_mul(G17P_FRAGMENT_RCE_PROGRAM_STRIDE as usize)
            .ok_or(RenderBuildError::ArithmeticOverflow)?;
        let source_end = source_offset
            .checked_add(bytes)
            .ok_or(RenderBuildError::ArithmeticOverflow)?;
        let target_end = target_offset
            .checked_add(bytes)
            .ok_or(RenderBuildError::ArithmeticOverflow)?;
        rce[target_offset..target_end]
            .copy_from_slice(&fragment[source_offset..source_end]);
    }
    let _ = rce_gpu_va;
    Ok(())
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct AddressedBytes<const N: usize> {
    pub(crate) address: u64,
    pub(crate) bytes: [u8; N],
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct QueuePublicationInput {
    pub(crate) queue: u64,
    pub(crate) pointers: u64,
    pub(crate) item_ring: u64,
    pub(crate) channel_ring: u64,
    pub(crate) channel_producer: u64,
    pub(crate) items: [u64; 3],
    pub(crate) grid_index: u16,
    pub(crate) slot_index: u16,
    pub(crate) current_write: u32,
    pub(crate) group_number: u32,
    pub(crate) first_submit: bool,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct QueuePublication {
    pub(crate) item_entries: [AddressedBytes<8>; 3],
    pub(crate) event: AddressedBytes<EVENT_RECORD_SIZE>,
    pub(crate) queue_write: AddressedBytes<4>,
    pub(crate) ring_slot: AddressedBytes<RING_SLOT_SIZE>,
    pub(crate) producer: AddressedBytes<4>,
    pub(crate) write_after: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PairedQueuePublication {
    pub(crate) tiling: QueuePublication,
    pub(crate) fragment: QueuePublication,
}

pub(crate) fn build_queue_publication(
    kind: RenderDescriptorKind,
    input: QueuePublicationInput,
) -> Result<QueuePublication, RenderBuildError> {
    let write_after = input
        .current_write
        .checked_add(3)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    if input.grid_index > 0xff {
        return Err(RenderBuildError::QueueGridOutOfRange);
    }
    if input.slot_index > 0xff {
        return Err(RenderBuildError::RingSlotOutOfRange);
    }
    if input.group_number > (u32::MAX >> 8) {
        return Err(RenderBuildError::ArithmeticOverflow);
    }

    let item_offset = (input.current_write as u64)
        .checked_mul(8)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    let first_item = input
        .item_ring
        .checked_add(item_offset)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    let item_entries = [
        AddressedBytes {
            address: first_item,
            bytes: input.items[0].to_le_bytes(),
        },
        AddressedBytes {
            address: first_item
                .checked_add(8)
                .ok_or(RenderBuildError::ArithmeticOverflow)?,
            bytes: input.items[1].to_le_bytes(),
        },
        AddressedBytes {
            address: first_item
                .checked_add(16)
                .ok_or(RenderBuildError::ArithmeticOverflow)?,
            bytes: input.items[2].to_le_bytes(),
        },
    ];

    let mut event = [0u8; EVENT_RECORD_SIZE];
    let counter = input.group_number << 8;
    put_u32(&mut event, 0x00, EVENT_SELECTOR);
    put_u32(
        &mut event,
        0x04,
        EVENT_SUBTYPE_BASE | input.grid_index as u32,
    );
    put_u32(&mut event, 0x08, counter);
    put_u32(
        &mut event,
        0x10,
        match kind {
            RenderDescriptorKind::Tiling => 0,
            RenderDescriptorKind::Fragment => 0x100,
        },
    );

    let mut ring_slot = [0u8; RING_SLOT_SIZE];
    put_u64(&mut ring_slot, 0x08, input.queue);
    put_u32(
        &mut ring_slot,
        0x10,
        match kind {
            RenderDescriptorKind::Tiling => 0,
            RenderDescriptorKind::Fragment => 1,
        },
    );
    let flags = (write_after & 0xffff)
        | ((input.grid_index as u32) << 16)
        | if input.first_submit { 1 << 24 } else { 0 };
    put_u32(&mut ring_slot, 0x14, flags);

    let queue_write_address = input
        .pointers
        .checked_add(QUEUE_WRITE_OFFSET)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    let slot_offset = (input.slot_index as u64)
        .checked_mul(RING_SLOT_SIZE as u64)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    let ring_slot_address = input
        .channel_ring
        .checked_add(slot_offset)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    let producer = (input.slot_index as u32 + 1) & 0xff;
    Ok(QueuePublication {
        item_entries,
        event: AddressedBytes {
            address: input.items[2],
            bytes: event,
        },
        queue_write: AddressedBytes {
            address: queue_write_address,
            bytes: write_after.to_le_bytes(),
        },
        ring_slot: AddressedBytes {
            address: ring_slot_address,
            bytes: ring_slot,
        },
        producer: AddressedBytes {
            address: input.channel_producer,
            bytes: producer.to_le_bytes(),
        },
        write_after,
    })
}

pub(crate) fn build_paired_queue_publication(
    tiling: QueuePublicationInput,
    fragment: QueuePublicationInput,
) -> Result<PairedQueuePublication, RenderBuildError> {
    Ok(PairedQueuePublication {
        tiling: build_queue_publication(RenderDescriptorKind::Tiling, tiling)?,
        fragment: build_queue_publication(RenderDescriptorKind::Fragment, fragment)?,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mcache_mapping_matches_native_g17p_3d_records() {
        assert_eq!(
            encode_g17p_mcache_mapping(G17PMcacheMapping {
                address: 0xeeee_0000,
                size_units: 0x4d000,
                hwsid: 0x18,
                selector: 3,
                range_shift: 2,
            })
            .unwrap(),
            [
                0x00, 0xdc, 0xdd, 0x01, 0x00, 0x06, 0x8c, 0x00, 0x00, 0xac, 0xe2, 0x01,
                0x00, 0x12, 0x00, 0x00,
            ]
        );
        assert_eq!(
            encode_g17p_mcache_mapping(G17PMcacheMapping {
                address: 0x100_0e83_8000,
                size_units: 0x26a80,
                hwsid: 0x18,
                selector: 3,
                range_shift: 1,
            })
            .unwrap(),
            [
                0x00, 0x07, 0x1d, 0x00, 0x02, 0x06, 0x8c, 0x00, 0x80, 0x71, 0x1f, 0x00,
                0x02, 0x0a, 0x00, 0x00,
            ]
        );
    }

    fn fnv1a64(parts: &[&[u8]]) -> u64 {
        let mut value = 0xcbf2_9ce4_8422_2325u64;
        for part in parts {
            for byte in *part {
                value ^= *byte as u64;
                value = value.wrapping_mul(0x0000_0100_0000_01b3);
            }
        }
        value
    }

    fn encoded_register_hash(registers: &[RegisterWrite]) -> u64 {
        let mut raw = [0u8; FRAGMENT_REGISTER_COUNT * REGISTER_BYTES];
        encode_registers(&mut raw, 0, registers);
        fnv1a64(&[&raw[..registers.len() * REGISTER_BYTES]])
    }

    fn get_u32(raw: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(raw[offset..offset + 4].try_into().unwrap())
    }

    fn get_u64(raw: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(raw[offset..offset + 8].try_into().unwrap())
    }

    fn objects() -> RenderDescriptorObjects {
        RenderDescriptorObjects {
            record_a: 0xffff_fc20_0150_0000,
            shared: 0xffff_fc20_0160_0000,
            record_b: 0xffff_fc20_0158_0000,
            zero: 0xffff_fc20_0168_0000,
        }
    }

    #[test]
    fn source_profile_pins_rsrc8_and_native_aligned_programs() {
        let parameters = G17pRenderParameters::source_rsrc8();
        assert_eq!(parameters.store_pipeline_bind, 0x8);
        assert_eq!(parameters.load_pipeline_bind, 0x0007_8000_0000_0040);
        assert_eq!(parameters.store_pipeline, 0x0000_0100_0199_0640);
        assert_eq!(parameters.load_pipeline, 0x0000_0100_0199_0240);
    }

    #[test]
    fn render_lifecycles_use_native_gid_predecessor_and_consecutive_currents() {
        // AtomicU32(1): reserve-group returns predecessor 1 and advances the
        // counter to 0x01000001. Native OSAddAtomic64 returns the old value,
        // hence 3D current 0x01000001 and TA current 0x01000002.
        assert_eq!(
            g17p_render_lifecycle_pair(1, 0x0100_0001),
            Some((0x0000_0001_0100_0001, 0x0000_0001_0100_0002))
        );
        // Exact first render pair from the working Metal capture.
        assert_eq!(
            g17p_render_lifecycle_pair(0x0700_c271, 0x0800_c294),
            Some((0x0700_c271_0800_c294, 0x0700_c271_0800_c295))
        );
        assert_eq!(g17p_render_lifecycle_pair(0, 1), None);
        assert_eq!(g17p_render_lifecycle_pair(1, u32::MAX), None);

        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.lifecycle = 0x1111_2222_3333_4444;
        parameters.fragment_lifecycle = 0x5555_6666_7777_8888;
        let ta = build_ta_registers(&parameters).unwrap();
        let fragment = build_fragment_registers(&parameters).unwrap();
        assert!(ta.iter().filter(|write| write.value == parameters.lifecycle).count() >= 3);
        assert_eq!(
            fragment
                .iter()
                .filter(|write| write.value == parameters.fragment_lifecycle)
                .count(),
            3
        );
        let mut ta_descriptor = [0u8; TA_DESCRIPTOR_SIZE];
        build_ta_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling),
            &mut ta_descriptor,
        )
        .unwrap();
        assert_eq!(get_u32(&ta_descriptor, 0x086e), 0x1111_2222);
        assert_eq!(get_u64(&ta_descriptor, 0x08ce), 0x3333_4444);

        let mut descriptor = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        build_fragment_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
            &mut descriptor,
        )
        .unwrap();
        assert_eq!(get_u32(&descriptor, 0x2108), 0x5555_6666);
        assert_eq!(get_u64(&descriptor, 0x2168), 0x7777_8888);
    }

    #[test]
    fn vdm_user_offset_matches_g17p_compact_window() {
        assert_eq!(vdm_user_offset(0x10_0186_0420).unwrap(), 0x0186_0420);
        assert_eq!(
            vdm_user_offset(0x4f_fff5_4400).unwrap(),
            0x3f_fff5_4400
        );
        assert_eq!(
            vdm_user_offset(0xbf_fff5_4400),
            Err(RenderBuildError::VdmAddressOutOfRange)
        );
    }

    #[test]
    fn native_ta_addresses_use_split_resource_and_user_domains() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.context_base = 0x70_0000_0000;
        parameters.tilemap = 0x70_011e_0a00;
        parameters.layermeta = 0x70_011e_2a00;
        parameters.heapmeta = 0x70_011e_2a00;
        parameters.tpc = 0x70_0129_0000;
        parameters.deflake_3 = 0x10_0009_ca60;
        parameters.deflake_2 = 0x10_0009_ca80;
        parameters.deflake_1 = 0x10_0009_cd00;

        let registers = build_ta_registers(&parameters).unwrap();
        let value = |number| {
            registers
                .iter()
                .find(|write| write.number == number)
                .unwrap()
                .value
        };
        assert_eq!(value(0x1c039), 0x011e_0a00);
        assert_eq!(value(0x1c031), 0x8000_0000_011e_2a00);
        assert_eq!(value(0x1c0a1), 0x0129_0000);
        assert_eq!(value(0x10111), 0x0009_cd00);
        assert_eq!(value(0x10119), 0x0009_ca80);
        assert_eq!(value(0x1c950), 0x0004_0000_0009_ca60);

        let mut descriptor = [0u8; TA_DESCRIPTOR_SIZE];
        build_ta_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata {
                context_id: 1,
                submission_ordinal: 0,
                command_buffer_state_word: 0,
                ta_hardware_buffer_id: 0,
                submit_sequence: 1,
            },
            &mut descriptor,
        )
        .unwrap();
        assert_eq!(get_u32(&descriptor, 0x07d6), 0x0009_cd00);
        assert_eq!(get_u64(&descriptor, 0x0780), parameters.tpc);

        parameters.deflake_1 = 0x11_0000_0000;
        assert_eq!(
            build_ta_registers(&parameters),
            Err(RenderBuildError::UserAddressOutOfRange)
        );
    }

    #[test]
    fn register_programs_match_m1n1_9165484() {
        let parameters = G17pRenderParameters::source_rsrc8();
        let tiling = build_ta_registers(&parameters).unwrap();
        let fragment = build_fragment_registers(&parameters).unwrap();
        assert_eq!(tiling.len(), 73);
        assert_eq!(fragment.len(), 89);
        assert_eq!(encoded_register_hash(&tiling), 0x29dc_2725_a3c2_90c9);
        assert_eq!(encoded_register_hash(&fragment), 0xdb21_2bfe_1bbf_85c8);
        assert_eq!(fragment[2], RegisterWrite::new(0x15379, 0x8));
        assert_eq!(
            fragment[3],
            RegisterWrite::new(0x15381, G17P_SOURCE_EOT_PIPELINE)
        );
        assert_eq!(
            fragment[4],
            RegisterWrite::new(0x15369, G17P_LOAD_RESOURCE_SPEC)
        );
        assert_eq!(
            fragment[5],
            RegisterWrite::new(0x15371, G17P_SOURCE_BG_PIPELINE)
        );
    }

    #[test]
    fn reused_cpu_staging_produces_identical_wire_descriptors() {
        let parameters = G17pRenderParameters::source_rsrc8();
        for dirty in [0x00, 0x55, 0xff] {
            let mut staging = [dirty; 0x8000];
            staging.fill(0);
            let mut tiling = [0; TA_DESCRIPTOR_SIZE];
            let mut fragment = [0; FRAGMENT_DESCRIPTOR_SIZE];
            let ta_metadata = RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling);
            let fragment_metadata = RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment);
            build_ta_descriptor(&parameters, objects(), ta_metadata, &mut tiling).unwrap();
            build_fragment_descriptor(&parameters, objects(), fragment_metadata, &mut fragment).unwrap();
            let (ta, fr) = staging.split_at_mut(0x4000);
            build_ta_descriptor(&parameters, objects(), ta_metadata, ta).unwrap();
            build_fragment_descriptor(&parameters, objects(), fragment_metadata, fr).unwrap();
            assert_eq!(&ta[..TA_DESCRIPTOR_SIZE], &tiling);
            assert_eq!(&fr[..FRAGMENT_DESCRIPTOR_SIZE], &fragment);
            assert!(ta[TA_DESCRIPTOR_SIZE..].iter().all(|v| *v == 0));
            assert!(fr[FRAGMENT_DESCRIPTOR_SIZE..].iter().all(|v| *v == 0));
        }
    }

    #[test]
    fn descriptors_match_m1n1_9165484_and_duplicate_pipeline_fields() {
        let parameters = G17pRenderParameters::source_rsrc8();
        let mut tiling = [0u8; TA_DESCRIPTOR_SIZE];
        let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        build_ta_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling),
            &mut tiling,
        )
        .unwrap();
        build_fragment_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
            &mut fragment,
        )
        .unwrap();

        assert_eq!(fnv1a64(&[&tiling]), 0x8902_c437_9d74_1cc7);
        assert_eq!(fnv1a64(&[&fragment]), 0xdd6e_bcc7_77e7_834d);
        assert_eq!(get_u32(&fragment, 0xb8), 0x15379);
        assert_eq!(get_u64(&fragment, 0xbc), 0x8);
        assert_eq!(get_u64(&fragment, 0xc8), G17P_SOURCE_EOT_PIPELINE);
        assert_eq!(get_u64(&fragment, 0x1e78), G17P_LOAD_RESOURCE_SPEC);
        assert_eq!(get_u64(&fragment, 0x1e80), G17P_SOURCE_BG_PIPELINE);
        assert_eq!(get_u32(&fragment, 0x1f78), 0x8);
        assert_eq!(get_u64(&fragment, 0x1f7c), G17P_SOURCE_EOT_PIPELINE);
    }

    #[test]
    fn pool_a_record_one_changes_only_the_two_stage_pointer_fields() {
        let parameters = G17pRenderParameters::source_rsrc8();
        let baseline_objects = objects();
        let record_one_objects = baseline_objects.with_pool_a_record_index(1).unwrap();
        let mut baseline_ta = [0u8; TA_DESCRIPTOR_SIZE];
        let mut record_one_ta = [0u8; TA_DESCRIPTOR_SIZE];
        let mut baseline_3d = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        let mut record_one_3d = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        build_ta_descriptor(
            &parameters,
            baseline_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling),
            &mut baseline_ta,
        )
        .unwrap();
        build_ta_descriptor(
            &parameters,
            record_one_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling),
            &mut record_one_ta,
        )
        .unwrap();
        build_fragment_descriptor(
            &parameters,
            baseline_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
            &mut baseline_3d,
        )
        .unwrap();
        build_fragment_descriptor(
            &parameters,
            record_one_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
            &mut record_one_3d,
        )
        .unwrap();

        assert_eq!(record_one_objects.record_a, baseline_objects.record_a + 0x100);
        assert_eq!(get_u64(&baseline_ta, 0x10), baseline_objects.record_a);
        assert_eq!(get_u64(&record_one_ta, 0x10), record_one_objects.record_a);
        assert_eq!(get_u64(&baseline_3d, 0x20), baseline_objects.record_a);
        assert_eq!(get_u64(&record_one_3d, 0x20), record_one_objects.record_a);
        for (offset, (before, after)) in baseline_ta.iter().zip(record_one_ta.iter()).enumerate() {
            if !(0x10..0x18).contains(&offset) {
                assert_eq!(before, after, "unexpected TA descriptor change at {offset:#x}");
            }
        }
        for (offset, (before, after)) in baseline_3d.iter().zip(record_one_3d.iter()).enumerate() {
            if !(0x20..0x28).contains(&offset) {
                assert_eq!(before, after, "unexpected 3D descriptor change at {offset:#x}");
            }
        }
    }

    #[test]
    fn pool_a_record_index_checks_overflow() {
        let objects = RenderDescriptorObjects {
            record_a: u64::MAX - 0xff,
            ..objects()
        };
        assert_eq!(
            objects.with_pool_a_record_index(1),
            Err(RenderBuildError::ArithmeticOverflow)
        );
    }

    #[test]
    fn fragment_hwpb_split_changes_only_fragment_plus_0x28() {
        let parameters = G17pRenderParameters::source_rsrc8();
        let baseline_objects = objects();
        let hwpb = 0xffff_fc20_c099_8000;
        let fragment_objects = baseline_objects.with_shared(hwpb);
        let mut baseline_ta = [0u8; TA_DESCRIPTOR_SIZE];
        let mut split_ta = [0u8; TA_DESCRIPTOR_SIZE];
        let mut baseline_3d = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        let mut split_3d = [0u8; FRAGMENT_DESCRIPTOR_SIZE];

        build_ta_descriptor(
            &parameters,
            baseline_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling),
            &mut baseline_ta,
        )
        .unwrap();
        build_ta_descriptor(
            &parameters,
            baseline_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling),
            &mut split_ta,
        )
        .unwrap();
        build_fragment_descriptor(
            &parameters,
            baseline_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
            &mut baseline_3d,
        )
        .unwrap();
        build_fragment_descriptor(
            &parameters,
            fragment_objects,
            RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
            &mut split_3d,
        )
        .unwrap();

        assert_eq!(baseline_ta, split_ta);
        assert_eq!(get_u64(&baseline_3d, 0x20), baseline_objects.record_a);
        assert_eq!(get_u64(&split_3d, 0x20), baseline_objects.record_a);
        assert_eq!(get_u64(&baseline_3d, 0x28), baseline_objects.shared);
        assert_eq!(get_u64(&split_3d, 0x28), hwpb);
        assert_eq!(get_u64(&baseline_3d, 0x30), baseline_objects.record_b);
        assert_eq!(get_u64(&split_3d, 0x30), baseline_objects.record_b);
        assert_eq!(get_u64(&baseline_3d, 0x38), baseline_objects.zero);
        assert_eq!(get_u64(&split_3d, 0x38), baseline_objects.zero);
        for (offset, (before, after)) in baseline_3d.iter().zip(split_3d.iter()).enumerate() {
            if !(0x28..0x30).contains(&offset) {
                assert_eq!(
                    before, after,
                    "unexpected 3D descriptor change at {offset:#x}"
                );
            }
        }
    }

    #[test]
    fn memory_backed_render_policy_is_independent_of_tib_size() {
        for tib_blocks in [4, 8, 9, 12, 16] {
            for process_empty_tiles in [false, true] {
                let mut parameters = G17pRenderParameters::source_rsrc8();
                parameters.tib_blocks = tib_blocks;
                parameters.process_empty_tiles = process_empty_tiles;
                parameters.tile_config = 0x280
                    | if process_empty_tiles { 0x1_0000 } else { 0 };
                let mut fragment = [0xa5u8; FRAGMENT_DESCRIPTOR_SIZE];
                build_cold_opening_fragment_descriptor(
                    &parameters, objects(), 1, 1, &mut fragment,
                )
                .unwrap();
                assert_eq!(fragment[0x2100], 0);
                assert_eq!(get_u64(&fragment, 0x1f38), tib_blocks);
                assert_eq!(get_u64(&fragment, 0x1f58), parameters.tile_config);
                // Keep real memory-backed partial phase programs, not the
                // native memoryless count-two reset-only phase variant.
                for (at, count) in [(0xec8, 16), (0x15e8, 23), (0x1d08, 10)] {
                    assert_eq!(get_u32(&fragment, at) & 0xffff, count);
                }
                apply_g17p_retained_descriptor_fields(
                    RenderDescriptorKind::Fragment, 0x7000098000, 0,
                    1, 1, 1, false, false, &mut fragment,
                )
                .unwrap();
                assert_eq!(fragment[0x2100], 0);
            }
        }
    }

    #[test]
    fn fragment_sample_count_matches_tile_configuration_on_every_submission() {
        for (sample_log2, samples) in [(0, 1), (1, 2), (2, 4)] {
            let mut parameters = G17pRenderParameters::source_rsrc8();
            parameters.utile_config = 0xa000 | sample_log2;
            let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];

            build_fragment_descriptor(
                &parameters,
                objects(),
                RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
                &mut fragment,
            )
            .unwrap();
            assert_eq!(get_u32(&fragment, 0x50), samples, "retained {samples}x");
            assert_eq!(get_u32(&fragment, 0xac), 0x10009);
            assert_eq!(get_u64(&fragment, 0xb0) & 3, sample_log2);

            build_cold_opening_fragment_descriptor(
                &parameters, objects(), 1, 1, &mut fragment,
            )
            .unwrap();
            assert_eq!(get_u32(&fragment, 0x50), samples, "first {samples}x");
            assert_eq!(get_u64(&fragment, 0xb0) & 3, sample_log2);
        }
    }

    #[test]
    fn partial_render_uses_phase_specific_zls_control() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.depth_buffer = 0x5be8_2a00_00;
        parameters.stencil_buffer = 0x5bea_4240_00;
        parameters.depth_flags = 0x000c_0000;
        parameters.partial_depth_flags = 0x000c_c000;

        let main = build_fragment_registers(&parameters).unwrap();
        assert_eq!(
            main.iter().find(|reg| reg.number == 0x15319).unwrap().value,
            0x000c_0000,
        );
        let store = partial_store_registers(&parameters);
        let resume = partial_resume_registers(&parameters);
        let load = partial_load_registers(&parameters);
        for registers in [store.as_slice(), resume.as_slice(), load.as_slice()] {
            assert_eq!(
                registers.iter().find(|reg| reg.number == 0x15319).unwrap().value,
                0x000c_c000,
            );
        }
    }

    #[test]
    fn cold_opening_descriptors_apply_only_the_native_first_work_overrides() {
        let parameters = G17pRenderParameters::source_rsrc8();
        let mut tiling = [0u8; TA_DESCRIPTOR_SIZE];
        let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        build_cold_opening_ta_descriptor(&parameters, objects(), 1, 0, 0, &mut tiling).unwrap();
        build_cold_opening_fragment_descriptor(&parameters, objects(), 1, 1, &mut fragment)
            .unwrap();

        assert_eq!(&tiling[0x38..0x3e], &[0x47, 0, 0x49, 0, 0x49, 0]);
        assert_eq!(tiling[0x789], 0x08);
        assert_eq!(get_u32(&tiling, 0x892), 1);
        assert_eq!(&tiling[0x93e..0x940], &[0xd0, 0x91]);

        assert_eq!(get_u32(&fragment, 0x50), 1);
        assert_eq!(&fragment[0x80..0x86], &[0x56, 0, 0x57, 0, 0x57, 0]);
        assert_eq!(&fragment[0x88..0x8a], &[0x59, 0]);
        assert_eq!(get_u32(&fragment, 0x90), 0); // final channel payload is staged later
        assert_eq!(get_u32(&fragment, 0x2154), 1);
        assert_eq!(get_u32(&fragment, 0x215c), 1);
        assert_eq!(fragment[0x2100], 0);
        assert_eq!(&fragment[0x2114..0x2134], &[0; 0x20]);
        assert_eq!(get_u64(&fragment, 0x21d7), 0);
        assert_eq!(fragment[0x222d], 1);

    }

    #[test]
    fn cold_opening_ta_couples_the_hardware_buffer_id_without_clobbering_registers() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.ta_hardware_buffer_id = 5;
        let mut tiling = [0u8; TA_DESCRIPTOR_SIZE];
        build_cold_opening_ta_descriptor(
            &parameters,
            objects(),
            1,
            0,
            parameters.ta_hardware_buffer_id,
            &mut tiling,
        )
        .unwrap();

        assert_eq!(get_u32(&tiling, 0x18), 5);
        // +0x300 is register 0x1c830 and +0x304 is its value word.
        assert_eq!(get_u32(&tiling, 0x300), 0x1c830);
        assert_eq!(get_u64(&tiling, 0x304), 5);
    }

    #[test]
    fn shared_usc_flist_id_drives_both_stage_registers_independently_of_pm_id() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.ta_hardware_buffer_id = 7;
        parameters.usc_flist_hardware_buffer_id = 5;
        // Every diagnostic bit set must not recouple the retired bit-16
        // capture literals to these source-owned registers.
        parameters.native_ta_registers = u32::MAX;

        let tiling = build_ta_registers(&parameters).unwrap();
        let fragment = build_fragment_registers(&parameters).unwrap();
        assert_eq!(
            tiling.iter().find(|write| write.number == 0x1c830).unwrap().value,
            7
        );
        assert_eq!(
            tiling.iter().find(|write| write.number == 0x14308).unwrap().value,
            5
        );
        assert_eq!(
            fragment.iter().find(|write| write.number == 0x14048).unwrap().value,
            5
        );
        assert_eq!(
            fragment.iter().find(|write| write.number == 0x1c838).unwrap().value,
            7
        );
    }

    #[test]
    fn descriptor_context_and_state_word_drive_every_native_work_key() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.ta_hardware_buffer_id = 5;
        parameters.gpc_perf_state_map = 7;
        let mut tiling = [0u8; TA_DESCRIPTOR_SIZE];
        let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        let tiling_metadata = RenderDescriptorMetadata {
            context_id: 9,
            submission_ordinal: 4,
            command_buffer_state_word: 0x40,
            ta_hardware_buffer_id: parameters.ta_hardware_buffer_id,
            submit_sequence: 7,
        };
        let fragment_metadata = RenderDescriptorMetadata {
            command_buffer_state_word: 0x41,
            ..tiling_metadata
        };

        build_ta_descriptor(&parameters, objects(), tiling_metadata, &mut tiling).unwrap();
        build_fragment_descriptor(&parameters, objects(), fragment_metadata, &mut fragment)
            .unwrap();

        for (raw, offset, register, value) in [
            (&tiling[..], 0x300, 0x1c830, 5u64),
            (&tiling[..], 0x36c, 0x10209, 0x900),
            (&tiling[..], 0x378, 0x1c9f0, 0x900),
            (&tiling[..], 0x384, 0x14320, 0x900),
            (&fragment[..], 0x454, 0x1c838, 5u64),
            (&fragment[..], 0x46c, 0x10211, 0x901),
            (&fragment[..], 0x478, 0x10420, 0x901),
        ] {
            assert_eq!(get_u32(raw, offset), register);
            assert_eq!(get_u64(raw, offset + 4), value);
        }
        assert_eq!(get_u32(&tiling, 0x0c), 9);
        assert_eq!(get_u32(&fragment, 0x0c), 9);
    }

    #[test]
    fn tpc_extent_survives_cold_and_retained_descriptor_construction() {
        for size in [0x800, 0x2000, 0x20000, 0x1000000] {
            let mut p = G17pRenderParameters::source_rsrc8();
            p.tpc_size = size;
            let mut ta = [0xa5; TA_DESCRIPTOR_SIZE];
            build_cold_opening_ta_descriptor(&p, objects(), 7, 0, 0, &mut ta).unwrap();
            for cold in [true, false] {
                if !cold {
                    build_ta_descriptor(&p, objects(),
                        RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling), &mut ta).unwrap();
                }
                apply_g17p_retained_descriptor_fields(RenderDescriptorKind::Tiling,
                    0x7000090000, 0xfffffc20c0828000, 11, 12, 32,
                    cold, true, &mut ta).unwrap();
                assert_eq!(get_u64(&ta, 0x780), p.tpc);
                assert_eq!(get_u64(&ta, 0x788), size);
            }
        }
    }

    #[test]
    fn rce_boundaries_survive_retained_descriptor_rebuilds() {
        let u16_at = |raw: &[u8], at: usize| u16::from_le_bytes([raw[at], raw[at + 1]]);
        for dimension in [32, 512, 1024] {
            let mut p = G17pRenderParameters::source_rsrc8();
            p.width = dimension;
            p.height = dimension;
            let mut raw = [0xa5; FRAGMENT_DESCRIPTOR_SIZE];
            let mut ta = [0xa5; TA_DESCRIPTOR_SIZE];
            for ordinal in [0, 1, 31, 32, 97, 232, 513] {
                if ordinal == 0 {
                    build_cold_opening_fragment_descriptor(&p, objects(), 7, 1, &mut raw).unwrap();
                    build_cold_opening_ta_descriptor(&p, objects(), 7, 0, 0, &mut ta).unwrap();
                } else {
                    build_fragment_descriptor(&p, objects(), RenderDescriptorMetadata {
                        context_id: 7, submission_ordinal: ordinal,
                        ..RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment)
                    }, &mut raw).unwrap();
                    build_ta_descriptor(&p, objects(), RenderDescriptorMetadata {
                        context_id: 7, submission_ordinal: ordinal,
                        ..RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling)
                    }, &mut ta).unwrap();
                }
                apply_g17p_retained_descriptor_fields(RenderDescriptorKind::Fragment,
                    0x7000098000, 0xfffffc20c0828000, 12, 12, ordinal,
                    ordinal == 0, true, &mut raw).unwrap();
                apply_g17p_retained_descriptor_fields(RenderDescriptorKind::Tiling,
                    0x7000090000, 0xfffffc20c0828000, 11, 12, ordinal,
                    ordinal == 0, true, &mut ta).unwrap();
                assert_eq!([u16_at(&ta, 0x38), u16_at(&ta, 0x3a),
                    u16_at(&ta, 0x3c)], [71, 73, 73]);
                assert_eq!(get_u32(&ta, 0x768) & 0xffff, 73);
                for (index, register) in [(70, 0x01740), (71, 0x1c880), (72, 0x1c898)] {
                    assert_eq!(get_u32(&ta, 0x60 + index * 12), register);
                }
                // Independent native publication oracle: all 160 full FRG
                // images carry these boundaries, including pending overlap.
                assert_eq!([u16_at(&raw, 0x80), u16_at(&raw, 0x82),
                    u16_at(&raw, 0x84), u16_at(&raw, 0x88)], [86, 87, 87, 89]);
                assert_eq!(get_u32(&raw, 0x7a8) & 0xffff, 89);
                // Check which actual encoded instructions bracket the indices,
                // so a changed recipe cannot silently retain stale boundaries.
                for (index, register, value) in [(85, 0x01731, 1),
                    (86, 0x16020, 1), (87, 0x16020, 0), (88, 0x16068, 0x40000)] {
                    let at = 0xa0 + index * 12;
                    assert_eq!(get_u32(&raw, at), register);
                    assert_eq!(get_u64(&raw, at + 4), value);
                }
            }
        }
    }

    #[test]
    fn allocation_epoch_survives_cold_and_retained_encoders_and_wrap() {
        for epoch in [0, 1, 2, 3, 127, 255] {
            for diagnostic in [false, true] {
                let mut parameters = G17pRenderParameters::source_rsrc8();
                parameters.context_generation = epoch;
                parameters.native_pm_bytes = diagnostic;
                for cold in [false, true] {
                    let mut ta = [0u8; TA_DESCRIPTOR_SIZE];
                    let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
                    if cold {
                        build_cold_opening_ta_descriptor(&parameters, objects(), 7, 0, 0, &mut ta).unwrap();
                        build_cold_opening_fragment_descriptor(&parameters, objects(), 7, 1, &mut fragment).unwrap();
                    } else {
                        build_ta_descriptor(&parameters, objects(), RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling), &mut ta).unwrap();
                        build_fragment_descriptor(&parameters, objects(), RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment), &mut fragment).unwrap();
                    }
                    assert_eq!(ta[0x94d], epoch);
                    assert_eq!(fragment[0x21e7], epoch);
                    // Retained record restaging changes submission identity,
                    // but must preserve the immutable execution-context epoch.
                    apply_g17p_retained_descriptor_fields(RenderDescriptorKind::Tiling,
                        0x7000098000, 0xfffffc20c0828000, 0, 1, 513, false, true, &mut ta).unwrap();
                    apply_g17p_retained_descriptor_fields(RenderDescriptorKind::Fragment,
                        0x7000098000, 0xfffffc20c0828000, 1, 1, 513, false, true, &mut fragment).unwrap();
                    assert_eq!(ta[0x94d], epoch);
                    assert_eq!(fragment[0x21e7], epoch);
                }
            }
        }
    }

    #[test]
    fn completion_generation_matches_render_tags_across_reuse_and_wrap() {
        let parameters = G17pRenderParameters::source_rsrc8();
        // Include a second client's cold submission and generations beyond the
        // six-bit hardware tag wrap. The slot ordinal is intentionally unrelated.
        for generation in [0, 1, 2, 62, 64, 65, 128, 0x12345678, u32::MAX - 1] {
            for context_id in [1, 7] {
                let metadata = RenderDescriptorMetadata {
                    context_id,
                    submission_ordinal: 2,
                    command_buffer_state_word: generation,
                    ..RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment)
                };
                let mut ta = [0u8; TA_DESCRIPTOR_SIZE];
                let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
                let mut event_control = [0xa5; 0x100];
                build_ta_descriptor(&parameters, objects(), metadata, &mut ta).unwrap();
                build_fragment_descriptor(&parameters, objects(), metadata, &mut fragment)
                    .unwrap();
                set_render_event_control_generation(&mut event_control, generation).unwrap();
                assert_eq!(get_u32(&event_control, 8), generation);
                assert_eq!(&event_control[..8], &[0xa5; 8]);
                assert!(event_control[12..].iter().all(|&v| v == 0xa5));
                let maintenance_key = ((context_id as u64) << 8)
                    | (get_u32(&event_control, 8) & 63) as u64;
                for (raw, offset) in [(&ta[..], 0x36c), (&ta[..], 0x378),
                                      (&ta[..], 0x384), (&fragment[..], 0x46c),
                                      (&fragment[..], 0x478)] {
                    assert_eq!(get_u64(raw, offset + 4), maintenance_key);
                }
            }
        }
        assert_eq!(
            set_render_event_control_generation(&mut [0; 11], 0),
            Err(RenderBuildError::BufferTooSmall)
        );
    }

    #[test]
    fn fragment_parameter_buffer_owner_is_independent_of_perf_map_and_tail() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.native_ta_registers = u32::MAX;

        for (control, low, map, id) in [(0, 7, 3, 0), (1, 3, 7, 5), (1, 0, 1, 3)] {
            parameters.ta_hardware_buffer_id = id;
            parameters.gpc_perf_state_control = control;
            parameters.gpc_perf_state_map_low = low;
            parameters.gpc_perf_state_map = map;
            let fragment = build_fragment_registers(&parameters).unwrap();
            let write = fragment
                .iter()
                .find(|write| write.number == 0x1c838)
                .unwrap();
            assert_eq!(write.value, id as u64);

            let mut descriptor = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
            build_fragment_descriptor(
                &parameters,
                objects(),
                RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment),
                &mut descriptor,
            )
            .unwrap();
            assert_eq!(get_u32(&descriptor, 0x2104), control);
            assert_eq!(get_u32(&descriptor, 0x210c), low);
            assert_eq!(get_u32(&descriptor, 0x2110), u32::MAX);
            assert_eq!(get_u32(&descriptor, 0x2124), 1);
            assert_eq!(get_u32(&descriptor, 0x2128), 1);
            assert_eq!(get_u32(&descriptor, 0x212c), 0);
            assert_eq!(get_u32(&descriptor, 0x2130), 0);
        }
    }

    #[test]
    fn priority_zero_ta_force_stamp_is_independent_of_physical_qid() {
        for (queue_id, ordinal, cold, expected) in [
            (0, 0, true, 1),
            (0, 1, false, 1),
            (0, 2, false, 1),
            (2, 0, true, 1),
            (2, 1, false, 1),
            (5, 0, true, 1),
            (5, 1, false, 1),
        ] {
            let mut tiling = [0u8; TA_DESCRIPTOR_SIZE];
            apply_g17p_retained_descriptor_fields(
                RenderDescriptorKind::Tiling,
                0x70_0001_8000,
                0xffff_fc20_c082_8000,
                queue_id,
                queue_id + 1,
                ordinal,
                cold,
                true,
                &mut tiling,
            )
            .unwrap();
            assert_eq!(get_u32(&tiling, 0x8c2), expected);
            assert_eq!(get_u32(&tiling, 0x8be), 0);
            assert_eq!(
                get_u64(&tiling, 0x8c6),
                ((ordinal + 1) as u64) << 40 | (ordinal + 1) as u64
            );
        }
    }

    #[test]
    fn retained_descriptor_refresh_stages_native_external_rce_and_advances_item_two() {
        let mut parameters = G17pRenderParameters::source_rsrc8();
        parameters.lifecycle = 0x1122_3344_5566_7788;
        parameters.fragment_lifecycle = 0x99aa_bbcc_ddee_ff00;
        let mut tiling = [0u8; TA_DESCRIPTOR_SIZE];
        let mut fragment = [0u8; FRAGMENT_DESCRIPTOR_SIZE];
        let tiling_va = 0x70_0001_8000;
        let fragment_va = 0x70_0009_8000;
        build_ta_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata {
                context_id: 1,
                submission_ordinal: 1,
                command_buffer_state_word: 2,
                ta_hardware_buffer_id: 0,
                submit_sequence: 3,
            },
            &mut tiling,
        )
        .unwrap();
        build_fragment_descriptor(
            &parameters,
            objects(),
            RenderDescriptorMetadata {
                context_id: 1,
                submission_ordinal: 1,
                command_buffer_state_word: 3,
                ta_hardware_buffer_id: 0,
                submit_sequence: 2,
            },
            &mut fragment,
        )
        .unwrap();
        apply_g17p_retained_descriptor_fields(
            RenderDescriptorKind::Tiling,
            tiling_va,
            0xffff_fc20_c082_8000,
            5,
            6,
            1,
            false,
            false,
            &mut tiling,
        )
        .unwrap();
        apply_g17p_retained_descriptor_fields(
            RenderDescriptorKind::Fragment,
            fragment_va,
            0xffff_fc20_c082_8000,
            6,
            6,
            1,
            false,
            false,
            &mut fragment,
        )
        .unwrap();
        let rce_va = 0x70_0028_cf20;
        let mut rce = [0u8; G17P_FRAGMENT_RCE_STORAGE_SIZE];
        stage_g17p_fragment_rce_programs(&mut fragment, rce_va, &mut rce).unwrap();

        assert_eq!(get_u64(&tiling, 0x760), tiling_va + 0x60);
        assert_eq!(get_u32(&tiling, 0x79c), 6);
        assert_eq!(get_u32(&tiling, 0x8ba), 5);
        assert_eq!(get_u32(&tiling, 0x7a0), 0x200);
        assert_eq!(get_u32(&tiling, 0x7a8), 2);
        assert_eq!(get_u32(&tiling, 0x8d4), 0x1_0000);
        assert_eq!(get_u64(&tiling, 0x934), 0xffff_fc20_c082_8000);
        assert_eq!(get_u64(&fragment, 0x7a0), fragment_va + 0xa0);
        assert_eq!(get_u64(&fragment, 0xec0), fragment_va + 0x7c0);
        assert_eq!(get_u64(&fragment, 0x15e0), fragment_va + 0xee0);
        assert_eq!(get_u64(&fragment, 0x1d00), fragment_va + 0x1600);
        assert_eq!(
            &rce[..FRAGMENT_REGISTER_COUNT * REGISTER_BYTES],
            &fragment[0xa0..0xa0 + FRAGMENT_REGISTER_COUNT * REGISTER_BYTES],
        );
        assert_eq!(get_u32(&tiling, 0x086e), 0x1122_3344);
        assert_eq!(get_u64(&tiling, 0x08ce), 0x5566_7788);
        assert_eq!(get_u32(&fragment, 0x2108), 0x99aa_bbcc);
        assert_eq!(get_u64(&fragment, 0x2168), 0xddee_ff00);
        assert_eq!(
            &rce[0x720..0x720 + PARTIAL_STORE_REGISTER_COUNT * REGISTER_BYTES],
            &fragment[0x7c0..0x7c0 + PARTIAL_STORE_REGISTER_COUNT * REGISTER_BYTES],
        );
        assert_eq!(get_u32(&fragment, 0x2154), 6);
        assert_eq!(get_u32(&fragment, 0x215c), 0);
        assert_eq!(get_u32(&fragment, 0x2170), 1);
        assert_eq!(get_u64(&fragment, 0x21ce), 0xffff_fc20_c082_8000);
    }

    fn publication_input(kind: RenderDescriptorKind) -> QueuePublicationInput {
        let fragment = matches!(kind, RenderDescriptorKind::Fragment);
        let grid = fragment as u16;
        QueuePublicationInput {
            queue: 0xffff_fc20_c000_8000 + grid as u64 * 0xc0,
            pointers: 0xffff_fc20_0001_0000 + grid as u64 * 0x80,
            item_ring: 0xffff_fc20_c000_c000 + grid as u64 * 0x2870,
            channel_ring: 0xffff_fc20_c010_0000 + grid as u64 * 0x4000,
            channel_producer: 0xffff_fc20_0170_0008 + grid as u64 * 0x100,
            items: [
                0xffff_fc20_c020_0000 + grid as u64 * 0x1_0000,
                0xffff_fc20_c020_4000 + grid as u64 * 0x1_0000,
                0xffff_fc20_c020_8000 + grid as u64 * 0x1_0000,
            ],
            grid_index: grid,
            slot_index: 0,
            current_write: 0,
            group_number: 1,
            first_submit: true,
        }
    }

    fn publication_hash(publication: &QueuePublication) -> u64 {
        let mut value = 0xcbf2_9ce4_8422_2325u64;
        let mut add = |address: u64, bytes: &[u8]| {
            for byte in address.to_le_bytes() {
                value ^= byte as u64;
                value = value.wrapping_mul(0x0000_0100_0000_01b3);
            }
            for byte in (bytes.len() as u32).to_le_bytes() {
                value ^= byte as u64;
                value = value.wrapping_mul(0x0000_0100_0000_01b3);
            }
            for byte in bytes {
                value ^= *byte as u64;
                value = value.wrapping_mul(0x0000_0100_0000_01b3);
            }
        };
        for entry in &publication.item_entries {
            add(entry.address, &entry.bytes);
        }
        add(publication.event.address, &publication.event.bytes);
        add(
            publication.queue_write.address,
            &publication.queue_write.bytes,
        );
        add(publication.ring_slot.address, &publication.ring_slot.bytes);
        add(publication.producer.address, &publication.producer.bytes);
        value
    }

    #[test]
    fn paired_publication_matches_m1n1_9165484() {
        let publication = build_paired_queue_publication(
            publication_input(RenderDescriptorKind::Tiling),
            publication_input(RenderDescriptorKind::Fragment),
        )
        .unwrap();
        assert_eq!(publication.tiling.write_after, 3);
        assert_eq!(publication.fragment.write_after, 3);
        assert_eq!(publication_hash(&publication.tiling), 0x8d2e_1298_cc1d_39c7);
        assert_eq!(
            publication_hash(&publication.fragment),
            0x75e1_3fd1_c48e_51ae
        );
        assert_eq!(
            publication.tiling.ring_slot.bytes[0x14..0x18],
            0x0100_0003u32.to_le_bytes()
        );
        assert_eq!(
            publication.fragment.ring_slot.bytes[0x14..0x18],
            0x0101_0003u32.to_le_bytes()
        );
        assert_eq!(publication.tiling.producer.bytes, 1u32.to_le_bytes());
        assert_eq!(publication.fragment.producer.bytes, 1u32.to_le_bytes());
    }
}
