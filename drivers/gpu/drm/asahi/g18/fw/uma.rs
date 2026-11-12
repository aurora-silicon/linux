// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! T8152 / 26A428 HAL400 UMA hardware descriptor serialization.
//!
//! Native `halLoadUMADesc` at 0xfffffe0008c40980 writes sixteen u64s per
//! hardware slot (0x80 bytes). This differs from M5's four-word format.
//! The caller must supply the owned firmware pool state and the four
//! device configuration words, and validate all addresses and lifetimes.
//! Encoding parity does not establish a usable pool or execute GPU work.

#![allow(dead_code)]

pub(crate) const G18_UMA_STATE_BYTES: usize = 0x7c;
pub(crate) const G18_UMA_DESCRIPTOR_BYTES: usize = 0x80;
pub(crate) const G18_POOL_STATE_BYTES: usize = 0xa0;

/// Resolved inputs to the full native FList firmware state. The directory
/// address includes its sparse page-end adjustment. Accessor/policy values
/// and resource ownership are the caller's responsibility.
pub(crate) struct G18UmaPoolState {
    pub(crate) statistics_id: u64,
    pub(crate) grow_engine: u32,
    pub(crate) priority: u32,
    pub(crate) directory_gpu: u64,
    pub(crate) directory_stride: u64,
    /// Original FList +0xac/+0xb0 order; native rev64 swaps the output lanes.
    pub(crate) chunk_counters: [u32; 2],
    pub(crate) block_list_gpu: u64,
    pub(crate) populated_blocks: u32,
    pub(crate) block_units_64k: u32,
    pub(crate) sma_mode: u32,
    pub(crate) preempt_counter_gpu: u64,
    pub(crate) preempt_counter: u64,
    pub(crate) compute: bool,
    pub(crate) policy_value: u32,
}

