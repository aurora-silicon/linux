// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

use crate::{image, proto, SepData, SksRequest, PHASE_READY};
use kernel::prelude::*;
use kernel::sync::atomic::Relaxed;

const WRAPPED_KEY_LEN: usize = 40;
const OPAQUE_KEY_LEN: usize = 64;
const IV_KEY_LEN: usize = 16;
const SYSTEM_CLIENT: u64 = 1;
const NO_KEYBAG_HANDLE: i32 = -1;
const WRAPPED_KEY_FLAG: i32 = 1 << 1;
const VOLUME_KEY_FLAG: i32 = 1;
const SECRET_MAX_LEN: usize = 1024;
const RECORD_MAX_LEN: usize = 528;
const FILE_KEY_MAX_LEN: usize = 168;
const EPHEMERAL_KEY_MAX_LEN: usize = 64;
const FV_UNWRAP_VERSION: u32 = 0;
const FV_UNWRAP_OPTIONS: u32 = 2;
const FV_NEW_KEK_OPTIONS: u32 = 0;
const FV_REARM_RECORD_VERSION: u32 = 1;
const FV_REARM_RECORD_HEADER: usize = 32;
const FV_REARM_RECORD_MAGIC: [u8; 16] = *b"AURORA-TID-FVR01";
const FV_REARM_BINDING: [u8; 16] = *b"AuroraTouchIDFV1";
const FV_REARM_STORE_KEY: crate::store::Key = crate::store::Key {
    // F5 is already accepted by rollback kernels. The UUID namespaces this
    // record away from the F5 root record used by the SBIO lockout snapshot.
    kind: 0xf5,
    uuid: *b"AURORA-TID-FV001",
};
const PFK_UNWRAP_VERSION: u32 = 2;
const PFK_SYSTEM_VOLUME_HANDLE: i32 = -5;
const PFK_UNWRAP_OPTIONS: u32 = 0x102;
const PFK_KEY_FLAG: i32 = 1 << 1;
const PFK_NEW_VERSION: u32 = 2;
const PFK_NEW_OPTIONS: u32 = 0x102;
const PFK_NEW_KEY_FLAG: i32 = 1 << 1;
const PFK_FS_CONTEXT_LEN: usize = 28;
const FV_LOAD_CLASS_KEYS: u32 = 0x12;
const FV_UNLOAD_CLASS_KEYS: u32 = 0x13;
const FV_SYSTEM_VOLUME_OPTION: u64 = 4;
const FV_MAX_VOLUME_MAPS: usize = 32;
const FV_STATE_UUID_KEY: &[u8] = b"kid";

pub(crate) struct VolumeMap {
    apfs_uuid: [u8; 16],
    bag_uuid: [u8; 16],
    refs: u32,
}

#[repr(C)]
struct KernelKey {
    opaque: [u8; OPAQUE_KEY_LEN],
    iv: [u8; IV_KEY_LEN],
}

#[repr(C)]
struct KernelNewFileKey {
    key: KernelKey,
    wrapped_ekwk: [u8; FILE_KEY_MAX_LEN],
    wrapped_ek: [u8; FILE_KEY_MAX_LEN],
    wrapped_ekwk_len: usize,
    wrapped_ek_len: usize,
}

#[repr(C)]
struct KernelOps {
    unwrap_media_key:
        unsafe extern "C" fn(*mut c_void, *const u8, usize, u32, *mut KernelKey) -> c_int,
    unwrap_volume_key: unsafe extern "C" fn(
        *mut c_void,
        *const u8,
        usize,
        *const u8,
        usize,
        *const u8,
        usize,
        *mut KernelKey,
    ) -> c_int,
    load_class_keys: unsafe extern "C" fn(
        *mut c_void,
        *const u8,
        *const u8,
        usize,
        *const u8,
        usize,
        *const u8,
        usize,
    ) -> c_int,
    unload_class_keys: unsafe extern "C" fn(*mut c_void, *const u8, *const u8, usize) -> c_int,
    unwrap_file_key: unsafe extern "C" fn(
        *mut c_void,
        *const u8,
        u32,
        *const u8,
        usize,
        *const u8,
        usize,
        *mut KernelKey,
    ) -> c_int,
    new_file_key:
        unsafe extern "C" fn(*mut c_void, *const u8, u32, u64, u16, *mut KernelNewFileKey) -> c_int,
}

extern "C" {
    fn sep_fv_register_v2(context: *mut c_void, ops: *const KernelOps) -> c_int;
    fn sep_fv_unregister_v2(context: *mut c_void);
}

static KERNEL_OPS: KernelOps = KernelOps {
    unwrap_media_key: kernel_unwrap_media_key,
    unwrap_volume_key: kernel_unwrap_volume_key,
    load_class_keys: kernel_load_class_keys,
    unload_class_keys: kernel_unload_class_keys,
    unwrap_file_key: kernel_unwrap_file_key,
    new_file_key: kernel_new_file_key,
};

const PFK_VOLUME_PARAMS_PREFIX: [u8; 12] = [
    0x31, 0x1a, 0x30, 0x18, 0x0c, 0x04, b'v', b'u', b'i', b'd', 0x04, 0x10,
];
const PFK_VOLUME_PARAMS_LEN: usize = PFK_VOLUME_PARAMS_PREFIX.len() + 16;

const FV_PARAMS_DER: [u8; 35] = [
    0x31, 0x21, 0x30, 0x0a, 0x0c, 0x02, b'k', b'c', 0x04, 0x04, 0, 0, 0, 0, 0x30, 0x13, 0x0c, 0x07,
    b'o', b'p', b't', b'i', b'o', b'n', b's', 0x04, 0x08, 0, 0, 0, 0, 0, 0, 0, 0,
];

struct MediaKey {
    opaque: [u8; OPAQUE_KEY_LEN],
    iv_key: [u8; IV_KEY_LEN],
}

struct VolumeKey {
    opaque: [u8; OPAQUE_KEY_LEN],
}

struct RearmRecords<'a> {
    kek: &'a [u8],
    vek: &'a [u8],
}

struct FileKey {
    opaque: [u8; OPAQUE_KEY_LEN],
    iv_key: [u8; IV_KEY_LEN],
}

struct NewFileKey {
    key: FileKey,
    wrapped_ekwk: [u8; FILE_KEY_MAX_LEN],
    wrapped_ek: [u8; FILE_KEY_MAX_LEN],
    wrapped_ekwk_len: usize,
    wrapped_ek_len: usize,
}

impl Drop for NewFileKey {
    fn drop(&mut self) {
        image::wipe(&mut self.wrapped_ekwk);
        image::wipe(&mut self.wrapped_ek);
    }
}

impl Drop for VolumeKey {
    fn drop(&mut self) {
        image::wipe(&mut self.opaque);
    }
}

fn fv_new_kek_body(secret: &[u8]) -> Result<image::Body> {
    let mut body = image::Body::new();
    body.put_u32(FV_UNWRAP_VERSION)?;
    body.put_u64(SYSTEM_CLIENT)?;
    body.put_blob(&FV_PARAMS_DER)?;
    body.put_u32(FV_NEW_KEK_OPTIONS)?;
    body.put_blob(secret)?;
    body.put_blob(&FV_REARM_BINDING)?;
    Ok(body)
}

fn fv_new_vek_body(secret: &[u8], kek: &[u8]) -> Result<image::Body> {
    let mut body = image::Body::new();
    body.put_u32(FV_UNWRAP_VERSION)?;
    body.put_u64(SYSTEM_CLIENT)?;
    body.put_blob(&FV_PARAMS_DER)?;
    // FV_NEW_VEK takes the same credential used for NEW_KEK first, and that
    // operation's returned KEK record second. The final fixed-size blob is
    // the same FV domain identifier used by NEW_KEK.
    body.put_blob(secret)?;
    body.put_blob(kek)?;
    body.put_blob(&FV_REARM_BINDING)?;
    Ok(body)
}

