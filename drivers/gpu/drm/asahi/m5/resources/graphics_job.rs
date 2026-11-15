// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Owned TA/fragment resource graph for the bounded native-program test.
//! Construction leaves roots and queues unpublished. Submission wiring must
//! retain this owner through completion, faults and firmware shutdown.

use super::{graphics_memory::M5GraphicsMemory, graphics_usc, MappedObject};
use super::parameter_buffer::{M5ParameterBuffer, BLOCK_BYTES, PAGE_LIST_BYTES, PAGE_COUNT};
use super::target_memory::M5TargetMemory;
use crate::{driver::AsahiDrmDevice, m5::render_desc, m5::render, mmu};
use crate::m5::completion::MAX_GRAPHICS_GENERATION;
use kernel::prelude::*;

const HIGH: u64 = 0xffff_fc20_c100_0000;
const LOW: u64 = 0x70_0030_0000;
const BYTES: usize = 0x40000;
const TA: usize = 0;
const FRAGMENT: usize = 0x4000;
const SCHEDULER: usize = 0x8000;
const SIGNALS_HIGH: u64 = HIGH + 0x80000;
const SUBMITTED: usize = 0;
const PM: usize = 0xc000;
const PM_SHARED: usize = 0x10000;
const PM_COUNTER: usize = 0x14000;
const PM_BLOCKS: usize = 0x18000;
const USC: usize = 0x1c000;
const USC_INNER: usize = 0x20000;
const USC_STATISTICS: usize = 0x24000;
const SCENES: usize = 0x30000;
const SCENE_ZERO: usize = SCENES + 80 * 0x80;
const TIMESTAMPS: usize = 0x38000;
const STAMPS_A: usize = 0x4000;
const STAMPS_B: usize = 0x8000;
const PAGE_LIST_HIGH: u64 = HIGH + 0x40000;
const METRICS_HIGH: u64 = HIGH + 0x60000;
const USER_BASE: u64 = 0x10_0000_0000;
pub(super) const PB_HARDWARE_ID: u32 = 0;
pub(super) const QUEUE_IDS: [u8; 2] = [10, 11];
// Match Mesa's framebuffer dimensions; target allocations follow each size.
// Parameter-pool growth for more complex scenes remains separate.
use crate::m5::command::M5_MAX_RENDER_DIMENSION as MAX_APPLICATION_DIMENSION;
/// Retired targets kept for reuse. A video player cycles through a handful of
/// pass sizes every frame and a compositor's frames alternate with it, so
/// allocating per size change put several milliseconds on most jobs.
const TARGET_CACHE_ENTRIES: usize = 8;
/// Compute operands (128 KiB), a preemption page and a zero robustness page.
const COMPUTE_STORAGE_BYTES: usize = 0x28000;

pub(super) struct M5GraphicsJob {
    // The first owner's context retracts its roots before page release.
    program: M5GraphicsMemory,
    parameter_buffer: M5ParameterBuffer,
    /// Storage of application compute, reserved with the parameter buffer so that its
    /// address is free in every VM the job is handed to.
    compute_storage: MappedObject,
    target_memory: M5TargetMemory,
    aliases: KVec<mmu::KernelMapping>,
    // PB/USC pool views in a VM this job was handed to; empty for the first
    // owner, whose pools were allocated in its own VM.
    vm_aliases: KVec<mmu::KernelMapping>,
    graph_alias: mmu::KernelMapping,
    graph: MappedObject,
    signals: MappedObject,
    descriptor_template: KVec<u8>,
    scene_template: KVec<u8>,
    generation: u32,
    target_shape: [u32; 5],
    /// The VM `target_memory` lives in; a handoff leaves it behind.
    target_vm: u64,
    /// Retired targets by (VM, shape), least recently used first.
    target_cache: KVec<(u64, [u32; 5], M5TargetMemory)>,
}

impl M5GraphicsJob {

