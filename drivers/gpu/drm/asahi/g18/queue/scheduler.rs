// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Retained command scheduler records and their independent submitted counters.
//!
//! Allocation and preparation do not publish a command. The containing live
//! firmware owner must retain these mappings until BOTH processors are stopped,
//! including after errors. A bounded bring-up session never recycles a record.

#![allow(dead_code)]

use crate::g18::buffer::object::MappedObject;
use crate::{driver::AsahiDrmDevice, g18::fw::scheduler::*, mmu};
use core::ops::Range;
use core::sync::atomic::{fence, Ordering};
use kernel::prelude::*;

#[derive(Debug, Copy, Clone)]
pub(crate) struct G18SchedulerBinding {
    pub(crate) record: u64,
    pub(crate) submitted_counter: u64,
    pub(crate) sequence: u32,
}

#[derive(Debug, Copy, Clone)]
pub(crate) struct G18SchedulerSnapshot {
    pub(crate) submitted: u32,
    pub(crate) completed: u32,
    pub(crate) linked: u32,
    pub(crate) batches: [u32; 3],
}

pub(crate) struct G18SchedulerMemory {
    records: MappedObject,
    counters: MappedObject,
    armed: u64,
    sequences: [u32; G18_SCHEDULER_RECORD_COUNT],
    submitted: [u32; G18_SCHEDULER_RECORD_COUNT],
}

impl G18SchedulerMemory {
    pub(crate) fn print_render_digests(&mut self, dev: &AsahiDrmDevice) -> Result {
        for (name, object) in [("scheduler", &mut self.records), ("submitted-pre-kick", &mut self.counters)] {
            object.with_bytes_mut(|bytes| {
                dev_info!(dev.as_ref(), "G18_RENDER_DIGEST name={} fnv={:#018x}\n",name,crate::g18::fw::graphics_payload::fnv64(bytes));
                Ok(())
            })?;
        }
        Ok(())
    }
    /// Runtime's deterministic reservation, separate from the legacy compute
    /// scheduler. Counters and records are independent uncached FW mappings.
    pub(crate) fn new_render(dev: &AsahiDrmDevice, uat: &mmu::Uat) -> Result<Self> {
        use crate::g18::address_map::GRAPHICS_SCHEDULER_START as BASE;
        if !uat.is_t8152_geometry() { return Err(ENODEV); }
        let counters = MappedObject::new_wc_at(dev, uat.kernel_vm(), BASE,
            G18_SCHEDULER_COUNTER_BYTES, mmu::PROT_FW_SHARED_RW)?;
        let mut records = MappedObject::new_wc_at(dev, uat.kernel_vm(), BASE + 0x4000,
            G18_SCHEDULER_RECORD_BYTES, mmu::PROT_FW_SHARED_RW)?;
        records.with_bytes_mut(|out| initialize_g18_scheduler_records(
            out.try_into().map_err(|_| EINVAL)?, counters.iova()).ok_or(EINVAL))?;
        fence(Ordering::SeqCst);
        Ok(Self { records, counters, armed: 0,
            sequences: [0; G18_SCHEDULER_RECORD_COUNT], submitted: [0; G18_SCHEDULER_RECORD_COUNT] })
    }

    pub(crate) fn new(dev: &AsahiDrmDevice, uat: &mmu::Uat, high_range: Range<u64>) -> Result<Self> {
        if !uat.is_t8152_geometry() || mmu::UAT_PGSZ != 0x4000 {
            return Err(ENODEV);
        }
        let counters = MappedObject::new_wc_in_range(
            dev,
            uat.kernel_vm(),
            high_range.clone(),
            G18_SCHEDULER_COUNTER_BYTES,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW,
        )?;
        let mut records = MappedObject::new_wc_in_range(
            dev,
            uat.kernel_vm(),
            high_range,
            G18_SCHEDULER_RECORD_BYTES,
            mmu::UAT_PGSZ as u64,
            mmu::PROT_FW_SHARED_RW,
        )?;
        // Both native extents fit in one independently owned 16 KiB page.
        // MappedObject zeroes its allocation before installing the mapping.
        for object in [&counters, &records] {
            uat.kernel_vm().translate_iova(object.iova())?;
        }
        records.with_bytes_mut(|out| {
            let out = out.try_into().map_err(|_| EINVAL)?;
            initialize_g18_scheduler_records(out, counters.iova()).ok_or(EINVAL)
        })?;
        fence(Ordering::SeqCst);
        dev_info!(dev.as_ref(),
            "G18 command scheduler: records={:#x} counters={:#x} count=36 stride=0x100 extent=0xc8 retained, unpublished\n",
            records.iova(), counters.iova());
        Ok(Self {
            records,
            counters,
            armed: 0,
            sequences: [0; G18_SCHEDULER_RECORD_COUNT],
            submitted: [0; G18_SCHEDULER_RECORD_COUNT],
        })
    }

