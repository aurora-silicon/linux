// SPDX-License-Identifier: GPL-2.0-only

//! Kernel-owned scratch for sparse fills (DRM_ASAHI_BIND_SPARSE_TILE).

use kernel::{prelude::*, sync::Arc};

/// A zeroed, physically contiguous kernel allocation. The page tables repeat
/// its first page across a filled range, as a SINGLE_PAGE bind repeats its
/// GEM's first page.
pub(crate) struct SparseTile {
    page: *mut kernel::bindings::page,
    physical: u64,
    order: u32,
}

// SAFETY: The pages are exclusively owned until Drop; only their physical
// address is shared, and it is immutable.
unsafe impl Send for SparseTile {}
// SAFETY: See Send. No interior mutability.
unsafe impl Sync for SparseTile {}

impl SparseTile {
    pub(super) fn new() -> Result<Arc<Self>> {
        Ok(Arc::new(Self::zeroed(crate::pgtable::UAT_PGSZ)?, GFP_KERNEL)?)
    }

    /// Allocate a naturally aligned contiguous block covering the requested bytes.
    pub(crate) fn zeroed(bytes: usize) -> Result<Self> {
        if bytes == 0 { return Err(EINVAL); }
        let pages = bytes.div_ceil(kernel::page::PAGE_SIZE);
        let order = pages.checked_next_power_of_two().ok_or(EOVERFLOW)?.trailing_zeros();
        // SAFETY: A plain zeroed higher-order page allocation.
        let page = unsafe { kernel::bindings::alloc_pages(
            kernel::bindings::GFP_KERNEL | kernel::bindings::__GFP_ZERO, order) };
        if page.is_null() { return Err(ENOMEM); }
        // SAFETY: page is a live allocation returned above.
        let physical = unsafe { kernel::bindings::page_to_phys(page) } as u64;
        Ok(Self { page, physical, order })
    }

    pub(crate) fn physical(&self) -> u64 { self.physical }

    pub(crate) fn with_cpu_page<R>(&self, offset: usize, f: impl FnOnce(*mut u8) -> R) -> Result<R> {
        let size = kernel::page::PAGE_SIZE;
        if offset % size != 0 || offset / size >= 1usize << self.order {
            return Err(EINVAL);
        }
        let pa = self.physical + offset as u64;
        // SAFETY: The complete physical allocation stays live across this scoped mapping.
        let page = unsafe { kernel::page::Page::borrow_phys(&pa) }.ok_or(EFAULT)?;
        Ok(page.with_page_mapped(f))
    }
}

impl Drop for SparseTile {
    fn drop(&mut self) {
        // SAFETY: The VM that maps the tile owns it and drops it after its
        // page tables.
        unsafe { kernel::bindings::__free_pages(self.page, self.order) };
    }
}
