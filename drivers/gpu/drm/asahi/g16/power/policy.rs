// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Composition and sampling cadence for the retained firmware ceiling, and the
//! utilization governor below it.
//!
//! Temperature decoding and regulation belong to the PMP provider and the
//! shared C cooling policy. This module defines no temperature thresholds.

/// Highest selector any M4 table can have: S9 on the ten-row J713/J604 tables, S10 on the
/// eleven-row J773g table. The board's own top state (`top`) bounds every request.
pub(crate) const PSTATE_MAX: u32 = 10;
/// Top state of the ten-row tables, used until the board's table is known.
pub(crate) const TOP_DEFAULT: u32 = 9;
pub(crate) const COOLING_MAX: u32 = 8;
pub(crate) const SAMPLE_NS: u64 = 5_000_000;
/// Utilization window of the governor (slow decay cadence).
pub(crate) const WINDOW_NS: u64 = 20_000_000;
/// Fast upward ramp window aligned with the PMP sampling cadence ([`SAMPLE_NS`], 5 ms = 1/4
/// of [`WINDOW_NS`]) so active bursts step up before the full window closes without
/// overshooting on light periodic desktop frames.
pub(crate) const RAMP_WINDOW_NS: u64 = SAMPLE_NS;
/// Short idle hold before dropping the CLPC active hint between consecutive jobs.
/// `drivers/soc/apple/clpc.c` already holds DCS/fabric rails for `APPLE_CLPC_HOLD_MS` (32 ms)
/// on a `APPLE_CLPC_SAMPLE_MS` (4 ms) cadence; a 2 ms hold (half of `APPLE_CLPC_SAMPLE_MS`)
/// bridges sub-ms depth-1 inter-job gaps and avoids redundant `mod_delayed_work(..., 0)`
/// flushes without holding across 60 Hz (16.67 ms) or 120 Hz (8.33 ms) frame periods.
pub(crate) const CLPC_IDLE_HOLD_NS: u64 = 2_000_000;
/// Lowest governed state. On the J713 S1-S3 draw under 0.1 W for a desktop
/// load and S4 several times that; S3 runs at 2.4 times the clock of S1.
pub(crate) const FLOOR: u32 = 3;

/// Completion polling must not turn into a PMP MMIO read on every pass.
pub(crate) fn read_due(last: Option<u64>, now: u64) -> bool {
    last.is_none_or(|last| now.wrapping_sub(last) >= SAMPLE_NS)
}

/// Zero is not an unlimited selector in the firmware. Every published state
/// is 1..=top, bounded by the initialized Main table and the user ceiling.
pub(crate) fn limit(configured: u32, ceiling: u32, user: u32) -> u32 {
    ceiling.min(PSTATE_MAX).min(configured).min(user).max(1)
}

/// The board's top state, from its firmware table's row count (row 0 is off).
pub(crate) fn top_of(rows: u32) -> u32 {
    rows.saturating_sub(1).clamp(FLOOR, PSTATE_MAX)
}

/// The cooling ceiling for a cooling `level` (0..=COOLING_MAX) under the board's `top`
/// state: one state per level below the top, and S1 at full cooling on every table.
pub(crate) fn cooling_ceiling(top: u32, level: u32) -> u32 {
    if level >= COOLING_MAX {
        1
    } else {
        top.saturating_sub(level).max(1)
    }
}

/// Percentage of `window` ns the GPU was busy for.
pub(crate) fn utilization(busy: u64, window: u64) -> u32 {
    if window == 0 {
        100
    } else {
        let busy = busy.min(window);
        // No 128-bit division in the kernel; a window near u64::MAX loses nothing.
        let util = match busy.checked_mul(100) {
            Some(scaled) => scaled / window,
            None => busy / (window / 100),
        };
        util.min(100) as u32
    }
}

/// Utilization averaged over about four windows; a longer gap restarts it.
pub(crate) fn smooth(average: u32, util: u32, window: u64) -> u32 {
    if window >= 4 * WINDOW_NS {
        util
    } else {
        (3 * average.min(100) + util.min(100)) / 4
    }
}

/// The state to request from the `current` published one: up on the last
/// window's utilization `util`, to the top state under heavy load, and down
/// on the smoothed `average`, to the floor when idle.
pub(crate) fn demand(current: u32, util: u32, average: u32, top: u32) -> u32 {
    let top = top.clamp(FLOOR, PSTATE_MAX);
    let next = if util >= 70 {
        top
    } else if util >= 40 {
        current.saturating_add(1)
    } else if average < 10 {
        FLOOR
    } else if average < 25 {
        current.saturating_sub(1)
    } else {
        current
    };
    next.clamp(FLOOR, top)
}

