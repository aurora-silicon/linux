// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Aurora Silicon

//! Strict, read-only APFS/FileVault consumer for Apple silicon.
//!
//! The on-disk structures are from the public Apple File System Reference and
//! the GPL-2.0 apfs-fuse parser.  Key bytes stay opaque: the SEP releases the
//! media key and VEK, and Apple NVMe consumes the hardware-wrapped result.

use core::ffi::{c_int, c_void};
use kernel::{
    error::to_result,
    fs::File,
    ioctl::{_IOR, _IOW},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    new_mutex,
    prelude::*,
    sync::{aref::ARef, Mutex},
    transmute::{AsBytes, FromBytes},
    uaccess::{UserPtr, UserSlice},
};

const BLOCK: usize = 4096;
const ROOT_INFO_SIZE: usize = 40;
const MAX_RANGE_BLOCKS: u64 = 16;
const MAX_RECORD: usize = 512;
const MOD: u64 = 0xffff_ffff;

const KB_VERSION: u16 = 2;
const KB_TAG_VOLUME_KEY: u16 = 2;
const KB_TAG_VOLUME_UNLOCK_RECORDS: u16 = 3;
const KB_TAG_WRAPPING_M_KEY: u16 = 5;
const KB_TAG_VOLUME_M_KEY: u16 = 6;
const WRAPPED_STATE_MAJOR: u16 = 5;
const WRAPPED_STATE_MINOR: u16 = 0;
const WRAPPED_MEDIA_KEY_LEN: usize = 40;
const MAX_FILE_KEY_RECORD: usize = 168;

const FVREAD_IOC_MAGIC: u32 = 0xf7;
const FVREAD_UNLOCK_FV_OFF: u32 = 1;

#[repr(C)]
#[derive(Clone, Copy)]
struct UnlockRequest {
    device_fd: i32,
    flags: u32,
    volume_uuid: [u8; 16],
}

// SAFETY: every bit pattern is valid for these integer and byte-array fields.
unsafe impl FromBytes for UnlockRequest {}

#[repr(C)]
#[derive(Clone, Copy)]
struct ReadRequest {
    physical_block: u64,
    dun: u64,
    block_count: u32,
    reserved: u32,
    buffer: u64,
}

// SAFETY: every bit pattern is valid for these integer fields.
unsafe impl FromBytes for ReadRequest {}

#[repr(C)]
struct ReadFileRequest {
    io: ReadRequest,
    protection_class: u32,
    wrapped_ekwk_len: u32,
    wrapped_ek_len: u32,
    reserved: u32,
    wrapped_ekwk: [u8; MAX_FILE_KEY_RECORD],
    wrapped_ek: [u8; MAX_FILE_KEY_RECORD],
}

// SAFETY: every bit pattern is valid for these integer and byte-array fields.
unsafe impl FromBytes for ReadFileRequest {}

impl Drop for ReadFileRequest {
    fn drop(&mut self) {
        self.wrapped_ekwk.fill(0);
        self.wrapped_ek.fill(0);
    }
}

#[repr(C)]
struct Info {
    container_bytes: u64,
    block_size: u32,
    flags: u32,
    volume_uuid: [u8; 16],
}

// SAFETY: Info has no padding and all fields contain initialized bytes.
unsafe impl AsBytes for Info {}

const FVREAD_IOC_UNLOCK: u32 = _IOW::<UnlockRequest>(FVREAD_IOC_MAGIC, 0x01);
const FVREAD_IOC_READ: u32 = _IOW::<ReadRequest>(FVREAD_IOC_MAGIC, 0x02);
const FVREAD_IOC_READ_FILE: u32 = _IOW::<ReadFileRequest>(FVREAD_IOC_MAGIC, 0x03);
const FVREAD_IOC_GET_INFO: u32 = _IOR::<Info>(FVREAD_IOC_MAGIC, 0x04);

#[repr(C)]
struct FvKey {
    opaque: [u8; 64],
    iv: [u8; 16],
}

impl FvKey {
    fn zeroed() -> Self {
        Self {
            opaque: [0; 64],
            iv: [0; 16],
        }
    }

    fn wipe(&mut self) {
        self.opaque.fill(0);
        self.iv.fill(0);
    }
}

impl Drop for FvKey {
    fn drop(&mut self) {
        self.wipe();
    }
}

