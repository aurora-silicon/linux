// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Composition and sampling cadence for the retained firmware ceiling, and the
//! utilization governor below it.
//!
//! Temperature decoding and regulation belong to the PMP provider and the
//! shared C cooling policy. This module defines no temperature thresholds.

pub(crate) const PSTATE_MAX: u32 = 9;
pub(crate) const COOLING_MAX: u32 = 8;
pub(crate) const SAMPLE_NS: u64 = 5_000_000;
/// Utilization window of the governor.
pub(crate) const WINDOW_NS: u64 = 20_000_000;
/// Lowest governed state. On the J713 S1-S3 draw under 0.1 W for a desktop
/// load and S4 several times that; S3 runs at 2.4 times the clock of S1.
pub(crate) const FLOOR: u32 = 3;

/// Completion polling must not turn into a PMP MMIO read on every pass.
pub(crate) fn read_due(last: Option<u64>, now: u64) -> bool {
    last.is_none_or(|last| now.wrapping_sub(last) >= SAMPLE_NS)
}

/// Zero is not an unlimited selector in the firmware. Every published state
/// is 1..=9, bounded by the initialized Main table and the user ceiling.
pub(crate) fn limit(configured: u32, ceiling: u32, user: u32) -> u32 {
    ceiling.min(PSTATE_MAX).min(configured).min(user).max(1)
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
pub(crate) fn demand(current: u32, util: u32, average: u32) -> u32 {
    let next = if util >= 70 {
        PSTATE_MAX
    } else if util >= 40 {
        current.saturating_add(1)
    } else if average < 10 {
        FLOOR
    } else if average < 25 {
        current.saturating_sub(1)
    } else {
        current
    };
    next.clamp(FLOOR, PSTATE_MAX)
}

/// Busy-time accounting over the governor's windows. The engine reports each
/// publication and drain; each power service closes the window once it is due.
pub(crate) struct Governor {
    /// The requested state and the smoothed utilization.
    pub(crate) demand: u32,
    pub(crate) average: u32,
    window_ns: u64,
    busy_since: Option<u64>,
    busy_ns: u64,
}

impl Governor {
    pub(crate) const fn new() -> Self {
        Self {
            demand: FLOOR,
            average: 0,
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
            self.demand = demand(self.demand.min(limit), util, self.average);
            self.window_ns = now;
            self.busy_ns = 0;
        }
        self.demand.min(limit)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert_eq!(COOLING_MAX, PSTATE_MAX - 1);
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
        while demand(PSTATE_MAX, 0, average) != FLOOR {
            average = smooth(average, 0, w);
            windows += 1;
        }
        assert!(windows <= 10);
    }

    #[test]
    fn demand_rises_fast_and_falls_on_the_average() {
        assert_eq!(demand(FLOOR, 100, 0), PSTATE_MAX);
        assert_eq!(demand(FLOOR, 70, 0), PSTATE_MAX);
        assert_eq!(demand(4, 50, 0), 5);
        assert_eq!(demand(PSTATE_MAX, 50, 0), PSTATE_MAX);
        // A short dip under a busy average holds the state.
        assert_eq!(demand(PSTATE_MAX, 5, 80), PSTATE_MAX);
        assert_eq!(demand(6, 30, 30), 6);
        assert_eq!(demand(6, 20, 20), 5);
        assert_eq!(demand(PSTATE_MAX, 0, 5), FLOOR);
        // A light 60 fps load near 25% busy settles on the floor.
        assert_eq!(demand(FLOOR, 25, 24), FLOOR);
        assert_eq!(demand(1, 30, 30), FLOOR);
        for current in [0, 1, FLOOR, 7, PSTATE_MAX, u32::MAX] {
            for util in [0, 20, 50, 70, 100, u32::MAX] {
                for average in [0, 9, 20, 30, 100, u32::MAX] {
                    let next = demand(current, util, average);
                    assert!((FLOOR..=PSTATE_MAX).contains(&next));
                }
            }
        }
    }

    const T: u64 = 1_000_000_000;

    /// A governor whose first window closed idle at `T`.
    fn started() -> Governor {
        let mut governor = Governor::new();
        assert_eq!(governor.govern(T, PSTATE_MAX, false), FLOOR);
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
        assert_eq!(g.govern(T + w - 1, PSTATE_MAX, false), FLOOR);
        assert_eq!(g.average, 0);
        // 5 ms of 20 ms: 25%, a quarter of it in the average.
        assert_eq!(g.govern(T + w, PSTATE_MAX, false), FLOOR);
        assert_eq!(g.average, 6);
    }

    #[test]
    fn a_busy_engine_counts_to_each_window_end() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        assert!(!g.decaying());
        // Busy through the window: all of it counts, and the load gets the top state.
        assert_eq!(g.govern(T + w, PSTATE_MAX, true), PSTATE_MAX);
        assert_eq!(g.average, 25);
        // Busy above the floor: the engine's own polls govern it, not the idle re-arm.
        assert!(!g.decaying());
        // The span carries on from the window's end, not from its publication.
        g.idle(T + w + w / 2);
        assert_eq!(g.govern(T + 2 * w, PSTATE_MAX, false), PSTATE_MAX);
        assert_eq!(g.average, (3 * 25 + 50) / 4);
    }

    #[test]
    fn flights_dropped_without_a_drain_end_the_busy_span() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        // Recovery dropped the flights and reported no drain: the next service,
        // finding none outstanding, ends the span there.
        assert_eq!(g.govern(T + w / 2, PSTATE_MAX, false), FLOOR);
        assert!(!g.decaying());
        assert_eq!(g.govern(T + w, PSTATE_MAX, false), FLOOR + 1);
        assert_eq!(g.average, 50 / 4);
        // Idle above the floor: the poll keeps coming back.
        assert!(g.decaying());
        // A late drain report counts nothing more.
        g.idle(T + w + 1);
        assert_eq!(g.govern(T + 2 * w, PSTATE_MAX, false), FLOOR);
        assert!(!g.decaying());
    }

    #[test]
    fn an_idle_engine_is_polled_back_to_the_floor_and_then_left_alone() {
        let w = WINDOW_NS;
        let mut g = started();
        g.busy(T);
        for n in 1..=8 {
            assert_eq!(g.govern(T + n * w, PSTATE_MAX, true), PSTATE_MAX);
        }
        let mut now = T + 8 * w;
        g.idle(now);
        // As in run_poll: an idle pass re-arms one window later while decaying.
        let mut passes = 0;
        while g.decaying() {
            now += w;
            g.govern(now, PSTATE_MAX, false);
            passes += 1;
            assert!(passes <= 10);
        }
        assert_eq!(g.demand, FLOOR);
        assert!(passes > 1);
        // At the floor nothing re-arms, and a later service holds it there.
        assert_eq!(g.govern(now + 10 * w, PSTATE_MAX, false), FLOOR);
        assert!(!g.decaying());
        // A long sleep between passes restarts the average and drops at once.
        let mut g = started();
        g.busy(T);
        assert_eq!(g.govern(T + w, PSTATE_MAX, true), PSTATE_MAX);
        g.idle(T + w);
        assert_eq!(g.govern(T + 6 * w, PSTATE_MAX, false), FLOOR);
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
        assert_eq!(g.govern(T + 2 * w + 2, PSTATE_MAX, true), PSTATE_MAX);
        // Idle under a ceiling below the floor: one window gives it all back.
        g.idle(T + 3 * w);
        assert!(g.decaying());
        assert_eq!(g.govern(T + 4 * w, 1, false), 1);
        assert_eq!(g.demand, FLOOR);
        assert!(!g.decaying());
    }
}
