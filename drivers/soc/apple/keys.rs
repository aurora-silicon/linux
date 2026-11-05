// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Application-owned biometric P-256 keys. Authentication contexts stay in kernel.

use crate::{bio, der, keybag, profile, proto, sbio, scrd, SepData};
use crate::der::RefKeyValue as V;
use kernel::prelude::*;
use kernel::sync::atomic::Relaxed;

pub(crate) const MAX_BLOB: usize = 4096;

/// A request owns a fresh credential. Discard the grant even on cancellation or
/// transport failure; a failed deletion disables application keys until reboot.
pub(crate) struct Credential<'a> {
    sep: &'a SepData,
    handle: [u8; scrd::SCRD_ACM_HANDLE_LEN],
}

impl<'a> Credential<'a> {
    pub(crate) fn new(sep: &'a SepData, handle: [u8; scrd::SCRD_ACM_HANDLE_LEN]) -> Self {
        Self { sep, handle }
    }

    pub(crate) fn handle(&self) -> &[u8; scrd::SCRD_ACM_HANDLE_LEN] {
        &self.handle
    }

    pub(crate) fn into_handle(self) -> [u8; scrd::SCRD_ACM_HANDLE_LEN] {
        let handle = self.handle;
        core::mem::forget(self);
        handle
    }
}

impl Drop for Credential<'_> {
    fn drop(&mut self) {
        if !matches!(self.sep.scrd_command(&scrd::scrd_context_delete(&self.handle)), Some((0, _))) {
            self.sep.key_api_failed.store(true, Relaxed);
            dev_err!(self.sep.dev, "keys: credential disposal failed; application key operations disabled until reboot\n");
        }
        self.handle.fill(0);
    }
}

impl SepData {
    pub(crate) fn biometric_keys_supported(&self) -> bool {
        // The credential-match layout and policy are validated on this protocol
        // generation only. Other profiles retain their existing capabilities.
        matches!(self.profile.key_store, profile::KeyStore::Sepos13 { .. })
            && !self.key_api_failed.load(Relaxed)
    }

