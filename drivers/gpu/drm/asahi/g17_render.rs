// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


const TA_REGISTER_COUNT: usize = 73;
const FRAGMENT_REGISTER_COUNT: usize = 89;

const _: () = assert!(TA_REGISTER_COUNT == 73);
const _: () = assert!(FRAGMENT_REGISTER_COUNT == 89);
const PARTIAL_STORE_REGISTER_COUNT: usize = 16;
const PARTIAL_RESUME_REGISTER_COUNT: usize = 23;
const PARTIAL_LOAD_REGISTER_COUNT: usize = 10;
const REGISTER_BYTES: usize = 12;

pub(crate) const TA_DESCRIPTOR_SIZE: usize = 0x9c0;
pub(crate) const FRAGMENT_DESCRIPTOR_SIZE: usize = 0x2240;
const G17P_FRAGMENT_TAIL_SETUP_WORD: u32 = u32::MAX;
const G17P_NEO_FRAGMENT_SHARED_RUNTIME_FLAG_SNAPSHOT: [u32; 2] = [1, 1];
pub(crate) const G17P_EOT_RESOURCE_SPEC: u64 = 0x8;
pub(crate) const G17P_LOAD_RESOURCE_SPEC: u64 = 0x0007_8000_0000_0040;
pub(crate) const G17P_SOURCE_BG_PIPELINE: u64 = 0x0000_0100_0199_0240;
pub(crate) const G17P_SOURCE_EOT_PIPELINE: u64 = 0x0000_0100_0199_0640;
pub(crate) const G17P_FRAGMENT_MCACHE_OFFSET: usize = 0x6a0;
pub(crate) const G17P_TILING_MCACHE_OFFSET: usize = 0x660;
pub(crate) const G17P_MCACHE_MAPPING_SIZE: usize = 0x10;
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

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PMcacheMapping {
    pub(crate) address: u64,
    pub(crate) size_units: u32,
    pub(crate) hwsid: u8,
    pub(crate) selector: u8,
    pub(crate) range_shift: u16,
}

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
    pub(crate) tilemap: u64,
    pub(crate) heapmeta: u64,
    pub(crate) tpc: u64,
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
    pub(crate) depth_dimensions: u64,
    pub(crate) merge_upper_x_bits: u32,
    pub(crate) merge_upper_y_bits: u32,
    pub(crate) layers: u16,
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
    pub(crate) lifecycle: u64,
    /// Fragment event correlation written to 0x160e0/0x01499/0x0a341.
    pub(crate) fragment_lifecycle: u64,
    pub(crate) work_stamp: u64,
    pub(crate) sampler_array: u64,
    pub(crate) sampler_count: u32,
    pub(crate) process_empty_tiles: bool,
    pub(crate) ta_timestamp_start: u64,
    pub(crate) ta_timestamp_end: u64,
    pub(crate) ta_user_timestamp_start: u64,
    pub(crate) ta_user_timestamp_end: u64,
    pub(crate) fragment_timestamp_start: u64,
    pub(crate) fragment_timestamp_end: u64,
    pub(crate) fragment_user_timestamp_start: u64,
    pub(crate) fragment_user_timestamp_end: u64,
    pub(crate) absolute_pointers: bool,
    pub(crate) bg_resource_prefix: u64,
    pub(crate) native_pm_bytes: bool,
    pub(crate) ta_hardware_buffer_id: u32,
    pub(crate) usc_flist_hardware_buffer_id: u32,
    pub(crate) native_ta_registers: u32,
    /// Retained GPU-ID diagnostic snapshot, not the fragment parameter-buffer
    /// owner. The old 0x1c838 use confused host descriptor +0x928 with a
    /// different object's transient raw-command +0x928 field.
    pub(crate) gpc_perf_state_map: u32,
    pub(crate) gpc_perf_state_map_low: u32,
    pub(crate) gpc_perf_state_control: u32,
    pub(crate) parameter_buffer: u64,
}

