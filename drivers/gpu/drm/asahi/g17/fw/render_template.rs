// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Self-verifying template delta-patching for G17P/V25G83 render descriptors.
//!
//! From one render of a pass to the next, only the per-render addresses,
//! ordinals, lifecycle IDs, work keys, stamps, timestamps, sampler bindings,
//! clear values, and USC program offsets change. The remaining ~175 register
//! entries and descriptor fields stay identical.
//!
//! Each render pair owns one heap-backed [`TemplateSlot`] holding the static
//! [`Key`], golden [`TaDescriptor`] and [`FragmentDescriptor`] templates, and
//! per-ring-slot template generation tracking. A new template enters
//! [`State::Pending`] and is admitted only after the next matching render
//! verifies byte-for-byte against a full encoding; any mismatch permanently
//! rejects that key until the key changes.

use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicU64, Ordering};
use kernel::prelude::*;

use super::kick::{McacheRange, McacheTable, RegisterWrite, WriteRange, MCACHE_INLINE_RANGES};
use super::render::{
    self, Deflake, FragmentDescriptor, FragmentSlot, RegisterPrograms, RenderArgs, RenderPass,
    RenderResources, RenderStage, TaDescriptor, UscProgram,
};
use super::render_rt::RayQueryStage;
use super::{offset_va, sampler_max, timestamp_end_va, work_key, ObjectIds};

/// Number of descriptor slots in a render pair's ring (`memory::RECORDS`).
pub(crate) const RING_SLOTS: usize = 32;

/// Device-lifetime G17P/V25G83 render-template counters:
/// `[full, patched, verified, mismatched, unmapped]`.
static COUNTERS: [AtomicU64; 5] = [const { AtomicU64::new(0) }; 5];

pub(crate) fn counters() -> [u64; 5] {
    core::array::from_fn(|i| COUNTERS[i].load(Ordering::Relaxed))
}

fn record_path(path: Path) {
    let idx = match path {
        Path::Full => 0,
        Path::Patched => 1,
        Path::Verified => 2,
        Path::Mismatch { .. } => 3,
        Path::Unmapped => 4,
    };
    COUNTERS[idx].fetch_add(1, Ordering::Relaxed);
}

/// How [`encode`] encoded a render pair's descriptors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Path {
    /// Encoded in full (empty slot, key changed, or rejected template).
    Full,
    /// Encoded in full because a required register slot could not be located.
    Unmapped,
    /// Patched from an admitted template.
    Patched,
    /// Patched and verified byte-for-byte against a full encode; template admitted.
    Verified,
    /// Patched encoding differed from a full encode; full encode used and key rejected.
    Mismatch { stage: u8, offset: u32 },
}

/// Every input to [`TaDescriptor::write`] and [`FragmentSlot::write`] except
/// the per-render fields patched by [`TemplateSlot::patch`].
///
/// Exhaustive destructuring ensures any new field added to [`RenderPass`],
/// [`RenderResources`], [`RenderStage`], or [`RenderArgs`] forces an explicit
/// compile-time choice between keying and patching.
#[derive(Clone, Copy, PartialEq, Eq)]
struct Key {
    pass: RenderPass,
    resources: RenderResources,
    ta: RenderStage,
    fragment: RenderStage,
    ray_query: [RayQueryStage; 2],
    programs: RegisterPrograms,
    ta_mode_1: bool,
    clusters: u32,
    deflake: Deflake,
}

impl Key {
    /// Compares the cached template key against `args` out of line so the
    /// masked temporary values pop before `patch()` or full encoding runs.
    #[inline(never)]
    fn matches(&self, args: &RenderArgs<'_>) -> bool {
        let RenderArgs {
            pass,
            ray_query,
            resources,
            ordinal: _,
            pair_idle: _,
            ta,
            fragment,
            programs,
            ta_mode_1,
            clusters,
            deflake,
        } = *args;
        self.programs == programs
            && self.ta_mode_1 == ta_mode_1
            && self.clusters == clusters
            && self.deflake == deflake
            && self.ray_query == ray_query
            && self.ta == Self::masked_stage(ta)
            && self.fragment == Self::masked_stage(fragment)
            && self.pass == Self::masked_pass(pass)
            && self.resources == Self::masked_resources(resources)
    }

    #[inline(never)]
    fn write_into(dst: &mut MaybeUninit<Self>, args: &RenderArgs<'_>) {
        let RenderArgs {
            pass,
            ray_query,
            resources,
            ordinal: _,
            pair_idle: _,
            ta,
            fragment,
            programs,
            ta_mode_1,
            clusters,
            deflake,
        } = *args;
        dst.write(Self {
            pass: Self::masked_pass(pass),
            resources: Self::masked_resources(resources),
            ta: Self::masked_stage(ta),
            fragment: Self::masked_stage(fragment),
            ray_query,
            programs,
            ta_mode_1,
            clusters,
            deflake,
        });
    }

