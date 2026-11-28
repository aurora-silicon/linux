// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Wire geometry for the shared parameter-buffer owner and growth machinery.

use super::{cfg, fw, BlockGeometry};
use kernel::prelude::*;

#[derive(Clone, Copy)]
pub(super) struct Layout {
    pub(super) pages: usize,
    pub(super) page_bytes: usize,
    pub(super) control: usize,
    pub(super) counter: usize,
    pub(super) scenes: usize,
    pub(super) blocks: usize,
    pub(super) bytes: usize,
    pub(super) scratch_slots: usize,
    pub(super) scratch_stride: usize,
    pub(super) metric_stride: usize,
    pub(super) normal_blocks: usize,
}

fn align(value: usize, alignment: usize) -> Result<usize> {
    value.checked_add(alignment - 1).map(|n| n & !(alignment - 1)).ok_or(EOVERFLOW)
}

/// The share of memory a 128 KiB-block heap may grow to ahead of a render: a 64th. On a
/// 48 GiB M4 Pro, macOS 26.6.2 runs with `BM+0x80` at 1/60 of memory (6,579 blocks, read live).
const NORMAL_MEMORY_SHARE: u64 = 64;

/// The normal maximum of a 128 KiB-block heap in blocks (`BM+0x80`, and the cap of its
/// pre-render growth): a 64th of `memory_bytes`, at least the fixed `NORMAL_MAX_BLOCKS` and
/// at most what the block table holds, `TVB_MAX_BLOCKS`.
pub(super) fn neo_normal_blocks(memory_bytes: u64) -> usize {
    let blocks = memory_bytes / NORMAL_MEMORY_SHARE / cfg::TVB_BLOCK_SIZE as u64;
    blocks.clamp(fw::NORMAL_MAX_BLOCKS as u64, cfg::TVB_MAX_BLOCKS as u64) as usize
}

impl Layout {
    pub(super) const NEO: Self = Self {
        pages: fw::PAGE_LIST, page_bytes: fw::PAGE_LIST_BYTES,
        control: fw::CONTROL, counter: fw::COUNTER, scenes: fw::SCENE_TABLE,
        blocks: fw::BLOCK_TABLE, bytes: fw::SIZE,
        scratch_slots: fw::SCRATCH_SLOTS, scratch_stride: fw::SCRATCH_STRIDE as usize,
        metric_stride: 4, normal_blocks: fw::NORMAL_MAX_BLOCKS,
    };

    /// The NEO layout of a device with `memory_bytes` of memory.
    pub(super) fn neo(memory_bytes: u64) -> Self {
        Self { normal_blocks: neo_normal_blocks(memory_bytes), ..Self::NEO }
    }

    pub(super) fn m5(geometry: BlockGeometry) -> Result<Self> {
        geometry.validate()?;
        let blocks = 0x4000;
        let pages = align(geometry.capacity.checked_mul(8)
            .and_then(|n| n.checked_add(blocks)).ok_or(EOVERFLOW)?, 0x4000)?;
        let page_bytes = align(geometry.capacity.checked_mul(geometry.pages())
            .and_then(|n| n.checked_mul(4)).ok_or(EOVERFLOW)?, 0x4000)?;
        let scratch_stride = crate::g17::fw::m5::pool::parameter_scratch_stride(
            geometry.capacity, 0).map_err(|_| EINVAL)?;
        Ok(Self {
            pages, page_bytes, control: 0x300, counter: 0x200,
            scenes: 0x400, blocks, bytes: pages.checked_add(page_bytes).ok_or(EOVERFLOW)?,
            scratch_slots: fw::SCENES, scratch_stride, metric_stride: 64,
            normal_blocks: geometry.capacity / 3,
        })
    }

