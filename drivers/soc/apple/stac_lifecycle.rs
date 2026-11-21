// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! Lifecycle of one trusted accessory (a keyboard with Touch ID) as the host
//! sees it: the enclave's `stac` service on one side, the keyboard's channel
//! (see `include/linux/hid-apple-ta.h`) on the other.
//!
//! This is the decision logic only. It takes events and returns the
//! operations to run; the caller runs them (stac exchanges, keyboard
//! commands) and feeds the outcomes back as events. Nothing here holds a key
//! or decides that an accessory is paired or authorised: those answers come
//! from the enclave.
//!
//! What the J616s recordings under macOS 27 show, and what this follows:
//!
//! - A keyboard that arrives is identified (identity, public point and its
//!   connection nonce), then the enclave is asked whether it is paired and
//!   authorised (0x02).
//! - The per-connection session (manifest with `after_pairing` clear, then
//!   0x06 and 0x07) runs when the keyboard's connection is new to the
//!   enclave: after the keyboard was switched off and on (a new nonce), and
//!   after the host or the enclave started without a session.
//! - Moving the keyboard between USB and Bluetooth keeps its nonce and its
//!   session: only the connection state (0x09) changes.
//! - A pairing ends with the manifest with `after_pairing` set and leaves a
//!   usable session; no separate session runs.
//! - Losing the link sends connection state 0 (0x09); the record stays for a
//!   grace period and is finalised (0x08) only when it ends.

use crate::stac_ops::AccessoryId;

/// The keyboard's connection nonce. Opaque; never printed.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct Nonce(pub(crate) [u8; NONCE_LEN]);

pub(crate) const NONCE_LEN: usize = 16;

impl core::fmt::Debug for Nonce {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str("Nonce(..)")
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Event {
    /// The enclave's stac service answered online.
    ServiceOnline,
    /// The enclave went away (sleep, crash, unload). Its session state is
    /// gone with it.
    ServiceOffline,
    /// A keyboard's channel came up.
    Attached,
    /// The keyboard answered its identity and connection nonce.
    Identified(AccessoryId, Nonce),
    /// The enclave's pairing-state answer (0x02).
    PairingState { paired: bool, authorized: bool },
    /// A pairing the user approved finished (0x05 with `after_pairing`).
    Paired,
    /// The per-connection session finished (0x07 answered).
    SessionReady,
    /// The enclave acknowledged connection state 1 (0x09).
    Connected,
    /// An operation for this keyboard failed; the reason is logged by the
    /// caller.
    Failed,
    /// The keyboard's channel went away.
    Detached,
    /// The detach grace period ended.
    GraceExpired,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    /// Ask the keyboard for its identity, public point and nonce.
    Identify,
    /// Ask the enclave about this accessory (0x02).
    QueryPairing(AccessoryId),
    /// Run the per-connection session: manifest with `after_pairing` clear,
    /// 0x06, keyboard session material, 0x07.
    BeginSession(AccessoryId),
    /// Report the link (0x09).
    ConnectionState(AccessoryId, bool),
    /// Start the detach grace timer.
    StartGrace,
    /// Stop the grace timer.
    CancelGrace,
    /// Finalise a detach (0x08).
    FinalizeDetach(AccessoryId),
    /// Tell user space a keyboard waits for a pairing it must approve.
    NeedsPairing(AccessoryId),
    /// The keyboard's sensor may be offered to biometric operations.
    BiometricReady(AccessoryId, bool),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum State {
    /// The stac service is not online.
    Offline,
    /// Online, no keyboard.
    Idle,
    /// Waiting for the keyboard's identity.
    Identifying,
    /// Waiting for the enclave's pairing state.
    Checking,
    /// Paired? No: waiting for a pairing the user approves.
    Unpaired,
    /// Running the per-connection session.
    Session,
    /// Waiting for the enclave to take connection state 1.
    Connecting,
    /// Connected. `authorized` says whether it may match.
    Up { authorized: bool },
    /// The link is down; the record is kept until the grace ends.
    Grace,
}

/// At most this many actions follow one event.
pub(crate) const MAX_ACTIONS: usize = 3;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Actions {
    list: [Option<Action>; MAX_ACTIONS],
    len: usize,
}

impl Actions {
    fn push(&mut self, a: Action) {
        // MAX_ACTIONS covers every transition below; a fourth is a bug.
        if self.len < MAX_ACTIONS {
            self.list[self.len] = Some(a);
            self.len += 1;
        }
    }

    pub(crate) fn as_slice(&self) -> impl Iterator<Item = Action> + '_ {
        self.list[..self.len].iter().filter_map(|a| *a)
    }

    pub(crate) fn len(&self) -> usize {
        self.len
    }
}

/// One keyboard's lifecycle.
pub(crate) struct Lifecycle {
    state: State,
    online: bool,
    attached: bool,
    id: Option<AccessoryId>,
    nonce: Option<Nonce>,
    /// The nonce the last session (or pairing) was made for. A connection
    /// with this nonce keeps its session.
    session_nonce: Option<Nonce>,
    authorized: bool,
}

impl Lifecycle {
    pub(crate) const fn new() -> Lifecycle {
        Lifecycle {
            state: State::Offline,
            online: false,
            attached: false,
            id: None,
            nonce: None,
            session_nonce: None,
            authorized: false,
        }
    }

