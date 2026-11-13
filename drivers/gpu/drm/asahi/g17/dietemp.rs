// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Host application of the PMP die-temperature controller's GPU ceiling.
//!
//! The PMP owns the sensor reads and closed loop. Its documented report byte
//! is an effort from 0 (no limit) through 255 (full limit). The host turns the
//! complement into the integer-percent ceiling accepted by the AGX power path.

/// Native cadence of the J700 PMP fast die-temperature controller.
pub(crate) const PERIOD_NS: u64 = 5_000_000;

/// Convert the complete PMP effort range to an integer-percent ceiling.
///
/// Multiplication precedes division so effort zero remains exactly 100.
pub(crate) const fn effort_to_ceiling(effort: u8) -> u32 {
    ((u8::MAX - effort) as u32 * 100) / u8::MAX as u32
}

/// Scale a native milliwatt target without overflowing a 32-bit product.
pub(crate) const fn scale_target(native_mw: u32, percent: u32) -> u32 {
    ((native_mw as u64 * percent as u64) / 100) as u32
}

/// Result of observing one report sample.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Change {
    /// The effective ceiling did not change.
    None,
    /// Publish this restrictive percentage.
    Limit(u32),
    /// Withdraw a prior limit and restore the native words.
    Restore,
}

/// Pure state machine shared by KUnit and host tests.
pub(crate) struct Controller {
    applied: Option<u32>,
}

impl Controller {
    pub(crate) const fn new() -> Self {
        Self { applied: None }
    }

    pub(crate) const fn applied(&self) -> Option<u32> {
        self.applied
    }

    /// Observe a valid effort, or `None` while the PMP is unavailable.
    pub(crate) fn observe(&mut self, effort: Option<u8>) -> Change {
        let desired = effort
            .map(effort_to_ceiling)
            .filter(|ceiling| *ceiling < 100);
        if desired == self.applied {
            return Change::None;
        }
        let change = desired.map_or(Change::Restore, Change::Limit);
        self.applied = desired;
        change
    }
}

#[cfg_attr(not(test), kernel::macros::kunit_tests(asahi_g17_dietemp))]
mod tests {
    use super::*;

    #[test]
    fn effort_mapping_covers_endpoints_and_is_monotonic() {
        assert_eq!(effort_to_ceiling(0), 100);
        assert_eq!(effort_to_ceiling(1), 99);
        assert_eq!(effort_to_ceiling(128), 49);
        assert_eq!(effort_to_ceiling(255), 0);
        for effort in 1..=u8::MAX {
            assert!(effort_to_ceiling(effort) <= effort_to_ceiling(effort - 1));
        }
    }

    #[test]
    fn zero_and_absence_withdraw_only_an_active_limit() {
        let mut controller = Controller::new();
        assert_eq!(controller.observe(None), Change::None);
        assert_eq!(controller.observe(Some(0)), Change::None);
        assert_eq!(controller.observe(Some(64)), Change::Limit(74));
        assert_eq!(controller.observe(Some(64)), Change::None);
        assert_eq!(controller.observe(Some(0)), Change::Restore);
        assert_eq!(controller.observe(None), Change::None);
    }

    #[test]
    fn target_scaling_preserves_native_endpoints() {
        assert_eq!(scale_target(9550, 100), 9550);
        assert_eq!(scale_target(9550, 74), 7067);
        assert_eq!(scale_target(9550, 0), 0);
        assert_eq!(scale_target(u32::MAX, 100), u32::MAX);
    }

    #[test]
    fn provider_loss_restores_and_a_new_epoch_can_limit_again() {
        let mut controller = Controller::new();
        assert_eq!(controller.observe(Some(255)), Change::Limit(0));
        assert_eq!(controller.observe(None), Change::Restore);
        assert_eq!(controller.observe(Some(255)), Change::Limit(0));
    }
}
