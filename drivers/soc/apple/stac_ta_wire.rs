// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! The keyboard side of a trusted-accessory pairing: how commands, replies
//! and large objects travel over the Touch ID channel of a Magic Keyboard
//! with Touch ID (see `include/linux/hid-apple-ta.h`).
//!
//! Every host command is a feature report `[id][u32 step][payload]`. The
//! keyboard answers a command with one input report `[id][u32 step][u32
//! status][payload]`. Objects too large for one command go in fragment
//! reports of a fixed 532 bytes after the ID: a 20-byte header (step, kind,
//! total length, offset, chunk length; little-endian words) and a chunk of at
//! most 512 bytes, the rest zero. Fragments are not answered one by one; the
//! next command's reply covers them.
//!
//! This module only encodes and checks bytes. It logs nothing: identities,
//! points, attestations and certificates are opaque here.

use kernel::prelude::*;

use crate::stac_ops::{ACCESSORY_ATTESTATION_SLOT, ACCESSORY_CERTIFICATE_MAX, MANIFEST_MAX};
use crate::stac_ops::{PublicPoint, PUBLIC_POINT_LEN, WORD};

/// Feature reports the host writes.
pub(crate) mod cmd {
    /// The protocol tag echoed back to the keyboard.
    pub(crate) const VERSION: u8 = 0x68;
    /// Asks for the keyboard's identity point (step 1).
    pub(crate) const IDENTITY: u8 = 0x67;
    /// The host object for the pairing-state check (step 2).
    pub(crate) const PAIRING_CHECK: u8 = 0x60;
    /// Asks for the keyboard's pairing ephemeral point (step 3).
    pub(crate) const PAIRING_EPHEMERAL: u8 = 0x66;
    /// One fragment of a large object (steps 4 and 6).
    pub(crate) const FRAGMENT: u8 = 0x6a;
    /// The host object after the certificate chain (step 5).
    pub(crate) const HOST_OBJECT: u8 = 0x6b;
    /// The host object and the enclave's ephemeral point (step 7).
    pub(crate) const PAIRING_FINISH: u8 = 0x61;
}

/// Feature reports the host reads.
pub(crate) mod get {
    /// The accessory's manifest, after a pairing.
    pub(crate) const MANIFEST: u8 = 0x45;
}

/// Input reports the keyboard answers with.
pub(crate) mod reply {
    pub(crate) const IDENTITY: u8 = 0x40;
    pub(crate) const PAIRING_CHECK: u8 = 0x42;
    pub(crate) const ACCESSORY_MATERIAL: u8 = 0x43;
    pub(crate) const PAIRING_EPHEMERAL: u8 = 0x47;
    pub(crate) const HOST_OBJECT: u8 = 0x49;
}

/// Kinds of fragmented objects.
pub(crate) mod kind {
    /// The host certificate chain (step 4).
    pub(crate) const CHAIN: u32 = 1;
    /// The enclave's pairing attestation (step 6).
    pub(crate) const ATTESTATION: u32 = 2;
}

/// The protocol tag and version the keyboard reports and the host echoes.
pub(crate) const VERSION_BODY: [u8; 8] = *b"TACP\x01\x00\x00\x00";

pub(crate) const FRAGMENT_LEN: usize = 532;
pub(crate) const FRAGMENT_HEADER_LEN: usize = 5 * WORD;
pub(crate) const FRAGMENT_CHUNK_MAX: usize = FRAGMENT_LEN - FRAGMENT_HEADER_LEN;
/// The largest object the fragment carrier takes here: the larger of the two
/// fragmented objects' bounds.
pub(crate) const FRAGMENTED_MAX: usize = 4096;

