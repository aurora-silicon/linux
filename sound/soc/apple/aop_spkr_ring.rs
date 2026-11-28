// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Pure ring geometry and cursor arithmetic for the AOP `spkr` TX and sense rings.
//!
//! Kept free of kernel and MMIO dependencies so the same code builds as both an in-kernel
//! KUnit suite and a host `rustc --test` binary.

#[cfg(not(test))]
use kernel::prelude::kunit_tests;

pub(crate) const SPKR_CHANNELS: u32 = 6;
pub(crate) const SENSE_CHANNELS: u32 = 12;
/// One ADMAC frame (FRAME_4_WORDS): streams need whole bursts per period.
pub(crate) const ADMAC_BURST_BYTES: u32 = 16;

pub(crate) const NSEC_PER_SEC: u64 = 1_000_000_000;

pub(crate) const SENSE_FRAME_BYTES: usize = SENSE_CHANNELS as usize * 2;
/// 2.5 ms periods (120 frames at 48 kHz), 32 of them: the same 80 ms ring, delivered in chunks
/// small enough that a 512-frame front-end period completes within 2.5 ms of its last frame.
/// Whole frames, so ring offsets on period edges stay on frames.
pub(crate) const SENSE_RING_PERIOD_BYTES: usize = 120 * SENSE_FRAME_BYTES;
pub(crate) const SENSE_RING_BYTES: usize = 32 * SENSE_RING_PERIOD_BYTES;
/// The sense rate (see the front-ends' `rate_min`/`rate_max`).
pub(crate) const SENSE_RATE: u64 = 48_000;
/// The ring's and one ring period's length in time.
pub(crate) const SENSE_RING_NS: u64 =
    (SENSE_RING_BYTES / SENSE_FRAME_BYTES) as u64 * NSEC_PER_SEC / SENSE_RATE;
pub(crate) const SENSE_RING_PERIOD_NS: u64 =
    (SENSE_RING_PERIOD_BYTES / SENSE_FRAME_BYTES) as u64 * NSEC_PER_SEC / SENSE_RATE;

/// Six 32-bit `S24_LE` playback slots per frame.
pub(crate) const TX_FRAME_BYTES: usize = SPKR_CHANNELS as usize * 4;
/// 1.0 ms periods (48 frames at 48 kHz = 1152 bytes = 72 x 16-byte ADMAC bursts), 32 of them.
pub(crate) const TX_PERIOD_FRAMES: usize = 48;
pub(crate) const TX_RING_PERIODS: usize = 32;
pub(crate) const TX_RING_FRAMES: usize = TX_RING_PERIODS * TX_PERIOD_FRAMES;
pub(crate) const TX_RING_PERIOD_BYTES: usize = TX_PERIOD_FRAMES * TX_FRAME_BYTES;
pub(crate) const TX_RING_BYTES: usize = TX_RING_PERIODS * TX_RING_PERIOD_BYTES;
pub(crate) const TX_RATE: u64 = 48_000;
pub(crate) const TX_RING_NS: u64 = TX_RING_FRAMES as u64 * NSEC_PER_SEC / TX_RATE;
pub(crate) const TX_RING_PERIOD_NS: u64 = TX_PERIOD_FRAMES as u64 * NSEC_PER_SEC / TX_RATE;
/// 16 frames (384 B = 0.33 ms): 2x ADMAC's 192 B TX FIFO limit (`0x30 * 4`).
pub(crate) const TX_GUARD_FRAMES: usize = 16;
/// 576 frames (12.0 ms): target frames queued ahead of `ring_hw` (including `TX_GUARD_FRAMES`).
pub(crate) const TX_LEAD_FRAMES: usize = 576;
/// 8 frames (192 B): ADMAC TX hardware FIFO limit.
pub(crate) const ADMAC_TX_FIFO_FRAMES: isize = 8;
/// Maximum age of `TxRing::done_at` before `is_healthy()` considers the channel stalled
/// (20 periods = 20 ms: above scheduling jitter and well below one 32 ms ring lap).
pub(crate) const TX_HEALTHY_STALE_NS: u64 = 20 * TX_RING_PERIOD_NS;

