// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 The Gravity Linux Contributors
//
// Derived from the crash buffer mapping of Gravity Linux's M4 GPU driver (CrashBuffer and
// BootOps::shmem_map in drivers/gpu/drm/asahi/g16.rs), written by Cody Ho:
// https://github.com/GravityLinux/linux/commit/bd25d194b0a295a75692a0e6caed3793603b9e65
// Gravity's g16.rs is adapted from Niklas Sheth's linux-m4-integration prototype.

//! Crash log buffers in the GPU firmware's data segment.
//!
//! A firmware that places its own crash log ([`super::CrashBuffers::FirmwareData`]) asks for it
//! by physical address inside its data segment, the `fw-data` reserved region, which the boot
//! loader keeps for the loaded firmware until reboot.

use kernel::{
    c_str, device,
    io::{
        self,
        IoSysMap,
        Region, //
    },
    prelude::*,
};

/// Whether the boot loader described the firmware's data segment as the `fw-data` region of
/// `dev`.
pub(super) fn described(dev: &device::Device) -> bool {
    dev.of_node()
        .is_some_and(|node| node.reserved_mem_region_to_resource_byname(c_str!("fw-data")).is_ok())
}

/// The offset of the `size` bytes at physical address `address` in a region of `bytes` bytes at
/// `base`; the range must be non-empty and lie wholly inside the region.
fn checked_offset(base: usize, bytes: usize, address: usize, size: usize) -> Result<usize> {
    let offset = address.checked_sub(base).ok_or(EINVAL)?;
    if size == 0 || offset.checked_add(size).ok_or(EINVAL)? > bytes {
        return Err(EINVAL);
    }
    Ok(offset)
}

/// `size` bytes at `offset` of the firmware's data segment, mapped write-back.
pub(super) struct DataSegmentLog {
    mem: io::mem::Mem,
    offset: usize,
    size: usize,
}

impl DataSegmentLog {
    /// Maps the `size` bytes at physical address `iova` that the firmware asked for, which
    /// must lie inside the `fw-data` region of `dev`.
    pub(super) fn map(dev: &device::Device, iova: usize, size: usize) -> Result<Self> {
        let node = dev.of_node().ok_or(EINVAL)?;
        let res = node.reserved_mem_region_to_resource_byname(c_str!("fw-data"))?;
        let offset = checked_offset(res.start().try_into()?, res.size().try_into()?, iova, size)?;
        // SAFETY: The boot loader reserved this RAM for the loaded GPU firmware, which keeps
        // it until reboot; only the requested subrange is exposed.
        let mem = unsafe { io::mem::Mem::try_new(res, io::mem::MemFlag::WB.into())? };
        Ok(Self { mem, offset, size })
    }

    /// The CPU view of the buffer.
    pub(super) fn buf(&mut self) -> Result<IoSysMap<'_, Region>> {
        // SAFETY: map() checked that the range lies inside the mapping.
        let base = unsafe { self.mem.ptr().add(self.offset) };
        let ptr = Region::<0>::ptr_try_from_raw_parts_mut(base, self.size)?;
        // SAFETY: The range is mapped RAM for as long as this borrow of the mapping.
        Ok(IoSysMap::Sys(unsafe { io::SysMem::new(ptr) }))
    }
}

#[kunit_tests(asahi_g17_crashlog_mapping)]
mod tests {
    use super::*;

    /// The T8140 `fw-data` region (both roles' data segments) and its 32 KiB crash logs.
    const BASE: usize = 0x100_01d7_c000;
    const BYTES: usize = 0x26_0000;
    const LOG: usize = 0x8000;

    #[test]
    fn both_role_logs_fit() {
        assert_eq!(checked_offset(BASE, BYTES, 0x100_01e1_c000, LOG), Ok(0xa_0000));
        assert_eq!(checked_offset(BASE, BYTES, 0x100_01f5_0000, LOG), Ok(0x1d_4000));
    }

    #[test]
    fn the_region_edges_are_inclusive() {
        assert_eq!(checked_offset(BASE, BYTES, BASE, LOG), Ok(0));
        assert_eq!(checked_offset(BASE, BYTES, BASE + BYTES - LOG, LOG), Ok(BYTES - LOG));
    }

    #[test]
    fn a_log_outside_or_across_the_region_is_refused() {
        assert_eq!(checked_offset(BASE, BYTES, BASE - LOG, LOG), Err(EINVAL));
        assert_eq!(checked_offset(BASE, BYTES, BASE + BYTES - LOG + 1, LOG), Err(EINVAL));
        assert_eq!(checked_offset(BASE, BYTES, BASE + BYTES, LOG), Err(EINVAL));
        // The low bits of the address, as an older driver mapped them
        assert_eq!(checked_offset(BASE, BYTES, 0x01e1_c000, LOG), Err(EINVAL));
    }

    #[test]
    fn overflow_and_empty_requests_are_refused() {
        assert_eq!(checked_offset(1, usize::MAX, usize::MAX - 1, LOG), Err(EINVAL));
        assert_eq!(checked_offset(BASE, BYTES, BASE, 0), Err(EINVAL));
    }
}
