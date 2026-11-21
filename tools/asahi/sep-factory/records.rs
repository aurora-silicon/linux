// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Host tests for the Touch ID factory-record reader, on synthetic APFS
//! containers laid out like the iBoot System Containers measured on an M1
//! (J313) and an M2 Pro (J414s): a preboot volume holding each volume
//! group's directory, and a Hardware volume whose factory-data cache holds
//! the records. The records are synthetic DER with the same structure.
//!
//! Built by sep-factory-records-tests.py with `rustc --test`, which also
//! builds each mutant of the production code and expects a test to fail.

#[path = "../../../drivers/soc/apple/apfs_read.rs"]
#[allow(dead_code)]
mod apfs_read;
#[path = "../../../drivers/soc/apple/sensor_factory.rs"]
mod sensor_factory;

use apfs_read::{Media, Read, Refusal};
use std::cell::RefCell;

const B: usize = 4096;
const GROUP: [u8; 16] = [
    0xdf, 0x58, 0x89, 0x07, 0x65, 0xe0, 0x40, 0xca, 0x8f, 0x70, 0x30, 0x25, 0xe7, 0x0d, 0xcb, 0x4f,
];
const GROUP_NAME: &str = "DF588907-65E0-40CA-8F70-3025E70DCB4F";
const XID: u64 = 42;

// ------------------------------------------------------------------ media

struct Image {
    data: Vec<u8>,
    reads: RefCell<usize>,
}

impl Media for Image {
    type Buf = Vec<u8>;
    fn size(&self) -> Read<u64> {
        Ok(self.data.len() as u64)
    }
    fn buffer(&self, len: usize) -> Read<Vec<u8>> {
        Ok(vec![0; len])
    }
    fn read(&self, offset: u64, buf: &mut [u8]) -> Read<()> {
        *self.reads.borrow_mut() += 1;
        let start = offset as usize;
        let src = self.data.get(start..start + buf.len()).ok_or(Refusal::Io)?;
        buf.copy_from_slice(src);
        Ok(())
    }
}

/// A read-only block device or image, for the real-container check.
struct Device(std::fs::File, u64);

impl Media for Device {
    type Buf = Vec<u8>;
    fn size(&self) -> Read<u64> {
        Ok(self.1)
    }
    fn buffer(&self, len: usize) -> Read<Vec<u8>> {
        Ok(vec![0; len])
    }
    fn read(&self, offset: u64, buf: &mut [u8]) -> Read<()> {
        use std::os::unix::fs::FileExt;
        self.0.read_exact_at(buf, offset).map_err(|_| Refusal::Io)
    }
}

// ------------------------------------------------------------------ sha-256

fn sha256(data: &[u8]) -> [u8; 32] {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for chunk in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[i * 4..i * 4 + 4].try_into().unwrap());
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let mut v = h;
        for i in 0..64 {
            let s1 = v[4].rotate_right(6) ^ v[4].rotate_right(11) ^ v[4].rotate_right(25);
            let ch = (v[4] & v[5]) ^ (!v[4] & v[6]);
            let t1 = v[7]
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = v[0].rotate_right(2) ^ v[0].rotate_right(13) ^ v[0].rotate_right(22);
            let maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            let t2 = s0.wrapping_add(maj);
            v = [
                t1.wrapping_add(t2),
                v[0],
                v[1],
                v[2],
                v[3].wrapping_add(t1),
                v[4],
                v[5],
                v[6],
            ];
        }
        for i in 0..8 {
            h[i] = h[i].wrapping_add(v[i]);
        }
    }
    let mut out = [0u8; 32];
    for i in 0..8 {
        out[i * 4..i * 4 + 4].copy_from_slice(&h[i].to_be_bytes());
    }
    out
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02X}")).collect()
}

fn digest(data: &[u8]) -> Read<[u8; 32]> {
    Ok(sha256(data))
}

// ------------------------------------------------------------------ DER

fn der(tag: u8, value: &[u8]) -> Vec<u8> {
    let mut out = vec![tag];
    if value.len() < 0x80 {
        out.push(value.len() as u8);
    } else {
        let len = (value.len() as u64).to_be_bytes();
        let skip = len.iter().take_while(|&&b| b == 0).count();
        out.push(0x80 | (8 - skip) as u8);
        out.extend_from_slice(&len[skip..]);
    }
    out.extend_from_slice(value);
    out
}

fn text(value: &str) -> Vec<u8> {
    der(0x16, value.as_bytes())
}

fn cat(parts: &[&[u8]]) -> Vec<u8> {
    parts.concat()
}

/// A manifest with a body large enough to need a long-form length.
fn manifest(name: &str) -> Vec<u8> {
    let body = der(0x04, name.repeat(40).as_bytes());
    der(
        0x30,
        &cat(&[
            &text("IM4M"),
            &der(0x02, &[0]),
            &der(0x31, &body),
            &der(0x04, &[1; 70]),
            &der(0x30, &[]),
        ]),
    )
}

