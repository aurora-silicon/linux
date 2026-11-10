// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Retained G18 channel working memory, one command, and completion storage.
//!
//! This owner never publishes a ring entry. All mappings remain retained by
//! G18FirmwareMemory until both firmware processors are stopped. Shader and
//! argument buffers, native device policy, and process context installation
//! remain separate obligations of the eventual submission path.

#![allow(dead_code)]

use crate::{g18::queue::scheduler::G18SchedulerBinding, g18::buffer::object::MappedObject};
use crate::{driver::AsahiDrmDevice, g18::fw::compute::*, g18::fw::compute_memory::*, mmu};
use core::ops::Range;
use core::sync::atomic::{fence, Ordering};
use kernel::prelude::*;

// Host-selected range, after the existing UMA directory/block list, before
// the fixed graph. The VM allocator checks overlap with every existing owner.
const LOW_RANGE: Range<u64> = 0x70_0800_0000..0x70_1000_0000;
const SUPPORT_BYTES: usize = 0x100;
const STAMP_OFFSETS: [u64; 2] = [0, 0x40];
const TIME_OFFSETS: [u64; 2] = [0x80, 0xc0];

#[derive(Debug, Copy, Clone)]
pub(crate) struct G18PreparedComputeCommand {
    pub(crate) firmware_address: u64,
    pub(crate) gpu_address: u64,
    pub(crate) expected_stamp: u32,
    pub(crate) context_id: u32,
    pub(crate) scheduler_record: u64,
    pub(crate) registers: G18ComputeRegisterSummary,
}

#[derive(Debug, Copy, Clone)]
pub(crate) struct G18ComputeCompletionSnapshot {
    pub(crate) stamps: [u32; 2],
    pub(crate) timestamps: [u64; 2],
}

pub(crate) struct G18ComputeMemory {
    // Retract the process roots before dropping their aliases and backing.
    process: Option<G18ComputeProcess>,
    // Alias owns the same GEM object and is released before the lower mapping.
    command_high: mmu::KernelMapping,
    command: MappedObject,
    channel: MappedObject,
    support: MappedObject,
    layout: G18ComputeChannelLayout,
    pointers: G18ComputeChannelPointers,
    prepared: bool,
    // Set before attempting publication, so even a partial failure cannot
    // authorize the unpublished preparation rollback path.
    context_published: bool,
}

struct G18ComputeProcess {
    context: mmu::G18ExecutionContext,
    // Same GEM pages and GPU VAs as the firmware-owned command/channel.
    aliases: [mmu::KernelMapping; 2],
}

impl G18ComputeMemory {
    /// The application owner calls this only after the complete queue,
    /// scheduler and stamp gate, while it still retains the submitted VM.
    pub(crate) fn retire_application(&mut self, stamp: u32) -> Result {
        let completed = self.completion_snapshot()?;
        if completed.stamps[0] != stamp || completed.timestamps[0] == 0
            || completed.timestamps[1] < completed.timestamps[0] { return Err(EBUSY); }
        self.release_application()
    }

    /// Release a dispatch that a firmware recovery ended after its queue
    /// consumed it: the GPU was reset, so no stamp or timestamp will come.
    pub(crate) fn retire_recovered(&mut self) -> Result {
        if !self.prepared { return Err(EINVAL); }
        self.release_application()
    }

    fn release_application(&mut self) -> Result {
        self.process = None;
        self.prepared = false;
        self.context_published = false;
        self.command.with_bytes_mut(|bytes| { bytes.fill(0); Ok(()) })?;
        self.support.with_bytes_mut(|bytes| { bytes.fill(0); Ok(()) })?;
        Ok(())
    }

    /// Drop a process lease acquired by a failed preparation. No prepared
    /// command or attempted root publication may cross this boundary.
    pub(crate) fn abort_unpublished_process(&mut self) -> Result {
        if self.prepared || self.context_published { return Err(EBUSY); }
        self.command.with_bytes_mut(|bytes| { bytes.fill(0); Ok(()) })?;
        self.process = None;
        Ok(())
    }

