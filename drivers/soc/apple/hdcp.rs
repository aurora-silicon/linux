// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's HDCP endpoint (`0x0e`): the protocol, without a caller yet.
//!
//! The AP runs the HDCP 2.2 transmitter; the enclave holds its secrets. The
//! host hands the enclave the receiver's messages and gets back the ones it
//! must send, one command at a time. Recorded on a J616s under macOS 27.0
//! during display wakes (lane SEPHDCP, 11-13; three sessions, identical):
//!
//! ```text
//! TX 0x0201_0000_0700_0100   command 0x01, slot 7, argument 0x0201_0000
//! RX 0x0000_0000_0000_000e   acknowledged, no output
//! TX 0x0000_020a_0700_0800   command 0x08, slot 7, 522 input bytes out of line
//! RX 0x0000_0000_0020_000e   32 output bytes out of line
//! ```
//!
//! - Byte 1 is the command, byte 2 is zero, byte 3 is the slot: the HDCP
//!   interface channel the session runs on. The upper word is the input
//!   length for a command that takes input, otherwise its argument.
//! - Input is one flat segment at offset 0 of a persistent 16 KiB request
//!   buffer; output is exactly the reply's byte 2 bytes at offset 0 of a
//!   second persistent 16 KiB buffer. Both are registered once.
//! - A reply carries no command or tag: one request may be in flight on the
//!   endpoint. The reply's second word is stale memory and is never read.
//!
//! The command set is closed ([`SPECS`]) and a session sends it in the
//! recorded order only ([`Session`]); anything else is refused before it
//! reaches the mailbox. The DCP side that carries the receiver's messages is
//! not known yet, so nothing sends these commands.

#![allow(dead_code)]

use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_HDCP: u8 = 0x0e;

/// Each out-of-line buffer: one enclave page.
pub(crate) const OOL_SIZE_HDCP: usize = 0x4000;

/// The reply to a command that is acknowledged, or that writes its output.
const REPLY_TAG: u8 = 0x00;

/// The arguments that ride in the upper word of a command without input.
/// Recorded values; their byte meanings are not established.
pub(crate) const OPEN_ARG: u32 = (0x02 << 24) | (0x01 << 16);
pub(crate) const PREPARE_ARG: u32 = 0x02 << 16;
pub(crate) const FINISH_ARG: u32 = 0x01 << 16;
/// Command 0x21's argument: bit 16 set as encryption is turned on, clear at
/// display power-off.
pub(crate) const ENCRYPTION_ON: u32 = 1 << 16;

/// HDCP 2.2 field sizes (public specification).
pub(crate) const RTX_LEN: usize = 8;
pub(crate) const RRX_LEN: usize = 8;
pub(crate) const RN_LEN: usize = 8;
pub(crate) const RIV_LEN: usize = 8;
pub(crate) const CERT_LEN: usize = 522;
pub(crate) const CAPS_LEN: usize = 3;
pub(crate) const H_PRIME_LEN: usize = 32;
pub(crate) const L_PRIME_LEN: usize = 32;
/// Ekh(km) and m, the body of AKE_Stored_km.
pub(crate) const STORED_KM_LEN: usize = 32;
pub(crate) const EKS_LEN: usize = 16;
/// r_tx, then RxCaps, then TxCaps.
pub(crate) const H_CONTEXT_LEN: usize = RTX_LEN + 2 * CAPS_LEN;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Command {
    Open,
    Prepare,
    Start,
    GetRtx,
    PutCert,
    PutRrx,
    GetStoredKm,
    PutHContext,
    PutHPrime,
    GetRn,
    LcStart,
    PutLPrime,
    GetEks,
    GetRiv,
    Finish,
    SetEncryption,
    Close,
}

/// What a command looks like on the wire.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Spec {
    pub(crate) command: Command,
    pub(crate) id: u8,
    /// Input bytes out of line (the upper word then carries this length).
    pub(crate) input: usize,
    /// Output bytes out of line; the reply's byte 2 must say this.
    pub(crate) output: usize,
}

const fn spec(command: Command, id: u8, input: usize, output: usize) -> Spec {
    Spec {
        command,
        id,
        input,
        output,
    }
}

