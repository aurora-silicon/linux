// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Lock-free host-refill pressure detector for G16 CPU frequency boosting.
//!
//! Tracks monotonic flight `[published_ns, retired_ns]` intervals so detection works
//! identically whether or not firmware microsequence timestamps are enabled.

use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

/// Maximum inter-flight idle gap (2 ms) that still counts as an active back-to-back burst
/// rather than paced desktop/compositor rendering (60 Hz = 16.67 ms, 120 Hz = 8.33 ms).
pub(crate) const MAX_REFILL_NS: u64 = 2_000_000;
/// Consecutive sub-`MAX_REFILL_NS` flight completions required before engaging CPU boost.
pub(crate) const STREAK_ON: u32 = 8;
/// Upper saturation bound on the consecutive-burst streak counter.
pub(crate) const STREAK_MAX: u32 = 64;
/// How long CPU capacity boost lingers after the most recent refill-pressure sample.
pub(crate) const LINGER_NS: u64 = 8_000_000;
/// EWMA busy-sum ceiling (~8x 600 us incremental flight time) below which overlapping
/// pipelined (`d >= 2`) flights are still host-recording-bound rather than GPU-bound.
pub(crate) const SHORT_PIPELINE_BUSY_SUM_NS: u64 = 4_800_000;

/// Lock-free refill-gap detector shared between completion retirement and the feed worker.
pub(crate) struct Detector {
    last_end: AtomicU64,
    idle: AtomicU64,
    busy: AtomicU64,
    streak: AtomicU32,
    until: AtomicU64,
    bound_until: AtomicU64,
    active: AtomicBool,
    pending: AtomicBool,
    paused: AtomicBool,
    stopping: AtomicBool,
}

impl Detector {
    pub(crate) const fn new() -> Self {
        Self {
            last_end: AtomicU64::new(0),
            idle: AtomicU64::new(0),
            busy: AtomicU64::new(0),
            streak: AtomicU32::new(0),
            until: AtomicU64::new(0),
            bound_until: AtomicU64::new(0),
            active: AtomicBool::new(false),
            pending: AtomicBool::new(false),
            paused: AtomicBool::new(false),
            stopping: AtomicBool::new(false),
        }
    }

    fn decay_add(sum: &AtomicU64, add: u64) -> u64 {
        let mut value = sum.load(Ordering::Acquire);
        loop {
            let next = value - value / 8 + add;
            match sum.compare_exchange_weak(value, next, Ordering::AcqRel, Ordering::Acquire) {
                Ok(_) => return next,
                Err(observed) => value = observed,
            }
        }
    }

    /// Records one completed flight's monotonic `[start_ns, end_ns]` span.
    pub(crate) fn note(&self, start_ns: u64, end_ns: u64, now_ns: u64) {
        if start_ns == 0
            || end_ns <= start_ns
            || self.paused.load(Ordering::Acquire)
            || self.stopping.load(Ordering::Acquire)
        {
            return;
        }
        let previous = self.last_end.fetch_max(end_ns, Ordering::AcqRel);
        let overlapped = previous != 0 && start_ns < previous;
        let (idle, busy, paced) = if previous == 0 {
            (0, end_ns - start_ns, true)
        } else if start_ns >= previous {
            let gap = start_ns - previous;
            if gap < MAX_REFILL_NS {
                (gap, end_ns - start_ns, false)
            } else {
                (0, end_ns - start_ns, true)
            }
        } else if end_ns > previous {
            (0, end_ns - previous, false)
        } else {
            (0, 0, false)
        };
        let idle_sum = Self::decay_add(&self.idle, idle);
        let busy_sum = Self::decay_add(&self.busy, busy);
        let streak = if paced {
            self.streak.store(0, Ordering::Release);
            0
        } else {
            let mut value = self.streak.load(Ordering::Acquire);
            loop {
                let next = (value + 1).min(STREAK_MAX);
                match self.streak.compare_exchange_weak(
                    value,
                    next,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => break next,
                    Err(observed) => value = observed,
                }
            }
        };
        if streak < STREAK_ON {
            return;
        }
        if idle_sum.saturating_mul(4) >= busy_sum
            || (overlapped && busy_sum <= SHORT_PIPELINE_BUSY_SUM_NS)
        {
            self.until
                .store(now_ns.saturating_add(LINGER_NS), Ordering::Release);
            if !self.active.swap(true, Ordering::AcqRel) {
                self.pending.store(true, Ordering::Release);
            }
        } else {
            self.bound_until
                .store(now_ns.saturating_add(LINGER_NS), Ordering::Release);
        }
    }

    pub(crate) fn wanted(&self, now_ns: u64) -> bool {
        !self.stopping.load(Ordering::Acquire)
            && !self.paused.load(Ordering::Acquire)
            && now_ns < self.until.load(Ordering::Acquire)
    }

    #[allow(dead_code)]
    pub(crate) fn bound(&self, now_ns: u64) -> bool {
        !self.stopping.load(Ordering::Acquire)
            && !self.paused.load(Ordering::Acquire)
            && now_ns < self.bound_until.load(Ordering::Acquire)
    }

    pub(crate) fn take_pending(&self) -> bool {
        self.pending.swap(false, Ordering::AcqRel)
    }

    pub(crate) fn clear_active(&self) {
        self.active.store(false, Ordering::SeqCst);
    }

    pub(crate) fn try_claim_active(&self) -> bool {
        !self.active.swap(true, Ordering::AcqRel)
    }