/// Native state initialization at 0xfffffe0008c8d404 and serialization at
/// 0xfffffe0008c8c004. This is the 0xa0-byte firmware state, not the 0x80-byte
/// hardware UMA descriptor. Zero padding matches a fresh native allocation.
pub(crate) fn encode_g18_pool_state(state: &G18UmaPoolState) -> [u8; G18_POOL_STATE_BYTES] {
    let mut out = [0; G18_POOL_STATE_BYTES];
    for (offset, value) in [
        (0x00, state.statistics_id),
        (0x14, state.directory_gpu),
        (0x1c, state.directory_stride),
        (0x4c, state.block_list_gpu),
        (0x88, state.preempt_counter_gpu),
        (0x90, state.preempt_counter),
    ] {
        out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    }
    for (offset, value) in [
        (0x08, u32::MAX),
        (0x0c, state.grow_engine),
        (0x10, state.priority),
        (0x44, state.chunk_counters[1]),
        (0x48, state.chunk_counters[0]),
        (0x54, state.populated_blocks),
        (0x58, state.block_units_64k),
        (0x7c, state.sma_mode),
        (0x98, u32::from(state.compute)),
        (0x9c, state.policy_value),
    ] {
        out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    out
}

/// Dimensions written by the M6 pool's `computeAndSetUMAPoolVariables`.
pub(crate) struct G18UmaGeometry {
    pub(crate) pool_bytes: u64,
    pub(crate) block_bytes: u64,
    pub(crate) block_count: u32,
    pub(crate) directory_bytes: u64,
    pub(crate) initial_committed_bytes: u64,
    pub(crate) total_1k_chunks: u64,
    pub(crate) directory_bytes_per_usc: u64,
    pub(crate) chunks_per_usc: u32,
}

/// Derive allocation geometry after resolving the device's native accessors.
///
/// Native arithmetic uses 32-bit intermediates for chunk counts and directory
/// sizes. Reject overflow instead of wrapping an allocation. The caller must
/// supply the actual mode and topology; this helper does not select defaults.
pub(crate) fn g18_uma_geometry(
    max_bytes: u64,
    block_bytes: u64,
    uscs: u32,
    chunks64k: bool,
    page_aligned: bool,
    page_shift: u32,
) -> Option<G18UmaGeometry> {
    if uscs == 0
        || uscs > 128
        || !block_bytes.is_power_of_two()
        || block_bytes < 0x10000
        || !(12..=16).contains(&page_shift)
    {
        return None;
    }
    let pool_bytes = max_bytes & !(block_bytes - 1);
    if pool_bytes == 0 {
        return None;
    }
    let units = u32::try_from(pool_bytes >> 16).ok()?;
    let per_usc = units.checked_add(uscs - 1)? / uscs;
    let raw_directory = per_usc.checked_mul(if chunks64k { 4 } else { 512 })?;
    let align = if page_aligned { 1u64 << page_shift } else { 1024 };
    let directory_bytes_per_usc = (u64::from(raw_directory) + align - 1) & !(align - 1);
    Some(G18UmaGeometry {
        pool_bytes,
        block_bytes,
        block_count: u32::try_from(pool_bytes / block_bytes).ok()?,
        directory_bytes: directory_bytes_per_usc.checked_mul(u64::from(uscs))?,
        initial_committed_bytes: 0,
        total_1k_chunks: u64::from(units.checked_mul(64)?),
        directory_bytes_per_usc,
        chunks_per_usc: (units / uscs).checked_mul(64)?,
    })
}

/// Initial sparse directory layout for the native G18 pool flags value 3.
///
/// The virtual extent is NOT a request for that much physical backing. Only
/// `initial_page_count` pages are committed initially, at `page_offset(i)`;
/// the owner must reserve the intervening unmapped addresses for later growth.
pub(crate) struct G18UmaSparseDirectory {
    pub(crate) reservation_bytes: u64,
    pub(crate) page_bytes: u64,
    pub(crate) initial_page_count: u32,
    pub(crate) gpu_base_offset: u64,
    directory_bytes_per_usc: u64,
}

impl G18UmaSparseDirectory {
    pub(crate) fn page_offset(&self, index: u32) -> Option<u64> {
        if index >= self.initial_page_count {
            return None;
        }
        let offset = self.directory_bytes_per_usc.checked_mul(u64::from(index))?;
        if offset.checked_add(self.page_bytes)? > self.reservation_bytes {
            return None;
        }
        Some(offset)
    }
}

/// Native FList init reserves one extra page and backs one page per USC plus
/// the final page. `prepareBufferResources` exposes the directory beginning
/// one KiB before the end of the first page. Other pool flag modes are outside
/// this contract; the caller must first resolve the device's actual flags.
pub(crate) fn g18_uma_sparse_directory(
    geometry: &G18UmaGeometry,
    uscs: u32,
    page_shift: u32,
) -> Option<G18UmaSparseDirectory> {
    if uscs == 0 || uscs > 128 || !(12..=16).contains(&page_shift) {
        return None;
    }
    let page_bytes = 1u64 << page_shift;
    let stride = geometry.directory_bytes_per_usc;
    if stride < page_bytes
        || stride & (page_bytes - 1) != 0
        || stride.checked_mul(u64::from(uscs))? != geometry.directory_bytes
    {
        return None;
    }
    Some(G18UmaSparseDirectory {
        reservation_bytes: geometry.directory_bytes.checked_add(page_bytes)?,
        page_bytes,
        initial_page_count: uscs + 1,
        gpu_base_offset: page_bytes - 0x400,
        directory_bytes_per_usc: stride,
    })
}

/// Native `halInitSMAEnableMask` at 0xfffffe0008c41bdc splits the active
/// 128-bit mask into sixteen groups, each stored in a 16-bit output lane.
/// The primary mask is accelerator+0x470; +0x480 is used only when it is zero.
/// `group_width` is accelerator+0x4ec. This helper admits widths fitting the
/// output lanes; wider native profiles need a separate qualified contract.
pub(crate) fn pack_g18_sma_masks(
    primary: [u64; 2],
    fallback: [u64; 2],
    group_width: u32,
) -> Option<[u64; 4]> {
    if !(1..=16).contains(&group_width) {
        return None;
    }
    let selected = if primary == [0; 2] { fallback } else { primary };
    let mut remaining = u128::from(selected[0]) | (u128::from(selected[1]) << 64);
    let mask = (1u128 << group_width) - 1;
    let mut words = [0; 4];
    for group in 0..16 {
        words[group / 4] |= ((remaining & mask) as u64) << ((group % 4) * 16);
        remaining >>= group_width;
    }
    Some(words)
}

fn read64(state: &[u8; G18_UMA_STATE_BYTES], offset: usize) -> u64 {
    let mut bytes = [0; 8];
    bytes.copy_from_slice(&state[offset..offset + 8]);
    u64::from_le_bytes(bytes)
}

fn read32(state: &[u8; G18_UMA_STATE_BYTES], offset: usize) -> u32 {
    let mut bytes = [0; 4];
    bytes.copy_from_slice(&state[offset..offset + 4]);
    u32::from_le_bytes(bytes)
}

/// Pack the exact M6 hardware format. Preserve native masks and truncation;
/// do not substitute M5 directory geometry or infer opaque state values.
///
/// `device_config` is native accelerator+0x11af8..0x11b10. These four
/// values are supplied by the caller; zero is not a qualified M6 default.
pub(crate) fn pack_g18_uma_descriptor(
    state: &[u8; G18_UMA_STATE_BYTES],
    device_config: [u64; 4],
) -> [u64; G18_UMA_DESCRIPTOR_BYTES / 8] {
    [
        (read64(state, 0x1c) << 40 & 0xfffc_0000_0000_0000) | (read64(state, 0x14) >> 10),
        read64(state, 0x24),
        read64(state, 0x2c),
        device_config[0],
        device_config[1],
        device_config[2],
        device_config[3],
        read64(state, 0x34) << 7,
        read64(state, 0x3c) << 7,
        read64(state, 0x44),
        (u64::from(read32(state, 0x58)) << 44) | (read64(state, 0x4c) >> 6),
        u64::from(read32(state, 0x54)),
        read64(state, 0x5c),
        read64(state, 0x64),
        read64(state, 0x6c),
        read64(state, 0x74),
    ]
}

/// Fixed first-dispatch policy, recovered with the original SMAV1 sizing code.
/// This is deliberately limited to the saved addition workload, not a general
/// replacement for native SMA sizing. Hardware execution remains to be tested.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18ComputeScratchPlan {
    pub(crate) block_bytes: u64,
    pub(crate) block_count: u32,
    pub(crate) register_pair: [u64; 2],
    pub(crate) chunk_counters: [u32; 2],
}