unsafe extern "C" {
    fn fvread_block_open(fd: c_int, handle: *mut *mut c_void, size: *mut u64) -> c_int;
    fn fvread_block_close(handle: *mut c_void);
    fn fvread_block_read_plain(
        handle: *mut c_void,
        offset: u64,
        data: *mut c_void,
        len: usize,
    ) -> isize;
    fn fvread_block_read_crypto(
        handle: *mut c_void,
        offset: u64,
        dun: u64,
        key: *const FvKey,
        data: *mut c_void,
        len: usize,
    ) -> isize;
    fn apple_sep_fv_unwrap_media_key(
        wrapped: *const u8,
        wrapped_len: usize,
        protection_class: u32,
        key: *mut FvKey,
    ) -> c_int;
    fn apple_sep_fv_unwrap_volume_key(
        secret: *const u8,
        secret_len: usize,
        unlock_record: *const u8,
        unlock_record_len: usize,
        volume_key: *const u8,
        volume_key_len: usize,
        key: *mut FvKey,
    ) -> c_int;
    fn apple_sep_fv_load_class_keys_v2(
        volume_uuid: *const u8,
        secret: *const u8,
        secret_len: usize,
        unlock_record: *const u8,
        unlock_record_len: usize,
        volume_key: *const u8,
        volume_key_len: usize,
    ) -> c_int;
    fn apple_sep_fv_unload_class_keys_v2(
        volume_uuid: *const u8,
        volume_key: *const u8,
        volume_key_len: usize,
    ) -> c_int;
    fn apple_sep_fv_unwrap_file_key(
        volume_uuid: *const u8,
        protection_class: u32,
        wrapped_ekwk: *const u8,
        wrapped_ekwk_len: usize,
        wrapped_ek: *const u8,
        wrapped_ek_len: usize,
        key: *mut FvKey,
    ) -> c_int;
}

fn le16(data: &[u8], off: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(
        data.get(off..off + 2)
            .ok_or(EINVAL)?
            .try_into()
            .map_err(|_| EINVAL)?,
    ))
}

fn le32(data: &[u8], off: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(
        data.get(off..off + 4)
            .ok_or(EINVAL)?
            .try_into()
            .map_err(|_| EINVAL)?,
    ))
}

fn le64(data: &[u8], off: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(
        data.get(off..off + 8)
            .ok_or(EINVAL)?
            .try_into()
            .map_err(|_| EINVAL)?,
    ))
}

fn checksum(data: &[u8]) -> Result<u64> {
    if data.len() < 32 || data.len() % 4 != 0 {
        return Err(EINVAL);
    }
    let mut s1 = 0u64;
    let mut s2 = 0u64;
    for word in data[8..].chunks_exact(4) {
        let value = u64::from(u32::from_le_bytes(word.try_into().map_err(|_| EINVAL)?));
        s1 = (s1 + value) % MOD;
        s2 = (s2 + s1) % MOD;
    }
    let c1 = (MOD - (s1 + s2) % MOD) % MOD;
    let c2 = (MOD - (s1 + c1) % MOD) % MOD;
    Ok((c2 << 32) | c1)
}

fn checked_object(data: &[u8]) -> Result<()> {
    if le64(data, 0)? != checksum(data)? {
        return Err(EIO);
    }
    Ok(())
}

#[derive(Clone, Copy)]
struct Range {
    start: u64,
    blocks: u64,
}

impl Range {
    fn from(data: &[u8], off: usize, device_blocks: u64) -> Result<Self> {
        let range = Self {
            start: le64(data, off)?,
            blocks: le64(data, off + 8)?,
        };
        if range.start == 0
            || range.blocks == 0
            || range.blocks > MAX_RANGE_BLOCKS
            || range.start.checked_add(range.blocks).ok_or(EINVAL)? > device_blocks
        {
            return Err(ENOTSUPP);
        }
        Ok(range)
    }

    fn bytes(self) -> Result<usize> {
        usize::try_from(self.blocks)
            .ok()
            .and_then(|v| v.checked_mul(BLOCK))
            .ok_or(EINVAL)
    }

    fn offset(self) -> Result<u64> {
        self.start.checked_mul(BLOCK as u64).ok_or(EINVAL)
    }
}

struct Nx {
    xid: u64,
    uuid: [u8; 16],
    blocks: u64,
    desc_base: u64,
    desc_count: u32,
    omap: u64,
    keylocker: Range,
    mkb_locker: Range,
}

impl Nx {
    fn parse(data: &[u8], device_size: u64) -> Result<Self> {
        checked_object(data)?;
        if data.len() != BLOCK
            || data.get(32..36) != Some(b"NXSB")
            || le32(data, 36)? != BLOCK as u32
            || le32(data, 24)? & 0xffff != 1
            || le64(data, 56)? != 0
            || le64(data, 64)? != 2
        {
            return Err(ENOTSUPP);
        }
        let device_blocks = device_size / BLOCK as u64;
        let blocks = le64(data, 40)?;
        if blocks == 0 || blocks > device_blocks {
            return Err(EINVAL);
        }
        let desc_count = le32(data, 104)?;
        let desc_base = le64(data, 112)?;
        if desc_count == 0
            || desc_count & (1 << 31) != 0
            || desc_count > 1024
            || desc_base.checked_add(u64::from(desc_count)).ok_or(EINVAL)? > blocks
        {
            return Err(ENOTSUPP);
        }
        let mut uuid = [0; 16];
        uuid.copy_from_slice(data.get(72..88).ok_or(EINVAL)?);
        Ok(Self {
            xid: le64(data, 16)?,
            uuid,
            blocks,
            desc_base,
            desc_count,
            omap: le64(data, 160)?,
            keylocker: Range::from(data, 0x510, blocks)?,
            mkb_locker: Range::from(data, 0x570, blocks)?,
        })
    }

