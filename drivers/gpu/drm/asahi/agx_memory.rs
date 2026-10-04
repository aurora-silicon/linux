// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Firmware buffers backed by common GEM objects and DRM-managed UAT ranges.

use kernel::prelude::*;
use crate::{driver, gem, mmu, pgtable::prot};

pub(crate) const KERNEL_RANGE: core::ops::Range<u64> =
    0xffff_fc20_0000_0000..0xffff_fc2f_fc00_0000;

// Firmware validates Timestamp user destinations against a dedicated 64 MiB
// interval published in HwDataB+0x28. Reserve the last 64 MiB of the
// firmware VM selector, disjoint from MMIO/command allocations above.
pub(crate) const TIMESTAMP_RANGE: core::ops::Range<u64> =
    0xffff_fc2f_fc00_0000..0xffff_fc30_0000_0000;

pub(crate) struct Buffer {
    // Unmap before releasing our GEM/vmap references.
    mapping: mmu::KernelMapping,
    gpu_mapping: Option<mmu::KernelMapping>,
    object: gem::ObjectRef,
    size: usize,
}

/// Storage whose addresses are published only in client GPU page lists.
/// Retain its GEM backing and client mapping without an unused firmware alias.
pub(crate) struct GpuBuffer {
    mapping: mmu::KernelMapping,
    object: gem::ObjectRef,
    size: usize,
}

impl GpuBuffer {
    pub(crate) fn new(dev: &driver::AsahiDevice, vm: &mmu::Vm, size: usize) -> Result<Self> {
        if size == 0 || size.checked_add(mmu::UAT_PGMSK).is_none() { return Err(EINVAL); }
        let mut object = gem::new_kernel_object_wc(dev, size)?;
        let mapping = object.map_into_range(vm, 0x71_0000_0000..0x80_0000_0000,
            mmu::UAT_PGSZ as u64, prot::PROT_GPU_SHARED_RW, true)?;
        Ok(Self { mapping, object, size })
    }
    pub(crate) fn va(&self) -> u64 { self.mapping.iova() }
    /// Zero `len` bytes from `offset` through a retained WC CPU alias, for
    /// an owner whose engine consumers have provably retired.
    pub(crate) fn clear_range(&mut self, offset: usize, len: usize) -> Result {
        if offset.checked_add(len).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.cached_vmap()?;
        // SAFETY: checked range inside the retained page-aligned GEM vmap.
        unsafe { core::ptr::write_bytes(vmap.as_mut_ptr().add(offset), 0, len) };
        Ok(())
    }
}

impl Buffer {
    pub(crate) fn new(dev: &driver::AsahiDevice, vm: &mmu::Vm, size: usize) -> Result<Self> {
        Self::allocate(dev, vm, size, prot::PROT_FW_SHARED_RW)
    }
    fn allocate(dev: &driver::AsahiDevice, vm: &mmu::Vm, size: usize,
        protection: prot::Prot) -> Result<Self> {
        if size == 0 || size.checked_add(mmu::UAT_PGMSK).is_none() { return Err(EINVAL); }
        let mut object = gem::new_kernel_object_wc(dev, size)?;
        // Fresh GEM shmem backing is already zero-filled. map_into_range pins
        // and DMA-maps those pages before publishing the UAT mapping. Do not
        // vmap and clear the entire allocation a second time through a WC CPU
        // alias; GPU-only private pools need no CPU mapping at all.
        let mapping = object.map_into_range(vm, KERNEL_RANGE, mmu::UAT_PGSZ as u64,
            protection, true)?;
        Ok(Self { mapping, gpu_mapping: None, object, size })
    }

