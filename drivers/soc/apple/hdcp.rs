// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The enclave's HDCP endpoint (`0x0e`): the HDCP 2.2 transmitter service.
//!
//! The AP runs the HDCP 2.2 transmitter protocol; the enclave holds the
//! transmitter's secrets and does every cryptographic step. The host gives
//! the enclave the fields the receiver sent and gets back the fields it must
//! send, one command at a time. Only fields that the protocol sends in the
//! clear cross the endpoint; km, ks and the device keys never do.
//!
//! ```text
//! TX 0x0000_0000_0000_270e   query interface count
//! RX 0x0000_0009_0000_000e   status 0, 9 interfaces
//! TX 0x0201_0000_0700_010e   command 0x01, slot 7, argument 0x0201_0000
//! RX 0x0000_0000_0000_000e   status 0, no output
//! TX 0x0000_020a_0700_080e   command 0x08, slot 7, 522 input bytes out of line
//! RX 0x0000_0000_0020_000e   status 0, 32 output bytes out of line
//! ```
//!
//! - Request: byte 1 is the command, byte 3 the slot (the HDCP interface
//!   channel the session runs on). The upper word is the input length for a
//!   command that takes input, otherwise its argument.
//! - Reply: byte 1 is the status, byte 2 the output length (except for
//!   interface-count query `0x27`, whose upper word carries the count). A
//!   reply carries no command or tag, so one request may be in flight on the
//!   endpoint. Its second word is stale memory and is never read.
//! - Input is one flat segment at offset 0 of a persistent 16 KiB buffer;
//!   output is at offset 0 of a second one. Both are registered once.
//! - Every input is a protocol field, or fields concatenated in the order the
//!   HDCP 2.2 specification hashes them (H', V', M').
//!
//! The command set is closed ([`SPECS`]) and a session sends it in protocol
//! order only ([`Session`]); anything else is refused before it reaches the
//! mailbox. The display driver reaches this through `hdcp_shim.c`.

use super::*;
use crate::proto::*;
use kernel::prelude::*;
use kernel::soc::apple::mailbox::Message;

pub(crate) const EP_HDCP: u8 = 0x0e;

/// The endpoint's discovery name.
pub(crate) const HDCP_NAME: u32 = u32::from_be_bytes(*b"hdcp");

/// One enclave page, and each out-of-line buffer size.
pub(crate) const SEP_PAGE: usize = 0x4000;
pub(crate) const OOL_SIZE_HDCP: usize = SEP_PAGE;

/// Maximum HDCP interface channels tracked per SEP; the actual count is
/// queried at runtime with command `0x27` (for example 9 on T8112, 15 on
/// T6040).
pub(crate) const MAX_CHANNELS: usize = 32;

/// How long one command may take. The slowest, the RSA encryption of km,
/// takes about 90 ms.
pub(crate) const HDCP_TIMEOUT_MS: u32 = 1000;

/// Locality check attempts per session (HDCP 2.2: at most 1024).
pub(crate) const LC_MAX_TRIES: u16 = 1024;

/// Repeater stream-manage attempts per session (HDCP 2.2: initial + 1 retry).
pub(crate) const STREAM_MAX_TRIES: u8 = 2;

/// Reply status of the stored-km query when no km is stored for the receiver.
pub(crate) const STATUS_NO_STORED_KM: u8 = 0x08;

/// The arguments that ride in the upper word of a command without input.
/// Observed values; their byte meanings are not established.
pub(crate) const OPEN_ARG: u32 = (0x02 << 24) | (0x01 << 16);
pub(crate) const PREPARE_ARG: u32 = 0x02 << 16;
pub(crate) const FINISH_ARG: u32 = 0x01 << 16;
/// Command 0x21's argument: bit 16 set as encryption is turned on, clear as
/// it is turned off.
pub(crate) const ENCRYPTION_ON: u32 = 1 << 16;

/// Per-channel descriptor size returned by command `0x28`.
pub(crate) const CHANNEL_INFO_LEN: usize = 8;

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
pub(crate) const EKPUB_KM_LEN: usize = 128;
pub(crate) const EKH_KM_LEN: usize = 16;
pub(crate) const EKS_LEN: usize = 16;
pub(crate) const RECEIVER_ID_LEN: usize = 5;
pub(crate) const MAX_DEVICES: usize = 31;
pub(crate) const RXINFO_LEN: usize = 2;
pub(crate) const SEQ_NUM_LEN: usize = 3;
pub(crate) const V_PRIME_LEN: usize = 16;
pub(crate) const V_LEN: usize = 32;
pub(crate) const M_PRIME_LEN: usize = 32;
/// r_tx, then RxCaps, then TxCaps.
pub(crate) const H_CONTEXT_LEN: usize = RTX_LEN + 2 * CAPS_LEN;
/// Receiver ID list, RxInfo, seq_num_V.
pub(crate) const LIST_MIN: usize = RECEIVER_ID_LEN + RXINFO_LEN + SEQ_NUM_LEN;
pub(crate) const LIST_MAX: usize = MAX_DEVICES * RECEIVER_ID_LEN + RXINFO_LEN + SEQ_NUM_LEN;
/// StreamID, Type, seq_num_M.
pub(crate) const STREAM_LEN: usize = 2 + SEQ_NUM_LEN;

/// Validates the endpoint's discovery buffer-layout word (min/max pages each
/// way, MSB to LSB: send_min, recv_min, send_max, recv_max). One page each
/// way must be permitted by the advertised range.
pub(crate) fn ool_sizes(word: u32) -> Option<(usize, usize)> {
    let send_min = (word >> 24) as u8;
    let recv_min = (word >> 16) as u8;
    let send_max = (word >> 8) as u8;
    let recv_max = word as u8;
    if send_min == 0 || recv_min == 0 || send_min > 1 || recv_min > 1 {
        return None;
    }
    if send_max < 1 || recv_max < 1 || send_min > send_max || recv_min > recv_max {
        return None;
    }
    Some((OOL_SIZE_HDCP, OOL_SIZE_HDCP))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Command {
    QueryChannels,
    GetChannelInfo,
    Open,
    Prepare,
    Start,
    GetRtx,
    PutCert,
    PutRrx,
    QueryStoredKm,
    GetEkpubKm,
    PutHContext,
    PutHPrime,
    PutPairing,
    GetRn,
    LcStart,
    PutLPrime,
    GetEks,
    GetRiv,
    PutReceiverList,
    PutVPrime,
    GetV,
    PutStream,
    PutMPrime,
    Finish,
    SetEncryption,
    Close,
}

/// What a command looks like on the wire.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Spec {
    pub(crate) command: Command,
    pub(crate) id: u8,
    /// Input bytes out of line (the upper word then carries the length).
    /// A command with a list takes any length from `input` to `input_max`
    /// in whole entries; for the others the two are equal.
    pub(crate) input: usize,
    pub(crate) input_max: usize,
    /// Output bytes out of line; a successful reply's byte 2 must say this.
    pub(crate) output: usize,
    /// The enclave checks something the receiver sent: a refusal means the
    /// receiver failed authentication, not that the enclave failed.
    pub(crate) verifies: bool,
}

const fn spec(command: Command, id: u8, input: usize, output: usize) -> Spec {
    Spec {
        command,
        id,
        input,
        input_max: input,
        output,
        verifies: false,
    }
}

const fn verify(command: Command, id: u8, input: usize) -> Spec {
    Spec {
        command,
        id,
        input,
        input_max: input,
        output: 0,
        verifies: true,
    }
}

