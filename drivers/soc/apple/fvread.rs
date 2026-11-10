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
const UUID_SIZE: usize = 16;
const MAX_RANGE_BLOCKS: u64 = 16;
const MAX_RECORD: usize = 512;
const MOD: u64 = 0xffff_ffff;

// APFS object header (obj_phys_t).
const OBJ_CKSUM_OFF: usize = 0;
const OBJ_CKSUM_SIZE: usize = 8;
const OBJ_OID_OFF: usize = 8;
const OBJ_XID_OFF: usize = 16;
const OBJ_TYPE_OFF: usize = 24;
const OBJ_SUBTYPE_OFF: usize = 28;
const OBJ_PHYS_SIZE: usize = 32;
const OBJ_TYPE_MASK: u32 = 0xffff;
const OBJECT_TYPE_NX_SUPERBLOCK: u32 = 1;
const OBJECT_TYPE_BTREE: u32 = 2;
const OBJECT_TYPE_BTREE_NODE: u32 = 3;
const OBJECT_TYPE_OMAP: u32 = 11;
const OBJECT_TYPE_CHECKPOINT_MAP: u32 = 12;
const APFS_KEYBAG_OBJ: u32 = 0x6b65_7973;

// nx_superblock_t fields used by the read-only checkpoint walker.
const NX_MAGIC_OFF: usize = 0x20;
const NX_BLOCK_SIZE_OFF: usize = 0x24;
const NX_BLOCK_COUNT_OFF: usize = 0x28;
const NX_ROCOMPAT_FEATURES_OFF: usize = 0x38;
const NX_INCOMPAT_FEATURES_OFF: usize = 0x40;
const NX_UUID_OFF: usize = 0x48;
const NX_XP_DESC_BLOCKS_OFF: usize = 0x68;
const NX_XP_DESC_BASE_OFF: usize = 0x70;
const NX_XP_DESC_INDEX_OFF: usize = 0x88;
const NX_XP_DESC_LEN_OFF: usize = 0x8c;
const NX_OMAP_OID_OFF: usize = 0xa0;
const NX_FS_OID_OFF: usize = 0xb8;
const NX_MAX_FILE_SYSTEMS: usize = 100;
const NX_KEYLOCKER_OFF: usize = 0x510;
const NX_MKB_LOCKER_OFF: usize = 0x570;
const NX_INCOMPAT_VERSION2: u64 = 2;

// apfs_superblock_t fields used to identify the requested Data volume.
const APFS_MAGIC_OFF: usize = 0x20;
const APFS_VOL_UUID_OFF: usize = 0xf0;
const APFS_VOL_ROLE_OFF: usize = 0x3c4;
const APFS_VOLUME_GROUP_ID_OFF: usize = 0x3f0;
const APFS_VOL_ROLE_SYSTEM: u16 = 0x0001;
const APFS_VOL_ROLE_DATA: u16 = 0x0040;

// omap_phys_t, btree_node_phys_t and fixed omap key/value fields.
const OMAP_TREE_OID_OFF: usize = 0x30;
const BTNODE_FLAGS_OFF: usize = 0x20;
const BTNODE_LEVEL_OFF: usize = 0x22;
const BTNODE_NKEYS_OFF: usize = 0x24;
const BTNODE_TABLE_SPACE_OFF_OFF: usize = 0x28;
const BTNODE_TABLE_SPACE_LEN_OFF: usize = 0x2a;
const BTNODE_DATA_OFF: usize = 0x38;
const BTNODE_ROOT: u16 = 0x0001;
const BTNODE_LEAF: u16 = 0x0002;
const BTNODE_FIXED_KV_SIZE: u16 = 0x0004;
const BTREE_INFO_SIZE: usize = 40;
const BTREE_INFO_FLAGS_OFF: usize = BLOCK - BTREE_INFO_SIZE;
const BTREE_INFO_NODE_SIZE_OFF: usize = BTREE_INFO_FLAGS_OFF + 4;
const BTREE_INFO_KEY_SIZE_OFF: usize = BTREE_INFO_FLAGS_OFF + 8;
const BTREE_INFO_VALUE_SIZE_OFF: usize = BTREE_INFO_FLAGS_OFF + 12;
const BTREE_PHYSICAL: u32 = 0x0000_0010;
const OMAP_KEY_SIZE: usize = 16;
const OMAP_VALUE_SIZE: usize = 16;
const OMAP_INDEX_VALUE_SIZE: usize = 8;
const OMAP_KEY_OID_OFF: usize = 0;
const OMAP_KEY_XID_OFF: usize = 8;
const OMAP_VALUE_FLAGS_OFF: usize = 0;
const OMAP_VALUE_SIZE_OFF: usize = 4;
const OMAP_VALUE_PADDR_OFF: usize = 8;
const OMAP_INDEX_CHILD_OFF: usize = 0;
const KVOFF_VALUE_OFF: usize = 2;
const OMAP_VAL_DELETED: u32 = 0x0000_0001;
const MAX_OMAP_DEPTH: u16 = 64;

