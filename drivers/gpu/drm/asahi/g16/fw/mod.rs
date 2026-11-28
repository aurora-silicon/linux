// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(
    dead_code,
    reason = "Firmware layout catalog also describes fields used by host wire regressions."
)]

//! M4 firmware 25G83 and 26B123 structures.
//!
//! Layouts of the records, work images and objects the host shares with the qualified 25G83 firmware,
//! in the style of `g17/fw`: `#[repr(C)]` structures with `static_assert!` offsets where the
//! layout is a fixed record, and offset constants where the driver writes sparse words into a
//! larger image. Nothing here allocates, locks or publishes.
//!
//! What differs in another firmware ABI is held per file as `V25G83` / `V26B123` constants;
//! [`FwAbi`] selects them.

pub(crate) mod buffer;
pub(crate) mod channels;
pub(crate) mod graph;
pub(crate) mod initdata;
pub(crate) mod work;

use super::hw::FwAbi;

impl FwAbi {
    /// The device-control opcodes the host sends.
    pub(crate) const fn devctl(self) -> &'static channels::devctl::Opcodes {
        match self {
            FwAbi::V25G83 => &channels::devctl::V25G83,
            FwAbi::V26B123 | FwAbi::V26B123G15S => &channels::devctl::V26B123,
        }
    }

    /// The parameter-buffer manager layout.
    pub(crate) const fn pb_manager(self) -> &'static buffer::pb_manager::Layout {
        match self {
            FwAbi::V25G83 | FwAbi::V26B123 => &buffer::pb_manager::V25G83,
            FwAbi::V26B123G15S => &buffer::pb_manager::V26B123_G15S,
        }
    }

    /// The queue-info header.
    pub(crate) const fn qinfo(self) -> &'static graph::qinfo::Layout {
        match self {
            FwAbi::V25G83 => &graph::qinfo::V25G83,
            FwAbi::V26B123 => &graph::qinfo::V26B123,
            FwAbi::V26B123G15S => &graph::qinfo::V26B123_G15S,
        }
    }

    /// The TA work.
    pub(crate) const fn ta(self) -> &'static work::ta::Layout {
        match self {
            FwAbi::V25G83 => &work::ta::V25G83,
            FwAbi::V26B123 => &work::ta::V26B123,
            FwAbi::V26B123G15S => &work::ta::V26B123_G15S,
        }
    }

    /// The FS work.
    pub(crate) const fn fs(self) -> &'static work::fs::Layout {
        match self {
            FwAbi::V25G83 => &work::fs::V25G83,
            FwAbi::V26B123 => &work::fs::V26B123,
            FwAbi::V26B123G15S => &work::fs::V26B123_G15S,
        }
    }

    /// The compute work.
    pub(crate) const fn compute(self) -> &'static work::compute::Layout {
        match self {
            FwAbi::V25G83 => &work::compute::V25G83,
            FwAbi::V26B123 => &work::compute::V26B123,
            FwAbi::V26B123G15S => &work::compute::V26B123_G15S,
        }
    }
}