/// Fast upward-only demand step for partial burst windows ([`RAMP_WINDOW_NS`]):
/// matches [`demand`]'s upward thresholds (`>= 70%` -> `top`, `>= 40%` -> `+1`) and
/// never decays, so light periodic frames cannot pump to `top` while sustained bursts
/// still reach `top` within one 5 ms sample.
pub(crate) fn ramp_demand(current: u32, util: u32, top: u32) -> u32 {
    let top = top.clamp(FLOOR, PSTATE_MAX);
    let next = if util >= 70 {
        top
    } else if util >= 40 {
        current.saturating_add(1)
    } else {
        current
    };
    next.max(current).clamp(FLOOR, top)
}

/// CLPC active-floor hysteresis: keeps the memory and fabric floor raised for
/// [`CLPC_IDLE_HOLD_NS`] after flights drain so short `d=1` inter-job gaps do
/// not drop and re-raise CLPC on every submission.
pub(crate) struct ClpcHold {
    reported: Option<bool>,
    idle_since: Option<u64>,
}

impl ClpcHold {
    pub(crate) const fn new() -> Self {
        Self {
            reported: None,
            idle_since: None,
        }
    }

    /// Updates work-in-flight state. Returns `Some(active)` only when the
    /// CLPC provider must transition immediately.
    pub(crate) fn report(&mut self, in_flight: bool, now: u64) -> Option<bool> {
        if in_flight {
            self.idle_since = None;
            if self.reported != Some(true) {
                self.reported = Some(true);
                return Some(true);
            }
            return None;
        }
        if self.reported == Some(true) {
            self.idle_since.get_or_insert(now);
            None
        } else if self.reported.is_none() {
            self.reported = Some(false);
            Some(false)
        } else {
            None
        }
    }

    /// Releases a held CLPC floor once [`CLPC_IDLE_HOLD_NS`] has elapsed idle.
    pub(crate) fn expire(&mut self, now: u64) -> bool {
        if self.reported == Some(true)
            && self
                .idle_since
                .is_some_and(|since| now.wrapping_sub(since) >= CLPC_IDLE_HOLD_NS)
        {
            self.idle_since = None;
            self.reported = Some(false);
            return true;
        }
        false
    }

    /// Immediately drops the CLPC floor (fail-stop or system sleep).
    pub(crate) fn release_now(&mut self) -> bool {
        self.idle_since = None;
        if self.reported != Some(false) {
            self.reported = Some(false);
            true
        } else {
            false
        }
    }

    /// Whether the CLPC floor is currently being held across an idle gap.
    pub(crate) fn holding(&self) -> bool {
        self.reported == Some(true) && self.idle_since.is_some()
    }
}

/// Maximum inter-flight idle gap bridged for active-burst GPU demand.
/// Matches [`CLPC_IDLE_HOLD_NS`] (2 ms): back-to-back `d=1` CPU-recording bursts
/// sit well below 2 ms while 60 Hz (16.67 ms) and 120 Hz (8.33 ms) desktop frames
/// exceed it cleanly and stay at [`FLOOR`].
const BURST_GAP_NS: u64 = CLPC_IDLE_HOLD_NS;
/// Consecutive sub-`BURST_GAP_NS` turnarounds required before bridging short gaps.
const BURST_STREAK_ON: u32 = 4;

/// Busy-time accounting over the governor's windows. The engine reports each
/// publication and drain; each power service closes the window once it is due.
pub(crate) struct Governor {
    /// The requested state and the smoothed utilization.
    pub(crate) demand: u32,
    pub(crate) average: u32,
    /// The board's top state.
    pub(crate) top: u32,
    window_ns: u64,
    busy_since: Option<u64>,
    idle_since: Option<u64>,
    burst_streak: u32,
    busy_ns: u64,
}

impl Governor {
    pub(crate) const fn new() -> Self {
        Self {
            demand: FLOOR,
            average: 0,
            top: TOP_DEFAULT,
            window_ns: 0,
            busy_since: None,
            idle_since: None,
            burst_streak: 0,
            busy_ns: 0,
        }
    }

    /// The engine published work; it stays busy until [`Governor::idle`].
    pub(crate) fn busy(&mut self, now: u64) {
        if self.busy_since.is_none() {
            if let Some(idle_at) = self.idle_since.take() {
                let gap = now.saturating_sub(idle_at);
                if gap < BURST_GAP_NS {
                    self.burst_streak = self.burst_streak.saturating_add(1);
                    if self.burst_streak >= BURST_STREAK_ON {
                        self.busy_ns = self.busy_ns.saturating_add(gap);
                    }
                } else {
                    self.burst_streak = 0;
                }
            }
            self.busy_since = Some(now);
        }
    }

    /// The engine drained.
    pub(crate) fn idle(&mut self, now: u64) {
        if let Some(since) = self.busy_since.take() {
            self.busy_ns += now.saturating_sub(since);
            self.idle_since = Some(now);
        }
    }

    /// Whether an idle engine still has a state above the floor to give back.
    /// Its poll comes back once a window while this holds.
    pub(crate) fn decaying(&self) -> bool {
        self.busy_since.is_none() && self.demand > FLOOR
    }

