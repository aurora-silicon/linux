// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Transactional allocation for a fresh, unpublished shared compute pool.
//! Previously submitted firmware state cannot be grown or reset here.

use super::*;
use crate::g18::uapi::compute_plan::{G18ComputeDirectory, G18ComputePlan, G18_COMPUTE_BLOCK_BYTES};

/// Reserve first, then append without any fallible operation after publication.
fn append_reserved<T>(out: &mut KVec<T>, values: KVec<T>) {
    let length = out.len() + values.len();
    assert!(out.capacity() >= length);
    for (slot, value) in out.spare_capacity_mut().iter_mut().zip(values) { slot.write(value); }
    // SAFETY: capacity was checked and every added slot was initialized once.
    unsafe { out.set_len(length); }
}

struct DirectoryTail {
    mapping: Option<mmu::KernelMapping>,
    backing: MappedObject,
    offset: u64,
    anchor: usize,
}

/// Until commit, every tail restores its original anchor reservation on Drop.
/// Mapping teardown then removes its PTEs before releasing its backing.
struct DirectoryTransaction<'a> {
    anchors: &'a mut KVec<mmu::KernelMapping>,
    tails: KVec<DirectoryTail>,
}

impl DirectoryTransaction<'_> {
    fn commit(mut self) -> KVec<DirectoryTail> {
        core::mem::replace(&mut self.tails, KVec::new())
    }
}

impl Drop for DirectoryTransaction<'_> {
    fn drop(&mut self) {
        for tail in self.tails.iter_mut().rev() {
            if let Some(mapping) = tail.mapping.take() {
                // Each tail came from this exact anchor under exclusive access.
                // Reverse order preserves adjacency; rollback allocates nothing.
                assert!(self.anchors[tail.anchor].rollback_guard_tail(mapping).is_ok());
            }
        }
    }
}

impl G18UmaPoolMemory {
    pub(crate) fn new_shared_compute(dev: &AsahiDrmDevice, uat: &mmu::Uat,
        high: Range<u64>, plan: G18ComputePlan) -> Result<Self>
    {
        let mut pool = Self::new_layout(dev, uat, high.clone(), 12,
            true, false, 0x40000000, 1, G18_COMPUTE_BLOCK_BYTES)?;
        pool.prepare_shared_compute(dev, uat, high, plan)?;
        Ok(pool)
    }

    /// A fresh FList view, retaining the previous scratch GEM objects while
    /// giving the directory, list, state and counter independent ownership.
    /// The caller must keep the old view until a verified firmware rebind.
    pub(crate) fn replacement_shared_compute(&self, dev: &AsahiDrmDevice, uat: &mmu::Uat,
        high: Range<u64>, plan: G18ComputePlan) -> Result<Self>
    {
        if self.render || !self.encoded || !self.descriptor_loaded
            || self.hardware_slot != 1 || !self.process_aliases.is_empty()
        { return Err(EBUSY); }
        let bias = if self.low_bias == 0x2_4000_0000 { 0x3_4000_0000 }
            else { 0x2_4000_0000 };
        let mut pool = Self::new_layout(dev, uat, high.clone(), 12,
            true, false, bias, 1, G18_COMPUTE_BLOCK_BYTES)?;
        pool.prepare_shared_compute_retaining(dev, uat, high, plan, Some(self))?;
        Ok(pool)
    }

    /// Increase an unpublished pool without moving any existing mapping.
    /// Every backing object, alias and list entry is staged. A failure restores
    /// all directory guards and leaves the previous plan and block list intact.
    pub(crate) fn prepare_shared_compute(&mut self, dev: &AsahiDrmDevice, uat: &mmu::Uat,
        high: Range<u64>, plan: G18ComputePlan) -> Result
    {
        self.prepare_shared_compute_retaining(dev, uat, high, plan, None)
    }

