// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Dj

//! Narrow, read-only APFS reader for small files in the iBoot System
//! Container.
//!
//! It reads only what the Touch ID factory records need: the newest
//! complete checkpoint, the container's and a volume's object maps, the
//! volume's file-system tree, and the contents of small regular files that
//! are neither encrypted nor compressed. Every object is checksummed and
//! every offset bounded. Any other layout fails closed with a [`Refusal`].
//! It never writes. `xart_apfs` keeps its own, narrower, rules for the
//! gigalocker.
//!
//! The module uses `core` alone, so the host tests build it unchanged.

use core::ops::DerefMut;

/// Why a factory-record lookup was refused. Each value names one cause for
/// the log line it gets.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Refusal {
    /// The container device could not be opened (not yet present).
    Unavailable,
    /// A read from the device failed.
    Io,
    /// No allocation for a block or a record.
    NoMemory,
    /// The device does not hold an APFS container.
    NotApfs,
    /// A layout this reader does not handle.
    Unsupported,
    /// A checksum, bound or cross-reference does not hold.
    Malformed,
    /// The volume or the record is encrypted.
    Encrypted,
    /// The record is compressed.
    Compressed,
    /// The volume, directory or record is not there.
    Absent,
    /// More than one candidate where exactly one is required.
    Ambiguous,
    /// The calibration and the root key name different sensors.
    Mismatched,
    /// The record is larger than any factory record.
    TooLarge,
    /// The container does not hold this boot's volume group.
    Foreign,
    /// This boot does not name its volume group.
    Unbound,
}

impl Refusal {
    /// A short, lower-case cause for the one log line a refusal gets.
    pub(crate) fn reason(self) -> &'static str {
        match self {
            Refusal::Unavailable => "the iBoot System Container is not available",
            Refusal::Io => "reading the iBoot System Container failed",
            Refusal::NoMemory => "out of memory",
            Refusal::NotApfs => "the iBoot System Container is not APFS",
            Refusal::Unsupported => "unsupported APFS layout",
            Refusal::Malformed => "malformed APFS metadata or record",
            Refusal::Encrypted => "the record is encrypted",
            Refusal::Compressed => "the record is compressed",
            Refusal::Absent => "no such record",
            Refusal::Ambiguous => "more than one candidate record",
            Refusal::Mismatched => "the calibration and the root key are for different sensors",
            Refusal::TooLarge => "the record is too large",
            Refusal::Foreign => "the iBoot System Container is not this boot's",
            Refusal::Unbound => "this boot does not name its volume group",
        }
    }
}

pub(crate) type Read<T> = core::result::Result<T, Refusal>;

/// What the reader reads from: the container's block device, or a host
/// test's image.
pub(crate) trait Media {
    /// An owned, zero-filled byte buffer.
    type Buf: DerefMut<Target = [u8]>;
    /// The size of the device in bytes.
    fn size(&self) -> Read<u64>;
    /// A zero-filled buffer of `len` bytes.
    fn buffer(&self, len: usize) -> Read<Self::Buf>;
    /// Fills `buf` from byte `offset` of the device.
    fn read(&self, offset: u64, buf: &mut [u8]) -> Read<()>;
}

/// The only block size this reader accepts, as every iBoot System Container
/// measured so far uses.
pub(crate) const BLOCK: usize = 4096;
const BLOCK_U64: u64 = BLOCK as u64;
const ROOT_INFO_SIZE: usize = 40;
const MOD: u64 = 0xffff_ffff;

const TYPE_MASK: u32 = 0xffff;
const STORAGE_MASK: u32 = 0xc000_0000;
const PHYSICAL: u32 = 0x4000_0000;
const TYPE_NX: u32 = 0x1;
const TYPE_BTREE: u32 = 0x2;
const TYPE_NODE: u32 = 0x3;
const TYPE_OMAP: u32 = 0xb;
const TYPE_CHECKPOINT_MAP: u32 = 0xc;
const TYPE_FS: u32 = 0xd;
const SUBTYPE_OMAP: u32 = 0xb;
const SUBTYPE_FSTREE: u32 = 0xe;

