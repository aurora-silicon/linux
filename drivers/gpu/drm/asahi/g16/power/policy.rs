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
            busy_ns: 0,
        }
    }

    /// The engine published work; it stays busy until [`Governor::idle`].
    pub(crate) fn busy(&mut self, now: u64) {
        self.busy_since.get_or_insert(now);
    }

    /// The engine drained.
    pub(crate) fn idle(&mut self, now: u64) {
        if let Some(since) = self.busy_since.take() {
            self.busy_ns += now.saturating_sub(since);
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
}
