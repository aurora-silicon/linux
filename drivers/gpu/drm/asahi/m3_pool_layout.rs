// SPDX-License-Identifier: GPL-2.0-only OR MIT
use crate::m3_queue_layout::{Error, FirmwareVa};
pub(crate) const MANAGER_SIZE: usize = 0x84;
pub(crate) const BACKING_SIZE: usize = 4 * 1024 * 1024;
pub(crate) const PAGE_SIZE: usize = 0x1000;
pub(crate) const PAGE_CAPACITY: usize = 0x80000;
pub(crate) const INITIAL_PAGES: usize = 8 * BACKING_SIZE / PAGE_SIZE;
pub(crate) const POOL_TABLE_SIZE: usize = 0x1000;
pub(crate) const POOL_ENTRY_SIZE: usize = 0x40;
const POOL_TAG: u64 = 1 << 62; // Qualified ownership tag; precise meaning unresolved.
/// An owned page-aligned region in G15's lower 42-bit UAT root. A raw firmware
/// pointer, tagged table entry or compact scene pointer cannot pass this check.
#[derive(Clone, Copy, Debug)]
pub(crate) struct GpuRegion { base: u64, size: usize }
impl GpuRegion {
    pub(crate) fn base(self) -> u64 { self.base }
    pub(crate) fn size(self) -> usize { self.size }
    pub(crate) fn new(base: u64, size: usize) -> Result<Self, Error> {
        let end = base.checked_add(size as u64).ok_or(Error::Address)?;
        if base == 0 || base & (PAGE_SIZE as u64 - 1) != 0 || size == 0 || end > 1 << 42 {
            return Err(Error::Address);
        }
        Ok(Self { base, size })
    }
}
pub(crate) struct Pool<'a> {
    backings: &'a [GpuRegion], pages: GpuRegion, table: GpuRegion,
    counter: FirmwareVa, table_index: u8,
}
impl<'a> Pool<'a> {
    pub(crate) fn new(backings: &'a [GpuRegion], pages: GpuRegion, table: GpuRegion,
        counter: FirmwareVa, table_index: u8) -> Result<Self, Error> {
        if !(8..=22).contains(&backings.len()) || backings.iter().any(|b| b.size != BACKING_SIZE)
            || pages.size < PAGE_CAPACITY * 8 || table.size < POOL_TABLE_SIZE {
            return Err(Error::Size);
        }
        Ok(Self { backings, pages, table, counter, table_index })
    }
    /// Address of an initially available 4-KiB page; the remaining freelist is
    /// zero from allocation. Firmware subsequently advances its own cursors.
    pub(crate) fn page(&self, index: usize) -> Result<u64, Error> {
        if index >= INITIAL_PAGES { return Err(Error::Index); }
        Ok(self.backings[index / (BACKING_SIZE / PAGE_SIZE)].base
            + (index % (BACKING_SIZE / PAGE_SIZE) * PAGE_SIZE) as u64)
    }
    pub(crate) fn table_entry(&self, index: usize) -> Result<[u8; POOL_ENTRY_SIZE], Error> {
        let backing = self.backings.get(index).ok_or(Error::Index)?;
        let mut bytes = [0; POOL_ENTRY_SIZE];
        bytes[..8].copy_from_slice(&(backing.base | POOL_TAG).to_le_bytes());
        Ok(bytes)
    }
    /// Exact packed layout, including unaligned pointer fields. All validation
    /// precedes mutation and caller bytes beyond MANAGER_SIZE are untouched.
    pub(crate) fn encode(&self, out: &mut [u8]) -> Result<(), Error> {
        let bytes = out.get_mut(..MANAGER_SIZE).ok_or(Error::Size)?;
        bytes.fill(0);
        for (offset, value) in [(0x00, u32::from(self.table_index) + 1), // software pool ID
            (0x08, u32::from(self.table_index)), // HW table index, FW34584
            (0x0c, 1), // retained unk_c
            (0x1c, PAGE_CAPACITY as u32),
            (0x24, INITIAL_PAGES as u32), // initial producer cursor
            (0x2c, INITIAL_PAGES as u32), // retained available-page count
            (0x44, 0x40)] { // retained unk_44; not the number of allocations
            bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        }
        for (offset, value) in [(0x14, self.pages.base), (0x34, self.table.base), (0x48, self.counter.get())] {
            bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        }
        Ok(())
    }
}
