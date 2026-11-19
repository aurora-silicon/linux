// SPDX-License-Identifier: MIT
//! M5/G17G graphics register construction from owned render resources.
//!
//! Common resource lowering is shared with G17P. Register order, tile packing
//! and per-stage controls follow the native 25G76 M5 reference. This codec is
//! not a claim that Linux TA/3D submission or presentation is qualified.

use crate::g18::fw::render_params::{
    self as g17_render, G17pRenderParameters, RegisterWrite, RenderBuildError, RenderDescriptorMetadata,
    RenderDescriptorObjects,
};

#[cfg(test)]
#[path = "m5_render_test.rs"]
mod tests;

pub(crate) const TA_REGISTER_COUNT: usize = 82;
pub(crate) const FRAGMENT_REGISTER_COUNT: usize = 90;

/// Encode the TA parameter-manager metrics slot in the compact GART window.
/// The native 25G76 slot at 0x1000080040 produces register 1c910 = 0x80041:
/// the low bit enables the record; the remaining bits address its 64 bytes.
pub(crate) fn parameter_buffer_metrics_record(
    scene_metrics: u64,
) -> Result<u64, M5RenderBuildError> {
    let offset = scene_metrics.checked_sub(0x10_0000_0000)
        .ok_or(M5RenderBuildError::OwnershipMismatch)?;
    if offset >= (1u64 << 32) || offset & 0x3f != 0 {
        return Err(M5RenderBuildError::OwnershipMismatch);
    }
    Ok(offset | 1)
}

