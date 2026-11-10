// SPDX-License-Identifier: MIT
//! Pure 26A428 G18 render register lowering over the unchanged M5 builder.
//!
//! See the G18 render-register notes for the oracle, supported envelope and unresolved
//! runtime policy. All compact pointers here are already in the native G18
//! user-process view. The job, not this codec, chooses/maps that address view.

use crate::g18::fw::render_params::{G17pRenderParameters, RegisterWrite};
use crate::g18::fw::m5_render::{self as m5_render, M5RenderControls, M5RenderStageControls};
use crate::g18::fw::m5_render_target as m5_render_target;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G18RenderError {
    M5(m5_render::M5RenderBuildError),
    RegisterLayout,
    UnsupportedGeometry,
    CompactAddress,
    UnsupportedFeature,
}

impl From<m5_render::M5RenderBuildError> for G18RenderError {
    fn from(e: m5_render::M5RenderBuildError) -> Self {
        Self::M5(e)
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderStageControls {
    /// TA descriptor +1188 | (+1190 << 16); fragment +820 | (+828 << 16).
    /// This is NOT the M5 d499/d419/a5a1 tuple. Runtime must supply pool policy.
    pub(crate) resource_word: u64,
    /// Native literal window triple (TA a109/a129/a149; fragment a101/a121/a141).
    /// 8c4f4d4..8c4f56c / 8c3b358..8c3b3f0; no descriptor read.
    pub(crate) address_windows: [u64; 3],
    /// (TA +12c4 / fragment +154) << 8 | (+12d0 / +988) & 63.
    pub(crate) work_key: u64,
    /// Optional USC statistics mapping from +1468 / +bb0 pointee chain.
    /// Native helper 8c4bdb8: absent -> 0, present -> VA & 7ffffffffc0,
    /// OR context id << 56 and bit 0. This is NOT ta/fragment_status | 1.
    pub(crate) usc_statistics: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderControls {
    pub(crate) ta: G18RenderStageControls,
    pub(crate) fragment: G18RenderStageControls,
    /// Native TA +12d4/+12d5, device +4d4/+6b9, base 10c08860.
    pub(crate) ta_tile_control: u64,
    /// Native device +10ac4 and descriptor +1659; bit 0 belongs to dispatch.
    pub(crate) ta_dispatch_control: u64,
    /// Native fragment +819 and policy/device state; baseline 200000ff03ef.
    pub(crate) fragment_raster_control: u64,
    /// Native 16068 nongeometry bits; +8cb/+ca0, device +6b0.
    pub(crate) fragment_tile_flags: u64,
    /// Native fragment +982/+983 and device +4d4/+6b9; baseline 8860.
    pub(crate) fragment_tile_control: u64,
    /// Native descriptor +ac8 & 0x7f, emitted at 8c3c208..8c3c230.
    /// Independent of work-key low bits (+988) and utile dimensions.
    pub(crate) fragment_control: u64,
    /// Native +770 / register 10051 sample control. This is independent of
    /// the application's tile-storage block count used by metadata +4d0.
    pub(crate) fragment_sample_control: u64,
    /// Native +8cb and device +10aa4; first 160a8, final has bit 5 set.
    pub(crate) fragment_dispatch_control: u64,
    /// Native +7a0/+7a8. The second EOT binding is independent of +790/+798.
    pub(crate) store_pipeline_2_bind: u64,
    pub(crate) store_pipeline_2: u64,
    /// Native partial helper uses +7b0/+7b8 for both EOT binding pairs.
    pub(crate) partial_tile_flags: u64,
    /// Native TA loader +11f8/+1200 (second tilemap/layer metadata).
    pub(crate) ta_cluster_tilemap: u64,
    pub(crate) ta_cluster_layermeta: u64,
    /// Native TA loader +1218/+1220/+1228/+1238 (optional cluster resources).
    pub(crate) ta_cluster_meta: [u64; 4],
    /// Native TA loader +1240/+1248/+1250, copied from target +d8/+e0/+e8.
    pub(crate) ta_cluster_control: [u64; 3],
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18Program<const N: usize> {
    pub(crate) registers: [RegisterWrite; N],
    pub(crate) rce_count: u16,
    pub(crate) command_offset: u32,
}

impl<const N: usize> G18Program<N> {
    pub(crate) const fn empty() -> Self {
        Self {
            registers: [RegisterWrite {
                number: 0,
                value: 0,
            }; N],
            rce_count: 0,
            command_offset: 0,
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18FragmentPrograms {
    pub(crate) main: G18Program<92>,
    pub(crate) store: G18Program<17>,
    pub(crate) resume: G18Program<24>,
    pub(crate) load: G18Program<12>,
}

struct List<'a, const N: usize> {
    program: &'a mut G18Program<N>,
    len: usize,
}
impl<'a, const N: usize> List<'a, N> {
    fn new(program: &'a mut G18Program<N>) -> Self {
        Self { program, len: 0 }
    }
    fn push(&mut self, number: u32, value: u64) -> Result<(), G18RenderError> {
        if self.len == N {
            return Err(G18RenderError::RegisterLayout);
        }
        self.program.registers[self.len] = RegisterWrite { number, value };
        self.len += 1;
        Ok(())
    }
    fn finish(self, rce_count: u16, command_offset: u32) -> Result<(), G18RenderError> {
        if self.len != N {
            return Err(G18RenderError::RegisterLayout);
        }
        self.program.rce_count = rce_count;
        self.program.command_offset = command_offset;
        Ok(())
    }
}

/// msaa163: the one multisample render the G18 path admits, Apple's 4x
/// render of run112 workload 8 (the MSAA scope
/// notes, section 2): 32x16 utiles and four samples (register 0x10009 =
/// 0x6002), the rotated-grid sample table 0xeaa26e26 (0x10019 / 3D +0x48),
/// eight-byte samples (3D +0x1fb0 = 8). Only `g18_application` admits it,
/// and only with `g18_admit_msaa4=1`. `p.samples` stays 1: that field is
/// the unused `set_samples` convention (32x32 utiles, Y-doubled geometry),
/// whereas the UAPI already names the halved utile.
pub(crate) const G18_MSAA4_UTILE: (u8, u8) = (32, 16);
pub(crate) const G18_MSAA4_UTILE_CONFIG: u64 = 0x6002;
pub(crate) const G18_MSAA4_MULTISAMPLE_CONTROL: u64 = 0xeaa2_6e26;
pub(crate) const G18_MSAA4_SAMPLES: u8 = 4;
pub(crate) const G18_MSAA4_SAMPLE_SIZE: u8 = 8;
/// Native331 RGBA32 two/four-sample renders use 16-byte samples, with the
/// same geometry and sample control.
pub(crate) const G18_MSAA_WIDE_SAMPLE_SIZE: u8 = 16;
/// Native464 retains three RGBA32 targets: 48-byte samples in a 96-KiB tile.
pub(crate) const G18_MSAA_MRT_SAMPLE_SIZE: u8 = 48;
/// Register 0x10051 / 3D +0x1f38: samples << 16 (native 0x40000 at 4x).
pub(crate) const G18_MSAA4_SAMPLE_CONTROL: u64 = (G18_MSAA4_SAMPLES as u64) << 16;
/// Partial-load list 0x10039 bit 16: native 0x200280 at 1x, 0x210280 at 4x.
pub(crate) const G18_MSAA4_LOAD_TILE_FLAG: u64 = 0x1_0000;

/// Native326: two samples retain a 32x32 utile and use diagonal positions.
pub(crate) const G18_MSAA2_UTILE: (u8, u8) = (32, 32);
pub(crate) const G18_MSAA2_UTILE_CONFIG: u64 = 0xa001;
pub(crate) const G18_MSAA2_MULTISAMPLE_CONTROL: u64 = 0x44cc;

pub(crate) fn g18_msaa2(p: &G17pRenderParameters) -> bool {
    (p.utile_width, p.utile_height) == G18_MSAA2_UTILE
        && p.utile_config == G18_MSAA2_UTILE_CONFIG
        && p.multisample_control == G18_MSAA2_MULTISAMPLE_CONTROL
        && p.samples == 1
        // Native673 combines the two-sample positions and 32x32 utiles with
        // layered target metadata and all five complete register lists.
        && (1..=2048).contains(&p.layers)
}

pub(crate) fn g18_multisample(p: &G17pRenderParameters) -> bool {
    g18_msaa2(p) || g18_msaa4(p)
}

/// Whether `p` is the admitted 4x render (see [`G18_MSAA4_UTILE`]).
pub(crate) fn g18_msaa4(p: &G17pRenderParameters) -> bool {
    (p.utile_width, p.utile_height) == G18_MSAA4_UTILE
        && p.utile_config == G18_MSAA4_UTILE_CONFIG
        && p.multisample_control == G18_MSAA4_MULTISAMPLE_CONTROL
        && p.samples == 1
        // Native618 compares layered 32x16 targets and all five register
        // lists with the same four-sample positions and partial-load mode.
        && (1..=2048).contains(&p.layers)
}

/// Rasterization samples (3D +0x50): 4 for the admitted 4x render, else the
/// existing `p.samples`.
pub(crate) fn g18_sample_count(p: &G17pRenderParameters) -> u32 {
    if g18_msaa4(p) {
        u32::from(G18_MSAA4_SAMPLES)
    } else if g18_msaa2(p) {
        2
    } else {
        u32::from(p.samples)
    }
}

/// 3D +0x54 macro size in utiles, (x << 16) | y: the register builders'
/// macro_size (g17_render.rs geometry). 4x splits the utile in Y, so y
/// doubles (native 0x40004 -> 0x40008); 32x32 renders keep the one-utile
/// formula unchanged.
pub(crate) fn g18_macro_size(p: &G17pRenderParameters) -> u32 {
    let tiles_x = (p.width + 31) / 32;
    let tiles_y = (p.height + 31) / 32;
    let macro_x = ((tiles_x + 3) / 4 + 3) & !3;
    let macro_y = ((tiles_y + 3) / 4 + 3) & !3;
    if g18_msaa4(p) {
        let utiles_x = 32 / u32::from(G18_MSAA4_UTILE.0);
        let utiles_y = 32 / u32::from(G18_MSAA4_UTILE.1);
        ((macro_x * utiles_x) << 16) | (macro_y * utiles_y)
    } else {
        (macro_x << 16) | macro_y
    }
}

fn validate(p: &G17pRenderParameters) -> Result<(), G18RenderError> {
    if !(1..=16384).contains(&p.width)
        || !(1..=16384).contains(&p.height)
        || !(1..=2048).contains(&p.layers)
        || p.samples != 1
        || (!g18_multisample(p)
            && ((p.utile_width, p.utile_height) != (32, 32)
                || p.utile_config != 0xa000
                || p.multisample_control != 0x88))
    {
        return Err(G18RenderError::UnsupportedGeometry);
    }
    if p.sampler_count != 0
        || p.sampler_array != 0
        || p.occlusion_query_base & 0xf != 0
        || p.ta_timestamp_start != 0
        || p.ta_timestamp_end != 0
        || p.fragment_timestamp_start != 0
        || p.fragment_timestamp_end != 0
        || p.absolute_pointers
        || p.native_ta_registers != 0
        || p.bg_resource_prefix != 0
        || (p.layers == 1 && (p.stencil_aux_buffer != 0
            || p.stencil_stride != 0 || p.stencil_aux_stride != 0))
        || p.stencil_clear_value > 0xff
    {
        return Err(G18RenderError::UnsupportedFeature);
    }
    Ok(())
}

fn m5_controls(c: &G18RenderControls) -> M5RenderControls {
    let stage = |s: G18RenderStageControls| M5RenderStageControls {
        resource_flags: 0,
        resource_control: 0,
        ptc_max: 0,
        work_key: s.work_key,
    };
    M5RenderControls {
        ta: stage(c.ta),
        fragment: stage(c.fragment),
        ta_tile_control: c.ta_tile_control,
        ta_dispatch_control: c.ta_dispatch_control,
        fragment_raster_control: c.fragment_raster_control,
        fragment_tile_flags: c.fragment_tile_flags,
        store_pipeline_2_bind: c.store_pipeline_2_bind,
        store_pipeline_2: c.store_pipeline_2,
        partial_store_pipeline_2_bind: 0,
        partial_store_pipeline_2: 0,
        partial_tile_flags: c.partial_tile_flags,
    }
}

/// Convert ONLY compact-address inputs for the shared M5 arithmetic. The
/// emitted values remain the caller's G18 view; no physical alias is invented.
fn m5_parameters(p: &G17pRenderParameters) -> Result<G17pRenderParameters, G18RenderError> {
    validate(p)?;
    if p.encoder >= 1 << 39
        || p.encoder & 3 != 0
        || [p.deflake_1, p.deflake_2, p.deflake_3]
            .iter()
            .any(|&v| v > u32::MAX as u64 || v & 3 != 0)
    {
        return Err(G18RenderError::CompactAddress);
    }
    let mut m = *p;
    m.encoder += 0x10_0000_0000;
    m.deflake_1 += 0x10_0000_0000;
    m.deflake_2 += 0x10_0000_0000;
    m.deflake_3 += 0x10_0000_0000;
    Ok(m)
}

#[inline(never)]
fn common_ta(
    p: &G17pRenderParameters,
    c: &M5RenderControls,
) -> Result<[RegisterWrite; 82], m5_render::M5RenderBuildError> {
    m5_render::build_ta_registers(p, c)
}

#[inline(never)]
fn common_fragment(
    p: &G17pRenderParameters,
    c: &G18RenderControls,
) -> Result<[RegisterWrite; 90], m5_render::M5RenderBuildError> {
    m5_render::build_fragment_registers(p, &m5_controls(c))
}

#[inline(never)]
pub(crate) fn build_g18_ta_program(
    p: &G17pRenderParameters,
    c: &G18RenderControls,
) -> Result<G18Program<91>, G18RenderError> {
    let mut program = G18Program::empty();
    build_g18_ta_program_into(&mut program, p, c)?;
    Ok(program)
}

/// H1 writes into heap-owned storage rather than nesting return-value arrays.
#[inline(never)]
pub(crate) fn build_g18_ta_program_into(program: &mut G18Program<91>,
    p: &G17pRenderParameters, c: &G18RenderControls) -> Result<(), G18RenderError> {
    let common = common_ta(&m5_parameters(p)?, &m5_controls(c))?;
    let mut out = List::new(program);
    for r in &common {
        let (mut n, mut v) = (r.number, r.value);
        match n {
            0x1748 | 0xd499 | 0xd419 | 0xa5a1 | 0xd471 | 0x14320 | 0x1740 => continue,
            0x1c8f8 => out.push(0xa5e9, c.ta.resource_word)?,
            // Fixed J873 device capability, independent of user PPP flags.
            // Native device +6b0 bit 16 is emitted as bit 9 (8c4f004).
            0x10121 => v |= 0x200,
            0x1c079 | 0x1c9d8 => {
                v = p
                    .layermeta
                    .checked_sub(p.context_base)
                    .ok_or(G18RenderError::CompactAddress)?
            }
            0x1a099 | 0x1a0a1 | 0x1a069 | 0x1a071 | 0x1a0c9 | 0x1a0d1 => n -= 0x10,
            0x1a0f1 => n = 0x1a0e1,
            0x1a149 => {
                n = 0x1a111;
                v = 1;
            }
            // Native owner +28 masks the TA cycle/scene slot to 64 bytes.
            0x1ca30 | 0x16c39 => v &= !63,
            0x14a1 => n = 0x11201,
            0x14308 => n = 0x10249,
            0x14318 => {
                n = 0xa619;
                v = c.ta.usc_statistics;
            }
            _ => {}
        }
        out.push(n, v)?;
        match n {
            0x1c9c8 => {
                out.push(0x1c041, c.ta_cluster_tilemap)?;
                out.push(0x1c9d0, c.ta_cluster_tilemap)?;
            }
            0x1c880 if out.len < 30 => {
                out.push(0x1c948, c.ta_cluster_meta[0])?;
                out.push(0x1c888, c.ta_cluster_meta[1])?;
                out.push(0x1c890, c.ta_cluster_control[0])?;
                out.push(0x1c918, c.ta_cluster_control[1])?;
            }
            0x1c9d8 => {
                out.push(0x1c089, c.ta_cluster_layermeta)?;
                out.push(0x1c9e0, c.ta_cluster_layermeta)?;
                out.push(0x16c41, c.ta_cluster_meta[2])?;
                out.push(0x1ca40, c.ta_cluster_meta[2])?;
                out.push(0x1c9a8, c.ta_cluster_control[2])?;
                out.push(0x1c920, c.ta_cluster_meta[3])?;
            }
            0x10169 => {
                for (n, v) in [0xa109, 0xa129, 0xa149].iter().zip(c.ta.address_windows) {
                    out.push(*n, v)?;
                }
            }
            _ => {}
        }
    }
    // Native descriptor +1608; the final three writes are outside the RCE tag.
    out.finish(88, 0x60)?;
    Ok(())
}

#[inline(never)]
fn fragment_main(
    program: &mut G18Program<92>,
    p: &G17pRenderParameters,
    c: &G18RenderControls,
) -> Result<(), G18RenderError> {
    validate(p)?;
    let common = common_fragment(p, c)?;
    let mut out = List::new(program);
    let mut tiles = 0;
    for r in &common {
        let (mut n, mut v) = (r.number, r.value);
        match n {
            0xd491 | 0xd429 | 0xa5a9 | 0xd469 => continue,
            0x1739 => n = 0x8139,
            0x1731 => n = 0x8131,
            0x10791 => {
                for (n, v) in [0xa101, 0xa121, 0xa141]
                    .iter()
                    .zip(c.fragment.address_windows)
                {
                    out.push(*n, v)?;
                }
            }
            0x15321 => out.push(0xa5f1, c.fragment.resource_word)?,
            0x16060 => v = p.layermeta,
            0x16138 => v = c.fragment_tile_control,
            0x101e9 => v = c.fragment_control & 0x7f,
            0x10051 => v = c.fragment_sample_control,
            0x1ca28 => v &= !15,
            0x16068 => {
                tiles += 1;
                out.push(
                    0x160a8,
                    c.fragment_dispatch_control | if tiles == 2 { 0x20 } else { 0 },
                )?;
            }
            0x1a0a9 | 0x1a0b1 | 0x1a079 | 0x1a081 | 0x1a0d9 | 0x1a0e1 => n -= 0x10,
            0x1a0f9 => n = 0x1a0e9,
            0x1a151 => {
                n = 0x1a119;
                v = 1;
            }
            0x1499 => n = 0x111f9,
            0x14048 => n = 0x10251,
            0x14080 => {
                n = 0xa621;
                v = c.fragment.usc_statistics;
            }
            _ => {}
        }
        out.push(n, v)?;
    }
    out.finish(88, 0xa0)
}

#[inline(never)]
pub(crate) fn build_g18_fragment_programs(
    p: &G17pRenderParameters,
    c: &G18RenderControls,
) -> Result<G18FragmentPrograms, G18RenderError> {
    let mut programs = G18FragmentPrograms {
        main: G18Program::empty(),
        store: G18Program::empty(),
        resume: G18Program::empty(),
        load: G18Program::empty(),
    };
    build_g18_fragment_programs_into(&mut programs, p, c)?;
    Ok(programs)
}

#[inline(never)]
pub(crate) fn build_g18_fragment_programs_into(programs: &mut G18FragmentPrograms,
    p: &G17pRenderParameters, c: &G18RenderControls) -> Result<(), G18RenderError> {
    fragment_main(&mut programs.main, p, c)?;
    fragment_partials(programs, p, c)
}

#[inline(never)]
fn fragment_partials(
    programs: &mut G18FragmentPrograms,
    p: &G17pRenderParameters,
    c: &G18RenderControls,
) -> Result<(), G18RenderError> {
    let mut mc = m5_controls(c);
    mc.partial_store_pipeline_2_bind = p.partial_store_pipeline_bind;
    mc.partial_store_pipeline_2 = p.partial_store_pipeline;
    fn partial<const N: usize>(
        r: &[RegisterWrite],
        offset: u32,
    ) -> Result<G18Program<N>, G18RenderError> {
        let mut program = G18Program::empty();
        let mut out = List::new(&mut program);
        for r in r {
            if r.number != 0x15359 {
                out.push(r.number, r.value)?;
            }
        }
        out.finish(N as u16, offset)?;
        Ok(program)
    }
    let mut partial_p = *p;
    // G18 helper 8c4c1d0 drops main-pass +8d2/+8d3 flags for a split.
    partial_p.tile_config &= 0x1ffff;
    partial_p.partial_depth_flags = p.depth_flags;
    let mut load_mc = mc;
    // Original G18 partial-load helper reads +790/+798 (main EOT), whereas
    // store/resume read +7b0/+7b8. M5 used the partial pair for all three.
    load_mc.partial_store_pipeline_2_bind = p.store_pipeline_bind;
    load_mc.partial_store_pipeline_2 = p.store_pipeline;
    programs.store = partial(
        &m5_render::build_partial_store_registers(&partial_p, &mc)?,
        0x7c0,
    )?;
    programs.resume = partial(&m5_render::build_partial_resume_registers(p, &mc)?, 0xee0)?;
    programs.load = partial(
        &m5_render::build_partial_load_registers(p, &load_mc)?,
        0x1600,
    )?;
    // msaa163: native 4x keeps tile_config bit 16 in the partial-load list
    // (0x10039 = 0x210280 against 0x200280 at 1x, MSAA notes s2.1; the M5
    // 4x load word +0x161c moves the same way). Store/resume do not move.
    if g18_multisample(p) {
        // Native326 retains the same partial-load flag at two samples.
        for register in &mut programs.load.registers {
            if register.number == 0x10039 {
                register.value |= G18_MSAA4_LOAD_TILE_FLAG;
            }
        }
    }
    // Native G18 partial helpers retain main stencil flags and add the
    // transfer required by each program when a plane is present. The
    // ordinary main list already forwards these fields unchanged.
    fn stencil_flags<const N: usize>(program: &mut G18Program<N>,
        p: &G17pRenderParameters, transfer: u64, store: bool) {
        let mask = (1 << 4) | (1 << 8) | (1 << 14) | (1 << 18);
        let flags = (p.depth_flags & mask)
            | if p.stencil_buffer != 0 { transfer } else { 0 }
            | if store && p.stencil_aux_buffer != 0 { 1 << 8 } else { 0 };
        for register in &mut program.registers {
            if register.number == 0x15319 {
                register.value = (register.value & !mask) | flags;
            }
        }
    }
    stencil_flags(&mut programs.store, p, 1 << 18, true);
    stencil_flags(&mut programs.resume, p, (1 << 14) | (1 << 18), true);
    stencil_flags(&mut programs.load, p, 1 << 14, false);
    Ok(())
}

pub(crate) fn g18_default_controls(p: &G17pRenderParameters) -> G18RenderControls {
    let stage = G18RenderStageControls {
        resource_word: 0,
        address_windows: [0xffff_0000_0000, 0x70_0000_0000, 0x71_0000_0000],
        work_key: 0,
        usc_statistics: 0,
    };
    G18RenderControls {
        ta: stage,
        fragment: stage,
        ta_tile_control: 0x10c08860,
        ta_dispatch_control: 0x700,
        fragment_raster_control: 0x200000ff03ef,
        fragment_tile_flags: 0x20_0003_6011,
        fragment_tile_control: 0x8860,
        // Original a/b/g1 user descriptors and working native run 112.
        fragment_control: 0x1c,
        // Capture parameters already carry native sample control here.
        fragment_sample_control: p.tib_blocks,
        fragment_dispatch_control: 0,
        store_pipeline_2_bind: p.store_pipeline_bind,
        store_pipeline_2: p.store_pipeline,
        partial_tile_flags: 0x200000,
        ta_cluster_tilemap: 0,
        ta_cluster_layermeta: 0,
        ta_cluster_meta: [0; 4],
        ta_cluster_control: [0; 3],
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G18Capture {
    ColorA,
    ColorB,
    GearsG1,
    FragmentWitness,
}

/// Captured user fields plus explicitly unallocated kernel-owned fields.
/// The job MUST replace resource addresses, IDs, cycle/metrics and policy.
/// Geometry is constructed for the capture's 256x256 single-layer target;
/// native oracle parity injects that same geometry into the owned RT loader.
pub(crate) fn g18_capture_parameters(
    capture: G18Capture,
) -> (G17pRenderParameters, G18RenderControls) {
    let mut p = G17pRenderParameters::source_rsrc8();
    p.width = 256;
    p.height = 256;
    p.context_base = 0;
    p.tilemap = 0;
    p.layermeta = 0;
    p.heapmeta = 0;
    p.tpc = 0;
    p.deflake_1 = 0x68980;
    p.deflake_2 = 0x680c0;
    p.deflake_3 = 0x68000;
    p.encoder = 0x18000;
    p.ta_status = 0;
    p.fragment_status = 0;
    p.store_pipeline_bind = 0;
    p.partial_store_pipeline_bind = 0;
    p.load_pipeline_bind = 0xffff800000000040;
    p.partial_load_pipeline_bind = p.load_pipeline_bind;
    let (shader, aux, id) = match capture {
        G18Capture::ColorA => (0x10000168000, 0x10000280000, 0x0e0260a2u64),
        G18Capture::ColorB => (0x10000168000, 0x10000280000, 0x1c0268ba),
        G18Capture::GearsG1 => (0x100001b0000, 0x100002c8000, 0x4a13d3dc),
        G18Capture::FragmentWitness => (0x10000170000, 0x10000288000, 0x0c0060a2),
    };
    p.load_pipeline = shader + 0x240;
    p.partial_load_pipeline = shader;
    p.store_pipeline = shader + 0x480;
    p.partial_store_pipeline = p.store_pipeline;
    p.scissor_array = 0;
    p.depth_bias_array = 0;
    p.aux_fb = aux;
    p.utile_config = 0xa000;
    p.multisample_control = 0x88;
    // Native J873 device +6b0 bit 16 becomes PPP bit 9 (8c4f004).
    // Keep the same 0x202 used by live native draws.
    p.ppp_control = 0x202;
    p.tib_blocks = 0;
    p.tile_config = 0xc10280;
    p.aux_fb_flags = if capture == G18Capture::GearsG1 {
        0x30c000
    } else {
        0x30c001
    };
    p.cycle = 0;
    p.record_index = 0x7000000001;
    p.lifecycle = id << 32;
    p.fragment_lifecycle = id << 32;
    p.merge_upper_x_bits = 0;
    p.merge_upper_y_bits = 0;
    p.ta_timestamp_start = 0;
    p.ta_timestamp_end = 0;
    p.fragment_timestamp_start = 0;
    p.fragment_timestamp_end = 0;
    if capture == G18Capture::GearsG1 {
        p.depth_buffer = 0x100000c8000;
        p.depth_aux_buffer = 0x10000108000;
        p.depth_flags = 0x80044;
        p.partial_depth_flags = 0x88044;
        p.depth_dimensions = 0xff00ff;
    }
    let c = g18_default_controls(&p);
    (p, c)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderTargetLayout {
    pub(crate) tilemap_bytes: usize,
    pub(crate) heapmeta_offset: usize,
    pub(crate) heapmeta_bytes: usize,
    pub(crate) target_bytes: usize,
    pub(crate) tpc_bytes: usize,
    pub(crate) deflake_bytes: usize,
    pub(crate) aux_bytes: usize,
}

/// Original G18 geometry and updateGtpOnlyBuffers packing for one layer,
/// 32x32 utiles and one sample. `kind` selects clustering, not physical
/// cluster count: native TPC sizing still uses both J873 clusters for kind 1.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18NativeTargetLayout {
    pub layout: G18RenderTargetLayout,
    pub slice_sizes: [usize; 8],
    pub slice_offsets: [usize; 8],
    pub cluster_control: [u64; 3],
}

/// Native target layout for one layer. msaa163: `utile` is the render's
/// utile. The tilemap and the TPC slice scale with utiles per 32x32 tile, the
/// M5 rule (`m5_render_target.rs:36-43`) and the same term that doubles
/// size1/size3 (TA 0x1c0b1/0x1c850/0x1c0a9). Native 4x (32x16) measures TA
/// +0x788 = 0x2000 against 0x1000 at 1x (MSAA notes s2.2). Only the witnessed
/// 32x16 single-target layout (kind 1) is admitted besides 32x32, which is
/// unchanged.
pub(crate) fn g18_native_target_layout(w: u32, h: u32, kind: u32, utile: (u8, u8))
    -> Option<G18NativeTargetLayout> {
    g18_native_target_layout_layers(w, h, 1, kind, utile)
}

/// Original G18 target preparation also allocates a tilemap and TPC for each
/// layer. Layer metadata is one separate 0x100-byte region, not one per layer.
/// Native598 executes the original parser and preparation through 2048 layers,
/// including the 32x16 four-sample kind-1 geometry. Submission admission remains
/// separate: allocation arithmetic alone does not qualify layered rendering.
pub(crate) fn g18_native_target_layout_layers(
    w: u32, h: u32, layers: u16, kind: u32, utile: (u8, u8),
) -> Option<G18NativeTargetLayout> {
    if !(1..=16384).contains(&w) || !(1..=16384).contains(&h)
        || !(1..=2048).contains(&layers) || !(1..=2).contains(&kind) {
        return None;
    }
    let utiles = match utile {
        (32, 32) => 1,
        G18_MSAA4_UTILE if kind == 1 => (32 / G18_MSAA4_UTILE.0 as usize)
            * (32 / G18_MSAA4_UTILE.1 as usize),
        _ => return None,
    };
    let mx = ((((w as usize + 31) / 32 + 3) / 4) + 3) & !3;
    let my = ((((h as usize + 31) / 32 + 3) / 4) + 3) & !3;
    let layers = usize::from(layers);
    let tilemap = (5usize * 16 * mx * my * utiles).checked_mul(layers)?;
    let layermeta_bytes = if layers > 1 { 0x100usize } else { 0 };
    // Explicit experiment policy, not inferred live macOS configuration:
    // segment mode 4 (native table gives 64), range differences zero, 8 PB
    // blocks of 512 KiB. The original generators receive these same inputs.
    let mut slice_sizes = [0; 8];
    slice_sizes[0] = (2usize * 128 * mx * my * utiles).checked_mul(layers)?;
    if kind == 2 {
        slice_sizes[1] = tilemap.checked_mul(64)?;
        slice_sizes[2] = layermeta_bytes * 64;
        slice_sizes[4] = 0x680 + 2 * 0x300;
        slice_sizes[5] = 2 * 0x5c0;
        slice_sizes[6] = (((0x400000usize / 0x8000 + 0x17ff) / 0x1800) + 1) * 8 * 64;
        slice_sizes[7] = 0x80;
    }
    let alignments = if kind == 2 { [0x40, 0x40, 0x100, 0, 0x40, 0x40, 0x80, 0x40] }
        else { [0x40, 0, 0, 0, 0, 0, 0, 0] };
    let mut slice_offsets = [0; 8];
    let mut bytes = 0usize;
    for n in 0..8 {
        let align = alignments[n];
        if align != 0 { bytes = bytes.checked_add(align - 1)? & !(align - 1); }
        slice_offsets[n] = bytes;
        bytes = bytes.checked_add(slice_sizes[n])?;
    }
    let tilemap_bytes = tilemap.checked_add(0xfff)? & !0xfff;
    let heapmeta_offset = tilemap_bytes.checked_add(layermeta_bytes)?;
    let target_bytes = heapmeta_offset.checked_add(0x200 + 0x3fff)? & !0x3fff;
    Some(G18NativeTargetLayout {
        layout: G18RenderTargetLayout { tilemap_bytes, heapmeta_offset,
            heapmeta_bytes: 0x200, target_bytes,
            tpc_bytes: bytes.checked_add(0x3fff)? & !0x3fff, deflake_bytes: 0xc000, aux_bytes: 0x10000 },
        slice_sizes, slice_offsets,
        cluster_control: if kind == 1 { [1, 0, 0x80] } else { [0x40, 4, 0x80] },
    })
}

/// Correctly rounded positive IEEE-754 division of 0x3fddb3d9 by a target
/// dimension. Integer arithmetic keeps the kernel free of FP/SIMD use.
pub(crate) fn g18_merge_coefficient(dimension: u32) -> Option<u32> {
    if !(1..=16384).contains(&dimension) { return None; }
    let b = 31 - dimension.leading_zeros();
    let shift = b + u32::from(0xddb3d9u64 < (u64::from(dimension) << (23 - b)));
    let numerator = 0xddb3d9u64 << shift;
    let denominator = u64::from(dimension);
    let mut mantissa = numerator / denominator;
    let remainder = numerator % denominator;
    if remainder * 2 > denominator || (remainder * 2 == denominator && mantissa & 1 != 0) {
        mantissa += 1;
    }
    Some(((127 - shift) << 23) + mantissa as u32 - (1 << 23))
}

/// Conservative 2x M5 storage pending native G18 geometry qualification.
/// Only H1/H2 geometry is admitted; clusters must be one or two.
pub(crate) fn g18_render_target_layout(
    w: u32,
    h: u32,
    layers: u16,
    utile: (u8, u8),
    samples: u8,
    clusters: u32,
) -> Option<G18RenderTargetLayout> {
    if layers != 1 || utile != (32, 32) || samples != 1 || !(1..=2).contains(&clusters) {
        return None;
    }
    let m = m5_render_target::M5RenderTargetLayout::new(w, h, layers, utile.0, utile.1, clusters)?;
    let tilemap_bytes = m.tilemap_bytes.checked_mul(2)?;
    let heapmeta_offset = tilemap_bytes.checked_add(0xfff)? & !0xfff;
    let heapmeta_bytes = m.heapmeta_bytes.checked_mul(2)?;
    let target_bytes = core::cmp::max(
        m.target_bytes.checked_mul(2)?,
        heapmeta_offset
            .checked_add(heapmeta_bytes)?
            .checked_add(0x3fff)?
            & !0x3fff,
    );
    Some(G18RenderTargetLayout {
        tilemap_bytes,
        heapmeta_offset,
        heapmeta_bytes,
        target_bytes,
        tpc_bytes: m.tpc_bytes.checked_mul(2)?,
        deflake_bytes: 0xc000,
        aux_bytes: 0x10000,
    })
}

/// Full 0xc000-byte captured G18 deflake allocation. Viewport is at +2c00,
/// not M5's +9c0. Both colour and gears captures witness these eight dwords.
pub(crate) fn initialize_g18_deflake(page: &mut [u8], w: u32, h: u32) -> Option<()> {
    if page.len() != 0xc000 || !(1..=16384).contains(&w) || !(1..=16384).contains(&h) {
        return None;
    }
    fn half(v: u32) -> u32 {
        let b = 31 - v.leading_zeros();
        ((b + 126) << 23) | ((v << (23 - b)) & 0x7fffff)
    }
    page.fill(0);
    for (off, v) in [
        (0x2c00, 0x3000),
        (0x2c04, 0x80000000 | ((w + 31) / 32 - 1)),
        (0x2c08, (h + 31) / 32 - 1),
        (0x2c10, half(w)),
        (0x2c14, half(w)),
        (0x2c18, half(h)),
        (0x2c1c, half(h) | 0x80000000),
        (0x2c24, 0x3f800000),
    ] {
        page[off..off + 4].copy_from_slice(&v.to_le_bytes());
    }
    Some(())
}