    fn prepare_shared_compute_retaining(&mut self, dev: &AsahiDrmDevice, uat: &mmu::Uat,
        high: Range<u64>, plan: G18ComputePlan, retained: Option<&Self>) -> Result
    {
        if self.encoded || self.descriptor_loaded || !self.process_aliases.is_empty() {
            return Err(EBUSY);
        }
        let previous = self.shared_compute_plan.map_or(0, |p| p.ideal.block_count);
        let new = plan.ideal;
        let directory = G18ComputeDirectory::for_blocks(new.block_count).ok_or(EINVAL)?;
        let old_tail = if previous == 0 { 0 } else {
            G18ComputeDirectory::for_blocks(previous).ok_or(EINVAL)?.tail_pages_per_usc
        };
        if self.render || ![0x40000000, 0x2_4000_0000, 0x3_4000_0000].contains(&self.low_bias)
            || self.hardware_slot != 1
            || self.geometry.block_bytes != G18_COMPUTE_BLOCK_BYTES
            || self.geometry.directory_bytes_per_usc != 0x158000
            || self.layout.reservation_bytes != directory.reservation_bytes()
            || self.scratch.len() != previous as usize
            || (previous == 0 && self.scratch_plan.is_some())
            || new.block_count < previous
            || new.bytes != u64::from(new.block_count) * G18_COMPUTE_BLOCK_BYTES
            || directory.tail_pages_per_usc < old_tail
            || high.start >= high.end
        { return Err(EINVAL); }
        let added = (new.block_count - previous) as usize;
        let extra_pages = directory.tail_pages_per_usc - old_tail;
        let extent_count = if extra_pages == 0 { 0 } else { 12 };
        let extent_bytes = extra_pages as usize * mmu::UAT_PGSZ;
        // Reject impossible address-space demands before any GEM allocations.
        // Actual free ranges and collisions are still checked by the allocator.
        let needed = (added as u64).checked_mul(G18_COMPUTE_BLOCK_BYTES).ok_or(EOVERFLOW)?
            .checked_add((extent_count * extent_bytes) as u64).ok_or(EOVERFLOW)?
            .checked_add(((added + extent_count) * mmu::UAT_PGSZ) as u64).ok_or(EOVERFLOW)?;
        if needed > high.end - high.start { return Err(ENOSPC); }
        let low_end = (BLOCK_DATA_BASE + self.low_bias)
            .checked_add(u64::from(new.block_count) * self.block_stride).ok_or(EOVERFLOW)?;
        if low_end > uat.kernel_lower_vm().kernel_range().end { return Err(ENOSPC); }
        self.scratch.reserve(added, GFP_KERNEL)?;
        self.scratch_aliases.reserve(added, GFP_KERNEL)?;
        self.directory_growth.reserve(extent_count, GFP_KERNEL)?;
        self.directory_growth_offsets.reserve(extent_count, GFP_KERNEL)?;
        self.aliases.reserve(extent_count, GFP_KERNEL)?;
        let mut backing = KVec::with_capacity(added, GFP_KERNEL)?;
        let mut aliases = KVec::with_capacity(added, GFP_KERNEL)?;
        let mut entries = KVec::with_capacity(added, GFP_KERNEL)?;
        for index in previous..new.block_count {
            let low = BLOCK_DATA_BASE + self.low_bias + u64::from(index) * self.block_stride;
            let mut block = if let Some(old) = retained.and_then(|p| p.scratch.get(index as usize)) {
                if !old.cpu_wc || old.object_offset != 0
                    || old.logical_size != G18_COMPUTE_BLOCK_BYTES as usize { return Err(EINVAL); }
                let mut object = crate::gem::ObjectRef::new(old.object.gem.clone());
                let mapping = object.map_into_range(uat.kernel_vm(), high.clone(),
                    mmu::UAT_PGSZ as u64, mmu::PROT_FW_SHARED_RW, true)?;
                let iova = mapping.iova();
                MappedObject { mapping: Some(mapping), object, iova, object_offset: 0,
                    logical_size: old.logical_size, cpu_wc: true }
            } else {
                MappedObject::new_wc_in_range(dev, uat.kernel_vm(), high.clone(),
                    G18_COMPUTE_BLOCK_BYTES as usize, mmu::UAT_PGSZ as u64, mmu::PROT_FW_SHARED_RW)?
            };
            let alias = block.map_alias_at_with_guards(uat.kernel_lower_vm(), low,
                mmu::PROT_GPU_SHARED_RW, 1)?;
            for offset in (0..G18_COMPUTE_BLOCK_BYTES).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(block.iova() + offset)?
                    != uat.kernel_lower_vm().translate_iova(low + offset)? { return Err(EFAULT); }
                if let Some(old) = retained.and_then(|p| p.scratch.get(index as usize)) {
                    if uat.kernel_vm().translate_iova(old.iova() + offset)?
                        != uat.kernel_vm().translate_iova(block.iova() + offset)? { return Err(EFAULT); }
                }
            }
            if uat.kernel_lower_vm().translate_iova(low + G18_COMPUTE_BLOCK_BYTES).is_ok() {
                return Err(EFAULT);
            }
            entries.push(g18_uma::g18_uma_block_entry(low, 0).ok_or(EINVAL)?, GFP_KERNEL)?;
            aliases.push(alias, GFP_KERNEL)?;
            backing.push(block, GFP_KERNEL)?;
        }
        let mut transaction = DirectoryTransaction {
            anchors: &mut self.aliases,
            tails: KVec::with_capacity(extent_count, GFP_KERNEL)?,
        };
        for usc in 0..extent_count {
            let extent = match MappedObject::new_wc_in_range(dev, uat.kernel_vm(), high.clone(),
                extent_bytes, mmu::UAT_PGSZ as u64, mmu::PROT_FW_SHARED_RW) {
                Ok(extent) => extent,
                Err(error) => {
                    dev_info!(dev.as_ref(), "G18_SHARED_COMPUTE_ROLLBACK directory_tails={} error={:?}\n",
                        transaction.tails.len(), error);
                    return Err(error);
                }
            };
            let offset = (usc as u64 + 1) * self.geometry.directory_bytes_per_usc
                - u64::from(directory.tail_pages_per_usc) * self.layout.page_bytes;
            let mapping = extent.mapping.as_ref().ok_or(EINVAL)?.map_alias_into_guard(
                &mut transaction.anchors[usc], mmu::PROT_GPU_SHARED_RW)?;
            let low = DIRECTORY_BASE + self.low_bias + offset;
            // Retain rollback ownership before any fallible translation check.
            transaction.tails.push(DirectoryTail { mapping: Some(mapping), backing: extent,
                offset, anchor: usc }, GFP_KERNEL)?;
            let tail = &transaction.tails[usc];
            if tail.mapping.as_ref().ok_or(EINVAL)?.iova() != low { return Err(EFAULT); }
            for page in (0..extent_bytes).step_by(mmu::UAT_PGSZ) {
                if uat.kernel_vm().translate_iova(tail.backing.iova() + page as u64)?
                    != uat.kernel_lower_vm().translate_iova(low + page as u64)? { return Err(EFAULT); }
            }
        }
        // vmap may fail, and list validation may fail: both precede mutation.
        // No fallible operation follows the complete block-list update.
        self.block_list.with_bytes_mut(|bytes| {
            let start = previous as usize * 4;
            let end = new.block_count as usize * 4;
            if end > bytes.len() || bytes[start..].iter().any(|&v| v != 0) { return Err(EBUSY); }
            for (index, value) in entries.iter().enumerate() {
                bytes[start + index*4..start + index*4 + 4].copy_from_slice(&value.to_le_bytes());
            }
            Ok(())
        })?;
        let tails = transaction.commit();
        for mut tail in tails {
            // All destination capacity was reserved before staging began.
            self.aliases.push(tail.mapping.take().expect("owned directory tail"), GFP_KERNEL)
                .expect("reserved alias capacity");
            self.directory_growth.push(tail.backing, GFP_KERNEL).expect("reserved backing capacity");
            self.directory_growth_offsets.push(tail.offset, GFP_KERNEL).expect("reserved offset capacity");
        }
        append_reserved(&mut self.scratch_aliases, aliases);
        append_reserved(&mut self.scratch, backing);
        self.scratch_plan = Some(g18_uma::G18ComputeScratchPlan {
            block_bytes: G18_COMPUTE_BLOCK_BYTES, block_count: new.block_count,
            register_pair: new.register_pair.map(u64::from), chunk_counters: new.chunk_counters,
        });
        self.shared_compute_plan = Some(plan);
        fence(Ordering::SeqCst);
        crate::g18_kick_info!(dev.as_ref(),
            "G18_SHARED_COMPUTE_OWNER bytes={} blocks={} directory_pages={} previous_blocks={} pair={:?} counters={:?} unpublished=1\n",
            new.bytes, new.block_count, directory.committed_pages(), previous,
            new.register_pair, new.chunk_counters);
        if let Some(old) = retained {
            crate::g18_kick_info!(dev.as_ref(),
                "G18_SHARED_COMPUTE_REPLACEMENT old_bias={:#x} new_bias={:#x} retained_blocks={} blocks={} unpublished=1\n",
                old.low_bias, self.low_bias, old.scratch.len().min(new.block_count as usize), new.block_count);
        }
        Ok(())
    }
}
