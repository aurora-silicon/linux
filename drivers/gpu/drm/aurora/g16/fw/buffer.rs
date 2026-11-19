// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4 parameter-buffer and UMA wire images. This module only builds fresh private state;
//! firmware-mutated managers, descriptor completions and lists must survive reuse.

pub(crate) const PAGE_BYTES: u64 = 0x8000;
pub(crate) const PAGES_PER_BLOCK: u32 = 4;
pub(crate) const BLOCK_BYTES: u64 = 0x20000;
pub(crate) const MAX_BLOCKS: u32 = 2048;
pub(crate) const MAX_PAGES: u32 = 8192;
pub(crate) const BLOCK_LIST_RECORDS: u32 = 4096;
pub(crate) const PAGE_LIST_SIZE: usize = 0x8000;
pub(crate) const BLOCK_LIST_SIZE: usize = 0x8000;
pub(crate) const BLOCK_RECORD_BYTES: usize = 8;
pub(crate) const GROW_STEP: u32 = 10;
pub(crate) const MIN_BLOCKS: u32 = 64;
pub(crate) const COMPACT_BASE: u64 = 0x1000000000;
pub(crate) const SCENE_LIST_SIZE: usize = 0x4000;
pub(crate) const fn compact(gva: u64) -> u64 {
    gva.wrapping_sub(COMPACT_BASE)
}
pub(crate) const fn page_record(heap: u64, page: u32) -> u32 {
    (compact(heap + page as u64 * PAGE_BYTES) / PAGE_BYTES) as u32
}
pub(crate) const fn block_record(heap: u64, block: u32) -> u32 {
    (compact(heap + block as u64 * BLOCK_BYTES) / PAGE_BYTES) as u32
}
pub(crate) const fn block_record_offset(block: u32) -> usize {
    BLOCK_RECORD_BYTES * (block % BLOCK_LIST_RECORDS) as usize
}

pub(crate) mod pb_manager {
    pub(crate) const SIZE: usize = 0xc0;
    pub(crate) const ID_A: usize = 0xc;
    pub(crate) const ID_B: usize = 0x10;
    pub(crate) const PAGE_LIST_FW: usize = 0x20;
    pub(crate) const PAGE_LIST: usize = 0x28;
    pub(crate) const PAGE_LIST_BYTES: usize = 0x30;
    pub(crate) const PAGES: usize = 0x34;
    pub(crate) const K: usize = 0x38;
    pub(crate) const BLOCKS: usize = 0x3c;
    pub(crate) const BLOCK_LIST: usize = 0x44;
    pub(crate) const BLOCK_CTL: usize = 0x4c;
    pub(crate) const LAST_PAGE: usize = 0x54;
    pub(crate) const BLOCK_BYTES: usize = 0x58;
    pub(crate) const COUNTER: usize = 0x64;
    pub(crate) const MAX_PAGES_A: usize = 0x7c;
    pub(crate) const MAX_PAGES_B: usize = 0x80;
}
pub(crate) mod uma_manager {
    pub(crate) const SIZE: usize = 0xa0;
    pub(crate) const COMPLETED: usize = 0x54;
    pub(crate) const SEEDS: u32 = 8192;
}
pub(crate) mod uma_pools {
    pub(crate) const POOLS: usize = 16;
    pub(crate) const POOL_SIZE: u64 = 0x200000;
    pub(crate) const SEED_BYTES: u64 = 0x1000;
    pub(crate) const FREELIST_SIZE: usize = 0x20000;
    pub(crate) const POOL_TABLE_SIZE: usize = 0x4000;
    pub(crate) const POOL_TABLE_STRIDE: usize = 0x40;
    pub(crate) const fn row(pool: u64) -> u64 {
        pool | (1 << 61)
    }
}
pub(crate) mod block_ctl {
    pub(crate) const SIZE: usize = 8;
    pub(crate) const BLOCKS: usize = 0;
    pub(crate) const PRODUCER: usize = 4;
}
pub(crate) mod misc {
    pub(crate) const SIZE: usize = 0x40;
}
pub(crate) mod counter {
    pub(crate) const SIZE: usize = 0x40;
}

