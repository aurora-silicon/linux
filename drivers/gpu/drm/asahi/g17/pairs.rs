// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Render pairs one logical render queue may hold: two on G17C and G17D (4+ clusters),
//! one elsewhere.
//!
//! A second pair lets one queue's depth run on two physical render rings. It comes only from
//! surplus: the QID budget ([`super::kick::render_reserve`]) reserves one pair for every live
//! render demand (open queues, non-reclaimable siblings, and teardown-held released pairs) plus
//! headroom for future queues, admission claims a released pair only while surplus first-pair
//! capacity remains and builds a fresh sibling only while a pair can also be built, and an unused
//! sibling is reclaimed before refusing a new logical queue or reassigned to an owner that needs a
//! first pair. An owner's overlapping renders on its two pairs are ordered in firmware
//! ([`super::hazard::plan`]).
//!
//! Two pairs are enabled at 4+ GPU clusters (40-core G17C and 80-core G17D, 8-16 GiB render-pool
//! inventory), where a single physical render ring underutilizes the wider shader array across
//! back-to-back deep renders. Narrower M5 parts (1-cluster J813, 2-cluster J714, 2-4 GiB
//! inventory) keep one pair per queue to avoid the extra per-pair ring/graph/PB footprint and
//! doubled ray-query render-pool extras (`fw::render_rt::required_blocks`) until measured.
//!
//! A queue samples the count once, when it is created ([`per_owner`]): its scheduler credits,
//! render pool and pair limit all follow that one value.

use core::sync::atomic::{AtomicU8, Ordering::Relaxed};

use crate::hw::t8140::queues::RENDER_PAIRS_MAX;

/// Multi-pair count for large multi-cluster M5 parts (G17C/G17D).
const MULTI_PAIRS: u8 = RENDER_PAIRS_MAX as u8;

/// Maximum physical compute channels one logical compute owner may hold: the most firmware
/// compute queues any board spreads one command queue over ([`Board::compute_channels`], three
/// on M5 Pro). It sizes the per-owner channel arrays and the compute waits of a render kick, so
/// every channel of an owner costs one dependency slot in each of its consumers.
///
/// [`Board::compute_channels`]: crate::g17::fw::m5::initdata::Board::compute_channels
pub(crate) const COMPUTE_CHANNELS_MAX: usize = 3;

static PAIRS: AtomicU8 = AtomicU8::new(1);
static COMPUTE_CHANNELS: AtomicU8 = AtomicU8::new(1);

/// The counts for the probed board, set once at probe (one GPU per system).
pub(crate) fn select_board(board: &crate::g17::fw::m5::initdata::Board) {
    PAIRS.store(
        if board.clusters >= 4 { MULTI_PAIRS } else { 1 },
        Relaxed,
    );
    COMPUTE_CHANNELS.store(board.compute_channels(), Relaxed);
}

/// Render pairs a logical render queue created now may hold.
pub(crate) fn per_owner() -> u8 {
    PAIRS.load(Relaxed)
}

/// Physical compute channels a logical compute queue created now may hold.
pub(crate) fn compute_channels_per_owner() -> u8 {
    COMPUTE_CHANNELS.load(Relaxed)
}

/// GPU execution time of a compute kick, between the start and end timestamps of its firmware
/// execution record, from which the kick is worth a sibling channel.
///
/// Measured on M5 Pro: one physical QID streams queued kicks back to back for about 11 us each;
/// the same kicks spread over three QIDs take about 16.4 us each, because every kick then pays
/// about 5 us of firmware arbitration between QIDs. A sibling only pays for itself through
/// overlapped execution, so the kick must run long enough for that cost to vanish into it:
/// 100 us is where the 5 us of arbitration falls to 5% of the kick, while a kick of a few
/// microseconds would spend more on arbitration than it runs.
pub(crate) const SIBLING_EXECUTION_NS: u64 = 100_000;

/// [`SIBLING_EXECUTION_NS`] in the command timestamp ticks of an execution record.
const SIBLING_EXECUTION_TICKS: u64 = SIBLING_EXECUTION_NS
    * crate::hw::t8140::resources::COMMAND_TIMESTAMP_HZ
    / 1_000_000_000;

/// Consecutive heavy kicks ([`SIBLING_EXECUTION_NS`]) after which a primary channel's execution is
/// sustained: a single long kick among short ones (a first dispatch, a cold pipeline) does not
/// fan its queue out.
pub(crate) const SIBLING_SUSTAINED_KICKS: u8 = 2;

