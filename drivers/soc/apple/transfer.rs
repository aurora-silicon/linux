// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use kernel::prelude::*;

pub(crate) const MARKER_FIRST: u8 = 0xFC;
pub(crate) const MARKER_NEXT: u8 = 0xFD;
/// Requests the peer's next packet and acks the final one; same marker for both.
pub(crate) const MARKER_REQUEST: u8 = 0xFE;
pub(crate) const MARKER_ERROR: u8 = 0xFF;

// A tag below MARKER_FIRST is not a marker; sound only while every marker >= it.
static_assert!(MARKER_NEXT >= MARKER_FIRST);
static_assert!(MARKER_REQUEST >= MARKER_FIRST);

#[derive(Clone, Copy)]
pub(crate) enum DeviceStatus {
    /// `0xffffffff` is a real refusal (signed −1), not the absence of an answer.
    Answered(u32),
    ReportedWithoutStatus,
    NotAnswered,
    BufferNeverWritten,
    /// A reply arrived, but its header never fully landed or failed a check.
    Malformed,
}

impl DeviceStatus {
    pub(crate) fn answered(&self) -> Option<u32> {
        match self {
            DeviceStatus::Answered(err) => Some(*err),
            DeviceStatus::ReportedWithoutStatus
            | DeviceStatus::NotAnswered
            | DeviceStatus::BufferNeverWritten
            | DeviceStatus::Malformed => None,
        }
    }

    pub(crate) fn is_ok(&self) -> bool {
        matches!(self, DeviceStatus::Answered(0))
    }
}

impl kernel::fmt::Display for DeviceStatus {
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        match self {
            DeviceStatus::Answered(err) => write!(f, "0x{:x}", err),
            DeviceStatus::ReportedWithoutStatus => write!(
                f,
                "an error report from the device carrying no status word (the device DID answer)"
            ),
            DeviceStatus::NotAnswered => write!(
                f,
                "NONE — no message arrived at all (host sentinel, not an enclave value)"
            ),
            DeviceStatus::BufferNeverWritten => write!(
                f,
                "NONE — a payload notification arrived but the outbound buffer was never written (a race with the enclave's DMA, not a silent enclave)"
            ),
            DeviceStatus::Malformed => write!(
                f,
                "NONE — a reply arrived but its header was left partly written or failed a check (host sentinel, not an enclave value)"
            ),
        }
    }
}

pub(crate) const HEADER_WORDS: usize = 7;
pub(crate) const HEADER_LEN: usize = HEADER_WORDS * 4;

pub(crate) const VERSION: u32 = 1;

pub(crate) const MAX_TRANSACTION: u32 = 0x4B000;

#[derive(Clone, Copy)]
pub(crate) struct Packet {
    pub(crate) version: u32,
    pub(crate) total: u32,
    pub(crate) offset: u32,
    pub(crate) flags: u32,
    pub(crate) err: u32,
    pub(crate) opcode: u32,
    pub(crate) chunk: u32,
}

fn le32(buf: &[u8], word: usize) -> u32 {
    let o = word * 4;
    u32::from_le_bytes([buf[o], buf[o + 1], buf[o + 2], buf[o + 3]])
}

// The header words, numbered as in `Packet::decode`, that taking a message
// reads. A data chunk reads all but flags; an error report reads only err.
// None of them can hold a fill of four equal nonzero bytes in a valid reply:
// version is VERSION, opcode is an echoed 16-bit opcode, and total, offset
// and chunk are at most MAX_TRANSACTION. err is the enclave's status. A
// poison err taken as landed would be reported as a refusal the enclave did
// not make. If the enclave ever answered with that value, waiting costs the
// poll budget and the exchange fails either way.
const DATA_WORDS: [usize; 6] = [0, 1, 2, 4, 5, 6];
const ERROR_WORDS: [usize; 1] = [4];

static_assert!(MAX_TRANSACTION < 0x0101_0101);

