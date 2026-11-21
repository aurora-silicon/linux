// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5 render admission, target layout and command inputs on the shared submission path.

use kernel::prelude::*;
use crate::g17::fw::m5::{render as cmd, render_registers as regs};
use crate::g17::{buffer::target, command::Validated, fw::render::{RenderArgs, RenderPass, RegisterPrograms}};

/// Compressed single-layer MSAA ZLS admission: D32 depth with optional plain
/// stencil, and the compressed Z/S form.
pub(crate) fn depth_msaa_compression_enabled() -> bool {
    crate::module_parameters::m5_depth_msaa_compression.value() == 1
}

pub(crate) fn validate(pass: &RenderPass) -> Result {
    // The hardware carries the sample stride in eight bits. The 64-byte limit
    // belonged to the first qualified profile, not the M5 imageblock.
    let single_sample = pass.samples_log2 == 0
        && matches!((pass.utile_width, pass.utile_height),
                    (32, 32) | (32, 16) | (16, 16))
        && pass.sample_size % 8 == 0
        && u32::from(pass.sample_size) * u32::from(pass.utile_width)
            * u32::from(pass.utile_height) <= 65536
        && (pass.sample_size != 0 || pass.layers == 1);
    // ZLS addresses/actions and page strides are sample-independent.
    // Retain the checked plain layout; compression is not admitted here,
    // except in the layered compressed Z/S form (every enabled Z/S load or
    // store compressed, a compression address per compressed plane, depth
    // and stencil strides as supplied by userspace). The same form in
    // single-layer 2x/4x MSAA depth/stencil passes is admitted only with the
    // opt-in.
    let plain_zls = pass.zls_control == 0
        || (pass.zls_control & 0x154 == 0
            && pass.depth.compression_va == 0 && pass.stencil.compression_va == 0);
    let multisample = (plain_zls || layered_compressed_zls(pass)
        || (depth_msaa_compression_enabled() && (single_layer_compressed_depth_zls(pass)
            || single_layer_compressed_zls(pass))))
        && matches!(pass.samples_log2, 1..=3)
        && matches!((pass.utile_width, pass.utile_height),
                    (32, 32) | (32, 16) | (16, 16))
        && pass.sample_size >= 8 && pass.sample_size % 8 == 0
        && (u32::from(pass.utile_width) * u32::from(pass.utile_height)
            << pass.samples_log2) <= 2048
        && (u32::from(pass.sample_size) * u32::from(pass.utile_width)
            * u32::from(pass.utile_height) << pass.samples_log2) <= 65536;
    if !(single_sample || multisample)
        || (pass.zls_control != 0 && pass.depth.va == 0 && pass.stencil.va == 0) {
        return Err(EOPNOTSUPP);
    }
    Ok(())
}

/// Layered multisample depth/stencil in the compressed Z/S form.
fn layered_compressed_zls(pass: &RenderPass) -> bool {
    pass.layers > 1 && compressed_zls_form(pass)
}

/// Single-layer 2x/4x depth/stencil in the compressed Z/S form, the same form
/// as the layered one, compressed stencil included.
fn single_layer_compressed_zls(pass: &RenderPass) -> bool {
    pass.layers == 1 && matches!(pass.samples_log2, 1 | 2) && compressed_zls_form(pass)
}

