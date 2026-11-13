// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's trusted-accessory endpoint (`stac`).
//!
//! The service behind it pairs and authorises accessories with their own
//! biometric sensor, such as a keyboard with Touch ID. Linux has no such
//! accessory stack yet, so the driver does only what the host owes the
//! service to keep its state as macOS keeps it: it tells the service that
//! the system is online once the enclave has come up, and again each time the
//! enclave comes back from a sleep. Recorded on a J616s under macOS, that is
//! one exchange right after the enclave's wake messages:
//!
//! ```text
//! TX 0x0000_0000_000f_fc18   endpoint 0x18, first packet, opcode 0x0f, sequence 0
//! RX 0x0000_0000_000f_fc18   the same word back: the opcode echoed, success
//! ```
//!
//! The request travels in the endpoint's out-of-line buffer in the generic
//! transfer framing that `sbio` uses (see `transfer.rs`): one packet whose body
//! is a single little-endian word, 1 for online. The answer is a packet with no
//! body and a zero status.
//!
//! Nothing here is reachable from userspace. No other operation of the service
//! is sent, and a message the driver did not ask for is counted as unhandled.

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_STAC: u8 = 0x18;

/// The name in the endpoint's discovery descriptor (upper half of `msg0`).
pub(crate) const STAC_NAME: u32 = u32::from_be_bytes(*b"stac");

/// The buffer layout the endpoint advertises in its discovery configuration
/// word on every SEP we have booted (M1 Pro, M2, M4 Pro, A18 Pro): two pages
/// each way. The driver talks to the endpoint only when it advertises this.
pub(crate) const STAC_LAYOUT: u32 = 0x0202_0202;

/// Each out-of-line buffer: two 16 KiB enclave pages, as the layout says.
pub(crate) const OOL_SIZE_STAC: usize = 0x8000;

/// The power-state operation.
pub(crate) const OP_POWER_STATE: u16 = 0x0f;

/// The power state the host reports after the enclave comes up or wakes.
pub(crate) const POWER_STATE_ONLINE: u32 = 1;

pub(crate) const POWER_STATE_BODY_LEN: usize = 4;
pub(crate) const POWER_STATE_REQUEST_LEN: usize = transfer::HEADER_LEN + POWER_STATE_BODY_LEN;
static_assert!(POWER_STATE_REQUEST_LEN <= OOL_SIZE_STAC);

/// How long the enclave gets to answer. It answered within 0.1 ms in the
/// recording.
pub(crate) const STAC_TIMEOUT_MS: u32 = 2000;

const fn encode_stac(opcode: u16, marker: u8, seq: u16) -> Message {
    Message {
        msg0: (EP_STAC as u64)
            | ((marker as u64) << MSG_TAG_SHIFT)
            | ((opcode as u64) << MSG_TYPE_SHIFT)
            | ((seq as u64) << 48),
        msg1: 0,
    }
}

// The request word in the recording.
static_assert!(
    encode_stac(OP_POWER_STATE, transfer::MARKER_FIRST, 0).msg0 == 0x0000_0000_000f_fc18
);

/// The message that hands the enclave the power-state packet.
pub(crate) const fn power_state_message() -> Message {
    encode_stac(OP_POWER_STATE, transfer::MARKER_FIRST, 0)
}

/// The out-of-line bytes of a power-state request: the packet header and the
/// state word.
pub(crate) fn power_state_request(state: u32) -> [u8; POWER_STATE_REQUEST_LEN] {
    let mut out = [0u8; POWER_STATE_REQUEST_LEN];
    let packet = transfer::Packet {
        version: transfer::VERSION,
        total: POWER_STATE_BODY_LEN as u32,
        offset: 0,
        flags: 0,
        err: 0,
        opcode: u32::from(OP_POWER_STATE),
        chunk: POWER_STATE_BODY_LEN as u32,
    };
    // The buffer is exactly one header and one word long.
    let _ = packet.encode(&mut out);
    out[transfer::HEADER_LEN..].copy_from_slice(&state.to_le_bytes());
    out
}

/// What a `stac` message is, before its out-of-line header is looked at.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Arrival {
    /// A data packet for `opcode`, or an error report for it.
    Reply { opcode: u16, error: bool },
    /// Anything else: an announcement, flow control, another endpoint's word.
    Other,
}

