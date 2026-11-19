// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M3 queue counters and modulo-32-bit firmware completion stamps.
//!
//! Firmware stamps advance by 0x100 and barriers compare their signed modular
//! difference, as in event::EventValue. Only a bounded batch is outstanding;
//! its distance is well below the half-range. Keep the host command ordinal
//! separately in u64 so stamp wrap never re-registers a queue or emits InitBM.

/// Nonzero wire stamp after `ordinal` commands, from the retired seed.
/// As on the M4 runtime, do not publish zero as a completion stamp. The
/// 24-bit stamp sequence skips zero; command ordinals/event counts do not.
pub(crate) const fn stamp(seed: u32, ordinal: u64) -> u32 {
    const PERIOD: u64 = 0x00ff_ffff;
    let unit = (((seed >> 8) as u64 - 1 + ordinal % PERIOD) % PERIOD) + 1;
    (unit as u32) << 8
}

/// Firmware command count, before the command with this host ordinal.
pub(crate) const fn previous(ordinal: u64) -> u32 {
    (ordinal as u32).wrapping_sub(1)
}

/// Notifier's modulo-32-bit event threshold (two stages for render).
pub(crate) const fn events(ordinal: u64, stages: u32) -> u32 {
    (ordinal as u32).wrapping_mul(stages)
}

/// Signed modular ordering, valid for the bounded set of outstanding draws.
pub(crate) const fn reached(observed: u32, expected: u32) -> bool {
    observed.wrapping_sub(expected) as i32 >= 0
}