fn im4p(kind: &str, payload: &str) -> Vec<u8> {
    // Several blocks, as the real records are (64 KiB for FSCl).
    let body = payload.repeat(800);
    der(
        0x30,
        &cat(&[
            &text("IM4P"),
            &text(kind),
            &text("1.0"),
            &der(0x04, body.as_bytes()),
        ]),
    )
}

fn img4(payload: &[u8], manifest: &[u8]) -> Vec<u8> {
    der(0x30, &cat(&[&text("IMG4"), payload, &der(0xa0, manifest)]))
}

fn comb(image: &[u8], trust: &[u8]) -> Vec<u8> {
    der(
        0x30,
        &cat(&[
            &text("comb"),
            &der(0x30, &cat(&[&text("fdrd"), &der(0x04, image)])),
            &der(0x30, &cat(&[&text("secb"), &der(0x04, trust)])),
        ]),
    )
}

fn trust() -> Vec<u8> {
    der(0x30, &cat(&[&text("secb"), &der(0x30, &[7; 300])]))
}

fn seal() -> Vec<u8> {
    manifest("seal")
}

/// An FSCl record, stored as the enclave loads it.
fn fscl() -> Vec<u8> {
    comb(
        &img4(&im4p("FSCl", "CALB synthetic FSCl "), &seal()),
        &trust(),
    )
}

fn fsc2() -> Vec<u8> {
    img4(&im4p("FSC2", "CALB synthetic FSC2 "), &manifest("factory"))
}

fn msrk() -> Vec<u8> {
    img4(&im4p("MSRk", "synthetic root key "), &manifest("factory"))
}

/// What the enclave loads for a standalone image: its IM4P under the seal.
fn resealed(image: &[u8]) -> Vec<u8> {
    let payload = im4p_of(image);
    comb(&img4(&payload, &seal()), &trust())
}

fn im4p_of(image: &[u8]) -> Vec<u8> {
    // IMG4 { IA5 "IMG4", IM4P, [0] }: skip the outer header and the IA5.
    let header = if image[1] & 0x80 != 0 {
        2 + (image[1] & 0x7f) as usize
    } else {
        2
    };
    let at = header + 6;
    let len_byte = image[at + 1];
    let (hl, len) = if len_byte & 0x80 == 0 {
        (2, len_byte as usize)
    } else {
        let n = (len_byte & 0x7f) as usize;
        (
            2 + n,
            image[at + 2..at + 2 + n]
                .iter()
                .fold(0usize, |a, b| a << 8 | *b as usize),
        )
    };
    image[at..at + hl + len].to_vec()
}

// ------------------------------------------------------------------ APFS

fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(b: &mut [u8], at: usize, v: u64) {
    b[at..at + 8].copy_from_slice(&v.to_le_bytes());
}

fn fletcher(block: &[u8]) -> u64 {
    const M: u64 = 0xffff_ffff;
    let (mut s1, mut s2) = (0u64, 0u64);
    for w in block[8..].chunks_exact(4) {
        s1 = (s1 + u32::from_le_bytes(w.try_into().unwrap()) as u64) % M;
        s2 = (s2 + s1) % M;
    }
    let c1 = M - (s1 + s2) % M;
    let c2 = M - (s1 + c1) % M;
    (c2 << 32) | c1
}

fn finish(block: &mut [u8]) {
    let sum = fletcher(block);
    put64(block, 0, sum);
}

fn header(block: &mut [u8], oid: u64, xid: u64, kind: u32, subtype: u32) {
    put64(block, 8, oid);
    put64(block, 16, xid);
    put32(block, 24, kind);
    put32(block, 28, subtype);
}

/// A B-tree node block from sorted (key, value) entries.
fn node(entries: &[(Vec<u8>, Vec<u8>)], root: bool, level: u16, fixed: bool) -> Vec<u8> {
    let mut b = vec![0u8; B];
    let flags = (root as u16) | if level == 0 { 2 } else { 0 } | if fixed { 4 } else { 0 };
    put16(&mut b, 32, flags);
    put16(&mut b, 34, level);
    put32(&mut b, 36, entries.len() as u32);
    let stride = if fixed { 4 } else { 8 };
    let table = entries.len() * stride;
    put16(&mut b, 40, 0);
    put16(&mut b, 42, table as u16);
    let key_start = 56 + table;
    let val_end = B - if root { 40 } else { 0 };
    let (mut ko, mut vo) = (0usize, 0usize);
    for (i, (k, v)) in entries.iter().enumerate() {
        b[key_start + ko..key_start + ko + k.len()].copy_from_slice(k);
        vo += v.len();
        b[val_end - vo..val_end - vo + v.len()].copy_from_slice(v);
        let at = 56 + i * stride;
        if fixed {
            put16(&mut b, at, ko as u16);
            put16(&mut b, at + 2, vo as u16);
        } else {
            put16(&mut b, at, ko as u16);
            put16(&mut b, at + 2, k.len() as u16);
            put16(&mut b, at + 4, vo as u16);
            put16(&mut b, at + 6, v.len() as u16);
        }
        ko += k.len();
    }
    assert!(key_start + ko <= val_end - vo, "node overflow");
    if root {
        put32(&mut b, B - 40 + 4, B as u32);
    }
    b
}

