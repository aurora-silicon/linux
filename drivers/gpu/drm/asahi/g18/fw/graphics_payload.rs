// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounded private pre-submit color capture for G18 graphics bring-up.
//!
//! This reader preserves user virtual pages and the userspace render segment.
//! It neither creates a GPU VM nor submits a job. Native TA/3D command builders,
//! kernel-owned graphics resources and completion remain separate prerequisites.
//! Resource usage bits are capture metadata, NOT native PTE permissions.

#![allow(dead_code)]

pub(crate) const HEADER_BYTES: usize = 64;
pub(crate) const PAGE_BYTES: usize = 0x4000;
pub(crate) const RECORD_BYTES: usize = 16 + PAGE_BYTES;
pub(crate) const COMMAND_BYTES: usize = 0xac0;
pub(crate) const GEARS_COMMAND_BYTES: usize = 0xad8;
pub(crate) const PAGE_COUNT: usize = 214;
pub(crate) const GEARS_PAGE_COUNT: usize = 231;
pub(crate) const WITNESS_PAGE_COUNT: usize = 215;
pub(crate) const WITNESS: u64 = 0x100_000a_0000;
pub(crate) const WITNESS_DATA_BYTES: usize = 4 + 256 * 256 / 8;
pub(crate) const OUTPUT: u64 = 0x100_0005_8000;
pub(crate) const OUTPUT_BYTES: u64 = 256 * 256 * 4;

// Root allocation address, length, and captured usage for submission 1 in the
// two independent native color runs. Their shader heaps need not be identical.
const RANGES: [(u64, usize, u32); 24] = [
    (0, 0x10000, 128),
    (0x18000, 0x8000, 1), (0x28000, 0x8000, 1),
    (0x38000, 0x8000, 1), (0x48000, 0x8000, 1),
    (0x58000, 0x8000, 1024), (0x68000, 0xc000, 1024),
    (0x100_0000_0000, 0x10000, 128),
    (0x100_0001_8000, 0x20000, 129),
    (0x100_0004_0000, 0x10000, 128), (OUTPUT, 0x40000, 12),
    (0x100_000a_0000, 0x20000, 128),
    (0x100_000c_8000, 0x6c000, 128),
    (0x100_0013_8000, 0x8000, 1),
    (0x100_0014_8000, 0x8000, 4),
    (0x100_0015_8000, 0x8000, 1),
    (0x100_0016_8000, 0x8000, 4),
    (0x100_0017_8000, 0x100000, 4),
    (0x100_0028_0000, 0x8000, 4),
    (0x100_0029_0000, 0x8000, 1),
    (0x100_002a_0000, 0x8000, 1),
    (0x100_002b_0000, 0x8000, 1),
    (0x100_002c_0000, 0x8000, 1),
    (0x100_002d_0000, 0xc0000, 4),
];

const GEARS_RANGES: [(u64, usize, u32); 25] = [
    (0, 0x10000, 128), (0x18000, 0x8000, 1), (0x28000, 0x8000, 1),
    (0x38000, 0x8000, 1), (0x48000, 0x8000, 1), (0x58000, 0x8000, 1024),
    (0x68000, 0xc000, 1024), (0x100_0000_0000, 0x10000, 128),
    (0x100_0001_8000, 0x20000, 129), (0x100_0004_0000, 0x10000, 128),
    (OUTPUT, 0x40000, 12), (0x100_000a_0000, 0x20000, 128),
    (0x100_000c_8000, 0x44000, 12), (0x100_0011_0000, 0x6c000, 128),
    (0x100_0018_0000, 0x8000, 1), (0x100_0019_0000, 0x8000, 4),
    (0x100_001a_0000, 0x8000, 1), (0x100_001b_0000, 0x8000, 4),
    (0x100_001c_0000, 0x100000, 4), (0x100_002c_8000, 0x8000, 4),
    (0x100_002d_8000, 0x8000, 1), (0x100_002e_8000, 0x8000, 1),
    (0x100_002f_8000, 0x8000, 1), (0x100_0030_8000, 0x8000, 1),
    (0x100_0031_8000, 0xc0000, 4),
];

// Separate native fragment-witness capture: its extra 16-KiB buffer shifts
// every subsequent pipeline allocation. Never mix these with colour-a/b.
const WITNESS_RANGES: [(u64, usize, u32); 25] = [
    (0, 0x10000, 128), (0x18000, 0x8000, 1), (0x28000, 0x8000, 1),
    (0x38000, 0x8000, 1), (0x48000, 0x8000, 1), (0x58000, 0x8000, 1024),
    (0x68000, 0xc000, 1024), (0x100_0000_0000, 0x10000, 128),
    (0x100_0001_8000, 0x20000, 129), (0x100_0004_0000, 0x10000, 128),
    (OUTPUT, 0x40000, 12), (WITNESS, 0x4000, 12),
    (0x100_000a_8000, 0x20000, 128), (0x100_000d_0000, 0x6c000, 128),
    (0x100_0014_0000, 0x8000, 1), (0x100_0015_0000, 0x8000, 4),
    (0x100_0016_0000, 0x8000, 1), (0x100_0017_0000, 0x8000, 4),
    (0x100_0018_0000, 0x100000, 4), (0x100_0028_8000, 0x8000, 4),
    (0x100_0029_8000, 0x8000, 1), (0x100_002a_8000, 0x8000, 1),
    (0x100_002b_8000, 0x8000, 1), (0x100_002c_8000, 0x8000, 1),
    (0x100_002d_8000, 0xc0000, 4),
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Capture { ColorA = 1, ColorB = 2, GearsG1 = 3, FragmentWitness = 4 }

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum PayloadError { Length, Header, Checksum, Page, Command, Output, Witness }

pub(crate) struct GraphicsPayload<'a> {
    bytes: &'a [u8], capture: Capture, command_bytes: usize, page_count: usize,
}
pub(crate) struct PayloadPage<'a> {
    pub(crate) address: u64,
    pub(crate) usage: u32,
    pub(crate) bytes: &'a [u8],
}