/// Every command the driver may send.
pub(crate) const SPECS: [Spec; 26] = [
    spec(Command::QueryChannels, 0x27, 0, 0),
    spec(Command::GetChannelInfo, 0x28, 0, CHANNEL_INFO_LEN),
    spec(Command::Open, 0x01, 0, 0),
    spec(Command::Prepare, 0x26, 0, 0),
    spec(Command::Start, 0x22, 0, 0),
    spec(Command::GetRtx, 0x04, 0, RTX_LEN),
    verify(Command::PutCert, 0x08, CERT_LEN),
    spec(Command::PutRrx, 0x05, RRX_LEN, 0),
    spec(Command::QueryStoredKm, 0x09, 0, STORED_KM_LEN),
    spec(Command::GetEkpubKm, 0x0c, 0, EKPUB_KM_LEN),
    spec(Command::PutHContext, 0x0e, H_CONTEXT_LEN, 0),
    verify(Command::PutHPrime, 0x0f, H_PRIME_LEN),
    spec(Command::PutPairing, 0x16, EKH_KM_LEN, 0),
    spec(Command::GetRn, 0x18, 0, RN_LEN),
    spec(Command::LcStart, 0x1a, 0, 0),
    verify(Command::PutLPrime, 0x1c, L_PRIME_LEN),
    spec(Command::GetEks, 0x10, 0, EKS_LEN),
    spec(Command::GetRiv, 0x12, 0, RIV_LEN),
    Spec {
        command: Command::PutReceiverList,
        id: 0x1d,
        input: LIST_MIN,
        input_max: LIST_MAX,
        output: 0,
        verifies: true,
    },
    verify(Command::PutVPrime, 0x1f, V_PRIME_LEN),
    spec(Command::GetV, 0x1e, 0, V_LEN),
    spec(Command::PutStream, 0x23, STREAM_LEN, 0),
    verify(Command::PutMPrime, 0x25, M_PRIME_LEN),
    spec(Command::Finish, 0x20, 0, 0),
    spec(Command::SetEncryption, 0x21, 0, 0),
    spec(Command::Close, 0x02, 0, 0),
];

static_assert!(LIST_MAX <= OOL_SIZE_HDCP && CERT_LEN <= OOL_SIZE_HDCP);
static_assert!(EKPUB_KM_LEN <= u8::MAX as usize);
static_assert!(MAX_CHANNELS <= u8::MAX as usize);

pub(crate) fn spec_of(command: Command) -> Spec {
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

fn input_len_ok(s: &Spec, len: usize) -> bool {
    if s.input == s.input_max {
        return len == s.input;
    }
    // A list: whole Receiver IDs ahead of the fixed fields.
    len >= s.input && len <= s.input_max && (len - s.input) % RECEIVER_ID_LEN == 0
}

/// Builds the request for `command` on `slot`. `arg` is used only by commands
/// without input, `input_len` only by those with it, and must fit the spec.
pub(crate) fn request(command: Command, slot: u8, arg: u32, input_len: usize) -> Result<Message> {
    let s = spec_of(command);
    if usize::from(slot) >= MAX_CHANNELS {
        return Err(EINVAL);
    }
    if command == Command::QueryChannels && slot != 0 {
        return Err(EINVAL);
    }
    let upper = if s.input != 0 {
        if arg != 0 || !input_len_ok(&s, input_len) {
            return Err(EINVAL);
        }
        input_len as u32
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
    /// Status 0, with this many output bytes (the expected count).
    Done(usize),
    /// Status 0 for `QueryChannels`: the enclave offers this many channels.
    ChannelCount(u8),
    /// The stored-km query found no km for this receiver: pair first.
    NotStored,
    /// The enclave refused the command with this status.
    Refused(u8),
    /// Not a reply this command gets.
    Malformed,
    /// No answer in time: the endpoint is quarantined until the driver
    /// rebinds, since a late reply would be taken for the next command's.
    NoReply,
}

/// Judges a reply to `command` by its first word only.
pub(crate) fn check_reply(command: Command, msg: &Message) -> Outcome {
    let f = decode(msg);
    let s = spec_of(command);
    if f.ep != EP_HDCP || f.param != 0 {
        return Outcome::Malformed;
    }
    if command == Command::QueryChannels {
        return match f.tag {
            0 if f.ty == 0 && f.data_lo >= 1 && (f.data_lo as usize) <= MAX_CHANNELS => {
                Outcome::ChannelCount(f.data_lo as u8)
            }
            0 => Outcome::Malformed,
            status => Outcome::Refused(status),
        };
    }
    if f.data_lo != 0 {
        return Outcome::Malformed;
    }
    match f.tag {
        0 if usize::from(f.ty) == s.output => Outcome::Done(s.output),
        0 => Outcome::Malformed,
        STATUS_NO_STORED_KM if command == Command::QueryStoredKm => Outcome::NotStored,
        status => Outcome::Refused(status),
    }
}

/// The errno a failed command reports to the display driver.
pub(crate) fn errno(command: Command, outcome: Outcome) -> Option<Error> {
    match outcome {
        Outcome::Done(_) | Outcome::ChannelCount(_) | Outcome::NotStored => None,
        Outcome::Refused(_) if spec_of(command).verifies => Some(EACCES),
        _ => Some(EIO),
    }
}

/// One session on one slot: which command may go next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Session {
    last: Option<Command>,
    open: bool,
    /// The stored-km query's answer, once asked.
    stored: Option<bool>,
    encrypting: bool,
    failed: bool,
    lc_tries: u16,
    stream_tries: u8,
    /// Set when the previous L' or M' verification was refused while retries
    /// remain; allows only the corresponding retry (or teardown).
    lc_retry: bool,
    stream_retry: bool,
    /// r_tx of this session, for the H' context.
    pub(crate) rtx: [u8; RTX_LEN],
}

impl Session {
    pub(crate) const fn new() -> Session {
        Session {
            last: None,
            open: false,
            stored: None,
            encrypting: false,
            failed: false,
            lc_tries: 0,
            stream_tries: 0,
            lc_retry: false,
            stream_retry: false,
            rtx: [0; RTX_LEN],
        }
    }

    /// The protocol order: `next` may follow `last`.
    fn follows(&self, last: Command, next: Command) -> bool {
        use Command::*;
        if self.lc_retry {
            return next == GetRn && self.lc_tries < LC_MAX_TRIES;
        }
        if self.stream_retry {
            return next == PutStream && self.stream_tries < STREAM_MAX_TRIES;
        }
        let stored = self.stored == Some(true);
        let fresh = self.stored == Some(false);
        matches!(
            (last, next),
            (Open, Prepare)
                | (Prepare, Start)
                | (Start, GetRtx)
                | (GetRtx, PutCert)
                | (PutCert, PutRrx)
                | (PutRrx, QueryStoredKm)
                | (GetEkpubKm, PutHContext)
                | (PutHContext, PutHPrime)
                | (PutPairing, GetRn)
                | (GetRn, LcStart)
                | (LcStart, PutLPrime)
                | (PutLPrime, GetEks)
                | (GetEks, GetRiv)
                | (GetRiv, PutReceiverList)
                | (GetRiv, Finish)
                | (PutReceiverList, PutVPrime)
                | (PutVPrime, GetV)
                | (GetV, PutStream)
                | (PutStream, PutMPrime)
                | (PutMPrime, Finish)
        ) || match (last, next) {
            (QueryStoredKm, GetEkpubKm) => fresh,
            (QueryStoredKm, PutHContext) => stored,
            (PutHPrime, PutPairing) => fresh,
            (PutHPrime, GetRn) => stored,
            // The receiver missed the locality check: try again with a new r_n.
            (GetRn, GetRn) => self.lc_tries < LC_MAX_TRIES,
            (Finish, SetEncryption) => true,
            _ => false,
        }
    }

    /// Whether `command` may be sent now.
    pub(crate) fn allows(&self, command: Command, arg: u32) -> bool {
        if matches!(command, Command::QueryChannels | Command::GetChannelInfo) {
            return false;
        }
        if !self.open {
            return command == Command::Open;
        }
        match command {
            Command::Open => false,
            // Teardown: encryption off, then close, from any open state.
            Command::SetEncryption if arg == 0 => true,
            Command::Close => !self.encrypting,
            _ if self.failed || self.encrypting => false,
            Command::SetEncryption if arg != ENCRYPTION_ON => false,
            _ => self.last.is_some_and(|last| self.follows(last, command)),
        }
    }

    /// Records the outcome of a command [`Session::allows`] let through;
    /// `false` when the session cannot advance on this command. Anything other
    /// than a bounded LC/stream verification retry fails the session so only
    /// teardown is allowed after it.
    pub(crate) fn record(&mut self, command: Command, arg: u32, outcome: Outcome) -> bool {
        match outcome {
            Outcome::Done(_) => {
                if command == Command::QueryStoredKm {
                    self.stored = Some(true);
                }
            }
            Outcome::NotStored if command == Command::QueryStoredKm => {
                self.stored = Some(false);
            }
            Outcome::Refused(_)
                if command == Command::PutLPrime && self.lc_tries < LC_MAX_TRIES =>
            {
                self.lc_retry = true;
                self.last = Some(command);
                return false;
            }
            Outcome::Refused(_)
                if command == Command::PutMPrime && self.stream_tries < STREAM_MAX_TRIES =>
            {
                self.stream_retry = true;
                self.last = Some(command);
                return false;
            }
            _ => {
                if command == Command::Close {
                    // The enclave refused to close, or never answered:
                    // nothing more goes to this slot from this session.
                    *self = Session::new();
                    return false;
                }
                self.failed = true;
                self.encrypting = false;
                self.lc_retry = false;
                self.stream_retry = false;
                return false;
            }
        }
        self.lc_retry = false;
        self.stream_retry = false;
        match command {
            Command::Close => {
                *self = Session::new();
                return true;
            }
            Command::Open => self.open = true,
            Command::SetEncryption if arg == 0 => {
                // No way back to encryption without a new session.
                self.encrypting = false;
                self.failed = true;
            }
            Command::SetEncryption => self.encrypting = true,
            Command::GetRn => self.lc_tries += 1,
            Command::PutStream => self.stream_tries += 1,
            _ => {}
        }
        self.last = Some(command);
        true
    }

    /// Encryption is on: every step acknowledged, in order.
    pub(crate) fn encrypting(&self) -> bool {
        self.encrypting
    }

    pub(crate) fn is_open(&self) -> bool {
        self.open
    }
}

/// The endpoint: probed channel bitmask, a session per slot, and whether it is
/// quarantined. Channel IDs returned by command `0x28` may be sparse (e.g. an
/// absent channel ID with higher channel IDs above the interface count).
pub(crate) struct State {
    pub(crate) channel_mask: Option<u32>,
    pub(crate) sessions: [Session; MAX_CHANNELS],
    pub(crate) quarantined: bool,
}

impl State {
    pub(crate) const fn new() -> State {
        State {
            channel_mask: None,
            sessions: [Session::new(); MAX_CHANNELS],
            quarantined: false,
        }
    }

    /// Whether `slot` is an enumerated HDCP channel on this enclave.
    pub(crate) fn has_channel(&self, slot: u8) -> bool {
        usize::from(slot) < MAX_CHANNELS
            && self
                .channel_mask
                .is_some_and(|mask| (mask & (1u32 << slot)) != 0)
    }
}

/// The single exchange the endpoint can have in flight.
pub(crate) struct Exchange {
    waiting: bool,
    generation: u64,
    reply: Option<Message>,
}

impl Exchange {
    pub(crate) const fn new() -> Exchange {
        Exchange {
            waiting: false,
            generation: 0,
            reply: None,
        }
    }

    pub(crate) fn begin(&mut self) -> u64 {
        self.generation = self.generation.wrapping_add(1);
        self.waiting = true;
        self.reply = None;
        self.generation
    }

    /// Takes `msg` as the answer if an exchange waits and has none yet.
    pub(crate) fn answer(&mut self, msg: Message) -> bool {
        if !self.waiting || self.reply.is_some() {
            return false;
        }
        self.reply = Some(msg);
        true
    }

    pub(crate) fn take(&mut self) -> Option<Message> {
        let reply = self.reply.take();
        if reply.is_some() {
            self.waiting = false;
        }
        reply
    }

    pub(crate) fn end(&mut self) {
        self.waiting = false;
        self.reply = None;
    }
}

/// The steps the display driver asks for (`enum sep_hdcp_step` in shim.h).
/// Each is a fixed run of commands; inputs and outputs are flat.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum Step {
    /// out: r_tx.
    Open = 0,
    /// in: cert, r_rx. out: 1 + Ekh(km) + m (stored), or 0 + Ekpub(km).
    AkeCert = 1,
    /// in: RxCaps, TxCaps, H'.
    HPrime = 2,
    /// in: Ekh(km).
    Pairing = 3,
    /// out: r_n.
    LcInit = 4,
    /// in: L'.
    LPrime = 5,
    /// out: Edkey(ks), r_iv.
    Ske = 6,
    /// in: Receiver IDs, RxInfo, seq_num_V, V'. out: V.
    ReceiverList = 7,
    /// in: StreamID, Type, seq_num_M, M'.
    Stream = 8,
    Finish = 9,
    EncryptionOn = 10,
    EncryptionOff = 11,
}

