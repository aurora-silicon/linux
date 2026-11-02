// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Fixed arithmetic shader for explicit hardware qualification.
pub(crate) const PAGE_BYTES: usize = 0x4000;
pub(crate) const ELEMENTS: usize = 1024;
pub(crate) const SENTINEL: u32 = 0xdead_beef;
pub(crate) const SHADER_PAGE: u64 = 0x100_0000_0000;
pub(crate) const PIPELINE_PAGE: u64 = 0x100_0009_0000;
pub(crate) const STREAM_PAGE: u64 = 0x100_000b_0000;
/// The argument page: the shader loads its input and output pointers from the absolute address
/// `ARGUMENT_PAGE + 0x14a0`.
pub(crate) const ARGUMENT_PAGE: u64 = 0x100_000e_0000;
pub(crate) const BUFFER_PAGE: u64 = 0x100_0001_c000;
pub(crate) const INPUT_OFFSET: usize = 0xd00;
pub(crate) const OUTPUT_OFFSET: usize = 0x1d00;

/// The reference code container (J714, G17S, two clusters): the lead, a helper table per cluster at
/// 0x100 and 0x200, padding, then the CODE block ([`CAPTURED_CODE`]). [`initialize_page`] lays it out
/// for [`CLUSTERS`].
const SHADER: [u32; 256] = [
    0x00000340, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x0e33005e, 0x00000000, 0x00060000, 0x00060006, 0x0e31405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e31005e, 0x00000000, 0x00060000, 0x00060006, 0x0e1b405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e1b005e, 0x00000000, 0x00060000, 0x00060006, 0x0e19405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e19005e, 0x00000000, 0x00060000, 0x00060006, 0x0e13405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e13005e, 0x00000000, 0x00060000, 0x00060006, 0x0e11405e, 0x00000000, 0x00060000, 0x00060006,
    0x0f100007, 0x00270000, 0x00200004, 0x00060aa5, 0x0f100007, 0x00270000, 0x00200004, 0x00060aa5,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x0e33005e, 0x00000000, 0x00060000, 0x00060006, 0x0e31405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e31005e, 0x00000000, 0x00060000, 0x00060006, 0x0e1b405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e1b005e, 0x00000000, 0x00060000, 0x00060006, 0x0e19405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e19005e, 0x00000000, 0x00060000, 0x00060006, 0x0e13405e, 0x00000000, 0x00060000, 0x00060006,
    0x0e13005e, 0x00000000, 0x00060000, 0x00060006, 0x0e11405e, 0x00000000, 0x00060000, 0x00060006,
    0x0f100007, 0x00270000, 0x00200004, 0x00060aa5, 0x0f100007, 0x00270000, 0x00200004, 0x00060aa5,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x000000c0, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x02070023, 0x02a02042, 0x0000000e, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006, 0x00060006,
    0x0610a01c, 0x0203000f, 0xc0102218, 0x00800041, 0x002f0000, 0x00291a04, 0x802802a3, 0x01270024,
    0x00253a04, 0x802002a3, 0x040f0021, 0x06010203, 0x000e4010, 0x00000000, 0x00000000, 0x00000000,
];
const PIPELINE: [u32; 33] = [
    0x0002a02c, 0x00700812, 0x0002803c, 0x00000004, 0x0043042f, 0x40000ce8, 0x0c000007, 0x00000000,
    0x0e4003c7, 0x00000000, 0x00070100, 0x00000d00, 0x00000000, 0x0002801c, 0x00000000, 0x00018134,
    0x02a00020, 0x0002800c, 0x00000004, 0x1a040427, 0x02a30021, 0x00008088, 0x0002801c, 0x00000004,
    0x00040427, 0x12a40030, 0x4409240b, 0x0409261b, 0x0409282b, 0x04092a3b, 0x02070023, 0x02a02042,
    0x0000000e,
];
const STREAM: [u32; 12] = [
    0x00080000, 0x01000000, 0x00002400, 0x40000001, 0x00000400, 1, 1, 0x40, 1, 1, 0x60000160,
    0x40000000,
];
/// The dispatch's workgroup: the stream's grid is `ELEMENTS` x 1 x 1 in workgroups of this size.
pub(crate) const WORKGROUP_THREADS: u32 = STREAM[7];
const _: () = assert!(STREAM[4] as usize == ELEMENTS && STREAM[7] == 0x40);