fn fv_unwrap_vek_body(secret: &[u8], kek: &[u8], vek: &[u8]) -> Result<image::Body> {
    let mut body = image::Body::new();
    body.put_u32(FV_UNWRAP_VERSION)?;
    body.put_u64(SYSTEM_CLIENT)?;
    body.put_blob(&FV_PARAMS_DER)?;
    body.put_u32(FV_UNWRAP_OPTIONS)?;
    body.put_blob(secret)?;
    body.put_blob(kek)?;
    body.put_blob(vek)?;
    body.put_blob(&[])?;
    Ok(body)
}

fn encode_rearm_records(kek: &[u8], vek: &[u8]) -> Result<KVec<u8>> {
    if kek.is_empty() || kek.len() > RECORD_MAX_LEN || vek.is_empty() || vek.len() > RECORD_MAX_LEN
    {
        return Err(EINVAL);
    }
    let total = FV_REARM_RECORD_HEADER
        .checked_add(kek.len())
        .and_then(|n| n.checked_add(vek.len()))
        .ok_or(EOVERFLOW)?;
    let mut raw = KVec::with_capacity(total, GFP_KERNEL)?;
    raw.extend_from_slice(&FV_REARM_RECORD_MAGIC, GFP_KERNEL)?;
    raw.extend_from_slice(&FV_REARM_RECORD_VERSION.to_le_bytes(), GFP_KERNEL)?;
    raw.extend_from_slice(&(kek.len() as u32).to_le_bytes(), GFP_KERNEL)?;
    raw.extend_from_slice(&(vek.len() as u32).to_le_bytes(), GFP_KERNEL)?;
    raw.extend_from_slice(&0u32.to_le_bytes(), GFP_KERNEL)?;
    raw.extend_from_slice(kek, GFP_KERNEL)?;
    raw.extend_from_slice(vek, GFP_KERNEL)?;
    Ok(raw)
}

fn decode_rearm_records(raw: &[u8]) -> Result<RearmRecords<'_>> {
    if raw.len() < FV_REARM_RECORD_HEADER || raw[..16] != FV_REARM_RECORD_MAGIC {
        return Err(EINVAL);
    }
    let version = u32::from_le_bytes(raw[16..20].try_into().map_err(|_| EINVAL)?);
    let kek_len = u32::from_le_bytes(raw[20..24].try_into().map_err(|_| EINVAL)?) as usize;
    let vek_len = u32::from_le_bytes(raw[24..28].try_into().map_err(|_| EINVAL)?) as usize;
    let reserved = u32::from_le_bytes(raw[28..32].try_into().map_err(|_| EINVAL)?);
    let vek_at = FV_REARM_RECORD_HEADER
        .checked_add(kek_len)
        .ok_or(EOVERFLOW)?;
    let end = vek_at.checked_add(vek_len).ok_or(EOVERFLOW)?;
    if version != FV_REARM_RECORD_VERSION
        || reserved != 0
        || kek_len == 0
        || kek_len > RECORD_MAX_LEN
        || vek_len == 0
        || vek_len > RECORD_MAX_LEN
        || end != raw.len()
    {
        return Err(EINVAL);
    }
    Ok(RearmRecords {
        kek: &raw[FV_REARM_RECORD_HEADER..vek_at],
        vek: &raw[vek_at..end],
    })
}

/// The non-secret layout of an `FV_UNWRAP_VEK` reply body: version, one
/// opaque-key blob, then flags. Only lengths and flags are kept, so the shape
/// can be logged; the key bytes stay in the reply buffer, which is wiped.
///
/// The Neo answers the option-2 unwrap of a Linux-owned pair with the same
/// 64-byte opaque key as a volume unwrap (a 0x98-byte reply image). The
/// 0x2c-byte macOS body seen in the capture held a shorter key, so the length
/// is bounded rather than fixed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct UnwrapReplyShape {
    body_len: usize,
    version: Option<i32>,
    key_len: Option<usize>,
    flags: Option<i32>,
    trailing: usize,
}

impl UnwrapReplyShape {
    fn of(body: &[u8]) -> UnwrapReplyShape {
        let mut fields = proto::FieldCursor::new(body);
        let version = fields.i32();
        let key_len = version.and_then(|_| fields.blob()).map(<[u8]>::len);
        let flags = key_len.and_then(|_| fields.i32());
        let consumed = match (version, key_len, flags) {
            (Some(_), Some(len), Some(_)) => 12 + len + (len.wrapping_neg() % 4),
            _ => body.len(),
        };
        UnwrapReplyShape {
            body_len: body.len(),
            version,
            key_len,
            flags,
            trailing: body.len().saturating_sub(consumed),
        }
    }

    /// Reply version 0, a complete non-empty key no longer than a volume key,
    /// flags bit 0, and nothing after the flags.
    fn usable(&self) -> bool {
        self.version == Some(FV_UNWRAP_VERSION as i32)
            && matches!(self.key_len, Some(1..=OPAQUE_KEY_LEN))
            && self.flags.is_some_and(|flags| flags & VOLUME_KEY_FLAG != 0)
            && self.trailing == 0
    }
}

#[cfg(CONFIG_APPLE_SEP_KUNIT_TEST)]
#[::kernel::macros::kunit_tests(apple_sep_fv_rearm)]
mod rearm_tests {
    use super::*;

    fn le32(bytes: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
    }

    fn common(body: &image::Body) -> &[u8] {
        let bytes = body.as_slice();
        assert_eq!(le32(bytes, 0), FV_UNWRAP_VERSION);
        assert_eq!(&bytes[4..12], &SYSTEM_CLIENT.to_le_bytes());
        assert_eq!(le32(bytes, 12) as usize, FV_PARAMS_DER.len());
        assert_eq!(&bytes[16..51], &FV_PARAMS_DER);
        assert_eq!(bytes[51], 0);
        bytes
    }

    #[test]
    fn new_kek_layout_binds_secret() {
        let body = fv_new_kek_body(b"secret").unwrap();
        let bytes = common(&body);
        assert_eq!(le32(bytes, 52), FV_NEW_KEK_OPTIONS);
        assert_eq!(le32(bytes, 56), 6);
        assert_eq!(&bytes[60..66], b"secret");
        assert_eq!(&bytes[66..68], &[0, 0]);
        assert_eq!(le32(bytes, 68), FV_REARM_BINDING.len() as u32);
        assert_eq!(&bytes[72..88], &FV_REARM_BINDING);
        assert_eq!(bytes.len(), 88);
    }

    #[test]
    fn new_vek_layout_uses_secret_then_kek() {
        let body = fv_new_vek_body(b"secret", b"kk").unwrap();
        let bytes = common(&body);
        assert_eq!(le32(bytes, 52), 6);
        assert_eq!(&bytes[56..62], b"secret");
        assert_eq!(&bytes[62..64], &[0, 0]);
        assert_eq!(le32(bytes, 64), 2);
        assert_eq!(&bytes[68..70], b"kk");
        assert_eq!(&bytes[70..72], &[0, 0]);
        assert_eq!(le32(bytes, 72), FV_REARM_BINDING.len() as u32);
        assert_eq!(&bytes[76..92], &FV_REARM_BINDING);
        assert_eq!(bytes.len(), 92);
    }

    #[test]
    fn unwrap_layout_uses_option_two_and_empty_auxiliary_blob() {
        let body = fv_unwrap_vek_body(b"secret", b"kk", b"vek").unwrap();
        let bytes = common(&body);
        assert_eq!(le32(bytes, 52), FV_UNWRAP_OPTIONS);
        assert_eq!(le32(bytes, 56), 6);
        assert_eq!(&bytes[60..66], b"secret");
        assert_eq!(&bytes[66..68], &[0, 0]);
        assert_eq!(le32(bytes, 68), 2);
        assert_eq!(&bytes[72..74], b"kk");
        assert_eq!(le32(bytes, 76), 3);
        assert_eq!(&bytes[80..83], b"vek");
        assert_eq!(le32(bytes, 84), 0);
        assert_eq!(bytes.len(), 88);
    }

