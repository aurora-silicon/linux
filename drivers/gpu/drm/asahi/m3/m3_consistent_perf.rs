// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Opt-in T6030 performance requests. Firmware thermal ceilings stay independent.

pub(crate) fn request(chip: u32, level: u64, busy: bool, top: u32) -> Option<u32> {
    if chip != 0x6030 || top != 8 { return None; }
    match level {
        0 => Some(0),
        3 => Some(if busy { top * 100 } else { 0 }),
        _ => None,
    }
}

pub(crate) const REFRESH_INTERVAL_NS: i64 = 1_000_000;

/// Transitions apply immediately. While busy, recheck the firmware-owned word
/// at most once per millisecond; an inactive request does no repeated accesses.
pub(crate) fn refresh_due(desired: u32, previous: u32, since_check_ns: Option<i64>) -> bool {
    desired != previous || (desired != 0 &&
        since_check_ns.is_none_or(|ns| ns >= REFRESH_INTERVAL_NS))
}

/// Write a packed word with byte accesses, clearing it before raising it so
/// intermediate values never exceed either the old or new complete request.
///
/// # Safety
///
/// The caller must retain four writable bytes for these accesses, serialize
/// host writes, and publish the request after these stores.
pub(crate) unsafe fn store_packed(ptr: *mut u8, value: u32) {
    for i in (0..4).rev() {
        // SAFETY: caller supplies four writable bytes retained for this access.
        unsafe { ptr.add(i).write_volatile(0); }
    }
    for (i, byte) in value.to_le_bytes().iter().enumerate() {
        // SAFETY: the same bounded packed span, with no unaligned word store.
        unsafe { ptr.add(i).write_volatile(*byte); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn request_lifecycle_and_other_profiles() {
        assert_eq!(request(0x6030, 0, true, 8), Some(0));
        assert_eq!(request(0x6030, 3, true, 8), Some(800));
        assert_eq!(request(0x6030, 3, false, 8), Some(0));
        for chip in [0x8103, 0x8112, 0x8122, 0x6031] {
            assert_eq!(request(chip, 3, true, 8), None);
        }
        assert_eq!(request(0x6030, 3, true, 7), None);
        assert_eq!(request(0x6030, 2, true, 8), None);
    }
    #[test]
    fn packed_request_preserves_neighbors_and_clears() {
        let mut bytes = [0x5a; 8];
        // SAFETY: four bytes beginning at offset 1 fit the live array.
        unsafe { store_packed(bytes.as_mut_ptr().add(1), 800); }
        assert_eq!(bytes, [0x5a, 0x20, 3, 0, 0, 0x5a, 0x5a, 0x5a]);
        unsafe { store_packed(bytes.as_mut_ptr().add(1), 0); }
        assert_eq!(bytes, [0x5a, 0, 0, 0, 0, 0x5a, 0x5a, 0x5a]);
    }
    #[test]
    fn busy_refresh_is_bounded_and_zero_transitions_are_immediate() {
        assert!(!refresh_due(0,0,None));
        assert!(!refresh_due(0,0,Some(10_000_000)));
        assert!(refresh_due(800,0,Some(0)));
        assert!(refresh_due(0,800,Some(0)));
        assert!(refresh_due(800,800,None));
        for elapsed in [-1,0,999_999] {assert!(!refresh_due(800,800,Some(elapsed)));}
        for elapsed in [1_000_000,1_000_001,10_000_000] {
            assert!(refresh_due(800,800,Some(elapsed)));
        }
    }
}
