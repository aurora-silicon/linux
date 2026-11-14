// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's boot-policy endpoint (`pnon`).
//!
//! The service behind it keeps the LocalPolicy anti-replay state: the nonce
//! that the blessed LocalPolicy of the running OS is bound to stays inside the
//! enclave, and the service hands out its SHA-384 digest. That digest is the
//! enclave's own account of which LocalPolicy admitted this boot. It changes
//! only when the policy is replaced (from recoveryOS), so it lets the boot
//! chain report be checked against something no earlier boot stage can
//! write.
//!
//! The driver sends exactly one request: read the blessed LocalPolicy nonce
//! digest, once after the bring-up. Recorded on a J616s under macOS 27 (a
//! system daemon asks the same at every display power change):
//!
//! ```text
//! TX 0x0000_0020_0000_0215   endpoint 0x15, call tag 2, 32 request bytes out of line
//!    out of line: "hcPB" (request header), operation 0x0e, 24 zero bytes
//! RX 0x0000_0050_0000_0215   call tag 2, status 0, 80 reply bytes out of line
//!    out of line: "hrPB" (reply header), result 0, 24 zero bytes, 48-byte digest
//! ```
//!
//! The mailbox word carries no operation. Two mailbox formats exist, and
//! the endpoint's discovery layout says which one the enclave expects:
//!
//! * five to seven pages (M4 Pro): call tag 2, `data` is the request
//!   length; in the reply `type` is the status and `data` the reply length;
//! * two to four pages (M1 Pro, M2, A18 Pro): call tag 1, the request
//!   length in bits 16-31 and `data` zero; in the reply bits 16-31 are the
//!   reply length and `data` the service result.
//!
//! The out-of-line request and reply are the same in both. On an A18 Pro
//! the tag 2 word is answered with tag 2 and `data` 1 (a refusal with no
//! reply bytes), so the format is chosen from the layout and never retried
//! in the other one.
//!
//! Nothing here is reachable from userspace except the digest itself,
//! read-only for root in `diag/local_policy_digest`. No operation that
//! changes policy or nonce state is sent, and a message the driver did not
//! ask for is counted as unhandled.

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_PNON: u8 = 0x15;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`).
pub(crate) const PNON_NAME: u32 = u32::from_be_bytes(*b"pnon");

/// One enclave page.
pub(crate) const SEP_PAGE: usize = 0x4000;

/// The endpoint's buffer limits from its discovery configuration word, in
/// enclave pages. Byte for byte from the top: the least the host-to-enclave
/// buffer may be, the least the enclave-to-host buffer may be, then the most
/// each may be. `sks` advertises 2/1/2/1 and the driver registers two pages
/// out and one back for it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Layout {
    pub(crate) send_min: u8,
    pub(crate) receive_min: u8,
    pub(crate) send_max: u8,
    pub(crate) receive_max: u8,
}

impl Layout {
    pub(crate) const fn from_word(word: u32) -> Layout {
        Layout {
            send_min: (word >> 24) as u8,
            receive_min: (word >> 16) as u8,
            send_max: (word >> 8) as u8,
            receive_max: word as u8,
        }
    }
}

/// The layout the endpoint advertises on M4 Pro: five to seven pages each
/// way.
pub(crate) const LAYOUT_7: Layout = Layout {
    send_min: 5,
    receive_min: 5,
    send_max: 7,
    receive_max: 7,
};

/// The layout the endpoint advertises on M1 Pro, M2 and A18 Pro: two to
/// four pages each way.
pub(crate) const LAYOUT_4: Layout = Layout {
    send_min: 2,
    receive_min: 2,
    send_max: 4,
    receive_max: 4,
};

/// The mailbox format of a call and its reply.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Carrier {
    /// Tag 2; lengths in `data`, the status in `type` (M4 Pro).
    Tag2,
    /// Tag 1; lengths in bits 16-31, the service result in `data` (M1 Pro,
    /// M2, A18 Pro).
    Tag1,
}

impl Carrier {
    const fn tag(self) -> u8 {
        match self {
            Carrier::Tag2 => 2,
            Carrier::Tag1 => 1,
        }
    }
}

/// The mailbox format and buffer sizes (send, receive) for a discovery
/// layout word, if it is one of the two the driver knows. The sizes are
/// what macOS registers: seven pages out and five back on the five-to-seven
/// layout, the largest size each way (four pages) on the two-to-four one.
/// Any other layout is left alone.
pub(crate) fn offer(word: u32) -> Option<(Carrier, usize, usize)> {
    let layout = Layout::from_word(word);
    if layout == LAYOUT_7 {
        Some((
            Carrier::Tag2,
            usize::from(layout.send_max) * SEP_PAGE,
            usize::from(layout.receive_min) * SEP_PAGE,
        ))
    } else if layout == LAYOUT_4 {
        Some((
            Carrier::Tag1,
            usize::from(layout.send_max) * SEP_PAGE,
            usize::from(layout.receive_max) * SEP_PAGE,
        ))
    } else {
        None
    }
}

/// What the driver allocates each way: the largest size [`offer`] can
/// return.
pub(crate) const OOL_SIZE_PNON: usize = 7 * SEP_PAGE;

/// Request and reply header signatures, as they travel: the ASCII bytes
/// "hcPB" and "hrPB" read as little-endian words.
pub(crate) const REQUEST_SIGNATURE: u32 = u32::from_le_bytes(*b"hcPB");
pub(crate) const REPLY_SIGNATURE: u32 = u32::from_le_bytes(*b"hrPB");

/// The operation that reads the blessed LocalPolicy nonce digest.
pub(crate) const OP_READ_BLESSED_DIGEST: u32 = 0x0e;

/// Request and reply headers: signature, operation (request) or result
/// (reply), then 24 reserved bytes.
pub(crate) const HEADER_LEN: usize = 32;
pub(crate) const DIGEST_LEN: usize = 48;
pub(crate) const DIGEST_REPLY_LEN: usize = HEADER_LEN + DIGEST_LEN;
static_assert!(HEADER_LEN <= 2 * SEP_PAGE && DIGEST_REPLY_LEN <= 2 * SEP_PAGE);

/// How long the enclave gets to answer. It answered within 0.2 ms in the
/// recording.
pub(crate) const PNON_TIMEOUT_MS: u32 = 2000;

/// The request word for `len` out-of-line bytes.
pub(crate) const fn request_message(carrier: Carrier, len: u16) -> Message {
    let len_bits = match carrier {
        Carrier::Tag2 => (len as u64) << MSG_DATA_SHIFT,
        Carrier::Tag1 => (len as u64) << MSG_TYPE_SHIFT,
    };
    Message {
        msg0: (EP_PNON as u64) | ((carrier.tag() as u64) << MSG_TAG_SHIFT) | len_bits,
        msg1: 0,
    }
}

// The request word in the recording.
static_assert!(request_message(Carrier::Tag2, HEADER_LEN as u16).msg0 == 0x0000_0020_0000_0215);
static_assert!(request_message(Carrier::Tag1, HEADER_LEN as u16).msg0 == 0x0000_0000_0020_0115);

/// The out-of-line bytes of a digest read: the header alone.
pub(crate) fn digest_request() -> [u8; HEADER_LEN] {
    let mut out = [0u8; HEADER_LEN];
    out[0..4].copy_from_slice(&REQUEST_SIGNATURE.to_le_bytes());
    out[4..8].copy_from_slice(&OP_READ_BLESSED_DIGEST.to_le_bytes());
    out
}

/// What a reply word says, before its out-of-line bytes are looked at.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Arrival {
    /// The answer to a call: its status and how many bytes it wrote.
    Reply { status: u32, len: u32 },
    /// Anything else.
    Other,
}

/// Reads a word that arrived on the endpoint as the answer to a call made
/// in `carrier`.
pub(crate) fn classify(carrier: Carrier, msg: &Message) -> Arrival {
    let f = decode(msg);
    if f.ep != EP_PNON || f.tag != carrier.tag() {
        return Arrival::Other;
    }
    match carrier {
        Carrier::Tag2 if f.param == 0 => Arrival::Reply {
            status: u32::from(f.ty),
            len: f.data_lo,
        },
        Carrier::Tag2 => Arrival::Other,
        Carrier::Tag1 => Arrival::Reply {
            status: f.data_lo,
            len: ((msg.msg0 >> MSG_TYPE_SHIFT) & 0xffff) as u32,
        },
    }
}

/// The result of one digest read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// The service answered with this digest.
    Digest([u8; DIGEST_LEN]),
    /// The enclave answered with this nonzero status (mailbox or header).
    Refused(u32),
    /// The answer was not the one asked for, or its header did not check out.
    Malformed,
    /// No answer within [`PNON_TIMEOUT_MS`].
    NoReply,
}

/// Checks a digest reply: `status` and `len` from the mailbox word, `bytes`
/// the first [`DIGEST_REPLY_LEN`] bytes of the receive buffer.
pub(crate) fn check_reply(status: u32, len: u32, bytes: &[u8]) -> Outcome {
    if status != 0 {
        return Outcome::Refused(status);
    }
    let len = len as usize;
    if len < HEADER_LEN || bytes.len() < HEADER_LEN || bytes.len() < len.min(DIGEST_REPLY_LEN) {
        return Outcome::Malformed;
    }
    let word =
        |at: usize| u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]]);
    if word(0) != REPLY_SIGNATURE {
        return Outcome::Malformed;
    }
    // A refusal can come back as a header alone.
    let result = word(4);
    if result != 0 {
        return Outcome::Refused(result);
    }
    if len != DIGEST_REPLY_LEN || bytes.len() < DIGEST_REPLY_LEN {
        return Outcome::Malformed;
    }
    let mut digest = [0u8; DIGEST_LEN];
    digest.copy_from_slice(&bytes[HEADER_LEN..DIGEST_REPLY_LEN]);
    Outcome::Digest(digest)
}

/// The single exchange the endpoint can have in flight.
pub(crate) struct Exchange {
    waiting: bool,
    generation: u64,
    carrier: Carrier,
    outcome: Option<Outcome>,
}

impl Exchange {
    pub(crate) fn new() -> Exchange {
        Exchange {
            waiting: false,
            generation: 0,
            carrier: Carrier::Tag2,
            outcome: None,
        }
    }

    /// Starts an exchange sent in `carrier`; returns its number.
    pub(crate) fn begin(&mut self, carrier: Carrier) -> u64 {
        self.generation = self.generation.wrapping_add(1);
        self.waiting = true;
        self.carrier = carrier;
        self.outcome = None;
        self.generation
    }

    /// The exchange a message arriving now can answer, if one is waiting,
    /// and the mailbox format its answer comes in.
    pub(crate) fn current(&self) -> Option<(u64, Carrier)> {
        if self.waiting {
            Some((self.generation, self.carrier))
        } else {
            None
        }
    }

    /// Records the answer to exchange `generation`. `false` if it is not the
    /// one waiting: it timed out, or an answer was already taken.
    pub(crate) fn answer(&mut self, generation: u64, outcome: Outcome) -> bool {
        if !self.waiting || generation != self.generation || self.outcome.is_some() {
            return false;
        }
        self.outcome = Some(outcome);
        true
    }

    pub(crate) fn take(&mut self) -> Option<Outcome> {
        let outcome = self.outcome.take();
        if outcome.is_some() {
            self.waiting = false;
        }
        outcome
    }

    pub(crate) fn end(&mut self) {
        self.waiting = false;
        self.outcome = None;
    }
}

impl SepData {
    /// The mailbox format and buffer sizes for the endpoint, if the enclave
    /// offers it as the driver knows it.
    fn pnon_offered(&self) -> Option<(Carrier, usize, usize)> {
        if !self.endpoint_present(EP_PNON) || !self.pnon_named.load(Relaxed) {
            return None;
        }
        offer(self.pnon_layout.load(Relaxed))
    }

    /// Records the discovery messages of the endpoint.
    pub(crate) fn pnon_discovered(&self, f: &Fields) {
        if f.param != EP_PNON {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.pnon_named.store(f.data_lo == PNON_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.pnon_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    /// Queues the digest read, once after the bring-up. Runs on a freezable
    /// queue, so a suspend waits for an exchange in flight.
    pub(crate) fn queue_pnon_digest(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        let _ = workqueue::system_freezable().enqueue::<Arc<SepData>, PNON_WORK_ID>(this);
    }

    pub(crate) fn run_pnon_digest(&self) {
        if self.shutting_down.load(Relaxed) {
            return;
        }
        // Once per probe: the digest changes only when the policy is
        // replaced, which needs a reboot.
        if self.pnon_done.load(Relaxed) {
            return;
        }
        let Some((carrier, send, receive)) = self.pnon_offered() else {
            if self.endpoint_present(EP_PNON) {
                dev_dbg!(
                    self.dev,
                    "pnon: the endpoint advertises layout {:#010x}; left alone\n",
                    self.pnon_layout.load(Relaxed)
                );
            }
            self.pnon_done.store(true, Relaxed);
            diag::pnon(diag::Pnon::Absent, None);
            return;
        };
        let registered = self
            .size_ool(&self.ool_pnon, (send, receive))
            .and_then(|()| self.register_ool(&self.ool_pnon));
        if let Err(e) = registered {
            dev_err_ratelimited!(
                self.dev,
                "pnon: out-of-line buffer registration failed ({:?})\n",
                e
            );
            self.pnon_done.store(true, Relaxed);
            diag::pnon(diag::Pnon::Failed, None);
            return;
        }
        self.pnon_done.store(true, Relaxed);
        match self.pnon_read_digest(carrier) {
            Ok(Outcome::Digest(digest)) => {
                diag::pnon(diag::Pnon::Read, Some(&digest));
                dev_dbg!(
                    self.dev,
                    "pnon: read the LocalPolicy nonce digest ({:?})\n",
                    carrier
                );
            }
            Ok(outcome) => {
                diag::pnon(diag::Pnon::Failed, None);
                dev_warn_ratelimited!(
                    self.dev,
                    "pnon: the boot-policy service did not return the digest ({:?})\n",
                    outcome
                );
            }
            Err(e) => {
                diag::pnon(diag::Pnon::Failed, None);
                dev_dbg!(self.dev, "pnon: digest read not sent ({:?})\n", e);
            }
        }
    }

    fn pnon_read_digest(&self, carrier: Carrier) -> Result<Outcome> {
        self.pnon_rx.lock().begin(carrier);
        // The last answer must not be taken for this one's.
        self.ool_repoison(&self.ool_pnon, 0, DIGEST_REPLY_LEN);
        let sent = self
            .ool_write(&self.ool_pnon, 0, &digest_request())
            .and_then(|()| self.send(request_message(carrier, HEADER_LEN as u16)));
        if let Err(e) = sent {
            self.pnon_rx.lock().end();
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(PNON_TIMEOUT_MS);
        let mut guard = self.pnon_rx.lock();
        loop {
            if self.shutting_down.load(Relaxed) {
                guard.end();
                return Err(ENODEV);
            }
            if let Some(outcome) = guard.take() {
                return Ok(outcome);
            }
            if remaining == 0 {
                guard.end();
                return Ok(Outcome::NoReply);
            }
            // Uninterruptible, as the other reply waits.
            match self.pnon_wq.wait_timeout(&mut guard, remaining) {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { .. } => remaining = 0,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    pub(crate) fn on_pnon(&self, msg: Message) {
        // Sampled before the buffer is read: an exchange that times out
        // meanwhile and is followed by another must not take this answer.
        let Some((generation, carrier)) = self.pnon_rx.lock().current() else {
            self.pnon_unexpected(&msg);
            return;
        };
        let Arrival::Reply { status, len } = classify(carrier, &msg) else {
            self.pnon_unexpected(&msg);
            return;
        };
        let outcome = if status != 0 {
            check_reply(status, len, &[])
        } else if !(HEADER_LEN..=DIGEST_REPLY_LEN).contains(&(len as usize))
            || !self.ool_await_written(&self.ool_pnon, 0, len as usize)
        {
            Outcome::Malformed
        } else {
            match self.ool_read(&self.ool_pnon, 0, len as usize) {
                Ok(bytes) => check_reply(status, len, &bytes),
                Err(_) => Outcome::Malformed,
            }
        };
        if outcome == Outcome::Malformed {
            self.pnon_malformed(&msg);
        }
        self.ool_repoison(&self.ool_pnon, 0, DIGEST_REPLY_LEN);
        if self.pnon_rx.lock().answer(generation, outcome) {
            self.pnon_wq.notify_all();
        }
    }

    /// Says what a reply that did not check out looked like: its word, the
    /// reply header (signature and result, never the digest) and where the
    /// enclave's bytes start in the receive buffer.
    fn pnon_malformed(&self, msg: &Message) {
        let mut header = [0u8; 8];
        let sampled = self.ool_peek(&self.ool_pnon, 0, &mut header);
        let mut first = None;
        let mut chunk = [0u8; 64];
        let mut at = 0;
        while first.is_none() && at + chunk.len() <= 2 * SEP_PAGE {
            if !self.ool_peek(&self.ool_pnon, at, &mut chunk) {
                break;
            }
            first = chunk
                .iter()
                .position(|&b| b != OOL_POISON_OUTBOUND)
                .map(|i| at + i);
            at += chunk.len();
        }
        dev_info_ratelimited!(
            self.dev,
            "pnon: reply msg0 {:#018x}, header {:02x?} (sampled {}), first enclave byte at {:?}\n",
            msg.msg0,
            header,
            sampled,
            first
        );
    }

    fn pnon_unexpected(&self, msg: &Message) {
        if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "pnon: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_pnon)]
mod tests {
    use super::*;

    // Recorded on a J616s under macOS 27.0 (lane SEPENDPTS, 11-13): the
    // mailbox words of a blessed-digest read and the request header the host
    // put in the send buffer.
    const TRACE_TX: u64 = 0x0000_0020_0000_0215;
    const TRACE_RX: u64 = 0x0000_0050_0000_0215;
    const TRACE_REQUEST: [u8; HEADER_LEN] = [
        0x68, 0x63, 0x50, 0x42, 0x0e, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ];
    // The reply header the enclave wrote (the digest itself is not kept).
    const TRACE_REPLY_HEADER: [u8; 8] = [0x68, 0x72, 0x50, 0x42, 0x00, 0x00, 0x00, 0x00];
    // The endpoint's discovery messages in our Linux boot logs: M1 Pro, M2
    // and A18 Pro, then M4 Pro.
    const DISCOVER_NAME: u64 = 0x706e_6f6e_1500_00fd;
    const DISCOVER_LAYOUT_4: u64 = 0x0202_0404_1501_00fd;
    const DISCOVER_LAYOUT_7: u64 = 0x0505_0707_1501_00fd;
    // sks, whose sizes the driver registers already.
    const SKS_LAYOUT: u32 = 0x0201_0201;
    // The A18 Pro's answer to the tag 2 request (Linux, 11-13): tag 2,
    // `data` 1, no reply bytes written.
    const NEO_TAG2_ANSWER: u64 = 0x0000_0001_0000_0215;
    // The tag 1 request, its success reply and a refusal with result 1.
    const TAG1_TX: u64 = 0x0000_0000_0020_0115;
    const TAG1_RX: u64 = 0x0000_0000_0050_0115;
    const TAG1_REFUSED: u64 = 0x0000_0001_0000_0115;

    fn word(msg0: u64) -> Message {
        Message { msg0, msg1: 0 }
    }

    fn reply(digest_byte: u8) -> [u8; DIGEST_REPLY_LEN] {
        let mut r = [0u8; DIGEST_REPLY_LEN];
        r[..8].copy_from_slice(&TRACE_REPLY_HEADER);
        r[HEADER_LEN..].fill(digest_byte);
        r
    }

    #[test]
    fn the_request_matches_the_recording() {
        let msg = request_message(Carrier::Tag2, HEADER_LEN as u16);
        assert_eq!(msg.msg0, TRACE_TX);
        assert_eq!(msg.msg1, 0);
        assert_eq!(digest_request(), TRACE_REQUEST);
    }

    #[test]
    fn the_tag1_request_carries_the_length_in_bits_16_to_31() {
        let msg = request_message(Carrier::Tag1, HEADER_LEN as u16);
        assert_eq!(msg.msg0, TAG1_TX);
        assert_eq!(msg.msg1, 0);
        let f = decode(&msg);
        assert_eq!((f.ep, f.tag, f.data_lo), (EP_PNON, 1, 0));
    }

    #[test]
    fn tag1_replies_carry_the_result_in_data() {
        assert_eq!(
            classify(Carrier::Tag1, &word(TAG1_RX)),
            Arrival::Reply {
                status: 0,
                len: DIGEST_REPLY_LEN as u32
            }
        );
        assert_eq!(
            classify(Carrier::Tag1, &word(TAG1_REFUSED)),
            Arrival::Reply { status: 1, len: 0 }
        );
        assert_eq!(check_reply(1, 0, &[]), Outcome::Refused(1));
        assert_eq!(
            check_reply(0, DIGEST_REPLY_LEN as u32, &reply(0xcd)),
            Outcome::Digest([0xcd; DIGEST_LEN])
        );
        // A tag 2 word is not the answer to a tag 1 call, and the other way.
        assert_eq!(classify(Carrier::Tag1, &word(TRACE_RX)), Arrival::Other);
        assert_eq!(classify(Carrier::Tag2, &word(TAG1_RX)), Arrival::Other);
    }

    #[test]
    fn the_a18_answer_to_tag2_is_one_without_bytes() {
        // Read in the format it was sent in, it looks like a one-byte reply
        // (too short to be a header); the tag 1 call never sees it.
        assert_eq!(
            classify(Carrier::Tag2, &word(NEO_TAG2_ANSWER)),
            Arrival::Reply { status: 0, len: 1 }
        );
        assert_eq!(check_reply(0, 1, &[]), Outcome::Malformed);
        assert_eq!(
            classify(Carrier::Tag1, &word(NEO_TAG2_ANSWER)),
            Arrival::Other
        );
    }

    #[test]
    fn the_recorded_answer_is_a_digest() {
        let arrival = classify(
            Carrier::Tag2,
            &Message {
                msg0: TRACE_RX,
                msg1: 0,
            },
        );
        assert_eq!(
            arrival,
            Arrival::Reply {
                status: 0,
                len: DIGEST_REPLY_LEN as u32
            }
        );
        assert_eq!(
            check_reply(0, DIGEST_REPLY_LEN as u32, &reply(0xab)),
            Outcome::Digest([0xab; DIGEST_LEN])
        );
    }

    #[test]
    fn refusals_and_bad_answers_are_not_digests() {
        assert_eq!(check_reply(3, 0, &[]), Outcome::Refused(3));
        let mut r = reply(1);
        r[4] = 7;
        assert_eq!(
            check_reply(0, DIGEST_REPLY_LEN as u32, &r),
            Outcome::Refused(7)
        );
        let mut r = reply(1);
        r[0] = b'x';
        assert_eq!(
            check_reply(0, DIGEST_REPLY_LEN as u32, &r),
            Outcome::Malformed
        );
        // The request header back is not a reply header.
        let mut r = reply(1);
        r[..HEADER_LEN].copy_from_slice(&TRACE_REQUEST);
        assert_eq!(
            check_reply(0, DIGEST_REPLY_LEN as u32, &r),
            Outcome::Malformed
        );
        // A refusal that comes back as a header alone.
        let mut r = reply(1);
        r[4] = 9;
        assert_eq!(
            check_reply(0, HEADER_LEN as u32, &r[..HEADER_LEN]),
            Outcome::Refused(9)
        );
        // A header alone that says success carries no digest.
        assert_eq!(
            check_reply(0, HEADER_LEN as u32, &reply(1)[..HEADER_LEN]),
            Outcome::Malformed
        );
        // The pairing-status reply length.
        assert_eq!(check_reply(0, 0x24, &reply(1)), Outcome::Malformed);
        assert_eq!(
            check_reply(0, DIGEST_REPLY_LEN as u32, &reply(1)[..40]),
            Outcome::Malformed
        );
    }

    #[test]
    fn other_words_are_not_replies() {
        // Another endpoint, another tag, a nonzero param.
        for w in [
            0x0000_0050_0000_0218u64,
            0x0000_0050_0000_0115,
            0x0000_0050_0100_0215,
        ] {
            assert_eq!(classify(Carrier::Tag2, &word(w)), Arrival::Other);
        }
    }

    #[test]
    fn the_discovery_words_name_the_endpoint_and_its_layout() {
        let name = decode(&Message {
            msg0: DISCOVER_NAME,
            msg1: 0,
        });
        assert_eq!(name.ep, EP_DISCOVER);
        assert_eq!(name.param, EP_PNON);
        assert_eq!(name.ty, DISCOVER_TYPE_DESCRIPTOR);
        assert_eq!(name.data_lo, PNON_NAME);
        let f = decode(&Message {
            msg0: DISCOVER_LAYOUT_7,
            msg1: 0,
        });
        assert_eq!(f.param, EP_PNON);
        assert_eq!(f.ty, DISCOVER_TYPE_CONFIG);
        assert_eq!(offer(f.data_lo), Some((Carrier::Tag2, 0x1c000, 0x14000)));
        assert_eq!(OOL_SIZE_PNON, 0x1c000);
        // The two-to-four-page layout of M1 Pro, M2 and A18 Pro: tag 1, four
        // pages each way.
        let f = decode(&Message {
            msg0: DISCOVER_LAYOUT_4,
            msg1: 0,
        });
        assert_eq!(f.param, EP_PNON);
        assert_eq!(offer(f.data_lo), Some((Carrier::Tag1, 0x10000, 0x10000)));
    }

    #[test]
    fn the_layout_bytes_read_as_the_sks_sizes() {
        let l = Layout::from_word(SKS_LAYOUT);
        assert_eq!(usize::from(l.send_max) * SEP_PAGE, 0x8000);
        assert_eq!(usize::from(l.receive_min) * SEP_PAGE, 0x4000);
        // An unknown layout is left alone.
        assert_eq!(offer(SKS_LAYOUT), None);
        assert_eq!(offer(0x0202_0202), None);
        assert_eq!(offer(0x0505_0707 + 1), None);
    }

    #[test]
    fn a_late_answer_does_not_complete_a_later_exchange() {
        let mut x = Exchange::new();
        let first = x.begin(Carrier::Tag1);
        x.end();
        let second = x.begin(Carrier::Tag2);
        assert_eq!(x.current(), Some((second, Carrier::Tag2)));
        assert!(!x.answer(first, Outcome::Digest([1; DIGEST_LEN])));
        assert!(x.answer(second, Outcome::NoReply));
        assert!(!x.answer(second, Outcome::Malformed));
        assert_eq!(x.take(), Some(Outcome::NoReply));
        assert_eq!(x.current(), None);
    }
}
