#!/usr/bin/env python3
"""Read-only, metadata-only inspector for the iBoot APFS xART volume.

This intentionally never prints gigalocker contents or attempts a write.
It rejects metadata that it cannot validate instead of guessing an extent.
Only single-node object maps and a single-node xART file-system tree are
supported; this is a diagnostic, not an APFS mount or write authorization.
"""

import os
import struct
import sys
import zlib

BLOCK = 4096
MOD = 0xffffffff


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def checksum(buf):
    s1 = s2 = 0
    for value in struct.unpack("<%dI" % ((len(buf) - 8) // 4), buf[8:]):
        s1 = (s1 + value) % MOD
        s2 = (s2 + s1) % MOD
    c1 = (MOD - (s1 + s2) % MOD) % MOD
    c2 = (MOD - (s1 + c1) % MOD) % MOD
    return (c2 << 32) | c1


class Disk:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        self.count = self.size // BLOCK

    def read(self, paddr, verify=True):
        if paddr < 0 or paddr >= self.count:
            raise ValueError("physical address out of partition")
        data = os.pread(self.fd, BLOCK, paddr * BLOCK)
        if len(data) != BLOCK:
            raise ValueError("short block read")
        if verify and u64(data, 0) != checksum(data):
            raise ValueError("APFS checksum mismatch at block %d" % paddr)
        return data


def node_entries(buf):
    level, flags, count = u16(buf, 34), u16(buf, 32), u32(buf, 36)
    fixed = bool(flags & 4)
    start = 56 + u16(buf, 40)
    key_start = start + u16(buf, 42)
    val_end = BLOCK - (40 if flags & 1 else 0)
    stride = 4 if fixed else 8
    if count > 512 or start < 56 or start + stride * count > key_start or key_start > val_end:
        raise ValueError("invalid B-tree table")
    entries = []
    for i in range(count):
        at = start + i * stride
        if fixed:
            koff, voff = u16(buf, at), u16(buf, at + 2)
            klen, vlen = 16, 16
        else:
            koff, klen, voff, vlen = struct.unpack_from("<HHHH", buf, at)
        kbeg, vbeg = key_start + koff, val_end - voff
        if not (key_start <= kbeg <= kbeg + klen <= val_end and
                key_start <= vbeg <= vbeg + vlen <= val_end):
            raise ValueError("invalid B-tree key/value bounds")
        entries.append((buf[kbeg:kbeg + klen], buf[vbeg:vbeg + vlen]))
    return level, flags, entries


def checkpoint_complete(disk, sb, sb_paddr, desc_base, desc_count):
    if u64(sb, 112) != desc_base or u32(sb, 104) != desc_count:
        return False
    index, length = u32(sb, 136), u32(sb, 140)
    if index >= desc_count or not 2 <= length <= desc_count:
        return False
    if desc_base + (index + length - 1) % desc_count != sb_paddr:
        return False
    xid = u64(sb, 16)
    try:
        for j in range(length - 1):
            mapping = disk.read(desc_base + (index + j) % desc_count)
            if u32(mapping, 24) & 0xffff != 12 or u64(mapping, 16) != xid:
                return False
            if bool(u32(mapping, 32) & 1) != (j == length - 2):
                return False
            count = u32(mapping, 36)
            if count > (BLOCK - 40) // 40:
                return False
            for i in range(count):
                off = 40 + i * 40
                if u32(mapping, off + 8) != BLOCK:
                    return False
                item = disk.read(u64(mapping, off + 32))
                if (u64(item, 8) != u64(mapping, off + 24) or
                        u64(item, 16) != xid or
                        u32(item, 24) != u32(mapping, off)):
                    return False
    except ValueError:
        return False
    return True


def omap_lookup(disk, omap_paddr, oid, max_xid):
    omap = disk.read(omap_paddr)
    if u64(omap, 8) != omap_paddr:
        raise ValueError("object-map identity mismatch")
    tree = u64(omap, 48)
    node = disk.read(tree)
    level, flags, entries = node_entries(node)
    if level:
        raise ValueError("multilevel object map needs further inspection")
    if not flags & 4:
        raise ValueError("object map is not fixed-size")
    matches = []
    for key, val in entries:
        if u64(key, 0) == oid and u64(key, 8) <= max_xid:
            matches.append((u64(key, 8), u32(val, 0), u64(val, 8), u32(val, 4)))
    if not matches:
        raise ValueError("object %d not found in map" % oid)
    selected = max(matches)
    if selected[1] & 1:
        raise ValueError("object %d deleted at selected transaction" % oid)
    return selected[2:]


def inspect(disk):
    base = disk.read(0)
    if base[32:36] != b"NXSB" or u32(base, 36) != BLOCK:
        raise ValueError("unsupported container superblock")
    desc_base, desc_count = u64(base, 112), u32(base, 104)
    if desc_count < 1 or desc_count > 1024:
        raise ValueError("unsupported checkpoint descriptor geometry")
    checkpoints = []
    for paddr in range(desc_base, desc_base + desc_count):
        try:
            block = disk.read(paddr)
        except ValueError:
            continue
        if (block[32:36] == b"NXSB" and u32(block, 36) == BLOCK and
                checkpoint_complete(disk, block, paddr, desc_base, desc_count)):
            checkpoints.append((u64(block, 16), paddr, block))
    if not checkpoints:
        raise ValueError("no valid container superblock")
    xid, cpaddr, sb = max(checkpoints)
    print("partition_bytes=%d blocks=%d" % (disk.size, disk.count))
    print("block_zero_xid=%d" % u64(base, 16))
    print("valid_superblocks=%s" % [(x, p) for x, p, _ in checkpoints])
    print("selected_checkpoint_xid=%d block=%d" % (xid, cpaddr))
    print("container_omap=%d" % u64(sb, 160))
    fs_oids = [u64(sb, 184 + i * 8) for i in range(100)]
    fs_oids = [v for v in fs_oids if v]
    print("volume_oids=%s" % fs_oids)
    for oid in fs_oids:
        paddr, size = omap_lookup(disk, u64(sb, 160), oid, xid)
        vol = disk.read(paddr)
        if vol[32:36] != b"APSB" or u64(vol, 8) != oid:
            raise ValueError("bad volume superblock")
        role = u16(vol, 964)
        print("volume oid=%d block=%d xid=%d role=0x%x" %
              (oid, paddr, u64(vol, 16), role))
        if role != 0x100:
            continue
        vol_omap = u64(vol, 128)
        root_oid = u64(vol, 136)
        root_paddr, root_size = omap_lookup(disk, vol_omap, root_oid, xid)
        root = disk.read(root_paddr)
        level, flags, entries = node_entries(root)
        print("xart_root oid=%d block=%d level=%d records=%d" %
              (root_oid, root_paddr, level, len(entries)))
        if level:
            raise ValueError("multilevel xART FS tree needs further inspection")
        files = []
        for key, val in entries:
            if len(key) < 12 or len(val) < 8:
                continue
            k = u64(key, 0)
            if k == ((9 << 60) | 2):
                name_len = u32(key, 8) & 0x3ff
                if name_len and 12 + name_len <= len(key):
                    name = key[12:12 + name_len - 1]
                    if name == b".gl":
                        files.append(u64(val, 0))
        print("gigalocker_file_ids=%s" % files)
        if len(files) != 1:
            raise ValueError("expected exactly one .gl file")
        inode_values = [value for key, value in entries
                        if len(key) >= 8 and u64(key, 0) == ((3 << 60) | files[0])]
        if len(inode_values) != 1 or len(inode_values[0]) < 92:
            raise ValueError(".gl inode missing or short")
        inode = inode_values[0]
        private_id, inode_flags = u64(inode, 8), u64(inode, 48)
        print("gigalocker_inode_private_id=%d flags=0x%x links=%d write_generation=%d" %
              (private_id, inode_flags, u32(inode, 56), u32(inode, 64)))
        print("xart_volume_flags=0x%x snapshot_count=%d" % (u64(vol, 264), u32(disk.read(vol_omap), 36)))
        extents = []
        for key, val in entries:
            if len(key) >= 16 and len(val) >= 24 and u64(key, 0) == ((8 << 60) | private_id):
                logical, length_flags, physical = u64(key, 8), u64(val, 0), u64(val, 8)
                length = length_flags & ((1 << 56) - 1)
                if logical % BLOCK or length % BLOCK or physical + length // BLOCK > disk.count:
                    raise ValueError("invalid .gl extent")
                extents.append((logical, length, physical, length_flags >> 56, u64(val, 16)))
        extents.sort()
        print("gigalocker_extents_logical_bytes_physical_blocks_flags=%s" % extents)
        if not extents:
            raise ValueError("no .gl extents")
        if len(extents) == 1 and extents[0][0] == 0 and extents[0][1] == 0x600000:
            inspect_raw_locator(disk, extents[0][2])
            scan_other_roots(disk, extents[0][2], extents[0][1])


def inspect_raw_locator(disk, gl_block):
    """Mirror the removed first-root candidate scoring, without writes."""
    slot_size, store_size = 0x9000, 0x600000
    gl_base = gl_block * BLOCK
    gl = os.pread(disk.fd, store_size, gl_base)
    if len(gl) != store_size:
        raise ValueError("short gigalocker extent read")
    roots = []
    records = []
    for idx in range(store_size // slot_size):
        off = idx * slot_size
        if gl[off + 1] in (1, 2) and gl[off + 2:off + 18] == bytes(16):
            roots.append(idx)
        kind = gl[off + 1]
        if not kind:
            continue
        length = u32(gl, off + 0x12)
        valid = (1 <= kind <= 4 and (kind > 2 or gl[off + 2:off + 18] == bytes(16))
                 and 1 <= length <= 0x8000 and
                 zlib.crc32(gl[off + 0x22:off + 0x22 + length]) == u32(gl, off + 0x16))
        records.append((idx, kind, length, u64(gl, off + 0x1a), valid))
    print("gigalocker_records_slot_kind_length_revision_crc_valid=%s" % records)
    print("raw_root_signature_slots=%s" % roots)
    if not roots:
        return
    hit = gl_base + roots[0] * slot_size
    candidates = []
    attempts = 0
    for k in range(4097):
        step = k * slot_size
        if step > hit:
            break
        base = hit - step
        if base + store_size > disk.size:
            continue
        attempts += 1
        if attempts > 64:
            break
        raw = os.pread(disk.fd, store_size, base)
        live = {}
        malformed = 0
        for idx in range(store_size // slot_size):
            off = idx * slot_size
            kind = raw[off + 1]
            if kind == 0:
                continue
            uid = raw[off + 2:off + 18]
            length = u32(raw, off + 0x12)
            crc = u32(raw, off + 0x16)
            revision = u64(raw, off + 0x1a)
            if not (1 <= kind <= 4 and (kind > 2 or uid == bytes(16)) and
                    1 <= length <= 0x8000 and
                    zlib.crc32(raw[off + 0x22:off + 0x22 + length]) == crc):
                malformed += 1
                continue
            key = (kind, uid)
            if key not in live or revision >= live[key]:
                live[key] = revision
        if (1, bytes(16)) in live and (2, bytes(16)) in live:
            candidates.append((len(live), -malformed, base))
    if not candidates:
        print("raw_locator=no_valid_candidate")
        return
    best = max(candidates)
    print("raw_locator_candidates=%d attempts=%d" % (len(candidates), attempts))
    print("raw_locator_best_block=%d apfs_extent_block=%d shift_blocks=%d live_records=%d malformed=%d" %
          (best[2] // BLOCK, gl_block, (best[2] - gl_base) // BLOCK, best[0], -best[1]))


def scan_other_roots(disk, gl_block, gl_size):
    """Count CRC-valid root signatures outside APFS's current file extent."""
    outside = []
    for block in range(disk.count):
        off = block * BLOCK
        header = os.pread(disk.fd, 0x22, off)
        if len(header) != 0x22 or header[1] not in (1, 2) or header[2:18] != bytes(16):
            continue
        length = u32(header, 0x12)
        if not 1 <= length <= 0x8000:
            continue
        payload = os.pread(disk.fd, length, off + 0x22)
        if len(payload) != length or zlib.crc32(payload) != u32(header, 0x16):
            continue
        if not gl_block <= block < gl_block + gl_size // BLOCK:
            outside.append((block, header[1], u64(header, 0x1a)))
    print("crc_valid_root_signatures_outside_apfs_gl=%s" % outside[:32])
    if len(outside) > 32:
        print("crc_valid_root_signatures_outside_apfs_gl_additional=%d" % (len(outside) - 32))


if __name__ == "__main__":
    try:
        inspect(Disk(sys.argv[1]))
    except (ValueError, OSError, IndexError) as exc:
        print("INCOMPLETE: %s" % exc, file=sys.stderr)
        sys.exit(1)
