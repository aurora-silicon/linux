// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
//! Ref-key (op 0x22) ECIES **encrypt** — the AP half of the SE seal.
//!
//! The SEP has no scalar×G, so it cannot make the sender ephemeral (in-SEP `ow`
//! returns -10); `od` imports the one this module produces, ECDHs it against the
//! in-SEP private key and AES-GCM-decrypts. The encrypt runs here by construction.
//!
//! For keys with an access-control list the enclave does not decrypt at all
//! (`oecd` is refused even when the list leaves it free); as on macOS, the
//! enclave only does the key exchange (`ock`, which the list constrains) and
//! [`ecies_open_with_shared`] finishes the decrypt here.

use kernel::prelude::*;

extern "C" {
    fn sep_p256_sender(
        peer_pub_be: *const c_void,
        eph_pub_be_out: *mut u8,
        shared_be_out: *mut u8,
    ) -> c_int;

    fn sep_gcm(
        encrypt: c_int,
        key: *const c_void,
        keylen: usize,
        iv: *const c_void,
        ivlen: usize,
        aadlen: usize,
        buf: *mut c_void,
        buflen: usize,
        datalen: usize,
    ) -> c_int;
}

// The enclave's SE seal is an ECIES scheme:
// ECDH P-256 (shared = X), X9.63-SHA256 KDF, KDF-derived key‖IV, AES-GCM.

pub(crate) const TAG_LEN: usize = 16;
/// 16-byte IV (KDF-derived "variable IV"), not the 12-byte GCM default.
pub(crate) const IV_LEN: usize = 16;
pub(crate) const AES256_KEY_LEN: usize = 32;
pub(crate) const AES128_KEY_LEN: usize = 16;
/// Uncompressed P-256 point: `0x04 ‖ X ‖ Y`.
pub(crate) const POINT_LEN: usize = 65;
/// Raw `X ‖ Y` (no prefix), as the ECDH shim exchanges.
const COORDS_LEN: usize = 64;

fn wipe(bytes: &mut [u8]) {
    for b in bytes.iter_mut() {
        // SAFETY: `b` is a valid, uniquely borrowed byte for this write.
        unsafe { core::ptr::write_volatile(b, 0) };
    }
}

fn p256_sender(peer_pub64: &[u8]) -> Result<([u8; COORDS_LEN], [u8; 32])> {
    if peer_pub64.len() != COORDS_LEN {
        return Err(EINVAL);
    }
    let mut eph = [0u8; COORDS_LEN];
    let mut shared = [0u8; 32];
    // SAFETY: `peer_pub64` is 64 live bytes; `eph`/`shared` are exactly the 64
    // and 32 bytes the shim writes.
    let rc = unsafe {
        sep_p256_sender(
            peer_pub64.as_ptr().cast(),
            eph.as_mut_ptr(),
            shared.as_mut_ptr(),
        )
    };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }
    Ok((eph, shared))
}

fn kdf_sha256_mode(z: &[u8], shared_info: &[u8], out_len: usize, mode: u8) -> Result<KVec<u8>> {
    let mut out = KVec::new();
    let mut counter: u32 = 1;
    while out.len() < out_len {
        let ctr = counter.to_be_bytes();
        let mut buf = KVec::new();
        match mode {
            0 => {
                buf.extend_from_slice(z, GFP_KERNEL)?;
                buf.extend_from_slice(&ctr, GFP_KERNEL)?;
                buf.extend_from_slice(shared_info, GFP_KERNEL)?;
            }
            1 => {
                buf.extend_from_slice(&ctr, GFP_KERNEL)?;
                buf.extend_from_slice(z, GFP_KERNEL)?;
                buf.extend_from_slice(shared_info, GFP_KERNEL)?;
            }
            2 => {
                buf.extend_from_slice(z, GFP_KERNEL)?;
                buf.extend_from_slice(shared_info, GFP_KERNEL)?;
                buf.extend_from_slice(&ctr, GFP_KERNEL)?;
            }
            3 => {
                buf.extend_from_slice(shared_info, GFP_KERNEL)?;
                buf.extend_from_slice(z, GFP_KERNEL)?;
                buf.extend_from_slice(&ctr, GFP_KERNEL)?;
            }
            _ => {
                buf.extend_from_slice(z, GFP_KERNEL)?;
                buf.extend_from_slice(shared_info, GFP_KERNEL)?;
            }
        }
        let digest = crate::image::sha256(&buf)?;
        wipe(&mut buf);
        let take = core::cmp::min(digest.len(), out_len - out.len());
        out.extend_from_slice(&digest[..take], GFP_KERNEL)?;
        counter = counter.checked_add(1).ok_or(EINVAL)?;
    }
    Ok(out)
}