    fn masked_pass(pass: &RenderPass) -> RenderPass {
        let RenderPass {
            width,
            height,
            layers,
            utile_width,
            utile_height,
            samples_log2,
            sample_size,
            process_empty_tiles,
            msaa_control,
            ppp_control,
            vdm_va: _,
            scissor_va: _,
            depth_bias_va: _,
            depth_bias_is_int,
            few_primitives,
            mesh_output,
            fragment_barriers,
            stage_prefixes,
            whole_attachments,
            shader_writes_memory,
            occlusion_query_va: _,
            sampler_heap_va: _,
            sampler_count: _,
            zls_control,
            depth_dimensions,
            depth,
            stencil,
            depth_clear: _,
            stencil_clear: _,
            merge_upper_x,
            merge_upper_y,
            usc_base,
            bg,
            partial_bg,
            eot_offset: _,
            eot_rsrc_spec,
            partial_eot_offset: _,
            partial_eot_rsrc_spec,
            rsrc_spec_hi,
        } = *pass;
        RenderPass {
            width,
            height,
            layers,
            utile_width,
            utile_height,
            samples_log2,
            sample_size,
            process_empty_tiles,
            msaa_control,
            ppp_control,
            vdm_va: 0,
            scissor_va: 0,
            depth_bias_va: 0,
            depth_bias_is_int,
            few_primitives,
            mesh_output,
            fragment_barriers,
            stage_prefixes,
            whole_attachments,
            shader_writes_memory,
            occlusion_query_va: 0,
            sampler_heap_va: 0,
            sampler_count: 0,
            zls_control,
            depth_dimensions,
            depth,
            stencil,
            depth_clear: 0,
            stencil_clear: 0,
            merge_upper_x,
            merge_upper_y,
            usc_base,
            bg: UscProgram { offset: 0, ..bg },
            partial_bg: UscProgram { offset: 0, ..partial_bg },
            eot_offset: 0,
            eot_rsrc_spec,
            partial_eot_offset: 0,
            partial_eot_rsrc_spec,
            rsrc_spec_hi,
        }
    }

    fn masked_resources(resources: &RenderResources) -> RenderResources {
        let RenderResources {
            context_id,
            context_generation,
            pb_slot,
            free_list_slot,
            free_list_control_va,
            kick_record_va: _,
            pb_va,
            scene_va: _,
            scene_end_va,
            pm_scratch: _,
            pm_metric: _,
            tilemap_va: _,
            layermeta_va: _,
            heapmeta_va: _,
            tpc_va,
            tpc_size,
            gtp_va,
            deflake_va: _,
            ta_status_va: _,
            mesh_storage,
            fragment_status_va: _,
            fragment_status_fw_va: _,
            aux_fb_va: _,
            timestamp_va: _,
            timestamp_window: _,
            ta_ids: _,
            fragment_ids: _,
            unk_2104,
            unk_210c,
        } = *resources;
        let none = ObjectIds {
            predecessor: 0,
            current: 0,
        };
        RenderResources {
            context_id,
            context_generation,
            pb_slot,
            free_list_slot,
            free_list_control_va,
            kick_record_va: 0,
            pb_va,
            scene_va: 0,
            scene_end_va,
            pm_scratch: 0,
            pm_metric: 0,
            tilemap_va: 0,
            layermeta_va: 0,
            heapmeta_va: 0,
            tpc_va,
            tpc_size,
            gtp_va,
            deflake_va: 0,
            ta_status_va: 0,
            mesh_storage,
            fragment_status_va: 0,
            fragment_status_fw_va: 0,
            aux_fb_va: 0,
            timestamp_va: 0,
            timestamp_window: [0; 4],
            ta_ids: none,
            fragment_ids: none,
            unk_2104,
            unk_210c,
        }
    }