    pub(super) fn initialize(self, geometry: BlockGeometry, args: &fw::GraphArgs<'_>,
        out: &mut [u8]) -> Result {
        geometry.validate()?;
        if out.len() < self.bytes || args.generation == 0 || args.blocks.is_empty()
            || args.blocks.len() > geometry.max_blocks() { return Err(EINVAL); }
        let scratch = args.scratch.checked_sub(geometry.base).ok_or(EINVAL)?;
        let discard = args.discard.checked_sub(geometry.base).ok_or(EINVAL)?;
        let scratch_end = scratch.checked_add((self.scratch_slots * self.scratch_stride) as u64)
            .ok_or(EOVERFLOW)?;
        if scratch_end > u32::MAX as u64 || discard > u32::MAX as u64 { return Err(EINVAL); }
        let pages = u32::try_from(args.blocks.len().checked_mul(geometry.pages()).ok_or(EOVERFLOW)?)?;
        let maximum = u32::try_from(geometry.capacity.checked_mul(geometry.pages()).ok_or(EOVERFLOW)?)?;
        let normal = u32::try_from(args.normal_blocks.checked_mul(geometry.pages()).ok_or(EOVERFLOW)?)?;
        // Every object is unpublished here. Keep all unused words zero on reuse.
        out[..self.bytes].fill(0);
        fn put<const N: usize>(out: &mut [u8], at: usize, value: [u8; N]) -> Result {
            out.get_mut(at..at.checked_add(N).ok_or(EOVERFLOW)?).ok_or(EINVAL)?
                .copy_from_slice(&value);
            Ok(())
        }
        let address = |offset: usize| args.fw_va.checked_add(offset as u64).ok_or(EOVERFLOW);
        for (index, &block) in args.blocks.iter().enumerate() {
            let first = geometry.compact_page(block)?;
            put(out, self.blocks + index * 8, first.to_le_bytes())?;
            for page in 0..geometry.pages() {
                put(out, self.pages + (index * geometry.pages() + page) * 4,
                    first.checked_add(page as u32).ok_or(EOVERFLOW)?.to_le_bytes())?;
            }
        }
        for index in 0..fw::SCENES {
            let at = self.scenes + index * size_of::<fw::Scene>();
            let metric = (index * self.metric_stride) as u64;
            put(out, at, args.metrics.client.checked_add(metric).ok_or(EOVERFLOW)?.to_le_bytes())?;
            put(out, at + 8, args.metrics.firmware.checked_add(metric).ok_or(EOVERFLOW)?.to_le_bytes())?;
            put(out, at + 0x28, (scratch + (index % self.scratch_slots * self.scratch_stride) as u64).to_le_bytes())?;
            put(out, at + 0x40, address(self.control + 0x40)?.to_le_bytes())?;
        }
        put(out, self.scenes + fw::FIRST_SCENE * size_of::<fw::Scene>() + 0x4c, args.generation.to_le_bytes())?;
        for (at, value) in [
            (0xc, args.buffer_id), (0x30, u32::try_from(self.page_bytes)?), (0x34, pages),
            (0x38, u32::try_from(geometry.capacity)?), (0x3c, args.blocks.len() as u32),
            (0x54, pages - 1), (0x58, u32::try_from(geometry.block_bytes)?),
            (0x7c, maximum), (0x80, normal), (self.control, args.blocks.len() as u32),
            (self.control + 4, args.blocks.len() as u32), (self.control + 0x60, 1),
        ] { put(out, at, value.to_le_bytes())?; }
        for (at, value) in [
            (0x20, address(self.pages)?), (0x28, args.page_list_client),
            (0x44, address(self.blocks)?), (0x4c, address(self.control)?),
            (0x64, address(self.counter)?), (0x84, discard),
        ] { put(out, at, value.to_le_bytes())?; }
        Ok(())
    }
}

#[kernel::macros::kunit_tests(asahi_g17_tvb_limits)]
mod tests {
    use super::*;

    const GIB: u64 = 1 << 30;

    #[test]
    fn normal_maximum_is_a_share_of_memory_within_the_block_table() {
        // 8 GiB and less keep the fixed value: BM+0x80 stays 0x45e * 4 pages.
        assert_eq!(neo_normal_blocks(8 * GIB), fw::NORMAL_MAX_BLOCKS);
        assert_eq!(neo_normal_blocks(7 * GIB) * cfg::TVB_PAGES_PER_BLOCK, 0x45e * 4);
        assert_eq!(neo_normal_blocks(0), fw::NORMAL_MAX_BLOCKS);
        assert_eq!(neo_normal_blocks(9 * GIB), 1152);
        assert_eq!(neo_normal_blocks(16 * GIB), 2048);
        assert_eq!(neo_normal_blocks(24 * GIB), 3072);
        // About 26.2 GiB and more fill the block table, as on a 48 GiB M4 Pro.
        assert_eq!(neo_normal_blocks(27 * GIB), cfg::TVB_MAX_BLOCKS);
        assert_eq!(neo_normal_blocks(46 * GIB), cfg::TVB_MAX_BLOCKS);
        assert_eq!(neo_normal_blocks(u64::MAX), cfg::TVB_MAX_BLOCKS);
    }

    #[test]
    fn compact_objects_reach_the_growth_arenas_last() {
        let [first, second] = cfg::tvb_growth_ranges();
        assert!(cfg::compact_ranges().ends_with(&[first, second]));
    }
}
