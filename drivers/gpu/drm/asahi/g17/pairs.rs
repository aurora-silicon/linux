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