const NODE_ROOT: u16 = 0x1;
const NODE_LEAF: u16 = 0x2;
const NODE_FIXED: u16 = 0x4;
const NODE_HASHED: u16 = 0x8;
const NODE_NOHEADER: u16 = 0x10;

const OMAP_DELETED: u32 = 0x1;
const OMAP_ENCRYPTED: u32 = 0x4;

const MAX_VOLUMES: usize = 100;
const FS_UNENCRYPTED: u64 = 0x1;
const INCOMPAT_CASE_INSENSITIVE: u64 = 0x1;
const INCOMPAT_NORMALIZATION_INSENSITIVE: u64 = 0x8;

const KEY_TYPE_SHIFT: u32 = 60;
const OBJ_ID_MASK: u64 = (1 << KEY_TYPE_SHIFT) - 1;
const REC_INODE: u64 = 3;
const REC_FILE_EXTENT: u64 = 8;
const REC_DIR: u64 = 9;
const EXTENT_LEN_MASK: u64 = (1 << 56) - 1;

const S_IFMT: u16 = 0o170000;
const S_IFREG: u16 = 0o100000;
const UF_COMPRESSED: u32 = 0x20;
const INODE_IS_SPARSE: u64 = 0x200;
const XFIELD_DSTREAM: u8 = 8;
const INODE_XFIELDS: usize = 92;

/// The root directory's inode number.
pub(crate) const ROOT_DIR: u64 = 2;
/// Directory-entry types.
pub(crate) const DT_DIR: u16 = 4;
pub(crate) const DT_REG: u16 = 8;

/// Bounds on one tree walk. The largest tree read so far has 8 nodes.
const MAX_NODES: usize = 4096;
const MAX_DEPTH: u16 = 8;
const MAX_CHECKPOINT_BLOCKS: u64 = 1024;

fn le16(data: &[u8], off: usize) -> Read<u16> {
    let bytes = data.get(off..off.checked_add(2).ok_or(Refusal::Malformed)?);
    Ok(u16::from_le_bytes(
        bytes
            .ok_or(Refusal::Malformed)?
            .try_into()
            .map_err(|_| Refusal::Malformed)?,
    ))
}

fn le32(data: &[u8], off: usize) -> Read<u32> {
    let bytes = data.get(off..off.checked_add(4).ok_or(Refusal::Malformed)?);
    Ok(u32::from_le_bytes(
        bytes
            .ok_or(Refusal::Malformed)?
            .try_into()
            .map_err(|_| Refusal::Malformed)?,
    ))
}

fn le64(data: &[u8], off: usize) -> Read<u64> {
    let bytes = data.get(off..off.checked_add(8).ok_or(Refusal::Malformed)?);
    Ok(u64::from_le_bytes(
        bytes
            .ok_or(Refusal::Malformed)?
            .try_into()
            .map_err(|_| Refusal::Malformed)?,
    ))
}

/// APFS's Fletcher-64 over the little-endian words after the checksum.
pub(crate) fn checksum(data: &[u8]) -> u64 {
    let mut s1 = 0u64;
    let mut s2 = 0u64;
    for word in data[8..].chunks_exact(4) {
        let value = u64::from(u32::from_le_bytes([word[0], word[1], word[2], word[3]]));
        s1 = (s1 + value) % MOD;
        s2 = (s2 + s1) % MOD;
    }
    let c1 = (MOD - (s1 + s2) % MOD) % MOD;
    let c2 = (MOD - (s1 + c1) % MOD) % MOD;
    (c2 << 32) | c1
}

/// One parsed B-tree node, borrowed from its block.
struct Node<'a> {
    data: &'a [u8],
    fixed: bool,
    level: u16,
    count: usize,
    toc: usize,
    key_start: usize,
    val_end: usize,
}

