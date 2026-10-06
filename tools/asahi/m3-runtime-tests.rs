// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Host tests for the production M3 admission, IO ownership and thermal policy.
#![allow(dead_code)]

#[path = "../../drivers/gpu/drm/asahi/t8122_admission.rs"]
mod t8122_admission;
#[path = "../../drivers/gpu/drm/asahi/agx_resources.rs"]
mod agx_resources;
#[path = "../../drivers/gpu/drm/asahi/m3_init_layout.rs"]
mod m3_init_layout;
#[path = "../../drivers/gpu/drm/asahi/m3_init_storage.rs"]
mod m3_init_storage;
#[path = "../../drivers/gpu/drm/asahi/m3_thermal_policy.rs"]
mod m3_thermal_policy;

// Firmware layout tests need only the image-record type, without device access.
mod m3_board {
    #[derive(Debug)]
    pub(crate) struct KnownImage;
}
use agx_resources as m3_resources;
#[path = "../../drivers/gpu/drm/asahi/m3_firmware.rs"]
mod m3_firmware;