impl G17pRenderParameters {
    pub(crate) const fn source_rsrc8() -> Self {
        Self {
            width: 64,
            height: 64,
            context_base: 0x0000_0010_0000_0000,
            tilemap: 0x0000_0010_001b_0000,
            heapmeta: 0x0000_0010_001b_5000,
            tpc: 0x0000_0010_0024_0000,
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
            depth_dimensions: 0,
            merge_upper_x_bits: 0x3cdd_b3d9,
            merge_upper_y_bits: 0x3cdd_b3d9,
            layers: 1,
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
            work_stamp: 0x100,
            sampler_array: 0,
            sampler_count: 0,
            process_empty_tiles: true,
            ta_timestamp_start: 0xffff_fc20_0002_4c68,
            ta_timestamp_end: 0,
            ta_user_timestamp_start: 0,
            ta_user_timestamp_end: 0,
            fragment_timestamp_start: 0xffff_fc20_0002_4c68,
            fragment_timestamp_end: 0xffff_fc20_0002_4c70,
            fragment_user_timestamp_start: 0,
            fragment_user_timestamp_end: 0,
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

fn ta_resource_offset(
    parameters: &G17pRenderParameters,
    address: u64,
) -> Result<u64, RenderBuildError> {
    address
        .checked_sub(parameters.context_base)
        .ok_or(RenderBuildError::AddressBeforeContext)
}

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
    let g = geometry(p)?;
    let tilemap = ta_resource_offset(p, p.tilemap)?;
    let heapmeta = ta_resource_offset(p, p.heapmeta)?;
    let tpc = ta_resource_offset(p, p.tpc)?;
    let deflake_1 = ta_user_offset(p.deflake_1)?;
    let deflake_2 = ta_user_offset(p.deflake_2)?;
    let deflake_3 = ta_user_offset(p.deflake_3)?;
    let encoder = if p.absolute_pointers {
        p.encoder
    } else {
        vdm_user_offset(p.encoder)?
    };
    let layer_mode = if p.layers > 1 {
        0xe000 | (p.layers as u64 - 1)
    } else {
        0x8000
    };

    Ok([
        RegisterWrite::new(0x01748, 1),
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
        RegisterWrite::new(0x1c079, heapmeta),
        RegisterWrite::new(0x1c9d8, heapmeta),
        RegisterWrite::new(0x10151, 0),
        RegisterWrite::new(0x1c199, 0),
        RegisterWrite::new(0x1c1a1, 0),
        RegisterWrite::new(0x1c1a9, 0),
        RegisterWrite::new(0x1c1b1, 0),
        RegisterWrite::new(0x1c1b9, 0),
        RegisterWrite::new(0x1c8f8, 0x8860),
        RegisterWrite::new(0x1c0b1, g.size1),
        RegisterWrite::new(0x1c850, g.size1),
        RegisterWrite::new(0x10131, p.multisample_control),
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
        RegisterWrite::new(0x1c9e8, 0),
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
        RegisterWrite::new(0x1c830, p.ta_hardware_buffer_id as u64),
        RegisterWrite::new(0x1ca30, p.cycle),
        RegisterWrite::new(0x16c39, p.cycle),
        RegisterWrite::new(0x1c910, p.record_index),
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
        RegisterWrite::new(0x10209, p.work_stamp),
        RegisterWrite::new(0x1c9f0, p.work_stamp),
        RegisterWrite::new(0x14320, p.work_stamp),
        RegisterWrite::new(0x14308, p.usc_flist_hardware_buffer_id as u64),
        RegisterWrite::new(0x14318, p.ta_status | 1),
        RegisterWrite::new(0x01740, 1),
        RegisterWrite::new(0x1c880, deflake_1),
        RegisterWrite::new(0x1c898, 1),
    ])
}

fn apply_g17p_ta_oracle(raw: &mut [u8]) {
    let mask = *crate::module_parameters::g17p_ta_oracle.value();
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
    build_fragment_registers_counted(p).map(|(regs, _)| regs)
}

/// As `build_fragment_registers`, plus the live entry count. The count is
/// always `FRAGMENT_REGISTER_COUNT`; the pair form is kept because the
/// descriptor's count word at +0x7a8 must agree with the slice actually
/// written, and having one source for both makes them impossible to desync.
pub(crate) fn build_fragment_registers_counted(
    p: &G17pRenderParameters,
) -> Result<([RegisterWrite; FRAGMENT_REGISTER_COUNT], usize), RenderBuildError> {
    Ok((build_fragment_registers_full(p)?, FRAGMENT_REGISTER_COUNT))
}

fn build_fragment_registers_full(
    p: &G17pRenderParameters,
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
        RegisterWrite::new(0x16060, p.heapmeta),
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
        RegisterWrite::new(0x1c838, p.ta_hardware_buffer_id as u64),
        RegisterWrite::new(0x1ca28, p.cycle),
        RegisterWrite::new(0x10211, p.work_stamp),
        RegisterWrite::new(0x10420, p.work_stamp),
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

pub(crate) const G17P_RENDER_GID_GROUP_STRIDE: u32 = 0x0100_0000;

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

fn partial_store_registers(
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
        RegisterWrite::new(0x15319, p.depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
    ]
}

fn partial_resume_registers(
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
        RegisterWrite::new(0x15319, p.depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
    ]
}

fn partial_load_registers(
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
        RegisterWrite::new(0x15319, p.depth_flags),
        RegisterWrite::new(0x15349, 0x0404_0404),
    ]
}

fn put_u8(raw: &mut [u8], offset: usize, value: u8) {
    raw[offset] = value;
}

fn put_u16(raw: &mut [u8], offset: usize, value: u16) {
    raw[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(raw: &mut [u8], offset: usize, value: u32) {
    raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(raw: &mut [u8], offset: usize, value: u64) {
    raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
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

    pub(crate) const fn with_shared(mut self, shared: u64) -> Self {
        self.shared = shared;
        self
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RenderDescriptorMetadata {
    pub(crate) context_id: u32,
    pub(crate) submission_ordinal: u32,
    pub(crate) ta_hardware_buffer_id: u32,
    pub(crate) submit_sequence: u64,
}

impl RenderDescriptorMetadata {
    pub(crate) const fn source(kind: RenderDescriptorKind) -> Self {
        Self {
            context_id: 1,
            submission_ordinal: 0,
            ta_hardware_buffer_id: 0,
            submit_sequence: match kind {
                RenderDescriptorKind::Tiling => 1,
                RenderDescriptorKind::Fragment => 0,
            },
        }
    }
}

fn descriptor_work_ordinal(ordinal: u32) -> u32 {
    ordinal + ordinal / 2
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
    raw[..TA_DESCRIPTOR_SIZE].fill(0);
    let registers = build_ta_registers(parameters)?;
    write_common_descriptor(
        RenderDescriptorKind::Tiling,
        objects,
        metadata,
        &registers,
        raw,
    );

    let deflake = ta_user_offset(parameters.deflake_1)?;
    let sampler_max = if parameters.sampler_count == 0 {
        0
    } else {
        parameters.sampler_count + 1
    };
    put_u32(raw, 0x0768, 0x036c_0049);
    put_u64(raw, 0x0780, parameters.tpc);
    put_u8(raw, 0x0789, 0x78);
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
    put_u8(raw, 0x094d, if parameters.native_pm_bytes { 3 } else { 1 });
    put_u64(raw, 0x08fe, parameters.ta_timestamp_start);
    put_u64(raw, 0x0906, parameters.ta_timestamp_end);
    put_u64(raw, 0x090e, parameters.ta_user_timestamp_start);
    put_u64(raw, 0x0916, parameters.ta_user_timestamp_end);
    put_u32(raw, 0x086e, (parameters.lifecycle >> 32) as u32);
    put_u64(raw, 0x08ce, parameters.lifecycle as u32 as u64);
    apply_g17p_ta_oracle(raw);
    Ok(())
}

pub(crate) fn build_cold_opening_ta_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    context_id: u32,
    ta_hardware_buffer_id: u32,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    let mut metadata = RenderDescriptorMetadata::source(RenderDescriptorKind::Tiling);
    metadata.context_id = context_id;
    metadata.ta_hardware_buffer_id = ta_hardware_buffer_id;
    build_ta_descriptor(parameters, objects, metadata, raw)?;
    put_u16(raw, 0x38, 0x47);
    put_u16(raw, 0x3a, 0x49);
    put_u16(raw, 0x3c, 0x49);
    // +0x788 carries the GTP-only RT-buffer size. The valid 32x32 records use
    // one 0x800-byte buffer and therefore encode qword 0x800 (byte +0x789 =
    // 0x08). Cold qid0's 0x7800 belongs to a different 15-buffer pool and must
    // not be copied into this one-buffer Linux pool.
    put_u8(raw, 0x789, 0x08);
    put_u8(raw, 0x93e, 0xd0);
    put_u8(raw, 0x93f, if parameters.native_pm_bytes { 0x87 } else { 0x91 });
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

pub(crate) fn build_fragment_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    if raw.len() < FRAGMENT_DESCRIPTOR_SIZE {
        return Err(RenderBuildError::BufferTooSmall);
    }
    raw[..FRAGMENT_DESCRIPTOR_SIZE].fill(0);
    let geometry = geometry(parameters)?;
    let (registers, register_count) = build_fragment_registers_counted(parameters)?;
    write_common_descriptor(
        RenderDescriptorKind::Fragment,
        objects,
        metadata,
        &registers[..register_count],
        raw,
    );

    put_u64(raw, 0x40, parameters.tilemap);
    put_u64(raw, 0x48, parameters.multisample_control);
    put_u32(raw, 0x54, geometry.macro_size as u32);
    put_u32(raw, 0x68, parameters.merge_upper_x_bits);
    put_u32(raw, 0x6c, parameters.merge_upper_y_bits);
    put_u64(raw, 0x78, geometry.tiles_x * geometry.tiles_y);

    put_u32(
        raw,
        0x07a8,
        (((register_count * REGISTER_BYTES) as u32) << 16) | register_count as u32,
    );
    put_embedded_registers(raw, 0x0ec8, 0x07c0, &partial_store_registers(parameters));
    put_embedded_registers(raw, 0x15e8, 0x0ee0, &partial_resume_registers(parameters));
    put_embedded_registers(raw, 0x1d08, 0x1600, &partial_load_registers(parameters));

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
    {
        let oracle = *crate::module_parameters::g17p_frag_oracle.value();
        if oracle & 0x01 != 0 {
            put_u64(raw, 0x158, 0x7007);
        }
        if oracle & 0x02 != 0 {
            put_u64(raw, 0x488, 1);
        }
        if oracle & 0x04 != 0 {
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
            put_u64(raw, 0x1d0, 0);
            put_u64(raw, 0x1f0, 0);
            put_u64(raw, 0x1f8, 0);
        }
        if oracle & 0x80 != 0 {
            put_u32(raw, 0x1d8 + 4, 0x3f80_0000);
        }
    }
    put_u8(raw, 0x2100, 0);
    put_u32(raw, 0x2104, parameters.gpc_perf_state_control);
    put_u32(raw, 0x2108, (parameters.fragment_lifecycle >> 32) as u32);
    put_u32(raw, 0x210c, parameters.gpc_perf_state_map_low);
    put_u32(raw, 0x2110, G17P_FRAGMENT_TAIL_SETUP_WORD);
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
    put_u64(raw, 0x21a8, parameters.fragment_user_timestamp_start);
    put_u64(raw, 0x21b0, parameters.fragment_user_timestamp_end);
    put_u8(raw, 0x21cc, 0x53);
    put_u8(raw, 0x21d6, 1);
    put_u8(raw, 0x21e7, 1);
    put_u8(raw, 0x2208, 1);
    put_u8(raw, 0x220c, 1);
    put_u8(raw, 0x222d, 1);
    Ok(())
}

pub(crate) fn build_cold_opening_fragment_descriptor(
    parameters: &G17pRenderParameters,
    objects: RenderDescriptorObjects,
    context_id: u32,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    let mut metadata = RenderDescriptorMetadata::source(RenderDescriptorKind::Fragment);
    metadata.context_id = context_id;
    build_fragment_descriptor(parameters, objects, metadata, raw)?;
    put_u32(raw, 0x50, 1);
    put_u16(raw, 0x80, 0x56);
    put_u16(raw, 0x82, 0x57);
    put_u16(raw, 0x84, 0x57);
    put_u16(raw, 0x88, 0x59);
    put_u32(raw, 0x90, 1);
    put_u32(raw, 0x215c, 1);
    Ok(())
}

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
    submission_ordinal: u32,
    cold_opening: bool,
    native_pm_bytes: bool,
    raw: &mut [u8],
) -> Result<(), RenderBuildError> {
    let item_number = submission_ordinal
        .checked_add(1)
        .ok_or(RenderBuildError::ArithmeticOverflow)?;
    // Tag-14 and the current dependency/event interface carry a 24-bit stamp.
    // This is a representability limit, not an eight-bit ring-slot lifetime.
    if item_number > 0x00ff_ffff {
        return Err(RenderBuildError::ArithmeticOverflow);
    }
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
            put_u32(raw, 0x079c, u32::from(linked_fragment_queue_id));
            put_u32(raw, 0x07a0, item_number << 8);
            apply_g17p_ta_linked_completion_scratch(
                u64::from(item_number), u64::from(item_number), item_number as u8, raw,
            )?;
            put_u32(raw, 0x08b6, item_number << 8);
            put_u32(
                raw,
                0x08ba,
                if *crate::module_parameters::g17p_render_ta_qid_field.value() != 0 {
                    u32::from(queue_id)
                } else {
                    0
                },
            );
            if queue_id == 0 && crate::g17_submission::G17P_NATIVE_RENDER_DOORBELL_PRIORITY == 0 {
                put_u32(raw, 0x08c2, 1);
            }
            put_u32(raw, 0x08d6, submission_ordinal);
            put_u64(raw, 0x0934, runtime_control);
            if cold_opening {
                // Retain the one-buffer GTP size installed by the cold
                // descriptor builder.
                put_u8(raw, 0x0789, 0x08);
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
            put_u32(raw, 0x2150, item_number << 8);
            put_u32(raw, 0x2154, u32::from(queue_id));
            put_u32(raw, 0x215c, u32::from(submission_ordinal == 0));
            // +0x2160 is the packed 3D-channel SKSM completion scratch value:
            // the physical queue timestamp in bits [39:0] and the channel's
            // payload byte in bits [47:40]. G17PUserRenderStorage stages it
            // from the retained fragment producer immediately before publish.
            put_u32(raw, 0x2170, submission_ordinal);
            put_u64(raw, 0x21ce, runtime_control);
        }
    }
    Ok(())
}

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