impl<'a> Node<'a> {
    fn parse(data: &'a [u8], root: bool) -> Read<Node<'a>> {
        let flags = le16(data, 32)?;
        let level = le16(data, 34)?;
        if flags & (NODE_HASHED | NODE_NOHEADER) != 0 || level >= MAX_DEPTH {
            return Err(Refusal::Unsupported);
        }
        if (flags & NODE_ROOT != 0) != root || (flags & NODE_LEAF != 0) != (level == 0) {
            return Err(Refusal::Malformed);
        }
        let fixed = flags & NODE_FIXED != 0;
        let count = le32(data, 36)? as usize;
        let toc = 56 + le16(data, 40)? as usize;
        let key_start = toc + le16(data, 42)? as usize;
        let val_end = BLOCK - if root { ROOT_INFO_SIZE } else { 0 };
        let stride = if fixed { 4 } else { 8 };
        let table = count.checked_mul(stride).ok_or(Refusal::Malformed)?;
        if key_start > val_end || toc + table > key_start {
            return Err(Refusal::Malformed);
        }
        Ok(Node {
            data,
            fixed,
            level,
            count,
            toc,
            key_start,
            val_end,
        })
    }

    /// Entry `index` as (key, value). Fixed-size nodes are the object maps:
    /// 16-byte keys, 16-byte leaf values and 8-byte child addresses.
    fn entry(&self, index: usize) -> Read<(&'a [u8], &'a [u8])> {
        if index >= self.count {
            return Err(Refusal::Malformed);
        }
        let (ko, kl, vo, vl) = if self.fixed {
            let at = self.toc + index * 4;
            let vl = if self.level == 0 { 16 } else { 8 };
            (
                le16(self.data, at)? as usize,
                16,
                le16(self.data, at + 2)? as usize,
                vl,
            )
        } else {
            let at = self.toc + index * 8;
            (
                le16(self.data, at)? as usize,
                le16(self.data, at + 2)? as usize,
                le16(self.data, at + 4)? as usize,
                le16(self.data, at + 6)? as usize,
            )
        };
        let ks = self.key_start + ko;
        let ke = ks + kl;
        let vs = self.val_end.checked_sub(vo).ok_or(Refusal::Malformed)?;
        let ve = vs + vl;
        if ke > self.val_end || vs < self.key_start || ve > self.val_end {
            return Err(Refusal::Malformed);
        }
        Ok((&self.data[ks..ke], &self.data[vs..ve]))
    }
}

/// A volume, resolved through the container's object map.
pub(crate) struct Volume<B> {
    sb: B,
    omap: u64,
    xid: u64,
    root: u64,
    physical: bool,
    hashed: bool,
}

/// A directory entry: the child's inode number and its type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Entry {
    pub(crate) id: u64,
    pub(crate) kind: u16,
}

/// An opened container: its newest complete checkpoint.
pub(crate) struct Container<'m, M: Media> {
    media: &'m M,
    blocks: u64,
    xid: u64,
    sb: M::Buf,
}

impl<'m, M: Media> Container<'m, M> {
    /// Reads the container superblock and selects the newest checkpoint whose
    /// every mapping and ephemeral object is present and current.
    pub(crate) fn open(media: &'m M) -> Read<Self> {
        let device = media.size()?;
        if device < BLOCK_U64 {
            return Err(Refusal::NotApfs);
        }
        let mut base = media.buffer(BLOCK)?;
        media.read(0, &mut base)?;
        if &base[32..36] != b"NXSB" {
            return Err(Refusal::NotApfs);
        }
        if le64(&base, 0)? != checksum(&base) {
            return Err(Refusal::Malformed);
        }
        if le32(&base, 36)? != BLOCK as u32 || le64(&base, 56)? != 0 || le64(&base, 64)? != 2 {
            return Err(Refusal::Unsupported);
        }
        let blocks = le64(&base, 40)?;
        if blocks == 0 || blocks > device / BLOCK_U64 {
            return Err(Refusal::Malformed);
        }
        let desc_raw = le32(&base, 104)?;
        if desc_raw & (1 << 31) != 0 {
            return Err(Refusal::Unsupported);
        }
        let desc_count = u64::from(desc_raw);
        let desc_base = le64(&base, 112)?;
        if desc_count == 0
            || desc_count > MAX_CHECKPOINT_BLOCKS
            || desc_base
                .checked_add(desc_count)
                .ok_or(Refusal::Malformed)?
                > blocks
        {
            return Err(Refusal::Malformed);
        }
        let mut this = Container {
            media,
            blocks,
            xid: 0,
            sb: base,
        };
        let mut best: Option<M::Buf> = None;
        for paddr in desc_base..desc_base + desc_count {
            let Ok(candidate) = this.object(paddr) else {
                continue;
            };
            let xid = le64(&candidate, 16)?;
            if &candidate[32..36] == b"NXSB"
                && le32(&candidate, 24)? & TYPE_MASK == TYPE_NX
                && le32(&candidate, 36)? == BLOCK as u32
                && le64(&candidate, 40)? == blocks
                && le64(&candidate, 56)? == 0
                && le64(&candidate, 64)? == 2
                && xid > this.xid
                && this
                    .complete(&candidate, paddr, desc_base, desc_count)
                    .is_ok()
            {
                this.xid = xid;
                best = Some(candidate);
            }
        }
        this.sb = best.ok_or(Refusal::Malformed)?;
        Ok(this)
    }