fn header_words_read(marker: u8) -> &'static [usize] {
    match marker {
        MARKER_FIRST | MARKER_NEXT => &DATA_WORDS,
        MARKER_ERROR => &ERROR_WORDS,
        // A grant or a notification: `Reassembly::on_chunk` reads no header.
        _ => &[],
    }
}

/// Whether a message with `marker` has a payload after its header.
pub(crate) fn carries_payload(marker: u8) -> bool {
    matches!(marker, MARKER_FIRST | MARKER_NEXT)
}

fn header_landed(header: &[u8; HEADER_LEN], words: &[usize], poison: u8) -> bool {
    let poison = u32::from_le_bytes([poison; 4]);
    words.iter().all(|&word| le32(header, word) != poison)
}

/// The host's view of the header at the start of an endpoint's outbound
/// buffer, which the enclave fills by DMA.
pub(crate) trait HeaderSlot {
    /// Copies the header out; `false` once the buffers are gone.
    fn sample(&self, header: &mut [u8; HEADER_LEN]) -> bool;
    /// Fills the header with the poison again.
    fn repoison(&self);
    /// Waits before the next sample.
    fn pause(&self);
}

/// Takes the header of a message with `marker` from `slot`, sampling it up to
/// `attempts` times until every word that taking the message reads has left
/// the `poison` fill, a nonzero byte. The mailbox notification is not ordered
/// against the enclave's DMA, so the header can be unwritten, or only partly
/// written, when the message arrives: one changed byte is not enough. A
/// message whose handling reads no header is taken without looking at the
/// buffer.
///
/// Once looked at, the header is poisoned again whatever the outcome, so the
/// next message cannot take it for its own. A message whose header is not
/// looked at, such as a grant, leaves the buffer alone: the reply may already
/// be landing behind it. [`send_for_reply`] poisons that header instead.
pub(crate) fn take_header(
    slot: &impl HeaderSlot,
    marker: u8,
    poison: u8,
    attempts: u32,
) -> Result<Packet, DeviceStatus> {
    let words = header_words_read(marker);
    if words.is_empty() {
        return Ok(Packet::NONE);
    }

    let mut header = [poison; HEADER_LEN];
    let mut landed = false;
    for attempt in 0..attempts {
        if attempt > 0 {
            slot.pause();
        }
        if !slot.sample(&mut header) {
            return Err(DeviceStatus::NotAnswered);
        }
        if header_landed(&header, words, poison) {
            landed = true;
            break;
        }
    }
    slot.repoison();

    if !landed {
        return Err(if header.iter().all(|&b| b == poison) {
            DeviceStatus::BufferNeverWritten
        } else {
            DeviceStatus::Malformed
        });
    }
    Packet::decode(&header).map_err(|_| DeviceStatus::Malformed)
}

/// Poisons the header in `slot` again, then calls `send` for a host message
/// that asks the enclave for its next one: a request, or the next chunk of
/// one. The enclave may have written a header there that nothing read, such
/// as a grant's, and the next reply must not take it for its own. Until
/// `send`, the enclave is waiting for the host, so no header can be landing.
pub(crate) fn send_for_reply<T>(
    slot: &impl HeaderSlot,
    send: impl FnOnce() -> Result<T>,
) -> Result<T> {
    slot.repoison();
    send()
}

impl Packet {
    /// Stands in for the header of a message whose handling reads none of it.
    pub(crate) const NONE: Packet = Packet {
        version: 0,
        total: 0,
        offset: 0,
        flags: 0,
        err: 0,
        opcode: 0,
        chunk: 0,
    };

    pub(crate) fn decode(buf: &[u8]) -> Result<Packet> {
        if buf.len() < HEADER_LEN {
            return Err(EINVAL);
        }
        Ok(Packet {
            version: le32(buf, 0),
            total: le32(buf, 1),
            offset: le32(buf, 2),
            flags: le32(buf, 3),
            err: le32(buf, 4),
            opcode: le32(buf, 5),
            chunk: le32(buf, 6),
        })
    }

