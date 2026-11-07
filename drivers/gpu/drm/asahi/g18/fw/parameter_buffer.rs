// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 (26A428) parameter buffer: PB state object, page list and PB-table row.
//!
//! G18 uses AGXParameterManagementVirtual (halNewParameterManager
//! 0xfffffe0008c50c78). Each PB block is 0x80000 bytes (PM +0x70, constant
//! 0xfffffe000714f800) of 32 KiB pages; the page list holds one u32 page
//! number (user GPU VA >> 15) per page. The encoders below reproduce, byte for
//! byte, the original writers executed by the native parameter-buffer audit:
//! AGXHWParamBufferManager::resetHWParamBufferState (0xfffffe0008c5e06c),
//! AGXParameterManagement::buildPageList (0xfffffe0008c5dc98) and
//! AGXParameterManagement::loadPBDesc (0xfffffe0008c60918) with the HAL400
//! halLoadPBDesc (0xfffffe0008c40b40).
//!
//! Pure encoders only: nothing here allocates, maps or publishes. The kernel
//! owner (G18ParameterBuffer) lives in g18_graphics_queues.rs, which is a
//! g17_resources submodule and can use its MappedObject.

#![allow(dead_code)]

/// Native InternalResource size for the PB state (createNewHWPBManager
/// 0xfffffe0008c5fd90). Firmware caches the first 0x90 bytes (b000 0x13764)
/// and writes the GPU context ID to +0x08.
pub(crate) const G18_PB_STATE_BYTES: usize = 0xc0;
pub(crate) const G18_PB_STATE_FIRMWARE_BYTES: usize = 0x90;
pub(crate) const G18_PB_ROW_BYTES: usize = 16;
/// Native 0x4000-byte PB table (g18_resources.rs PBDESC_TABLE_SIZE): 0x400 rows.
pub(crate) const G18_PB_TABLE_ROWS: u32 = 0x400;
pub(crate) const G18_PB_BLOCK_BYTES: u64 = 0x80000;
pub(crate) const G18_PB_PAGE_BYTES: u64 = 0x8000;
pub(crate) const G18_PB_PAGES_PER_BLOCK: u32 = (G18_PB_BLOCK_BYTES / G18_PB_PAGE_BYTES) as u32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum G18PbError {
    Granule,
    BlockIndex,
    PageListTooSmall,
    Slot,
}

/// Parameter-management fields consumed by the state/page-list writers.
/// Names are native AGXParameterManagement offsets.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18PmConfig {
    /// Block ring capacity (+0x148, u32): accelerator PB maximum >> 19.
    pub pm_148: u32,
    /// Current PB bytes (+0x138); state +0x34 = (pm_138 >> 19) * pm_1d8.
    pub pm_138: u64,
    /// Pages per block (+0x1d8; device PM config word 0, 0x10 on G18).
    pub pm_1d8: u32,
    /// Block granule (+0x70), 0x80000 on G18; state +0x58.
    pub pm_70: u64,
    pub pm_c0: u64,
    pub pm_c8: u64,
    /// State +0x80 = pm_154 << 4; +0x7c = pm_15c << 4.
    pub pm_154: u32,
    pub pm_15c: u32,
    /// State +0x10 and TA command +0x1c.
    pub pm_1d4: u32,
}

/// Addresses and ring positions of one PB state.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18PbStateInputs {
    pub config: G18PmConfig,
    /// State +0x20: firmware VA of the page list (converted, identity on G18).
    pub page_list_firmware: u64,
    /// State +0x28: GPU VA of the page list; also encoded in the PB row.
    pub page_list_gpu: u64,
    /// State +0x30: page-list resource length word (resource +0x78).
    pub page_list_bytes: u32,
    /// State +0x64: GPU VA of the 4-byte submitted-operations counter.
    pub counter_gpu: u64,
    /// State +0x84: GPU VA of the 0x8000-byte discard page.
    pub discard_gpu: u64,
    /// PB table slot (TA HardwareBufferID), state +0x0c.
    pub slot: u32,
    /// Block ring positions: first block (PM +0x98 -> +8) and staged end.
    pub start_block: u32,
    pub end_block: u32,
}

/// AGXSPBDescRingState, the HWPB manager's +0x34..+0x44 words.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18PbRing {
    pub words: [u32; 4],
}