/// Initial M5 PB slot with fresh ring cursors. The owning job chooses the
/// hardware slot separately and retains the page-list mapping.
pub(crate) fn initial_parameter_buffer_descriptor(
    page_list: u64,
    page_count: u32,
) -> Result<[u32; 4], M5RenderBuildError> {
    if page_list == 0 || page_list >= (1u64 << 42) || page_list & 0x7f != 0
        || page_count == 0 || page_count >= (1 << 22) {
        return Err(M5RenderBuildError::OwnershipMismatch);
    }
    // 25G76 halLoadPBDesc, 89de844..89de8dc. The descriptor has four
    // dwords; +0's low flags and both cursors start at zero. Native's
    // untagged user address is translated into its PB segment encoding.
    let segment = ((page_list + 0x70_0000_0000) & 0x70_0000_0000) >> 8;
    Ok([((page_list >> 4) as u32) & !7,
        (segment as u32 & 0xf000_0000) | page_count, 0, 0])
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum M5RenderBuildError {
    Common(RenderBuildError),
    SharedRegisterLayout,
    TileCountOutOfRange,
    OverlappingTileFlags,
    DispatchBitAlreadySet,
    InvalidContext,
    OwnershipMismatch,
    InvalidUscState,
}

impl From<RenderBuildError> for M5RenderBuildError {
    fn from(error: RenderBuildError) -> Self {
        Self::Common(error)
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5RenderStageControls {
    /// Native host descriptor +0x1120 (TA) / +0x7e8 (fragment).
    pub(crate) resource_flags: u64,
    /// Explicit per-stage d419/d429 input, independent of the work key.
    pub(crate) resource_control: u64,
    /// Native +0x1128 / +0x7f0; a zero source selects the native 0x60 fallback.
    pub(crate) ptc_max: u32,
    pub(crate) work_key: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5RenderControls {
    pub(crate) ta: M5RenderStageControls,
    pub(crate) fragment: M5RenderStageControls,
    pub(crate) ta_tile_control: u64,
    /// Base value for the two 1c898 writes. Bit zero is supplied by the codec.
    pub(crate) ta_dispatch_control: u64,
    pub(crate) fragment_raster_control: u64,
    /// Non-geometry bits for 16068. Never includes tile counts, utile or layers.
    pub(crate) fragment_tile_flags: u64,
    /// M5 adds this second pair of store-program bindings at 153b1/153b9.
    pub(crate) store_pipeline_2_bind: u64,
    pub(crate) store_pipeline_2: u64,
    pub(crate) partial_store_pipeline_2_bind: u64,
    pub(crate) partial_store_pipeline_2: u64,
    pub(crate) partial_tile_flags: u64,
}

/// Per-submission USC private-memory accounting, separate from user status
/// registers and queue completion stamps.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5RenderUscState {
    pub(crate) control: u64,
    /// AGXUMADataRec +0x50. Native allocatePagesEnd derives this from pool
    /// sizes and 4-KiB allocation units; it is a byte count, not a GPU VA.
    pub(crate) required_bytes: u64,
    /// Capacity actually backed by this job's retained USC allocation pages.
    pub(crate) allocated_bytes: u64,
    /// A 64-byte slot in the firmware-visible statistics ring.
    pub(crate) statistics: u64,
}

impl M5RenderUscState {
    fn valid(self) -> bool {
        let firmware_range = |address: u64, size: u64| {
            address >= 0xffff_fc00_0000_0000
                && address & 0x3f == 0
                && address.checked_add(size).is_some()
        };
        firmware_range(self.control, 0x80)
            && firmware_range(self.statistics, 0x40)
            && self.required_bytes != 0
            && self.required_bytes <= self.allocated_bytes
            && self.allocated_bytes & 0xfff == 0
    }
}

/// Fill the native USC fields only after the caller owns the pool and the
/// statistics slot. Other runtime fields still need queue/link publication.
/// Validate both stages before modifying either descriptor.
pub(crate) fn apply_usc_submission_state(
    ta: &mut [u8],
    fragment: &mut [u8],
    ta_state: M5RenderUscState,
    fragment_state: M5RenderUscState,
) -> Result<(), M5RenderBuildError> {
    if ta.len() < g17_render::TA_DESCRIPTOR_SIZE
        || fragment.len() < g17_render::FRAGMENT_DESCRIPTOR_SIZE
    {
        return Err(RenderBuildError::BufferTooSmall.into());
    }
    if !ta_state.valid() || !fragment_state.valid() {
        return Err(M5RenderBuildError::InvalidUscState);
    }
    // 25G76 AGXTAChannelSKSM::submitBuffer: 8a244c4..8a244ec.
    // AGX3DChannelSKSM::submitBuffer takes the corresponding value from
    // host +0xba0 = AGXUMADataRec +0x50 at 895b70c..895b718.
    for (raw, state, control, required, statistics) in [
        (ta, ta_state, 0x934, 0x93d, 0x945),
        (fragment, fragment_state, 0x21ce, 0x21d7, 0x21df),
    ] {
        put_u64(raw, control, state.control);
        put_u64(raw, required, state.required_bytes);
        put_u64(raw, statistics, state.statistics);
    }
    Ok(())
}

const EMPTY_REGISTER: RegisterWrite = RegisterWrite {
    number: 0,
    value: 0,
};

struct RegisterList<const N: usize> {
    values: [RegisterWrite; N],
    count: usize,
}

impl<const N: usize> RegisterList<N> {
    fn new() -> Self {
        Self {
            values: [EMPTY_REGISTER; N],
            count: 0,
        }
    }

    fn push(&mut self, number: u32, value: u64) -> Result<(), M5RenderBuildError> {
        if self.count == N {
            return Err(M5RenderBuildError::SharedRegisterLayout);
        }
        self.values[self.count] = RegisterWrite { number, value };
        self.count += 1;
        Ok(())
    }

    fn finish(self) -> Result<[RegisterWrite; N], M5RenderBuildError> {
        if self.count != N {
            return Err(M5RenderBuildError::SharedRegisterLayout);
        }
        Ok(self.values)
    }
}

fn value(registers: &[RegisterWrite], number: u32) -> Result<u64, M5RenderBuildError> {
    registers
        .iter()
        .find(|r| r.number == number)
        .map(|r| r.value)
        .ok_or(M5RenderBuildError::SharedRegisterLayout)
}

const fn ptc_control(maximum: u32) -> u64 {
    let maximum = if maximum == 0 { 0x60 } else { maximum };
    ((maximum as u64) << 32) | 0x400020
}

fn tile_state(p: &G17pRenderParameters, flags: u64) -> Result<u64, M5RenderBuildError> {
    let tiles_x = (p.width as u64 + 31) / 32;
    let tiles_y = (p.height as u64 + 31) / 32;
    if !(1..=1024).contains(&tiles_x) || !(1..=1024).contains(&tiles_y) {
        return Err(M5RenderBuildError::TileCountOutOfRange);
    }
    // 25G76 fragment generator: bfi at 89dcb98, 89dcbb8 and 89dcbbc.
    let geometry_mask = (0xfffff_u64 << 42) | (0xf_u64 << 38) | (1_u64 << 32);
    if flags & geometry_mask != 0 {
        return Err(M5RenderBuildError::OverlappingTileFlags);
    }
    Ok(flags
        | ((tiles_x - 1) << 42)
        | ((tiles_y - 1) << 52)
        | ((p.utile_config & 0xf000) << 26)
        | if p.layers > 1 { 1_u64 << 32 } else { 0 })
}

fn tile_blocks(pixels: u32) -> u64 {
    let tiles = (pixels as u64 + 31) / 32;
    let first = (((tiles + 3) / 4) + 3) & !3;
    // Native TA host +1208/+120c supplies 0x40200c for the reference:
    // boundaries 12, 8, 4 in ten-bit fields, versus G17P's nine-bit fields.
    (3 * first) | ((2 * first) << 10) | (first << 20)
}

pub(crate) fn build_ta_registers(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
) -> Result<[RegisterWrite; TA_REGISTER_COUNT], M5RenderBuildError> {
    // M5's captured register program addresses heap metadata directly.
    // G17P's layered path now uses a separate layer table in these slots.
    let mut common_parameters = *p;
    common_parameters.layermeta = p.heapmeta;
    let common = g17_render::build_ta_registers(&common_parameters)?;
    if controls.ta_dispatch_control & 1 != 0 {
        return Err(M5RenderBuildError::DispatchBitAlreadySet);
    }
    // Validate the M5 tile fields even when only constructing the TA half.
    tile_state(p, controls.fragment_tile_flags)?;
    let mut out = RegisterList::new();
    for mut register in common {
        match register.number {
            0x0a309 | 0x0a5a1 | 0x0d419 => continue,
            0x1c8f8 => {
                out.push(0x0d499, controls.ta.resource_flags)?;
                out.push(0x0d419, controls.ta.resource_control)?;
                out.push(0x0a5a1, ptc_control(controls.ta.ptc_max))?;
                register.value = controls.ta_tile_control;
            }
            0x10171 => {
                for (number, source) in [(0x1ca50, 0x101b9), (0x1ca58, 0x10149), (0x1ca60, 0x10141)]
                {
                    out.push(number, value(&common, source)?)?;
                }
                out.push(0x1ca70, tile_blocks(p.width))?;
                out.push(0x1ca78, tile_blocks(p.height))?;
                out.push(0x1ca68, value(&common, 0x1c081)?)?;
            }
            0x1c069 => register.value = tile_blocks(p.width),
            0x1c071 => register.value = tile_blocks(p.height),
            0x1c898 => {
                if register.value > 1 {
                    return Err(M5RenderBuildError::SharedRegisterLayout);
                }
                register.value |= controls.ta_dispatch_control;
            }
            0x10799 => out.push(0x1a149, 0)?,
            0x10209 => {
                out.push(0x0a739, 0)?;
                register.value = controls.ta.work_key;
            }
            0x1c9f0 | 0x14320 => register.value = controls.ta.work_key,
            0x14308 => register.value = p.usc_flist_hardware_buffer_id as i32 as i64 as u64,
            _ => {}
        }
        out.push(register.number, register.value)?;
        if register.number == 0x01740 {
            out.push(0x0a739, 0)?;
        }
    }
    out.finish()
}

pub(crate) fn build_fragment_registers(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
) -> Result<[RegisterWrite; FRAGMENT_REGISTER_COUNT], M5RenderBuildError> {
    // M5's captured register program addresses heap metadata directly.
    // G17P's layered path now uses a separate layer table in these slots.
    let mut common_parameters = *p;
    common_parameters.layermeta = p.heapmeta;
    let common = g17_render::build_fragment_registers(&common_parameters)?;
    let geometry = tile_state(p, controls.fragment_tile_flags)?;
    let mut out = RegisterList::new();
    let mut dispatch_writes = 0;
    let mut tile_writes = 0;
    for mut register in common {
        match register.number {
            0x0a301 | 0x15109 | 0x15101 | 0x160a8 | 0x0a5a9 | 0x0d429 => continue,
            0x100b1 => register.number = 0x16469,
            0x100d9 => register.number = 0x16471,
            0x100b8 => register.number = 0x16138,
            0x10791 => register.value = controls.fragment_raster_control,
            0x15321 => {
                out.push(0x0d491, controls.fragment.resource_flags)?;
                out.push(0x0d429, controls.fragment.resource_control)?;
                out.push(0x0a5a9, ptc_control(controls.fragment.ptc_max))?;
            }
            0x16068 => {
                tile_writes += 1;
                if tile_writes == 1 {
                    register.value = geometry;
                }
            }
            0x16020 => {
                dispatch_writes += 1;
                if dispatch_writes == 3 {
                    if register.value != 0 {
                        return Err(M5RenderBuildError::SharedRegisterLayout);
                    }
                    continue;
                }
            }
            0x160e0 => out.push(0x1a151, 0)?,
            0x10211 => {
                out.push(0x0a741, 0)?;
                register.value = controls.fragment.work_key;
            }
            0x10420 => register.value = controls.fragment.work_key,
            0x14048 => register.value = p.usc_flist_hardware_buffer_id as i32 as i64 as u64,
            _ => {}
        }
        out.push(register.number, register.value)?;
        if register.number == 0x15381 {
            out.push(0x153b1, controls.store_pipeline_2_bind)?;
            out.push(0x153b9, controls.store_pipeline_2)?;
        }
        if register.number == 0x01731 {
            out.push(0x0a741, 0)?;
        }
    }
    if dispatch_writes != 3 || tile_writes != 2 {
        return Err(M5RenderBuildError::SharedRegisterLayout);
    }
    out.finish()
}

fn partial_registers<const N: usize>(
    common: &[RegisterWrite],
    insert_after: u32,
    controls: &M5RenderControls,
) -> Result<[RegisterWrite; N], M5RenderBuildError> {
    let mut out = RegisterList::new();
    for register in common {
        let bits = if register.number == 0x10039 {
            controls.partial_tile_flags
        } else {
            0
        };
        out.push(register.number, register.value | bits)?;
        if register.number == insert_after {
            out.push(0x153b1, controls.partial_store_pipeline_2_bind)?;
            out.push(0x153b9, controls.partial_store_pipeline_2)?;
        }
    }
    out.finish()
}

pub(crate) fn build_partial_store_registers(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
) -> Result<[RegisterWrite; 18], M5RenderBuildError> {
    partial_registers(&g17_render::partial_store_registers(p), 0x15381, controls)
}

pub(crate) fn build_partial_resume_registers(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
) -> Result<[RegisterWrite; 25], M5RenderBuildError> {
    partial_registers(&g17_render::partial_resume_registers(p), 0x15381, controls)
}

pub(crate) fn build_partial_load_registers(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
) -> Result<[RegisterWrite; 12], M5RenderBuildError> {
    partial_registers(&g17_render::partial_load_registers(p), 0x15309, controls)
}

fn work_key(
    p: &G17pRenderParameters,
    metadata: RenderDescriptorMetadata,
) -> Result<u64, M5RenderBuildError> {
    if metadata.context_id == 0 || metadata.context_id > 255 {
        return Err(M5RenderBuildError::InvalidContext);
    }
    if metadata.ta_hardware_buffer_id != p.ta_hardware_buffer_id {
        return Err(M5RenderBuildError::OwnershipMismatch);
    }
    metadata
        .submission_ordinal
        .checked_add(metadata.submission_ordinal / 2)
        .ok_or(M5RenderBuildError::Common(
            RenderBuildError::ArithmeticOverflow,
        ))?;
    Ok(((metadata.context_id as u64) << 8) | (metadata.command_buffer_state_word & 0x3f) as u64)
}

fn alias_offset(alias: u64, offset: u64) -> Result<u64, M5RenderBuildError> {
    alias.checked_add(offset).ok_or(M5RenderBuildError::Common(
        RenderBuildError::ArithmeticOverflow,
    ))
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

/// Construct the M5 TA record's common layout and all register programs.
/// Runtime ownership/completion fields must be populated before publication.
#[inline(never)]
pub(crate) fn build_ta_descriptor_base(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    low_alias: u64,
    raw: &mut [u8],
) -> Result<(), M5RenderBuildError> {
    let mut controls = *controls;
    controls.ta.work_key = work_key(p, metadata)?;
    let program = alias_offset(low_alias, 0x60)?;
    alias_offset(low_alias, g17_render::TA_DESCRIPTOR_SIZE as u64)?;
    let registers = build_ta_registers(p, &controls)?;
    g17_render::build_ta_descriptor_base(p, objects, metadata, &registers, raw)?;
    // Native boundaries: main work ends before the final a739/deflake/dispatch.
    for (offset, count) in [
        (0x38, TA_REGISTER_COUNT - 3),
        (0x3a, TA_REGISTER_COUNT),
        (0x3c, TA_REGISTER_COUNT),
    ] {
        put_u16(raw, offset, count as u16);
    }
    put_u64(raw, 0x760, program);
    Ok(())
}

/// Construct the M5 fragment record, including all three partial programs.
/// The shared encoder is pure and does not apply Neo diagnostic overrides.
#[inline(never)]
pub(crate) fn build_fragment_descriptor_base(
    p: &G17pRenderParameters,
    controls: &M5RenderControls,
    objects: RenderDescriptorObjects,
    metadata: RenderDescriptorMetadata,
    low_alias: u64,
    raw: &mut [u8],
) -> Result<(), M5RenderBuildError> {
    let mut controls = *controls;
    controls.fragment.work_key = work_key(p, metadata)?;
    alias_offset(low_alias, g17_render::FRAGMENT_DESCRIPTOR_SIZE as u64)?;
    let main = build_fragment_registers(p, &controls)?;
    let store = build_partial_store_registers(p, &controls)?;
    let resume = build_partial_resume_registers(p, &controls)?;
    let load = build_partial_load_registers(p, &controls)?;
    g17_render::build_fragment_descriptor_base(
        p,
        objects,
        metadata,
        g17_render::G17pFragmentRegisters {
            main: &main,
            store: &store,
            resume: &resume,
            load: &load,
        },
        raw,
    )?;
    put_u32(raw, 0x50, 1);
    put_u32(raw, 0x90, 1);
    for (offset, count) in [
        (0x80, FRAGMENT_REGISTER_COUNT - 3),
        (0x82, FRAGMENT_REGISTER_COUNT - 1),
        (0x84, FRAGMENT_REGISTER_COUNT - 1),
        (0x88, FRAGMENT_REGISTER_COUNT),
    ] {
        put_u16(raw, offset, count as u16);
    }
    for (offset, target) in [
        (0x7a0, 0xa0),
        (0xec0, 0x7c0),
        (0x15e0, 0xee0),
        (0x1d00, 0x1600),
    ] {
        put_u64(raw, offset, alias_offset(low_alias, target)?);
    }
    Ok(())
}