    pub(crate) fn unpublished_idle(&self) -> bool {
        !self.prepared && !self.context_published && self.process.is_none()
    }

    pub(crate) fn new(
        dev: &AsahiDrmDevice,
        uat: &mmu::Uat,
        high_range: Range<u64>,
        clusters: u32,
        width: u32,
    ) -> Result<Self> {
        if !uat.is_t8152_geometry() || mmu::UAT_PGSZ != 0x4000 {
            return Err(ENODEV);
        }
        let layout = G18ComputeChannelLayout::new(clusters, width).ok_or(EINVAL)?;
        let size: usize = layout.allocation_bytes.try_into()?;
        let channel = MappedObject::new_wc_in_range(
            dev,
            uat.kernel_lower_vm(),
            LOW_RANGE,
            size,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
        )?;
        let pointers = layout.bind(channel.iova()).ok_or(ERANGE)?;
        for offset in (0..size).step_by(mmu::UAT_PGSZ) {
            uat.kernel_lower_vm()
                .translate_iova(channel.iova() + offset as u64)?;
        }
        let mut command = MappedObject::new_wc_in_range(
            dev,
            uat.kernel_lower_vm(),
            LOW_RANGE,
            G18_COMPUTE_COMMAND_BYTES,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_GPU_FW_SHARED_RW,
        )?;
        let command_high = command.map_alias_in_range(
            uat.kernel_vm(),
            high_range.clone(),
            mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW,
        )?;
        for offset in (0..G18_COMPUTE_COMMAND_BYTES).step_by(mmu::UAT_PGSZ) {
            if uat
                .kernel_lower_vm()
                .translate_iova(command.iova() + offset as u64)?
                != uat
                    .kernel_vm()
                    .translate_iova(command_high.iova() + offset as u64)?
            {
                return Err(EFAULT);
            }
        }
        let support = MappedObject::new_wc_in_range(
            dev,
            uat.kernel_vm(),
            high_range,
            SUPPORT_BYTES,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW,
        )?;
        uat.kernel_vm().translate_iova(support.iova())?;
        fence(Ordering::SeqCst);
        dev_info!(dev.as_ref(),
            "G18 compute memory: channel={:#x}+{:#x} operands={:#x}/{:#x} control={:#x} tail={:#x} command={:#x}/{:#x} support={:#x}, retained unpublished\n",
            channel.iova(), layout.allocation_bytes, pointers.operands[0], pointers.operands[1],
            pointers.control, pointers.tail, command.iova(), command_high.iova(), support.iova());
        Ok(Self {
            process: None,
            command_high,
            command,
            channel,
            support,
            layout,
            pointers,
            prepared: false,
            context_published: false,
        })
    }

    /// Bind the payload VM while retaining both aliases and the context lease.
    /// A failure leaves the original command/channel and firmware roots intact.
    pub(crate) fn bind_process(
        &mut self, dev: &AsahiDrmDevice, uat: &mmu::Uat, vm: &mmu::Vm,
    ) -> Result<u32> {
        if !self.unpublished_idle() { return Err(EBUSY); }
        let context = uat.prepare_t8152_execution_context(vm)?;
        let command = self.command.map_alias_at(vm, self.command.iova(), mmu::PROT_GPU_SHARED_RW)?;
        let channel = self.channel.map_alias_at(vm, self.channel.iova(), mmu::PROT_GPU_SHARED_RW)?;
        for (address, length) in [(self.command.iova(), G18_COMPUTE_COMMAND_BYTES),
                                 (self.channel.iova(), self.layout.allocation_bytes as usize)] {
            for offset in (0..length).step_by(mmu::UAT_PGSZ) {
                let address = address + offset as u64;
                if vm.translate_iova(address)? != uat.kernel_lower_vm().translate_iova(address)? {
                    return Err(EFAULT);
                }
            }
        }
        let id = u32::from(context.context_id());
        crate::g18_kick_info!(dev.as_ref(),
            "G18 compute context: id={} roots={:x?}; command/channel aliases retained, unpublished\n",
            id, context.roots());
        self.process = Some(G18ComputeProcess { context, aliases: [command, channel] });
        Ok(id)
    }

