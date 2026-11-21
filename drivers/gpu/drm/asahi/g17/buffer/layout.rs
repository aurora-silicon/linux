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

/// The tiler-heap maximum macOS 25G83 gives a 128 KiB-block heap
/// (`halGetDefaultParameterBufferSizeMax`): a twentieth of the memory, at least 192 MiB,
/// rounded up to whole groups of 129 blocks. 3,354 blocks with 8 GiB, 19,737 with 48 GiB.
fn macos_maximum_blocks(memory_bytes: u64) -> u64 {
    const GROUP_BLOCKS: u64 = 129;
    const GROUP_BYTES: u64 = GROUP_BLOCKS * cfg::TVB_BLOCK_SIZE as u64;
    (memory_bytes / 20).max(0xc00_0000).div_ceil(GROUP_BYTES) * GROUP_BLOCKS
}

/// The normal maximum of a 128 KiB-block heap, in blocks. macOS gives a queue a third of
/// its maximum until the queue renders memoryless (`BM+0x80`, and the cap of its pre-render
/// growth): 1,118 blocks with 8 GiB, 6,579 with 48 GiB. The block table holds
/// `TVB_MAX_BLOCKS`. `memory_bytes` is usable memory, below the installed amount macOS
/// reads, so never go below the 8 GiB value of the smallest G17 Mac.
pub(super) fn neo_normal_blocks(memory_bytes: u64) -> usize {
    let normal = macos_maximum_blocks(memory_bytes).div_ceil(3);
    (normal.min(cfg::TVB_MAX_BLOCKS as u64) as usize).max(fw::NORMAL_MAX_BLOCKS)
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
        let normal = u32::try_from(self.normal_blocks.checked_mul(geometry.pages()).ok_or(EOVERFLOW)?)?;
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
    fn maximum_matches_macos() {
        // The maxima macOS 25G83 reports (AGXParameterBufferMaxSize), in blocks.
        assert_eq!(macos_maximum_blocks(8 * GIB), 3354);
        assert_eq!(macos_maximum_blocks(16 * GIB), 6579);
        assert_eq!(macos_maximum_blocks(48 * GIB), 19737);
        // The 192 MiB floor.
        assert_eq!(macos_maximum_blocks(GIB), 1548);
    }

    #[test]
    fn normal_maximum_scales_with_memory_up_to_the_block_table() {
        assert_eq!(neo_normal_blocks(8 * GIB), fw::NORMAL_MAX_BLOCKS);
        // Usable memory of an 8 GiB Mac keeps the 8 GiB value.
        assert_eq!(neo_normal_blocks(7 * GIB), fw::NORMAL_MAX_BLOCKS);
        assert_eq!(neo_normal_blocks(16 * GIB), 2193);
        assert_eq!(neo_normal_blocks(24 * GIB), 3311);
        assert_eq!(neo_normal_blocks(48 * GIB), cfg::TVB_MAX_BLOCKS);
        assert_eq!(neo_normal_blocks(u64::MAX), cfg::TVB_MAX_BLOCKS);
    }
}