/// `[id][u32 step][u32 status]`, the start of every reply but one.
pub(crate) const REPLY_HEADER_LEN: usize = 1 + 2 * WORD;
/// The identity and pairing ephemeral replies: the header and one point.
pub(crate) const POINT_REPLY_LEN: usize = REPLY_HEADER_LEN + PUBLIC_POINT_LEN;
/// The pairing-check reply: `[id][u32 step][u8 status][u8 paired][u8 1]`.
pub(crate) const PAIRING_CHECK_REPLY_LEN: usize = 1 + WORD + 3;
/// The accessory material: the header, the attestation in a length-prefixed
/// fixed slot, then the certificate in a length-prefixed fixed slot.
pub(crate) const MATERIAL_REPLY_LEN: usize =
    REPLY_HEADER_LEN + WORD + ACCESSORY_ATTESTATION_SLOT + WORD + ACCESSORY_CERTIFICATE_MAX;
/// The buffer for the manifest read: `[id][u32 length][manifest]`.
pub(crate) const MANIFEST_READ_LEN: usize = 1 + WORD + MANIFEST_MAX;

/// The host certificate chain: two DER certificates.
pub(crate) const CHAIN_MAX: usize = FRAGMENTED_MAX;

// The lengths in the recording.
static_assert!(FRAGMENT_CHUNK_MAX == 512);
static_assert!(POINT_REPLY_LEN == 74);
static_assert!(PAIRING_CHECK_REPLY_LEN == 8);
static_assert!(MATERIAL_REPLY_LEN == 3601);
static_assert!(1 + WORD + 133 == 138);

/// Why a keyboard reply or an object was not taken.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum WireError {
    /// Not the report asked for.
    Id,
    /// The reply is for another step.
    Step,
    /// The keyboard reported this nonzero status.
    Status(u32),
    /// The report is not the length its kind has.
    Length,
    /// A length field is out of its bound.
    Field,
    /// A point is not an uncompressed point.
    Point,
    /// The certificate chain is not two DER certificates covering this
    /// enclave's host object.
    Chain,
}

fn le32(b: &[u8], at: usize) -> core::result::Result<usize, WireError> {
    let w = b.get(at..at + WORD).ok_or(WireError::Length)?;
    Ok(u32::from_le_bytes([w[0], w[1], w[2], w[3]]) as usize)
}

/// A command report: `[id][u32 step][payload]`.
pub(crate) fn command(id: u8, step: u32, payload: &[u8]) -> Result<KVec<u8>> {
    let mut out = KVec::with_capacity(1 + WORD + payload.len(), GFP_KERNEL)?;
    out.push(id, GFP_KERNEL)?;
    out.extend_from_slice(&step.to_le_bytes(), GFP_KERNEL)?;
    out.extend_from_slice(payload, GFP_KERNEL)?;
    Ok(out)
}

/// The version echo: the tag without a step word.
pub(crate) fn version_report() -> [u8; 1 + VERSION_BODY.len()] {
    let mut out = [0u8; 1 + VERSION_BODY.len()];
    out[0] = cmd::VERSION;
    out[1..].copy_from_slice(&VERSION_BODY);
    out
}

/// How many fragment reports an object of `total` bytes takes.
pub(crate) fn fragment_count(total: usize) -> usize {
    total.div_ceil(FRAGMENT_CHUNK_MAX)
}

/// Fragment `index` of `object` as one report, ID first. The tail after the
/// chunk is zero.
pub(crate) fn fragment(
    step: u32,
    kind: u32,
    object: &[u8],
    index: usize,
) -> Result<[u8; 1 + FRAGMENT_LEN]> {
    if object.is_empty() || object.len() > FRAGMENTED_MAX {
        return Err(EINVAL);
    }
    let offset = index.checked_mul(FRAGMENT_CHUNK_MAX).ok_or(EINVAL)?;
    if offset >= object.len() {
        return Err(EINVAL);
    }
    let chunk = core::cmp::min(FRAGMENT_CHUNK_MAX, object.len() - offset);
    let mut out = [0u8; 1 + FRAGMENT_LEN];
    out[0] = cmd::FRAGMENT;
    // Bounded by FRAGMENTED_MAX above, so every word fits.
    for (i, w) in [step, kind, object.len() as u32, offset as u32, chunk as u32]
        .iter()
        .enumerate()
    {
        out[1 + i * WORD..1 + (i + 1) * WORD].copy_from_slice(&w.to_le_bytes());
    }
    out[1 + FRAGMENT_HEADER_LEN..1 + FRAGMENT_HEADER_LEN + chunk]
        .copy_from_slice(&object[offset..offset + chunk]);
    Ok(out)
}

