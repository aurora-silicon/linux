// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
//! Machine ref-key: Secure-Enclave key sealing, signing, and attestation.
//!
//! A ref-key (op `0x22`) is an EC keypair the enclave keeps; the private half
//! never leaves the SEP. The host-side ECIES encrypt lives in [`refkey_seal`];
//! every half that needs the private key runs in the enclave.

use crate::{image, keybag, proto, refkey_seal, shim};
use crate::{LockState, MachineRefKey, SepData, SksRequest};
use kernel::prelude::*;

pub(crate) const REFKEY_ENVELOPE_VERSION: u32 = 2;

impl SepData {
    fn sks_req_refkey(&self, keybag: i32, der_set: &[u8]) -> Result<SksRequest> {
        let mut body = image::Body::new();
        body.put_u32(0)?;
        body.put_u64(crate::sks::SKS_CLIENT_ID)?;
        body.put_i32(keybag)?;
        body.put_u32(crate::refkey::REFKEY_ENVELOPE_VERSION)?;
        body.put_blob(der_set)?;
        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        let msg =
            crate::sks::encode_sks_perform_operation(self.sks_next_seq(), len).ok_or(EINVAL)?;
        Ok(SksRequest {
            name: crate::sks::SKS_PERFORM_OP_NAME,
            msg,
            img,
        })
    }

    fn sks_create_refkey(
        &self,
        handle: crate::sks::KeyBagHandle,
        secret: &[u8],
    ) -> Option<KVec<u8>> {
        use crate::der::RefKeyValue as RV;

        // Class 9 = non-extractable (private stays enclave-resident); key type 5 = P-256.
        const PROTECTION_CLASS: u32 = 9;
        const KEY_TYPE: u32 = 5;

        // Unlock the bag first; an unbound create is refused.
        if let Some(healthy) = self.sks_health_check(c"ref-key create") {
            let _ = self.sks_send(self.sks_req_change_lock_state(
                handle,
                LockState::Unlocked,
                secret,
                healthy,
            ));
        }

        let create_der = {
            let mut items: KVec<(&[u8], RV<'_>)> = KVec::new();
            if items.push((b"o", RV::Utf8(b"oc")), GFP_KERNEL).is_err()
                || items
                    .push((b"bc", RV::Integer(PROTECTION_CLASS)), GFP_KERNEL)
                    .is_err()
                || items
                    .push((b"kt", RV::Integer(KEY_TYPE)), GFP_KERNEL)
                    .is_err()
            {
                return None;
            }
            crate::der::encode_refkey_set(&items).ok()?
        };
        let cout = self.sks_send(self.sks_req_refkey(handle.value(), &create_der))?;
        if cout.reply.status != 0 {
            return None;
        }
        let cbody = self.sks_report_response(crate::sks::SKS_PERFORM_OP_NAME, &cout)?;
        let mut cf = proto::FieldCursor::new(cbody);
        let (Some(0), Some(cblob)) = (cf.i32(), cf.blob()) else {
            return None;
        };
        let mut refkey_blob: KVec<u8> = KVec::new();
        refkey_blob.extend_from_slice(cblob, GFP_KERNEL).ok()?;
        Some(refkey_blob)
    }

    fn refkey_pub(blob: &[u8]) -> Option<&[u8]> {
        let rk_tlv = crate::der::refkey_find(blob, b"rk")?;
        let pub_raw =
            crate::der::refkey_find(rk_tlv, b"pub").and_then(crate::der::octet_string_body)?;
        (pub_raw.len() == refkey_seal::POINT_LEN).then_some(pub_raw)
    }

    pub(crate) fn sks_machine_refkey(&self, handle: crate::sks::KeyBagHandle, secret: &[u8]) {
        const MACHINE_REFKEY_PATH: &CStr = c"/var/lib/aurora-sep-refkey.bin";

        if self.machine_refkey.lock().is_some() {
            return;
        }

        if let Ok(f) = shim::StoreFile::open_readonly(MACHINE_REFKEY_PATH) {
            let sz = f.size().unwrap_or(0);
            if (1..=8192).contains(&sz) {
                let mut blob: KVec<u8> = KVec::new();
                if blob.resize(sz as usize, 0u8, GFP_KERNEL).is_ok()
                    && f.read_exact(0, &mut blob).is_ok()
                    && self.cache_machine_refkey(blob)
                {
                    return;
                }
            }
        }

        let Some(blob) = self.sks_create_refkey(handle, secret) else {
            dev_warn!(
                self.dev,
                "sks: could not create the machine ref-key; trusted-key sealing is unavailable this boot.\n"
            );
            return;
        };
        if let Ok(f) = shim::StoreFile::open(MACHINE_REFKEY_PATH) {
            if f.write_all(0, &blob).is_ok() {
                let _ = f.sync();
            }
        }
        let _ = self.cache_machine_refkey(blob);
    }

    fn cache_machine_refkey(&self, blob: KVec<u8>) -> bool {
        let Some(pubv) = Self::refkey_pub(&blob) else {
            return false;
        };
        let mut pub_raw: KVec<u8> = KVec::new();
        if pub_raw.extend_from_slice(pubv, GFP_KERNEL).is_err() {
            return false;
        }
        *self.machine_refkey.lock() = Some(MachineRefKey { blob, pub_raw });
        true
    }

    fn ensure_machine_refkey(&self) -> Result<()> {
        if self.machine_refkey.lock().is_some() {
            return Ok(());
        }
        if !self.sks_ready() {
            return Err(ENODEV);
        }
        let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
            return Err(ENODEV);
        };
        let (handle, _uuid) = self.sks_recover(&stored).ok_or(EIO)?;
        self.sks_machine_refkey(handle, stored.secret());
        let _ = self.sks_send(self.sks_req_unload_keybag(handle));
        if self.machine_refkey.lock().is_some() {
            Ok(())
        } else {
            Err(EIO)
        }
    }

