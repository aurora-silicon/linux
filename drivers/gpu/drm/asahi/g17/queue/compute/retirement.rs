// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Exact identities and visibility witnesses for a physical compute queue.
//! A shared work-state counter can include other engines and cannot prove this queue retired.

use crate::g17::initdata::ComputeCompletion;
use kernel::prelude::*;

pub(crate) const IN_FLIGHT: usize = 16;
pub(crate) const RECORD_SLOTS: usize = 32;
const QIDS: usize = 128;
const TIME_MASK: u64 = 0x003f_ffff_ffff_ffff;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Ticket {
    pub(crate) owner: u64,
    pub(crate) submission: u64,
    pub(crate) context_id: u32,
    pub(crate) qid: u8,
    pub(crate) descriptor: u64,
    pub(crate) work_node: u64,
    pub(crate) kick: u64,
    pub(crate) ordinal: u32,
    pub(crate) item_producer: u32,
    pub(crate) item_count: u32,
    pub(crate) epoch: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Observation {
    pub(crate) timestamps: [u64; 2],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Cursors {
    pub(crate) producer: u32,
    pub(crate) consumer: u32,
    pub(crate) count: u32,
}

impl Cursors {
    /// Whether firmware consumed every record of the given publication, allowing later work.
    pub(crate) fn consumed(&self, end: u32, count: u32) -> Result<bool> {
        if self.count == 0 || self.count != count {
            return Err(EIO);
        }
        let producer = self.producer % count;
        let consumer = self.consumer % count;
        let unconsumed = (producer + count - consumer) % count;
        let following = (producer + count - end % count) % count;
        Ok(unconsumed <= following)
    }
}

/// Outstanding kicks span fewer than half the 24-bit stamp sequence. Zero is a valid wrap.
pub(crate) const fn stamp_covers(word: u32, kick: u64) -> bool {
    let delta = word.wrapping_sub((kick as u32).wrapping_shl(8));
    delta & 0xff == 0 && delta < 0x8000_0000
}

#[derive(Clone, Copy)]
pub(crate) enum Recordless {
    Allowed,
    NotYet,
    CrossedRecovery,
}

/// How long a command whose exact completion record is present waits for its completion stamp
/// before it retires on the record alone.
///
/// The firmware stores the stamp after the cache maintenance that follows the record, so the
/// stamp, not the record, proves that the command's writes are complete and that its mappings
/// may be released. While other commands of the same submit ioctl are still running, the stamp
/// routinely lands several milliseconds after the record (on J700 under games, up to about 3 ms
/// after the 1 ms this used to wait). Only a stamp still missing well past that is treated as
/// never stored. The bound stays far below the scheduler timeout, so such a command cannot fail
/// its VM.
pub(crate) const STAMP_FALLBACK_NS: u64 = 50_000_000;

// A command that waits out the fallback must still retire well inside the scheduler timeout.
const _: () = assert!(STAMP_FALLBACK_NS * 4 <= crate::g17::job::TIMEOUT_MS as u64 * 1_000_000);

/// Whether a completion record latched at `observed_at` (host time, ns) has waited out
/// [`STAMP_FALLBACK_NS`] by `now`.
pub(crate) fn record_aged(observed_at: Option<u64>, now: u64) -> bool {
    observed_at.is_some_and(|at| now.saturating_sub(at) >= STAMP_FALLBACK_NS)
}

static STAMPLESS: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);

/// Counts a command that retired on its record because its completion stamp never arrived;
/// logs the first and every 64th.
pub(crate) fn note_stampless(qid: u8, kick: u64) {
    let count = STAMPLESS.fetch_add(1, core::sync::atomic::Ordering::Relaxed) + 1;
    if count == 1 || count % 64 == 0 {
        pr_info!(
            "G17: {} compute commands retired on their completion record after {} ms without a completion stamp (latest: queue {} kick {:#x})\n",
            count,
            STAMP_FALLBACK_NS / 1_000_000,
            qid,
            kick
        );
    }
}

/// A zero-duration record fails before visibility polling. It proves execution did not occur;
/// ownership still requires a separate retirement witness before mappings can be released.
/// The command is complete when its stamp covers it, or when its exact completion record has
/// been present for [`STAMP_FALLBACK_NS`] without one (`record_aged`); in both cases its items
/// must be consumed.
#[expect(
    clippy::too_many_arguments,
    reason = "Retirement validates the ticket against independent firmware observations."
)]
pub(crate) fn poll(
    ticket: &Ticket,
    observed: Option<Observation>,
    cursors: Cursors,
    stamp: u32,
    recordless: Recordless,
    floor: u64,
    next_end: Option<u64>,
    record_aged: bool,
) -> Result<Option<[u64; 2]>> {
    if observed.is_some_and(|record| record.timestamps[0] == record.timestamps[1]) {
        return Err(ENODATA);
    }
    let completed = stamp_covers(stamp, ticket.kick) || (observed.is_some() && record_aged);
    if !completed || !cursors.consumed(ticket.item_producer, ticket.item_count)? {
        return Ok(None);
    }
    Ok(Some(match observed {
        Some(record) => record.timestamps,
        None => match recordless {
            Recordless::Allowed => [floor, next_end.unwrap_or(floor).max(floor)],
            Recordless::NotYet => return Ok(None),
            Recordless::CrossedRecovery => return Err(ENODATA),
        },
    }))
}

