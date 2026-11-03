// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! DESIGN PROTOTYPE: a per-user key bag whose secret is the login password.
//!
//! Today the driver unlocks the designated identity bag at boot with a secret
//! it keeps on disk, and never locks it. This module sketches the macOS model
//! instead:
//!
//! * the machine bag (ref-key, trusted keys, system Catacomb) stays as it is;
//! * each Unix user who enrols gets a second bag, created with that user's
//!   password as the secret, and designated as the biometric user. The
//!   enclave entangles the secret with its own device key, so the bag cannot
//!   be opened off the machine, and root on the machine cannot open it
//!   without the password;
//! * the bag stays locked from boot until the first password unlock, locks
//!   on screen lock after a grace period and at once on suspend;
//! * Touch ID is offered only while [`Policy::biometry_allowed`] says so.
//!
//! What is wired today: the policy (pure, tested) and the unlock request,
//! which reuses the unlock layout the driver already sends for the identity
//! bag. What is NOT: the lock, the passcode change and the lock-state read.
//! Their wire values have not been captured yet; each is a `TODO(capture)`
//! naming the capture that fills it. Nothing here invents a wire value.
//!
//! The ioctls (UAPI `SEP_BIO_KEYBAG_*`) are meant for the root-only re-arm
//! node, so fprintd never sees a password. The dispatcher is not hooked to a
//! file yet.

#![allow(dead_code)]

use kernel::prelude::*;

/// macOS: "five unsuccessful biometric authentication match attempts".
pub(crate) const MAX_FAILED_MATCHES: u32 = 5;
/// macOS: "hasn't unlocked their device for more than 48 hours".
pub(crate) const NO_UNLOCK_LIMIT_S: u64 = 48 * 3600;
/// macOS: no password unlock for 156 hours (six and a half days) ...
pub(crate) const NO_PASSWORD_LIMIT_S: u64 = 156 * 3600;
/// ... and no biometric unlock in four hours.
pub(crate) const NO_BIOMETRY_LIMIT_S: u64 = 4 * 3600;
/// Longest grace period a lock request may ask for. The default comes from
/// the user's desktop setting, not from here.
pub(crate) const MAX_GRACE_S: u32 = 3600;

/// Longest secret accepted. TODO(capture N1): the enclave's own limit for a
/// bag secret is unknown; macOS passes the password bytes as typed.
pub(crate) const SECRET_MAX: usize = 256;

/// The lowest designated user the key store accepts (`DesignateUser`).
pub(crate) const MIN_UID: u32 = crate::sks::SKS_DESIGNATE_USER_MIN as u32;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum BagState {
    /// Since boot (or logout) no password has opened the bag.
    LockedSinceBoot,
    Unlocked,
    /// Locked by the screen; class keys stay until `deadline_s`.
    Locking {
        deadline_s: u64,
    },
    Locked,
}

impl BagState {
    pub(crate) const fn uapi(self) -> u32 {
        match self {
            BagState::LockedSinceBoot => 0,
            BagState::Unlocked => 1,
            BagState::Locking { .. } => 2,
            BagState::Locked => 3,
        }
    }
}

/// Why Touch ID is not offered; the reason the lock screen shows.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Refusal {
    PasswordSinceBoot,
    NoUnlockIn48h,
    NoPasswordIn156hAndNoBiometryIn4h,
    FailedMatches,
}

impl Refusal {
    pub(crate) const fn uapi(self) -> u32 {
        match self {
            Refusal::PasswordSinceBoot => 1,
            Refusal::NoUnlockIn48h => 2,
            Refusal::NoPasswordIn156hAndNoBiometryIn4h => 3,
            Refusal::FailedMatches => 4,
        }
    }
}

/// What the driver must tell the enclave after a policy step.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum EnclaveAction {
    Nothing,
    /// Send the bag lock (TODO(capture N2)).
    LockBag,
}

/// Host-side copy of the macOS rules for one user's bag. Times are
/// CLOCK_BOOTTIME seconds, so time asleep counts, as it does on macOS.
///
/// Once the user bag is real the enclave enforces the part that matters (no
/// keys without the password after boot). This copy decides when to offer
/// Touch ID at all and when to send the lock.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Policy {
    state: BagState,
    last_unlock_s: Option<u64>,
    last_password_s: Option<u64>,
    last_biometry_s: Option<u64>,
    failed_matches: u32,
}