    pub(crate) fn new_gpu(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize) -> Result<Self> {
        Self::new_gpu_protected(dev, firmware_vm, gpu_vm, size, prot::PROT_GPU_SHARED_RW)
    }
    /// Instruction/control streams are immutable to the GPU (AP=2, UXN=0),
    /// while writable command state uses the distinct data permission.
    pub(crate) fn new_gpu_readonly(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize) -> Result<Self> {
        Self::new_gpu_protected(dev, firmware_vm, gpu_vm, size, prot::PROT_GPU_SHARED_RO)
    }
    fn new_gpu_protected(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize, protection: prot::Prot) -> Result<Self> {
        // Firmware-generated engine state can retain canonical aliases.
        // Match the shared firmware/GPU access of the qualified M4 objects.
        let mut buffer = Self::allocate(dev, firmware_vm, size, prot::PROT_GPU_FW_SHARED_RW)?;
        buffer.gpu_mapping = Some(buffer.object.map_into_range(gpu_vm,
            0x71_0000_0000..0x80_0000_0000, mmu::UAT_PGSZ as u64,
            protection, true)?);
        Ok(buffer)
    }
    /// The bootstrap engine fetches command registers before switching ASID.
    /// Give mirrored command VAs their own interval, disjoint from client
    /// backing. A retained prior pass must not collide with the next command.
    pub(crate) fn new_command(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        bootstrap_vm: &mmu::Vm, size: usize) -> Result<Self> {
        let mut buffer = Self::allocate(dev, firmware_vm, size, prot::PROT_GPU_FW_SHARED_RW)?;
        buffer.gpu_mapping = Some(buffer.object.map_into_range(bootstrap_vm,
            0x70_0000_0000..0x71_0000_0000, mmu::UAT_PGSZ as u64, prot::PROT_GPU_SHARED_RW, true)?);
        Ok(buffer)
    }
    pub(crate) fn new_gpu_aligned(dev: &driver::AsahiDevice, firmware_vm: &mmu::Vm,
        gpu_vm: &mmu::Vm, size: usize, alignment: u64) -> Result<Self> {
        if alignment < mmu::UAT_PGSZ as u64 || !alignment.is_power_of_two() { return Err(EINVAL); }
        let mut buffer = Self::allocate(dev, firmware_vm, size, prot::PROT_GPU_FW_SHARED_RW)?;
        buffer.gpu_mapping = Some(buffer.object.map_into_range(gpu_vm,
            0x71_0000_0000..0x80_0000_0000, alignment, prot::PROT_GPU_SHARED_RW, true)?);
        Ok(buffer)
    }
    /// Retain one explicit command-buffer view across the bootstrap and client
    /// contexts. The engine fetches its register list before switching ASID.
    pub(crate) fn map_gpu_view(&mut self, vm: &mmu::Vm) -> Result<mmu::KernelMapping> {
        let address = self.gpu_va()?;
        self.object.map_at(vm, address, prot::PROT_GPU_SHARED_RW, true)
    }
    pub(crate) fn gpu_va(&self) -> Result<u64> {
        self.gpu_mapping.as_ref().map(|mapping| mapping.iova()).ok_or(EINVAL)
    }
    pub(crate) fn read_u64(&mut self, offset: usize) -> Result<u64> {
        let mut bytes = [0; 8];
        self.read(offset, &mut bytes)?;
        Ok(u64::from_le_bytes(bytes))
    }
    pub(crate) fn va(&self) -> u64 { self.mapping.iova() }
    pub(crate) fn size(&self) -> usize { self.size }
    /// Reset a retired allocation before rebuilding its firmware records.
    /// The caller must have proved that no engine or firmware owner still
    /// consumes the previous contents.
    pub(crate) fn clear(&mut self) -> Result {
        self.object.cached_vmap()?.memset(0);
        Ok(())
    }
    pub(crate) fn clear_range(&mut self, offset: usize, len: usize) -> Result {
        if offset.checked_add(len).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.cached_vmap()?;
        // SAFETY: checked range inside the retained page-aligned GEM vmap.
        unsafe { core::ptr::write_bytes(vmap.as_mut_ptr().add(offset), 0, len) };
        Ok(())
    }
    pub(crate) fn write(&mut self, offset: usize, bytes: &[u8]) -> Result {
        if offset.checked_add(bytes.len()).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        self.object.cached_vmap()?.write(bytes, offset)
    }
    pub(crate) fn u32(&mut self, offset: usize, value: u32) -> Result {
        if offset & 3 != 0 || offset.checked_add(4).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.cached_vmap()?;
        // SAFETY: aligned, checked u32 in retained WC normal memory. Channel
        // producer/consumer indices must be published with one word store.
        unsafe { vmap.as_mut_ptr().add(offset).cast::<u32>().write_volatile(value.to_le()) };
        Ok(())
    }
    pub(crate) fn u64(&mut self, offset: usize, value: u64) -> Result {
        self.write(offset, &value.to_le_bytes())
    }
    /// Snapshot shared bytes. Callers follow the firmware's producer/consumer
    /// protocol; no Rust slice aliases memory that firmware can mutate.
    pub(crate) fn read(&mut self, offset: usize, bytes: &mut [u8]) -> Result {
        if offset.checked_add(bytes.len()).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.cached_vmap()?;
        // SAFETY: WC normal RAM, retained by the page-aligned GEM/vmap,
        // checked byte range. Read aligned words without creating references
        // to firmware-owned memory. Byte accesses handle unaligned edges.
        // A volatile byte loop makes every diagnostic record pay for dozens
        // of separate uncached reads; normal RAM permits wider accesses.
        unsafe {
            core::arch::asm!("dmb osh", options(nostack, preserves_flags));
            let source = vmap.as_ptr().add(offset);
            let mut i = 0;
            while i < bytes.len() && (offset + i) & 7 != 0 {
                bytes[i] = source.add(i).read_volatile();
                i += 1;
            }
            while bytes.len() - i >= 8 {
                let word = source.add(i).cast::<u64>().read_volatile();
                bytes[i..i + 8].copy_from_slice(&word.to_ne_bytes());
                i += 8;
            }
            while i < bytes.len() {
                bytes[i] = source.add(i).read_volatile();
                i += 1;
            }
        }
        Ok(())
    }
    pub(crate) fn read_u32(&mut self, offset: usize) -> Result<u32> {
        if offset & 3 != 0 || offset.checked_add(4).ok_or(EOVERFLOW)? > self.size { return Err(ERANGE); }
        let vmap = self.object.cached_vmap()?;
        // SAFETY: Aligned u32 in the checked GEM allocation; firmware channel
        // indices and ready flags are naturally aligned atomic-sized accesses.
        let value = unsafe { vmap.as_ptr().add(offset).cast::<u32>().read_volatile() };
        acquire();
        Ok(u32::from_le(value))
    }
}

