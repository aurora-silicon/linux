// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The accessory client of the trusted-accessory service: a Magic Keyboard
//! with Touch ID on one side (through `hid-apple-ta`, see
//! `include/linux/hid-apple-ta.h` and `stac_ta_shim.c`), the enclave's `stac`
//! service on the other, and the lifecycle (see `stac_lifecycle.rs`) deciding
//! what runs next.
//!
//! When a keyboard's channel comes up, or the service comes back online with
//! one attached, the client runs the accessory handshake (see
//! [`stac_ta_handshake`]) to learn the keyboard's identity and connection
//! nonce, then asks the enclave whether that accessory is paired and
//! authorised (0x02); for an unpaired one it also logs a fingerprint of the
//! enclave's host attestation object (0x01). The handshake writes only the two
//! protocol-handshake reports a keyboard needs before it will answer its
//! identity (so a keyboard powered on cold, which comes up idle, is brought to
//! where it identifies); it changes no pairing or enclave state. Whatever the
//! lifecycle wants next (a per-connection session, a connection state) is
//! logged as held and not run.
//!
//! A pairing runs only when root writes 1 to the module's `ta_pair`
//! parameter, and only for a keyboard the enclave reports unpaired. It relays
//! between the keyboard (see `stac_ta_wire.rs`) and the enclave (0x01, 0x03,
//! 0x15, 0x05) in the order a macOS pairing uses, and stops at the first
//! answer that does not check out. The host certificate chain the keyboard
//! wants comes from the firmware file [`HOST_CHAIN_FW`]: the Mac's own
//! Apple-issued device attestation chain, which certifies the enclave's host
//! object. It is checked against the enclave's object before anything is
//! written to the keyboard.
//!
//! A per-connection session runs only when root writes 1 to the module's
//! `ta_session` parameter, and only for a keyboard the enclave reports paired
//! whose connection is new to the enclave. It confirms the pairing, hands the
//! enclave the manifest as on a reconnection and relays the two session halves
//! (0x06, 0x07) with the keyboard's session ephemeral point and material in
//! between (see `stac_ta_wire.rs`). Once on, the connection-state (0x09) and
//! detach-finalise (0x08) steps the lifecycle asks for run too; without it they
//! stay held, as before. The pairing path (`ta_pair`) is untouched.
//!
//! The identity, the nonce and anything else the keyboard answers are never
//! logged.

use kernel::{
    prelude::*,
    sync::{atomic::Relaxed, Arc},
};

use crate::stac_lifecycle::{Action, Event, Nonce, State, NONCE_LEN};
use crate::stac_ops::{self, AccessoryId, PublicPoint, ACCESSORY_ID_LEN};
use crate::stac_ta_wire::{self as wire, WireError};
use crate::SepData;

type AttachFn = unsafe extern "C" fn(*mut c_void);
type IdentifyFn = unsafe extern "C" fn(*mut c_void);
type PairFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type SessionFn = unsafe extern "C" fn(*mut c_void) -> c_int;

/// The Mac's host certificate chain for keyboard pairing (two DER
/// certificates), installed per machine with the other Apple data.
pub(crate) const HOST_CHAIN_FW: &CStr = c"apple/stac-host-chain.der";

/// How long the keyboard gets for a reply: its material takes about 3.5 s.
const REPLY_MS: u32 = 2000;
const MATERIAL_MS: u32 = 15000;

extern "C" {
    fn sep_stac_ta_register(
        ctx: *mut c_void,
        attached: AttachFn,
        detached: AttachFn,
        identify: IdentifyFn,
        pair: PairFn,
        session: SessionFn,
    ) -> c_int;
    fn sep_stac_ta_unregister();
    fn sep_stac_ta_identify();
    fn sep_stac_ta_command(
        cmd: *const u8,
        len: usize,
        reply_id: u8,
        reply: *mut u8,
        cap: usize,
        timeout_ms: c_uint,
    ) -> isize;
    fn sep_stac_ta_read(id: u8, buf: *mut u8, len: usize) -> isize;
}

// The lengths the shim reads into.
static_assert!(ACCESSORY_ID_LEN == 16);
static_assert!(NONCE_LEN == 16);

/// What the enclave said about the keyboard, for the log.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Query {
    State(stac_ops::PairingState),
    Refused(u32),
    Malformed,
    NoReply,
}

/// The event the answer to a pairing-state query feeds the lifecycle.
pub(crate) fn query_event(q: Query) -> Event {
    match q {
        Query::State(s) => Event::PairingState {
            paired: s.paired,
            authorized: s.authorized,
        },
        _ => Event::Failed,
    }
}

/// Whether the read-only client runs `action`. Only reads of the keyboard and
/// the enclave's pairing-state query do; everything that writes to the
/// keyboard or changes the enclave's state is held.
pub(crate) fn runs_read_only(action: &Action) -> bool {
    matches!(action, Action::Identify | Action::QueryPairing(_))
}

/// Whether the session relay runs `action` once the `ta_session` parameter has
/// enabled it: the per-connection session and the connection-state and
/// detach-finalise operations it leads to. A pairing (`NeedsPairing`) still
/// only runs through `ta_pair`; the timers and readiness are the caller's.
fn runs_with_session(action: &Action) -> bool {
    matches!(
        action,
        Action::BeginSession(_) | Action::ConnectionState(..) | Action::FinalizeDetach(_)
    )
}

/// The registration with `hid-apple-ta`; holds the context alive until the
/// shim has dropped it.
pub(crate) struct Client {
    sep: Arc<SepData>,
    registered: bool,
}

impl Client {
    pub(crate) fn register(sep: Arc<SepData>) -> Self {
        // SAFETY: the owned Arc pins the context until unregister, which
        // detaches every keyboard and waits for the shim's work first.
        let ret = unsafe {
            sep_stac_ta_register(
                Arc::as_ptr(&sep).cast_mut().cast(),
                attached,
                detached,
                identify,
                pair,
                session,
            )
        };
        if ret != 0 {
            dev_dbg!(sep.dev, "stac: no keyboard client ({})\n", ret);
        }
        Self {
            sep,
            registered: ret == 0,
        }
    }

