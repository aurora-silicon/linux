// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! One application compute dispatch sharing the retained graphics VM and USC.
//!
//! Queue 12 is separate from TA/fragment queues 10/11. All publication and
//! completion gates are explicit; constructing this owner does not submit it.

use super::{graphics_job::M5GraphicsJob, graphics_usc,
    firmware_view::FirmwareDataView, MappedObject};
use crate::{driver::AsahiDrmDevice, m5::submission, m5::command,
    m5::channel, m5::completion::{graphics_pointer_head, GRAPHICS_POINTER_COUNT,
    MAX_GRAPHICS_GENERATION}, m5::compute, m5::sksm, mmu};
use kernel::prelude::*;

pub(super) const QUEUE_ID: u8 = 12;
const GRAPH: u64 = 0xffff_fc20_c008_0000;
const POINTERS: usize = 0x4000;
const ITEMS: usize = 0x8000;
/// Past the 0x500 command pointers the channel state declares, as for the
/// graphics queues, so the producer head wraps like theirs.
const CONFIG: usize = ITEMS + GRAPHICS_POINTER_COUNT as usize * 8;
const CONTROL: usize = 0xc000;
const _: () = assert!(CONFIG + 0x300 <= CONTROL);

/// Installation publishes one command pointer; every dispatch generation then
/// appends the same three -- descriptor, channel config, kick -- so the
/// producer head runs 1, 4, 7, ... (mod 0x500) exactly as
/// graphics_pointer_head() does for queues 10/11.
pub(super) const fn compute_pointer_head(generation: u32) -> u32 {
    graphics_pointer_head(generation)
}

pub(super) const fn compute_generation_admitted(generation: u32) -> bool {
    generation >= 1 && generation <= MAX_GRAPHICS_GENERATION
}

const LIST: u64 = GRAPH + 0x10000;
const ENTRY: u64 = 0xffff_fc20_003d_0000;
const ENTRY_LOW: u64 = 0x70_0063_0000;
const DESC: u64 = 0xffff_fc20_c040_0000;
const DESC_LOW: u64 = 0x70_0038_0000;
const SCHEDULER: u64 = DESC + 0x4000;
const ZERO: u64 = DESC + 0x8000;
const SIGNALS: u64 = DESC + 0x10000;

pub(super) struct M5ApplicationCompute {
    aliases: KVec<mmu::KernelMapping>,
    descriptor: MappedObject,
    graph: MappedObject,
    entry: MappedObject,
    list: MappedObject,
    signals: MappedObject,
    /// GPU address of the job's compute storage.
    storage: u64,
    /// The operands mapped into the VM that took the graphics job over.
    firmware: FirmwareDataView,
}

