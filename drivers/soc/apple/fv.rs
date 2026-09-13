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
    new_file_key: unsafe extern "C" fn(
        *mut c_void,
        *const u8,
        u32,
        u64,
        u16,
        *mut KernelNewFileKey,
    ) -> c_int,
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

    fn record_fv_volume(&self, apfs_uuid: &[u8; 16], bag_uuid: &[u8; 16]) -> Result<()> {
        let mut volumes = self.fv_volumes.lock();
        if let Some(entry) = volumes
            .iter_mut()
            .find(|entry| entry.apfs_uuid == *apfs_uuid)
        {
            if entry.bag_uuid != *bag_uuid {
                return Err(EINVAL);
            }
            entry.refs = entry.refs.checked_add(1).ok_or(EOVERFLOW)?;
            return Ok(());
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
        Ok(())
    }

    fn unrecord_fv_volume(&self, apfs_uuid: &[u8; 16], bag_uuid: &[u8; 16]) -> Result<()> {
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
            dev_err!(self.dev, "fv: GET_BLOB_STATE failed with status {}\n", status);
            return Err(EACCES);
        }
        let body = self
            .sks_report_response(crate::sks::SKS_GET_BLOB_STATE_NAME, &out)
            .ok_or(EMSGSIZE)?;
        let mut fields = proto::FieldCursor::new(body);
        let version = fields.i32();
        if version != Some(0) {
            dev_err!(
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
                dev_err!(
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
                dev_err!(
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
            dev_err!(
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
            dev_err!(
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
        let mut body = image::Body::new();
        body.put_u32(FV_UNWRAP_VERSION)?;
        body.put_u64(SYSTEM_CLIENT)?;
        body.put_blob(&FV_PARAMS_DER)?;
        body.put_u32(FV_UNWRAP_OPTIONS)?;
        body.put_blob(secret)?;
        body.put_blob(kek)?;
        body.put_blob(vek)?;
        body.put_blob(&[])?;

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
            dev_err!(self.dev, "fv: UNWRAP_VEK failed with status {}\n", status);
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
            dev_err!(
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

    fn new_file_key(
        &self,
        volume_uuid: &[u8; 16],
        protection_class: u32,
    ) -> Result<NewFileKey> {
        self.fv_ready()?;
        let class = Self::pfk_class(protection_class)?;
        let out = self
            .sks_send(self.sks_req_new_file_key(volume_uuid, protection_class))
            .ok_or(EIO)?;
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err!(
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
            dev_err!(
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
        let request = self.sks_req_load_class_keys(
            volume_uuid,
            secret,
            unlock_record,
            volume_key,
        )?;
        self.record_fv_volume(volume_uuid, &bag_uuid)?;
        let out = match self.sks_send(Ok(request)) {
            Some(out) => out,
            None => {
                let _ = self.unrecord_fv_volume(volume_uuid, &bag_uuid);
                return Err(EIO);
            }
        };
        if out.reply.status != 0 {
            let status: i32 = out.reply.status.into();
            dev_err!(
                self.dev,
                "fv: LOAD_CLASS_KEYS failed with status {}\n",
                status
            );
            let _ = self.unrecord_fv_volume(volume_uuid, &bag_uuid);
            return Err(EACCES);
        }
        let body = match self.sks_report_response(crate::sks::SKS_SET_PROTECTION_NAME, &out) {
            Some(body) => body,
            None => {
                self.rollback_class_keys(volume_uuid, volume_key);
                let _ = self.unrecord_fv_volume(volume_uuid, &bag_uuid);
                return Err(EMSGSIZE);
            }
        };
        let mut fields = proto::FieldCursor::new(body);
        if fields.i32() != Some(0) || fields.blob().is_none() {
            self.rollback_class_keys(volume_uuid, volume_key);
            let _ = self.unrecord_fv_volume(volume_uuid, &bag_uuid);
            return Err(EMSGSIZE);
        }
        let apfs_hi = u64::from_be_bytes(volume_uuid[..8].try_into().unwrap());
        let apfs_lo = u64::from_be_bytes(volume_uuid[8..].try_into().unwrap());
        let bag_hi = u64::from_be_bytes(bag_uuid[..8].try_into().unwrap());
        let bag_lo = u64::from_be_bytes(bag_uuid[8..].try_into().unwrap());
        dev_info!(
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
        self.unrecord_fv_volume(volume_uuid, &bag_uuid)
    }
}

unsafe fn input<'a>(ptr: *const u8, len: usize, max: usize) -> Result<&'a [u8]> {
    if ptr.is_null() || len == 0 || len > max {
        return Err(EINVAL);
    }
    // SAFETY: the C API requires `ptr` to remain readable for `len` bytes for
    // the duration of the callback, and the callback does not retain it.
    Ok(unsafe { core::slice::from_raw_parts(ptr, len) })
}

unsafe fn optional_input<'a>(ptr: *const u8, len: usize, max: usize) -> Result<&'a [u8]> {
    if len == 0 {
        return Ok(&[]);
    }
    unsafe { input(ptr, len, max) }
}

unsafe fn output<'a>(ptr: *mut KernelKey) -> Result<&'a mut KernelKey> {
    if ptr.is_null() {
        return Err(EINVAL);
    }
    // SAFETY: the C API provides exclusive writable storage for one key.
    Ok(unsafe { &mut *ptr })
}

unsafe fn new_file_output<'a>(ptr: *mut KernelNewFileKey) -> Result<&'a mut KernelNewFileKey> {
    if ptr.is_null() {
        return Err(EINVAL);
    }
    // SAFETY: the C API provides exclusive writable storage for one result.
    Ok(unsafe { &mut *ptr })
}

unsafe fn sep<'a>(context: *mut c_void) -> Result<&'a SepData> {
    if context.is_null() {
        return Err(ENODEV);
    }
    // SAFETY: registration keeps `SepData` alive until all callbacks finish.
    Ok(unsafe { &*context.cast::<SepData>() })
}

unsafe extern "C" fn kernel_unwrap_media_key(
    context: *mut c_void,
    wrapped: *const u8,
    wrapped_len: usize,
    protection_class: u32,
    key: *mut KernelKey,
) -> c_int {
    let result: Result<()> = (|| {
        let this = unsafe { sep(context)? };
        let wrapped = unsafe { input(wrapped, wrapped_len, WRAPPED_KEY_LEN)? };
        let wrapped: &[u8; WRAPPED_KEY_LEN] = wrapped.try_into().map_err(|_| EINVAL)?;
        let key = unsafe { output(key)? };
        let unwrapped = this.unwrap_media_key_from_class(wrapped, protection_class)?;
        key.opaque.copy_from_slice(&unwrapped.opaque);
        key.iv.copy_from_slice(&unwrapped.iv_key);
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |_| 0)
}

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
        let this = unsafe { sep(context)? };
        let secret = unsafe { optional_input(secret, secret_len, SECRET_MAX_LEN)? };
        let unlock_record =
            unsafe { optional_input(unlock_record, unlock_record_len, RECORD_MAX_LEN)? };
        let volume_key = unsafe { input(volume_key, volume_key_len, RECORD_MAX_LEN)? };
        let key = unsafe { output(key)? };
        let unwrapped = this.unwrap_vek(secret, unlock_record, volume_key)?;
        key.opaque.copy_from_slice(&unwrapped.opaque);
        key.iv.fill(0);
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |_| 0)
}

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
        let this = unsafe { sep(context)? };
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        let secret = unsafe { optional_input(secret, secret_len, SECRET_MAX_LEN)? };
        let unlock_record =
            unsafe { optional_input(unlock_record, unlock_record_len, RECORD_MAX_LEN)? };
        let volume_key = unsafe { input(volume_key, volume_key_len, RECORD_MAX_LEN)? };
        this.load_class_keys(volume_uuid, secret, unlock_record, volume_key)
    })();
    result.map_or_else(|error| error.to_errno(), |_| 0)
}

unsafe extern "C" fn kernel_unload_class_keys(
    context: *mut c_void,
    volume_uuid: *const u8,
    volume_key: *const u8,
    volume_key_len: usize,
) -> c_int {
    let result: Result<()> = (|| {
        let this = unsafe { sep(context)? };
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        let volume_key = unsafe { input(volume_key, volume_key_len, RECORD_MAX_LEN)? };
        this.unload_class_keys(volume_uuid, volume_key)
    })();
    result.map_or_else(|error| error.to_errno(), |_| 0)
}

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
        let this = unsafe { sep(context)? };
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
        let wrapped_ekwk = unsafe { input(wrapped_ekwk, wrapped_ekwk_len, FILE_KEY_MAX_LEN)? };
        let wrapped_ek = unsafe { input(wrapped_ek, wrapped_ek_len, FILE_KEY_MAX_LEN)? };
        let key = unsafe { output(key)? };
        let unwrapped =
            this.unwrap_file_key(volume_uuid, protection_class, wrapped_ekwk, wrapped_ek)?;
        key.opaque.copy_from_slice(&unwrapped.opaque);
        key.iv.copy_from_slice(&unwrapped.iv_key);
        Ok(())
    })();
    result.map_or_else(|error| error.to_errno(), |_| 0)
}

unsafe extern "C" fn kernel_new_file_key(
    context: *mut c_void,
    volume_uuid: *const u8,
    protection_class: u32,
    crypto_id: u64,
    key_revision: u16,
    key: *mut KernelNewFileKey,
) -> c_int {
    let result: Result<()> = (|| {
        let this = unsafe { sep(context)? };
        let volume_uuid = unsafe { input(volume_uuid, 16, 16)? };
        let volume_uuid: &[u8; 16] = volume_uuid.try_into().map_err(|_| EINVAL)?;
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
    result.map_or_else(|error| error.to_errno(), |_| 0)
}
