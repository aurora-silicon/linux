// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Attribute a restart's blame by when the work in flight started, where the recovery table's
//! guilty flags are the firmware's only blame. A restart that reports no reason leaves
//! [`super::Sources`] nothing to attribute (M5), so the table alone named the guilty queue, and
//! it can name a pass that never started while the work that ran on is marked innocent.
//!
//! Neo's rule ([`super::Sources::attribute`]) on the queues' own evidence: each queue's work in
//! flight is read at the halted instant, as Neo reads pass starts, and once the table is read
//! the blame goes to the work that started. Work that never started is never guilty; of several
//! started candidates, the one that started first, the longest running, is blamed alone; with
//! nothing started, nobody is. Missing evidence for a guilty queue keeps the firmware's blame.
//!
//! The same evidence says which work in flight is proven never to have started, which recovery
//! may run again instead of failing ([`Progress::never_started`]).

use super::{Attribution, Entries};
use kernel::fmt;

/// Started work recorded per restart. More leaves the firmware's blame standing.
const STARTED: usize = 16;

/// What a queue's work in flight is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub(crate) enum Work {
    /// A render pass, by its pair's ordinal.
    #[default]
    Pass,
    /// A compute kick, by its kick number.
    Kick,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub(super) struct Started {
    /// The QIDs the work runs on (a render pass: its pair's; a kick: its lane's, twice). The
    /// first is the one its blame settles through (`Queues::settle_blamed_render`, `_compute`).
    qids: [u8; 2],
    work: Work,
    id: u64,
    /// Its earliest start time the firmware wrote (stage or descriptor times).
    start: u64,
}

/// The work in flight at a restart's halted instant, queue by queue.
#[derive(Default)]
pub(crate) struct Progress {
    /// QIDs whose work in flight was read, started or not.
    known: u128,
    /// QIDs whose work in flight was read and had no start time written: never started.
    unstarted: u128,
    started: [Started; STARTED],
    count: usize,
    overflow: bool,
}

/// QIDs, as a list.
struct Qids(u128);

impl fmt::Display for Qids {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("[")?;
        let mut first = true;
        for qid in 0..128u32 {
            if self.0 & (1u128 << qid) != 0 {
                if !first {
                    f.write_str(", ")?;
                }
                write!(f, "{}", qid)?;
                first = false;
            }
        }
        f.write_str("]")
    }
}

/// Work as the recovery lines name it: `pass 9663`, `kick 0x30a0`.
struct Named(Work, u64);

impl fmt::Display for Named {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Work::Pass => write!(f, "pass {}", self.1),
            Work::Kick => write!(f, "kick {:#x}", self.1),
        }
    }
}

impl Started {
    pub(super) fn qids(&self) -> u128 {
        mask(self.qids)
    }
    pub(super) fn qid(&self) -> u8 {
        self.qids[0]
    }
}

fn mask(qids: [u8; 2]) -> u128 {
    qids.iter()
        .fold(0u128, |mask, qid| mask | 1u128 << (qid & 0x7f))
}

impl Progress {
    /// One queue's work in flight on `qids` (a lane names its QID twice): `start` is its
    /// earliest start time, None if none was written (it never started). A queue whose
    /// evidence could not be read is not noted, and stays unknown.
    pub(crate) fn note(&mut self, qids: [u8; 2], work: Work, id: u64, start: Option<u64>) {
        self.known |= mask(qids);
        let Some(start) = start else {
            self.unstarted |= mask(qids);
            return;
        };
        match self.started.get_mut(self.count) {
            Some(slot) => {
                *slot = Started {
                    qids,
                    work,
                    id,
                    start,
                };
                self.count += 1;
            }
            None => self.overflow = true,
        }
    }

    /// Whether the work in flight on `qids` was read at the halted instant and none of it had
    /// started: its start words were read and all were zero. Work that could not be read is
    /// not proven unstarted.
    pub(crate) fn never_started(&self, qids: [u8; 2]) -> bool {
        let qids = mask(qids);
        self.known & qids == qids && self.unstarted & qids == qids
    }

    /// The blame for a restart whose table read `entries`, the firmware having blamed `blamed`:
    /// [`Attribution::Firmware`] where the firmware blamed exactly the started work (or the
    /// evidence cannot decide), otherwise the work that started and ran longest, returned with
    /// it, or [`Attribution::NoCulprit`] when nothing listed had started.
    pub(super) fn attribute(
        &self,
        entries: &Entries,
        blamed: Option<u8>,
    ) -> (Attribution, Option<Started>) {
        let guilty = entries.guilty | blamed.map_or(0, |qid| 1u128 << qid);
        if self.overflow || guilty == 0 || guilty & !self.known != 0 {
            return (Attribution::Firmware, None);
        }
        let started = &self.started[..self.count];
        let longest = |among: u128| {
            started
                .iter()
                .filter(|work| work.qids() & among != 0)
                .min_by_key(|work| work.start)
                .copied()
        };
        let Some(culprit) = longest(guilty).or_else(|| longest(entries.listed_qids)) else {
            return (Attribution::NoCulprit, None);
        };
        if guilty & !culprit.qids() == 0 {
            return (Attribution::Firmware, None);
        }
        (Attribution::Queues(culprit.qids()), Some(culprit))
    }

