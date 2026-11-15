// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use crate::proto;
use kernel::prelude::*;

const MAX_INFLIGHT: usize = 8;

const POOL_LEN: usize = (proto::TAG_POOL_LAST - proto::TAG_POOL_FIRST + 1) as usize;

#[derive(Clone, Copy)]
struct Slot {
    used: bool,
    tag: u8,
    reply: Option<proto::ControlReply>,
}

impl Slot {
    const FREE: Slot = Slot {
        used: false,
        tag: 0,
        reply: None,
    };
}

/// After an entropy request goes unanswered, the next one waits this long,
/// doubling with each further failure up to [`ENTROPY_RETRY_MAX_MS`].
pub(crate) const ENTROPY_RETRY_MIN_MS: u64 = 1000;
pub(crate) const ENTROPY_RETRY_MAX_MS: u64 = 60_000;

struct EntropySink {
    value: Option<u32>,
    busy: bool,
    // Requests made and not answered yet, oldest first. A request that timed
    // out may still be answered, and every reply carries the reserved tag,
    // so only their order tells them apart.
    owed: u32,
    // The system is going to sleep: no request may be sent.
    asleep: bool,
    // A request went unanswered: none is sent before this time, in ms.
    retry_at: Option<u64>,
    retry_ms: u64,
}

/// How an entropy exchange ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum EntropyEnd {
    /// The reply was taken.
    Answered,
    /// No reply in time. It may still come.
    TimedOut,
    /// The mailbox did not take the request; `full` if it ran out of time
    /// waiting for room, so the enclave is not reading it either.
    NotSent { full: bool },
    /// Given up for a removal. The reply may still come.
    Abandoned,
}

impl EntropyEnd {
    /// How a request ended that the mailbox refused with `e`. ETIMEDOUT is
    /// its wait for room running out.
    pub(crate) fn not_sent(e: Error) -> Self {
        EntropyEnd::NotSent {
            full: e == ETIMEDOUT,
        }
    }
}

/// How a hwrng read ends.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ReadEnd {
    /// It took this many bytes.
    Bytes(usize),
    /// None, but nothing failed: requests are being held back, or a signal
    /// ended the wait for that to end. The core tries again later.
    Held(Error),
    /// None, as the request failed.
    Failed(Error),
}

/// How a hwrng read ends that took `written` bytes and then stopped for
/// `failure`, if anything stopped it.
pub(crate) fn read_end(written: usize, failure: Option<Error>) -> ReadEnd {
    if written > 0 {
        return ReadEnd::Bytes(written);
    }
    // must return a negative errno, not 0: a blocking reader would spin on 0
    match failure.unwrap_or(EIO) {
        // The reader blocked while requests were held back, as long as a
        // request waits for its reply. Tell it that it timed out, as an
        // unanswered request would. EAGAIN would invite it to read again
        // at once.
        e if e == EAGAIN => ReadEnd::Held(ETIMEDOUT),
        e if e == ERESTARTSYS => ReadEnd::Held(e),
        e => ReadEnd::Failed(e),
    }
}

/// What came of an attempt at the ready-phase hwrng survey.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Survey {
    Registered,
    /// Requests were held back, so the enclave was not asked: try again,
    /// without counting the attempt.
    Held,
    /// It failed, and attempts are left.
    Retry(Error),
    /// It failed, and that was the last attempt.
    GiveUp(Error),
}

/// Runs `survey` once if fewer than `max` attempts have been `used`.
/// Returns the attempts used after it, and what came of it.
pub(crate) fn survey_step(
    used: u32,
    max: u32,
    survey: impl FnOnce() -> Result<()>,
) -> Option<(u32, Survey)> {
    let attempt = used + 1;
    if attempt > max {
        return None;
    }
    Some(match survey() {
        Ok(()) => (attempt, Survey::Registered),
        Err(e) if e == EAGAIN => (used, Survey::Held),
        Err(e) if attempt < max => (attempt, Survey::Retry(e)),
        Err(e) => (attempt, Survey::GiveUp(e)),
    })
}

