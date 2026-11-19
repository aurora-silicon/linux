// SPDX-License-Identifier: GPL-2.0-only OR MIT

/// Complete client CDM range and its USC address window.
#[derive(Clone, Copy)]
pub(crate) struct Control {
    pub base: u64,
    pub end: u64,
    pub usc_base: u64,
}