/// The newest exact publication covers all preceding kicks, but only a fully consumed ring
/// permits releasing quarantined command pins. The installed graph remains retained.
pub(crate) fn quarantined_retired(
    ticket: &Ticket,
    cursors: Cursors,
    stamp: u32,
    pending: u32,
    submitted: u32,
) -> Result<bool> {
    if ticket.item_count == 0 || ticket.descriptor == 0 || pending == 0 {
        return Err(EINVAL);
    }
    if cursors.count == 0 || cursors.count != ticket.item_count {
        return Err(EIO);
    }
    let end = ticket.item_producer % cursors.count;
    Ok(stamp_covers(stamp, ticket.kick)
        && cursors.producer % cursors.count == end
        && cursors.consumer % cursors.count == end
        && submitted == pending)
}

#[derive(Clone, Copy)]
struct Request {
    descriptor: u64,
    kick: u64,
    observed: Option<Observation>,
    next: u16,
}

const NO_REQUEST: u16 = u16::MAX;
const _: () = assert!(QIDS * IN_FLIGHT < NO_REQUEST as usize);

fn request_bucket(qid: usize, kick: u64) -> usize {
    qid * IN_FLIGHT + kick as usize % IN_FLIGHT
}

/// Reusable heap scratch for one shared-ring scan. QID requests are contiguous and bounded;
/// staging and folding never allocate under the device mutex.
pub(crate) struct Batch {
    requests: KVVec<Request>,
    /// Per-QID kick buckets; collisions retain the complete identity check.
    heads: KVVec<u16>,
    slots: [(usize, usize); QIDS],
    open: Option<u8>,
    /// Requests with no observation yet; the scan stops when this reaches zero.
    unobserved: usize,
}

