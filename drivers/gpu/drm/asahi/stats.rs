// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Snapshot of AGX firmware statistics.
//!
//! [`StatsSnapshot`] accumulates the values decoded by `StatsChannel::poll`
//! (see `fw::channels.rs::StatsMsg`) and exposes them to the sysfs file
//! `agx_stats` (registered from `sysfs.c`).
//!
//! The snapshot belongs to one GPU manager. The firmware-stat
//! decoder that feeds it lives in `StatsChannel::poll` (`channel.rs`), inside
//! the versioned context where `StatsMsg` is nameable; `note_job()` is called
//! from the `recv_message` rtkit callback (so the firmware mailbox IRQ
//! context); the sysfs `show` callback runs in arbitrary process context. All
//! fields use `AtomicU*` so neither side takes a lock.
//!
//! Field semantics (firmware units, late-validated):
//! - `util1..util4`: raw `Utilization` u32s from the firmware. Meaning
//!   (firmware tick or %?) is validated against a controlled MLX load; we
//!   publish them unscaled per the producer contract.
//! - `pstate`: current performance state index from `PowerState`.
//! - `avg_power_mw`: average power in milliwatts from `AvgPower` (firmware
//!   field is u32 — units not validated for every chip rev; treat as raw).
//! - `temperature`: raw `Temperature` value, scale, tmin, tmax.
//! - `busy_ns`: cumulative utilization-weighted busy time in nanoseconds,
//!   derived from successive `Utilization` windows (busiest subqueue
//!   percentage x window duration; the firmware timestamps are base-clock
//!   ticks, 24 MHz, converted with `HwConfig::base_clock_hz`). Monotonic and
//!   bounded by elapsed time. Saturates at u64::MAX.
//! - `jobs`: completed submissions since boot, counted at submission
//!   completion (`JobFence::command_complete`).
//!
//! This module is read-only with respect to scheduling or power behaviour:
//! the firmware already sends the messages, the channel already polls them,
//! we only retain the latest values.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// Per-device snapshot of AGX firmware stats.
///
/// The manager and bound driver retain Arcs. The driver's Arc outlives the
/// per-device C sysfs registration; repr(C) establishes the shared layout.
#[derive(Default)]
#[repr(C)]
pub(crate) struct StatsSnapshot {
    pub(crate) util1: AtomicU32,
    pub(crate) util2: AtomicU32,
    pub(crate) util3: AtomicU32,
    pub(crate) util4: AtomicU32,
    pub(crate) pstate: AtomicU32,
    pub(crate) avg_power_mw: AtomicU32,
    pub(crate) temperature_raw: AtomicU32,
    pub(crate) temperature_scale: AtomicU32,
    pub(crate) temperature_tmin: AtomicU32,
    pub(crate) temperature_tmax: AtomicU32,
    /// Cumulative utilization-weighted busy time.
    pub(crate) busy_ns: AtomicU64,
    /// Completed submissions since boot, counted at fence signal in
    /// `JobFence::command_complete` when the last command of a submission
    /// completes.
    pub(crate) jobs: AtomicU64,
}

// Keep the Rust half of the C sysfs layout contract checked as well.
const _: () = {
    assert!(core::mem::offset_of!(StatsSnapshot, pstate) == 16);
    assert!(core::mem::offset_of!(StatsSnapshot, busy_ns) == 40);
    assert!(core::mem::offset_of!(StatsSnapshot, jobs) == 48);
    assert!(core::mem::size_of::<StatsSnapshot>() == 56);
};

impl StatsSnapshot {
    /// Bump the completed-submission counter (called from the queue
    /// completion path in `JobFence::command_complete`, NOT from the stats
    /// channel).
    pub(crate) fn note_job(&self) {
        self.jobs.fetch_add(1, Ordering::Relaxed);
    }

