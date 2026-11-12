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
//! The mailbox word carries no operation: `data` is the request length, and
//! in the reply `type` is the status and `data` the reply length. Nothing
//! here is reachable from userspace except the digest itself, read-only for
//! root in `diag/local_policy_digest`. No operation that changes policy or
//! nonce state is sent, and a message the driver did not ask for is counted
//! as unhandled.

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

/// The layouts the endpoint advertises on every SEP we have booted: four
/// pages at most each way on M1 Pro, M2 and A18 Pro, seven on M4 Pro (whose
/// SEP asks for five at least). The driver talks to the endpoint only when
/// it advertises one of these.
pub(crate) const LAYOUT_4: Layout = Layout {
    send_min: 2,
    receive_min: 2,
    send_max: 4,
    receive_max: 4,
};
pub(crate) const LAYOUT_7: Layout = Layout {
    send_min: 5,
    receive_min: 5,
    send_max: 7,
    receive_max: 7,
};

/// The buffer sizes registered for a known layout: the largest send buffer
/// and the smallest receive buffer the layout allows, which is what macOS
/// registers on the M4 Pro (seven pages out, five back).
pub(crate) fn ool_sizes(word: u32) -> Option<(usize, usize)> {
    let layout = Layout::from_word(word);
    if layout != LAYOUT_4 && layout != LAYOUT_7 {
        return None;
    }
    Some((
        usize::from(layout.send_max) * SEP_PAGE,
        usize::from(layout.receive_min) * SEP_PAGE,
    ))
}

/// What the driver allocates each way: the largest size [`ool_sizes`]
/// can return.
pub(crate) const OOL_SIZE_PNON: usize = 7 * SEP_PAGE;

/// The call tag the host uses; the reply carries it back.
pub(crate) const CALL_TAG: u8 = 0x02;

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
pub(crate) const fn request_message(len: u32) -> Message {
    Message {
        msg0: (EP_PNON as u64)
            | ((CALL_TAG as u64) << MSG_TAG_SHIFT)
            | ((len as u64) << MSG_DATA_SHIFT),
        msg1: 0,
    }
}

// The request word in the recording.
static_assert!(request_message(HEADER_LEN as u32).msg0 == 0x0000_0020_0000_0215);

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
    Reply { status: u8, len: u32 },
    /// Anything else.
    Other,
}

pub(crate) fn classify(msg: &Message) -> Arrival {
    let f = decode(msg);
    if f.ep != EP_PNON || f.tag != CALL_TAG || f.param != 0 {
        return Arrival::Other;
    }
    Arrival::Reply {
        status: f.ty,
        len: f.data_lo,
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
pub(crate) fn check_reply(status: u8, len: u32, bytes: &[u8]) -> Outcome {
    if status != 0 {
        return Outcome::Refused(u32::from(status));
    }
    if len as usize != DIGEST_REPLY_LEN || bytes.len() < DIGEST_REPLY_LEN {
        return Outcome::Malformed;
    }
    let word =
        |at: usize| u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]]);
    if word(0) != REPLY_SIGNATURE {
        return Outcome::Malformed;
    }
    let result = word(4);
    if result != 0 {
        return Outcome::Refused(result);
    }
    let mut digest = [0u8; DIGEST_LEN];
    digest.copy_from_slice(&bytes[HEADER_LEN..DIGEST_REPLY_LEN]);
    Outcome::Digest(digest)
}

/// The single exchange the endpoint can have in flight.
pub(crate) struct Exchange {
    waiting: bool,
    generation: u64,
    outcome: Option<Outcome>,
}

impl Exchange {
    pub(crate) fn new() -> Exchange {
        Exchange {
            waiting: false,
            generation: 0,
            outcome: None,
        }
    }

    /// Starts an exchange; returns its number.
    pub(crate) fn begin(&mut self) -> u64 {
        self.generation = self.generation.wrapping_add(1);
        self.waiting = true;
        self.outcome = None;
        self.generation
    }