const _: () = {
    assert!(TX_RING_PERIOD_BYTES % ADMAC_BURST_BYTES as usize == 0);
    assert!(TX_RING_PERIOD_BYTES % TX_FRAME_BYTES == 0);
    assert!(TX_GUARD_FRAMES < TX_LEAD_FRAMES);
    assert!(TX_LEAD_FRAMES * 2 < TX_RING_FRAMES);
    assert!(TX_HEALTHY_STALE_NS < TX_RING_NS - TX_RING_PERIOD_NS);
};

/// The sense ring may have gone round once or more in `elapsed_ns` since the last callback: its
/// position alone cannot tell a lap from no advance, or two laps from one. The DMA runs at the
/// sense rate and its reported position lags it by less than a ring period, so a lap needs at
/// least the ring's time less a period.
pub(crate) fn sense_ring_may_lap(elapsed_ns: i64) -> bool {
    elapsed_ns < 0 || elapsed_ns as u64 + SENSE_RING_PERIOD_NS >= SENSE_RING_NS
}

/// The TX ring may have lapped once or more in `elapsed_ns` since the last callback.
pub(crate) fn tx_ring_may_lap(elapsed_ns: i64) -> bool {
    elapsed_ns < 0 || elapsed_ns as u64 + TX_RING_PERIOD_NS >= TX_RING_NS
}

/// Whether `TxRing::done_at` is too old for the channel to be considered healthy.
pub(crate) fn tx_ring_is_stale(elapsed_ns: i64) -> bool {
    elapsed_ns < 0 || elapsed_ns as u64 > TX_HEALTHY_STALE_NS
}

/// Overflow-safe `(a + b) % m` for `m > 0`.
pub(crate) fn add_mod(a: usize, b: usize, m: usize) -> usize {
    let a = a % m;
    let b = b % m;
    if b >= m - a {
        b - (m - a)
    } else {
        a + b
    }
}

/// Forward distance in frames from `from` to `to` modulo `boundary`; 0 if `to` is behind `from`.
pub(crate) fn frames_ahead(from: usize, to: usize, boundary: usize) -> usize {
    if boundary == 0 {
        return 0;
    }
    let from = from % boundary;
    let to = to % boundary;
    let diff = if to >= from {
        to - from
    } else {
        to + (boundary - from)
    };
    if diff > boundary / 2 {
        0
    } else {
        diff
    }
}

/// Playback cursors of `TxRing` across one period-advance step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct TxStep {
    pub(crate) ring_hw: usize,
    pub(crate) ring_wr: usize,
    pub(crate) guard_left: usize,
    pub(crate) fe_played: usize,
    pub(crate) fe_copied: usize,
    pub(crate) fe_total_played: usize,
}

/// Result of advancing `TxRing` to `dma_now`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct TxAdvance {
    pub(crate) step: TxStep,
    pub(crate) advanced: usize,
    /// Number of previously non-zero frames starting at the old `ring_hw` that must be zeroed.
    pub(crate) zero_count: usize,
}

/// Advances the `TxRing` cursors from `s.ring_hw` to `dma_now`, preserving both the clean-ring
/// invariant and `queued == guard_left + frames_ahead(fe_played, fe_copied, boundary)`.
///
/// Treats backward-wrap glitches (`delta >= TX_RING_FRAMES - TX_PERIOD_FRAMES`) as zero advance
/// (matching `tx_ring_live_progress`, since genuine delays that long are caught by
/// `tx_ring_may_lap`).
///
/// When DMA overtakes `ring_wr` (underrun, end of drain, or a delayed callback), `fe_copied`
/// resynchronizes to `fe_played` and `ring_wr` collapses to `dma_now` with `guard_left = 0` so
/// playback time keeps advancing at exact `TX_RATE` until fresh frames arrive (at which point
/// refill re-inserts `TX_GUARD_FRAMES` ahead of `dma_now`).
pub(crate) fn tx_ring_advance(mut s: TxStep, dma_now: usize, boundary: usize) -> TxAdvance {
    let advanced = (dma_now + TX_RING_FRAMES - s.ring_hw) % TX_RING_FRAMES;
    if advanced == 0 || advanced >= TX_RING_FRAMES - TX_PERIOD_FRAMES {
        return TxAdvance {
            step: s,
            advanced: 0,
            zero_count: 0,
        };
    }
    let prev_queued = (s.ring_wr + TX_RING_FRAMES - s.ring_hw) % TX_RING_FRAMES;
    let zero_count = advanced.min(prev_queued);
    let from_guard = advanced.min(s.guard_left);
    s.guard_left -= from_guard;
    let audio_adv = advanced - from_guard;
    if boundary > 0 {
        s.fe_played = add_mod(s.fe_played, audio_adv, boundary);
    }
    s.fe_total_played = s.fe_total_played.wrapping_add(audio_adv);
    s.ring_hw = dma_now;
    if advanced > prev_queued {
        s.ring_wr = dma_now;
        s.guard_left = 0;
        s.fe_copied = s.fe_played;
    }
    TxAdvance {
        step: s,
        advanced,
        zero_count,
    }
}