/// Kicks of an owner that must already be in flight on its compute channels before a new kick
/// may run beside them on a sibling channel.
///
/// A kick queued behind one in flight on the same ring starts the moment its predecessor ends,
/// while the host turns that completion into the next submission (60 to 100 us). Two overlapped
/// kicks that each fill the shader cores share them and end together, both rings then wait for
/// the host, and every pair of kicks pays that turnaround. With two kicks already in flight the
/// primary ring keeps one queued behind the overlap, so a sibling only adds overlap and never
/// exposes the turnaround: an owner keeping at most two kicks in flight runs exactly as on one
/// ring.
pub(crate) const SIBLING_MIN_IN_FLIGHT: usize = 2;

/// How long publication keeps to an owner's bound compute channels after no sibling could be found
/// (no reusable queue to bind or re-arm and no QID headroom) before it looks again.
pub(crate) const SIBLING_HEADROOM_BACKOFF_NS: u64 = 10_000_000;

/// Whether a new kick of an owner with `in_flight` kicks in flight on its compute channels may
/// overlap them on a sibling channel ([`SIBLING_MIN_IN_FLIGHT`]).
pub(crate) fn overlaps(in_flight: usize) -> bool {
    in_flight >= SIBLING_MIN_IN_FLIGHT
}

/// Folds the execution record `[start, end]` of a retired compute kick into its channel's run of
/// consecutive heavy kicks.
pub(crate) fn heavy_run(run: u8, start: u64, end: u64) -> u8 {
    if end.saturating_sub(start) >= SIBLING_EXECUTION_TICKS {
        run.saturating_add(1)
    } else {
        0
    }
}

/// Whether a primary compute channel with this run of heavy kicks runs sustained heavy work, the
/// only work whose independent kicks fan out onto sibling channels.
pub(crate) fn sustained(run: u8) -> bool {
    run >= SIBLING_SUSTAINED_KICKS
}

/// Renders a busy pair of a two-pair owner takes before its next render goes to
/// an idle sibling: 1 keeps a queue at depth 1-2 on one pair.
const SIBLING_BACKLOG: u8 = 1;
static BACKLOG: AtomicU8 = AtomicU8::new(SIBLING_BACKLOG);

/// A two-pair owner's next render stays on its least-loaded busy pair while
/// that pair holds at most this many renders.
pub(crate) fn sibling_backlog() -> u8 {
    BACKLOG.load(Relaxed)
}

#[cfg(CONFIG_KUNIT = "y")]
pub(crate) fn set_sibling_backlog(renders: u8) {
    BACKLOG.store(renders, Relaxed);
}


/// The pairs a queue of `queue` pairs uses for a render each of whose pairs reserves
/// `window_bytes` of the client's M5 compact window of `window` bytes (`None`: no M5 footprint).
/// Each pair maps its own copy of a layout's objects in that one window, so a second pair is
/// built and used only for a layout the window holds twice. Any other layout runs on one of the
/// queue's pairs, always the same one, as on a part with one pair per queue
/// (`Pool::acquire_render`). SUBMIT has already refused a layout that one pair cannot hold.
pub(crate) fn for_window(queue: u8, window_bytes: Option<u64>, window: u64) -> u8 {
    match window_bytes {
        Some(bytes) if queue > 1 && bytes.saturating_mul(u64::from(queue)) > window => 1,
        _ => queue,
    }
}

#[kernel::macros::kunit_tests(asahi_g17_pair_count)]
mod tests {
    use super::*;

    const WINDOW: u64 = 1 << 32;

    #[test]
    fn a_layout_the_window_holds_twice_keeps_both_pairs() {
        assert_eq!(for_window(2, Some(WINDOW / 2), WINDOW), 2);
        assert_eq!(for_window(2, Some(64 << 20), WINDOW), 2);
    }

    #[test]
    fn a_larger_layout_runs_on_one_pair() {
        assert_eq!(for_window(2, Some(WINDOW / 2 + 1), WINDOW), 1);
        assert_eq!(for_window(2, Some(WINDOW), WINDOW), 1);
    }

    #[test]
    fn a_one_pair_queue_and_a_render_without_footprint_are_unchanged() {
        assert_eq!(for_window(1, Some(WINDOW), WINDOW), 1);
        assert_eq!(for_window(2, None, WINDOW), 2);
    }
}