    pub(crate) fn encode(&self, buf: &mut [u8]) -> Result<()> {
        if buf.len() < HEADER_LEN {
            return Err(EINVAL);
        }
        for (word, value) in [
            self.version,
            self.total,
            self.offset,
            self.flags,
            self.err,
            self.opcode,
            self.chunk,
        ]
        .into_iter()
        .enumerate()
        {
            buf[word * 4..word * 4 + 4].copy_from_slice(&value.to_le_bytes());
        }
        Ok(())
    }

    pub(crate) fn request_next(opcode: u32, received: u32, total: u32) -> Packet {
        Packet {
            version: VERSION,
            total,
            offset: received,
            flags: 0,
            err: 0,
            opcode,
            chunk: 0,
        }
    }
}

struct Active {
    opcode: u32,
    total: u32,
    payload: KVec<u8>,
    seq: u16,
}

pub(crate) struct Completed {
    pub(crate) status: DeviceStatus,
    pub(crate) payload: KVec<u8>,
}

pub(crate) struct Continuation {
    opcode: u32,
    received: u32,
    total: u32,
    seq: u16,
}

impl Continuation {
    pub(crate) fn opcode(&self) -> u32 {
        self.opcode
    }
    pub(crate) fn seq(&self) -> u16 {
        self.seq
    }
    pub(crate) fn packet(&self) -> Packet {
        Packet::request_next(self.opcode, self.received, self.total)
    }
}

pub(crate) enum Progress {
    NeedMore(Continuation),
    Complete,
    /// A stray chunk, dropped without touching transfer state — distinct from
    /// `Failed`, which kills the transfer.
    Ignored,
    Grant,
    Notification,
    Failed,
}

pub(crate) struct Reassembly {
    active: Option<Active>,
    done: Option<Completed>,
    sending: Option<u32>,
    grants: u32,
    // Counts exchanges begun. A reply is matched only by its opcode, so a
    // late reply to an exchange that timed out would otherwise complete the
    // next exchange with the same opcode.
    generation: u64,
}

impl Reassembly {
    pub(crate) fn new() -> Reassembly {
        Reassembly {
            active: None,
            done: None,
            sending: None,
            grants: 0,
            generation: 0,
        }
    }

    /// The exchange a message arriving now can belong to. Sampled before the
    /// message is read and passed back to [`Self::on_chunk`].
    pub(crate) fn generation(&self) -> u64 {
        self.generation
    }

    pub(crate) fn begin(&mut self, opcode: u32) -> Result<()> {
        if self.active.is_some() {
            return Err(EBUSY);
        }
        self.done = None;
        self.generation = self.generation.wrapping_add(1);
        self.active = Some(Active {
            opcode,
            total: 0,
            payload: KVec::new(),
            seq: 0,
        });
        Ok(())
    }

    pub(crate) fn begin_send(&mut self, opcode: u32) {
        self.sending = Some(opcode);
        self.grants = 0;
    }

    pub(crate) fn end_send(&mut self) {
        self.sending = None;
        self.grants = 0;
    }

    pub(crate) fn take_grant(&mut self) -> bool {
        if self.grants > 0 {
            self.grants -= 1;
            return true;
        }
        false
    }

    pub(crate) fn take_done(&mut self) -> Option<Completed> {
        self.done.take()
    }

    pub(crate) fn abort(&mut self) {
        self.abort_with(DeviceStatus::NotAnswered);
    }

    pub(crate) fn abort_with(&mut self, status: DeviceStatus) {
        if self.active.take().is_some() {
            self.done = Some(Completed {
                status,
                payload: KVec::new(),
            });
        }
    }

    fn fail(&mut self) {
        self.abort_with(DeviceStatus::Malformed);
    }