pub(crate) fn g18_addition_scratch(state: &[u8]) -> Option<G18ComputeScratchPlan> {
    const WORDS: [u32; 23] = [
        32, 10, 0, 0, 192, 16, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 128, 4, 0, 0,
    ];
    if state.len() != 0x268 { return None; }
    for (index, expected) in WORDS.iter().enumerate() {
        let offset = 0x198 + index * 4;
        let value = u32::from_le_bytes(state[offset..offset + 4].try_into().ok()?);
        if value != *expected { return None; }
    }
    // Twelve USCs, one compute job, engine/priority 2, flags 3, no preemption,
    // 2 GiB maximum and 1 MiB blocks: ideal 5 MiB, minimum 3 MiB.
    // Native allocatePagesEnd supplies d550/d558 and FList+0xac/+0xb0 below.
    Some(G18ComputeScratchPlan {
        block_bytes: 0x10_0000, block_count: 5,
        register_pair: [16, 6], chunk_counters: [0, 350],
    })
}

/// Native 8c8bfc8..8c8bfe0: preserve metadata while encoding a GPU address.
/// Reject truncation; current backing uses one entry per 1 MiB allocation.
pub(crate) fn g18_uma_block_entry(gpu_address: u64, previous: u32) -> Option<u32> {
    if gpu_address == 0 || gpu_address >= (1u64 << 48)
        || gpu_address & 0x0f_ffff != 0 {
        return None;
    }
    Some((previous & 0xf000_0000) | ((gpu_address >> 20) as u32))
}

/// Whether the current fixed first-dispatch policy avoids sparse-directory
/// growth. Native 8c8b8f8..8c8b9ac admits 28 one-MiB blocks on twelve USCs;
/// 29 requires new backing and is intentionally refused by this owner.
pub(crate) fn g18_initial_directory_fits(
    blocks: u32, block_bytes: u64, uscs: u32, flags: u32, page_shift: u32,
) -> Option<bool> {
    if blocks == 0 || !matches!(block_bytes, 0x10_0000 | 0x20_0000)
        || uscs != 12 || flags != 3 || page_shift != 14 {
        return None;
    }
    let units = (block_bytes >> 16).checked_mul(u64::from(blocks))?;
    let per_usc = units.checked_add(u64::from(uscs) - 1)? / u64::from(uscs);
    let initial_units = (((1u64 << page_shift) - 0x400) >> 9) + 8;
    Some(per_usc <= initial_units)
}