/// Live `(audio_extra, remaining_guard)` for `pcm_pointer()` from `hw`, `dma_now`, and
/// `guard_left`. Clamps a small backward glitch (`delta >= TX_RING_FRAMES - TX_PERIOD_FRAMES`)
/// to zero progress so the reported pointer never jumps forward by nearly a full ring.
pub(crate) fn tx_ring_live_progress(
    hw: usize,
    dma_now: usize,
    guard_left: usize,
) -> (usize, usize) {
    let delta = (dma_now + TX_RING_FRAMES - hw) % TX_RING_FRAMES;
    if delta < TX_RING_FRAMES - TX_PERIOD_FRAMES {
        (
            delta.saturating_sub(guard_left),
            guard_left.saturating_sub(delta),
        )
    } else {
        (0, guard_left)
    }
}

/// Two-phase zero plan for `TxRing::silence_locked`: zero frames strictly ahead of ADMAC's
/// in-flight burst (`dma_now + 2`) first so DMA meets silence on a whole-frame boundary, then
/// zero the remaining queued prefix `[hw, ...)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct SilencePlan {
    pub(crate) ahead_start: usize,
    pub(crate) ahead_count: usize,
    pub(crate) prefix_start: usize,
    pub(crate) prefix_count: usize,
}

pub(crate) fn tx_ring_silence_plan(hw: usize, wr: usize, dma_now: usize) -> SilencePlan {
    let queued = (wr + TX_RING_FRAMES - hw) % TX_RING_FRAMES;
    if queued == 0 {
        return SilencePlan {
            ahead_start: 0,
            ahead_count: 0,
            prefix_start: 0,
            prefix_count: 0,
        };
    }
    let past = (dma_now + TX_RING_FRAMES - hw) % TX_RING_FRAMES;
    let ahead = queued.saturating_sub(past);
    let (ahead_start, ahead_count) = if ahead > 2 {
        ((dma_now + 2) % TX_RING_FRAMES, ahead - 2)
    } else {
        (0, 0)
    };
    let prefix_count = past.min(queued) + ahead.min(2);
    SilencePlan {
        ahead_start,
        ahead_count,
        prefix_start: hw % TX_RING_FRAMES,
        prefix_count,
    }
}

#[cfg_attr(not(test), kunit_tests(apple_aop_sense_ring))]
#[cfg_attr(test, allow(dead_code))]
mod tests {
    use super::*;

    const fn frames_ns(frames: u64) -> i64 {
        (frames * NSEC_PER_SEC / SENSE_RATE) as i64
    }

    const fn tx_frames_ns(frames: u64) -> i64 {
        (frames * NSEC_PER_SEC / TX_RATE) as i64
    }

    const RING_FRAMES: u64 = (SENSE_RING_BYTES / SENSE_FRAME_BYTES) as u64;
    const PERIOD_FRAMES: u64 = (SENSE_RING_PERIOD_BYTES / SENSE_FRAME_BYTES) as u64;

    fn queued(s: &TxStep) -> usize {
        (s.ring_wr + TX_RING_FRAMES - s.ring_hw) % TX_RING_FRAMES
    }

    fn assert_conservation(s: &TxStep, boundary: usize) {
        assert_eq!(
            queued(s),
            s.guard_left + frames_ahead(s.fe_played, s.fe_copied, boundary)
        );
    }

