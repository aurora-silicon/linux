// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Per-user key-bag protocol and host policy.
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
//! The ioctls (UAPI `SEP_BIO_KEYBAG_*`) are meant for the root-only re-arm
//! node, so fprintd never sees a password. Request images are wiped on drop,
//! and the ioctl request copies holding passwords are wiped before return.

#![allow(dead_code)]

use kernel::prelude::*;
use kernel::transmute::FromBytes;
use kernel::uaccess::{UserPtr, UserSlice};

const CLIENT: u64 = crate::sks::SKS_CLIENT_ID;
const CREATE_VARIANT: u32 = 4;
const CREATE_BAG_TYPE: u32 = 0;
const CREATE_ARG: i32 = -1;
const DESIGNATE_VARIANT: u32 = 1;
const CHANGE_VARIANT: u32 = 3;
const LOCK_STATE_VARIANT: u32 = 1;
const DEVICE_STATE_VARIANT: u32 = 0;
const READ_VARIANT: u32 = 0;
const CHANGE_TRAILER: u32 = 0xff;

#[derive(Debug, PartialEq, Eq)]
struct Create<'a> {
    secret: &'a [u8],
}

#[derive(Debug, PartialEq, Eq)]
struct Designate {
    source_handle: i32,
    user_handle: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TransitionState {
    Unlocked,
    Locked,
}

impl TransitionState {
    const fn wire(self) -> u32 {
        match self {
            TransitionState::Unlocked => 0,
            TransitionState::Locked => 1,
        }
    }
}

/// State passed to the key store's explicit change-lock-state operation.
///
/// Device-state transitions model screen/sleep transitions and may enter the
/// bag's configured grace period.  This operation is the direct key-bag lock
/// used when the caller asked for no grace (logout, suspend, or a zero-grace
/// screen lock).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LockStateChange {
    Query,
    Locked,
}

impl LockStateChange {
    const fn wire(self) -> i32 {
        match self {
            LockStateChange::Query => -1,
            LockStateChange::Locked => 1,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
struct Transition<'a> {
    user_handle: i32,
    state: TransitionState,
    flags: u64,
    secret: &'a [u8],
}

#[derive(Debug, PartialEq, Eq)]
struct ChangeSecret<'a> {
    user_handle: i32,
    old: &'a [u8],
    new: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct GetState {
    state_word: u32,
    lock_phase: u32,
    failed_attempts: u32,
    attempt_limit: u32,
    handle: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Configuration {
    grace_period_s: u32,
    expiry_time_s: u32,
    extended_period_s: u32,
    attempt_limit: u32,
    biometric_disable_limit: u32,
}

struct Cursor<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Cursor<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, at: 0 }
    }

    fn u32(&mut self) -> Option<u32> {
        let end = self.at.checked_add(4)?;
        let value = u32::from_le_bytes(self.bytes.get(self.at..end)?.try_into().ok()?);
        self.at = end;
        Some(value)
    }

    fn i32(&mut self) -> Option<i32> {
        Some(self.u32()? as i32)
    }

    fn u64(&mut self) -> Option<u64> {
        let end = self.at.checked_add(8)?;
        let value = u64::from_le_bytes(self.bytes.get(self.at..end)?.try_into().ok()?);
        self.at = end;
        Some(value)
    }

    fn blob(&mut self) -> Option<&'a [u8]> {
        let (blob, next) = crate::image::read_blob(self.bytes, self.at)?;
        self.at = next;
        Some(blob)
    }

    fn done(self) -> bool {
        self.at == self.bytes.len()
    }
}

fn encode_create(secret: &[u8]) -> Result<crate::image::Body> {
    let mut body = crate::image::Body::new();
    body.put_u32(CREATE_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_u32(CREATE_BAG_TYPE)?;
    body.put_i32(CREATE_ARG)?;
    body.put_blob(secret)?;
    // Five absent trailing fields in the captured variant-4 structure.
    for _ in 0..5 {
        body.put_u32(0)?;
    }
    Ok(body)
}

fn decode_create(body: &[u8]) -> Option<Create<'_>> {
    let mut c = Cursor::new(body);
    if c.u32()? != CREATE_VARIANT
        || c.u64()? != CLIENT
        || c.u32()? != CREATE_BAG_TYPE
        || c.i32()? != CREATE_ARG
    {
        return None;
    }
    let secret = c.blob()?;
    for _ in 0..5 {
        if c.u32()? != 0 {
            return None;
        }
    }
    c.done().then_some(Create { secret })
}

fn decode_create_reply(body: &[u8]) -> Option<crate::sks::KeyBagHandle> {
    let mut c = Cursor::new(body);
    if c.u32()? != CREATE_VARIANT {
        return None;
    }
    let handle = c.i32()?;
    if handle < 0 || c.u32()? != 0 || !c.done() {
        return None;
    }
    Some(crate::sks::KeyBagHandle::from_create_reply(handle))
}

