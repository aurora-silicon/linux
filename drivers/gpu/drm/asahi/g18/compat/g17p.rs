// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module 1134 G17P items read by carried translation and diagnostic code.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.
#[cfg(CONFIG_DEV_COREDUMP)]
use core::ptr::NonNull;
#[cfg(CONFIG_DEV_COREDUMP)]
use core::sync::atomic::{AtomicU64, Ordering};
#[cfg(CONFIG_DEV_COREDUMP)]
use kernel::{addr::PhysicalAddr, bindings, ffi::c_void, prelude::*};
#[cfg(CONFIG_DEV_COREDUMP)]
use crate::pgtable::{UAT_PGMSK, UAT_PGSZ};

// Pinned J700 T8140 GFX A000 scheduler root used by the exact EP type-4
// thunk. The extracted image has SHA-256
// 3e2821bb40d3abffe30488a06ede5f523cfa47544d6a0ad7654a08be6b5dc30f.
// Its __DATA vmaddr/size are 0xfffffc000004c000/0x134000, exactly matching
// the J700 gfx-data carveout size. These addresses are diagnostic only.
const G17P_GFX_LINK_DATA_BASE: u64 = 0xffff_fc00_0004_c000;

const G17P_GFX_PHYSICAL_DATA_BASE: u64 = 0x0000_0100_01d7_c000;

/// Translate a GFX firmware LINK address into the physical address the host
/// can map. Same arithmetic the render status-page probes use.
pub(crate) fn g17p_gfx_link_to_physical(link: u64) -> u64 {
    G17P_GFX_PHYSICAL_DATA_BASE + (link - G17P_GFX_LINK_DATA_BASE)
}

/// Platform-derived SKSM scratch offsets within the mapped accelerator
/// register window.
///
/// Apple's host loads these three values from accelerator object fields
/// `+0x10730`, `+0x10734`, and `+0x10738`. Those are object offsets, not SGX
/// MMIO offsets. This type carries the loaded values and no physical base.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmScratchGeometry {
    pub(crate) write0_base: u32,
    pub(crate) write1_base: u32,
    pub(crate) stride: u32,
}

/// The two offsets used by the fixed G17P scratch FIFO slot.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmScratchOffsets {
    pub(crate) write0: u32,
    pub(crate) write1: u32,
}

impl G17SksmScratchGeometry {
    /// Apply the exact host equation for fixed FIFO slot 9.
    ///
    /// The slot is independent of the SKSM queue ID carried in each write.
    pub(crate) const fn fifo_offsets(
        self,
    ) -> core::result::Result<G17SksmScratchOffsets, ConstructionError> {
        const FIFO_SLOT: u32 = 9;

        let fifo_offset = match self.stride.checked_mul(FIFO_SLOT) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };
        let write0 = match self.write0_base.checked_add(fifo_offset) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };
        let write1 = match self.write1_base.checked_add(fifo_offset) {
            Some(offset) => offset,
            None => return Err(ConstructionError::SksmScratchOffsetOverflow),
        };

        Ok(G17SksmScratchOffsets { write0, write1 })
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum ConstructionError {
    SksmScratchOffsetOverflow,
}

#[cfg(CONFIG_DEV_COREDUMP)]
struct PhysPageMapping {
    ptr: NonNull<c_void>,
}

/// A bounded read-only probe for one qword in live firmware-owned physical
/// memory.
///
/// Construction maps the containing page once. [`Self::observe_mask`] then
/// invalidates only the sampled AP cacheline before each load, so a short
/// firmware transition can be observed without remapping, writing, or holding
/// any firmware lock.
#[cfg(CONFIG_DEV_COREDUMP)]
pub(crate) struct LiveFirmwareU64Probe {
    page: PhysPageMapping,
    offset: usize,
}

/// Result of one bounded live-mask observation.
#[cfg(CONFIG_DEV_COREDUMP)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct LiveFirmwareMaskObservation {
    pub(crate) first: u64,
    pub(crate) last: u64,
    pub(crate) observed_or: u64,
    pub(crate) mask_seen: bool,
    pub(crate) polls: usize,
}

#[cfg(CONFIG_DEV_COREDUMP)]
impl LiveFirmwareU64Probe {
    const MAX_POLLS: usize = 256;

    pub(crate) fn new(physical: PhysicalAddr) -> Result<Self> {
        if physical & 7 != 0 {
            return Err(EINVAL);
        }
        let page_mask = UAT_PGMSK as u64;
        let page = PhysPageMapping::new(physical & !page_mask)?;
        let offset = (physical & page_mask) as usize;
        if offset.checked_add(size_of::<u64>()).ok_or(EOVERFLOW)? > UAT_PGSZ {
            return Err(EINVAL);
        }
        Ok(Self { page, offset })
    }

