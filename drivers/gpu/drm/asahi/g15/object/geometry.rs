// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The DMA geometry a firmware- or GPU-visible backing must have.
//!
//! Core-only (no `kernel` imports): this file is compiled by the host test harness as well.

/// The page size and page-frame mask [`backing_ok`] is host-tested with (`hw::PAGE`,
/// `hw::PA_MASK`; `object.rs` asserts they are the same).
pub(crate) const PAGE: u64 = 0x4000;
pub(crate) const PA_MASK: u64 = 0x3ffffffc000;

/// Whether the DMA segments `(address, length)` back exactly `bytes` with whole pages of `page`
/// bytes that the UAT can name:
///
/// - `page` is a power of two and `bytes` a nonzero multiple of it;
/// - every segment is nonzero, starts and ends on a page boundary, does not start at 0 and its
///   last byte lies within `pa_mask` (the page-frame bits), with no overflow;
/// - the segments' lengths add up to `bytes`, no more and no less.
pub(crate) fn backing_ok(
    bytes: u64,
    page: u64,
    pa_mask: u64,
    segments: impl IntoIterator<Item = (u64, u64)>,
) -> bool {
    if !page.is_power_of_two() || bytes == 0 || bytes & (page - 1) != 0 {
        return false;
    }
    let mut covered = 0u64;
    for (pa, len) in segments {
        if pa == 0 || len == 0 || (pa | len) & (page - 1) != 0 {
            return false;
        }
        let Some(last) = pa.checked_add(len - 1) else {
            return false;
        };
        if last & !(pa_mask | (page - 1)) != 0 {
            return false;
        }
        match covered.checked_add(len) {
            Some(sum) if sum <= bytes => covered = sum,
            _ => return false,
        }
    }
    covered == bytes
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ok(bytes: u64, segments: &[(u64, u64)]) -> bool {
        backing_ok(bytes, PAGE, PA_MASK, segments.iter().copied())
    }

    #[test]
    fn exact_coverage() {
        assert!(ok(PAGE, &[(0x8_0000_0000, PAGE)]));
        assert!(ok(
            3 * PAGE,
            &[(0x8_0000_0000, PAGE), (0x9_0000_0000, 2 * PAGE)]
        ));
        // A short or an oversized scatterlist.
        assert!(!ok(3 * PAGE, &[(0x8_0000_0000, 2 * PAGE)]));
        assert!(!ok(
            2 * PAGE,
            &[(0x8_0000_0000, 2 * PAGE), (0x9_0000_0000, PAGE)]
        ));
        assert!(!ok(PAGE, &[]));
    }

    #[test]
    fn sizes() {
        assert!(!ok(0, &[]));
        assert!(!ok(PAGE + 1, &[(0x8_0000_0000, PAGE)]));
        assert!(!backing_ok(PAGE, 0, PA_MASK, [(0x8_0000_0000, PAGE)]));
        assert!(!backing_ok(
            0x6000,
            0x3000,
            PA_MASK,
            [(0x8_0000_0000, 0x6000)]
        ));
    }

    #[test]
    fn segments() {
        // Address 0, a zero-length segment, unaligned starts and lengths.
        assert!(!ok(PAGE, &[(0, PAGE)]));
        assert!(!ok(PAGE, &[(0x8_0000_0000, 0), (0x9_0000_0000, PAGE)]));
        assert!(!ok(PAGE, &[(0x8_0000_1000, PAGE)]));
        assert!(!ok(
            2 * PAGE,
            &[
                (0x8_0000_0000, PAGE + 0x1000),
                (0x9_0000_0000, PAGE - 0x1000)
            ]
        ));
    }

    #[test]
    fn physical_bounds() {
        // The last page the UAT names, and the one beyond it.
        let top = PA_MASK & !(PAGE - 1);
        assert!(ok(PAGE, &[(top, PAGE)]));
        assert!(!ok(PAGE, &[(top + PAGE, PAGE)]));
        assert!(!ok(2 * PAGE, &[(top, 2 * PAGE)]));
        // The end overflows instead of wrapping.
        assert!(!ok(2 * PAGE, &[(u64::MAX & !(PAGE - 1), 2 * PAGE)]));
        // With every bit nameable: a segment ending at the last byte is fine, one page further
        // overflows.
        let len = u64::MAX & !(PAGE - 1);
        assert!(backing_ok(len, PAGE, u64::MAX, [(PAGE, len)]));
        assert!(!backing_ok(len, PAGE, u64::MAX, [(2 * PAGE, len)]));
    }

    #[test]
    fn coverage_does_not_overflow() {
        // Two halves wrap the total to 0, and a third segment then makes it look exact.
        let bytes = u64::MAX & !(PAGE - 1);
        let half = 1u64 << 63;
        assert!(!backing_ok(
            bytes,
            PAGE,
            u64::MAX,
            [(PAGE, half), (PAGE, half), (PAGE, bytes)]
        ));
        assert!(!backing_ok(
            PAGE,
            PAGE,
            u64::MAX,
            [(PAGE, half), (PAGE, half)]
        ));
    }
}