    fn same_container(&self, other: &Self) -> bool {
        self.uuid == other.uuid
            && self.blocks == other.blocks
            && self.desc_base == other.desc_base
            && self.desc_count == other.desc_count
            && self.omap == other.omap
    }
}

fn node_entry(node: &[u8], index: usize) -> Result<(&[u8], &[u8])> {
    if node.len() != BLOCK || le16(node, 34)? != 0 || le16(node, 32)? & 4 == 0 {
        return Err(ENOTSUPP);
    }
    let count = le32(node, 36)? as usize;
    if index >= count || count > 512 {
        return Err(EINVAL);
    }
    let toc = 56usize
        .checked_add(le16(node, 40)? as usize)
        .ok_or(EINVAL)?;
    let table_len = le16(node, 42)? as usize;
    let key_start = toc.checked_add(table_len).ok_or(EINVAL)?;
    let value_end = BLOCK
        - if le16(node, 32)? & 1 != 0 {
            ROOT_INFO_SIZE
        } else {
            0
        };
    if key_start > value_end || toc.checked_add(count * 4).ok_or(EINVAL)? > key_start {
        return Err(EINVAL);
    }
    let item = toc.checked_add(index * 4).ok_or(EINVAL)?;
    let key_offset = le16(node, item)? as usize;
    let value_offset = le16(node, item + 2)? as usize;
    let key_start_at = key_start.checked_add(key_offset).ok_or(EINVAL)?;
    let value_start_at = value_end.checked_sub(value_offset).ok_or(EINVAL)?;
    let key_end = key_start_at.checked_add(16).ok_or(EINVAL)?;
    let value_end_at = value_start_at.checked_add(16).ok_or(EINVAL)?;
    if key_start_at < key_start
        || key_end > value_end
        || value_start_at < key_start
        || value_end_at > value_end
    {
        return Err(EINVAL);
    }
    Ok((
        &node[key_start_at..key_end],
        &node[value_start_at..value_end_at],
    ))
}

struct Entry<'a> {
    uuid: [u8; 16],
    tag: u16,
    data: &'a [u8],
}

struct Locker<'a> {
    entries: KVec<Entry<'a>>,
}

impl<'a> Locker<'a> {
    fn parse(data: &'a [u8]) -> Result<Self> {
        if le16(data, 0)? != KB_VERSION {
            return Err(EPROTONOSUPPORT);
        }
        let count = le16(data, 2)? as usize;
        let nbytes = le32(data, 4)? as usize;
        if count > 256 || nbytes > data.len().saturating_sub(16) {
            return Err(EINVAL);
        }
        let end = 16usize.checked_add(nbytes).ok_or(EINVAL)?;
        let mut entries = KVec::new();
        let mut off = 16usize;
        for _ in 0..count {
            let header_end = off.checked_add(24).ok_or(EINVAL)?;
            if header_end > end {
                return Err(EINVAL);
            }
            let keylen = le16(data, off + 18)? as usize;
            if keylen > MAX_RECORD {
                return Err(E2BIG);
            }
            let data_end = header_end.checked_add(keylen).ok_or(EINVAL)?;
            if data_end > end {
                return Err(EINVAL);
            }
            let mut uuid = [0; 16];
            uuid.copy_from_slice(&data[off..off + 16]);
            entries.push(
                Entry {
                    uuid,
                    tag: le16(data, off + 16)?,
                    data: &data[header_end..data_end],
                },
                GFP_KERNEL,
            )?;
            off = data_end.checked_add(15).ok_or(EINVAL)? & !15;
        }
        if off > end {
            return Err(EINVAL);
        }
        Ok(Self { entries })
    }

    fn one(&self, uuid: &[u8; 16], tag: u16) -> Result<&'a [u8]> {
        let mut found = None;
        for entry in &self.entries {
            if entry.uuid == *uuid && entry.tag == tag {
                if found.replace(entry.data).is_some() {
                    return Err(EEXIST);
                }
            }
        }
        found.ok_or(ENODATA)
    }
}

struct MediaRecord {
    wrapped: [u8; WRAPPED_MEDIA_KEY_LEN],
    protection_class: u32,
}

impl Drop for MediaRecord {
    fn drop(&mut self) {
        self.wrapped.fill(0);
    }
}

struct SensitiveBuffer(KVec<u8>);

impl SensitiveBuffer {
    fn as_slice(&self) -> &[u8] {
        &self.0
    }

    fn as_mut_ptr(&mut self) -> *mut u8 {
        self.0.as_mut_ptr()
    }

    fn len(&self) -> usize {
        self.0.len()
    }
}

impl Drop for SensitiveBuffer {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}

