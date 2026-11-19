// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The GFX and GFX1 ASCWrap v6 mailbox providers, as seen by the M5 firmware session.

use core::mem::MaybeUninit;

use kernel::{
    bindings,
    error::{from_err_ptr, to_result},
    io::{IoSysMap, Region},
    prelude::*,
    soc::apple::rtkit,
};

use crate::mmu;

const G17P_CRASHLOG_MAX_BYTES: usize = 256 * 1024;

const G17P_RTKIT_UAT_DVA_MASK: u64 = (1u64 << 40) - 1;

/// Module-local, read-only view of one existing ASCWrap mailbox provider.
///
/// The C provider owns the mapping and all lifecycle transitions. This view
/// neither maps an alias nor changes callback ownership; it only borrows the
/// provider's established mailbox-register pointer for two control reads.
pub(super) struct G17PMailboxProvider {
    mbox: *mut bindings::apple_mbox,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(super) enum G17PAscwrapV6Role {
    Gfx,
    Gfx1,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(super) struct G17PAscwrapV6Lifecycle {
    role: G17PAscwrapV6Role,
    wrapper: (u64, u64),
    iop_vbar: (u64, u64),
    missing: u32,
}

impl G17PMailboxProvider {

    pub(super) fn get(dev: &kernel::device::Device, index: usize) -> Result<Self> {
        // SAFETY: `dev` is live and apple_mbox_get() establishes a device link
        // to keep the resolved provider alive for this consumer.
        let mbox = unsafe {
            from_err_ptr(bindings::apple_mbox_get(
                dev.as_raw(),
                index.try_into()?,
            ))?
        };
        Ok(Self { mbox })
    }

    pub(super) fn lifecycle(&self) -> Result<G17PAscwrapV6Lifecycle> {
        let mut raw = MaybeUninit::<bindings::apple_mbox_ascwrap_v6_lifecycle>::uninit();
        // SAFETY: `self.mbox` is the live provider returned by
        // apple_mbox_get() and `raw` points to writable storage.
        to_result(unsafe {
            bindings::apple_mbox_ascwrap_v6_get_lifecycle(self.mbox, raw.as_mut_ptr())
        })?;
        // SAFETY: the successful C call initialized every field.
        let raw = unsafe { raw.assume_init() };
        let role = match raw.role as u32 {
            1 => G17PAscwrapV6Role::Gfx,
            2 => G17PAscwrapV6Role::Gfx1,
            _ => return Err(EINVAL),
        };
        Ok(G17PAscwrapV6Lifecycle {
            role,
            wrapper: (raw.wrapper_start, raw.wrapper_size),
            iop_vbar: (raw.iop_vbar_start, raw.iop_vbar_size),
            missing: raw.missing,
        })
    }

    pub(super) fn require_safe_cpu_lifecycle(&self) -> Result {
        // SAFETY: this is a capability check on the live provider pointer.
        to_result(unsafe {
            bindings::apple_mbox_ascwrap_v6_require_safe_cpu_lifecycle(self.mbox)
        })
    }

    pub(super) fn start_cpu(&self) -> Result {
        // SAFETY: lifecycle serialization and MMIO ownership remain in C.
        to_result(unsafe { bindings::apple_mbox_ascwrap_v6_start_cpu(self.mbox) })
    }

    pub(super) fn stop_cpu(&self) -> Result {
        // SAFETY: lifecycle serialization and MMIO ownership remain in C.
        to_result(unsafe { bindings::apple_mbox_ascwrap_v6_stop_cpu(self.mbox) })
    }

}

unsafe impl Send for G17PMailboxProvider {}

unsafe impl Sync for G17PMailboxProvider {}

pub(super) const fn g17p_rtkit_uat_address(dva: u64) -> u64 {
    dva & G17P_RTKIT_UAT_DVA_MASK
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u32)]
pub(super) enum G17PProcessor {
    Gfx = 0,
    Gfx1 = 1,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(super) struct G17PFirmwareCrashBuffer {
    dva: u64,
    size: usize,
}

impl G17PProcessor {

    pub(super) const fn firmware_crash_buffer(self) -> G17PFirmwareCrashBuffer {
        // Both endpoint-1 buffer requests are stable across the retained
        // G17P captures. Message 0x0010810001e1c000 decodes to the primary
        // DVA below and size 0x8000; GFX1 advertises the second DVA with the
        // same size.
        match self {
            Self::Gfx => G17PFirmwareCrashBuffer {
                dva: 0x100_01e1_c000,
                size: 0x8000,
            },
            Self::Gfx1 => G17PFirmwareCrashBuffer {
                dva: 0x100_01f5_0000,
                size: 0x8000,
            },
        }
    }
}

pub(super) const fn bounded_crashlog_size(size: usize) -> usize {
    if size < G17P_CRASHLOG_MAX_BYTES {
        size
    } else {
        G17P_CRASHLOG_MAX_BYTES
    }
}

const _: () = {
    assert!(bounded_crashlog_size(0) == 0);
    assert!(bounded_crashlog_size(4096) == 4096);
    assert!(bounded_crashlog_size(G17P_CRASHLOG_MAX_BYTES + 1) == G17P_CRASHLOG_MAX_BYTES);
    let gfx = G17PProcessor::Gfx.firmware_crash_buffer();
    let gfx1 = G17PProcessor::Gfx1.firmware_crash_buffer();
    assert!(gfx.dva == 0x100_01e1_c000 && gfx1.dva == 0x100_01f5_0000);
    assert!(g17p_rtkit_uat_address(gfx.dva) == 0x01e1_c000);
    assert!(g17p_rtkit_uat_address(gfx1.dva) == 0x01f5_0000);
    assert!(gfx.size == 0x8000 && gfx1.size == 0x8000);
    assert!(gfx.dva & (mmu::UAT_PGSZ as u64 - 1) == 0);
    assert!(gfx1.dva & (mmu::UAT_PGSZ as u64 - 1) == 0);
    assert!(gfx.dva + gfx.size as u64 <= 1u64 << 42);
    assert!(gfx1.dva + gfx1.size as u64 <= 1u64 << 42);
};

/// Active G17P boot does not ask Rust to back system-endpoint buffers. The
/// generic RTKit core uses its coherent-DMA fallback when these callbacks are
/// absent.
pub(super) struct G17PNoRtkitBuffer;

impl rtkit::Buffer for G17PNoRtkitBuffer {
    fn iova(&self) -> Result<usize> {
        Err(ENOTSUPP)
    }

    fn buf(&mut self) -> Result<IoSysMap<'_, Region>> {
        Err(ENOTSUPP)
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(super) struct ExpectedProvider {
    pub(super) role: G17PAscwrapV6Role,
    pub(super) wrapper: (u64, u64),
    pub(super) iop_vbar: (u64, u64),
}

pub(super) fn validate_provider(
    provider: &G17PMailboxProvider,
    expected: ExpectedProvider,
) -> Result<G17PAscwrapV6Lifecycle> {
    let lifecycle = provider.lifecycle()?;
    if lifecycle.role != expected.role
        || lifecycle.wrapper != expected.wrapper
        || lifecycle.iop_vbar != expected.iop_vbar
    {
        return Err(EINVAL);
    }

    Ok(lifecycle)
}