    /// Takes one chunk of a reply. `generation` is [`Self::generation`] as
    /// sampled when the message arrived: a chunk that arrived before the
    /// current exchange began belongs to an earlier one and is dropped.
    pub(crate) fn on_chunk(
        &mut self,
        generation: u64,
        marker: u8,
        packet: &Packet,
        payload: &[u8],
    ) -> Progress {
        if marker < MARKER_FIRST {
            return Progress::Notification;
        }

        // 0xFE is flow control for the request being sent, answered from `sending`.
        if marker == MARKER_REQUEST {
            return match self.sending {
                Some(_) => {
                    // Counted under the caller's lock, closing the race with the waiter.
                    self.grants = self.grants.saturating_add(1);
                    Progress::Grant
                }
                None => Progress::Ignored,
            };
        }

        let Some(active_opcode) = self.active.as_ref().map(|a| a.opcode) else {
            return Progress::Ignored;
        };
        if generation != self.generation {
            return Progress::Ignored;
        }

        // Checked before the opcode test below, deliberately.
        if marker == MARKER_ERROR {
            let status = if packet.err != 0 {
                DeviceStatus::Answered(packet.err)
            } else {
                DeviceStatus::ReportedWithoutStatus
            };
            self.finish(status, KVec::new());
            return Progress::Complete;
        }

        // The device echoes the opcode on a data chunk.
        if packet.opcode != active_opcode {
            return Progress::Ignored;
        }

        if packet.version != VERSION {
            self.fail();
            return Progress::Failed;
        }
        if packet.chunk as usize != payload.len() {
            self.fail();
            return Progress::Failed;
        }
        if packet.total > MAX_TRANSACTION {
            self.fail();
            return Progress::Failed;
        }

        match marker {
            MARKER_FIRST => {
                let Some(active) = self.active.as_ref() else {
                    return Progress::Ignored;
                };
                if !active.payload.is_empty() {
                    self.fail();
                    return Progress::Failed;
                }
                if packet.offset != 0 {
                    self.fail();
                    return Progress::Failed;
                }
                if let Some(active) = self.active.as_mut() {
                    active.total = packet.total;
                }
            }
            MARKER_NEXT => {
                let Some(active) = self.active.as_ref() else {
                    return Progress::Ignored;
                };
                if packet.offset as usize != active.payload.len() {
                    self.fail();
                    return Progress::Failed;
                }
                if packet.total != active.total {
                    self.fail();
                    return Progress::Failed;
                }
            }
            _ => {
                self.fail();
                return Progress::Failed;
            }
        }

        let Some(active) = self.active.as_mut() else {
            return Progress::Ignored;
        };

        if packet.err != 0 {
            let status = DeviceStatus::Answered(packet.err);
            let collected = core::mem::take(&mut active.payload);
            self.finish(status, collected);
            return Progress::Complete;
        }

        if active
            .payload
            .extend_from_slice(payload, GFP_KERNEL)
            .is_err()
        {
            self.abort();
            return Progress::Failed;
        }

        let received = active.payload.len() as u32;
        let total = active.total;

        if received >= total {
            let collected = core::mem::take(&mut active.payload);
            self.finish(DeviceStatus::Answered(0), collected);
            Progress::Complete
        } else {
            active.seq = active.seq.wrapping_add(1);
            Progress::NeedMore(Continuation {
                opcode: active_opcode,
                received,
                total,
                seq: active.seq,
            })
        }
    }

    fn finish(&mut self, status: DeviceStatus, payload: KVec<u8>) {
        self.active = None;
        self.done = Some(Completed { status, payload });
    }
}

#[kernel::macros::kunit_tests(apple_sep_transfer)]
mod tests {
    use super::*;
    use core::cell::Cell;

    const OPCODE: u32 = 0x48;
    const POISON: u8 = 0x5A;
    const ATTEMPTS: u32 = 200;

    fn reply(err: u32) -> Packet {
        Packet {
            version: VERSION,
            total: 0,
            offset: 0,
            flags: 0,
            err,
            opcode: OPCODE,
            chunk: 0,
        }
    }

    fn data(total: u32, offset: u32, chunk: u32) -> Packet {
        Packet {
            total,
            offset,
            chunk,
            ..reply(0)
        }
    }

    /// An outbound buffer header that the enclave's DMA fills word by word,
    /// from word 0 up, while the host pauses between samples.
    struct Dma {
        header: Cell<[u8; HEADER_LEN]>,
        incoming: Cell<[u8; HEADER_LEN]>,
        landed: Cell<usize>,
        per_pause: Cell<usize>,
        samples: Cell<u32>,
    }

