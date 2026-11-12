// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Opt-in allocation E2E on the real UAT, before firmware publication.
//! This never submits GPU work or claims shader/CTS qualification.

use super::*;
use crate::g18::uapi::compute_plan::{G18ComputeDirectory, G18ComputePlan, G18_COMPUTE_BLOCK_BYTES};

struct Snapshot {
    translations: KVec<(u64, u64, u64)>,
    list: KVec<u8>,
    state: [u8; STATE_BYTES],
    plan: Option<G18ComputePlan>,
}

impl G18UmaPoolMemory {
    fn owner_snapshot(&mut self, uat: &mmu::Uat) -> Result<Snapshot> {
        let plan = self.shared_compute_plan;
        let blocks = self.scratch_plan.ok_or(EINVAL)?.block_count;
        let directory = G18ComputeDirectory::for_blocks(blocks).ok_or(EINVAL)?;
        let mut translations = KVec::new();
        let mut capture = |high: u64, low: u64, size: usize| -> Result {
            for offset in (0..size).step_by(mmu::UAT_PGSZ) {
                let high = high + offset as u64;
                let low = low + offset as u64;
                let physical = uat.kernel_vm().translate_iova(high)?;
                if uat.kernel_lower_vm().translate_iova(low)? != physical { return Err(EFAULT); }
                translations.push((high, low, physical as u64), GFP_KERNEL)?;
            }
            Ok(())
        };
        let base = DIRECTORY_BASE + self.low_bias;
        for (index, page) in self.directory.iter().enumerate() {
            capture(page.iova(), base + self.layout.page_offset(index as u32).ok_or(EINVAL)?,
                mmu::UAT_PGSZ)?;
        }
        for (extent, offset) in self.directory_growth.iter().zip(self.directory_growth_offsets.iter()) {
            capture(extent.iova(), base + offset, extent.logical_size)?;
        }
        capture(self.block_list.iova(), BLOCK_LIST_BASE + self.low_bias, self.block_list.logical_size)?;
        for (index, block) in self.scratch.iter().enumerate() {
            capture(block.iova(), BLOCK_DATA_BASE + self.low_bias + index as u64 * self.block_stride,
                block.logical_size)?;
        }
        let tail_bytes = u64::from(directory.tail_pages_per_usc) * self.layout.page_bytes;
        for offset in (0..directory.reservation_bytes()).step_by(mmu::UAT_PGSZ) {
            let in_stride = offset % self.geometry.directory_bytes_per_usc;
            let expected = in_stride == 0 || (offset < 12 * self.geometry.directory_bytes_per_usc
                && in_stride >= self.geometry.directory_bytes_per_usc - tail_bytes);
            if uat.kernel_lower_vm().translate_iova(base + offset).is_ok() != expected {
                return Err(EFAULT);
            }
        }
        // The sparse holes must still belong to the allocator, not merely
        // have absent PTEs. Probe both ends of the aperture when holes exist.
        if directory.tail_pages_per_usc < 85 {
            for offset in [self.layout.page_bytes,
                11 * self.geometry.directory_bytes_per_usc + self.layout.page_bytes] {
                match self.directory[0].map_alias_at(uat.kernel_lower_vm(), base + offset,
                    mmu::PROT_GPU_SHARED_RW) {
                    Err(error) if error == EBUSY || error == ENOSPC => {},
                    _ => return Err(EFAULT),
                }
            }
        }
        let list = self.block_list.with_bytes_mut(|bytes| {
            let mut copy = KVec::with_capacity(bytes.len(), GFP_KERNEL)?;
            for &byte in bytes.iter() { copy.push(byte, GFP_KERNEL)?; }
            Ok(copy)
        })?;
        Ok(Snapshot { translations, list, state: self.render_state()?, plan })
    }

    fn unchanged(&mut self, uat: &mmu::Uat, before: &Snapshot) -> Result {
        let after = self.owner_snapshot(uat)?;
        if before.translations[..] != after.translations[..] || before.list[..] != after.list[..]
            || before.state != after.state || before.plan != after.plan { return Err(EFAULT); }
        Ok(())
    }

    /// E2E injection at the last allocation boundary: a complete replacement
    /// is discarded before descriptor publication. Compare every old mapping,
    /// state byte and list entry, then let the real preparation reuse the bank.
    pub(crate) fn verify_replacement_rollback(&mut self, dev: &AsahiDrmDevice,
        uat: &mmu::Uat, plan: G18ComputePlan) -> Result {
        use crate::g18::address_map as va;
        let before = self.owner_snapshot(uat)?;
        let mut staged = self.replacement_shared_compute(dev, uat,
            va::DYNAMIC_HIGH_START..va::DYNAMIC_HIGH_END, plan)?;
        let _binding = staged.prepare_binding()?;
        let staged_before = staged.owner_snapshot(uat)?;
        let bias = staged.low_bias;
        let retained = self.scratch.len().min(plan.ideal.block_count as usize);
        drop(staged);
        self.unchanged(uat, &before)?;
        for &(high, low, _) in staged_before.translations.iter() {
            if uat.kernel_vm().translate_iova(high).is_ok()
                || uat.kernel_lower_vm().translate_iova(low).is_ok() { return Err(EFAULT); }
        }
        dev_info!(dev.as_ref(),
            "G18_COMPUTE_POOL_ROLLBACK old_pages={} staged_pages={} retained_blocks={} released_bias={:#x} state=list=translations=unchanged unpublished=1\n",
            before.translations.len(), staged_before.translations.len(), retained, bias);
        Ok(())
    }

