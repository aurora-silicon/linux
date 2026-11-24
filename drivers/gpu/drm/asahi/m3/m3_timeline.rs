// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M3 queue counters and modulo-32-bit firmware completion stamps.
//!
//! Firmware stamps advance by 0x100 and barriers compare their signed modular
//! difference, as in event::EventValue. Only a bounded batch is outstanding;
//! its distance is well below the half-range. Keep the host command ordinal
//! separately in u64 so stamp wrap never re-registers a queue or emits InitBM.

/// Wire stamp after `ordinal` commands, from the retired seed.
/// Skipping zero would advance by 0x200 and stop the firmware with
/// "DAG: Non Sequential Stamp Updates". Live queues use `StampEpoch` and
/// reseed while idle before reaching zero, rather than relying on zero stamps.
pub(crate) const fn stamp(seed: u32, ordinal: u64) -> u32 {
    seed.wrapping_add((ordinal as u32).wrapping_mul(0x100))
}

/// Leave ample room for a complete bounded batch before the earliest wrap,
/// as the G15 runtime does. Admission stops extending the pipeline at this
/// boundary; reseeding requires the runtime's final batch retirement checks.
const RESEED_MARGIN: u64 = 0x10_0000;

/// Wire-stamp epoch, independent of monotonic command/pool/notifier ordinals.
#[derive(Default)]
pub(crate) struct StampEpoch {
    origin: u64,
}

impl StampEpoch {
    pub(crate) fn value(&self, seed: u32, ordinal: u64) -> u32 {
        stamp(seed, ordinal - self.origin)
    }

    pub(crate) fn needs_reseed(&self, seed: u32, ordinal: u64) -> bool {
        ordinal - self.origin >= (1u64 << 24) - (seed >> 8) as u64 - RESEED_MARGIN
    }

    /// Caller has drained all active batches and written both stamp copies
    /// back to their seeds. No dependency from the old epoch may remain live.
    pub(crate) fn reseed(&mut self, retired: u64) {
        self.origin = retired;
    }
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
