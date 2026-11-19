// SPDX-License-Identifier: GPL-2.0-only OR MIT

// Firmware validates Timestamp user destinations against a dedicated 64 MiB
// interval published in HwDataB+0x28. Reserve the last 64 MiB of the
// firmware VM selector, disjoint from MMIO/command allocations above.
pub(crate) const TIMESTAMP_RANGE: core::ops::Range<u64> =
    0xffff_fc2f_fc00_0000..0xffff_fc30_0000_0000;

pub(crate) fn publish() {
    // SAFETY: Orders preceding WC buffer stores before a device doorbell.
    unsafe { core::arch::asm!("dsb oshst", options(nostack, preserves_flags)) };
}