/// Waits for `busy()` to clear, sleeping `step_ms` at a time through
/// `sleep()`, for at most `limit_ms`. Returns whether it cleared.
pub(crate) fn wait_idle(
    limit_ms: u32,
    step_ms: u32,
    mut busy: impl FnMut() -> bool,
    mut sleep: impl FnMut(u32),
) -> bool {
    let mut waited: u32 = 0;
    while busy() {
        if waited >= limit_ms {
            return false;
        }
        sleep(step_ms);
        waited = waited.saturating_add(step_ms);
    }
    true
}

pub(crate) enum Delivery {
    Entropy,
    /// The answer to an entropy request that had timed out. `recovered` if
    /// requests were being held back until the enclave answered again.
    EntropyLate {
        recovered: bool,
    },
    Matched,
    Unmatched,
}

pub(crate) struct ControlState {
    slots: [Slot; MAX_INFLIGHT],
    next_tag: u8,
    retired: [u64; 4],
    retired_count: u32,
    entropy: EntropySink,
    wake_seen: u8,
}

impl ControlState {
    pub(crate) fn new() -> Self {
        ControlState {
            slots: [Slot::FREE; MAX_INFLIGHT],
            next_tag: proto::TAG_POOL_FIRST,
            retired: [0; 4],
            retired_count: 0,
            entropy: EntropySink {
                value: None,
                busy: false,
                owed: 0,
                asleep: false,
                retry_at: None,
                retry_ms: ENTROPY_RETRY_MIN_MS,
            },
            wake_seen: 0,
        }
    }

    fn is_retired(&self, tag: u8) -> bool {
        self.retired[(tag >> 6) as usize] & (1u64 << (tag & 0x3f)) != 0
    }

    fn retire(&mut self, tag: u8) {
        if !self.is_retired(tag) {
            self.retired[(tag >> 6) as usize] |= 1u64 << (tag & 0x3f);
            self.retired_count += 1;
        }
    }

    fn reclaim_retired(&mut self, tag: u8) -> bool {
        if !self.is_retired(tag) {
            return false;
        }
        self.retired[(tag >> 6) as usize] &= !(1u64 << (tag & 0x3f));
        self.retired_count -= 1;
        true
    }

    fn tag_in_flight(&self, tag: u8) -> bool {
        self.slots.iter().any(|s| s.used && s.tag == tag)
    }

    pub(crate) fn alloc(&mut self) -> Result<(usize, u8)> {
        let idx = self.slots.iter().position(|s| !s.used).ok_or(EBUSY)?;

        for _ in 0..POOL_LEN {
            let tag = self.next_tag;
            self.next_tag = if tag >= proto::TAG_POOL_LAST {
                proto::TAG_POOL_FIRST
            } else {
                tag + 1
            };
            if tag != proto::TAG_SLEEP && !self.tag_in_flight(tag) && !self.is_retired(tag) {
                self.slots[idx] = Slot {
                    used: true,
                    tag,
                    reply: None,
                };
                return Ok((idx, tag));
            }
        }
        Err(EBUSY)
    }

    /// Claims the sleep request's own tag, which the pool never hands out. A
    /// request still in flight under it means the last one was never answered.
    pub(crate) fn alloc_sleep(&mut self) -> Result<usize> {
        let tag = proto::TAG_SLEEP;
        if self.tag_in_flight(tag) {
            return Err(EBUSY);
        }
        let idx = self.slots.iter().position(|s| !s.used).ok_or(EBUSY)?;
        self.reclaim_retired(tag);
        self.slots[idx] = Slot {
            used: true,
            tag,
            reply: None,
        };
        Ok(idx)
    }

    /// Whether any request is waiting for its reply.
    pub(crate) fn requests_in_flight(&self) -> bool {
        self.slots.iter().any(|s| s.used)
    }

    /// Forget the wake messages seen, before asking for them.
    pub(crate) fn wake_reset(&mut self) {
        self.wake_seen = 0;
    }