fn put32(out: &mut [u8], offset: usize, value: u32) {
    out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put64(out: &mut [u8], offset: usize, value: u64) {
    out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn get32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

/// PB state after the native first-TA sequence: resetHWParamBufferState,
/// buildPageList and (when the manager was not loaded before) loadPBDesc.
/// Bytes +0x08..+0x0c (firmware-owned context), +0x1c..+0x20 and
/// +0x90..+0xc0 are not written, as native.
pub(crate) fn encode_g18_pb_state(
    out: &mut [u8; G18_PB_STATE_BYTES],
    p: &G18PbStateInputs,
    load: bool,
) {
    let c = &p.config;
    // resetHWParamBufferState 0x8c5e094..0x8c5e1e4.
    put32(out, 0x38, c.pm_148);
    put64(out, 0x3c, 0);
    put64(out, 0x44, c.pm_c0);
    put64(out, 0x4c, c.pm_c8);
    put64(out, 0x20, p.page_list_firmware);
    put64(out, 0x28, p.page_list_gpu);
    put32(out, 0x30, p.page_list_bytes);
    put32(out, 0x34, 0);
    put64(out, 0x5c, 0);
    put32(out, 0x54, u32::MAX);
    put32(out, 0x58, c.pm_70 as u32);
    put64(out, 0x64, p.counter_gpu);
    put64(out, 0, 0);
    out[0x6c..0x7c].fill(0);
    put32(out, 0x8c, 0);
    put64(out, 0x14, 0);
    put32(out, 0x80, c.pm_154.wrapping_shl(4));
    put32(out, 0x7c, c.pm_15c.wrapping_shl(4));
    put64(out, 0x84, p.discard_gpu);
    put32(out, 0x10, c.pm_1d4);
    // buildPageList 0x8c5dd54..0x8c5dd74 and 0x8c5df3c.
    let pages = g18_pb_page_count(c);
    put32(out, 0x34, pages);
    put32(out, 0x54, pages.wrapping_sub(1));
    put32(out, 0x3c, p.end_block);
    put32(out, 0x40, p.start_block);
    // loadPBDesc 0x8c609b8 on a manager that was never loaded.
    if load {
        put32(out, 0x0c, p.slot);
    }
}

/// State +0x34, the PB page count encoded in the row.
pub(crate) fn g18_pb_page_count(c: &G18PmConfig) -> u32 {
    ((c.pm_138 >> 19) as u32).wrapping_mul(c.pm_1d8)
}

/// buildPageList, G18 fast path (granule 0x80000): 16 consecutive page
/// numbers per ring slot from `start` up to, not including, `end` (mod the
/// ring capacity), written from the start of the list. `page_bases[slot]` is
/// that slot's block page base (block user GPU VA >> 15, 25 bits).
pub(crate) fn g18_pb_page_list(
    out: &mut [u8],
    c: &G18PmConfig,
    page_bases: &[u32],
    start: u32,
    end: u32,
) -> Result<usize, G18PbError> {
    if c.pm_70 != G18_PB_BLOCK_BYTES {
        return Err(G18PbError::Granule);
    }
    if start >= c.pm_148 || end >= c.pm_148 || page_bases.len() < c.pm_148 as usize {
        return Err(G18PbError::BlockIndex);
    }
    let mut slot = start;
    let mut offset = 0usize;
    while slot != end {
        let base = page_bases[slot as usize];
        for page in 0..G18_PB_PAGES_PER_BLOCK {
            let entry = out.get_mut(offset..offset + 4).ok_or(G18PbError::PageListTooSmall)?;
            entry.copy_from_slice(&base.wrapping_add(page).to_le_bytes());
            offset += 4;
        }
        slot = (slot + 1) % c.pm_148;
    }
    Ok(offset / 4)
}

/// halLoadPBDesc row. `page_list_gpu`/`page_count` are PB state +0x28/+0x34
/// at load time; `ring` is the manager's +0x34..+0x44 words (all zero after
/// buildPageList, so zero for every first TA). `old` is the row's previous
/// contents: native keeps bits 1..2 of word 0, 22..27 of word 1, 22..31 of
/// word 2 and 22..30 of word 3.
pub(crate) fn g18_pb_row(
    page_list_gpu: u64,
    page_count: u32,
    ring: G18PbRing,
    old: [u8; G18_PB_ROW_BYTES],
) -> [u8; G18_PB_ROW_BYTES] {
    let list = page_list_gpu;
    let rebase = if list & (1 << 42) == 0 { 0x70_0000_0000 } else { 0 };
    let segment = (rebase + list) & 0x70_0000_0000 | (list >> 3) & 0x80_0000_0000;
    let old = [get32(&old, 0), get32(&old, 4), get32(&old, 8), get32(&old, 12)];
    let word0 = ((list >> 4) as u32 & !7) | (old[0] & 7);
    let word0 = (word0 & !1) | u32::from(ring.words[3] != 0);
    let word1 = ((segment >> 8) as u32 & !0x0fff_ffff) | (old[1] & 0x0fff_ffff);
    let word1 = (word1 & !0x3f_ffff) | (page_count & 0x3f_ffff);
    let word2 = (old[2] & !0x3f_ffff) | (ring.words[0] & 0x3f_ffff);
    let word3 = (old[3] & !0x3f_ffff) | (ring.words[1] & 0x3f_ffff);
    let word3 = (word3 & !(1 << 31)) | ((ring.words[2] & 1) << 31);
    let mut row = [0; G18_PB_ROW_BYTES];
    for (index, word) in [word0, word1, word2, word3].into_iter().enumerate() {
        put32(&mut row, index * 4, word);
    }
    row
}

/// A page-list VA without bit 42 is stored minus 64 GiB in a 3-bit segment,
/// so only [64 GiB, 512 GiB) round-trips; stage33c's 0x200_0000 decoded to
/// 0x80_0200_0000 and faulted in the PM (see the H1 parameter-buffer fault notes).
pub(crate) const fn g18_pb_row_address_valid(page_list_gpu: u64) -> bool {
    page_list_gpu & 0x7f == 0
        && page_list_gpu & (1 << 42) == 0
        && page_list_gpu >= 0x10_0000_0000
        && page_list_gpu < 0x80_0000_0000
}

/// The page-list VA the Parameter Manager reads back from a row with word 1
/// bit 31 clear. `None` for the bit-42 form, which Linux never programs.
pub(crate) fn g18_pb_row_decoded_address(row: &[u8; G18_PB_ROW_BYTES]) -> Option<u64> {
    let (word0, word1) = (get32(row, 0), get32(row, 4));
    if word1 & (1 << 31) != 0 {
        return None;
    }
    Some(((u64::from(word1 >> 28) & 7) << 36) + (u64::from(word0 & !7) << 4) + 0x10_0000_0000)
}
