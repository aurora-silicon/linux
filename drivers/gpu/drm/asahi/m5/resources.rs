// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![allow(dead_code)]

//! Retained RAM for the J813 G17G/25G76 initialization graph.
//!
//! Construction only: no processor start, mailbox publication, work submission
//! or DRM registration. The eventual boot session must retain this entire
//! owner until BOTH firmware processors are stopped, including on failure.
//! M5 UAT lifecycle admission remains separate from this memory layout.

use super::mapped::MappedObject;
use crate::{driver::AsahiDrmDevice, m5::initdata as init, mmu};
use crate::m5::completion::{graphics_pointer_head, MAX_GRAPHICS_GENERATION};
use core::sync::atomic::{fence, Ordering};
use kernel::prelude::*;
use kernel::sync::Arc;

mod firmware_view;
mod graphics_memory;
mod graphics_usc;
mod parameter_buffer;
mod target_memory;
mod graphics_job;
mod graphics_queues;
mod application_compute;

const KERNEL_BASE: u64 = 0xffff_fc20_0000_0000;
const PRIVATE_BASE: u64 = KERNEL_BASE + 0x2_0000;
const ROOTS_BASE: u64 = KERNEL_BASE + 0x1a_8000;
const SHARED_BASE: u64 = KERNEL_BASE + 0xc078_8000;
const FWCTL_BASE: u64 = KERNEL_BASE + 0x1c_0000;
const SENTINEL_BASE: u64 = KERNEL_BASE + 0xc07e_0000;
const PBDESC_BASE: u64 = KERNEL_BASE + 0x15e_0000;
const UMA_BASE: u64 = KERNEL_BASE + 0x15e_8000;

/// Owning mappings are dropped before the UAT. Aliases retain the SAME GEM
/// backing; no firmware pointer refers to a copied snapshot or temporary map.
pub(crate) struct M5FirmwareMemory {
    primary_root: MappedObject,
    secondary_root: MappedObject,
    shared: MappedObject,
    private: MappedObject,
    region_a: MappedObject,
    region_c: MappedObject,
    fwctl: MappedObject,
    aon: MappedObject,
    qos: MappedObject,
    pbdesc: MappedObject,
    uma: MappedObject,
    completions: KVec<MappedObject>,
    aliases: KVec<mmu::KernelMapping>,
    registers: KVec<mmu::KernelMapping>,
    graphics: Option<(graphics_job::M5GraphicsJob, graphics_queues::M5GraphicsQueues)>,
    application_compute: Option<application_compute::M5ApplicationCompute>,
    application_outer_head: u32,
    report_counts: [[u32; 2]; 2],
    trace_printed: [u32; 2],
    compute_kick_observed: bool,
    compute_dispatch_returned: bool,
    graphics_stamp_flags: u128,
    uat: Arc<mmu::Uat>,
}

impl M5FirmwareMemory {
    /// Internal defaults for the initial M5 render-state envelope. The DRM
    /// translator replaces all program/input addresses, and owned target
    /// allocation replaces private storage before any queue publication.
    pub(crate) fn reference_render_profile() ->
        (crate::m5::render_desc::G17pRenderParameters, crate::m5::render::M5RenderControls) {
        graphics_job::triangle_parameters()
    }

    /// All fallible preparation happens before publication. The caller must
    /// pass a prepared M5 UAT while both processors await initdata. The
    /// caller retains its own Arc until both CPUs stop, including when this
    /// unpublished graph fails construction. There is no Neo tuning fallback.
    #[inline(never)]
    pub(crate) fn new_with_uat(
        dev: &AsahiDrmDevice,
        uat: Arc<mmu::Uat>,
        boot_timestamp: u64,
    ) -> Result<KBox<Self>> {
        let memory = Self::allocate_with_uat(dev, uat, boot_timestamp)?;
        fence(Ordering::SeqCst);
        Ok(memory)
    }