pub(crate) const AKE_CERT_IN: usize = CERT_LEN + RRX_LEN;
pub(crate) const AKE_CERT_OUT: usize = 1 + EKPUB_KM_LEN;
pub(crate) const H_PRIME_IN: usize = 2 * CAPS_LEN + H_PRIME_LEN;
pub(crate) const SKE_OUT: usize = EKS_LEN + RIV_LEN;
pub(crate) const STREAM_IN: usize = STREAM_LEN + M_PRIME_LEN;

impl Step {
    fn from_raw(raw: u32) -> Option<Step> {
        Some(match raw {
            0 => Step::Open,
            1 => Step::AkeCert,
            2 => Step::HPrime,
            3 => Step::Pairing,
            4 => Step::LcInit,
            5 => Step::LPrime,
            6 => Step::Ske,
            7 => Step::ReceiverList,
            8 => Step::Stream,
            9 => Step::Finish,
            10 => Step::EncryptionOn,
            11 => Step::EncryptionOff,
            _ => return None,
        })
    }

    /// Whether `inp` and `out` have this step's sizes.
    pub(crate) fn sizes_ok(self, inp: usize, out: usize) -> bool {
        match self {
            Step::Open => inp == 0 && out == RTX_LEN,
            Step::AkeCert => inp == AKE_CERT_IN && out == AKE_CERT_OUT,
            Step::HPrime => inp == H_PRIME_IN && out == 0,
            Step::Pairing => inp == EKH_KM_LEN && out == 0,
            Step::LcInit => inp == 0 && out == RN_LEN,
            Step::LPrime => inp == L_PRIME_LEN && out == 0,
            Step::Ske => inp == 0 && out == SKE_OUT,
            Step::ReceiverList => {
                inp >= V_PRIME_LEN
                    && input_len_ok(&spec_of(Command::PutReceiverList), inp - V_PRIME_LEN)
                    && out == V_LEN
            }
            Step::Stream => inp == STREAM_IN && out == 0,
            Step::Finish | Step::EncryptionOn | Step::EncryptionOff => inp == 0 && out == 0,
        }
    }
}

/// Runs commands against the HDCP endpoint; the SEP side or a test double.
pub(crate) trait Transport {
    /// Sends `command` with `arg` and `input`; on [`Outcome::Done`] with
    /// output, the output bytes are copied to `out` (exactly its length).
    fn exchange(
        &self,
        slot: u8,
        command: Command,
        arg: u32,
        input: &[u8],
        out: &mut [u8],
    ) -> Result<Outcome>;

    /// Notified when the endpoint is quarantined after an unanswered command.
    fn on_quarantine(&self, _slot: u8, _command: Command) {}
}