    /// Simulates `TxRing::refill_locked(..., at_dma_now = true)` on a pure `TxStep`.
    fn simulate_refill(s: &mut TxStep, appl_ptr: usize, buf_frames: usize, boundary: usize) {
        let mut q = queued(s);
        let ready = frames_ahead(s.fe_copied, appl_ptr, boundary).min(buf_frames);
        if q == 0 {
            if ready == 0 {
                return;
            }
            s.ring_wr = (s.ring_hw + TX_GUARD_FRAMES) % TX_RING_FRAMES;
            s.guard_left = TX_GUARD_FRAMES;
            q = TX_GUARD_FRAMES;
        }
        let copy = TX_LEAD_FRAMES.saturating_sub(q).min(ready);
        s.ring_wr = (s.ring_wr + copy) % TX_RING_FRAMES;
        s.fe_copied = add_mod(s.fe_copied, copy, boundary);
    }

    #[test]
    fn the_usual_cadence_is_no_lap() {
        assert!(!sense_ring_may_lap(0));
        assert!(!sense_ring_may_lap(frames_ns(PERIOD_FRAMES)));
        // A tasklet tens of milliseconds late still reads every frame.
        assert!(!sense_ring_may_lap(frames_ns(RING_FRAMES / 2)));
        assert!(!sense_ring_may_lap(
            frames_ns(RING_FRAMES - PERIOD_FRAMES) - 1
        ));
    }

    #[test]
    fn a_lap_or_more_is_caught() {
        assert!(sense_ring_may_lap(frames_ns(RING_FRAMES - PERIOD_FRAMES)));
        // Exactly one lap: the position has not moved.
        assert!(sense_ring_may_lap(frames_ns(RING_FRAMES)));
        // Two and a bit: the position shows only the bit.
        assert!(sense_ring_may_lap(frames_ns(
            2 * RING_FRAMES + PERIOD_FRAMES
        )));
        // A clock that went back: assume the worst.
        assert!(sense_ring_may_lap(-1));
    }

    #[test]
    fn the_ring_is_80_ms_of_2_5_ms_periods() {
        assert_eq!(SENSE_RING_NS, 80_000_000);
        assert_eq!(SENSE_RING_PERIOD_NS, 2_500_000);
    }

    #[test]
    fn tx_ring_geometry_and_alignment() {
        assert_eq!(TX_FRAME_BYTES, 24);
        assert_eq!(TX_RING_FRAMES, 1_536);
        assert_eq!(TX_RING_PERIOD_BYTES, 1_152);
        assert_eq!(TX_RING_BYTES, 36_864);
        assert_eq!(TX_RING_NS, 32_000_000);
        assert_eq!(TX_RING_PERIOD_NS, 1_000_000);
        assert_eq!(TX_GUARD_FRAMES, 16);
        assert_eq!(TX_LEAD_FRAMES, 576);
        assert_eq!(TX_HEALTHY_STALE_NS, 20_000_000);
        assert_eq!(TX_RING_PERIOD_BYTES % ADMAC_BURST_BYTES as usize, 0);
        assert_eq!(TX_RING_PERIOD_BYTES % TX_FRAME_BYTES, 0);
    }

    #[test]
    fn tx_ring_lap_detection() {
        let ring = TX_RING_FRAMES as u64;
        let period = TX_PERIOD_FRAMES as u64;
        assert!(!tx_ring_may_lap(0));
        assert!(!tx_ring_may_lap(tx_frames_ns(period)));
        assert!(!tx_ring_may_lap(tx_frames_ns(ring / 2)));
        assert!(!tx_ring_may_lap(tx_frames_ns(ring - period) - 1));
        assert!(tx_ring_may_lap(tx_frames_ns(ring - period)));
        assert!(tx_ring_may_lap(tx_frames_ns(ring)));
        assert!(tx_ring_may_lap(tx_frames_ns(2 * ring + period)));
        assert!(tx_ring_may_lap(-1));
        assert!(!tx_ring_is_stale(0));
        assert!(!tx_ring_is_stale(TX_RING_PERIOD_NS as i64));
        assert!(!tx_ring_is_stale(TX_HEALTHY_STALE_NS as i64));
        assert!(tx_ring_is_stale(TX_HEALTHY_STALE_NS as i64 + 1));
        assert!(tx_ring_is_stale(-1));
    }