/// Multisample depth/stencil in the compressed Z/S form: each enabled load or
/// store of a plane is compressed (ZLS bits: Z load 15 / compress 2, S load 14
/// / compress 4, Z store 19 / compress 6, S store 18 / compress 8; the depth
/// format bits 25:26 are allowed), a plane
/// with any compression has a compression address, and no other ZLS bits.
fn compressed_zls_form(pass: &RenderPass) -> bool {
    const Z_LOAD: u64 = 1 << 15;
    const S_LOAD: u64 = 1 << 14;
    const Z_STORE: u64 = 1 << 19;
    const S_STORE: u64 = 1 << 18;
    const Z_LOAD_COMPRESS: u64 = 1 << 2;
    const S_LOAD_COMPRESS: u64 = 1 << 4;
    const Z_STORE_COMPRESS: u64 = 1 << 6;
    const S_STORE_COMPRESS: u64 = 1 << 8;
    /// Depth format (bits 25:26) is a property of the plane, not of layering.
    const Z_FORMAT: u64 = 3 << 25;
    const KNOWN: u64 = Z_LOAD | S_LOAD | Z_STORE | S_STORE | Z_LOAD_COMPRESS | S_LOAD_COMPRESS
        | Z_STORE_COMPRESS | S_STORE_COMPRESS | Z_FORMAT;
    let control = pass.zls_control;
    let z_compressed = control & (Z_LOAD_COMPRESS | Z_STORE_COMPRESS) != 0;
    let s_compressed = control & (S_LOAD_COMPRESS | S_STORE_COMPRESS) != 0;
    control != 0
        && control & !KNOWN == 0
        && (control & Z_LOAD == 0 || control & Z_LOAD_COMPRESS != 0)
        && (control & S_LOAD == 0 || control & S_LOAD_COMPRESS != 0)
        && (control & Z_STORE == 0 || control & Z_STORE_COMPRESS != 0)
        && (control & S_STORE == 0 || control & S_STORE_COMPRESS != 0)
        && (!z_compressed || (pass.depth.va != 0 && pass.depth.compression_va != 0))
        && (!s_compressed || (pass.stencil.va != 0 && pass.stencil.compression_va != 0))
        && (z_compressed || pass.depth.compression_va == 0)
        && (s_compressed || pass.stencil.compression_va == 0)
}

/// Single-layer D32 depth compression, with optional plain S8.
/// The compressed Z/S form is checked separately. Both depth compression bits
/// are required because partial rendering forces depth load and store even
/// when one of those operations is disabled in the final render pass.
fn single_layer_compressed_depth_zls(pass: &RenderPass) -> bool {
    const Z_LOAD: u64 = 1 << 15;
    const S_LOAD: u64 = 1 << 14;
    const Z_STORE: u64 = 1 << 19;
    const S_STORE: u64 = 1 << 18;
    const Z_COMPRESS: u64 = (1 << 2) | (1 << 6);
    const KNOWN: u64 = Z_LOAD | S_LOAD | Z_STORE | S_STORE | Z_COMPRESS;
    pass.layers == 1
        && matches!(pass.samples_log2, 1 | 2)
        && pass.zls_control & !KNOWN == 0
        && pass.zls_control & Z_COMPRESS == Z_COMPRESS
        && pass.depth.va != 0 && pass.depth.compression_va != 0
        && pass.stencil.compression_va == 0
        && (pass.zls_control & (S_LOAD | S_STORE) == 0 || pass.stencil.va != 0)
}

pub(crate) fn parameter_geometry(blocks: usize) -> Result<crate::g17::buffer::BlockGeometry> {
    let mut info = core::mem::MaybeUninit::<kernel::bindings::sysinfo>::zeroed();
    // SAFETY: The complete sysinfo is initialized and writable.
    unsafe { kernel::bindings::si_meminfo(info.as_mut_ptr()); }
    // SAFETY: The object was initialized before si_meminfo filled it.
    let info = unsafe { info.assume_init() };
    let ram = (info.totalram as u64).checked_mul(u64::from(info.mem_unit)).ok_or(EOVERFLOW)?;
    let initial = blocks.checked_mul(0x80000).ok_or(EOVERFLOW)?;
    crate::g17::buffer::BlockGeometry::m5(ram, initial as u64)
}

/// The target layout of a render: G17P's for a command without an M5 plan.
pub(crate) fn layout(command: &Validated, clusters: u32, programs: RegisterPrograms)
    -> Result<target::Layout> {
    Ok(layout_and_window(command, clusters, programs)?.0)
}

/// The target layout of a render and, for an M5 render, the bytes of the client's compact
/// window that each render pair built for it reserves.
pub(crate) fn layout_and_window(command: &Validated, clusters: u32, programs: RegisterPrograms)
    -> Result<(target::Layout, Option<usize>)> {
    let Validated::Render { pass, plan, .. } = command else { return Err(EINVAL); };
    let layout = target::Layout::new(pass, clusters, programs)?;
    if plan.is_none() { return Ok((layout, None)); }
    validate(pass)?;
    let geometry = parameter_geometry(layout.tvb_blocks)?;
    let (layout, reserved) = m5_layout(layout, pass, clusters, geometry)?;
    Ok((layout, Some(reserved)))
}