#[derive(Clone)]
struct File {
    name: String,
    data: Vec<u8>,
    /// Lay the file out in three extents (one block, one block, the rest)
    /// that are not contiguous on disk, as the J313's FSCl record is.
    split: bool,
    bsd_flags: u32,
    crypto: u64,
    hole: bool,
    gap: bool,
}

impl File {
    fn new(name: &str, data: Vec<u8>) -> File {
        File {
            name: name.into(),
            data,
            split: false,
            bsd_flags: 0,
            crypto: 0,
            hole: false,
            gap: false,
        }
    }
}

#[derive(Clone)]
struct Dir {
    name: String,
    dirs: Vec<Dir>,
    files: Vec<File>,
}

fn dir(name: &str, dirs: Vec<Dir>, files: Vec<File>) -> Dir {
    Dir {
        name: name.into(),
        dirs,
        files,
    }
}

#[derive(Clone)]
struct Vol {
    role: u16,
    root: Dir,
    fs_flags: u64,
    leaves: usize,
    hashed_nodes: bool,
}

/// What a test changes on the built image.
#[derive(Clone, Default)]
struct Faults {
    /// Flip one byte in the first fs-tree node of the Hardware volume.
    corrupt_fs_node: bool,
    /// An 8 KiB container block size.
    block_size: Option<u32>,
    /// A newer checkpoint in the ring whose mapping block is from another
    /// transaction, and whose object map is not there.
    incomplete_newer: bool,
    /// A complete newer checkpoint whose object map is not there.
    complete_newer: bool,
    /// Older and newer versions of the Hardware fs-tree root in its volume's
    /// object map, both pointing at a block that is not a node.
    stale_versions: bool,
}

struct Builder {
    blocks: Vec<Vec<u8>>,
    /// Blocks that hold objects, which get checksums; the rest is file data.
    objects: Vec<usize>,
    omap: Vec<(u64, u64)>,
    next_oid: u64,
    hardware_node: Option<u64>,
}

impl Builder {
    fn alloc(&mut self) -> u64 {
        self.blocks.push(vec![0u8; B]);
        (self.blocks.len() - 1) as u64
    }
    fn object(&mut self) -> u64 {
        let paddr = self.alloc();
        self.objects.push(paddr as usize);
        paddr
    }
    fn oid(&mut self) -> u64 {
        self.next_oid += 1;
        self.next_oid
    }

    fn records(
        &mut self,
        d: &Dir,
        id: u64,
        parent: u64,
        next: &mut u64,
        out: &mut Vec<(Vec<u8>, Vec<u8>)>,
    ) {
        out.push((inode_key(id), inode_val(parent, id, 0o040755, 0, None, 0)));
        for sub in &d.dirs {
            *next += 1;
            let child = *next;
            out.push(drec(id, &sub.name, child, 4));
            self.records(sub, child, id, next, out);
        }
        for f in &d.files {
            *next += 1;
            let child = *next;
            out.push(drec(id, &f.name, child, 8));
            out.push((
                inode_key(child),
                inode_val(
                    id,
                    child,
                    0o100644,
                    f.bsd_flags,
                    Some(f.data.len() as u64),
                    0,
                ),
            ));
            let blocks = f.data.len().div_ceil(B);
            let pieces = if f.split && blocks >= 3 {
                vec![1, 1, blocks - 2]
            } else {
                vec![blocks]
            };
            let mut logical = 0usize;
            for (n, &count) in pieces.iter().enumerate() {
                let first = self.blocks.len() as u64;
                for i in 0..count {
                    let p = self.alloc() as usize;
                    let src = &f.data[(logical + i) * B..((logical + i + 1) * B).min(f.data.len())];
                    self.blocks[p][..src.len()].copy_from_slice(src);
                }
                // Leave a gap between pieces so that they are not contiguous on disk.
                self.alloc();
                let paddr = if f.hole && n == 0 { 0 } else { first };
                let at = if f.gap && n == 1 {
                    logical + 1
                } else {
                    logical
                };
                let mut key = (child | 8 << 60).to_le_bytes().to_vec();
                key.extend_from_slice(&((at * B) as u64).to_le_bytes());
                let mut val = ((count * B) as u64).to_le_bytes().to_vec();
                val.extend_from_slice(&paddr.to_le_bytes());
                val.extend_from_slice(&f.crypto.to_le_bytes());
                out.push((key, val));
                logical += count;
            }
        }
    }

