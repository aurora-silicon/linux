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
    /// The InitData objects.
    pub(crate) const fn initdata(self) -> &'static initdata::Layout {
        match self {
            FwAbi::V25G83 => &initdata::V25G83,
            FwAbi::V26B123 => &initdata::V26B123,
        }
    }

    /// The device-control opcodes the host sends.
    pub(crate) const fn devctl(self) -> &'static channels::devctl::Opcodes {
        match self {
            FwAbi::V25G83 => &channels::devctl::V25G83,
            FwAbi::V26B123 => &channels::devctl::V26B123,
        }
    }

    /// The queue-info header.
    pub(crate) const fn qinfo(self) -> &'static graph::qinfo::Layout {
        match self {
            FwAbi::V25G83 => &graph::qinfo::V25G83,
            FwAbi::V26B123 => &graph::qinfo::V26B123,
        }
    }

    /// The TA work.
    pub(crate) const fn ta(self) -> &'static work::ta::Layout {
        match self {
            FwAbi::V25G83 => &work::ta::V25G83,
            FwAbi::V26B123 => &work::ta::V26B123,
        }
    }

    /// The FS work.
    pub(crate) const fn fs(self) -> &'static work::fs::Layout {
        match self {
            FwAbi::V25G83 => &work::fs::V25G83,
            FwAbi::V26B123 => &work::fs::V26B123,
        }
    }

    /// The compute work.
    pub(crate) const fn compute(self) -> &'static work::compute::Layout {
        match self {
            FwAbi::V25G83 => &work::compute::V25G83,
            FwAbi::V26B123 => &work::compute::V26B123,
        }
    }
}