    /// Observe one mask for at most `polls` cache-invalidated qword loads.
    pub(crate) fn observe_mask(
        &self,
        mask: u64,
        polls: usize,
    ) -> Result<LiveFirmwareMaskObservation> {
        if mask == 0 || polls == 0 || polls > Self::MAX_POLLS {
            return Err(EINVAL);
        }

        let mut first = 0;
        let mut last = 0;
        let mut observed_or = 0;
        let mut mask_seen = false;
        let mut completed = 0;
        for index in 0..polls {
            self.page.invalidate_live_range(self.offset, size_of::<u64>())?;
            let value = self.page.read_u64(self.offset)?;
            if index == 0 {
                first = value;
            }
            last = value;
            observed_or |= value;
            completed = index + 1;
            if value & mask != 0 {
                mask_seen = true;
                break;
            }
            core::hint::spin_loop();
        }

        Ok(LiveFirmwareMaskObservation {
            first,
            last,
            observed_or,
            mask_seen,
            polls: completed,
        })
    }
}

#[cfg(CONFIG_DEV_COREDUMP)]
impl PhysPageMapping {
    fn new(phys: PhysicalAddr) -> Result<Self> {
        if phys & UAT_PGMSK as u64 != 0 {
            return Err(EINVAL);
        }
        let ptr = unsafe {
            // SAFETY: the caller supplies a page-aligned physical address
            // obtained from a live firmware UAT PTE. The mapping is used only
            // for bounded reads and is released by Drop.
            bindings::memremap(
                phys,
                UAT_PGSZ,
                bindings::MEMREMAP_WB as _,
            )
        };
        let ptr = NonNull::new(ptr).ok_or_else(|| {
            pr_err!("UAT crash walker: memremap failed for {:#x}\n", phys);
            ENOMEM
        })?;
        Ok(Self { ptr })
    }

    fn invalidate_live_range(&self, offset: usize, size: usize) -> Result {
        let end = offset.checked_add(size).ok_or(EOVERFLOW)?;
        if size == 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        let ctr_el0: u64;
        unsafe {
            // SAFETY: CTR_EL0 is readable at EL1 and reports DminLine as
            // log2(words). The resulting line size controls only bounded cache
            // maintenance on this read-only mapping.
            core::arch::asm!(
                "mrs {ctr}, ctr_el0",
                ctr = out(reg) ctr_el0,
                options(nomem, nostack, preserves_flags),
            );
        }
        let line_size = 4usize
            .checked_shl(((ctr_el0 >> 16) & 0xf) as u32)
            .ok_or(EOVERFLOW)?;
        if !line_size.is_power_of_two() {
            return Err(EINVAL);
        }
        let start_address = self.ptr.as_ptr() as usize + offset;
        let end_address = self.ptr.as_ptr() as usize + end;
        let mut address = start_address & !(line_size - 1);
        unsafe {
            // SAFETY: the page remains mapped for the whole loop. `dc ivac`
            // invalidates AP cachelines only; this diagnostic never writes the
            // firmware-owned page. The barriers order invalidation before the
            // atomic loads in `read_u64()`.
            core::arch::asm!("dsb osh", options(nostack, preserves_flags));
            while address < end_address {
                core::arch::asm!(
                    "dc ivac, {address}",
                    address = in(reg) address,
                    options(nostack, preserves_flags),
                );
                address = address.checked_add(line_size).ok_or(EOVERFLOW)?;
            }
            core::arch::asm!("dsb osh", options(nostack, preserves_flags));
        }
        Ok(())
    }

    fn read_u64(&self, offset: usize) -> Result<u64> {
        let end = offset.checked_add(8).ok_or(EOVERFLOW)?;
        if offset & 7 != 0 || end > UAT_PGSZ {
            return Err(EINVAL);
        }
        let pointer = unsafe {
            // SAFETY: offset is qword-aligned and bounded to the mapped page.
            self.ptr.as_ptr().cast::<u8>().add(offset).cast::<AtomicU64>()
        };
        Ok(unsafe { &*pointer }.load(Ordering::Acquire))
    }
}

#[cfg(CONFIG_DEV_COREDUMP)]
impl Drop for PhysPageMapping {
    fn drop(&mut self) {
        unsafe {
            // SAFETY: ptr was returned by memremap and has not been unmapped.
            bindings::memunmap(self.ptr.as_ptr());
        }
    }
}
