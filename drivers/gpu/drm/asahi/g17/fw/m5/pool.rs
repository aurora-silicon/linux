// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 private-memory pool encodings for compute queues (linux-aurora t6050-gpu's
//! g17/fw/m5/pool.rs, its fixed-block G17S compute pool): the page directory, the block free
//! list, the eight-word page-pool (UMA) descriptor row and the shared pool state.
//!
//! The pool spans a fixed address inventory, which sizes the directory, not physical memory:
//! only the initial blocks are backed.

/// Bytes of one pool block.
const BLOCK_BYTES: usize = 0x40_0000;
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

/// Sizes of one compute pool.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PoolGeometry {
    /// Backed blocks.
    pub(crate) blocks: usize,
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
    /// A compute pool of `blocks` backed blocks over the full inventory.
    pub(crate) fn compute(blocks: usize) -> Result<Self, PoolError> {
        let bytes =
            INVENTORY_BYTES.min(RING_MAX_PAGES * POOL_PAGE as u64) & !(BLOCK_BYTES as u64 - 1);
        Self::new(blocks, (bytes / POOL_PAGE as u64) as u32)
    }

    fn new(blocks: usize, directory_entries: u32) -> Result<Self, PoolError> {
        let pages = blocks
            .checked_mul(BLOCK_BYTES / POOL_PAGE)
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
            directory_entries,
            pages,
            directory_bytes,
            freelist_bytes,
        })
    }

    pub(crate) const fn block_bytes(self) -> usize {
        BLOCK_BYTES
    }

    /// Bytes of one allocation holding the directory, the free list and the blocks, in that
    /// order.
    pub(crate) fn backing_bytes(self) -> Result<usize, PoolError> {
        self.blocks
            .checked_mul(BLOCK_BYTES)
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
    if PoolGeometry::new(g.blocks, g.directory_entries)? != g || blocks.len() != g.blocks {
        return Err(PoolError::Geometry);
    }
    for address in blocks {
        private_range(*address, BLOCK_BYTES)?;
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
    let pages_per_block = BLOCK_BYTES / POOL_PAGE;
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
    if PoolGeometry::new(p.geometry.blocks, p.geometry.directory_entries)? != p.geometry {
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
        (p.freelist >> 6) | (((BLOCK_BYTES / POOL_PAGE) as u64) << 44),
        blocks,
        0,
        blocks,
    ])
}

/// Writes the shared state of a fresh compute pool whose host-submitted counter is at
/// `submitted`.
pub(crate) fn encode_compute_state(
    p: PoolLayout,
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
        (0x40, (BLOCK_BYTES / POOL_PAGE) as u32),
        (0x68, blocks),
        (0x7c, 1),
        (0x80, 1),
    ] {
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