impl Batch {
    pub(crate) fn new() -> Result<Self> {
        let mut heads = KVVec::with_capacity(QIDS * IN_FLIGHT, GFP_KERNEL)?;
        for _ in 0..QIDS * IN_FLIGHT {
            heads.push_within_capacity(NO_REQUEST).map_err(|_| ENOSPC)?;
        }
        Ok(Self {
            requests: KVVec::with_capacity(QIDS * IN_FLIGHT, GFP_KERNEL)?,
            heads,
            slots: [(0, 0); QIDS],
            open: None,
            unobserved: 0,
        })
    }
    pub(crate) fn clear(&mut self) {
        self.requests.clear();
        self.slots.fill((0, 0));
        self.open = None;
        self.unobserved = 0;
    }
    /// True once every requested identity has an observation.
    pub(crate) fn complete(&self) -> bool {
        self.unobserved == 0
    }
    pub(crate) fn request(&mut self, ticket: &Ticket) -> Result {
        if ticket.descriptor == 0 || ticket.kick == 0 {
            return Err(EINVAL);
        }
        let slot = self.slots.get_mut(ticket.qid as usize).ok_or(EINVAL)?;
        if slot.1 != 0 && self.open != Some(ticket.qid) {
            return Err(EBUSY);
        }
        if slot.1 >= IN_FLIGHT {
            return Err(ENOSPC);
        }
        let start = if slot.1 == 0 {
            let begin = ticket.qid as usize * IN_FLIGHT;
            self.heads.get_mut(begin..begin + IN_FLIGHT).ok_or(EIO)?.fill(NO_REQUEST);
            self.requests.len()
        } else {
            slot.0
        };
        let bucket = request_bucket(ticket.qid as usize, ticket.kick);
        let next = *self.heads.get(bucket).ok_or(EIO)?;
        let index = u16::try_from(self.requests.len()).map_err(|_| ENOSPC)?;
        self.requests
            .push_within_capacity(Request {
                descriptor: ticket.descriptor,
                kick: ticket.kick,
                observed: None,
                next,
            })
            .map_err(|_| ENOSPC)?;
        *self.heads.get_mut(bucket).ok_or(EIO)? = index;
        *slot = (start, slot.1 + 1);
        self.open = Some(ticket.qid);
        self.unobserved += 1;
        Ok(())
    }
    /// Capture the control word once. Most slots belong to an older kick of a
    /// selected QID; reject those before reading any additional shared words.
    pub(crate) fn fold(&mut self, record: ComputeCompletion<'_>) -> Result {
        self.fold_reader(|word| record.read(word))
    }

    fn fold_reader(&mut self, mut read: impl FnMut(usize) -> Result<u64>) -> Result {
        let control = read(0)?;
        let qid = ((control >> 9) & 0x7f) as usize;
        let (_, count) = self.slots[qid];
        if count == 0 {
            return Ok(());
        }
        let kick =
            (((control >> 8) & 0x00ff_ffff_ff00) | ((control >> 48) & 0xff)) & 0xff_ffff_ffff;
        let mut index = *self.heads.get(request_bucket(qid, kick)).ok_or(EIO)?;
        let mut descriptor = None;
        let mut found = None;
        for _ in 0..count {
            if index == NO_REQUEST {
                break;
            }
            let request = self.requests.get(usize::from(index)).ok_or(EIO)?;
            if request.kick == kick {
                let value = if let Some(value) = descriptor {
                    value
                } else {
                    let value = read(6)? & !1;
                    if value == 0 || read(7)? == 0 {
                        return Ok(());
                    }
                    descriptor = Some(value);
                    value
                };
                if request.descriptor == value {
                    // Chains run newest first. Retain the original first-match
                    // rule even if a caller stages the same identity twice.
                    found = Some(usize::from(index));
                }
            }
            index = request.next;
        }
        let Some(index) = found else { return Ok(()); };
        let request = self.requests.get_mut(index).ok_or(EIO)?;
        let timestamps = [read(2)? & TIME_MASK, read(5)? & TIME_MASK];
        if timestamps[0] == 0
            || timestamps[1] == 0
            || request
                .observed
                .is_some_and(|old| old.timestamps[1] >= timestamps[1])
        {
            return Ok(());
        }
        if request.observed.is_none() {
            self.unobserved -= 1;
        }
        request.observed = Some(Observation { timestamps });
        Ok(())
    }
    /// A command published after this batch staged its queue's requests was not part of the
    /// ring snapshot: it stays unobserved until a later batch.
    pub(crate) fn take(&mut self, ticket: &Ticket) -> Result<Option<Observation>> {
        let &(start, count) = self.slots.get(ticket.qid as usize).ok_or(EINVAL)?;
        Ok(self
            .requests
            .get_mut(start..start + count)
            .ok_or(EIO)?
            .iter_mut()
            .find(|r| r.descriptor == ticket.descriptor && r.kick == ticket.kick)
            .and_then(|request| request.observed.take()))
    }
}

