// SPDX-License-Identifier: GPL-2.0-only OR MIT

use crate::g17::fw::initdata::RegisterWindow;

const MMIO_APERTURE: u64 = 0xffff_fc21_8000_0000;

pub(super) const fn window(
    slot: usize,
    phys: u64,
    offset: u64,
    size: u32,
    unk_18: u64,
    unk_20: u32,
) -> RegisterWindow {
    RegisterWindow {
        slot,
        phys,
        va: MMIO_APERTURE + offset,
        size,
        unk_18,
        unk_20,
        instances: 1,
        phys_stride: 0,
    }
}

pub(super) const fn instances(window: RegisterWindow, instances: u32, phys_stride: u64) -> RegisterWindow {
    RegisterWindow {
        instances,
        phys_stride,
        ..window
    }
}

/// Whether `windows` are laid out as the 25G83 build lays out its register map: in order from
/// `+0x20_0000`, the pages of each window's instances followed by a 16K guard page.
pub(super) const fn laid_out_25g83(windows: &[RegisterWindow]) -> bool {
    let mut next = MMIO_APERTURE + 0x20_0000;
    let mut index = 0;
    while index < windows.len() {
        let window = &windows[index];
        let page_offset = window.phys & 0x3fff;
        if window.va != next + page_offset {
            return false;
        }
        let pages = (page_offset + window.size as u64 + 0x3fff) & !0x3fff;
        next += pages * window.instances as u64 + 0x4000;
        index += 1;
    }
    true
}