fn u32_at(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(b[off..off + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes(b[off..off + 8].try_into().unwrap())
}

/// Transfer integrity only; the offline receipt carries SHA256 provenance.
pub(crate) fn fnv64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, b|
        (h ^ u64::from(*b)).wrapping_mul(0x100_0000_01b3))
}

impl<'a> GraphicsPayload<'a> {
    pub(crate) fn parse(bytes: &'a [u8]) -> Result<Self, PayloadError> {
        if bytes.len() < HEADER_BYTES { return Err(PayloadError::Length); }
        let capture = match u64_at(bytes, 56) {
            1 => Capture::ColorA, 2 => Capture::ColorB, 3 => Capture::GearsG1,
            4 => Capture::FragmentWitness,
            _ => return Err(PayloadError::Header),
        };
        let gears = capture == Capture::GearsG1;
        let witness = capture == Capture::FragmentWitness;
        let command_bytes = if gears || witness { GEARS_COMMAND_BYTES } else { COMMAND_BYTES };
        let page_count = if gears { GEARS_PAGE_COUNT } else if witness { WITNESS_PAGE_COUNT } else { PAGE_COUNT };
        let size = HEADER_BYTES + command_bytes + page_count * RECORD_BYTES;
        if bytes.len() != size { return Err(PayloadError::Length); }
        if &bytes[..8] != b"G18GFX01" || u32_at(bytes, 8) != 2
            || u32_at(bytes, 12) != HEADER_BYTES as u32
            || u32_at(bytes, 16) != PAGE_BYTES as u32
            || u32_at(bytes, 20) != page_count as u32
            || u32_at(bytes, 24) != 256 || u32_at(bytes, 28) != 256
            || u32_at(bytes, 32) != 1024
            || u32_at(bytes, 36) != command_bytes as u32
            || u64_at(bytes, 40) != OUTPUT
        { return Err(PayloadError::Header); }
        if fnv64(&bytes[HEADER_BYTES..]) != u64_at(bytes, 48) {
            return Err(PayloadError::Checksum);
        }
        let p = Self { bytes, capture, command_bytes, page_count };
        let cmd = p.command();
        // Fixture identity is bound to the actual userspace command; the two
        // colour shader heaps must never be interchanged by relabelling a file.
        let command_fnv = match capture {
            Capture::ColorA => 0x89e7_aca5_9572_cb78,
            Capture::ColorB => 0x75e0_2b53_1fcc_46f2,
            Capture::GearsG1 => 0x23d3_27af_95ce_9eb7,
            Capture::FragmentWitness => 0x5eed_ef2d_0bb3_31af,
        };
        if u32_at(cmd, 0) != 0x10000 || u32_at(cmd, 4) != command_bytes as u32
            || u64_at(cmd, command_bytes - 24) != OUTPUT || fnv64(cmd) != command_fnv
        { return Err(PayloadError::Command); }
        let mut index = 0;
        let mut output_pages = 0;
        let ranges: &[(u64, usize, u32)] = if gears { &GEARS_RANGES }
            else if witness { &WITNESS_RANGES } else { &RANGES };
        for &(base, length, usage) in ranges {
            for offset in (0..length).step_by(PAGE_BYTES) {
                let page = p.page(index).ok_or(PayloadError::Page)?;
                let record = HEADER_BYTES + command_bytes + index * RECORD_BYTES;
                if page.address != base + offset as u64 || page.usage != usage
                    || u32_at(bytes, record + 12) != PAGE_BYTES as u32
                { return Err(PayloadError::Page); }
                if (OUTPUT..OUTPUT + OUTPUT_BYTES).contains(&page.address) {
                    if page.bytes.iter().any(|&b| b != 0xa5) {
                        return Err(PayloadError::Output);
                    }
                    output_pages += 1;
                }
                if witness && page.address == WITNESS
                    && (page.bytes[..WITNESS_DATA_BYTES].iter().any(|&b| b != 0)
                        || page.bytes[WITNESS_DATA_BYTES..].iter().any(|&b| b != 0xa5))
                { return Err(PayloadError::Witness); }
                index += 1;
            }
        }
        if index != page_count || output_pages != 16 { return Err(PayloadError::Page); }
        Ok(p)
    }

    pub(crate) fn capture(&self) -> Capture { self.capture }
    pub(crate) fn body_digest(&self) -> u64 { u64_at(self.bytes, 48) }
    pub(crate) fn page_count(&self) -> usize { self.page_count }
    pub(crate) fn command(&self) -> &'a [u8] {
        &self.bytes[HEADER_BYTES..HEADER_BYTES + self.command_bytes]
    }
    pub(crate) fn page(&self, index: usize) -> Option<PayloadPage<'a>> {
        if index >= self.page_count { return None; }
        let off = HEADER_BYTES + self.command_bytes + index * RECORD_BYTES;
        Some(PayloadPage { address: u64_at(self.bytes, off), usage: u32_at(self.bytes, off + 8),
            bytes: &self.bytes[off + 16..off + RECORD_BYTES] })
    }
}