    fn unwrap_reply(version: i32, key_len: usize, flags: i32, extra: usize) -> KVec<u8> {
        let mut body = image::Body::new();
        body.put_i32(version).unwrap();
        let mut key = KVec::new();
        key.resize(key_len, 0xa5, GFP_KERNEL).unwrap();
        body.put_blob(&key).unwrap();
        body.put_i32(flags).unwrap();
        let mut bytes = KVec::new();
        bytes.extend_from_slice(body.as_slice(), GFP_KERNEL).unwrap();
        bytes.resize(bytes.len() + extra, 0, GFP_KERNEL).unwrap();
        bytes
    }

    #[test]
    fn unwrap_reply_accepts_the_neo_64_byte_key() {
        // The Neo's 0x98-byte reply image less its 0x4c-byte prefix.
        let body = unwrap_reply(0, 64, 1, 0);
        assert_eq!(body.len(), 0x98 - 0x4c);
        let shape = UnwrapReplyShape::of(&body);
        assert_eq!(shape.key_len, Some(64));
        assert_eq!(shape.flags, Some(1));
        assert_eq!(shape.trailing, 0);
        assert!(shape.usable());
    }

    #[test]
    fn unwrap_reply_accepts_the_macos_32_byte_key() {
        assert!(UnwrapReplyShape::of(&unwrap_reply(0, 32, 1, 0)).usable());
    }

    #[test]
    fn unwrap_reply_refuses_bad_shapes() {
        assert!(!UnwrapReplyShape::of(&unwrap_reply(1, 64, 1, 0)).usable());
        assert!(!UnwrapReplyShape::of(&unwrap_reply(0, 0, 1, 0)).usable());
        assert!(!UnwrapReplyShape::of(&unwrap_reply(0, 68, 1, 0)).usable());
        assert!(!UnwrapReplyShape::of(&unwrap_reply(0, 64, 0, 0)).usable());
        let trailing = UnwrapReplyShape::of(&unwrap_reply(0, 64, 1, 4));
        assert_eq!(trailing.trailing, 4);
        assert!(!trailing.usable());
        let short = unwrap_reply(0, 64, 1, 0);
        let cut = UnwrapReplyShape::of(&short[..40]);
        assert_eq!(cut.key_len, None);
        assert!(!cut.usable());
        assert!(!UnwrapReplyShape::of(&[]).usable());
    }

    #[test]
    fn persistent_pair_round_trips_and_rejects_trailing_bytes() {
        let raw = encode_rearm_records(b"kek-record", b"vek-record").unwrap();
        let decoded = decode_rearm_records(&raw).unwrap();
        assert_eq!(decoded.kek, b"kek-record");
        assert_eq!(decoded.vek, b"vek-record");

        let mut damaged = raw;
        damaged.push(0, GFP_KERNEL).unwrap();
        assert!(decode_rearm_records(&damaged).is_err());
    }
}

impl Drop for MediaKey {
    fn drop(&mut self) {
        image::wipe(&mut self.opaque);
        image::wipe(&mut self.iv_key);
    }
}

impl Drop for FileKey {
    fn drop(&mut self) {
        image::wipe(&mut self.opaque);
        image::wipe(&mut self.iv_key);
    }
}

impl SepData {
    fn pfk_params(volume_uuid: &[u8; 16]) -> [u8; PFK_VOLUME_PARAMS_LEN] {
        let mut params = [0u8; PFK_VOLUME_PARAMS_LEN];
        params[..PFK_VOLUME_PARAMS_PREFIX.len()].copy_from_slice(&PFK_VOLUME_PARAMS_PREFIX);
        params[PFK_VOLUME_PARAMS_PREFIX.len()..].copy_from_slice(volume_uuid);
        params
    }

    fn pfk_class(protection_class: u32) -> Result<u32> {
        match protection_class & 0x1f {
            1..=4 => Ok(protection_class),
            6 => Ok((protection_class & !0x1f) | 13),
            7 => Ok((protection_class & !0x1f) | 17),
            _ => Err(EINVAL),
        }
    }

    fn fv_options(&self, volume_uuid: &[u8; 16]) -> u64 {
        if self.xarm.lock().os_uuid == Some(*volume_uuid) {
            FV_SYSTEM_VOLUME_OPTION
        } else {
            0
        }
    }

    fn fv_params(options: u64) -> [u8; FV_PARAMS_DER.len()] {
        let mut params = FV_PARAMS_DER;
        let options_at = params.len() - 8;
        params[options_at..].copy_from_slice(&options.to_le_bytes());
        params
    }

    fn fv_ready(&self) -> Result<()> {
        if self.phase.load(Relaxed) != PHASE_READY {
            return Err(EAGAIN);
        }
        if !self.sks_ready() {
            return Err(ENODEV);
        }
        Ok(())
    }

    fn resolve_fv_uuid(&self, apfs_uuid: &[u8; 16]) -> [u8; 16] {
        self.fv_volumes
            .lock()
            .iter()
            .find(|entry| entry.apfs_uuid == *apfs_uuid)
            .map_or(*apfs_uuid, |entry| entry.bag_uuid)
    }

    fn retain_fv_volume(&self, apfs_uuid: &[u8; 16], bag_uuid: &[u8; 16]) -> Result<bool> {
        let mut volumes = self.fv_volumes.lock();
        if let Some(entry) = volumes
            .iter_mut()
            .find(|entry| entry.apfs_uuid == *apfs_uuid)
        {
            if entry.bag_uuid != *bag_uuid {
                return Err(EINVAL);
            }
            entry.refs = entry.refs.checked_add(1).ok_or(EOVERFLOW)?;
            return Ok(false);
        }
        if volumes.len() >= FV_MAX_VOLUME_MAPS {
            return Err(ENOSPC);
        }
        volumes.push(
            VolumeMap {
                apfs_uuid: *apfs_uuid,
                bag_uuid: *bag_uuid,
                refs: 1,
            },
            GFP_KERNEL,
        )?;
        Ok(true)
    }

    fn fv_volume_is_last(&self, apfs_uuid: &[u8; 16], bag_uuid: &[u8; 16]) -> Result<bool> {
        let volumes = self.fv_volumes.lock();
        let entry = volumes
            .iter()
            .find(|entry| entry.apfs_uuid == *apfs_uuid)
            .ok_or(ENOENT)?;
        if entry.bag_uuid != *bag_uuid {
            return Err(EINVAL);
        }
        Ok(entry.refs == 1)
    }

    fn release_fv_volume(&self, apfs_uuid: &[u8; 16], bag_uuid: &[u8; 16]) -> Result<()> {
        let mut volumes = self.fv_volumes.lock();
        let index = volumes
            .iter()
            .position(|entry| entry.apfs_uuid == *apfs_uuid)
            .ok_or(ENOENT)?;
        if volumes[index].bag_uuid != *bag_uuid {
            return Err(EINVAL);
        }
        if volumes[index].refs > 1 {
            volumes[index].refs -= 1;
        } else {
            volumes.swap_remove(index);
        }
        Ok(())
    }

