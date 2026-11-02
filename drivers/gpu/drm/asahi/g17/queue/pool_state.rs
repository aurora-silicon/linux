// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! A retained compute pool's registration as the firmware sees it, carried across its owners: the
//! release transaction Neo's compute queues (`queue::compute::Queue`) and M5 lanes
//! (`queue::m5::Channel`) share for vacating and growing a pool.
//!
//! Populated: backed; nothing in flight releases it. ReleaseSent: its FreeListRelease is published
//! at that control cursor. Vacant: the firmware holds no registration, and any backing left is the
//! host's to free or replace. A pool is registered from the publication that names it to the
//! firmware (a Neo queue's installation, an M5 kick's configuration update) until a release of it
//! is consumed; a pool never registered goes vacant without a release.

use crate::g17::{
    channel::Rings,
    fw::channels::{ControlRecord, FreeListRelease},
    initdata::InitData,
};
use core::sync::atomic::{fence, Ordering};
use kernel::prelude::*;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum PoolPhase {
    Populated,
    ReleaseSent(u32),
    Vacant,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct PoolState {
    phase: PoolPhase,
    registered: bool,
    release_sent_ns: Option<u64>,
}

impl PoolState {
    /// A pool just built: backed, not yet registered.
    pub(crate) const FRESH: Self = Self {
        phase: PoolPhase::Populated,
        registered: false,
        release_sent_ns: None,
    };

    pub(crate) fn phase(&self) -> PoolPhase {
        self.phase
    }

    /// The firmware holds a registration: a change of backing needs a consumed release first.
    pub(crate) fn registered(&self) -> bool {
        self.registered
    }

    /// The publication that names the pool to the firmware is visible.
    pub(crate) fn register(&mut self) {
        self.registered = true;
    }

    /// Releases the pool: publishes `record` for a registered one, or marks an unregistered one
    /// vacant at once. Returns whether a record was published (the caller rings the control
    /// doorbell). EAGAIN: the control ring is full, and nothing changed.
    pub(crate) fn release(&mut self, init: &InitData, record: FreeListRelease) -> Result<bool> {
        if !self.registered {
            self.phase = PoolPhase::Vacant;
            self.release_sent_ns = None;
            return Ok(false);
        }
        let cursor = Rings::publish_control(init, &ControlRecord::FreeListRelease(record))?;
        self.sent(cursor, crate::g17::now_ns());
        fence(Ordering::SeqCst);
        Ok(true)
    }

    fn sent(&mut self, cursor: u32, now: u64) {
        self.phase = PoolPhase::ReleaseSent(cursor);
        self.release_sent_ns = Some(now);
    }

    /// Whether no release is in flight, or the firmware's control consumer (`consumer` of
    /// `producer`) passed it.
    pub(crate) fn release_consumed(&self, consumer: u32, producer: u32) -> Result<bool> {
        match self.phase {
            PoolPhase::ReleaseSent(cursor) => {
                crate::g17::freelist::control_consumed(consumer, producer, cursor)
            }
            _ => Ok(true),
        }
    }

    /// Moves a consumed release to vacant. Returns whether it became vacant now.
    pub(crate) fn observe(&mut self, consumer: u32, producer: u32) -> Result<bool> {
        if !self.pending() || !self.release_consumed(consumer, producer)? {
            return Ok(false);
        }
        self.vacate();
        Ok(true)
    }

    fn vacate(&mut self) {
        self.phase = PoolPhase::Vacant;
        self.registered = false;
        self.release_sent_ns = None;
    }

    /// Fresh backing is installed (a re-arm, or a growth whose release was consumed).
    pub(crate) fn populate(&mut self) {
        self.phase = PoolPhase::Populated;
        self.release_sent_ns = None;
    }

    /// A release is in flight.
    pub(crate) fn pending(&self) -> bool {
        matches!(self.phase, PoolPhase::ReleaseSent(_))
    }

    /// A release in flight past the background poll's absolute bound.
    pub(crate) fn stalled(&self, now: u64) -> bool {
        self.pending() && crate::g17::timeout::background_stalled(self.release_sent_ns, now)
    }
}

#[cfg_attr(not(test), kernel::macros::kunit_tests(asahi_g17_pool_state))]
mod tests {
    use super::*;

    /// Neo's queue: unregistered until its installation, then every release is published.
    #[test]
    fn neo_phase_sequence() {
        let mut state = PoolState::FRESH;
        assert_eq!(state.phase(), PoolPhase::Populated);
        assert!(!state.registered());
        // Installation registers; vacate/begin_growth publish.
        state.register();
        state.sent(7, 100);
        assert!(state.pending());
        assert!(!state.stalled(100 + 9_999_999_999));
        assert!(state.stalled(100 + 10_000_000_000));
        assert!(!state.release_consumed(6, 9).unwrap());
        assert!(!state.observe(6, 9).unwrap());
        assert!(state.release_consumed(8, 9).unwrap());
        assert!(state.observe(8, 9).unwrap());
        assert_eq!(state.phase(), PoolPhase::Vacant);
        assert!(!state.registered());
        assert!(!state.observe(8, 9).unwrap());
        // rearm: populated and, for an installed queue, registered again.
        state.populate();
        state.register();
        assert_eq!(state.phase(), PoolPhase::Populated);
        assert!(state.registered());
        assert!(!state.stalled(u64::MAX));
    }

    /// finish_growth checks the release, then populates, keeping the registration.
    #[test]
    fn growth_keeps_registration() {
        let mut state = PoolState::FRESH;
        state.register();
        state.sent(3, 0);
        assert!(state.release_consumed(4, 4).unwrap());
        state.populate();
        assert_eq!(state.phase(), PoolPhase::Populated);
        assert!(state.registered());
        assert!(state.release_consumed(0, 0).unwrap());
    }

    /// begin_growth before installation, or an M5 lane whose pool no kick registered yet: vacant
    /// at once, with nothing to observe.
    #[test]
    fn unregistered_pool_has_no_release() {
        let mut state = PoolState::FRESH;
        state.phase = PoolPhase::Vacant;
        assert!(!state.pending());
        assert!(!state.observe(0, 0).unwrap());
        assert_eq!(state.phase(), PoolPhase::Vacant);
    }
}