    /// A control message that isn't a reply: note it if it's a wake message.
    pub(crate) fn wake_note(&mut self, ty: u8) {
        if let Some(n) = proto::WAKE_TYPES.iter().position(|&t| t == ty) {
            self.wake_seen |= 1 << n;
        }
    }

    /// All the wake messages have come.
    pub(crate) fn wake_complete(&self) -> bool {
        self.wake_seen == (1 << proto::WAKE_TYPES.len()) - 1
    }

    pub(crate) fn take_reply(&mut self, idx: usize) -> Option<proto::ControlReply> {
        self.slots[idx].reply.take()
    }

    pub(crate) fn release(&mut self, idx: usize) {
        self.slots[idx] = Slot::FREE;
    }

    // Retire the tag so a late reply cannot be mistaken for a later request's answer.
    pub(crate) fn abandon(&mut self, idx: usize) -> u8 {
        let tag = self.slots[idx].tag;
        self.retire(tag);
        self.slots[idx] = Slot::FREE;
        tag
    }

    pub(crate) fn retired_count(&self) -> u32 {
        self.retired_count
    }

    pub(crate) fn deliver(&mut self, reply: proto::ControlReply) -> Delivery {
        // The reserved entropy tag is claimed first, outstanding request or not.
        if reply.tag == proto::TAG_ENTROPY {
            return self.deliver_entropy(reply.data_lo);
        }

        if let Some(slot) = self
            .slots
            .iter_mut()
            .find(|s| s.used && s.tag == reply.tag && s.reply.is_none())
        {
            slot.reply = Some(reply);
            return Delivery::Matched;
        }

        if self.reclaim_retired(reply.tag) {
            return Delivery::Unmatched;
        }

        // No match: drop it, never hand it to another waiter.
        Delivery::Unmatched
    }

    // The enclave answers in order, so a reply belongs to the oldest request
    // still owed. Only the one that settles the newest request is its answer;
    // any before it answer requests that timed out, and are dropped.
    fn deliver_entropy(&mut self, value: u32) -> Delivery {
        let entropy = &mut self.entropy;
        if entropy.owed == 0 {
            // Answers a request already written off as lost.
            return Delivery::Unmatched;
        }
        entropy.owed -= 1;
        if entropy.owed == 0 && entropy.busy {
            entropy.value = Some(value);
            return Delivery::Entropy;
        }
        // The enclave is answering again, so stop holding requests back.
        let recovered = !entropy.busy && entropy.retry_at.take().is_some();
        if recovered {
            entropy.retry_ms = ENTROPY_RETRY_MIN_MS;
        }
        Delivery::EntropyLate { recovered }
    }

    /// Claims the entropy exchange for a request to be sent at `now_ms`.
    ///
    /// Refused with EAGAIN while the system sleeps, and after a request the
    /// enclave did not answer, until it answers or the retry time comes. A
    /// request that fails then doubles the wait. Each claim counts as owed
    /// until [`Self::entropy_end`], because the reply can arrive before the
    /// sender returns from the mailbox.
    pub(crate) fn entropy_begin(&mut self, now_ms: u64) -> Result<()> {
        if self.entropy.busy {
            return Err(EBUSY);
        }
        if self.entropy.asleep {
            return Err(EAGAIN);
        }
        if let Some(at) = self.entropy.retry_at {
            if now_ms < at {
                return Err(EAGAIN);
            }
            // Nothing came back in all that time. Count those replies as
            // lost, or the answer to this request would be taken for one of
            // theirs and no request would be answered again.
            self.entropy.owed = 0;
        }
        self.entropy.busy = true;
        self.entropy.value = None;
        self.entropy.owed = self.entropy.owed.saturating_add(1);
        Ok(())
    }

    pub(crate) fn entropy_take(&mut self) -> Option<u32> {
        self.entropy.value.take()
    }