    /// Account one `Utilization` record, whose `timestamp` runs at `ts_hz`.
    /// `last_ts` is the previous record's timestamp, 0 before the first one.
    ///
    /// busy_ns: utilization-weighted busy time in nanoseconds. Each window
    /// contributes its duration scaled by the busiest subqueue's percentage.
    /// MEASURED on T6021 (round-3 window): util1..4 read 100 across a saturated
    /// 30 s matmul and 0-2 on an idle desktop, while FwBusy timestamp deltas
    /// (the prior producer) covered 0.17% of that matmul. The timestamps are
    /// base-clock ticks (24 MHz), so convert to ns once here. The kernel runtime
    /// has no 128-bit division, so split whole seconds from the tick remainder:
    /// whole seconds contribute util * 1e7 ns each (1e9 / 100 exactly), the
    /// remainder contributes rem * util * 1e9 / (100 * ts_hz), which cannot
    /// overflow u64 (rem < ts_hz <= 24e6 on supported SoCs).
    pub(crate) fn note_utilization(
        &self,
        last_ts: &mut u64,
        timestamp: u64,
        util: [u32; 4],
        ts_hz: u64,
    ) {
        self.util1.store(util[0], Ordering::Relaxed);
        self.util2.store(util[1], Ordering::Relaxed);
        self.util3.store(util[2], Ordering::Relaxed);
        self.util4.store(util[3], Ordering::Relaxed);
        let busiest = u64::from(util.into_iter().max().unwrap_or(0).min(100));
        if *last_ts != 0 && timestamp > *last_ts {
            let delta = timestamp - *last_ts;
            let whole_s = delta / ts_hz;
            let rem = delta % ts_hz;
            let busy = whole_s
                .saturating_mul(busiest)
                .saturating_mul(10_000_000)
                .saturating_add(rem * busiest * 1_000_000_000 / (100 * ts_hz));
            self.busy_ns.fetch_add(busy, Ordering::Relaxed);
        }
        *last_ts = timestamp;
    }
}

#[kernel::macros::kunit_tests(asahi_stats)]
mod tests {
    use super::*;

    const HZ: u64 = 24_000_000;

    fn busy(stats: &StatsSnapshot) -> u64 {
        stats.busy_ns.load(Ordering::Relaxed)
    }

    #[test]
    fn first_record_only_opens_the_window() {
        let stats = StatsSnapshot::default();
        let mut last = 0;
        stats.note_utilization(&mut last, 5 * HZ, [100; 4], HZ);
        assert_eq!(busy(&stats), 0);
        assert_eq!(last, 5 * HZ);
        assert_eq!(stats.util4.load(Ordering::Relaxed), 100);
    }

    #[test]
    fn windows_add_duration_scaled_by_the_busiest_field() {
        let stats = StatsSnapshot::default();
        let mut last = HZ;
        // 1.5 s at 40%, then 250 ms at 100%.
        stats.note_utilization(&mut last, HZ + HZ * 3 / 2, [10, 40, 0, 40], HZ);
        assert_eq!(busy(&stats), 600_000_000);
        stats.note_utilization(&mut last, HZ + HZ * 7 / 4, [100, 0, 0, 0], HZ);
        assert_eq!(busy(&stats), 850_000_000);
    }

    #[test]
    fn out_of_range_percentage_cannot_exceed_the_window() {
        let stats = StatsSnapshot::default();
        let mut last = HZ;
        stats.note_utilization(&mut last, 2 * HZ, [0, 0, 0, u32::MAX], HZ);
        assert_eq!(busy(&stats), 1_000_000_000);
    }

    #[test]
    fn stale_timestamp_adds_nothing_and_restarts_the_window() {
        let stats = StatsSnapshot::default();
        let mut last = 3 * HZ;
        stats.note_utilization(&mut last, 2 * HZ, [100; 4], HZ);
        assert_eq!(busy(&stats), 0);
        assert_eq!(last, 2 * HZ);
    }
}