    fn masked_stage(stage: RenderStage) -> RenderStage {
        let RenderStage {
            qid,
            descriptor_va: _,
            state_word: _,
            stamp_va,
            aux_stamp_va,
        } = stage;
        RenderStage {
            qid,
            descriptor_va: 0,
            state_word: 0,
            stamp_va,
            aux_stamp_va,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Empty,
    Pending,
    Admitted,
    Rejected,
}

/// Entry indices of per-render registers inside `TaDescriptor.registers`.
#[derive(Clone, Copy)]
struct TaIndices {
    tilemap: [u8; 2],
    heapmeta: [u8; 2],
    deflake_2a0: [u8; 3],
    deflake_20: [u8; 2],
    deflake: u8,
    vdm: u8,
    layermeta: [u8; 2],
    pm_scratch: [u8; 2],
    pm_metric: u8,
    ids: [u8; 3],
    key: [u8; 3],
    ta_status: u8,
}

/// Entry indices of per-render registers inside `FragmentDescriptor.arrays[0..4]`.
#[derive(Clone, Copy)]
struct FragIndices {
    eot_va: u8,
    bg_va: u8,
    heapmeta: u8,
    scissor: u8,
    depth_bias: u8,
    depth_clear: u8,
    stencil_clear: u8,
    occlusion: u8,
    tilemap: u8,
    layermeta: u8,
    aux_fb: [u8; 2],
    ids: [u8; 3],
    pm_scratch: u8,
    key: [u8; 2],
    frag_status: u8,
    store_eot_va: u8,
    resume_eot_va: u8,
    resume_bg_va: u8,
    resume_stencil: u8,
    load_bg_va: u8,
    load_stencil: u8,
}

fn find_reg(entries: &[RegisterWrite], reg: u32, nth: usize) -> Option<u8> {
    entries
        .iter()
        .enumerate()
        .filter(|(_, w)| w.reg == reg)
        .nth(nth)
        .and_then(|(idx, _)| u8::try_from(idx).ok())
}

impl TaIndices {
    fn discover(ta: &TaDescriptor) -> Option<Self> {
        let count = (ta.registers.size & 0xffff) as usize;
        let e = ta.registers.entries.get(..count)?;
        Some(Self {
            tilemap: [find_reg(e, 0x1c039, 0)?, find_reg(e, 0x1c9c8, 0)?],
            heapmeta: [find_reg(e, 0x1c031, 0)?, find_reg(e, 0x1c9c0, 0)?],
            deflake_2a0: [
                find_reg(e, 0x10111, 0)?,
                find_reg(e, 0x1c9b0, 0)?,
                find_reg(e, 0x1c880, 1)?,
            ],
            deflake_20: [find_reg(e, 0x10119, 0)?, find_reg(e, 0x1c9b8, 0)?],
            deflake: find_reg(e, 0x1c950, 0)?,
            vdm: find_reg(e, 0x1c880, 0)?,
            layermeta: [find_reg(e, 0x1c079, 0)?, find_reg(e, 0x1c9d8, 0)?],
            pm_scratch: [find_reg(e, 0x1ca30, 0)?, find_reg(e, 0x16c39, 0)?],
            pm_metric: find_reg(e, 0x1c910, 0)?,
            ids: [
                find_reg(e, 0x1ca10, 0)?,
                find_reg(e, 0x014a1, 0)?,
                find_reg(e, 0x0a349, 0)?,
            ],
            key: [
                find_reg(e, 0x10209, 0)?,
                find_reg(e, 0x1c9f0, 0)?,
                find_reg(e, 0x14320, 0)?,
            ],
            ta_status: find_reg(e, 0x14318, 0)?,
        })
    }
}

impl FragIndices {
    fn discover(frag: &FragmentDescriptor) -> Option<Self> {
        let slice = |i: usize| -> Option<&[RegisterWrite]> {
            let count = (frag.arrays[i].size & 0xffff) as usize;
            frag.arrays[i].entries.get(..count)
        };
        let m = slice(0)?;
        let s = slice(1)?;
        let r = slice(2)?;
        let l = slice(3)?;
        Some(Self {
            eot_va: find_reg(m, 0x15381, 0)?,
            bg_va: find_reg(m, 0x15371, 0)?,
            heapmeta: find_reg(m, 0x16098, 0)?,
            scissor: find_reg(m, 0x15109, 0)?,
            depth_bias: find_reg(m, 0x15101, 0)?,
            depth_clear: find_reg(m, 0x15301, 0)?,
            stencil_clear: find_reg(m, 0x15309, 0)?,
            occlusion: find_reg(m, 0x15311, 0)?,
            tilemap: find_reg(m, 0x16429, 0)?,
            layermeta: find_reg(m, 0x16060, 0)?,
            aux_fb: [find_reg(m, 0x16461, 0)?, find_reg(m, 0x16090, 0)?],
            ids: [
                find_reg(m, 0x160e0, 0)?,
                find_reg(m, 0x01499, 0)?,
                find_reg(m, 0x0a341, 0)?,
            ],
            pm_scratch: find_reg(m, 0x1ca28, 0)?,
            key: [find_reg(m, 0x10211, 0)?, find_reg(m, 0x10420, 0)?],
            frag_status: find_reg(m, 0x14080, 0)?,
            store_eot_va: find_reg(s, 0x15381, 0)?,
            resume_eot_va: find_reg(r, 0x15381, 0)?,
            resume_bg_va: find_reg(r, 0x15371, 0)?,
            resume_stencil: find_reg(r, 0x15309, 0)?,
            load_bg_va: find_reg(l, 0x15371, 0)?,
            load_stencil: find_reg(l, 0x15309, 0)?,
        })
    }
}

/// Heap-backed template state for one non-M5 render pair.
pub(crate) struct TemplateSlot {
    state: State,
    generation: u64,
    key: MaybeUninit<Key>,
    ta_idx: MaybeUninit<TaIndices>,
    frag_idx: MaybeUninit<FragIndices>,
    slot_gen: [u64; RING_SLOTS],
    slot_mcache: [u8; RING_SLOTS],
    ta: TaDescriptor,
    fragment: FragmentDescriptor,
    verify_ta: TaDescriptor,
    verify_fragment: FragmentDescriptor,
}

impl TemplateSlot {
    /// Allocates an empty heap-backed template slot without placing descriptors on the stack.
    pub(crate) fn new_box() -> Result<KBox<Self>> {
        let mut storage = KBox::<Self>::new_uninit(GFP_KERNEL)?;
        // SAFETY: `TemplateSlot` contains only integers, enums where zero is valid?
        // No: `State` is not `Zeroable`; zero the descriptor arrays then initialize
        // non-zeroable metadata fields explicitly.
        unsafe {
            let ptr = storage.as_mut_ptr();
            core::ptr::write_bytes(ptr, 0, 1);
            core::ptr::addr_of_mut!((*ptr).state).write(State::Empty);
            Ok(storage.assume_init())
        }
    }

    fn capture(&mut self, args: &RenderArgs<'_>, ta: &TaDescriptor, frag: &FragmentDescriptor) -> bool {
        let (Some(ta_idx), Some(frag_idx)) = (TaIndices::discover(ta), FragIndices::discover(frag))
        else {
            self.state = State::Empty;
            return false;
        };
        // SAFETY: Both descriptors are POD packed integer structs.
        unsafe {
            core::ptr::copy_nonoverlapping(ta, &mut self.ta, 1);
            core::ptr::copy_nonoverlapping(frag, &mut self.fragment, 1);
        }
        Key::write_into(&mut self.key, args);
        self.ta_idx.write(ta_idx);
        self.frag_idx.write(frag_idx);
        self.generation = self.generation.wrapping_add(1).max(1);
        self.state = State::Pending;
        true
    }

    /// Patches per-render fields into `ta` and `frag`, validating every input
    /// before mutating either descriptor.
    #[inline(never)]
    fn patch(
        ta_idx: &TaIndices,
        frag_idx: &FragIndices,
        ta: &mut TaDescriptor,
        frag: &mut FragmentDescriptor,
        args: &RenderArgs<'_>,
    ) -> Result {
        let p = args.pass;
        let r = args.resources;

        // Pre-validate every fallible derivation before mutating storage.
        let tilemap = render::user_offset(r.tilemap_va)?;
        let layermeta = render::user_offset(r.layermeta_va)?;
        let heapmeta = render::user_offset(r.heapmeta_va)? | (1 << 63);
        let [sub0, sub1] = args.deflake.sub;
        let deflake = u64::from(render::user_offset32(r.deflake_va)?);
        let deflake_20 = u64::from(render::user_offset32(offset_va(r.deflake_va, sub0)?)?);
        let deflake_2a0_u32 = render::user_offset32(offset_va(r.deflake_va, sub1)?)?;
        let deflake_2a0 = u64::from(deflake_2a0_u32);
        let vdm = render::user_offset(p.vdm_va)?;
        if vdm >= render::VDM_OFFSET_LIMIT {
            return Err(EINVAL);
        }
        let ta_array_va = offset_va(args.ta.descriptor_va, TaDescriptor::ARRAY_OFFSET)?;
        let frag_base = args.fragment.descriptor_va;
        let frag_arrays = [
            offset_va(frag_base, FragmentDescriptor::array_offset(0))?,
            offset_va(frag_base, FragmentDescriptor::array_offset(1))?,
            offset_va(frag_base, FragmentDescriptor::array_offset(2))?,
            offset_va(frag_base, FragmentDescriptor::array_offset(3))?,
        ];
        let smax = sampler_max(p.sampler_count)?;
        let ts_end = timestamp_end_va(r.timestamp_va)?;
        let usc = |off: u32| offset_va(p.usc_base, u64::from(off & !render::USC_OFFSET_TAG_MASK));
        let bg_va = usc(p.bg.offset)?;
        let partial_bg_va = usc(p.partial_bg.offset)?;
        let eot_va = usc(p.eot_offset)?;
        let partial_eot_va = usc(p.partial_eot_offset)?;

        let stamp = args.stamp_value();
        let ta_key = work_key(r.context_id, args.ta.state_word);
        let frag_key = work_key(r.context_id, args.fragment.state_word);
        let ta_ids = r.ta_ids.word();
        let frag_ids = r.fragment_ids.word();
        let stencil_word = u64::from(p.stencil_clear | render::STENCIL_CLEAR_BASE);

        // --- Patch TA descriptor ---
        ta.submit_sequence = args.ordinal.wrapping_mul(2).wrapping_add(1);
        ta.kick_record_va = r.kick_record_va;
        ta.scene_va = r.scene_va;
        ta.unk_48 = (args.ordinal as u32).wrapping_add((args.ordinal / 2) as u32);
        ta.registers.self_va = ta_array_va;
        ta.registers.entries[usize::from(ta_idx.tilemap[0])].value = tilemap;
        ta.registers.entries[usize::from(ta_idx.tilemap[1])].value = tilemap;
        ta.registers.entries[usize::from(ta_idx.heapmeta[0])].value = heapmeta;
        ta.registers.entries[usize::from(ta_idx.heapmeta[1])].value = heapmeta;
        ta.registers.entries[usize::from(ta_idx.deflake_2a0[0])].value = deflake_2a0;
        ta.registers.entries[usize::from(ta_idx.deflake_2a0[1])].value = deflake_2a0;
        ta.registers.entries[usize::from(ta_idx.deflake_2a0[2])].value = deflake_2a0;
        ta.registers.entries[usize::from(ta_idx.deflake_20[0])].value = deflake_20;
        ta.registers.entries[usize::from(ta_idx.deflake_20[1])].value = deflake_20;
        ta.registers.entries[usize::from(ta_idx.deflake)].value = deflake | (1 << 50);
        ta.registers.entries[usize::from(ta_idx.vdm)].value = vdm;
        ta.registers.entries[usize::from(ta_idx.layermeta[0])].value = layermeta;
        ta.registers.entries[usize::from(ta_idx.layermeta[1])].value = layermeta;
        ta.registers.entries[usize::from(ta_idx.pm_scratch[0])].value = r.pm_scratch;
        ta.registers.entries[usize::from(ta_idx.pm_scratch[1])].value = r.pm_scratch;
        ta.registers.entries[usize::from(ta_idx.pm_metric)].value = r.pm_metric;
        ta.registers.entries[usize::from(ta_idx.ids[0])].value = ta_ids;
        ta.registers.entries[usize::from(ta_idx.ids[1])].value = ta_ids;
        ta.registers.entries[usize::from(ta_idx.ids[2])].value = ta_ids;
        ta.registers.entries[usize::from(ta_idx.key[0])].value = ta_key;
        ta.registers.entries[usize::from(ta_idx.key[1])].value = ta_key;
        ta.registers.entries[usize::from(ta_idx.key[2])].value = ta_key;
        ta.registers.entries[usize::from(ta_idx.ta_status)].value = r.ta_status_va | 1;

        ta.stamp_value = stamp;
        ta.fragment_busy = 0;
        ta.fragment_kick = 0;
        ta.pass_seq = [0; 2];
        ta.deflake_2a0_offset = deflake_2a0_u32;
        ta.predecessor_id = r.ta_ids.predecessor;
        ta.sampler_heap_va = p.sampler_heap_va;
        ta.sampler_count = p.sampler_count;
        ta.sampler_max = smax;
        ta.stamp_value_2 = stamp;
        ta.unk_8be = 0;
        ta.first_kick = match args.programs == RegisterPrograms::V25G83 {
            true => u32::from(args.ordinal == 0 || args.pair_idle),
            false => u32::from(args.ordinal == 0),
        };
        ta.completion = 0;
        ta.current_id = r.ta_ids.current.into();
        ta.ordinal = args.ordinal as u32;
        ta.timestamp_start_va = r.timestamp_va;
        ta.unk_90e[..8].copy_from_slice(&r.timestamp_window[0].to_le_bytes());
        ta.unk_90e[8..16].copy_from_slice(&r.timestamp_window[1].to_le_bytes());
        ta.unk_93e = if args.ordinal == 0 {
            TaDescriptor::UNK_93E_FIRST_KICK
        } else {
            [0; 2]
        };
        ta.status_va = r.ta_status_va;

        // --- Patch Fragment descriptor ---
        frag.submit_sequence = args.ordinal.wrapping_mul(2);
        frag.kick_record_va = r.kick_record_va;
        frag.scene_va = r.scene_va;
        frag.tilemap_va = r.tilemap_va;
        frag.pass_seq = 0;
        for (i, va) in frag_arrays.into_iter().enumerate() {
            frag.arrays[i].self_va = va;
        }
        frag.arrays[0].entries[usize::from(frag_idx.eot_va)].value = eot_va;
        frag.arrays[0].entries[usize::from(frag_idx.bg_va)].value = bg_va;
        frag.arrays[0].entries[usize::from(frag_idx.heapmeta)].value = r.heapmeta_va;
        frag.arrays[0].entries[usize::from(frag_idx.scissor)].value = p.scissor_va;
        frag.arrays[0].entries[usize::from(frag_idx.depth_bias)].value = p.depth_bias_va;
        frag.arrays[0].entries[usize::from(frag_idx.depth_clear)].value = u64::from(p.depth_clear);
        frag.arrays[0].entries[usize::from(frag_idx.stencil_clear)].value = stencil_word;
        frag.arrays[0].entries[usize::from(frag_idx.occlusion)].value = p.occlusion_query_va;
        frag.arrays[0].entries[usize::from(frag_idx.tilemap)].value = r.tilemap_va;
        frag.arrays[0].entries[usize::from(frag_idx.layermeta)].value = r.layermeta_va;
        frag.arrays[0].entries[usize::from(frag_idx.aux_fb[0])].value = r.aux_fb_va;
        frag.arrays[0].entries[usize::from(frag_idx.aux_fb[1])].value = r.aux_fb_va;
        frag.arrays[0].entries[usize::from(frag_idx.ids[0])].value = frag_ids;
        frag.arrays[0].entries[usize::from(frag_idx.ids[1])].value = frag_ids;
        frag.arrays[0].entries[usize::from(frag_idx.ids[2])].value = frag_ids;
        frag.arrays[0].entries[usize::from(frag_idx.pm_scratch)].value = r.pm_scratch;
        frag.arrays[0].entries[usize::from(frag_idx.key[0])].value = frag_key;
        frag.arrays[0].entries[usize::from(frag_idx.key[1])].value = frag_key;
        frag.arrays[0].entries[usize::from(frag_idx.frag_status)].value = r.fragment_status_va | 1;

        frag.arrays[1].entries[usize::from(frag_idx.store_eot_va)].value = partial_eot_va;
        frag.arrays[2].entries[usize::from(frag_idx.resume_eot_va)].value = partial_eot_va;
        frag.arrays[2].entries[usize::from(frag_idx.resume_bg_va)].value = partial_bg_va;
        frag.arrays[2].entries[usize::from(frag_idx.resume_stencil)].value = stencil_word;
        frag.arrays[3].entries[usize::from(frag_idx.load_bg_va)].value = partial_bg_va;
        frag.arrays[3].entries[usize::from(frag_idx.load_stencil)].value = stencil_word;

        frag.depth_bias_va = p.depth_bias_va;
        frag.scissor_va = p.scissor_va;
        frag.occlusion_query_va = p.occlusion_query_va;
        frag.bg_va = bg_va;
        frag.partial_bg_va = partial_bg_va;
        frag.eot_va = eot_va;
        frag.partial_eot_va = partial_eot_va;
        frag.depth_clear = p.depth_clear;
        frag.predecessor_id = r.fragment_ids.predecessor;
        frag.stamp_value = stamp;
        frag.unk_2158 = 0;
        frag.first_kick = u32::from(args.ordinal == 0);
        frag.completion = 0;
        frag.current_id = r.fragment_ids.current.into();
        frag.ordinal = args.ordinal as u32;
        frag.timestamp_start_va = r.timestamp_va;
        frag.timestamp_end_va = ts_end;
        frag.unk_21a8[..8].copy_from_slice(&r.timestamp_window[2].to_le_bytes());
        frag.unk_21a8[8..16].copy_from_slice(&r.timestamp_window[3].to_le_bytes());
        frag.status_va = r.fragment_status_va;
        frag.status_fw_va = r.fragment_status_fw_va;
        Ok(())
    }
}

fn first_diff<T>(a: &T, b: &T) -> Option<u32> {
    let size = core::mem::size_of::<T>();
    // SAFETY: Both `TaDescriptor` and `FragmentDescriptor` are packed integer-only structs with no padding.
    let sa = unsafe { core::slice::from_raw_parts(core::ptr::from_ref(a).cast::<u8>(), size) };
    let sb = unsafe { core::slice::from_raw_parts(core::ptr::from_ref(b).cast::<u8>(), size) };
    sa.iter()
        .zip(sb.iter())
        .position(|(x, y)| x != y)
        .map(|pos| pos as u32)
}

/// Encodes `ta` and `fragment` using `template`, returning the MCache table and
/// which encoding path ran.
pub(crate) fn encode(
    ta: &mut TaDescriptor,
    fragment: &mut FragmentSlot,
    args: &RenderArgs<'_>,
    attachments: &[WriteRange],
    context: u8,
    template: &mut TemplateSlot,
) -> Result<(Option<McacheTable>, Path)> {
    let slot = args.ordinal as usize % RING_SLOTS;
    let key_matches = template.state != State::Empty
        // SAFETY: `key` is initialized whenever `state != State::Empty`.
        && unsafe { template.key.assume_init_ref().matches(args) };

    let path = match (template.state, key_matches) {
        (State::Admitted, true) => {
            if template.slot_gen[slot] != template.generation {
                // SAFETY: Both descriptors are POD packed integer structs.
                unsafe {
                    core::ptr::copy_nonoverlapping(&template.ta, ta, 1);
                    core::ptr::copy_nonoverlapping(&template.fragment, &mut fragment.descriptor, 1);
                }
                template.slot_gen[slot] = template.generation;
                template.slot_mcache[slot] = 0;
            } else if template.slot_mcache[slot] != 0 {
                let clear_len = usize::from(template.slot_mcache[slot]).min(MCACHE_INLINE_RANGES);
                // SAFETY: `McacheRange` is packed integers; zeroing clears prior inline ranges.
                unsafe {
                    core::ptr::write_bytes(
                        core::ptr::addr_of_mut!(fragment.descriptor.arrays[0].mcache)
                            .cast::<McacheRange>(),
                        0,
                        clear_len,
                    );
                }
                template.slot_mcache[slot] = 0;
            }
            // SAFETY: Indices are initialized while `state == State::Admitted`.
            let (ta_idx, frag_idx) = unsafe {
                (
                    template.ta_idx.assume_init_ref(),
                    template.frag_idx.assume_init_ref(),
                )
            };
            match TemplateSlot::patch(ta_idx, frag_idx, ta, &mut fragment.descriptor, args) {
                Ok(()) => Path::Patched,
                Err(_) => {
                    // Fall back to full encode; propagate real validation errors.
                    template.slot_gen[slot] = 0;
                    template.state = State::Empty;
                    render::write_pair(ta, fragment, args)?;
                    template.slot_mcache[slot] = 0;
                    Path::Full
                }
            }
        }
        (State::Pending, true) => {
            // Copy golden template, patch it, and verify byte-for-byte against a full encode.
            // SAFETY: Both descriptors are POD packed integer structs; template is initialized.
            unsafe {
                core::ptr::copy_nonoverlapping(&template.ta, ta, 1);
                core::ptr::copy_nonoverlapping(&template.fragment, &mut fragment.descriptor, 1);
            }
            let (ta_idx, frag_idx) = unsafe {
                (
                    template.ta_idx.assume_init_ref(),
                    template.frag_idx.assume_init_ref(),
                )
            };
            let patch_res =
                TemplateSlot::patch(ta_idx, frag_idx, ta, &mut fragment.descriptor, args);
            let full_res =
                render::write_pair_descriptors(&mut template.verify_ta, &mut template.verify_fragment, args);
            match (patch_res, full_res) {
                (Ok(()), Ok(())) => {
                    if let Some(offset) = first_diff(ta, &template.verify_ta) {
                        unsafe {
                            core::ptr::copy_nonoverlapping(&template.verify_ta, ta, 1);
                            core::ptr::copy_nonoverlapping(
                                &template.verify_fragment,
                                &mut fragment.descriptor,
                                1,
                            );
                        }
                        template.state = State::Rejected;
                        template.slot_gen[slot] = 0;
                        template.slot_mcache[slot] = 0;
                        Path::Mismatch { stage: 0, offset }
                    } else if let Some(offset) =
                        first_diff(&fragment.descriptor, &template.verify_fragment)
                    {
                        unsafe {
                            core::ptr::copy_nonoverlapping(&template.verify_ta, ta, 1);
                            core::ptr::copy_nonoverlapping(
                                &template.verify_fragment,
                                &mut fragment.descriptor,
                                1,
                            );
                        }
                        template.state = State::Rejected;
                        template.slot_gen[slot] = 0;
                        template.slot_mcache[slot] = 0;
                        Path::Mismatch { stage: 1, offset }
                    } else {
                        template.state = State::Admitted;
                        template.slot_gen[slot] = template.generation;
                        template.slot_mcache[slot] = 0;
                        Path::Verified
                    }
                }
                (Err(_), Ok(())) => {
                    unsafe {
                        core::ptr::copy_nonoverlapping(&template.verify_ta, ta, 1);
                        core::ptr::copy_nonoverlapping(
                            &template.verify_fragment,
                            &mut fragment.descriptor,
                            1,
                        );
                    }
                    template.state = State::Rejected;
                    template.slot_gen[slot] = 0;
                    template.slot_mcache[slot] = 0;
                    Path::Mismatch {
                        stage: 2,
                        offset: 0,
                    }
                }
                (_, Err(err)) => {
                    template.slot_gen[slot] = 0;
                    return Err(err);
                }
            }
        }
        (State::Rejected, true) => {
            render::write_pair(ta, fragment, args)?;
            template.slot_gen[slot] = 0;
            template.slot_mcache[slot] = 0;
            Path::Full
        }
        _ => {
            render::write_pair(ta, fragment, args)?;
            template.slot_mcache[slot] = 0;
            if template.capture(args, ta, &fragment.descriptor) {
                template.slot_gen[slot] = template.generation;
                Path::Full
            } else {
                template.slot_gen[slot] = 0;
                Path::Unmapped
            }
        }
    };

    record_path(path);

    let mcache = fragment.set_mcache(
        args.fragment.descriptor_va,
        args.resources.aux_fb_va,
        attachments,
        context,
    )?;
    if !attachments.is_empty() && attachments.len() + 1 <= MCACHE_INLINE_RANGES {
        template.slot_mcache[slot] = (attachments.len() + 1) as u8;
    }
    Ok((mcache, path))
}

#[kunit_tests(asahi_g17_render_template)]
mod template_tests {
    use super::*;
    use crate::g17::fw::{
        compute::{IntersectionTables, ScratchRequest},
        render::{RenderPass, RenderResources, RenderStage, UscProgram},
    };

    const USER_BASE: u64 = 0x10_0000_0000;

    fn bytes_of<T>(val: &T) -> &[u8] {
        // SAFETY: `TaDescriptor` and `FragmentSlot` are packed integer-only structs.
        unsafe { core::slice::from_raw_parts(core::ptr::from_ref(val).cast::<u8>(), size_of::<T>()) }
    }

    fn zero_box<T>() -> KBox<T> {
        let mut b = KBox::<T>::new_uninit(GFP_KERNEL).unwrap();
        // SAFETY: Called only for POD integer descriptor structs.
        unsafe {
            core::ptr::write_bytes(b.as_mut_ptr(), 0, 1);
            b.assume_init()
        }
    }

    fn zero_kvbox<T>() -> KVBox<T> {
        let mut b = KVBox::<T>::new_uninit(GFP_KERNEL).unwrap();
        // SAFETY: Called only for arrays of POD integer descriptor structs.
        unsafe {
            core::ptr::write_bytes(b.as_mut_ptr(), 0, 1);
            b.assume_init()
        }
    }

    fn base_fixture() -> (RenderPass, RenderResources) {
        // SAFETY: Both structs contain only integers, bools, and integer structs.
        let mut pass: RenderPass = unsafe { core::mem::zeroed() };
        let mut resources: RenderResources = unsafe { core::mem::zeroed() };
        pass.width = 1920;
        pass.height = 1080;
        pass.layers = 1;
        pass.utile_width = 32;
        pass.utile_height = 32;
        pass.sample_size = 4;
        pass.vdm_va = USER_BASE + 0x10_0000;
        pass.scissor_va = USER_BASE + 0x11_0000;
        pass.depth_bias_va = USER_BASE + 0x12_0000;
        pass.occlusion_query_va = USER_BASE + 0x13_0000;
        pass.sampler_heap_va = USER_BASE + 0x14_0000;
        pass.sampler_count = 8;
        pass.usc_base = USER_BASE + 0x20_0000;
        pass.bg = UscProgram { offset: 0x100, rsrc_spec: 0x11 };
        pass.partial_bg = UscProgram { offset: 0x200, rsrc_spec: 0x12 };
        pass.eot_offset = 0x300;
        pass.partial_eot_offset = 0x400;

        resources.context_id = 4;
        resources.context_generation = 2;
        resources.pb_slot = 1;
        resources.free_list_slot = 2;
        resources.free_list_control_va = 0xffff_f000_0001_0000;
        resources.kick_record_va = 0xffff_f000_0002_0000;
        resources.pb_va = USER_BASE + 0x30_0000;
        resources.scene_va = USER_BASE + 0x40_0000;
        resources.scene_end_va = USER_BASE + 0x41_0000;
        resources.tilemap_va = USER_BASE + 0x50_0000;
        resources.layermeta_va = USER_BASE + 0x51_0000;
        resources.heapmeta_va = USER_BASE + 0x52_0000;
        resources.tpc_va = USER_BASE + 0x53_0000;
        resources.tpc_size = 0x10000;
        resources.gtp_va = core::array::from_fn(|i| USER_BASE + 0x60_0000 + i as u64 * 0x1000);
        resources.deflake_va = USER_BASE + 0x70_0000;
        resources.ta_status_va = 0xffff_f000_0003_0000;
        resources.fragment_status_va = 0xffff_f000_0004_0000;
        resources.fragment_status_fw_va = 0xffff_f000_0004_0000;
        resources.aux_fb_va = USER_BASE + 0x80_0000;
        resources.timestamp_va = 0xffff_f000_0005_0000;
        (pass, resources)
    }

    fn make_args<'a>(
        pass: &'a mut RenderPass,
        resources: &'a mut RenderResources,
        ordinal: u64,
        programs: RegisterPrograms,
        scs: bool,
    ) -> RenderArgs<'a> {
        let step = ordinal * 0x100;
        pass.vdm_va = USER_BASE + 0x10_0000 + step;
        pass.scissor_va = USER_BASE + 0x11_0000 + step;
        pass.depth_bias_va = USER_BASE + 0x12_0000 + step;
        pass.occlusion_query_va = USER_BASE + 0x13_0000 + step;
        pass.sampler_heap_va = if ordinal % 3 == 0 { 0 } else { USER_BASE + 0x14_0000 + step };
        pass.sampler_count = if ordinal % 3 == 0 { 0 } else { (ordinal as u32 % 16) + 1 };
        pass.depth_clear = 0x3f80_0000u32.wrapping_add(ordinal as u32);
        pass.stencil_clear = (ordinal as u32) & 0xff;
        pass.bg.offset = 0x100 + ((ordinal as u32 & 0x1f) << 4);
        pass.partial_bg.offset = 0x200 + ((ordinal as u32 & 0x1f) << 4);
        pass.eot_offset = 0x300 + ((ordinal as u32 & 0x1f) << 4);
        pass.partial_eot_offset = 0x400 + ((ordinal as u32 & 0x1f) << 4);

        resources.kick_record_va = 0xffff_f000_0002_0000 + step;
        resources.scene_va = USER_BASE + 0x40_0000 + step;
        resources.pm_scratch = USER_BASE + 0x42_0000 + step;
        resources.pm_metric = USER_BASE + 0x43_0000 + step;
        resources.tilemap_va = USER_BASE + 0x50_0000 + step;
        resources.layermeta_va = USER_BASE + 0x51_0000 + step;
        resources.heapmeta_va = USER_BASE + 0x52_0000 + step;
        resources.deflake_va = USER_BASE + 0x70_0000 + step;
        resources.ta_status_va = 0xffff_f000_0003_0000 + step;
        resources.fragment_status_va = 0xffff_f000_0004_0000 + step;
        resources.fragment_status_fw_va = 0xffff_f000_0004_0000 + step;
        resources.aux_fb_va = USER_BASE + 0x80_0000 + step;
        resources.timestamp_va = 0xffff_f000_0005_0000 + step;
        resources.timestamp_window = [
            0x1000 + ordinal,
            0x2000 + ordinal,
            0x3000 + ordinal,
            0x4000 + ordinal,
        ];
        resources.ta_ids = ObjectIds {
            predecessor: (ordinal as u32).wrapping_add(10),
            current: (ordinal as u32).wrapping_add(11),
        };
        resources.fragment_ids = ObjectIds {
            predecessor: (ordinal as u32).wrapping_add(20),
            current: (ordinal as u32).wrapping_add(21),
        };

        let ray = RayQueryStage {
            intersection: IntersectionTables::NONE,
            scratch: ScratchRequest::new(if scs { (1 << 32) | (1 << 24) | 8 } else { 0 }).unwrap(),
        };
        RenderArgs {
            pass,
            resources,
            ray_query: [ray; 2],
            ordinal,
            pair_idle: ordinal % 4 == 0,
            ta: RenderStage {
                qid: 5,
                descriptor_va: USER_BASE + 0x90_0000 + (ordinal % RING_SLOTS as u64) * 0x1000,
                state_word: (ordinal as u32) & 0x3f,
                stamp_va: 0xffff_f000_0006_0000,
                aux_stamp_va: 0xffff_f000_0006_0008,
            },
            fragment: RenderStage {
                qid: 6,
                descriptor_va: USER_BASE + 0xa0_0000 + (ordinal % RING_SLOTS as u64) * 0x3000,
                state_word: (ordinal as u32).wrapping_add(3) & 0x3f,
                stamp_va: 0xffff_f000_0006_0010,
                aux_stamp_va: 0xffff_f000_0006_0018,
            },
            programs,
            ta_mode_1: false,
            clusters: 2,
            deflake: Deflake::FOUR_CLUSTERS,
        }
    }

