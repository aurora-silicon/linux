// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 private-memory pool encodings (linux-aurora t6050-gpu's g17/fw/m5/pool.rs, its
//! fixed-block pools): the page directory, the block free list, the eight-word page-pool (UMA)
//! descriptor row and the shared pool state, for compute and render queues alike.
//!
//! The pool spans a fixed address inventory, which sizes the directory, not physical memory:
//! only the initial blocks are backed. The block size is the product's
//! ([`super::budget::Product::block_bytes`]).

/// USC pool pages are 4 KiB, independent of the UAT page size.
const POOL_PAGE: usize = 0x1000;
/// The pool's address inventory (G17S). J714 value.
const INVENTORY_BYTES: u64 = 1 << 32;
/// The largest inventory a pool ring can describe, in pool pages.
const RING_MAX_PAGES: u64 = 0x3f_f800;
/// Directory and free-list tables are rounded to the GPU's 16 KiB pages.
const GPU_PAGE: usize = 0x4000;
/// Pool addresses are canonical low addresses of the 42-bit address space.
const PRIVATE_TOP: u64 = 1 << 42;
/// Bytes of the shared pool state the firmware reads.
pub(crate) const STATE_BYTES: usize = 0x100;
/// Offset of the firmware-completed submission counter in the pool state. Unaligned.
pub(crate) const COMPLETED: usize = 0x74;

/// Why a pool encoding was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PoolError {
    Geometry,
    Address,
    Buffer,
}

/// The engine a pool serves: its shared state differs in two words (S
/// `encode_g17s_compute_state`, `encode_g17s_render_state`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Engine {
    Compute,
    Render,
}

/// Sizes of one pool.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PoolGeometry {
    /// Backed blocks.
    pub(crate) blocks: usize,
    /// Bytes of one block.
    pub(crate) block_bytes: usize,
    /// Directory entries, one per pool page of the inventory.
    pub(crate) directory_entries: u32,
    /// Pool pages of the backed blocks.
    pub(crate) pages: u32,
    pub(crate) directory_bytes: usize,
    pub(crate) freelist_bytes: usize,
}

fn round_page(bytes: usize) -> Result<usize, PoolError> {
    bytes
        .checked_add(GPU_PAGE - 1)
        .map(|n| n & !(GPU_PAGE - 1))
        .ok_or(PoolError::Geometry)
}

impl PoolGeometry {
    /// A pool of `blocks` backed blocks of `block_bytes` over the full inventory.
    pub(crate) fn of(blocks: usize, block_bytes: usize) -> Result<Self, PoolError> {
        if block_bytes == 0 || block_bytes % POOL_PAGE != 0 {
            return Err(PoolError::Geometry);
        }
        let bytes =
            INVENTORY_BYTES.min(RING_MAX_PAGES * POOL_PAGE as u64) & !(block_bytes as u64 - 1);
        Self::new(blocks, block_bytes, (bytes / POOL_PAGE as u64) as u32)
    }

    fn new(blocks: usize, block_bytes: usize, directory_entries: u32) -> Result<Self, PoolError> {
        let pages = blocks
            .checked_mul(block_bytes / POOL_PAGE)
            .and_then(|n| u32::try_from(n).ok())
            .ok_or(PoolError::Geometry)?;
        if blocks == 0 || pages > directory_entries {
            return Err(PoolError::Geometry);
        }
        let directory_bytes = round_page(
            (directory_entries as usize)
                .checked_mul(8)
                .ok_or(PoolError::Geometry)?,
        )?;
        let freelist_bytes = round_page(blocks.checked_mul(8).ok_or(PoolError::Geometry)?)?;
        if freelist_bytes / POOL_PAGE >= 1 << 20 {
            return Err(PoolError::Geometry);
        }
        Ok(Self {
            blocks,
            block_bytes,
            directory_entries,
            pages,
            directory_bytes,
            freelist_bytes,
        })
    }

    pub(crate) const fn block_bytes(self) -> usize {
        self.block_bytes
    }

    /// Bytes of one allocation holding the directory, the free list and the blocks, in that
    /// order.
    pub(crate) fn backing_bytes(self) -> Result<usize, PoolError> {
        self.blocks
            .checked_mul(self.block_bytes)
            .and_then(|n| n.checked_add(self.directory_bytes))
            .and_then(|n| n.checked_add(self.freelist_bytes))
            .ok_or(PoolError::Geometry)
    }
}

fn private_range(address: u64, bytes: usize) -> Result<(), PoolError> {
    if address == 0
        || address & (GPU_PAGE as u64 - 1) != 0
        || address
            .checked_add(bytes as u64)
            .is_none_or(|end| end > PRIVATE_TOP)
    {
        Err(PoolError::Address)
    } else {
        Ok(())
    }
}

fn validate_blocks(g: PoolGeometry, blocks: &[u64]) -> Result<(), PoolError> {
    if PoolGeometry::new(g.blocks, g.block_bytes, g.directory_entries)? != g
        || blocks.len() != g.blocks
    {
        return Err(PoolError::Geometry);
    }
    for address in blocks {
        private_range(*address, g.block_bytes)?;
    }
    Ok(())
}

/// Writes the page directory: the address of every pool page of the backed blocks, in order.
pub(crate) fn encode_directory(
    g: PoolGeometry,
    blocks: &[u64],
    out: &mut [u8],
) -> Result<(), PoolError> {
    validate_blocks(g, blocks)?;
    let out = out.get_mut(..g.directory_bytes).ok_or(PoolError::Buffer)?;
    out.fill(0);
    let pages_per_block = g.block_bytes / POOL_PAGE;
    for (block, address) in blocks.iter().enumerate() {
        for page in 0..pages_per_block {
            let offset = (block * pages_per_block + page) * 8;
            out[offset..offset + 8]
                .copy_from_slice(&(address + (page * POOL_PAGE) as u64).to_le_bytes());
        }
    }
    Ok(())
}