fn encode_designate(source_handle: i32, user_handle: i32) -> Result<crate::image::Body> {
    if source_handle < 0 || user_handle >= 0 {
        return Err(EINVAL);
    }
    let mut body = crate::image::Body::new();
    body.put_u32(DESIGNATE_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_i32(source_handle)?;
    body.put_i32(user_handle)?;
    body.put_blob(&[])?;
    body.put_u64(0)?;
    Ok(body)
}

fn decode_designate(body: &[u8]) -> Option<Designate> {
    let mut c = Cursor::new(body);
    if c.u32()? != DESIGNATE_VARIANT || c.u64()? != CLIENT {
        return None;
    }
    let source_handle = c.i32()?;
    let user_handle = c.i32()?;
    if source_handle < 0 || user_handle >= 0 || !c.blob()?.is_empty() || c.u64()? != 0 || !c.done()
    {
        return None;
    }
    Some(Designate {
        source_handle,
        user_handle,
    })
}

fn decode_designate_reply(body: &[u8]) -> bool {
    body == DESIGNATE_VARIANT.to_le_bytes()
}

fn encode_transition(
    user_handle: i32,
    state: TransitionState,
    flags: u64,
    secret: &[u8],
) -> Result<crate::image::Body> {
    if user_handle >= 0
        || (state == TransitionState::Unlocked && flags != 0)
        || (state == TransitionState::Locked && (!secret.is_empty() || flags > 1))
    {
        return Err(EINVAL);
    }
    let mut body = crate::image::Body::new();
    body.put_u32(DEVICE_STATE_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_i32(user_handle)?;
    body.put_u32(state.wire())?;
    body.put_u64(flags)?;
    body.put_blob(secret)?;
    Ok(body)
}

fn decode_transition(body: &[u8]) -> Option<Transition<'_>> {
    let mut c = Cursor::new(body);
    if c.u32()? != DEVICE_STATE_VARIANT || c.u64()? != CLIENT {
        return None;
    }
    let user_handle = c.i32()?;
    let state = match c.u32()? {
        0 => TransitionState::Unlocked,
        1 => TransitionState::Locked,
        _ => return None,
    };
    let flags = c.u64()?;
    let secret = c.blob()?;
    if user_handle >= 0
        || !c.done()
        || (state == TransitionState::Unlocked && flags != 0)
        || (state == TransitionState::Locked && (!secret.is_empty() || flags > 1))
    {
        return None;
    }
    Some(Transition {
        user_handle,
        state,
        flags,
        secret,
    })
}

fn encode_timer_fire() -> Result<crate::image::Body> {
    let mut body = crate::image::Body::new();
    body.put_u32(0)?;
    body.put_u32(1)?;
    body.put_u32(0)?;
    Ok(body)
}

fn decode_timer_fire(body: &[u8]) -> bool {
    body == [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]
}

/// The grace-expiry answer, as the Neo's key store gave it on Linux on
/// 11-09: `u32 0`, the handle whose grace period ended (0 when none has
/// yet), the timer kind (1), `u32 0`, then two opaque words (a token, and a
/// small counter that was 0 while grace ran and 3 when the bag locked).
fn decode_timer_fire_reply(body: &[u8]) -> Option<i32> {
    let mut c = Cursor::new(body);
    let lead = c.u32()?;
    let handle = c.i32()?;
    let kind = c.u32()?;
    let reserved = c.u32()?;
    let _token = c.u32()?;
    let _count = c.u32()?;
    (lead == 0 && handle <= 0 && kind == 1 && reserved == 0 && c.done()).then_some(handle)
}

fn encode_change_secret(user_handle: i32, old: &[u8], new: &[u8]) -> Result<crate::image::Body> {
    if user_handle >= 0 || old.is_empty() || new.is_empty() {
        return Err(EINVAL);
    }
    let mut body = crate::image::Body::new();
    body.put_u32(CHANGE_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_i32(user_handle)?;
    body.put_blob(old)?;
    body.put_blob(new)?;
    body.put_u64(0)?;
    body.put_u32(CHANGE_TRAILER)?;
    body.put_u64(0)?;
    Ok(body)
}

fn decode_change_secret(body: &[u8]) -> Option<ChangeSecret<'_>> {
    let mut c = Cursor::new(body);
    if c.u32()? != CHANGE_VARIANT || c.u64()? != CLIENT {
        return None;
    }
    let user_handle = c.i32()?;
    let old = c.blob()?;
    let new = c.blob()?;
    if user_handle >= 0
        || old.is_empty()
        || new.is_empty()
        || c.u64()? != 0
        || c.u32()? != CHANGE_TRAILER
        || c.u64()? != 0
        || !c.done()
    {
        return None;
    }
    Some(ChangeSecret {
        user_handle,
        old,
        new,
    })
}

fn decode_change_secret_reply(body: &[u8]) -> bool {
    body == [3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
}

fn encode_lock_state_change(
    user_handle: i32,
    state: LockStateChange,
) -> Result<crate::image::Body> {
    if user_handle >= 0 {
        return Err(EINVAL);
    }
    let mut body = crate::image::Body::new();
    body.put_u32(LOCK_STATE_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_i32(user_handle)?;
    body.put_i32(state.wire())?;
    body.put_blob(&[])?;
    body.put_u64(0)?;
    Ok(body)
}

fn encode_lock_query(user_handle: i32) -> Result<crate::image::Body> {
    encode_lock_state_change(user_handle, LockStateChange::Query)
}

fn encode_explicit_lock(user_handle: i32) -> Result<crate::image::Body> {
    encode_lock_state_change(user_handle, LockStateChange::Locked)
}

fn decode_lock_query(body: &[u8]) -> Option<i32> {
    let mut c = Cursor::new(body);
    if c.u32()? != LOCK_STATE_VARIANT || c.u64()? != CLIENT {
        return None;
    }
    let handle = c.i32()?;
    if handle >= 0 || c.i32()? != -1 || !c.blob()?.is_empty() || c.u64()? != 0 || !c.done() {
        return None;
    }
    Some(handle)
}

fn decode_lock_query_reply(body: &[u8]) -> Option<u32> {
    let mut c = Cursor::new(body);
    if c.u32()? != LOCK_STATE_VARIANT {
        return None;
    }
    let state = c.u32()?;
    if c.u32()? != 0 || c.u32()? != 0 || !c.done() {
        return None;
    }
    Some(state)
}

fn encode_get_state(user_handle: i32) -> Result<crate::image::Body> {
    if user_handle >= 0 {
        return Err(EINVAL);
    }
    let mut body = crate::image::Body::new();
    body.put_u32(READ_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_i32(user_handle)?;
    body.put_u32(0)?;
    Ok(body)
}

fn decode_get_state_request(body: &[u8]) -> Option<i32> {
    let mut c = Cursor::new(body);
    if c.u32()? != READ_VARIANT || c.u64()? != CLIENT {
        return None;
    }
    let handle = c.i32()?;
    if handle >= 0 || c.u32()? != 0 || !c.done() {
        return None;
    }
    Some(handle)
}

/// A DER-dictionary reply: `u32 0`, then the dictionary as a blob (padded to
/// 4), as the Neo's key store answered on Linux on 11-08.
fn response_der(body: &[u8]) -> Option<&[u8]> {
    let mut c = Cursor::new(body);
    if c.u32()? != 0 {
        return None;
    }
    let der = c.blob()?;
    if !c.done() {
        return None;
    }
    Some(der)
}

fn der_u32(der: &[u8], key: &[u8]) -> Option<u32> {
    crate::der::refkey_find(der, key).and_then(crate::der::integer_u32)
}

fn decode_get_state_reply(body: &[u8]) -> Option<GetState> {
    let der = response_der(body)?;
    Some(GetState {
        state_word: der_u32(der, b"ss")?,
        lock_phase: der_u32(der, b"sls")?,
        failed_attempts: der_u32(der, b"sfa")?,
        attempt_limit: der_u32(der, b"mua")?,
        handle: crate::der::refkey_find(der, b"bh").and_then(crate::der::integer_i32)?,
    })
}

fn encode_get_configuration(user_handle: i32) -> Result<crate::image::Body> {
    if user_handle >= 0 {
        return Err(EINVAL);
    }
    let mut body = crate::image::Body::new();
    body.put_u32(READ_VARIANT)?;
    body.put_u64(CLIENT)?;
    body.put_i32(user_handle)?;
    Ok(body)
}

fn decode_get_configuration_request(body: &[u8]) -> Option<i32> {
    let mut c = Cursor::new(body);
    if c.u32()? != READ_VARIANT || c.u64()? != CLIENT {
        return None;
    }
    let handle = c.i32()?;
    (handle < 0 && c.done()).then_some(handle)
}

fn decode_get_configuration_reply(body: &[u8]) -> Option<Configuration> {
    let der = response_der(body)?;
    Some(Configuration {
        grace_period_s: der_u32(der, b"gp")?,
        expiry_time_s: der_u32(der, b"etp")?,
        extended_period_s: der_u32(der, b"epp")?,
        attempt_limit: der_u32(der, b"mua")?,
        biometric_disable_limit: der_u32(der, b"bd")?,
    })
}

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

/// Longest secret accepted by the draft UAPI. The enclave's own limit remains
/// unknown; macOS passes the password bytes as typed.
pub(crate) const SECRET_MAX: usize = 256;

/// The lowest designated user the key store accepts (`DesignateUser`).
pub(crate) const MIN_UID: u32 = crate::sks::SKS_DESIGNATE_USER_MIN as u32;

/// Linux user bags are designated at `USER_BAG_BASE + uid`, never at the
/// uid itself: the Linux identity bag (trusted keys, the machine ref-key,
/// Touch ID's catacomb user) is designated as user 1000, which is also the
/// first desktop user's uid. Offsetting every user bag keeps the two apart
/// for every uid.
pub(crate) const USER_BAG_BASE: u32 = 0x0100_0000;
/// The largest Linux uid with a user bag.
pub(crate) const MAX_UID: u32 = i32::MAX as u32 - USER_BAG_BASE;

/// The key store's designated user for `uid`'s bag.
pub(crate) fn bag_designation(uid: u32) -> Result<crate::sks::DesignateUser> {
    if uid > MAX_UID {
        return Err(EINVAL);
    }
    crate::sks::DesignateUser::new((USER_BAG_BASE + uid) as i32).ok_or(EINVAL)
}

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
    /// Send the bag lock.
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

impl Drop for UnlockRequest {
    fn drop(&mut self) {
        crate::image::wipe(&mut self.secret);
    }
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
    /// The enclave's own state word from GET_DEVICE_STATE.
    pub(crate) enclave_lock_state: u32,
    /// The enclave's lock phase from GET_DEVICE_STATE.
    pub(crate) lock_phase: u32,
}

/// The UAPI `struct sep_keybag_change`.
#[repr(C)]
pub(crate) struct ChangeRequest {
    pub(crate) uid: u32,
    pub(crate) old_len: u32,
    pub(crate) new_len: u32,
    pub(crate) reserved: u32,
    pub(crate) old_secret: [u8; SECRET_MAX],
    pub(crate) new_secret: [u8; SECRET_MAX],
}

impl Drop for ChangeRequest {
    fn drop(&mut self) {
        crate::image::wipe(&mut self.old_secret);
        crate::image::wipe(&mut self.new_secret);
    }
}

static_assert!(core::mem::size_of::<UnlockRequest>() == 8 + SECRET_MAX);
static_assert!(core::mem::size_of::<LockRequest>() == 16);
static_assert!(core::mem::size_of::<StateReply>() == 40);
static_assert!(core::mem::size_of::<ChangeRequest>() == 16 + 2 * SECRET_MAX);

// SAFETY: these C-layout structures contain no padding with uninitialised
// bytes, and every bit pattern of each integer/byte member is valid.
unsafe impl kernel::transmute::FromBytes for UnlockRequest {}
unsafe impl kernel::transmute::FromBytes for LockRequest {}
unsafe impl kernel::transmute::FromBytes for StateReply {}
unsafe impl kernel::transmute::FromBytes for ChangeRequest {}
unsafe impl kernel::transmute::AsBytes for StateReply {}

const IOC_MAGIC: u32 = 0xB1;
pub(crate) const IOC_STATE: u32 = kernel::ioctl::_IOWR::<StateReply>(IOC_MAGIC, 0x30);
pub(crate) const IOC_UNLOCK: u32 = kernel::ioctl::_IOW::<UnlockRequest>(IOC_MAGIC, 0x31);
pub(crate) const IOC_LOCK: u32 = kernel::ioctl::_IOW::<LockRequest>(IOC_MAGIC, 0x32);
pub(crate) const IOC_CHANGE: u32 = kernel::ioctl::_IOW::<ChangeRequest>(IOC_MAGIC, 0x33);
pub(crate) const IOC_CREATE: u32 = kernel::ioctl::_IOW::<UnlockRequest>(IOC_MAGIC, 0x34);
pub(crate) const IOC_EXPORT: u32 = kernel::ioctl::_IOWR::<BagBlob>(IOC_MAGIC, 0x35);
pub(crate) const IOC_IMPORT: u32 = kernel::ioctl::_IOW::<BagBlob>(IOC_MAGIC, 0x36);

/// The largest wrapped user bag `EXPORT`/`IMPORT` carry. A fresh variant-5
/// user bag wraps to 1504 bytes; the bound leaves room for the class keys a
/// bag gains as it is used.
pub(crate) const BAG_BLOB_MAX: usize = 4096;

/// The UAPI `struct sep_keybag_blob`: a user's wrapped bag. It holds no
/// secret: the enclave wrapped it under a key entangled with its own UID
/// key and the bag opens only with the user's password.
/// Only its layout is used: the ioctl handlers copy it to the heap.
#[repr(C)]
#[allow(dead_code)]
pub(crate) struct BagBlob {
    pub(crate) uid: u32,
    pub(crate) len: u32,
    pub(crate) data: [u8; BAG_BLOB_MAX],
}
static_assert!(core::mem::size_of::<BagBlob>() == 8 + BAG_BLOB_MAX);
// SAFETY: C layout, no padding, every bit pattern valid.
unsafe impl kernel::transmute::FromBytes for BagBlob {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for BagBlob {}

/// `struct sep_keybag_blob`'s uid and len, read from a copy of the whole
/// struct (the ioctl handlers keep the 4 KiB blob on the heap).
fn blob_uid(blob: &[u8]) -> u32 {
    u32::from_ne_bytes([blob[0], blob[1], blob[2], blob[3]])
}
fn blob_len(blob: &[u8]) -> usize {
    u32::from_ne_bytes([blob[4], blob[5], blob[6], blob[7]]) as usize
}

/// The largest sealing-key record `SEP_BIO_FDE_*` carry.
pub(crate) const FDE_RECORD_MAX: usize = 2048;
/// The largest volume secret the enclave seals (a LUKS2 key-slot secret).
pub(crate) const FDE_SECRET_MAX: usize = 64;
/// `ephemeral point (65) || ciphertext || tag (16)` for the largest secret.
pub(crate) const FDE_SEALED_MAX: usize = 256;

/// Magic and version at the front of every sealed FDE plaintext.
const FDE_BINDING_MAGIC: [u8; 8] = *b"AURFDEO1";
/// Magic followed by the 16-byte OS install identity, then the secret.
const FDE_BINDING_LEN: usize = FDE_BINDING_MAGIC.len() + 16;

/// Frames a volume secret with the OS install identity it is sealed under.
/// The frame sits inside the ECIES envelope, so the enclave's AEAD covers it.
pub(crate) fn fde_bind(os: &[u8; 16], secret: &[u8]) -> Result<KVec<u8>> {
    if secret.is_empty() || secret.len() > FDE_SECRET_MAX {
        return Err(EINVAL);
    }
    let mut v = KVec::with_capacity(FDE_BINDING_LEN + secret.len(), GFP_KERNEL)?;
    v.extend_from_slice(&FDE_BINDING_MAGIC, GFP_KERNEL)?;
    v.extend_from_slice(os, GFP_KERNEL)?;
    v.extend_from_slice(secret, GFP_KERNEL)?;
    Ok(v)
}

/// The secret inside a frame made by [`fde_bind`], if it was bound to `os`:
/// `EXDEV` for a secret sealed under another OS install, `EKEYREJECTED` for a
/// plaintext without the frame (not sealed by this driver).
pub(crate) fn fde_unbind<'a>(os: &[u8; 16], framed: &'a [u8]) -> Result<&'a [u8]> {
    if framed.len() <= FDE_BINDING_LEN
        || framed.len() > FDE_BINDING_LEN + FDE_SECRET_MAX
        || framed[..FDE_BINDING_MAGIC.len()] != FDE_BINDING_MAGIC
    {
        return Err(EKEYREJECTED);
    }
    if framed[FDE_BINDING_MAGIC.len()..FDE_BINDING_LEN] != os[..] {
        return Err(EXDEV);
    }
    Ok(&framed[FDE_BINDING_LEN..])
}

/// The UAPI `struct sep_fde_key`: a sealing key made under `uid`'s bag.
#[repr(C)]
pub(crate) struct FdeKey {
    pub(crate) uid: u32,
    pub(crate) record_len: u32,
    pub(crate) record: [u8; FDE_RECORD_MAX],
}

/// The UAPI `struct sep_fde_seal`, for both SEAL and UNSEAL.
#[repr(C)]
pub(crate) struct FdeSeal {
    pub(crate) uid: u32,
    pub(crate) record_len: u32,
    pub(crate) secret_len: u32,
    pub(crate) sealed_len: u32,
    pub(crate) secret: [u8; FDE_SECRET_MAX],
    pub(crate) sealed: [u8; FDE_SEALED_MAX],
    pub(crate) record: [u8; FDE_RECORD_MAX],
}

impl Drop for FdeSeal {
    fn drop(&mut self) {
        crate::image::wipe(&mut self.secret);
    }
}

static_assert!(core::mem::size_of::<FdeKey>() == 8 + FDE_RECORD_MAX);
static_assert!(
    core::mem::size_of::<FdeSeal>() == 16 + FDE_SECRET_MAX + FDE_SEALED_MAX + FDE_RECORD_MAX
);
// SAFETY: C layout, no padding, every bit pattern valid.
unsafe impl kernel::transmute::FromBytes for FdeKey {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for FdeKey {}
// SAFETY: as above.
unsafe impl kernel::transmute::FromBytes for FdeSeal {}
// SAFETY: as above.
unsafe impl kernel::transmute::AsBytes for FdeSeal {}

pub(crate) const IOC_FDE_CREATE: u32 = kernel::ioctl::_IOWR::<FdeKey>(IOC_MAGIC, 0x37);
pub(crate) const IOC_FDE_SEAL: u32 = kernel::ioctl::_IOWR::<FdeSeal>(IOC_MAGIC, 0x38);
pub(crate) const IOC_FDE_UNSEAL: u32 = kernel::ioctl::_IOWR::<FdeSeal>(IOC_MAGIC, 0x39);

/// Checks a SEAL/UNSEAL request: the user, the record, and on SEAL the
/// secret, on UNSEAL the sealed blob.
pub(crate) fn parse_fde(req: &FdeSeal, seal: bool) -> Result<crate::sks::DesignateUser> {
    let record_len = req.record_len as usize;
    let secret_len = req.secret_len as usize;
    let sealed_len = req.sealed_len as usize;
    if req.uid < MIN_UID || req.uid > i32::MAX as u32 || record_len == 0 || record_len > FDE_RECORD_MAX {
        return Err(EINVAL);
    }
    if seal && (secret_len == 0 || secret_len > FDE_SECRET_MAX || sealed_len != 0) {
        return Err(EINVAL);
    }
    if !seal && (sealed_len == 0 || sealed_len > FDE_SEALED_MAX || secret_len != 0) {
        return Err(EINVAL);
    }
    crate::sks::DesignateUser::new(req.uid as i32).ok_or(EINVAL)
}

pub(crate) fn is_ioctl(cmd: u32) -> bool {
    matches!(
        cmd,
        IOC_STATE
            | IOC_UNLOCK
            | IOC_LOCK
            | IOC_CHANGE
            | IOC_CREATE
            | IOC_EXPORT
            | IOC_IMPORT
            | IOC_FDE_CREATE
            | IOC_FDE_SEAL
            | IOC_FDE_UNSEAL
    )
}

/// The requests a password unlock in the initrd needs: load the bag saved in
/// the volume's token, unlock it with the typed password, open the sealed
/// key-slot secret, lock the bag again, and read its state. Enrolment and
/// password changes (create, change, export, new or re-sealed keys) wait for
/// the real root, where the host state and Touch ID are attached.
pub(crate) fn is_initrd_ioctl(cmd: u32) -> bool {
    matches!(
        cmd,
        IOC_STATE | IOC_IMPORT | IOC_UNLOCK | IOC_LOCK | IOC_FDE_UNSEAL
    )
}

/// Checks an unlock/create request and returns the user and the secret.
pub(crate) fn parse_unlock(
    req: &UnlockRequest,
    allow_empty: bool,
) -> Result<(crate::sks::DesignateUser, &[u8])> {
    let len = req.secret_len as usize;
    if (!allow_empty && len == 0)
        || len > SECRET_MAX
        || req.uid < MIN_UID
        || req.uid > i32::MAX as u32
    {
        return Err(EINVAL);
    }
    let user = bag_designation(req.uid)?;
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

/// Whether this UAPI request is an explicit, no-grace key-bag lock.
///
/// Only a screen lock may retain keys for a requested grace period.  Apple
/// documents sleep and logout as immediate/password-required boundaries, and
/// the UAPI promises the same for suspend and logout.
pub(crate) fn lock_is_immediate(reason: u32, grace_s: u32) -> bool {
    reason != LOCK_REASON_SCREEN || grace_s == 0
}

pub(crate) fn parse_change(
    req: &ChangeRequest,
) -> Result<(crate::sks::DesignateUser, &[u8], &[u8])> {
    let old_len = req.old_len as usize;
    let new_len = req.new_len as usize;
    if req.reserved != 0
        || old_len == 0
        || old_len > SECRET_MAX
        || new_len == 0
        || new_len > SECRET_MAX
        || req.uid < MIN_UID
        || req.uid > i32::MAX as u32
    {
        return Err(EINVAL);
    }
    let user = bag_designation(req.uid)?;
    Ok((user, &req.old_secret[..old_len], &req.new_secret[..new_len]))
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
        lock_phase: 0,
    }
}

impl crate::SepData {
    fn userbag_request(
        &self,
        name: &'static CStr,
        body: crate::image::Body,
        encode: impl FnOnce(
            crate::sks::Sequence,
            crate::sks::ImageLen,
        ) -> Result<kernel::soc::apple::mailbox::Message>,
    ) -> Result<crate::SksRequest> {
        let img =
            crate::image::build_request(self.sks_ipc_version(), self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(crate::SksRequest {
            name,
            msg: encode(self.sks_next_seq(), len)?,
            img,
        })
    }

    fn userbag_body<'a>(&self, name: &CStr, out: &'a crate::SksOutcome) -> Result<&'a [u8]> {
        let body = self.sks_report_response(name, out);
        if body.is_none() {
            dev_info!(self.dev, "userbag: {}: no usable response (status {})\n", name, out.reply.status);
        }
        body.ok_or(EPROTO)
    }

    /// Logs a reply the decoders refused: which step, its length and its
    /// first bytes. Replies to these queries carry no secret.
    fn userbag_refused(&self, step: &str, body: &[u8]) -> Error {
        let n = body.len().min(96);
        dev_info!(self.dev, "userbag: {}: reply refused, {} bytes: {:02x?}\n", step, body.len(), &body[..n]);
        EPROTO
    }

    fn userbag_transition(
        &self,
        user: crate::sks::DesignateUser,
        state: TransitionState,
        flags: u64,
        secret: &[u8],
    ) -> Result<()> {
        let handle = user.special_handle().value();
        self.sks_step(crate::sks::SKS_DEVICE_STATE_NAME, |_healthy| {
            self.userbag_request(
                crate::sks::SKS_DEVICE_STATE_NAME,
                encode_transition(handle, state, flags, secret)?,
                |seq, len| Ok(crate::sks::encode_sks_device_state_transition(seq, len)),
            )
        })
        .map(|_| ())
        .ok_or(EACCES)
    }

    /// Opens a user's bag through the captured device-state transition.
    pub(crate) fn userbag_unlock(
        &self,
        user: crate::sks::DesignateUser,
        secret: &[u8],
    ) -> Result<()> {
        self.userbag_transition(user, TransitionState::Unlocked, 0, secret)
    }

    pub(crate) fn userbag_lock(
        &self,
        user: crate::sks::DesignateUser,
        screen_lock: bool,
        immediate: bool,
    ) -> Result<()> {
        if immediate {
            let handle = user.special_handle().value();
            return self
                .sks_step(crate::sks::SKS_LOCK_STATE_NAME, |_healthy| {
                    self.userbag_request(
                        crate::sks::SKS_LOCK_STATE_NAME,
                        encode_explicit_lock(handle)?,
                        |seq, len| {
                            crate::sks::encode_sks_change_lock_state(seq, len).ok_or(EINVAL)
                        },
                    )
                })
                .map(|_| ())
                .ok_or(EACCES);
        }

        self.userbag_transition(user, TransitionState::Locked, u64::from(screen_lock), &[])?;
        Ok(())
    }

    pub(crate) fn userbag_change_secret(
        &self,
        user: crate::sks::DesignateUser,
        old: &[u8],
        new: &[u8],
    ) -> Result<()> {
        let handle = user.special_handle().value();
        let out = self
            .sks_step(crate::sks::SKS_CHANGE_SECRET_NAME, |_healthy| {
                self.userbag_request(
                    crate::sks::SKS_CHANGE_SECRET_NAME,
                    encode_change_secret(handle, old, new)?,
                    |seq, len| Ok(crate::sks::encode_sks_change_secret(seq, len)),
                )
            })
            .ok_or(EACCES)?;
        let body = self.userbag_body(crate::sks::SKS_CHANGE_SECRET_NAME, &out)?;
        if !decode_change_secret_reply(body) {
            return Err(EPROTO);
        }
        Ok(())
    }

    /// The designated user bag's wrapped container (`0x02` against `-uid`),
    /// for userspace to keep across reboots, as macOS saves it.
    pub(crate) fn userbag_export(&self, user: crate::sks::DesignateUser) -> Result<KVec<u8>> {
        let out = self.sks_send(self.sks_req_copy_keybag_special(user.special_handle())).ok_or(EACCES)?;
        let wrapped = self.wrapped_from_copy_reply(&out, c"a user bag").ok_or(EACCES)?;
        if wrapped.len() > BAG_BLOB_MAX {
            dev_info!(self.dev, "userbag: wrapped bag is {} bytes, more than {}\n", wrapped.len(), BAG_BLOB_MAX);
            return Err(E2BIG);
        }
        Ok(wrapped)
    }

    /// Loads a saved wrapped user bag and designates it as `-uid` again (it
    /// stays locked until the password unlocks it), then drops the temporary
    /// handle, in macOS's order.
    pub(crate) fn userbag_import(&self, user: crate::sks::DesignateUser, wrapped: &[u8]) -> Result<()> {
        let handle = self.sks_load_wrapped(wrapped).ok_or(EACCES)?;
        let designated = self.sks_step(crate::sks::SKS_DESIGNATE_NAME, |_healthy| {
            self.userbag_request(
                crate::sks::SKS_DESIGNATE_NAME,
                encode_designate(handle.value(), user.special_handle().value())?,
                |seq, len| crate::sks::encode_sks_designate(seq, len).ok_or(EINVAL),
            )
        });
        let valid = designated.as_ref().is_some_and(|out| {
            self.sks_report_response(crate::sks::SKS_DESIGNATE_NAME, out)
                .is_some_and(decode_designate_reply)
        });
        let _ = self.sks_send(self.sks_req_unload_keybag(handle));
        if !valid {
            dev_info!(self.dev, "userbag: designating the imported bag as {} failed\n", user.special_handle().value());
            return Err(EACCES);
        }
        Ok(())
    }

    pub(crate) fn userbag_create(
        &self,
        user: crate::sks::DesignateUser,
        secret: &[u8],
    ) -> Result<()> {
        let _healthy = self
            .sks_health_check(crate::sks::SKS_CREATE_NAME)
            .ok_or(EACCES)?;
        let request = self.userbag_request(
            crate::sks::SKS_CREATE_NAME,
            encode_create(secret)?,
            |seq, len| crate::sks::encode_sks_create(seq, len).ok_or(EINVAL),
        )?;
        let created = crate::xart_guard::keybag_create_window(|| self.sks_send(Ok(request)))
            .ok_or(EACCES)?;
        if created.reply.status != 0 {
            return Err(EACCES);
        }
        let body = self.userbag_body(crate::sks::SKS_CREATE_NAME, &created)?;
        let handle = decode_create_reply(body).ok_or(EPROTO)?;

        let designated = self.sks_step(crate::sks::SKS_DESIGNATE_NAME, |_healthy| {
            self.userbag_request(
                crate::sks::SKS_DESIGNATE_NAME,
                encode_designate(handle.value(), user.special_handle().value())?,
                |seq, len| crate::sks::encode_sks_designate(seq, len).ok_or(EINVAL),
            )
        });
        let valid = designated.as_ref().is_some_and(|out| {
            self.sks_report_response(crate::sks::SKS_DESIGNATE_NAME, out)
                .is_some_and(decode_designate_reply)
        });
        if !valid {
            let _ = self.sks_send(self.sks_req_unload_keybag(handle));
            return Err(EACCES);
        }
        // Copying the wrapped bag into a per-user durable store is phase 2.
        // Until then this ioctl establishes only the live designated bag.
        Ok(())
    }

    /// Tells the key store a grace timer is due (`0x2f`), as the Mac's own
    /// kernel does. True when the store locked `user`'s bag; false while its
    /// grace period has not ended (the store names no handle).
    pub(crate) fn userbag_timer_fire(&self, user: crate::sks::DesignateUser) -> Result<bool> {
        let out = self
            .sks_step(crate::sks::SKS_HANDLE_TIMER_FIRE_NAME, |_healthy| {
                self.userbag_request(
                    crate::sks::SKS_HANDLE_TIMER_FIRE_NAME,
                    encode_timer_fire()?,
                    |seq, len| Ok(crate::sks::encode_sks_handle_timer_fire(seq, len)),
                )
            })
            .ok_or(EACCES)?;
        let body = self.userbag_body(crate::sks::SKS_HANDLE_TIMER_FIRE_NAME, &out)?;
        match decode_timer_fire_reply(body) {
            Some(0) => Ok(false),
            Some(h) if h == user.special_handle().value() => Ok(true),
            fired => {
                dev_info!(self.dev, "userbag: timer fire answered {:?}, expected {}\n", fired, user.special_handle().value());
                Err(self.userbag_refused("timer fire", body))
            }
        }
    }

    /// The enclave's lock phase for `user`'s bag (0 unlocked, 1 locked,
    /// 2 locking, 5 asleep in grace), for the key self-tests.
    pub(crate) fn userbag_lock_phase(&self, user: crate::sks::DesignateUser) -> Result<u32> {
        Ok(self.userbag_state(user)?.lock_phase)
    }

    fn userbag_state(&self, user: crate::sks::DesignateUser) -> Result<GetState> {
        let handle = user.special_handle().value();
        let lock = self
            .sks_step(crate::sks::SKS_LOCK_STATE_NAME, |_healthy| {
                self.userbag_request(
                    crate::sks::SKS_LOCK_STATE_NAME,
                    encode_lock_query(handle)?,
                    |seq, len| crate::sks::encode_sks_change_lock_state(seq, len).ok_or(EINVAL),
                )
            })
            .ok_or(EACCES)?;
        let lock_body = self.userbag_body(crate::sks::SKS_LOCK_STATE_NAME, &lock)?;
        let _lock_state = decode_lock_query_reply(lock_body).ok_or_else(|| self.userbag_refused("lock-state query", lock_body))?;

        let state = self
            .sks_step(crate::sks::SKS_GET_DEVICE_STATE_NAME, |_healthy| {
                self.userbag_request(
                    crate::sks::SKS_GET_DEVICE_STATE_NAME,
                    encode_get_state(handle)?,
                    |seq, len| Ok(crate::sks::encode_sks_get_device_state(seq, len)),
                )
            })
            .ok_or(EACCES)?;
        let body = self.userbag_body(crate::sks::SKS_GET_DEVICE_STATE_NAME, &state)?;
        let state = decode_get_state_reply(body).ok_or_else(|| self.userbag_refused("device state", body))?;
        if state.handle != handle {
            dev_info!(self.dev, "userbag: device state names bag handle {}, asked about {}\n", state.handle, handle);
            return Err(self.userbag_refused("device state (handle)", body));
        }
        Ok(state)
    }

    fn userbag_configuration(&self, user: crate::sks::DesignateUser) -> Result<Configuration> {
        let handle = user.special_handle().value();
        let out = self
            .sks_step(crate::sks::SKS_GET_CONFIGURATION_NAME, |_healthy| {
                self.userbag_request(
                    crate::sks::SKS_GET_CONFIGURATION_NAME,
                    encode_get_configuration(handle)?,
                    |seq, len| Ok(crate::sks::encode_sks_get_configuration(seq, len)),
                )
            })
            .ok_or(EACCES)?;
        let body = self.userbag_body(crate::sks::SKS_GET_CONFIGURATION_NAME, &out)?;
        decode_get_configuration_reply(body).ok_or_else(|| self.userbag_refused("configuration", body))
    }

    pub(crate) fn userbag_ioctl(&self, cmd: u32, arg: usize) -> Result<crate::bio::Handled> {
        crate::bio::require_admin()?;
        if self.profile.key_store != crate::profile::KeyStore::Variant5 {
            // The user-bag layouts were captured on the variant-5 key store
            // (macOS 26 on the Neo). The 13.x key store of M1/M2-generation
            // Macs has not been captured, so nothing is sent there.
            dev_info!(
                self.dev,
                "userbag: per-user key bags are not supported on this key store yet (only variant 5 has been captured)\n"
            );
            return Err(EOPNOTSUPP);
        }
        let user_ptr = UserPtr::from_addr(arg);
        if cmd != IOC_STATE && !crate::bio::idle_for_mutation(&self.bio_session.lock()) {
            return Err(EBUSY);
        }
        match cmd {
            IOC_EXPORT => {
                // The blob is 4 KiB: keep it off the kernel stack.
                let (reader, mut writer) = UserSlice::new(user_ptr, core::mem::size_of::<BagBlob>()).reader_writer();
                let mut blob = KVec::<u8>::with_capacity(core::mem::size_of::<BagBlob>(), GFP_KERNEL)?;
                reader.read_all(&mut blob, GFP_KERNEL)?;
                let uid = blob_uid(&blob);
                let user = bag_designation(uid)?;
                if uid < MIN_UID {
                    return Err(EINVAL);
                }
                let wrapped = self.userbag_export(user)?;
                blob.fill(0);
                blob[..4].copy_from_slice(&uid.to_ne_bytes());
                blob[4..8].copy_from_slice(&(wrapped.len() as u32).to_ne_bytes());
                blob[8..8 + wrapped.len()].copy_from_slice(&wrapped);
                writer.write_slice(&blob)?;
                crate::bio::ok()
            }
            IOC_IMPORT => {
                let mut blob = KVec::<u8>::with_capacity(core::mem::size_of::<BagBlob>(), GFP_KERNEL)?;
                UserSlice::new(user_ptr, core::mem::size_of::<BagBlob>()).reader().read_all(&mut blob, GFP_KERNEL)?;
                let uid = blob_uid(&blob);
                let len = blob_len(&blob);
                if uid < MIN_UID || len == 0 || len > BAG_BLOB_MAX {
                    return Err(EINVAL);
                }
                let user = bag_designation(uid)?;
                if !self.xart_writable() {
                    return Err(EROFS);
                }
                self.userbag_import(user, &blob[8..8 + len])?;
                crate::bio::ok()
            }
            IOC_FDE_CREATE => {
                // 2 KiB: keep the request off the kernel stack.
                let (reader, mut writer) =
                    UserSlice::new(user_ptr, core::mem::size_of::<FdeKey>()).reader_writer();
                let mut buf = KVec::<u8>::with_capacity(core::mem::size_of::<FdeKey>(), GFP_KERNEL)?;
                reader.read_all(&mut buf, GFP_KERNEL)?;
                let key = FdeKey::from_bytes_mut(&mut buf).ok_or(EINVAL)?;
                if key.uid < MIN_UID || key.uid > i32::MAX as u32 {
                    return Err(EINVAL);
                }
                let user = crate::sks::DesignateUser::new(key.uid as i32).ok_or(EINVAL)?;
                let record = self.fde_key_create(user)?;
                if record.len() > FDE_RECORD_MAX {
                    return Err(E2BIG);
                }
                key.record.fill(0);
                key.record[..record.len()].copy_from_slice(&record);
                key.record_len = record.len() as u32;
                writer.write_slice(&buf)?;
                crate::bio::ok()
            }
            IOC_FDE_SEAL | IOC_FDE_UNSEAL => {
                let seal = cmd == IOC_FDE_SEAL;
                // 2.4 KiB: keep the request off the kernel stack. It carries the
                // volume secret, so the buffer is wiped when it is dropped.
                let (reader, mut writer) =
                    UserSlice::new(user_ptr, core::mem::size_of::<FdeSeal>()).reader_writer();
                let mut buf = crate::Secret(KVec::<u8>::with_capacity(
                    core::mem::size_of::<FdeSeal>(),
                    GFP_KERNEL,
                )?);
                reader.read_all(&mut buf.0, GFP_KERNEL)?;
                let req = FdeSeal::from_bytes_mut(&mut buf.0).ok_or(EINVAL)?;
                let user = parse_fde(req, seal)?;
                let record_len = req.record_len as usize;
                if seal {
                    let sealed = self.fde_seal(
                        user,
                        &req.record[..record_len],
                        &req.secret[..req.secret_len as usize],
                    )?;
                    if sealed.len() > FDE_SEALED_MAX {
                        return Err(E2BIG);
                    }
                    req.sealed.fill(0);
                    req.sealed[..sealed.len()].copy_from_slice(&sealed);
                    req.sealed_len = sealed.len() as u32;
                    crate::image::wipe(&mut req.secret);
                    req.secret_len = 0;
                } else {
                    let secret = crate::Secret(self.fde_unseal(
                        user,
                        &req.record[..record_len],
                        &req.sealed[..req.sealed_len as usize],
                    )?);
                    if secret.len() > FDE_SECRET_MAX {
                        return Err(E2BIG);
                    }
                    req.secret.fill(0);
                    req.secret[..secret.len()].copy_from_slice(&secret);
                    req.secret_len = secret.len() as u32;
                }
                writer.write_slice(&buf)?;
                crate::bio::ok()
            }
            IOC_CREATE | IOC_UNLOCK => {
                let request: UnlockRequest =
                    UserSlice::new(user_ptr, core::mem::size_of::<UnlockRequest>())
                        .reader()
                        .read()?;
                let (user, secret) = parse_unlock(&request, cmd == IOC_UNLOCK)?;
                if cmd == IOC_CREATE {
                    if !self.xart_writable() {
                        return Err(EROFS);
                    }
                    self.userbag_create(user, secret)?;
                } else {
                    self.userbag_unlock(user, secret)?;
                }
                crate::bio::ok()
            }
            IOC_LOCK => {
                let request: LockRequest =
                    UserSlice::new(user_ptr, core::mem::size_of::<LockRequest>())
                        .reader()
                        .read()?;
                let (uid, reason, grace_s) = parse_lock(&request)?;
                let user = bag_designation(uid)?;
                let screen = reason == LOCK_REASON_SCREEN;
                self.userbag_lock(user, screen, lock_is_immediate(reason, grace_s))?;
                crate::bio::ok()
            }
            IOC_CHANGE => {
                let request: ChangeRequest =
                    UserSlice::new(user_ptr, core::mem::size_of::<ChangeRequest>())
                        .reader()
                        .read()?;
                let (user, old, new) = parse_change(&request)?;
                self.userbag_change_secret(user, old, new)?;
                crate::bio::ok()
            }
            IOC_STATE => {
                let mut reply: StateReply =
                    UserSlice::new(user_ptr, core::mem::size_of::<StateReply>())
                        .reader()
                        .read()?;
                let user =
                    bag_designation(reply.uid).ok()
                        .ok_or(EINVAL)?;
                let state = self.userbag_state(user)?;
                let configuration = self.userbag_configuration(user)?;
                if state.attempt_limit != configuration.attempt_limit {
                    dev_info!(self.dev, "userbag: attempt limit {} in the state, {} in the configuration\n", state.attempt_limit, configuration.attempt_limit);
                    return Err(EPROTO);
                }
                reply.bag_state = match state.lock_phase {
                    0 => BagState::Unlocked.uapi(),
                    2 | 5 => BagState::Locking { deadline_s: 0 }.uapi(),
                    _ => BagState::Locked.uapi(),
                };
                reply.biometry = 0;
                reply.failed_matches = state.failed_attempts;
                reply.since_password_s = u64::MAX;
                reply.since_unlock_s = u64::MAX;
                reply.enclave_lock_state = state.state_word;
                reply.lock_phase = state.lock_phase;
                UserSlice::new(user_ptr, core::mem::size_of::<StateReply>())
                    .writer()
                    .write(&reply)?;
                crate::bio::ok()
            }
            _ => Err(ENOTTY),
        }
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
    fn initrd_allows_only_the_password_unlock() {
        for cmd in [IOC_STATE, IOC_IMPORT, IOC_UNLOCK, IOC_LOCK, IOC_FDE_UNSEAL] {
            assert!(is_initrd_ioctl(cmd));
            assert!(is_ioctl(cmd));
        }
        for cmd in [IOC_CREATE, IOC_CHANGE, IOC_EXPORT, IOC_FDE_CREATE, IOC_FDE_SEAL] {
            assert!(!is_initrd_ioctl(cmd));
        }
    }

    const USER_HANDLE: i32 = -501;
    const CLIENT_LE: [u8; 8] = [0x53, 0x4b, 0x53, 0x58, 0x55, 0x4e, 0x49, 0x4c];

    #[test]
    fn create_layout_golden_and_round_trip() {
        let body = encode_create(b"pw").expect("create");
        let mut golden = [0u8; 48];
        golden[0..4].copy_from_slice(&4u32.to_le_bytes());
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[16..20].copy_from_slice(&(-1i32).to_le_bytes());
        golden[20..24].copy_from_slice(&2u32.to_le_bytes());
        golden[24..26].copy_from_slice(b"pw");
        assert_eq!(body.as_slice(), &golden);
        assert_eq!(
            decode_create(body.as_slice()),
            Some(Create { secret: b"pw" })
        );
        assert_eq!(
            decode_create_reply(&[4, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0])
                .unwrap()
                .value(),
            7
        );
    }

    #[test]
    fn designate_layout_golden_and_round_trip() {
        let body = encode_designate(1, USER_HANDLE).expect("designate");
        let mut golden = [0u8; 32];
        golden[0..4].copy_from_slice(&1u32.to_le_bytes());
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&1i32.to_le_bytes());
        golden[16..20].copy_from_slice(&USER_HANDLE.to_le_bytes());
        assert_eq!(body.as_slice(), &golden);
        assert_eq!(
            decode_designate(body.as_slice()),
            Some(Designate {
                source_handle: 1,
                user_handle: USER_HANDLE
            })
        );
        assert!(decode_designate_reply(&1u32.to_le_bytes()));
    }

    #[test]
    fn device_state_layouts_golden_and_round_trip() {
        let lock =
            encode_transition(USER_HANDLE, TransitionState::Locked, 1, &[]).expect("screen lock");
        let mut golden = [0u8; 32];
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        golden[16..20].copy_from_slice(&1u32.to_le_bytes());
        golden[20..28].copy_from_slice(&1u64.to_le_bytes());
        assert_eq!(lock.as_slice(), &golden);
        assert_eq!(
            decode_transition(lock.as_slice()),
            Some(Transition {
                user_handle: USER_HANDLE,
                state: TransitionState::Locked,
                flags: 1,
                secret: &[],
            })
        );
        let sleep =
            encode_transition(USER_HANDLE, TransitionState::Locked, 0, &[]).expect("sleep lock");
        assert_eq!(decode_transition(sleep.as_slice()).unwrap().flags, 0);

        let unlock =
            encode_transition(USER_HANDLE, TransitionState::Unlocked, 0, b"pw").expect("unlock");
        let mut golden = [0u8; 36];
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        golden[28..32].copy_from_slice(&2u32.to_le_bytes());
        golden[32..34].copy_from_slice(b"pw");
        assert_eq!(unlock.as_slice(), &golden);
        assert_eq!(decode_transition(unlock.as_slice()).unwrap().secret, b"pw");
        let cancel = encode_transition(USER_HANDLE, TransitionState::Unlocked, 0, &[])
            .expect("grace cancel");
        assert!(decode_transition(cancel.as_slice()).unwrap().secret.is_empty());
    }

    #[test]
    fn grace_expiry_layout_golden_and_round_trip() {
        let body = encode_timer_fire().expect("timer");
        let golden = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(body.as_slice(), &golden);
        assert!(decode_timer_fire(body.as_slice()));
        let mut reply = [0u8; 24];
        reply[4..8].copy_from_slice(&USER_HANDLE.to_le_bytes());
        reply[8..12].copy_from_slice(&1u32.to_le_bytes());
        reply[16..20].copy_from_slice(&0x027e_4cb2u32.to_le_bytes());
        assert_eq!(decode_timer_fire_reply(&reply), Some(USER_HANDLE));
        // Before the grace period ends the store names no handle.
        reply[4..8].copy_from_slice(&0i32.to_le_bytes());
        assert_eq!(decode_timer_fire_reply(&reply), Some(0));
        // When the bag locks, the store names it and the last word is not 0.
        reply[4..8].copy_from_slice(&USER_HANDLE.to_le_bytes());
        reply[20..24].copy_from_slice(&3u32.to_le_bytes());
        assert_eq!(decode_timer_fire_reply(&reply), Some(USER_HANDLE));
        reply[0] = 1;
        assert_eq!(decode_timer_fire_reply(&reply), None);
    }

    #[test]
    fn change_secret_layout_golden_and_round_trip() {
        let body = encode_change_secret(USER_HANDLE, b"old", b"new").expect("change");
        let mut golden = [0u8; 52];
        golden[0..4].copy_from_slice(&3u32.to_le_bytes());
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        golden[16..20].copy_from_slice(&3u32.to_le_bytes());
        golden[20..23].copy_from_slice(b"old");
        golden[24..28].copy_from_slice(&3u32.to_le_bytes());
        golden[28..31].copy_from_slice(b"new");
        golden[40..44].copy_from_slice(&0xffu32.to_le_bytes());
        assert_eq!(body.as_slice(), &golden);
        assert_eq!(
            decode_change_secret(body.as_slice()),
            Some(ChangeSecret {
                user_handle: USER_HANDLE,
                old: b"old",
                new: b"new"
            })
        );
        assert!(decode_change_secret_reply(&[
            3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        ]));
    }

    #[test]
    fn lock_query_layout_golden_and_round_trip() {
        let body = encode_lock_query(USER_HANDLE).expect("lock query");
        let mut golden = [0u8; 32];
        golden[0..4].copy_from_slice(&1u32.to_le_bytes());
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        golden[16..20].copy_from_slice(&(-1i32).to_le_bytes());
        assert_eq!(body.as_slice(), &golden);
        assert_eq!(decode_lock_query(body.as_slice()), Some(USER_HANDLE));
        assert_eq!(
            decode_lock_query_reply(&[1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
            Some(1)
        );

        let lock = encode_explicit_lock(USER_HANDLE).expect("explicit lock");
        let mut golden = [0u8; 32];
        golden[0..4].copy_from_slice(&LOCK_STATE_VARIANT.to_le_bytes());
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        golden[16..20].copy_from_slice(&LockStateChange::Locked.wire().to_le_bytes());
        assert_eq!(lock.as_slice(), &golden);
    }

    #[test]
    fn get_state_layout_and_der_reply() {
        let body = encode_get_state(USER_HANDLE).expect("get state");
        let mut golden = [0u8; 20];
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        assert_eq!(body.as_slice(), &golden);
        assert_eq!(decode_get_state_request(body.as_slice()), Some(USER_HANDLE));

        // Canonical SET members, sorted by key: bh, mua, sfa, sls, ss.
        let der = [
            0x31, 0x34, 0x30, 0x08, 0x0c, 0x02, b'b', b'h', 0x02, 0x02, 0xfe, 0x0b, 0x30, 0x08,
            0x0c, 0x03, b'm', b'u', b'a', 0x02, 0x01, 10, 0x30, 0x08, 0x0c, 0x03, b's', b'f', b'a',
            0x02, 0x01, 3, 0x30, 0x08, 0x0c, 0x03, b's', b'l', b's', 0x02, 0x01, 2, 0x30, 0x0a,
            0x0c, 0x02, b's', b's', 0x02, 0x04, 0x06, 0, 0, 0x15,
        ];
        let mut response = crate::image::Body::new();
        response.put_u32(0).unwrap();
        response.put_blob(&der).unwrap();
        assert_eq!(
            decode_get_state_reply(response.as_slice()),
            Some(GetState {
                state_word: 0x0600_0015,
                lock_phase: 2,
                failed_attempts: 3,
                attempt_limit: 10,
                handle: USER_HANDLE,
            })
        );
    }

    #[test]
    fn configuration_layout_and_der_reply() {
        let body = encode_get_configuration(USER_HANDLE).expect("get config");
        let mut golden = [0u8; 16];
        golden[4..12].copy_from_slice(&CLIENT_LE);
        golden[12..16].copy_from_slice(&USER_HANDLE.to_le_bytes());
        assert_eq!(body.as_slice(), &golden);
        assert_eq!(
            decode_get_configuration_request(body.as_slice()),
            Some(USER_HANDLE)
        );

        // Canonical SET members, sorted by key: bd, epp, etp, gp, mua.
        let der = [
            0x31, 0x35, 0x30, 0x07, 0x0c, 0x02, b'b', b'd', 0x02, 0x01, 5, 0x30, 0x0a, 0x0c, 0x03,
            b'e', b'p', b'p', 0x02, 0x03, 0x09, 0x3a, 0x80, 0x30, 0x0a, 0x0c, 0x03, b'e', b't',
            b'p', 0x02, 0x03, 0x02, 0xa3, 0x00, 0x30, 0x08, 0x0c, 0x02, b'g', b'p', 0x02, 0x02,
            0x01, 0x2c, 0x30, 0x08, 0x0c, 0x03, b'm', b'u', b'a', 0x02, 0x01, 10,
        ];
        let mut response = crate::image::Body::new();
        response.put_u32(0).unwrap();
        response.put_blob(&der).unwrap();
        assert_eq!(
            decode_get_configuration_reply(response.as_slice()),
            Some(Configuration {
                grace_period_s: 300,
                expiry_time_s: 172800,
                extended_period_s: 604800,
                attempt_limit: 10,
                biometric_disable_limit: 5,
            })
        );
    }

    #[test]
    fn protocol_decoders_reject_trailing_or_nonzero_reserved_data() {
        let create = encode_create(b"pw").expect("create");
        let mut bad = [0u8; 48];
        bad.copy_from_slice(create.as_slice());
        bad[47] = 1;
        assert!(decode_create(&bad).is_none());
        assert!(decode_lock_query_reply(&[1, 0, 0, 0, 1, 0, 0, 0]).is_none());
        assert!(encode_transition(USER_HANDLE, TransitionState::Locked, 2, &[]).is_err());
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
        let (user, secret) = parse_unlock(&req, false).expect("valid request");
        // uid 1000's bag is not the identity bag's designation (1000).
        assert_eq!(user.special_handle().value(), -((USER_BAG_BASE + 1000) as i32));
        assert_eq!(secret.len(), 4);
        req.secret_len = 0;
        assert!(parse_unlock(&req, false).is_err());
        assert!(parse_unlock(&req, true).is_ok());
        req.secret_len = SECRET_MAX as u32 + 1;
        assert!(parse_unlock(&req, false).is_err());
        req.secret_len = 4;
        req.uid = 0;
        assert!(parse_unlock(&req, false).is_err());
        req.uid = u32::MAX;
        assert!(parse_unlock(&req, false).is_err());

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
    fn only_a_screen_lock_with_grace_uses_the_grace_transition() {
        assert!(!lock_is_immediate(LOCK_REASON_SCREEN, 300));
        assert!(lock_is_immediate(LOCK_REASON_SCREEN, 0));
        assert!(lock_is_immediate(LOCK_REASON_SUSPEND, 300));
        assert!(lock_is_immediate(LOCK_REASON_LOGOUT, 0));
        assert!(lock_is_immediate(LOCK_REASON_LOGOUT, 300));
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

    fn fde_request(uid: u32, record_len: u32, secret_len: u32, sealed_len: u32) -> FdeSeal {
        FdeSeal {
            uid,
            record_len,
            secret_len,
            sealed_len,
            secret: [0; FDE_SECRET_MAX],
            sealed: [0; FDE_SEALED_MAX],
            record: [0; FDE_RECORD_MAX],
        }
    }

    #[test]
    fn fde_secret_is_bound_to_its_os_install() {
        let a = [0x11u8; 16];
        let b = [0x22u8; 16];
        let secret = [0x5au8; 32];
        let framed = fde_bind(&a, &secret).expect("bind");
        assert_eq!(framed.len(), FDE_BINDING_LEN + secret.len());
        assert_eq!(fde_unbind(&a, &framed).expect("same install"), &secret[..]);
        assert_eq!(fde_unbind(&b, &framed), Err(EXDEV));
        // A plaintext without the frame (an older draft's blob) is refused.
        assert_eq!(fde_unbind(&a, &secret), Err(EKEYREJECTED));
        let mut bad = [0u8; FDE_BINDING_LEN + 32];
        bad.copy_from_slice(&framed);
        bad[0] ^= 1;
        assert_eq!(fde_unbind(&a, &bad), Err(EKEYREJECTED));
        assert_eq!(fde_unbind(&a, &framed[..FDE_BINDING_LEN]), Err(EKEYREJECTED));
        assert!(fde_bind(&a, &[]).is_err());
        assert!(fde_bind(&a, &[0u8; FDE_SECRET_MAX + 1]).is_err());
        assert!(fde_bind(&a, &[0u8; FDE_SECRET_MAX]).is_ok());
    }

    #[test]
    fn fde_parse_bounds() {
        let ok = MIN_UID + 1;
        assert!(parse_fde(&fde_request(ok, 600, 32, 0), true).is_ok());
        assert!(parse_fde(&fde_request(ok, 600, 0, 113), false).is_ok());
        // SEAL needs a secret and no sealed blob, UNSEAL the reverse.
        assert!(parse_fde(&fde_request(ok, 600, 0, 0), true).is_err());
        assert!(parse_fde(&fde_request(ok, 600, 32, 113), true).is_err());
        assert!(parse_fde(&fde_request(ok, 600, 32, 113), false).is_err());
        assert!(parse_fde(&fde_request(ok, 600, 0, 0), false).is_err());
        // Lengths past the arrays and system uids are refused.
        assert!(parse_fde(&fde_request(ok, 600, FDE_SECRET_MAX as u32 + 1, 0), true).is_err());
        assert!(parse_fde(&fde_request(ok, 600, 0, FDE_SEALED_MAX as u32 + 1), false).is_err());
        assert!(parse_fde(&fde_request(ok, 0, 32, 0), true).is_err());
        assert!(parse_fde(&fde_request(ok, FDE_RECORD_MAX as u32 + 1, 32, 0), true).is_err());
        assert!(parse_fde(&fde_request(MIN_UID - 1, 600, 32, 0), true).is_err());
        assert!(parse_fde(&fde_request(u32::MAX, 600, 32, 0), true).is_err());
    }

    #[test]
    fn fde_ioctls_are_userbag_ioctls() {
        assert!(is_ioctl(IOC_FDE_CREATE));
        assert!(is_ioctl(IOC_FDE_SEAL));
        assert!(is_ioctl(IOC_FDE_UNSEAL));
        assert_ne!(IOC_FDE_SEAL, IOC_FDE_UNSEAL);
        // The largest sealed secret fits: point || ciphertext || tag.
        assert!(65 + FDE_SECRET_MAX + 16 <= FDE_SEALED_MAX);
    }
}