    pub(crate) fn refkey_seal_trusted(&self, key: &[u8]) -> Result<KVec<u8>> {
        self.ensure_machine_refkey()?;
        let guard = self.machine_refkey.lock();
        let mk = guard.as_ref().ok_or(ENODEV)?;
        refkey_seal::ecies_seal(&mk.pub_raw, key)
    }

    pub(crate) fn refkey_unseal_trusted(&self, sealed: &[u8]) -> Result<KVec<u8>> {
        self.ensure_machine_refkey()?;
        let blob = {
            let guard = self.machine_refkey.lock();
            let mk = guard.as_ref().ok_or(ENODEV)?;
            let mut b: KVec<u8> = KVec::new();
            b.extend_from_slice(&mk.blob, GFP_KERNEL)?;
            b
        };
        let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
            return Err(ENODEV);
        };
        let (handle, _uuid) = self.sks_recover(&stored).ok_or(EIO)?;
        if let Some(healthy) = self.sks_health_check(c"trusted-key unseal") {
            let _ = self.sks_send(self.sks_req_change_lock_state(
                handle,
                LockState::Unlocked,
                stored.secret(),
                healthy,
            ));
        }
        let recovered = self.sks_refkey_unseal(handle, &blob, sealed);
        let _ = self.sks_send(self.sks_req_unload_keybag(handle));
        recovered.ok_or(EIO)
    }

    /// Op `osgn`: ECDSA-P256 over `challenge` as the pre-computed digest.
    fn sks_refkey_sign(
        &self,
        handle: crate::sks::KeyBagHandle,
        refkey_blob: &[u8],
        challenge: &[u8],
    ) -> Option<KVec<u8>> {
        use crate::der::RefKeyValue as RV;
        let mut items: KVec<(&[u8], RV<'_>)> = KVec::new();
        items.push((b"o", RV::Utf8(b"osgn")), GFP_KERNEL).ok()?;
        items.push((b"d", RV::Octets(challenge)), GFP_KERNEL).ok()?;
        items.push((b"rk", RV::Der(refkey_blob)), GFP_KERNEL).ok()?;
        let der = crate::der::encode_refkey_set(&items).ok()?;
        let out = self.sks_send(self.sks_req_refkey(handle.value(), &der))?;
        if out.reply.status != 0 {
            dev_warn!(
                self.dev,
                "sks: ref-key attest sign failed (status {})\n",
                out.reply.status
            );
            return None;
        }
        let body = self.sks_report_response(crate::sks::SKS_PERFORM_OP_NAME, &out)?;
        let mut f = proto::FieldCursor::new(body);
        let (Some(0), Some(sig)) = (f.i32(), f.blob()) else {
            return None;
        };
        // Enclave wraps the sig in OCTET STRING { SEQUENCE { r, s } }; strip to inner DER.
        let der_sig = crate::der::octet_string_body(sig).unwrap_or(sig);
        let mut owned: KVec<u8> = KVec::new();
        owned.extend_from_slice(der_sig, GFP_KERNEL).ok()?;
        Some(owned)
    }

    /// Attestation of key possession by signing proof; op `oa` (attestation-chained)
    /// needs the SEP device attestation key, absent on a Linux-attached SEP.
    pub(crate) fn refkey_attest_sign(&self, challenge: &[u8]) -> Result<(KVec<u8>, KVec<u8>)> {
        self.ensure_machine_refkey()?;
        let (blob, pubk) = {
            let guard = self.machine_refkey.lock();
            let mk = guard.as_ref().ok_or(ENODEV)?;
            let mut b: KVec<u8> = KVec::new();
            b.extend_from_slice(&mk.blob, GFP_KERNEL)?;
            let mut p: KVec<u8> = KVec::new();
            p.extend_from_slice(&mk.pub_raw, GFP_KERNEL)?;
            (b, p)
        };
        let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
            return Err(ENODEV);
        };
        let (handle, _uuid) = self.sks_recover(&stored).ok_or(EIO)?;
        if let Some(healthy) = self.sks_health_check(c"ref-key attest") {
            let _ = self.sks_send(self.sks_req_change_lock_state(
                handle,
                LockState::Unlocked,
                stored.secret(),
                healthy,
            ));
        }
        let sig = self.sks_refkey_sign(handle, &blob, challenge);
        let _ = self.sks_send(self.sks_req_unload_keybag(handle));
        Ok((sig.ok_or(EIO)?, pubk))
    }

    /// Enclave ECIES-decrypt (o = "oecd"); takes the ephemeral from the front of `d`.
    fn sks_refkey_unseal(
        &self,
        handle: crate::sks::KeyBagHandle,
        refkey_blob: &[u8],
        sealed: &[u8],
    ) -> Option<KVec<u8>> {
        use crate::der::RefKeyValue as RV;
        let mut items: KVec<(&[u8], RV<'_>)> = KVec::new();
        items.push((b"o", RV::Utf8(b"oecd")), GFP_KERNEL).ok()?;
        items.push((b"d", RV::Octets(sealed)), GFP_KERNEL).ok()?;
        items.push((b"rk", RV::Der(refkey_blob)), GFP_KERNEL).ok()?;
        let der = crate::der::encode_refkey_set(&items).ok()?;
        let out = self.sks_send(self.sks_req_refkey(handle.value(), &der))?;
        if out.reply.status != 0 {
            dev_warn!(
                self.dev,
                "trusted-keys: ref-key unseal failed (status {})\n",
                out.reply.status
            );
            return None;
        }
        let body = self.sks_report_response(crate::sks::SKS_PERFORM_OP_NAME, &out)?;
        let mut f = proto::FieldCursor::new(body);
        match (f.i32(), f.blob()) {
            (Some(0), Some(rec)) => {
                let mut owned = KVec::new();
                owned.extend_from_slice(rec, GFP_KERNEL).ok()?;
                Some(owned)
            }
            _ => None,
        }
    }
}
