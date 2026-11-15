// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! G18 shared compute pool sizing for the measured J873 profile.
//!
//! Native 26A428 prepareLockedBegin, compute sizing and allocatePagesEnd.
//! This is a pure plan for one active compute job in a fresh/retired pool.
//! It does not allocate memory or enable scratch submission. Compared with
//! the original code.

pub(crate) const G18_COMPUTE_BLOCK_BYTES: u64 = 0x20_0000;
pub(crate) const G18_COMPUTE_MAX_BYTES: u64 = 0x8000_0000;
const USCS: u64 = 12;
const CHUNKS_PER_USC: u64 = (G18_COMPUTE_MAX_BYTES / 0x1_0000 / USCS) * 64;
pub(crate) const G18_COMPUTE_DIRECTORY_STRIDE: u64 = 0x15_8000;
pub(crate) const G18_COMPUTE_DIRECTORY_PAGE: u64 = 0x4000;

/// Sparse directory commitment for the measured flags3 / 16-KiB profile.
/// Initial anchor pages remain mapped. Additional pages form the tail of
/// each USC stride; native growth rounds to quarter/half/full stride tiers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct G18ComputeDirectory {
    pub(crate) tail_pages_per_usc: u32,
}

impl G18ComputeDirectory {
    pub(crate) fn for_blocks(blocks: u32) -> Option<Self> {
        if blocks == 0 || u64::from(blocks) > G18_COMPUTE_MAX_BYTES / G18_COMPUTE_BLOCK_BYTES {
            return None;
        }
        let units = u64::from(blocks) * (G18_COMPUTE_BLOCK_BYTES >> 16);
        let per_usc = (units + USCS - 1) / USCS;
        let initial = ((G18_COMPUTE_DIRECTORY_PAGE - 0x400) >> 9) + 8;
        let excess = per_usc.saturating_sub(initial);
        let needed = (excess * 512 + G18_COMPUTE_DIRECTORY_PAGE - 1)
            / G18_COMPUTE_DIRECTORY_PAGE;
        let stride_pages = G18_COMPUTE_DIRECTORY_STRIDE / G18_COMPUTE_DIRECTORY_PAGE;
        let pages = if needed == 0 { 0 }
            else if needed <= stride_pages / 4 { stride_pages / 4 }
            else if needed <= stride_pages / 2 { stride_pages / 2 }
            else { stride_pages - 1 };
        if needed > pages { return None; }
        Some(Self { tail_pages_per_usc: pages as u32 })
    }

    pub(crate) fn reservation_bytes(&self) -> u64 {
        USCS * G18_COMPUTE_DIRECTORY_STRIDE + G18_COMPUTE_DIRECTORY_PAGE
    }

    pub(crate) fn committed_pages(&self) -> u32 {
        USCS as u32 + 1 + USCS as u32 * self.tail_pages_per_usc
    }