/// Checks a reply's ID, step and status, and returns its payload.
pub(crate) fn check_reply(
    report: &[u8],
    id: u8,
    step: u32,
) -> core::result::Result<&[u8], WireError> {
    if report.len() < REPLY_HEADER_LEN {
        return Err(WireError::Length);
    }
    if report[0] != id {
        return Err(WireError::Id);
    }
    if le32(report, 1)? != step as usize {
        return Err(WireError::Step);
    }
    match le32(report, 1 + WORD)? {
        0 => Ok(&report[REPLY_HEADER_LEN..]),
        s => Err(WireError::Status(s as u32)),
    }
}

/// A reply that carries one point: the identity (step 1) or the pairing
/// ephemeral (step 3).
pub(crate) fn parse_point_reply(
    report: &[u8],
    id: u8,
    step: u32,
) -> core::result::Result<PublicPoint, WireError> {
    let payload = check_reply(report, id, step)?;
    if report.len() != POINT_REPLY_LEN {
        return Err(WireError::Length);
    }
    if payload[0] != 0x04 {
        return Err(WireError::Point);
    }
    let mut p = PublicPoint([0; PUBLIC_POINT_LEN]);
    p.0.copy_from_slice(payload);
    Ok(p)
}

/// The pairing-check reply (step 2): whether the keyboard holds a pairing
/// with this host.
pub(crate) fn parse_pairing_check(report: &[u8]) -> core::result::Result<bool, WireError> {
    if report.len() != PAIRING_CHECK_REPLY_LEN {
        return Err(WireError::Length);
    }
    if report[0] != reply::PAIRING_CHECK {
        return Err(WireError::Id);
    }
    if le32(report, 1)? != 2 {
        return Err(WireError::Step);
    }
    if report[5] != 0 {
        return Err(WireError::Status(u32::from(report[5])));
    }
    match (report[6], report[7]) {
        (0, 1) => Ok(false),
        (1, 1) => Ok(true),
        _ => Err(WireError::Field),
    }
}

/// Where the keyboard's attestation and certificate sit in its material
/// reply (step 7).
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Material {
    pub(crate) attestation: core::ops::Range<usize>,
    pub(crate) certificate: core::ops::Range<usize>,
}

pub(crate) fn parse_material(report: &[u8]) -> core::result::Result<Material, WireError> {
    check_reply(report, reply::ACCESSORY_MATERIAL, 7)?;
    if report.len() != MATERIAL_REPLY_LEN {
        return Err(WireError::Length);
    }
    let a = REPLY_HEADER_LEN;
    let alen = le32(report, a)?;
    let c = a + WORD + ACCESSORY_ATTESTATION_SLOT;
    let clen = le32(report, c)?;
    if alen == 0 || alen > ACCESSORY_ATTESTATION_SLOT || clen == 0 || clen > ACCESSORY_CERTIFICATE_MAX
    {
        return Err(WireError::Field);
    }
    Ok(Material {
        attestation: a + WORD..a + WORD + alen,
        certificate: c + WORD..c + WORD + clen,
    })
}

/// The manifest read: `[id][u32 length][manifest]`, `n` bytes received.
pub(crate) fn parse_manifest(report: &[u8]) -> core::result::Result<&[u8], WireError> {
    if report.len() < 1 + WORD {
        return Err(WireError::Length);
    }
    if report[0] != get::MANIFEST {
        return Err(WireError::Id);
    }
    let len = le32(report, 1)?;
    if len == 0 || len > MANIFEST_MAX {
        return Err(WireError::Field);
    }
    report.get(1 + WORD..1 + WORD + len).ok_or(WireError::Length)
}