#[kunit_tests(asahi_g17_compute_stamp_wait)]
mod tests {
    use super::*;

    const KICK: u64 = 0x12_3456_7801;

    fn ticket() -> Ticket {
        Ticket {
            owner: 1,
            submission: 1,
            context_id: 1,
            qid: 28,
            descriptor: 0x1000,
            work_node: 0x2000,
            kick: KICK,
            ordinal: 1,
            item_producer: 4,
            item_count: 64,
            epoch: 1,
        }
    }

    fn consumed() -> Cursors {
        Cursors {
            producer: 4,
            consumer: 4,
            count: 64,
        }
    }

    fn own(kick: u64) -> u32 {
        (kick as u32).wrapping_shl(8)
    }

    #[test]
    fn stamp_covers_its_kick_and_earlier_ones() {
        assert!(stamp_covers(own(KICK), KICK));
        assert!(stamp_covers(own(KICK + 1), KICK));
        assert!(!stamp_covers(own(KICK - 1), KICK));
        assert!(!stamp_covers(own(KICK) | 1, KICK));
        assert!(stamp_covers(0, 0xff_ffff));
    }

    #[test]
    fn a_late_stamp_is_waited_for() {
        // Under games on J700 the stamp lands up to about 4 ms after the record while the
        // rest of the command's ioctl runs: the command keeps its mappings until then.
        let record = Some(Observation { timestamps: [10, 20] });
        let at = 1_000_000;
        assert!(!record_aged(Some(at), at + 4_000_000));
        assert!(!record_aged(Some(at), at + 10_000_000));
        let waiting = poll(
            &ticket(),
            record,
            consumed(),
            own(KICK - 1),
            Recordless::NotYet,
            0,
            None,
            record_aged(Some(at), at + 10_000_000),
        );
        assert_eq!(waiting, Ok(None));
        let stamped = poll(
            &ticket(),
            record,
            consumed(),
            own(KICK),
            Recordless::NotYet,
            0,
            None,
            false,
        );
        assert_eq!(stamped, Ok(Some([10, 20])));
    }

    #[test]
    fn a_later_kick_stamp_retires_it() {
        let record = Some(Observation { timestamps: [10, 20] });
        let result = poll(
            &ticket(),
            record,
            consumed(),
            own(KICK + 3),
            Recordless::NotYet,
            0,
            None,
            false,
        );
        assert_eq!(result, Ok(Some([10, 20])));
    }

    #[test]
    fn a_stamp_that_never_comes_retires_on_the_record_after_the_fallback() {
        let record = Some(Observation { timestamps: [0x1111, 0x2222] });
        let at = 1_000_000;
        assert!(record_aged(Some(at), at + STAMP_FALLBACK_NS));
        assert!(!record_aged(None, u64::MAX));
        let result = poll(
            &ticket(),
            record,
            consumed(),
            own(KICK - 1),
            Recordless::NotYet,
            0,
            None,
            record_aged(Some(at), at + STAMP_FALLBACK_NS),
        );
        // The record's own timestamps are published, never synthesized ones.
        assert_eq!(result, Ok(Some([0x1111, 0x2222])));
    }

    #[test]
    fn an_aged_record_still_waits_for_consumed_items() {
        let record = Some(Observation { timestamps: [10, 20] });
        let busy = Cursors {
            producer: 4,
            consumer: 2,
            count: 64,
        };
        let result = poll(&ticket(), record, busy, 0, Recordless::Allowed, 0, None, true);
        assert_eq!(result, Ok(None));
    }

    #[test]
    fn a_zero_duration_record_fails() {
        let record = Some(Observation { timestamps: [5, 5] });
        let result = poll(
            &ticket(),
            record,
            consumed(),
            own(KICK),
            Recordless::Allowed,
            0,
            None,
            true,
        );
        assert_eq!(result, Err(ENODATA));
    }
}


