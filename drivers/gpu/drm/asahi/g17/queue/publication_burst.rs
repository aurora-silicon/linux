// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Test-only G16S publication arms.
//!
//! The default publishes one tag-14 announcement and one doorbell per kick. B1
//! keeps one announcement per kick but coalesces the doorbell. B2 commits the
//! same consecutive kick entries and replaces their announcements with one
//! count-N announcement. A tail is published immediately; this policy never
//! waits for work which was not already accepted by the logical scheduler.

use crate::module_parameters;
use kernel::prelude::*;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Encoding {
    Immediate,
    CountOne,
    CountN,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct Arm {
    encoding: Encoding,
    limit: u16,
}

impl Arm {
    const DEFAULT: Self = Self {
        encoding: Encoding::Immediate,
        limit: 1,
    };

    fn parse(value: u32) -> Self {
        match value {
            2 | 4 | 8 => Self {
                encoding: Encoding::CountOne,
                limit: value as u16,
            },
            102 | 104 | 108 => Self {
                encoding: Encoding::CountN,
                limit: (value - 100) as u16,
            },
            _ => Self::DEFAULT,
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(super) struct Plan {
    /// Tag-14 count to append for this publication; `None` in an intermediate B2 publication.
    pub(super) announce: Option<u16>,
    /// Ring the work doorbell after exposing this publication.
    pub(super) notify: bool,
}

#[derive(Copy, Clone, Debug)]
pub(super) struct State {
    arm: Arm,
    pending: u16,
}

impl State {
    /// Enables the test arm only for the qualified 25G83 G16S protocol.
    pub(super) fn configured(v25g83: bool) -> Self {
        let arm = if v25g83 {
            Arm::parse(module_parameters::g16_test_burst_publication.value())
        } else {
            Arm::DEFAULT
        };
        Self { arm, pending: 0 }
    }

    /// Selects the record for one fully prepared kick and advances the burst.
    /// `more` means the same logical engine already has another accepted packet;
    /// `force` keeps first installation and recovery-sensitive publications on A.
    fn advance(&mut self, more: bool, force: bool) -> Plan {
        if force || self.arm.encoding == Encoding::Immediate {
            let announce = match self.arm.encoding {
                Encoding::CountN if self.pending != 0 => Some(self.pending.saturating_add(1)),
                _ => Some(1),
            };
            self.pending = 0;
            return Plan {
                announce,
                notify: true,
            };
        }

        let count = self.pending.saturating_add(1);
        let finish = !more || count >= self.arm.limit;
        let announce = match self.arm.encoding {
            Encoding::Immediate | Encoding::CountOne => Some(1),
            Encoding::CountN if finish => Some(count),
            Encoding::CountN => None,
        };
        self.pending = if finish { 0 } else { count };
        Plan {
            announce,
            notify: finish,
        }
    }

    /// Returns the publication plan and the state to commit once the kick is visible.
    pub(super) fn plan(self, more: bool, force: bool) -> (Self, Plan) {
        let mut next = self;
        let plan = next.advance(more, force);
        (next, plan)
    }

    /// Recovery discards the ingestion frontier; every replay starts a fresh group.
    pub(super) fn reset(&mut self) {
        self.pending = 0;
    }
}

#[kunit_tests(asahi_g17_publication_burst)]
mod tests {
    use super::*;

    fn run(value: u32, queued: usize) -> [Plan; 8] {
        let mut state = State {
            arm: Arm::parse(value),
            pending: 0,
        };
        let mut plans = [Plan {
            announce: None,
            notify: false,
        }; 8];
        for (index, plan) in plans.iter_mut().take(queued).enumerate() {
            let (next, selected) = state.plan(index + 1 < queued, false);
            state = next;
            *plan = selected;
        }
        plans
    }

    #[test]
    fn invalid_values_keep_the_shipped_default() {
        for value in [0, 1, 3, 16, 101, 103, 116] {
            assert_eq!(Arm::parse(value), Arm::DEFAULT);
        }
    }

    #[test]
    fn count_one_arms_keep_every_announcement_and_one_doorbell() {
        for limit in [2, 4, 8] {
            let plans = run(limit, limit as usize);
            for plan in &plans[..limit as usize - 1] {
                assert_eq!(plan.announce, Some(1));
                assert!(!plan.notify);
            }
            assert_eq!(plans[limit as usize - 1].announce, Some(1));
            assert!(plans[limit as usize - 1].notify);
        }
    }

    #[test]
    fn count_n_arms_announce_the_consecutive_committed_tail() {
        for value in [102, 104, 108] {
            let limit = (value - 100) as usize;
            let plans = run(value, limit);
            for plan in &plans[..limit - 1] {
                assert_eq!(plan.announce, None);
                assert!(!plan.notify);
            }
            assert_eq!(plans[limit - 1].announce, Some(limit as u16));
            assert!(plans[limit - 1].notify);
        }
    }

    #[test]
    fn count_n_plan_builds_the_tag_14_wire_count() {
        use crate::g17::fw::{
            kick::KickTimestamp,
            queue::{DataMaster, KickAnnounce},
        };

        let timestamp = KickTimestamp::new(0x1234).unwrap();
        let record = KickAnnounce::new_count(32, DataMaster::Compute, timestamp, 0, 8).unwrap();
        let count = record.count;
        let encoded_timestamp = record.timestamp;
        assert_eq!(count, 8);
        assert_eq!(encoded_timestamp, timestamp.get());
        assert!(KickAnnounce::new_count(32, DataMaster::Compute, timestamp, 0, 0).is_err());
    }

    #[test]
    fn tails_and_depth_one_are_not_delayed() {
        assert_eq!(run(108, 1)[0], Plan { announce: Some(1), notify: true });
        let plans = run(108, 3);
        assert_eq!(plans[0].announce, None);
        assert_eq!(plans[1].announce, None);
        assert_eq!(plans[2], Plan { announce: Some(3), notify: true });
    }

    #[test]
    fn forced_publication_closes_an_open_count_n_group() {
        let mut state = State {
            arm: Arm::parse(108),
            pending: 0,
        };
        let (next, plan) = state.plan(true, false);
        state = next;
        assert_eq!(plan.announce, None);
        let (next, plan) = state.plan(true, true);
        state = next;
        assert_eq!(plan, Plan { announce: Some(2), notify: true });
        let (_, plan) = state.plan(false, false);
        assert_eq!(plan, Plan { announce: Some(1), notify: true });
    }

    #[test]
    fn recovery_reset_starts_a_fresh_group() {
        let mut state = State {
            arm: Arm::parse(108),
            pending: 0,
        };
        let (next, _) = state.plan(true, false);
        state = next;
        state.reset();
        let (_, plan) = state.plan(false, false);
        assert_eq!(plan, Plan { announce: Some(1), notify: true });
    }
}