    /// Ends the exchange claimed by [`Self::entropy_begin`], at `now_ms`.
    pub(crate) fn entropy_end(&mut self, how: EntropyEnd, now_ms: u64) {
        let entropy = &mut self.entropy;
        entropy.busy = false;
        let failed = match how {
            EntropyEnd::Answered => {
                entropy.retry_at = None;
                entropy.retry_ms = ENTROPY_RETRY_MIN_MS;
                false
            }
            EntropyEnd::TimedOut => true,
            EntropyEnd::NotSent { full } => {
                entropy.owed = entropy.owed.saturating_sub(1);
                full
            }
            EntropyEnd::Abandoned => false,
        };
        if failed {
            entropy.retry_at = Some(now_ms.saturating_add(entropy.retry_ms));
            entropy.retry_ms =
                core::cmp::min(entropy.retry_ms.saturating_mul(2), ENTROPY_RETRY_MAX_MS);
        }
    }

    /// The system is going to sleep: refuse requests until
    /// [`Self::entropy_wake`]. Returns whether one is still in flight.
    pub(crate) fn entropy_sleep(&mut self) -> bool {
        self.entropy.asleep = true;
        self.entropy.busy
    }

    pub(crate) fn entropy_busy(&self) -> bool {
        self.entropy.busy
    }

    /// How long from `now_ms` requests are held back, up to `limit_ms`, or
    /// `None` if [`Self::entropy_begin`] would not refuse one for that.
    pub(crate) fn entropy_held_ms(&self, now_ms: u64, limit_ms: u64) -> Option<u64> {
        if self.entropy.asleep {
            return Some(limit_ms);
        }
        match self.entropy.retry_at {
            Some(at) if now_ms < at => Some(core::cmp::min(at - now_ms, limit_ms)),
            _ => None,
        }
    }

    /// Whether a request claimed by [`Self::entropy_begin`] may still be
    /// written: not once the system has begun going to sleep. Such a
    /// request ends as [`EntropyEnd::NotSent`], not full.
    pub(crate) fn entropy_may_send(&self) -> bool {
        !self.entropy.asleep
    }

    pub(crate) fn entropy_wake(&mut self) {
        self.entropy.asleep = false;
    }
}

#[kernel::macros::kunit_tests(apple_sep_control)]
mod tests {
    use super::*;
    use core::cell::Cell;

    fn entropy(value: u32) -> proto::ControlReply {
        proto::ControlReply {
            tag: proto::TAG_ENTROPY,
            data_lo: value,
        }
    }

    // One request answered in time, as the hwrng read makes it.
    fn draw(state: &mut ControlState, now_ms: u64, value: u32) -> Option<u32> {
        state.entropy_begin(now_ms).ok()?;
        let delivered = matches!(state.deliver(entropy(value)), Delivery::Entropy);
        let taken = state.entropy_take();
        state.entropy_end(
            if taken.is_some() {
                EntropyEnd::Answered
            } else {
                EntropyEnd::TimedOut
            },
            now_ms,
        );
        if delivered {
            taken
        } else {
            None
        }
    }

    #[test]
    fn the_pool_never_hands_out_the_sleep_tag() {
        let mut state = ControlState::new();
        for _ in 0..POOL_LEN * 2 {
            let (idx, tag) = state.alloc().unwrap();
            assert_ne!(tag, proto::TAG_SLEEP);
            state.release(idx);
        }
    }

    #[test]
    fn a_sleep_request_gets_its_own_answer() {
        let mut state = ControlState::new();
        let idx = state.alloc_sleep().unwrap();
        assert!(state.requests_in_flight());
        assert_eq!(state.alloc_sleep(), Err(EBUSY));
        let reply = proto::ControlReply {
            tag: proto::TAG_SLEEP,
            data_lo: 0,
        };
        assert!(matches!(state.deliver(reply), Delivery::Matched));
        assert!(state.take_reply(idx).is_some());
        state.release(idx);
        assert!(!state.requests_in_flight());
    }

    #[test]
    fn a_sleep_request_can_follow_one_that_timed_out() {
        let mut state = ControlState::new();
        let idx = state.alloc_sleep().unwrap();
        state.abandon(idx);
        assert!(state.alloc_sleep().is_ok());
    }