fn put32(x: &mut [u8], off: usize, v: u32) {
    x[off..off + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(x: &mut [u8], off: usize, v: u64) {
    x[off..off + 8].copy_from_slice(&v.to_le_bytes());
}

/// Literal fresh M4 PB manager. The firmware and GPU page-list aliases share backing.
#[allow(clippy::too_many_arguments)]
pub(crate) fn pb_manager_image(
    id: u32,
    pages_fw: u64,
    pages_gpu: u64,
    blocks_fw: u64,
    ring_fw: u64,
    count_fw: u64,
) -> [u8; pb_manager::SIZE] {
    let mut m = [0; pb_manager::SIZE];
    put32(&mut m, 0xc, id);
    put32(&mut m, 0x10, id);
    put64(&mut m, 0x20, pages_fw);
    put64(&mut m, 0x28, pages_gpu);
    put32(&mut m, 0x30, PAGE_LIST_SIZE as u32);
    put32(&mut m, 0x34, MAX_PAGES);
    put32(&mut m, 0x38, BLOCK_LIST_RECORDS);
    put32(&mut m, 0x3c, MAX_BLOCKS);
    put64(&mut m, 0x44, blocks_fw);
    put64(&mut m, 0x4c, ring_fw);
    put32(&mut m, 0x54, MAX_PAGES - 1);
    put32(&mut m, 0x58, BLOCK_BYTES as u32);
    put64(&mut m, 0x64, count_fw);
    put32(&mut m, 0x7c, MAX_PAGES);
    put32(&mut m, 0x80, MAX_PAGES);
    m
}

/// One retained M4 UMA descriptor shared by TA and FS (slot 33..64).
pub(crate) fn uma_manager_image(
    descriptor: u64,
    slot: u32,
    state_gpu: u64,
    list_gpu: u64,
    submitted_fw: u64,
) -> [u8; uma_manager::SIZE] {
    let mut m = [0; uma_manager::SIZE];
    put64(&mut m, 0, descriptor);
    put32(&mut m, 8, slot);
    put32(&mut m, 0x10, 2);
    put64(&mut m, 0x14, state_gpu);
    put32(&mut m, 0x1c, 16384);
    put32(&mut m, 0x24, 8192);
    put32(&mut m, 0x2c, 8192);
    put64(&mut m, 0x30, list_gpu);
    put32(&mut m, 0x40, 4);
    put32(&mut m, 0x48, 128);
    put64(&mut m, 0x4c, submitted_fw);
    // +0x54 is the ASC completion count; +0x5c is the compute-exclusion flag.
    // Both start at zero for paired render engines and are never AP command serials.
    put32(&mut m, 0x84, 1);
    put32(&mut m, 0x94, 2);
    m
}

#[cfg(test)]
mod tests {
    use super::*;
    fn r32(x: &[u8], o: usize) -> u32 {
        u32::from_le_bytes(x[o..o + 4].try_into().unwrap())
    }
    fn r64(x: &[u8], o: usize) -> u64 {
        u64::from_le_bytes(x[o..o + 8].try_into().unwrap())
    }
    #[test]
    fn exact_m4_pb_fields() {
        let m = pb_manager_image(
            1,
            0xfffffc2000400000,
            0x701a040000,
            0xfffffc2000500000,
            0xfffffc2000600000,
            0xfffffc2000700000,
        );
        assert_eq!(m.len(), 0xc0);
        assert_eq!((r32(&m, 0xc), r32(&m, 0x10)), (1, 1));
        assert_eq!(r64(&m, 0x20), 0xfffffc2000400000);
        assert_eq!(r64(&m, 0x28), 0x701a040000);
        assert_eq!(
            (r32(&m, 0x30), r32(&m, 0x34), r32(&m, 0x38), r32(&m, 0x3c)),
            (0x8000, 8192, 4096, 2048)
        );
        assert_eq!(r64(&m, 0x64), 0xfffffc2000700000);
        assert_eq!(r32(&m, 0x54), 8191);
        assert_eq!(r32(&m, 0x7c), 8192);
        assert_eq!(r32(&m, 0x80), 8192);
        let written = [
            (0xc, 8),
            (0x20, 16),
            (0x30, 16),
            (0x44, 16),
            (0x54, 8),
            (0x64, 8),
            (0x7c, 8),
        ];
        for (i, &b) in m.iter().enumerate() {
            if !written.iter().any(|&(o, n)| i >= o && i < o + n) {
                assert_eq!(b, 0);
            }
        }
    }
    #[test]
    fn exact_m4_uma_and_lists() {
        let m = uma_manager_image(
            0xfffffc2000400000,
            33,
            0x7012000000,
            0x7012020000,
            0xfffffc2000700000,
        );
        assert_eq!(r64(&m, 0), 0xfffffc2000400000);
        assert_eq!(r32(&m, 8), 33);
        assert_eq!(r64(&m, 0x14), 0x7012000000);
        assert_eq!(r64(&m, 0x30), 0x7012020000);
        assert_eq!(r64(&m, 0x4c), 0xfffffc2000700000);
        assert_eq!(r64(&m, 0x54), 0);
        assert_eq!(r32(&m, 0x5c), 0);
        assert_eq!(uma_pools::row(0x7010000000), 0x2000007010000000);
        assert_eq!(MAX_PAGES as usize * 4, PAGE_LIST_SIZE);
        assert_eq!(BLOCK_LIST_RECORDS as usize * 8, BLOCK_LIST_SIZE);
        assert_eq!(page_record(0x70c0000000, 8191), 0xc18000 + 8191);
        assert_eq!(block_record(0x70c0000000, 2047), 0xc18000 + 4 * 2047);
    }
}