    fn sks_req_fv_blob_state(
        &self,
        volume_uuid: &[u8; 16],
        volume_key: &[u8],
    ) -> Result<SksRequest> {
        let params = Self::fv_params(self.fv_options(volume_uuid));
        let mut body = image::Body::new();
        body.put_u32(0)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_blob(&params)?;
        body.put_blob(volume_key)?;

        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_GET_BLOB_STATE_NAME,
            msg: crate::sks::encode_sks_get_blob_state(self.sks_next_seq(), len),
            img,
        })
    }

    fn fv_blob_uuid(&self, volume_uuid: &[u8; 16], volume_key: &[u8]) -> Result<[u8; 16]> {
        let out = self
            .sks_send(self.sks_req_fv_blob_state(volume_uuid, volume_key))
            .ok_or(EIO)?;
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(
                self.dev,
                "fv: GET_BLOB_STATE failed with status {}\n",
                status
            );
            return Err(EACCES);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_GET_BLOB_STATE_NAME, &out)
            .ok_or(EMSGSIZE)?;
        let mut fields = proto::FieldCursor::new(body);
        let version = fields.i32();
        if version != Some(0) {
            dev_err_ratelimited!(
                self.dev,
                "fv: GET_BLOB_STATE response has version {:?}, {} body bytes\n",
                version,
                body.len()
            );
            return Err(EMSGSIZE);
        }
        let state = match fields.blob() {
            Some(state) => state,
            None => {
                dev_err_ratelimited!(
                    self.dev,
                    "fv: GET_BLOB_STATE has no complete state blob in {} body bytes\n",
                    body.len()
                );
                return Err(EMSGSIZE);
            }
        };
        let uuid_tlv = match crate::der::refkey_find(state, FV_STATE_UUID_KEY) {
            Some(uuid) => uuid,
            None => {
                let head0 = state
                    .get(..8)
                    .and_then(|bytes| bytes.try_into().ok())
                    .map(u64::from_be_bytes)
                    .unwrap_or(0);
                let head1 = state
                    .get(8..16)
                    .and_then(|bytes| bytes.try_into().ok())
                    .map(u64::from_be_bytes)
                    .unwrap_or(0);
                dev_err_ratelimited!(
                    self.dev,
                    "fv: GET_BLOB_STATE returned {} state bytes without a UUID (head {:016x}{:016x})\n",
                    state.len(),
                    head0,
                    head1
                );
                return Err(EMSGSIZE);
            }
        };
        let raw_uuid = crate::der::octet_string_body(uuid_tlv).ok_or(EMSGSIZE)?;
        if raw_uuid.len() != 16 {
            dev_err_ratelimited!(
                self.dev,
                "fv: GET_BLOB_STATE returned a {}-byte UUID\n",
                raw_uuid.len()
            );
            return Err(EMSGSIZE);
        }
        let mut bag_uuid = [0u8; 16];
        bag_uuid.copy_from_slice(raw_uuid);
        Ok(bag_uuid)
    }

    pub(crate) fn register_fv_kernel(&self) -> Result<()> {
        let context = core::ptr::from_ref(self).cast_mut().cast::<c_void>();
        // SAFETY: `KERNEL_OPS` is static and `remove()` unregisters this
        // pointer before the driver's `Arc<SepData>` can be dropped.
        kernel::error::to_result(unsafe { sep_fv_register_v2(context, &KERNEL_OPS) })
    }

    pub(crate) fn unregister_fv_kernel(&self) {
        let context = core::ptr::from_ref(self).cast_mut().cast::<c_void>();
        // SAFETY: the pointer is the one passed to `sep_fv_register_v2`.
        unsafe { sep_fv_unregister_v2(context) };
    }

    fn sks_req_unwrap_media_key_from_class(
        &self,
        wrapped: &[u8; WRAPPED_KEY_LEN],
        protection_class: u32,
    ) -> Result<SksRequest> {
        let mut body = image::Body::new();
        body.put_u32(1)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_blob(wrapped)?;
        body.put_i32(NO_KEYBAG_HANDLE)?;
        body.put_u32(protection_class)?;

        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        let msg =
            crate::sks::encode_sks_unwrap_media_key(self.sks_next_seq(), len).ok_or(EINVAL)?;
        Ok(SksRequest {
            name: crate::sks::SKS_UNWRAP_MEDIA_KEY_NAME,
            msg,
            img,
        })
    }

    fn decode_media_key(&self, out: crate::SksOutcome) -> Result<MediaKey> {
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(
                self.dev,
                "fv: UNWRAP_MEDIA_KEY failed with status {}\n",
                status
            );
            return Err(EACCES);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_UNWRAP_MEDIA_KEY_NAME, &out)
            .ok_or(EMSGSIZE)?;
        let mut fields = proto::FieldCursor::new(body);
        if fields.i32() != Some(1) {
            return Err(EMSGSIZE);
        }
        let key = fields.blob().ok_or(EMSGSIZE)?;
        let iv_key = fields.blob().ok_or(EMSGSIZE)?;
        let flags = fields.i32().ok_or(EMSGSIZE)?;

        if key.len() != OPAQUE_KEY_LEN
            || iv_key.len() != IV_KEY_LEN
            || flags & WRAPPED_KEY_FLAG == 0
        {
            return Err(EMSGSIZE);
        }

        let mut opaque = [0; OPAQUE_KEY_LEN];
        opaque.copy_from_slice(key);
        let mut iv = [0; IV_KEY_LEN];
        iv.copy_from_slice(iv_key);
        Ok(MediaKey { opaque, iv_key: iv })
    }

    fn unwrap_media_key_from_class(
        &self,
        wrapped: &[u8; WRAPPED_KEY_LEN],
        protection_class: u32,
    ) -> Result<MediaKey> {
        self.fv_ready()?;
        let out = self
            .sks_send(self.sks_req_unwrap_media_key_from_class(wrapped, protection_class))
            .ok_or(EIO)?;
        self.decode_media_key(out)
    }

    fn sks_req_unwrap_vek(&self, secret: &[u8], kek: &[u8], vek: &[u8]) -> Result<SksRequest> {
        let body = fv_unwrap_vek_body(secret, kek, vek)?;
        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        let msg = crate::sks::encode_sks_unwrap_vek(self.sks_next_seq(), len).ok_or(EINVAL)?;
        Ok(SksRequest {
            name: crate::sks::SKS_UNWRAP_VEK_NAME,
            msg,
            img,
        })
    }

    fn unwrap_vek(&self, secret: &[u8], kek: &[u8], vek: &[u8]) -> Result<VolumeKey> {
        self.fv_ready()?;
        let out = self
            .sks_send(self.sks_req_unwrap_vek(secret, kek, vek))
            .ok_or(EIO)?;
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(self.dev, "fv: UNWRAP_VEK failed with status {}\n", status);
            return Err(EACCES);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_UNWRAP_VEK_NAME, &out)
            .ok_or(EMSGSIZE)?;
        let mut fields = proto::FieldCursor::new(body);
        if fields.i32() != Some(FV_UNWRAP_VERSION as i32) {
            return Err(EMSGSIZE);
        }
        let key = fields.blob().ok_or(EMSGSIZE)?;
        let flags = fields.i32().ok_or(EMSGSIZE)?;
        if key.len() != OPAQUE_KEY_LEN || flags & VOLUME_KEY_FLAG == 0 {
            return Err(EMSGSIZE);
        }

        let mut opaque = [0; OPAQUE_KEY_LEN];
        opaque.copy_from_slice(key);
        Ok(VolumeKey { opaque })
    }

    fn sks_req_new_rearm_kek(&self, secret: &[u8]) -> Result<SksRequest> {
        let body = fv_new_kek_body(secret)?;
        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_NEW_KEK_NAME,
            msg: crate::sks::encode_sks_new_kek(self.sks_next_seq(), len),
            img,
        })
    }

    fn sks_req_new_rearm_vek(&self, secret: &[u8], kek: &[u8]) -> Result<SksRequest> {
        let body = fv_new_vek_body(secret, kek)?;
        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_NEW_VEK_NAME,
            msg: crate::sks::encode_sks_new_vek(self.sks_next_seq(), len),
            img,
        })
    }

    fn fv_record_from_reply(
        &self,
        name: &'static CStr,
        out: crate::SksOutcome,
    ) -> Result<KVec<u8>> {
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(self.dev, "fv: {} failed with status {}\n", name, status);
            return Err(EACCES);
        }
        let body = self.sks_report_response(name, &out).unwrap_or(&[]);
        let mut fields = proto::FieldCursor::new(body);
        let version = fields.i32();
        let record = fields.blob().filter(|_| version == Some(FV_UNWRAP_VERSION as i32));
        let Some(record) = record.filter(|r| !r.is_empty() && r.len() <= RECORD_MAX_LEN) else {
            dev_err_ratelimited!(
                self.dev,
                "fv: {} answered status 0 with an unusable reply: image {} bytes, body {} bytes, version {:?}\n",
                name,
                out.response.len(),
                body.len(),
                version
            );
            return Err(EMSGSIZE);
        };
        let mut owned = KVec::with_capacity(record.len(), GFP_KERNEL)?;
        owned.extend_from_slice(record, GFP_KERNEL)?;
        Ok(owned)
    }

    /// One re-arm SKS exchange. A missing reply is logged here: the exchange
    /// itself is silent when the key store was already given up on.
    fn rearm_exchange(&self, name: &'static CStr, req: Result<SksRequest>) -> Result<crate::SksOutcome> {
        let req = req.inspect_err(|e| {
            dev_err_ratelimited!(self.dev, "Touch ID: re-arm: could not build {}: {:?}\n", name, e);
        })?;
        self.sks_send(Ok(req)).ok_or_else(|| {
            dev_err_ratelimited!(self.dev, "Touch ID: re-arm: {} got no reply\n", name);
            EIO
        })
    }

    fn new_rearm_kek(&self, secret: &[u8]) -> Result<KVec<u8>> {
        let out = self.rearm_exchange(
            crate::sks::SKS_NEW_KEK_NAME,
            self.sks_req_new_rearm_kek(secret),
        )?;
        self.fv_record_from_reply(crate::sks::SKS_NEW_KEK_NAME, out)
    }

    fn new_rearm_vek(&self, secret: &[u8], kek: &[u8]) -> Result<KVec<u8>> {
        let out = self.rearm_exchange(
            crate::sks::SKS_NEW_VEK_NAME,
            self.sks_req_new_rearm_vek(secret, kek),
        )?;
        self.fv_record_from_reply(crate::sks::SKS_NEW_VEK_NAME, out)
    }

    /// Sends the option-2 `FV_UNWRAP_VEK` (0x41). Success is SKS status 0 and
    /// a well-formed reply; the opaque key is never copied out of the reply,
    /// whose buffer is wiped when `out` drops. EACCES means the enclave
    /// refused the credential or records; other errors are local.
    fn unwrap_rearm_vek(&self, secret: &[u8], kek: &[u8], vek: &[u8]) -> Result<()> {
        let out = self.rearm_exchange(
            crate::sks::SKS_UNWRAP_VEK_NAME,
            self.sks_req_unwrap_vek(secret, kek, vek),
        )?;
        let status: i32 = out.reply.status.into();
        if status != 0 {
            dev_err_ratelimited!(
                self.dev,
                "Touch ID: re-arm FV unwrap (0x41, option 2) failed with SKS status {}\n",
                status
            );
            return Err(EACCES);
        }
        let Some(body) = self.sks_report_response(crate::sks::SKS_UNWRAP_VEK_NAME, &out) else {
            dev_err_ratelimited!(
                self.dev,
                "Touch ID: re-arm FV unwrap answered SKS status 0 with a malformed {}-byte reply image\n",
                out.response.len()
            );
            return Err(EMSGSIZE);
        };
        let shape = UnwrapReplyShape::of(body);
        if !shape.usable() {
            dev_err_ratelimited!(
                self.dev,
                "Touch ID: re-arm FV unwrap answered SKS status 0 with an unusable reply: body {} bytes, version {:?}, key {:?} bytes, flags {:?}, {} trailing bytes\n",
                shape.body_len,
                shape.version,
                shape.key_len,
                shape.flags,
                shape.trailing
            );
            return Err(EMSGSIZE);
        }
        dev_info_ratelimited!(
            self.dev,
            "Touch ID: re-arm FV unwrap succeeded: SKS status 0, {}-byte key discarded, flags {:#x}\n",
            shape.key_len.unwrap_or(0),
            shape.flags.unwrap_or(0)
        );
        Ok(())
    }

    /// Creates a Linux-owned KEK/VEK pair bound to `secret`, proves it with an
    /// unwrap, and only then commits it (copy-on-write) to the host store.
    fn provision_rearm_records(&self, secret: &[u8]) -> Result<()> {
        let maximum = FV_REARM_RECORD_HEADER + 2 * RECORD_MAX_LEN;
        if !self
            .with_host_store(|store| store.can_write(&FV_REARM_STORE_KEY, maximum))
            .unwrap_or(false)
        {
            dev_err_ratelimited!(self.dev, "Touch ID: re-arm: the host-state store has no room for the FV record pair\n");
            return Err(ENOSPC);
        }

        let kek = self.new_rearm_kek(secret)?;
        let vek = self.new_rearm_vek(secret, &kek)?;
        self.unwrap_rearm_vek(secret, &kek, &vek)?;
        let raw = encode_rearm_records(&kek, &vek)?;
        self.with_host_store(|store| store.write(&FV_REARM_STORE_KEY, &raw))
            .ok_or(ENODEV)?
            .inspect_err(|e| {
                dev_err_ratelimited!(self.dev, "Touch ID: re-arm: could not save the FV record pair: {:?}\n", e);
            })?;
        dev_info_ratelimited!(
            self.dev,
            "Touch ID: provisioned the Linux FV re-arm record pair ({} + {} bytes)\n",
            kek.len(),
            vek.len()
        );
        Ok(())
    }

    /// Performs the password authorization event that ends a biometric
    /// lockout: one successful option-2 unwrap of the Linux-owned FV pair.
    /// The pair is created on first use and reloaded from the host store
    /// afterwards. A stored pair the enclave refuses, or one that no longer
    /// decodes, is replaced by a freshly proved pair.
    // No longer on the re-arm path: the password verification of the
    // designated bag ends the lockout without an FV unwrap. Kept until the
    // finger test settles whether any configuration still needs it.
    #[allow(dead_code)]
    pub(crate) fn rearm_touchid_fv(&self, secret: &[u8]) -> Result<()> {
        self.fv_ready()?;
        let stored = self
            .with_host_store(|store| store.read(&FV_REARM_STORE_KEY))
            .ok_or(ENODEV)??;
        if let Some(raw) = stored {
            match decode_rearm_records(&raw) {
                Ok(records) => match self.unwrap_rearm_vek(secret, records.kek, records.vek) {
                    Ok(()) => return Ok(()),
                    Err(e) if e == EACCES => dev_warn_ratelimited!(
                        self.dev,
                        "Touch ID: re-arm: the enclave refused the stored FV record pair; provisioning a new one\n"
                    ),
                    Err(e) => return Err(e),
                },
                Err(_) => dev_warn_ratelimited!(
                    self.dev,
                    "Touch ID: re-arm: the stored FV record pair does not decode; provisioning a new one\n"
                ),
            }
        }
        self.provision_rearm_records(secret)
    }

    fn sks_req_unwrap_file_key(
        &self,
        volume_uuid: &[u8; 16],
        protection_class: u32,
        wrapped_ekwk: &[u8],
        wrapped_ek: &[u8],
    ) -> Result<SksRequest> {
        let class = Self::pfk_class(protection_class)?;
        let bag_uuid = self.resolve_fv_uuid(volume_uuid);
        let params = Self::pfk_params(&bag_uuid);

        let mut body = image::Body::new();
        body.put_u32(PFK_UNWRAP_VERSION)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_i32(PFK_SYSTEM_VOLUME_HANDLE)?;
        body.put_blob(&params)?;
        body.put_u32(class)?;
        body.put_blob(wrapped_ek)?;
        body.put_blob(wrapped_ekwk)?;
        body.put_blob(&[])?;
        body.put_u32(PFK_UNWRAP_OPTIONS)?;

        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_UNWRAP_PFK_NAME,
            msg: crate::sks::encode_sks_unwrap_pfk(self.sks_next_seq(), len),
            img,
        })
    }

    fn unwrap_file_key(
        &self,
        volume_uuid: &[u8; 16],
        protection_class: u32,
        wrapped_ekwk: &[u8],
        wrapped_ek: &[u8],
    ) -> Result<FileKey> {
        self.fv_ready()?;
        let out = self
            .sks_send(self.sks_req_unwrap_file_key(
                volume_uuid,
                protection_class,
                wrapped_ekwk,
                wrapped_ek,
            ))
            .ok_or(EIO)?;
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(
                self.dev,
                "fv: UNWRAP_PFK v2 class {} ekwk {} ek {} failed with status {}\n",
                protection_class,
                wrapped_ekwk.len(),
                wrapped_ek.len(),
                status
            );
            return Err(EACCES);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_UNWRAP_PFK_NAME, &out)
            .ok_or(EMSGSIZE)?;
        let mut fields = proto::FieldCursor::new(body);
        if fields.i32() != Some(PFK_UNWRAP_VERSION as i32) {
            return Err(EMSGSIZE);
        }
        let key = fields.blob().ok_or(EMSGSIZE)?;
        let iv_key = fields.blob().ok_or(EMSGSIZE)?;
        let _ephemeral_key = fields.blob().ok_or(EMSGSIZE)?;
        let flags = fields.i32().ok_or(EMSGSIZE)?;
        if key.len() != OPAQUE_KEY_LEN || iv_key.len() != IV_KEY_LEN || flags & PFK_KEY_FLAG == 0 {
            return Err(EMSGSIZE);
        }
        let mut opaque = [0; OPAQUE_KEY_LEN];
        opaque.copy_from_slice(key);
        let mut iv = [0; IV_KEY_LEN];
        iv.copy_from_slice(iv_key);
        Ok(FileKey { opaque, iv_key: iv })
    }

    fn sks_req_new_file_key(
        &self,
        volume_uuid: &[u8; 16],
        protection_class: u32,
    ) -> Result<SksRequest> {
        let class = Self::pfk_class(protection_class)?;
        let bag_uuid = self.resolve_fv_uuid(volume_uuid);
        let params = Self::pfk_params(&bag_uuid);
        let mut context = [0u8; PFK_FS_CONTEXT_LEN];
        context[10..26].copy_from_slice(&bag_uuid);
        let mut body = image::Body::new();
        body.put_u32(PFK_NEW_VERSION)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_i32(PFK_SYSTEM_VOLUME_HANDLE)?;
        body.put_blob(&params)?;
        body.put_u32(class)?;
        body.put_u32(PFK_NEW_OPTIONS)?;
        body.put_blob(&context)?;
        body.put_blob(&[])?;

        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_NEW_PFK_NAME,
            msg: crate::sks::encode_sks_new_pfk(self.sks_next_seq(), len),
            img,
        })
    }

    fn new_file_key(&self, volume_uuid: &[u8; 16], protection_class: u32) -> Result<NewFileKey> {
        self.fv_ready()?;
        let class = Self::pfk_class(protection_class)?;
        let out = self
            .sks_send(self.sks_req_new_file_key(volume_uuid, protection_class))
            .ok_or(EIO)?;
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(
                self.dev,
                "fv: NEW_PFK class {} failed with status {}\n",
                protection_class,
                status
            );
            return Err(EACCES);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_NEW_PFK_NAME, &out)
            .ok_or(EMSGSIZE)?;
        let mut fields = proto::FieldCursor::new(body);
        if fields.i32() != Some(PFK_NEW_VERSION as i32) {
            return Err(EMSGSIZE);
        }
        let opaque = fields.blob().ok_or(EMSGSIZE)?;
        let iv = fields.blob().ok_or(EMSGSIZE)?;
        let ephemeral = fields.blob().ok_or(EMSGSIZE)?;
        let wrapped_ek = fields.blob().ok_or(EMSGSIZE)?;
        let wrapped_ekwk = fields.blob().ok_or(EMSGSIZE)?;
        let key_flags = fields.i32().ok_or(EMSGSIZE)?;
        let wrapped_class = fields.i32().ok_or(EMSGSIZE)?;
        if opaque.len() != OPAQUE_KEY_LEN
            || iv.len() != IV_KEY_LEN
            || ephemeral.is_empty()
            || ephemeral.len() > EPHEMERAL_KEY_MAX_LEN
            || wrapped_ek.is_empty()
            || wrapped_ek.len() > FILE_KEY_MAX_LEN
            || wrapped_ekwk.is_empty()
            || wrapped_ekwk.len() > FILE_KEY_MAX_LEN
            || key_flags & PFK_NEW_KEY_FLAG == 0
            || wrapped_class != class as i32
        {
            dev_err_ratelimited!(
                self.dev,
                "fv: NEW_PFK malformed response: key {} iv {} eph {} ek {} ekwk {} flags {:#x} class {} expected {}\n",
                opaque.len(),
                iv.len(),
                ephemeral.len(),
                wrapped_ek.len(),
                wrapped_ekwk.len(),
                key_flags,
                wrapped_class,
                class
            );
            return Err(EMSGSIZE);
        }

        let mut key = FileKey {
            opaque: [0; OPAQUE_KEY_LEN],
            iv_key: [0; IV_KEY_LEN],
        };
        key.opaque.copy_from_slice(opaque);
        key.iv_key.copy_from_slice(iv);
        let mut result = NewFileKey {
            key,
            wrapped_ekwk: [0; FILE_KEY_MAX_LEN],
            wrapped_ek: [0; FILE_KEY_MAX_LEN],
            wrapped_ekwk_len: wrapped_ekwk.len(),
            wrapped_ek_len: wrapped_ek.len(),
        };
        result.wrapped_ekwk[..wrapped_ekwk.len()].copy_from_slice(wrapped_ekwk);
        result.wrapped_ek[..wrapped_ek.len()].copy_from_slice(wrapped_ek);
        Ok(result)
    }

    fn sks_req_load_class_keys(
        &self,
        volume_uuid: &[u8; 16],
        secret: &[u8],
        unlock_record: &[u8],
        volume_key: &[u8],
    ) -> Result<SksRequest> {
        let options = self.fv_options(volume_uuid);
        let params = Self::fv_params(options);
        let mut body = image::Body::new();
        body.put_u32(0)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_blob(&params)?;
        body.put_u32(FV_LOAD_CLASS_KEYS)?;
        body.put_u64(options)?;
        body.put_blob(secret)?;
        body.put_blob(unlock_record)?;
        body.put_blob(volume_key)?;

        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_SET_PROTECTION_NAME,
            msg: crate::sks::encode_sks_set_protection(self.sks_next_seq(), len),
            img,
        })
    }

    fn load_class_keys(
        &self,
        volume_uuid: &[u8; 16],
        secret: &[u8],
        unlock_record: &[u8],
        volume_key: &[u8],
    ) -> Result<()> {
        self.fv_ready()?;
        let bag_uuid = self.fv_blob_uuid(volume_uuid, volume_key)?;
        let request =
            self.sks_req_load_class_keys(volume_uuid, secret, unlock_record, volume_key)?;
        if !self.retain_fv_volume(volume_uuid, &bag_uuid)? {
            return Ok(());
        }
        let out = match self.sks_send(Ok(request)) {
            Some(out) => out,
            None => {
                let _ = self.release_fv_volume(volume_uuid, &bag_uuid);
                return Err(EIO);
            }
        };
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err_ratelimited!(
                self.dev,
                "fv: LOAD_CLASS_KEYS failed with status {}\n",
                status
            );
            let _ = self.release_fv_volume(volume_uuid, &bag_uuid);
            return Err(EACCES);
        }
        let body = match self.sks_report_response(crate::sks::SKS_SET_PROTECTION_NAME, &out) {
            Some(body) => body,
            None => {
                self.rollback_class_keys(volume_uuid, volume_key);
                let _ = self.release_fv_volume(volume_uuid, &bag_uuid);
                return Err(EMSGSIZE);
            }
        };
        let mut fields = proto::FieldCursor::new(body);
        if fields.i32() != Some(0) || fields.blob().is_none() {
            self.rollback_class_keys(volume_uuid, volume_key);
            let _ = self.release_fv_volume(volume_uuid, &bag_uuid);
            return Err(EMSGSIZE);
        }
        let apfs_hi = u64::from_be_bytes(volume_uuid[..8].try_into().unwrap());
        let apfs_lo = u64::from_be_bytes(volume_uuid[8..].try_into().unwrap());
        let bag_hi = u64::from_be_bytes(bag_uuid[..8].try_into().unwrap());
        let bag_lo = u64::from_be_bytes(bag_uuid[8..].try_into().unwrap());
        dev_info_ratelimited!(
            self.dev,
            "fv: mapped APFS UUID {:016x}-{:016x} to keybag UUID {:016x}-{:016x}\n",
            apfs_hi,
            apfs_lo,
            bag_hi,
            bag_lo
        );
        Ok(())
    }

    fn rollback_class_keys(&self, volume_uuid: &[u8; 16], volume_key: &[u8]) {
        if let Ok(request) = self.sks_req_unload_class_keys(volume_uuid, volume_key) {
            let _ = self.sks_send(Ok(request));
        }
    }

    fn sks_req_unload_class_keys(
        &self,
        volume_uuid: &[u8; 16],
        volume_key: &[u8],
    ) -> Result<SksRequest> {
        let options = self.fv_options(volume_uuid);
        let params = Self::fv_params(options);
        let mut body = image::Body::new();
        body.put_u32(0)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_blob(&params)?;
        body.put_u32(FV_UNLOAD_CLASS_KEYS)?;
        body.put_u64(options)?;
        body.put_blob(&[])?;
        body.put_blob(&[])?;
        body.put_blob(volume_key)?;

        let img = image::build_request(image::Version::V1, self.sks_timestamp_us(), &body)?;
        let len = self.sks_image_len(&img)?;
        Ok(SksRequest {
            name: crate::sks::SKS_SET_PROTECTION_NAME,
            msg: crate::sks::encode_sks_set_protection(self.sks_next_seq(), len),
            img,
        })
    }

    fn unload_class_keys(&self, volume_uuid: &[u8; 16], volume_key: &[u8]) -> Result<()> {
        self.fv_ready()?;
        let bag_uuid = self.fv_blob_uuid(volume_uuid, volume_key)?;
        if !self.fv_volume_is_last(volume_uuid, &bag_uuid)? {
            return self.release_fv_volume(volume_uuid, &bag_uuid);
        }
        let result = (|| {
            let out = self
                .sks_send(self.sks_req_unload_class_keys(volume_uuid, volume_key))
                .ok_or(EIO)?;
            if out.reply.status != 0 {
                return Err(EACCES);
            }
            let body = self
                .sks_report_response(crate::sks::SKS_SET_PROTECTION_NAME, &out)
                .ok_or(EMSGSIZE)?;
            let mut fields = proto::FieldCursor::new(body);
            if fields.i32() != Some(0) || fields.blob().is_none() {
                return Err(EMSGSIZE);
            }
            Ok(())
        })();
        let release = self.release_fv_volume(volume_uuid, &bag_uuid);
        result.and(release)
    }
}

