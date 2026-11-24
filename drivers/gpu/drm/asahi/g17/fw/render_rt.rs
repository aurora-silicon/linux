// SPDX-License-Identifier: GPL-2.0-only OR MIT

use super::{
    compute::{IntersectionTables, ScratchRequest},
    compute_pool,
    freelist::{MULTI_CLUSTER_RENDER_BLOCKS_PER_CORE, PAGE_LIST_SIZE, RENDER_BLOCKS},
    initdata::{FREE_LIST_BLOCK_PAGES, FREE_LIST_BLOCK_SIZE},
};
use kernel::prelude::*;

#[derive(Clone, Copy)]
pub(crate) struct RayQueryStage {
    pub(crate) intersection: IntersectionTables,
    pub(crate) scratch: ScratchRequest,
}

impl RayQueryStage {
    fn parse(bytes: &[u8]) -> Result<Self> {
        let mut wire = [0u8; 56];
        wire[..bytes.len()].copy_from_slice(bytes);
        let layout = u64::from_le_bytes(wire[..8].try_into().unwrap());
        let intersection = IntersectionTables::parse(&wire[8..])?;
        let scratch = ScratchRequest::new(layout)?;
        if scratch.enabled() != intersection.enabled() { return Err(EINVAL); }
        Ok(Self { intersection, scratch })
    }

    pub(crate) fn parse_pair(bytes: &[u8]) -> Result<[Self; 2]> {
        if bytes.get(112..).is_some_and(|tail| tail.iter().any(|&b| b != 0)) {
            return Err(EINVAL);
        }
        let mut wire = [0u8; 112];
        let count = bytes.len().min(wire.len());
        wire[..count].copy_from_slice(&bytes[..count]);
        Ok([Self::parse(&wire[..56])?, Self::parse(&wire[56..])?])
    }
}

/// Minimum render pool in 1 MiB blocks for a GPU's cluster and active-core shape.
///
/// One- and two-cluster parts (A18 Pro, M4 Pro) keep [`RENDER_BLOCKS`] (44),
/// which fits the narrow shared pool window and has been validated there. Above
/// two clusters, clustered tiling runs vertex shaders across every cluster
/// concurrently with fragment work, so the base pool scales linearly with
/// enabled cores at [`MULTI_CLUSTER_RENDER_BLOCKS_PER_CORE`] (3) blocks per
/// core, floored at [`RENDER_BLOCKS`].
pub(crate) fn base_blocks(active_cores: u32, clusters: u32) -> Result<usize> {
    if active_cores == 0 || clusters == 0 {
        return Err(EINVAL);
    }
    if clusters <= 2 {
        return Ok(RENDER_BLOCKS);
    }
    let scaled = usize::try_from(active_cores)
        .ok()
        .and_then(|cores| cores.checked_mul(MULTI_CLUSTER_RENDER_BLOCKS_PER_CORE))
        .ok_or(EOVERFLOW)?;
    Ok(scaled.max(RENDER_BLOCKS))
}

/// The render pool of an owner is shared by its `pairs` render pairs, each with
/// its own compute-pool extras on top of the GPU-shape base pool.
pub(crate) fn required_blocks(
    active_cores: u32,
    clusters: u32,
    pages: [u16; 2],
    pairs: u8,
) -> Result<usize> {
    let base = base_blocks(active_cores, clusters)?;
    let extra = compute_pool::extra_bytes(active_cores, pages[0])?
        .checked_add(compute_pool::extra_bytes(active_cores, pages[1])?)
        .and_then(|bytes| bytes.checked_mul(u64::from(pairs)))
        .ok_or(EOVERFLOW)?;
    let base_bytes = u64::try_from(base)
        .ok()
        .and_then(|blocks| blocks.checked_mul(FREE_LIST_BLOCK_SIZE as u64))
        .ok_or(EOVERFLOW)?;
    let bytes = extra.checked_add(base_bytes).ok_or(EOVERFLOW)?;
    let blocks = usize::try_from(bytes.div_ceil(FREE_LIST_BLOCK_SIZE as u64))
        .map_err(|_| EOVERFLOW)?;
    let maximum = PAGE_LIST_SIZE / (FREE_LIST_BLOCK_PAGES as usize * 8);
    if blocks > maximum {
        return Err(E2BIG);
    }
    Ok(blocks)
}

#[kernel::macros::kunit_tests(asahi_g17_render_pool)]
mod tests {
    use super::*;

    #[test]
    fn one_and_two_cluster_parts_keep_default_floor() {
        for (cores, clusters) in [(5, 1), (6, 1), (8, 1), (10, 1), (16, 2), (20, 2)] {
            assert_eq!(base_blocks(cores, clusters), Ok(RENDER_BLOCKS));
            assert_eq!(required_blocks(cores, clusters, [0, 0], 1), Ok(RENDER_BLOCKS));
        }
    }

    #[test]
    fn multi_cluster_parts_scale_with_active_cores() {
        // 32-core J616C (4 clusters): 96 blocks; 40-core J614C/J575C (4 clusters): 120 blocks.
        assert_eq!(required_blocks(32, 4, [0, 0], 1), Ok(96));
        assert_eq!(required_blocks(40, 4, [0, 0], 1), Ok(120));
        // A small hypothetical >2-cluster part stays floored at RENDER_BLOCKS.
        assert_eq!(required_blocks(8, 4, [0, 0], 1), Ok(RENDER_BLOCKS));
        // Eight-cluster M5 Ultra topologies (M5 allocates its own pools, but queue creation
        // still checks required_blocks): 192 / 240 blocks, well within the table limit.
        assert_eq!(required_blocks(64, 8, [0, 0], 2), Ok(192));
        assert_eq!(required_blocks(80, 8, [0, 0], 2), Ok(240));
    }

    #[test]
    fn ray_query_scratch_adds_on_top_of_base_pool() {
        // 64 pages per stage -> per_core = 64 + 1 + 8 = 73 pages = 299,008 bytes/core/stage.
        assert_eq!(required_blocks(20, 2, [64, 64], 1), Ok(44 + 12));
        assert_eq!(required_blocks(32, 4, [64, 64], 1), Ok(96 + 19));
        assert_eq!(required_blocks(40, 4, [64, 64], 1), Ok(120 + 23));
    }

    #[test]
    fn rejects_invalid_shape_and_oversized_pools() {
        assert_eq!(required_blocks(0, 4, [0, 0], 1), Err(EINVAL));
        assert_eq!(required_blocks(32, 0, [0, 0], 1), Err(EINVAL));
        assert_eq!(required_blocks(400, 4, [0, 0], 1), Err(E2BIG));
    }
}