    pub(crate) fn state(&self) -> State {
        self.state
    }

    /// Whether biometric operations may use this keyboard now.
    pub(crate) fn ready(&self) -> bool {
        matches!(self.state, State::Up { authorized: true })
    }

    fn not_ready(&self, out: &mut Actions) {
        if let (true, Some(id)) = (self.ready(), self.id) {
            out.push(Action::BiometricReady(id, false));
        }
    }

    /// The accessory currently being dealt with; only set once identified.
    fn id(&self) -> Option<AccessoryId> {
        self.id
    }

    pub(crate) fn on(&mut self, ev: Event) -> Actions {
        let mut out = Actions::default();
        match ev {
            Event::ServiceOnline => {
                self.online = true;
                // A new enclave session has no channel to any keyboard.
                self.session_nonce = None;
                if self.attached {
                    self.state = State::Identifying;
                    out.push(Action::Identify);
                } else {
                    self.state = State::Idle;
                }
            }
            Event::ServiceOffline => {
                self.not_ready(&mut out);
                if self.state == State::Grace {
                    out.push(Action::CancelGrace);
                }
                self.online = false;
                self.session_nonce = None;
                self.state = State::Offline;
            }
            Event::Attached => {
                self.attached = true;
                if self.state == State::Grace {
                    out.push(Action::CancelGrace);
                }
                if self.online {
                    self.state = State::Identifying;
                    out.push(Action::Identify);
                }
            }
            Event::Identified(id, nonce) => {
                if self.state != State::Identifying {
                    return out;
                }
                // A different keyboard than the one in grace: the old
                // record's session does not carry over.
                if self.id.is_some_and(|old| old != id) {
                    if let Some(old) = self.id {
                        out.push(Action::FinalizeDetach(old));
                    }
                    self.session_nonce = None;
                }
                self.id = Some(id);
                self.nonce = Some(nonce);
                self.state = State::Checking;
                out.push(Action::QueryPairing(id));
            }
            Event::PairingState { paired, authorized } => {
                let Some(id) = self.id() else { return out };
                if self.state != State::Checking {
                    return out;
                }
                self.authorized = authorized;
                if !paired {
                    self.session_nonce = None;
                    self.state = State::Unpaired;
                    out.push(Action::NeedsPairing(id));
                } else if self.session_nonce.is_some() && self.session_nonce == self.nonce {
                    // Same connection (e.g. USB <-> Bluetooth): keep it.
                    self.state = State::Connecting;
                    out.push(Action::ConnectionState(id, true));
                } else {
                    self.state = State::Session;
                    out.push(Action::BeginSession(id));
                }
            }
            Event::Paired => {
                let Some(id) = self.id() else { return out };
                if self.state != State::Unpaired {
                    return out;
                }
                // The pairing leaves a usable channel for this connection.
                self.session_nonce = self.nonce;
                self.state = State::Checking;
                out.push(Action::QueryPairing(id));
            }
            Event::SessionReady => {
                let Some(id) = self.id() else { return out };
                if self.state != State::Session {
                    return out;
                }
                self.session_nonce = self.nonce;
                self.state = State::Connecting;
                out.push(Action::ConnectionState(id, true));
            }
            Event::Connected => {
                let Some(id) = self.id() else { return out };
                if self.state != State::Connecting {
                    return out;
                }
                self.state = State::Up { authorized: self.authorized };
                if self.authorized {
                    out.push(Action::BiometricReady(id, true));
                }
            }
            Event::Failed => {
                // Fail closed: not ready, and the next attach starts over.
                self.not_ready(&mut out);
                self.session_nonce = None;
                if self.attached && self.online {
                    self.state = State::Idle;
                }
            }
            Event::Detached => {
                self.attached = false;
                self.not_ready(&mut out);
                match (self.state, self.id()) {
                    (State::Up { .. } | State::Connecting | State::Session, Some(id)) => {
                        out.push(Action::ConnectionState(id, false));
                        out.push(Action::StartGrace);
                        self.state = State::Grace;
                    }
                    _ => {
                        self.state = if self.online { State::Idle } else { State::Offline };
                    }
                }
            }
            Event::GraceExpired => {
                if self.state != State::Grace {
                    return out;
                }
                if let Some(id) = self.id.take() {
                    out.push(Action::FinalizeDetach(id));
                }
                self.nonce = None;
                self.session_nonce = None;
                self.state = State::Idle;
            }
        }
        out
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_stac_lifecycle)]
mod tests {
    use super::*;
    use kernel::prelude::*;