    fn volume(&mut self, v: &Vol, hardware: bool, faults: &Faults) -> u64 {
        let mut recs = Vec::new();
        let mut next = 15u64;
        self.records(&v.root, 2, 1, &mut next, &mut recs);
        recs.sort_by(|a, b| sort_key(&a.0).cmp(&sort_key(&b.0)));
        // Volume object map entries: (virtual oid, paddr).
        let mut vomap: Vec<(u64, u64)> = Vec::new();
        let root_oid = self.oid();
        let root_paddr = self.object();
        vomap.push((root_oid, root_paddr));
        let leaves = v.leaves.max(1);
        let per = recs.len().div_ceil(leaves);
        let mut first_node: Option<u64> = None;
        if leaves == 1 {
            let mut b = node(&recs, true, 0, false);
            header(&mut b, root_oid, XID, 2, 0xe);
            if v.hashed_nodes {
                b[32] |= 8;
            }
            self.blocks[root_paddr as usize] = b;
            first_node = Some(root_paddr);
        } else {
            let mut index = Vec::new();
            for chunk in recs.chunks(per) {
                let oid = self.oid();
                let paddr = self.object();
                vomap.push((oid, paddr));
                let mut b = node(chunk, false, 0, false);
                header(&mut b, oid, XID, 3, 0xe);
                if v.hashed_nodes {
                    b[32] |= 8;
                }
                self.blocks[paddr as usize] = b;
                first_node.get_or_insert(paddr);
                index.push((chunk[0].0.clone(), oid.to_le_bytes().to_vec()));
            }
            let mut b = node(&index, true, 1, false);
            header(&mut b, root_oid, XID, 2, 0xe);
            self.blocks[root_paddr as usize] = b;
        }
        let mut versions: Vec<(u64, u64, u64)> = vomap.iter().map(|&(o, p)| (o, XID, p)).collect();
        if hardware {
            self.hardware_node = first_node;
            if faults.stale_versions {
                let junk = self.alloc();
                versions.push((root_oid, XID - 1, junk));
                versions.push((root_oid, XID + 1, junk));
            }
        }
        let omap = self.omap_versions(&versions);
        let vol_oid = self.oid();
        let paddr = self.object();
        let mut b = vec![0u8; B];
        header(&mut b, vol_oid, XID, 0xd, 0);
        b[32..36].copy_from_slice(b"APSB");
        put64(&mut b, 56, 1);
        put32(&mut b, 116, 2);
        put64(&mut b, 128, omap);
        put64(&mut b, 136, root_oid);
        put64(&mut b, 264, v.fs_flags);
        put16(&mut b, 964, v.role);
        self.blocks[paddr as usize] = b;
        self.omap.push((vol_oid, paddr));
        vol_oid
    }

    /// An object map, with its single-leaf tree, for `entries`.
    fn omap_object(&mut self, entries: &[(u64, u64)]) -> u64 {
        let versions: Vec<_> = entries.iter().map(|&(o, p)| (o, XID, p)).collect();
        self.omap_versions(&versions)
    }

    /// An object map for (oid, xid, paddr) `entries`.
    fn omap_versions(&mut self, entries: &[(u64, u64, u64)]) -> u64 {
        let mut sorted = entries.to_vec();
        sorted.sort();
        let tree = self.object();
        let kv: Vec<_> = sorted
            .iter()
            .map(|&(oid, xid, paddr)| {
                let mut k = oid.to_le_bytes().to_vec();
                k.extend_from_slice(&xid.to_le_bytes());
                let mut v = 0u32.to_le_bytes().to_vec();
                v.extend_from_slice(&(B as u32).to_le_bytes());
                v.extend_from_slice(&paddr.to_le_bytes());
                (k, v)
            })
            .collect();
        let mut b = node(&kv, true, 0, true);
        header(&mut b, tree, XID, 0x4000_0002, 0xb);
        self.blocks[tree as usize] = b;
        let obj = self.object();
        let mut b = vec![0u8; B];
        header(&mut b, obj, XID, 0x4000_000b, 0);
        put32(&mut b, 40, 0x4000_0002);
        put64(&mut b, 48, tree);
        self.blocks[obj as usize] = b;
        obj
    }
}

fn sort_key(k: &[u8]) -> (u64, u64, Vec<u8>) {
    let h = u64::from_le_bytes(k[..8].try_into().unwrap());
    (h & ((1 << 60) - 1), h >> 60, k[8..].to_vec())
}

fn inode_key(id: u64) -> Vec<u8> {
    (id | 3 << 60).to_le_bytes().to_vec()
}