    #[inline(never)]
    pub(crate) fn verify_shared_compute_owner(dev: &AsahiDrmDevice, uat: &mmu::Uat) -> Result {
        use crate::g18::address_map as va;
        let high = va::DYNAMIC_HIGH_START..va::DYNAMIC_HIGH_END;
        let mut words = [48,12,0,0,2304,24,0,0,2,1,0,0,0,0,0,0,1,0,0,128,4,0,0];
        let inverse = G18ComputePlan::from_descriptor(&words).ok_or(EINVAL)?;
        words[4] = 48;
        let small = G18ComputePlan::from_descriptor(&words).ok_or(EINVAL)?;
        if inverse.ideal.bytes != 30 * 1024 * 1024 || inverse.ideal.block_count != 15
            || small.ideal.block_count != 2 { return Err(EINVAL); }
        dev_info!(dev.as_ref(), "G18_COMPUTE_OWNER_E2E begin unpublished=1 gpu_kicks=0\n");
        let mut pool = Self::new_shared_compute(dev, uat, high.clone(), small)?;
        let before = pool.owner_snapshot(uat)?;
        // Refuse an actual collision after five additional blocks are staged.
        let blocked_low = BLOCK_DATA_BASE + pool.low_bias + 7 * pool.block_stride;
        let blocker = pool.directory[0].map_alias_at(uat.kernel_lower_vm(), blocked_low,
            mmu::PROT_GPU_SHARED_RW)?;
        match pool.prepare_shared_compute(dev, uat, high.clone(), inverse) {
            Err(error) if error == EBUSY || error == ENOSPC => {},
            _ => return Err(EFAULT),
        }
        pool.unchanged(uat, &before)?;
        drop(blocker);
        for index in 2..15 {
            if uat.kernel_lower_vm().translate_iova(BLOCK_DATA_BASE + pool.low_bias
                + index * pool.block_stride).is_ok() { return Err(EFAULT); }
        }
        dev_info!(dev.as_ref(), "G18_COMPUTE_OWNER_E2E scratch_collision=pass previous_pages={}\n",
            before.translations.len());

        // In a fresh 64-MiB subarena, leave exactly thirteen guarded blocks
        // and two guarded directory extents free. Third extent allocation
        // must fail after two real guard splits, causing their rollback.
        let constrained = high.end - 64 * 1024 * 1024..high.end;
        let page = mmu::UAT_PGSZ as u64;
        let free = 13 * (G18_COMPUTE_BLOCK_BYTES + page) + 2 * (21 * page + page);
        let cut = constrained.start + free;
        let guard_pages = ((constrained.end - cut) / page - 1) as usize;
        let blocker = MappedObject::new_wc_in_range_with_guards(dev, uat.kernel_vm(),
            cut..constrained.end, mmu::UAT_PGSZ, page, mmu::PROT_FW_SHARED_RW, guard_pages)?;
        if blocker.iova() != cut { return Err(EFAULT); }
        match pool.prepare_shared_compute(dev, uat, constrained.clone(), inverse) {
            Err(error) if error == ENOSPC => {},
            _ => return Err(EFAULT),
        }
        pool.unchanged(uat, &before)?;
        for at in (constrained.start..cut).step_by(mmu::UAT_PGSZ) {
            if uat.kernel_vm().translate_iova(at).is_ok() { return Err(EFAULT); }
        }
        drop(blocker);
        dev_info!(dev.as_ref(), "G18_COMPUTE_OWNER_E2E directory_allocation_rollback=pass\n");

        pool.prepare_shared_compute(dev, uat, high.clone(), inverse)?;
        let grown = pool.owner_snapshot(uat)?;
        for &(high, low, physical) in before.translations.iter() {
            if uat.kernel_vm().translate_iova(high)? as u64 != physical
                || uat.kernel_lower_vm().translate_iova(low)? as u64 != physical { return Err(EFAULT); }
        }
        let vm = uat.new_vm(0x267, 0x70_4000_0000..0x70_6000_0000)?.with_t8152_lifetime()?;
        pool.alias_into_process(dev, uat, &vm)?;
        for &(_, low, physical) in grown.translations.iter() {
            if vm.translate_iova(low)? as u64 != physical { return Err(EFAULT); }
        }
        for usc in [0,11] {
            let hole = DIRECTORY_BASE + pool.low_bias + usc * pool.geometry.directory_bytes_per_usc + page;
            if vm.translate_iova(hole).is_ok() { return Err(EFAULT); }
            match pool.directory[0].map_alias_at(&vm, hole, mmu::PROT_GPU_SHARED_RW) {
                Err(error) if error == EBUSY || error == ENOSPC => {},
                _ => return Err(EFAULT),
            }
        }
        // Publication state has the exact pure serializer output, and once
        // encoded the allocation path must refuse further growth/reset.
        let expected = inverse.pool_state(1, DIRECTORY_BASE + pool.low_bias,
            BLOCK_LIST_BASE + pool.low_bias, pool.preempt_counter.iova(), 0).ok_or(EINVAL)?;
        pool.prepare_binding()?;
        if pool.render_state()? != expected { return Err(EFAULT); }
        if pool.prepare_shared_compute(dev, uat, high.clone(), inverse) != Err(EBUSY) {
            return Err(EFAULT);
        }
        dev_info!(dev.as_ref(),
            "G18_COMPUTE_OWNER_E2E inverse_pool=pass bytes={} directory_pages=265 translated_pages={} process_aliases=pass state=pass\n",
            inverse.ideal.bytes, grown.translations.len());
        drop(pool);
        for &(_, low, _) in grown.translations.iter() {
            if uat.kernel_lower_vm().translate_iova(low).is_ok() || vm.translate_iova(low).is_ok() {
                return Err(EFAULT);
            }
        }
        drop(vm);
        // Reusing the exact low addresses catches leaked allocator reservations.
        drop(Self::new_shared_compute(dev, uat, high.clone(), inverse)?);
        Self::verify_guard_tiers(dev, uat, high)?;
        dev_info!(dev.as_ref(), "G18_COMPUTE_OWNER_E2E pass gpu_kicks=0 hardware_descriptor_loaded=0\n");
        Ok(())
    }

