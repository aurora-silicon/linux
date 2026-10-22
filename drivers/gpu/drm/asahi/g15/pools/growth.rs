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
//! The heap's VA range covers [`MAX_BLOCKS`] blocks; its RAM comes in chunks of
//! [`CHUNK_BLOCKS`] blocks from the heap base, which the ledger counts as `backed`. A request is
//! granted from the backed blocks above `posted`. A chunk is due ([`Ledger::refill_due`])
//! whenever fewer than [`RESERVE_BLOCKS`] backed blocks remain above the posted count, so that
//! it can be backed after a reply; a request that finds no backed block waits
//! ([`Answer::Wait`]) and keeps its counter. A re-initialization ([`Ledger::reinit`]) returns
//! the posted count to the initial size and keeps the backing.
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
    /// Blocks with RAM behind them, from the heap base.
    pub(crate) backed: u32,
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

/// Blocks of each PB backing chunk (16 MiB).
pub(crate) const CHUNK_BLOCKS: u32 = 128;

/// Backed blocks kept above the posted count: at 1-2 ms per request of ten blocks, 13 requests
/// of lead for the allocation of the next chunk.
pub(crate) const RESERVE_BLOCKS: u32 = CHUNK_BLOCKS;

/// Chunks of the whole heap.
pub(crate) const MAX_CHUNKS: u32 = MAX_BLOCKS / CHUNK_BLOCKS;

