// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Submission activity for the firmware's idle prediction.
//!
//! The firmware predicts whether the GPU will get work soon from a history of recurring time bins
//! (Region C +0x34 enables the prediction). It learns that history from counters the host bumps
//! as it submits: before each compute, tiling or fragment submission, the host increments the
//! byte of the current bin in the shared status/config block. The firmware publishes the bins'
//! period and count there and only compares each byte with its previous snapshot, so the counter
//! wraps and a lost increment between concurrent publishers is harmless. Without the counters
//! every bin looks idle, and the firmware powers the GPU off in the gaps between back-to-back
//! submissions. Only boards that set `M5Board::activity_counters` count (the M5 Ultra).

use core::sync::atomic::{fence, AtomicU32, AtomicU8, Ordering};

/// Offsets in the status/config block: the firmware-owned period (u64, 24 MHz ticks) and bin
/// count (u64), the sequence word that guards them (odd while the firmware rewrites them), and
/// the host's bin counters.
const PERIOD: usize = 0x73c;
const COUNT: usize = 0x744;
const SEQUENCE: usize = 0x74c;
const BINS: usize = 0x750;
/// Bins the counter array holds, and the width of one bin in 24 MHz ticks.
pub(super) const MAX_BINS: usize = 32;
const BIN_TICKS: u64 = 25_000;
/// The span [`Activity`] reads and writes, from [`PERIOD`] to the end of the counters.
pub(super) const SPAN: (usize, usize) = (PERIOD, BINS + MAX_BINS - PERIOD);
/// Reads of a geometry the firmware keeps rewriting before this submission goes uncounted.
const READ_TRIES: usize = 4;

/// The status/config block's activity fields, from [`PERIOD`] on.
pub(super) struct Activity {
    base: *mut u8,
}

// SAFETY: The view names memory owned by the firmware session, which outlives every user of the
// port, and only performs single-copy-atomic loads and stores of words and bytes.
unsafe impl Send for Activity {}
// SAFETY: As above.
unsafe impl Sync for Activity {}

impl Activity {
    /// The view of the [`SPAN`] bytes at `base`, which must be 4-byte aligned.
    pub(super) fn new(base: *mut u8) -> kernel::error::Result<Self> {
        if base.is_null() || base as usize % 4 != 0 {
            return Err(kernel::error::code::EINVAL);
        }
        Ok(Self { base })
    }

    fn word(&self, offset: usize) -> &AtomicU32 {
        // SAFETY: Every offset passed is 4-byte aligned inside SPAN, and `base` is 4-byte aligned
        // and valid for SPAN bytes for the view's life (`new`).
        unsafe { &*(self.base.add(offset - PERIOD) as *const AtomicU32) }
    }

    fn pair(&self, offset: usize) -> u64 {
        u64::from(self.word(offset).load(Ordering::Relaxed))
            | (u64::from(self.word(offset + 4).load(Ordering::Relaxed)) << 32)
    }

    /// The firmware's (period, bin count), read consistently with its sequence word.
    fn geometry(&self) -> Option<(u64, u64)> {
        for _ in 0..READ_TRIES {
            let before = self.word(SEQUENCE).load(Ordering::Acquire);
            let geometry = (self.pair(PERIOD), self.pair(COUNT));
            fence(Ordering::Acquire);
            if before & 1 == 0 && self.word(SEQUENCE).load(Ordering::Relaxed) == before {
                return Some(geometry);
            }
            core::hint::spin_loop();
        }
        None
    }

    /// Counts one submission in the bin of `now` (24 MHz ticks, the firmware's time base). Call
    /// it before the submission is published.
    pub(super) fn record(&self, now: u64) {
        let Some((period, count)) = self.geometry() else {
            return;
        };
        if period == 0 {
            return;
        }
        let bin = (now % period) / BIN_TICKS;
        if bin >= count.min(MAX_BINS as u64) {
            return;
        }
        // SAFETY: `bin` is below MAX_BINS, so the byte is inside SPAN (`new`).
        let counter = unsafe { &*(self.base.add(BINS - PERIOD + bin as usize) as *const AtomicU8) };
        counter.store(
            counter.load(Ordering::Relaxed).wrapping_add(1),
            Ordering::Relaxed,
        );
        // Visible before the publication that follows.
        fence(Ordering::Release);
    }
}