impl Policy {
    pub(crate) const fn at_boot() -> Policy {
        Policy {
            state: BagState::LockedSinceBoot,
            last_unlock_s: None,
            last_password_s: None,
            last_biometry_s: None,
            failed_matches: 0,
        }
    }

    pub(crate) fn state(&self) -> BagState {
        self.state
    }

    pub(crate) fn failed_matches(&self) -> u32 {
        self.failed_matches
    }

    pub(crate) fn since_password_s(&self, now_s: u64) -> Option<u64> {
        self.last_password_s.map(|t| now_s.saturating_sub(t))
    }

    pub(crate) fn since_unlock_s(&self, now_s: u64) -> Option<u64> {
        self.last_unlock_s.map(|t| now_s.saturating_sub(t))
    }

    /// The macOS rule set, in the order the lock screen should report it.
    pub(crate) fn biometry_allowed(&self, now_s: u64) -> core::result::Result<(), Refusal> {
        let Some(password) = self.last_password_s else {
            return Err(Refusal::PasswordSinceBoot);
        };
        if self.failed_matches >= MAX_FAILED_MATCHES {
            return Err(Refusal::FailedMatches);
        }
        let unlock = self.last_unlock_s.unwrap_or(password);
        if now_s.saturating_sub(unlock) > NO_UNLOCK_LIMIT_S {
            return Err(Refusal::NoUnlockIn48h);
        }
        let no_password = now_s.saturating_sub(password) > NO_PASSWORD_LIMIT_S;
        let no_biometry = self
            .last_biometry_s
            .map_or(true, |t| now_s.saturating_sub(t) > NO_BIOMETRY_LIMIT_S);
        if no_password && no_biometry {
            return Err(Refusal::NoPasswordIn156hAndNoBiometryIn4h);
        }
        Ok(())
    }

    /// The enclave accepted the password: the only way out of every refusal,
    /// the lockout included (macOS sends no separate clear).
    pub(crate) fn password_unlocked(&mut self, now_s: u64) {
        self.state = BagState::Unlocked;
        self.last_unlock_s = Some(now_s);
        self.last_password_s = Some(now_s);
        self.failed_matches = 0;
    }

    /// A match unlocked the bag. Refused (and nothing changes) when the
    /// rules do not allow Touch ID now.
    pub(crate) fn biometry_unlocked(&mut self, now_s: u64) -> core::result::Result<(), Refusal> {
        self.biometry_allowed(now_s)?;
        self.state = BagState::Unlocked;
        self.last_unlock_s = Some(now_s);
        self.last_biometry_s = Some(now_s);
        self.failed_matches = 0;
        Ok(())
    }

    pub(crate) fn match_failed(&mut self) {
        self.failed_matches = self.failed_matches.saturating_add(1);
    }

    /// Screen lock. A zero grace period locks at once.
    pub(crate) fn screen_locked(&mut self, now_s: u64, grace_s: u32) -> EnclaveAction {
        match self.state {
            BagState::Unlocked if grace_s == 0 => {
                self.state = BagState::Locked;
                EnclaveAction::LockBag
            }
            BagState::Unlocked => {
                let grace = u64::from(grace_s.min(MAX_GRACE_S));
                self.state = BagState::Locking {
                    deadline_s: now_s.saturating_add(grace),
                };
                EnclaveAction::Nothing
            }
            _ => EnclaveAction::Nothing,
        }
    }

    /// Called from a timer and before every verify.
    pub(crate) fn tick(&mut self, now_s: u64) -> EnclaveAction {
        match self.state {
            BagState::Locking { deadline_s } if now_s >= deadline_s => {
                self.state = BagState::Locked;
                EnclaveAction::LockBag
            }
            _ => EnclaveAction::Nothing,
        }
    }

    /// Suspend locks at once, grace or not ("locks ... every time it goes to
    /// sleep" on a Mac laptop with Touch ID).
    pub(crate) fn suspending(&mut self) -> EnclaveAction {
        match self.state {
            BagState::Unlocked | BagState::Locking { .. } => {
                self.state = BagState::Locked;
                EnclaveAction::LockBag
            }
            _ => EnclaveAction::Nothing,
        }
    }

    /// Logout: as after a restart, only the password opens the bag again.
    pub(crate) fn logged_out(&mut self) -> EnclaveAction {
        let was_open = matches!(self.state, BagState::Unlocked | BagState::Locking { .. });
        *self = Policy::at_boot();
        if was_open {
            EnclaveAction::LockBag
        } else {
            EnclaveAction::Nothing
        }
    }
}

