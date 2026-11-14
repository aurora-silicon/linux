// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-file engine time, shared by all firmware backends.
//!
//! Queues retain their file's ledger until their last completion. Only authenticated,
//! successful completion timestamps are charged, before signalling the completion fence.
//! These are command timer ticks, not shader clock cycles. Never export them as drm-cycles.
//!
//! A backend whose firmware overlaps the commands of one queue charges through a [`Lane`]
//! per queue, so each engine of a queue counts the union of its stage intervals.

use core::sync::atomic::{AtomicU64, Ordering};

/// Engine time charged across all clients, in ns, for the GPU busy share.
pub(crate) static TOTAL_NS: AtomicU64 = AtomicU64::new(0);
/// Longest single stage charged since the governor last read it, in ns.
pub(crate) static MAX_NS: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy)]
pub(crate) enum Engine {
    Vertex,
    Fragment,
    Compute,
}

/// One ledger per DRM file, shared by all of its queues and VMs.
pub(crate) struct Client {
    ns: [AtomicU64; 3],
}

impl Client {
    pub(crate) const fn new() -> Self {
        Self {
            ns: [const { AtomicU64::new(0) }; 3],
        }
    }

    /// Called exactly once per retired stage, while its internal timestamps are retained.
    /// Missing, reversed or reset timestamps contribute nothing. Sub-nanosecond fractions
    /// are rounded down per stage. Saturation keeps fdinfo monotonic even on overflow.
    pub(crate) fn add(&self, engine: Engine, start: u64, end: u64, frequency_hz: u32) {
        if start == 0 || end <= start || frequency_hz == 0 {
            return;
        }
        let ns = ticks_to_ns(end - start, frequency_hz);
        TOTAL_NS.fetch_add(ns, Ordering::Relaxed);
        MAX_NS.fetch_max(ns, Ordering::Relaxed);
        let counter = &self.ns[engine as usize];
        let mut old = counter.load(Ordering::Relaxed);
        loop {
            match counter.compare_exchange_weak(
                old,
                old.saturating_add(ns),
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => break,
                Err(value) => old = value,
            }
        }
    }

    /// Vertex, fragment and compute, in ns. No cross-engine atomic snapshot is promised.
    pub(crate) fn snapshot(&self) -> [u64; 3] {
        self.ns.each_ref().map(|ns| ns.load(Ordering::Relaxed))
    }
}

/// A stage this far behind its lane's mark is not an overlap, since no stage runs this
/// long: the timestamps jumped, so the mark restarts at the stage instead of holding every
/// later charge back.
const DISCONTINUITY_S: u64 = 60;

/// The engine time of one queue, charged to its file's [`Client`].
///
/// Firmware can run several commands of one queue at once. A queue that keeps several
/// commands in flight sees their stage intervals overlap, and their sum then exceeds the
/// time the engine was busy: three seconds of compute per second at depth 4 on the A18 Pro.
/// A lane charges each engine the union of the queue's stage intervals instead. A queue's
/// stages retire in the order they started, so a mark of the latest end charged per engine
/// gives the exact union: a stage adds only its part after the mark. Queues of one file
/// still add up, one union each.
pub(crate) struct Lane<P> {
    client: P,
    covered: [AtomicU64; 3],
}

impl<P: core::ops::Deref<Target = Client>> Lane<P> {
    pub(crate) fn new(client: P) -> Self {
        Self {
            client,
            covered: [const { AtomicU64::new(0) }; 3],
        }
    }

    /// As [`Client::add`], for one retired stage of this lane's queue: only the part of
    /// `start..end` after the engine's mark is charged, and the mark moves to `end`.
    pub(crate) fn add(&self, engine: Engine, start: u64, end: u64, frequency_hz: u32) {
        if start == 0 || end <= start || frequency_hz == 0 {
            return;
        }
        let limit = u64::from(frequency_hz).saturating_mul(DISCONTINUITY_S);
        let mark = &self.covered[engine as usize];
        let mut covered = mark.load(Ordering::Relaxed);
        let from = loop {
            let jumped = covered.saturating_sub(start) > limit;
            let from = if jumped { start } else { start.max(covered) };
            if from >= end {
                return;
            }
            match mark.compare_exchange_weak(covered, end, Ordering::Relaxed, Ordering::Relaxed) {
                Ok(_) => break from,
                Err(value) => covered = value,
            }
        };
        self.client.add(engine, from, end, frequency_hz);
    }
}