/// Queries the enclave's HDCP interface count (`0x27`) and per-interface
/// descriptors (`0x28`), once per boot before any session opens. Returns the
/// bitmask of advertised channel IDs (`info[0]` per interface index).
pub(crate) fn enumerate<T: Transport>(t: &T, state: &mut State) -> Result<u32> {
    if state.quarantined {
        return Err(EIO);
    }
    if let Some(mask) = state.channel_mask {
        return Ok(mask);
    }
    let outcome = match t.exchange(0, Command::QueryChannels, 0, &[], &mut []) {
        Ok(o) => o,
        Err(e) => return Err(e),
    };
    if outcome == Outcome::NoReply {
        state.quarantined = true;
        t.on_quarantine(0, Command::QueryChannels);
        return Err(EIO);
    }
    let Outcome::ChannelCount(count) = outcome else {
        return Err(EIO);
    };
    let mut mask = 0u32;
    for index in 0..count {
        let mut info = [0u8; CHANNEL_INFO_LEN];
        let o = match t.exchange(index, Command::GetChannelInfo, 0, &[], &mut info) {
            Ok(o) => o,
            Err(e) => return Err(e),
        };
        if o == Outcome::NoReply {
            state.quarantined = true;
            t.on_quarantine(index, Command::GetChannelInfo);
            return Err(EIO);
        }
        let ch = info[0];
        if o != Outcome::Done(CHANNEL_INFO_LEN) || usize::from(ch) >= MAX_CHANNELS {
            return Err(EIO);
        }
        mask |= 1u32 << ch;
    }
    if mask == 0 {
        return Err(EIO);
    }
    state.channel_mask = Some(mask);
    Ok(mask)
}

/// Sends one session command on `slot` if the session allows it, and records
/// the outcome.
fn run<T: Transport>(
    t: &T,
    state: &mut State,
    slot: u8,
    command: Command,
    arg: u32,
    input: &[u8],
    out: &mut [u8],
) -> Result<Outcome> {
    if state.quarantined {
        return Err(EIO);
    }
    if !state.has_channel(slot) {
        return Err(EINVAL);
    }
    let i = usize::from(slot);
    if !state.sessions[i].allows(command, arg) {
        return Err(EPROTO);
    }
    if out.len() != spec_of(command).output {
        return Err(EINVAL);
    }
    let outcome = match t.exchange(slot, command, arg, input, out) {
        Ok(o) => o,
        Err(e) => {
            // Not sent (sleep gate, shutdown): if the session is already open,
            // only teardown is safe afterwards.
            if state.sessions[i].is_open() {
                state.sessions[i].record(command, arg, Outcome::Malformed);
            }
            return Err(if e == EINVAL || e == EAGAIN || e == ENODEV {
                e
            } else {
                EIO
            });
        }
    };
    if outcome == Outcome::NoReply {
        state.quarantined = true;
        t.on_quarantine(slot, command);
    }
    state.sessions[i].record(command, arg, outcome);
    match errno(command, outcome) {
        Some(e) => Err(e),
        None => Ok(outcome),
    }
}

/// Runs one display-driver step on `slot`.
pub(crate) fn step<T: Transport>(
    t: &T,
    state: &mut State,
    slot: u8,
    step: Step,
    inp: &[u8],
    out: &mut [u8],
) -> Result {
    if !step.sizes_ok(inp.len(), out.len()) {
        return Err(EINVAL);
    }
    enumerate(t, state)?;
    if !state.has_channel(slot) {
        return Err(EINVAL);
    }
    let i = usize::from(slot);
    let was_open = state.sessions[i].is_open();
    let r = step_inner(t, state, slot, step, inp, out);
    if r.is_err() {
        out.fill(0);
        // Only tear down a session that this Open call started; never clobber
        // an already-open session when a duplicate Open fails with -EPROTO.
        if step == Step::Open && !was_open && state.sessions[i].is_open() {
            close(t, state, slot);
        }
    }
    r
}

fn step_inner<T: Transport>(
    t: &T,
    state: &mut State,
    slot: u8,
    step: Step,
    inp: &[u8],
    out: &mut [u8],
) -> Result {
    use Command::*;
    let none: &mut [u8] = &mut [];
    match step {
        Step::Open => {
            run(t, state, slot, Open, OPEN_ARG, &[], &mut [])?;
            run(t, state, slot, Prepare, PREPARE_ARG, &[], &mut [])?;
            run(t, state, slot, Start, 0, &[], &mut [])?;
            run(t, state, slot, GetRtx, 0, &[], out)?;
            state.sessions[usize::from(slot)].rtx.copy_from_slice(out);
        }
        Step::AkeCert => {
            run(t, state, slot, PutCert, 0, &inp[..CERT_LEN], none)?;
            run(t, state, slot, PutRrx, 0, &inp[CERT_LEN..], none)?;
            let (flag, rest) = out.split_at_mut(1);
            let o = run(
                t,
                state,
                slot,
                QueryStoredKm,
                0,
                &[],
                &mut rest[..STORED_KM_LEN],
            )?;
            if o == Outcome::NotStored {
                rest[..STORED_KM_LEN].fill(0);
                flag[0] = 0;
                run(t, state, slot, GetEkpubKm, 0, &[], rest)?;
            } else {
                flag[0] = 1;
            }
        }
        Step::HPrime => {
            let mut ctx = [0u8; H_CONTEXT_LEN];
            ctx[..RTX_LEN].copy_from_slice(&state.sessions[usize::from(slot)].rtx);
            ctx[RTX_LEN..].copy_from_slice(&inp[..2 * CAPS_LEN]);
            run(t, state, slot, PutHContext, 0, &ctx, none)?;
            run(t, state, slot, PutHPrime, 0, &inp[2 * CAPS_LEN..], none)?;
        }
        Step::Pairing => {
            run(t, state, slot, PutPairing, 0, inp, none)?;
        }
        Step::LcInit => {
            run(t, state, slot, GetRn, 0, &[], out)?;
        }
        Step::LPrime => {
            run(t, state, slot, LcStart, 0, &[], none)?;
            run(t, state, slot, PutLPrime, 0, inp, none)?;
        }
        Step::Ske => {
            let (eks, riv) = out.split_at_mut(EKS_LEN);
            run(t, state, slot, GetEks, 0, &[], eks)?;
            run(t, state, slot, GetRiv, 0, &[], riv)?;
        }
        Step::ReceiverList => {
            let (list, v_prime) = inp.split_at(inp.len() - V_PRIME_LEN);
            run(t, state, slot, PutReceiverList, 0, list, none)?;
            run(t, state, slot, PutVPrime, 0, v_prime, none)?;
            run(t, state, slot, GetV, 0, &[], out)?;
        }
        Step::Stream => {
            run(t, state, slot, PutStream, 0, &inp[..STREAM_LEN], none)?;
            run(t, state, slot, PutMPrime, 0, &inp[STREAM_LEN..], none)?;
        }
        Step::Finish => {
            run(t, state, slot, Finish, FINISH_ARG, &[], none)?;
        }
        Step::EncryptionOn => {
            run(t, state, slot, SetEncryption, ENCRYPTION_ON, &[], none)?;
        }
        Step::EncryptionOff => {
            run(t, state, slot, SetEncryption, 0, &[], none)?;
        }
    }
    Ok(())
}

/// Ends the session on `slot` from any state: encryption off, then close.
/// The local session is reset whatever the enclave answers.
pub(crate) fn close<T: Transport>(t: &T, state: &mut State, slot: u8) {
    if !state.has_channel(slot) {
        return;
    }
    let i = usize::from(slot);
    if !state.sessions[i].is_open() {
        return;
    }
    if !state.quarantined {
        if state.sessions[i].encrypting() {
            let _ = run(t, state, slot, Command::SetEncryption, 0, &[], &mut []);
        }
        if !state.quarantined {
            let _ = run(t, state, slot, Command::Close, 0, &[], &mut []);
        }
    }
    state.sessions[i] = Session::new();
}

/// The mailbox side.
struct Mailbox<'a>(&'a SepData);

