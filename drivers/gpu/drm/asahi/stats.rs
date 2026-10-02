// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Snapshot of AGX firmware statistics.
//!
//! [`StatsSnapshot`] accumulates the values decoded by `StatsChannel::poll`
//! (see `fw::channels.rs::StatsMsg`) and exposes them to the sysfs file
//! `agx_stats` (registered from `sysfs.c`).
//!
//! The snapshot is process-local and process-private. `update_from()` is called
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

use crate::fw::channels::StatsMsg;

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
    last_busy_ts: AtomicU64,
    /// Cumulative nanoseconds the firmware reports itself as busy.
    pub(crate) busy_ns: AtomicU64,
    /// Completed submissions since boot. Starts at 0; updated only when the
    /// firmware signals completion. T6001 firmware does not emit a dedicated
    /// job counter message, so this stays 0 on T6001.
    pub(crate) jobs: AtomicU64,
}

/// Pointer to the live snapshot, set from Rust probe (via `into_raw`) and
/// read by the C `show` callback. `Ordering::Relaxed` is fine: the show path
/// races against the IRQ path only in establishing visibility of the pointer
/// itself; once visible, individual `AtomicU*` loads carry the data race.
pub(crate) static SNAPSHOT_PTR: AtomicU64 = AtomicU64::new(0);

impl StatsSnapshot {
    /// Update retained fields from a `StatsMsg`. Called from
    /// `StatsChannel::poll` after the existing debug-log, only when
    /// `module_parameters::stats_export` is on.
    pub(crate) fn update_from(&self, msg: &StatsMsg::ver) {
        match *msg {
            StatsMsg::Utilization {
                timestamp: _,
                util1,
                util2,
                util3,
                util4,
            } => {
                self.util1.store(u32::from(util1), Ordering::Relaxed);
                self.util2.store(u32::from(util2), Ordering::Relaxed);
                self.util3.store(u32::from(util3), Ordering::Relaxed);
                self.util4.store(u32::from(util4), Ordering::Relaxed);
            }
            StatsMsg::PowerState {
                timestamp: _,
                last_busy_ts: _,
                active,
                poweroff,
                unk1: _,
                pstate,
                unk2: _,
                unk3: _,
            } => {
                self.pstate.store(u32::from(pstate), Ordering::Relaxed);
                // `active` / `poweroff` are debug-only flags (pwr state
                // transitions); surface them via util later if useful.
                let _ = (active, poweroff);
            }
            StatsMsg::AvgPower {
                active_cs: _,
                unk2: _,
                unk3: _,
                unk4: _,
                avg_power,
            } => {
                self.avg_power_mw
                    .store(u32::from(avg_power), Ordering::Relaxed);
            }
            StatsMsg::Temperature {
                __pad: _,
                raw_value,
                scale,
                tmin,
                tmax,
            } => {
                self.temperature_raw
                    .store(u32::from(raw_value), Ordering::Relaxed);
                self.temperature_scale
                    .store(u32::from(scale), Ordering::Relaxed);
                self.temperature_tmin
                    .store(u32::from(tmin), Ordering::Relaxed);
                self.temperature_tmax
                    .store(u32::from(tmax), Ordering::Relaxed);
            }
            StatsMsg::FwBusy { timestamp, busy } => {
                let ts = u64::from(timestamp);
                let prev = self.last_busy_ts.swap(ts, Ordering::Relaxed);
                if prev != 0 && ts >= prev {
                    let delta = ts - prev;
                    // Add firmware timestamp units to busy_ns. The firmware
                    // uses nanoseconds on T6001 (validated against an MLX
                    // matmul); for other SoCs the unit may differ and the
                    // field will be renamed.
                    self.busy_ns.fetch_add(delta, Ordering::Relaxed);
                }
                let _ = busy; // currently unused; busy count is the delta above.
            }
            StatsMsg::PowerOn { .. } | StatsMsg::PowerOff { .. } | StatsMsg::PState { .. } => {
                // Cumulative on/off times; not in the producer contract yet.
            }
            StatsMsg::Unk1(_)
            | StatsMsg::Unk5(_)
            | StatsMsg::Unk6(_)
            | StatsMsg::Unk7(_)
            | StatsMsg::Unk8(_)
            | StatsMsg::TempSensor { .. } => {
                // Unknowns / per-sensor temp; passed through silently.
            }
        }
    }

    /// Bump the submission completion counter (called from the queue submit
    /// path, NOT from the stats channel).
    pub(crate) fn note_job(&self) {
        self.jobs.fetch_add(1, Ordering::Relaxed);
    }
}