    pub(crate) fn key_credential(&self) -> Result<Credential<'_>> {
        if !self.biometric_keys_supported() {
            return Err(Error::from_errno(-(kernel::bindings::EOPNOTSUPP as i32)));
        }
        let keybag::State::Present(stored) = keybag::read(keybag::Slot::Identity)? else {
            return Err(ENOENT);
        };
        let (handle, _) = self.sks_recover(&stored).ok_or(EIO)?;
        let result = (|| {
            if !self.sks_prepare_identity_refkey(handle, stored.secret()) {
                return Err(EIO);
            }
            let user = sbio::UserId::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
            let context = self.establish_passcode_validated_context(user).ok_or_else(|| {
                self.key_api_failed.store(true, Relaxed);
                EIO
            })?;
            Ok(Credential::new(self, context))
        })();
        let unloaded = self.sks_send(self.sks_req_unload_keybag(handle));
        if !matches!(unloaded, Some(ref out) if out.reply.status == 0) {
            self.key_api_failed.store(true, Relaxed);
            return Err(EIO);
        }
        result
    }

    fn application_refkey_op(&self, items: &[(&[u8], V<'_>)]) -> Result<KVec<u8>> {
        let encoded = der::encode_refkey_set(items)?;
        let out = self.sks_send(self.sks_req_refkey(0, &encoded)).ok_or(EIO)?;
        if out.reply.status == -8 {
            return Err(EACCES);
        }
        if out.reply.status != 0 {
            return Err(EIO);
        }
        let body = self.sks_report_response(crate::sks::SKS_PERFORM_OP_NAME, &out).ok_or(EIO)?;
        let mut fields = proto::FieldCursor::new(body);
        match fields.i32().ok_or(EIO)? {
            0 => {},
            -8 => return Err(EACCES),
            _ => return Err(EIO),
        }
        let blob = fields.blob().ok_or(EIO)?;
        if blob.is_empty() || blob.len() > MAX_BLOB {
            return Err(EIO);
        }
        let mut owned = KVec::new();
        owned.extend_from_slice(blob, GFP_KERNEL)?;
        Ok(owned)
    }

    pub(crate) fn create_biometric_key(&self) -> Result<(KVec<u8>, [u8; bio::ATTEST_PUB_LEN])> {
        let credential = self.key_credential()?;
        let result = (|| {
            let user = sbio::UserId::new(crate::SBIO_PROBE_USER_ID).ok_or(EINVAL)?;
            let crate::SbioOutcome::Ok(binding) = self.sbio_call(&sbio::sbio_catacomb_id(user)) else {
                return Err(EIO);
            };
            let binding: &[u8; 16] = binding.as_slice().try_into().map_err(|_| EIO)?;
            let policy = crate::refkey_policy::biometric_signing_policy(binding)?;
            let blob = self.application_refkey_op(&[
                (b"o", V::Utf8(b"oc")),
                (b"bc", V::Integer(9)),
                (b"kt", V::Integer(4)),
                (b"ed", V::Der(&policy)),
                (b"acmh", V::Octets(credential.handle())),
            ])?;
            let mut public = [0; bio::ATTEST_PUB_LEN];
            public.copy_from_slice(Self::refkey_pub(&blob).ok_or(EIO)?);
            Ok((blob, public))
        })();
        drop(credential);
        if self.key_api_failed.load(Relaxed) { return Err(EIO); }
        result
    }

    pub(crate) fn sign_biometric_key(
        &self,
        blob: &[u8],
        digest: &[u8; 32],
        credential: &Credential<'_>,
    ) -> Result<Signature> {
        let encoded = self.application_refkey_op(&[
            (b"o", V::Utf8(b"osgn")),
            (b"d", V::Octets(digest)),
            (b"rk", V::Der(blob)),
            (b"acmh", V::Octets(credential.handle())),
        ])?;
        let bytes = der::octet_string_body(&encoded).unwrap_or(&encoded);
        if bytes.len() > bio::ATTEST_SIG_MAX || bytes.first() != Some(&0x30) {
            return Err(EIO);
        }
        let mut signature = Signature { len: bytes.len() as u32, bytes: [0; bio::ATTEST_SIG_MAX] };
        signature.bytes[..bytes.len()].copy_from_slice(bytes);
        Ok(signature)
    }
}

pub(crate) struct Signature {
    pub(crate) len: u32,
    pub(crate) bytes: [u8; bio::ATTEST_SIG_MAX],
}

pub(crate) struct SignRequest {
    pub(crate) blob: KVec<u8>,
    pub(crate) digest: [u8; 32],
    pub(crate) deadline_ns: u64,
}

pub(crate) struct Signing {
    pub(crate) request: Option<SignRequest>,
    pub(crate) deadline_ns: u64,
    pub(crate) terminal: Option<Result<Signature>>,
    pub(crate) guidance: bio::Guidance,
}

#[repr(C)]
struct Create {
    flags: u32,
    blob_len: u32,
    blob_ptr: u64,
    public: [u8; bio::ATTEST_PUB_LEN],
    reserved: [u8; 7],
}

#[repr(C)]
struct SignStart {
    flags: u32,
    timeout_ms: u32,
    blob_len: u32,
    reserved: u32,
    blob_ptr: u64,
    digest: [u8; 32],
}

#[repr(C)]
pub(crate) struct SignPoll {
    pub(crate) state: u32,
    pub(crate) status: i32,
    pub(crate) guidance: u32,
    pub(crate) sig_len: u32,
    pub(crate) signature: [u8; bio::ATTEST_SIG_MAX],
}

static_assert!(core::mem::size_of::<Create>() == 88);
static_assert!(core::mem::size_of::<SignStart>() == 56);
static_assert!(core::mem::size_of::<SignPoll>() == 88);
// SAFETY: these repr(C) structures contain only integers/bytes and no padding.
unsafe impl kernel::transmute::AsBytes for Create {}
// SAFETY: every bit pattern is valid for all fields.
unsafe impl kernel::transmute::FromBytes for Create {}
// SAFETY: every bit pattern is valid for all fields.
unsafe impl kernel::transmute::FromBytes for SignStart {}
// SAFETY: repr(C), all fields initialized, no padding.
unsafe impl kernel::transmute::AsBytes for SignPoll {}

pub(crate) const IOC_CREATE: u32 = kernel::ioctl::_IOWR::<Create>(0xb1, 0x0b);
pub(crate) const IOC_SIGN_START: u32 = kernel::ioctl::_IOW::<SignStart>(0xb1, 0x0c);
pub(crate) const IOC_SIGN_POLL: u32 = kernel::ioctl::_IOR::<SignPoll>(0xb1, 0x0d);

impl SepData {
    pub(crate) fn key_ioctl(&self, cmd: u32, arg: usize) -> Result<bio::Handled> {
        use kernel::uaccess::{UserPtr, UserSlice};
        let user = UserPtr::from_addr(arg);
        if cmd == IOC_SIGN_POLL {
            return bio::poll_sign(&mut self.bio_session.lock(), user);
        }
        if !self.biometric_keys_supported() {
            return Err(Error::from_errno(-(kernel::bindings::EOPNOTSUPP as i32)));
        }
        if !self.xart_writable() {
            return Err(EROFS);
        }
        if self.suspending.load(Relaxed) || !self.bio_session.lock().available() {
            return Err(EBUSY);
        }
        match cmd {
            IOC_CREATE => {
                let request: Create = UserSlice::new(user, core::mem::size_of::<Create>())
                    .reader().read()?;
                if request.flags != 0 || request.reserved != [0; 7]
                    || request.blob_len as usize != MAX_BLOB
                {
                    return Err(EINVAL);
                }
                bio::reserve_worker(&mut self.bio_session.lock())?;
                let result = if self.capture_begin() {
                    let result = self.create_biometric_key();
                    self.capture_end();
                    result
                } else {
                    Err(EBUSY)
                };
                bio::worker_done(&mut self.bio_session.lock());
                let (blob, public) = result?;
                UserSlice::new(UserPtr::from_addr(request.blob_ptr as usize), MAX_BLOB)
                    .writer().write_slice(&blob)?;
                let out = Create {
                    flags: 0,
                    blob_len: blob.len() as u32,
                    blob_ptr: request.blob_ptr,
                    public,
                    reserved: [0; 7],
                };
                UserSlice::new(user, core::mem::size_of::<Create>()).writer().write(&out)?;
                Ok(bio::Handled {
                    ret: 0, wake: false, start_enrol: false, start_verify: false,
                    delete_identity: None, delete_identities: KVec::new(),
                })
            }
            IOC_SIGN_START => {
                let request: SignStart = UserSlice::new(user, core::mem::size_of::<SignStart>())
                    .reader().read()?;
                if request.flags != 0 || request.reserved != 0 || request.blob_len == 0
                    || request.blob_len as usize > MAX_BLOB || !(1..=120_000).contains(&request.timeout_ms)
                {
                    return Err(EINVAL);
                }
                let mut blob = KVec::new();
                blob.resize(request.blob_len as usize, 0, GFP_KERNEL)?;
                UserSlice::new(UserPtr::from_addr(request.blob_ptr as usize), blob.len())
                    .reader().read_slice(&mut blob)?;
                if Self::refkey_pub(&blob).is_none() { return Err(EINVAL); }
                let deadline_ns = crate::shim::monotonic_ns()
                    .saturating_add(u64::from(request.timeout_ms) * 1_000_000);
                bio::start_sign(&mut self.bio_session.lock(), SignRequest {
                    blob,
                    digest: request.digest,
                    deadline_ns,
                })
            }
            _ => Err(ENOTTY),
        }
    }

    pub(crate) fn run_key_sign(&self, request: SignRequest) {
        let result = (|| {
            if !bio::sign_is_live(&self.bio_session.lock()) { return Err(ECANCELED); }
            let credential = self.key_credential()?;
            let result = match self.verify_capture(Some(credential.handle())) {
                bio::VerifyOutcome::Matched(_) => {
                    if !bio::sign_is_live(&self.bio_session.lock()) { Err(ECANCELED) }
                    else { self.sign_biometric_key(&request.blob, &request.digest, &credential) }
                }
                bio::VerifyOutcome::NoMatch => Err(EACCES),
                bio::VerifyOutcome::Failed(crate::ENROL_STATUS_TIMEOUT) => Err(ETIMEDOUT),
                bio::VerifyOutcome::Failed(_) => Err(EIO),
            };
            drop(credential);
            if self.key_api_failed.load(Relaxed) { return Err(EIO); }
            result
        })();
        if bio::finish_sign(&mut self.bio_session.lock(), result) { self.bio_wake(); }
    }
}
