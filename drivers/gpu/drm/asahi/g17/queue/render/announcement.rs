// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! How an M5 render publication reaches the firmware: outer records with a channel
//! notification, or its two host-port kicks alone.
//!
//! An M5 pair has two cursors the firmware can learn new work from: each queue's kick frontier,
//! which the host-port kicks advance, and the outer work ring's records, which name a queue's
//! item-ring producer (its channel cursor) and come with a channel notification. Once a ring
//! carries the pair's first records and nothing about the channel changes, the firmware needs
//! a record and a notification only when the channel's cursor advances. A render that adds no
//! firmware items ([`super::publication::Firmware::None`]) leaves the channel cursor where the
//! last record put it, so its kicks, with the submission history noted before them, are its
//! whole announcement.

use core::sync::atomic::{fence, Ordering};
use kernel::prelude::*;

/// How a publication reached the firmware.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Announced {
    /// Outer records, retained for the publication's ordered producer update: the window's
    /// next positions. A notification follows them.
    Records([u32; 2]),
    /// The kicks alone, the submission history noted before them. Nothing follows.
    Kicks,
    /// The previous render finished before or during the host-port kick (after submission
    /// history was noted): outer records naming the pair's frontier were published instead
    /// so the follow-up notification wakes firmware.
    Fallback,
}

impl Announced {
    /// What a publication announced this way tells the firmware after its records or kicks.
    /// An outer publication notes its submission history, then notifies. A warm one noted its
    /// history before its kicks ([`announce`]), so only a fallback's notification is left.
    pub(crate) fn notify(self) -> Notify {
        match self {
            Self::Records(_) => Notify::Submission,
            Self::Kicks => Notify::Nothing,
            Self::Fallback => Notify::SendOnly,
        }
    }
    /// The publication left an outer record naming the pair's channel cursor.
    pub(crate) fn recorded(self) -> bool {
        self != Self::Kicks
    }
}

/// What [`super::Host::notify_render`] tells the firmware.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Notify {
    /// A submission announced by records: its history entries, then the channel notification.
    Submission,
    /// A submission announced by its kicks alone, its history entries noted before them.
    Nothing,
    /// Records that are not a new submission (a fallback after its history was noted): the
    /// notification only, so the firmware's history counts each submission once.
    SendOnly,
}

impl Notify {
    pub(crate) fn notes_history(self) -> bool {
        self == Self::Submission
    }
    pub(crate) fn sends(self) -> bool {
        self != Self::Nothing
    }
}

/// The outer records that name a pair's frontier: `[fragment, tiling]` for item-ring
/// positions `frontier` (`[tiling, fragment]`, as the pair keeps its cursors) of queues `qids`
/// at `queues` (both `[tiling, fragment]`). Never a first record: only a started ring's pair
/// is announced by kicks alone.
pub(crate) fn frontier_records(
    queues: [u64; 2],
    qids: [u8; 2],
    frontier: [u32; 2],
) -> Result<[crate::g17::fw::channels::WorkSlot; 2]> {
    use crate::g17::fw::{channels::WorkSlot, queue::DataMaster};
    Ok([
        WorkSlot::new(DataMaster::Fragment, queues[1], qids[1], frontier[1].try_into()?, false)?,
        WorkSlot::new(DataMaster::Tiling, queues[0], qids[0], frontier[0].try_into()?, false)?,
    ])
}

/// A warm publication: commits accounting, notes the submission in firmware history,
/// makes both queues' items and producers visible (a render with no firmware work has
/// none), sends the host-port kick (propagating any host-port error), and re-checks
/// `still_live()` after the kick lands. If the prior render finished before or during
/// the kick, `records` publishes outer records naming the pair's frontier so the
/// follow-up notification (`Notify::SendOnly`) wakes firmware without double-counting
/// submission history.
pub(crate) fn announce(
    history: impl FnOnce(),
    commit: impl FnOnce() -> Result,
    publish_inner: impl FnOnce(),
    kick: impl FnOnce() -> Result,
    still_live: impl FnOnce() -> bool,
    records: impl FnOnce() -> Result,
) -> Result<Announced> {
    commit()?;
    history();
    publish_inner();
    // The ordering of these memory stores before the device write comes from the host port's
    // ordered write accessor, whose write barrier precedes its store. This fence states the
    // requirement; it does not replace that barrier, so the port must not be written through
    // a relaxed view.
    fence(Ordering::SeqCst);
    kick()?;
    // Full barrier: on arm64 Acquire compiles to dmb ishld, which does not order the
    // preceding MMIO kick store before the subsequent completion-stamp and pass-end loads.
    fence(Ordering::SeqCst);
    if still_live() {
        Ok(Announced::Kicks)
    } else {
        records()?;
        Ok(Announced::Fallback)
    }
}

