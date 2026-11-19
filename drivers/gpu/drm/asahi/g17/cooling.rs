// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! GPU power-ceiling cooling device of the G17-family firmware builds.
//!
//! The policy (requesters, mapping, thermal callbacks) is the shared C owner in `cooling.c`;
//! this file resolves the firmware words of the running build and registers it when the
//! device tree makes the GPU a cooling device (`#cooling-cells`).

use super::{fw::variant::Variant, initdata::InitData};
use core::{ffi::c_void, ptr::NonNull};
use kernel::{c_str, device, prelude::*, sync::aref::ARef};

extern "C" {
    fn asahi_gpu_cooling_register(dev: *mut c_void, words: *const PowerWords) -> *mut c_void;
    fn asahi_gpu_cooling_unregister(cooling: *mut c_void);
    fn asahi_gpu_cooling_rearm(cooling: *mut c_void);
}

/// Where a firmware build takes the GPU's absolute power inputs.
#[derive(Clone, Copy)]
pub(crate) enum Layout {
    /// A18 Pro 26A428 and M4 Pro 25G83: [`RegionC::max_power_mw`] and the primary
    /// [`PowerState`]'s power-management budget.
    ///
    /// [`RegionC::max_power_mw`]: super::fw::initdata::RegionC::max_power_mw
    /// [`PowerState`]: super::fw::power::PowerState
    Common,
}

/// C ABI; these are independent typed members, never numeric bundle offsets.
#[repr(C)]
pub(super) struct PowerWords {
    pub(super) max_power_mw: *mut u32,
    pub(super) ppm_enabled: *mut u32,
    pub(super) ppm_target_mw: *mut u32,
}

fn required(dev: &device::Device) -> Result<bool> {
    let Some(node) = dev.of_node() else {
        return Ok(false);
    };
    match node.get_property::<u32>(c_str!("#cooling-cells")) {
        Ok(2) => Ok(true),
        Ok(_) => Err(EINVAL),
        Err(ENOENT) => Ok(false),
        Err(error) => Err(error),
    }
}

/// Check before starting either processor. The firmware keeps its own die limiter, so a build
/// whose power inputs are not described here ([`Variant::live_power`] is `None`) still runs; the
/// GPU just does not become a cooling device and its thermal zones get no GPU ceiling.
pub(super) fn admit(dev: &device::Device, variant: &Variant) -> Result {
    if required(dev)? && variant.live_power.is_none() {
        dev_warn!(dev, "GPU live power layout unknown for this firmware, no GPU cooling device\n");
    }
    Ok(())
}

/// Dropped (and callbacks drained) before the firmware allocations. No C
/// callback takes the GPU mutex; thermal changes reach even a long dispatch.
pub(super) struct Cooling {
    inner: NonNull<c_void>,
    _dev: ARef<device::Device>,
}

// SAFETY: C serializes all shared access with its per-device mutex; Drop has
// unique ownership and drains thermal and sysfs callbacks.
unsafe impl Send for Cooling {}
// SAFETY: As for Send. Rearming uses the same C mutex as the callbacks.
unsafe impl Sync for Cooling {}

impl Cooling {
    pub(super) fn attach(
        dev: &device::Device,
        variant: &Variant,
        init: &InitData,
    ) -> Result<Option<Self>> {
        if !required(dev)? {
            return Ok(None);
        }
        let Some(layout) = variant.live_power.as_ref() else {
            return Ok(None);
        };
        let words = init.cooling_words(layout)?;
        // SAFETY: The owner keeps all validated shared words mapped until after
        // Drop drains the C callbacks. Registration republishes the initialized
        // words (state 0) before INIT, port attachment or any work publication.
        let inner = kernel::error::from_err_ptr(unsafe {
            asahi_gpu_cooling_register(dev.as_raw().cast(), &words)
        })?;
        Ok(Some(Self {
            inner: NonNull::new(inner).ok_or(ENOMEM)?,
            _dev: dev.into(),
        }))
    }

    pub(super) fn rearm(&self) {
        // SAFETY: The C allocation and the power mappings are still owned.
        unsafe { asahi_gpu_cooling_rearm(self.inner.as_ptr()) };
    }
}

impl Drop for Cooling {
    fn drop(&mut self) {
        // SAFETY: Unique owner; drains callbacks before mappings are freed.
        unsafe { asahi_gpu_cooling_unregister(self.inner.as_ptr()) };
    }
}
