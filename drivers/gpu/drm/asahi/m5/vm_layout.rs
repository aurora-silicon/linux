// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Application VA contract shared with Mesa's agx_m5_va.h.

use core::ops::Range;

pub(crate) const DRIVER_BASE: u64 = 0x10_0000_0000;
pub(crate) const COMPACT_END: u64 = DRIVER_BASE + (1u64 << 32);
pub(crate) const DATA_START: u64 = 0x40_0000_0000;

/// TA deflake fields encode an unsigned 32-bit offset from DRIVER_BASE.
/// Keep the private render arena inside that aperture and the USC backing
/// after it, within the same VM_CREATE reservation. A client may leave a
/// prefix of the compact aperture for its own command data. Mesa's default
/// reservation still begins at DRIVER_BASE and data begins at DATA_START.
pub(crate) fn compact_range(reserved: &Range<u64>) -> Option<Range<u64>> {
    if reserved.start < DRIVER_BASE || reserved.start >= COMPACT_END
        || reserved.start & 0x3fff != 0 || reserved.end <= COMPACT_END
        || reserved.end > DATA_START || reserved.end & 0x3fff != 0 {
        return None;
    }
    Some(reserved.start..COMPACT_END)
}

pub(crate) fn auxiliary_range(reserved: &Range<u64>) -> Option<Range<u64>> {
    compact_range(reserved)?;
    Some(COMPACT_END..reserved.end)
}

/// Command pointers must not reach driver-owned mappings, including when
/// the client leaves application data below its private arena reservation.
pub(crate) fn user_range_contains(
    reserved: &Range<u64>, address: u64, size: u64, user_end: u64,
) -> bool {
    let Some(end) = address.checked_add(size) else { return false; };
    size != 0 && address >= DRIVER_BASE && end <= user_end
        && (end <= reserved.start || address >= reserved.end)
}

