// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::{m3_pool_layout::GpuRegion, m3_queue_layout::{Error, FirmwareVa}};
pub(crate) const MANAGER_SIZE: usize = 0xac;
pub(crate) const PAGE_SIZE: usize = 0x8000;
pub(crate) const BLOCK_SIZE: usize = 0x20000;
pub(crate) const PAGES_PER_BLOCK: usize = BLOCK_SIZE / PAGE_SIZE;
const APERTURE: u64 = 0x10_0000_0000;
const QUALIFIED_MAX_BLOCKS: u32 = 0x26cd;
/// Compact page numbers count 32-KiB pages from the qualified low aperture.
/// They are neither byte addresses nor firmware virtual addresses.
pub(crate) struct Backing { first_page: u32, blocks: usize }
impl Backing {
    pub(crate) fn new(region: GpuRegion) -> Result<Self, Error> {
        let offset = region.base().checked_sub(APERTURE).ok_or(Error::Address)?;
        if offset & (PAGE_SIZE as u64 - 1) != 0 || region.size() % BLOCK_SIZE != 0 {
            return Err(Error::Address);
        }
        let first_page = u32::try_from(offset / PAGE_SIZE as u64).map_err(|_| Error::Address)?;
        let last = offset.checked_add(region.size() as u64 - 1).ok_or(Error::Address)? / PAGE_SIZE as u64;
        u32::try_from(last).map_err(|_| Error::Address)?;
        Ok(Self { first_page, blocks: region.size() / BLOCK_SIZE })
    }
    pub(crate) fn page(&self, index: usize) -> Result<u32, Error> {
        if index >= self.blocks * PAGES_PER_BLOCK { return Err(Error::Index); }
        Ok(self.first_page + index as u32)
    }
    pub(crate) fn block(&self, index: usize) -> Result<[u8; 8], Error> {
        if index >= self.blocks { return Err(Error::Index); }
        let mut bytes = [0; 8];
        bytes[..4].copy_from_slice(&self.page(index * PAGES_PER_BLOCK)?.to_le_bytes());
        Ok(bytes)
    }
}
pub(crate) struct Manager {
    pages: GpuRegion, blocks: GpuRegion, control: FirmwareVa, counter: FirmwareVa,
    block_count: u32, page_count: u32,
}
impl Manager {
    pub(crate) fn new(pages: GpuRegion, blocks: GpuRegion, control: FirmwareVa,
        counter: FirmwareVa, block_count: u32) -> Result<Self, Error> {
        let page_count = block_count.checked_mul(PAGES_PER_BLOCK as u32).ok_or(Error::Size)?;
        if block_count == 0 || block_count > QUALIFIED_MAX_BLOCKS
            || pages.size() > u32::MAX as usize || page_count as usize > pages.size() / 4
            || block_count as usize > blocks.size() / 8 {
            return Err(Error::Size);
        }
        Ok(Self { pages, blocks, control, counter, block_count, page_count })
    }
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        let bytes = out.get_mut(..MANAGER_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        for (offset, value) in [(0x1c, self.pages.base()), (0x38, self.blocks.base()),
            (0x40, self.control.get()), (0x58, self.counter.get())] {
            bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        }
        for (offset, value) in [(0x24, self.pages.size() as u32), (0x28, self.page_count),
            (0x2c, QUALIFIED_MAX_BLOCKS), (0x30, self.block_count), (0x48, self.page_count - 1),
            (0x4c, BLOCK_SIZE as u32), (0x70, self.page_count), (0x74, self.page_count)] {
            bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        Ok(())
    }
}