/// The answer to an admitted growth request.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum Answer {
    /// Post blocks `[first, first + count)` (`count` at most [`GROW_STEP`]) and reply grown.
    Grant { first: u32, count: u32 },
    /// Reply refused: the maximum is posted.
    Refuse,
    /// No backed block above the posted count: leave the request queued until a chunk is
    /// backed.
    Wait,
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
            backed: 0,
            next_request: 0,
            refusal_logged: false,
            refusals: 0,
        }
    }

    /// A re-initialization: the initial blocks are posted again, the request counter and the
    /// refusals start over, the backing stays.
    pub(crate) const fn reinit(&mut self) {
        self.posted = self.initial;
        self.next_request = 0;
        self.refusal_logged = false;
        self.refusals = 0;
    }

    /// The live block_ctl+4 producer.
    pub(crate) const fn producer(&self) -> u32 {
        self.posted % BLOCK_LIST_RECORDS
    }

    /// Records one more backed chunk.
    pub(crate) const fn back_chunk(&mut self) {
        self.backed += CHUNK_BLOCKS;
    }

    /// Backed blocks above the posted count.
    pub(crate) const fn spare(&self) -> u32 {
        self.backed.saturating_sub(self.posted)
    }

    /// Whether another chunk is due: fewer than the reserve above the posted count and the heap
    /// not yet backed whole.
    pub(crate) const fn refill_due(&self) -> bool {
        self.backed < MAX_BLOCKS && self.spare() < RESERVE_BLOCKS
    }

    /// Chunks that `blocks` posted blocks and the reserve need.
    pub(crate) const fn chunks_for(blocks: u32) -> u32 {
        let n = (blocks + RESERVE_BLOCKS).div_ceil(CHUNK_BLOCKS);
        if n > MAX_CHUNKS {
            MAX_CHUNKS
        } else {
            n
        }
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

    /// Accepts an admitted request. Granted (at most [`GROW_STEP`] blocks, and no more than are
    /// backed) or refused at the maximum, the next expected counter follows it. A request that
    /// finds no backed block waits: its counter is not consumed, and it is accepted again once a
    /// chunk is backed.
    pub(crate) fn accept(&mut self, request: &[u32; 14]) -> Answer {
        let room = self.max - self.posted;
        if room != 0 && self.spare() == 0 {
            return Answer::Wait;
        }
        self.next_request = request[grow_request::COUNTER].wrapping_add(1);
        if room == 0 {
            self.refusals += 1;
            return Answer::Refuse;
        }
        Answer::Grant {
            first: self.posted,
            count: GROW_STEP.min(room).min(self.spare()),
        }
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

    /// A ledger with the heap backed whole.
    fn backed(grow: bool, blocks: u32, max_blocks: u32) -> Ledger {
        let mut l = Ledger::new(grow, blocks, max_blocks);
        l.backed = MAX_BLOCKS;
        l
    }

    #[test]
    fn clamps() {
        let l = backed(true, 1024, 2048);
        assert_eq!(
            (l.initial, l.max, l.posted, l.backed, l.next_request),
            (1024, 2048, 1024, 4096, 0)
        );
        assert_eq!(l.producer(), 1024);
        assert_eq!(Ledger::new(true, 10, 20).initial, 64);
        assert_eq!(Ledger::new(true, 10, 20).max, 64);
        assert_eq!(Ledger::new(true, 5000, 100).initial, 4095);
        assert_eq!(Ledger::new(true, 512, 1024).max, 1024);
        assert_eq!(Ledger::new(true, 512, 100).max, 512);
        assert_eq!(Ledger::new(true, 512, 100).backed, 0);
        let fixed = Ledger::new(false, 512, 1024);
        assert_eq!((fixed.initial, fixed.max, fixed.posted), (4095, 4095, 4095));
        // The values the driver passes (g15_pb_blocks 64, tuning::PB_MAX_BLOCKS) are kept.
        let l = Ledger::new(true, 64, 4095);
        assert_eq!(
            (l.initial, l.max, l.posted, l.producer()),
            (64, 4095, 64, 64)
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
                    let mut l = backed(grow, blocks, max_blocks);
                    assert!(MIN_BLOCKS <= l.initial && l.initial <= l.max && l.max <= MAX_POSTED);
                    assert_eq!(l.posted, l.initial);
                    assert!((1..=MAX_POSTED).contains(&l.producer()));
                    // Every producer of a growth to the maximum stays in 1..=4095.
                    let mut counter = 0;
                    loop {
                        let (first, n) = match l.accept(&request(5, counter, 1)) {
                            Answer::Grant { first, count } => (first, count),
                            Answer::Refuse => break,
                            Answer::Wait => panic!("backed whole"),
                        };
                        assert_eq!(first, l.posted);
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
        let l = backed(true, 1024, 2048);
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
        let mut l = backed(true, 512, 1024);
        let mut grown = 0;
        let mut counter = 0;
        loop {
            let q = request(5, counter, 1);
            assert!(l.admits(&q, true, true, 1));
            let n = match l.accept(&q) {
                Answer::Grant { first, count } => {
                    assert_eq!(first, 512 + grown);
                    count
                }
                Answer::Refuse => break,
                Answer::Wait => panic!("backed whole"),
            };
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
        assert_eq!(l.accept(&q), Answer::Refuse);
        assert!(l.exhausted());
        assert!(!l.admits(&request(5, counter + 4, 1), true, true, 1));
        // A fresh ledger starts over, and so does a re-initialized one (backing kept).
        assert!(!Ledger::new(true, 512, 1024).exhausted());
        l.reinit();
        assert_eq!(
            (l.posted, l.next_request, l.refusals, l.backed),
            (512, 0, 0, 4096)
        );
        assert!(!l.exhausted() && !l.refusal_logged);
    }

    #[test]
    fn producer_wraps_at_k() {
        let mut l = backed(true, 2048, 2048);
        assert_eq!(l.producer(), 2048);
        l.posted = 4095;
        assert_eq!(l.post(1), 0);
        assert_eq!(l.post(10), 10);
    }

    #[test]
    fn backing() {
        // 64 initial blocks: two chunks (the initial posting and the reserve).
        assert_eq!(Ledger::chunks_for(64), 2);
        assert_eq!(Ledger::chunks_for(1024), 9);
        assert_eq!(Ledger::chunks_for(4095), MAX_CHUNKS);
        assert_eq!(MAX_CHUNKS, 32);
        let mut l = Ledger::new(true, 64, 4095);
        assert_eq!(l.spare(), 0);
        assert!(l.refill_due());
        // Nothing backed: a request waits and the counter does not advance.
        let q = request(3, 0, 1);
        assert_eq!(l.accept(&q), Answer::Wait);
        assert_eq!(l.next_request, 0);
        l.back_chunk();
        l.back_chunk();
        assert_eq!((l.backed, l.spare()), (256, 192));
        assert!(!l.refill_due());
        // Granting until the reserve line, then a refill is due; at the end of the backing the
        // grant is clipped and the next request waits.
        assert_eq!(
            l.accept(&q),
            Answer::Grant {
                first: 64,
                count: 10
            }
        );
        assert_eq!(l.post(10), 74);
        assert_eq!(l.next_request, 1);
        l.posted = 128;
        assert!(!l.refill_due());
        l.posted = 129;
        assert!(l.refill_due());
        l.posted = 250;
        assert_eq!(
            l.accept(&request(3, 1, 1)),
            Answer::Grant {
                first: 250,
                count: 6
            }
        );
        l.post(6);
        assert_eq!(l.accept(&request(3, 2, 1)), Answer::Wait);
        // The heap end: no refill past the whole heap, the maximum refuses.
        l.backed = MAX_BLOCKS;
        l.posted = 4090;
        assert!(!l.refill_due());
        assert_eq!(
            l.accept(&request(3, 2, 1)),
            Answer::Grant {
                first: 4090,
                count: 5
            }
        );
        l.post(5);
        assert_eq!(l.accept(&request(3, 3, 1)), Answer::Refuse);
        // Re-initialization keeps the backing.
        l.reinit();
        assert_eq!((l.posted, l.backed), (64, 4096));
    }

    /// Every ledger the constructor makes, the driver's included, backed a chunk at a time
    /// whenever the reserve is short: no grant posts a block past the backing, no request waits,
    /// and the backing ends within the heap.
    #[test]
    fn backing_ahead_of_every_grant() {
        let values = [
            0, 1, MIN_BLOCKS, 65, 1023, 1024, 1025, 3072, 4094, MAX_POSTED, 5000,
        ];
        for grow in [true, false] {
            for blocks in values {
                for max_blocks in values {
                    let mut l = Ledger::new(grow, blocks, max_blocks);
                    for _ in 0..Ledger::chunks_for(l.initial) {
                        l.back_chunk();
                    }
                    let mut counter = 0;
                    loop {
                        match l.accept(&request(5, counter, 1)) {
                            Answer::Grant { first, count } => {
                                assert_eq!(first, l.posted);
                                assert!(count >= 1 && first + count <= l.backed);
                                l.post(count);
                            }
                            Answer::Refuse => break,
                            Answer::Wait => panic!("waited at {} of {}", l.posted, l.backed),
                        }
                        counter += 1;
                        while l.refill_due() {
                            l.back_chunk();
                        }
                    }
                    assert_eq!(l.posted, l.max);
                    assert!(l.max <= l.backed && l.backed <= MAX_BLOCKS);
                }
            }
        }
    }

    #[test]
    fn grants_only_backed_blocks() {
        let mut l = Ledger::new(true, 64, 4095);
        // Nothing backed above the initial posting: a request waits and keeps its counter.
        l.backed = 64;
        let q = request(3, 0, 1);
        assert_eq!(l.accept(&q), Answer::Wait);
        assert_eq!(l.next_request, 0);
        // A grant is clipped to the backed blocks; the next request waits again.
        l.backed = 70;
        assert_eq!(
            l.accept(&q),
            Answer::Grant {
                first: 64,
                count: 6
            }
        );
        l.post(6);
        assert_eq!(l.next_request, 1);
        assert_eq!(l.accept(&request(3, 1, 1)), Answer::Wait);
        // At the maximum the refusal does not wait for backing.
        l.posted = l.max;
        l.backed = l.max;
        assert_eq!(l.accept(&request(3, 1, 1)), Answer::Refuse);
        // A re-initialization keeps the backing.
        l.reinit();
        assert_eq!((l.posted, l.backed, l.next_request), (64, 4095, 0));
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