fn inode_val(parent: u64, id: u64, mode: u16, bsd: u32, size: Option<u64>, crypto: u64) -> Vec<u8> {
    let mut v = vec![0u8; 92];
    put64(&mut v, 0, parent);
    put64(&mut v, 8, id);
    put64(&mut v, 48, 0x8000);
    put32(&mut v, 68, bsd);
    put16(&mut v, 80, mode);
    let Some(size) = size else {
        v.extend_from_slice(&[0, 0, 0, 0]);
        return v;
    };
    let mut x = Vec::new();
    x.extend_from_slice(&1u16.to_le_bytes());
    x.extend_from_slice(&40u16.to_le_bytes());
    x.extend_from_slice(&[8, 0x20]);
    x.extend_from_slice(&40u16.to_le_bytes());
    x.extend_from_slice(&size.to_le_bytes());
    x.extend_from_slice(&(size.div_ceil(B as u64) * B as u64).to_le_bytes());
    x.extend_from_slice(&crypto.to_le_bytes());
    x.extend_from_slice(&[0; 16]);
    v.extend_from_slice(&x);
    v
}

fn drec(parent: u64, name: &str, child: u64, kind: u16) -> (Vec<u8>, Vec<u8>) {
    let mut k = (parent | 9 << 60).to_le_bytes().to_vec();
    let len = name.len() as u32 + 1;
    k.extend_from_slice(&(len | (name.len() as u32 * 7919 & 0x3f_ffff) << 10).to_le_bytes());
    k.extend_from_slice(name.as_bytes());
    k.push(0);
    let mut v = child.to_le_bytes().to_vec();
    v.extend_from_slice(&[0; 8]);
    v.extend_from_slice(&kind.to_le_bytes());
    (k, v)
}

fn container(vols: &[Vol], faults: &Faults) -> Image {
    // 0: base superblock; 1-4: the checkpoint descriptor ring.
    let mut b = Builder {
        blocks: vec![vec![0u8; B]; 5],
        objects: vec![0, 1, 2, 3, 4],
        omap: Vec::new(),
        next_oid: 1024,
        hardware_node: None,
    };
    let mut oids = Vec::new();
    for v in vols {
        oids.push(b.volume(v, v.role == 0x140, faults));
    }
    let omap_entries = b.omap.clone();
    let omap = b.omap_object(&omap_entries);
    let blocks = b.blocks.len() as u64;
    let superblock = |xid: u64, index: u32, omap: u64| {
        let mut sb = vec![0u8; B];
        header(&mut sb, 1, xid, 0x8000_0001, 0);
        sb[32..36].copy_from_slice(b"NXSB");
        put32(&mut sb, 36, faults.block_size.unwrap_or(B as u32));
        put64(&mut sb, 40, blocks);
        put64(&mut sb, 64, 2);
        put32(&mut sb, 104, 4);
        put64(&mut sb, 112, 1);
        put32(&mut sb, 136, index);
        put32(&mut sb, 140, 2);
        put64(&mut sb, 160, omap);
        put32(&mut sb, 180, 100);
        for (i, oid) in oids.iter().enumerate() {
            put64(&mut sb, 184 + i * 8, *oid);
        }
        sb
    };
    let map = |paddr: u64, xid: u64| {
        let mut map = vec![0u8; B];
        header(&mut map, paddr, xid, 0x4000_000c, 0);
        put32(&mut map, 32, 1);
        map
    };
    let sb = superblock(XID, 0, omap);
    b.blocks[0] = sb.clone();
    b.blocks[1] = map(1, XID);
    b.blocks[2] = sb;
    if faults.incomplete_newer || faults.complete_newer {
        let map_xid = if faults.complete_newer {
            XID + 1
        } else {
            XID - 1
        };
        b.blocks[3] = map(3, map_xid);
        // The last two blocks are the real object map and its tree: point at the tree.
        b.blocks[4] = superblock(XID + 1, 2, blocks - 2);
    }
    for &i in &b.objects {
        finish(&mut b.blocks[i]);
    }
    if faults.corrupt_fs_node {
        let p = b.hardware_node.unwrap() as usize;
        b.blocks[p][200] ^= 0x55;
    }
    Image {
        data: b.blocks.concat(),
        reads: RefCell::new(0),
    }
}

// ------------------------------------------------------------------ layouts

fn factory(files: Vec<File>) -> Dir {
    dir(
        "",
        vec![
            dir(
                "FactoryData",
                vec![dir(
                    "System",
                    vec![dir(
                        "Library",
                        vec![dir(
                            "Caches",
                            vec![dir("com.apple.factorydata", vec![], files)],
                            vec![File::new("apticket.der", vec![9; 100])],
                        )],
                        vec![],
                    )],
                    vec![],
                )],
                vec![],
            ),
            dir(
                "MobileActivation",
                vec![],
                vec![File::new("data_ark.plist", vec![1; 476])],
            ),
        ],
        vec![],
    )
}

fn trust_files() -> Vec<File> {
    let t = trust();
    vec![
        File::new(&format!("trustobject-{}", hex(&sha256(&t))), t.clone()),
        File::new("trustobject-current", t),
    ]
}

