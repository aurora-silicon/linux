// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Utilization-weighted busy time for the legacy `agx_stats` `busy_ns` field.

/// Integrates `Utilization` windows into utilization-weighted busy time.
///
/// Each `Utilization` message carries a base-clock timestamp and the busy
/// percentage of the window that ends there; the window starts at the
/// previous message. The firmware sends no `Utilization` while the GPU is
/// powered off, so the first message after power-up would otherwise span
/// the whole idle gap and credit it at the new, typically 100%, load (the
/// first window after idle read 2.4-2.6 s too high on T6000 and T8112).
/// `PowerOff` and `PowerOn` therefore drop the baseline: the first window
/// after power-up only sets it. That loses at most the one short window in
/// which the GPU powered up, instead of crediting the idle gap.
pub(crate) struct BusyIntegrator {
    ts_hz: u64,
    last_ts: Option<u64>,
}

impl BusyIntegrator {
    /// `ts_hz` is the rate of the firmware timestamps (the chip base clock).
    pub(crate) fn new(ts_hz: u64) -> Self {
        Self {
            ts_hz: ts_hz.max(1),
            last_ts: None,
        }
    }

    /// The GPU powered off or on: the next window has no valid start.
    pub(crate) fn power_changed(&mut self) {
        self.last_ts = None;
    }

    /// Account one `Utilization` message at `ts` (base-clock ticks) with
    /// busiest-subqueue percentage `util`; returns the busy nanoseconds of
    /// the window it closes.
    ///
    /// The kernel runtime has no 128-bit division, so whole seconds
    /// contribute `util * 1e7` ns each (1e9 / 100 exactly) and the tick
    /// remainder `rem * util * 1e9 / (100 * ts_hz)`, which cannot overflow
    /// u64 (rem < ts_hz <= 24e6 on supported SoCs).
    pub(crate) fn utilization(&mut self, ts: u64, util: u32) -> u64 {
        let util = u64::from(util.min(100));
        let busy = match self.last_ts {
            Some(last) if ts > last => {
                let delta = ts - last;
                let whole_s = delta / self.ts_hz;
                let rem = delta % self.ts_hz;
                whole_s
                    .saturating_mul(util)
                    .saturating_mul(10_000_000)
                    .saturating_add(rem * util * 1_000_000_000 / (100 * self.ts_hz))
            }
            _ => 0,
        };
        self.last_ts = Some(ts);
        busy
    }
}

#[kernel::macros::kunit_tests(asahi_stats_busy)]
mod tests {
    use super::*;

    const HZ: u64 = 24_000_000;

    #[test]
    fn windows_are_weighted_by_utilization() {
        let mut b = BusyIntegrator::new(HZ);
        assert_eq!(b.utilization(1_000, 100), 0);
        // 10 ms at 100%, then 10 ms at 50%.
        assert_eq!(b.utilization(1_000 + 240_000, 100), 10_000_000);
        assert_eq!(b.utilization(1_000 + 480_000, 50), 5_000_000);
        // Over 100% is clamped.
        assert_eq!(b.utilization(1_000 + 720_000, 250), 10_000_000);
    }

    #[test]
    fn long_windows_split_whole_seconds() {
        let mut b = BusyIntegrator::new(HZ);
        b.utilization(0, 0);
        // 2.5 s at 100%. A first timestamp of 0 is a valid baseline.
        assert_eq!(b.utilization(60_000_000, 100), 2_500_000_000);
    }

    #[test]
    fn idle_gap_after_power_off_is_not_credited() {
        let mut b = BusyIntegrator::new(HZ);
        b.utilization(HZ, 0);
        // PowerOff, then PowerOn 2.5 s later.
        b.power_changed();
        b.power_changed();
        // First window after power-up: baseline only.
        assert_eq!(b.utilization(HZ + 60_000_000 + 240_000, 100), 0);
        assert_eq!(b.utilization(HZ + 60_000_000 + 480_000, 100), 10_000_000);
    }

    #[test]
    fn a_timestamp_that_goes_back_only_rebases() {
        let mut b = BusyIntegrator::new(HZ);
        b.utilization(5 * HZ, 100);
        assert_eq!(b.utilization(HZ, 100), 0);
        assert_eq!(b.utilization(HZ + 240_000, 100), 10_000_000);
        assert_eq!(b.utilization(HZ + 240_000, 100), 0);
    }

    #[test]
    fn huge_windows_saturate() {
        let mut b = BusyIntegrator::new(1);
        b.utilization(1, 100);
        assert_eq!(b.utilization(u64::MAX, 100), u64::MAX);
    }
}
