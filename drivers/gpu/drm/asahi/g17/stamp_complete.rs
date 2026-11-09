// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Test-only output completion policy. This never authorizes resource retirement.

use kernel::prelude::*;

pub(super) fn enabled() -> bool {
    crate::module_parameters::g17_stamp_complete_arm.value() == 1
}

/// Nonzero, ordered per-kick words must be refreshed after the completion stamp.
/// Missing/invalid words leave output on the ordinary retirement path.
pub(super) fn timestamps(arm: u32, healthy: bool, covered: bool, ticks: [u64; 2])
    -> Option<[u64; 2]>
{
    (arm == 1 && healthy && covered && ticks[0] != 0 && ticks[1] > ticks[0])
        .then_some(ticks)
}

#[kunit_tests(asahi_g17_stamp_complete)]
mod tests {
    use super::*;

    #[test]
    fn default_and_invalid_arms_do_not_signal() {
        for arm in [0, 2, 3, u32::MAX] {
            assert_eq!(timestamps(arm, true, true, [10, 20]), None);
        }
        assert_eq!(timestamps(1, true, true, [10, 20]), Some([10, 20]));
    }

    #[test]
    fn failure_and_incomplete_timestamps_stay_on_retirement_path() {
        assert_eq!(timestamps(1, false, true, [10, 20]), None);
        assert_eq!(timestamps(1, true, false, [10, 20]), None);
        for ticks in [[0, 0], [0, 20], [10, 0], [10, 10], [20, 10]] {
            assert_eq!(timestamps(1, true, true, ticks), None);
        }
    }

    #[test]
    fn one_commands_evidence_cannot_complete_another() {
        // A covered member does not lend its witness to another queue/member.
        assert_eq!(timestamps(1, true, true, [10, 20]), Some([10, 20]));
        assert_eq!(timestamps(1, true, false, [30, 40]), None);
        assert_eq!(timestamps(1, true, true, [30, 0]), None);
        assert_eq!(timestamps(0, true, true, [30, 40]), None);
    }
}
