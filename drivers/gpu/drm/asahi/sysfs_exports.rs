// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! FFI declarations for the C `sysfs.c` shim.
//!
//! The Rust driver keeps the firmware stats in [`crate::stats::StatsSnapshot`]
//! and uses these shims to register / unregister the sysfs file and to publish
//! the snapshot pointer for the C-side `show` callback.

use core::ffi::{c_int, c_ulonglong};

extern "C" {
    /// Pointer to the live snapshot, or 0 when no device has registered.
    /// Set by Rust (`crate::stats::SNAPSHOT_PTR`); read by C.
    static mut asahi_stats_snapshot_ptr: c_ulonglong;

    /// Register the `agx_stats` sysfs file on `dev`. Returns 0 on success.
    fn asahi_sysfs_register(dev: *mut kernel::bindings::device) -> c_int;

    /// Unregister the `agx_stats` sysfs file and clear the snapshot pointer.
    fn asahi_sysfs_unregister(dev: *mut kernel::bindings::device);
}

/// Register the sysfs file. Safe to call from `AsahiDriver::probe` after the
/// DRM device has been registered.
pub(crate) fn register(dev: *mut kernel::bindings::device) -> kernel::error::Result {
    let ret = unsafe { asahi_sysfs_register(dev) };
    if ret < 0 {
        Err(unsafe { kernel::error::Error::from_errno(ret) })
    } else {
        Ok(())
    }
}

/// Unregister the sysfs file. Safe to call from the device release path.
pub(crate) fn unregister(dev: *mut kernel::bindings::device) {
    unsafe { asahi_sysfs_unregister(dev) };
}
