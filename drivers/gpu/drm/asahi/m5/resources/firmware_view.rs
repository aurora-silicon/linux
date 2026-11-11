// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Read-only view of the GFX firmware's DATA segment (the `gfx-data` reserved region).

use crate::driver::AsahiDrmDevice;
use core::sync::atomic::{fence, Ordering};
use kernel::{c_str, io, prelude::*};

pub(super) struct FirmwareDataView {
    map: io::mem::Mem,
    cache_line: usize,
}

// SAFETY: This owns a fixed mapping of reserved RAM, read only through
// bounded volatile accesses. It neither transfers ownership nor writes DATA.
unsafe impl Send for FirmwareDataView {}
// SAFETY: Concurrent read-only volatile observations do not mutate host state.
unsafe impl Sync for FirmwareDataView {}

impl FirmwareDataView {
    pub(super) fn new(dev: &AsahiDrmDevice) -> Result<Self> {
        let resource = dev.as_ref().as_ref().of_node().ok_or(EINVAL)?
            .reserved_mem_region_to_resource_byname(c_str!("gfx-data"))?;
        if resource.size() != 0x134000 || resource.start() < 0x100_0000_0000
            || resource.start() & 0x3fff != 0
            || resource.start().checked_add(resource.size()).ok_or(EOVERFLOW)? > 0x104_0000_0000
            || 0x127430 - 0x4c000 + 13 * 0x28 > resource.size() as usize
            || 0x1257e0 - 0x4c000 + 13 * 8 > resource.size() as usize
            || 0x178458 - 0x4c000 + 8 > resource.size() as usize {
            return Err(EINVAL);
        }
        let ctr: u64;
        // SAFETY: CTR_EL0 is an architectural read-only cache geometry register.
        unsafe { core::arch::asm!("mrs {ctr}, ctr_el0", ctr = out(reg) ctr,
            options(nomem, nostack, preserves_flags)) };
        let cache_line = 4usize << ((ctr >> 16) & 0xf);
        if cache_line > 0x4000 { return Err(EINVAL); }
        // SAFETY: WB matches reserved System RAM. The host never writes this
        // extent; observations invalidate covering lines before volatile reads.
        let map = unsafe { io::mem::Mem::try_new(resource, io::mem::MemFlag::WB.into()) }?;
        fence(Ordering::SeqCst);
        Ok(Self { map, cache_line })
    }

    fn words<const N: usize>(&self, offset: usize) -> [u64; N] {
        // All callers use constructor-checked queue, scheduler or power extents.
        let line = self.cache_line;
        let start = offset & !(line - 1);
        let end = (offset + N * 8 + line - 1) & !(line - 1);
        for offset in (start..end).step_by(line) {
            // SAFETY: Whole covering lines are inside the retained DATA view.
            // The host never dirties them; do not clean stale data to firmware.
            unsafe { core::arch::asm!("dc ivac, {addr}",
                addr = in(reg) self.map.ptr().add(offset),
                options(nostack, preserves_flags)) };
        }
        // SAFETY: Complete invalidations before volatile observations.
        unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) };
        let mut out = [0; N];
        for (index, value) in out.iter_mut().enumerate() {
            // SAFETY: The constructor bounds-checks every supported extent.
            *value = unsafe { core::ptr::read_volatile(
                self.map.ptr().add(offset + index * 8).cast::<u64>()) };
        }
        fence(Ordering::Acquire);
        out
    }

    pub(super) fn queue_registration(&self, queue_id: u8) -> Result<([u64; 5], u64)> {
        // TA/fragment own 10/11; application compute owns 12. The constructor
        // bounds both complete tables through the last admitted queue.
        if !(10..=12).contains(&queue_id) { return Err(EINVAL); }
        Ok((self.words::<5>(0x127430 - 0x4c000 + queue_id as usize * 0x28),
            self.words::<1>(0x1257e0 - 0x4c000 + queue_id as usize * 8)[0]))
    }
}