    #[test]
    fn template_matches_full_across_programs_scs_and_ring_wraparound() {
        // `encode` keeps per-slot state for `ordinal % RING_SLOTS` and expects
        // that index to name the same descriptor storage every time, as the
        // driver's `RECORDS`-deep descriptor ring does.
        let mut ta_ring: KVBox<[TaDescriptor; RING_SLOTS]> = zero_kvbox();
        let mut frag_ring: KVBox<[FragmentSlot; RING_SLOTS]> = zero_kvbox();
        let mut ref_ta: KBox<TaDescriptor> = zero_box();
        let mut ref_frag: KBox<FragmentSlot> = zero_box();
        let mut slot = TemplateSlot::new_box().unwrap();
        let (mut pass, mut resources) = base_fixture();

        for programs in [RegisterPrograms::G17P, RegisterPrograms::V25G83] {
            for scs in [false, true] {
                for i in 0u64..40 {
                    // Ordinals `0..4` and `32..68` wrap around the ring twice.
                    let ordinal = if i < 4 { i } else { i + 28 };
                    let idx = (ordinal as usize) % RING_SLOTS;
                    let args = make_args(&mut pass, &mut resources, ordinal, programs, scs);
                    let mc = match i % 5 {
                        0 => 0,
                        1 => 4,
                        2 => 1,
                        3 => 0,
                        _ => 6,
                    };
                    let mut atts = [WriteRange { va: 0, size: 0 }; 6];
                    for (k, a) in atts[..mc].iter_mut().enumerate() {
                        *a = WriteRange {
                            va: USER_BASE + 0xc0_0000 + ((i + k as u64) * 0x4000),
                            size: 0x4000,
                        };
                    }
                    let (mcache, path) =
                        encode(&mut ta_ring[idx], &mut frag_ring[idx], &args, &atts[..mc], 4, &mut slot)
                            .unwrap();
                    let expected = match i {
                        0 => Path::Full,
                        1 => Path::Verified,
                        _ => Path::Patched,
                    };
                    assert_eq!(path, expected);

                    ref_ta.write(&args).unwrap();
                    ref_frag.write(&args).unwrap();
                    let ref_mcache = ref_frag
                        .set_mcache(args.fragment.descriptor_va, args.resources.aux_fb_va, &atts[..mc], 4)
                        .unwrap();
                    assert_eq!(mcache.is_some(), ref_mcache.is_some());
                    assert_eq!(bytes_of(&ta_ring[idx]), bytes_of(&*ref_ta));
                    assert_eq!(
                        bytes_of(&frag_ring[idx].descriptor),
                        bytes_of(&ref_frag.descriptor)
                    );
                }
            }
        }
    }

