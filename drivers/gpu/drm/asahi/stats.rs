// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Snapshot of AGX firmware statistics.
//!
//! [`StatsSnapshot`] accumulates the values decoded by `StatsChannel::poll`
//! (see `fw::channels.rs::StatsMsg`) and exposes them to the sysfs file
//! `agx_stats` (registered from `sysfs.c`).
//!
//! The snapshot is process-local and process-private. The firmware-stat
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
//! - `busy_ns`: cumulative nanoseconds the firmware reports itself as busy,
//!   derived from successive `FwBusy { busy, timestamp }` deltas. Saturates
//!   at u64::MAX.
//! - `jobs`: completed submissions since boot. Currently always 0 on T6001;
//!   reserved for the ring-event submission-counter landing.
//!
//! This module is read-only with respect to scheduling or power behaviour:
//! the firmware already sends the messages, the channel already polls them,
//! we only retain the latest values.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// Process-wide snapshot of AGX firmware stats.
///
/// Lives in the `GpuManager`; the raw pointer is published to a module-level
/// `AtomicPtr` (`stats::SNAPSHOT_PTR`) for the C `show` callback to read
/// without walking the kernel object graph.
#[derive(Default)]
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
    /// Last observed firmware busy timestamp (monotonic, firmware units).
    pub(crate) last_busy_ts: AtomicU64,
    /// Cumulative nanoseconds the firmware reports itself as busy.
    pub(crate) busy_ns: AtomicU64,
    /// Completed submissions since boot, counted at fence signal in
    /// `JobFence::command_complete` when the last command of a submission
    /// completes.
    pub(crate) jobs: AtomicU64,
}

/// Pointer to the live snapshot, set from Rust probe (via `into_raw`) and
/// read by the C `show` callback. `Ordering::Relaxed` is fine: the show path
/// races against the IRQ path only in establishing visibility of the pointer
/// itself; once visible, individual `AtomicU*` loads carry the data race.
pub(crate) static SNAPSHOT_PTR: AtomicU64 = AtomicU64::new(0);

impl StatsSnapshot {
    /// Bump the completed-submission counter (called from the queue
    /// completion path in `JobFence::command_complete`, NOT from the stats
    /// channel).
    pub(crate) fn note_job(&self) {
        self.jobs.fetch_add(1, Ordering::Relaxed);
    }
}