    #[test]
    fn frames_ahead_wrap_and_behind() {
        let boundary = 4_096;
        assert_eq!(frames_ahead(0, 0, boundary), 0);
        assert_eq!(frames_ahead(100, 356, boundary), 256);
        // Wrap across boundary.
        assert_eq!(frames_ahead(4_000, 100, boundary), 196);
        // Behind (underrun): returns 0 rather than nearly `boundary`.
        assert_eq!(frames_ahead(356, 100, boundary), 0);
        assert_eq!(frames_ahead(100, 4_000, boundary), 0);
        // Zero boundary is safe.
        assert_eq!(frames_ahead(10, 20, 0), 0);
        // High boundary values near `usize::MAX` do not overflow.
        let huge = usize::MAX - 15;
        assert_eq!(frames_ahead(huge - 10, 20, huge), 30);
        assert_eq!(frames_ahead(20, huge - 10, huge), 0);
    }

    #[test]
    fn tx_ring_advance_conservation_and_wrap() {
        let buf_frames = 1_024;
        let boundary = 4_096;
        // Start near the end of both the TX ring and ALSA's boundary to exercise both wraps.
        let mut s = TxStep {
            ring_hw: TX_RING_FRAMES - 20,
            ring_wr: (TX_RING_FRAMES - 20 + TX_GUARD_FRAMES) % TX_RING_FRAMES,
            guard_left: TX_GUARD_FRAMES,
            fe_played: boundary - 60,
            fe_copied: boundary - 60,
            fe_total_played: 0,
        };
        let mut appl_ptr = add_mod(s.fe_played, 768, boundary);
        simulate_refill(&mut s, appl_ptr, buf_frames, boundary);
        assert_eq!(queued(&s), TX_LEAD_FRAMES);
        assert_conservation(&s, boundary);

        // Step 200 ticks (200 ms, wrapping TX_RING_FRAMES ~6x and `boundary` >2x).
        for tick in 0..200usize {
            let dma_now = (s.ring_hw + TX_PERIOD_FRAMES) % TX_RING_FRAMES;
            let adv = tx_ring_advance(s, dma_now, boundary);
            assert_eq!(adv.advanced, TX_PERIOD_FRAMES);
            assert_eq!(adv.zero_count, TX_PERIOD_FRAMES);
            assert!(adv.zero_count == adv.advanced);
            s = adv.step;
            assert_conservation(&s, boundary);
            if tick == 0 {
                // First tick consumes all 16 guard frames and 32 audio frames.
                assert_eq!(s.guard_left, 0);
                assert_eq!(s.fe_total_played, TX_PERIOD_FRAMES - TX_GUARD_FRAMES);
            }
            appl_ptr = add_mod(appl_ptr, TX_PERIOD_FRAMES, boundary);
            simulate_refill(&mut s, appl_ptr, buf_frames, boundary);
            assert_eq!(queued(&s), TX_LEAD_FRAMES);
            assert_conservation(&s, boundary);
        }
        assert_eq!(s.fe_total_played, 200 * TX_PERIOD_FRAMES - TX_GUARD_FRAMES);
    }