/// An M1/M2-style cache: an FSCl record and the sealing records, no root key.
fn fscl_cache() -> Vec<File> {
    let mut cal = File::new("FSCl-0123456789ABCDEF", fscl());
    cal.split = true;
    let mut files = vec![
        cal,
        File::new("seal-00008103-0011548E3EA2001E", seal()),
        File::new("hop0-ABC", vec![3; 5000]),
    ];
    files.extend(trust_files());
    files
}

/// A Neo-style cache: standalone FSC2 and MSRk images and the sealing records.
fn fsc2_cache() -> Vec<File> {
    let mut files = vec![
        File::new("FSC2-FEDCBA9876543210", fsc2()),
        File::new("MSRk-FEDCBA9876543210", msrk()),
        File::new("seal-00008140-0000000000000001", seal()),
    ];
    files.extend(trust_files());
    files
}

fn layout(files: Vec<File>) -> Vec<Vol> {
    let preboot = dir(
        "",
        vec![
            dir(
                "3D3287DE-280D-4619-AAAB-D97469CA9C71",
                vec![dir("LocalPolicy", vec![], vec![])],
                vec![],
            ),
            dir(GROUP_NAME, vec![dir("LocalPolicy", vec![], vec![])], vec![]),
        ],
        vec![],
    );
    vec![
        Vol {
            role: 0x10,
            root: preboot,
            fs_flags: 1,
            leaves: 1,
            hashed_nodes: false,
        },
        Vol {
            role: 0x100,
            root: dir("", vec![], vec![File::new(".gl", vec![0; 8192])]),
            fs_flags: 1,
            leaves: 1,
            hashed_nodes: false,
        },
        Vol {
            role: 0x140,
            root: factory(files),
            fs_flags: 1,
            leaves: 3,
            hashed_nodes: false,
        },
    ]
}

fn read(image: &Image, root_key: bool) -> Read<sensor_factory::Records<Vec<u8>>> {
    sensor_factory::read(image, &GROUP, root_key, &digest)
}

fn calibration(files: Vec<File>) -> Read<Vec<u8>> {
    read(&container(&layout(files), &Faults::default()), false)?.calibration
}

fn without(mut files: Vec<File>, prefix: &str) -> Vec<File> {
    files.retain(|f| !f.name.starts_with(prefix));
    files
}

// ------------------------------------------------------------------ tests

#[test]
fn an_fscl_record_is_found_and_used_as_stored() {
    let image = container(&layout(fscl_cache()), &Faults::default());
    let records = read(&image, false).unwrap();
    assert_eq!(records.calibration.unwrap(), fscl());
    assert!(!records.resealed);
    assert!(records.root_key.is_none());
    // A single-leaf Hardware tree reads the same.
    let mut vols = layout(fscl_cache());
    vols[2].leaves = 1;
    let image = container(&vols, &Faults::default());
    assert_eq!(read(&image, false).unwrap().calibration.unwrap(), fscl());
}

#[test]
fn an_fsc2_record_and_the_root_key_are_rebuilt_under_the_seal() {
    let image = container(&layout(fsc2_cache()), &Faults::default());
    let records = read(&image, true).unwrap();
    assert!(records.resealed);
    assert_eq!(records.calibration.unwrap(), resealed(&fsc2()));
    assert_eq!(records.root_key.unwrap().unwrap(), resealed(&msrk()));
}

#[test]
fn the_root_key_is_only_read_when_asked_for_and_absent_on_fscl_macs() {
    let image = container(&layout(fscl_cache()), &Faults::default());
    let records = read(&image, true).unwrap();
    assert_eq!(records.calibration.unwrap(), fscl());
    assert_eq!(records.root_key.unwrap().unwrap_err(), Refusal::Absent);
    let image = container(&layout(fsc2_cache()), &Faults::default());
    assert!(read(&image, false).unwrap().root_key.is_none());
}

#[test]
fn an_absent_record_is_refused() {
    assert_eq!(
        calibration(without(fscl_cache(), "FSCl")).unwrap_err(),
        Refusal::Absent
    );
    let image = container(&layout(without(fsc2_cache(), "MSRk")), &Faults::default());
    let records = read(&image, true).unwrap();
    assert_eq!(records.calibration.unwrap(), resealed(&fsc2()));
    assert_eq!(records.root_key.unwrap().unwrap_err(), Refusal::Absent);
    // No factory-data directory at all.
    let mut vols = layout(fscl_cache());
    vols[2].root = dir("", vec![dir("FactoryData", vec![], vec![])], vec![]);
    assert_eq!(
        read(&container(&vols, &Faults::default()), false).err(),
        Some(Refusal::Absent)
    );
    // No Hardware volume.
    let vols = layout(fscl_cache())[..2].to_vec();
    assert_eq!(
        read(&container(&vols, &Faults::default()), false).err(),
        Some(Refusal::Absent)
    );
}

