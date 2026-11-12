// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Pure runtime assembly shared by the kernel owner and the host dry-run.
//! Register arithmetic belongs to g18_render; command fields to render-cmd.
#![allow(dead_code)]
use crate::{g18::fw::render_params::G17pRenderParameters, g18::address_map as va,
    g18::fw::graphics_payload::fnv64, g18::fw::render as regs, g18::fw::render_command as cmd,
    g18::fw::sksm::M5SksmRce};

pub(crate) const TA_OFFSET: usize = 0;
pub(crate) const FRAGMENT_OFFSET: usize = 0x4000;
pub(crate) const TARGET_OFFSETS: [u64; 6] = [0, 0x20000, 0x40000, 0x50000, 0x60000, 0x80000];
// Fixed virtual reservations, not eager allocations. Native tilemap/TPC sizes
// scale with layers: even a 32x32x64 target needs a 0x40000-byte TPC. Keep
// addresses stable when an application grows an already-owned target.
const NATIVE_TARGET_OFFSETS: [u64; 6] =
    [0, 0x400000, 0xc00000, 0xc10000, 0xc20000, 0xc30000];
pub(crate) const TARGET_NAMES: [&str; 6] = ["tilemap-heapmeta", "tpc", "deflake", "ta-status", "3d-status", "auxiliary"];
pub(crate) const DIGEST_NAMES: [&str; 7] = ["ta-command", "3d-command", "ta-registers",
    "3d-main", "3d-store", "3d-resume", "3d-load"];

#[derive(Clone, Copy, Debug)]
pub(crate) struct TargetPlan {
    pub layout: regs::G18RenderTargetLayout,
    pub sizes: [usize; 6],
    pub offsets: [u64; 6],
    pub native: Option<regs::G18NativeTargetLayout>,
}

pub(crate) fn target_plan(p: &G17pRenderParameters) -> Option<TargetPlan> {
    target_plan_for_kind(p, crate::g18::uapi::render_policy::target_kind())
}

pub(crate) fn target_plan_for_kind(p: &G17pRenderParameters, kind: u32) -> Option<TargetPlan> {
    // msaa163: the admitted 4x render (32x16 utiles) plans only with the
    // native single-target layout, whose TPC/tilemap scale with its utiles;
    // the legacy (kind 0) and clustered (kind 2) layouts have no 4x witness.
    let msaa4 = regs::g18_msaa4(p);
    if !(1..=2048).contains(&p.layers) || p.samples != 1
        || (!msaa4 && (p.utile_width, p.utile_height) != (32, 32))
        || (msaa4 && kind != 1) {
        return None;
    }
    let native = if kind == 0 { None } else {
        Some(regs::g18_native_target_layout_layers(p.width, p.height, p.layers, kind,
            (p.utile_width, p.utile_height))?)
    };
    let layout = if let Some(n) = native { n.layout } else {
        regs::g18_render_target_layout(p.width, p.height, p.layers,
            (p.utile_width, p.utile_height), p.samples, 2)?
    };
    // Stable VAs across application resizes. The extra slices share the TPC
    // allocation; reserve 4 MiB for it, covering 1280x720 without overlap.
    let offsets = match kind {
        1 => NATIVE_TARGET_OFFSETS,
        2 => [0, 0x100000, 0x500000, 0x510000, 0x520000, 0x600000],
        _ => TARGET_OFFSETS,
    };
    let sizes = [layout.target_bytes, layout.tpc_bytes, layout.deflake_bytes,
        0x4000, 0x4000, layout.aux_bytes];
    for (i, size) in sizes.iter().enumerate() {
        let end = offsets[i].checked_add(*size as u64)?;
        if i + 1 < sizes.len() && end > offsets[i + 1] { return None; }
        if end > va::GRAPHICS_TARGET_BYTES as u64 { return None; }
    }
    Some(TargetPlan { layout, sizes, offsets, native })
}

/// Native 3D uses full target VAs; TA subtracts the mapping's context base.
/// The owner maps identical backing at the compact and biased views.
pub(crate) fn apply_target_plan(p: &mut G17pRenderParameters, plan: &TargetPlan) {
    let a = plan.offsets.map(|offset| va::GRAPHICS_TARGET_OFFSET + offset);
    p.context_base = va::COMPACT_BIAS;
    p.tilemap = va::COMPACT_BIAS + a[0];
    p.heapmeta = p.tilemap + plan.layout.heapmeta_offset as u64;
    // Native target preparation places layer metadata directly after the
    // rounded tilemap; the heap follows it. Legacy kind 0 keeps its layout.
    p.layermeta = if plan.native.is_some() {
        p.tilemap + plan.layout.tilemap_bytes as u64
    } else { p.heapmeta };
    p.tpc = va::COMPACT_BIAS + a[1];
    // Deflake has a separate task-address encoding; keep these compact.
    p.deflake_3 = a[2]; p.deflake_2 = a[2] + 0xc0; p.deflake_1 = a[2] + 0x980;
    p.ta_status = a[3]; p.fragment_status = a[4]; p.aux_fb = a[5];
    // Generator does not admit timestamp registers yet; command timestamp
    // pointers are separately supplied by the owned command inputs.
    p.ta_timestamp_start = 0; p.ta_timestamp_end = 0;
    p.fragment_timestamp_start = 0; p.fragment_timestamp_end = 0;
}