// checkpoint_map_phys_t and checkpoint_mapping_t fields.
const CPM_FLAGS_OFF: usize = 0x20;
const CPM_COUNT_OFF: usize = 0x24;
const CPM_MAPS_OFF: usize = 0x28;
const CPM_MAPPING_SIZE: usize = 40;
const CPM_MAP_TYPE_OFF: usize = 0;
const CPM_MAP_SIZE_OFF: usize = 8;
const CPM_MAP_OID_OFF: usize = 24;
const CPM_MAP_PADDR_OFF: usize = 32;
const CHECKPOINT_MAP_LAST: u32 = 0x0000_0001;

// prange_t.
const PRANGE_START_OFF: usize = 0;
const PRANGE_BLOCK_COUNT_OFF: usize = 8;

// APFS keybag locker and entry fields.
const KB_VERSION: u16 = 2;
const KB_LOCKER_VERSION_OFF: usize = 0;
const KB_LOCKER_NKEYS_OFF: usize = 2;
const KB_LOCKER_NBYTES_OFF: usize = 4;
const KB_LOCKER_HEADER_SIZE: usize = 16;
const KB_ENTRY_UUID_OFF: usize = 0;
const KB_ENTRY_TAG_OFF: usize = 16;
const KB_ENTRY_KEYLEN_OFF: usize = 18;
const KB_ENTRY_HEADER_SIZE: usize = 24;
#[allow(dead_code)]
const KB_TAG_UNKNOWN: u16 = 0;
#[allow(dead_code)]
const KB_TAG_MEDIA_KEY: u16 = 1;
const KB_TAG_VOLUME_KEY: u16 = 2;
#[allow(dead_code)]
const KB_TAG_VOLUME_UNLOCK_RECORDS: u16 = 3;
#[allow(dead_code)]
const KB_TAG_VOLUME_PASSPHRASE_HINT: u16 = 4;
#[allow(dead_code)]
const KB_TAG_WRAPPING_M_KEY: u16 = 5;
#[allow(dead_code)]
const KB_TAG_VOLUME_M_KEY: u16 = 6;
const KB_TAG_VOLUME_DEVICE_KEY: u16 = 7;
#[allow(dead_code)]
const KB_TAG_RESERVED_F8: u16 = 0xf8;
const WRAPPED_MEDIA_KEY_LEN: usize = 40;
const PROTECTION_CLASS_M: u32 = 14;
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
    if data.len() < OBJ_PHYS_SIZE || data.len() % 4 != 0 {
        return Err(EINVAL);
    }
    let mut s1 = 0u64;
    let mut s2 = 0u64;
    for word in data[OBJ_CKSUM_SIZE..].chunks_exact(4) {
        let value = u64::from(u32::from_le_bytes(word.try_into().map_err(|_| EINVAL)?));
        s1 = (s1 + value) % MOD;
        s2 = (s2 + s1) % MOD;
    }
    let c1 = (MOD - (s1 + s2) % MOD) % MOD;
    let c2 = (MOD - (s1 + c1) % MOD) % MOD;
    Ok((c2 << 32) | c1)
}