    pub(crate) fn pause(&self) {
        self.paused.store(true, Ordering::Release);
        self.pending.store(false, Ordering::Release);
        self.until.store(0, Ordering::Release);
        self.bound_until.store(0, Ordering::Release);
        self.streak.store(0, Ordering::Release);
    }

    pub(crate) fn resume(&self) {
        self.last_end.store(0, Ordering::Release);
        self.idle.store(0, Ordering::Release);
        self.busy.store(0, Ordering::Release);
        self.streak.store(0, Ordering::Release);
        self.paused.store(false, Ordering::Release);
    }

    pub(crate) fn begin_shutdown(&self) {
        self.stopping.store(true, Ordering::Release);
        self.pending.store(false, Ordering::Release);
    }

    pub(crate) fn is_stopped_or_paused(&self) -> bool {
        self.stopping.load(Ordering::Acquire) || self.paused.load(Ordering::Acquire)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(detector: &Detector, flights: u64, busy_ns: u64, gap_ns: u64) -> u64 {
        let mut start = 10_000_000;
        for _ in 0..flights {
            let end = start + busy_ns;
            detector.note(start, end, end);
            start = end + gap_ns;
        }
        start - gap_ns
    }

    #[test]
    fn cpu_refill_bound_burst_requests_boost_and_expires_after_linger() {
        let d = Detector::new();
        // 170 us GPU flight with 450 us host recording gap (d=1 bindless_ubo / mdi).
        let now = run(&d, 16, 170_000, 450_000);
        assert!(d.wanted(now));
        assert!(d.take_pending());
        assert!(!d.take_pending());
        assert!(!d.bound(now));
        assert!(!d.wanted(now + LINGER_NS));
    }

    #[test]
    fn gpu_bound_back_to_back_flights_do_not_boost_cpu() {
        let d = Detector::new();
        // 2 ms GPU flights with 40 us host gaps: GPU-bound, not host-refill-bound.
        let now = run(&d, 16, 2_000_000, 40_000);
        assert!(d.bound(now));
        assert!(!d.wanted(now));
        assert!(!d.take_pending());
        assert!(!d.bound(now + LINGER_NS));
    }

    #[test]
    fn desktop_60hz_and_120hz_pacing_never_triggers_boost() {
        for period_ns in [16_666_667u64, 8_333_333u64] {
            let d = Detector::new();
            let now = run(&d, 20, 1_500_000, period_ns - 1_500_000);
            assert!(!d.wanted(now));
            assert!(!d.bound(now));
            assert!(!d.take_pending());
        }
    }

    #[test]
    fn short_runs_and_pause_resume_lifecycle_gate_boost_cleanly() {
        let d = Detector::new();
        // Fewer than STREAK_ON back-to-back flights cannot arm boost.
        let now = run(&d, u64::from(STREAK_ON), 100_000, 300_000);
        assert!(!d.wanted(now));
        assert!(!d.take_pending());

        // One more flight reaches streak == STREAK_ON and arms boost.
        let start = now + 300_000;
        let end = start + 100_000;
        d.note(start, end, end);
        assert!(d.wanted(end));
        assert!(d.take_pending());

        // Suspend pause drops wanted immediately and ignores samples until resume.
        assert!(!d.is_stopped_or_paused());
        assert!(!d.try_claim_active());
        d.pause();
        assert!(d.is_stopped_or_paused());
        assert!(!d.wanted(end));
        d.note(end + 100_000, end + 200_000, end + 200_000);
        assert!(!d.wanted(end + 200_000));

        // After resume + worker clear_active, try_claim_active works and a fresh burst re-arms.
        d.clear_active();
        assert!(d.try_claim_active());
        d.clear_active();
        d.resume();
        assert!(!d.is_stopped_or_paused());
        let resumed_now = run(&d, 16, 150_000, 400_000);
        assert!(d.wanted(resumed_now));
        assert!(d.take_pending());

        // Shutdown permanently suppresses boost.
        d.begin_shutdown();
        assert!(d.is_stopped_or_paused());
        assert!(!d.wanted(resumed_now));
        assert!(!d.take_pending());
    }

    #[test]
    fn pipelined_d2_cpu_recording_burst_sustains_boost_and_expires_after_linger() {
        let d = Detector::new();
        // Overlapping d=2 short flights (~300 us GPU duration, 200 us host submit cadence):
        // idle gap is 0 while overlapping, so EWMA busy_sum (~1.6 ms) sustains CPU boost.
        let mut start = 10_000_000u64;
        let mut last_end = 0u64;
        for _ in 0..20 {
            let end = start + 300_000;
            d.note(start, end, end);
            last_end = end;
            start += 200_000;
        }
        assert!(d.wanted(last_end));
        assert!(d.take_pending());
        assert!(!d.bound(last_end));
        assert!(!d.wanted(last_end + LINGER_NS));
    }

    #[test]
    fn pipelined_d2_gpu_bound_flights_do_not_boost_cpu() {
        let d = Detector::new();
        // Overlapping d=2 long GPU-bound flights (2 ms GPU duration, 1.8 ms cadence):
        // EWMA busy_sum (~14.4 ms >> SHORT_PIPELINE_BUSY_SUM_NS) stays GPU-bound without boost.
        let mut start = 10_000_000u64;
        let mut last_end = 0u64;
        for _ in 0..20 {
            let end = start + 2_000_000;
            d.note(start, end, end);
            last_end = end;
            start += 1_800_000;
        }
        assert!(d.bound(last_end));
        assert!(!d.wanted(last_end));
        assert!(!d.take_pending());
    }
}
