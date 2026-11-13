// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! Typed operations of the enclave's trusted-accessory service (`stac`).
//!
//! The service pairs the enclave with an accessory that carries its own
//! fingerprint sensor (a keyboard with Touch ID) and tracks whether that
//! accessory is connected and allowed to authenticate. The host only carries
//! public points, attestations, certificates and manifests between the
//! accessory and the enclave; it never holds a private or session key, and it
//! cannot declare an accessory paired or authorised.
//!
//! Every operation is one generic-transfer exchange (see `transfer.rs`) on the
//! endpoint's out-of-line buffers. The opcodes below are the ones recorded on a
//! J616s under macOS 27.0 while a keyboard was paired, enabled for Touch ID,
//! disconnected and reconnected, forgotten and paired again, and switched off
//! and on. The body lengths in those recordings match the layouts here
//! exactly:
//!
//! ```text
//! op    request  reply       when
//! 0x01  0        133         before pairing
//! 0x02  16       2           around every state change
//! 0x03  146      69 + 1509   pairing, first half
//! 0x15  3619     0           pairing, second half (1625 + 1994-byte certificate)
//! 0x05  2412     0           after pairing (21 + 2391-byte manifest)
//! 0x14  60       0           enabling the accessory for Touch ID
//! 0x09  17       0           link down / link up
//! 0x0b  0        20          paired list, one accessory
//! 0x04  16       0           forgetting the accessory
//! 0x06  81       69 + 159    per-connection session, first half
//! 0x07  1556     0           per-connection session, second half
//! ```
//!
//! The per-connection session (a manifest with `after_pairing` clear, then
//! 0x06 and 0x07) ran when the accessory came back after being switched off.
//! It did not run at pairing or when the accessory moved between USB and
//! Bluetooth.
//!
//! Operations the recording does not contain are not defined here.
//!
//! Nothing in this module logs or formats an accessory identity, a key, an
//! attestation, a certificate, a manifest or an ACM handle.

use kernel::prelude::*;

/// An operation of the service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub(crate) enum Op {
    /// Reads the enclave's public attestation object for the accessory.
    HostAttestationObject = 0x01,
    /// Asks whether an accessory is paired and authorised.
    PairingState = 0x02,
    /// Starts a pairing with the accessory's identity and ephemeral points.
    PairingBegin = 0x03,
    /// Forgets one paired accessory.
    Unpair = 0x04,
    /// Hands the enclave the accessory's manifest.
    Manifest = 0x05,
    /// Starts a per-connection session with the accessory's ephemeral point.
    SessionBegin = 0x06,
    /// Finishes a per-connection session with the accessory's material.
    SessionFinish = 0x07,
    /// Tells the enclave the accessory's link went down or came back.
    ConnectionState = 0x09,
    /// Lists the paired accessories.
    PairedList = 0x0b,
    /// Reports the system power state (see `stac.rs`).
    PowerState = 0x0f,
    /// Enables a paired accessory for biometric use under an ACM context.
    Authorize = 0x14,
    /// Finishes a pairing with the accessory's attestation and certificate.
    PairingFinish = 0x15,
}

impl Op {
    pub(crate) const fn opcode(self) -> u16 {
        self as u16
    }
}

/// The accessory's 16-byte identity. Opaque; never printed.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct AccessoryId(pub(crate) [u8; ACCESSORY_ID_LEN]);

impl core::fmt::Debug for AccessoryId {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str("AccessoryId(..)")
    }
}

/// An uncompressed P-256 public point: public handshake material.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct PublicPoint(pub(crate) [u8; PUBLIC_POINT_LEN]);

impl core::fmt::Debug for PublicPoint {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str("PublicPoint(..)")
    }
}

pub(crate) const ACCESSORY_ID_LEN: usize = 16;
pub(crate) const PUBLIC_POINT_LEN: usize = 65;
const WORD: usize = 4;

/// The host attestation object reply: a length word and a fixed slot.
pub(crate) const HOST_OBJECT_SLOT: usize = 129;
pub(crate) const HOST_OBJECT_REPLY_LEN: usize = WORD + HOST_OBJECT_SLOT;

