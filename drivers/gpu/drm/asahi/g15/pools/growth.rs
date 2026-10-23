// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The PB growth ledger and the growth-request checks.
//!
//! The firmware asks for more PB with an event of type 7 naming PB slot 0, the context of a
//! published render, its request counter (P+0x60 before its increment), the TA position and the
//! halt epoch; the host appends at most ten blocks of the mapped heap to the block list and
//! advances the block_ctl+4 producer (modulo K), then replies with DEVCTL 7. At the ledger's
//! maximum the reply refuses; after [`MAX_REFUSALS`] refusals since the last initialization,
//! later requests are left unanswered. That bounds the poll pass. It does not make the scene
//! finish: a refused or unanswered request can leave the TA stalled, and the host deadline and
//! its recovery path then apply, without a guarantee that they end the stall. A request from
//! before a halt (epoch) or not matching a published render prefix is left unanswered.
//!
//! Core-only (no `kernel` imports): this file is compiled by the host test harness as well.

use crate::g15::fw::buffer::{grow_request, BLOCK_LIST_RECORDS, GROW_STEP, MAX_BLOCKS, MIN_BLOCKS};

/// The PB ledger since the last (re)initialization.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) struct Ledger {
    /// Initial blocks (the C1 pages, InitBM blocks and TA register #33 name them).
    pub(crate) initial: u32,
    /// The most blocks posted, at most [`MAX_POSTED`]. The PB manager's P+0x70/+0x74 hold
    /// 4 * the firmware's growth maximum (`Pools::soft_max()`) in pages, which may be lower.
    pub(crate) max: u32,
    /// Blocks posted to the block list, cumulative (the block_ctl+4 producer is this modulo K).
    pub(crate) posted: u32,
    /// The next expected request counter.
    pub(crate) next_request: u32,
    /// A refusal at the maximum was logged since the last initialization.
    pub(crate) refusal_logged: bool,
    /// Refusals answered at the maximum since the last initialization. After
    /// [`MAX_REFUSALS`] further requests are left unanswered: a firmware that keeps asking at
    /// the maximum would otherwise be answered forever, under the device lock (ticket
    /// malik-na/linux-aurora#69). Unanswered, the poll pass returns, and the host deadline and
    /// its recovery path apply.
    pub(crate) refusals: u32,
}

/// Refusals answered at the maximum before requests are left to the deadline.
pub(crate) const MAX_REFUSALS: u32 = 2;

/// Blocks of each PB backing chunk after the initial one (4 MiB): growth backs at most this
/// much at a time while the firmware waits for its answer.
pub(crate) const CHUNK_BLOCKS: u32 = 32;

/// The backing chunk that follows `backed` blocks of a ledger with maximum `max`: its first block
/// and size, at most [`CHUNK_BLOCKS`] and never past `max`; none from `max` on.
pub(crate) const fn next_chunk(backed: u32, max: u32) -> Option<(u32, u32)> {
    if backed >= max {
        None
    } else if max - backed < CHUNK_BLOCKS {
        Some((backed, max - backed))
    } else {
        Some((backed, CHUNK_BLOCKS))
    }
}

/// The chunks that back a ledger with `initial` and `max` blocks at most: the initial one and
/// [`next_chunk`]'s from `initial` to `max`.
pub(crate) const fn max_chunks(initial: u32, max: u32) -> usize {
    1 + max.saturating_sub(initial).div_ceil(CHUNK_BLOCKS) as usize
}

/// The most blocks a ledger posts: the mapped heap, and below K, so that the block_ctl+4
/// producer (posted modulo K) is never 0, the value of an empty block list.
pub(crate) const MAX_POSTED: u32 = if MAX_BLOCKS < BLOCK_LIST_RECORDS {
    MAX_BLOCKS
} else {
    BLOCK_LIST_RECORDS - 1
};