#[derive(Clone, Copy)]
pub(crate) enum PageKind {
    Shader,
    Pipeline,
    Stream,
    Arguments,
    Buffers,
}
pub(crate) fn initialize_page(kind: PageKind, page: &mut [u8]) -> Result<(), ()> {
    if page.len() != PAGE_BYTES {
        return Err(());
    }
    page.fill(0);
    let (offset, words): (usize, &[u32]) = match kind {
        PageKind::Shader => {
            lay_out_container(page);
            return Ok(());
        }
        PageKind::Pipeline => {
            write_words(page, 0, &PIPELINE);
            page[MAIN_ENTRY_WORD..MAIN_ENTRY_WORD + 4]
                .copy_from_slice(&main_entry_word().to_le_bytes());
            return Ok(());
        }
        PageKind::Stream => (0, &STREAM),
        PageKind::Arguments => {
            page[0x14a0..0x14a8]
                .copy_from_slice(&(BUFFER_PAGE + INPUT_OFFSET as u64).to_le_bytes());
            page[0x14a8..0x14b0]
                .copy_from_slice(&(BUFFER_PAGE + OUTPUT_OFFSET as u64).to_le_bytes());
            return Ok(());
        }
        PageKind::Buffers => {
            for index in 0..ELEMENTS {
                let input = INPUT_OFFSET + index * 4;
                let output = OUTPUT_OFFSET + index * 4;
                page[input..input + 4].copy_from_slice(&(index as u32 * 7).to_le_bytes());
                page[output..output + 4].copy_from_slice(&SENTINEL.to_le_bytes());
            }
            return Ok(());
        }
    };
    write_words(page, offset, words);
    Ok(())
}

fn write_words(page: &mut [u8], offset: usize, words: &[u32]) {
    for (index, word) in words.iter().enumerate() {
        let start = offset + index * 4;
        page[start..start + 4].copy_from_slice(&word.to_le_bytes());
    }
}

/// G17D's USC clusters, numbered 0..7 densely across both dies.
pub(crate) const CLUSTERS: usize = 8;
/// The profile loader calls `SHADER_PAGE + TABLE_BASE + 16 * SR14`, with SR14 = 16 * cluster + core
/// slot, so cluster c's helper table is at `TABLE_BASE + TABLE_BYTES * c`.
const TABLE_BASE: usize = 0x100;
const TABLE_BYTES: usize = 0x100;
/// The lead runs 0x40 past the last table; its first word is its length, and the CODE block follows.
const LEAD_BYTES: usize = TABLE_BASE + CLUSTERS * TABLE_BYTES + 0x40;
/// The reference helper table, padding and CODE block (header, constant program, main at +0x80).
const CAPTURED_TABLE: core::ops::Range<usize> = 0x100..0x200;
const CAPTURED_PADDING: core::ops::Range<usize> = 0x300..0x340;
const CAPTURED_CODE: core::ops::Range<usize> = 0x340..0x400;
const MAIN_OFFSET: usize = LEAD_BYTES + 0x80;
/// The pipeline word holding LDSHDR's shader address bits 6..15, in the state tail at PIPELINE[6..13].
const MAIN_ENTRY_WORD: usize = 8 * 4;
const _: () = assert!(
    LEAD_BYTES + CAPTURED_CODE.end - CAPTURED_CODE.start <= PAGE_BYTES
        && MAIN_OFFSET % 0x40 == 0
        && MAIN_OFFSET < 0x10000
);

/// The reference G17S pipeline's word 8 for the relocated main entry: 0x0e40_0007 with the entry's
/// address bits 6..15, as Mesa's agx_ioto_state_tail_pack packs LDSHDR f0 (it gives the reference
/// 0x0e40_03c7 for 0x3c0, and SHADER_PAGE's upper bits sit in word 10).
const fn main_entry_word() -> u32 {
    0x0e40_0007 | (MAIN_OFFSET as u32 & 0xffc0)
}

/// Lays the reference container out for [`CLUSTERS`]: the lead with its length, a copy of the
/// helper table at every cluster's slot, the padding, then the CODE block.
fn lay_out_container(page: &mut [u8]) {
    let mut captured = [0u8; 0x400];
    write_words(&mut captured, 0, &SHADER);
    page[..TABLE_BASE].copy_from_slice(&captured[..TABLE_BASE]);
    page[..4].copy_from_slice(&(LEAD_BYTES as u32).to_le_bytes());
    for cluster in 0..CLUSTERS {
        let table = TABLE_BASE + cluster * TABLE_BYTES;
        page[table..table + TABLE_BYTES].copy_from_slice(&captured[CAPTURED_TABLE]);
    }
    page[LEAD_BYTES - 0x40..LEAD_BYTES].copy_from_slice(&captured[CAPTURED_PADDING]);
    page[LEAD_BYTES..LEAD_BYTES + CAPTURED_CODE.len()].copy_from_slice(&captured[CAPTURED_CODE]);
}