pub(crate) const PAIRING_STATE_REPLY_LEN: usize = 2;

pub(crate) const PAIRING_BEGIN_REQUEST_LEN: usize = ACCESSORY_ID_LEN + 2 * PUBLIC_POINT_LEN;
/// The reply: the enclave's ephemeral point, a length word, the attestation.
pub(crate) const PAIRING_BEGIN_REPLY_FIXED: usize = PUBLIC_POINT_LEN + WORD;

/// The accessory's attestation travels in a fixed, zero-padded slot.
pub(crate) const ACCESSORY_ATTESTATION_SLOT: usize = 1536;
pub(crate) const ACCESSORY_CERTIFICATE_MAX: usize = 2048;
pub(crate) const PAIRING_FINISH_FIXED: usize =
    ACCESSORY_ID_LEN + PUBLIC_POINT_LEN + WORD + ACCESSORY_ATTESTATION_SLOT + WORD;

pub(crate) const MANIFEST_MAX: usize = 2560;
pub(crate) const MANIFEST_FIXED: usize = ACCESSORY_ID_LEN + 1 + WORD;

pub(crate) const CONNECTION_STATE_REQUEST_LEN: usize = ACCESSORY_ID_LEN + 1;

pub(crate) const UNPAIR_REQUEST_LEN: usize = ACCESSORY_ID_LEN;

pub(crate) const SESSION_BEGIN_REQUEST_LEN: usize = ACCESSORY_ID_LEN + PUBLIC_POINT_LEN;
/// The reply: the enclave's ephemeral point, a length word, the attestation.
pub(crate) const SESSION_BEGIN_REPLY_FIXED: usize = PUBLIC_POINT_LEN + WORD;
/// The accessory's session material travels in a fixed, zero-padded slot.
pub(crate) const SESSION_MATERIAL_SLOT: usize = 1536;
pub(crate) const SESSION_FINISH_REQUEST_LEN: usize =
    ACCESSORY_ID_LEN + WORD + SESSION_MATERIAL_SLOT;

/// The ACM context handle travels in a fixed, zero-padded slot.
pub(crate) const ACM_HANDLE_SLOT: usize = 32;
pub(crate) const AUTHORIZE_FIXED: usize = WORD + ACCESSORY_ID_LEN + WORD + ACM_HANDLE_SLOT + WORD;

// The lengths in the recording.
static_assert!(HOST_OBJECT_REPLY_LEN == 133);
static_assert!(PAIRING_BEGIN_REQUEST_LEN == 146);
static_assert!(PAIRING_FINISH_FIXED + 1994 == 3619);
static_assert!(MANIFEST_FIXED + 2391 == 2412);
static_assert!(CONNECTION_STATE_REQUEST_LEN == 17);
static_assert!(UNPAIR_REQUEST_LEN == 16);
static_assert!(SESSION_BEGIN_REQUEST_LEN == 81);
static_assert!(SESSION_BEGIN_REPLY_FIXED + 159 == 228);
static_assert!(SESSION_FINISH_REQUEST_LEN == 1556);
static_assert!(SESSION_BEGIN_REPLY_FIXED == PAIRING_BEGIN_REPLY_FIXED);
static_assert!(AUTHORIZE_FIXED == 60);

/// Why a reply was not taken.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ParseError {
    /// The body is not the length the operation answers with.
    Length,
    /// A length field is out of its bound or disagrees with the body.
    Field,
    /// A flag byte is neither 0 nor 1.
    Flag,
}

fn flag(b: u8) -> core::result::Result<bool, ParseError> {
    match b {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(ParseError::Flag),
    }
}

fn word(b: &[u8], at: usize) -> core::result::Result<usize, ParseError> {
    let w = b.get(at..at + WORD).ok_or(ParseError::Length)?;
    Ok(u32::from_le_bytes([w[0], w[1], w[2], w[3]]) as usize)
}

