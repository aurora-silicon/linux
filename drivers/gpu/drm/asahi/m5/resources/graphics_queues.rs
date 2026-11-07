// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Paired first-submission queues for the retained M5 graphics experiment.

use super::{graphics_job::{M5GraphicsJob, PB_HARDWARE_ID, QUEUE_IDS}, graphics_usc,
    firmware_view::FirmwareDataView, MappedObject};
use crate::{driver::AsahiDrmDevice, m5::submission, m5::channel, m5::sksm, mmu};
use crate::m5::completion::{graphics_pointer_head, GRAPHICS_POINTER_COUNT,
    MAX_GRAPHICS_GENERATION};
use kernel::prelude::*;

const HIGH: u64 = 0xffff_fc20_c000_0000;
const ENTRY_HIGH: u64 = 0xffff_fc20_0039_0000;
const ENTRY_LOW: u64 = 0x70_005f_0000;
const ENTRY_BYTES: usize = 0x20000;
const CONTROL: usize = 0x18000;
// Native TA and fragment channel+0x10 both point to fffffc2000000048.
// One scheduler belongs to one intrusive list even when two SKSM queues run it.
pub(super) const JOB_LIST: u64 = HIGH + 0x20000;
const POINTERS: [usize; 2] = [0x4000, 0xc000];
const ITEMS: [usize; 2] = [0x8000, 0x10000];
const CONFIG: [usize; 2] = [0xa800, 0x12800];

pub(super) struct M5GraphicsQueues {
    aliases: KVec<mmu::KernelMapping>,
    entries: KVec<MappedObject>,
    graph: MappedObject,
    job_list: MappedObject,
    firmware: FirmwareDataView,
}