fn checked_object(data: &[u8]) -> Result<()> {
    if le64(data, OBJ_CKSUM_OFF)? != checksum(data)? {
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
            start: le64(data, off + PRANGE_START_OFF)?,
            blocks: le64(data, off + PRANGE_BLOCK_COUNT_OFF)?,
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
            || data.get(NX_MAGIC_OFF..NX_MAGIC_OFF + 4) != Some(b"NXSB")
            || le32(data, NX_BLOCK_SIZE_OFF)? != BLOCK as u32
            || le32(data, OBJ_TYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_NX_SUPERBLOCK
            || le64(data, NX_ROCOMPAT_FEATURES_OFF)? != 0
            || le64(data, NX_INCOMPAT_FEATURES_OFF)? != NX_INCOMPAT_VERSION2
        {
            return Err(ENOTSUPP);
        }
        let device_blocks = device_size / BLOCK as u64;
        let blocks = le64(data, NX_BLOCK_COUNT_OFF)?;
        if blocks == 0 || blocks > device_blocks {
            return Err(EINVAL);
        }
        let desc_count = le32(data, NX_XP_DESC_BLOCKS_OFF)?;
        let desc_base = le64(data, NX_XP_DESC_BASE_OFF)?;
        if desc_count == 0
            || desc_count & (1 << 31) != 0
            || desc_count > 1024
            || desc_base.checked_add(u64::from(desc_count)).ok_or(EINVAL)? > blocks
        {
            return Err(ENOTSUPP);
        }
        let mut uuid = [0; 16];
        uuid.copy_from_slice(
            data.get(NX_UUID_OFF..NX_UUID_OFF + UUID_SIZE)
                .ok_or(EINVAL)?,
        );
        Ok(Self {
            xid: le64(data, OBJ_XID_OFF)?,
            uuid,
            blocks,
            desc_base,
            desc_count,
            omap: le64(data, NX_OMAP_OID_OFF)?,
            keylocker: Range::from(data, NX_KEYLOCKER_OFF, blocks)?,
            mkb_locker: Range::from(data, NX_MKB_LOCKER_OFF, blocks)?,
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
    let flags = le16(node, BTNODE_FLAGS_OFF)?;
    let level = le16(node, BTNODE_LEVEL_OFF)?;
    if node.len() != BLOCK || flags & BTNODE_FIXED_KV_SIZE == 0 {
        return Err(ENOTSUPP);
    }
    if (level == 0) != (flags & BTNODE_LEAF != 0) {
        return Err(EINVAL);
    }
    let count = le32(node, BTNODE_NKEYS_OFF)? as usize;
    if index >= count || count > 512 {
        return Err(EINVAL);
    }
    let table_off = le16(node, BTNODE_TABLE_SPACE_OFF_OFF)? as usize;
    if table_off != 0 {
        return Err(ENOTSUPP);
    }
    let toc = BTNODE_DATA_OFF.checked_add(table_off).ok_or(EINVAL)?;
    let table_len = le16(node, BTNODE_TABLE_SPACE_LEN_OFF)? as usize;
    let key_start = toc.checked_add(table_len).ok_or(EINVAL)?;
    let value_end = BLOCK
        - if flags & BTNODE_ROOT != 0 {
            BTREE_INFO_SIZE
        } else {
            0
        };
    if key_start > value_end || toc.checked_add(count * 4).ok_or(EINVAL)? > key_start {
        return Err(EINVAL);
    }
    let item = toc.checked_add(index * 4).ok_or(EINVAL)?;
    let key_offset = le16(node, item)? as usize;
    let value_offset = le16(node, item + KVOFF_VALUE_OFF)? as usize;
    let key_start_at = key_start.checked_add(key_offset).ok_or(EINVAL)?;
    let value_start_at = value_end.checked_sub(value_offset).ok_or(EINVAL)?;
    let value_size = if level == 0 {
        OMAP_VALUE_SIZE
    } else {
        OMAP_INDEX_VALUE_SIZE
    };
    let key_end = key_start_at.checked_add(OMAP_KEY_SIZE).ok_or(EINVAL)?;
    let value_end_at = value_start_at.checked_add(value_size).ok_or(EINVAL)?;
    if key_start_at < key_start
        || key_end > value_end
        || value_start_at < key_start
        || value_end_at > value_end
        || key_end > value_start_at
    {
        return Err(EINVAL);
    }
    Ok((
        &node[key_start_at..key_end],
        &node[value_start_at..value_end_at],
    ))
}

fn compare_omap_key(key: &[u8], oid: u64, xid: u64) -> Result<core::cmp::Ordering> {
    let key_oid = le64(key, OMAP_KEY_OID_OFF)?;
    let key_xid = le64(key, OMAP_KEY_XID_OFF)?;
    Ok(key_oid.cmp(&oid).then_with(|| key_xid.cmp(&xid)))
}

fn omap_node_entry_le(node: &[u8], oid: u64, xid: u64) -> Result<(&[u8], &[u8])> {
    let mut selected = None;
    let mut previous = None;
    for index in 0..le32(node, BTNODE_NKEYS_OFF)? as usize {
        let (key, value) = node_entry(node, index)?;
        let current = (le64(key, OMAP_KEY_OID_OFF)?, le64(key, OMAP_KEY_XID_OFF)?);
        if previous.is_some_and(|item| item >= current) {
            return Err(EINVAL);
        }
        previous = Some(current);
        if compare_omap_key(key, oid, xid)? != core::cmp::Ordering::Greater {
            selected = Some((key, value));
        }
    }
    selected.ok_or(ENODATA)
}

fn validate_omap_root(node: &[u8]) -> Result<u16> {
    let flags = le16(node, BTNODE_FLAGS_OFF)?;
    let level = le16(node, BTNODE_LEVEL_OFF)?;
    if flags & BTNODE_ROOT == 0
        || le32(node, BTREE_INFO_NODE_SIZE_OFF)? != BLOCK as u32
        || le32(node, BTREE_INFO_KEY_SIZE_OFF)? != OMAP_KEY_SIZE as u32
        || le32(node, BTREE_INFO_VALUE_SIZE_OFF)? != OMAP_VALUE_SIZE as u32
        || le32(node, BTREE_INFO_FLAGS_OFF)? & BTREE_PHYSICAL == 0
        || level > MAX_OMAP_DEPTH
    {
        return Err(ENOTSUPP);
    }
    Ok(level)
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
        if le16(data, KB_LOCKER_VERSION_OFF)? != KB_VERSION {
            return Err(EPROTONOSUPPORT);
        }
        let count = le16(data, KB_LOCKER_NKEYS_OFF)? as usize;
        let nbytes = le32(data, KB_LOCKER_NBYTES_OFF)? as usize;
        if count > 256 || nbytes > data.len().saturating_sub(KB_LOCKER_HEADER_SIZE) {
            return Err(EINVAL);
        }
        let end = KB_LOCKER_HEADER_SIZE.checked_add(nbytes).ok_or(EINVAL)?;
        let mut entries = KVec::new();
        let mut off = KB_LOCKER_HEADER_SIZE;
        for _ in 0..count {
            let header_end = off.checked_add(KB_ENTRY_HEADER_SIZE).ok_or(EINVAL)?;
            if header_end > end {
                return Err(EINVAL);
            }
            let keylen = le16(data, off + KB_ENTRY_KEYLEN_OFF)? as usize;
            if keylen > MAX_RECORD {
                return Err(E2BIG);
            }
            let data_end = header_end.checked_add(keylen).ok_or(EINVAL)?;
            if data_end > end {
                return Err(EINVAL);
            }
            let mut uuid = [0; 16];
            uuid.copy_from_slice(
                data.get(off + KB_ENTRY_UUID_OFF..off + KB_ENTRY_UUID_OFF + UUID_SIZE)
                    .ok_or(EINVAL)?,
            );
            entries.push(
                Entry {
                    uuid,
                    tag: le16(data, off + KB_ENTRY_TAG_OFF)?,
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

struct VolumeRecords<'a> {
    wrapped_vek: &'a [u8],
}

fn device_key_record<'a>(locker: &Locker<'a>, container_uuid: &[u8; 16]) -> Result<&'a [u8]> {
    let wrapped = locker.one(container_uuid, KB_TAG_VOLUME_DEVICE_KEY)?;
    if wrapped.len() != WRAPPED_MEDIA_KEY_LEN {
        return Err(EPROTONOSUPPORT);
    }
    Ok(wrapped)
}

fn volume_records<'a>(locker: &Locker<'a>, volume_uuid: &[u8; 16]) -> Result<VolumeRecords<'a>> {
    let wrapped_vek = locker.one(volume_uuid, KB_TAG_VOLUME_KEY)?;
    if wrapped_vek.is_empty() {
        return Err(EINVAL);
    }
    Ok(VolumeRecords { wrapped_vek })
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
    if paddr >= block.size / BLOCK as u64 {
        return Err(EINVAL);
    }
    let object = block.read_plain(paddr.checked_mul(BLOCK as u64).ok_or(EINVAL)?, BLOCK)?;
    checked_object(&object)?;
    Ok(object)
}

fn omap_lookup(block: &OpenBlock, omap_paddr: u64, oid: u64, xid: u64) -> Result<u64> {
    let omap = read_object(block, omap_paddr)?;
    if le64(&omap, OBJ_OID_OFF)? != omap_paddr
        || le32(&omap, OBJ_TYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_OMAP
    {
        return Err(EINVAL);
    }
    let mut node_paddr = le64(&omap, OMAP_TREE_OID_OFF)?;
    let mut node = read_object(block, node_paddr)?;
    if le64(&node, OBJ_OID_OFF)? != node_paddr
        || le32(&node, OBJ_TYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_BTREE
        || le32(&node, OBJ_SUBTYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_OMAP
    {
        return Err(EINVAL);
    }
    let mut expected_level = validate_omap_root(&node)?;

    loop {
        if le16(&node, BTNODE_LEVEL_OFF)? != expected_level {
            return Err(EINVAL);
        }
        let (key, value) = omap_node_entry_le(&node, oid, xid)?;
        if expected_level == 0 {
            if le64(key, OMAP_KEY_OID_OFF)? != oid {
                return Err(ENODATA);
            }
            let flags = le32(value, OMAP_VALUE_FLAGS_OFF)?;
            let object_size = le32(value, OMAP_VALUE_SIZE_OFF)?;
            let paddr = le64(value, OMAP_VALUE_PADDR_OFF)?;
            if flags & OMAP_VAL_DELETED != 0 || object_size != BLOCK as u32 {
                return Err(ENOTSUPP);
            }
            if paddr >= block.size / BLOCK as u64 {
                return Err(EINVAL);
            }
            return Ok(paddr);
        }

        node_paddr = le64(value, OMAP_INDEX_CHILD_OFF)?;
        let child = read_object(block, node_paddr)?;
        let child_flags = le16(&child, BTNODE_FLAGS_OFF)?;
        if le64(&child, OBJ_OID_OFF)? != node_paddr
            || le32(&child, OBJ_TYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_BTREE_NODE
            || le32(&child, OBJ_SUBTYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_OMAP
            || child_flags & BTNODE_ROOT != 0
        {
            return Err(EINVAL);
        }
        expected_level -= 1;
        node = child;
    }
}

struct VolumeIdentity {
    uuid: [u8; 16],
    group: [u8; 16],
    role: u16,
}

fn validate_data_volume(block: &OpenBlock, nx: &[u8], live: &Nx, wanted: &[u8; 16]) -> Result<()> {
    let mut volumes = KVec::new();
    for index in 0..NX_MAX_FILE_SYSTEMS {
        let oid = le64(nx, NX_FS_OID_OFF + index * core::mem::size_of::<u64>())?;
        if oid == 0 {
            continue;
        }
        let paddr = omap_lookup(block, live.omap, oid, live.xid)?;
        let volume = read_object(block, paddr)?;
        if volume.get(APFS_MAGIC_OFF..APFS_MAGIC_OFF + 4) != Some(b"APSB")
            || le64(&volume, OBJ_OID_OFF)? != oid
            || le64(&volume, OBJ_XID_OFF)? > live.xid
        {
            return Err(EINVAL);
        }
        let mut uuid = [0; 16];
        uuid.copy_from_slice(
            volume
                .get(APFS_VOL_UUID_OFF..APFS_VOL_UUID_OFF + UUID_SIZE)
                .ok_or(EINVAL)?,
        );
        let mut group = [0; 16];
        group.copy_from_slice(
            volume
                .get(APFS_VOLUME_GROUP_ID_OFF..APFS_VOLUME_GROUP_ID_OFF + UUID_SIZE)
                .ok_or(EINVAL)?,
        );
        volumes.push(
            VolumeIdentity {
                uuid,
                group,
                role: le16(&volume, APFS_VOL_ROLE_OFF)?,
            },
            GFP_KERNEL,
        )?;
    }

    let mut selected_group = None;
    for volume in &volumes {
        if volume.uuid == *wanted {
            if volume.role != APFS_VOL_ROLE_DATA
                || volume.group == [0; 16]
                || selected_group.is_some()
            {
                return Err(ENOTSUPP);
            }
            selected_group = Some(volume.group);
        }
    }
    let selected_group = selected_group.ok_or(ENODATA)?;
    let systems = volumes
        .iter()
        .filter(|volume| volume.role == APFS_VOL_ROLE_SYSTEM && volume.group == selected_group)
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
    let index = u64::from(le32(superblock, NX_XP_DESC_INDEX_OFF)?);
    let len = u64::from(le32(superblock, NX_XP_DESC_LEN_OFF)?);
    let count = u64::from(base.desc_count);
    if index >= count || len < 2 || len > count {
        return Err(EINVAL);
    }
    let last = base.desc_base + (index + len - 1) % count;
    if last != superblock_paddr {
        return Err(EINVAL);
    }
    let xid = le64(superblock, OBJ_XID_OFF)?;
    for item in 0..len - 1 {
        let paddr = base.desc_base + (index + item) % count;
        let mapping = block.read_plain(paddr.checked_mul(BLOCK as u64).ok_or(EINVAL)?, BLOCK)?;
        checked_object(&mapping)?;
        if le32(&mapping, OBJ_TYPE_OFF)? & OBJ_TYPE_MASK != OBJECT_TYPE_CHECKPOINT_MAP
            || le64(&mapping, OBJ_XID_OFF)? != xid
        {
            return Err(EINVAL);
        }
        let flags = le32(&mapping, CPM_FLAGS_OFF)?;
        if flags & CHECKPOINT_MAP_LAST != u32::from(item == len - 2) {
            return Err(EINVAL);
        }
        let mappings = le32(&mapping, CPM_COUNT_OFF)? as usize;
        if mappings > (BLOCK - CPM_MAPS_OFF) / CPM_MAPPING_SIZE {
            return Err(EINVAL);
        }
        for map_index in 0..mappings {
            let at = CPM_MAPS_OFF + map_index * CPM_MAPPING_SIZE;
            if le32(&mapping, at + CPM_MAP_SIZE_OFF)? != BLOCK as u32 {
                return Err(ENOTSUPP);
            }
            let object_paddr = le64(&mapping, at + CPM_MAP_PADDR_OFF)?;
            let object =
                block.read_plain(object_paddr.checked_mul(BLOCK as u64).ok_or(EINVAL)?, BLOCK)?;
            checked_object(&object)?;
            if le64(&object, OBJ_OID_OFF)? != le64(&mapping, at + CPM_MAP_OID_OFF)?
                || le64(&object, OBJ_XID_OFF)? != xid
                || le32(&object, OBJ_TYPE_OFF)? != le32(&mapping, at + CPM_MAP_TYPE_OFF)?
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
        let mkb_locker = Locker::parse(mkb.as_slice().get(OBJ_PHYS_SIZE..).ok_or(EINVAL)?)?;
        let device_key = device_key_record(&mkb_locker, &live.uuid)?;
        let mut media_key = FvKey::zeroed();
        // Tag 7 is the class-M device wrapping key on hardware-encrypted APFS.
        // SAFETY: pointers describe fixed live buffers and media_key is exclusive output.
        to_result(unsafe {
            apple_sep_fv_unwrap_media_key(
                device_key.as_ptr(),
                device_key.len(),
                PROTECTION_CLASS_M,
                &mut media_key,
            )
        })?;

        let container = block.read_crypto(live.keylocker, &media_key)?;
        if le32(container.as_slice(), OBJ_TYPE_OFF)? != APFS_KEYBAG_OBJ {
            return Err(EBADMSG);
        }
        checked_object(container.as_slice())?;
        let locker = Locker::parse(container.as_slice().get(OBJ_PHYS_SIZE..).ok_or(EINVAL)?)?;
        let records = volume_records(&locker, &request.volume_uuid)?;
        // With FileVault off, the VEK is UID-only.  There is no user KEK or
        // volume-keybag record to select: empty secret/KEK fields tell SEP to
        // follow that hardware-bound path for the container's tag-2 VEK.

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
        buf[off + KB_ENTRY_UUID_OFF..off + KB_ENTRY_UUID_OFF + uuid.len()].copy_from_slice(uuid);
        buf[off + KB_ENTRY_TAG_OFF..off + KB_ENTRY_TAG_OFF + 2].copy_from_slice(&tag.to_le_bytes());
        buf[off + KB_ENTRY_KEYLEN_OFF..off + KB_ENTRY_KEYLEN_OFF + 2]
            .copy_from_slice(&(payload.len() as u16).to_le_bytes());
        let data = off + KB_ENTRY_HEADER_SIZE;
        buf[data..data + payload.len()].copy_from_slice(payload);
        (data + payload.len() + 15) & !15
    }

    fn header(buf: &mut [u8], count: u16, end: usize) {
        buf[KB_LOCKER_VERSION_OFF..KB_LOCKER_VERSION_OFF + 2]
            .copy_from_slice(&KB_VERSION.to_le_bytes());
        buf[KB_LOCKER_NKEYS_OFF..KB_LOCKER_NKEYS_OFF + 2].copy_from_slice(&count.to_le_bytes());
        buf[KB_LOCKER_NBYTES_OFF..KB_LOCKER_NBYTES_OFF + 4]
            .copy_from_slice(&((end - KB_LOCKER_HEADER_SIZE) as u32).to_le_bytes());
    }

    fn omap_node(entries: &[(u64, u64, u64)], level: u16, root: bool) -> [u8; BLOCK] {
        let mut node = [0u8; BLOCK];
        let mut flags = BTNODE_FIXED_KV_SIZE;
        if level == 0 {
            flags |= BTNODE_LEAF;
        }
        if root {
            flags |= BTNODE_ROOT;
        }
        node[BTNODE_FLAGS_OFF..BTNODE_FLAGS_OFF + 2].copy_from_slice(&flags.to_le_bytes());
        node[BTNODE_LEVEL_OFF..BTNODE_LEVEL_OFF + 2].copy_from_slice(&level.to_le_bytes());
        node[BTNODE_NKEYS_OFF..BTNODE_NKEYS_OFF + 4]
            .copy_from_slice(&(entries.len() as u32).to_le_bytes());
        let table_len = entries.len() * 4;
        node[BTNODE_TABLE_SPACE_LEN_OFF..BTNODE_TABLE_SPACE_LEN_OFF + 2]
            .copy_from_slice(&(table_len as u16).to_le_bytes());
        let key_start = BTNODE_DATA_OFF + table_len;
        let value_end = BLOCK - if root { BTREE_INFO_SIZE } else { 0 };
        let value_size = if level == 0 {
            OMAP_VALUE_SIZE
        } else {
            OMAP_INDEX_VALUE_SIZE
        };
        for (index, (oid, xid, payload)) in entries.iter().enumerate() {
            let item = BTNODE_DATA_OFF + index * 4;
            node[item..item + 2].copy_from_slice(&((index * OMAP_KEY_SIZE) as u16).to_le_bytes());
            node[item + KVOFF_VALUE_OFF..item + KVOFF_VALUE_OFF + 2]
                .copy_from_slice(&(((index + 1) * value_size) as u16).to_le_bytes());
            let key = key_start + index * OMAP_KEY_SIZE;
            node[key + OMAP_KEY_OID_OFF..key + OMAP_KEY_OID_OFF + 8]
                .copy_from_slice(&oid.to_le_bytes());
            node[key + OMAP_KEY_XID_OFF..key + OMAP_KEY_XID_OFF + 8]
                .copy_from_slice(&xid.to_le_bytes());
            let value = value_end - (index + 1) * value_size;
            if level == 0 {
                node[value + OMAP_VALUE_SIZE_OFF..value + OMAP_VALUE_SIZE_OFF + 4]
                    .copy_from_slice(&(BLOCK as u32).to_le_bytes());
                node[value + OMAP_VALUE_PADDR_OFF..value + OMAP_VALUE_PADDR_OFF + 8]
                    .copy_from_slice(&payload.to_le_bytes());
            } else {
                node[value + OMAP_INDEX_CHILD_OFF..value + OMAP_INDEX_CHILD_OFF + 8]
                    .copy_from_slice(&payload.to_le_bytes());
            }
        }
        if root {
            node[BTREE_INFO_FLAGS_OFF..BTREE_INFO_FLAGS_OFF + 4]
                .copy_from_slice(&BTREE_PHYSICAL.to_le_bytes());
            node[BTREE_INFO_NODE_SIZE_OFF..BTREE_INFO_NODE_SIZE_OFF + 4]
                .copy_from_slice(&(BLOCK as u32).to_le_bytes());
            node[BTREE_INFO_KEY_SIZE_OFF..BTREE_INFO_KEY_SIZE_OFF + 4]
                .copy_from_slice(&(OMAP_KEY_SIZE as u32).to_le_bytes());
            node[BTREE_INFO_VALUE_SIZE_OFF..BTREE_INFO_VALUE_SIZE_OFF + 4]
                .copy_from_slice(&(OMAP_VALUE_SIZE as u32).to_le_bytes());
        }
        node
    }

    #[test]
    fn parses_class_m_device_key() -> Result {
        let uuid = [0x11; 16];
        let wrapped = [0xa5; WRAPPED_MEDIA_KEY_LEN];
        let mut bag = [0u8; 128];
        let end = entry(
            &mut bag,
            KB_LOCKER_HEADER_SIZE,
            &uuid,
            KB_TAG_VOLUME_DEVICE_KEY,
            &wrapped,
        );
        header(&mut bag, 1, end);
        let locker = Locker::parse(&bag[..end])?;
        assert_eq!(device_key_record(&locker, &uuid)?, wrapped);
        Ok(())
    }

    #[test]
    fn parses_fv_off_wrapped_vek_without_volume_keybag() -> Result {
        let uuid = [0x22; 16];
        let vek = [0x5a; 80];
        let mut bag = [0u8; 256];
        let end = entry(
            &mut bag,
            KB_LOCKER_HEADER_SIZE,
            &uuid,
            KB_TAG_VOLUME_KEY,
            &vek,
        );
        header(&mut bag, 1, end);
        let locker = Locker::parse(&bag[..end])?;
        let records = volume_records(&locker, &uuid)?;
        assert_eq!(records.wrapped_vek, vek);
        Ok(())
    }

    #[test]
    fn selects_latest_omap_leaf_version() -> Result {
        let node = omap_node(
            &[(9, 9, 1), (10, 2, 22), (10, 6, 66), (10, 9, 99), (11, 0, 1)],
            0,
            true,
        );
        assert_eq!(validate_omap_root(&node)?, 0);
        let (key, value) = omap_node_entry_le(&node, 10, 7)?;
        assert_eq!(le64(key, OMAP_KEY_XID_OFF)?, 6);
        assert_eq!(le64(value, OMAP_VALUE_PADDR_OFF)?, 66);
        Ok(())
    }

    #[test]
    fn selects_multilevel_omap_child() -> Result {
        let node = omap_node(&[(0, 0, 100), (10, 5, 200), (20, 0, 300)], 1, true);
        assert_eq!(validate_omap_root(&node)?, 1);
        let (_, value) = omap_node_entry_le(&node, 10, 8)?;
        assert_eq!(le64(value, OMAP_INDEX_CHILD_OFF)?, 200);
        Ok(())
    }

    #[test]
    fn refuses_unknown_versions_and_truncation() -> Result {
        let mut bag = [0u8; 64];
        bag[KB_LOCKER_VERSION_OFF..KB_LOCKER_VERSION_OFF + 2].copy_from_slice(&3u16.to_le_bytes());
        assert!(Locker::parse(&bag).is_err());
        bag[KB_LOCKER_VERSION_OFF..KB_LOCKER_VERSION_OFF + 2]
            .copy_from_slice(&KB_VERSION.to_le_bytes());
        bag[KB_LOCKER_NKEYS_OFF..KB_LOCKER_NKEYS_OFF + 2].copy_from_slice(&1u16.to_le_bytes());
        bag[KB_LOCKER_NBYTES_OFF..KB_LOCKER_NBYTES_OFF + 4].copy_from_slice(&8u32.to_le_bytes());
        assert!(Locker::parse(&bag[..24]).is_err());
        Ok(())
    }

    #[test]
    fn refuses_wrong_device_key_length() -> Result {
        let uuid = [0x33; 16];
        let wrapped = [0u8; WRAPPED_MEDIA_KEY_LEN - 1];
        let mut bag = [0u8; 128];
        let end = entry(
            &mut bag,
            KB_LOCKER_HEADER_SIZE,
            &uuid,
            KB_TAG_VOLUME_DEVICE_KEY,
            &wrapped,
        );
        header(&mut bag, 1, end);
        let locker = Locker::parse(&bag[..end])?;
        assert!(device_key_record(&locker, &uuid).is_err());
        Ok(())
    }

    #[test]
    fn refuses_duplicate_volume_keys() -> Result {
        let uuid = [0x44; 16];
        let vek = [0x11; 32];
        let mut bag = [0u8; 160];
        let off = entry(
            &mut bag,
            KB_LOCKER_HEADER_SIZE,
            &uuid,
            KB_TAG_VOLUME_KEY,
            &vek,
        );
        let end = entry(&mut bag, off, &uuid, KB_TAG_VOLUME_KEY, &vek);
        header(&mut bag, 2, end);
        let locker = Locker::parse(&bag[..end])?;
        assert!(locker.one(&uuid, KB_TAG_VOLUME_KEY).is_err());
        Ok(())
    }
}