pub(crate) fn classify(msg: &Message) -> Arrival {
    let f = decode(msg);
    if f.ep != EP_STAC {
        return Arrival::Other;
    }
    let opcode = u16::from(f.ty) | (u16::from(f.param) << 8);
    match f.tag {
        transfer::MARKER_FIRST => Arrival::Reply {
            opcode,
            error: false,
        },
        transfer::MARKER_ERROR => Arrival::Reply {
            opcode,
            error: true,
        },
        _ => Arrival::Other,
    }
}

/// The result of one power-state exchange.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// The service took the state.
    Accepted,
    /// The enclave answered with this nonzero status, or an error report.
    Refused(u32),
    /// The answer was not the one asked for, or its header did not check out.
    Malformed,
    /// No answer within [`STAC_TIMEOUT_MS`].
    NoReply,
}

/// Checks the answer to a power-state request. `error` is whether it came as an
/// error report, `packet` the header the enclave wrote.
pub(crate) fn check_reply(error: bool, packet: &transfer::Packet) -> Outcome {
    if error {
        // An error report carries only the status word.
        return Outcome::Refused(packet.err);
    }
    if packet.version != transfer::VERSION || packet.opcode != u32::from(OP_POWER_STATE) {
        return Outcome::Malformed;
    }
    if packet.err != 0 {
        return Outcome::Refused(packet.err);
    }
    // The answer has no body; one that has is not this answer.
    if packet.total != 0 || packet.chunk != 0 || packet.offset != 0 {
        return Outcome::Malformed;
    }
    Outcome::Accepted
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

/// The reply header in the `stac` outbound buffer, as [`transfer::take_header`]
/// samples it.
struct StacHeader<'a>(&'a SepData);

impl transfer::HeaderSlot for StacHeader<'_> {
    fn sample(&self, header: &mut [u8; transfer::HEADER_LEN]) -> bool {
        self.0.ool_peek(&self.0.ool_stac, 0, header)
    }

    fn repoison(&self) {
        self.0
            .ool_repoison(&self.0.ool_stac, 0, transfer::HEADER_LEN);
    }

    fn pause(&self) {
        let poll = kernel::time::Delta::from_millis(i64::from(OOL_WRITE_POLL_MS));
        kernel::time::delay::fsleep(poll);
    }
}

impl SepData {
    /// Whether the enclave offers the endpoint with the buffer layout the
    /// driver knows.
    fn stac_offered(&self) -> bool {
        self.endpoint_present(EP_STAC)
            && self.stac_named.load(Relaxed)
            && self.stac_layout.load(Relaxed) == STAC_LAYOUT
    }