/// Borrow a C input buffer as a slice, rejecting null/empty/oversized inputs.
///
/// # Safety
///
/// When `ptr` is non-null and `len` is in `1..=max`, `ptr` must be valid for
/// reads of `len` bytes and that region must stay live and unmutated for the
/// whole of `'a`. The caller must pick `'a` so the returned slice cannot
/// outlive the underlying buffer.
unsafe fn input<'a>(ptr: *const u8, len: usize, max: usize) -> Result<&'a [u8]> {
    if ptr.is_null() || len == 0 || len > max {
        return Err(EINVAL);
    }
    // SAFETY: `ptr` is non-null and `len` is in `1..=max` here, so by this
    // function's contract the region is valid for reads of `len` bytes and
    // stays live for `'a`.
    Ok(unsafe { core::slice::from_raw_parts(ptr, len) })
}

/// Borrow an optional C input buffer, treating a zero length as empty.
///
/// # Safety
///
/// Same obligations as [`input`]: when `len` is non-zero, `ptr` must be valid
/// for reads of `len` bytes and stay live for `'a`.
unsafe fn optional_input<'a>(ptr: *const u8, len: usize, max: usize) -> Result<&'a [u8]> {
    if len == 0 {
        return Ok(&[]);
    }
    // SAFETY: `len` is non-zero here; this function's contract is identical to
    // `input`'s, so the caller already guarantees `ptr`/`len` are valid.
    unsafe { input(ptr, len, max) }
}