fn push_word(out: &mut KVec<u8>, v: usize) -> Result {
    let v = u32::try_from(v).map_err(|_| EINVAL)?;
    out.extend_from_slice(&v.to_le_bytes(), GFP_KERNEL)?;
    Ok(())
}

/// What the enclave says about one accessory.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PairingState {
    pub(crate) paired: bool,
    /// Enabled for biometric use.
    pub(crate) authorized: bool,
}

pub(crate) fn pairing_state_request(id: &AccessoryId) -> [u8; ACCESSORY_ID_LEN] {
    id.0
}

pub(crate) fn parse_pairing_state(
    body: &[u8],
) -> core::result::Result<PairingState, ParseError> {
    if body.len() != PAIRING_STATE_REPLY_LEN {
        return Err(ParseError::Length);
    }
    let state = PairingState {
        paired: flag(body[0])?,
        authorized: flag(body[1])?,
    };
    // Authorised implies paired; anything else is not a state the service has.
    if state.authorized && !state.paired {
        return Err(ParseError::Field);
    }
    Ok(state)
}

/// The meaningful length of the host attestation object in its slot.
pub(crate) fn parse_host_object_len(body: &[u8]) -> core::result::Result<usize, ParseError> {
    if body.len() != HOST_OBJECT_REPLY_LEN {
        return Err(ParseError::Length);
    }
    let len = word(body, 0)?;
    if len == 0 || len > HOST_OBJECT_SLOT {
        return Err(ParseError::Field);
    }
    Ok(len)
}

pub(crate) fn pairing_begin_request(
    id: &AccessoryId,
    identity: &PublicPoint,
    ephemeral: &PublicPoint,
) -> [u8; PAIRING_BEGIN_REQUEST_LEN] {
    let mut out = [0u8; PAIRING_BEGIN_REQUEST_LEN];
    out[..ACCESSORY_ID_LEN].copy_from_slice(&id.0);
    out[ACCESSORY_ID_LEN..ACCESSORY_ID_LEN + PUBLIC_POINT_LEN].copy_from_slice(&identity.0);
    out[ACCESSORY_ID_LEN + PUBLIC_POINT_LEN..].copy_from_slice(&ephemeral.0);
    out
}

/// Where the enclave's answer to a pairing start sits in its body.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PairingBeginReply {
    /// The enclave's ephemeral point: `body[0..65]`.
    pub(crate) host_ephemeral: core::ops::Range<usize>,
    /// The enclave's attestation for the accessory to check.
    pub(crate) host_attestation: core::ops::Range<usize>,
}

pub(crate) fn parse_pairing_begin(
    body: &[u8],
) -> core::result::Result<PairingBeginReply, ParseError> {
    if body.len() < PAIRING_BEGIN_REPLY_FIXED {
        return Err(ParseError::Length);
    }
    let len = word(body, PUBLIC_POINT_LEN)?;
    let total = PAIRING_BEGIN_REPLY_FIXED
        .checked_add(len)
        .ok_or(ParseError::Field)?;
    if len == 0 || total != body.len() {
        return Err(ParseError::Field);
    }
    Ok(PairingBeginReply {
        host_ephemeral: 0..PUBLIC_POINT_LEN,
        host_attestation: PAIRING_BEGIN_REPLY_FIXED..total,
    })
}

pub(crate) fn pairing_finish_request(
    id: &AccessoryId,
    identity: &PublicPoint,
    attestation: &[u8],
    certificate: &[u8],
) -> Result<KVec<u8>> {
    if attestation.is_empty()
        || attestation.len() > ACCESSORY_ATTESTATION_SLOT
        || certificate.is_empty()
        || certificate.len() > ACCESSORY_CERTIFICATE_MAX
    {
        return Err(EINVAL);
    }
    let mut out = KVec::with_capacity(PAIRING_FINISH_FIXED + certificate.len(), GFP_KERNEL)?;
    out.extend_from_slice(&id.0, GFP_KERNEL)?;
    out.extend_from_slice(&identity.0, GFP_KERNEL)?;
    push_word(&mut out, attestation.len())?;
    out.extend_from_slice(attestation, GFP_KERNEL)?;
    out.resize(
        ACCESSORY_ID_LEN + PUBLIC_POINT_LEN + WORD + ACCESSORY_ATTESTATION_SLOT,
        0,
        GFP_KERNEL,
    )?;
    push_word(&mut out, certificate.len())?;
    out.extend_from_slice(certificate, GFP_KERNEL)?;
    Ok(out)
}