    /// Reads and checksums the object at block `paddr`.
    fn object(&self, paddr: u64) -> Read<M::Buf> {
        if paddr == 0 || paddr >= self.blocks {
            return Err(Refusal::Malformed);
        }
        let mut data = self.media.buffer(BLOCK)?;
        self.media.read(paddr * BLOCK_U64, &mut data)?;
        if le64(&data, 0)? != checksum(&data) {
            return Err(Refusal::Malformed);
        }
        Ok(data)
    }

    /// A checksummed superblock alone is not a complete checkpoint: its
    /// mapping blocks precede it in the descriptor ring, and each maps an
    /// ephemeral object of the same transaction.
    fn complete(&self, sb: &[u8], sb_paddr: u64, desc_base: u64, desc_count: u64) -> Read<()> {
        if le64(sb, 112)? != desc_base || u64::from(le32(sb, 104)?) != desc_count {
            return Err(Refusal::Unsupported);
        }
        let index = u64::from(le32(sb, 136)?);
        let len = u64::from(le32(sb, 140)?);
        if index >= desc_count || len < 2 || len > desc_count {
            return Err(Refusal::Malformed);
        }
        if desc_base + (index + len - 1) % desc_count != sb_paddr {
            return Err(Refusal::Malformed);
        }
        let xid = le64(sb, 16)?;
        for j in 0..len - 1 {
            let map = self.object(desc_base + (index + j) % desc_count)?;
            if le32(&map, 24)? & TYPE_MASK != TYPE_CHECKPOINT_MAP || le64(&map, 16)? != xid {
                return Err(Refusal::Malformed);
            }
            if le32(&map, 32)? & 1 != u32::from(j == len - 2) {
                return Err(Refusal::Malformed);
            }
            let count = le32(&map, 36)? as usize;
            if count > (BLOCK - 40) / 40 {
                return Err(Refusal::Malformed);
            }
            for i in 0..count {
                let at = 40 + i * 40;
                if le32(&map, at + 8)? != BLOCK as u32 {
                    return Err(Refusal::Unsupported);
                }
                let object = self.object(le64(&map, at + 32)?)?;
                if le64(&object, 8)? != le64(&map, at + 24)?
                    || le64(&object, 16)? != xid
                    || le32(&object, 24)? != le32(&map, at)?
                {
                    return Err(Refusal::Malformed);
                }
            }
        }
        Ok(())
    }