/// The M5 layout of `pass` from its G17P layout, and the compact-window bytes a pair built for
/// it reserves ([`pair_window_bytes`]).
fn m5_layout(layout: target::Layout, pass: &RenderPass, clusters: u32,
    geometry: crate::g17::buffer::BlockGeometry) -> Result<(target::Layout, usize)> {
    // One target group per cluster unless the pass asks for few primitives.
    // Both target storage and the segment controls use the physical cluster count.
    // The auxiliary tables cover the complete parameter inventory, including
    // future growth, rather than just the initially resident physical blocks.
    let bytes = geometry.capacity.checked_mul(geometry.block_bytes).ok_or(EOVERFLOW)?;
    let groups = if pass.few_primitives { 1 } else { clusters };
    let layout = layout.with_m5_slices(pass, clusters, groups, 64, bytes)?;
    // A pair maps its objects in the client's compact window, whose 32-bit offsets make it
    // 4 GiB. A layout whose pair does not fit there on its own can never be mapped: refuse it
    // as memory that cannot be had, before any of it is allocated. SUBMIT derives the layout,
    // so the refusal is the ioctl's.
    let reserved = pair_window_bytes(&layout, geometry, clusters)?;
    let window = crate::g17::buffer::BlockGeometry::M5_COMPACT;
    if reserved as u64 > window.end - window.start {
        return Err(ENOMEM);
    }
    Ok((layout, reserved))
}

/// Bytes of the client's M5 compact window that a render pair built for `layout` reserves: its
/// target records (a page-rounded record for each command slot), its TPC object and its initial
/// parameter blocks, which the layout sizes, and the objects of [`pair_fixed_bytes`].
fn pair_window_bytes(layout: &target::Layout, geometry: crate::g17::buffer::BlockGeometry,
    clusters: u32) -> Result<usize> {
    let depth = crate::hw::t8140::queues::RENDER_DEPTH as usize;
    let records = placed(layout.record_stride()?.checked_mul(depth).ok_or(EOVERFLOW)?, true)?;
    let tpc = placed(layout.tpc_capacity()?, true)?;
    // Each block is mapped at the geometry's stride, which includes its guard.
    let parameters = layout.tvb_blocks.checked_mul(geometry.stride).ok_or(EOVERFLOW)?;
    [tpc, parameters, pair_fixed_bytes(geometry, clusters)?]
        .into_iter()
        .try_fold(records, |sum, bytes| sum.checked_add(bytes))
        .ok_or(EOVERFLOW)
}

/// Bytes of the client's M5 compact window that every render pair reserves whatever its
/// layout: the context state of each command slot, its parameter buffer's scene scratch and
/// discard, its block manager's page list, its metrics entry, and its queue's pool page list.
fn pair_fixed_bytes(geometry: crate::g17::buffer::BlockGeometry, clusters: u32)
    -> Result<usize> {
    use crate::g17::fw::m5::pool;
    let depth = crate::hw::t8140::queues::RENDER_DEPTH as usize;
    let page = crate::mmu::UAT_PGSZ;
    let state = placed(super::generation::Layout::m5_bytes(clusters as usize), true)?
        .checked_mul(depth).ok_or(EOVERFLOW)?;
    let scratch = placed(pool::parameter_scratch_stride(geometry.capacity, 0)
        .map_err(|_| EINVAL)?.checked_mul(pool::SCENE_TABLE_ENTRIES).ok_or(EOVERFLOW)?, true)?;
    let discard = placed(0x8000, true)?;
    let page_list = placed(crate::g17::buffer::m5_page_list_bytes(geometry)?, true)?;
    let metrics = placed(page, false)?;
    let pool_pages = placed(page, false)?;
    [scratch, discard, page_list, metrics, pool_pages]
        .into_iter()
        .try_fold(state, |sum, bytes| sum.checked_add(bytes))
        .ok_or(EOVERFLOW)
}