fn gcm_encrypt(key: &[u8], iv: &[u8], aad: &[u8], plaintext: &[u8]) -> Result<KVec<u8>> {
    let mut buf = KVec::new();
    buf.extend_from_slice(aad, GFP_KERNEL)?;
    buf.extend_from_slice(plaintext, GFP_KERNEL)?;
    buf.resize(aad.len() + plaintext.len() + TAG_LEN, 0u8, GFP_KERNEL)?;
    // SAFETY: `key`/`iv` are live; `buf` holds aad‖plaintext‖tag-space and its
    // length is passed as both the buffer size and the extent to authenticate.
    let rc = unsafe {
        sep_gcm(
            1,
            key.as_ptr().cast(),
            key.len(),
            iv.as_ptr().cast(),
            iv.len(),
            aad.len(),
            buf.as_mut_ptr().cast(),
            buf.len(),
            plaintext.len(),
        )
    };
    if rc != 0 {
        return Err(Error::from_errno(rc));
    }
    let mut out = KVec::new();
    out.extend_from_slice(&buf[aad.len()..], GFP_KERNEL)?;
    Ok(out)
}

pub(crate) struct EciesParts {
    /// Ephemeral point `0x04 ‖ X ‖ Y`; also the KDF sharedInfo.
    pub(crate) eph_point: KVec<u8>,
    pub(crate) ciphertext: KVec<u8>,
    pub(crate) tag: KVec<u8>,
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn ecies_encrypt_parts(
    recipient_pub: &[u8],
    plaintext: &[u8],
    key_len: usize,
    variable_iv: bool,
    iv_len: usize,
    aad_eph: bool,
    si_mode: u8,
    kdf_mode: u8,
) -> Result<EciesParts> {
    if recipient_pub.len() != POINT_LEN || recipient_pub[0] != 0x04 {
        return Err(EINVAL);
    }
    if key_len != AES256_KEY_LEN && key_len != AES128_KEY_LEN {
        return Err(EINVAL);
    }
    if iv_len != 12 && iv_len != 16 {
        return Err(EINVAL);
    }
    let (eph_coords, mut shared) = p256_sender(&recipient_pub[1..])?;

    let mut eph_point = KVec::new();
    eph_point.push(0x04, GFP_KERNEL)?;
    eph_point.extend_from_slice(&eph_coords, GFP_KERNEL)?;

    let mut si = KVec::new();
    match si_mode {
        0 => si.extend_from_slice(&eph_point, GFP_KERNEL)?,
        1 => {}
        2 => si.extend_from_slice(&eph_point[1..], GFP_KERNEL)?,
        3 => si.extend_from_slice(&eph_point[1..33], GFP_KERNEL)?,
        4 => si.extend_from_slice(recipient_pub, GFP_KERNEL)?,
        _ => {
            si.extend_from_slice(&eph_point, GFP_KERNEL)?;
            si.extend_from_slice(recipient_pub, GFP_KERNEL)?;
        }
    }

    let kdf_len = if variable_iv {
        key_len + iv_len
    } else {
        key_len
    };
    let mut km = kdf_sha256_mode(&shared, &si, kdf_len, kdf_mode)?;
    wipe(&mut shared);
    let mut iv = KVec::new();
    if variable_iv {
        iv.extend_from_slice(&km[key_len..key_len + iv_len], GFP_KERNEL)?;
    } else {
        iv.resize(iv_len, 0u8, GFP_KERNEL)?;
    }
    let aad: &[u8] = if aad_eph { &eph_point } else { &[] };
    let ct_tag = gcm_encrypt(&km[..key_len], &iv, aad, plaintext);
    wipe(&mut km);
    let ct_tag = ct_tag?;
    if ct_tag.len() != plaintext.len() + TAG_LEN {
        return Err(EINVAL);
    }

    let mut ciphertext = KVec::new();
    ciphertext.extend_from_slice(&ct_tag[..plaintext.len()], GFP_KERNEL)?;
    let mut tag = KVec::new();
    tag.extend_from_slice(&ct_tag[plaintext.len()..], GFP_KERNEL)?;

    Ok(EciesParts {
        eph_point,
        ciphertext,
        tag,
    })
}

fn gcm_decrypt(key: &[u8], iv: &[u8], ct_tag: &[u8]) -> Result<KVec<u8>> {
    if ct_tag.len() <= TAG_LEN {
        return Err(EINVAL);
    }
    let datalen = ct_tag.len() - TAG_LEN;
    let mut buf = KVec::new();
    buf.extend_from_slice(ct_tag, GFP_KERNEL)?;
    // SAFETY: `key`/`iv` are live; `buf` holds ciphertext‖tag (no AAD) and its
    // length is passed as the buffer size; the plaintext is left in place.
    let rc = unsafe {
        sep_gcm(
            0,
            key.as_ptr().cast(),
            key.len(),
            iv.as_ptr().cast(),
            iv.len(),
            0,
            buf.as_mut_ptr().cast(),
            buf.len(),
            datalen,
        )
    };
    if rc != 0 {
        wipe(&mut buf);
        return Err(Error::from_errno(rc));
    }
    buf.truncate(datalen);
    Ok(buf)
}

/// The ephemeral point at the front of a blob from [`ecies_seal`].
pub(crate) fn sealed_ephemeral(sealed: &[u8]) -> Result<&[u8]> {
    if sealed.len() <= POINT_LEN + TAG_LEN || sealed[0] != 0x04 {
        return Err(EINVAL);
    }
    Ok(&sealed[..POINT_LEN])
}

/// Opens a blob from [`ecies_seal`] given the ECDH shared X of its ephemeral
/// point and the recipient key (what the enclave's `ock` returns): the same
/// X9.63-SHA256 KDF and AES-256-GCM with the KDF-derived 16-byte IV.
/// `EBADMSG` when the tag does not match (wrong key or a modified blob).
pub(crate) fn ecies_open_with_shared(shared: &[u8], sealed: &[u8]) -> Result<KVec<u8>> {
    let eph = sealed_ephemeral(sealed)?;
    if shared.len() != 32 {
        return Err(EINVAL);
    }
    let mut km = kdf_sha256_mode(shared, eph, AES256_KEY_LEN + IV_LEN, 0)?;
    let r = gcm_decrypt(&km[..AES256_KEY_LEN], &km[AES256_KEY_LEN..], &sealed[POINT_LEN..]);
    wipe(&mut km);
    r
}

/// A fresh sender ephemeral for `recipient_pub` (uncompressed point) and the
/// ECDH shared X it makes with that key: what an enclave key exchange
/// (`ock`) with the returned point must give back. Self-test only.
pub(crate) fn ecdh_probe_pair(recipient_pub: &[u8]) -> Result<([u8; POINT_LEN], [u8; 32])> {
    if recipient_pub.len() != POINT_LEN || recipient_pub[0] != 0x04 {
        return Err(EINVAL);
    }
    let (coords, shared) = p256_sender(&recipient_pub[1..])?;
    let mut point = [0u8; POINT_LEN];
    point[0] = 0x04;
    point[1..].copy_from_slice(&coords);
    Ok((point, shared))
}

/// Returns the enclave-ready blob: `ephemeral_point ‖ ciphertext ‖ tag`.
pub(crate) fn ecies_seal(recipient_pub: &[u8], secret: &[u8]) -> Result<KVec<u8>> {
    let parts = ecies_encrypt_parts(
        recipient_pub,
        secret,
        AES256_KEY_LEN,
        true,
        IV_LEN,
        false,
        0,
        0,
    )?;
    let mut blob = KVec::new();
    blob.extend_from_slice(&parts.eph_point, GFP_KERNEL)?;
    blob.extend_from_slice(&parts.ciphertext, GFP_KERNEL)?;
    blob.extend_from_slice(&parts.tag, GFP_KERNEL)?;
    Ok(blob)
}

#[::kernel::macros::kunit_tests(apple_sep_ecies_open)]
mod tests {
    use super::*;

