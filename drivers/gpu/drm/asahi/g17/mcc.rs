// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T6041 memory-cache lifetime handoff.

use kernel::{device, prelude::*};

unsafe extern "C" {
    fn asahi_mcc_release_parameter_buffer(consumer: *mut core::ffi::c_void) -> core::ffi::c_int;
}

/// Releases parameter-buffer-write residency after the GPU owner proves that
/// no render can still write it. `false` means this GPU has no such provider.
pub(super) fn release_parameter_buffer(dev: &device::Device) -> Result<bool> {
    // SAFETY: `dev` stays live for the call. The provider owns and serializes
    // its mapped planes; this call transfers no pointer or MMIO ownership.
    let ret = unsafe { asahi_mcc_release_parameter_buffer(dev.as_raw().cast()) };
    match ret {
        0 => Ok(true),
        1 => Ok(false),
        error => Err(Error::from_errno(error)),
    }
}
