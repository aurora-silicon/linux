// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's secure-element sharing endpoint (`sse`).
//!
//! The service behind it keeps the state the Secure Enclave shares with the
//! Secure Element: the two pair once with long-term identities, after which
//! the Secure Element takes payment authorisations only from the enclave. It
//! is not the NFC controller's command channel. Linux has no Secure Element
//! stack, so the driver sends one request, the protocol query, once after the
//! bring-up. It changes nothing and tells whether the service answers.
//! Recorded on a J616s under macOS 27 (the NFC daemon's first request):
//!
//! ```text
//! TX 0x0000_0000_0008_010c   endpoint 0x0c, call tag 1, 8 request bytes out of line
//!    out of line: "cESS" (service signature), operation 1
//! RX 0x0000_0000_0004_010c   call tag 1, result 0, 4 reply bytes out of line
//!    out of line: the protocol word
//! ```
//!
//! In both directions `type` and `param` are the low and high bytes of the
//! out-of-line length and `data` is zero in a request and the service result
//! in a reply. The other calls the NFC daemon makes (the sharing state of a
//! given Secure Element, installing signed platform data, the key sharing
//! itself) need a Secure Element and are not sent. The enclave may announce
//! events on its own (tag 2); they are counted and otherwise ignored, as no
//! Linux client consumes them. Nothing here is reachable from userspace except
//! the protocol word in `diag/sse_protocol`.

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_SSE: u8 = 0x0c;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`):
/// three letters and a space.
pub(crate) const SSE_NAME: u32 = u32::from_be_bytes(*b"sse ");

/// The buffer layout the endpoint advertises in its discovery configuration
/// word on every SEP we have booted (M1 Pro, M2, M4 Pro, A18 Pro): one page
/// each way. The driver talks to the endpoint only when it advertises this.
pub(crate) const SSE_LAYOUT: u32 = 0x0101_0101;

/// Each out-of-line buffer: one 16 KiB enclave page, as the layout says.
pub(crate) const OOL_SIZE_SSE: usize = 0x4000;

/// Mailbox tags: a call and its reply, and an event the enclave announces.
pub(crate) const TAG_CALL: u8 = 0x01;
pub(crate) const TAG_EVENT: u8 = 0x02;

/// The service signature every request starts with, as it travels: the
/// ASCII bytes "cESS" read as a little-endian word.
pub(crate) const SERVICE_SIGNATURE: u32 = u32::from_le_bytes(*b"cESS");

/// The operation that returns the service's protocol word.
pub(crate) const OP_PROTOCOL: u32 = 0x01;

/// A request header: the signature, then the operation.
pub(crate) const HEADER_LEN: usize = 8;
pub(crate) const PROTOCOL_REPLY_LEN: usize = 4;
static_assert!(HEADER_LEN <= OOL_SIZE_SSE && PROTOCOL_REPLY_LEN <= OOL_SIZE_SSE);

/// How long the enclave gets to answer. It answered within 3 ms in the
/// recording.
pub(crate) const SSE_TIMEOUT_MS: u32 = 2000;

/// The request word for a call with `len` out-of-line bytes.
pub(crate) const fn request_message(len: u16) -> Message {
    Message {
        msg0: (EP_SSE as u64)
            | ((TAG_CALL as u64) << MSG_TAG_SHIFT)
            | (((len & 0xff) as u64) << MSG_TYPE_SHIFT)
            | (((len >> 8) as u64) << MSG_PARAM_SHIFT),
        msg1: 0,
    }
}

// The request word of the protocol query, and that of the 32-byte sharing
// state query in the same recording.
static_assert!(request_message(HEADER_LEN as u16).msg0 == 0x0000_0000_0008_010c);
static_assert!(request_message(0x20).msg0 == 0x0000_0000_0020_010c);

/// The out-of-line bytes of the protocol query: the header alone.
pub(crate) fn protocol_request() -> [u8; HEADER_LEN] {
    let mut out = [0u8; HEADER_LEN];
    out[0..4].copy_from_slice(&SERVICE_SIGNATURE.to_le_bytes());
    out[4..8].copy_from_slice(&OP_PROTOCOL.to_le_bytes());
    out
}

/// What an `sse` message is, before its out-of-line bytes are looked at.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Arrival {
    /// The answer to a call: the service result and how many bytes it wrote.
    Reply { result: u32, len: u16 },
    /// An event the enclave announces on its own, with its word.
    Event(u32),
    /// Anything else.
    Other,
}

pub(crate) fn classify(msg: &Message) -> Arrival {
    let f = decode(msg);
    if f.ep != EP_SSE {
        return Arrival::Other;
    }
    match f.tag {
        TAG_CALL => Arrival::Reply {
            result: f.data_lo,
            len: u16::from(f.ty) | (u16::from(f.param) << 8),
        },
        TAG_EVENT => Arrival::Event(f.data_lo),
        _ => Arrival::Other,
    }
}

/// The result of one protocol query.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// The service answered with this protocol word.
    Protocol(u32),
    /// The enclave answered with this nonzero service result.
    Refused(u32),
    /// The answer was not the one asked for.
    Malformed,
    /// No answer within [`SSE_TIMEOUT_MS`].
    NoReply,
}

/// Checks a protocol reply: `result` and `len` from the mailbox word, `bytes`
/// the reply bytes from the receive buffer.
pub(crate) fn check_reply(result: u32, len: u16, bytes: &[u8]) -> Outcome {
    if result != 0 {
        return Outcome::Refused(result);
    }
    if usize::from(len) != PROTOCOL_REPLY_LEN || bytes.len() != PROTOCOL_REPLY_LEN {
        return Outcome::Malformed;
    }
    Outcome::Protocol(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
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
    /// Whether the enclave offers the endpoint as the driver knows it.
    fn sse_offered(&self) -> bool {
        self.endpoint_present(EP_SSE)
            && self.sse_named.load(Relaxed)
            && self.sse_layout.load(Relaxed) == SSE_LAYOUT
    }

    /// Records the discovery messages of the endpoint.
    pub(crate) fn sse_discovered(&self, f: &Fields) {
        if f.param != EP_SSE {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.sse_named.store(f.data_lo == SSE_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.sse_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    /// Queues the protocol query, once after the bring-up. Runs on a
    /// freezable queue, so a suspend waits for an exchange in flight.
    pub(crate) fn queue_sse_protocol(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        let _ = workqueue::system_freezable().enqueue::<Arc<SepData>, SSE_WORK_ID>(this);
    }

    pub(crate) fn run_sse_protocol(&self) {
        if self.shutting_down.load(Relaxed) || self.sse_done.load(Relaxed) {
            return;
        }
        self.sse_done.store(true, Relaxed);
        if !self.sse_offered() {
            if self.endpoint_present(EP_SSE) {
                dev_dbg!(
                    self.dev,
                    "sse: the endpoint advertises layout {:#010x}; left alone\n",
                    self.sse_layout.load(Relaxed)
                );
            }
            diag::sse(diag::Sse::Absent, 0);
            return;
        }
        // As macOS does, the buffers are registered on the first call.
        if let Err(e) = self.register_ool(&self.ool_sse) {
            dev_err_ratelimited!(
                self.dev,
                "sse: out-of-line buffer registration failed ({:?})\n",
                e
            );
            diag::sse(diag::Sse::Failed, 0);
            return;
        }
        match self.sse_query_protocol() {
            Ok(Outcome::Protocol(word)) => {
                diag::sse(diag::Sse::Ready, word);
                dev_dbg!(self.dev, "sse: protocol {:#010x}\n", word);
            }
            Ok(outcome) => {
                diag::sse(diag::Sse::Failed, 0);
                dev_warn_ratelimited!(
                    self.dev,
                    "sse: the secure-element sharing service did not answer the protocol query ({:?})\n",
                    outcome
                );
            }
            Err(e) => {
                diag::sse(diag::Sse::Failed, 0);
                dev_dbg!(self.dev, "sse: protocol query not sent ({:?})\n", e);
            }
        }
    }

    fn sse_query_protocol(&self) -> Result<Outcome> {
        self.sse_rx.lock().begin();
        // The last answer must not be taken for this one's.
        self.ool_repoison(&self.ool_sse, 0, PROTOCOL_REPLY_LEN);
        let sent = self
            .ool_write(&self.ool_sse, 0, &protocol_request())
            .and_then(|()| self.send(request_message(HEADER_LEN as u16)));
        if let Err(e) = sent {
            self.sse_rx.lock().end();
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(SSE_TIMEOUT_MS);
        let mut guard = self.sse_rx.lock();
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
            match self.sse_wq.wait_timeout(&mut guard, remaining) {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { .. } => remaining = 0,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    pub(crate) fn on_sse(&self, msg: Message) {
        let (result, len) = match classify(&msg) {
            Arrival::Reply { result, len } => (result, len),
            Arrival::Event(word) => {
                // No Linux client consumes the service's events.
                if diag::sse_event() <= UNHANDLED_LOG_LIMIT {
                    dev_dbg!(self.dev, "sse: event {:#010x}\n", word);
                }
                return;
            }
            Arrival::Other => {
                self.sse_unexpected(&msg);
                return;
            }
        };
        // Sampled before the buffer is read: an exchange that times out
        // meanwhile and is followed by another must not take this answer.
        let Some(generation) = self.sse_rx.lock().current() else {
            self.sse_unexpected(&msg);
            return;
        };
        let want = usize::from(len);
        let outcome = if result != 0 {
            check_reply(result, len, &[])
        } else if want != PROTOCOL_REPLY_LEN || !self.ool_await_written(&self.ool_sse, 0, want) {
            Outcome::Malformed
        } else {
            match self.ool_read(&self.ool_sse, 0, want) {
                Ok(bytes) => check_reply(result, len, &bytes),
                Err(_) => Outcome::Malformed,
            }
        };
        self.ool_repoison(&self.ool_sse, 0, PROTOCOL_REPLY_LEN);
        if self.sse_rx.lock().answer(generation, outcome) {
            self.sse_wq.notify_all();
        }
    }

    fn sse_unexpected(&self, msg: &Message) {
        if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "sse: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_sse)]
mod tests {
    use super::*;

    // Recorded on a J616s under macOS 27.0 (lane SEPWIRE, 11-12): the protocol
    // query the NFC daemon sent first, the reply word and the reply bytes.
    const TRACE_REQUEST: [u8; HEADER_LEN] = [0x63, 0x45, 0x53, 0x53, 0x01, 0x00, 0x00, 0x00];
    const TRACE_RX: u64 = 0x0000_0000_0004_010c;
    const TRACE_REPLY: [u8; PROTOCOL_REPLY_LEN] = [0x00, 0x00, 0x0a, 0x00];
    // The endpoint's discovery messages in our Linux boot logs (all four Macs).
    const DISCOVER_NAME: u64 = 0x7373_6520_0c00_00fd;
    const DISCOVER_LAYOUT: u64 = 0x0101_0101_0c01_00fd;

    #[test]
    fn the_request_matches_the_recording() {
        let msg = request_message(HEADER_LEN as u16);
        assert_eq!(msg.msg0, 0x0000_0000_0008_010c);
        assert_eq!(msg.msg1, 0);
        assert_eq!(protocol_request(), TRACE_REQUEST);
    }

    #[test]
    fn the_recorded_answer_is_the_protocol_word() {
        let arrival = classify(&Message {
            msg0: TRACE_RX,
            msg1: 0,
        });
        assert_eq!(arrival, Arrival::Reply { result: 0, len: 4 });
        assert_eq!(
            check_reply(0, 4, &TRACE_REPLY),
            Outcome::Protocol(0x000a_0000)
        );
    }

    #[test]
    fn the_length_spans_type_and_param() {
        // The 11293-byte request of the same recording.
        let msg = request_message(11293);
        assert_eq!(msg.msg0, 0x0000_0000_2c1d_010c);
        assert_eq!(
            classify(&msg),
            Arrival::Reply {
                result: 0,
                len: 11293
            }
        );
    }

    #[test]
    fn refusals_and_bad_answers_are_not_protocol_words() {
        assert_eq!(check_reply(5, 4, &TRACE_REPLY), Outcome::Refused(5));
        assert_eq!(check_reply(0, 0, &[]), Outcome::Malformed);
        assert_eq!(check_reply(0, 8, &TRACE_REPLY), Outcome::Malformed);
        assert_eq!(check_reply(0, 4, &TRACE_REPLY[..3]), Outcome::Malformed);
        // The 144-byte key-sharing reply length.
        assert_eq!(check_reply(0, 144, &TRACE_REPLY), Outcome::Malformed);
    }

    #[test]
    fn events_and_other_words_are_not_replies() {
        assert_eq!(
            classify(&Message {
                msg0: 0x0000_0007_0000_020c,
                msg1: 0
            }),
            Arrival::Event(7)
        );
        // Another endpoint, a probe, a continuation.
        for w in [
            0x0000_0000_0004_0118u64,
            0x0000_0000_0000_030c,
            0x0000_0000_0004_040c,
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
        assert_eq!(name.param, EP_SSE);
        assert_eq!(name.ty, DISCOVER_TYPE_DESCRIPTOR);
        assert_eq!(name.data_lo, SSE_NAME);
        let f = decode(&Message {
            msg0: DISCOVER_LAYOUT,
            msg1: 0,
        });
        assert_eq!(f.param, EP_SSE);
        assert_eq!(f.ty, DISCOVER_TYPE_CONFIG);
        assert_eq!(f.data_lo, SSE_LAYOUT);
    }

    #[test]
    fn a_late_answer_does_not_complete_a_later_exchange() {
        let mut x = Exchange::new();
        let first = x.begin();
        x.end();
        let second = x.begin();
        assert!(!x.answer(first, Outcome::Protocol(1)));
        assert!(x.answer(second, Outcome::NoReply));
        assert!(!x.answer(second, Outcome::Malformed));
        assert_eq!(x.take(), Some(Outcome::NoReply));
        assert_eq!(x.current(), None);
    }
}