impl Transport for Mailbox<'_> {
    fn exchange(
        &self,
        slot: u8,
        command: Command,
        arg: u32,
        input: &[u8],
        out: &mut [u8],
    ) -> Result<Outcome> {
        let sep = self.0;
        let msg = request(command, slot, arg, input.len())?;
        if !input.is_empty() {
            sep.ool_write(&sep.ool_hdcp, 0, input)?;
        }
        if !out.is_empty() {
            // The last reply must not be taken for this one's.
            sep.ool_repoison(&sep.ool_hdcp, 0, out.len());
        }
        sep.hdcp_rx.lock().begin();
        if let Err(e) = sep.send(msg) {
            sep.hdcp_rx.lock().end();
            sep.hdcp_scrub_buffers(input.len(), out.len());
            return Err(e);
        }
        let reply = match sep.hdcp_wait() {
            Ok(Some(r)) => r,
            Ok(None) => {
                sep.hdcp_scrub_buffers(input.len(), 0);
                return Ok(Outcome::NoReply);
            }
            Err(e) => {
                sep.hdcp_scrub_buffers(input.len(), out.len());
                return Err(e);
            }
        };
        let outcome = check_reply(command, &reply);
        let final_outcome = match outcome {
            Outcome::Done(n) if n > 0 => {
                if n != out.len() || !sep.ool_await_written(&sep.ool_hdcp, 0, n) {
                    Outcome::Malformed
                } else {
                    match sep.ool_read(&sep.ool_hdcp, 0, n) {
                        Ok(bytes) => {
                            out.copy_from_slice(&bytes);
                            Outcome::Done(n)
                        }
                        Err(_) => Outcome::Malformed,
                    }
                }
            }
            other => other,
        };
        // Log non-success replies using only the mailbox word (status/len),
        // never any out-of-line payload bytes.
        match final_outcome {
            Outcome::Done(_) | Outcome::NotStored | Outcome::ChannelCount(_) => {}
            Outcome::Refused(_) if spec_of(command).verifies => {
                dev_dbg!(
                    sep.dev,
                    "hdcp: command {:#04x} channel {}: verification refused (reply {:#018x})\n",
                    spec_of(command).id,
                    slot,
                    reply.msg0
                );
            }
            other => {
                dev_warn_ratelimited!(
                    sep.dev,
                    "hdcp: command {:#04x} channel {}: {:?} (reply {:#018x}, expected {} out)\n",
                    spec_of(command).id,
                    slot,
                    other,
                    reply.msg0,
                    out.len()
                );
            }
        }
        sep.hdcp_scrub_buffers(input.len(), out.len());
        Ok(final_outcome)
    }

    fn on_quarantine(&self, slot: u8, command: Command) {
        self.0.hdcp_quarantined.store(true, Release);
        dev_err_ratelimited!(
            self.0.dev,
            "hdcp: no reply to command {:#04x} on channel {}; endpoint quarantined until reset\n",
            spec_of(command).id,
            slot
        );
    }
}

impl SepData {
    /// Records the discovery messages of the endpoint.
    pub(crate) fn hdcp_discovered(&self, f: &Fields) {
        if f.param != EP_HDCP {
            return;
        }
        match f.ty {
            DISCOVER_TYPE_DESCRIPTOR => self.hdcp_named.store(f.data_lo == HDCP_NAME, Relaxed),
            DISCOVER_TYPE_CONFIG => self.hdcp_layout.store(f.data_lo, Relaxed),
            _ => {}
        }
    }

    pub(crate) fn hdcp_offered(&self) -> bool {
        self.endpoint_present(EP_HDCP)
            && self.hdcp_named.load(Relaxed)
            && ool_sizes(self.hdcp_layout.load(Relaxed)).is_some()
    }

    fn hdcp_ready_state(&self) -> Result {
        if self.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        if self.phase.load(Relaxed) != PHASE_READY {
            return Err(EAGAIN);
        }
        if !self.hdcp_offered() {
            return Err(ENODEV);
        }
        match self.sleep_gate() {
            pm::SleepGate::Open => Ok(()),
            pm::SleepGate::Lost => Err(EIO),
            _ => Err(EAGAIN),
        }
    }

    fn hdcp_available(&self) -> bool {
        self.hdcp_ready_state().is_ok() && !self.hdcp_quarantined.load(Acquire)
    }

    fn hdcp_scrub_buffers(&self, in_len: usize, out_len: usize) {
        let guard = self.ool_hdcp.lock();
        let Some(buffers) = guard.as_ref() else {
            return;
        };
        let in_n = in_len.min(buffers.allocated);
        let out_n = out_len.min(buffers.allocated);
        // SAFETY: the exchange is finished (or never sent) under `hdcp_state`;
        // no concurrent host accessor touches these slices.
        unsafe {
            if in_n > 0 {
                buffers.inbound.as_mut()[..in_n].fill(0);
            }
            if out_n > 0 {
                buffers.outbound.as_mut()[..out_n].fill(OOL_POISON_OUTBOUND);
            }
        }
    }

    /// Waits for the reply to the exchange in flight; `None` on timeout.
    fn hdcp_wait(&self) -> Result<Option<Message>> {
        let mut remaining = time::msecs_to_jiffies(HDCP_TIMEOUT_MS);
        let mut guard = self.hdcp_rx.lock();
        loop {
            if self.shutting_down.load(Relaxed) {
                guard.end();
                return Err(ENODEV);
            }
            if let Some(reply) = guard.take() {
                return Ok(Some(reply));
            }
            if remaining == 0 {
                guard.end();
                return Ok(None);
            }
            // Uninterruptible, as the other reply waits.
            match self.hdcp_wq.wait_timeout(&mut guard, remaining) {
                CondVarTimeoutResult::Woken { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Signal { jiffies } => remaining = jiffies,
                CondVarTimeoutResult::Timeout => remaining = 0,
            }
        }
    }

    pub(crate) fn on_hdcp(&self, msg: Message) {
        if self.hdcp_rx.lock().answer(msg) {
            self.hdcp_wq.notify_all();
        } else if diag::unhandled() <= UNHANDLED_LOG_LIMIT {
            dev_info_ratelimited!(
                self.dev,
                "hdcp: unexpected message (msg0 {:#018x})\n",
                msg.msg0
            );
        }
    }

    fn hdcp_ready(&self) -> Result {
        self.hdcp_ready_state()?;
        self.register_ool(&self.ool_hdcp).map_err(|e| {
            dev_warn_ratelimited!(
                self.dev,
                "hdcp: out-of-line buffer registration failed ({:?})\n",
                e
            );
            if e == EAGAIN || e == ENODEV {
                e
            } else {
                EIO
            }
        })
    }

    fn hdcp_step(&self, channel: u32, raw: u32, inp: &[u8], out: &mut [u8]) -> Result {
        let slot = u8::try_from(channel).map_err(|_| EINVAL)?;
        let step_id = Step::from_raw(raw).ok_or(EINVAL)?;
        if let Err(e) = self.hdcp_ready() {
            out.fill(0);
            return Err(e);
        }
        let mut state = self.hdcp_state.lock();
        step(&Mailbox(self), &mut state, slot, step_id, inp, out)
    }

    fn hdcp_close(&self, channel: u32) {
        let Ok(slot) = u8::try_from(channel) else {
            return;
        };
        let mut state = self.hdcp_state.lock();
        close(&Mailbox(self), &mut state, slot);
    }

    pub(crate) fn register_hdcp_kernel(&self) -> Result {
        let context = core::ptr::from_ref(self).cast_mut().cast::<c_void>();
        // SAFETY: `KERNEL_OPS` is static and `remove()` unregisters this
        // pointer before the driver's `Arc<SepData>` can be dropped.
        kernel::error::to_result(unsafe { sep_hdcp_register(context, &KERNEL_OPS) })
    }

    pub(crate) fn unregister_hdcp_kernel(&self) {
        let context = core::ptr::from_ref(self).cast_mut().cast::<c_void>();
        // SAFETY: the pointer is the one passed to `sep_hdcp_register`.
        unsafe { sep_hdcp_unregister(context) };
    }
}

/// `struct sep_hdcp_ops` in shim.h.
#[repr(C)]
struct KernelOps {
    available: unsafe extern "C" fn(*mut c_void) -> bool,
    step: unsafe extern "C" fn(*mut c_void, u32, u32, *const u8, usize, *mut u8, usize) -> c_int,
    close: unsafe extern "C" fn(*mut c_void, u32),
}

extern "C" {
    fn sep_hdcp_register(context: *mut c_void, ops: *const KernelOps) -> c_int;
    fn sep_hdcp_unregister(context: *mut c_void);
}

static KERNEL_OPS: KernelOps = KernelOps {
    available: kernel_available,
    step: kernel_step,
    close: kernel_close,
};

/// # Safety
///
/// `context` is the registered `SepData`; the shim holds its registration
/// lock, so it outlives the call.
unsafe extern "C" fn kernel_available(context: *mut c_void) -> bool {
    // SAFETY: see the function's safety section.
    let sep = unsafe { &*context.cast::<SepData>() };
    sep.hdcp_available()
}

/// # Safety
///
/// As [`kernel_available`]; `inp`/`out` are valid for their lengths (or the
/// length is 0) and do not overlap.
unsafe extern "C" fn kernel_step(
    context: *mut c_void,
    channel: u32,
    step: u32,
    inp: *const u8,
    inp_len: usize,
    out: *mut u8,
    out_len: usize,
) -> c_int {
    // SAFETY: see the function's safety section.
    let sep = unsafe { &*context.cast::<SepData>() };
    let inp: &[u8] = if inp_len == 0 {
        &[]
    } else {
        // SAFETY: see the function's safety section.
        unsafe { core::slice::from_raw_parts(inp, inp_len) }
    };
    let out: &mut [u8] = if out_len == 0 {
        &mut []
    } else {
        // SAFETY: see the function's safety section.
        unsafe { core::slice::from_raw_parts_mut(out, out_len) }
    };
    match sep.hdcp_step(channel, step, inp, out) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
///
/// As [`kernel_available`].
unsafe extern "C" fn kernel_close(context: *mut c_void, channel: u32) {
    // SAFETY: see the function's safety section.
    let sep = unsafe { &*context.cast::<SepData>() };
    sep.hdcp_close(channel);
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_hdcp)]
mod tests {
    use super::*;
    use core::cell::{Cell, RefCell};

