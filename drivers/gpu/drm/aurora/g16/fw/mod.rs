// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(
    dead_code,
    reason = "Firmware layout catalog also describes fields used by host wire regressions."
)]

//! M4 firmware 25G83 structures.
//!
//! Layouts of the records, work images and objects the host shares with the qualified 25G83 firmware,
//! in the style of `g17/fw`: `#[repr(C)]` structures with `static_assert!` offsets where the
//! layout is a fixed record, and offset constants where the driver writes sparse words into a
//! larger image. Nothing here allocates, locks or publishes.

pub(crate) mod buffer;
pub(crate) mod channels;
pub(crate) mod graph;
pub(crate) mod initdata;
pub(crate) mod work;
