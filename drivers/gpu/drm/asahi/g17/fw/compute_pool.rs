// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! G17P compute pool capacity for a validated Gen1 SCS request.

use kernel::prelude::*;
use super::initdata::{FREE_LIST_BLOCK_PAGES, FREE_LIST_BLOCK_SIZE};
use super::freelist::{COMPUTE_BLOCKS, PAGE_LIST_SIZE};

pub(crate) fn extra_bytes(active_cores: u32, scs_pages: u16) -> Result<u64> {
    if active_cores == 0 { return Err(EINVAL); }
    if scs_pages == 0 { return Ok(0); }
    // Register0x101d1 counts 4-KiB pages and their 1-MiB large-page inventory.
    // Register0x0d479 reserves eight further pages for each enabled USC.
    let small = u64::from(scs_pages);
    let per_core = small.checked_add(small.div_ceil(256))
        .and_then(|pages| pages.checked_add(8)).ok_or(EOVERFLOW)?;
    let extra = per_core.checked_mul(u64::from(active_cores))
        .and_then(|pages| pages.checked_mul(4096)).ok_or(EOVERFLOW)?;
    Ok(extra)
}

static BASE_BLOCKS: core::sync::atomic::AtomicUsize =
    core::sync::atomic::AtomicUsize::new(COMPUTE_BLOCKS);

pub(crate) fn set_base_blocks(blocks: usize) -> Result {
    if blocks < COMPUTE_BLOCKS || blocks > PAGE_LIST_SIZE / (FREE_LIST_BLOCK_PAGES as usize * 8) {
        return Err(EINVAL);
    }
    BASE_BLOCKS.store(blocks, core::sync::atomic::Ordering::Relaxed);
    Ok(())
}

pub(crate) fn required_blocks(active_cores: u32, scs_pages: u16) -> Result<usize> {
    let extra = extra_bytes(active_cores, scs_pages)?;
    let base = BASE_BLOCKS.load(core::sync::atomic::Ordering::Relaxed);
    let bytes = extra
        .checked_add((base * FREE_LIST_BLOCK_SIZE) as u64)
        .ok_or(EOVERFLOW)?;
    let blocks = usize::try_from(bytes.div_ceil(FREE_LIST_BLOCK_SIZE as u64))
        .map_err(|_| EOVERFLOW)?;
    let maximum = PAGE_LIST_SIZE / (FREE_LIST_BLOCK_PAGES as usize * 8);
    if blocks > maximum { return Err(E2BIG); }
    Ok(blocks)
}