fn ticks_to_ns(ticks: u64, frequency_hz: u32) -> u64 {
    const NSEC_PER_SEC: u64 = 1_000_000_000;
    let hz = u64::from(frequency_hz);
    // The remainder is smaller than a u32 frequency, so its product fits in u64.
    (ticks / hz)
        .saturating_mul(NSEC_PER_SEC)
        .saturating_add(ticks % hz * NSEC_PER_SEC / hz)
}

#[cfg(test)]
mod tests {
    use super::*;

    const HZ: u32 = 24_000_000;

    #[test]
    fn idle_and_invalid_intervals() {
        let client = Client::new();
        assert_eq!(client.snapshot(), [0; 3]);
        for (start, end, hz) in [
            (0, 100, HZ),
            (100, 0, HZ),
            (20, 10, HZ),
            (1, 1, HZ),
            (1, 2, 0),
        ] {
            client.add(Engine::Compute, start, end, hz);
        }
        assert_eq!(client.snapshot(), [0; 3]);
    }

    #[test]
    fn engine_and_client_isolation() {
        let a = Client::new();
        let b = Client::new();
        a.add(Engine::Vertex, 10, 24_010, HZ);
        a.add(Engine::Fragment, 20, 48_020, HZ);
        b.add(Engine::Compute, 30, 72_030, HZ);
        assert_eq!(a.snapshot(), [1_000_000, 2_000_000, 0]);
        assert_eq!(b.snapshot(), [0, 0, 3_000_000]);
    }

    #[test]
    fn overlapping_stages_are_independent() {
        let client = Client::new();
        client.add(Engine::Vertex, 1, 25, HZ);
        client.add(Engine::Fragment, 13, 37, HZ);
        assert_eq!(client.snapshot(), [1_000, 1_000, 0]);
    }

    #[test]
    fn conversion_matches_wide_reference() {
        for hz in [1, HZ, 1_000_000_000, u32::MAX] {
            for ticks in [0, 1, 23, 24, 25, u32::MAX as u64, u64::MAX / 2, u64::MAX] {
                let expected = (u128::from(ticks) * 1_000_000_000 / u128::from(hz))
                    .min(u128::from(u64::MAX)) as u64;
                assert_eq!(ticks_to_ns(ticks, hz), expected);
            }
        }
    }

    #[test]
    fn fractional_ticks_round_down() {
        assert_eq!(ticks_to_ns(1, HZ), 41);
        assert_eq!(ticks_to_ns(3, HZ), 125);
    }

    #[test]
    fn out_of_order_completions_accumulate() {
        let client = Client::new();
        client.add(Engine::Compute, 100, 124, HZ);
        client.add(Engine::Compute, 1, 25, HZ);
        assert_eq!(client.snapshot(), [0, 0, 2_000]);
    }

    #[test]
    fn saturation_is_monotonic() {
        let client = Client::new();
        client.add(Engine::Compute, 1, u64::MAX, 1);
        assert_eq!(client.snapshot()[2], u64::MAX);
        client.add(Engine::Compute, 1, 25, HZ);
        assert_eq!(client.snapshot()[2], u64::MAX);
    }

    /// A queue `depth` commands deep: one command retires every `period` ticks, and each
    /// stays in flight for `depth` periods, so neighbouring intervals overlap.
    fn pipeline(lane: &Lane<&Client>, base: u64, period: u64, depth: u64, commands: u64) {
        for i in 0..commands {
            let start = base + i * period;
            lane.add(Engine::Compute, start, start + depth * period, HZ);
        }
    }