/// The length of the DER element at the start of `b`, if it is a SEQUENCE
/// with a definite length that fits.
fn der_sequence_len(b: &[u8]) -> Option<usize> {
    if b.len() < 2 || b[0] != 0x30 {
        return None;
    }
    let (hdr, len) = match b[1] {
        l @ 0..=0x7f => (2, l as usize),
        0x81 => (3, *b.get(2)? as usize),
        0x82 => (4, (*b.get(2)? as usize) << 8 | *b.get(3)? as usize),
        _ => return None,
    };
    let total = hdr + len;
    (total <= b.len()).then_some(total)
}

fn hex_lower(d: &[u8; 32]) -> [u8; 64] {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut out = [0u8; 64];
    for (i, b) in d.iter().enumerate() {
        out[2 * i] = H[(b >> 4) as usize];
        out[2 * i + 1] = H[(b & 15) as usize];
    }
    out
}

/// Checks the host certificate chain before it goes to a keyboard: exactly
/// two DER certificates, nothing after them, and a leaf whose subject names
/// `host_object_digest` (the SHA-256 of the enclave's host object, which the
/// leaf certifies) in lowercase hex. A chain for another Mac, or for a host
/// object this enclave no longer has, is refused here rather than by the
/// keyboard. Signatures are the keyboard's to check.
pub(crate) fn check_chain(
    chain: &[u8],
    host_object_digest: &[u8; 32],
) -> core::result::Result<(), WireError> {
    if chain.is_empty() || chain.len() > CHAIN_MAX {
        return Err(WireError::Chain);
    }
    let leaf = der_sequence_len(chain).ok_or(WireError::Chain)?;
    let ca = der_sequence_len(&chain[leaf..]).ok_or(WireError::Chain)?;
    if leaf + ca != chain.len() {
        return Err(WireError::Chain);
    }
    let name = hex_lower(host_object_digest);
    if chain[..leaf].windows(name.len()).any(|w| w == name) {
        Ok(())
    } else {
        Err(WireError::Chain)
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_stac_ta_wire)]
mod tests {
    use super::*;

    fn obj(n: usize) -> KVec<u8> {
        let mut v = KVec::new();
        for i in 0..n {
            v.push((i * 7 + 3) as u8, GFP_KERNEL).unwrap();
        }
        v
    }

    fn word_at(b: &[u8], at: usize) -> u32 {
        u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
    }

    #[test]
    fn a_command_is_id_step_payload() {
        let c = command(cmd::PAIRING_CHECK, 2, &[0xaa; 133]).unwrap();
        assert_eq!(c.len(), 138);
        assert_eq!(c[0], 0x60);
        assert_eq!(word_at(&c, 1), 2);
        assert!(c[5..].iter().all(|&b| b == 0xaa));
        let v = version_report();
        assert_eq!(&v, b"\x68TACP\x01\x00\x00\x00");
    }

    // The certificate chain of the recording: 1663 bytes in four fragments.
    #[test]
    fn a_chain_goes_in_532_byte_fragments() {
        let o = obj(1663);
        assert_eq!(fragment_count(o.len()), 4);
        let mut back = KVec::new();
        for i in 0..4 {
            let f = fragment(4, kind::CHAIN, &o, i).unwrap();
            assert_eq!(f.len(), 533);
            assert_eq!(f[0], cmd::FRAGMENT);
            assert_eq!(word_at(&f, 1), 4);
            assert_eq!(word_at(&f, 5), kind::CHAIN);
            assert_eq!(word_at(&f, 9), 1663);
            assert_eq!(word_at(&f, 13), (i * 512) as u32);
            let chunk = word_at(&f, 17) as usize;
            assert_eq!(chunk, if i < 3 { 512 } else { 127 });
            back.extend_from_slice(&f[21..21 + chunk], GFP_KERNEL).unwrap();
            assert!(f[21 + chunk..].iter().all(|&b| b == 0));
        }
        assert!(back.as_slice() == o.as_slice());
        assert!(fragment(4, kind::CHAIN, &o, 4).is_err());
    }