    impl Dma {
        fn new() -> Dma {
            Dma {
                header: Cell::new([POISON; HEADER_LEN]),
                incoming: Cell::new([POISON; HEADER_LEN]),
                landed: Cell::new(0),
                per_pause: Cell::new(0),
                samples: Cell::new(0),
            }
        }

        /// The enclave starts writing `packet`: `now` words are visible at
        /// once, and `per_pause` more after each pause.
        fn deliver(&self, packet: &Packet, now: usize, per_pause: usize) {
            let mut bytes = [0u8; HEADER_LEN];
            let _ = packet.encode(&mut bytes);
            self.incoming.set(bytes);
            self.landed.set(0);
            self.per_pause.set(per_pause);
            self.land(now);
        }

        fn land(&self, words: usize) {
            let landed = core::cmp::min(self.landed.get() + words, HEADER_WORDS);
            let mut header = self.header.get();
            header[..landed * 4].copy_from_slice(&self.incoming.get()[..landed * 4]);
            self.header.set(header);
            self.landed.set(landed);
        }

        /// One word becomes visible on its own.
        fn show(&self, word: usize, value: u32) {
            let mut header = self.header.get();
            header[word * 4..word * 4 + 4].copy_from_slice(&value.to_le_bytes());
            self.header.set(header);
        }

        fn poisoned(&self) -> bool {
            self.header.get().iter().all(|&b| b == POISON)
        }
    }

    impl HeaderSlot for Dma {
        fn sample(&self, header: &mut [u8; HEADER_LEN]) -> bool {
            self.samples.set(self.samples.get() + 1);
            *header = self.header.get();
            true
        }

        fn repoison(&self) {
            self.header.set([POISON; HEADER_LEN]);
        }

        fn pause(&self) {
            self.land(self.per_pause.get());
        }
    }

    fn take(dma: &Dma, marker: u8) -> Result<Packet, DeviceStatus> {
        take_header(dma, marker, POISON, ATTEMPTS)
    }

    #[test]
    fn a_reply_to_the_current_exchange_completes_it() {
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        let generation = rx.generation();
        assert!(matches!(
            rx.on_chunk(generation, MARKER_FIRST, &reply(0), &[]),
            Progress::Complete
        ));
        assert!(rx.take_done().is_some_and(|done| done.status.is_ok()));
    }

    #[test]
    fn a_late_reply_does_not_complete_the_next_exchange() {
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        // The reply to the first exchange is being read when it times out.
        let late = rx.generation();
        rx.abort();
        assert!(rx.take_done().is_some());
        // A retry begins the same opcode before the late reply is delivered.
        assert!(rx.begin(OPCODE).is_ok());
        assert!(matches!(
            rx.on_chunk(late, MARKER_FIRST, &reply(0), &[]),
            Progress::Ignored
        ));
        assert!(matches!(
            rx.on_chunk(late, MARKER_ERROR, &reply(0x16), &[]),
            Progress::Ignored
        ));
        assert!(rx.take_done().is_none());
        let current = rx.generation();
        assert!(matches!(
            rx.on_chunk(current, MARKER_FIRST, &reply(0), &[]),
            Progress::Complete
        ));
    }

    #[test]
    fn a_torn_header_is_taken_once_it_has_landed() {
        let dma = Dma::new();
        // version..flags are visible; err, opcode and chunk are still poison.
        dma.deliver(&data(8, 0, 8), 4, 1);
        let taken = take(&dma, MARKER_FIRST);
        assert!(taken.is_ok());
        let Ok(packet) = taken else {
            return;
        };
        assert_eq!(packet.chunk, 8);
        assert_eq!(packet.err, 0);
        assert_eq!(packet.opcode, OPCODE);

        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        let generation = rx.generation();
        assert!(matches!(
            rx.on_chunk(generation, MARKER_FIRST, &packet, &[0u8; 8]),
            Progress::Complete
        ));
        assert!(rx.take_done().is_some_and(|done| done.status.is_ok()));
    }

