// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 (T8152, Mac mini M6 / J873) backend.
//!
//! Probe, firmware start and DRM registration follow the J873 module 1134
//! path; the backend plugs into the shared driver through `gpu::Gpu` and
//! `queue::Queue`.

#[allow(dead_code)]
pub(crate) mod address_map;
#[allow(dead_code)]
pub(crate) mod boot;
pub(crate) mod buffer;
pub(crate) mod compat;
#[allow(dead_code)]
pub(crate) mod drm;
pub(crate) mod fixture;
pub(crate) mod fw;
#[allow(dead_code)]
pub(crate) mod initdata;
pub(crate) mod params;
pub(crate) mod queue;
pub(crate) mod runtime;
pub(crate) mod uapi;
#[allow(dead_code)]
pub(crate) mod debug;
#[allow(dead_code)]
pub(crate) mod hw;
#[allow(dead_code)]
pub(crate) mod identity;
#[allow(dead_code)]
pub(crate) mod probe;
#[allow(dead_code)]
pub(crate) mod regs;