    /// The physical address of virtual object `oid` as of transaction `xid`
    /// in the object map at `omap_paddr`.
    fn resolve(&self, omap_paddr: u64, oid: u64, xid: u64) -> Read<u64> {
        let omap = self.object(omap_paddr)?;
        if le64(&omap, 8)? != omap_paddr
            || le32(&omap, 24)? != PHYSICAL | TYPE_OMAP
            || le32(&omap, 40)? != PHYSICAL | TYPE_BTREE
        {
            return Err(Refusal::Malformed);
        }
        let mut paddr = le64(&omap, 48)?;
        let mut parent: Option<u16> = None;
        loop {
            let block = self.object(paddr)?;
            let kind = if parent.is_none() {
                TYPE_BTREE
            } else {
                TYPE_NODE
            };
            if le64(&block, 8)? != paddr
                || le32(&block, 24)? != PHYSICAL | kind
                || le32(&block, 28)? != SUBTYPE_OMAP
            {
                return Err(Refusal::Malformed);
            }
            let node = Node::parse(&block, parent.is_none())?;
            if !node.fixed || parent.is_some_and(|level| node.level + 1 != level) {
                return Err(Refusal::Malformed);
            }
            let mut previous: Option<(u64, u64)> = None;
            if node.level == 0 {
                let mut found: Option<(u64, u32, u32, u64)> = None;
                for i in 0..node.count {
                    let (key, val) = node.entry(i)?;
                    let key = (le64(key, 0)?, le64(key, 8)?);
                    if previous.is_some_and(|p| p >= key) {
                        return Err(Refusal::Malformed);
                    }
                    previous = Some(key);
                    if key.0 == oid && key.1 <= xid {
                        found = Some((key.1, le32(val, 0)?, le32(val, 4)?, le64(val, 8)?));
                    }
                }
                let (_, flags, size, addr) = found.ok_or(Refusal::Absent)?;
                if flags & OMAP_ENCRYPTED != 0 {
                    return Err(Refusal::Encrypted);
                }
                if flags & OMAP_DELETED != 0 {
                    return Err(Refusal::Absent);
                }
                if size != BLOCK as u32 {
                    return Err(Refusal::Unsupported);
                }
                return Ok(addr);
            }
            let mut child: Option<u64> = None;
            for i in 0..node.count {
                let (key, val) = node.entry(i)?;
                let key = (le64(key, 0)?, le64(key, 8)?);
                if previous.is_some_and(|p| p >= key) {
                    return Err(Refusal::Malformed);
                }
                previous = Some(key);
                if key <= (oid, xid) {
                    child = Some(le64(val, 0)?);
                }
            }
            paddr = child.ok_or(Refusal::Absent)?;
            parent = Some(node.level);
        }
    }

    /// The one volume whose role is `role`, if it is unencrypted.
    pub(crate) fn volume(&self, role: u16) -> Read<Volume<M::Buf>> {
        let omap = le64(&self.sb, 160)?;
        let mut found: Option<Volume<M::Buf>> = None;
        for i in 0..MAX_VOLUMES {
            let oid = le64(&self.sb, 184 + i * 8)?;
            if oid == 0 {
                continue;
            }
            let sb = self.object(self.resolve(omap, oid, self.xid)?)?;
            if &sb[32..36] != b"APSB"
                || le64(&sb, 8)? != oid
                || le64(&sb, 16)? > self.xid
                || le32(&sb, 24)? != TYPE_FS
            {
                return Err(Refusal::Malformed);
            }
            if le16(&sb, 964)? != role {
                continue;
            }
            if found.is_some() {
                return Err(Refusal::Ambiguous);
            }
            if le64(&sb, 264)? & FS_UNENCRYPTED == 0 {
                return Err(Refusal::Encrypted);
            }
            let tree_type = le32(&sb, 116)?;
            if tree_type & TYPE_MASK != TYPE_BTREE || tree_type & STORAGE_MASK & !PHYSICAL != 0 {
                return Err(Refusal::Unsupported);
            }
            let physical = tree_type & PHYSICAL != 0;
            let vol_omap = le64(&sb, 128)?;
            let xid = le64(&sb, 16)?;
            let root_oid = le64(&sb, 136)?;
            let root = if physical {
                root_oid
            } else {
                self.resolve(vol_omap, root_oid, xid)?
            };
            let incompat = le64(&sb, 56)?;
            let hashed =
                incompat & (INCOMPAT_CASE_INSENSITIVE | INCOMPAT_NORMALIZATION_INSENSITIVE) != 0;
            found = Some(Volume {
                sb,
                omap: vol_omap,
                xid,
                root,
                physical,
                hashed,
            });
        }
        found.ok_or(Refusal::Absent)
    }

    /// Calls `visit` with every (key, value) in `vol`'s file-system tree, in
    /// tree order.
    fn scan(
        &self,
        vol: &Volume<M::Buf>,
        visit: &mut dyn FnMut(&[u8], &[u8]) -> Read<()>,
    ) -> Read<()> {
        let mut nodes = 0usize;
        let root_oid = le64(&vol.sb, 136)?;
        self.walk(vol, vol.root, root_oid, None, &mut nodes, visit)
    }