pub(crate) fn manifest_request(
    id: &AccessoryId,
    after_pairing: bool,
    manifest: &[u8],
) -> Result<KVec<u8>> {
    if manifest.is_empty() || manifest.len() > MANIFEST_MAX {
        return Err(EINVAL);
    }
    let mut out = KVec::with_capacity(MANIFEST_FIXED + manifest.len(), GFP_KERNEL)?;
    out.extend_from_slice(&id.0, GFP_KERNEL)?;
    out.push(u8::from(after_pairing), GFP_KERNEL)?;
    push_word(&mut out, manifest.len())?;
    out.extend_from_slice(manifest, GFP_KERNEL)?;
    Ok(out)
}

pub(crate) fn connection_state_request(
    id: &AccessoryId,
    connected: bool,
) -> [u8; CONNECTION_STATE_REQUEST_LEN] {
    let mut out = [0u8; CONNECTION_STATE_REQUEST_LEN];
    out[..ACCESSORY_ID_LEN].copy_from_slice(&id.0);
    out[ACCESSORY_ID_LEN] = u8::from(connected);
    out
}

pub(crate) fn unpair_request(id: &AccessoryId) -> [u8; UNPAIR_REQUEST_LEN] {
    id.0
}

/// Opens a per-connection session: the accessory's identity and the fresh
/// ephemeral point it generated for this connection.
pub(crate) fn session_begin_request(
    id: &AccessoryId,
    ephemeral: &PublicPoint,
) -> [u8; SESSION_BEGIN_REQUEST_LEN] {
    let mut out = [0u8; SESSION_BEGIN_REQUEST_LEN];
    out[..ACCESSORY_ID_LEN].copy_from_slice(&id.0);
    out[ACCESSORY_ID_LEN..].copy_from_slice(&ephemeral.0);
    out
}

/// Where the enclave's answer to a session start sits in its body. The
/// layout is the pairing start's: a point, a length word, an attestation.
pub(crate) type SessionBeginReply = PairingBeginReply;

pub(crate) fn parse_session_begin(
    body: &[u8],
) -> core::result::Result<SessionBeginReply, ParseError> {
    parse_pairing_begin(body)
}

/// Closes a per-connection session with the material the accessory answered
/// the enclave's attestation with. The material is opaque to the host.
pub(crate) fn session_finish_request(
    id: &AccessoryId,
    material: &[u8],
) -> Result<KVec<u8>> {
    if material.is_empty() || material.len() > SESSION_MATERIAL_SLOT {
        return Err(EINVAL);
    }
    let mut out = KVec::with_capacity(SESSION_FINISH_REQUEST_LEN, GFP_KERNEL)?;
    out.extend_from_slice(&id.0, GFP_KERNEL)?;
    push_word(&mut out, material.len())?;
    out.extend_from_slice(material, GFP_KERNEL)?;
    out.resize(SESSION_FINISH_REQUEST_LEN, 0, GFP_KERNEL)?;
    Ok(out)
}

/// Parses the paired list into `out`; returns how many identities it holds.
pub(crate) fn parse_paired_list(
    body: &[u8],
    out: &mut KVec<AccessoryId>,
) -> core::result::Result<usize, ParseError> {
    let count = word(body, 0)?;
    let total = count
        .checked_mul(ACCESSORY_ID_LEN)
        .and_then(|n| n.checked_add(WORD))
        .ok_or(ParseError::Field)?;
    if total != body.len() {
        return Err(ParseError::Field);
    }
    out.clear();
    for rec in body[WORD..].chunks_exact(ACCESSORY_ID_LEN) {
        let mut id = [0u8; ACCESSORY_ID_LEN];
        id.copy_from_slice(rec);
        out.push(AccessoryId(id), GFP_KERNEL)
            .map_err(|_| ParseError::Length)?;
    }
    Ok(count)
}