    #[inline(never)]
    fn verify_guard_tiers(dev: &AsahiDrmDevice, uat: &mmu::Uat, high: Range<u64>) -> Result {
        let low = DIRECTORY_BASE + 0x40000000;
        let mut anchor_object = MappedObject::new_wc_in_range(dev, uat.kernel_vm(), high.clone(),
            mmu::UAT_PGSZ, mmu::UAT_PGSZ as u64, mmu::PROT_FW_SHARED_RW)?;
        let mut anchor = anchor_object.map_alias_at_with_guards(uat.kernel_lower_vm(), low,
            mmu::PROT_GPU_SHARED_RW, 85)?;
        let original = uat.kernel_lower_vm().translate_iova(low)?;
        let mut tails = KVec::with_capacity(3, GFP_KERNEL)?;
        for pages in [21,22,42] {
            let extent = MappedObject::new_wc_in_range(dev, uat.kernel_vm(), high.clone(),
                pages * mmu::UAT_PGSZ, mmu::UAT_PGSZ as u64, mmu::PROT_FW_SHARED_RW)?;
            let alias = extent.mapping.as_ref().ok_or(EINVAL)?.map_alias_into_guard(&mut anchor,
                mmu::PROT_GPU_SHARED_RW)?;
            for offset in (0..pages * mmu::UAT_PGSZ).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(extent.iova() + offset as u64)?
                    != uat.kernel_lower_vm().translate_iova(alias.iova() + offset as u64)? {
                    return Err(EFAULT);
                }
            }
            tails.push((alias, extent), GFP_KERNEL)?;
        }
        match anchor_object.mapping.as_ref().ok_or(EINVAL)?.map_alias_into_guard(&mut anchor,
            mmu::PROT_GPU_SHARED_RW) {
            Err(error) if error == ENOSPC => {},
            _ => return Err(EFAULT),
        }
        while let Some((tail, extent)) = tails.pop() {
            let address = tail.iova();
            anchor.rollback_guard_tail(tail)?;
            for offset in (0..extent.logical_size).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_lower_vm().translate_iova(address + offset as u64).is_ok() {
                    return Err(EFAULT);
                }
            }
            if uat.kernel_lower_vm().translate_iova(low)? != original { return Err(EFAULT); }
            for (retained, backing) in tails.iter() {
                for offset in (0..backing.logical_size).step_by(mmu::UAT_PGSZ) {
                    if uat.kernel_lower_vm().translate_iova(retained.iova() + offset as u64)?
                        != uat.kernel_vm().translate_iova(backing.iova() + offset as u64)? {
                        return Err(EFAULT);
                    }
                }
            }
            match anchor_object.map_alias_at(uat.kernel_lower_vm(), address, mmu::PROT_GPU_SHARED_RW) {
                Err(error) if error == EBUSY || error == ENOSPC => {},
                _ => return Err(EFAULT),
            }
        }
        dev_info!(dev.as_ref(), "G18_COMPUTE_OWNER_E2E directory_tiers=21,43,85 rollback=pass\n");
        Ok(())
    }
}
