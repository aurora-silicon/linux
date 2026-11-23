// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! One engine's accepted commands that have not finished, oldest first.
//!
//! Commands mostly finish in order, so a prune takes finished entries from the front and stops at
//! the first unfinished one: it costs what it removes, not the length of the list. An entry that
//! finishes behind an older unfinished one stays until it reaches the front or a full scan, and
//! every reader skips it by its status. While it stays it keeps its command's resources, such as
//! a compute command's timestamp page, so a full scan also runs once there have been as many
//! prunes as entries were held after the last one. That target is fixed at each scan, as a stuck
//! front with one new entry per prune would otherwise keep the length ahead of the count. It
//! costs at most two more status reads per prune, amortized, and nothing finished waits longer
//! than one queue length of submissions.
//!
//! A failure must not wait at all, because later commands whose prefix covers it are cancelled:
//! a prune is given its VM's failure counts and scans the whole list from when a failure starts
//! until it has finished and one scan has run after that.

use crate::g17::{
    dependency::{FailureRecord, Retention},
    job::Order,
    status::Failures,
};
use kernel::prelude::*;

/// Prunes an engine's frontier and its list of publishers, a subset of it, with the same counts.
/// Each entry that leaves either list goes to `record` and then to `finished`. A publisher can
/// leave its own list while the frontier still holds it behind an older unfinished command, and
/// the readers of that list would then not see its failure, so it is recorded wherever it leaves.
/// One that leaves both lists is recorded twice until it retires.
pub(crate) fn prune_engine<T: Status + Retention + Clone>(
    frontier: &mut Outstanding<T>,
    publishers: &mut Outstanding<T>,
    failures: Failures,
    record: &mut FailureRecord<T>,
    finished: &mut KVVec<T>,
) {
    let mut left = |sequence, status, entry: T| {
        record.note(sequence, status, &entry);
        // Without room the entry is released here, as it always could be.
        let _ = finished.push(entry, GFP_KERNEL_ACCOUNT);
    };
    frontier.prune(failures, &mut left);
    publishers.prune(failures, &mut left);
}

/// A command's completion status, as `dma_fence_get_status()`: 0 while unfinished, then 1 or
/// a negative error.
pub(crate) trait Status {
    fn status(&self) -> i32;
}

pub(crate) struct Outstanding<T> {
    /// Entries before `head` have left the front and hold nothing. Every entry from `head`
    /// on holds its command. It needs no contiguous pages, and its growth must not fail a
    /// submit on fragmentation.
    entries: KVVec<(u64, Option<T>)>,
    head: usize,
    /// [`Failures::finished`] at the last full scan.
    failures: u64,
    /// Front-only prunes left before the next full scan: the length after the last one.
    scan_in: usize,
}

impl<T: Status> Outstanding<T> {
    /// `failures`: the VM's [`Failures::finished`] now.
    pub(crate) const fn new(failures: u64) -> Self {
        Self {
            entries: KVVec::new(),
            head: 0,
            failures,
            scan_in: 0,
        }
    }

    /// The entries still held. Some may have finished since the last prune.
    pub(crate) fn len(&self) -> usize {
        self.entries.len() - self.head
    }

    /// The entries still held, oldest first.
    pub(crate) fn iter(&self) -> impl Iterator<Item = (u64, &T)> + Clone {
        Self::held(&self.entries[self.head..])
    }

    /// The entries still held whose sequence `prefix` covers ([`Order::contains`]).
    pub(crate) fn prefix(&self, prefix: Option<u64>) -> impl Iterator<Item = (u64, &T)> + Clone {
        let held = &self.entries[self.head..];
        // Accepted sequences increase from one, so the entries a prefix covers come first.
        let end = held.partition_point(|(sequence, _)| Order::contains(prefix, *sequence));
        Self::held(&held[..end])
    }

    fn held(entries: &[(u64, Option<T>)]) -> impl Iterator<Item = (u64, &T)> + Clone {
        entries
            .iter()
            .filter_map(|(sequence, entry)| Some((*sequence, entry.as_ref()?)))
    }