    /// The P-256 generator as a recipient point: the ECDH shared X of an
    /// ephemeral `e·G` with `G` is that ephemeral's own X, so a test can open
    /// what it sealed without an enclave.
    fn generator() -> [u8; POINT_LEN] {
        let mut p = [0u8; POINT_LEN];
        p[0] = 0x04;
        p[1..33].copy_from_slice(&[
            0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
            0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
        ]);
        p[33..].copy_from_slice(&[
            0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
            0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5,
        ]);
        p
    }

    #[test]
    fn opens_what_it_sealed_with_the_shared_x() {
        let secret = [0x42u8; 32];
        let sealed = ecies_seal(&generator(), &secret).unwrap();
        let shared = &sealed[1..33];
        let got = ecies_open_with_shared(shared, &sealed).unwrap();
        assert_eq!(&got[..], &secret[..]);
    }

    #[test]
    fn refuses_a_wrong_shared_x_or_a_modified_blob() {
        let secret = [0x24u8; 20];
        let mut sealed = ecies_seal(&generator(), &secret).unwrap();
        let mut wrong = [0u8; 32];
        wrong.copy_from_slice(&sealed[1..33]);
        wrong[0] ^= 1;
        assert!(ecies_open_with_shared(&wrong, &sealed).is_err());
        let mut shared = [0u8; 32];
        shared.copy_from_slice(&sealed[1..33]);
        let last = sealed.len() - 1;
        sealed[last] ^= 0x80;
        assert!(ecies_open_with_shared(&shared, &sealed).is_err());
        assert!(ecies_open_with_shared(&shared, &sealed[..POINT_LEN]).is_err());
    }
}