    /// Build a job from the submitting VM and translated application state.
    /// No captured program is loaded; the caller supplies stage controls and
    /// USC requirements derived for its program. Submission remains explicit.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn from_vm(
        dev: &AsahiDrmDevice, uat: &mmu::Uat, vm: &mmu::Vm,
        parameters: render_desc::G17pRenderParameters, controls: render::M5RenderControls,
        clusters: u32, usc_required: [u64; 2],
    ) -> Result<Self> {
        let program = M5GraphicsMemory::from_vm(dev, uat, vm)?;
        Self::from_memory(dev, uat, program, parameters, controls, clusters, usc_required)
    }

    #[allow(clippy::too_many_arguments)]
    fn from_memory(
        dev: &AsahiDrmDevice, uat: &mmu::Uat, program: M5GraphicsMemory,
        mut p: render_desc::G17pRenderParameters, controls: render::M5RenderControls,
        clusters: u32, usc_required: [u64; 2],
    ) -> Result<Self> {
        if !(1..=MAX_APPLICATION_DIMENSION).contains(&p.width)
            || !(1..=MAX_APPLICATION_DIMENSION).contains(&p.height)
            || p.layers != 1 || clusters != 1 {
            return Err(ENOTSUPP);
        }
        let mut parameter_buffer = M5ParameterBuffer::new_reference(dev, &program)?;
        let compute_storage = program.allocate_compact(
            dev, COMPUTE_STORAGE_BYTES, 0x8000, mmu::PROT_GPU_SHARED_RW)?;
        let target_memory = M5TargetMemory::new(dev, &program, &p, clusters)?;
        target_memory.apply(&mut p);
        p.cycle = parameter_buffer.scene_cycle();
        // Keep the PM register and firmware scene descriptor on the same
        // owned metrics slot. The captured address is unmapped in a new VM.
        p.record_index = render::parameter_buffer_metrics_record(
            parameter_buffer.metrics() + 0x40).map_err(|_| EINVAL)?;
        p.ta_hardware_buffer_id = PB_HARDWARE_ID;
        p.usc_flist_hardware_buffer_id = graphics_usc::HARDWARE_ID;
        p.ta_timestamp_start = HIGH + TIMESTAMPS as u64;
        p.ta_timestamp_end = p.ta_timestamp_start + 8;
        p.fragment_timestamp_start = p.ta_timestamp_start;
        p.fragment_timestamp_end = p.ta_timestamp_end;
        let mut graph = MappedObject::new_wc_at(dev, uat.kernel_vm(), HIGH,
            BYTES, mmu::PROT_FW_PRIV_RW)?;
        // Native submitted totals and host-visible stamps are uncached in
        // firmware. Keep them separate from the cached descriptor graph.
        let mut signals = MappedObject::new_wc_at(dev, uat.kernel_vm(), SIGNALS_HIGH,
            0xc000, mmu::PROT_FW_SHARED_RW)?;
        signals.write_u32(SIGNALS_HIGH + SUBMITTED as u64, 2)?;
        let graph_alias = graph.map_alias_at(uat.kernel_lower_vm(), LOW,
            mmu::PROT_GPU_SHARED_RW)?;
        for offset in (0..BYTES).step_by(mmu::UAT_PGSZ) {
            if uat.kernel_vm().translate_iova(HIGH + offset as u64)?
                != uat.kernel_lower_vm().translate_iova(LOW + offset as u64)? {
                return Err(EFAULT);
            }
        }
        let aliases = parameter_buffer.firmware_aliases(
            uat, program.vm(), PAGE_LIST_HIGH, METRICS_HIGH,
        )?;
        let block_page_bases = parameter_buffer.block_page_bases()?;
        graph.with_bytes_mut(|out| {
            put64(out, SCHEDULER, SIGNALS_HIGH + SUBMITTED as u64);
            put32(out, SCHEDULER + 0x10, 0x50);
            // Two submitted stage records; firmware compares its completed
            // count with this total, then unlinks the scheduler.
            for (block, base) in block_page_bases.into_iter().enumerate() {
                put32(out, PM_BLOCKS + block * 8, base);
            }
            put32(out, PM_SHARED, 3);
            put32(out, PM_SHARED + 4, 3);
            put32(out, PM_SHARED + 0x60, 1);
            put32(out, PM_COUNTER, 1);
            for (offset, value) in [(0xc, PB_HARDWARE_ID), (0x30, PAGE_LIST_BYTES as u32),
                (0x34, PAGE_COUNT), (0x38, 1650), (0x3c, 3), (0x54, PAGE_COUNT - 1),
                (0x58, BLOCK_BYTES as u32), (0x7c, 1650 * 16), (0x80, 550 * 16)] {
                put32(out, PM + offset, value);
            }
            for (offset, address) in [(0x20, PAGE_LIST_HIGH), (0x28, parameter_buffer.page_list()),
                (0x44, HIGH + PM_BLOCKS as u64), (0x4c, HIGH + PM_SHARED as u64),
                (0x64, HIGH + PM_COUNTER as u64), (0x84, parameter_buffer.discard_offset())] {
                put64(out, PM + offset, address);
            }
            // This sole job owns scene 1. M5 metrics use 64-byte spacing,
            // unlike Neo dwords. Other reserved scenes remain unassigned;
            // do not guess a scratch-ring wrap rule from a single draw.
            let selected_scene = SCENES + 0x80;
            put64(out, selected_scene, parameter_buffer.metrics() + 0x40);
            put64(out, selected_scene + 8, METRICS_HIGH + 0x40);
            put64(out, selected_scene + 0x28, parameter_buffer.scene_cycle());
            put64(out, selected_scene + 0x40, HIGH + PM_SHARED as u64 + 0x40);
            put64(out, SCENES + 0x80 + 0x4c, 1);
            put32(out, USC_INNER, 1);
            program.usc().encode_initial_owner(HIGH + USC_INNER as u64,
                &mut out[USC..USC + 0x70])?;

            Self::encode_descriptors(out, &p, &controls, program.context_id(), usc_required)?;
            Ok(())
        })?;
        let mut descriptor_template = KVec::new();
        let mut scene_template = KVec::new();
        graph.with_bytes_mut(|out| {
            descriptor_template.extend_from_slice(&out[..SCHEDULER], GFP_KERNEL)?;
            scene_template.extend_from_slice(&out[SCENES + 0x80..SCENES + 0x100], GFP_KERNEL)?;
            Ok(())
        })?;
        let target_vm = program.vm().id();
        Ok(Self { program, parameter_buffer, compute_storage, target_memory, aliases,
            vm_aliases: KVec::new(),
            graph_alias, graph, signals, descriptor_template, scene_template, generation: 1,
            target_shape: [p.width, p.height, u32::from(p.layers),
                u32::from(p.utile_width), u32::from(p.utile_height)],
            target_vm, target_cache: KVec::new() })
    }

    #[allow(clippy::too_many_arguments)]
    fn encode_descriptors(
        out: &mut [u8], p: &render_desc::G17pRenderParameters,
        controls: &render::M5RenderControls, context_id: u8,
        usc_required: [u64; 2],
    ) -> Result {
        let objects = render_desc::RenderDescriptorObjects {
            record_a: HIGH + SCHEDULER as u64,
            shared: HIGH + PM as u64,
            record_b: HIGH + SCENES as u64 + 0x80,
            zero: HIGH + SCENE_ZERO as u64,
        };
        let metadata = render_desc::RenderDescriptorMetadata {
            context_id: context_id as u32, submission_ordinal: 0, command_buffer_state_word: 0,
            ta_hardware_buffer_id: PB_HARDWARE_ID, submit_sequence: 1,
        };
        render::build_ta_descriptor_base(p, controls, objects, metadata,
            LOW + TA as u64, &mut out[TA..TA + render_desc::TA_DESCRIPTOR_SIZE])
            .map_err(|_| EINVAL)?;
        render::build_fragment_descriptor_base(p, controls, objects,
            render_desc::RenderDescriptorMetadata { submit_sequence: 0, ..metadata },
            LOW + FRAGMENT as u64,
            &mut out[FRAGMENT..FRAGMENT + render_desc::FRAGMENT_DESCRIPTOR_SIZE])
            .map_err(|_| EINVAL)?;
        let (ta, fragment) = out.split_at_mut(FRAGMENT);
        let ta_usc = render::M5RenderUscState {
            control: HIGH + USC as u64, required_bytes: usc_required[0],
            allocated_bytes: graphics_usc::CAPACITY_BYTES,
            statistics: HIGH + USC_STATISTICS as u64,
        };
        render::apply_usc_submission_state(ta, fragment, ta_usc,
            render::M5RenderUscState {
                required_bytes: usc_required[1], statistics: ta_usc.statistics + 0x40, ..ta_usc
            }).map_err(|_| EINVAL)?;
        // This bounded first submission owns hardware stamp1 in each
        // queue and event-machine timestamp0x100. Do not import native
        // allocator history (the captured queues had stamps2/3).
        put32(ta, 0x79c, QUEUE_IDS[1] as u32);
        put64(ta, 0x7a0, 0x100);
        put64(ta, 0x7a8, 1);
        ta[0x7b0] = 1;
        ta[0x7b1] = 1;
        put64(ta, 0x8a6, SIGNALS_HIGH + STAMPS_A as u64 + QUEUE_IDS[0] as u64 * 4);
        put64(ta, 0x8ae, SIGNALS_HIGH + STAMPS_B as u64 + QUEUE_IDS[0] as u64 * 4);
        put32(ta, 0x8b6, 0x100);
        put32(ta, 0x8ba, QUEUE_IDS[0] as u32);
        put64(ta, 0x8c6, (1u64 << 40) | 1);
        put64(fragment, 0x2140, SIGNALS_HIGH + STAMPS_A as u64 + QUEUE_IDS[1] as u64 * 4);
        put64(fragment, 0x2148, SIGNALS_HIGH + STAMPS_B as u64 + QUEUE_IDS[1] as u64 * 4);
        put32(fragment, 0x2150, 0x100);
        put32(fragment, 0x2154, QUEUE_IDS[1] as u32);
        put32(fragment, 0x215c, 0);
        put64(fragment, 0x2160, (1u64 << 40) | 1);
        // Fixed 256x256 reference controls independently observed in
        // the successful 25G76 draw; these replace inherited Neo values.
        ta[0x789] = 8;
        ta[0x85f] = 7;
        put32(ta, 0x892, 0);
        ta[0x932] = 0;
        put32(fragment, 0x1fb0, 8);
        fragment[0x21cc] = 0;
        fragment[0x222d] = 0;
        Ok(())
    }

    pub(super) fn parameter_descriptor(&self) -> Result<[u32; 4]> {
        render::initial_parameter_buffer_descriptor(self.parameter_buffer.page_list(), PAGE_COUNT)
            .map_err(|_| EINVAL)
    }

    pub(super) fn uma_descriptor(&self) -> [u64; 4] { self.program.usc().descriptor() }

    pub(super) fn roots(&self) -> [u64; 2] { self.program.roots() }

    pub(super) fn context_id(&self) -> u8 { self.program.context_id() }

    pub(super) fn owns_vm(&self, vm: &mmu::Vm) -> bool {
        self.program.vm().id() == vm.id()
    }

    /// Hand this fully retired job to another submitting VM. The hardware
    /// context slot, descriptor graph, signals, queues and the PB/USC pools
    /// all keep their firmware-visible addresses: the context is re-leased
    /// for the new VM and the pools are aliased into it. The previous VM's
    /// render targets are not used in the new VM: they stay cached under the
    /// previous VM for its next turn, and the next application frame takes or
    /// allocates targets in the new VM. The caller publishes the new roots
    /// before the next draw.
    pub(super) fn rebind_vm(&mut self, dev: &AsahiDrmDevice, vm: &mmu::Vm) -> Result {
        if self.owns_vm(vm) { return Ok(()); }
        if self.scheduler_status()? != [2, 0] { return Err(EBUSY); }
        let progress = self.progress()?;
        if progress[0] != 2 || progress[1] != self.generation * 0x100
            || progress[3] != self.generation * 0x100 { return Err(EBUSY); }
        // Release an earlier handoff's views before reserving the same
        // addresses in the incoming VM.
        self.vm_aliases = KVec::new();
        let mut aliases = self.program.rebind(dev, vm)?;
        for alias in self.parameter_buffer.alias_into(vm)? {
            aliases.push(alias, GFP_KERNEL)?;
        }
        let storage = self.compute_storage.iova();
        if let Some(alias) = self.compute_storage.map_alias_at_with_guards(
            vm, storage, mmu::PROT_GPU_SHARED_RW, 2)? {
            aliases.push(alias, GFP_KERNEL)?;
        }
        self.vm_aliases = aliases;
        dev_dbg!(dev.as_ref(), "M5_RENDER_VM_HANDOFF context={} generation={} vm={}\n",
            self.program.context_id(), self.generation, vm.id());
        Ok(())
    }

    /// Compute shares this application's published roots and USC owner. Only
    /// its operand/preemption storage is its own: the GPU address of that storage.
    pub(super) fn compute_storage(&self) -> u64 {
        self.compute_storage.iova()
    }

    pub(super) fn descriptor_addresses(&self, stage: usize) -> Result<[u64; 2]> {
        let offset = match stage { 0 => TA, 1 => FRAGMENT, _ => return Err(EINVAL) };
        Ok([HIGH + offset as u64, LOW + offset as u64])
    }

    pub(super) fn usc_control(&self) -> u64 { HIGH + USC as u64 }

    pub(super) fn parameter_state(&self) -> u64 { HIGH + PM as u64 }

    pub(super) fn publish_context(&mut self) -> Result { self.program.publish() }

    pub(super) fn quarantine(&self) { self.program.quarantine(); }

    pub(super) fn progress(&mut self) -> Result<[u32; 5]> {
        Ok([self.signals.read_u32(SIGNALS_HIGH + SUBMITTED as u64)?,
            self.signals.read_u32(SIGNALS_HIGH + STAMPS_A as u64 + QUEUE_IDS[0] as u64 * 4)?,
            self.signals.read_u32(SIGNALS_HIGH + STAMPS_B as u64 + QUEUE_IDS[0] as u64 * 4)?,
            self.signals.read_u32(SIGNALS_HIGH + STAMPS_A as u64 + QUEUE_IDS[1] as u64 * 4)?,
            self.signals.read_u32(SIGNALS_HIGH + STAMPS_B as u64 + QUEUE_IDS[1] as u64 * 4)?])
    }

    pub(super) fn scheduler_status(&mut self) -> Result<[u32; 2]> {
        // GFX cleans this 0xc8-byte object after unlinking the finished pair.
        Ok([self.graph.read_u32(HIGH + SCHEDULER as u64 + 0xc)?,
            self.graph.read_u32(HIGH + SCHEDULER as u64 + 0x94)?])
    }

    fn rebuild_retired_frame(&mut self, generation: u32) -> Result {
        if generation != self.generation + 1 || generation > MAX_GRAPHICS_GENERATION
            || self.scheduler_status()? != [2, 0] {
            return Err(EBUSY);
        }
        let progress = self.progress()?;
        if progress[0] != 2 || progress[1] != self.generation * 0x100
            || progress[3] != self.generation * 0x100 { return Err(EBUSY); }
        self.graph.with_bytes_mut(|out| {
            out[..SCHEDULER].copy_from_slice(&self.descriptor_template);
            out[SCHEDULER..SCHEDULER + 0xc8].fill(0);
            put64(out, SCHEDULER, SIGNALS_HIGH + SUBMITTED as u64);
            put32(out, SCHEDULER + 0x10, 0x50);
            out[SCENES + 0x80..SCENES + 0x100].copy_from_slice(&self.scene_template);
            out[TIMESTAMPS..TIMESTAMPS + 16].fill(0);
            let stamp = generation * 0x100;
            put64(out, TA + 0x7a0, stamp as u64);
            put64(out, TA + 0x7a8, generation as u64);
            put32(out, TA + 0x8b6, stamp);
            put64(out, TA + 0x8c6, (1u64 << 40) | generation as u64);
            put32(out, FRAGMENT + 0x2150, stamp);
            put64(out, FRAGMENT + 0x2160, (1u64 << 40) | generation as u64);
            Ok(())
        })?;
        // Leave previous stamps intact: they must be superseded by firmware,
        // never host-written to the expected new completion value.
        self.generation = generation;
        Ok(())
    }

    /// Re-encode a retired application's descriptors without replaying its old
    /// command pointers. The current bounded queue retains one VM. Target
    /// storage can change only after complete retirement; the job
    /// stamp keeps increasing and context replacement remains unsupported.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn prepare_application_frame(
        &mut self, dev: &AsahiDrmDevice, vm: &mmu::Vm, mut p: render_desc::G17pRenderParameters,
        controls: render::M5RenderControls, clusters: u32,
        usc_required: [u64; 2], generation: u32,
    ) -> Result {
        if vm.id() != self.program.vm().id() || clusters != 1
            || !(1..=MAX_APPLICATION_DIMENSION).contains(&p.width)
            || !(1..=MAX_APPLICATION_DIMENSION).contains(&p.height) || p.layers != 1
            || generation != self.generation + 1 || generation > MAX_GRAPHICS_GENERATION {
            return Err(ENOTSUPP);
        }
        // Validate retirement before allocating or releasing any target pages.
        // The caller additionally checks both queues, stamps and event bits.
        let progress = self.progress()?;
        if self.scheduler_status()? != [2, 0] || progress[0] != 2
            || progress[1] != self.generation * 0x100
            || progress[3] != self.generation * 0x100 { return Err(EBUSY); }
        let shape = [p.width, p.height, u32::from(p.layers),
            u32::from(p.utile_width), u32::from(p.utile_height)];
        let vm_id = self.program.vm().id();
        let replacement = if (vm_id, shape) != (self.target_vm, self.target_shape) {
            let cached = self.target_cache.iter()
                .position(|(cached_vm, cached_shape, _)| (*cached_vm, *cached_shape) == (vm_id, shape));
            Some(match cached {
                Some(index) => self.target_cache.remove(index).map_err(|_| EINVAL)?.2,
                None => M5TargetMemory::new(dev, &self.program, &p, clusters).inspect_err(|e| pr_err!("M5: target memory failed: {:?}\n", e))?,
            })
        } else { None };
        replacement.as_ref().unwrap_or(&self.target_memory).apply(&mut p);
        p.cycle = self.parameter_buffer.scene_cycle();
        p.record_index = render::parameter_buffer_metrics_record(
            self.parameter_buffer.metrics() + 0x40).map_err(|_| EINVAL)?;
        p.ta_hardware_buffer_id = PB_HARDWARE_ID;
        p.usc_flist_hardware_buffer_id = graphics_usc::HARDWARE_ID;
        p.ta_timestamp_start = HIGH + TIMESTAMPS as u64;
        p.ta_timestamp_end = p.ta_timestamp_start + 8;
        p.fragment_timestamp_start = p.ta_timestamp_start;
        p.fragment_timestamp_end = p.ta_timestamp_end;
        let mut descriptors = KVec::new();
        descriptors.extend_from_slice(&self.descriptor_template, GFP_KERNEL)?;
        Self::encode_descriptors(&mut descriptors, &p, &controls,
            self.program.context_id(), usc_required).inspect_err(|e| pr_err!("M5: encode_descriptors failed: {:?}\n", e))?;
        self.descriptor_template = descriptors;
        // The application owns its output. It must never be erased by the
        // native diagnostic's sentinel/reset logic.
        self.rebuild_retired_frame(generation).inspect_err(|e| pr_err!("M5: rebuild_retired_frame failed: {:?}\n", e))?;
        if let Some(target) = replacement {
            dev_dbg!(dev.as_ref(), "M5_RENDER_TARGET_RESIZE generation={} old={:?} new={:?}\n",
                generation, self.target_shape, shape);
            let retired = core::mem::replace(&mut self.target_memory, target);
            if self.target_cache.len() == TARGET_CACHE_ENTRIES {
                self.target_cache.remove(0).map_err(|_| EINVAL)?;
            }
            self.target_cache.push((self.target_vm, self.target_shape, retired), GFP_KERNEL)?;
            self.target_vm = vm_id;
            self.target_shape = shape;
        }
        Ok(())
    }

}