    /// Userspace sets how many commands are outstanding, so their room is charged to it.
    pub(crate) fn reserve(&mut self, additional: usize) -> Result {
        // Reuse the room in front instead of growing past it, once moving the held entries
        // down costs at most two moves for each entry that left since the last move.
        if self.entries.capacity() - self.entries.len() < additional && 2 * self.head >= self.len()
        {
            drop(self.entries.drain(..self.head));
            self.head = 0;
        }
        Ok(self.entries.reserve(additional, GFP_KERNEL_ACCOUNT)?)
    }

    pub(crate) fn push_within_capacity(&mut self, sequence: u64, entry: T) -> Result {
        self.entries
            .push_within_capacity((sequence, Some(entry)))
            .map_err(|_| ENOMEM)
    }

    /// Removes finished entries and hands each to `finished` with its status, oldest first.
    /// While no failure has started since the last full scan, only entries in front of the
    /// oldest unfinished one leave, until there have been as many prunes as entries were held
    /// after the last full scan.
    pub(crate) fn prune(&mut self, failures: Failures, mut finished: impl FnMut(u64, i32, T)) {
        if failures.started != self.failures || self.scan_in == 0 {
            // Only a finished failure is sure to be seen by this scan. One still being signalled
            // keeps `started` ahead, so the scans stay full until it has finished.
            self.failures = failures.finished;
            self.entries.retain(|(sequence, entry)| {
                let Some(status) = entry.as_ref().map(Status::status) else {
                    return false;
                };
                if status == 0 {
                    return true;
                }
                if let Some(entry) = entry.take() {
                    finished(*sequence, status, entry);
                }
                false
            });
            self.head = 0;
            self.scan_in = self.len();
            return;
        }
        self.scan_in -= 1;
        while let Some((sequence, entry)) = self.entries.get_mut(self.head) {
            let Some(status) = entry.as_ref().map(Status::status) else {
                break;
            };
            if status == 0 {
                break;
            }
            if let Some(entry) = entry.take() {
                finished(*sequence, status, entry);
            }
            self.head += 1;
        }
    }
}