fn media_record(locker: &Locker<'_>, uuid: &[u8; 16], tag: u16) -> Result<MediaRecord> {
    if !matches!(tag, KB_TAG_WRAPPING_M_KEY | KB_TAG_VOLUME_M_KEY) {
        return Err(EINVAL);
    }
    let data = locker.one(uuid, tag)?;
    if data.len() != 20 + WRAPPED_MEDIA_KEY_LEN
        || le16(data, 0)? != WRAPPED_STATE_MAJOR
        || le16(data, 2)? != WRAPPED_STATE_MINOR
        || le16(data, 18)? as usize != WRAPPED_MEDIA_KEY_LEN
    {
        return Err(EPROTONOSUPPORT);
    }
    let protection_class = le32(data, 8)?;
    if !matches!(protection_class, 1 | 2 | 3 | 4 | 6) {
        return Err(EPROTONOSUPPORT);
    }
    let mut wrapped = [0; WRAPPED_MEDIA_KEY_LEN];
    wrapped.copy_from_slice(&data[20..]);
    Ok(MediaRecord {
        wrapped,
        protection_class,
    })
}

struct VolumeRecords<'a> {
    wrapped_vek: &'a [u8],
    volume_keybag: Range,
}

fn volume_records<'a>(
    locker: &Locker<'a>,
    volume_uuid: &[u8; 16],
    device_blocks: u64,
) -> Result<VolumeRecords<'a>> {
    let wrapped_vek = locker.one(volume_uuid, KB_TAG_VOLUME_KEY)?;
    if wrapped_vek.is_empty() {
        return Err(EINVAL);
    }
    let prange = locker.one(volume_uuid, KB_TAG_VOLUME_UNLOCK_RECORDS)?;
    if prange.len() != 16 {
        return Err(EPROTONOSUPPORT);
    }
    Ok(VolumeRecords {
        wrapped_vek,
        volume_keybag: Range::from(prange, 0, device_blocks)?,
    })
}

struct OpenBlock {
    handle: *mut c_void,
    size: u64,
}

// The C allocation owns a referenced struct file and is serialized by the per-fd mutex.
unsafe impl Send for OpenBlock {}

impl OpenBlock {
    fn open(fd: i32) -> Result<Self> {
        let mut handle = core::ptr::null_mut();
        let mut size = 0;
        // SAFETY: both output pointers are valid for the call.
        to_result(unsafe { fvread_block_open(fd, &mut handle, &mut size) })?;
        Ok(Self { handle, size })
    }

    fn read_plain(&self, offset: u64, len: usize) -> Result<KVec<u8>> {
        let mut data = KVec::new();
        data.resize(len, 0, GFP_KERNEL)?;
        // SAFETY: handle is live and data is writable for len bytes.
        let ret =
            unsafe { fvread_block_read_plain(self.handle, offset, data.as_mut_ptr().cast(), len) };
        if ret < 0 {
            return Err(Error::from_errno((-ret) as c_int));
        }
        if ret as usize != len {
            return Err(EIO);
        }
        Ok(data)
    }

    fn read_crypto(&self, range: Range, key: &FvKey) -> Result<SensitiveBuffer> {
        let len = range.bytes()?;
        let mut data = KVec::new();
        data.resize(len, 0, GFP_KERNEL)?;
        let mut data = SensitiveBuffer(data);
        // SAFETY: handle/key are live and data is writable for len bytes.
        let ret = unsafe {
            fvread_block_read_crypto(
                self.handle,
                range.offset()?,
                range.start,
                key,
                data.as_mut_ptr().cast(),
                len,
            )
        };
        if ret < 0 {
            return Err(Error::from_errno((-ret) as c_int));
        }
        if ret as usize != len {
            return Err(EIO);
        }
        Ok(data)
    }
}

fn read_object(block: &OpenBlock, paddr: u64) -> Result<KVec<u8>> {
    let object = block.read_plain(paddr.checked_mul(BLOCK as u64).ok_or(EINVAL)?, BLOCK)?;
    checked_object(&object)?;
    Ok(object)
}

fn omap_lookup(block: &OpenBlock, omap_paddr: u64, oid: u64, xid: u64) -> Result<u64> {
    let omap = read_object(block, omap_paddr)?;
    if le64(&omap, 8)? != omap_paddr {
        return Err(EINVAL);
    }
    let tree_paddr = le64(&omap, 48)?;
    let tree = read_object(block, tree_paddr)?;
    if le64(&tree, 8)? != tree_paddr || le16(&tree, 34)? != 0 || le16(&tree, 32)? & 4 == 0 {
        return Err(ENOTSUPP);
    }
    let mut selected: Option<(u64, u32, u32, u64)> = None;
    for index in 0..le32(&tree, 36)? as usize {
        let (key, value) = node_entry(&tree, index)?;
        let key_xid = le64(key, 8)?;
        if le64(key, 0)? == oid && key_xid <= xid && selected.is_none_or(|item| key_xid > item.0) {
            selected = Some((key_xid, le32(value, 0)?, le32(value, 4)?, le64(value, 8)?));
        }
    }
    let (_, flags, object_size, paddr) = selected.ok_or(ENODATA)?;
    if flags & 1 != 0 || object_size != BLOCK as u32 {
        return Err(ENOTSUPP);
    }
    Ok(paddr)
}