/// The UAPI `struct sep_keybag_unlock` (also used to create the bag).
#[repr(C)]
pub(crate) struct UnlockRequest {
    pub(crate) uid: u32,
    pub(crate) secret_len: u32,
    pub(crate) secret: [u8; SECRET_MAX],
}

/// The UAPI `struct sep_keybag_lock`.
#[repr(C)]
pub(crate) struct LockRequest {
    pub(crate) uid: u32,
    pub(crate) reason: u32,
    pub(crate) grace_s: u32,
    pub(crate) reserved: u32,
}

pub(crate) const LOCK_REASON_SCREEN: u32 = 1;
pub(crate) const LOCK_REASON_SUSPEND: u32 = 2;
pub(crate) const LOCK_REASON_LOGOUT: u32 = 3;

/// The UAPI `struct sep_keybag_state`.
#[repr(C)]
pub(crate) struct StateReply {
    pub(crate) uid: u32,
    pub(crate) bag_state: u32,
    /// 0 when Touch ID may be offered, otherwise a `Refusal`.
    pub(crate) biometry: u32,
    pub(crate) failed_matches: u32,
    /// `u64::MAX` when there was none since boot.
    pub(crate) since_password_s: u64,
    pub(crate) since_unlock_s: u64,
    /// The enclave's own lock-state word. TODO(capture N4): the read op and
    /// its reply layout are unknown, so this stays 0.
    pub(crate) enclave_lock_state: u32,
    pub(crate) reserved: u32,
}

static_assert!(core::mem::size_of::<UnlockRequest>() == 8 + SECRET_MAX);
static_assert!(core::mem::size_of::<LockRequest>() == 16);
static_assert!(core::mem::size_of::<StateReply>() == 40);

/// Checks an unlock/create request and returns the user and the secret.
pub(crate) fn parse_unlock(req: &UnlockRequest) -> Result<(crate::sks::DesignateUser, &[u8])> {
    let len = req.secret_len as usize;
    if len == 0 || len > SECRET_MAX || req.uid < MIN_UID || req.uid > i32::MAX as u32 {
        return Err(EINVAL);
    }
    let user = crate::sks::DesignateUser::new(req.uid as i32).ok_or(EINVAL)?;
    Ok((user, &req.secret[..len]))
}

pub(crate) fn parse_lock(req: &LockRequest) -> Result<(u32, u32, u32)> {
    if req.reserved != 0 || req.uid < MIN_UID || req.uid > i32::MAX as u32 {
        return Err(EINVAL);
    }
    match req.reason {
        LOCK_REASON_SCREEN | LOCK_REASON_SUSPEND | LOCK_REASON_LOGOUT => {}
        _ => return Err(EINVAL),
    }
    Ok((req.uid, req.reason, req.grace_s.min(MAX_GRACE_S)))
}

pub(crate) fn state_reply(uid: u32, p: &Policy, now_s: u64) -> StateReply {
    StateReply {
        uid,
        bag_state: p.state().uapi(),
        biometry: match p.biometry_allowed(now_s) {
            Ok(()) => 0,
            Err(r) => r.uapi(),
        },
        failed_matches: p.failed_matches(),
        since_password_s: p.since_password_s(now_s).unwrap_or(u64::MAX),
        since_unlock_s: p.since_unlock_s(now_s).unwrap_or(u64::MAX),
        enclave_lock_state: 0,
        reserved: 0,
    }
}

impl crate::SepData {
    /// Opens a user's bag with the password. KNOWN: this is the unlock the
    /// driver already sends for the identity bag (0x04 state 0 on variant-5
    /// key stores, 0x18 state 0 on 13.5), addressed to the user's special
    /// handle (-uid, the same convention macOS uses for its user bags).
    ///
    /// TODO(capture N1): that a bag CREATED with a password answers this
    /// unlock exactly as the identity bag does. macOS on J700 unlocks its
    /// user bag with 0x18 carrying the typed password; this path sends 0x04
    /// on J700. Confirm on hardware which one a user bag needs.
    pub(crate) fn userbag_unlock(
        &self,
        user: crate::sks::DesignateUser,
        secret: &[u8],
    ) -> Result<()> {
        let special = user.special_handle();
        self.sks_step(crate::sks::SKS_LOCK_STATE_NAME, |healthy| {
            self.sks_req_unlock_special(special, secret, healthy)
        })
        .map(|_| ())
        .ok_or(EACCES)
    }