#[kernel::macros::kunit_tests(asahi_g17_render_announcement)]
mod tests {
    use super::*;

    /// The order of the announcement's steps, as small integers.
    struct Steps {
        log: [u8; 6],
        n: usize,
    }

    impl Steps {
        fn note(&mut self, step: u8) {
            self.log[self.n] = step;
            self.n += 1;
        }
    }

    const HISTORY: u8 = 1;
    const COMMIT: u8 = 2;
    const INNER: u8 = 3;
    const KICK: u8 = 4;
    const CHECK: u8 = 5;
    const RECORDS: u8 = 6;

    fn run(commit: Result, kick: Result, live: bool) -> (Result<Announced>, [u8; 6], usize) {
        let steps = core::cell::RefCell::new(Steps { log: [0; 6], n: 0 });
        let announced = announce(
            || steps.borrow_mut().note(HISTORY),
            || {
                steps.borrow_mut().note(COMMIT);
                commit
            },
            || steps.borrow_mut().note(INNER),
            || {
                steps.borrow_mut().note(KICK);
                kick
            },
            || {
                steps.borrow_mut().note(CHECK);
                live
            },
            || {
                steps.borrow_mut().note(RECORDS);
                Ok(())
            },
        );
        let steps = steps.into_inner();
        (announced, steps.log, steps.n)
    }

    /// The history is the firmware's only notice of a warm render: it comes before the kick,
    /// as the items and producers do, and nothing follows when the prior render is still live.
    #[test]
    fn the_history_and_items_come_before_the_kick_and_nothing_follows_it() {
        let (announced, log, n) = run(Ok(()), Ok(()), true);
        assert_eq!(announced.unwrap(), Announced::Kicks);
        assert_eq!(&log[..n], &[COMMIT, HISTORY, INNER, KICK, CHECK]);
        assert!(!Announced::Kicks.recorded());
    }

    #[test]
    fn a_completed_prior_render_publishes_fallback_records_and_one_notification() {
        let (announced, log, n) = run(Ok(()), Ok(()), false);
        assert_eq!(announced.unwrap(), Announced::Fallback);
        assert_eq!(&log[..n], &[COMMIT, HISTORY, INNER, KICK, CHECK, RECORDS]);
        assert!(Announced::Fallback.recorded());
    }

    #[test]
    fn a_failed_kick_propagates_its_error() {
        let (announced, log, n) = run(Ok(()), Err(ENODEV), true);
        assert_eq!(announced, Err(ENODEV));
        assert_eq!(&log[..n], &[COMMIT, HISTORY, INNER, KICK]);
    }

    #[test]
    fn a_failed_commit_announces_nothing() {
        let (announced, log, n) = run(Err(EBUSY), Ok(()), true);
        assert_eq!(announced, Err(EBUSY));
        assert_eq!(&log[..n], &[COMMIT]);
    }

    #[test]
    fn fallback_records_name_the_frontier() {
        use crate::g17::fw::queue::DataMaster;
        let [fragment, tiling] = frontier_records([0x1000, 0x2000], [5, 6], [17, 33]).unwrap();
        assert_eq!((fragment.qid, fragment.items_end, fragment.queue_va), (6, 33, 0x2000));
        assert_eq!((tiling.qid, tiling.items_end, tiling.queue_va), (5, 17, 0x1000));
        assert_eq!(fragment.data_master, DataMaster::Fragment as u32);
        assert_eq!(tiling.data_master, DataMaster::Tiling as u32);
        assert_eq!((fragment.first, tiling.first), (0, 0));
        // An item-ring position past the record's 16 bits is refused, not truncated.
        assert!(frontier_records([0, 0], [5, 6], [0x1_0000, 0]).is_err());
    }

    #[test]
    fn each_announcement_tells_the_firmware_its_submission_once() {
        assert_eq!(Announced::Records([1, 2]).notify(), Notify::Submission);
        assert!(Notify::Submission.notes_history() && Notify::Submission.sends());
        let warm = Announced::Kicks.notify();
        assert_eq!(warm, Notify::Nothing);
        assert!(!warm.notes_history() && !warm.sends());
        let fallback = Announced::Fallback.notify();
        assert_eq!(fallback, Notify::SendOnly);
        assert!(!fallback.notes_history() && fallback.sends());
    }
}