struct VolumeIdentity {
    uuid: [u8; 16],
    group: [u8; 16],
    role: u16,
}

fn validate_data_volume(block: &OpenBlock, nx: &[u8], live: &Nx, wanted: &[u8; 16]) -> Result<()> {
    let mut volumes = KVec::new();
    for index in 0..100usize {
        let oid = le64(nx, 184 + index * 8)?;
        if oid == 0 {
            continue;
        }
        let paddr = omap_lookup(block, live.omap, oid, live.xid)?;
        let volume = read_object(block, paddr)?;
        if volume.get(32..36) != Some(b"APSB")
            || le64(&volume, 8)? != oid
            || le64(&volume, 16)? > live.xid
        {
            return Err(EINVAL);
        }
        let mut uuid = [0; 16];
        uuid.copy_from_slice(volume.get(0xf0..0x100).ok_or(EINVAL)?);
        let mut group = [0; 16];
        group.copy_from_slice(volume.get(0x3f0..0x400).ok_or(EINVAL)?);
        volumes.push(
            VolumeIdentity {
                uuid,
                group,
                role: le16(&volume, 0x3c4)?,
            },
            GFP_KERNEL,
        )?;
    }

    let mut selected_group = None;
    for volume in &volumes {
        if volume.uuid == *wanted {
            if volume.role != 0x40 || volume.group == [0; 16] || selected_group.is_some() {
                return Err(ENOTSUPP);
            }
            selected_group = Some(volume.group);
        }
    }
    let selected_group = selected_group.ok_or(ENODATA)?;
    let systems = volumes
        .iter()
        .filter(|volume| volume.role == 1 && volume.group == selected_group)
        .count();
    if systems != 1 {
        return Err(ENOTSUPP);
    }
    Ok(())
}

fn valid_checkpoint(
    block: &OpenBlock,
    superblock: &[u8],
    superblock_paddr: u64,
    base: &Nx,
) -> Result<()> {
    let index = u64::from(le32(superblock, 136)?);
    let len = u64::from(le32(superblock, 140)?);
    let count = u64::from(base.desc_count);
    if index >= count || len < 2 || len > count {
        return Err(EINVAL);
    }
    let last = base.desc_base + (index + len - 1) % count;
    if last != superblock_paddr {
        return Err(EINVAL);
    }
    let xid = le64(superblock, 16)?;
    for item in 0..len - 1 {
        let paddr = base.desc_base + (index + item) % count;
        let mapping = block.read_plain(paddr.checked_mul(BLOCK as u64).ok_or(EINVAL)?, BLOCK)?;
        checked_object(&mapping)?;
        if le32(&mapping, 24)? & 0xffff != 12 || le64(&mapping, 16)? != xid {
            return Err(EINVAL);
        }
        let flags = le32(&mapping, 32)?;
        if flags & 1 != u32::from(item == len - 2) {
            return Err(EINVAL);
        }
        let mappings = le32(&mapping, 36)? as usize;
        if mappings > (BLOCK - 40) / 40 {
            return Err(EINVAL);
        }
        for map_index in 0..mappings {
            let at = 40 + map_index * 40;
            if le32(&mapping, at + 8)? != BLOCK as u32 {
                return Err(ENOTSUPP);
            }
            let object_paddr = le64(&mapping, at + 32)?;
            let object =
                block.read_plain(object_paddr.checked_mul(BLOCK as u64).ok_or(EINVAL)?, BLOCK)?;
            checked_object(&object)?;
            if le64(&object, 8)? != le64(&mapping, at + 24)?
                || le64(&object, 16)? != xid
                || le32(&object, 24)? != le32(&mapping, at)?
            {
                return Err(EINVAL);
            }
        }
    }
    Ok(())
}

impl Drop for OpenBlock {
    fn drop(&mut self) {
        // SAFETY: this is the unique live handle returned by fvread_block_open.
        unsafe { fvread_block_close(self.handle) };
    }
}

struct Unlocked {
    block: OpenBlock,
    volume_uuid: [u8; 16],
    wrapped_vek: [u8; MAX_RECORD],
    wrapped_vek_len: usize,
    volume_key: FvKey,
}

impl Drop for Unlocked {
    fn drop(&mut self) {
        // SAFETY: these buffers remain live for the synchronous provider call.
        let ret = unsafe {
            apple_sep_fv_unload_class_keys_v2(
                self.volume_uuid.as_ptr(),
                self.wrapped_vek.as_ptr(),
                self.wrapped_vek_len,
            )
        };
        if ret != 0 {
            pr_warn!("apple-fvread: class-key unload failed: {}\n", ret);
        }
        self.wrapped_vek.fill(0);
    }
}

struct Session {
    unlocked: Option<Unlocked>,
}

