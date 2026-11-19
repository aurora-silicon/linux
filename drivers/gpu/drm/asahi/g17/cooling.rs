// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Common G16S/G17P absolute-power ceiling owner.

use super::{fw::variant::Variant, initdata::InitData};
use core::{ffi::c_void, ptr::NonNull};
use kernel::{c_str, device, prelude::*, sync::aref::ARef};

extern "C" {
    fn asahi_gpu_cooling_register(dev: *mut c_void, words: *const PowerWords) -> *mut c_void;
    fn asahi_gpu_cooling_unregister(cooling: *mut c_void);
    fn asahi_gpu_cooling_rearm(cooling: *mut c_void);
}

/// Exact RegionC/PowerState pairing admitted for live publication.
#[derive(Clone, Copy)]
pub(crate) enum Layout {
    /// G17P 26A428 and G16S 25G83, RegionC::max_power_mw at +0x84.
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

/// The live power layout of the selected firmware. [`Layout::Common`] names objects of the
/// G17P and G16S 25G83 graphs; a packed (M5-profile) graph places RegionC and its power state
/// elsewhere, so it has no common layout whatever its variant inherits.
fn live_layout(variant: &Variant, packed_graph: bool) -> Option<Layout> {
    match packed_graph {
        true => None,
        false => variant.live_power,
    }
}

/// Check before starting either processor. The cooling device is optional: when the board asks
/// for one but the firmware layout describes no live power state, say so once and start the
/// GPU without it. The firmware die limiter and the board's other thermal zones still apply.
pub(super) fn admit(dev: &device::Device, variant: &Variant, packed_graph: bool) -> Result {
    if required(dev)? && live_layout(variant, packed_graph).is_none() {
        dev_info!(
            dev,
            "GPU firmware layout has no live power state: no GPU cooling device\n"
        );
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
        // Only the unpacked graphs boot through here; admit() reported a missing layout.
        let Some(layout) = variant.live_power else {
            return Ok(None);
        };
        let Some(words) = init.cooling_words(&layout)? else {
            dev_warn!(
                dev,
                "GPU initialization data has no power state: no GPU cooling device\n"
            );
            return Ok(None);
        };
        // SAFETY: The owner keeps all validated shared words mapped until after
        // Drop drains the C callbacks. Registration publishes the provisional
        // 25% floor before INIT, port attachment or any work publication.
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