    /// TODO(capture N2): the bag lock. The macOS trace shows a
    /// DEVICE_STATE_TRANSITION (0x18) for the user's handle with state 1 and
    /// no secret, sent by the login window at screen lock. The body on the
    /// wire (the flags word in particular) was not dumped. Capture: m1n1 HV
    /// `J700_SEP_SWEEP=sks:0x18:...` at a macOS screen lock, or a dtrace
    /// tracemem of the request image. Until then this refuses.
    pub(crate) fn userbag_lock(&self, _user: crate::sks::DesignateUser) -> Result<()> {
        Err(EOPNOTSUPP)
    }

    /// TODO(capture N3): change the bag secret (password change). Neither the
    /// opcode nor the body is known. Capture: a macOS password change in
    /// System Settings under the same dtrace method, plus the HV sweep for
    /// the payload. Until then a password change goes through the recovery
    /// path in DESIGN.md (unlock with the old password, re-create, migrate).
    pub(crate) fn userbag_change_secret(
        &self,
        _user: crate::sks::DesignateUser,
        _old: &[u8],
        _new: &[u8],
    ) -> Result<()> {
        Err(EOPNOTSUPP)
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_userbag_policy)]
mod tests {
    use super::*;

    const H: u64 = 3600;
    const T0: u64 = 1000;

    fn unlocked_at(t: u64) -> Policy {
        let mut p = Policy::at_boot();
        p.password_unlocked(t);
        p
    }

    #[test]
    fn password_is_required_after_boot() {
        let mut p = Policy::at_boot();
        assert_eq!(p.state(), BagState::LockedSinceBoot);
        assert_eq!(p.biometry_allowed(T0), Err(Refusal::PasswordSinceBoot));
        assert_eq!(p.biometry_unlocked(T0), Err(Refusal::PasswordSinceBoot));
        assert_eq!(p.state(), BagState::LockedSinceBoot);
        p.password_unlocked(T0);
        assert_eq!(p.biometry_allowed(T0 + 1), Ok(()));
    }

    #[test]
    fn five_failed_matches_lock_out_until_the_password() {
        let mut p = unlocked_at(T0);
        for _ in 0..MAX_FAILED_MATCHES - 1 {
            p.match_failed();
        }
        assert_eq!(p.biometry_allowed(T0 + 1), Ok(()));
        p.match_failed();
        assert_eq!(p.biometry_allowed(T0 + 1), Err(Refusal::FailedMatches));
        // A match cannot clear it; only the password does.
        assert_eq!(p.biometry_unlocked(T0 + 2), Err(Refusal::FailedMatches));
        p.password_unlocked(T0 + 3);
        assert_eq!(p.failed_matches(), 0);
        assert_eq!(p.biometry_allowed(T0 + 4), Ok(()));
    }

    #[test]
    fn a_successful_match_resets_the_failure_count() {
        let mut p = unlocked_at(T0);
        p.match_failed();
        p.match_failed();
        assert_eq!(p.biometry_unlocked(T0 + 1), Ok(()));
        assert_eq!(p.failed_matches(), 0);
    }

    #[test]
    fn no_unlock_for_48_hours_needs_the_password() {
        let mut p = unlocked_at(T0);
        assert_eq!(p.biometry_allowed(T0 + 48 * H), Ok(()));
        assert_eq!(
            p.biometry_allowed(T0 + 48 * H + 1),
            Err(Refusal::NoUnlockIn48h)
        );
        // Touch ID unlocks restart the 48 h window.
        assert_eq!(p.biometry_unlocked(T0 + 47 * H), Ok(()));
        assert_eq!(p.biometry_allowed(T0 + 94 * H), Ok(()));
    }

    #[test]
    fn the_156_hour_rule_needs_both_conditions() {
        let mut p = unlocked_at(T0);
        // Keep unlocking by finger every 3 h: the 4 h condition never holds.
        let mut t = T0;
        while t < T0 + 160 * H {
            t += 3 * H;
            assert_eq!(p.biometry_unlocked(t), Ok(()));
        }
        assert!(t - T0 > NO_PASSWORD_LIMIT_S);
        assert_eq!(p.biometry_allowed(t + 4 * H), Ok(()));
        // Over 156 h since the password and over 4 h since a finger.
        assert_eq!(
            p.biometry_allowed(t + 4 * H + 1),
            Err(Refusal::NoPasswordIn156hAndNoBiometryIn4h)
        );
    }