impl Session {
    fn unlock(&mut self, request: UnlockRequest) -> Result<()> {
        if self.unlocked.is_some() {
            return Err(EBUSY);
        }
        if request.flags != FVREAD_UNLOCK_FV_OFF || request.volume_uuid == [0; 16] {
            return Err(EOPNOTSUPP);
        }
        let block = OpenBlock::open(request.device_fd)?;
        let base_data = block.read_plain(0, BLOCK)?;
        let base = Nx::parse(&base_data, block.size)?;

        let mut live_data: Option<KVec<u8>> = None;
        let mut live_xid = 0;
        for index in 0..base.desc_count {
            let paddr = base.desc_base + u64::from(index);
            let Some(offset) = paddr.checked_mul(BLOCK as u64) else {
                continue;
            };
            let Ok(candidate_data) = block.read_plain(offset, BLOCK) else {
                continue;
            };
            let Ok(candidate) = Nx::parse(&candidate_data, block.size) else {
                continue;
            };
            if candidate.same_container(&base)
                && candidate.xid > live_xid
                && valid_checkpoint(&block, &candidate_data, paddr, &base).is_ok()
            {
                live_xid = candidate.xid;
                live_data = Some(candidate_data);
            }
        }
        let live_data = live_data.ok_or(ENODATA)?;
        let live = Nx::parse(&live_data, block.size)?;
        validate_data_volume(&block, &live_data, &live, &request.volume_uuid)?;

        let mkb =
            SensitiveBuffer(block.read_plain(live.mkb_locker.offset()?, live.mkb_locker.bytes()?)?);
        checked_object(mkb.as_slice())?;
        let mkb_locker = Locker::parse(mkb.as_slice().get(32..).ok_or(EINVAL)?)?;
        let media = media_record(&mkb_locker, &live.uuid, KB_TAG_WRAPPING_M_KEY)?;
        let mut media_key = FvKey::zeroed();
        // SAFETY: pointers describe fixed live buffers and media_key is exclusive output.
        to_result(unsafe {
            apple_sep_fv_unwrap_media_key(
                media.wrapped.as_ptr(),
                media.wrapped.len(),
                media.protection_class,
                &mut media_key,
            )
        })?;

        let container = block.read_crypto(live.keylocker, &media_key)?;
        if le32(container.as_slice(), 24)? != 0x6b65_7973 {
            return Err(EBADMSG);
        }
        checked_object(container.as_slice())?;
        let locker = Locker::parse(container.as_slice().get(32..).ok_or(EINVAL)?)?;
        let records = volume_records(&locker, &request.volume_uuid, block.size / BLOCK as u64)?;
        // The FV-off path must not inspect or select any volume unlock record.
        let volume_media = media_record(&mkb_locker, &request.volume_uuid, KB_TAG_VOLUME_M_KEY)?;
        let mut volume_media_key = FvKey::zeroed();
        // SAFETY: pointers describe fixed live buffers and volume_media_key is exclusive output.
        to_result(unsafe {
            apple_sep_fv_unwrap_media_key(
                volume_media.wrapped.as_ptr(),
                volume_media.wrapped.len(),
                volume_media.protection_class,
                &mut volume_media_key,
            )
        })?;
        let volume_bag = block.read_crypto(records.volume_keybag, &volume_media_key)?;
        if le32(volume_bag.as_slice(), 24)? != 0x7265_6373 {
            return Err(EBADMSG);
        }
        checked_object(volume_bag.as_slice())?;
        let _volume_locker = Locker::parse(volume_bag.as_slice().get(32..).ok_or(EINVAL)?)?;

        let mut volume_key = FvKey::zeroed();
        // SAFETY: empty fields are represented by null/zero; all other buffers are live.
        to_result(unsafe {
            apple_sep_fv_unwrap_volume_key(
                core::ptr::null(),
                0,
                core::ptr::null(),
                0,
                records.wrapped_vek.as_ptr(),
                records.wrapped_vek.len(),
                &mut volume_key,
            )
        })?;
        to_result(unsafe {
            apple_sep_fv_load_class_keys_v2(
                request.volume_uuid.as_ptr(),
                core::ptr::null(),
                0,
                core::ptr::null(),
                0,
                records.wrapped_vek.as_ptr(),
                records.wrapped_vek.len(),
            )
        })?;

        let mut wrapped_vek = [0; MAX_RECORD];
        wrapped_vek[..records.wrapped_vek.len()].copy_from_slice(records.wrapped_vek);
        self.unlocked = Some(Unlocked {
            block,
            volume_uuid: request.volume_uuid,
            wrapped_vek,
            wrapped_vek_len: records.wrapped_vek.len(),
            volume_key,
        });
        Ok(())
    }

    fn read(&self, request: ReadRequest) -> Result<SensitiveBuffer> {
        let unlocked = self.unlocked.as_ref().ok_or(ENOKEY)?;
        self.read_with_key(request, &unlocked.volume_key)
    }