    #[inline(never)]
    fn allocate_with_uat(
        dev: &AsahiDrmDevice,
        uat: Arc<mmu::Uat>,
        boot_timestamp: u64,
    ) -> Result<KBox<Self>> {
        let mut registers = KVec::new();
        for window in init::M5_25G76_REGISTER_WINDOWS {
            let (physical, virtual_address, span) = window.mapped_pages().map_err(|_| EINVAL)?;
            let protection = match window.flag {
                0 => mmu::PROT_FW_MMIO_RO,
                2 => mmu::PROT_FW_MMIO_RW,
                _ => return Err(EINVAL),
            };
            // Page-table writes only. No register contents are accessed.
            registers.push(uat.kernel_vm().map_io(dev.as_ref().as_ref(), virtual_address, physical.try_into()?,
                span.try_into()?, protection)?, GFP_KERNEL)?;
        }
        let fixed = |address, size, protection| MappedObject::new_wc_at(
            dev, uat.kernel_vm(), address, size, protection);
        // Install every fixed address before allocator-selected objects.
        let mut private = fixed(PRIVATE_BASE, init::M5_25G76_PRIVATE_CLUSTER_SIZE, mmu::PROT_FW_SHARED_RW)?;
        // Separate mappings retain the empty page between the two roots.
        let mut primary_root = fixed(ROOTS_BASE, init::ROOT_SIZE_PRIMARY, mmu::PROT_FW_SHARED_RO)?;
        let mut secondary_root = fixed(ROOTS_BASE + init::SECONDARY_ROOT_DELTA,
            init::ROOT_SIZE_SECONDARY, mmu::PROT_FW_SHARED_RO)?;
        let mut shared = fixed(SHARED_BASE, init::NATIVE_SHARED_CLUSTER_SIZE, mmu::PROT_FW_PRIV_RW)?;
        let region_a = fixed(KERNEL_BASE + 0x1b_8000, init::REGION_A_SIZE, mmu::PROT_FW_SHARED_RO)?;
        let mut region_c = fixed(KERNEL_BASE + 0x1a_0000, init::REGION_C_SIZE, mmu::PROT_FW_SHARED_RO)?;
        let fwctl = fixed(FWCTL_BASE, 0x4000, mmu::PROT_FW_SHARED_RW)?;
        // These two tables have zero-backed mappings at handoff. Mode6
        // prepares its own UMA slot before any compute publication.
        let mut pbdesc = fixed(PBDESC_BASE, 0x4000, mmu::PROT_FW_SHARED_RW)?;
        let mut uma = fixed(UMA_BASE, 0x4000, mmu::PROT_FW_SHARED_RW)?;
        let mut aliases = KVec::new();
        for (object, address) in [(&mut pbdesc, init::REGION_VIEW_LOW_ADDRS[0]),
                                  (&mut uma, init::REGION_VIEW_LOW_ADDRS[1])] {
            aliases.push(object.map_alias_at(uat.kernel_lower_vm(), address,
                mmu::PROT_GPU_SHARED_RW)?, GFP_KERNEL)?;
            if uat.kernel_vm().translate_iova(object.iova())?
                != uat.kernel_lower_vm().translate_iova(address)? {
                return Err(EFAULT);
            }
        }

        let mut completions = KVec::new();
        let mut completion_bindings = [init::G17PKsmCompletionAliases { low: 0, high: 0 }; 4];
        for (ordinal, binding) in completion_bindings.iter_mut().enumerate() {
            // The fixed alias pairs are measured on M5, including ordinals
            // 1 and 3 which are absent from the older Neo initialization.
            let low = 0x70_0184_8000 + ordinal as u64 * 0x2_8000;
            let high = KERNEL_BASE + 0xc07e_8000 + ordinal as u64 * 0x2_8000;
            let mut backing = MappedObject::new_wc_at(dev, uat.kernel_vm(), high,
                init::M5_25G76_COMPLETION_BACKING_SIZE, mmu::PROT_FW_PRIV_RW)?;
            aliases.push(backing.map_alias_at(uat.kernel_lower_vm(), low,
                mmu::PROT_GPU_SHARED_RW)?, GFP_KERNEL)?;
            for offset in (0..init::M5_25G76_COMPLETION_BACKING_SIZE).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(high + offset as u64)?
                    != uat.kernel_lower_vm().translate_iova(low + offset as u64)? {
                    return Err(EFAULT);
                }
            }
            *binding = init::G17PKsmCompletionAliases { low, high };
            completions.push(backing, GFP_KERNEL)?;
        }
        // Keep allocator-selected objects below the high fixed graph, and
        // above the PB/UMA tables. This cannot occupy the deferred blank
        // sentinel address or its neighboring guard page.
        let dynamic = KERNEL_BASE + 0x168_0000..KERNEL_BASE + 0x1000_0000;
        let qos = MappedObject::new_wc_in_range(dev, uat.kernel_vm(), dynamic.clone(),
            0xe40, mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW)?;
        let mut aon = MappedObject::new_wc_in_range(dev, uat.kernel_vm(), dynamic,
            init::M5_25G76_AON_PTD_SIZE,
            mmu::UAT_PGSZ as u64, mmu::PROT_FW_PRIV_RW)?;
        aon.with_bytes_mut(|out| init::encode_m5_25g76_aon_ptd(out).map_err(|_| EINVAL))?;
        region_c.with_bytes_mut(|out| init::encode_region_c_for_profile(
            init::RootProfile::J813G17G25G76, out).map_err(|_| EINVAL))?;
        private.with_bytes_mut(|out| init::encode_m5_25g76_private_cluster(
            FWCTL_BASE, out).map_err(|_| EINVAL))?;

