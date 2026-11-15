// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! Ownership of the hwrng-core registration.

use kernel::prelude::*;

pub(crate) type ReadFn =
    unsafe extern "C" fn(ctx: *mut c_void, data: *mut c_void, max: usize, wait: bool) -> c_int;

extern "C" {
    fn sep_hwrng_alloc() -> *mut c_void;
    fn sep_hwrng_free(mem: *mut c_void);
    fn sep_hwrng_register(
        mem: *mut c_void,
        name: *const c_char,
        quality: c_ushort,
        ctx: *mut c_void,
        read: Option<ReadFn>,
    ) -> c_int;
    fn sep_hwrng_unregister(mem: *mut c_void);
}

pub(crate) const QUALITY: c_ushort = 1024;

pub(crate) const NAME: &CStr = c"apple-sep";

pub(crate) struct HwRngHandle {
    mem: *mut c_void,
    registered: bool,
}

// SAFETY: `mem` is a plain heap allocation with no thread affinity; all access
// goes through the C shim, which locks inside the hwrng core.
unsafe impl Send for HwRngHandle {}

impl HwRngHandle {
    pub(crate) fn new() -> Result<Self> {
        // SAFETY: no preconditions; returns NULL on allocation failure.
        let mem = unsafe { sep_hwrng_alloc() };
        if mem.is_null() {
            return Err(ENOMEM);
        }
        Ok(HwRngHandle {
            mem,
            registered: false,
        })
    }

    /// # Safety
    /// `ctx` must stay valid and safe to pass to `read` until [`Self::unregister`]
    /// returns or this handle is dropped.
    pub(crate) unsafe fn register(&mut self, ctx: *mut c_void, read: ReadFn) -> Result<()> {
        if self.registered {
            return Err(EBUSY);
        }
        // SAFETY: `mem` is live, `NAME` is a static NUL-terminated string, and
        // the caller guarantees `ctx` outlives the registration.
        let ret =
            unsafe { sep_hwrng_register(self.mem, NAME.as_char_ptr(), QUALITY, ctx, Some(read)) };
        kernel::error::to_result(ret)?;
        self.registered = true;
        Ok(())
    }

    pub(crate) fn unregister(&mut self) {
        if self.registered {
            self.registered = false;
            // SAFETY: `mem` is live and was registered.
            unsafe { sep_hwrng_unregister(self.mem) };
        }
    }
}

impl Drop for HwRngHandle {
    fn drop(&mut self) {
        self.unregister();
        // SAFETY: `mem` is live and now unregistered, so nothing else refers to it.
        unsafe { sep_hwrng_free(self.mem) };
    }
}