    /// Prepare one fresh record. A record can be armed only once during this
    /// bounded session; completion alone does not authorize host reset/reuse.
    pub(crate) fn prepare(&mut self, index: usize, sequence: u32) -> Result<G18SchedulerBinding> {
        if index >= G18_SCHEDULER_RECORD_COUNT {
            return Err(EINVAL);
        }
        if self.armed & (1u64 << index) != 0 {
            return Err(EBUSY);
        }
        let start = index * G18_SCHEDULER_RECORD_STRIDE;
        let submitted_counter = self.counters.iova() + index as u64 * 4;
        self.counters.write_u32(submitted_counter, 0)?;
        self.records.with_bytes_mut(|out| {
            let record = out[start..start + G18_SCHEDULER_RECORD_STRIDE]
                .as_mut()
                .try_into()
                .map_err(|_| EINVAL)?;
            prepare_g18_scheduler_record(
                record,
                submitted_counter,
                sequence,
                G18_SCHEDULER_DEFAULT_THRESHOLD,
                0,
                0,
            )
            .ok_or(EINVAL)
        })?;
        fence(Ordering::SeqCst);
        self.sequences[index] = sequence;
        self.armed |= 1u64 << index;
        self.binding(index)
    }

    pub(crate) fn binding(&self, index: usize) -> Result<G18SchedulerBinding> {
        if index >= G18_SCHEDULER_RECORD_COUNT || self.armed & (1u64 << index) == 0 {
            return Err(EINVAL);
        }
        Ok(G18SchedulerBinding {
            record: self.records.iova() + (index * G18_SCHEDULER_RECORD_STRIDE) as u64,
            submitted_counter: self.counters.iova() + index as u64 * 4,
            sequence: self.sequences[index],
        })
    }

    /// Final counter write before the caller publishes a fully prepared kick.
    /// The caller must arrange all fallible preparation before this operation.
    /// Once it succeeds, a failed publication requires retaining this session;
    /// rolling the target back could race firmware. No firmware-owned record
    /// fields are written here, even when submitting another job to the record.
    pub(crate) fn publish_kick(&mut self, index: usize) -> Result<u32> {
        let binding = self.binding(index)?;
        let previous = self.submitted[index];
        if self.counters.read_u32(binding.submitted_counter)? != previous {
            return Err(EIO);
        }
        let next = previous.checked_add(1).ok_or(EOVERFLOW)?;
        self.counters.write_u32(binding.submitted_counter, next)?;
        fence(Ordering::SeqCst);
        self.submitted[index] = next;
        Ok(next)
    }

    /// Diagnostic snapshot only: these separate reads are not an atomic
    /// completion fence and do not by themselves prove that a shader ran.
    pub(crate) fn snapshot(&mut self, index: usize) -> Result<G18SchedulerSnapshot> {
        let binding = self.binding(index)?;
        fence(Ordering::SeqCst);
        if self.records.read_u64(binding.record)? != binding.submitted_counter
            || self.records.read_u32(binding.record + 8)? != binding.sequence
        {
            return Err(EIO);
        }
        Ok(G18SchedulerSnapshot {
            submitted: self.counters.read_u32(binding.submitted_counter)?,
            completed: self.records.read_u32(binding.record + 0xc)?,
            linked: self.records.read_u32(binding.record + 0x94)?,
            batches: [
                self.records.read_u32(binding.record + 0x14)?,
                self.records.read_u32(binding.record + 0x18)?,
                self.records.read_u32(binding.record + 0x1c)?,
            ],
        })
    }
}