    fn read_file(&self, request: ReadFileRequest) -> Result<SensitiveBuffer> {
        let unlocked = self.unlocked.as_ref().ok_or(ENOKEY)?;
        let ekwk_len = request.wrapped_ekwk_len as usize;
        let ek_len = request.wrapped_ek_len as usize;
        if request.reserved != 0
            || ekwk_len == 0
            || ekwk_len > MAX_FILE_KEY_RECORD
            || ek_len == 0
            || ek_len > MAX_FILE_KEY_RECORD
        {
            return Err(EINVAL);
        }
        let mut file_key = FvKey::zeroed();
        // SAFETY: all slices and the exclusive output remain live for the call.
        to_result(unsafe {
            apple_sep_fv_unwrap_file_key(
                unlocked.volume_uuid.as_ptr(),
                request.protection_class,
                request.wrapped_ekwk.as_ptr(),
                ekwk_len,
                request.wrapped_ek.as_ptr(),
                ek_len,
                &mut file_key,
            )
        })?;
        self.read_with_key(request.io, &file_key)
    }

    fn info(&self) -> Result<Info> {
        let unlocked = self.unlocked.as_ref().ok_or(ENOKEY)?;
        Ok(Info {
            container_bytes: unlocked.block.size,
            block_size: BLOCK as u32,
            flags: FVREAD_UNLOCK_FV_OFF,
            volume_uuid: unlocked.volume_uuid,
        })
    }

    fn read_with_key(&self, request: ReadRequest, key: &FvKey) -> Result<SensitiveBuffer> {
        let unlocked = self.unlocked.as_ref().ok_or(ENOKEY)?;
        if request.reserved != 0
            || request.block_count == 0
            || u64::from(request.block_count) > MAX_RANGE_BLOCKS
        {
            return Err(EINVAL);
        }
        let range = Range {
            start: request.physical_block,
            blocks: u64::from(request.block_count),
        };
        let len = range.bytes()?;
        let mut bytes = KVec::new();
        bytes.resize(len, 0, GFP_KERNEL)?;
        let mut data = SensitiveBuffer(bytes);
        // SAFETY: the session owns all buffers and the call is synchronous.
        let ret = unsafe {
            fvread_block_read_crypto(
                unlocked.block.handle,
                range.offset()?,
                request.dun,
                key,
                data.as_mut_ptr().cast(),
                len,
            )
        };
        if ret < 0 {
            return Err(Error::from_errno((-ret) as c_int));
        }
        if ret as usize != len {
            return Err(EIO);
        }
        Ok(data)
    }
}

module! {
    type: FvreadModule,
    name: "apple_fvread",
    authors: ["Aurora Silicon"],
    description: "Read-only Apple-silicon APFS FileVault consumer",
    license: "GPL v2",
}

#[pin_data]
struct FvreadModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<FvreadFile>,
}

impl kernel::InPlaceModule for FvreadModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(MiscDeviceOptions {
                name: c"apple-fvread",
            }),
        })
    }
}

#[pin_data]
struct FvreadFile {
    #[pin]
    session: Mutex<Session>,
    _dev: ARef<kernel::device::Device>,
}

#[vtable]
impl MiscDevice for FvreadFile {
    type Ptr = Pin<KBox<Self>>;

    fn open(_file: &File, misc: &MiscDeviceRegistration<Self>) -> Result<Self::Ptr> {
        KBox::try_pin_init(
            try_pin_init!(Self {
                session <- new_mutex!(Session { unlocked: None }),
                _dev: ARef::from(misc.device()),
            }),
            GFP_KERNEL,
        )
    }

    fn ioctl(me: Pin<&Self>, _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        let ptr = UserPtr::from_addr(arg);
        match cmd {
            FVREAD_IOC_UNLOCK => {
                let mut reader =
                    UserSlice::new(ptr, core::mem::size_of::<UnlockRequest>()).reader();
                let request = reader.read::<UnlockRequest>()?;
                me.session.lock().unlock(request)?;
            }
            FVREAD_IOC_READ => {
                let mut reader = UserSlice::new(ptr, core::mem::size_of::<ReadRequest>()).reader();
                let request = reader.read::<ReadRequest>()?;
                let data = me.session.lock().read(request)?;
                let out = UserPtr::from_addr(usize::try_from(request.buffer).map_err(|_| EINVAL)?);
                UserSlice::new(out, data.len())
                    .writer()
                    .write_slice(data.as_slice())?;
            }
            FVREAD_IOC_READ_FILE => {
                let mut reader =
                    UserSlice::new(ptr, core::mem::size_of::<ReadFileRequest>()).reader();
                let request = reader.read::<ReadFileRequest>()?;
                let buffer = request.io.buffer;
                let data = me.session.lock().read_file(request)?;
                let out = UserPtr::from_addr(usize::try_from(buffer).map_err(|_| EINVAL)?);
                UserSlice::new(out, data.len())
                    .writer()
                    .write_slice(data.as_slice())?;
            }
            FVREAD_IOC_GET_INFO => {
                let info = me.session.lock().info()?;
                UserSlice::new(ptr, core::mem::size_of::<Info>())
                    .writer()
                    .write::<Info>(&info)?;
            }
            _ => return Err(ENOTTY),
        }
        Ok(0)
    }
}