    pub(crate) fn unregister(&mut self) {
        if self.registered {
            // SAFETY: matched to the successful register and called once.
            unsafe { sep_stac_ta_unregister() };
            self.registered = false;
        }
        let _ = &self.sep;
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.unregister();
    }
}

/// # Safety
/// The shim calls these with the context pinned by [`Client`], in process
/// context, never after unregister has returned.
unsafe extern "C" fn attached(ctx: *mut c_void) {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    sep.stac_ta_event(Event::Attached);
}

/// # Safety
/// As for [`attached`].
unsafe extern "C" fn detached(ctx: *mut c_void) {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    sep.stac_ta_event(Event::Detached);
}

/// # Safety
/// As for [`attached`]; called on the shim's identify work with the client
/// lock held and a keyboard attached, so the keyboard cannot go away mid-way.
unsafe extern "C" fn identify(ctx: *mut c_void) {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    sep.stac_ta_identify_run();
}

/// # Safety
/// As for [`attached`]; called with the shim's client lock held and a
/// keyboard attached, from the writer of the `ta_pair` parameter.
unsafe extern "C" fn pair(ctx: *mut c_void) -> c_int {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    match sep.stac_ta_pair() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// # Safety
/// As for [`pair`]; called with the shim's client lock held and a keyboard
/// attached, from the writer of the `ta_session` parameter.
unsafe extern "C" fn session(ctx: *mut c_void) -> c_int {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    match sep.stac_ta_session() {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    }
}

/// Where a pairing stopped, for the log (the fields are read through
/// `Debug`).
#[derive(Clone, Copy, Debug)]
#[allow(dead_code)]
enum PairStop {
    /// No host certificate chain installed for this Mac.
    NoChain,
    /// The installed chain does not certify this enclave's host object.
    WrongChain,
    /// The enclave did not give its host object.
    HostObject,
    /// The keyboard's tag or version is not the one this relay speaks.
    Version,
    /// The keyboard did not answer, or its answer did not check out.
    Keyboard(u32, WireError),
    /// The keyboard could not be written or read.
    Transport(u32, i32),
    /// The keyboard already holds a pairing with this host the enclave lacks.
    KeyboardPaired,
    /// The enclave refused or did not answer an operation.
    Enclave(u16),
}

/// Where a per-connection session stopped, for the log (the fields are read
/// through `Debug`).
#[derive(Clone, Copy, Debug)]
#[allow(dead_code)]
enum SessionStop {
    /// The keyboard's tag or version is not the one this relay speaks.
    Version,
    /// The keyboard did not answer, or its answer did not check out.
    Keyboard(u32, WireError),
    /// The keyboard could not be written or read.
    Transport(u32, i32),
    /// The enclave reports the keyboard unpaired, or the keyboard's
    /// pairing-state check does not agree: there is no session to open.
    NotPaired,
    /// The enclave refused or did not answer an operation.
    Enclave(u16),
}

/// Where the accessory handshake stopped. Steps 0 and 1 mirror the pairing's:
/// the tag, handoff and identity are step 0; the identity point and nonce are
/// step 1. The pairing and session relays fold these into their own stops.
#[derive(Clone, Copy, Debug)]
#[allow(dead_code)]
enum HandshakeStop {
    /// The keyboard's tag or version is not the one this relay speaks.
    Version,
    /// The keyboard did not answer, or its answer did not check out.
    Keyboard(u32, WireError),
    /// The keyboard could not be written or read.
    Transport(u32, i32),
}

impl From<HandshakeStop> for PairStop {
    fn from(h: HandshakeStop) -> Self {
        match h {
            HandshakeStop::Version => PairStop::Version,
            HandshakeStop::Keyboard(step, e) => PairStop::Keyboard(step, e),
            HandshakeStop::Transport(step, e) => PairStop::Transport(step, e),
        }
    }
}

impl From<HandshakeStop> for SessionStop {
    fn from(h: HandshakeStop) -> Self {
        match h {
            HandshakeStop::Version => SessionStop::Version,
            HandshakeStop::Keyboard(step, e) => SessionStop::Keyboard(step, e),
            HandshakeStop::Transport(step, e) => SessionStop::Transport(step, e),
        }
    }
}

/// One keyboard exchange: a command and, if `reply_id` is not 0, its reply.
fn ta_command(cmd: &[u8], reply_id: u8, cap: usize, timeout_ms: u32) -> core::result::Result<KVec<u8>, i32> {
    let mut reply = KVec::new();
    reply.resize(cap, 0, GFP_KERNEL).map_err(|_| -12)?;
    // SAFETY: both buffers are live for the call; the shim stops writing to
    // `reply` before it returns.
    let n = unsafe {
        sep_stac_ta_command(cmd.as_ptr(), cmd.len(), reply_id, reply.as_mut_ptr(), cap, timeout_ms)
    };
    if n < 0 {
        return Err(n as i32);
    }
    reply.truncate(n as usize);
    Ok(reply)
}

/// One feature-report read of at most `len` bytes, ID first.
fn ta_read(id: u8, len: usize) -> core::result::Result<KVec<u8>, i32> {
    let mut buf = KVec::new();
    buf.resize(len, 0, GFP_KERNEL).map_err(|_| -12)?;
    // SAFETY: `buf` is live and `len` bytes long for the call.
    let n = unsafe { sep_stac_ta_read(id, buf.as_mut_ptr(), len) };
    if n < 0 {
        return Err(n as i32);
    }
    buf.truncate(n as usize);
    Ok(buf)
}

/// The accessory channel the handshake and the pairing steps drive: one
/// feature-report read (ID first), and one feature-report command that, when
/// `reply_id` is not 0, awaits the keyboard's reply. The live channel talks to
/// the keyboard through the shim; the tests record the order and feed canned
/// replies, so the handshake order is checked without hardware.
trait TaChannel {
    fn read(&mut self, id: u8, len: usize) -> core::result::Result<KVec<u8>, i32>;
    fn command(
        &mut self,
        cmd: &[u8],
        reply_id: u8,
        cap: usize,
        timeout_ms: u32,
    ) -> core::result::Result<KVec<u8>, i32>;
}

/// The live channel: the shim's feature-report read and command, run with the
/// client lock held.
struct ShimChannel;

impl TaChannel for ShimChannel {
    fn read(&mut self, id: u8, len: usize) -> core::result::Result<KVec<u8>, i32> {
        ta_read(id, len)
    }
    fn command(
        &mut self,
        cmd: &[u8],
        reply_id: u8,
        cap: usize,
        timeout_ms: u32,
    ) -> core::result::Result<KVec<u8>, i32> {
        ta_command(cmd, reply_id, cap, timeout_ms)
    }
}

/// The accessory handshake every attach runs before the host knows whether to
/// pair, in order:
///
///   1. GET the protocol tag and version (`get::TAG`), which must be the ones
///      this relay speaks;
///   2. SET the protocol handoff ([`wire::version_report`], `cmd::VERSION`):
///      echo the tag back to select the protocol;
///   3. GET the accessory identity (`get::UID`);
///   4. SET the identity-point request (`cmd::IDENTITY`, step 1), answered by
///      the keyboard's identity point (`reply::IDENTITY`);
///   5. GET the fresh connection nonce (`get::NONCE`).
///
/// The two SETs are the only writes and carry no pairing or enclave state. A
/// keyboard the other OS already handed off answers the reads without them, but
/// one powered on cold comes up idle and stalls the identity read until the
/// handoff, so the handshake always issues them. Returns the identity, its
/// point and the nonce.
fn stac_ta_handshake<C: TaChannel>(
    ch: &mut C,
) -> core::result::Result<(AccessoryId, PublicPoint, Nonce), HandshakeStop> {
    let kb = |step: u32, e: WireError| HandshakeStop::Keyboard(step, e);
    let io = |step: u32, e: i32| HandshakeStop::Transport(step, e);

    // 1. The protocol tag and version.
    let tag = ch.read(wire::get::TAG, 1 + wire::VERSION_BODY.len()).map_err(|e| io(0, e))?;
    if tag.len() != 1 + wire::VERSION_BODY.len() || tag[1..] != wire::VERSION_BODY[..] {
        return Err(HandshakeStop::Version);
    }
    // 2. Echo it back as the protocol handoff.
    ch.command(&wire::version_report(), 0, 0, 0).map_err(|e| io(0, e))?;
    // 3. The accessory identity.
    let uid = ch.read(wire::get::UID, 1 + ACCESSORY_ID_LEN).map_err(|e| io(0, e))?;
    if uid.len() != 1 + ACCESSORY_ID_LEN {
        return Err(kb(0, WireError::Length));
    }
    let mut id = AccessoryId([0; ACCESSORY_ID_LEN]);
    id.0.copy_from_slice(&uid[1..]);
    // 4. The identity point.
    let c = wire::command(wire::cmd::IDENTITY, 1, &[]).map_err(|_| io(1, -12))?;
    let r = ch
        .command(&c, wire::reply::IDENTITY, wire::POINT_REPLY_LEN, REPLY_MS)
        .map_err(|e| io(1, e))?;
    let point = wire::parse_point_reply(&r, wire::reply::IDENTITY, 1).map_err(|e| kb(1, e))?;
    // 5. The fresh connection nonce.
    let nr = ch.read(wire::get::NONCE, 1 + NONCE_LEN).map_err(|e| io(1, e))?;
    if nr.len() != 1 + NONCE_LEN {
        return Err(kb(1, WireError::Length));
    }
    let mut nonce = Nonce([0; NONCE_LEN]);
    nonce.0.copy_from_slice(&nr[1..]);
    Ok((id, point, nonce))
}

/// Sends one logical report to the keyboard (`report[0]` its logical ID, the
/// rest its body), fragmenting it over the `0xfe` transport when the body is
/// too large for a direct feature SET. A short body goes direct, as before.
///
/// The frames of one logical report are streamed back to back as feature
/// SETs: the keyboard reassembles them and answers only after the last frame,
/// with the single logical reply that covers the whole object. This keyboard
/// sends nothing between frames — the macOS relay's between-frame feature
/// request (see [`wire::parse_fragment_notification`]) is a lower-transport
/// path this USB keyboard does not use, and waiting for it here deadlocked the
/// host against a keyboard already waiting for the next frame. If `reply_id`
/// is not 0 the reply is awaited after the last frame and returned; otherwise
/// the report is fire-and-forget and an empty vector is returned.
fn ta_set_logical(
    report: &[u8],
    reply_id: u8,
    cap: usize,
    timeout_ms: u32,
) -> core::result::Result<KVec<u8>, i32> {
    if report.is_empty() {
        return Err(-22);
    }
    let body = &report[1..];
    if body.len() <= wire::XFE_DIRECT_MAX {
        return ta_command(report, reply_id, cap, timeout_ms);
    }
    let logical_id = report[0];
    let n = wire::xfe_frame_count(body.len());
    for k in 0..n {
        let frame = wire::xfe_frame(logical_id, body, k).map_err(|_| -22i32)?;
        if k + 1 == n {
            // The last frame completes the logical report: its reply, if any,
            // covers the whole object and follows this frame.
            return ta_command(&frame, reply_id, cap, timeout_ms);
        }
        // Earlier frames are streamed with no reply awaited; the keyboard
        // answers only after the last one.
        ta_command(&frame, 0, 0, 0)?;
    }
    Ok(KVec::new())
}

impl SepData {
    /// The accessory handshake, run on the shim's identify work (so the client
    /// lock is held): bring the keyboard to where it answers its identity, read
    /// its identity and nonce, then feed the lifecycle the result. Only the two
    /// protocol-handshake reports are written; no pairing or enclave state
    /// changes. A keyboard that will not answer feeds `Failed`, as the plain
    /// reads did before.
    fn stac_ta_identify_run(&self) {
        match stac_ta_handshake(&mut ShimChannel) {
            Ok((id, _point, nonce)) => self.stac_ta_event(Event::Identified(id, nonce)),
            Err(stop) => {
                match stop {
                    HandshakeStop::Transport(_, e) => dev_info_ratelimited!(
                        self.dev,
                        "stac: the keyboard did not identify itself ({})\n",
                        e
                    ),
                    other => dev_info_ratelimited!(
                        self.dev,
                        "stac: the keyboard did not identify itself ({:?})\n",
                        other
                    ),
                }
                self.stac_ta_event(Event::Failed);
            }
        }
    }

    /// Pairs the attached keyboard with the enclave. Runs only from the
    /// `ta_pair` trigger, for a keyboard the enclave reports unpaired.
    fn stac_ta_pair(&self) -> Result<()> {
        if self.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        let state = self.stac_lc.lock().state();
        if state != State::Unpaired {
            dev_info!(self.dev, "stac: pairing not started: keyboard {:?}, not unpaired\n", state);
            return Err(EBUSY);
        }
        dev_info!(self.dev, "stac: pairing the keyboard\n");
        match self.stac_ta_pair_steps() {
            Ok(id) => {
                let q = self.stac_query_pairing(&id);
                match q {
                    Query::State(s) => dev_info!(
                        self.dev,
                        "stac: pairing done; the enclave reports paired={} authorized={}\n",
                        s.paired,
                        s.authorized
                    ),
                    other => dev_info!(self.dev, "stac: pairing done; state query failed ({:?})\n", other),
                }
                let paired = matches!(q, Query::State(s) if s.paired);
                if paired {
                    self.stac_ta_event(Event::Paired);
                    Ok(())
                } else {
                    Err(EPROTO)
                }
            }
            Err(stop) => {
                dev_info!(self.dev, "stac: pairing stopped: {:?}\n", stop);
                Err(match stop {
                    PairStop::NoChain => ENOENT,
                    PairStop::WrongChain => EKEYREJECTED,
                    PairStop::Transport(_, e) => Error::from_errno(e),
                    _ => EPROTO,
                })
            }
        }
    }

    fn stac_body(&self, op: stac_ops::Op, req: &[u8], max: usize) -> core::result::Result<KVec<u8>, PairStop> {
        match self.stac_call(op.opcode(), req, max) {
            Ok(crate::stac::Reply::Body(b)) => Ok(b),
            _ => Err(PairStop::Enclave(op.opcode())),
        }
    }

    /// The pairing exchange; returns the keyboard's identity when it went
    /// through.
    fn stac_ta_pair_steps(&self) -> core::result::Result<AccessoryId, PairStop> {
        use stac_ops::Op;
        let kb = |step: u32, e: WireError| PairStop::Keyboard(step, e);
        let io = |step: u32, e: i32| PairStop::Transport(step, e);

        // Everything the keyboard will be shown is checked before the first
        // write to it: the chain must certify this enclave's host object.
        let fw = kernel::firmware::Firmware::request_nowarn(HOST_CHAIN_FW, &self.dev)
            .map_err(|_| PairStop::NoChain)?;
        let host = self.stac_body(Op::HostAttestationObject, &[], stac_ops::HOST_OBJECT_REPLY_LEN)?;
        let len = stac_ops::parse_host_object_len(&host).map_err(|_| PairStop::HostObject)?;
        let digest = crate::image::sha256(&host[stac_ops::WORD..stac_ops::WORD + len])
            .map_err(|_| PairStop::HostObject)?;
        wire::check_chain(fw.data(), &digest).map_err(|_| PairStop::WrongChain)?;

        // The accessory handshake: tag, protocol handoff, identity, identity
        // point, nonce (see [`stac_ta_handshake`]). Its stops fold into this
        // relay's through `From`. The nonce is unused here; the pairing binds
        // to the enclave's object, not this read.
        let (id, identity, _nonce) = stac_ta_handshake(&mut ShimChannel)?;

        // Step 2: the keyboard checks the host object. Its 137-byte body is
        // too large for a direct SET on USB, so it goes over the 0xfe
        // transport; a fresh pairing uses that form, not a direct SET 0x60
        // (which is the already-paired session's).
        let c = wire::command(wire::cmd::PAIRING_CHECK, 2, &host).map_err(|_| io(2, -12))?;
        let r = ta_set_logical(&c, wire::reply::PAIRING_CHECK, wire::PAIRING_CHECK_REPLY_LEN, REPLY_MS)
            .map_err(|e| io(2, e))?;
        if wire::parse_pairing_check(&r).map_err(|e| kb(2, e))? {
            return Err(PairStop::KeyboardPaired);
        }

        // Step 3: the keyboard's pairing ephemeral point, then 0x03.
        let c = wire::command(wire::cmd::PAIRING_EPHEMERAL, 3, &1u32.to_le_bytes()).map_err(|_| io(3, -12))?;
        let r = ta_command(&c, wire::reply::PAIRING_EPHEMERAL, wire::POINT_REPLY_LEN, REPLY_MS)
            .map_err(|e| io(3, e))?;
        let eph = wire::parse_point_reply(&r, wire::reply::PAIRING_EPHEMERAL, 3).map_err(|e| kb(3, e))?;
        let begin = self.stac_body(
            Op::PairingBegin,
            &stac_ops::pairing_begin_request(&id, &identity, &eph),
            stac_ops::PAIRING_BEGIN_REPLY_FIXED + stac_ops::ACCESSORY_ATTESTATION_SLOT,
        )?;
        let b = stac_ops::parse_pairing_begin(&begin).map_err(|_| PairStop::Enclave(Op::PairingBegin.opcode()))?;

        // Step 4: the chain; step 5: the host object again.
        let chain = fw.data();
        for i in 0..wire::fragment_count(chain.len()) {
            let f = wire::fragment(4, wire::kind::CHAIN, chain, i).map_err(|_| io(4, -22))?;
            ta_set_logical(&f, 0, 0, 0).map_err(|e| io(4, e))?;
        }
        let c = wire::command(wire::cmd::HOST_OBJECT, 5, &host).map_err(|_| io(5, -12))?;
        let r = ta_set_logical(&c, wire::reply::HOST_OBJECT, wire::REPLY_HEADER_LEN, REPLY_MS).map_err(|e| io(5, e))?;
        wire::check_reply(&r, wire::reply::HOST_OBJECT, 5).map_err(|e| kb(5, e))?;

        // Step 6: the enclave's attestation; step 7: host object and the
        // enclave's point, answered with the keyboard's material.
        let att = &begin[b.host_attestation.clone()];
        for i in 0..wire::fragment_count(att.len()) {
            let f = wire::fragment(6, wire::kind::ATTESTATION, att, i).map_err(|_| io(6, -22))?;
            ta_set_logical(&f, 0, 0, 0).map_err(|e| io(6, e))?;
        }
        let mut p7 = KVec::new();
        p7.extend_from_slice(&host, GFP_KERNEL).map_err(|_| io(7, -12))?;
        p7.extend_from_slice(&begin[b.host_ephemeral.clone()], GFP_KERNEL).map_err(|_| io(7, -12))?;
        let c = wire::command(wire::cmd::PAIRING_FINISH, 7, &p7).map_err(|_| io(7, -12))?;
        let r = ta_set_logical(&c, wire::reply::ACCESSORY_MATERIAL, wire::MATERIAL_REPLY_LEN, MATERIAL_MS)
            .map_err(|e| io(7, e))?;
        let m = wire::parse_material(&r).map_err(|e| kb(7, e))?;
        let finish = stac_ops::pairing_finish_request(&id, &identity, &r[m.attestation], &r[m.certificate])
            .map_err(|_| io(7, -22))?;
        let body = self.stac_body(Op::PairingFinish, &finish, 0)?;
        stac_ops::expect_empty(&body).map_err(|_| PairStop::Enclave(Op::PairingFinish.opcode()))?;

        // The manifest, handed to the enclave as after a pairing.
        let r = ta_read(wire::get::MANIFEST, wire::MANIFEST_READ_LEN).map_err(|e| io(8, e))?;
        let manifest = wire::parse_manifest(&r).map_err(|e| kb(8, e))?;
        let req = stac_ops::manifest_request(&id, true, manifest).map_err(|_| io(8, -22))?;
        let body = self.stac_body(Op::Manifest, &req, 0)?;
        stac_ops::expect_empty(&body).map_err(|_| PairStop::Enclave(Op::Manifest.opcode()))?;
        Ok(id)
    }

    /// Turns the per-connection session on and runs it for the attached
    /// keyboard. Runs only from the `ta_session` trigger, for a keyboard the
    /// enclave reports paired whose connection is new to the enclave. Once on,
    /// the connection-state and detach-finalise steps the lifecycle asks for
    /// run too, through [`stac_ta_run`](Self::stac_ta_run).
    fn stac_ta_session(&self) -> Result<()> {
        if self.shutting_down.load(Relaxed) {
            return Err(ENODEV);
        }
        let state = self.stac_lc.lock().state();
        if state != State::Session {
            dev_info!(
                self.dev,
                "stac: session not started: keyboard {:?}, no session wanted\n",
                state
            );
            return Err(EBUSY);
        }
        // From now on the session, connection-state and detach-finalise steps
        // run rather than being held.
        self.stac_ta_session.store(true, Relaxed);
        dev_info!(self.dev, "stac: running the per-connection session\n");
        match self.stac_ta_session_steps() {
            Ok(()) => {
                // The lifecycle takes it from here (connection state 1).
                self.stac_ta_event(Event::SessionReady);
                Ok(())
            }
            Err(stop) => {
                dev_info!(self.dev, "stac: session stopped: {:?}\n", stop);
                self.stac_ta_event(Event::Failed);
                Err(match stop {
                    SessionStop::Transport(_, e) => Error::from_errno(e),
                    _ => EPROTO,
                })
            }
        }
    }

    fn stac_session_body(
        &self,
        op: stac_ops::Op,
        req: &[u8],
        max: usize,
    ) -> core::result::Result<KVec<u8>, SessionStop> {
        match self.stac_call(op.opcode(), req, max) {
            Ok(crate::stac::Reply::Body(b)) => Ok(b),
            _ => Err(SessionStop::Enclave(op.opcode())),
        }
    }

    /// The session exchange, mirroring a macOS session after the keyboard came
    /// back on its connection: confirm paired, hand the enclave the manifest
    /// with `after_pairing` clear, then the two session halves (0x06, 0x07)
    /// with the keyboard's session ephemeral point and material in between.
    fn stac_ta_session_steps(&self) -> core::result::Result<(), SessionStop> {
        use stac_ops::Op;
        let kb = |step: u32, e: WireError| SessionStop::Keyboard(step, e);
        let io = |step: u32, e: i32| SessionStop::Transport(step, e);

        // The enclave's host object the keyboard checks it against: read it
        // first, as a pairing does, before anything is written to the keyboard.
        let host = self.stac_session_body(Op::HostAttestationObject, &[], stac_ops::HOST_OBJECT_REPLY_LEN)?;
        stac_ops::parse_host_object_len(&host).map_err(|_| SessionStop::Enclave(Op::HostAttestationObject.opcode()))?;

        // The accessory handshake, the same opening the pairing uses, so the
        // keyboard is the one this relay speaks to (see [`stac_ta_handshake`]).
        // Its stops fold into this relay's through `From`.
        let (id, _identity, _nonce) = stac_ta_handshake(&mut ShimChannel)?;

        // Confirm paired, both at the enclave (0x02) and at the keyboard
        // (SET 0x60 -> IN 0x42): a session only opens on a paired keyboard.
        match self.stac_query_pairing(&id) {
            Query::State(s) if s.paired => {}
            _ => return Err(SessionStop::NotPaired),
        }
        let c = wire::command(wire::cmd::PAIRING_CHECK, 2, &host).map_err(|_| io(2, -12))?;
        let r = ta_command(&c, wire::reply::PAIRING_CHECK, wire::PAIRING_CHECK_REPLY_LEN, REPLY_MS)
            .map_err(|e| io(2, e))?;
        if !wire::parse_pairing_check(&r).map_err(|e| kb(2, e))? {
            return Err(SessionStop::NotPaired);
        }

        // The manifest, handed to the enclave as on a reconnection (not a
        // pairing): `after_pairing` clear.
        let r = ta_read(wire::get::MANIFEST, wire::MANIFEST_READ_LEN).map_err(|e| io(2, e))?;
        let manifest = wire::parse_manifest(&r).map_err(|e| kb(2, e))?;
        let req = stac_ops::manifest_request(&id, false, manifest).map_err(|_| io(2, -22))?;
        let body = self.stac_session_body(Op::Manifest, &req, 0)?;
        stac_ops::expect_empty(&body).map_err(|_| SessionStop::Enclave(Op::Manifest.opcode()))?;

        // Session step 3: the keyboard's session ephemeral point, then 0x06.
        let c = wire::command(wire::cmd::SESSION_EPHEMERAL, 3, &4u32.to_le_bytes()).map_err(|_| io(3, -12))?;
        let r = ta_command(&c, wire::reply::SESSION_EPHEMERAL, wire::POINT_REPLY_LEN, REPLY_MS)
            .map_err(|e| io(3, e))?;
        let eph = wire::parse_point_reply(&r, wire::reply::SESSION_EPHEMERAL, 3).map_err(|e| kb(3, e))?;
        let begin = self.stac_session_body(
            Op::SessionBegin,
            &stac_ops::session_begin_request(&id, &eph),
            stac_ops::SESSION_BEGIN_REPLY_FIXED + stac_ops::SESSION_MATERIAL_SLOT,
        )?;
        let b = stac_ops::parse_session_begin(&begin).map_err(|_| SessionStop::Enclave(Op::SessionBegin.opcode()))?;

        // Session step 5: the host session material to the keyboard, answered
        // with the keyboard's session material; then 0x07 finishes.
        let c = wire::command(wire::cmd::SESSION_MATERIAL, 5, &begin[b.host_attestation.clone()])
            .map_err(|_| io(5, -12))?;
        let r = ta_command(&c, wire::reply::SESSION_MATERIAL, wire::SESSION_MATERIAL_REPLY_LEN, MATERIAL_MS)
            .map_err(|e| io(5, e))?;
        let material = wire::parse_session_material(&r).map_err(|e| kb(5, e))?;
        let finish = stac_ops::session_finish_request(&id, &r[material]).map_err(|_| io(5, -22))?;
        let body = self.stac_session_body(Op::SessionFinish, &finish, 0)?;
        stac_ops::expect_empty(&body).map_err(|_| SessionStop::Enclave(Op::SessionFinish.opcode()))?;
        Ok(())
    }

    /// Runs the per-connection session the lifecycle asked for by driving the
    /// `ta_session` relay again for this connection. Returns the event to feed
    /// back: ready on success, failed otherwise.
    fn stac_run_session(&self, _id: &AccessoryId) -> Event {
        match self.stac_ta_session_steps() {
            Ok(()) => Event::SessionReady,
            Err(stop) => {
                dev_info!(self.dev, "stac: session stopped: {:?}\n", stop);
                Event::Failed
            }
        }
    }

    /// Tells the enclave the keyboard's link went up or down (0x09). On an
    /// acknowledged link-up the lifecycle moves on; a link-down answers
    /// nothing.
    fn stac_run_connection_state(&self, id: &AccessoryId, up: bool) -> Option<Event> {
        let req = stac_ops::connection_state_request(id, up);
        match self.stac_call(stac_ops::Op::ConnectionState.opcode(), &req, 0) {
            Ok(crate::stac::Reply::Body(body)) if stac_ops::expect_empty(&body).is_ok() => {
                up.then_some(Event::Connected)
            }
            other => {
                dev_info!(
                    self.dev,
                    "stac: connection state {} not acknowledged ({})\n",
                    u8::from(up),
                    if other.is_ok() { "refused or malformed" } else { "no reply" }
                );
                Some(Event::Failed)
            }
        }
    }

    /// Finalises a detach the grace period ended (0x08). It answers nothing and
    /// feeds no event back.
    fn stac_run_finalize(&self, id: &AccessoryId) {
        let req = stac_ops::finalize_request(id);
        match self.stac_call(stac_ops::Op::Finalize.opcode(), &req, 0) {
            Ok(crate::stac::Reply::Body(body)) if stac_ops::expect_empty(&body).is_ok() => {}
            other => dev_info!(
                self.dev,
                "stac: detach finalise not acknowledged ({})\n",
                if other.is_ok() { "refused or malformed" } else { "no reply" }
            ),
        }
    }

    /// Feeds the lifecycle `first`, runs what it asks for and feeds the
    /// outcomes back until it asks for nothing this client runs.
    pub(crate) fn stac_ta_event(&self, first: Event) {
        let mut next = Some(first);
        while let Some(ev) = next.take() {
            let (actions, state) = {
                let mut lc = self.stac_lc.lock();
                let actions = lc.on(ev);
                (actions, lc.state())
            };
            for action in actions.as_slice() {
                if let Some(ev) = self.stac_ta_run(&action, state) {
                    next = Some(ev);
                }
            }
        }
    }

    fn stac_ta_run(&self, action: &Action, state: State) -> Option<Event> {
        if self.shutting_down.load(Relaxed) {
            return None;
        }
        let session_on = self.stac_ta_session.load(Relaxed);
        if !runs_read_only(action) && !(session_on && runs_with_session(action)) {
            dev_info!(
                self.dev,
                "stac: keyboard {:?}: next step {} held ({})\n",
                state,
                action_name(action),
                if session_on { "read-only client" } else { "ta_session off" }
            );
            return None;
        }
        match action {
            Action::Identify => {
                // SAFETY: no preconditions; queues the shim's read.
                unsafe { sep_stac_ta_identify() };
                None
            }
            Action::BeginSession(id) => Some(self.stac_run_session(id)),
            Action::ConnectionState(id, up) => self.stac_run_connection_state(id, *up),
            Action::FinalizeDetach(id) => {
                self.stac_run_finalize(id);
                None
            }
            Action::QueryPairing(id) => {
                let q = self.stac_query_pairing(id);
                match q {
                    Query::State(s) => dev_info!(
                        self.dev,
                        "stac: keyboard identified; the enclave reports paired={} authorized={}\n",
                        s.paired,
                        s.authorized
                    ),
                    other => dev_info!(
                        self.dev,
                        "stac: keyboard identified; pairing-state query failed ({:?})\n",
                        other
                    ),
                }
                if let Query::State(st) = q {
                    if !st.paired {
                        self.stac_host_object_fingerprint();
                    }
                }
                Some(query_event(q))
            }
            _ => None,
        }
    }

    /// Logs a short fingerprint of the enclave's host attestation object
    /// (0x01): the object a pairing presents to the keyboard, which an
    /// Apple-issued certificate must cover. Comparing it with the object the
    /// other OS on this Mac presents tells whether that certificate can serve
    /// here. Reads only; the object itself is not logged.
    fn stac_host_object_fingerprint(&self) {
        let reply = self.stac_call(
            stac_ops::Op::HostAttestationObject.opcode(),
            &[],
            stac_ops::HOST_OBJECT_REPLY_LEN,
        );
        let body = match reply {
            Ok(crate::stac::Reply::Body(body)) => body,
            other => {
                dev_info!(
                    self.dev,
                    "stac: host attestation object: no usable reply ({})\n",
                    if other.is_ok() { "refused or malformed" } else { "no reply" }
                );
                return;
            }
        };
        let len = match stac_ops::parse_host_object_len(&body) {
            Ok(len) => len,
            Err(_) => {
                dev_info!(self.dev, "stac: host attestation object: malformed\n");
                return;
            }
        };
        match crate::image::sha256(&body[stac_ops::WORD..stac_ops::WORD + len]) {
            Ok(d) => dev_info!(
                self.dev,
                "stac: host attestation object: {} bytes, sha256 {:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}\n",
                len, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]
            ),
            Err(_) => dev_info!(self.dev, "stac: host attestation object: {} bytes (no digest)\n", len),
        }
    }

    fn stac_query_pairing(&self, id: &AccessoryId) -> Query {
        let req = stac_ops::pairing_state_request(id);
        match self.stac_call(
            stac_ops::Op::PairingState.opcode(),
            &req,
            stac_ops::PAIRING_STATE_REPLY_LEN,
        ) {
            Ok(crate::stac::Reply::Body(body)) => match stac_ops::parse_pairing_state(&body) {
                Ok(s) => Query::State(s),
                Err(_) => Query::Malformed,
            },
            Ok(crate::stac::Reply::Refused(status)) => Query::Refused(status),
            Ok(crate::stac::Reply::Malformed) => Query::Malformed,
            Ok(crate::stac::Reply::NoReply) => Query::NoReply,
            Err(_) => Query::NoReply,
        }
    }
}