    /// Whether a busy engine below its ceiling has enough partial-window
    /// utilization to step up before the full [`WINDOW_NS`] window closes.
    pub(crate) fn ramp_due(&self, now: u64, limit: u32) -> bool {
        let cap = self.top.min(limit);
        if self.demand >= cap || self.busy_since.is_none() {
            return false;
        }
        let window = now.wrapping_sub(self.window_ns);
        if window < RAMP_WINDOW_NS {
            return false;
        }
        let active = self.busy_since.map_or(0, |s| now.saturating_sub(s));
        let util = utilization(self.busy_ns.saturating_add(active), window);
        ramp_demand(self.demand, util, self.top).min(limit) > self.demand
    }

    /// Closes the window once it is due and returns the state to publish
    /// under the ceiling `limit`; `busy` is whether flights are outstanding.
    pub(crate) fn govern(&mut self, now: u64, limit: u32, busy: bool) -> u32 {
        // Recovery can drop flights without a drain.
        if !busy {
            self.idle(now);
            if self
                .idle_since
                .is_some_and(|at| now.saturating_sub(at) >= BURST_GAP_NS)
            {
                self.burst_streak = 0;
            }
        }
        let window = now.wrapping_sub(self.window_ns);
        if window >= WINDOW_NS {
            if let Some(since) = self.busy_since.as_mut() {
                self.busy_ns += now.saturating_sub(*since);
                *since = now;
            }
            let util = utilization(self.busy_ns, window);
            self.average = smooth(self.average, util, window);
            self.demand = demand(self.demand.min(limit), util, self.average, self.top);
            self.window_ns = now;
            self.busy_ns = 0;
        } else if busy && window >= RAMP_WINDOW_NS && self.demand < self.top.min(limit) {
            let active = self.busy_since.map_or(0, |since| now.saturating_sub(since));
            let util = utilization(self.busy_ns.saturating_add(active), window);
            let next = ramp_demand(self.demand.min(limit), util, self.top).min(limit);
            if next > self.demand {
                self.demand = next;
            }
        }
        self.demand.min(limit)
    }
}

/// Startup/fault confirmation cadence (minimum spacing across 10 distinct sensor polls)
/// and gradual cooling-release step interval.
pub(crate) const ZONE_CONFIRM_NS: u64 = 20_000_000;
pub(crate) const ZONE_CONFIRM_READS: u32 = 10;
pub(crate) const ZONE_RELEASE_NS: u64 = 200_000_000;

/// Machine-supplied thermal-zone trip limits (millidegrees Celsius) read from the kernel
/// thermal zone (`passive`/`active` trip with hysteresis, and `hot`/`critical` trip).
/// If the machine provides no valid limits, [`ZoneBackstop`] fails closed at [`COOLING_MAX`] (S1).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct ZoneLimits {
    pub(crate) release_mc: i64,
    pub(crate) hot_mc: i64,
    pub(crate) critical_mc: i64,
}

impl ZoneLimits {
    pub(crate) const fn is_valid(&self) -> bool {
        self.release_mc > 0 && self.release_mc <= self.hot_mc && self.critical_mc > self.hot_mc
    }
}

/// Cooling level for a measured thermal-zone die temperature against machine-supplied trip
/// limits: 0 below `hot_mc`, 1..=7 linearly across `hot_mc..critical_mc`, and [`COOLING_MAX`]
/// at or above `critical_mc` (or whenever `limits` are invalid).
pub(crate) fn zone_heat_level(mc: i64, limits: ZoneLimits) -> u32 {
    if !limits.is_valid() {
        return COOLING_MAX;
    }
    if mc < limits.hot_mc {
        0
    } else if mc >= limits.critical_mc {
        COOLING_MAX
    } else {
        let span = limits.critical_mc - limits.hot_mc;
        let delta = mc - limits.hot_mc;
        let steps = (COOLING_MAX - 1) as i64;
        (((delta * steps) / span) as u32 + 1).min(COOLING_MAX)
    }
}

/// Fail-closed thermal backstop driven by kernel SoC die thermal zones when a GPU DT node
/// has no `apple,pmp` telemetry bridge. Starts at [`COOLING_MAX`] (S1), requires
/// [`ZONE_CONFIRM_READS`] distinct fresh sensor polls with valid machine-supplied trip limits
/// before stepping down gradually, and immediately clamps back to [`COOLING_MAX`] on missing,
/// stale, unconfigured, or critical readings.
pub(crate) struct ZoneBackstop {
    level: u32,
    hard: bool,
    good_reads: u32,
    last_seq: u64,
    good_ns: u64,
    release_ns: u64,
}

impl ZoneBackstop {
    pub(crate) const fn new() -> Self {
        Self {
            level: COOLING_MAX,
            hard: true,
            good_reads: 0,
            last_seq: 0,
            good_ns: 0,
            release_ns: 0,
        }
    }

    pub(crate) const fn level(&self) -> u32 {
        self.level
    }