/// Every command the driver may send, in session order.
pub(crate) const SPECS: [Spec; 17] = [
    spec(Command::Open, 0x01, 0, 0),
    spec(Command::Prepare, 0x26, 0, 0),
    spec(Command::Start, 0x22, 0, 0),
    spec(Command::GetRtx, 0x04, 0, RTX_LEN),
    spec(Command::PutCert, 0x08, CERT_LEN, 0),
    spec(Command::PutRrx, 0x05, RRX_LEN, 0),
    spec(Command::GetStoredKm, 0x09, 0, STORED_KM_LEN),
    spec(Command::PutHContext, 0x0e, H_CONTEXT_LEN, 0),
    spec(Command::PutHPrime, 0x0f, H_PRIME_LEN, 0),
    spec(Command::GetRn, 0x18, 0, RN_LEN),
    spec(Command::LcStart, 0x1a, 0, 0),
    spec(Command::PutLPrime, 0x1c, L_PRIME_LEN, 0),
    spec(Command::GetEks, 0x10, 0, EKS_LEN),
    spec(Command::GetRiv, 0x12, 0, RIV_LEN),
    spec(Command::Finish, 0x20, 0, 0),
    spec(Command::SetEncryption, 0x21, 0, 0),
    spec(Command::Close, 0x02, 0, 0),
];

static_assert!(CERT_LEN <= OOL_SIZE_HDCP && STORED_KM_LEN <= OOL_SIZE_HDCP);

pub(crate) fn spec_of(command: Command) -> Spec {
    // SPECS is indexed by nothing else; a linear look-up over 17 entries.
    let mut i = 0;
    while i < SPECS.len() {
        if SPECS[i].command == command {
            return SPECS[i];
        }
        i += 1;
    }
    unreachable!()
}

/// The request word: command, slot and the upper word (input length or
/// argument).
pub(crate) const fn request_word(id: u8, slot: u8, upper: u32) -> Message {
    Message {
        msg0: (EP_HDCP as u64)
            | ((id as u64) << MSG_TAG_SHIFT)
            | ((slot as u64) << MSG_PARAM_SHIFT)
            | ((upper as u64) << MSG_DATA_SHIFT),
        msg1: 0,
    }
}

/// Builds the request for `command` on `slot`. `arg` is used only by commands
/// without input, `input_len` only by those with it, and must be exact.
pub(crate) fn request(command: Command, slot: u8, arg: u32, input_len: usize) -> Result<Message> {
    let s = spec_of(command);
    let upper = if s.input != 0 {
        if input_len != s.input {
            return Err(EINVAL);
        }
        s.input as u32
    } else {
        if input_len != 0 {
            return Err(EINVAL);
        }
        let allowed = match command {
            Command::Open => arg == OPEN_ARG,
            Command::Prepare => arg == PREPARE_ARG,
            Command::Finish => arg == FINISH_ARG,
            Command::SetEncryption => arg == 0 || arg == ENCRYPTION_ON,
            _ => arg == 0,
        };
        if !allowed {
            return Err(EINVAL);
        }
        arg
    };
    Ok(request_word(s.id, slot, upper))
}

/// What a reply says about the command in flight.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    /// Acknowledged, with this many output bytes (the expected count).
    Done(usize),
    /// Not the answer this command gets.
    Malformed,
    /// No answer in time: the endpoint is quarantined until the SEP resets.
    NoReply,
}

/// Judges a reply to `command` by its first word only.
pub(crate) fn check_reply(command: Command, msg: &Message) -> Outcome {
    let f = decode(msg);
    let s = spec_of(command);
    if f.ep != EP_HDCP || f.tag != REPLY_TAG || f.param != 0 || f.data_lo != 0 {
        return Outcome::Malformed;
    }
    if usize::from(f.ty) != s.output {
        return Outcome::Malformed;
    }
    Outcome::Done(s.output)
}

/// The authentication order: Open through Finish, then SetEncryption once
/// the transmitter's 200 ms wait has passed.
const AUTH_ORDER: [Command; 16] = [
    Command::Open,
    Command::Prepare,
    Command::Start,
    Command::GetRtx,
    Command::PutCert,
    Command::PutRrx,
    Command::GetStoredKm,
    Command::PutHContext,
    Command::PutHPrime,
    Command::GetRn,
    Command::LcStart,
    Command::PutLPrime,
    Command::GetEks,
    Command::GetRiv,
    Command::Finish,
    Command::SetEncryption,
];

/// One session on one slot: which command may go next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Session {
    pub(crate) slot: u8,
    next: usize,
    open: bool,
    encrypting: bool,
    quarantined: bool,
}

