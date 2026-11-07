// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! VM-owned parameter buffer for the qualified 256x256 M5 render profile.
//!
//! The allocation addresses are dynamic. Capacity and scene layout remain
//! the witnessed native profile; larger renders and pool growth are separate.

use super::{graphics_memory::M5GraphicsMemory, MappedObject};
use crate::{driver::AsahiDrmDevice, mmu, m5::vm_layout};
use kernel::prelude::*;

pub(super) const BLOCK_BYTES: usize = 0x80000;
pub(super) const PAGE_LIST_BYTES: usize = 0x1c000;
pub(super) const PAGE_COUNT: u32 = 48;

pub(super) struct M5ParameterBuffer {
    blocks: [MappedObject; 3],
    page_list: MappedObject,
    metrics: MappedObject,
    scene_scratch: MappedObject,
    discard: MappedObject,
}

impl M5ParameterBuffer {
    pub(super) fn new_reference(dev: &AsahiDrmDevice, memory: &M5GraphicsMemory) -> Result<Self> {
        let allocate = |size| memory.allocate_compact(
            dev, size, 0x8000, mmu::PROT_GPU_SHARED_RW,
        );
        let mut owner = Self {
            blocks: [allocate(BLOCK_BYTES)?, allocate(BLOCK_BYTES)?, allocate(BLOCK_BYTES)?],
            page_list: allocate(PAGE_LIST_BYTES)?,
            metrics: allocate(0x4000)?,
            scene_scratch: allocate(0x4000)?,
            discard: allocate(0x8000)?,
        };
        let block_bases = owner.block_page_bases()?;
        owner.page_list.with_bytes_mut(|out| {
            for (block, first) in block_bases.into_iter().enumerate() {
                for page in 0..16 {
                    let offset = (block * 16 + page) * 4;
                    out[offset..offset + 4].copy_from_slice(&(first + page as u32).to_le_bytes());
                }
            }
            Ok(())
        })?;
        owner.validate(memory.vm())?;
        dev_dbg!(dev.as_ref(),
            "M5_PARAMETER_BUFFER blocks={:x?} page_list={:#x} metrics={:#x} scene={:#x} discard={:#x}; compact mappings owned\n",
            owner.blocks.each_ref().map(|b| b.iova()), owner.page_list(), owner.metrics(),
            owner.scene_scratch.iova(), owner.discard.iova());
        Ok(owner)
    }

    pub(super) fn block_page_bases(&self) -> Result<[u32; 3]> {
        let mut bases = [0; 3];
        for (base, block) in bases.iter_mut().zip(self.blocks.iter()) {
            let offset = block.iova().checked_sub(vm_layout::DRIVER_BASE).ok_or(ERANGE)?;
            if offset & 0x7fff != 0 { return Err(EINVAL); }
            *base = u32::try_from(offset >> 15).map_err(|_| ERANGE)?;
        }
        Ok(bases)
    }

    pub(super) fn page_list(&self) -> u64 { self.page_list.iova() }
    pub(super) fn metrics(&self) -> u64 { self.metrics.iova() }
    pub(super) fn scene_cycle(&self) -> u64 {
        self.scene_scratch.iova() - vm_layout::DRIVER_BASE + 0x40
    }
    pub(super) fn discard_offset(&self) -> u64 {
        self.discard.iova() - vm_layout::DRIVER_BASE
    }

    /// Firmware and application views must reference the same backing.
    pub(super) fn firmware_aliases(
        &mut self, uat: &mmu::Uat, vm: &mmu::Vm, page_list_high: u64, metrics_high: u64,
    ) -> Result<KVec<mmu::KernelMapping>> {
        let mut aliases = KVec::new();
        for (object, high) in [(&mut self.page_list, page_list_high), (&mut self.metrics, metrics_high)] {
            let alias = object.map_alias_at(uat.kernel_vm(), high, mmu::PROT_FW_SHARED_RW)?;
            for offset in (0..object.logical_size).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(high + offset as u64)?
                    != vm.translate_iova(object.iova() + offset as u64)? {
                    return Err(EFAULT);
                }
            }
            aliases.push(alias, GFP_KERNEL)?;
        }
        Ok(aliases)
    }

    /// Map the retained pool into another application VM at the same compact
    /// addresses with the original two-page guard reservations, so the
    /// firmware's parameter-manager records keep naming valid pages after a
    /// render client handoff.
    pub(super) fn alias_into(&mut self, vm: &mmu::Vm) -> Result<KVec<mmu::KernelMapping>> {
        let range = vm_layout::compact_range(&vm.kernel_range()).ok_or(EINVAL)?;
        let mut aliases = KVec::new();
        let alias = |object: &mut MappedObject, aliases: &mut KVec<mmu::KernelMapping>| -> Result {
            let end = object.iova()
                .checked_add((object.logical_size + 2 * mmu::UAT_PGSZ) as u64)
                .ok_or(EOVERFLOW)?;
            if object.iova() < range.start || end > range.end { return Err(ERANGE); }
            if let Some(mapping) = object.map_alias_at_with_guards(
                vm, object.iova(), mmu::PROT_GPU_SHARED_RW, 2)? {
                aliases.push(mapping, GFP_KERNEL)?;
            }
            Ok(())
        };
        for block in self.blocks.iter_mut() { alias(block, &mut aliases)?; }
        alias(&mut self.page_list, &mut aliases)?;
        alias(&mut self.metrics, &mut aliases)?;
        alias(&mut self.scene_scratch, &mut aliases)?;
        alias(&mut self.discard, &mut aliases)?;
        self.validate(vm)?;
        Ok(aliases)
    }

    fn validate(&self, vm: &mmu::Vm) -> Result {
        let check = |object: &MappedObject| -> Result {
            let start = object.iova();
            let end = start.checked_add(object.logical_size as u64).ok_or(EOVERFLOW)?;
            if start < vm_layout::DRIVER_BASE || end > vm_layout::COMPACT_END
                || start & 0x7fff != 0 { return Err(ERANGE); }
            for offset in (0..object.logical_size).step_by(mmu::UAT_PGSZ) {
                vm.translate_iova(start + offset as u64)?;
            }
            for page in 0..2 {
                if vm.translate_iova(end + page * mmu::UAT_PGSZ as u64).is_ok() {
                    return Err(EFAULT);
                }
            }
            Ok(())
        };
        for block in &self.blocks { check(block)?; }
        for object in [&self.page_list, &self.metrics, &self.scene_scratch, &self.discard] {
            check(object)?;
        }
        Ok(())
    }
}