/// The compact-window reservation of an object of `bytes`: page-rounded, with its guard page if
/// it has one, and rounded to the 32 KiB alignment such objects are placed at.
fn placed(bytes: usize, guard: bool) -> Result<usize> {
    let page = crate::mmu::UAT_PGSZ;
    let alignment = crate::hw::t8140::resources::TVB_PAGE as usize;
    bytes.checked_add(page - 1).map(|n| n & !(page - 1))
        .and_then(|n| n.checked_add(if guard { page } else { 0 }))
        .and_then(|n| n.checked_add(alignment - 1))
        .map(|n| n & !(alignment - 1))
        .ok_or(EOVERFLOW)
}

pub(crate) fn controls(
    pass: &RenderPass,
    board: &'static crate::g17::fw::m5::initdata::Board,
) -> Result<regs::M5RenderControls> {
    validate(pass)?;
    let stage = |resource_flags| regs::M5RenderStageControls {
        scs: crate::g17::render_resources::StageScratch::NONE,
        resource_flags, resource_control: 0x200000001, ptc_max: 96, work_key: 0,
    };
    Ok(regs::M5RenderControls {
        board,
        fragment_event_control: regs::fragment_event_control(true, 1),
        ta: stage(0x4054b), fragment: stage(0x104054b),
        ta_tile_control: 0x10c08860, ta_dispatch_control: 0x700,
        fragment_raster_control: 0x200000ff0200, fragment_tile_flags: 0x200003717f,
        store_pipeline_2_bind: pass.eot_rsrc_spec << 32,
        store_pipeline_2: pass.usc_base + u64::from(pass.eot_offset & !7),
        partial_store_pipeline_2_bind: pass.partial_eot_rsrc_spec << 32,
        partial_store_pipeline_2: pass.usc_base + u64::from(pass.partial_eot_offset & !7),
        partial_tile_flags: 0x200000,
        aux_fb_flags: 0x10c000 | u64::from(pass.few_primitives)
            | (u64::from(pass.depth_bias_is_int) << 18),
        aux_fb_control: 0x100000,
    })
}

pub(crate) fn inputs(args: &RenderArgs<'_>, usage: [u64; 2]) -> Result<cmd::M5RenderCommandInputs> {
    let r = args.resources;
    let mut fields = cmd::M5RenderCommandInputs::default();
    let token = crate::g17::fw::m5::render_tail::context_token(u32::from(r.context_id),
        u64::from(r.context_generation)).ok_or(EINVAL)?;
    fields.ta.context_token = token;
    fields.fragment.context_token = token;
    fields.ta.resolved_tail_value = usage[0];
    fields.fragment.resolved_tail_value = Some(usage[1]);
    fields.scheduler_record = r.kick_record_va;
    // The compiler's external-write class of the render's graphics programs.
    fields.ta.shader_write_class = cmd::shader_write_class(args.pass.shader_writes_memory);
    fields.ta.sequence = args.ordinal.checked_mul(2).and_then(|n| n.checked_add(1)).ok_or(EOVERFLOW)?;
    fields.fragment.sequence = args.ordinal.checked_mul(2).ok_or(EOVERFLOW)?;
    fields.fragment.pass = 1;
    fields.ta.predecessor_id = r.ta_ids.predecessor;
    fields.fragment.predecessor_id = r.fragment_ids.predecessor;
    fields.fragment.fragment_word_n = 1;
    fields.fragment.fragment_flag_c = 1;
    fields.fragment.binding_pairs[22][1] = 0x04040404;
    // Pass policy is filled by apply_pass alongside its register copies.
    fields.fragment.empty_tile_flags = [0; 4];
    fields.ta.timestamp_va = r.timestamp_va;
    fields.ta.diagnostic_ta_timestamp_upper = r.timestamp_va + 8;
    fields.fragment.timestamp_va = [r.timestamp_va + 16, r.timestamp_va + 24];
    let stamp = (args.ordinal as u32).wrapping_add(1).wrapping_shl(8);
    fields.ta.linked_3d = cmd::M5TaLinked3d {
        queue_index: u32::from(args.fragment.qid), stamp, ..Default::default()
    };
    let completion = |stage: &crate::g17::fw::render::RenderStage, key| cmd::M5StageCompletion {
        stamp_addresses: [stage.stamp_va, stage.aux_stamp_va], stamp,
        queue_index: u32::from(stage.qid), ordinal: args.ordinal as u32,
        key, ..Default::default()
    };
    fields.ta.completion = completion(&args.ta, u64::from(r.ta_ids.current));
    fields.fragment.completion = completion(&args.fragment, u64::from(r.fragment_ids.current));
    fields.ta.pb_slot = r.pb_slot;
    fields.ta.pb_state = r.pb_va;
    fields.fragment.pm_state = r.pb_va;
    fields.ta.pm_scene = r.scene_va;
    fields.fragment.pm_scene = r.scene_va;
    fields.ta.scene_end_va = r.scene_end_va;
    fields.fragment.scene_end_va = r.scene_end_va;
    fields.ta.uma_state = r.free_list_control_va;
    fields.fragment.uma_state = r.free_list_control_va;
    fields.ta.tail_flag = 1;
    fields.fragment.tail_flag = 1;
    Ok(fields)
}