fn put32(out: &mut [u8], offset: usize, value: u32) {
    out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(out: &mut [u8], offset: usize, value: u64) {
    out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

pub(super) fn triangle_parameters() -> (render_desc::G17pRenderParameters, render::M5RenderControls) {
    let mut p = render_desc::G17pRenderParameters::source_rsrc8();
    p.aux_fb = 0x10000288000;
    p.aux_fb_flags = 0x10c001;
    p.aux_fb_page_count = 0x100000;
    p.context_base = USER_BASE;
    p.cycle = 0x220040;
    p.deflake_1 = 0x1000068340;
    p.deflake_2 = 0x1000068040;
    p.deflake_3 = 0x1000068000;
    p.depth_aux_buffer = 0;
    p.depth_aux_stride = 0;
    p.depth_buffer = 0;
    p.depth_clear_value_bits = 0x3f800000;
    p.depth_dimensions = 0;
    p.depth_flags = 0;
    p.depth_stride = 0;
    p.encoder = 0x1000018000;
    p.fragment_lifecycle = 0x4089f4c0508e74c;
    p.fragment_status = 0x1000258000;
    p.heapmeta = 0x1000261000;
    p.width = 256;
    p.height = 256;
    p.layers = 1;
    p.lifecycle = 0x4089f4c0508e74d;
    p.load_pipeline = 0x10000170200;
    p.load_pipeline_bind = 0xffff800000000040;
    p.merge_upper_x_bits = 0x3bddb3d9;
    p.merge_upper_y_bits = 0x3bddb3d9;
    p.multisample_control = 0x88;
    p.occlusion_query_base = 0;
    p.ppp_control = 0x202;
    p.record_index = 0x80041;
    p.stencil_aux_buffer = 0;
    p.stencil_aux_stride = 0;
    p.stencil_buffer = 0;
    p.stencil_clear_value = 0;
    p.stencil_stride = 0;
    p.store_pipeline = 0x10000170400;
    p.store_pipeline_bind = 0;
    p.ta_hardware_buffer_id = PB_HARDWARE_ID;
    p.ta_status = 0x1000078000;
    p.tib_blocks = 0;
    p.tile_config = 0x10280;
    p.tilemap = 0x1000260000;
    p.tpc = 0x1000288000;
    p.usc_flist_hardware_buffer_id = graphics_usc::HARDWARE_ID;
    p.utile_config = 0xa000;
    p.utile_height = 32;
    p.utile_width = 32;
    p.absolute_pointers = false;
    p.partial_store_pipeline_bind = 0;
    p.partial_store_pipeline = 0x10000170400;
    p.partial_load_pipeline_bind = 0xffff800000000040;
    p.partial_load_pipeline = 0x10000170000;
    p.depth_bias_array = 0x100002d8000;
    p.scissor_array = 0x10000180000;
    p.ta_timestamp_start = HIGH + TIMESTAMPS as u64;
    p.ta_timestamp_end = p.ta_timestamp_start + 8;
    p.fragment_timestamp_start = p.ta_timestamp_start;
    p.fragment_timestamp_end = p.ta_timestamp_end;
    let controls = render::M5RenderControls {
        ta: render::M5RenderStageControls {
            resource_flags: 0x4054b, resource_control: 0x200000001, ptc_max: 0xc6, work_key: 0x300,
        },
        fragment: render::M5RenderStageControls {
            resource_flags: 0x104054b, resource_control: 0x200000001, ptc_max: 0xce, work_key: 0x300,
        },
        ta_tile_control: 0x10c08860, ta_dispatch_control: 0x700,
        fragment_raster_control: 0x200000ff0200, fragment_tile_flags: 0x200003717f,
        store_pipeline_2_bind: 0, store_pipeline_2: 0x10000170400,
        partial_store_pipeline_2_bind: 0, partial_store_pipeline_2: 0x10000170400,
        partial_tile_flags: 0x200000,
    };
    (p, controls)
}
