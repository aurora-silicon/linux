// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Render pairs one logical render queue may hold: two on G17C and G17D (4+ clusters),
//! one elsewhere.
//!
//! A second pair lets one queue's depth run on two physical render rings. It comes only from
//! surplus: the QID budget ([`super::kick::render_reserve`]) reserves one pair for every live
//! render demand (open queues, live siblings, and teardown-held released pairs) plus headroom for
//! future queues, admission claims a released pair only while surplus first-pair capacity remains
//! and builds a fresh sibling only while a pair can also be built, and an unused sibling can still
//! be reassigned to an owner that needs a first pair. An owner's overlapping renders on its two
//! pairs are ordered in firmware ([`super::hazard::plan`]).
//!
//! A queue samples the count once, when it is created ([`per_owner`]): its scheduler credits,
//! render pool and pair limit all follow that one value.

use core::sync::atomic::{AtomicU8, Ordering::Relaxed};

use crate::hw::t8140::queues::RENDER_PAIRS_MAX;

/// Multi-pair count for large multi-cluster M5 parts (G17C/G17D).
const MULTI_PAIRS: u8 = RENDER_PAIRS_MAX as u8;

static PAIRS: AtomicU8 = AtomicU8::new(1);

/// The count for the probed board, set once at probe (one GPU per system).
pub(crate) fn select_board(board: &crate::g17::fw::m5::initdata::Board) {
    PAIRS.store(
        if board.g17d_forms() || board.clusters >= 4 {
            MULTI_PAIRS
        } else {
            1
        },
        Relaxed,
    );
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
