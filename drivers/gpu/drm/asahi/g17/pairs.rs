// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Render pairs one logical render queue may hold: two on the G17D (M5 Ultra), one elsewhere.
//!
//! A second pair lets one queue's depth run on two physical render rings. It comes only from
//! surplus: the QID budget ([`super::kick::render_reserve`]) keeps counting one pair per open
//! queue, admission claims a sibling only while a pair can still be built, and an unused
//! sibling is reassigned to an owner that needs a first pair. An owner's overlapping renders on
//! its two pairs are ordered in firmware ([`super::hazard::plan`]).
//!
//! A queue samples the count once, when it is created ([`per_owner`]): its scheduler credits,
//! render pool and pair limit all follow that one value.

use core::sync::atomic::{AtomicU8, Ordering::Relaxed};

use crate::hw::t8140::queues::RENDER_PAIRS_MAX;

/// G17D's count.
const G17D_PAIRS: u8 = RENDER_PAIRS_MAX as u8;

static PAIRS: AtomicU8 = AtomicU8::new(1);

/// The count for the probed board, set once at probe (one GPU per system).
pub(crate) fn select_board(board: &crate::g17::fw::m5::initdata::Board) {
    PAIRS.store(if board.g17d_forms() { G17D_PAIRS } else { 1 }, Relaxed);
}

/// Render pairs a logical render queue created now may hold.
pub(crate) fn per_owner() -> u8 {
    PAIRS.load(Relaxed)
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