impl Ledger {
    /// A fresh ledger. With growth, the initial size is clamped to 64..=[`MAX_POSTED`] blocks
    /// and the maximum to initial..=[`MAX_POSTED`]; without, both are [`MAX_POSTED`].
    pub(crate) const fn new(grow: bool, blocks: u32, max_blocks: u32) -> Self {
        let (initial, max) = if grow {
            let nb = clamp(blocks, MIN_BLOCKS, MAX_POSTED);
            (nb, clamp(max_blocks, nb, MAX_POSTED))
        } else {
            (MAX_POSTED, MAX_POSTED)
        };
        Self {
            initial,
            max,
            posted: initial,
            next_request: 0,
            refusal_logged: false,
            refusals: 0,
        }
    }

    /// The live block_ctl+4 producer.
    pub(crate) const fn producer(&self) -> u32 {
        self.posted % BLOCK_LIST_RECORDS
    }

    /// Whether the refusals at the maximum are used up: further requests go unanswered.
    pub(crate) const fn exhausted(&self) -> bool {
        self.refusals >= MAX_REFUSALS
    }

    /// Whether `request` may be answered: growth on, an event of type 7 for PB slot 0 from a
    /// context with a published render prefix (`live`), a counter not below the next expected
    /// one, the current halt epoch, and refusals at the maximum not used up.
    pub(crate) fn admits(&self, request: &[u32; 14], grow: bool, live: bool, epoch: u64) -> bool {
        !self.exhausted()
            && grow
            && request[grow_request::TYPE] == 7
            && request[grow_request::SLOT] == 0
            && live
            && request[grow_request::COUNTER] >= self.next_request
            && request_epoch(request) == epoch
    }

    /// The blocks the next accepted request posts: at most [`GROW_STEP`], none at the maximum.
    pub(crate) fn next_count(&self) -> u32 {
        GROW_STEP.min(self.max - self.posted)
    }

    /// Accepts an admitted request: the next expected counter follows it, and the returned
    /// blocks `[first, first + count)` are to be written to the block list, `count` 0 meaning a
    /// refusal at the maximum.
    pub(crate) fn accept(&mut self, request: &[u32; 14]) -> (u32, u32) {
        self.next_request = request[grow_request::COUNTER].wrapping_add(1);
        let count = self.next_count();
        if count == 0 {
            self.refusals += 1;
        }
        (self.posted, count)
    }

    /// Records `count` blocks as posted after their records were written and returns the new
    /// producer.
    pub(crate) fn post(&mut self, count: u32) -> u32 {
        self.posted += count;
        self.producer()
    }

    /// Whether the answer is logged at info level: a failed reply, the first two requests, or
    /// the first refusal at the maximum since the initialization. Records the refusal.
    pub(crate) fn log_answer(
        &mut self,
        request: &[u32; 14],
        count: u32,
        reply_failed: bool,
    ) -> bool {
        let info = reply_failed
            || request[grow_request::COUNTER] < 2
            || (count == 0 && self.posted == self.max && !self.refusal_logged);
        self.refusal_logged |= count == 0;
        info
    }
}

/// The halt epoch a request names (u64 at words 5..6).
pub(crate) const fn request_epoch(request: &[u32; 14]) -> u64 {
    request[grow_request::EPOCH] as u64 | (request[grow_request::EPOCH + 1] as u64) << 32
}