    /// Records the discovery messages of the endpoint.
    pub(crate) fn stac_discovered(&self, f: &Fields) {
        if f.param != EP_STAC {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.stac_named.store(f.data_lo == STAC_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.stac_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    /// Queues the power-state report: after the bring-up, and after each wake
    /// of the enclave. Runs on a freezable queue, so a suspend waits for an
    /// exchange in flight and none starts while devices are down.
    pub(crate) fn queue_stac_online(this: Arc<SepData>) {
        if this.shutting_down.load(Relaxed) {
            return;
        }
        // Already queued: that run reports the same state.
        let _ = workqueue::system_freezable().enqueue::<Arc<SepData>, STAC_WORK_ID>(this);
    }

    pub(crate) fn run_stac_online(&self) {
        if self.shutting_down.load(Relaxed) {
            return;
        }
        if !self.stac_offered() {
            if self.endpoint_present(EP_STAC) && !self.stac_absent_logged.xchg(true, Relaxed) {
                dev_info_ratelimited!(
                    self.dev,
                    "stac: the endpoint advertises layout {:#010x}, not {:#010x}; left alone\n",
                    self.stac_layout.load(Relaxed),
                    STAC_LAYOUT
                );
            }
            diag::stac(diag::Stac::Absent);
            return;
        }
        if let Err(e) = self.register_ool(&self.ool_stac) {
            dev_err_ratelimited!(
                self.dev,
                "stac: out-of-line buffer registration failed ({:?})\n",
                e
            );
            diag::stac(diag::Stac::Failed);
            return;
        }
        let outcome = self.stac_power_state(POWER_STATE_ONLINE);
        match outcome {
            Ok(Outcome::Accepted) => {
                diag::stac(diag::Stac::Online);
                dev_dbg!(self.dev, "stac: the accessory service is online\n");
            }
            Ok(outcome) => {
                diag::stac(diag::Stac::Failed);
                dev_err_ratelimited!(
                    self.dev,
                    "stac: the accessory service did not take the online state ({:?})\n",
                    outcome
                );
            }
            // Shutting down, or a sleep started: the next wake reports again.
            Err(e) => {
                diag::stac(diag::Stac::Failed);
                dev_dbg!(self.dev, "stac: online report not sent ({:?})\n", e);
            }
        }
    }

    fn stac_power_state(&self, state: u32) -> Result<Outcome> {
        self.stac_rx.lock().begin();
        let sent = self
            .ool_write(&self.ool_stac, 0, &power_state_request(state))
            .and_then(|()| {
                // The last answer's header must not be taken for this one's.
                transfer::send_for_reply(&StacHeader(self), || self.send(power_state_message()))
            });
        if let Err(e) = sent {
            self.stac_rx.lock().end();
            return Err(e);
        }

        let mut remaining = time::msecs_to_jiffies(STAC_TIMEOUT_MS);
        let mut guard = self.stac_rx.lock();
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
            // Uninterruptible, as the other reply waits: a pending signal
            // would otherwise end the wait at once, every time.
            match self.stac_wq.wait_timeout(&mut guard, remaining) {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { .. } => remaining = 0,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    pub(crate) fn on_stac(&self, msg: Message) {
        let Arrival::Reply { opcode, error } = classify(&msg) else {
            self.stac_unexpected(&msg);
            return;
        };
        // Sampled before the header is read: an exchange that times out
        // meanwhile and is followed by another must not take this answer.
        let Some(generation) = self.stac_rx.lock().current() else {
            self.stac_unexpected(&msg);
            return;
        };
        if opcode != OP_POWER_STATE {
            self.stac_unexpected(&msg);
            return;
        }
        let marker = if error {
            transfer::MARKER_ERROR
        } else {
            transfer::MARKER_FIRST
        };
        let outcome = match transfer::take_header(
            &StacHeader(self),
            marker,
            OOL_POISON_OUTBOUND,
            OOL_WRITE_POLL_ATTEMPTS,
        ) {
            Ok(packet) => check_reply(error, &packet),
            Err(_) => Outcome::Malformed,
        };
        if self.stac_rx.lock().answer(generation, outcome) {
            self.stac_wq.notify_all();
        }
    }

    fn stac_unexpected(&self, msg: &Message) {
        if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "stac: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_stac)]
mod tests {
    use super::*;

    // Mailbox words recorded on a J616s under macOS 27.0, right after the
    // enclave's wake messages (traces/macos/20261112-sepwire, run 1 and 2).
    const TRACE_TX: u64 = 0x0000_0000_000f_fc18;
    const TRACE_RX: u64 = 0x0000_0000_000f_fc18;
    // The power-state body the host handed the enclave in the same recording.
    const TRACE_BODY: [u8; 4] = [0x01, 0x00, 0x00, 0x00];
    // The endpoint's discovery messages in our Linux boot logs.
    const DISCOVER_NAME: u64 = 0x7374_6163_1800_00fd;
    const DISCOVER_LAYOUT: u64 = 0x0202_0202_1801_00fd;

    fn ok_reply() -> transfer::Packet {
        transfer::Packet {
            version: transfer::VERSION,
            total: 0,
            offset: 0,
            flags: 0,
            err: 0,
            opcode: u32::from(OP_POWER_STATE),
            chunk: 0,
        }
    }

    #[test]
    fn the_request_word_matches_the_recording() {
        let msg = power_state_message();
        assert_eq!(msg.msg0, TRACE_TX);
        assert_eq!(msg.msg1, 0);
    }

    #[test]
    fn the_request_packet_carries_the_online_word() -> Result {
        let bytes = power_state_request(POWER_STATE_ONLINE);
        assert_eq!(&bytes[transfer::HEADER_LEN..], &TRACE_BODY);
        let packet = transfer::Packet::decode(&bytes)?;
        assert_eq!(packet.version, transfer::VERSION);
        assert_eq!(packet.opcode, u32::from(OP_POWER_STATE));
        assert_eq!(packet.total, 4);
        assert_eq!(packet.chunk, 4);
        assert_eq!(packet.offset, 0);
        assert_eq!(packet.err, 0);
        Ok(())
    }

    #[test]
    fn the_recorded_answer_is_a_reply_to_the_request() {
        let arrival = classify(&Message {
            msg0: TRACE_RX,
            msg1: 0,
        });
        assert_eq!(
            arrival,
            Arrival::Reply {
                opcode: OP_POWER_STATE,
                error: false
            }
        );
        assert_eq!(check_reply(false, &ok_reply()), Outcome::Accepted);
    }

    #[test]
    fn the_discovery_words_name_the_endpoint_and_its_layout() {
        let name = decode(&Message {
            msg0: DISCOVER_NAME,
            msg1: 0,
        });
        assert_eq!(name.ep, EP_DISCOVER);
        assert_eq!(name.param, EP_STAC);
        assert_eq!(name.ty, DISCOVER_TYPE_DESCRIPTOR);
        assert_eq!(name.data_lo, STAC_NAME);
        let layout = decode(&Message {
            msg0: DISCOVER_LAYOUT,
            msg1: 0,
        });
        assert_eq!(layout.param, EP_STAC);
        assert_eq!(layout.ty, DISCOVER_TYPE_CONFIG);
        assert_eq!(layout.data_lo, STAC_LAYOUT);
    }

    #[test]
    fn other_words_are_not_replies() {
        // Flow control, a continuation and a notification-range tag.
        for tag in [transfer::MARKER_REQUEST, transfer::MARKER_NEXT, 0x01] {
            let msg = Message {
                msg0: (TRACE_RX & !0xff00) | (u64::from(tag) << MSG_TAG_SHIFT),
                msg1: 0,
            };
            assert_eq!(classify(&msg), Arrival::Other);
        }
        // The same word on another endpoint.
        let sbio = Message {
            msg0: (TRACE_RX & !0xff) | u64::from(EP_SBIO),
            msg1: 0,
        };
        assert_eq!(classify(&sbio), Arrival::Other);
    }

    #[test]
    fn a_refusal_is_reported_with_its_status() {
        let refused = transfer::Packet {
            err: 0x16,
            ..ok_reply()
        };
        assert_eq!(check_reply(false, &refused), Outcome::Refused(0x16));
        assert_eq!(check_reply(true, &refused), Outcome::Refused(0x16));
        // An error report counts as a refusal whatever its status word holds.
        assert_eq!(check_reply(true, &ok_reply()), Outcome::Refused(0));
    }

    #[test]
    fn a_wrong_or_oversized_answer_is_malformed() {
        let wrong_opcode = transfer::Packet {
            opcode: 0x2b,
            ..ok_reply()
        };
        assert_eq!(check_reply(false, &wrong_opcode), Outcome::Malformed);
        let wrong_version = transfer::Packet {
            version: transfer::VERSION + 1,
            ..ok_reply()
        };
        assert_eq!(check_reply(false, &wrong_version), Outcome::Malformed);
        let with_body = transfer::Packet {
            total: 4,
            chunk: 4,
            ..ok_reply()
        };
        assert_eq!(check_reply(false, &with_body), Outcome::Malformed);
        let continued = transfer::Packet {
            total: 8,
            ..ok_reply()
        };
        assert_eq!(check_reply(false, &continued), Outcome::Malformed);
    }

    #[test]
    fn a_late_answer_does_not_complete_the_next_exchange() {
        let mut rx = Exchange::new();
        assert_eq!(rx.current(), None);
        let first = rx.begin();
        assert_eq!(rx.current(), Some(first));
        // The first exchange times out; a second begins before its answer lands.
        rx.end();
        assert_eq!(rx.current(), None);
        let second = rx.begin();
        assert!(!rx.answer(first, Outcome::Accepted));
        assert_eq!(rx.take(), None);
        assert!(rx.answer(second, Outcome::Accepted));
        // A second answer to the same exchange is dropped.
        assert!(!rx.answer(second, Outcome::Refused(1)));
        assert_eq!(rx.take(), Some(Outcome::Accepted));
        assert_eq!(rx.current(), None);
        // Nothing is waiting now.
        assert!(!rx.answer(second, Outcome::Accepted));
    }
}