#[test]
fn a_corrupt_record_is_refused() {
    let mut files = fscl_cache();
    let data = &mut files[0].data;
    let at = data.len() - 400;
    data.truncate(at);
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // A record that is DER but not a calibration.
    let mut files = fscl_cache();
    files[0].data = der(0x30, &cat(&[&text("comb"), &der(0x30, &[])]));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // A standalone FSCl image is not what the enclave loads.
    let mut files = fscl_cache();
    files[0].data = img4(&im4p("FSCl", "CALB "), &manifest("factory"));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // A comb record whose image is another record type.
    let mut files = fscl_cache();
    files[0].data = comb(&img4(&im4p("MSRk", "x"), &seal()), &trust());
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // A non-minimal DER length.
    let mut files = fscl_cache();
    let mut bad = vec![0x30, 0x81, 0x05];
    bad.extend_from_slice(&text("IMG"));
    files[0].data = bad;
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // A seal that is not a manifest refuses only what needs it.
    let mut files = fsc2_cache();
    files[2].data = der(0x30, &text("IM4X"));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    let mut files = fscl_cache();
    files[1].data = der(0x30, &text("IM4X"));
    assert_eq!(calibration(files).unwrap(), fscl());
}

#[test]
fn corrupt_metadata_is_refused() {
    let faults = Faults {
        corrupt_fs_node: true,
        ..Faults::default()
    };
    let image = container(&layout(fscl_cache()), &faults);
    assert_eq!(read(&image, false).err(), Some(Refusal::Malformed));
    // A checksum error in the container superblock's checkpoint copy.
    let mut image = container(&layout(fscl_cache()), &Faults::default());
    image.data[2 * B + 300] ^= 1;
    assert_eq!(read(&image, false).err(), Some(Refusal::Malformed));
    // Not APFS at all.
    let image = Image {
        data: vec![0; 64 * B],
        reads: RefCell::new(0),
    };
    assert_eq!(read(&image, false).err(), Some(Refusal::NotApfs));
}

#[test]
fn unsupported_tree_shapes_are_refused() {
    let mut vols = layout(fscl_cache());
    vols[2].hashed_nodes = true;
    assert_eq!(
        read(&container(&vols, &Faults::default()), false).err(),
        Some(Refusal::Unsupported)
    );
    let faults = Faults {
        block_size: Some(8192),
        ..Faults::default()
    };
    assert_eq!(
        read(&container(&layout(fscl_cache()), &faults), false).err(),
        Some(Refusal::Unsupported)
    );
    // A hole in the record.
    let mut files = fscl_cache();
    assert!(files[0].split);
    files[0].hole = true;
    assert_eq!(calibration(files).unwrap_err(), Refusal::Unsupported);
    // Extents that leave a gap in the file.
    let mut files = fscl_cache();
    files[0].gap = true;
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
}

#[test]
fn encrypted_and_compressed_records_are_refused() {
    let mut vols = layout(fscl_cache());
    vols[2].fs_flags = 0;
    assert_eq!(
        read(&container(&vols, &Faults::default()), false).err(),
        Some(Refusal::Encrypted)
    );
    let mut files = fscl_cache();
    files[0].crypto = 5;
    assert_eq!(calibration(files).unwrap_err(), Refusal::Encrypted);
    let mut files = fscl_cache();
    files[0].bsd_flags = 0x20;
    assert_eq!(calibration(files).unwrap_err(), Refusal::Compressed);
}

#[test]
fn another_macs_container_is_refused() {
    let mut vols = layout(fscl_cache());
    vols[0].root.dirs.retain(|d| d.name != GROUP_NAME);
    assert_eq!(
        read(&container(&vols, &Faults::default()), false).err(),
        Some(Refusal::Foreign)
    );
    // The group directory name is matched case-insensitively.
    let mut vols = layout(fscl_cache());
    for d in &mut vols[0].root.dirs {
        d.name = d.name.to_lowercase();
    }
    assert_eq!(
        read(&container(&vols, &Faults::default()), false)
            .unwrap()
            .calibration
            .unwrap(),
        fscl()
    );
}

#[test]
fn ambiguous_and_mismatched_records_are_refused() {
    let mut files = fscl_cache();
    files.push(File::new("FSC2-0123456789ABCDEF", fsc2()));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Ambiguous);
    let mut files = fsc2_cache();
    files.push(File::new("MSRk-0000", msrk()));
    assert_eq!(
        read(&container(&layout(files), &Faults::default()), true).err(),
        Some(Refusal::Ambiguous)
    );
    let mut files = fsc2_cache();
    files[1].name = "MSRk-1111111111111111".into();
    assert_eq!(calibration(files).unwrap_err(), Refusal::Mismatched);
    let mut files = fsc2_cache();
    files.push(File::new("seal-00008140-0000000000000002", seal()));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Ambiguous);
}