    #[test]
    fn a_deep_queue_counts_its_busy_time_once() {
        // 1,181 us per command, as the A18 Pro's ALU load at depth 4.
        const PERIOD: u64 = 28_344;
        for depth in [1, 4, 8] {
            let client = Client::new();
            let lane = Lane::new(&client);
            pipeline(&lane, 1_000, PERIOD, depth, 1_000);
            let wall = ticks_to_ns((1_000 - 1 + depth) * PERIOD, HZ);
            assert_eq!(client.snapshot()[2], wall, "depth {depth}");
            // The sum of the intervals is `depth` times the busy time.
            let summed = Client::new();
            for i in 0..1_000 {
                let start = 1_000 + i * PERIOD;
                summed.add(Engine::Compute, start, start + depth * PERIOD, HZ);
            }
            assert_eq!(summed.snapshot()[2], 1_000 * ticks_to_ns(depth * PERIOD, HZ));
        }
    }

    #[test]
    fn idle_gaps_and_disjoint_stages() {
        let client = Client::new();
        let lane = Lane::new(&client);
        lane.add(Engine::Compute, 24, 48, HZ);
        lane.add(Engine::Compute, 96, 120, HZ);
        lane.add(Engine::Compute, 100, 110, HZ); // inside the previous stage
        lane.add(Engine::Compute, 0, 100, HZ); // no start: nothing
        assert_eq!(client.snapshot(), [0, 0, 2_000]);
        // A covered stage leaves the mark where it was.
        lane.add(Engine::Compute, 104, 144, HZ);
        assert_eq!(client.snapshot(), [0, 0, 3_000]);
    }

    #[test]
    fn engines_keep_their_own_marks() {
        let client = Client::new();
        let lane = Lane::new(&client);
        lane.add(Engine::Vertex, 1, 25, HZ);
        lane.add(Engine::Fragment, 13, 37, HZ);
        lane.add(Engine::Vertex, 13, 37, HZ);
        assert_eq!(client.snapshot(), [1_500, 1_000, 0]);
    }

    #[test]
    fn queues_of_one_file_add_up() {
        let client = Client::new();
        let a = Lane::new(&client);
        let b = Lane::new(&client);
        a.add(Engine::Compute, 1, 49, HZ);
        b.add(Engine::Compute, 1, 49, HZ);
        a.add(Engine::Compute, 25, 49, HZ);
        assert_eq!(client.snapshot(), [0, 0, 4_000]);
    }

    #[test]
    fn a_timestamp_jump_restarts_the_mark() {
        let client = Client::new();
        let lane = Lane::new(&client);
        let far = 61 * u64::from(HZ);
        lane.add(Engine::Compute, 1, far, HZ);
        let before = client.snapshot()[2];
        // Far behind the mark: charged in full, and the mark follows it.
        lane.add(Engine::Compute, 24, 48, HZ);
        lane.add(Engine::Compute, 36, 72, HZ);
        assert_eq!(client.snapshot()[2] - before, 2_000);
        // Within the limit, a stage behind the mark is an overlap.
        let near = Client::new();
        let lane = Lane::new(&near);
        lane.add(Engine::Compute, 1, 59 * u64::from(HZ), HZ);
        let before = near.snapshot()[2];
        lane.add(Engine::Compute, 24, 48, HZ);
        assert_eq!(near.snapshot()[2], before);
    }

    #[test]
    fn concurrent_retirements_never_exceed_the_union() {
        let client = std::sync::Arc::new(Client::new());
        let lane = std::sync::Arc::new(Lane::new(client.clone()));
        let threads: std::vec::Vec<_> = (0..8)
            .map(|_| {
                let lane = lane.clone();
                std::thread::spawn(move || {
                    for i in 0..10_000u64 {
                        lane.add(Engine::Compute, 24 + i * 24, 24 + i * 24 + 96, HZ);
                    }
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        let union = ticks_to_ns((10_000 - 1) * 24 + 96, HZ);
        let charged = client.snapshot()[2];
        assert!(charged > 0 && charged <= union, "{charged} > {union}");
    }

    #[test]
    fn concurrent_queues_share_one_file() {
        let client = std::sync::Arc::new(Client::new());
        let threads: std::vec::Vec<_> = (0..8)
            .map(|_| {
                let client = client.clone();
                std::thread::spawn(move || {
                    for _ in 0..10_000 {
                        client.add(Engine::Compute, 1, 25, HZ);
                    }
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        assert_eq!(client.snapshot(), [0, 0, 80_000_000]);
    }
}