    #[test]
    fn tx_ring_advance_underrun_freewheeling_and_drain() {
        let buf_frames = 1_024;
        let boundary = 4_096;
        let mut s = TxStep {
            ring_hw: 0,
            ring_wr: TX_GUARD_FRAMES,
            guard_left: TX_GUARD_FRAMES,
            fe_played: 0,
            fe_copied: 0,
            fe_total_played: 0,
        };
        // Userspace commits only 100 frames (short drain / underrun scenario).
        let mut appl_ptr = 100;
        simulate_refill(&mut s, appl_ptr, buf_frames, boundary);
        assert_eq!(queued(&s), TX_GUARD_FRAMES + 100);
        assert_conservation(&s, boundary);

        // Tick 1: plays 16 guard + 32 audio frames -> 68 queued remain.
        let adv = tx_ring_advance(s, 48, boundary);
        assert_eq!(adv.zero_count, 48);
        assert_eq!(adv.step.fe_played, 32);
        s = adv.step;
        assert_conservation(&s, boundary);

        // Tick 2: plays 48 audio frames -> 20 queued remain.
        let adv = tx_ring_advance(s, 96, boundary);
        assert_eq!(adv.zero_count, 48);
        assert_eq!(adv.step.fe_played, 80);
        s = adv.step;
        assert_conservation(&s, boundary);

        // Tick 3: DMA passes `ring_wr` (20 audio + 28 silence).
        let adv = tx_ring_advance(s, 144, boundary);
        assert_eq!(adv.zero_count, 20);
        assert!(adv.zero_count < adv.advanced);
        assert_eq!(adv.step.fe_played, 128);
        assert_eq!(adv.step.fe_copied, 128);
        assert_eq!(adv.step.guard_left, 0);
        assert_eq!(queued(&adv.step), 0);
        // In DRAINING state, `frames_ahead(fe_played, appl_ptr)` is now 0 -> drain finishes.
        assert_eq!(frames_ahead(adv.step.fe_played, appl_ptr, boundary), 0);
        s = adv.step;
        assert_conservation(&s, boundary);

        // While free-wheeling with no new writes, time keeps advancing at exact 48 kHz and
        // touches zero ring frames.
        for i in 1..=3usize {
            let dma_now = 144 + i * TX_PERIOD_FRAMES;
            let adv = tx_ring_advance(s, dma_now, boundary);
            assert_eq!(adv.zero_count, 0);
            assert!(adv.zero_count < adv.advanced);
            assert_eq!(adv.step.fe_played, 128 + i * TX_PERIOD_FRAMES);
            assert_eq!(adv.step.fe_copied, adv.step.fe_played);
            assert_eq!(adv.step.guard_left, 0);
            s = adv.step;
            assert_conservation(&s, boundary);
        }

        // Mid-period recovery via `pcm_ack`: DMA moved 22 frames past `ring_hw` before userspace
        // commits 768 frames ahead of current live position.
        let mid_dma = (s.ring_hw + 22) % TX_RING_FRAMES;
        let adv = tx_ring_advance(s, mid_dma, boundary);
        assert_eq!(adv.zero_count, 0);
        s = adv.step;
        appl_ptr = add_mod(s.fe_played, 768, boundary);
        simulate_refill(&mut s, appl_ptr, buf_frames, boundary);
        assert_eq!(s.guard_left, TX_GUARD_FRAMES);
        assert_eq!(queued(&s), TX_LEAD_FRAMES);
        assert_conservation(&s, boundary);
    }

    #[test]
    fn tx_ring_advance_clamps_backward_glitch() {
        let boundary = 4_096;
        // 1-frame stale/backward `dma_now` right after `trigger_start` at ring wrap (0 -> 1535).
        let after_start = TxStep {
            ring_hw: 0,
            ring_wr: TX_LEAD_FRAMES,
            guard_left: TX_GUARD_FRAMES,
            fe_played: 100,
            fe_copied: 100 + TX_LEAD_FRAMES - TX_GUARD_FRAMES,
            fe_total_played: 100,
        };
        assert_conservation(&after_start, boundary);
        let adv = tx_ring_advance(after_start, TX_RING_FRAMES - 1, boundary);
        assert_eq!(adv.advanced, 0);
        assert_eq!(adv.zero_count, 0);
        assert_eq!(adv.step, after_start);
        assert_conservation(&adv.step, boundary);

        // 1-frame stale/backward `dma_now` right after `ack` in steady state.
        let after_ack = TxStep {
            ring_hw: 500,
            ring_wr: (500 + TX_LEAD_FRAMES) % TX_RING_FRAMES,
            guard_left: 0,
            fe_played: 1_000,
            fe_copied: 1_000 + TX_LEAD_FRAMES,
            fe_total_played: 1_000,
        };
        assert_conservation(&after_ack, boundary);
        let adv = tx_ring_advance(after_ack, 499, boundary);
        assert_eq!(adv.advanced, 0);
        assert_eq!(adv.zero_count, 0);
        assert_eq!(adv.step, after_ack);

        // Exact backward-glitch threshold (`delta == TX_RING_FRAMES - TX_PERIOD_FRAMES`).
        let at_thresh = (after_ack.ring_hw + TX_RING_FRAMES - TX_PERIOD_FRAMES) % TX_RING_FRAMES;
        let adv = tx_ring_advance(after_ack, at_thresh, boundary);
        assert_eq!(adv.advanced, 0);
        assert_eq!(adv.zero_count, 0);
        assert_eq!(adv.step, after_ack);

        // One frame below the threshold still advances normally.
        let below_thresh =
            (after_ack.ring_hw + TX_RING_FRAMES - TX_PERIOD_FRAMES - 1) % TX_RING_FRAMES;
        let adv = tx_ring_advance(after_ack, below_thresh, boundary);
        assert_eq!(adv.advanced, TX_RING_FRAMES - TX_PERIOD_FRAMES - 1);
        assert_eq!(adv.zero_count, TX_LEAD_FRAMES);
        assert_conservation(&adv.step, boundary);
    }