    /// Logs a verdict of [`Self::attribute`] that changed the firmware's blame.
    pub(super) fn report(
        &self,
        entries: &Entries,
        blamed: Option<u8>,
        verdict: &(Attribution, Option<Started>),
    ) {
        let guilty = Qids(entries.guilty | blamed.map_or(0, |qid| 1u128 << qid));
        match verdict {
            (Attribution::Queues(_), Some(culprit)) => kernel::pr_warn!(
                "G17: recovery blame: the firmware blamed queues {}; {} on queues {}, started at {:#x}, ran longest of the work in flight ({} started) and is blamed instead\n",
                guilty, Named(culprit.work, culprit.id), Qids(culprit.qids()), culprit.start, self.count
            ),
            (Attribution::NoCulprit, _) => kernel::pr_warn!(
                "G17: recovery blame: the firmware blamed queues {}; none of the work in flight on queues {} had started, so none is blamed\n",
                guilty, Qids(entries.listed_qids)
            ),
            _ => {}
        }
    }
}

#[kernel::macros::kunit_tests(asahi_g17_recovery_progress)]
mod tests {
    use super::*;

    fn table(records: &[u32]) -> Entries {
        let mut entries = Entries::default();
        for flags in records {
            assert!(entries.include(*flags));
        }
        entries
    }

    /// Measured: the desktop's pass on [3, 4] never started and was marked guilty; the
    /// spin's started pass on [15, 16] was listed innocent.
    #[test]
    fn a_pass_that_never_started_is_not_blamed_while_one_ran() {
        let mut progress = Progress::default();
        progress.note([3, 4], Work::Pass, 2502, None);
        progress.note([15, 16], Work::Pass, 9663, Some(0x127e472d3));
        let entries = table(&[0x3f, 0x45, 0x101]);
        let (attribution, culprit) = progress.attribute(&entries, Some(3));
        assert_eq!(attribution, Attribution::Queues(1 << 15 | 1 << 16));
        assert_eq!(culprit.map(|c| (c.qid(), c.id)), Some((15, 9663)));
    }

    /// Every ordinary restart: the firmware blamed the spin's started pass.
    #[test]
    fn the_firmware_blame_of_started_work_stands() {
        let mut progress = Progress::default();
        progress.note([12, 13], Work::Pass, 8768, Some(0xa8c0e399));
        progress.note([15, 16], Work::Pass, 7547, None);
        progress.note([17, 17], Work::Kick, 0x30a0, Some(0xa8c0f000));
        let entries = table(&[0xd7, 0x111]);
        assert_eq!(
            progress.attribute(&entries, Some(13)),
            (Attribution::Firmware, None)
        );
    }

    /// Measured: a second restart, nothing launched since the first.
    #[test]
    fn nothing_started_blames_nobody() {
        let mut progress = Progress::default();
        progress.note([17, 17], Work::Kick, 0x3790, None);
        progress.note([12, 13], Work::Pass, 9710, None);
        progress.note([15, 16], Work::Pass, 8478, None);
        let entries = table(&[0x91, 0x91, 0xcf]);
        assert_eq!(
            progress.attribute(&entries, Some(12)),
            (Attribution::NoCulprit, None)
        );
    }

    /// Of several started candidates the one running longest is blamed alone; work not in
    /// the table is not a candidate.
    #[test]
    fn the_longest_running_listed_work_is_blamed() {
        let mut progress = Progress::default();
        progress.note([3, 4], Work::Pass, 1, None);
        progress.note([5, 6], Work::Pass, 2, Some(300));
        progress.note([9, 9], Work::Kick, 3, Some(200));
        progress.note([20, 20], Work::Kick, 4, Some(100));
        let entries = table(&[0x3f, 0x51, 0x91]);
        let (attribution, culprit) = progress.attribute(&entries, Some(3));
        assert_eq!(attribution, Attribution::Queues(1 << 9));
        assert_eq!(culprit.map(|c| c.qid()), Some(9));
    }

    /// Measured twice (the desktop's pass never started, the spin's had; lane 14 had launched
    /// nothing): only work read with no start is proven unstarted.
    #[test]
    fn only_work_read_with_no_start_is_proven_unstarted() {
        let mut progress = Progress::default();
        progress.note([3, 4], Work::Pass, 1836, None);
        progress.note([11, 12], Work::Pass, 0, Some(0x4caa5e0d));
        progress.note([10, 10], Work::Kick, 0x939, Some(0x5726df1b));
        progress.note([14, 14], Work::Kick, 0xbba, None);
        assert!(progress.never_started([3, 4]));
        assert!(!progress.never_started([11, 12]));
        assert!(!progress.never_started([5, 6]));
        assert!(!progress.never_started([10, 10]));
        assert!(progress.never_started([14, 14]));
    }

    /// A guilty queue whose work in flight was not read keeps the firmware's blame.
    #[test]
    fn unknown_guilty_work_keeps_the_firmware_blame() {
        let mut progress = Progress::default();
        progress.note([15, 16], Work::Pass, 9663, Some(5));
        let entries = table(&[0x3f, 0x101]);
        assert_eq!(
            progress.attribute(&entries, Some(3)),
            (Attribution::Firmware, None)
        );
    }
}