/// Borrow a C output slot for one key.
///
/// # Safety
///
/// When `ptr` is non-null it must be aligned, point to a live and writable
/// `KernelKey`, and grant exclusive access for the whole of `'a` (no other
/// alias may touch it while the returned reference lives).
unsafe fn output<'a>(ptr: *mut KernelKey) -> Result<&'a mut KernelKey> {
    if ptr.is_null() {
        return Err(EINVAL);
    }
    // SAFETY: `ptr` is non-null here, so by this function's contract it is an
    // aligned, exclusively-owned, writable `KernelKey` that outlives `'a`.
    Ok(unsafe { &mut *ptr })
}

/// Borrow a C output slot for one new-file-key result.
///
/// # Safety
///
/// When `ptr` is non-null it must be aligned, point to a live and writable
/// `KernelNewFileKey`, and grant exclusive access for the whole of `'a`.
unsafe fn new_file_output<'a>(ptr: *mut KernelNewFileKey) -> Result<&'a mut KernelNewFileKey> {
    if ptr.is_null() {
        return Err(EINVAL);
    }
    // SAFETY: `ptr` is non-null here, so by this function's contract it is an
    // aligned, exclusively-owned, writable `KernelNewFileKey` that outlives
    // `'a`.
    Ok(unsafe { &mut *ptr })
}

