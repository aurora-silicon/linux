// SPDX-License-Identifier: GPL-2.0-only OR MIT

use super::{compute::{IntersectionTables, ScratchRequest}, compute_pool,
    freelist::{PAGE_LIST_SIZE, RENDER_BLOCKS},
    initdata::{FREE_LIST_BLOCK_PAGES, FREE_LIST_BLOCK_SIZE}};
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

/// The render pool of an owner is shared by its `pairs` render pairs, each with
/// its own compute-pool extras.
pub(crate) fn required_blocks(active_cores: u32, pages: [u16; 2], pairs: u8) -> Result<usize> {
    let extra = compute_pool::extra_bytes(active_cores, pages[0])?
        .checked_add(compute_pool::extra_bytes(active_cores, pages[1])?)
        .and_then(|bytes| bytes.checked_mul(u64::from(pairs)))
        .ok_or(EOVERFLOW)?;
    let bytes = extra.checked_add((RENDER_BLOCKS * FREE_LIST_BLOCK_SIZE) as u64)
        .ok_or(EOVERFLOW)?;
    let blocks = usize::try_from(bytes.div_ceil(FREE_LIST_BLOCK_SIZE as u64))
        .map_err(|_| EOVERFLOW)?;
    let maximum = PAGE_LIST_SIZE / (FREE_LIST_BLOCK_PAGES as usize * 8);
    if blocks > maximum { return Err(E2BIG); }
    Ok(blocks)
}