    /// The exchange a message arriving now can answer, if one is waiting.
    pub(crate) fn current(&self) -> Option<u64> {
        if self.waiting {
            Some(self.generation)
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
    /// The buffer sizes for the endpoint, if the enclave offers it as the
    /// driver knows it.
    fn pnon_offered(&self) -> Option<(usize, usize)> {
        if !self.endpoint_present(EP_PNON) || !self.pnon_named.load(Relaxed) {
            return None;
        }
        ool_sizes(self.pnon_layout.load(Relaxed))
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
        let Some(sizes) = self.pnon_offered() else {
            if self.endpoint_present(EP_PNON) {
                dev_info_ratelimited!(
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
            .size_ool(&self.ool_pnon, sizes)
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
        match self.pnon_read_digest() {
            Ok(Outcome::Digest(digest)) => {
                diag::pnon(diag::Pnon::Read, Some(&digest));
                dev_dbg!(self.dev, "pnon: read the LocalPolicy nonce digest\n");
            }
            Ok(outcome) => {
                diag::pnon(diag::Pnon::Failed, None);
                dev_err_ratelimited!(
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

    fn pnon_read_digest(&self) -> Result<Outcome> {
        self.pnon_rx.lock().begin();
        // The last answer must not be taken for this one's.
        self.ool_repoison(&self.ool_pnon, 0, DIGEST_REPLY_LEN);
        let sent = self
            .ool_write(&self.ool_pnon, 0, &digest_request())
            .and_then(|()| self.send(request_message(HEADER_LEN as u32)));
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
        let Arrival::Reply { status, len } = classify(&msg) else {
            self.pnon_unexpected(&msg);
            return;
        };
        // Sampled before the buffer is read: an exchange that times out
        // meanwhile and is followed by another must not take this answer.
        let Some(generation) = self.pnon_rx.lock().current() else {
            self.pnon_unexpected(&msg);
            return;
        };
        let outcome = if status != 0 {
            check_reply(status, len, &[])
        } else if len as usize != DIGEST_REPLY_LEN
            || !self.ool_await_written(&self.ool_pnon, 0, DIGEST_REPLY_LEN)
        {
            Outcome::Malformed
        } else {
            match self.ool_read(&self.ool_pnon, 0, DIGEST_REPLY_LEN) {
                Ok(bytes) => check_reply(status, len, &bytes),
                Err(_) => Outcome::Malformed,
            }
        };
        self.ool_repoison(&self.ool_pnon, 0, DIGEST_REPLY_LEN);
        if self.pnon_rx.lock().answer(generation, outcome) {
            self.pnon_wq.notify_all();
        }
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

    fn reply(digest_byte: u8) -> [u8; DIGEST_REPLY_LEN] {
        let mut r = [0u8; DIGEST_REPLY_LEN];
        r[..8].copy_from_slice(&TRACE_REPLY_HEADER);
        r[HEADER_LEN..].fill(digest_byte);
        r
    }

    #[test]
    fn the_request_matches_the_recording() {
        let msg = request_message(HEADER_LEN as u32);
        assert_eq!(msg.msg0, TRACE_TX);
        assert_eq!(msg.msg1, 0);
        assert_eq!(digest_request(), TRACE_REQUEST);
    }

    #[test]
    fn the_recorded_answer_is_a_digest() {
        let arrival = classify(&Message {
            msg0: TRACE_RX,
            msg1: 0,
        });
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
            assert_eq!(classify(&Message { msg0: w, msg1: 0 }), Arrival::Other);
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
        for (w, sizes) in [
            (DISCOVER_LAYOUT_4, (0x10000, 0x8000)),
            (DISCOVER_LAYOUT_7, (0x1c000, 0x14000)),
        ] {
            let f = decode(&Message { msg0: w, msg1: 0 });
            assert_eq!(f.param, EP_PNON);
            assert_eq!(f.ty, DISCOVER_TYPE_CONFIG);
            assert_eq!(ool_sizes(f.data_lo), Some(sizes));
            assert!(sizes.0 <= OOL_SIZE_PNON && sizes.1 <= OOL_SIZE_PNON);
        }
    }

    #[test]
    fn the_layout_bytes_read_as_the_sks_sizes() {
        let l = Layout::from_word(SKS_LAYOUT);
        assert_eq!(usize::from(l.send_max) * SEP_PAGE, 0x8000);
        assert_eq!(usize::from(l.receive_min) * SEP_PAGE, 0x4000);
        // An unknown layout is left alone.
        assert_eq!(ool_sizes(SKS_LAYOUT), None);
        assert_eq!(ool_sizes(0x0202_0202), None);
    }

    #[test]
    fn a_late_answer_does_not_complete_a_later_exchange() {
        let mut x = Exchange::new();
        let first = x.begin();
        x.end();
        let second = x.begin();
        assert!(!x.answer(first, Outcome::Digest([1; DIGEST_LEN])));
        assert!(x.answer(second, Outcome::NoReply));
        assert!(!x.answer(second, Outcome::Malformed));
        assert_eq!(x.take(), Some(Outcome::NoReply));
        assert_eq!(x.current(), None);
    }
}