    /// Incorporates one sampled temperature (millidegrees Celsius), poller sequence `seq`,
    /// machine-supplied `limits`, or sensor `error` at `now`.
    pub(crate) fn sample(
        &mut self,
        now: u64,
        seq: u64,
        mc: i64,
        limits: Option<ZoneLimits>,
        error: bool,
    ) -> u32 {
        let Some(limits) = limits.filter(ZoneLimits::is_valid) else {
            self.level = COOLING_MAX;
            self.hard = true;
            self.good_reads = 0;
            if seq != 0 {
                self.last_seq = seq;
            }
            self.good_ns = now;
            self.release_ns = now;
            return self.level;
        };

        if error || seq == 0 || mc <= 0 || mc >= limits.critical_mc {
            self.level = COOLING_MAX;
            self.hard = true;
            self.good_reads = 0;
            if seq != 0 {
                self.last_seq = seq;
            }
            self.good_ns = now;
            self.release_ns = now;
            return self.level;
        }

        let fresh = seq != self.last_seq;
        if self.hard {
            if mc < limits.hot_mc {
                let due = self.good_reads == 0 || now.wrapping_sub(self.good_ns) >= ZONE_CONFIRM_NS;
                if fresh && due {
                    self.last_seq = seq;
                    self.good_ns = now;
                    self.good_reads = self.good_reads.saturating_add(1).min(ZONE_CONFIRM_READS);
                }
                if self.good_reads >= ZONE_CONFIRM_READS {
                    self.hard = false;
                }
            } else {
                self.last_seq = seq;
                self.good_reads = 0;
                self.good_ns = now;
            }
            self.release_ns = now;
            if self.hard {
                return self.level;
            }
        }

        if fresh {
            self.last_seq = seq;
        }
        let target = zone_heat_level(mc, limits);
        if target > self.level {
            self.level = target;
            self.release_ns = now;
        } else if fresh
            && mc < limits.release_mc
            && now.wrapping_sub(self.release_ns) >= ZONE_RELEASE_NS
        {
            self.level = self.level.saturating_sub(1);
            self.release_ns = now;
        } else if mc >= limits.release_mc {
            self.release_ns = now;
        }
        self.level
    }