pub(crate) fn apply_target_controls(p: &mut G17pRenderParameters,
    c: &mut regs::G18RenderControls, plan: &TargetPlan) {
    if let Some(n) = plan.native {
        let pointer = |i: usize| if n.slice_sizes[i] == 0 { 0 }
            else { p.tpc - p.context_base + n.slice_offsets[i] as u64 };
        c.ta_cluster_tilemap = pointer(1);
        c.ta_cluster_layermeta = pointer(2);
        c.ta_cluster_meta = [pointer(4), pointer(5), pointer(6), pointer(7)];
        c.ta_cluster_control = n.cluster_control;
        p.merge_upper_x_bits = regs::g18_merge_coefficient(p.width).expect("validated width");
        p.merge_upper_y_bits = regs::g18_merge_coefficient(p.height).expect("validated height");
    }
}

#[derive(Debug)]
pub(crate) enum AssemblyError { Registers(regs::G18RenderError), Command(cmd::G18RenderCommandError) }
impl From<regs::G18RenderError> for AssemblyError { fn from(e: regs::G18RenderError) -> Self { Self::Registers(e) } }
impl From<cmd::G18RenderCommandError> for AssemblyError { fn from(e: cmd::G18RenderCommandError) -> Self { Self::Command(e) } }

pub(crate) struct EncodedJob { pub rce: [[M5SksmRce; 4]; 2], pub digests: [u64; 7] }

// Only integer fields: zero is a valid initial representation. Kept on the
// heap by the kernel caller, including across the register-builder calls.
pub(crate) struct Workspace { pub ta: regs::G18Program<91>, pub fragment: regs::G18FragmentPrograms }

fn view<const N: usize>(p: &regs::G18Program<N>) -> cmd::G18ProgramRef<'_> {
    cmd::G18ProgramRef { registers: &p.registers, rce_count: p.rce_count, command_offset: p.command_offset }
}

#[inline(never)]
#[cfg(test)]
pub(crate) fn encode(
    ta: &mut [u8; cmd::G18_TA_COMMAND_BYTES], fragment: &mut [u8; cmd::G18_3D_COMMAND_BYTES],
    p: &G17pRenderParameters, controls: &regs::G18RenderControls,
    inputs: &cmd::G18RenderCommandInputs,
) -> Result<EncodedJob, AssemblyError> {
    let mut workspace = Workspace { ta: regs::G18Program::empty(), fragment: regs::G18FragmentPrograms {
        main: regs::G18Program::empty(), store: regs::G18Program::empty(),
        resume: regs::G18Program::empty(), load: regs::G18Program::empty() } };
    encode_into(ta, fragment, p, controls, inputs, &mut workspace)
}

#[inline(never)]
pub(crate) fn encode_into(
    ta: &mut [u8; cmd::G18_TA_COMMAND_BYTES], fragment: &mut [u8; cmd::G18_3D_COMMAND_BYTES],
    p: &G17pRenderParameters, controls: &regs::G18RenderControls,
    inputs: &cmd::G18RenderCommandInputs, workspace: &mut Workspace,
) -> Result<EncodedJob, AssemblyError> {
    ta.fill(0); fragment.fill(0);
    regs::build_g18_ta_program_into(&mut workspace.ta, p, controls)?;
    regs::build_g18_fragment_programs_into(&mut workspace.fragment, p, controls)?;
    let t = &workspace.ta;
    let f = &workspace.fragment;
    // Native main boundaries N-4 / N-2 / N, per render-cmd §2.
    let programs = cmd::G18FragmentProgramRefs { main: view(&f.main), main_mid_count: 90,
        store: view(&f.store), resume: view(&f.resume), load: view(&f.load) };
    let fr_rce = cmd::encode_g18_3d_command(fragment,
        va::GRAPHICS_JOB_LOW + FRAGMENT_OFFSET as u64, inputs, &programs)?;
    // The linked 3D tuple in inputs is finalized before encoding TA.
    let ta_rce = cmd::encode_g18_ta_command(ta, va::GRAPHICS_JOB_LOW, inputs, &view(&t))?;
    let digests = [fnv64(ta), fnv64(fragment), fnv64(&ta[0x60..0x60 + 91 * 12]),
        fnv64(&fragment[0xa0..0xa0 + 92 * 12]), fnv64(&fragment[0x7c0..0x7c0 + 17 * 12]),
        fnv64(&fragment[0xee0..0xee0 + 24 * 12]), fnv64(&fragment[0x1600..0x1600 + 12 * 12])];
    Ok(EncodedJob { rce: [ta_rce, fr_rce], digests })
}

/// Runtime variants are deliberately a closed set. H2 repeats three frames;
/// no caller may infer retirement from an output hash before the next frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RenderVariant { Color, TaOnly, Clear, GearsThreeFrames }
impl RenderVariant {
    pub(crate) fn parse(value: u32) -> Option<Self> { match value {
        0 => Some(Self::Color), 1 => Some(Self::TaOnly), 2 => Some(Self::Clear),
        3 => Some(Self::GearsThreeFrames), _ => None,
    } }
    pub(crate) fn stage_count(self) -> u32 { if self == Self::TaOnly { 1 } else { 2 } }
    pub(crate) fn frames(self) -> u32 { if self == Self::GearsThreeFrames { 3 } else { 1 } }
}