/// Recover the driver state from a registered callback context pointer.
///
/// # Safety
///
/// When `context` is non-null it must be the pointer passed to
/// `sep_fv_register_v2` — a `*const SepData` taken from a live `Arc<SepData>` —
/// and must still be registered, so the `SepData` is alive and only shared
/// (never uniquely) borrowed for the whole of `'a`.
unsafe fn sep<'a>(context: *mut c_void) -> Result<&'a SepData> {
    if context.is_null() {
        return Err(ENODEV);
    }
    // SAFETY: `context` is non-null here, so by this function's contract it
    // points to a live `SepData` that stays valid and shared for `'a`.
    Ok(unsafe { &*context.cast::<SepData>() })
}

/// # Safety
///
/// Invoked only through the registered `sep_fv_ops` dispatch (which holds the
/// registration lock): `context` is the pointer passed to `sep_fv_register_v2`
/// and is still registered; `wrapped`/`wrapped_len` describe a buffer readable
/// for the duration of the call (or are null/0); and `key` points to aligned,
/// exclusively-owned, writable storage for one `KernelKey`.
unsafe extern "C" fn kernel_unwrap_media_key(
    context: *mut c_void,
    wrapped: *const u8,
    wrapped_len: usize,
    protection_class: u32,
    key: *mut KernelKey,
) -> c_int {
    let result: Result<()> = (|| {
        // SAFETY: per this function's contract `context` is the still-registered
        // `SepData` pointer, alive for the duration of the call.
        let this = unsafe { sep(context)? };
        // SAFETY: per this function's contract `wrapped`/`wrapped_len` describe a
        // buffer readable for the call; the borrow does not outlive it.
        let wrapped = unsafe { input(wrapped, wrapped_len, WRAPPED_KEY_LEN)? };
        let wrapped: &[u8; WRAPPED_KEY_LEN] = wrapped.try_into().map_err(|_| EINVAL)?;
        // SAFETY: per this function's contract `key` is exclusive writable
        // storage for one `KernelKey`, valid for the duration of the call.
        let key = unsafe { output(key)? };
        let unwrapped = this.unwrap_media_key_from_class(wrapped, protection_class)?;
        key.opaque.copy_from_slice(&unwrapped.opaque);
        key.iv.copy_from_slice(&unwrapped.iv_key);
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |()| 0)
}

/// # Safety
///
/// Invoked only through the registered `sep_fv_ops` dispatch (which holds the
/// registration lock): `context` is the pointer passed to `sep_fv_register_v2`
/// and is still registered; each `*const u8`/length pair describes a buffer
/// readable for the duration of the call (the optional `secret`/`unlock_record`
/// may be null with a zero length); and `key` points to aligned,
/// exclusively-owned, writable storage for one `KernelKey`.
unsafe extern "C" fn kernel_unwrap_volume_key(
    context: *mut c_void,
    secret: *const u8,
    secret_len: usize,
    unlock_record: *const u8,
    unlock_record_len: usize,
    volume_key: *const u8,
    volume_key_len: usize,
    key: *mut KernelKey,
) -> c_int {
    let result: Result<()> = (|| {
        // SAFETY: per this function's contract `context` is the still-registered
        // `SepData` pointer, alive for the duration of the call.
        let this = unsafe { sep(context)? };
        // SAFETY: per this function's contract `secret`/`secret_len` describe a
        // buffer readable for the call, or are null/0.
        let secret = unsafe { optional_input(secret, secret_len, SECRET_MAX_LEN)? };
        // SAFETY: per this function's contract `unlock_record`/`unlock_record_len`
        // describe a buffer readable for the call, or are null/0.
        let unlock_record =
            unsafe { optional_input(unlock_record, unlock_record_len, RECORD_MAX_LEN)? };
        // SAFETY: per this function's contract `volume_key`/`volume_key_len`
        // describe a buffer readable for the call.
        let volume_key = unsafe { input(volume_key, volume_key_len, RECORD_MAX_LEN)? };
        // SAFETY: per this function's contract `key` is exclusive writable
        // storage for one `KernelKey`, valid for the duration of the call.
        let key = unsafe { output(key)? };
        let unwrapped = this.unwrap_vek(secret, unlock_record, volume_key)?;
        key.opaque.copy_from_slice(&unwrapped.opaque);
        key.iv.fill(0);
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |()| 0)
}

/// # Safety
///
/// Invoked only through the registered `sep_fv_ops` dispatch (which holds the
/// registration lock): `context` is the pointer passed to `sep_fv_register_v2`
/// and is still registered; `volume_uuid` points to 16 readable bytes; and each
/// other `*const u8`/length pair describes a buffer readable for the duration
/// of the call (the optional `secret`/`unlock_record` may be null with a zero
/// length).
unsafe extern "C" fn kernel_load_class_keys(
    context: *mut c_void,
    volume_uuid: *const u8,
    secret: *const u8,
    secret_len: usize,
    unlock_record: *const u8,
    unlock_record_len: usize,
    volume_key: *const u8,
    volume_key_len: usize,
) -> c_int {
    let result: Result<()> = (|| {
        // SAFETY: per this function's contract `context` is the still-registered
        // `SepData` pointer, alive for the duration of the call.
        let this = unsafe { sep(context)? };
        // SAFETY: per this function's contract `volume_uuid` points to 16
        // readable bytes for the duration of the call.
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        // SAFETY: per this function's contract `secret`/`secret_len` describe a
        // buffer readable for the call, or are null/0.
        let secret = unsafe { optional_input(secret, secret_len, SECRET_MAX_LEN)? };
        // SAFETY: per this function's contract `unlock_record`/`unlock_record_len`
        // describe a buffer readable for the call, or are null/0.
        let unlock_record =
            unsafe { optional_input(unlock_record, unlock_record_len, RECORD_MAX_LEN)? };
        // SAFETY: per this function's contract `volume_key`/`volume_key_len`
        // describe a buffer readable for the call.
        let volume_key = unsafe { input(volume_key, volume_key_len, RECORD_MAX_LEN)? };
        this.load_class_keys(volume_uuid, secret, unlock_record, volume_key)
    })();
    result.map_or_else(|error| error.to_errno(), |()| 0)
}

/// # Safety
///
/// Invoked only through the registered `sep_fv_ops` dispatch (which holds the
/// registration lock): `context` is the pointer passed to `sep_fv_register_v2`
/// and is still registered; `volume_uuid` points to 16 readable bytes; and
/// `volume_key`/`volume_key_len` describe a buffer readable for the duration of
/// the call.
unsafe extern "C" fn kernel_unload_class_keys(
    context: *mut c_void,
    volume_uuid: *const u8,
    volume_key: *const u8,
    volume_key_len: usize,
) -> c_int {
    let result: Result<()> = (|| {
        // SAFETY: per this function's contract `context` is the still-registered
        // `SepData` pointer, alive for the duration of the call.
        let this = unsafe { sep(context)? };
        // SAFETY: per this function's contract `volume_uuid` points to 16
        // readable bytes for the duration of the call.
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        // SAFETY: per this function's contract `volume_key`/`volume_key_len`
        // describe a buffer readable for the call.
        let volume_key = unsafe { input(volume_key, volume_key_len, RECORD_MAX_LEN)? };
        this.unload_class_keys(volume_uuid, volume_key)
    })();
    result.map_or_else(|error| error.to_errno(), |()| 0)
}

/// # Safety
///
/// Invoked only through the registered `sep_fv_ops` dispatch (which holds the
/// registration lock): `context` is the pointer passed to `sep_fv_register_v2`
/// and is still registered; `volume_uuid` points to 16 readable bytes; each
/// `wrapped_*`/length pair describes a buffer readable for the duration of the
/// call; and `key` points to aligned, exclusively-owned, writable storage for
/// one `KernelKey`.
unsafe extern "C" fn kernel_unwrap_file_key(
    context: *mut c_void,
    volume_uuid: *const u8,
    protection_class: u32,
    wrapped_ekwk: *const u8,
    wrapped_ekwk_len: usize,
    wrapped_ek: *const u8,
    wrapped_ek_len: usize,
    key: *mut KernelKey,
) -> c_int {
    let result: Result<()> = (|| {
        // SAFETY: per this function's contract `context` is the still-registered
        // `SepData` pointer, alive for the duration of the call.
        let this = unsafe { sep(context)? };
        // SAFETY: per this function's contract `volume_uuid` points to 16
        // readable bytes for the duration of the call.
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        // SAFETY: per this function's contract `wrapped_ekwk`/`wrapped_ekwk_len`
        // describe a buffer readable for the call.
        let wrapped_ekwk = unsafe { input(wrapped_ekwk, wrapped_ekwk_len, FILE_KEY_MAX_LEN)? };
        // SAFETY: per this function's contract `wrapped_ek`/`wrapped_ek_len`
        // describe a buffer readable for the call.
        let wrapped_ek = unsafe { input(wrapped_ek, wrapped_ek_len, FILE_KEY_MAX_LEN)? };
        // SAFETY: per this function's contract `key` is exclusive writable
        // storage for one `KernelKey`, valid for the duration of the call.
        let key = unsafe { output(key)? };
        let unwrapped =
            this.unwrap_file_key(volume_uuid, protection_class, wrapped_ekwk, wrapped_ek)?;
        key.opaque.copy_from_slice(&unwrapped.opaque);
        key.iv.copy_from_slice(&unwrapped.iv_key);
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |()| 0)
}

/// # Safety
///
/// Invoked only through the registered `sep_fv_ops` dispatch (which holds the
/// registration lock): `context` is the pointer passed to `sep_fv_register_v2`
/// and is still registered; `volume_uuid` points to 16 readable bytes; and
/// `key` points to aligned, exclusively-owned, writable storage for one
/// `KernelNewFileKey`.
unsafe extern "C" fn kernel_new_file_key(
    context: *mut c_void,
    volume_uuid: *const u8,
    protection_class: u32,
    crypto_id: u64,
    key_revision: u16,
    key: *mut KernelNewFileKey,
) -> c_int {
    let result: Result<()> = (|| {
        // SAFETY: per this function's contract `context` is the still-registered
        // `SepData` pointer, alive for the duration of the call.
        let this = unsafe { sep(context)? };
        // SAFETY: per this function's contract `volume_uuid` points to 16
        // readable bytes for the duration of the call.
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        // SAFETY: per this function's contract `key` is exclusive writable
        // storage for one `KernelNewFileKey`, valid for the duration of the call.
        let key = unsafe { new_file_output(key)? };
        if crypto_id == 0 || key_revision == 0 {
            return Err(EINVAL);
        }
        let generated = this.new_file_key(volume_uuid, protection_class)?;
        key.key.opaque.copy_from_slice(&generated.key.opaque);
        key.key.iv.copy_from_slice(&generated.key.iv_key);
        key.wrapped_ekwk.copy_from_slice(&generated.wrapped_ekwk);
        key.wrapped_ek.copy_from_slice(&generated.wrapped_ek);
        key.wrapped_ekwk_len = generated.wrapped_ekwk_len;
        key.wrapped_ek_len = generated.wrapped_ek_len;
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |()| 0)
}