/// Writes the free list: the address of every backed block.
pub(crate) fn encode_freelist(
    g: PoolGeometry,
    blocks: &[u64],
    out: &mut [u8],
) -> Result<(), PoolError> {
    validate_blocks(g, blocks)?;
    let out = out.get_mut(..g.freelist_bytes).ok_or(PoolError::Buffer)?;
    out.fill(0);
    for (index, address) in blocks.iter().enumerate() {
        out[index * 8..index * 8 + 8].copy_from_slice(&address.to_le_bytes());
    }
    Ok(())
}

/// Addresses and identity of one pool.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PoolLayout {
    pub(crate) geometry: PoolGeometry,
    pub(crate) directory: u64,
    pub(crate) freelist: u64,
    /// The pool's buffer slot.
    pub(crate) hardware_id: u32,
}

fn validate_layout(p: PoolLayout) -> Result<(), PoolError> {
    if PoolGeometry::new(
        p.geometry.blocks,
        p.geometry.block_bytes,
        p.geometry.directory_entries,
    )? != p.geometry
    {
        return Err(PoolError::Geometry);
    }
    private_range(p.directory, p.geometry.directory_bytes)?;
    private_range(p.freelist, p.geometry.freelist_bytes)
}

/// The pool's page-pool descriptor row before its first use.
pub(crate) fn initial_uma_descriptor(p: PoolLayout) -> Result<[u64; 8], PoolError> {
    validate_layout(p)?;
    let blocks = p.geometry.blocks as u64;
    let pages = u64::from(p.geometry.pages);
    Ok([
        p.directory >> 7,
        pages << 32,
        pages | (u64::from(p.geometry.directory_entries) << 32),
        0,
        (p.freelist >> 6) | (((p.geometry.block_bytes / POOL_PAGE) as u64) << 44),
        blocks,
        0,
        blocks,
    ])
}

/// Writes the shared state of a fresh `engine` pool whose host-submitted counter is at
/// `submitted`.
pub(crate) fn encode_state(
    p: PoolLayout,
    engine: Engine,
    submitted: u64,
    out: &mut [u8],
) -> Result<(), PoolError> {
    validate_layout(p)?;
    if submitted == 0 || submitted & 3 != 0 {
        return Err(PoolError::Address);
    }
    let out = out.get_mut(..STATE_BYTES).ok_or(PoolError::Buffer)?;
    out.fill(0);
    let blocks = p.geometry.blocks as u32;
    for (offset, value) in [
        (0, 1u32),
        (8, p.hardware_id),
        (0x0c, 2),
        (0x10, 2),
        (0x1c, p.geometry.directory_entries),
        (0x24, p.geometry.pages),
        (0x2c, p.geometry.pages),
        (0x38, blocks),
        (0x3c, blocks),
        (0x40, (p.geometry.block_bytes / POOL_PAGE) as u32),
        (0x68, blocks),
    ] {
        out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    let engine_words: [u32; 2] = match engine {
        Engine::Compute => [1, 1],
        Engine::Render => [0, 4],
    };
    for (offset, value) in [(0x7c, engine_words[0]), (0x80, engine_words[1])] {
        out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    for (offset, value) in [
        (0x14, p.directory),
        (0x30, p.freelist),
        (0x50, p.freelist),
        (0x6c, submitted),
    ] {
        out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    }
    Ok(())
}

/// The parameter buffer's scene table: entries, their stride and the trailer after them.
pub(crate) const SCENE_TABLE_ENTRIES: usize = 0x50;
pub(crate) const SCENE_STRIDE: usize = 0x80;
pub(crate) const SCENE_TRAILER_SIZE: usize = 0x40;

/// Parameter-buffer capacity follows the memory budget and initial demand.
/// Capacity is rounded to groups of 33 half-MiB blocks; it reserves list space,
/// not physical backing for every possible block.
pub(crate) fn parameter_capacity(
    memory_bytes: u64,
    initial_bytes: u64,
) -> Result<usize, PoolError> {
    const GROUP: u64 = 33 * 0x80000;
    let scaled = memory_bytes.checked_mul(5).ok_or(PoolError::Geometry)? >> 2;
    let budget = (((u128::from(scaled) * 0x28f5_c28f_5c28_f5c3) >> 64) as u64) >> 2;
    let groups = |bytes: u64| {
        bytes
            .checked_add(GROUP - 1)
            .map(|n| n / GROUP)
            .ok_or(PoolError::Geometry)
    };
    let groups = groups(budget)?.max(groups(initial_bytes)?);
    usize::try_from(groups.checked_mul(33).ok_or(PoolError::Geometry)?)
        .map_err(|_| PoolError::Geometry)
}

/// Per-scene scratch covers the complete parameter-buffer inventory. Optional
/// cluster entries are supplied by the dispatch policy, not the active draw's
/// physical block count. Each entry and the header occupy eight bytes.
pub(crate) fn parameter_scratch_stride(
    capacity: usize,
    cluster_entries: usize,
) -> Result<usize, PoolError> {
    if capacity == 0 {
        return Err(PoolError::Geometry);
    }
    let pages = capacity.checked_mul(16).ok_or(PoolError::Geometry)?;
    let entries = pages.checked_add(0x17ff).ok_or(PoolError::Geometry)? / 0x1800;
    let entries = entries
        .checked_add(cluster_entries)
        .ok_or(PoolError::Geometry)?;
    entries
        .checked_mul(8)
        .and_then(|n| n.checked_add(8 + 63))
        .map(|n| n & !63)
        .ok_or(PoolError::Geometry)
}
