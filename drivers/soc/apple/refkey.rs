// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj
//! Machine ref-key: Secure-Enclave key sealing, signing, and attestation.
//!
//! A ref-key (op `0x22`) is an EC keypair the enclave keeps; the private half
//! never leaves the SEP. The host-side ECIES encrypt lives in [`refkey_seal`];
//! every half that needs the private key runs in the enclave.

use crate::{image, keybag, profile, proto, refkey_seal, shim};
use crate::{LockState, MachineRefKey, SepData, SksRequest};
use kernel::prelude::*;
use kernel::sync::atomic::Relaxed;

pub(crate) const REFKEY_ENVELOPE_VERSION: u32 = 2;

impl SepData {
    /// Keep only decoded bytes across switch-root, never an open initrd file.
    fn with_identity_keybag<T>(
        &self,
        f: impl FnOnce(&keybag::StoredKeyBag) -> Result<T>,
    ) -> Result<T> {
        let mut cache = self.identity_keybag.lock();
        if self.keystore_only.load(Relaxed) {
            if cache.is_none() {
                let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
                    return Err(ENODEV);
                };
                *cache = Some(stored);
            }
            return f(cache.as_ref().ok_or(ENODEV)?);
        }
        drop(cache);
        let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
            return Err(ENODEV);
        };
        f(&stored)
    }

    fn sks_req_refkey(&self, keybag: i32, der_set: &[u8]) -> Result<SksRequest> {
        let (keybag, envelope) = match self.profile.key_store {
            // The 13.5 operation envelope is 3. Use the designated identity
            // session rather than the source handle; class F with -1 expired
            // at the next boot on J313.
            profile::KeyStore::Sepos13 { .. } => {
                let user = crate::sks::DesignateUser::new(crate::SBIO_PROBE_USER_ID)
                    .ok_or(EINVAL)?;
                (user.special_handle().value(), 3)
            }
            profile::KeyStore::Variant5 => (keybag, REFKEY_ENVELOPE_VERSION),
        };
        let mut body = image::Body::new();
        body.put_u32(0)?;
        body.put_u64(crate::sks::SKS_CLIENT_ID)?;
        body.put_i32(keybag)?;
        body.put_u32(envelope)?;
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

    fn sks_prepare_identity_refkey(
        &self,
        handle: crate::sks::KeyBagHandle,
        secret: &[u8],
    ) -> bool {
        if !self.keybag_designated.load(Relaxed) {
            self.sks_designate_user_keybag(handle, secret);
        }
        if !self.keybag_designated.load(Relaxed) {
            dev_warn!(self.dev, "sks: ref-key identity designation failed\n");
            return false;
        }
        let Some(user) = crate::sks::DesignateUser::new(crate::SBIO_PROBE_USER_ID) else {
            return false;
        };
        let Some(healthy) = self.sks_health_check(c"ref-key identity unlock") else {
            return false;
        };
        let Some(out) = self.sks_send(self.sks_req_unlock_special(
            user.special_handle(),
            secret,
            healthy,
        )) else {
            dev_warn!(self.dev, "sks: ref-key identity unlock got no reply\n");
            return false;
        };
        dev_info!(
            self.dev,
            "sks: ref-key identity unlock mailbox status {}\n",
            out.reply.status
        );
        if out.reply.status != 0 {
            return false;
        }
        let Some(body) = self.sks_report_response(c"DEVICE_STATE_TRANSITION", &out) else {
            return false;
        };
        body.len() == 20 && body[..4] == [0; 4]
    }

    fn sks_create_refkey(
        &self,
        handle: crate::sks::KeyBagHandle,
        secret: &[u8],
        acl: Option<&[u8]>,
    ) -> Option<KVec<u8>> {
        let (protection_class, key_type) = match self.profile.key_store {
            // J313 accepted this non-class-F class and 13.5 EC key type for
            // creation, same-boot unseal, and post-reboot unseal. The exact
            // Apple policy name for class 9 is not established here.
            profile::KeyStore::Sepos13 { .. } => (9, 4),
            profile::KeyStore::Variant5 => (9, 5),
        };

        // A ref-key created this boot belongs to the current boot policy.
        crate::xart_guard::close_policy_window();
        if self.profile.key_store == profile::KeyStore::Variant5 {
            // On variant 5 the ref-key is bound to the unlocked identity bag.
            if let Some(healthy) = self.sks_health_check(c"ref-key create") {
                let _ = self.sks_send(self.sks_req_change_lock_state(
                    handle,
                    LockState::Unlocked,
                    secret,
                    healthy,
                ));
            }
        } else if !self.sks_prepare_identity_refkey(handle, secret) {
            return None;
        }

        self.sks_create_refkey_at(handle.value(), protection_class, key_type, acl)
    }

    /// The ref-key create itself, with `handle` as the outer key-bag handle:
    /// a loaded bag, or a designated user's special handle `-uid` (the bag a
    /// macOS Secure Enclave key lives under). The bag must already be
    /// unlocked.
    fn sks_create_refkey_at(
        &self,
        handle: i32,
        protection_class: u32,
        key_type: u32,
        acl: Option<&[u8]>,
    ) -> Option<KVec<u8>> {
        use crate::der::RefKeyValue as RV;
        let create_der = {
            let mut items: KVec<(&[u8], RV<'_>)> = KVec::new();
            if items.push((b"o", RV::Utf8(b"oc")), GFP_KERNEL).is_err()
                || items
                    .push((b"bc", RV::Integer(protection_class)), GFP_KERNEL)
                    .is_err()
                || items
                    .push((b"kt", RV::Integer(key_type)), GFP_KERNEL)
                    .is_err()
            {
                return None;
            }
            // `ed` carries the key's access-control list; the enclave keeps it
            // in the record and enforces it on every later operation.
            if let Some(acl) = acl {
                items.push((b"ed", RV::Der(acl)), GFP_KERNEL).ok()?;
            }
            crate::der::encode_refkey_set(&items).ok()?
        };
        let cout = match self.sks_send(self.sks_req_refkey(handle, &create_der)) {
            Some(out) => out,
            None => {
                dev_warn!(self.dev, "sks: ref-key create got no usable reply\n");
                return None;
            }
        };
        dev_info!(
            self.dev,
            "sks: ref-key create mailbox status {}, response {} bytes\n",
            cout.reply.status,
            cout.response.len()
        );
        if cout.reply.status != 0 {
            return None;
        }
        let cbody = self.sks_report_response(crate::sks::SKS_PERFORM_OP_NAME, &cout)?;
        let mut cf = proto::FieldCursor::new(cbody);
        let (Some(version), Some(cblob)) = (cf.i32(), cf.blob()) else {
            dev_warn!(self.dev, "sks: ref-key create reply was malformed\n");
            return None;
        };
        if version != 0 {
            dev_warn!(self.dev, "sks: ref-key create reply version {}\n", version);
            return None;
        }
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
        const MACHINE_REFKEY_V2_PATH: &CStr = c"/var/lib/aurora-sep-refkey-v2.bin";
        let path = if crate::module_parameters::refkey_v2.value() != 0 {
            MACHINE_REFKEY_V2_PATH
        } else {
            MACHINE_REFKEY_PATH
        };

        if self.machine_refkey.lock().is_some() {
            return;
        }

        if let Ok(f) = shim::StoreFile::open_readonly(path) {
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
            dev_err!(
                self.dev,
                "sks: existing machine ref-key is unreadable or invalid; refusing replacement\n"
            );
            return;
        }

        if self.keystore_only.load(Relaxed) {
            // A missing initrd copy is never permission to replace an identity.
            return;
        }
        let Some(blob) = self.sks_create_refkey(handle, secret, None) else {
            dev_warn!(
                self.dev,
                "sks: could not create the machine ref-key; trusted-key sealing is unavailable this boot.\n"
            );
            return;
        };
        let Ok(f) = shim::StoreFile::open(path) else {
            dev_err!(self.dev, "sks: could not persist machine ref-key\n");
            return;
        };
        if f.write_all(0, &blob).is_err() || f.sync().is_err() {
            dev_err!(self.dev, "sks: machine ref-key persistence failed\n");
            return;
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
        self.with_identity_keybag(|stored| {
            let (handle, _uuid) = self.sks_recover(stored).ok_or(EIO)?;
            self.sks_machine_refkey(handle, stored.secret());
            let _ = self.sks_send(self.sks_req_unload_keybag(handle));
            Ok(())
        })?;
        if self.machine_refkey.lock().is_some() {
            Ok(())
        } else {
            Err(EIO)
        }
    }

    pub(crate) fn refkey_seal_trusted(&self, key: &[u8]) -> Result<KVec<u8>> {
        self.wait_for_trusted_ready()?;
        self.ensure_machine_refkey()?;
        let sealed = {
            let guard = self.machine_refkey.lock();
            let mk = guard.as_ref().ok_or(ENODEV)?;
            refkey_seal::ecies_seal(&mk.pub_raw, key)?
        };

        // The cached public key can still encrypt after the identity bag
        // stops admitting its private ref-key. Do not return a new trusted
        // blob unless this bag can recover it. Release the cache lock before
        // unsealing, which acquires that lock again.
        let recovered = crate::Secret(self.refkey_unseal_trusted(&sealed)?);
        if &*recovered != key {
            dev_err!(self.dev, "trusted-keys: refusing a new blob whose private-key round trip failed\n");
            return Err(EIO);
        }
        Ok(sealed)
    }

    pub(crate) fn refkey_unseal_trusted(&self, sealed: &[u8]) -> Result<KVec<u8>> {
        let refusals = self.keystore_write_refusals.load(Relaxed);
        let recovered = self.refkey_unseal_cached(sealed)?;
        if !crate::attach::unseal_allowed(refusals, self.keystore_write_refusals.load(Relaxed)) {
            drop(crate::Secret(recovered));
            return Err(EIO); // trusted.rs maps every unseal rejection to EBADMSG.
        }
        Ok(recovered)
    }

    fn refkey_unseal_cached(&self, sealed: &[u8]) -> Result<KVec<u8>> {
        self.wait_for_trusted_ready()?;
        self.ensure_machine_refkey()?;
        let blob = {
            let guard = self.machine_refkey.lock();
            let mk = guard.as_ref().ok_or(ENODEV)?;
            let mut b: KVec<u8> = KVec::new();
            b.extend_from_slice(&mk.blob, GFP_KERNEL)?;
            b
        };
        self.with_identity_keybag(|stored| {
            let (handle, _uuid) = self.sks_recover(stored).ok_or(EIO)?;
            // The first unlock under a changed boot policy rewrites one session
            // record; see crate::xart_guard.
            let unlock = || {
                if self.profile.key_store == profile::KeyStore::Variant5 {
                    if let Some(healthy) = self.sks_health_check(c"trusted-key unseal") {
                        let _ = self.sks_send(self.sks_req_change_lock_state(
                            handle,
                            LockState::Unlocked,
                            stored.secret(),
                            healthy,
                        ));
                    }
                    true
                } else {
                    self.sks_prepare_identity_refkey(handle, stored.secret())
                }
            };
            // Do not consume the real root's one policy window in the initrd.
            let unlocked = if self.keystore_only.load(Relaxed) {
                unlock()
            } else {
                crate::xart_guard::policy_window(unlock)
            };
            if !unlocked {
                let _ = self.sks_send(self.sks_req_unload_keybag(handle));
                return Err(EIO);
            }
            let recovered = self.sks_refkey_unseal(handle, &blob, sealed);
            let _ = self.sks_send(self.sks_req_unload_keybag(handle));
            recovered.ok_or(EIO)
        })
    }

    /// Op `osgn`: ECDSA-P256 over `challenge` as the pre-computed digest.
    /// `acm` is the credential context an access-controlled key's operation
    /// is checked against (`acmh`). `EKEYREJECTED` when the enclave refuses
    /// the operation (a record another SEP or key bag made, or an access-
    /// control list the context does not satisfy), `EIO` for no or a
    /// malformed reply. The enclave's status is logged and kept in
    /// `last_key_status` for the self-test.
    fn sks_refkey_sign(
        &self,
        handle: crate::sks::KeyBagHandle,
        refkey_blob: &[u8],
        challenge: &[u8],
        acm: Option<&[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]>,
    ) -> Result<KVec<u8>> {
        use crate::der::RefKeyValue as RV;
        let mut items: KVec<(&[u8], RV<'_>)> = KVec::new();
        items.push((b"o", RV::Utf8(b"osgn")), GFP_KERNEL)?;
        items.push((b"d", RV::Octets(challenge)), GFP_KERNEL)?;
        items.push((b"rk", RV::Der(refkey_blob)), GFP_KERNEL)?;
        if let Some(acm) = acm {
            items.push((b"acmh", RV::Octets(acm)), GFP_KERNEL)?;
        }
        let der = crate::der::encode_refkey_set(&items)?;
        let Some(out) = self.sks_send(self.sks_req_refkey(handle.value(), &der)) else {
            self.last_key_status.store(i32::MIN as u32, Relaxed);
            return Err(EIO);
        };
        self.last_key_status.store(i32::from(out.reply.status) as u32, Relaxed);
        if out.reply.status != 0 {
            dev_warn!(
                self.dev,
                "sks: ref-key sign refused by the enclave (status {}{})\n",
                out.reply.status,
                (if acm.is_some() { ", with a credential context" } else { "" })
            );
            return Err(EKEYREJECTED);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_PERFORM_OP_NAME, &out)
            .ok_or(EIO)?;
        let mut f = proto::FieldCursor::new(body);
        let (Some(0), Some(sig)) = (f.i32(), f.blob()) else {
            return Err(EIO);
        };
        // Enclave wraps the sig in OCTET STRING { SEQUENCE { r, s } }; strip to inner DER.
        let der_sig = crate::der::octet_string_body(sig).unwrap_or(sig);
        let mut owned: KVec<u8> = KVec::new();
        owned.extend_from_slice(der_sig, GFP_KERNEL)?;
        Ok(owned)
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
        crate::xart_guard::policy_window(|| {
            if let Some(healthy) = self.sks_health_check(c"ref-key attest") {
                let _ = self.sks_send(self.sks_req_change_lock_state(
                    handle,
                    LockState::Unlocked,
                    stored.secret(),
                    healthy,
                ));
            }
        });
        let sig = self.sks_refkey_sign(handle, &blob, challenge, None);
        let _ = self.sks_send(self.sks_req_unload_keybag(handle));
        Ok((sig?, pubk))
    }

    /// Runs `op` with the identity key bag loaded and unlocked, as a trusted
    /// key unseal does, and unloads the bag afterwards whatever `op` returns.
    fn with_identity_bag<T>(
        &self,
        op: impl FnOnce(crate::sks::KeyBagHandle, &[u8]) -> Result<T>,
    ) -> Result<T> {
        self.wait_for_trusted_ready()?;
        let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
            return Err(ENODEV);
        };
        let (handle, _uuid) = self.sks_recover(&stored).ok_or(EIO)?;
        let unlocked = crate::xart_guard::policy_window(|| {
            if self.profile.key_store == profile::KeyStore::Variant5 {
                if let Some(healthy) = self.sks_health_check(c"sep-key unlock") {
                    let _ = self.sks_send(self.sks_req_change_lock_state(
                        handle,
                        LockState::Unlocked,
                        stored.secret(),
                        healthy,
                    ));
                }
                true
            } else {
                self.sks_prepare_identity_refkey(handle, stored.secret())
            }
        });
        let result = if unlocked {
            op(handle, stored.secret())
        } else {
            Err(EIO)
        };
        let _ = self.sks_send(self.sks_req_unload_keybag(handle));
        result
    }

    /// `/dev/sep-key` create: a new enclave P-256 ref-key under the identity
    /// key bag. Returns its record and public point; the private half stays
    /// in the enclave. With `acl` the record carries that access-control
    /// list (see `bio_acl`).
    pub(crate) fn user_key_create(
        &self,
        acl: Option<&[u8]>,
    ) -> Result<(KVec<u8>, [u8; refkey_seal::POINT_LEN])> {
        let blob = self.with_identity_bag(|handle, secret| {
            self.sks_create_refkey(handle, secret, acl).ok_or(EIO)
        })?;
        let mut point = [0u8; refkey_seal::POINT_LEN];
        point.copy_from_slice(Self::refkey_pub(&blob).ok_or(EIO)?);
        Ok((blob, point))
    }

    /// Test only (`key_selftest`): a ref-key under the designated user bag
    /// `user` (outer handle `-uid`), which the caller has unlocked. Variant-5
    /// key store only.
    pub(crate) fn user_key_create_in_bag(
        &self,
        user: crate::sks::DesignateUser,
        acl: Option<&[u8]>,
    ) -> Result<KVec<u8>> {
        if self.profile.key_store != profile::KeyStore::Variant5 {
            return Err(EOPNOTSUPP);
        }
        crate::xart_guard::close_policy_window();
        self.sks_create_refkey_at(user.special_handle().value(), 9, 5, acl).ok_or(EIO)
    }

    /// Test only: sign with a ref-key under the designated user bag `user`.
    pub(crate) fn user_key_sign_in_bag(
        &self,
        user: crate::sks::DesignateUser,
        blob: &[u8],
        digest: &[u8; 32],
        acm: Option<&[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]>,
    ) -> Result<KVec<u8>> {
        let handle = crate::sks::KeyBagHandle::from_load_reply(user.special_handle().value());
        self.sks_refkey_sign(handle, blob, digest, acm)
    }

    /// The public point recorded in a ref-key record; no enclave round trip.
    pub(crate) fn user_key_public(blob: &[u8]) -> Result<[u8; refkey_seal::POINT_LEN]> {
        let mut point = [0u8; refkey_seal::POINT_LEN];
        point.copy_from_slice(Self::refkey_pub(blob).ok_or(EINVAL)?);
        Ok(point)
    }

    /// `/dev/sep-key` sign: the enclave's ECDSA-P256 over `digest` with the
    /// key in `blob`, checked against the credential context `acm` when one
    /// is given. A record this SEP and key bag did not make, or an access-
    /// control list `acm` does not satisfy, is refused by the enclave
    /// (`EKEYREJECTED`).
    pub(crate) fn user_key_sign(
        &self,
        blob: &[u8],
        digest: &[u8; 32],
        acm: Option<&[u8; crate::scrd::SCRD_ACM_HANDLE_LEN]>,
    ) -> Result<KVec<u8>> {
        // A record without a public point is not a ref-key record at all.
        Self::refkey_pub(blob).ok_or(EINVAL)?;
        self.with_identity_bag(|handle, _secret| self.sks_refkey_sign(handle, blob, digest, acm))
    }

    /// Volume-key sealing: a ref-key without an access-control list under
    /// the designated user bag `user` (outer handle `-uid`), protection
    /// class 9, so the enclave uses it only while that bag is unlocked. The
    /// caller has unlocked the bag with the user's password. Variant-5 key
    /// store only.
    pub(crate) fn fde_key_create(&self, user: crate::sks::DesignateUser) -> Result<KVec<u8>> {
        if self.profile.key_store != profile::KeyStore::Variant5 {
            return Err(EOPNOTSUPP);
        }
        crate::xart_guard::close_policy_window();
        let blob = self
            .sks_create_refkey_at(user.special_handle().value(), 9, 5, None)
            .ok_or(EACCES)?;
        Self::refkey_pub(&blob).ok_or(EIO)?;
        Ok(blob)
    }

    /// The OS install this kernel runs as (the explicit `os_uuid_hi/lo`
    /// identity, else `/chosen/apfs-preboot-uuid`), which FDE secrets are
    /// bound to, as FileVault binds a volume key to its volume.
    fn fde_os_identity(&self) -> Result<[u8; 16]> {
        self.xarm.lock().os_uuid.ok_or(ENOKEY)
    }

    /// ECIES-encrypts `secret`, framed with this OS install's identity, to the
    /// sealing key in `record` and has the enclave open the result once before
    /// returning it, so a sealed blob this bag cannot open is never handed
    /// out. `ENOKEY` when this kernel has no OS identity.
    pub(crate) fn fde_seal(
        &self,
        user: crate::sks::DesignateUser,
        record: &[u8],
        secret: &[u8],
    ) -> Result<KVec<u8>> {
        let os = self.fde_os_identity()?;
        let framed = crate::Secret(crate::userbag::fde_bind(&os, secret)?);
        let sealed = refkey_seal::ecies_seal(Self::refkey_pub(record).ok_or(EINVAL)?, &framed)?;
        let recovered = crate::Secret(self.fde_unseal(user, record, &sealed)?);
        if &*recovered != secret {
            dev_err!(self.dev, "fde: refusing a sealed volume secret whose round trip failed\n");
            return Err(EIO);
        }
        Ok(sealed)
    }

    /// Has the enclave open `sealed` with the sealing key in `record`. It
    /// does so only on the Mac whose enclave made the key and only while
    /// `user`'s bag is unlocked; any refusal is `EKEYREJECTED`. The secret is
    /// returned only to the OS install it was sealed under: `EXDEV` for
    /// another install, `ENOKEY` when this kernel has no OS identity.
    pub(crate) fn fde_unseal(
        &self,
        user: crate::sks::DesignateUser,
        record: &[u8],
        sealed: &[u8],
    ) -> Result<KVec<u8>> {
        if self.profile.key_store != profile::KeyStore::Variant5 {
            return Err(EOPNOTSUPP);
        }
        Self::refkey_pub(record).ok_or(EINVAL)?;
        let os = self.fde_os_identity()?;
        let handle = crate::sks::KeyBagHandle::from_load_reply(user.special_handle().value());
        let framed =
            crate::Secret(self.sks_refkey_unseal(handle, record, sealed).ok_or(EKEYREJECTED)?);
        let secret = crate::userbag::fde_unbind(&os, &framed)?;
        let mut out = KVec::with_capacity(secret.len(), GFP_KERNEL)?;
        out.extend_from_slice(secret, GFP_KERNEL)?;
        Ok(out)
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