#[test]
fn the_trust_bundle_must_be_the_named_one_in_use() {
    // trustobject-current that matches no named bundle.
    let mut files = fsc2_cache();
    files.retain(|f| f.name != "trustobject-current");
    let mut other = trust();
    let n = other.len();
    other[n - 1] ^= 1;
    files.push(File::new("trustobject-current", other));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // A named bundle whose bytes do not match its name.
    let mut files = without(fsc2_cache(), "trustobject-");
    let mut wrong = trust();
    let n = wrong.len();
    wrong[n - 1] ^= 1;
    files.push(File::new(
        &format!("trustobject-{}", hex(&sha256(&trust()))),
        wrong,
    ));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // The named bundle trustobject-current points at, with other bytes.
    let mut files = fsc2_cache();
    for f in &mut files {
        if f.name.starts_with("trustobject-") && f.name != "trustobject-current" {
            let n = f.data.len();
            f.data[n - 1] ^= 1;
        }
    }
    assert_eq!(calibration(files).unwrap_err(), Refusal::Malformed);
    // Without trustobject-current, the only named bundle is used.
    let mut files = fsc2_cache();
    files.retain(|f| f.name != "trustobject-current");
    assert_eq!(calibration(files).unwrap(), resealed(&fsc2()));
    // Two named bundles and no current one.
    let mut files = fsc2_cache();
    files.retain(|f| f.name != "trustobject-current");
    let older = der(0x30, &text("older"));
    files.push(File::new(
        &format!("trustobject-{}", hex(&sha256(&older))),
        older,
    ));
    assert_eq!(calibration(files).unwrap_err(), Refusal::Ambiguous);
    // An FSCl record needs neither.
    assert_eq!(
        calibration(without(fscl_cache(), "trustobject-")).unwrap(),
        fscl()
    );
}

#[test]
fn only_a_complete_checkpoint_is_used() {
    let faults = Faults {
        incomplete_newer: true,
        ..Faults::default()
    };
    let image = container(&layout(fscl_cache()), &faults);
    assert_eq!(read(&image, false).unwrap().calibration.unwrap(), fscl());
    // The newest complete checkpoint wins, and here its object map is bad.
    let faults = Faults {
        complete_newer: true,
        ..Faults::default()
    };
    let image = container(&layout(fscl_cache()), &faults);
    assert_eq!(read(&image, false).err(), Some(Refusal::Malformed));
}

#[test]
fn the_object_map_gives_the_volume_transactions_version() {
    let faults = Faults {
        stale_versions: true,
        ..Faults::default()
    };
    let image = container(&layout(fscl_cache()), &faults);
    assert_eq!(read(&image, false).unwrap().calibration.unwrap(), fscl());
}

#[test]
fn an_oversized_record_is_refused() {
    let mut files = fscl_cache();
    files[0].data = vec![0x30; (1 << 20) + 1];
    assert_eq!(calibration(files).unwrap_err(), Refusal::TooLarge);
}

#[test]
fn the_reader_reads_little() {
    let image = container(&layout(fscl_cache()), &Faults::default());
    read(&image, false).unwrap();
    let reads = *image.reads.borrow();
    assert!(reads < 400, "{reads} reads");
}

/// `SEP_FACTORY_IMAGE=<container or device> SEP_FACTORY_GROUP=<uuid>
/// [SEP_FACTORY_ROOT_KEY=1] tests --ignored real_container` prints what a
/// real container gives, read-only.
#[test]
#[ignore]
fn real_container() {
    let path = std::env::var("SEP_FACTORY_IMAGE").unwrap();
    let group = std::env::var("SEP_FACTORY_GROUP").unwrap().replace('-', "");
    let mut uuid = [0u8; 16];
    for i in 0..16 {
        uuid[i] = u8::from_str_radix(&group[i * 2..i * 2 + 2], 16).unwrap();
    }
    let file = std::fs::File::open(&path).unwrap();
    let size = {
        use std::io::{Seek, SeekFrom};
        let mut f = &file;
        f.seek(SeekFrom::End(0)).unwrap()
    };
    let root_key = std::env::var("SEP_FACTORY_ROOT_KEY").is_ok();
    let device = Device(file, size);
    match sensor_factory::read(&device, &uuid, root_key, &digest) {
        Err(refusal) => println!("refused: {}", refusal.reason()),
        Ok(records) => {
            match &records.calibration {
                Ok(c) => println!(
                    "calibration {} bytes sha256 {} resealed {}",
                    c.len(),
                    hex(&sha256(c)),
                    records.resealed
                ),
                Err(r) => println!("calibration refused: {}", r.reason()),
            }
            match &records.root_key {
                None => println!("root key not asked for"),
                Some(Ok(k)) => println!("root key {} bytes sha256 {}", k.len(), hex(&sha256(k))),
                Some(Err(r)) => println!("root key refused: {}", r.reason()),
            }
        }
    }
}
