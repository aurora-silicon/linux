// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! FFI declarations for the C `sysfs.c` shim.
//!
//! The Rust driver keeps the firmware stats in [`crate::stats::StatsSnapshot`]
//! and uses these shims to register / unregister the sysfs file and to publish
//! the snapshot pointer for the C-side `show` callback.

use core::ffi::{c_int, c_ulonglong};

extern "C" {
    /// Register the `agx_stats` sysfs file on `dev`; `export_enabled` 0
    /// makes the file print `unsupported`. Returns 0 on success.
    fn asahi_sysfs_register(dev: *mut kernel::bindings::device, export_enabled: c_int) -> c_int;

    /// Unregister the `agx_stats` sysfs file and clear the snapshot pointer.
    fn asahi_sysfs_unregister(dev: *mut kernel::bindings::device);

    /// Publish the snapshot pointer for the C-side `show` callback (the C
    /// static `asahi_stats_snapshot_ptr` is the single storage location).
    fn asahi_stats_set_snapshot_ptr(p: c_ulonglong);
}

/// Register the sysfs file. Safe to call from `AsahiDriver::probe` after the
/// DRM device has been registered.
pub(crate) fn register(
    dev: *mut kernel::bindings::device,
    export_enabled: bool,
) -> kernel::error::Result {
    let ret = unsafe { asahi_sysfs_register(dev, export_enabled as c_int) };
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

/// Publish the address of the live `StatsSnapshot` to the C shim.
pub(crate) fn set_snapshot_ptr(ptr: *const crate::stats::StatsSnapshot) {
    unsafe { asahi_stats_set_snapshot_ptr(ptr as c_ulonglong) };
}