    #[test]
    fn a_late_header_is_taken_whole() {
        let dma = Dma::new();
        // Nothing is visible when the message arrives; then a word per pause.
        dma.deliver(&data(8, 0, 8), 0, 1);
        let taken = take(&dma, MARKER_FIRST);
        assert!(taken.is_ok());
        let Ok(packet) = taken else {
            return;
        };
        assert_eq!(packet.total, 8);
        assert_eq!(packet.chunk, 8);
        assert_eq!(packet.opcode, OPCODE);
        assert_eq!(dma.samples.get(), HEADER_WORDS as u32 + 1);
    }

    #[test]
    fn a_header_that_never_lands_is_reported_as_never_written() {
        let dma = Dma::new();
        assert!(matches!(
            take(&dma, MARKER_FIRST),
            Err(DeviceStatus::BufferNeverWritten)
        ));
        assert_eq!(dma.samples.get(), ATTEMPTS);
    }

    #[test]
    fn a_header_left_partly_written_is_reported_as_malformed() {
        let dma = Dma::new();
        dma.deliver(&data(8, 0, 8), 4, 0);
        assert!(matches!(
            take(&dma, MARKER_FIRST),
            Err(DeviceStatus::Malformed)
        ));
    }

    #[test]
    fn flags_may_hold_the_poison() {
        let dma = Dma::new();
        let packet = Packet {
            flags: u32::from_le_bytes([POISON; 4]),
            ..data(8, 0, 8)
        };
        dma.deliver(&packet, HEADER_WORDS, 0);
        assert!(take(&dma, MARKER_FIRST).is_ok());
        assert_eq!(dma.samples.get(), 1);
    }

    #[test]
    fn an_error_report_reads_only_its_status() {
        let dma = Dma::new();
        dma.show(4, 0x16);
        let taken = take(&dma, MARKER_ERROR);
        assert!(taken.is_ok());
        let Ok(packet) = taken else {
            return;
        };
        assert_eq!(packet.err, 0x16);

        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        let generation = rx.generation();
        assert!(matches!(
            rx.on_chunk(generation, MARKER_ERROR, &packet, &[]),
            Progress::Complete
        ));
        assert!(rx
            .take_done()
            .is_some_and(|done| done.status.answered() == Some(0x16)));
    }

    #[test]
    fn a_grant_or_a_notification_reads_no_header() {
        let dma = Dma::new();
        assert!(take(&dma, MARKER_REQUEST).is_ok());
        assert!(take(&dma, MARKER_FIRST - 1).is_ok());
        assert_eq!(dma.samples.get(), 0);
    }

    #[test]
    fn a_reply_failing_a_check_is_reported_as_malformed() {
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        let generation = rx.generation();
        let bad = Packet {
            version: VERSION + 1,
            ..reply(0)
        };
        assert!(matches!(
            rx.on_chunk(generation, MARKER_FIRST, &bad, &[]),
            Progress::Failed
        ));
        assert!(rx
            .take_done()
            .is_some_and(|done| matches!(done.status, DeviceStatus::Malformed)));
    }

    #[test]
    fn the_previous_replys_header_is_not_taken_for_the_next() {
        let dma = Dma::new();
        dma.deliver(&data(8, 0, 8), HEADER_WORDS, 0);
        assert!(take(&dma, MARKER_FIRST).is_ok_and(|p| p.chunk == 8));

        // The next exchange's message arrives before any of its header.
        dma.deliver(&data(4, 0, 4), 0, HEADER_WORDS);
        let taken = take(&dma, MARKER_FIRST);
        assert!(taken.is_ok());
        let Ok(packet) = taken else {
            return;
        };
        assert_eq!(packet.chunk, 4);
        assert_eq!(packet.total, 4);
    }