impl M5ApplicationCompute {
    pub(super) fn new(dev: &AsahiDrmDevice, uat: &mmu::Uat, job: &M5GraphicsJob,
        command: &command::TranslatedComputeCommand) -> Result<Self> {
        // The adapter excludes helpers, timestamps, legacy sampler registers
        // and the Neo add3 proof ABI before allocating or publishing anything.
        if command.sampler_heap != 0 || command.sampler_count != 0
            || command.g17p_add3_buffers.is_some() { return Err(ENOTSUPP); }
        // Validate observation support and vacancy before publishing a queue.
        let firmware = FirmwareDataView::new(dev)?;
        if firmware.queue_registration(QUEUE_ID)? != ([0; 5], 0) { return Err(EBUSY); }
        // 128 KiB operands, a preemption page and a zero robustness page.
        // Their addresses come from the application's reserved driver arena.
        let storage = job.compute_storage();
        let mut descriptor = MappedObject::new_wc_at(dev, uat.kernel_vm(), DESC,
            0x10000, mmu::PROT_FW_PRIV_RW)?;
        let mut entry = MappedObject::new_wc_at(dev, uat.kernel_vm(), ENTRY,
            0x20000, mmu::PROT_FW_PRIV_RW)?;
        let mut aliases = KVec::new();
        for (object, low, size) in [(&mut descriptor, DESC_LOW, 0x10000),
            (&mut entry, ENTRY_LOW, 0x20000)] {
            aliases.push(object.map_alias_at(uat.kernel_lower_vm(), low,
                mmu::PROT_GPU_SHARED_RW)?, GFP_KERNEL)?;
            for offset in (0..size).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(object.iova() + offset as u64)?
                    != uat.kernel_lower_vm().translate_iova(low + offset as u64)? {
                    return Err(EFAULT);
                }
            }
        }
        let mut signals = MappedObject::new_wc_at(dev, uat.kernel_vm(), SIGNALS,
            0x4000, mmu::PROT_FW_SHARED_RW)?;
        // The submitted count is the host's and stays put; firmware republishes
        // the completed count into the scheduler record each generation.
        signals.write_u32(SIGNALS, 1)?;
        Self::encode_descriptor(&mut descriptor, job, command, storage, 1)?;
        let mut graph = MappedObject::new_wc_at(dev, uat.kernel_vm(), GRAPH,
            0x10000, mmu::PROT_FW_PRIV_RW)?;
        let mut list = MappedObject::new_wc_at(dev, uat.kernel_vm(), LIST,
            0x4000, mmu::PROT_FW_SHARED_RW)?;
        list.with_bytes_mut(|out| { put64(out, 8, LIST); Ok(()) })?;
        let prepared = Self::prepare_entry(1)?;
        entry.with_bytes_mut(|out| {
            let offset = prepared.offset as usize;
            out[offset..offset + prepared.bytes.len()].copy_from_slice(&prepared.bytes);
            Ok(())
        })?;
        let configuration = |install| channel::encode_m5_channel_config(
            channel::M5ChannelConfig {
                ring_low: ENTRY_LOW, ring_high: ENTRY, queue_id: QUEUE_ID, priority: 2,
                install_queue: install, gpu_context: job.context_id(),
                event: if install { None } else { Some(channel::M5ChannelEvent {
                    address: job.usc_control(), timestamp: 1, index: graphics_usc::HARDWARE_ID,
                }) },
                channel_control: GRAPH + CONTROL as u64, update_scheduler: install,
                scheduler_index: 0, channel_stamp_index: 0,
                update_context: !install, owner_context: if install { 0 } else { 1 },
                extended_command: false,
            }).map_err(|_| EINVAL);
        let install = configuration(true)?;
        let config = configuration(false)?;
        let kick = Self::prepare_kick(1)?;
        graph.with_bytes_mut(|out| {
            let reset = submission::encode_m5_25g76_channel_state_reset(
                submission::M5ChannelStateResetValues {
                    uncached_state: GRAPH + POINTERS as u64,
                    command_pointer_ring: GRAPH + ITEMS as u64, gpu_va_10: LIST,
                    word_48: 0, gpu_va_9c: GRAPH + CONTROL as u64,
                });
            let queue = usize::from(QUEUE_ID) * 0xc0;
            out[queue..queue + 0xc0].copy_from_slice(&reset);
            for offset in [0x28, 0x2c, 0x40] { put32(out, queue + offset, 2); }
            out[queue + 0x36..queue + 0x38].fill(0xff);
            put32(out, POINTERS + 0x50, u32::MAX);
            put32(out, POINTERS + 0x60, 0x500);
            out[CONTROL..CONTROL + 2].fill(0xff);
            out[CONTROL + 5] = 1;
            out[CONTROL + 0x26] = 2;
            out[CONTROL + 0x33] = 0xff;
            for (offset, bytes) in [(CONFIG, &install[..]), (CONFIG + 0x100, &config[..]),
                (CONFIG + 0x200, &kick[..])] {
                out[offset..offset + bytes.len()].copy_from_slice(bytes);
            }
            for (index, address) in [GRAPH + CONFIG as u64, DESC,
                GRAPH + CONFIG as u64 + 0x100, GRAPH + CONFIG as u64 + 0x200]
                .into_iter().enumerate() { put64(out, ITEMS + index * 8, address); }
            put32(out, POINTERS + 0x40, 1);
            Ok(())
        })?;
        Ok(Self { aliases, descriptor, graph, entry, list, signals, storage,
            firmware })
    }

    pub(super) fn queue_address() -> u64 { GRAPH + QUEUE_ID as u64 * 0xc0 }

    /// Re-encode the whole descriptor for one dispatch generation. The
    /// scheduler record is cleared and re-seeded exactly as
    /// M5GraphicsJob::rebuild_retired_frame() does for queues 10/11: firmware
    /// republishes the completed count into it, so a stale one would satisfy
    /// the next generation's completion gate before the dispatch even ran.
    fn encode_descriptor(descriptor: &mut MappedObject, job: &M5GraphicsJob,
        command: &command::TranslatedComputeCommand, storage: u64,
        generation: u32) -> Result {
        if !compute_generation_admitted(generation) { return Err(EINVAL); }
        descriptor.with_bytes_mut(|out| {
            out[0x4000..0x4000 + 0xc8].fill(0);
            put64(out, 0x4000, SIGNALS);
            put32(out, 0x4010, 0x50);
            compute::encode_m5_compute_descriptor(compute::M5ComputeDescriptor {
                registers: compute::M5ComputeParameters {
                    preempt_base: storage + 0x20000,
                    cdm_base: command.control_stream_base,
                    dispatch_identity: 0x0000_0001_0000_0001,
                    context_id: job.context_id(), work_ordinal: 0,
                    robustness: storage + 0x24000,
                    operand_state_base: storage,
                    private_pool_hw_id: u64::from(graphics_usc::HARDWARE_ID),
                    descriptor_control_530: 0x54b, descriptor_control_538: 0xf5,
                    descriptor_flag_60a: false, descriptor_flag_60b: false,
                },
                submit_sequence: 0, scheduler_record: SCHEDULER,
                descriptor_low_alias: DESC_LOW,
                cdm_terminator: command.control_stream_terminator,
                dispatch_stamps: [SIGNALS + 0x40, SIGNALS + 0x80],
                event_machine_stamp: generation * 0x100, queue_id: u32::from(QUEUE_ID),
                queue_ordinal: 0, has_event: false, submission_index: generation,
                status: [SIGNALS + 0xc0, SIGNALS + 0xc8],
                shared_control: job.usc_control(), shared_control_enabled: true,
                event_metadata: [0; 16], zero_page: ZERO, owner_id: 1,
                descriptor_work_ordinal: 0,
            }, &mut out[..0x1000]).map_err(|_| EINVAL)
        })
    }

    fn prepare_entry(generation: u32) -> Result<sksm::PreparedM5SksmEntry> {
        if !compute_generation_admitted(generation) { return Err(EINVAL); }
        let stamp = u64::from(generation);
        sksm::prepare_m5_sksm_entry(sksm::M5SksmQueue {
            id: QUEUE_ID, priority: 2, current_stamp: stamp, submitted: 0,
            previous_stamp: if generation == 1 { None } else { Some(stamp - 1) },
        }, sksm::M5SksmOperands {
            barriers: &[], first_predecessor: 0, implicit_dependency_type: 1,
            mcache: None, flag_bit1: false, flag_bit61: false, flag_bit63: false,
            payload: [DESC, Self::queue_address()], qos_byte_32: 3, qos_class_40: 8,
            qos_word_48: 0xffff, event_mask: [0; 4], rce_kind: 0,
            rce: [sksm::M5SksmRce { address: DESC_LOW + 0x40, tag: 37 },
                sksm::M5SksmRce { address: DESC_LOW + 0x760, tag: 4 },
                sksm::M5SksmRce { address: 0, tag: 0 },
                sksm::M5SksmRce { address: 0, tag: 0 }],
            auxiliary: 0x3f_ffff_ffff_ffff,
        }).map_err(|_| EINVAL)
    }

    fn prepare_kick(generation: u32) -> Result<[u8; 21]> {
        if !compute_generation_admitted(generation) { return Err(EINVAL); }
        sksm::encode_m5_sksm_add_kicks(QUEUE_ID, 2, u64::from(generation), 1)
            .map_err(|_| EINVAL)
    }

    /// Stage a further dispatch on the installed queue. Nothing here is
    /// published: the producer head moves only in publish_dispatch().
    pub(super) fn prepare_next_dispatch(&mut self, job: &M5GraphicsJob,
        command: &command::TranslatedComputeCommand, generation: u32) -> Result {
        if generation < 2 || !compute_generation_admitted(generation) { return Err(EINVAL); }
        if self.counters()? != [compute_pointer_head(generation - 1); 3]
            || !self.installed()? { return Err(EBUSY); }
        Self::encode_descriptor(&mut self.descriptor, job, command,
            self.storage, generation)?;
        let prepared = Self::prepare_entry(generation)?;
        self.entry.with_bytes_mut(|out| {
            let offset = prepared.offset as usize;
            let slot = &mut out[offset..offset + prepared.bytes.len()];
            // Every preceding dispatch has retired, including the prior user
            // of this 256-entry SKSM slot. Preserve the first-use check.
            if generation < 256 && slot.iter().any(|byte| *byte != 0) {
                return Err(EBUSY);
            }
            slot.copy_from_slice(&prepared.bytes);
            Ok(())
        })?;
        let kick = Self::prepare_kick(generation)?;
        self.graph.with_bytes_mut(|out| {
            out[CONFIG + 0x200..CONFIG + 0x200 + kick.len()].copy_from_slice(&kick);
            for (index, address) in [DESC, GRAPH + CONFIG as u64 + 0x100,
                GRAPH + CONFIG as u64 + 0x200].into_iter().enumerate() {
                let absolute = 1 + (generation as usize - 1) * 3 + index;
                let offset = ITEMS + (absolute % GRAPHICS_POINTER_COUNT as usize) * 8;
                if absolute < GRAPHICS_POINTER_COUNT as usize
                    && out[offset..offset + 8].iter().any(|byte| *byte != 0) {
                    return Err(EBUSY);
                }
                put64(out, offset, address);
            }
            Ok(())
        })
    }

    pub(super) fn counters(&mut self) -> Result<[u32; 3]> {
        let mut out = [0; 3];
        for (index, offset) in [0, 0x30, 0x40].into_iter().enumerate() {
            out[index] = self.graph.read_u32(GRAPH + (POINTERS + offset) as u64)?;
        }
        Ok(out)
    }

    pub(super) fn installed(&mut self) -> Result<bool> {
        let (slot, scheduler) = self.firmware.queue_registration(QUEUE_ID)?;
        let header = self.graph.read_u32(GRAPH + CONTROL as u64)?;
        let bitmap = self.graph.read_u64(GRAPH + CONTROL as u64 + 0x12)?;
        Ok(slot[0] & 1 != 0 && slot[1] == ENTRY && slot[2] == ENTRY_LOW
            && slot[3] == 4u64 << 32 && slot[4] as u32 == 2
            && scheduler == GRAPH + CONTROL as u64
            && header & 0xff != 0xff && (header >> 8) & 0xff != 0xff
            && (header >> 16) & 0xff == 4 && bitmap == 1u64 << QUEUE_ID)
    }

    pub(super) fn publish_dispatch(&mut self, generation: u32) -> Result {
        if !compute_generation_admitted(generation) { return Err(EINVAL); }
        if self.counters()? != [compute_pointer_head(generation - 1); 3]
            || !self.installed()? { return Err(EBUSY); }
        self.graph.write_u32(GRAPH + POINTERS as u64 + 0x40,
            compute_pointer_head(generation))
    }

    pub(super) fn completion(&mut self, signalled: u128)
        -> Result<crate::m5::completion::M5ComputeCompletion> {
        Ok(crate::m5::completion::M5ComputeCompletion {
            submitted: self.signals.read_u32(SIGNALS)?,
            completed: self.descriptor.read_u32(SCHEDULER + 0xc)?,
            linked: self.descriptor.read_u32(SCHEDULER + 0x94)?,
            list: [self.list.read_u64(LIST)?, self.list.read_u64(LIST + 8)?],
            stamp: self.signals.read_u32(SIGNALS + 0x40)?, signalled,
        })
    }

    pub(super) fn complete(&mut self, signalled: u128, generation: u32) -> Result<bool> {
        if !compute_generation_admitted(generation) { return Err(EINVAL); }
        Ok(self.counters()? == [compute_pointer_head(generation); 3]
            && self.completion(signalled)?
                .complete_at(LIST, QUEUE_ID, generation * 0x100))
    }
}

fn put32(out: &mut [u8], offset: usize, value: u32) {
    out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(out: &mut [u8], offset: usize, value: u64) {
    out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
