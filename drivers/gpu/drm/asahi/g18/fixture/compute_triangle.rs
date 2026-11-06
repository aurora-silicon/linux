// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Fixed, private compute-triangle patch and post-completion pixel checks.
//! Failure model: the triangle plan, section 2.7. This format only
//! patches four known sites in an unpublished, verified compute-add VM.

use crate::g18::fixture::compute_payload::{fnv64, PAGE_BYTES, POISON};

pub(crate) const PATCH_COUNT: usize = 4;
pub(crate) const OUTPUT_FNV: u64 = 0x5a2c_fd9c_8627_b035;
const BASE_FNV: u64 = 0x4263_044c_89a6_b244;
const PATCH_FNV: u64 = 0xe40f_1f0c_5b96_cb69;
const HEADER_BYTES: usize = 64;
const PATCH_BYTES: usize = 16574;
// Address, length, original bytes' FNV. Patch provenance is SHA256-pinned in
// the host build receipt; these checks reject damaged or different payloads.
const SITES: [(u64, usize, u64); PATCH_COUNT] = [
    (0x100_0000_8000, PAGE_BYTES, 0x9c1b_da7f_8c87_2325),
    (0x100_000a_8038, 10, 0x3def_155f_b602_ffc3),
    (0x100_000a_8042, 12, 0xa522_f63b_a76a_995c),
    (0x100_0001_8020, 8, 0xa8c7_f832_281a_39c5),
];

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
fn u64_at(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

pub(crate) struct TrianglePatch<'a> { bytes: &'a [u8] }
pub(crate) struct PatchSite<'a> {
    pub(crate) address: u64,
    pub(crate) before_fnv: u64,
    pub(crate) bytes: &'a [u8],
}

impl<'a> TrianglePatch<'a> {
    pub(crate) fn parse(bytes: &'a [u8], base_fnv: u64) -> Option<Self> {
        if bytes.len() != PATCH_BYTES || &bytes[..8] != b"G18TRI01"
            || u32_at(bytes, 8) != 1 || u32_at(bytes, 12) != HEADER_BYTES as u32
            || u32_at(bytes, 16) != PATCH_COUNT as u32
            || u32_at(bytes, 20) != 64 || u32_at(bytes, 24) != 64
            || u32_at(bytes, 28) != 1568 || u64_at(bytes, 32) != OUTPUT_FNV
            || base_fnv != BASE_FNV || u64_at(bytes, 40) != BASE_FNV
            || u64_at(bytes, 48) != fnv64(&bytes[HEADER_BYTES..])
            || u64_at(bytes, 56) != 0 || fnv64(bytes) != PATCH_FNV {
            return None;
        }
        let mut cursor = HEADER_BYTES;
        for (address, length, before_fnv) in SITES {
            let end = cursor.checked_add(24 + length)?;
            if end > bytes.len() || u64_at(bytes, cursor) != address
                || u32_at(bytes, cursor + 8) as usize != length
                || u32_at(bytes, cursor + 12) != 0
                || u64_at(bytes, cursor + 16) != before_fnv {
                return None;
            }
            cursor = end;
        }
        if cursor != bytes.len() { return None; }
        Some(Self { bytes })
    }

    pub(crate) fn site(&self, index: usize) -> Option<PatchSite<'a>> {
        if index >= PATCH_COUNT { return None; }
        let cursor = HEADER_BYTES + SITES[..index].iter().map(|s| 24 + s.1).sum::<usize>();
        let (address, length, before_fnv) = SITES[index];
        Some(PatchSite { address, before_fnv, bytes: &self.bytes[cursor + 24..cursor + 24 + length] })
    }
}

#[derive(Debug)]
pub(crate) struct TriangleResult {
    pub(crate) digest: u64,
    pub(crate) foreground: usize,
    pub(crate) background: usize,
    pub(crate) untouched: usize,
    pub(crate) zero: usize,
}

impl TriangleResult {
    /// Only interpret after independent completion; this function neither
    /// generates pixels nor establishes GPU execution by itself.
    pub(crate) fn inspect(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != PAGE_BYTES { return None; }
        let mut result = Self { digest: fnv64(bytes), foreground: 0, background: 0,
                                untouched: 0, zero: 0 };
        for word in bytes.chunks_exact(4) {
            match u32_at(word, 0) {
                POISON => result.untouched += 1,
                0 => result.zero += 1,
                0xff20_2020 => result.background += 1,
                _ => result.foreground += 1,
            }
        }
        Some(result)
    }

    pub(crate) fn passed(&self) -> bool {
        self.digest == OUTPUT_FNV && self.foreground == 1568 && self.background == 2528
            && self.untouched == 0 && self.zero == 0
    }
}