    /// Retains a cooling-device governor restriction inside the gradual release envelope.
    pub(crate) fn observe(&mut self, now: u64, governor: u32) {
        let clamped = governor.min(COOLING_MAX);
        if clamped > self.level {
            self.level = clamped;
            self.release_ns = now;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The ten-row tables' top state; the J773g cases name theirs.
    const TOP: u32 = TOP_DEFAULT;

    #[test]
    fn provider_sampling_is_independent_of_completion_polling() {
        assert!(read_due(None, 0));
        assert!(!read_due(Some(0), 0));
        assert!(!read_due(Some(100_000_000), 104_999_999));
        assert!(read_due(Some(100_000_000), 105_000_000));
        assert!(read_due(Some(u64::MAX - SAMPLE_NS + 1), 0));
    }

    #[test]
    fn initialized_selectors_and_user_caps_bound_every_request() {
        assert_eq!(COOLING_MAX, TOP_DEFAULT - 1);
        for configured in [0, 1, 7, 9, 10, u32::MAX] {
            for ceiling in [0, 1, 5, 9, 10, u32::MAX] {
                for user in [0, 1, 3, 9, 10, u32::MAX] {
                    let value = limit(configured, ceiling, user);
                    assert!((1..=PSTATE_MAX).contains(&value));
                    assert!(value <= configured.max(1));
                    assert!(value <= ceiling.max(1));
                    assert!(value <= user.max(1));
                }
            }
        }
    }

    #[test]
    fn utilization_is_a_bounded_share_of_the_window() {
        let w = WINDOW_NS;
        assert_eq!(utilization(0, w), 0);
        assert_eq!(utilization(w / 4, w), 25);
        assert_eq!(utilization(w, w), 100);
        assert_eq!(utilization(2 * w, w), 100);
        assert_eq!(utilization(0, 0), 100);
        assert_eq!(utilization(u64::MAX, u64::MAX), 100);
    }

    #[test]
    fn the_average_decays_and_restarts_after_a_gap() {
        let w = WINDOW_NS;
        assert_eq!(smooth(100, 0, w), 75);
        assert_eq!(smooth(0, 100, w), 25);
        assert_eq!(smooth(100, 0, 4 * w), 0);
        assert_eq!(smooth(u32::MAX, u32::MAX, w), 100);
        let mut average = 100;
        let mut windows = 0;
        while demand(TOP, 0, average, TOP) != FLOOR {
            average = smooth(average, 0, w);
            windows += 1;
        }
        assert!(windows <= 10);
    }

    #[test]
    fn demand_rises_fast_and_falls_on_the_average() {
        assert_eq!(demand(FLOOR, 100, 0, TOP), TOP);
        assert_eq!(demand(FLOOR, 70, 0, TOP), TOP);
        assert_eq!(demand(4, 50, 0, TOP), 5);
        assert_eq!(demand(TOP, 50, 0, TOP), TOP);
        // A short dip under a busy average holds the state.
        assert_eq!(demand(TOP, 5, 80, TOP), TOP);
        assert_eq!(demand(6, 30, 30, TOP), 6);
        assert_eq!(demand(6, 20, 20, TOP), 5);
        assert_eq!(demand(TOP, 0, 5, TOP), FLOOR);
        // A light 60 fps load near 25% busy settles on the floor.
        assert_eq!(demand(FLOOR, 25, 24, TOP), FLOOR);
        assert_eq!(demand(1, 30, 30, TOP), FLOOR);
        for current in [0, 1, FLOOR, 7, TOP, u32::MAX] {
            for util in [0, 20, 50, 70, 100, u32::MAX] {
                for average in [0, 9, 20, 30, 100, u32::MAX] {
                    let next = demand(current, util, average, TOP);
                    assert!((FLOOR..=TOP).contains(&next));
                }
            }
        }
    }

    const T: u64 = 1_000_000_000;

    /// A governor whose first window closed idle at `T`.
    fn started() -> Governor {
        let mut governor = Governor::new();
        assert_eq!(governor.govern(T, TOP, false), FLOOR);
        assert_eq!(governor.average, 0);
        governor
    }

    #[test]
    fn busy_time_runs_from_publication_to_drain() {
        let w = WINDOW_NS;
        let mut g = started();
        // A second publication before the drain does not restart the span.
        g.busy(T + 1_000_000);
        g.busy(T + 3_000_000);
        g.idle(T + 6_000_000);
        // A drain without a publication adds nothing.
        g.idle(T + 9_000_000);
        // A service before the window is due changes nothing.
        assert_eq!(g.govern(T + w - 1, TOP, false), FLOOR);
        assert_eq!(g.average, 0);
        // 5 ms of 20 ms: 25%, a quarter of it in the average.
        assert_eq!(g.govern(T + w, TOP, false), FLOOR);
        assert_eq!(g.average, 6);
    }

    #[test]
    fn a_busy_engine_counts_to_each_window_end() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        assert!(!g.decaying());
        // Busy through the window: all of it counts, and the load gets the top state.
        assert_eq!(g.govern(T + w, TOP, true), TOP);
        assert_eq!(g.average, 25);
        // Busy above the floor: the engine's own polls govern it, not the idle re-arm.
        assert!(!g.decaying());
        // The span carries on from the window's end, not from its publication.
        g.idle(T + w + w / 2);
        assert_eq!(g.govern(T + 2 * w, TOP, false), TOP);
        assert_eq!(g.average, (3 * 25 + 50) / 4);
    }

    #[test]
    fn flights_dropped_without_a_drain_end_the_busy_span() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        // Recovery dropped the flights and reported no drain: the next service,
        // finding none outstanding, ends the span there.
        assert_eq!(g.govern(T + w / 2, TOP, false), FLOOR);
        assert!(!g.decaying());
        assert_eq!(g.govern(T + w, TOP, false), FLOOR + 1);
        assert_eq!(g.average, 50 / 4);
        // Idle above the floor: the poll keeps coming back.
        assert!(g.decaying());
        // A late drain report counts nothing more.
        g.idle(T + w + 1);
        assert_eq!(g.govern(T + 2 * w, TOP, false), FLOOR);
        assert!(!g.decaying());
    }