        let addresses = init::SharedAddresses {
            region_a: region_a.iova(), region_c: region_c.iova(),
            hw_data_bundle: SHARED_BASE, hw_data_bundle_alloc: init::NATIVE_SHARED_CLUSTER_SIZE,
        };
        let primary = init::InstanceAddresses {
            root: ROOTS_BASE, main_config: SHARED_BASE + init::NATIVE_PRIMARY_MAIN_BUNDLE_OFFSET as u64,
            status_a: PRIVATE_BASE + 0xee40, status_b: PRIVATE_BASE + 0x340,
            secondary_extras: [0, 0],
        };
        let secondary = init::InstanceAddresses {
            root: ROOTS_BASE + init::SECONDARY_ROOT_DELTA,
            main_config: SHARED_BASE + init::NATIVE_SECONDARY_MAIN_BUNDLE_OFFSET as u64,
            status_a: PRIVATE_BASE + 0xc3100, status_b: 0,
            secondary_extras: [primary.status_b + init::PRIMARY_STATUS_B_CONFIG_OFFSET as u64,
                               PRIVATE_BASE + 0x177400],
        };
        let views = init::PrimaryRegionViews {
            sentinel_high: SENTINEL_BASE, alias_high: [PBDESC_BASE, UMA_BASE],
        };
        let trace_mask = crate::module_parameters::m5_fw_trace.value();
        if trace_mask & !0x203 != 0 { return Err(EINVAL); }
        // Exact M5 GFX firmware 0x1fadc retains root+0xa8 (status A),
        // then 0x3074..0x3088 copies status-A+0 into its trace flags.
        // Bits 0 and 1 gate power/submission records; 0x6260 reads the
        // NEXT byte and tests bit 1 (full-word bit 9) for recovery summary
        // 0x909. Publish through the host field before either descriptor.
        let native_trace = private.read_u32(primary.status_a)?;
        private.write_u32(primary.status_a, native_trace | trace_mask)?;
        shared.with_bytes_mut(|out| {
            init::encode_m5_25g76_hw_data(completion_bindings, qos.iova(),
                &mut out[..init::M5_25G76_HW_DATA_SIZE]).map_err(|_| EINVAL)?;
            init::encode_m5_25g76_bundle_policy(init::M5PolicyBindings {
                state: PRIVATE_BASE + 0x177480, aon_ptd: aon.iova(), boot_timestamp,
            }, out).map_err(|_| EINVAL)?;
            for (role, instance, grid, views) in [
                (init::InstanceRole::Primary, &primary, PRIVATE_BASE, Some(&views)),
                (init::InstanceRole::Secondary, &secondary, PRIVATE_BASE + 0x1770c0, None),
            ] {
                let channels = init::derive_channel_table(role, grid, instance.status_a,
                    instance.main_config, SHARED_BASE);
                let offset = (instance.main_config - SHARED_BASE) as usize;
                init::encode_main_config_for_profile(init::RootProfile::J813G17G25G76,
                    role, &addresses, instance, &channels, views,
                    &mut out[offset..offset + init::MAIN_CONFIG_SIZE]).map_err(|_| EINVAL)?;
            }
            Ok(())
        })?;
        primary_root.with_bytes_mut(|out| {
            init::encode_root_for_profile(init::RootProfile::J813G17G25G76,
                init::InstanceRole::Primary, &addresses, &primary,
                out).map_err(|_| EINVAL)
        })?;
        secondary_root.with_bytes_mut(|out| {
            init::encode_root_for_profile(init::RootProfile::J813G17G25G76,
                init::InstanceRole::Secondary, &addresses, &secondary,
                out).map_err(|_| EINVAL)
        })?;
        // Unlike PB/UMA, this future blank sentinel is unmapped at native
        // handoff. It becomes zero-backed later in macOS initialization.
        if uat.kernel_vm().translate_iova(SENTINEL_BASE).is_ok() {
            return Err(EINVAL);
        }
        // Writes to all CPU WC mappings precede any future handoff. Only
        // the two measured control openings are present; work rings and
        // later compute/render descriptors remain unsubmitted.
        fence(Ordering::SeqCst);
        // Keep the retained graph off the kernel stack, including
        // construction and error-return temporaries in the boot caller.
        KBox::init(try_init!(Self { primary_root, secondary_root, shared, private, region_a, region_c, fwctl, aon, qos,
                  pbdesc, uma, completions, aliases, registers, graphics: None,
                  report_counts: [[0; 2]; 2], trace_printed: [0; 2],
                  compute_kick_observed: false, compute_dispatch_returned: false,
                  graphics_stamp_flags: 0, application_compute: None,
                  application_outer_head: 0, uat }), GFP_KERNEL)
    }

    /// Prepare a first application job while retaining its actual VM. No
    /// native program bundle or captured input/output pages are requested.
    /// The caller has validated the command and all user mapping coverage.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn initialize_application_graphics(
        &mut self, dev: &AsahiDrmDevice, vm: &mmu::Vm,
        parameters: crate::m5::render_desc::G17pRenderParameters,
        controls: crate::m5::render::M5RenderControls, clusters: u32,
        usc_required: [u64; 2],
    ) -> Result {
        if self.graphics.is_some() { return Err(EBUSY); }
        let job = graphics_job::M5GraphicsJob::from_vm(
            dev, &self.uat, vm, parameters, controls, clusters, usc_required)?;
        self.attach_graphics(dev, job)
    }

    fn attach_graphics(&mut self, dev: &AsahiDrmDevice, job: graphics_job::M5GraphicsJob) -> Result {
        if self.graphics.is_some() { return Err(EBUSY); }
        let queues = graphics_queues::M5GraphicsQueues::new(dev, &self.uat, &job)?;
        let pb = job.parameter_descriptor()?;
        self.pbdesc.with_bytes_mut(|out| {
            let base = graphics_job::PB_HARDWARE_ID as usize * 16;
            for (index, word) in pb.into_iter().enumerate() {
                out[base + index * 4..base + index * 4 + 4].copy_from_slice(&word.to_le_bytes());
            }
            Ok(())
        })?;
        let usc = job.uma_descriptor();
        self.uma.with_bytes_mut(|out| {
            let base = graphics_usc::HARDWARE_ID as usize * 32;
            for (index, word) in usc.into_iter().enumerate() {
                out[base + index * 8..base + index * 8 + 8].copy_from_slice(&word.to_le_bytes());
            }
            Ok(())
        })?;
        self.graphics = Some((job, queues));
        Ok(())
    }

    pub(crate) fn quarantine_graphics(&self) {
        if let Some((job, _)) = &self.graphics { job.quarantine(); }
    }

    /// Drain M5 report channels using the native producer contracts.
    /// GFX 0x6ca8..0x6d18 emits 256 x 0x48 event records; 0x17e18
    /// emits 512 x 0x48 trace records. In each cursor object the host owns
    /// +0 and firmware owns +0x20. Copy before releasing each slot.
    pub(crate) fn drain_reports(&mut self, dev: &AsahiDrmDevice) -> Result<bool> {
        let mut recovery = false;
        for (role, status_offset) in [0xee40u64, 0xc3100].into_iter().enumerate() {
            for (channel, (cursor_offset, ring_offset, capacity)) in
                [(0x40u64, 0x2c0u64, 256u32), (0x240, 0xa6ac0, 512)]
                    .into_iter().enumerate() {
                let cursor = PRIVATE_BASE + status_offset + cursor_offset;
                let ring = (status_offset + ring_offset) as usize;
                for _ in 0..128 {
                    let consumer = self.private.read_u32(cursor)?;
                    let producer = self.private.read_u32(cursor + 0x20)?;
                    if consumer >= capacity || producer >= capacity { return Err(EIO); }
                    if consumer == producer { break; }
                    fence(Ordering::Acquire);
                    let offset = ring + consumer as usize * 0x48;
                    let mut raw = [0u8; 0x48];
                    self.private.with_bytes_mut(|bytes| {
                        raw.copy_from_slice(bytes.get(offset..offset + 0x48).ok_or(EINVAL)?);
                        Ok(())
                    })?;
                    let word = |off: usize| u32::from_le_bytes(raw[off..off + 4].try_into().unwrap());
                    let qword = |off: usize| u64::from_le_bytes(raw[off..off + 8].try_into().unwrap());
                    let count = self.report_counts[role][channel];
                    if channel == 0 {
                        // Type 1 (graphics stamps) arrives for every job.
                        if word(0) == 1 {
                            dev_dbg!(dev.as_ref(), "M5_EVENT role={} ordinal={} type={} payload={:x?}\n",
                                role, count, word(0), [qword(4), qword(12), qword(20), qword(28), qword(36)]);
                        } else {
                            dev_dbg!(dev.as_ref(), "M5_EVENT role={} ordinal={} type={} payload={:x?}\n",
                                role, count, word(0), [qword(4), qword(12), qword(20), qword(28), qword(36)]);
                        }
                        recovery |= word(0) == 4;
                        if role == 0 && word(0) == 1 {
                            self.graphics_stamp_flags |= u128::from(qword(4))
                                | (u128::from(qword(12)) << 64);
                        }
                    } else {
                        let args = [qword(12), qword(20), qword(28), qword(36)];
                        // This one-shot diagnostic owns qid11/stamp1. The
                        // exact M5 trace point precedes the hardware kick;
                        // the admission probe also requires its later callback
                        // return trace. Firmware-private counters can be cached.
                        if role == 0 && word(0) == 5 && word(44) == 0x41
                            && args == [11, 1, 2, 1] {
                            self.compute_kick_observed = true;
                        }
                        // 0x26aa4 invokes the pending-work callback, which
                        // dispatches tag14 and writes SGX+0x21018 at0x11f5c.
                        // 0x26ac4 emits 0x130 only after that callback returns.
                        if role == 0 && self.compute_kick_observed && word(0) == 5
                            && word(44) == 0x130 && args == [4, 0, 0, 0] {
                            self.compute_dispatch_returned = true;
                        }
                        // Exact M5 0x24eb4/0x26754 emit zero-argument
                        // processing-loop brackets. Count and consume them
                        // without spending the diagnostic print budget.
                        let loop_marker = word(0) == 5 && args == [0; 4]
                            && matches!(word(44) & 0x00ff_ffff, 0x105 | 0x106);
                        if !loop_marker && self.trace_printed[role] < if role == 0 { 512 } else { 16 } {
                            dev_dbg!(dev.as_ref(), "M5_TRACE role={} ordinal={} timestamp={} type={} id={:#x} args={:x?}\n",
                                role, count, qword(4), word(0), word(44), args);
                            self.trace_printed[role] += 1;
                        }
                    }
                    self.report_counts[role][channel] = count.saturating_add(1);
                    fence(Ordering::SeqCst);
                    self.private.write_u32(cursor, (consumer + 1) % capacity)?;
                }
            }
        }
        Ok(recovery)
    }

    pub(crate) fn graphics_status(&mut self) -> Result<([u32; 3], [[u32; 3]; 2], [u32; 5])> {
        let mut outer = [0; 3];
        let base = init::WORK_STATE_GRID_OFFSETS[8];
        for index in 0..3 {
            outer[index] = self.private.read_u32(PRIVATE_BASE
                + (base + index * init::CHANNEL_STATE_SPACING) as u64)?;
        }
        let (job, queues) = self.graphics.as_mut().ok_or(EINVAL)?;
        Ok((outer, queues.counters()?, job.progress()?))
    }

    pub(crate) fn graphics_completion(&mut self) -> Result<crate::m5::completion::M5GraphicsCompletion> {
        let (job, queues) = self.graphics.as_mut().ok_or(EINVAL)?;
        let progress = job.progress()?;
        let scheduler = job.scheduler_status()?;
        Ok(crate::m5::completion::M5GraphicsCompletion {
            submitted: progress[0], completed: scheduler[0], linked: scheduler[1],
            list: queues.completion_list()?, stamps: [progress[1], progress[3]],
            signalled: self.graphics_stamp_flags,
        })
    }

    pub(crate) fn graphics_pair_complete(
        completion: crate::m5::completion::M5GraphicsCompletion,
        generation: u32,
    ) -> bool {
        (1..=MAX_GRAPHICS_GENERATION).contains(&generation)
            && completion.pair_complete_at(graphics_queues::JOB_LIST, generation * 0x100)
    }

    pub(crate) fn graphics_installation_status(&mut self) -> Result<([[u64; 5]; 2], [u64; 2], [u64; 2])> {
        self.graphics.as_mut().ok_or(EINVAL)?.1.installation_status()
    }

    pub(crate) fn graphics_queues_installed(&mut self) -> Result<bool> {
        self.graphics.as_mut().ok_or(EINVAL)?.1.installed()
    }

    pub(crate) fn publish_graphics_install(&mut self) -> Result {
        let (outer, inner, progress) = self.graphics_status()?;
        if outer != [0; 3] || inner != [[0, 0, 1]; 2]
            || progress != [2, 0, 0, 0, 0] { return Err(EBUSY); }
        if self.graphics_installation_status()? != ([[0; 5]; 2], [0; 2], [0xffff, 0]) {
            return Err(EBUSY);
        }
        self.publish_graphics_records(0, 1)
    }

    pub(crate) fn publish_graphics_job(&mut self) -> Result {
        let (outer, inner, progress) = self.graphics_status()?;
        if outer != [2; 3] || inner != [[1; 3]; 2]
            || progress != [2, 0, 0, 0, 0] { return Err(EBUSY); }
        let (job, queues) = self.graphics.as_mut().ok_or(EINVAL)?;
        if !queues.installed()? { return Err(EBUSY); }
        self.graphics_stamp_flags = 0;
        job.publish_context()?;
        queues.publish_draw()?;
        self.publish_graphics_records(2, 4)
    }

    /// Reuse only a fully retired pair, replacing its application descriptors
    /// while preserving firmware-owned PB/USC state and the installed queues.
    #[allow(clippy::too_many_arguments)]
    /// A different submitting VM takes over the retired job in place: same
    /// context slot, descriptor graph, queues and PB/USC pools. The compute
    /// owner shares that context, so its operands follow the job.
    fn hand_off_application(&mut self, dev: &AsahiDrmDevice, vm: &mmu::Vm) -> Result {
        let (job, _) = self.graphics.as_mut().ok_or(EINVAL)?;
        if job.owns_vm(vm) { return Ok(()); }
        job.rebind_vm(dev, vm).inspect_err(|e| pr_err!("M5: rebind_vm failed: {:?}\n", e))?;
        job.publish_context().inspect_err(|e| pr_err!("M5: publish_context failed: {:?}\n", e))?;
        Ok(())
    }

    pub(crate) fn publish_next_application_job(
        &mut self, dev: &AsahiDrmDevice, vm: &mmu::Vm,
        parameters: crate::m5::render_desc::G17pRenderParameters,
        controls: crate::m5::render::M5RenderControls, clusters: u32,
        usc_required: [u64; 2], generation: u32,
    ) -> Result {
        if !(2..=MAX_GRAPHICS_GENERATION).contains(&generation) { return Err(EINVAL); }
        let previous = generation - 1;
        let (outer, inner, _) = self.graphics_status()?;
        if outer != [self.application_outer_head; 3]
            || inner != [[graphics_pointer_head(previous); 3]; 2]
            || !Self::graphics_pair_complete(self.graphics_completion()?, previous) {
            return Err(EBUSY);
        }
        self.hand_off_application(dev, vm)?;
        let (job, queues) = self.graphics.as_mut().ok_or(EINVAL)?;
        job.prepare_application_frame(dev, vm, parameters, controls, clusters,
            usc_required, generation).inspect_err(|e| pr_err!("M5: prepare_application_frame failed: {:?}\n", e))?;
        queues.publish_next_draw(job, generation).inspect_err(|e| pr_err!("M5: publish_next_draw failed: {:?}\n", e))?;
        self.graphics_stamp_flags = 0;
        self.publish_graphics_records(self.application_outer_head as usize,
            graphics_pointer_head(generation) as u16)
    }

    fn publish_graphics_records(&mut self, outer_index: usize, inner_head: u16) -> Result {
        // Only recycle storage after every outer consumer reaches the old
        // producer. The application caller also requires paired retirement.
        if outer_index >= 256 || self.graphics_status()?.0 != [outer_index as u32; 3] {
            return Err(EBUSY);
        }
        self.shared.with_bytes_mut(|out| {
            for stage in 0..2 {
                let offset = init::WORK_RING_BUNDLE_OFFSETS[8] + ((outer_index + stage) % 256) * 24;
                let record = &mut out[offset..offset + 24];
                record.fill(0);
                let address = graphics_queues::M5GraphicsQueues::queue_address(stage)?;
                record[8..16].copy_from_slice(&address.to_le_bytes());
                record[16..20].copy_from_slice(&2u32.to_le_bytes());
                record[20..22].copy_from_slice(&inner_head.to_le_bytes());
                record[22] = graphics_job::QUEUE_IDS[stage];
                record[23] = 1;
            }
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        // Both stage-tagged SKSM queues travel through the CL_2 command ring.
        let producer = init::WORK_STATE_GRID_OFFSETS[8] + 2 * init::CHANNEL_STATE_SPACING;
        self.private.write_u32(PRIVATE_BASE + producer as u64, ((outer_index + 2) % 256) as u32)?;
        self.application_outer_head = ((outer_index + 2) % 256) as u32;
        fence(Ordering::SeqCst);
        Ok(())
    }

    pub(crate) fn application_outer_head(&self) -> u32 { self.application_outer_head }

    /// Initial admission is deliberately one compute job after retired graphics.
    /// No private diagnostic context, queue 11, or replacement UMA is installed.
    pub(crate) fn initialize_application_compute(&mut self, dev: &AsahiDrmDevice,
        vm: &mmu::Vm, command: &crate::m5::command::TranslatedComputeCommand,
        graphics_generation: u32) -> Result {
        if self.application_compute.is_some() || self.application_outer_head + 2 > 256 {
            return Err(ENOTSUPP);
        }
        let (outer, inner, _) = self.graphics_status()?;
        if graphics_generation == 0 || outer != [self.application_outer_head; 3]
            || inner != [[graphics_pointer_head(graphics_generation); 3]; 2]
            || !Self::graphics_pair_complete(self.graphics_completion()?, graphics_generation) {
            return Err(EBUSY);
        }
        self.hand_off_application(dev, vm)?;
        let job = &self.graphics.as_ref().ok_or(EINVAL)?.0;
        self.application_compute = Some(application_compute::M5ApplicationCompute::new(
            dev, &self.uat, job, command)?);
        Ok(())
    }

    pub(crate) fn application_compute_installed(&mut self) -> Result<bool> {
        let compute = self.application_compute.as_mut().ok_or(EINVAL)?;
        Ok(compute.counters()? == [1; 3] && compute.installed()?)
    }

    /// Publish one command-ring record for queue 12. `dispatch` false is the
    /// installation record; true is dispatch generation `generation`, whose
    /// inner head is the producer that generation's three pointers reach.
    pub(crate) fn publish_application_compute(&mut self, dispatch: bool,
        generation: u32) -> Result {
        let outer_index = self.application_outer_head as usize;
        if outer_index >= 256 || self.graphics_status()?.0 != [outer_index as u32; 3] {
            return Err(EBUSY);
        }
        let inner_head = u16::try_from(application_compute::compute_pointer_head(
            if dispatch { generation } else { 0 })).map_err(|_| EINVAL)?;
        let compute = self.application_compute.as_mut().ok_or(EINVAL)?;
        if dispatch {
            // Preserve the retired graphics notifications for later render reuse.
            self.graphics_stamp_flags &= !(1u128 << application_compute::QUEUE_ID);
            compute.publish_dispatch(generation)?;
        } else if compute.counters()? != [0, 0, 1] { return Err(EBUSY); }
        let offset = init::WORK_RING_BUNDLE_OFFSETS[8] + outer_index * 24;
        self.shared.with_bytes_mut(|out| {
            let record = &mut out[offset..offset + 24];
            // Every outer consumer reached this slot (checked above), so its
            // prior user, graphics or compute, has retired. Graphics records
            // wrap this ring after 128 renders, so even the first compute
            // record can land on a used slot.
            record.fill(0);
            record[8..16].copy_from_slice(
                &application_compute::M5ApplicationCompute::queue_address().to_le_bytes());
            record[16..20].copy_from_slice(&2u32.to_le_bytes());
            record[20..22].copy_from_slice(&inner_head.to_le_bytes());
            record[22] = application_compute::QUEUE_ID;
            record[23] = 1;
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        let head = ((outer_index + 1) % 256) as u32;
        let producer = init::WORK_STATE_GRID_OFFSETS[8] + 2 * init::CHANNEL_STATE_SPACING;
        self.private.write_u32(PRIVATE_BASE + producer as u64, head)?;
        self.application_outer_head = head;
        fence(Ordering::SeqCst);
        Ok(())
    }

    /// Stage a further dispatch on the already installed compute queue. The
    /// installed owner keeps its VM, USC and operand storage; only the command
    /// stream and the generation's stamps change.
    pub(crate) fn prepare_next_application_compute(&mut self, dev: &AsahiDrmDevice,
        vm: &mmu::Vm, command: &crate::m5::command::TranslatedComputeCommand,
        generation: u32) -> Result {
        if !(2..=MAX_GRAPHICS_GENERATION).contains(&generation) { return Err(EINVAL); }
        self.hand_off_application(dev, vm)?;
        let (job, _) = self.graphics.as_ref().ok_or(EINVAL)?;
        self.application_compute.as_mut().ok_or(EINVAL)?
            .prepare_next_dispatch(job, command, generation)
    }

    pub(crate) fn application_compute_complete(&mut self, generation: u32) -> Result<bool> {
        let (outer, _, _) = self.graphics_status()?;
        Ok(outer == [self.application_outer_head; 3]
            && self.application_compute.as_mut().ok_or(EINVAL)?
                .complete(self.graphics_stamp_flags, generation)?)
    }

    pub(crate) fn application_compute_status(&mut self)
        -> Result<([u32; 3], crate::m5::completion::M5ComputeCompletion)> {
        let compute = self.application_compute.as_mut().ok_or(EINVAL)?;
        Ok((compute.counters()?, compute.completion(self.graphics_stamp_flags)?))
    }

    pub(crate) fn publish_idle_wake(&mut self) -> Result {
        if self.control_counters()? != [[1; 3]; 2] { return Err(EBUSY); }
        self.shared.with_bytes_mut(|out| {
            let record = &mut out[0x1eac0..0x1eb00];
            if record.iter().any(|x| *x != 0) { return Err(EBUSY); }
            record[..4].copy_from_slice(&0x0au32.to_le_bytes());
            Ok(())
        })?;
        fence(Ordering::SeqCst);
        let producer = init::WORK_STATE_GRID_OFFSETS[init::CONTROL_CHANNEL_INDEX]
            + 2 * init::CHANNEL_STATE_SPACING;
        self.private.write_u32(PRIVATE_BASE + producer as u64, 2)?;
        fence(Ordering::SeqCst);
        Ok(())
    }

    pub(crate) fn root(&self, secondary: bool) -> u64 {
        if secondary { self.secondary_root.iova() } else { self.primary_root.iova() }
    }

    /// Read the actual shared control state, not a detached host copy. The
    /// offsets and three pointers per role are checked by the M5 capture
    /// verifier against both native main objects and the private arena.
    pub(crate) fn control_counters(&mut self) -> Result<[[u32; 3]; 2]> {
        self.private.with_bytes_mut(|bytes| {
            let mut counters = [[0; 3]; 2];
            for (role, grid) in [0usize, 0x1770c0].into_iter().enumerate() {
                let base = grid + init::WORK_STATE_GRID_OFFSETS[init::CONTROL_CHANNEL_INDEX];
                for (index, value) in counters[role].iter_mut().enumerate() {
                    let offset = base + index * init::CHANNEL_STATE_SPACING;
                    let word = bytes.get(offset..offset + 4).ok_or(EINVAL)?;
                    // SAFETY: arena and native counter offsets are u32 aligned;
                    // this bounded WC mapping is retained while firmware runs.
                    *value = unsafe { core::ptr::read_volatile(word.as_ptr().cast::<u32>()) };
                }
            }
            fence(Ordering::Acquire);
            Ok(counters)
        })
    }

}