    // The enclave's attestation of the recording: 1505 bytes in three.
    #[test]
    fn the_attestation_goes_in_three() {
        let o = obj(1505);
        assert_eq!(fragment_count(o.len()), 3);
        let f = fragment(6, kind::ATTESTATION, &o, 2).unwrap();
        assert_eq!(word_at(&f, 1), 6);
        assert_eq!(word_at(&f, 5), 2);
        assert_eq!(word_at(&f, 13), 1024);
        assert_eq!(word_at(&f, 17), 481);
        // Exactly one chunk.
        assert_eq!(fragment_count(512), 1);
        assert_eq!(fragment_count(513), 2);
    }

    #[test]
    fn fragments_refuse_empty_and_oversized_objects() {
        assert!(fragment(4, kind::CHAIN, &[], 0).is_err());
        let big = obj(FRAGMENTED_MAX + 1);
        assert!(fragment(4, kind::CHAIN, &big, 0).is_err());
    }

    fn reply_of(id: u8, step: u32, status: u32, payload: &[u8]) -> KVec<u8> {
        let mut r = KVec::new();
        r.push(id, GFP_KERNEL).unwrap();
        r.extend_from_slice(&step.to_le_bytes(), GFP_KERNEL).unwrap();
        r.extend_from_slice(&status.to_le_bytes(), GFP_KERNEL).unwrap();
        r.extend_from_slice(payload, GFP_KERNEL).unwrap();
        r
    }

    #[test]
    fn replies_check_id_step_and_status() {
        let mut point = [0x11u8; PUBLIC_POINT_LEN];
        point[0] = 0x04;
        let r = reply_of(reply::IDENTITY, 1, 0, &point);
        assert_eq!(r.len(), 74);
        assert_eq!(parse_point_reply(&r, reply::IDENTITY, 1).unwrap().0, point);
        assert_eq!(parse_point_reply(&r, reply::PAIRING_EPHEMERAL, 3), Err(WireError::Id));
        assert_eq!(parse_point_reply(&r, reply::IDENTITY, 3), Err(WireError::Step));
        let r = reply_of(reply::IDENTITY, 1, 5, &point);
        assert_eq!(parse_point_reply(&r, reply::IDENTITY, 1), Err(WireError::Status(5)));
        let r = reply_of(reply::IDENTITY, 1, 0, &point[..64]);
        assert_eq!(parse_point_reply(&r, reply::IDENTITY, 1), Err(WireError::Length));
        point[0] = 0x02;
        let r = reply_of(reply::IDENTITY, 1, 0, &point);
        assert_eq!(parse_point_reply(&r, reply::IDENTITY, 1), Err(WireError::Point));
        // The host-object acknowledgement: header only.
        let r = reply_of(reply::HOST_OBJECT, 5, 0, &[]);
        assert_eq!(r.len(), 9);
        assert_eq!(check_reply(&r, reply::HOST_OBJECT, 5).unwrap().len(), 0);
        assert_eq!(check_reply(&r[..8], reply::HOST_OBJECT, 5), Err(WireError::Length));
    }

    #[test]
    fn the_pairing_check_reports_paired_or_not() {
        let unpaired = *b"\x42\x02\x00\x00\x00\x00\x00\x01";
        let paired = *b"\x42\x02\x00\x00\x00\x00\x01\x01";
        assert_eq!(parse_pairing_check(&unpaired), Ok(false));
        assert_eq!(parse_pairing_check(&paired), Ok(true));
        let mut bad = paired;
        bad[5] = 3;
        assert_eq!(parse_pairing_check(&bad), Err(WireError::Status(3)));
        let mut bad = paired;
        bad[6] = 2;
        assert_eq!(parse_pairing_check(&bad), Err(WireError::Field));
        let mut bad = paired;
        bad[1] = 3;
        assert_eq!(parse_pairing_check(&bad), Err(WireError::Step));
        assert_eq!(parse_pairing_check(&paired[..7]), Err(WireError::Length));
    }

