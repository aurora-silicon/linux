// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The accessory client of the trusted-accessory service: a Magic Keyboard
//! with Touch ID on one side (through `hid-apple-ta`, see
//! `include/linux/hid-apple-ta.h` and `stac_ta_shim.c`), the enclave's `stac`
//! service on the other, and the lifecycle (see `stac_lifecycle.rs`) deciding
//! what runs next.
//!
//! When a keyboard's channel comes up, or the service comes back online with
//! one attached, the shim reads the keyboard's protocol tag, identity and
//! connection nonce with three feature-report reads, and the client asks the
//! enclave whether that accessory is paired and authorised (0x02); for an
//! unpaired one it also logs a fingerprint of the enclave's host attestation
//! object (0x01). Nothing is written to the keyboard on its own. Whatever the
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
//! The identity, the nonce and anything else the keyboard answers are never
//! logged.

use kernel::{
    prelude::*,
    sync::{atomic::Relaxed, Arc},
};

use crate::stac_lifecycle::{Action, Event, Nonce, State, NONCE_LEN};
use crate::stac_ops::{self, AccessoryId, ACCESSORY_ID_LEN};
use crate::stac_ta_wire::{self as wire, WireError};
use crate::SepData;

type AttachFn = unsafe extern "C" fn(*mut c_void);
type IdentifiedFn = unsafe extern "C" fn(*mut c_void, *const u8, *const u8);
type FailedFn = unsafe extern "C" fn(*mut c_void, c_int);
type PairFn = unsafe extern "C" fn(*mut c_void) -> c_int;

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
        identified: IdentifiedFn,
        failed: FailedFn,
        pair: PairFn,
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
                identified,
                failed,
                pair,
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
/// As for [`attached`]; `uid` and `nonce` point to 16 bytes each.
unsafe extern "C" fn identified(ctx: *mut c_void, uid: *const u8, nonce: *const u8) {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    let mut id = AccessoryId([0; ACCESSORY_ID_LEN]);
    let mut n = Nonce([0; NONCE_LEN]);
    // SAFETY: the shim passes two 16-byte buffers it owns for the call.
    unsafe {
        id.0.copy_from_slice(core::slice::from_raw_parts(uid, ACCESSORY_ID_LEN));
        n.0.copy_from_slice(core::slice::from_raw_parts(nonce, NONCE_LEN));
    }
    sep.stac_ta_event(Event::Identified(id, n));
}

/// # Safety
/// As for [`attached`].
unsafe extern "C" fn failed(ctx: *mut c_void, err: c_int) {
    // SAFETY: guaranteed by the registration.
    let sep = unsafe { &*ctx.cast::<SepData>() };
    dev_info_ratelimited!(
        sep.dev,
        "stac: the keyboard did not identify itself ({})\n",
        err
    );
    sep.stac_ta_event(Event::Failed);
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

impl SepData {
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

        // Tag, version echo, identity, identity point, nonce.
        let tag = ta_read(0x4b, 9).map_err(|e| io(0, e))?;
        if tag.len() != 9 || tag[1..] != wire::VERSION_BODY[..] {
            return Err(PairStop::Version);
        }
        ta_command(&wire::version_report(), 0, 0, 0).map_err(|e| io(0, e))?;
        let uid = ta_read(0x41, 1 + ACCESSORY_ID_LEN).map_err(|e| io(0, e))?;
        if uid.len() != 1 + ACCESSORY_ID_LEN {
            return Err(kb(0, WireError::Length));
        }
        let mut id = AccessoryId([0; ACCESSORY_ID_LEN]);
        id.0.copy_from_slice(&uid[1..]);
        let c = wire::command(wire::cmd::IDENTITY, 1, &[]).map_err(|_| io(1, -12))?;
        let r = ta_command(&c, wire::reply::IDENTITY, wire::POINT_REPLY_LEN, REPLY_MS).map_err(|e| io(1, e))?;
        let identity = wire::parse_point_reply(&r, wire::reply::IDENTITY, 1).map_err(|e| kb(1, e))?;
        ta_read(0x4c, 1 + NONCE_LEN).map_err(|e| io(1, e))?;

        // Step 2: the keyboard checks the host object.
        let c = wire::command(wire::cmd::PAIRING_CHECK, 2, &host).map_err(|_| io(2, -12))?;
        let r = ta_command(&c, wire::reply::PAIRING_CHECK, wire::PAIRING_CHECK_REPLY_LEN, REPLY_MS)
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
            ta_command(&f, 0, 0, 0).map_err(|e| io(4, e))?;
        }
        let c = wire::command(wire::cmd::HOST_OBJECT, 5, &host).map_err(|_| io(5, -12))?;
        let r = ta_command(&c, wire::reply::HOST_OBJECT, wire::REPLY_HEADER_LEN, REPLY_MS).map_err(|e| io(5, e))?;
        wire::check_reply(&r, wire::reply::HOST_OBJECT, 5).map_err(|e| kb(5, e))?;

        // Step 6: the enclave's attestation; step 7: host object and the
        // enclave's point, answered with the keyboard's material.
        let att = &begin[b.host_attestation.clone()];
        for i in 0..wire::fragment_count(att.len()) {
            let f = wire::fragment(6, wire::kind::ATTESTATION, att, i).map_err(|_| io(6, -22))?;
            ta_command(&f, 0, 0, 0).map_err(|e| io(6, e))?;
        }
        let mut p7 = KVec::new();
        p7.extend_from_slice(&host, GFP_KERNEL).map_err(|_| io(7, -12))?;
        p7.extend_from_slice(&begin[b.host_ephemeral.clone()], GFP_KERNEL).map_err(|_| io(7, -12))?;
        let c = wire::command(wire::cmd::PAIRING_FINISH, 7, &p7).map_err(|_| io(7, -12))?;
        let r = ta_command(&c, wire::reply::ACCESSORY_MATERIAL, wire::MATERIAL_REPLY_LEN, MATERIAL_MS)
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
        if !runs_read_only(action) {
            dev_info!(
                self.dev,
                "stac: keyboard {:?}: next step {} held (read-only client)\n",
                state,
                action_name(action)
            );
            return None;
        }
        match action {
            Action::Identify => {
                // SAFETY: no preconditions; queues the shim's read.
                unsafe { sep_stac_ta_identify() };
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
