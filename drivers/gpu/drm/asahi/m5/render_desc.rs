// SPDX-License-Identifier: GPL-2.0-only OR MIT

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

const TILE_WIDTH: u64 = 32;

const TILE_HEIGHT: u64 = 32;

const MACRO_TILES_X: u64 = 4;

const MACRO_TILES_Y: u64 = 4;

const REGION_ENTRY_SIZE: u64 = 5;

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
}

impl RenderBuildError {
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
    /// Utile dimensions in pixels, including the sample-layout adjustment.
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
    // The UAPI's utile height already halves at four samples. Use the actual
    // dimensions once; a second sample-count factor overallocates tile lists
    // and publishes a macro size that disagrees with the native register set.
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

pub(crate) fn build_fragment_registers(
    p: &G17pRenderParameters,
) -> Result<[RegisterWrite; FRAGMENT_REGISTER_COUNT], RenderBuildError> {
    build_fragment_registers_for_work_key(p, render_work_key(1, 1))
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
}

/// Native's TA/3D register-list producer couples the command-buffer state tag
/// to the descriptor's allocated application-GART context. The physical QID,
/// QoS HardwareBufferID, descriptor-array slot and 36-entry state-ring index
/// are independent namespaces and must not be substituted here.
const fn render_work_key(context_id: u32, command_buffer_state_word: u32) -> u64 {
    ((context_id as u64) << 8) | ((command_buffer_state_word & 0x3f) as u64)
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

/// FRG +0x222d mode byte: Linux has always written 1; all 160 native light
/// FRG captures hold 0.
fn g17p_frg_hwdsid_mode() -> u8 {
    1
}