fn action_name(action: &Action) -> &'static str {
    match action {
        Action::Identify => "identify",
        Action::QueryPairing(_) => "pairing-state query",
        Action::BeginSession(_) => "per-connection session",
        Action::ConnectionState(_, true) => "connection state 1",
        Action::ConnectionState(_, false) => "connection state 0",
        Action::StartGrace => "detach grace",
        Action::CancelGrace => "cancel detach grace",
        Action::FinalizeDetach(_) => "finalise detach",
        Action::NeedsPairing(_) => "pairing",
        Action::BiometricReady(_, _) => "biometric readiness",
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_stac_client)]
mod tests {
    use super::*;
    use crate::stac_lifecycle::Lifecycle;

    const ID: AccessoryId = AccessoryId([7; ACCESSORY_ID_LEN]);
    const NONCE: Nonce = Nonce([9; NONCE_LEN]);

    /// One exchange the recording channel logged: a read of a report, or a
    /// command of a report awaiting the given reply ID (0 for none).
    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    enum Op {
        Read(u8),
        Command(u8, u8),
    }

    /// A [`TaChannel`] that records the order of exchanges and answers each
    /// with a canned reply, so the handshake order is checked without a
    /// keyboard. The identity reads back as all 7s, the nonce as all 9s.
    struct Recorder {
        ops: KVec<Op>,
        good_tag: bool,
    }