    #[test]
    fn a_continuation_does_not_take_the_first_chunks_header() {
        let dma = Dma::new();
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        let generation = rx.generation();

        dma.deliver(&data(16, 0, 8), HEADER_WORDS, 0);
        let taken = take(&dma, MARKER_FIRST);
        assert!(taken.is_ok());
        let Ok(first) = taken else {
            return;
        };
        assert!(matches!(
            rx.on_chunk(generation, MARKER_FIRST, &first, &[0u8; 8]),
            Progress::NeedMore(_)
        ));

        // The second chunk's message arrives before its header lands.
        dma.deliver(&data(16, 8, 8), 0, 2);
        let taken = take(&dma, MARKER_NEXT);
        assert!(taken.is_ok());
        let Ok(next) = taken else {
            return;
        };
        assert!(matches!(
            rx.on_chunk(generation, MARKER_NEXT, &next, &[0u8; 8]),
            Progress::Complete
        ));
        assert!(rx
            .take_done()
            .is_some_and(|done| done.status.is_ok() && done.payload.len() == 16));
    }

    #[test]
    fn a_header_is_poisoned_again_once_looked_at() {
        let dma = Dma::new();
        dma.deliver(&data(8, 0, 8), HEADER_WORDS, 0);
        assert!(take(&dma, MARKER_FIRST).is_ok());
        assert!(dma.poisoned());

        dma.deliver(&data(8, 0, 8), 4, 0);
        assert!(take(&dma, MARKER_FIRST).is_err());
        assert!(dma.poisoned());

        // A grant leaves the buffer alone; the host's next send poisons it.
        dma.show(4, 0x16);
        assert!(take(&dma, MARKER_REQUEST).is_ok());
        assert!(!dma.poisoned());
        assert!(send_for_reply(&dma, || Ok(())).is_ok());
        assert!(dma.poisoned());
    }

    /// The enclave's grant writes a request-next header, as the host's own
    /// grant does, and the host sends its next chunk.
    fn grant_then_send(dma: &Dma) {
        dma.deliver(
            &Packet::request_next(OPCODE, 0x3fe4, 0x8000),
            HEADER_WORDS,
            0,
        );
        assert!(take(dma, MARKER_REQUEST).is_ok());
        assert!(send_for_reply(dma, || Ok(())).is_ok());
    }

    #[test]
    fn a_grants_header_is_not_taken_for_the_reply() {
        let dma = Dma::new();
        let mut rx = Reassembly::new();
        assert!(rx.begin(OPCODE).is_ok());
        rx.begin_send(OPCODE);
        let generation = rx.generation();
        grant_then_send(&dma);
        assert!(matches!(
            rx.on_chunk(generation, MARKER_REQUEST, &Packet::NONE, &[]),
            Progress::Grant
        ));
        rx.end_send();

        // The reply arrives before any of its header.
        dma.deliver(&data(8, 0, 8), 0, HEADER_WORDS);
        let taken = take(&dma, MARKER_FIRST);
        assert!(taken.is_ok());
        let Ok(packet) = taken else {
            return;
        };
        assert_eq!(packet.offset, 0);
        assert_eq!(packet.chunk, 8);
        assert!(matches!(
            rx.on_chunk(generation, MARKER_FIRST, &packet, &[0u8; 8]),
            Progress::Complete
        ));
        assert!(rx.take_done().is_some_and(|done| done.status.is_ok()));
    }

    #[test]
    fn a_grants_header_does_not_hide_an_error_status() {
        let dma = Dma::new();
        grant_then_send(&dma);
        dma.deliver(&reply(0x16), 0, HEADER_WORDS);
        assert!(take(&dma, MARKER_ERROR).is_ok_and(|p| p.err == 0x16));
    }

    #[test]
    fn the_header_is_poisoned_before_the_host_sends() {
        let dma = Dma::new();
        dma.deliver(&data(8, 0, 8), HEADER_WORDS, 0);
        let sent = send_for_reply(&dma, || {
            assert!(dma.poisoned());
            Ok(7u32)
        });
        assert!(sent.is_ok_and(|v| v == 7));
        assert!(send_for_reply(&dma, || -> Result<()> { Err(EIO) }).is_err());
    }
}