/// Populate a USC private-memory freelist and its 2 MiB block list for
/// `pages` pages of storage at `base`, with one staged copy per list.
/// The freelist ring and the block page list for a private pool, as the
/// bytes written at pool-state +0 and page-list +0. Owners keep both images
/// so a recycled owner rewrites its pool with two bulk copies.
pub(crate) fn freelist_images(base: u64, pages: usize) -> Result<(KVec<u8>, KVec<u8>)> {
    let mut pool = KVec::new();
    pool.reserve(pages * 8, GFP_KERNEL)?;
    for page in 0..pages {
        pool.extend_from_slice(&(base + page as u64 * 0x1000).to_le_bytes(), GFP_KERNEL)?;
    }
    let mut list = KVec::new();
    let blocks = pages * 0x1000 / 0x200000;
    list.reserve(blocks * 64, GFP_KERNEL)?;
    for block in 0..blocks {
        list.extend_from_slice(&((1u64 << 61) | (base + block as u64 * 0x200000)).to_le_bytes(), GFP_KERNEL)?;
        list.extend_from_slice(&[0; 56], GFP_KERNEL)?;
    }
    Ok((pool, list))
}

pub(crate) fn write_freelist(pool_state: &mut Buffer, page_list: &mut Buffer,
    base: u64, pages: usize) -> Result {
    let mut pool = KVec::new();
    pool.reserve(pages * 8, GFP_KERNEL)?;
    for page in 0..pages {
        pool.extend_from_slice(&(base + page as u64 * 0x1000).to_le_bytes(), GFP_KERNEL)?;
    }
    pool_state.write(0, &pool)?;
    let mut list = KVec::new();
    let blocks = pages * 0x1000 / 0x200000;
    list.reserve(blocks * 64, GFP_KERNEL)?;
    for block in 0..blocks {
        list.extend_from_slice(&((1u64 << 61) | (base + block as u64 * 0x200000)).to_le_bytes(), GFP_KERNEL)?;
        list.extend_from_slice(&[0; 56], GFP_KERNEL)?;
    }
    page_list.write(0, &list)
}

/// Pages handed out from a retired owner's freelist, from the descriptor's
/// consumer index at +0x20. The rebuilt ring lists pages in address order,
/// so the first `consumed` entries are the first `consumed` pages of the
/// pool. A consumer beyond the populated count means recycled entries were
/// reused; report the whole pool so the caller clears everything.
pub(crate) fn consumed_pages(descriptor: &mut Buffer, offset: usize, pages: usize) -> Result<usize> {
    let consumed = descriptor.read_u32(offset + 0x20)? as usize;
    Ok(if consumed <= pages { consumed } else { pages })
}

pub(crate) fn publish() {
    // SAFETY: Orders preceding WC buffer stores before a device doorbell.
    unsafe { core::arch::asm!("dsb oshst", options(nostack, preserves_flags)) };
}
fn acquire() {
    // SAFETY: Orders firmware payload reads after its published producer.
    unsafe { core::arch::asm!("dmb oshld", options(nostack, preserves_flags)) };
}