    #[test]
    fn an_idle_engine_is_polled_back_to_the_floor_and_then_left_alone() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        for n in 1..=8 {
            assert_eq!(g.govern(T + n * w, TOP, true), TOP);
        }
        let mut now = T + 8 * w;
        g.idle(now);
        // As in run_poll: an idle pass re-arms one window later while decaying.
        let mut passes = 0;
        while g.decaying() {
            now += w;
            g.govern(now, TOP, false);
            passes += 1;
            assert!(passes <= 10);
        }
        assert_eq!(g.demand, FLOOR);
        assert!(passes > 1);
        // At the floor nothing re-arms, and a later service holds it there.
        assert_eq!(g.govern(now + 10 * w, TOP, false), FLOOR);
        assert!(!g.decaying());
        // A long sleep between passes restarts the average and drops at once.
        let mut g = started();
        g.busy(T);
        assert_eq!(g.govern(T + w, TOP, true), TOP);
        g.idle(T + w);
        assert_eq!(g.govern(T + 6 * w, TOP, false), FLOOR);
        assert!(!g.decaying());
    }

    #[test]
    fn ceilings_bound_the_published_state_even_below_the_floor() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        // A thermal or user ceiling under load holds the request to it.
        assert_eq!(g.govern(T + w, 5, true), 5);
        // A ceiling below the floor is published as it is.
        assert_eq!(g.govern(T + 2 * w, 1, true), 1);
        assert_eq!(g.govern(T + 2 * w + 1, 2, true), 2);
        // Released, the load's request applies at once.
        assert_eq!(g.govern(T + 2 * w + 2, TOP, true), TOP);
        // Idle under a ceiling below the floor: one window gives it all back.
        g.idle(T + 3 * w);
        assert!(g.decaying());
        assert_eq!(g.govern(T + 4 * w, 1, false), 1);
        assert_eq!(g.demand, FLOOR);
        assert!(!g.decaying());
    }

    #[test]
    fn the_board_table_sets_the_top_state() {
        // J713/J604: ten rows, top S9. J773g: eleven rows, top S10 (1578 MHz on the Mac mini).
        assert_eq!(top_of(10), 9);
        assert_eq!(top_of(11), 10);
        assert_eq!(top_of(12), PSTATE_MAX);
        assert_eq!(top_of(0), FLOOR);
        for top in [9, 10] {
            assert_eq!(demand(FLOOR, 100, 0, top), top);
            assert_eq!(demand(top, 50, 80, top), top);
            assert_eq!(demand(top, 0, 5, top), FLOOR);
            let mut g = Governor::new();
            g.top = top;
            assert_eq!(g.govern(T, top, false), FLOOR);
            g.busy(T);
            assert_eq!(g.govern(T + WINDOW_NS, top, true), top);
            // A ceiling still bounds the request.
            assert_eq!(g.govern(T + 2 * WINDOW_NS, top - 1, true), top - 1);
        }
    }

    #[test]
    fn cooling_keeps_one_state_per_level_and_s1_at_full_cooling() {
        // Ten-row tables: unchanged, 9 - level, S1 at level 8.
        for level in 0..=COOLING_MAX {
            assert_eq!(cooling_ceiling(9, level), 9 - level);
        }
        // Eleven rows: S10..S3 for levels 0..7, S1 at full cooling.
        assert_eq!(cooling_ceiling(10, 0), 10);
        assert_eq!(cooling_ceiling(10, 7), 3);
        assert_eq!(cooling_ceiling(10, COOLING_MAX), 1);
        assert_eq!(cooling_ceiling(10, u32::MAX), 1);
        assert_eq!(limit(10, cooling_ceiling(10, 0), 10), 10);
    }

    #[test]
    fn fast_ramp_raises_active_bursts_while_preserving_decay_and_cooling_ceiling() {
        let mut g = started();
        g.top = PSTATE_MAX;
        // Simulate d=1 back-to-back burst (80% busy) from FLOOR: reaches S10 at 5 ms.
        let mut t = T;
        for _ in 0..20 {
            g.busy(t);
            g.idle(t + 200_000);
            t += 250_000;
        }
        assert_eq!(t, T + RAMP_WINDOW_NS);
        g.busy(t);
        assert!(g.ramp_due(t, PSTATE_MAX));
        assert_eq!(g.govern(t, PSTATE_MAX, true), PSTATE_MAX);
        assert!(!g.ramp_due(t, PSTATE_MAX));

        // Moderate 50% active load steps +1 (aligned with demand()), never jumps to top
        // and never decays before the full 20 ms window.
        let mut g = started();
        g.top = PSTATE_MAX;
        g.busy(T);
        g.idle(T + 2_500_000);
        g.busy(T + RAMP_WINDOW_NS);
        assert!(g.ramp_due(T + RAMP_WINDOW_NS, PSTATE_MAX));
        assert_eq!(g.govern(T + RAMP_WINDOW_NS, PSTATE_MAX, true), FLOOR + 1);
        g.idle(T + RAMP_WINDOW_NS);
        assert_eq!(g.govern(T + WINDOW_NS - 1, PSTATE_MAX, false), FLOOR + 1);

        // Light 60 Hz compositor frames (2.5 ms busy every 16.67 ms -> 15% over 20 ms)
        // never pump to top and settle back at FLOOR.
        let mut g = started();
        g.top = PSTATE_MAX;
        let mut now = T;
        for _ in 0..10 {
            g.busy(now);
            assert!(g.govern(now + 2_500_000, PSTATE_MAX, true) <= FLOOR + 1);
            g.idle(now + 2_500_000);
            now += 16_666_667;
            let state = g.govern(now, PSTATE_MAX, false);
            assert!(state <= FLOOR + 1);
        }
        assert_eq!(g.demand, FLOOR);

        // Thermal cooling ceiling clamps stored demand too: relaxing by one level steps
        // cleanly to cap + 1 rather than jumping straight to top.
        let mut g = started();
        g.top = PSTATE_MAX;
        g.busy(T);
        let cap = cooling_ceiling(PSTATE_MAX, 5);
        assert_eq!(g.govern(T + RAMP_WINDOW_NS, cap, true), cap);
        assert_eq!(g.demand, cap);
        assert!(!g.ramp_due(T + RAMP_WINDOW_NS, cap));
        // Even if the ceiling opens to TOP right after the capped ramp, stored demand
        // stays at `cap` until new utilization above `cap` is observed.
        g.idle(T + RAMP_WINDOW_NS + 1);
        assert_eq!(g.govern(T + RAMP_WINDOW_NS + 2, PSTATE_MAX, false), cap);
        assert_eq!(g.demand, cap);
    }

    #[test]
    fn clpc_hold_keeps_floor_across_short_gaps_and_releases_on_expiry_or_reset() {
        let mut hold = ClpcHold::new();
        assert!(!hold.holding());
        assert_eq!(hold.report(true, T), Some(true));
        assert!(!hold.holding());
        // Short idle gap (< 2 ms): CLPC stays active without toggling.
        assert_eq!(hold.report(false, T + 200_000), None);
        assert!(hold.holding());
        assert!(!hold.expire(T + 200_000 + CLPC_IDLE_HOLD_NS - 1));
        assert_eq!(hold.report(true, T + 500_000), None);
        assert!(!hold.holding());
        // Full 2 ms idle expires and releases the CLPC floor (well below 60/120 Hz frames).
        assert_eq!(hold.report(false, T + 1_000_000), None);
        assert!(hold.holding());
        assert!(hold.expire(T + 1_000_000 + CLPC_IDLE_HOLD_NS));
        assert!(!hold.holding());
        assert!(!hold.expire(T + 1_000_000 + 2 * CLPC_IDLE_HOLD_NS));
        // Fail-stop / sleep drops CLPC immediately regardless of hold timer.
        assert_eq!(hold.report(true, T + 30_000_000), Some(true));
        assert_eq!(hold.report(false, T + 30_100_000), None);
        assert!(hold.holding());
        assert!(hold.release_now());
        assert!(!hold.holding());
        assert!(!hold.release_now());
    }

    #[test]
    fn active_burst_demand_ramps_low_duty_d1_while_preserving_120hz_and_idle_decay() {
        // Low-duty d=1 CPU-recording-bound burst: 170 us GPU + 600 us host gap (22% raw duty,
        // < 2 ms turnaround). Bridges short refill gaps after BURST_STREAK_ON and ramps to top.
        let mut g = started();
        g.top = PSTATE_MAX;
        let mut t = T;
        for _ in 0..8 {
            g.busy(t);
            g.idle(t + 170_000);
            t += 770_000;
        }
        assert!(t - T >= RAMP_WINDOW_NS);
        g.busy(t);
        assert!(g.ramp_due(t, PSTATE_MAX));
        assert_eq!(g.govern(t, PSTATE_MAX, true), PSTATE_MAX);

        // Once idle, burst streak resets after 2 ms and demand decays cleanly back to FLOOR.
        g.idle(t + 170_000);
        let mut now = t + 170_000;
        for _ in 0..20 {
            now += WINDOW_NS;
            let _ = g.govern(now, PSTATE_MAX, false);
        }
        assert_eq!(g.demand, FLOOR);
        assert!(!g.decaying());

        // 120 Hz desktop pacing (2 ms busy every 8.33 ms -> 6.33 ms gap >= 2 ms) never bridges
        // gaps and stays bounded at FLOOR.
        let mut g = started();
        g.top = PSTATE_MAX;
        let mut now = T;
        for _ in 0..16 {
            g.busy(now);
            assert!(g.govern(now + 2_000_000, PSTATE_MAX, true) <= FLOOR + 1);
            g.idle(now + 2_000_000);
            now += 8_333_333;
            assert!(g.govern(now, PSTATE_MAX, false) <= FLOOR + 1);
        }
        assert_eq!(g.demand, FLOOR);
    }

    #[test]
    fn zone_backstop_starts_fail_closed_releases_gradually_and_clamps_on_heat_or_fault() {
        let limits = ZoneLimits {
            release_mc: 75_000,
            hot_mc: 80_000,
            critical_mc: 94_000,
        };
        let mut z = ZoneBackstop::new();
        assert_eq!(z.level(), COOLING_MAX);
        assert_eq!(cooling_ceiling(8, z.level()), 1);

        // Missing or invalid machine trip limits keep the GPU clamped at S1 forever.
        for i in 1..=20u64 {
            assert_eq!(
                z.sample(T + i * ZONE_CONFIRM_NS, i, 55_000, None, false),
                COOLING_MAX
            );
        }
        let bad_limits = ZoneLimits {
            release_mc: 80_000,
            hot_mc: 80_000,
            critical_mc: 80_000,
        };
        assert_eq!(
            z.sample(T + 500_000_000, 21, 55_000, Some(bad_limits), false),
            COOLING_MAX
        );

        // Sensor errors or non-positive readings keep the GPU clamped at S1.
        assert_eq!(
            z.sample(T + 1_000_000_000, 22, 0, Some(limits), true),
            COOLING_MAX
        );
        assert_eq!(
            z.sample(T + 1_020_000_000, 23, 0, Some(limits), false),
            COOLING_MAX
        );

        // Repeated reads of a single cached temperature (unchanged seq) CANNOT satisfy
        // startup confirmation even as timestamps advance past 10 * ZONE_CONFIRM_NS.
        let base = T + 2_000_000_000;
        let cached_seq = 100u64;
        for step in 0..50u64 {
            assert_eq!(
                z.sample(
                    base + step * ZONE_CONFIRM_NS,
                    cached_seq,
                    55_000,
                    Some(limits),
                    false
                ),
                COOLING_MAX
            );
        }
        // Advancing past release interval with the same cached seq still stays at S1.
        assert_eq!(
            z.sample(
                base + 60 * ZONE_CONFIRM_NS + ZONE_RELEASE_NS,
                cached_seq,
                55_000,
                Some(limits),
                false
            ),
            COOLING_MAX
        );

        // 9 more distinct fresh sensor polls complete the 10-poll startup confirmation.
        let fresh_base = base + 60 * ZONE_CONFIRM_NS + 2 * ZONE_RELEASE_NS;
        for step in 1..ZONE_CONFIRM_READS as u64 {
            assert_eq!(
                z.sample(
                    fresh_base + step * ZONE_CONFIRM_NS,
                    cached_seq + step,
                    55_000,
                    Some(limits),
                    false
                ),
                COOLING_MAX
            );
        }
        // Gradual release: one cooling level per fresh poll every 200 ms below release_mc.
        let confirmed = fresh_base + (ZONE_CONFIRM_READS as u64 - 1) * ZONE_CONFIRM_NS;
        let mut seq = cached_seq + ZONE_CONFIRM_READS as u64;
        assert_eq!(
            z.sample(confirmed + ZONE_RELEASE_NS - 1, seq, 55_000, Some(limits), false),
            COOLING_MAX
        );
        for step in 1..=COOLING_MAX as u64 {
            seq += 1;
            assert_eq!(
                z.sample(
                    confirmed + step * ZONE_RELEASE_NS,
                    seq,
                    55_000,
                    Some(limits),
                    false
                ),
                COOLING_MAX - step as u32
            );
        }
        assert_eq!(z.level(), 0);
        assert_eq!(cooling_ceiling(8, z.level()), 8);

        // Progressive heat throttling across hot_mc..critical_mc, hard clamp at >= critical_mc.
        let hot_t = confirmed + 10 * ZONE_RELEASE_NS;
        seq += 1;
        assert_eq!(z.sample(hot_t, seq, 79_999, Some(limits), false), 0);
        seq += 1;
        assert_eq!(z.sample(hot_t + 10_000_000, seq, 80_000, Some(limits), false), 1);
        seq += 1;
        assert_eq!(z.sample(hot_t + 20_000_000, seq, 86_000, Some(limits), false), 4);
        // Holding between release_mc and hot_mc does not release cooling.
        seq += 1;
        assert_eq!(
            z.sample(hot_t + 500_000_000, seq, 75_000, Some(limits), false),
            4
        );
        seq += 1;
        assert_eq!(
            z.sample(hot_t + 1_000_000_000, seq, 79_000, Some(limits), false),
            4
        );
        // Critical heat immediately returns to hard COOLING_MAX (S1).
        seq += 1;
        assert_eq!(
            z.sample(
                hot_t + 1_010_000_000,
                seq,
                limits.critical_mc,
                Some(limits),
                false
            ),
            COOLING_MAX
        );
        assert_eq!(cooling_ceiling(8, z.level()), 1);

        // Different machine-supplied trip limits govern dynamically without code constants.
        let higher_limits = ZoneLimits {
            release_mc: 85_000,
            hot_mc: 90_000,
            critical_mc: 104_000,
        };
        let mut z2 = ZoneBackstop::new();
        for step in 1..=ZONE_CONFIRM_READS as u64 {
            z2.sample(
                base + step * ZONE_CONFIRM_NS,
                step,
                84_000,
                Some(higher_limits),
                false,
            );
        }
        let c2 = base + (ZONE_CONFIRM_READS as u64) * ZONE_CONFIRM_NS;
        for step in 1..=COOLING_MAX as u64 {
            z2.sample(
                c2 + step * ZONE_RELEASE_NS,
                ZONE_CONFIRM_READS as u64 + step,
                84_000,
                Some(higher_limits),
                false,
            );
        }
        assert_eq!(z2.level(), 0);
        // 89 C is still below higher_limits.hot_mc (90 C).
        assert_eq!(
            z2.sample(
                c2 + 9 * ZONE_RELEASE_NS,
                100,
                89_000,
                Some(higher_limits),
                false
            ),
            0
        );
        // Governor observation is retained in the release envelope.
        z2.observe(c2 + 9 * ZONE_RELEASE_NS, 5);
        assert_eq!(z2.level(), 5);
        assert_eq!(
            z2.sample(
                c2 + 9 * ZONE_RELEASE_NS + 1,
                101,
                84_000,
                Some(higher_limits),
                false
            ),
            5
        );
        assert_eq!(
            z2.sample(
                c2 + 10 * ZONE_RELEASE_NS,
                102,
                84_000,
                Some(higher_limits),
                false
            ),
            4
        );
    }
}