impl M5GraphicsQueues {
    pub(super) fn new(dev: &AsahiDrmDevice, uat: &mmu::Uat, job: &M5GraphicsJob) -> Result<Self> {
        let mut graph = MappedObject::new_wc_at(dev, uat.kernel_vm(), HIGH,
            0x20000, mmu::PROT_FW_PRIV_RW)?;
        let mut job_list = MappedObject::new_wc_at(dev, uat.kernel_vm(), JOB_LIST,
            0x4000, mmu::PROT_FW_SHARED_RW)?;
        job_list.with_bytes_mut(|out| { put64(out, 8, JOB_LIST); Ok(()) })?;
        let mut entries = KVec::new();
        let mut aliases = KVec::new();
        for stage in 0..2 {
            let id = QUEUE_IDS[stage];
            let high = ENTRY_HIGH + (stage * ENTRY_BYTES) as u64;
            let low = ENTRY_LOW + (stage * ENTRY_BYTES) as u64;
            let mut entry = MappedObject::new_wc_at(dev, uat.kernel_vm(), high,
                ENTRY_BYTES, mmu::PROT_FW_PRIV_RW)?;
            let alias = entry.map_alias_at(uat.kernel_lower_vm(), low, mmu::PROT_GPU_SHARED_RW)?;
            for offset in (0..ENTRY_BYTES).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(high + offset as u64)?
                    != uat.kernel_lower_vm().translate_iova(low + offset as u64)? {
                    return Err(EFAULT);
                }
            }
            let [descriptor, _] = job.descriptor_addresses(stage)?;
            let dm = if stage == 0 { sksm::M5DataMaster::Tiling }
                     else { sksm::M5DataMaster::Fragment };
            let prepared = Self::prepare_entry(job, stage, 1)?;
            entry.with_bytes_mut(|out| {
                out[prepared.offset as usize..prepared.offset as usize + prepared.bytes.len()]
                    .copy_from_slice(&prepared.bytes);
                Ok(())
            })?;
            let install = channel::encode_m5_channel_config_for(channel::M5ChannelConfig {
                ring_low: low, ring_high: high, queue_id: id, priority: 2,
                install_queue: true, gpu_context: job.context_id(), event: None,
                channel_control: HIGH + CONTROL as u64, update_scheduler: true,
                scheduler_index: 0, channel_stamp_index: stage as u32,
                update_context: false, owner_context: 0, extended_command: false,
            }, stage as u32, 2, None, None).map_err(|_| EINVAL)?;
            let config = channel::encode_m5_channel_config_for(channel::M5ChannelConfig {
                ring_low: low, ring_high: high, queue_id: id, priority: 2,
                install_queue: false, gpu_context: job.context_id(),
                event: Some(channel::M5ChannelEvent { address: job.usc_control(),
                    timestamp: 1, index: graphics_usc::HARDWARE_ID }),
                channel_control: HIGH + CONTROL as u64, update_scheduler: false,
                scheduler_index: 0, channel_stamp_index: stage as u32,
                update_context: true, owner_context: 1, extended_command: false,
            }, stage as u32, 2, Some(0), if stage == 0 {
                Some(channel::M5ParameterBufferEvent { address: job.parameter_state(),
                    timestamp: 1, hardware_buffer_id: PB_HARDWARE_ID, manager_id: 0 })
            } else { None }).map_err(|_| EINVAL)?;
            let kick = sksm::encode_m5_sksm_add_kicks_for(dm, id, 2, 1, 1)
                .map_err(|_| EINVAL)?;
            graph.with_bytes_mut(|out| {
                let pointers = POINTERS[stage];
                let queue = id as usize * 0xc0;
                let reset = submission::encode_m5_25g76_channel_state_reset(
                    submission::M5ChannelStateResetValues {
                        uncached_state: HIGH + pointers as u64,
                        command_pointer_ring: HIGH + ITEMS[stage] as u64,
                        gpu_va_10: JOB_LIST, word_48: 0,
                        gpu_va_9c: HIGH + CONTROL as u64,
                    });
                out[queue..queue + 0xc0].copy_from_slice(&reset);
                for offset in [0x28, 0x2c, 0x40] { put32(out, queue + offset, 2); }
                out[queue + 0x36..queue + 0x38].fill(0xff);
                put32(out, pointers + 0x50, u32::MAX);
                put32(out, pointers + 0x60, 0x500);
                out[CONFIG[stage]..CONFIG[stage] + install.len()].copy_from_slice(&install);
                out[CONFIG[stage] + 0x100..CONFIG[stage] + 0x100 + config.len()].copy_from_slice(&config);
                out[CONFIG[stage] + 0x200..CONFIG[stage] + 0x200 + kick.len()].copy_from_slice(&kick);
                for (index, address) in [HIGH + CONFIG[stage] as u64, descriptor,
                    HIGH + CONFIG[stage] as u64 + 0x100,
                    HIGH + CONFIG[stage] as u64 + 0x200].into_iter().enumerate() {
                    put64(out, ITEMS[stage] + index * 8, address);
                }
                // Publish only installation. The draw stays beyond the producer
                // until both firmware queue slots and scheduler bindings verify.
                put32(out, pointers + 0x40, 1);
                Ok(())
            })?;
            aliases.push(alias, GFP_KERNEL)?;
            entries.push(entry, GFP_KERNEL)?;
        }
        graph.with_bytes_mut(|out| {
            out[CONTROL..CONTROL + 2].fill(0xff);
            out[CONTROL + 5] = 1;
            out[CONTROL + 0x26] = 2;
            out[CONTROL + 0x33] = 0xff;
            Ok(())
        })?;
        Ok(Self { aliases, entries, graph, job_list, firmware: FirmwareDataView::new(dev)? })
    }

    pub(super) fn queue_address(stage: usize) -> Result<u64> {
        Ok(HIGH + *QUEUE_IDS.get(stage).ok_or(EINVAL)? as u64 * 0xc0)
    }

    fn prepare_entry(job: &M5GraphicsJob, stage: usize, generation: u32)
        -> Result<sksm::PreparedM5SksmEntry> {
        if !(1..=MAX_GRAPHICS_GENERATION).contains(&generation) { return Err(EINVAL); }
        let id = *QUEUE_IDS.get(stage).ok_or(EINVAL)?;
        let [descriptor, descriptor_low] = job.descriptor_addresses(stage)?;
        let stamp = generation as u64;
        let barrier = [sksm::M5SksmDependency { queue_id: QUEUE_IDS[0], stamp, kind: 1 }];
        let mut rce = [sksm::M5SksmRce { address: 0, tag: 0 }; 4];
        if stage == 0 {
            rce[0] = sksm::M5SksmRce { address: descriptor_low + 0x60, tag: 79 };
        } else {
            for (index, (offset, count)) in [(0xa0, 87), (0x7c0, 18),
                (0xee0, 25), (0x1600, 12)].into_iter().enumerate() {
                rce[index] = sksm::M5SksmRce { address: descriptor_low + offset, tag: count };
            }
        }
        sksm::prepare_m5_sksm_entry(sksm::M5SksmQueue {
            id, priority: 2, current_stamp: stamp, submitted: 0,
            previous_stamp: if generation == 1 { None } else { Some(stamp - 1) },
        }, sksm::M5SksmOperands {
            barriers: if stage == 0 { &[] } else { &barrier }, first_predecessor: 0,
            implicit_dependency_type: 1, mcache: None, flag_bit1: false,
            flag_bit61: false, flag_bit63: false,
            payload: [descriptor, Self::queue_address(stage)?],
            qos_byte_32: 3, qos_class_40: if stage == 0 { 12 } else { 24 },
            qos_word_48: 0xffff, event_mask: [0; 4], rce_kind: 0, rce,
            auxiliary: 0x3f_ffff_ffff_ffff,
        }).map_err(|_| EINVAL)
    }

    pub(super) fn publish_next_draw(&mut self, job: &M5GraphicsJob, generation: u32) -> Result {
        if !(2..=MAX_GRAPHICS_GENERATION).contains(&generation) { return Err(EINVAL); }
        let previous_head = graphics_pointer_head(generation - 1);
        if self.counters()? != [[previous_head; 3]; 2] || !self.installed()?
            || self.completion_list()? != [0, JOB_LIST] { return Err(EBUSY); }
        for stage in 0..2 {
            let prepared = Self::prepare_entry(job, stage, generation)?;
            self.entries[stage].with_bytes_mut(|out| {
                let offset = prepared.offset as usize;
                let slot = &mut out[offset..offset + prepared.bytes.len()];
                // Every preceding job has retired, including the prior user
                // of this 256-entry SKSM slot. Preserve the first-use check.
                if generation < 256 && slot.iter().any(|byte| *byte != 0) {
                    return Err(EBUSY);
                }
                slot.copy_from_slice(&prepared.bytes);
                Ok(())
            })?;
            let dm = if stage == 0 { sksm::M5DataMaster::Tiling }
                     else { sksm::M5DataMaster::Fragment };
            let kick = sksm::encode_m5_sksm_add_kicks_for(dm, QUEUE_IDS[stage],
                2, generation as u64, 1).map_err(|_| EINVAL)?;
            self.graph.with_bytes_mut(|out| {
                out[CONFIG[stage] + 0x200..CONFIG[stage] + 0x200 + kick.len()]
                    .copy_from_slice(&kick);
                let addresses = [job.descriptor_addresses(stage)?[0],
                    HIGH + CONFIG[stage] as u64 + 0x100,
                    HIGH + CONFIG[stage] as u64 + 0x200];
                for (index, address) in addresses.into_iter().enumerate() {
                    let absolute = 1 + (generation as usize - 1) * 3 + index;
                    let offset = ITEMS[stage] + (absolute % GRAPHICS_POINTER_COUNT as usize) * 8;
                    if absolute < GRAPHICS_POINTER_COUNT as usize
                        && out[offset..offset + 8].iter().any(|byte| *byte != 0) {
                        return Err(EBUSY);
                    }
                    put64(out, offset, address);
                }
                Ok(())
            })?;
        }
        for pointers in POINTERS {
            self.graph.write_u32(HIGH + pointers as u64 + 0x40,
                graphics_pointer_head(generation))?;
        }
        Ok(())
    }

    pub(super) fn counters(&mut self) -> Result<[[u32; 3]; 2]> {
        let mut result = [[0; 3]; 2];
        for stage in 0..2 {
            for (index, offset) in [0, 0x30, 0x40].into_iter().enumerate() {
                result[stage][index] = self.graph.read_u32(HIGH + (POINTERS[stage] + offset) as u64)?;
            }
        }
        Ok(result)
    }

    pub(super) fn completion_list(&mut self) -> Result<[u64; 2]> {
        Ok([self.job_list.read_u64(JOB_LIST)?, self.job_list.read_u64(JOB_LIST + 8)?])
    }

    pub(super) fn installation_status(&mut self) -> Result<([[u64; 5]; 2], [u64; 2], [u64; 2])> {
        let mut slots = [[0; 5]; 2];
        let mut pointers = [0; 2];
        for stage in 0..2 {
            (slots[stage], pointers[stage]) = self.firmware.queue_registration(QUEUE_IDS[stage])?;
        }
        Ok((slots, pointers, [u64::from(self.graph.read_u32(HIGH + CONTROL as u64)?),
            self.graph.read_u64(HIGH + CONTROL as u64 + 0x12)?]))
    }

    pub(super) fn installed(&mut self) -> Result<bool> {
        let (slots, pointers, control) = self.installation_status()?;
        for stage in 0..2 {
            let slot = slots[stage];
            let offset = (stage * ENTRY_BYTES) as u64;
            // GFX 0x12240/0x1241c copies tag15 +0x22 to slot +0x20:
            // this word is the TA/fragment stage, not the outer channel.
            if slot[0] & 1 == 0 || slot[1] != ENTRY_HIGH + offset
                || slot[2] != ENTRY_LOW + offset || slot[3] != 4u64 << 32
                || slot[4] as u32 != stage as u32 || pointers[stage] != HIGH + CONTROL as u64 {
                return Ok(false);
            }
        }
        Ok(control[0] & 0xff != 0xff && (control[0] >> 8) & 0xff != 0xff
            && (control[0] >> 16) & 0xff == 4
            && control[1] == ((1u64 << QUEUE_IDS[0]) | (1u64 << QUEUE_IDS[1])))
    }

    pub(super) fn publish_draw(&mut self) -> Result {
        if self.counters()? != [[1; 3]; 2] || !self.installed()? { return Err(EBUSY); }
        for pointers in POINTERS {
            self.graph.write_u32(HIGH + pointers as u64 + 0x40, 4)?;
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