    fn walk(
        &self,
        vol: &Volume<M::Buf>,
        paddr: u64,
        oid: u64,
        parent: Option<u16>,
        nodes: &mut usize,
        visit: &mut dyn FnMut(&[u8], &[u8]) -> Read<()>,
    ) -> Read<()> {
        *nodes += 1;
        if *nodes > MAX_NODES {
            return Err(Refusal::Unsupported);
        }
        let block = self.object(paddr)?;
        let storage = if vol.physical { PHYSICAL } else { 0 };
        let kind = if parent.is_none() {
            TYPE_BTREE
        } else {
            TYPE_NODE
        };
        if le64(&block, 8)? != oid
            || le64(&block, 16)? > vol.xid
            || le32(&block, 24)? != storage | kind
            || le32(&block, 28)? != SUBTYPE_FSTREE
        {
            return Err(Refusal::Malformed);
        }
        let node = Node::parse(&block, parent.is_none())?;
        if node.fixed {
            return Err(Refusal::Unsupported);
        }
        if parent.is_some_and(|level| node.level + 1 != level) {
            return Err(Refusal::Malformed);
        }
        for i in 0..node.count {
            let (key, val) = node.entry(i)?;
            if node.level == 0 {
                visit(key, val)?;
                continue;
            }
            if val.len() != 8 {
                return Err(Refusal::Malformed);
            }
            let child = le64(val, 0)?;
            let child_paddr = if vol.physical {
                child
            } else {
                self.resolve(vol.omap, child, vol.xid)?
            };
            self.walk(vol, child_paddr, child, Some(node.level), nodes, visit)?;
        }
        Ok(())
    }

    /// Calls `visit` with (name, entry) for every entry of directory `dir`.
    pub(crate) fn list(
        &self,
        vol: &Volume<M::Buf>,
        dir: u64,
        visit: &mut dyn FnMut(&[u8], Entry) -> Read<()>,
    ) -> Read<()> {
        let hashed = vol.hashed;
        self.scan(vol, &mut |key, val| {
            let header = le64(key, 0)?;
            if header >> KEY_TYPE_SHIFT != REC_DIR || header & OBJ_ID_MASK != dir {
                return Ok(());
            }
            let (len, at) = if hashed {
                ((le32(key, 8)? & 0x3ff) as usize, 12)
            } else {
                (le16(key, 8)? as usize, 10)
            };
            let name = key.get(at..at + len).ok_or(Refusal::Malformed)?;
            let (&nul, name) = name.split_last().ok_or(Refusal::Malformed)?;
            if nul != 0 || name.is_empty() || name.contains(&0) || at + len != key.len() {
                return Err(Refusal::Malformed);
            }
            let id = le64(val, 0)?;
            let kind = le16(val, 16)? & 0xf;
            visit(name, Entry { id, kind })
        })
    }

    /// The one entry of directory `dir` whose name `matches`.
    pub(crate) fn lookup(
        &self,
        vol: &Volume<M::Buf>,
        dir: u64,
        matches: &dyn Fn(&[u8]) -> bool,
    ) -> Read<Entry> {
        let mut found: Option<Entry> = None;
        let mut twice = false;
        self.list(vol, dir, &mut |name, entry| {
            if matches(name) {
                twice |= found.replace(entry).is_some();
            }
            Ok(())
        })?;
        if twice {
            return Err(Refusal::Ambiguous);
        }
        found.ok_or(Refusal::Absent)
    }

