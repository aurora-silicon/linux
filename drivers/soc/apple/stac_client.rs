// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The accessory client of the trusted-accessory service: a Magic Keyboard
//! with Touch ID on one side (through `hid-apple-ta`, see
//! `include/linux/hid-apple-ta.h` and `stac_ta_shim.c`), the enclave's `stac`
//! service on the other, and the lifecycle (see `stac_lifecycle.rs`) deciding
//! what runs next.
//!
//! This first part only looks. When a keyboard's channel comes up, or the
//! service comes back online with one attached, the shim reads the keyboard's
//! protocol tag, identity and connection nonce with three feature-report
//! reads, and the client asks the enclave whether that accessory is paired and
//! authorised (0x02). Nothing is written to the keyboard. Whatever the
//! lifecycle wants next (a per-connection session, a pairing, a connection
//! state) is logged as held and not run.
//!
//! The identity, the nonce and anything else the keyboard answers are never
//! logged.

use kernel::{
    prelude::*,
    sync::{atomic::Relaxed, Arc},
};

use crate::stac_lifecycle::{Action, Event, Nonce, State, NONCE_LEN};
use crate::stac_ops::{self, AccessoryId, ACCESSORY_ID_LEN};
use crate::SepData;

type AttachFn = unsafe extern "C" fn(*mut c_void);
type IdentifiedFn = unsafe extern "C" fn(*mut c_void, *const u8, *const u8);
type FailedFn = unsafe extern "C" fn(*mut c_void, c_int);

extern "C" {
    fn sep_stac_ta_register(
        ctx: *mut c_void,
        attached: AttachFn,
        detached: AttachFn,
        identified: IdentifiedFn,
        failed: FailedFn,
    ) -> c_int;
    fn sep_stac_ta_unregister();
    fn sep_stac_ta_identify();
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

impl SepData {
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
                Some(query_event(q))
            }
            _ => None,
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
