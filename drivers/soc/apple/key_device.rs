// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! `/dev/sep-key` as a per-open device: each file owns its state and a
//! reference to the driver; the key commands keep their existing ABI.

use crate::{key as keys, SepData};
use kernel::prelude::*;
use kernel::sync::atomic::{Acquire, Full, Relaxed, Release};
use kernel::sync::{Arc, ArcBorrow};
use kernel::types::ForeignOwnable;

struct File {
    sep: Arc<SepData>,
}

impl File {
    fn ioctl(&self, cmd: u32, arg: usize) -> Result {
        let _call = Call::enter(&self.sep)?;
        self.sep.key_ioctl(cmd, arg)
    }
}

/// A `/dev/sep-key` call in progress. A file outlives the device's
/// registration, so `remove` waits for every call to end
/// ([`wait_for_calls`]) before it takes the hardware down, and no call
/// starts once it has begun.
pub(crate) struct Call<'a>(&'a SepData);

impl<'a> Call<'a> {
    pub(crate) fn enter(sep: &'a SepData) -> Result<Self> {
        // Full: pairs with the barrier in `wait_for_calls`, so either this
        // sees `shutting_down` or that sees this call.
        sep.key_calls.fetch_add(1, Full);
        let call = Self(sep);
        if sep.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        Ok(call)
    }
}

impl Drop for Call<'_> {
    fn drop(&mut self) {
        self.0.key_calls.fetch_sub(1, Release);
    }
}

/// Waits for the calls in progress once `shutting_down` is set. Each sees
/// it and ends: a touch wait checks it, and `remove` wakes the enclave's
/// waits.
pub(crate) fn wait_for_calls(sep: &SepData) {
    kernel::sync::barrier::smp_mb(Full);
    while sep.key_calls.load(Acquire) != 0 {
        kernel::time::delay::fsleep(kernel::time::Delta::from_millis(10));
    }
}

extern "C" {
    fn sep_key_register(
        name: *const c_char,
        mode: u16,
        ctx: *mut c_void,
        open: unsafe extern "C" fn(*mut c_void, *mut *mut c_void) -> c_int,
        release: unsafe extern "C" fn(*mut c_void),
        ioctl: unsafe extern "C" fn(*mut c_void, c_uint, c_ulong) -> c_long,
    ) -> *mut c_void;
    fn sep_key_unregister(device: *mut c_void);
}

pub(crate) struct Registration(*mut c_void);
// SAFETY: registration has no thread affinity; the C shim serializes open/detach.
unsafe impl Send for Registration {}

impl Registration {
    /// # Safety
    /// The driver's original Arc must keep sep alive until this is dropped.
    pub(crate) unsafe fn new(sep: &SepData) -> Result<Self> {
        // SAFETY: the caller supplies the registered-window guarantee above.
        let device = unsafe {
            sep_key_register(
                keys::DEVICE_NAME.as_char_ptr(),
                keys::DEVICE_MODE,
                core::ptr::from_ref(sep).cast_mut().cast(),
                open,
                release,
                ioctl,
            )
        };
        if device.is_null() {
            return Err(ENODEV);
        }
        Ok(Self(device))
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        // SAFETY: one live registration, never used after this call.
        unsafe { sep_key_unregister(self.0) };
    }
}

/// `/dev/sep-key` open.
///
/// # Safety
/// The C shim holds its registration lock throughout. `ctx` is the pointer
/// of the driver's original `SepData` Arc, and `out` points to the file's
/// private-data slot.
unsafe extern "C" fn open(ctx: *mut c_void, out: *mut *mut c_void) -> c_int {
    // SAFETY: `ctx` came from that Arc, which outlives the registration.
    let sep = unsafe { ArcBorrow::<SepData>::from_raw(ctx.cast()) };
    if sep.shutting_down.load(Relaxed) {
        return ENODEV.to_errno();
    }
    let file = Arc::new(
        File { sep: sep.into() },
        kernel::alloc::flags::GFP_KERNEL_ACCOUNT,
    );
    match file {
        Ok(file) => {
            // SAFETY: out is writable; ownership moves to the matching release.
            unsafe { out.write(file.into_foreign()) };
            0
        }
        Err(_) => ENOMEM.to_errno(),
    }
}

/// `/dev/sep-key` release.
///
/// # Safety
/// C passes the pointer `open` stored, once, after every file callback has
/// returned.
unsafe extern "C" fn release(ctx: *mut c_void) {
    // SAFETY: the foreign Arc from `open`, given back exactly once.
    drop(unsafe { <Arc<File> as ForeignOwnable>::from_foreign(ctx) });
}

/// `/dev/sep-key` ioctl.
///
/// # Safety
/// `ctx` is the pointer `open` stored; the file reference the VFS holds for
/// this call keeps it from being released meanwhile.
unsafe extern "C" fn ioctl(ctx: *mut c_void, cmd: c_uint, arg: c_ulong) -> c_long {
    // SAFETY: live until `release`, which cannot run during this call.
    let file = unsafe { <Arc<File> as ForeignOwnable>::borrow(ctx) };
    match file.ioctl(cmd, arg) {
        Ok(()) => 0,
        Err(error) => error.to_errno() as c_long,
    }
}