const fn clamp(v: u32, lo: u32, hi: u32) -> u32 {
    if v < lo {
        lo
    } else if v > hi {
        hi
    } else {
        v
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(ctx: u32, counter: u32, epoch: u64) -> [u32; 14] {
        let mut q = [0u32; 14];
        q[0] = 7;
        q[1] = ctx;
        q[2] = 0;
        q[3] = counter;
        q[4] = 0x100;
        q[5] = epoch as u32;
        q[6] = (epoch >> 32) as u32;
        q
    }

    #[test]
    fn clamps() {
        let l = Ledger::new(true, 1024, 2048);
        assert_eq!(
            (l.initial, l.max, l.posted, l.next_request),
            (1024, 2048, 1024, 0)
        );
        assert_eq!(l.producer(), 1024);
        assert_eq!(Ledger::new(true, 10, 20).initial, 64);
        assert_eq!(Ledger::new(true, 10, 20).max, 64);
        assert_eq!(Ledger::new(true, 5000, 100).initial, 4095);
        assert_eq!(Ledger::new(true, 512, 1024).max, 1024);
        assert_eq!(Ledger::new(true, 512, 100).max, 512);
        let fixed = Ledger::new(false, 512, 1024);
        assert_eq!((fixed.initial, fixed.max, fixed.posted), (4095, 4095, 4095));
        // The values the driver passes (tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS) are kept.
        let l = Ledger::new(true, 1024, 4095);
        assert_eq!(
            (l.initial, l.max, l.posted, l.producer()),
            (1024, 4095, 1024, 1024)
        );
    }

    #[test]
    fn producer_never_returns_to_zero() {
        assert_eq!(MAX_POSTED, 4095);
        assert!(MAX_POSTED <= MAX_BLOCKS && MAX_POSTED < BLOCK_LIST_RECORDS);
        let values = [
            0,
            1,
            MIN_BLOCKS - 1,
            MIN_BLOCKS,
            MIN_BLOCKS + 1,
            1023,
            1024,
            1025,
            3071,
            3072,
            3073,
            MAX_POSTED - 1,
            MAX_POSTED,
            MAX_BLOCKS,
            MAX_BLOCKS + 1,
            BLOCK_LIST_RECORDS,
            2 * BLOCK_LIST_RECORDS,
            u32::MAX,
        ];
        for grow in [true, false] {
            for blocks in values {
                for max_blocks in values {
                    let mut l = Ledger::new(grow, blocks, max_blocks);
                    assert!(MIN_BLOCKS <= l.initial && l.initial <= l.max && l.max <= MAX_POSTED);
                    assert_eq!(l.posted, l.initial);
                    assert!((1..=MAX_POSTED).contains(&l.producer()));
                    // Every producer of a growth to the maximum stays in 1..=4095.
                    let mut counter = 0;
                    loop {
                        let (first, n) = l.accept(&request(5, counter, 1));
                        assert_eq!(first, l.posted);
                        if n == 0 {
                            break;
                        }
                        let producer = l.post(n);
                        assert!((1..=MAX_POSTED).contains(&producer));
                        assert_eq!(producer, l.posted);
                        counter += 1;
                    }
                    assert_eq!(l.posted, l.max);
                    assert_eq!(l.refusals, 1);
                }
            }
        }
    }

    #[test]
    fn admission() {
        let l = Ledger::new(true, 1024, 2048);
        let q = request(5, 0, 3);
        assert!(l.admits(&q, true, true, 3));
        assert!(!l.admits(&q, false, true, 3));
        assert!(!l.admits(&q, true, false, 3));
        // Request of an earlier halt epoch (dropped at recovery, never answered).
        assert!(!l.admits(&q, true, true, 4));
        let mut bad = q;
        bad[0] = 6;
        assert!(!l.admits(&bad, true, true, 3));
        let mut bad = q;
        bad[2] = 1;
        assert!(!l.admits(&bad, true, true, 3));
        // 64-bit epochs.
        let q = request(5, 0, 0x1_0000_0002);
        assert!(l.admits(&q, true, true, 0x1_0000_0002));
        assert!(!l.admits(&q, true, true, 2));
    }

    #[test]
    fn growth_to_the_maximum() {
        // 512 initial and 1024 maximum blocks.
        let mut l = Ledger::new(true, 512, 1024);
        let mut grown = 0;
        let mut counter = 0;
        loop {
            let q = request(5, counter, 1);
            assert!(l.admits(&q, true, true, 1));
            let (first, n) = l.accept(&q);
            assert_eq!(first, 512 + grown);
            if n == 0 {
                break;
            }
            assert!(n <= 10);
            assert_eq!(l.post(n), (512 + grown + n) % 4096);
            grown += n;
            counter += 1;
        }
        assert_eq!(l.posted, 1024);
        assert_eq!(grown, 512);
        // 51 requests of 10 blocks and one of 2.
        assert_eq!(counter, 52);
        // A stale (repeated) counter is refused, a later one accepted.
        assert!(!l.admits(&request(5, counter - 1, 1), true, true, 1));
        assert!(l.admits(&request(5, counter + 3, 1), true, true, 1));
        // One refusal was answered above; one more is, then requests go unanswered.
        assert_eq!(l.refusals, 1);
        assert!(!l.exhausted());
        let q = request(5, counter + 3, 1);
        assert_eq!(l.accept(&q), (1024, 0));
        assert!(l.exhausted());
        assert!(!l.admits(&request(5, counter + 4, 1), true, true, 1));
        // A fresh ledger starts over.
        assert!(!Ledger::new(true, 512, 1024).exhausted());
    }

    /// Backing for every ledger the constructor makes, the driver's included: the chunks from
    /// the initial one are contiguous, at most [`CHUNK_BLOCKS`] each, end at the maximum and
    /// number at most [`max_chunks`]; growth with the backing made first never posts a block
    /// past it.
    #[test]
    fn backing_chunks() {
        assert_eq!(max_chunks(1024, 4095), 1 + 96);
        assert_eq!(max_chunks(MIN_BLOCKS, MAX_POSTED), 1 + 126);
        assert_eq!(max_chunks(MAX_POSTED, MAX_POSTED), 1);
        assert_eq!(next_chunk(1024, 4095), Some((1024, 32)));
        assert_eq!(next_chunk(4064, 4095), Some((4064, 31)));
        assert_eq!(next_chunk(4095, 4095), None);
        assert_eq!(next_chunk(4096, 4095), None);
        let values = [
            0, 1, MIN_BLOCKS, 65, 1023, 1024, 1025, 3072, 4094, MAX_POSTED, 5000,
        ];
        for grow in [true, false] {
            for blocks in values {
                for max_blocks in values {
                    let mut l = Ledger::new(grow, blocks, max_blocks);
                    let (mut backed, mut chunks) = (l.initial, 1);
                    let mut counter = 0;
                    loop {
                        let end = l.posted + l.next_count();
                        while backed < end {
                            let (first, n) = next_chunk(backed, l.max).unwrap();
                            assert_eq!(first, backed);
                            assert!(n >= 1 && n <= CHUNK_BLOCKS);
                            backed += n;
                            chunks += 1;
                        }
                        let (first, n) = l.accept(&request(5, counter, 1));
                        assert!(first + n <= backed);
                        if n == 0 {
                            break;
                        }
                        l.post(n);
                        counter += 1;
                    }
                    assert_eq!((l.posted, backed), (l.max, l.max));
                    assert!(chunks <= max_chunks(l.initial, l.max));
                    assert_eq!(next_chunk(backed, l.max), None);
                }
            }
        }
    }

    #[test]
    fn producer_wraps_at_k() {
        let mut l = Ledger::new(true, 2048, 2048);
        assert_eq!(l.producer(), 2048);
        l.posted = 4095;
        assert_eq!(l.post(1), 0);
        assert_eq!(l.post(10), 10);
    }

    #[test]
    fn logging() {
        let mut l = Ledger::new(true, 64, 64);
        // The first two requests are logged.
        assert!(l.log_answer(&request(1, 0, 0), 10, false));
        assert!(l.log_answer(&request(1, 1, 0), 10, false));
        assert!(!l.log_answer(&request(1, 2, 0), 10, false));
        // A failed reply is logged.
        assert!(l.log_answer(&request(1, 5, 0), 10, true));
        // The first refusal at the maximum is logged once.
        assert!(l.log_answer(&request(1, 6, 0), 0, false));
        assert!(!l.log_answer(&request(1, 7, 0), 0, false));
        assert!(l.refusal_logged);
    }
}