#[cfg(CONFIG_APPLE_FVREAD_KUNIT_TEST)]
#[kunit_tests(apple_fvread_keybag)]
mod tests {
    use super::*;

    fn entry(buf: &mut [u8], off: usize, uuid: &[u8; 16], tag: u16, payload: &[u8]) -> usize {
        buf[off..off + 16].copy_from_slice(uuid);
        buf[off + 16..off + 18].copy_from_slice(&tag.to_le_bytes());
        buf[off + 18..off + 20].copy_from_slice(&(payload.len() as u16).to_le_bytes());
        buf[off + 24..off + 24 + payload.len()].copy_from_slice(payload);
        (off + 24 + payload.len() + 15) & !15
    }

    fn header(buf: &mut [u8], count: u16, end: usize) {
        buf[0..2].copy_from_slice(&KB_VERSION.to_le_bytes());
        buf[2..4].copy_from_slice(&count.to_le_bytes());
        buf[4..8].copy_from_slice(&((end - 16) as u32).to_le_bytes());
    }

    #[test]
    fn parses_media_record_and_class() -> Result {
        let uuid = [0x11; 16];
        let mut state = [0u8; 60];
        state[0..2].copy_from_slice(&WRAPPED_STATE_MAJOR.to_le_bytes());
        state[2..4].copy_from_slice(&WRAPPED_STATE_MINOR.to_le_bytes());
        state[8..12].copy_from_slice(&4u32.to_le_bytes());
        state[18..20].copy_from_slice(&(WRAPPED_MEDIA_KEY_LEN as u16).to_le_bytes());
        state[20..].fill(0xa5);
        let mut bag = [0u8; 128];
        let end = entry(&mut bag, 16, &uuid, KB_TAG_WRAPPING_M_KEY, &state);
        header(&mut bag, 1, end);
        let locker = Locker::parse(&bag[..end])?;
        let record = media_record(&locker, &uuid, KB_TAG_WRAPPING_M_KEY)?;
        assert_eq!(record.protection_class, 4);
        assert_eq!(record.wrapped, [0xa5; WRAPPED_MEDIA_KEY_LEN]);
        Ok(())
    }

    #[test]
    fn parses_volume_records() -> Result {
        let uuid = [0x22; 16];
        let vek = [0x5a; 80];
        let mut prange = [0u8; 16];
        prange[..8].copy_from_slice(&9u64.to_le_bytes());
        prange[8..].copy_from_slice(&2u64.to_le_bytes());
        let mut bag = [0u8; 256];
        let off = entry(&mut bag, 16, &uuid, KB_TAG_VOLUME_KEY, &vek);
        let end = entry(&mut bag, off, &uuid, KB_TAG_VOLUME_UNLOCK_RECORDS, &prange);
        header(&mut bag, 2, end);
        let locker = Locker::parse(&bag[..end])?;
        let records = volume_records(&locker, &uuid, 100)?;
        assert_eq!(records.wrapped_vek, vek);
        assert_eq!(records.volume_keybag.start, 9);
        assert_eq!(records.volume_keybag.blocks, 2);
        Ok(())
    }

    #[test]
    fn refuses_unknown_versions_and_truncation() -> Result {
        let mut bag = [0u8; 64];
        bag[0..2].copy_from_slice(&3u16.to_le_bytes());
        assert!(Locker::parse(&bag).is_err());
        bag[0..2].copy_from_slice(&KB_VERSION.to_le_bytes());
        bag[2..4].copy_from_slice(&1u16.to_le_bytes());
        bag[4..8].copy_from_slice(&8u32.to_le_bytes());
        assert!(Locker::parse(&bag[..24]).is_err());
        Ok(())
    }

    #[test]
    fn refuses_unknown_media_state_version() -> Result {
        let uuid = [0x33; 16];
        let mut state = [0u8; 60];
        state[0..2].copy_from_slice(&6u16.to_le_bytes());
        state[8..12].copy_from_slice(&4u32.to_le_bytes());
        state[18..20].copy_from_slice(&40u16.to_le_bytes());
        let mut bag = [0u8; 128];
        let end = entry(&mut bag, 16, &uuid, KB_TAG_WRAPPING_M_KEY, &state);
        header(&mut bag, 1, end);
        let locker = Locker::parse(&bag[..end])?;
        assert!(media_record(&locker, &uuid, KB_TAG_WRAPPING_M_KEY).is_err());
        Ok(())
    }

    #[test]
    fn refuses_duplicate_volume_keys() -> Result {
        let uuid = [0x44; 16];
        let vek = [0x11; 32];
        let mut bag = [0u8; 160];
        let off = entry(&mut bag, 16, &uuid, KB_TAG_VOLUME_KEY, &vek);
        let end = entry(&mut bag, off, &uuid, KB_TAG_VOLUME_KEY, &vek);
        header(&mut bag, 2, end);
        let locker = Locker::parse(&bag[..end])?;
        assert!(locker.one(&uuid, KB_TAG_VOLUME_KEY).is_err());
        Ok(())
    }
}