#[kunit_tests(asahi_g17_completion_index)]
mod index_tests {
    use super::*;

    fn ticket(qid: u8, kick: u64, descriptor: u64) -> Ticket {
        Ticket { owner: 1, submission: 1, context_id: 1, qid, descriptor,
            work_node: 0x2000, kick, ordinal: 1, item_producer: 4, item_count: 64, epoch: 1 }
    }
    fn record(ticket: &Ticket, timestamps: [u64; 2]) -> [u64; 8] {
        let mut words = [0; 8];
        words[0] = ((ticket.kick & !0xff) << 8) | ((ticket.kick & 0xff) << 48)
            | (u64::from(ticket.qid) << 9);
        words[2] = timestamps[0]; words[5] = timestamps[1];
        words[6] = ticket.descriptor | 1; words[7] = 1;
        words
    }
    #[test]
    fn stale_kick_reads_only_the_control_even_on_a_bucket_collision() {
        let mut batch = Batch::new().unwrap();
        batch.request(&ticket(7, 33, 0x1000)).unwrap();
        let words = record(&ticket(7, 17, 0x1000), [10, 20]);
        batch.fold_reader(|word| {
            assert_eq!(word, 0);
            Ok(words[word])
        }).unwrap();
        assert!(!batch.complete());
    }
    #[test]
    fn collisions_and_reused_descriptors_keep_full_kick_identity() {
        let mut batch = Batch::new().unwrap();
        let first = ticket(7, 33, 0x1000);
        let second = ticket(7, 49, 0x1000);
        batch.request(&first).unwrap(); batch.request(&second).unwrap();
        let words = record(&first, [10, 20]);
        batch.fold_reader(|word| Ok(words[word])).unwrap();
        assert!(!batch.complete());
        let words = record(&second, [30, 40]);
        batch.fold_reader(|word| Ok(words[word])).unwrap();
        assert!(batch.complete());
        assert_eq!(batch.take(&first).unwrap().unwrap().timestamps, [10, 20]);
        assert_eq!(batch.take(&second).unwrap().unwrap().timestamps, [30, 40]);
    }
    #[test]
    fn same_kick_still_checks_descriptor_and_zero_duration_is_retained() {
        let mut batch = Batch::new().unwrap();
        let first = ticket(7, 33, 0x1000);
        let second = ticket(7, 33, 0x2000);
        batch.request(&first).unwrap(); batch.request(&second).unwrap();
        let words = record(&first, [20, 20]);
        batch.fold_reader(|word| Ok(words[word])).unwrap();
        assert_eq!(batch.take(&first).unwrap().unwrap().timestamps, [20, 20]);
        assert!(batch.take(&second).unwrap().is_none());
    }
    #[test]
    fn duplicate_identity_keeps_the_first_staged_request() {
        let mut batch = Batch::new().unwrap();
        let current = ticket(7, 33, 0x1000);
        batch.request(&current).unwrap(); batch.request(&current).unwrap();
        let words = record(&current, [10, 20]);
        batch.fold_reader(|word| Ok(words[word])).unwrap();
        assert_eq!(batch.take(&current).unwrap().unwrap().timestamps, [10, 20]);
        assert_eq!(batch.unobserved, 1);
    }
    #[test]
    fn clear_restarts_buckets_and_later_publication_stays_unobserved() {
        let mut batch = Batch::new().unwrap();
        batch.request(&ticket(7, 33, 0x1000)).unwrap(); batch.clear();
        let current = ticket(7, 49, 0x1000);
        batch.request(&current).unwrap();
        let stale = record(&ticket(7, 33, 0x1000), [10, 20]);
        batch.fold_reader(|word| Ok(stale[word])).unwrap();
        assert!(!batch.complete());
        assert!(batch.take(&ticket(7, 50, 0x2000)).unwrap().is_none());
        let words = record(&current, [30, 40]);
        batch.fold_reader(|word| Ok(words[word])).unwrap();
        assert!(batch.complete());
    }
}