    #[test]
    fn mismatch_rejects_and_stays_rejected_until_key_changes() {
        let mut ta: KBox<TaDescriptor> = zero_box();
        let mut frag: KBox<FragmentSlot> = zero_box();
        let mut ref_ta: KBox<TaDescriptor> = zero_box();
        let mut ref_frag: KBox<FragmentSlot> = zero_box();
        let mut slot = TemplateSlot::new_box().unwrap();
        let (mut pass, mut resources) = base_fixture();

        let args0 = make_args(&mut pass, &mut resources, 0, RegisterPrograms::G17P, false);
        assert_eq!(encode(&mut ta, &mut frag, &args0, &[], 4, &mut slot).unwrap().1, Path::Full);
        // Corrupt golden template in Pending state.
        slot.ta.unk_876 ^= 1;

        let args1 = make_args(&mut pass, &mut resources, 1, RegisterPrograms::G17P, false);
        let (_, path1) = encode(&mut ta, &mut frag, &args1, &[], 4, &mut slot).unwrap();
        assert!(matches!(path1, Path::Mismatch { stage: 0, .. }));
        ref_ta.write(&args1).unwrap();
        ref_frag.write(&args1).unwrap();
        assert_eq!(bytes_of(&*ta), bytes_of(&*ref_ta));
        assert_eq!(bytes_of(&frag.descriptor), bytes_of(&ref_frag.descriptor));

        // Subsequent matching render stays Full (Rejected).
        let args2 = make_args(&mut pass, &mut resources, 2, RegisterPrograms::G17P, false);
        assert_eq!(encode(&mut ta, &mut frag, &args2, &[], 4, &mut slot).unwrap().1, Path::Full);

        // Changing a keyed field re-enters Pending and verifies cleanly.
        pass.width = 1280;
        let args3 = make_args(&mut pass, &mut resources, 3, RegisterPrograms::G17P, false);
        assert_eq!(encode(&mut ta, &mut frag, &args3, &[], 4, &mut slot).unwrap().1, Path::Full);
        let args4 = make_args(&mut pass, &mut resources, 4, RegisterPrograms::G17P, false);
        assert_eq!(encode(&mut ta, &mut frag, &args4, &[], 4, &mut slot).unwrap().1, Path::Verified);
    }
}