    #[test]
    fn tx_ring_lead_survives_softirq_stall() {
        let buf_frames = 2_048;
        let boundary = 8_192;
        let mut s = TxStep {
            ring_hw: TX_RING_FRAMES - 200,
            ring_wr: (TX_RING_FRAMES - 200 + TX_LEAD_FRAMES) % TX_RING_FRAMES,
            guard_left: 0,
            fe_played: 1_000,
            fe_copied: 1_000 + TX_LEAD_FRAMES,
            fe_total_played: 1_000,
        };
        let mut appl_ptr = add_mod(s.fe_played, 1_024, boundary);
        assert_eq!(queued(&s), TX_LEAD_FRAMES);
        assert_conservation(&s, boundary);

        // Simulate a 10 ms softirq stall (480 frames = 10 periods) in a single callback.
        let stall_frames = 10 * TX_PERIOD_FRAMES;
        let dma_now = (s.ring_hw + stall_frames) % TX_RING_FRAMES;
        let adv = tx_ring_advance(s, dma_now, boundary);
        assert_eq!(adv.advanced, stall_frames);
        assert_eq!(adv.zero_count, stall_frames);
        assert_eq!(queued(&adv.step), TX_LEAD_FRAMES - stall_frames);
        assert!(queued(&adv.step) > 0);
        assert_eq!(adv.step.fe_copied, s.fe_copied);
        s = adv.step;
        assert_conservation(&s, boundary);

        // Refill tops lookahead cleanly back up to `TX_LEAD_FRAMES`.
        appl_ptr = add_mod(appl_ptr, stall_frames, boundary);
        simulate_refill(&mut s, appl_ptr, buf_frames, boundary);
        assert_eq!(queued(&s), TX_LEAD_FRAMES);
        assert_conservation(&s, boundary);
    }

    #[test]
    fn tx_ring_live_progress_guard_wrap_and_glitch() {
        // Still inside initial guard window.
        assert_eq!(tx_ring_live_progress(100, 110, TX_GUARD_FRAMES), (0, 6));
        // Past guard window, wrapping across TX_RING_FRAMES.
        assert_eq!(
            tx_ring_live_progress(TX_RING_FRAMES - 10, 20, TX_GUARD_FRAMES),
            (14, 0)
        );
        // Steady state with no guard.
        assert_eq!(tx_ring_live_progress(200, 235, 0), (35, 0));
        // Small backward glitch clamped to zero progress.
        assert_eq!(tx_ring_live_progress(200, 199, 4), (0, 4));
    }

    #[test]
    fn tx_ring_silence_plan_covers_exact_queued_slice() {
        // Empty queue -> nothing to zero.
        let p = tx_ring_silence_plan(100, 100, 120);
        assert_eq!((p.ahead_count, p.prefix_count), (0, 0));

        // Normal mid-queue stop wrapping across TX_RING_FRAMES.
        let hw = TX_RING_FRAMES - 40;
        let wr = (hw + TX_LEAD_FRAMES) % TX_RING_FRAMES;
        let dma_now = (hw + 30) % TX_RING_FRAMES;
        let p = tx_ring_silence_plan(hw, wr, dma_now);
        assert_eq!(p.ahead_start, (dma_now + 2) % TX_RING_FRAMES);
        assert_eq!(p.ahead_count, TX_LEAD_FRAMES - 32);
        assert_eq!(p.prefix_start, hw);
        assert_eq!(p.prefix_count, 32);
        assert_eq!(p.ahead_count + p.prefix_count, TX_LEAD_FRAMES);

        // DMA already passed `wr` -> bounded to `queued`, never zeroes the whole ring.
        let dma_past = (hw + TX_LEAD_FRAMES + 200) % TX_RING_FRAMES;
        let p = tx_ring_silence_plan(hw, wr, dma_past);
        assert_eq!(p.ahead_count, 0);
        assert_eq!(p.prefix_start, hw);
        assert_eq!(p.prefix_count, TX_LEAD_FRAMES);
    }
}