    /// The contents of regular file `id`, at most `max` bytes, read from its
    /// extents. Compressed, encrypted, sparse and holed files are refused.
    pub(crate) fn read_file(&self, vol: &Volume<M::Buf>, id: u64, max: usize) -> Read<M::Buf> {
        let mut inode: Option<(u64, u64)> = None;
        let mut twice = false;
        let mut failure: Option<Refusal> = None;
        self.scan(vol, &mut |key, val| {
            let header = le64(key, 0)?;
            if header >> KEY_TYPE_SHIFT != REC_INODE || header & OBJ_ID_MASK != id {
                return Ok(());
            }
            if key.len() != 8 {
                return Err(Refusal::Malformed);
            }
            match regular_file(val) {
                Ok(found) => twice |= inode.replace(found).is_some(),
                Err(refusal) => failure = Some(refusal),
            }
            Ok(())
        })?;
        if let Some(refusal) = failure {
            return Err(refusal);
        }
        if twice {
            return Err(Refusal::Malformed);
        }
        let (private_id, size) = inode.ok_or(Refusal::Absent)?;
        if size == 0 {
            return Err(Refusal::Malformed);
        }
        if size > max as u64 {
            return Err(Refusal::TooLarge);
        }
        let size = size as usize;
        let mut out = self.media.buffer(size)?;
        let mut covered = 0u64;
        let blocks = self.blocks;
        let media = self.media;
        self.scan(vol, &mut |key, val| {
            let header = le64(key, 0)?;
            if header >> KEY_TYPE_SHIFT != REC_FILE_EXTENT || header & OBJ_ID_MASK != private_id {
                return Ok(());
            }
            if key.len() != 16 || val.len() < 24 {
                return Err(Refusal::Malformed);
            }
            let logical = le64(key, 8)?;
            let len_flags = le64(val, 0)?;
            let len = len_flags & EXTENT_LEN_MASK;
            let paddr = le64(val, 8)?;
            if logical != covered || len == 0 || len % BLOCK_U64 != 0 {
                return Err(Refusal::Malformed);
            }
            if le64(val, 16)? != 0 {
                return Err(Refusal::Encrypted);
            }
            if len_flags >> 56 != 0 || paddr == 0 {
                return Err(Refusal::Unsupported);
            }
            if paddr
                .checked_add(len / BLOCK_U64)
                .ok_or(Refusal::Malformed)?
                > blocks
            {
                return Err(Refusal::Malformed);
            }
            if covered < size as u64 {
                let start = covered as usize;
                let take = core::cmp::min(len, size as u64 - covered) as usize;
                media.read(paddr * BLOCK_U64, &mut out[start..start + take])?;
            }
            covered = covered.checked_add(len).ok_or(Refusal::Malformed)?;
            Ok(())
        })?;
        if covered < size as u64 {
            return Err(Refusal::Malformed);
        }
        Ok(out)
    }
}

/// (data-stream id, size) of a regular, uncompressed, non-sparse inode.
fn regular_file(val: &[u8]) -> Read<(u64, u64)> {
    if val.len() < INODE_XFIELDS {
        return Err(Refusal::Malformed);
    }
    let private_id = le64(val, 8)?;
    let internal = le64(val, 48)?;
    let bsd = le32(val, 68)?;
    let mode = le16(val, 80)?;
    if mode & S_IFMT != S_IFREG {
        return Err(Refusal::Malformed);
    }
    if bsd & UF_COMPRESSED != 0 {
        return Err(Refusal::Compressed);
    }
    if internal & INODE_IS_SPARSE != 0 {
        return Err(Refusal::Unsupported);
    }
    let fields = &val[INODE_XFIELDS..];
    if fields.len() < 4 {
        return Err(Refusal::Malformed);
    }
    let count = le16(fields, 0)? as usize;
    let used = le16(fields, 2)? as usize;
    let mut data = 4 + count * 4;
    if data > fields.len() || data + used > fields.len() {
        return Err(Refusal::Malformed);
    }
    let end = data + used;
    let mut size: Option<u64> = None;
    for i in 0..count {
        let kind = fields[4 + i * 4];
        let len = le16(fields, 4 + i * 4 + 2)? as usize;
        if data + len > end {
            return Err(Refusal::Malformed);
        }
        if kind == XFIELD_DSTREAM {
            if len < 24 || size.is_some() {
                return Err(Refusal::Malformed);
            }
            if le64(fields, data + 16)? != 0 {
                return Err(Refusal::Encrypted);
            }
            size = Some(le64(fields, data)?);
        }
        data += len.next_multiple_of(8);
    }
    Ok((private_id, size.ok_or(Refusal::Malformed)?))
}