    const KB: AccessoryId = AccessoryId([0x4b; 16]);
    const OTHER: AccessoryId = AccessoryId([0x0d; 16]);
    const N1: Nonce = Nonce([1; NONCE_LEN]);
    const N2: Nonce = Nonce([2; NONCE_LEN]);

    fn acts(l: &mut Lifecycle, ev: Event) -> KVec<Action> {
        let mut v = KVec::new();
        for a in l.on(ev).as_slice() {
            v.push(a, GFP_KERNEL).unwrap();
        }
        v
    }

    fn up(l: &mut Lifecycle, nonce: Nonce) {
        assert_eq!(&acts(l, Event::Attached)[..], &[Action::Identify]);
        assert_eq!(&acts(l, Event::Identified(KB, nonce))[..], &[Action::QueryPairing(KB)]);
    }

    #[test]
    fn boot_with_a_paired_keyboard_runs_a_session() {
        let mut l = Lifecycle::new();
        assert!(acts(&mut l, Event::ServiceOnline).is_empty());
        up(&mut l, N1);
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: true, authorized: true })[..],
            &[Action::BeginSession(KB)]
        );
        assert_eq!(&acts(&mut l, Event::SessionReady)[..], &[Action::ConnectionState(KB, true)]);
        assert!(!l.ready());
        assert_eq!(&acts(&mut l, Event::Connected)[..], &[Action::BiometricReady(KB, true)]);
        assert!(l.ready());
    }

    #[test]
    fn a_keyboard_that_arrives_before_the_service_waits_for_it() {
        let mut l = Lifecycle::new();
        assert!(acts(&mut l, Event::Attached).is_empty());
        assert_eq!(&acts(&mut l, Event::ServiceOnline)[..], &[Action::Identify]);
        assert_eq!(l.state(), State::Identifying);
    }

    #[test]
    fn a_transport_switch_keeps_the_session() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        acts(&mut l, Event::PairingState { paired: true, authorized: true });
        acts(&mut l, Event::SessionReady);
        acts(&mut l, Event::Connected);
        assert_eq!(
            &acts(&mut l, Event::Detached)[..],
            &[
                Action::BiometricReady(KB, false),
                Action::ConnectionState(KB, false),
                Action::StartGrace
            ]
        );
        assert_eq!(&acts(&mut l, Event::Attached)[..], &[Action::CancelGrace, Action::Identify]);
        acts(&mut l, Event::Identified(KB, N1));
        // Same nonce: no session, only the link state.
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: true, authorized: true })[..],
            &[Action::ConnectionState(KB, true)]
        );
        assert_eq!(&acts(&mut l, Event::Connected)[..], &[Action::BiometricReady(KB, true)]);
    }

    #[test]
    fn a_power_cycle_runs_a_new_session() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        acts(&mut l, Event::PairingState { paired: true, authorized: true });
        acts(&mut l, Event::SessionReady);
        acts(&mut l, Event::Connected);
        acts(&mut l, Event::Detached);
        acts(&mut l, Event::Attached);
        acts(&mut l, Event::Identified(KB, N2));
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: true, authorized: true })[..],
            &[Action::BeginSession(KB)]
        );
    }

    #[test]
    fn the_grace_period_ends_in_a_detach() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        acts(&mut l, Event::PairingState { paired: true, authorized: false });
        acts(&mut l, Event::SessionReady);
        // Paired but not enabled for Touch ID: up, never ready.
        assert!(acts(&mut l, Event::Connected).is_empty());
        assert_eq!(l.state(), State::Up { authorized: false });
        assert!(!l.ready());
        assert_eq!(
            &acts(&mut l, Event::Detached)[..],
            &[Action::ConnectionState(KB, false), Action::StartGrace]
        );
        assert_eq!(&acts(&mut l, Event::GraceExpired)[..], &[Action::FinalizeDetach(KB)]);
        assert_eq!(l.state(), State::Idle);
        assert!(acts(&mut l, Event::GraceExpired).is_empty());
    }

    #[test]
    fn an_unpaired_keyboard_waits_for_the_user() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: false, authorized: false })[..],
            &[Action::NeedsPairing(KB)]
        );
        // Out-of-order events change nothing.
        assert!(acts(&mut l, Event::SessionReady).is_empty());
        assert!(acts(&mut l, Event::Connected).is_empty());
        assert_eq!(&acts(&mut l, Event::Paired)[..], &[Action::QueryPairing(KB)]);
        // The pairing made the channel: no session for this connection.
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: true, authorized: false })[..],
            &[Action::ConnectionState(KB, true)]
        );
    }

    #[test]
    fn the_enclave_going_away_drops_readiness_and_sessions() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        acts(&mut l, Event::PairingState { paired: true, authorized: true });
        acts(&mut l, Event::SessionReady);
        acts(&mut l, Event::Connected);
        assert_eq!(&acts(&mut l, Event::ServiceOffline)[..], &[Action::BiometricReady(KB, false)]);
        assert_eq!(l.state(), State::Offline);
        // Back online with the keyboard still attached: same nonce, but the
        // enclave has no session, so one runs.
        assert_eq!(&acts(&mut l, Event::ServiceOnline)[..], &[Action::Identify]);
        acts(&mut l, Event::Identified(KB, N1));
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: true, authorized: true })[..],
            &[Action::BeginSession(KB)]
        );
    }

    #[test]
    fn a_failure_fails_closed() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        acts(&mut l, Event::PairingState { paired: true, authorized: true });
        acts(&mut l, Event::SessionReady);
        acts(&mut l, Event::Connected);
        assert_eq!(&acts(&mut l, Event::Failed)[..], &[Action::BiometricReady(KB, false)]);
        assert!(!l.ready());
        assert_eq!(l.state(), State::Idle);
    }

    #[test]
    fn another_keyboard_in_the_grace_period_finalises_the_first() {
        let mut l = Lifecycle::new();
        acts(&mut l, Event::ServiceOnline);
        up(&mut l, N1);
        acts(&mut l, Event::PairingState { paired: true, authorized: true });
        acts(&mut l, Event::SessionReady);
        acts(&mut l, Event::Connected);
        acts(&mut l, Event::Detached);
        acts(&mut l, Event::Attached);
        assert_eq!(
            &acts(&mut l, Event::Identified(OTHER, N1))[..],
            &[Action::FinalizeDetach(KB), Action::QueryPairing(OTHER)]
        );
        // Its nonce matches by chance, but it never had a session.
        assert_eq!(
            &acts(&mut l, Event::PairingState { paired: true, authorized: true })[..],
            &[Action::BeginSession(OTHER)]
        );
    }
}