    #[test]
    fn a_wake_takes_both_messages() {
        let mut state = ControlState::new();
        state.wake_reset();
        assert!(!state.wake_complete());
        state.wake_note(0x33);
        assert!(!state.wake_complete());
        state.wake_note(proto::WAKE_TYPES[0]);
        assert!(!state.wake_complete());
        state.wake_note(proto::WAKE_TYPES[1]);
        assert!(state.wake_complete());
        state.wake_reset();
        assert!(!state.wake_complete());
    }

    #[test]
    fn a_request_is_answered() {
        let mut state = ControlState::new();
        assert_eq!(draw(&mut state, 0, 0x1234_5678), Some(0x1234_5678));
        assert_eq!(draw(&mut state, 1, 0x9abc_def0), Some(0x9abc_def0));
    }

    #[test]
    fn no_request_is_sent_while_the_system_sleeps() {
        let mut state = ControlState::new();
        assert!(!state.entropy_sleep());
        assert_eq!(state.entropy_begin(0), Err(EAGAIN));
        assert_eq!(state.entropy_begin(ENTROPY_RETRY_MAX_MS * 10), Err(EAGAIN));
        state.entropy_wake();
        assert_eq!(draw(&mut state, 1, 7), Some(7));
    }

    #[test]
    fn a_request_in_flight_finishes_before_sleep() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        // The sleep has to wait for this one.
        assert!(state.entropy_sleep());
        assert!(matches!(state.deliver(entropy(5)), Delivery::Entropy));
        assert_eq!(state.entropy_take(), Some(5));
        state.entropy_end(EntropyEnd::Answered, 0);
        assert!(!state.entropy_busy());
        // And nothing new starts until the system is awake.
        assert_eq!(state.entropy_begin(0), Err(EAGAIN));
        state.entropy_wake();
        assert_eq!(draw(&mut state, 0, 6), Some(6));
    }

    #[test]
    fn a_request_not_yet_written_when_sleep_begins_is_not_sent() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        assert!(state.entropy_may_send());
        // The sleep begins while the request waits for the mailbox.
        assert!(state.entropy_sleep());
        assert!(!state.entropy_may_send());
        state.entropy_end(EntropyEnd::not_sent(EAGAIN), 0);
        // That ends the drain, and the request is not held against the
        // enclave: nothing is owed, and nothing is held back after the wake.
        assert!(!state.entropy_busy());
        assert_eq!(state.entropy_begin(0), Err(EAGAIN));
        state.entropy_wake();
        assert!(state.entropy_begin(0).is_ok());
        assert!(state.entropy_may_send());
        assert!(matches!(state.deliver(entropy(3)), Delivery::Entropy));
        assert_eq!(state.entropy_take(), Some(3));
        state.entropy_end(EntropyEnd::Answered, 0);
    }

    #[test]
    fn a_late_reply_is_not_the_next_requests_answer() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        // The retry is held back until the enclave answers.
        assert_eq!(state.entropy_begin(1), Err(EAGAIN));
        // It answers the request that timed out: dropped, and reads resume.
        assert!(matches!(
            state.deliver(entropy(1)),
            Delivery::EntropyLate { recovered: true }
        ));
        assert_eq!(draw(&mut state, 2, 2), Some(2));
    }

    #[test]
    fn a_reply_later_than_the_retry_shifts_nothing_after_it() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        // The retry goes out once its time comes, the first reply written off.
        let at = ENTROPY_RETRY_MIN_MS;
        assert!(state.entropy_begin(at).is_ok());
        // That reply comes after all. The retry takes it: it is enclave
        // output nobody has used.
        assert!(matches!(state.deliver(entropy(1)), Delivery::Entropy));
        assert_eq!(state.entropy_take(), Some(1));
        state.entropy_end(EntropyEnd::Answered, at);
        // The retry's own reply is then owed to nobody.
        assert!(matches!(state.deliver(entropy(2)), Delivery::Unmatched));
        assert_eq!(draw(&mut state, at + 1, 3), Some(3));
    }

    #[test]
    fn a_request_abandoned_for_removal_does_not_shift_the_answers() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::Abandoned, 0);
        assert!(state.entropy_begin(0).is_ok());
        assert!(matches!(
            state.deliver(entropy(1)),
            Delivery::EntropyLate { recovered: false }
        ));
        assert_eq!(state.entropy_take(), None);
        assert!(matches!(state.deliver(entropy(2)), Delivery::Entropy));
        assert_eq!(state.entropy_take(), Some(2));
        state.entropy_end(EntropyEnd::Answered, 0);
    }

    #[test]
    fn a_lost_reply_does_not_stop_the_channel_for_good() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        // That reply never comes. The retry must still get its own answer.
        let at = ENTROPY_RETRY_MIN_MS;
        assert_eq!(draw(&mut state, at, 9), Some(9));
        assert_eq!(draw(&mut state, at + 1, 10), Some(10));
    }

    #[test]
    fn a_full_mailbox_holds_requests_back_too() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::NotSent { full: true }, 0);
        assert_eq!(state.entropy_begin(ENTROPY_RETRY_MIN_MS - 1), Err(EAGAIN));
        // Nothing was owed: the next request's reply is its own.
        assert_eq!(draw(&mut state, ENTROPY_RETRY_MIN_MS, 4), Some(4));
    }

    #[test]
    fn a_failed_send_owes_nothing() {
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::NotSent { full: false }, 0);
        assert_eq!(draw(&mut state, 0, 8), Some(8));
    }

    #[test]
    fn retries_back_off_up_to_a_limit() {
        let mut state = ControlState::new();
        let mut now = 0;
        let mut wait = ENTROPY_RETRY_MIN_MS;
        for _ in 0..12 {
            assert!(state.entropy_begin(now).is_ok());
            state.entropy_end(EntropyEnd::TimedOut, now);
            assert_eq!(state.entropy_begin(now + wait - 1), Err(EAGAIN));
            now += wait;
            wait = core::cmp::min(wait * 2, ENTROPY_RETRY_MAX_MS);
        }
        assert_eq!(wait, ENTROPY_RETRY_MAX_MS);
        // An answer resets the wait.
        assert_eq!(draw(&mut state, now, 1), Some(1));
        assert!(state.entropy_begin(now).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, now);
        assert!(state.entropy_begin(now + ENTROPY_RETRY_MIN_MS).is_ok());
        state.entropy_end(EntropyEnd::Abandoned, now);
    }

    #[test]
    fn a_reply_nobody_is_owed_is_dropped() {
        let mut state = ControlState::new();
        assert!(matches!(state.deliver(entropy(1)), Delivery::Unmatched));
        assert_eq!(draw(&mut state, 0, 2), Some(2));
    }

    #[test]
    fn a_full_mailbox_is_told_from_other_send_failures() {
        assert_eq!(
            EntropyEnd::not_sent(ETIMEDOUT),
            EntropyEnd::NotSent { full: true }
        );
        for e in [ENODEV, EINTR, EIO] {
            assert_eq!(EntropyEnd::not_sent(e), EntropyEnd::NotSent { full: false });
        }
        // Only a full mailbox holds the next request back.
        let mut state = ControlState::new();
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::not_sent(ENODEV), 0);
        assert_eq!(draw(&mut state, 0, 1), Some(1));
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::not_sent(ETIMEDOUT), 0);
        assert_eq!(state.entropy_begin(0), Err(EAGAIN));
    }

    #[test]
    fn a_read_returns_what_it_took_or_why_not() {
        assert_eq!(read_end(4, Some(ETIMEDOUT)), ReadEnd::Bytes(4));
        assert_eq!(read_end(64, None), ReadEnd::Bytes(64));
        assert_eq!(read_end(0, Some(ETIMEDOUT)), ReadEnd::Failed(ETIMEDOUT));
        // Never 0, which a blocking reader would spin on.
        assert_eq!(read_end(0, None), ReadEnd::Failed(EIO));
    }

    #[test]
    fn a_held_read_is_not_a_failure() {
        assert_eq!(read_end(0, Some(ERESTARTSYS)), ReadEnd::Held(ERESTARTSYS));
        assert_eq!(read_end(8, Some(EAGAIN)), ReadEnd::Bytes(8));
    }

    #[test]
    fn a_read_held_until_it_gives_up_is_told_it_timed_out() {
        // Not EAGAIN: a blocking reader that retries on it would spin.
        assert_eq!(read_end(0, Some(EAGAIN)), ReadEnd::Held(ETIMEDOUT));
    }

    #[test]
    fn a_held_read_knows_how_long_to_wait() {
        let limit = u64::from(proto::CONTROL_TIMEOUT_MS);
        let mut state = ControlState::new();
        assert_eq!(state.entropy_held_ms(0, limit), None);
        // Held back after a request went unanswered: until the retry time,
        // or the reader's limit if that comes first.
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        let at = ENTROPY_RETRY_MIN_MS;
        assert_eq!(
            state.entropy_held_ms(0, limit),
            Some(core::cmp::min(at, limit))
        );
        assert_eq!(state.entropy_held_ms(at - 10, limit), Some(10));
        assert_eq!(state.entropy_held_ms(at - 10, 0), Some(0));
        assert_eq!(state.entropy_held_ms(at, limit), None);
        // The hold ends early when the enclave answers again.
        assert!(matches!(
            state.deliver(entropy(1)),
            Delivery::EntropyLate { recovered: true }
        ));
        assert_eq!(state.entropy_held_ms(0, limit), None);
        // Asleep: for as long as the reader may wait.
        assert!(!state.entropy_sleep());
        assert_eq!(state.entropy_held_ms(0, limit), Some(limit));
        state.entropy_wake();
        assert_eq!(state.entropy_held_ms(0, limit), None);
        // Whenever begin refuses with EAGAIN, the reader has a hold to wait on.
        assert!(state.entropy_begin(0).is_ok());
        state.entropy_end(EntropyEnd::TimedOut, 0);
        assert_eq!(state.entropy_begin(1), Err(EAGAIN));
        assert!(state.entropy_held_ms(1, limit).is_some());
    }

    #[test]
    fn a_held_survey_attempt_is_not_counted() {
        let max = 5;
        assert_eq!(survey_step(0, max, || Err(EAGAIN)), Some((0, Survey::Held)));
        assert_eq!(survey_step(3, max, || Err(EAGAIN)), Some((3, Survey::Held)));
        // Not even when it would have been the last.
        assert_eq!(
            survey_step(max - 1, max, || Err(EAGAIN)),
            Some((max - 1, Survey::Held))
        );
    }

    #[test]
    fn the_survey_gives_up_after_its_attempts() {
        let max = 5;
        let mut used = 0;
        for _ in 1..max {
            assert_eq!(
                survey_step(used, max, || Err(EIO)),
                Some((used + 1, Survey::Retry(EIO)))
            );
            used += 1;
        }
        assert_eq!(
            survey_step(used, max, || Err(EIO)),
            Some((max, Survey::GiveUp(EIO)))
        );
        let ran = Cell::new(false);
        let run = || {
            ran.set(true);
            Ok(())
        };
        assert_eq!(survey_step(max, max, run), None);
        assert!(!ran.get());
        assert_eq!(
            survey_step(2, max, || Ok(())),
            Some((3, Survey::Registered))
        );
    }

    #[test]
    fn the_drain_waits_for_the_request_in_flight() {
        let slept = Cell::new(0u32);
        let polls = Cell::new(0u32);
        let busy = || {
            polls.set(polls.get() + 1);
            polls.get() <= 3
        };
        assert!(wait_idle(3000, 10, busy, |ms| slept.set(slept.get() + ms)));
        assert_eq!(slept.get(), 30);
        // Nothing in flight: no wait at all.
        slept.set(0);
        assert!(wait_idle(
            3000,
            10,
            || false,
            |ms| slept.set(slept.get() + ms)
        ));
        assert_eq!(slept.get(), 0);
    }

    #[test]
    fn the_drain_gives_up_at_its_limit() {
        let slept = Cell::new(0u32);
        assert!(!wait_idle(
            3000,
            10,
            || true,
            |ms| slept.set(slept.get() + ms)
        ));
        assert_eq!(slept.get(), 3000);
    }
}