    /// Encode one unpublished command. Caller must retain shader/argument
    /// bindings in the selected process VM and supply native device policy.
    pub(crate) fn prepare(
        &mut self,
        scheduler: G18SchedulerBinding,
        inputs: &G18ComputeCommandInputs,
        registers: &G18ComputeRegisterInputs,
    ) -> Result<G18PreparedComputeCommand> {
        if self.prepared {
            return Err(EBUSY);
        }
        let process = self.process.as_ref().ok_or(EINVAL)?;
        if registers.clusters != self.layout.clusters
            || registers.cluster_width != self.layout.width
            || registers.context_id != u32::from(process.context.context_id())
            || inputs.channel_ordinal >= 4
            || inputs.completion.stamp == 0
            || scheduler.record == 0
        {
            return Err(EINVAL);
        }
        let inputs = G18ComputeCommandInputs {
            d198: scheduler.record,
            stamp_addresses: STAMP_OFFSETS.map(|offset| self.support.iova() + offset),
            d1b0: self.support.iova() + TIME_OFFSETS[0],
            d1b8: self.support.iova() + TIME_OFFSETS[1],
            ..*inputs
        };
        let registers = G18ComputeRegisterInputs {
            command_gpu: self.command.iova(),
            operands: self.pointers.operands,
            support_control: self.pointers.control,
            // 26A428 loadCommandBufferData 8be5eac selects this record;
            // prepare 8be58b0 then takes its +8 word, independently of the
            // UAT context ID. Optional SMAPP resource objects are disabled.
            d630: scheduler.sequence,
            ..*registers
        };
        let summary = self.command.with_bytes_mut(|out| {
            let out = out.try_into().map_err(|_| EINVAL)?;
            encode_g18_compute_command(out, &inputs, &registers).map_err(|_| EINVAL)
        })?;
        fence(Ordering::SeqCst);
        self.prepared = true;
        Ok(G18PreparedComputeCommand {
            firmware_address: self.command_high.iova(),
            gpu_address: self.command.iova(),
            expected_stamp: inputs.completion.stamp,
            context_id: registers.context_id,
            scheduler_record: scheduler.record,
            registers: summary,
        })
    }

    /// Root publication is delayed until every command field is prepared.
    /// It does not publish a queue entry or advance a scheduler counter.
    pub(crate) fn publish_context(&mut self) -> Result<[u64; 2]> {
        if !self.prepared { return Err(EINVAL); }
        let process = self.process.as_mut().ok_or(EINVAL)?;
        self.context_published = true;
        process.context.publish()?;
        if !process.context.is_current() { return Err(EIO); }
        Ok(process.context.roots())
    }

    /// These independent WC reads are diagnostic, not an atomic completion
    /// fence. Shader execution still requires verified output data and a clean
    /// firmware/fault observation, even if both stamp words change.
    pub(crate) fn completion_snapshot(&mut self) -> Result<G18ComputeCompletionSnapshot> {
        if !self.prepared {
            return Err(EINVAL);
        }
        fence(Ordering::SeqCst);
        let base = self.support.iova();
        Ok(G18ComputeCompletionSnapshot {
            stamps: [
                self.support.read_u32(base + STAMP_OFFSETS[0])?,
                self.support.read_u32(base + STAMP_OFFSETS[1])?,
            ],
            timestamps: [
                self.support.read_u64(base + TIME_OFFSETS[0])?,
                self.support.read_u64(base + TIME_OFFSETS[1])?,
            ],
        })
    }
}