    fn material(alen: u32, clen: u32) -> KVec<u8> {
        let mut r = reply_of(reply::ACCESSORY_MATERIAL, 7, 0, &[]);
        r.extend_from_slice(&alen.to_le_bytes(), GFP_KERNEL).unwrap();
        r.resize(REPLY_HEADER_LEN + WORD + ACCESSORY_ATTESTATION_SLOT, 0xa5, GFP_KERNEL)
            .unwrap();
        r.extend_from_slice(&clen.to_le_bytes(), GFP_KERNEL).unwrap();
        r.resize(MATERIAL_REPLY_LEN, 0x5a, GFP_KERNEL).unwrap();
        r
    }

    #[test]
    fn the_material_reply_has_two_bounded_slots() {
        let r = material(980, 1994);
        let m = parse_material(&r).unwrap();
        assert_eq!(m.attestation, 13..993);
        assert_eq!(m.certificate, 1553..3547);
        // Together with the identity and point they make the 3619-byte
        // pairing finish request of the recording.
        assert_eq!(
            crate::stac_ops::PAIRING_FINISH_FIXED + m.certificate.len(),
            3619
        );
        assert_eq!(parse_material(&material(0, 10)), Err(WireError::Field));
        assert_eq!(parse_material(&material(1537, 10)), Err(WireError::Field));
        assert_eq!(parse_material(&material(10, 2049)), Err(WireError::Field));
        assert_eq!(parse_material(&r[..3600]), Err(WireError::Length));
    }

    #[test]
    fn the_manifest_read_is_bounded() {
        let mut r = KVec::new();
        r.push(get::MANIFEST, GFP_KERNEL).unwrap();
        r.extend_from_slice(&2391u32.to_le_bytes(), GFP_KERNEL).unwrap();
        r.resize(MANIFEST_READ_LEN, 0x30, GFP_KERNEL).unwrap();
        assert_eq!(parse_manifest(&r).unwrap().len(), 2391);
        assert_eq!(parse_manifest(&r[..1000]), Err(WireError::Length));
        r[1..5].copy_from_slice(&(MANIFEST_MAX as u32 + 1).to_le_bytes());
        assert_eq!(parse_manifest(&r), Err(WireError::Field));
        r[0] = 0x44;
        assert_eq!(parse_manifest(&r), Err(WireError::Id));
    }

    fn cert(body: &[u8]) -> KVec<u8> {
        let mut c = KVec::new();
        c.extend_from_slice(&[0x30, 0x82, (body.len() >> 8) as u8, body.len() as u8], GFP_KERNEL)
            .unwrap();
        c.extend_from_slice(body, GFP_KERNEL).unwrap();
        c
    }

    #[test]
    fn the_chain_must_name_this_host_object() {
        let digest = [0x39u8; 32];
        let mut leaf_body = KVec::new();
        leaf_body.resize(300, 0x11, GFP_KERNEL).unwrap();
        leaf_body.extend_from_slice(&hex_lower(&digest), GFP_KERNEL).unwrap();
        leaf_body.resize(1072, 0x22, GFP_KERNEL).unwrap();
        let mut chain = cert(&leaf_body);
        let ca = cert(&[0x33; 583]);
        chain.extend_from_slice(&ca, GFP_KERNEL).unwrap();
        assert_eq!(chain.len(), 1663);
        assert_eq!(check_chain(&chain, &digest), Ok(()));
        // Another Mac's host object.
        assert_eq!(check_chain(&chain, &[0x40; 32]), Err(WireError::Chain));
        // Trailing bytes, a missing intermediate, a name only in the intermediate.
        let mut t = KVec::new();
        t.extend_from_slice(&chain, GFP_KERNEL).unwrap();
        t.push(0, GFP_KERNEL).unwrap();
        assert_eq!(check_chain(&t, &digest), Err(WireError::Chain));
        let leaf_len = 4 + leaf_body.len();
        assert_eq!(check_chain(&chain[..leaf_len], &digest), Err(WireError::Chain));
        let mut swapped = KVec::new();
        swapped.extend_from_slice(&ca, GFP_KERNEL).unwrap();
        swapped.extend_from_slice(&chain[..leaf_len], GFP_KERNEL).unwrap();
        assert_eq!(check_chain(&swapped, &digest), Err(WireError::Chain));
        assert_eq!(check_chain(&[], &digest), Err(WireError::Chain));
    }
}