    #[test]
    fn screen_lock_waits_for_the_grace_period() {
        let mut p = unlocked_at(T0);
        assert_eq!(p.screen_locked(T0, 10), EnclaveAction::Nothing);
        assert_eq!(
            p.state(),
            BagState::Locking {
                deadline_s: T0 + 10
            }
        );
        assert_eq!(p.tick(T0 + 9), EnclaveAction::Nothing);
        assert_eq!(p.tick(T0 + 10), EnclaveAction::LockBag);
        assert_eq!(p.state(), BagState::Locked);
        assert_eq!(p.tick(T0 + 11), EnclaveAction::Nothing);
    }

    #[test]
    fn a_match_inside_the_grace_period_keeps_the_bag_open() {
        let mut p = unlocked_at(T0);
        p.screen_locked(T0, 10);
        assert_eq!(p.biometry_unlocked(T0 + 4), Ok(()));
        assert_eq!(p.state(), BagState::Unlocked);
        assert_eq!(p.tick(T0 + 20), EnclaveAction::Nothing);
    }

    #[test]
    fn zero_grace_and_suspend_lock_at_once() {
        let mut p = unlocked_at(T0);
        assert_eq!(p.screen_locked(T0, 0), EnclaveAction::LockBag);
        let mut q = unlocked_at(T0);
        q.screen_locked(T0, 300);
        assert_eq!(q.suspending(), EnclaveAction::LockBag);
        assert_eq!(q.state(), BagState::Locked);
        assert_eq!(q.suspending(), EnclaveAction::Nothing);
        // Touch ID may still open a bag locked by sleep.
        assert_eq!(q.biometry_unlocked(T0 + 60), Ok(()));
    }

    #[test]
    fn grace_is_capped() {
        let mut p = unlocked_at(T0);
        p.screen_locked(T0, u32::MAX);
        assert_eq!(
            p.state(),
            BagState::Locking {
                deadline_s: T0 + u64::from(MAX_GRACE_S)
            }
        );
    }

    #[test]
    fn logout_behaves_like_a_restart() {
        let mut p = unlocked_at(T0);
        assert_eq!(p.logged_out(), EnclaveAction::LockBag);
        assert_eq!(p.biometry_allowed(T0 + 1), Err(Refusal::PasswordSinceBoot));
        assert_eq!(p.logged_out(), EnclaveAction::Nothing);
    }

    #[test]
    fn requests_are_checked() {
        let mut req = UnlockRequest {
            uid: 1000,
            secret_len: 4,
            secret: [0x61; SECRET_MAX],
        };
        let (user, secret) = parse_unlock(&req).expect("valid request");
        assert_eq!(user.special_handle().value(), -1000);
        assert_eq!(secret.len(), 4);
        req.secret_len = 0;
        assert!(parse_unlock(&req).is_err());
        req.secret_len = SECRET_MAX as u32 + 1;
        assert!(parse_unlock(&req).is_err());
        req.secret_len = 4;
        req.uid = 0;
        assert!(parse_unlock(&req).is_err());
        req.uid = u32::MAX;
        assert!(parse_unlock(&req).is_err());

        let mut lock = LockRequest {
            uid: 1000,
            reason: LOCK_REASON_SCREEN,
            grace_s: 99999,
            reserved: 0,
        };
        assert_eq!(
            parse_lock(&lock).ok(),
            Some((1000, LOCK_REASON_SCREEN, MAX_GRACE_S))
        );
        lock.reason = 0;
        assert!(parse_lock(&lock).is_err());
        lock.reason = LOCK_REASON_SUSPEND;
        lock.reserved = 1;
        assert!(parse_lock(&lock).is_err());
    }

    #[test]
    fn the_user_bag_handle_is_minus_the_uid() {
        let user = crate::sks::DesignateUser::new(501).expect("user");
        assert_eq!(user.special_handle().value(), -501);
    }

    #[test]
    fn state_reply_reports_the_refusal() {
        let p = Policy::at_boot();
        let r = state_reply(1000, &p, T0);
        assert_eq!(r.bag_state, 0);
        assert_eq!(r.biometry, Refusal::PasswordSinceBoot.uapi());
        assert_eq!(r.since_password_s, u64::MAX);
        let q = unlocked_at(T0);
        let r = state_reply(1000, &q, T0 + 5);
        assert_eq!(r.bag_state, 1);
        assert_eq!(r.biometry, 0);
        assert_eq!(r.since_password_s, 5);
        assert_eq!(r.enclave_lock_state, 0);
    }
}
