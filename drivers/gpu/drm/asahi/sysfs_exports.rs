// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-device sysfs registration. The caller retains the snapshot Arc until
//! unregister has drained active readers and freed the C attribute wrapper.

use core::ffi::{c_int, c_void};
use crate::stats::StatsSnapshot;

extern "C" {
    fn asahi_sysfs_register(
        dev: *mut c_void,
        snapshot: *const StatsSnapshot,
        export_enabled: c_int,
        handle: *mut *mut c_void,
    ) -> c_int;
    fn asahi_sysfs_unregister(dev: *mut c_void, handle: *mut c_void);
}

/// Register only after initialization succeeds, keeping the snapshot Arc alive.
pub(crate) fn register(
    dev: *mut kernel::bindings::device,
    snapshot: *const StatsSnapshot,
    export_enabled: bool,
) -> kernel::error::Result<*mut c_void> {
    let mut handle = core::ptr::null_mut();
    let ret = unsafe { asahi_sysfs_register(dev.cast(), snapshot, export_enabled as c_int, &mut handle) };
    if ret < 0 {
        Err(kernel::error::Error::from_errno(ret))
    } else {
        Ok(handle)
    }
}

/// Drain readers before the caller drops its snapshot Arc.
pub(crate) fn unregister(dev: *mut kernel::bindings::device, handle: *mut c_void) {
    unsafe { asahi_sysfs_unregister(dev.cast(), handle) };
}