    impl Recorder {
        fn new() -> Self {
            Recorder {
                ops: KVec::new(),
                good_tag: true,
            }
        }
    }

    impl TaChannel for Recorder {
        fn read(&mut self, id: u8, _len: usize) -> core::result::Result<KVec<u8>, i32> {
            self.ops.push(Op::Read(id), GFP_KERNEL).unwrap();
            let mut v = KVec::new();
            v.push(id, GFP_KERNEL).unwrap();
            match id {
                wire::get::TAG => {
                    let mut body = wire::VERSION_BODY;
                    if !self.good_tag {
                        body[0] ^= 0xff; // no longer the tag this relay speaks
                    }
                    v.extend_from_slice(&body, GFP_KERNEL).unwrap();
                }
                wire::get::UID => v.resize(1 + ACCESSORY_ID_LEN, 7, GFP_KERNEL).unwrap(),
                wire::get::NONCE => v.resize(1 + NONCE_LEN, 9, GFP_KERNEL).unwrap(),
                _ => {}
            }
            Ok(v)
        }

        fn command(
            &mut self,
            cmd: &[u8],
            reply_id: u8,
            _cap: usize,
            _timeout_ms: u32,
        ) -> core::result::Result<KVec<u8>, i32> {
            self.ops.push(Op::Command(cmd[0], reply_id), GFP_KERNEL).unwrap();
            if reply_id == 0 {
                return Ok(KVec::new());
            }
            // The identity point reply: [id][u32 step 1][u32 status 0][point],
            // the point led by the uncompressed-point byte.
            let mut r = KVec::new();
            r.push(reply_id, GFP_KERNEL).unwrap();
            r.extend_from_slice(&1u32.to_le_bytes(), GFP_KERNEL).unwrap();
            r.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL).unwrap();
            r.push(0x04, GFP_KERNEL).unwrap();
            r.resize(wire::POINT_REPLY_LEN, 0x11, GFP_KERNEL).unwrap();
            Ok(r)
        }
    }

    // The attach handshake runs exactly the macOS order: GET 0x4b, SET 0x68,
    // GET 0x41, SET 0x67 answered by IN 0x40, GET 0x4c — and returns the
    // identity, its point and the nonce the reads carried.
    #[test]
    fn the_attach_handshake_follows_the_macos_order() {
        let mut ch = Recorder::new();
        let (id, point, nonce) = stac_ta_handshake(&mut ch).unwrap();
        assert_eq!(
            &ch.ops[..],
            &[
                Op::Read(wire::get::TAG),                             // GET 0x4b
                Op::Command(wire::cmd::VERSION, 0),                   // SET 0x68
                Op::Read(wire::get::UID),                            // GET 0x41
                Op::Command(wire::cmd::IDENTITY, wire::reply::IDENTITY), // SET 0x67 / IN 0x40
                Op::Read(wire::get::NONCE),                          // GET 0x4c
            ]
        );
        assert_eq!(id.0, [7; ACCESSORY_ID_LEN]);
        assert_eq!(nonce.0, [9; NONCE_LEN]);
        assert_eq!(point.0[0], 0x04);
    }

    // A keyboard whose protocol tag is not the one this relay speaks stops at
    // the first read, before the handoff or any other write.
    #[test]
    fn the_handshake_refuses_a_foreign_tag_before_writing() {
        let mut ch = Recorder::new();
        ch.good_tag = false;
        assert!(matches!(
            stac_ta_handshake(&mut ch),
            Err(HandshakeStop::Version)
        ));
        assert_eq!(&ch.ops[..], &[Op::Read(wire::get::TAG)]);
    }

    #[test]
    fn only_reads_run() {
        assert!(runs_read_only(&Action::Identify));
        assert!(runs_read_only(&Action::QueryPairing(ID)));
        for held in [
            Action::BeginSession(ID),
            Action::ConnectionState(ID, true),
            Action::ConnectionState(ID, false),
            Action::StartGrace,
            Action::CancelGrace,
            Action::FinalizeDetach(ID),
            Action::NeedsPairing(ID),
            Action::BiometricReady(ID, true),
        ] {
            assert!(!runs_read_only(&held));
        }
    }

    // With ta_session on, the session relay also runs the per-connection
    // session and the connection-state and detach-finalise steps it leads to,
    // but never a pairing, the grace timers or the readiness notice.
    #[test]
    fn the_session_relay_runs_only_its_own_steps() {
        for run in [
            Action::BeginSession(ID),
            Action::ConnectionState(ID, true),
            Action::ConnectionState(ID, false),
            Action::FinalizeDetach(ID),
        ] {
            assert!(runs_with_session(&run));
            // A read is a read either way; the session steps are not reads.
            assert!(!runs_read_only(&run));
        }
        for held in [
            Action::Identify,
            Action::QueryPairing(ID),
            Action::StartGrace,
            Action::CancelGrace,
            Action::NeedsPairing(ID),
            Action::BiometricReady(ID, true),
        ] {
            assert!(!runs_with_session(&held));
        }
    }

    #[test]
    fn a_failed_query_fails_closed() {
        assert_eq!(query_event(Query::NoReply), Event::Failed);
        assert_eq!(query_event(Query::Malformed), Event::Failed);
        assert_eq!(query_event(Query::Refused(1)), Event::Failed);
        let s = stac_ops::PairingState {
            paired: true,
            authorized: false,
        };
        assert_eq!(
            query_event(Query::State(s)),
            Event::PairingState {
                paired: true,
                authorized: false
            }
        );
    }

    // The read-only walk of a keyboard that is attached before the service
    // comes online: identify, query, then a held step.
    #[test]
    fn the_read_only_walk_stops_at_the_first_write() {
        let mut lc = Lifecycle::new();
        assert_eq!(lc.on(Event::Attached).len(), 0);
        let a = lc.on(Event::ServiceOnline);
        assert!(a.as_slice().all(|a| runs_read_only(&a)));
        let a = lc.on(Event::Identified(ID, NONCE));
        assert!(a.as_slice().all(|a| a == Action::QueryPairing(ID)));
        // Paired, no session for this connection yet: a session is next.
        let a = lc.on(query_event(Query::State(stac_ops::PairingState {
            paired: true,
            authorized: true,
        })));
        assert!(a.as_slice().all(|a| !runs_read_only(&a)));
        assert!(!lc.ready());
        // Unpaired: a pairing is next, also held.
        let mut lc = Lifecycle::new();
        lc.on(Event::ServiceOnline);
        lc.on(Event::Attached);
        lc.on(Event::Identified(ID, NONCE));
        let a = lc.on(query_event(Query::State(stac_ops::PairingState {
            paired: false,
            authorized: false,
        })));
        assert!(a.as_slice().all(|a| a == Action::NeedsPairing(ID)));
        assert!(!lc.ready());
    }
}