#[kunit_tests(asahi_g17_outstanding)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicI32, AtomicUsize, Ordering};
    use kernel::sync::Arc;

    /// Status reads of every test command. KUnit runs a suite's cases one after another.
    static READS: AtomicUsize = AtomicUsize::new(0);

    /// A command whose status the test sets.
    #[derive(Clone)]
    struct Command(Arc<AtomicI32>);

    impl Command {
        fn new() -> Self {
            Self(Arc::new(AtomicI32::new(0), GFP_KERNEL).unwrap())
        }
        fn finish(&self, status: i32) {
            self.0.store(status, Ordering::Relaxed);
        }
    }

    impl Retention for Command {
        fn retained(&self) -> bool {
            true
        }
    }

    impl Status for Command {
        fn status(&self) -> i32 {
            READS.fetch_add(1, Ordering::Relaxed);
            self.0.load(Ordering::Relaxed)
        }
    }

    fn push(queue: &mut Outstanding<Command>, sequence: u64) -> Command {
        let command = Command::new();
        queue.reserve(1).unwrap();
        queue
            .push_within_capacity(sequence, command.clone())
            .unwrap();
        command
    }

    fn queue(count: u64) -> (Outstanding<Command>, KVec<Command>) {
        let mut queue = Outstanding::new(0);
        let mut commands = KVec::new();
        for sequence in 1..=count {
            let command = push(&mut queue, sequence);
            commands.push(command, GFP_KERNEL).unwrap();
        }
        // As a queue in use: every enqueue prunes before it pushes, so it has been scanned.
        assert!(prune(&mut queue, 0).is_empty());
        (queue, commands)
    }

    fn sequences<'a>(entries: impl Iterator<Item = (u64, &'a Command)>) -> KVec<u64> {
        let mut sequences = KVec::new();
        for (sequence, _) in entries {
            sequences.push(sequence, GFP_KERNEL).unwrap();
        }
        sequences
    }

    /// A prune with `failures` failures started and finished.
    fn prune(queue: &mut Outstanding<Command>, failures: u64) -> KVec<(u64, i32)> {
        prune_counts(queue, failures, failures)
    }

    fn prune_counts(
        queue: &mut Outstanding<Command>,
        started: u64,
        finished: u64,
    ) -> KVec<(u64, i32)> {
        let mut left = KVec::new();
        queue.prune(Failures { started, finished }, |sequence, status, _| {
            left.push((sequence, status), GFP_KERNEL).unwrap()
        });
        left
    }

    #[test]
    fn finished_commands_leave_from_the_front_in_order() {
        let (mut queue, commands) = queue(4);
        commands[0].finish(1);
        commands[1].finish(EIO.to_errno());
        assert_eq!(prune(&mut queue, 0), [(1u64, 1i32), (2, EIO.to_errno())]);
        assert_eq!(sequences(queue.iter()), [3u64, 4]);
        assert_eq!(queue.len(), 2);
        assert!(prune(&mut queue, 0).is_empty());
    }

    #[test]
    fn a_command_finished_behind_an_unfinished_one_waits_for_the_front() {
        let (mut queue, commands) = queue(3);
        commands[1].finish(1);
        assert!(prune(&mut queue, 0).is_empty());
        assert_eq!(sequences(queue.iter()), [1u64, 2, 3]);
        commands[0].finish(1);
        assert_eq!(prune(&mut queue, 0), [(1u64, 1i32), (2, 1)]);
        assert_eq!(sequences(queue.iter()), [3u64]);
    }

    #[test]
    fn a_command_finished_behind_an_unfinished_one_leaves_within_a_queue_length() {
        let (mut queue, commands) = queue(4);
        commands[2].finish(1);
        let mut prunes = 0;
        while prune(&mut queue, 0).is_empty() {
            prunes += 1;
            assert!(prunes <= 4);
        }
        assert_eq!(sequences(queue.iter()), [1u64, 2, 4]);
    }

    #[test]
    fn a_stuck_front_does_not_hold_finished_commands_past_a_queue_length() {
        let (mut queue, commands) = queue(4);
        commands[1].finish(1);
        commands[2].finish(1);
        commands[3].finish(1);
        // Command 1 never finishes, and each prune comes with a new unfinished command, as a
        // long command at the front of a busy queue.
        let mut left = KVec::new();
        let mut prunes = 0;
        for sequence in 5..=16u64 {
            prunes += 1;
            for (sequence, _) in prune(&mut queue, 0) {
                left.push(sequence, GFP_KERNEL).unwrap();
            }
            push(&mut queue, sequence);
            if !left.is_empty() {
                break;
            }
        }
        // The 4 held at the last scan, then the scan itself.
        assert_eq!(left, [2u64, 3, 4]);
        assert_eq!(prunes, 4 + 1);
    }

    #[test]
    fn a_publisher_that_fails_behind_an_unfinished_command_is_recorded() {
        // Command 1 is unfinished; command 2 publishes timestamps, so both lists hold it.
        let (mut frontier, commands) = queue(2);
        let mut publishers = Outstanding::new(0);
        publishers.reserve(1).unwrap();
        publishers
            .push_within_capacity(2, commands[1].clone())
            .unwrap();
        let mut record = FailureRecord::new();
        let mut finished = KVVec::new();
        let counts = Failures {
            started: 0,
            finished: 0,
        };
        prune_engine(
            &mut frontier,
            &mut publishers,
            counts,
            &mut record,
            &mut finished,
        );
        // Command 2 fails after this prune read the counts, so they have not moved: the
        // frontier keeps it behind command 1 and its own list lets it go first.
        commands[1].finish(EIO.to_errno());
        prune_engine(
            &mut frontier,
            &mut publishers,
            counts,
            &mut record,
            &mut finished,
        );
        assert_eq!(sequences(frontier.iter()), [1u64, 2]);
        assert!(publishers.iter().next().is_none());
        assert_eq!(record.earliest().map(|(sequence, _)| sequence), Some(2));
    }

    #[test]
    fn a_moved_failure_count_scans_the_whole_queue() {
        let (mut queue, commands) = queue(5);
        commands[0].finish(1);
        assert_eq!(prune(&mut queue, 0), [(1u64, 1i32)]);
        commands[2].finish(ECANCELED.to_errno());
        commands[4].finish(1);
        assert_eq!(prune(&mut queue, 1), [(3u64, ECANCELED.to_errno()), (5, 1)]);
        assert_eq!(sequences(queue.iter()), [2u64, 4]);
        commands[1].finish(1);
        commands[3].finish(1);
        assert_eq!(prune(&mut queue, 1), [(2u64, 1i32), (4, 1)]);
        assert_eq!(queue.len(), 0);
    }

    #[test]
    fn a_failure_behind_an_unfinished_command_leaves_once_counted() {
        let (mut queue, commands) = queue(3);
        commands[1].finish(EIO.to_errno());
        // Not counted yet: like a success, it waits for the front.
        assert!(prune(&mut queue, 0).is_empty());
        assert_eq!(prune(&mut queue, 1), [(2u64, EIO.to_errno())]);
        // Only the prune that saw the count move looks behind the front.
        commands[2].finish(1);
        assert!(prune(&mut queue, 1).is_empty());
        assert_eq!(sequences(queue.iter()), [1u64, 3]);
    }

    #[test]
    fn a_failure_being_signalled_keeps_prunes_full_until_it_has_finished() {
        let (mut queue, commands) = queue(4);
        // Started, but its error is not visible yet.
        assert!(prune_counts(&mut queue, 1, 0).is_empty());
        // Visible but not finished: the prune must still look behind the front for it.
        commands[1].finish(EIO.to_errno());
        assert_eq!(prune_counts(&mut queue, 1, 0), [(2u64, EIO.to_errno())]);
        // Its finish is scanned for once more, then prunes go back to the front.
        commands[2].finish(1);
        assert_eq!(prune_counts(&mut queue, 1, 1), [(3u64, 1i32)]);
        commands[3].finish(1);
        assert!(prune_counts(&mut queue, 1, 1).is_empty());
        assert_eq!(sequences(queue.iter()), [1u64, 4]);
    }

    #[test]
    fn a_prefix_covers_the_oldest_held_commands() {
        let (mut queue, commands) = queue(4);
        commands[0].finish(1);
        prune(&mut queue, 0);
        assert!(sequences(queue.prefix(None)).is_empty());
        assert!(sequences(queue.prefix(Some(1))).is_empty());
        assert_eq!(sequences(queue.prefix(Some(3))), [2u64, 3]);
        assert_eq!(sequences(queue.prefix(Some(9))), [2u64, 3, 4]);
    }

    #[test]
    fn a_deep_steady_queue_reads_a_few_entries_per_prune_and_never_grows() {
        // Commands queued deep, then one finishing for each new one, as under vkd3d-proton.
        // A little over half of the room is held: moving down only once as many have left
        // as are held would double it.
        const DEPTH: u64 = 1100;
        let (mut queue, mut commands) = queue(DEPTH);
        let capacity = queue.entries.capacity();
        READS.store(0, Ordering::Relaxed);
        for sequence in DEPTH + 1..=10 * DEPTH {
            commands[(sequence - DEPTH - 1) as usize].finish(1);
            assert_eq!(prune(&mut queue, 0), [(sequence - DEPTH, 1i32)]);
            let command = push(&mut queue, sequence);
            commands.push(command, GFP_KERNEL).unwrap();
            assert_eq!(queue.len(), DEPTH as usize);
            assert_eq!(queue.entries.capacity(), capacity);
        }
        // Each prune read the one that left and the unfinished one behind it, and once per
        // queue length a full scan read them all.
        assert!(READS.load(Ordering::Relaxed) <= 3 * 9 * DEPTH as usize);
    }
}