    // The request words of one first-pairing session to a repeater on slot 7,
    // each with its reply word. The stored-km query answers status 0x08: no
    // km stored.
    const SLOT: u8 = 0x07;
    const TRACE: [(Command, u32, usize, u64, u64); 23] = [
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
            Command::QueryStoredKm,
            0,
            0,
            0x0000_0000_0700_090e,
            0x0000_0000_0020_080e,
        ),
        (
            Command::GetEkpubKm,
            0,
            0,
            0x0000_0000_0700_0c0e,
            0x0000_0000_0080_000e,
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
            Command::PutPairing,
            0,
            16,
            0x0000_0010_0700_160e,
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
            Command::PutReceiverList,
            0,
            10,
            0x0000_000a_0700_1d0e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::PutVPrime,
            0,
            16,
            0x0000_0010_0700_1f0e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::GetV,
            0,
            0,
            0x0000_0000_0700_1e0e,
            0x0000_0000_0020_000e,
        ),
        (
            Command::PutStream,
            0,
            5,
            0x0000_0005_0700_230e,
            0x0000_0000_0000_000e,
        ),
        (
            Command::PutMPrime,
            0,
            32,
            0x0000_0020_0700_250e,
            0x0000_0000_0000_000e,
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
        // The second word is stale memory; it must not matter.
        Message {
            msg0: w,
            msg1: 0x0bad_f00d,
        }
    }

    /// A transport that answers from a script of reply words and logs what
    /// was sent.
    struct Fake {
        replies: RefCell<KVec<u64>>,
        channel_ids: RefCell<KVec<u8>>,
        sent: RefCell<KVec<(Command, u8, u32, usize)>>,
        next: Cell<usize>,
        quarantined: Cell<bool>,
    }

    impl Fake {
        fn new(replies: &[u64]) -> Fake {
            let mut v = KVec::new();
            for r in replies {
                v.push(*r, GFP_KERNEL).unwrap();
            }
            Fake {
                replies: RefCell::new(v),
                channel_ids: RefCell::new(KVec::new()),
                sent: RefCell::new(KVec::new()),
                next: Cell::new(0),
                quarantined: Cell::new(false),
            }
        }

        fn with_channel_ids(replies: &[u64], ids: &[u8]) -> Fake {
            let f = Fake::new(replies);
            for &id in ids {
                f.channel_ids.borrow_mut().push(id, GFP_KERNEL).unwrap();
            }
            f
        }

        fn sent(&self) -> KVec<Command> {
            let mut v = KVec::new();
            for s in self.sent.borrow().iter() {
                v.push(s.0, GFP_KERNEL).unwrap();
            }
            v
        }
    }

    impl Transport for Fake {
        fn exchange(
            &self,
            slot: u8,
            command: Command,
            arg: u32,
            input: &[u8],
            out: &mut [u8],
        ) -> Result<Outcome> {
            request(command, slot, arg, input.len())?;
            self.sent
                .borrow_mut()
                .push((command, slot, arg, input.len()), GFP_KERNEL)?;
            let i = self.next.get();
            self.next.set(i + 1);
            let Some(&w) = self.replies.borrow().get(i) else {
                return Ok(Outcome::NoReply);
            };
            let o = check_reply(command, &reply(w));
            if let Outcome::Done(n) = o {
                if n != out.len() {
                    return Ok(Outcome::Malformed);
                }
                if command == Command::GetChannelInfo {
                    out.fill(0);
                    let ids = self.channel_ids.borrow();
                    out[0] = ids.get(usize::from(slot)).copied().unwrap_or(slot);
                } else {
                    out.fill(0x5c);
                }
            }
            Ok(o)
        }

        fn on_quarantine(&self, _slot: u8, _command: Command) {
            self.quarantined.set(true);
        }
    }

    const OK: u64 = 0x0000_0000_0000_000e;

    fn ok_for(c: Command) -> u64 {
        OK | ((spec_of(c).output as u64) << 16)
    }

    fn state_with_channels(n: u8) -> State {
        let mut st = State::new();
        st.channel_mask = Some((1u32 << n) - 1);
        st
    }