#[kernel::macros::kunit_tests(asahi_g17_m5_pair_window)]
mod tests {
    use super::*;
    use crate::g17::buffer::BlockGeometry;

    /// The J775D (M5 Ultra, eight clusters) of the measured boot, 98970480 KiB of memory.
    const RAM: u64 = 98970480 << 10;
    const WINDOW: usize = 1 << 32;
    const MIB: usize = 1 << 20;

    /// A single-sample pass as Borealis sends it: `utile` tiles, and few-primitives for a pass
    /// of at most four primitives.
    fn reserved(size: u32, layers: u16, utile: u8, few: bool) -> Result<usize> {
        // SAFETY: RenderPass and its nested surfaces/programs contain only
        // integers and bools, for which the all-zero representation is valid.
        let mut pass: RenderPass = unsafe { core::mem::zeroed() };
        pass.width = size;
        pass.height = size;
        pass.layers = layers;
        pass.utile_width = utile;
        pass.utile_height = utile;
        pass.few_primitives = few;
        let layout = target::Layout::new(&pass, 8, RegisterPrograms::G17P)?;
        let geometry = BlockGeometry::m5(RAM, (layout.tvb_blocks * 0x80000) as u64)?;
        m5_layout(layout, &pass, 8, geometry).map(|(_, bytes)| bytes)
    }

    #[test]
    fn borealis_32x32_passes_of_more_than_four_primitives() {
        assert!(reserved(16384, 22, 32, false).unwrap() <= WINDOW);
        assert_eq!(reserved(16384, 23, 32, false), Err(ENOMEM));
        assert!(reserved(512, 2048, 32, false).unwrap() <= WINDOW);
        assert!(reserved(1024, 2048, 32, false).unwrap() <= WINDOW);
        assert_eq!(reserved(1152, 2048, 32, false), Err(ENOMEM));
    }

    #[test]
    fn borealis_32x32_passes_of_few_primitives() {
        assert!(reserved(16384, 53, 32, true).unwrap() <= WINDOW);
        assert_eq!(reserved(16384, 54, 32, true), Err(ENOMEM));
        assert!(reserved(2048, 2047, 32, true).unwrap() <= WINDOW);
        assert!(reserved(2048, 2048, 32, true).unwrap() <= WINDOW);
        assert_eq!(reserved(2112, 2048, 32, true), Err(ENOMEM));
        assert!(reserved(1152, 2048, 32, true).unwrap() <= WINDOW);
    }

    #[test]
    fn passes_with_16x16_tiles() {
        assert!(reserved(16384, 5, 16, false).unwrap() <= WINDOW);
        assert_eq!(reserved(16384, 6, 16, false), Err(ENOMEM));
        assert!(reserved(512, 2048, 16, false).unwrap() <= WINDOW);
        assert_eq!(reserved(1024, 2048, 16, false), Err(ENOMEM));
    }

    #[test]
    fn a_pair_counts_the_objects_its_layout_does_not_size() {
        // A trace of a pair on the J775D found 3824 KiB of these objects, guards included and
        // alignment holes not.
        let geometry = BlockGeometry::m5(RAM, 32 * 0x80000).unwrap();
        let fixed = pair_fixed_bytes(geometry, 8).unwrap();
        assert!(fixed >= 3824 * 1024);
        assert!(fixed <= 5 * MIB);
    }
}