impl Session {
    pub(crate) const fn new(slot: u8) -> Session {
        Session {
            slot,
            next: 0,
            open: false,
            encrypting: false,
            quarantined: false,
        }
    }

    /// Whether `command` may be sent now.
    pub(crate) fn allows(&self, command: Command, arg: u32) -> bool {
        if self.quarantined {
            return false;
        }
        match command {
            // Teardown: encryption off, then close, from any open state.
            Command::SetEncryption if arg == 0 => self.open,
            Command::Close => self.open && !self.encrypting,
            _ => {
                self.next < AUTH_ORDER.len()
                    && AUTH_ORDER[self.next] == command
                    && (command != Command::SetEncryption || arg == ENCRYPTION_ON)
            }
        }
    }

    /// Records the outcome of a command [`Session::allows`] let through.
    /// Anything but the expected acknowledgement ends the session: the
    /// caller tears it down (or, after [`Outcome::NoReply`], leaves the
    /// endpoint alone until the SEP resets).
    pub(crate) fn record(&mut self, command: Command, arg: u32, outcome: Outcome) -> bool {
        match outcome {
            Outcome::Done(_) => {}
            Outcome::NoReply => {
                self.quarantined = true;
                return false;
            }
            Outcome::Malformed => {
                // Not trusted any more: no further step, and encryption no
                // longer counts as on. Teardown stays allowed.
                self.next = AUTH_ORDER.len();
                self.encrypting = false;
                return false;
            }
        }
        match command {
            Command::SetEncryption if arg == 0 => {
                self.encrypting = false;
                self.next = AUTH_ORDER.len();
            }
            Command::Close => *self = Session::new(self.slot),
            _ => {
                if command == Command::Open {
                    self.open = true;
                }
                if command == Command::SetEncryption {
                    self.encrypting = true;
                }
                self.next += 1;
            }
        }
        true
    }

    /// Encryption is on: every step acknowledged, in order.
    pub(crate) fn encrypting(&self) -> bool {
        self.encrypting && !self.quarantined
    }

    pub(crate) fn is_open(&self) -> bool {
        self.open
    }

    /// The SEP reset: the session and any quarantine are gone.
    pub(crate) fn reset(&mut self) {
        *self = Session::new(self.slot);
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_hdcp)]
mod tests {
    use super::*;

    // traces/touchid/20261113-sephdcp/mailbox-hdcp.tsv (lane SEPHDCP, J616s,
    // macOS 27.0): the request words of one display-wake session with the
    // endpoint byte added (the trace records it on the reply side only),
    // each with its reply word.
    const SLOT: u8 = 0x07;
    const TRACE: [(Command, u32, usize, u64, u64); 16] = [
        (
            Command::Open,
            OPEN_ARG,
            0,
            0x0201_0000_0700_010e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::Prepare,
            PREPARE_ARG,
            0,
            0x0002_0000_0700_260e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::Start,
            0,
            0,
            0x0000_0000_0700_220e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::GetRtx,
            0,
            0,
            0x0000_0000_0700_040e,
            0x0000_0000_0008_000e,
        ),
        (
            Command::PutCert,
            0,
            522,
            0x0000_020a_0700_080e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::PutRrx,
            0,
            8,
            0x0000_0008_0700_050e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::GetStoredKm,
            0,
            0,
            0x0000_0000_0700_090e,
            0x0000_0000_0020_000e,
        ),
        (
            Command::PutHContext,
            0,
            14,
            0x0000_000e_0700_0e0e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::PutHPrime,
            0,
            32,
            0x0000_0020_0700_0f0e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::GetRn,
            0,
            0,
            0x0000_0000_0700_180e,
            0x0000_0000_0008_000e,
        ),
        (
            Command::LcStart,
            0,
            0,
            0x0000_0000_0700_1a0e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::PutLPrime,
            0,
            32,
            0x0000_0020_0700_1c0e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::GetEks,
            0,
            0,
            0x0000_0000_0700_100e,
            0x0000_0000_0010_000e,
        ),
        (
            Command::GetRiv,
            0,
            0,
            0x0000_0000_0700_120e,
            0x0000_0000_0008_000e,
        ),
        (
            Command::Finish,
            FINISH_ARG,
            0,
            0x0001_0000_0700_200e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::SetEncryption,
            ENCRYPTION_ON,
            0,
            0x0001_0000_0700_210e,
            0x0000_0000_0000_000e,
        ),
    ];
    // Display power-off: encryption off, then close.
    const TRACE_OFF: [(Command, u64); 2] = [
        (Command::SetEncryption, 0x0000_0000_0700_210e),
        (Command::Close, 0x0000_0000_0700_020e),
    ];