/// Enables a paired accessory for biometric use. `user` is the account the
/// enclave binds it to, `acm_handle` the credential context the enclave has
/// already satisfied. In the recording the ticket was empty.
pub(crate) fn authorize_request(
    user: u32,
    id: &AccessoryId,
    acm_handle: &[u8],
    ticket: &[u8],
) -> Result<KVec<u8>> {
    if acm_handle.is_empty() || acm_handle.len() > ACM_HANDLE_SLOT {
        return Err(EINVAL);
    }
    let mut out = KVec::with_capacity(AUTHORIZE_FIXED + ticket.len(), GFP_KERNEL)?;
    out.extend_from_slice(&user.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(&id.0, GFP_KERNEL)?;
    push_word(&mut out, acm_handle.len())?;
    out.extend_from_slice(acm_handle, GFP_KERNEL)?;
    out.resize(WORD + ACCESSORY_ID_LEN + WORD + ACM_HANDLE_SLOT, 0, GFP_KERNEL)?;
    push_word(&mut out, ticket.len())?;
    out.extend_from_slice(ticket, GFP_KERNEL)?;
    Ok(out)
}

/// Checks that an operation answered with an empty body, as every state
/// change does.
pub(crate) fn expect_empty(body: &[u8]) -> core::result::Result<(), ParseError> {
    if body.is_empty() {
        Ok(())
    } else {
        Err(ParseError::Length)
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_stac_ops)]
mod tests {
    use super::*;

    const ID: AccessoryId = AccessoryId([0x5a; ACCESSORY_ID_LEN]);
    const POINT_A: PublicPoint = PublicPoint([0x04; PUBLIC_POINT_LEN]);
    const POINT_B: PublicPoint = PublicPoint([0x05; PUBLIC_POINT_LEN]);

    #[test]
    fn opcodes_match_the_recording() {
        assert_eq!(Op::HostAttestationObject.opcode(), 0x01);
        assert_eq!(Op::PairingState.opcode(), 0x02);
        assert_eq!(Op::PairingBegin.opcode(), 0x03);
        assert_eq!(Op::Unpair.opcode(), 0x04);
        assert_eq!(Op::Manifest.opcode(), 0x05);
        assert_eq!(Op::SessionBegin.opcode(), 0x06);
        assert_eq!(Op::SessionFinish.opcode(), 0x07);
        assert_eq!(Op::ConnectionState.opcode(), 0x09);
        assert_eq!(Op::PairedList.opcode(), 0x0b);
        assert_eq!(Op::PowerState.opcode(), crate::stac::OP_POWER_STATE);
        assert_eq!(Op::Authorize.opcode(), 0x14);
        assert_eq!(Op::PairingFinish.opcode(), 0x15);
    }

    #[test]
    fn pairing_state_flags() {
        assert_eq!(
            parse_pairing_state(&[0, 0]),
            Ok(PairingState { paired: false, authorized: false })
        );
        assert_eq!(
            parse_pairing_state(&[1, 0]),
            Ok(PairingState { paired: true, authorized: false })
        );
        assert_eq!(
            parse_pairing_state(&[1, 1]),
            Ok(PairingState { paired: true, authorized: true })
        );
        assert_eq!(parse_pairing_state(&[0, 1]), Err(ParseError::Field));
        assert_eq!(parse_pairing_state(&[2, 0]), Err(ParseError::Flag));
        assert_eq!(parse_pairing_state(&[1]), Err(ParseError::Length));
        assert_eq!(parse_pairing_state(&[1, 1, 0]), Err(ParseError::Length));
        assert_eq!(pairing_state_request(&ID), ID.0);
    }

    #[test]
    fn host_object_length_is_bounded_by_its_slot() {
        let mut body = [0u8; HOST_OBJECT_REPLY_LEN];
        body[0] = 97;
        assert_eq!(parse_host_object_len(&body), Ok(97));
        body[0] = 0;
        assert_eq!(parse_host_object_len(&body), Err(ParseError::Field));
        body[..4].copy_from_slice(&130u32.to_le_bytes());
        assert_eq!(parse_host_object_len(&body), Err(ParseError::Field));
        assert_eq!(parse_host_object_len(&body[..132]), Err(ParseError::Length));
    }

    #[test]
    fn pairing_begin_round_trip() -> Result {
        let req = pairing_begin_request(&ID, &POINT_A, &POINT_B);
        assert_eq!(req.len(), 146);
        assert_eq!(&req[..16], &ID.0);
        assert_eq!(&req[16..81], &POINT_A.0);
        assert_eq!(&req[81..], &POINT_B.0);

        // The recorded reply shape: 65 + 4 + 1509.
        let mut body = KVec::new();
        body.resize(1578, 0, GFP_KERNEL)?;
        body[65..69].copy_from_slice(&1509u32.to_le_bytes());
        let r = parse_pairing_begin(&body).map_err(|_| EINVAL)?;
        assert_eq!(r.host_ephemeral, 0..65);
        assert_eq!(r.host_attestation, 69..1578);
        // Truncated, oversized and zero-length attestation.
        assert_eq!(parse_pairing_begin(&body[..1577]), Err(ParseError::Field));
        body.push(0, GFP_KERNEL)?;
        assert_eq!(parse_pairing_begin(&body), Err(ParseError::Field));
        assert_eq!(parse_pairing_begin(&body[..68]), Err(ParseError::Length));
        let mut empty = [0u8; 69];
        empty[65..69].copy_from_slice(&0u32.to_le_bytes());
        assert_eq!(parse_pairing_begin(&empty), Err(ParseError::Field));
        Ok(())
    }

    #[test]
    fn pairing_finish_layout_matches_the_recorded_length() -> Result {
        let att = [0xa1u8; 981];
        let mut cert = KVec::new();
        cert.resize(1994, 0xc3, GFP_KERNEL)?;
        let req = pairing_finish_request(&ID, &POINT_A, &att, &cert)?;
        assert_eq!(req.len(), 3619);
        assert_eq!(&req[81..85], &981u32.to_le_bytes());
        assert!(req[85..85 + 981].iter().all(|&b| b == 0xa1));
        // The attestation slot is zero-padded.
        assert!(req[85 + 981..85 + 1536].iter().all(|&b| b == 0));
        assert_eq!(&req[1621..1625], &1994u32.to_le_bytes());
        // Bounds.
        assert!(pairing_finish_request(&ID, &POINT_A, &[], &cert).is_err());
        let big = [0u8; ACCESSORY_ATTESTATION_SLOT + 1];
        assert!(pairing_finish_request(&ID, &POINT_A, &big, &cert).is_err());
        cert.resize(ACCESSORY_CERTIFICATE_MAX + 1, 0, GFP_KERNEL)?;
        assert!(pairing_finish_request(&ID, &POINT_A, &att, &cert).is_err());
        Ok(())
    }

    #[test]
    fn manifest_layout_matches_the_recorded_length() -> Result {
        let mut m = KVec::new();
        m.resize(2391, 0x6d, GFP_KERNEL)?;
        let req = manifest_request(&ID, true, &m)?;
        assert_eq!(req.len(), 2412);
        assert_eq!(req[16], 1);
        assert_eq!(&req[17..21], &2391u32.to_le_bytes());
        assert_eq!(manifest_request(&ID, false, &m)?[16], 0);
        assert!(manifest_request(&ID, true, &[]).is_err());
        m.resize(MANIFEST_MAX + 1, 0, GFP_KERNEL)?;
        assert!(manifest_request(&ID, true, &m).is_err());
        Ok(())
    }

    #[test]
    fn connection_state_is_id_and_flag() {
        let down = connection_state_request(&ID, false);
        let up = connection_state_request(&ID, true);
        assert_eq!(down.len(), 17);
        assert_eq!(&down[..16], &ID.0);
        assert_eq!(down[16], 0);
        assert_eq!(up[16], 1);
    }

    #[test]
    fn paired_list_counts_exactly() -> Result {
        let mut ids = KVec::new();
        let mut body = [0u8; 20];
        body[0] = 1;
        body[4..].copy_from_slice(&ID.0);
        assert_eq!(parse_paired_list(&body, &mut ids), Ok(1));
        assert_eq!(ids[0], ID);
        assert_eq!(parse_paired_list(&[0, 0, 0, 0], &mut ids), Ok(0));
        assert!(ids.is_empty());
        // Count and body disagree, or the multiplication overflows.
        assert_eq!(parse_paired_list(&body[..19], &mut ids), Err(ParseError::Field));
        let mut over = [0u8; 20];
        over[..4].copy_from_slice(&u32::MAX.to_le_bytes());
        assert_eq!(parse_paired_list(&over, &mut ids), Err(ParseError::Field));
        assert_eq!(parse_paired_list(&[1, 0], &mut ids), Err(ParseError::Length));
        Ok(())
    }

    #[test]
    fn authorize_layout_matches_the_recorded_length() -> Result {
        let handle = [0x11u8; 16];
        let req = authorize_request(501, &ID, &handle, &[])?;
        assert_eq!(req.len(), 60);
        assert_eq!(&req[..4], &501u32.to_le_bytes());
        assert_eq!(&req[4..20], &ID.0);
        assert_eq!(&req[20..24], &16u32.to_le_bytes());
        assert_eq!(&req[24..40], &handle);
        // The handle slot is zero-padded.
        assert!(req[40..56].iter().all(|&b| b == 0));
        assert_eq!(&req[56..60], &0u32.to_le_bytes());
        assert_eq!(authorize_request(501, &ID, &handle, &[7; 8])?.len(), 68);
        assert!(authorize_request(501, &ID, &[], &[]).is_err());
        assert!(authorize_request(501, &ID, &[0; 33], &[]).is_err());
        Ok(())
    }

    #[test]
    fn unpair_is_the_identity() {
        assert_eq!(unpair_request(&ID), ID.0);
    }

    #[test]
    fn session_begin_round_trip() -> Result {
        let req = session_begin_request(&ID, &POINT_B);
        assert_eq!(req.len(), 81);
        assert_eq!(&req[..16], &ID.0);
        assert_eq!(&req[16..], &POINT_B.0);

        // The recorded reply shape: 65 + 4 + 159.
        let mut body = [0u8; 228];
        body[65..69].copy_from_slice(&159u32.to_le_bytes());
        let r = parse_session_begin(&body).map_err(|_| EINVAL)?;
        assert_eq!(r.host_ephemeral, 0..65);
        assert_eq!(r.host_attestation, 69..228);
        assert_eq!(parse_session_begin(&body[..227]), Err(ParseError::Field));
        assert_eq!(parse_session_begin(&body[..68]), Err(ParseError::Length));
        Ok(())
    }

    #[test]
    fn session_finish_layout_matches_the_recorded_length() -> Result {
        let material = [0x3cu8; 979];
        let req = session_finish_request(&ID, &material)?;
        assert_eq!(req.len(), 1556);
        assert_eq!(&req[..16], &ID.0);
        assert_eq!(&req[16..20], &979u32.to_le_bytes());
        assert!(req[20..20 + 979].iter().all(|&b| b == 0x3c));
        // The material slot is zero-padded.
        assert!(req[20 + 979..].iter().all(|&b| b == 0));
        assert_eq!(session_finish_request(&ID, &[0; 1536])?.len(), 1556);
        assert!(session_finish_request(&ID, &[]).is_err());
        assert!(session_finish_request(&ID, &[0; 1537]).is_err());
        Ok(())
    }

    #[test]
    fn state_changes_answer_empty() {
        assert_eq!(expect_empty(&[]), Ok(()));
        assert_eq!(expect_empty(&[0]), Err(ParseError::Length));
    }
}
