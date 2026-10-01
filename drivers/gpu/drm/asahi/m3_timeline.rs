// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M3 queue counters and modulo-32-bit firmware completion stamps.
//!
//! Firmware stamps advance by 0x100 and barriers compare their signed modular
//! difference, as in event::EventValue. Only a bounded batch is outstanding;
//! its distance is well below the half-range. Keep the host command ordinal
//! separately in u64 so stamp wrap never re-registers a queue or emits InitBM.

/// Wire stamp after `ordinal` commands, starting from the retired seed.
pub(crate) const fn stamp(seed: u32, ordinal: u64) -> u32 {
    seed.wrapping_add((ordinal as u32).wrapping_mul(0x100))
}

/// Firmware command count, before the command with this host ordinal.
pub(crate) const fn previous(ordinal: u64) -> u32 {
    (ordinal as u32).wrapping_sub(1)
}

/// Notifier's modulo-32-bit event threshold (two stages for render).
pub(crate) const fn events(ordinal: u64, stages: u32) -> u32 {
    (ordinal as u32).wrapping_mul(stages)
}