    #[test]
    fn the_requests_match_the_recording() {
        assert_eq!(
            request(Command::QueryChannels, 0, 0, 0).unwrap().msg0,
            0x0000_0000_0000_270e
        );
        assert_eq!(
            request(Command::GetChannelInfo, SLOT, 0, 0).unwrap().msg0,
            0x0000_0000_0700_280e
        );
        assert!(request(Command::QueryChannels, 1, 0, 0).is_err());
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
    fn discovery_layout_and_channel_enumeration_follow_hardware() -> Result {
        assert_eq!(ool_sizes(0x0101_0101), Some((OOL_SIZE_HDCP, OOL_SIZE_HDCP)));
        assert_eq!(ool_sizes(0x0101_0202), Some((OOL_SIZE_HDCP, OOL_SIZE_HDCP)));
        assert_eq!(ool_sizes(0x0202_0202), None);
        assert_eq!(ool_sizes(0), None);

        // Enclave advertises 9 contiguous channels (as on T8112); slot 8 ok, slot 9 EINVAL.
        let mut replies = KVec::new();
        replies.push(0x0000_0009_0000_000e, GFP_KERNEL)?;
        for _ in 0..9 {
            replies.push(ok_for(Command::GetChannelInfo), GFP_KERNEL)?;
        }
        let t = Fake::new(&replies);
        let mut st = State::new();
        assert_eq!(enumerate(&t, &mut st)?, 0x1ff);
        assert!(st.has_channel(8));
        assert!(!st.has_channel(9));
        // Second enumeration is cached (no further mailbox traffic).
        assert_eq!(enumerate(&t, &mut st)?, 0x1ff);
        assert_eq!(t.sent().len(), 10);
        assert_eq!(
            step(&t, &mut st, 9, Step::Open, &[], &mut [0; RTX_LEN]),
            Err(EINVAL)
        );
        assert_eq!(t.sent().len(), 10);

        // Sparse channel table (14 interfaces: 0..=5 and 7..=14, skipping 6,
        // as on T6040): channel 6 is refused, channels 7 and 14 are accepted.
        let sparse_ids: [u8; 14] = [0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14];
        let mut sparse_replies = KVec::new();
        sparse_replies.push(0x0000_000e_0000_000e, GFP_KERNEL)?;
        for _ in 0..14 {
            sparse_replies.push(ok_for(Command::GetChannelInfo), GFP_KERNEL)?;
        }
        for c in [Command::Open, Command::Prepare, Command::Start, Command::GetRtx] {
            sparse_replies.push(ok_for(c), GFP_KERNEL)?;
        }
        let ts = Fake::with_channel_ids(&sparse_replies, &sparse_ids);
        let mut sts = State::new();
        let mask = enumerate(&ts, &mut sts)?;
        assert!(!sts.has_channel(6));
        assert!(sts.has_channel(7));
        assert!(sts.has_channel(14));
        assert!(!sts.has_channel(15));
        assert_eq!(mask, 0x7fbf);
        assert_eq!(
            step(&ts, &mut sts, 6, Step::Open, &[], &mut [0; RTX_LEN]),
            Err(EINVAL)
        );
        step(&ts, &mut sts, 14, Step::Open, &[], &mut [0; RTX_LEN])?;
        Ok(())
    }

    #[test]
    fn the_recorded_session_runs_to_encryption_and_tears_down() {
        let mut s = Session::new();
        for &(c, arg, _, _, rx) in TRACE.iter() {
            assert!(s.allows(c, arg));
            let o = check_reply(c, &reply(rx));
            assert!(errno(c, o).is_none());
            assert!(s.record(c, arg, o));
        }
        assert!(s.encrypting());
        assert!(!s.allows(Command::Close, 0));
        assert!(!s.allows(Command::GetRn, 0));
        assert!(s.allows(Command::SetEncryption, 0));
        assert!(s.record(Command::SetEncryption, 0, Outcome::Done(0)));
        assert!(!s.encrypting());
        assert!(!s.allows(Command::SetEncryption, ENCRYPTION_ON));
        assert!(s.allows(Command::Close, 0));
        assert!(s.record(Command::Close, 0, Outcome::Done(0)));
        assert!(!s.is_open());
        assert!(s.allows(Command::Open, OPEN_ARG));
    }

    #[test]
    fn the_steps_send_the_first_pairing_repeater_order() -> Result {
        let mut replies = KVec::new();
        for &(_, _, _, _, rx) in TRACE.iter() {
            replies.push(rx, GFP_KERNEL)?;
        }
        replies.push(OK, GFP_KERNEL)?;
        replies.push(OK, GFP_KERNEL)?;
        let t = Fake::new(&replies);
        let mut st = state_with_channels(9);
        let mut rtx = [0u8; RTX_LEN];
        step(&t, &mut st, SLOT, Step::Open, &[], &mut rtx)?;
        let mut cert_out = [0u8; AKE_CERT_OUT];
        step(
            &t,
            &mut st,
            SLOT,
            Step::AkeCert,
            &[1; AKE_CERT_IN],
            &mut cert_out,
        )?;
        assert_eq!(cert_out[0], 0);
        step(&t, &mut st, SLOT, Step::HPrime, &[2; H_PRIME_IN], &mut [])?;
        step(&t, &mut st, SLOT, Step::Pairing, &[3; EKH_KM_LEN], &mut [])?;
        step(&t, &mut st, SLOT, Step::LcInit, &[], &mut [0; RN_LEN])?;
        step(&t, &mut st, SLOT, Step::LPrime, &[4; L_PRIME_LEN], &mut [])?;
        step(&t, &mut st, SLOT, Step::Ske, &[], &mut [0; SKE_OUT])?;
        step(
            &t,
            &mut st,
            SLOT,
            Step::ReceiverList,
            &[5; LIST_MIN + V_PRIME_LEN],
            &mut [0; V_LEN],
        )?;
        step(&t, &mut st, SLOT, Step::Stream, &[6; STREAM_IN], &mut [])?;
        step(&t, &mut st, SLOT, Step::Finish, &[], &mut [])?;
        step(&t, &mut st, SLOT, Step::EncryptionOn, &[], &mut [])?;
        assert!(st.sessions[SLOT as usize].encrypting());
        close(&t, &mut st, SLOT);
        let sent = t.sent();
        assert_eq!(sent.len(), TRACE.len() + 2);
        for (i, &(c, ..)) in TRACE.iter().enumerate() {
            assert_eq!(sent[i], c);
        }
        assert_eq!(sent[TRACE.len()], Command::SetEncryption);
        assert_eq!(sent[TRACE.len() + 1], Command::Close);
        assert!(!st.sessions[SLOT as usize].is_open());
        Ok(())
    }

    #[test]
    fn duplicate_open_does_not_clobber_active_session() -> Result {
        use Command::*;
        let mut replies = KVec::new();
        for c in [Open, Prepare, Start, GetRtx] {
            replies.push(ok_for(c), GFP_KERNEL)?;
        }
        let t = Fake::new(&replies);
        let mut st = state_with_channels(9);
        step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; RTX_LEN])?;
        let sent_after_open = t.sent().len();
        assert_eq!(
            step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; RTX_LEN]),
            Err(EPROTO)
        );
        assert!(st.sessions[SLOT as usize].is_open());
        assert_eq!(t.sent().len(), sent_after_open);
        Ok(())
    }

    #[test]
    fn the_stored_km_path_skips_pairing() -> Result {
        use Command::*;
        let order = [
            Open,
            Prepare,
            Start,
            GetRtx,
            PutCert,
            PutRrx,
            QueryStoredKm,
            PutHContext,
            PutHPrime,
            GetRn,
            LcStart,
            PutLPrime,
            GetEks,
            GetRiv,
            Finish,
            SetEncryption,
        ];
        let mut replies = KVec::new();
        for c in order.iter() {
            replies.push(ok_for(*c), GFP_KERNEL)?;
        }
        let t = Fake::new(&replies);
        let mut st = state_with_channels(15);
        step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; RTX_LEN])?;
        let mut cert_out = [0u8; AKE_CERT_OUT];
        step(
            &t,
            &mut st,
            SLOT,
            Step::AkeCert,
            &[1; AKE_CERT_IN],
            &mut cert_out,
        )?;
        assert_eq!(cert_out[0], 1);
        step(&t, &mut st, SLOT, Step::HPrime, &[2; H_PRIME_IN], &mut [])?;
        // No pairing after a stored km.
        assert_eq!(
            step(&t, &mut st, SLOT, Step::Pairing, &[3; EKH_KM_LEN], &mut []),
            Err(EPROTO)
        );
        Ok(())
    }

    #[test]
    fn out_of_order_and_bad_sizes_are_refused() {
        let t = Fake::new(&[]);
        let mut st = state_with_channels(15);
        // Nothing before open; nothing reaches the transport.
        assert_eq!(
            step(&t, &mut st, SLOT, Step::LcInit, &[], &mut [0; RN_LEN]),
            Err(EPROTO)
        );
        assert_eq!(
            step(&t, &mut st, SLOT, Step::EncryptionOn, &[], &mut []),
            Err(EPROTO)
        );
        assert_eq!(
            step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; 7]),
            Err(EINVAL)
        );
        assert_eq!(
            step(&t, &mut st, 15, Step::Open, &[], &mut [0; RTX_LEN]),
            Err(EINVAL)
        );
        // A list of no Receiver ID, of 32, or of a partial one.
        for n in [
            V_PRIME_LEN + 5,
            V_PRIME_LEN + LIST_MAX + 5,
            V_PRIME_LEN + LIST_MIN + 3,
        ] {
            assert!(!Step::ReceiverList.sizes_ok(n, V_LEN));
        }
        assert!(Step::ReceiverList.sizes_ok(V_PRIME_LEN + LIST_MAX, V_LEN));
        assert!(t.sent().is_empty());
        assert!(request(Command::PutCert, SLOT, 0, 521).is_err());
        assert!(request(Command::Open, SLOT, 0, 0).is_err());
        assert!(request(Command::GetRtx, SLOT, 0, 8).is_err());
        assert!(request(Command::SetEncryption, SLOT, 3, 0).is_err());
    }

    #[test]
    fn a_refused_verification_is_eacces_and_fails_the_session() -> Result {
        use Command::*;
        let mut replies = KVec::new();
        for c in [
            Open,
            Prepare,
            Start,
            GetRtx,
            PutCert,
            PutRrx,
            QueryStoredKm,
            PutHContext,
        ] {
            replies.push(ok_for(c), GFP_KERNEL)?;
        }
        // H' refused with some status.
        replies.push(0x0000_0000_0000_030e, GFP_KERNEL)?;
        replies.push(OK, GFP_KERNEL)?;
        let t = Fake::new(&replies);
        let mut st = state_with_channels(15);
        step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; RTX_LEN])?;
        step(
            &t,
            &mut st,
            SLOT,
            Step::AkeCert,
            &[1; AKE_CERT_IN],
            &mut [0; AKE_CERT_OUT],
        )?;
        assert_eq!(
            step(&t, &mut st, SLOT, Step::HPrime, &[2; H_PRIME_IN], &mut []),
            Err(EACCES)
        );
        assert_eq!(
            step(&t, &mut st, SLOT, Step::LcInit, &[], &mut [0; RN_LEN]),
            Err(EPROTO)
        );
        close(&t, &mut st, SLOT);
        assert_eq!(*t.sent().last().unwrap(), Close);
        assert!(st.sessions[SLOT as usize].allows(Open, OPEN_ARG));
        // A refused non-verification command is EIO.
        assert_eq!(errno(GetRtx, Outcome::Refused(1)), Some(EIO));
        assert_eq!(errno(PutMPrime, Outcome::Refused(1)), Some(EACCES));
        assert_eq!(errno(PutReceiverList, Outcome::Refused(1)), Some(EACCES));
        Ok(())
    }

    #[test]
    fn stream_manage_allows_one_retry_after_refused_m_prime() -> Result {
        use Command::*;
        let mut replies = KVec::new();
        for c in [
            Open,
            Prepare,
            Start,
            GetRtx,
            PutCert,
            PutRrx,
            QueryStoredKm,
            PutHContext,
            PutHPrime,
            GetRn,
            LcStart,
            PutLPrime,
            GetEks,
            GetRiv,
            PutReceiverList,
            PutVPrime,
            GetV,
            PutStream,
        ] {
            replies.push(ok_for(c), GFP_KERNEL)?;
        }
        // First M' refused, second attempt succeeds, then Finish succeeds.
        replies.push(0x0000_0000_0000_030e, GFP_KERNEL)?;
        replies.push(OK, GFP_KERNEL)?;
        replies.push(OK, GFP_KERNEL)?;
        replies.push(OK, GFP_KERNEL)?;
        let t = Fake::new(&replies);
        let mut st = state_with_channels(9);
        step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; RTX_LEN])?;
        step(
            &t,
            &mut st,
            SLOT,
            Step::AkeCert,
            &[1; AKE_CERT_IN],
            &mut [0; AKE_CERT_OUT],
        )?;
        step(&t, &mut st, SLOT, Step::HPrime, &[2; H_PRIME_IN], &mut [])?;
        step(&t, &mut st, SLOT, Step::LcInit, &[], &mut [0; RN_LEN])?;
        step(&t, &mut st, SLOT, Step::LPrime, &[4; L_PRIME_LEN], &mut [])?;
        step(&t, &mut st, SLOT, Step::Ske, &[], &mut [0; SKE_OUT])?;
        step(
            &t,
            &mut st,
            SLOT,
            Step::ReceiverList,
            &[5; LIST_MIN + V_PRIME_LEN],
            &mut [0; V_LEN],
        )?;
        assert_eq!(
            step(&t, &mut st, SLOT, Step::Stream, &[6; STREAM_IN], &mut []),
            Err(EACCES)
        );
        // Finish is refused while retry is pending.
        assert_eq!(step(&t, &mut st, SLOT, Step::Finish, &[], &mut []), Err(EPROTO));
        // Retry succeeds, then Finish proceeds.
        step(&t, &mut st, SLOT, Step::Stream, &[7; STREAM_IN], &mut [])?;
        step(&t, &mut st, SLOT, Step::Finish, &[], &mut [])?;
        Ok(())
    }

    #[test]
    fn replies_are_judged_by_status_and_length() {
        assert_eq!(
            check_reply(Command::QueryChannels, &reply(0x0000_0009_0000_000e)),
            Outcome::ChannelCount(9)
        );
        assert_eq!(
            check_reply(Command::QueryChannels, &reply(0x0000_0000_0000_000e)),
            Outcome::Malformed
        );
        assert_eq!(
            check_reply(Command::QueryChannels, &reply(0x0000_0021_0000_000e)),
            Outcome::Malformed
        );
        assert_eq!(
            check_reply(Command::GetRtx, &reply(0x0000_0000_0010_000e)),
            Outcome::Malformed
        );
        assert_eq!(
            check_reply(Command::GetEkpubKm, &reply(0x0000_0000_0080_000e)),
            Outcome::Done(128)
        );
        assert_eq!(
            check_reply(Command::QueryStoredKm, &reply(0x0000_0000_0020_080e)),
            Outcome::NotStored
        );
        // Status 0x08 means "not stored" for the query only.
        assert_eq!(
            check_reply(Command::GetRn, &reply(0x0000_0000_0000_080e)),
            Outcome::Refused(8)
        );
        for w in [
            0x0000_0000_0000_000fu64,
            0x0000_0000_0100_000e,
            0x0000_0001_0000_000e,
        ] {
            assert_eq!(check_reply(Command::Start, &reply(w)), Outcome::Malformed);
        }
    }

    #[test]
    fn silence_quarantines_every_slot() -> Result {
        let t = Fake::new(&[OK]);
        let mut st = state_with_channels(15);
        // Open answered, Prepare never: the whole endpoint stops.
        assert_eq!(
            step(&t, &mut st, SLOT, Step::Open, &[], &mut [0; RTX_LEN]),
            Err(EIO)
        );
        assert!(st.quarantined);
        assert!(t.quarantined.get());
        let n = t.sent().len();
        assert_eq!(
            step(&t, &mut st, 5, Step::Open, &[], &mut [0; RTX_LEN]),
            Err(EIO)
        );
        close(&t, &mut st, SLOT);
        assert_eq!(t.sent().len(), n);
        assert!(!st.sessions[SLOT as usize].is_open());
        Ok(())
    }

    #[test]
    fn the_locality_check_may_be_retried_a_bounded_number_of_times() {
        use Command::*;
        let mut s = Session::new();
        for c in [Open, Prepare, Start, GetRtx, PutCert, PutRrx] {
            let arg = match c {
                Open => OPEN_ARG,
                Prepare => PREPARE_ARG,
                _ => 0,
            };
            assert!(s.record(c, arg, Outcome::Done(spec_of(c).output)));
        }
        assert!(s.record(QueryStoredKm, 0, Outcome::Done(32)));
        for c in [PutHContext, PutHPrime] {
            assert!(s.allows(c, 0));
            assert!(s.record(c, 0, Outcome::Done(0)));
        }
        // Refused L' allows retrying GetRn while retries remain.
        assert!(s.allows(GetRn, 0));
        assert!(s.record(GetRn, 0, Outcome::Done(8)));
        assert!(s.allows(LcStart, 0));
        assert!(s.record(LcStart, 0, Outcome::Done(0)));
        assert!(s.allows(PutLPrime, 0));
        assert!(!s.record(PutLPrime, 0, Outcome::Refused(1)));
        assert!(!s.allows(GetEks, 0));
        for _ in 1..LC_MAX_TRIES {
            assert!(s.allows(GetRn, 0));
            assert!(s.record(GetRn, 0, Outcome::Done(8)));
        }
        assert!(!s.allows(GetRn, 0));
        assert!(s.allows(LcStart, 0));
        assert!(s.record(LcStart, 0, Outcome::Done(0)));
        // Refused L' after the last attempt fails the session.
        assert!(!s.record(PutLPrime, 0, Outcome::Refused(1)));
        assert!(!s.allows(GetRn, 0));
        assert!(!s.allows(GetEks, 0));
        assert!(s.allows(Close, 0));
    }

    #[test]
    fn the_table_is_closed_and_fits_the_buffers() {
        for (i, a) in SPECS.iter().enumerate() {
            assert!(a.input_max <= OOL_SIZE_HDCP && a.output <= u8::MAX as usize);
            for b in SPECS[i + 1..].iter() {
                assert!(a.id != b.id && a.command != b.command);
            }
        }
    }
}