    fn reply(w: u64) -> Message {
        // The second word is stale memory in the recording; it must not matter.
        Message {
            msg0: w,
            msg1: 0x0bad_f00d,
        }
    }

    #[test]
    fn the_requests_match_the_recording() {
        for &(c, arg, len, tx, _) in TRACE.iter() {
            assert_eq!(request(c, SLOT, arg, len).unwrap().msg0, tx);
        }
        assert_eq!(
            request(Command::SetEncryption, SLOT, 0, 0).unwrap().msg0,
            TRACE_OFF[0].1
        );
        assert_eq!(
            request(Command::Close, SLOT, 0, 0).unwrap().msg0,
            TRACE_OFF[1].1
        );
    }

    #[test]
    fn the_recorded_session_runs_to_encryption_and_tears_down() {
        let mut s = Session::new(SLOT);
        for &(c, arg, _, _, rx) in TRACE.iter() {
            assert!(s.allows(c, arg));
            let o = check_reply(c, &reply(rx));
            assert_eq!(o, Outcome::Done(spec_of(c).output));
            assert!(s.record(c, arg, o));
        }
        assert!(s.encrypting());
        assert!(!s.allows(Command::Close, 0));
        assert!(s.allows(Command::SetEncryption, 0));
        assert!(s.record(Command::SetEncryption, 0, Outcome::Done(0)));
        assert!(!s.encrypting());
        assert!(s.allows(Command::Close, 0));
        assert!(s.record(Command::Close, 0, Outcome::Done(0)));
        assert!(!s.is_open());
        assert!(s.allows(Command::Open, OPEN_ARG));
    }

    #[test]
    fn commands_out_of_order_or_off_the_table_are_refused() {
        let s = Session::new(SLOT);
        assert!(!s.allows(Command::GetRtx, 0));
        assert!(!s.allows(Command::SetEncryption, ENCRYPTION_ON));
        assert!(!s.allows(Command::Close, 0));
        // Exact input lengths and recorded arguments only.
        assert!(request(Command::PutCert, SLOT, 0, 521).is_err());
        assert!(request(Command::PutHContext, SLOT, 0, 0).is_err());
        assert!(request(Command::Open, SLOT, 0, 0).is_err());
        assert!(request(Command::GetRtx, SLOT, 0, 8).is_err());
        assert!(request(Command::SetEncryption, SLOT, 3, 0).is_err());
    }

    #[test]
    fn a_wrong_reply_ends_the_session_and_silence_quarantines() {
        // Output length other than the command's.
        assert_eq!(
            check_reply(Command::GetRtx, &reply(0x0000_0000_0010_000e)),
            Outcome::Malformed
        );
        assert_eq!(
            check_reply(Command::PutCert, &reply(0x0000_0000_0008_000e)),
            Outcome::Malformed
        );
        // Another endpoint, a tag, a param, a nonzero upper word.
        for w in [
            0x0000_0000_0000_000fu64,
            0x0000_0000_0000_010e,
            0x0000_0000_0100_000e,
            0x0000_0001_0000_000e,
        ] {
            assert_eq!(check_reply(Command::Start, &reply(w)), Outcome::Malformed);
        }
        let mut s = Session::new(SLOT);
        assert!(s.record(Command::Open, OPEN_ARG, Outcome::Done(0)));
        assert!(!s.record(Command::Prepare, PREPARE_ARG, Outcome::Malformed));
        assert!(!s.allows(Command::Start, 0));
        assert!(!s.encrypting());
        // Teardown is still allowed after a bad step.
        assert!(s.allows(Command::Close, 0));
        assert!(!s.record(Command::Close, 0, Outcome::NoReply));
        assert!(!s.allows(Command::Close, 0));
        assert!(!s.allows(Command::Open, OPEN_ARG));
        s.reset();
        assert!(s.allows(Command::Open, OPEN_ARG));
    }

    #[test]
    fn the_table_is_closed_and_fits_the_buffers() {
        for (i, a) in SPECS.iter().enumerate() {
            assert!(a.input <= OOL_SIZE_HDCP && a.output <= u8::MAX as usize);
            for b in SPECS[i + 1..].iter() {
                assert!(a.id != b.id);
            }
        }
    }
}