    /// Enumerate anchors first, followed by each USC's contiguous tail.
    pub(crate) fn page_offset(&self, index: u32) -> Option<u64> {
        if index >= self.committed_pages() { return None; }
        if index <= USCS as u32 {
            return Some(u64::from(index) * G18_COMPUTE_DIRECTORY_STRIDE);
        }
        let tail = u64::from(self.tail_pages_per_usc);
        let index = u64::from(index) - USCS - 1;
        let usc = index / tail;
        let page = index % tail;
        let offset = (usc + 1) * G18_COMPUTE_DIRECTORY_STRIDE
            - (tail - page) * G18_COMPUTE_DIRECTORY_PAGE;
        if offset + G18_COMPUTE_DIRECTORY_PAGE > self.reservation_bytes() { return None; }
        Some(offset)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct G18ComputeAllocation {
    pub(crate) bytes: u64,
    pub(crate) block_count: u32,
    pub(crate) register_pair: [u32; 2],
    pub(crate) chunk_counters: [u32; 2],
    pub(crate) chunks_per_usc: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct G18ComputePlan {
    pub(crate) ideal: G18ComputeAllocation,
    /// Native minimum is retained for comparison, not an allocation fallback.
    pub(crate) minimum: G18ComputeAllocation,
    /// Native AGXUMADataRec+0x100, packed above the slot in register0x10241.
    pub(crate) sma_units: u32,
}

impl G18ComputePlan {
    /// Serialize the ideal plan only after its backing and aliases are owned.
    /// Ownership, address-space validity and retirement are enforced by the
    /// pool owner, not this arithmetic helper. The initial hardware slot is
    /// unbound; load_hardware_descriptor publishes it in a separate step.
    pub(crate) fn pool_state(&self, statistics_id: u64, directory_base: u64,
        block_list_gpu: u64, counter_gpu: u64, counter: u64)
        -> Option<[u8; crate::g18::fw::uma::G18_POOL_STATE_BYTES]>
    {
        let directory = G18ComputeDirectory::for_blocks(self.ideal.block_count)?;
        if directory_base == 0 || directory_base & 0x3fff != 0
            || block_list_gpu == 0 || block_list_gpu & 0x3fff != 0
            || counter_gpu == 0 || counter_gpu & 7 != 0
            || directory_base.checked_add(directory.reservation_bytes())? > (1 << 48)
            || block_list_gpu.checked_add(0x4000)? > (1 << 48)
        {
            return None;
        }
        Some(crate::g18::fw::uma::encode_g18_pool_state(&crate::g18::fw::uma::G18UmaPoolState {
            statistics_id, grow_engine: 2, priority: 2,
            directory_gpu: directory_base + G18_COMPUTE_DIRECTORY_PAGE - 0x400,
            directory_stride: G18_COMPUTE_DIRECTORY_STRIDE,
            chunk_counters: self.ideal.chunk_counters,
            block_list_gpu, populated_blocks: self.ideal.block_count,
            block_units_64k: (G18_COMPUTE_BLOCK_BYTES >> 16) as u32,
            sma_mode: 0, preempt_counter_gpu: counter_gpu, preempt_counter: counter,
            compute: true, policy_value: 2,
        }))
    }

    /// Bounded compute-only descriptor from G18ComputeInputs::descriptor().
    /// The caller must finish its accumulator and preserve effective linked
    /// metadata. Render, protected, alternate-policy and concurrent-job
    /// contributions need their own contract and are rejected here.
    pub(crate) fn from_descriptor(words: &[u32; 23]) -> Option<Self> {
        for index in [2, 3, 6, 7, 13, 14, 15, 17, 18, 21, 22] {
            if words[index] != 0 { return None; }
        }
        if words[16] != 1 || words[19] != 128 || words[20] != 4
            || words[0] < 32 || words[1] < 10
            || words[0] > words[4] || words[1] > words[5]
        {
            return None;
        }
        let d = words.map(u64::from);
        // Original SIMD sequence at 8c916c0..8c91724. Each header
        // component rounds separately; summing before rounding is wrong.
        let h = &d[8..16];
        let header = h.iter().sum::<u64>() + d[16]
            + ((h[1] + 131) >> 7)
            + ((h[2] + h[3] + 127) >> 7)
            + ((h[4] + h[5] + 127) >> 7)
            + ((h[6] + h[2] + h[7] + d[16] + 127) >> 7);
        // Shared compute reserves its minimum in a separate contribution.
        let reserved = d[0] + d[1] + d[19] + d[20];
        let size = |ideal: bool| -> Option<G18ComputeAllocation> {
            let registers = if ideal { d[5] } else { d[1] };
            let scratch = if ideal { d[4] } else { d[0] };
            // One active job, up to two native shared-compute slots; native
            // helper 8c91ae4 includes the two-slot minimum only for ideal.
            let demand = scratch.saturating_sub(d[0] * 2)
                + (registers - d[1]) + if ideal { d[1] * 2 } else { 0 };
            let chunks = (demand + reserved + header + 3 + 63) & !63;
            if chunks > CHUNKS_PER_USC { return None; }
            let bytes = (chunks * USCS * 1024 + G18_COMPUTE_BLOCK_BYTES - 1)
                & !(G18_COMPUTE_BLOCK_BYTES - 1);
            if bytes == 0 || bytes > G18_COMPUTE_MAX_BYTES { return None; }
            // Both launch fields occupy sixteen bits in register 0xa5e1.
            if registers > 0xffff || header > 0xffff { return None; }
            Some(G18ComputeAllocation {
                bytes,
                block_count: u32::try_from(bytes / G18_COMPUTE_BLOCK_BYTES).ok()?,
                register_pair: [registers as u32, header as u32],
                chunk_counters: [0, u32::try_from(demand).ok()?],
                chunks_per_usc: u32::try_from(chunks).ok()?,
            })
        };
        Some(Self { ideal: size(true)?, minimum: size(false)?,
            sma_units: u32::try_from(reserved).ok()? })
    }
}
