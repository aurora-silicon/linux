// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounded G18 compute resource demand, before pool sizing or allocation.
//!
//! Derived from 26A428 HAL400 SpillInfoGen5::checkSpillParamsForCompute and
//! allocateUSCSpillBuffer. It was compared
//! with the original code. This module is not yet
//! connected to submission; it does not admit scratch or allocate GPU memory.

/// Original Device constructor default (0x1ace020b8 in the exact BaseSystem).
/// Capacity is 64 KiB plus this many 4 KiB units; 63 disables the limit.
pub(crate) const G18_TGMEM_POLICY: u32 = 12;

/// Inputs in native units. Main and constant-program storage are independent.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18ComputeDemand {
    pub(crate) scratch_bytes: u32,
    pub(crate) constant_scratch_bytes: u32,
    pub(crate) registers_32: u32,
    pub(crate) ipr_bytes: u32,
    pub(crate) constant_ipr_bytes: u32,
    pub(crate) threadgroup_bytes: u32,
    pub(crate) imageblock_bytes: u32,
    /// Zero means unknown local geometry, not an empty global grid.
    pub(crate) threads: u32,
}

/// Effective compiler metadata, including any linked prolog/epilog demand.
/// Counts use Mesa's units. This is an internal type, not a userspace ABI.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18ComputeShader {
    pub(crate) registers_16: u32,
    pub(crate) scratch_bytes: u32,
    pub(crate) spectpr_scratch_bytes: u32,
    pub(crate) staging_16: u32,
    pub(crate) staging_known: bool,
    pub(crate) constant_present: bool,
    pub(crate) constant_registers_16: u32,
    pub(crate) constant_scratch_bytes: u32,
    pub(crate) constant_staging_16: u32,
    pub(crate) constant_staging_known: bool,
    pub(crate) local_bytes: u32,
    pub(crate) imageblock_stride: u32,
}

impl G18ComputeDemand {
    /// Legacy metadata v1 and precompiled G17G kernels use byte offsets in
    /// element-index fields, requiring a fourfold SPECLM footprint.
    pub(crate) fn from_shader(shader: G18ComputeShader, local: Option<[u32; 3]>,
        variable_shared_bytes: u32) -> Option<Self> {
        Self::from_shader_scaled(shader, local, variable_shared_bytes, 4)
    }

    /// Metadata v2 describes shaders with correctly lowered element indices.
    /// Their static and variable shared storage both count actual bytes.
    pub(crate) fn from_shader_element_indices(shader: G18ComputeShader,
        local: Option<[u32; 3]>, variable_shared_bytes: u32) -> Option<Self> {
        Self::from_shader_scaled(shader, local, variable_shared_bytes, 1)
    }

    fn from_shader_scaled(shader: G18ComputeShader, local: Option<[u32; 3]>,
        variable_shared_bytes: u32, shared_scale: u32) -> Option<Self> {
        if shader.registers_16 == 0 || shader.registers_16 > 256
            || !shader.staging_known || shader.staging_16 > 0xffff
            || shader.imageblock_stride != 0
        {
            return None;
        }
        if shader.constant_present {
            if shader.constant_registers_16 == 0 || shader.constant_registers_16 > 256
                || !shader.constant_staging_known || shader.constant_staging_16 > 0xffff
            {
                return None;
            }
        } else if shader.constant_registers_16 != 0 || shader.constant_scratch_bytes != 0
            || shader.constant_staging_16 != 0 || shader.constant_staging_known
        {
            return None;
        }
        let threads = if let Some(dims) = local {
            if dims.contains(&0) { return None; }
            dims[0].checked_mul(dims[1])?.checked_mul(dims[2])?
        } else { 0 };
        if threads > 1024 { return None; }
        let scratch = shader.scratch_bytes.max(shader.spectpr_scratch_bytes);
        // Match the qualified static AGX3 TPS frame's 16-byte alignment.
        let scratch = scratch.checked_add(15)? & !15;
        if scratch > 0xffff * 4 || shader.constant_scratch_bytes > 0xffff * 4 {
            return None;
        }
        let shared = shader.local_bytes.checked_add(variable_shared_bytes)?.checked_mul(shared_scale)?;
        let shared = if shared == 0 { 0 } else { (shared.checked_add(63)? & !63).max(256) };
        if shared > 32768 { return None; }
        Some(Self {
            scratch_bytes: scratch,
            constant_scratch_bytes: shader.constant_scratch_bytes,
            registers_32: (shader.registers_16 + 1) / 2,
            ipr_bytes: shader.staging_16 * 2,
            constant_ipr_bytes: shader.constant_staging_16 * 2,
            threadgroup_bytes: shared,
            imageblock_bytes: 0,
            threads,
        })
    }
}

/// Native compute-only accumulator. Keep component maxima until serialization.
/// The two pointer words stay zero; no host or GPU pointer is encoded here.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct G18ComputeInputs {
    words: [u32; 58],
}

impl G18ComputeInputs {
    pub(crate) fn new() -> Self {
        let mut result = Self { words: [0; 58] };
        result.words[0xc4 / 4] = 4;
        result.words[0xc8 / 4] = 1;
        result.words[0xcc / 4] = 128;
        result
    }

    fn maximum(&mut self, offset: usize, value: u64) -> Option<()> {
        let value = u32::try_from(value).ok()?;
        self.words[offset / 4] = self.words[offset / 4].max(value);
        Some(())
    }

    /// Arithmetic domain covers 16-bit SPECTPR word counts, 128 32-bit GPRs,
    /// current GL threadgroup memory, and at most 1024 local invocations.
    /// Validation is independent of later pool/backing limits. A rejected
    /// merge never modifies the caller's accumulated state.
    pub(crate) fn merge(&mut self, d: G18ComputeDemand, tgmem_policy: u32) -> Option<()> {
        if d.scratch_bytes > 0xffff * 4
            || d.constant_scratch_bytes > 0xffff * 4
            || d.registers_32 > 128
            || d.ipr_bytes > 0xffff * 2
            || d.constant_ipr_bytes > 0xffff * 2
            || d.threadgroup_bytes > 32768
            || d.imageblock_bytes > 32768
            || d.threads > 1024
            || tgmem_policy > 63
        {
            return None;
        }

        // Calculate in u64, retaining native rounding order. The input bounds
        // make every intermediate fit the original unsigned 32-bit domain.
        let align = |v: u64, a: u64| (v + a - 1) & !(a - 1);
        let pages = |v: u64| (v + 8191) / 8192;
        let warps = if d.threads == 0 { 32 } else { (u64::from(d.threads) + 31) / 32 };
        let mut groups = 96 / if d.threads == 0 { 1 } else { warps };
        if d.threadgroup_bytes != 0 && tgmem_policy < 63 {
            let capacity = 65536 + u64::from(tgmem_policy) * 4096;
            groups = groups.min(capacity / align(u64::from(d.threadgroup_bytes), 64));
        }
        let ideal_warps = (groups * warps).min(96);
        let minimum_page_warps = warps.min(8);
        let ideal_page_warps = (groups * warps).min(8);

        let scratch_warp = align(u64::from(d.scratch_bytes) * 32 + 896, 1024);
        let register_warp = align(u64::from(d.registers_32) * 128, 1024);
        let ipr_warp = align(u64::from(d.ipr_bytes) * 32, 1024);
        let constant_scratch = align(u64::from(d.constant_scratch_bytes) * 32 + 896, 1024);
        let constant_ipr = align(u64::from(d.constant_ipr_bytes) * 32, 1024);
        let constant_floor = (constant_scratch + constant_ipr) / 1024 + 16;
        let warp_kib = (scratch_warp + register_warp + ipr_warp) / 1024;
        let local_kib = align(u64::from(d.threadgroup_bytes), 1024) / 1024;
        let image_kib = align(u64::from(d.imageblock_bytes), 1024) / 1024;
        let scratch_pages = pages(scratch_warp);
        let register_pages = pages(register_warp);
        let ipr_pages = pages(ipr_warp);
        let constant_scratch_pages = pages(constant_scratch);
        let constant_ipr_pages = pages(constant_ipr);
        let local_pages = pages(u64::from(d.threadgroup_bytes));
        let image_pages = pages(u64::from(d.imageblock_bytes));

        let mut next = *self;
        next.words[0x08 / 4] = 3;
        next.maximum(0x44, (warps * warp_kib).max(constant_floor) + local_kib + image_kib)?;
        next.maximum(0x84, (ideal_warps * warp_kib).max(constant_floor) + groups * (local_kib + image_kib))?;
        next.maximum(0x0c, (minimum_page_warps * register_pages).max(2))?;
        next.maximum(0x10, (minimum_page_warps * scratch_pages).max(constant_scratch_pages))?;
        next.maximum(0x14, (minimum_page_warps * ipr_pages).max(constant_ipr_pages))?;
        next.maximum(0x18, register_pages.max(2))?;
        next.maximum(0x1c, scratch_pages.max(constant_scratch_pages))?;
        next.maximum(0x20, ipr_pages.max(constant_ipr_pages))?;
        next.maximum(0x24, local_pages)?;
        next.maximum(0x28, image_pages)?;
        next.maximum(0x30, local_pages)?;
        next.maximum(0x34, image_pages)?;
        next.maximum(0x4c, (ideal_page_warps * register_pages).max(2))?;
        next.maximum(0x50, (ideal_page_warps * scratch_pages).max(constant_scratch_pages))?;
        next.maximum(0x54, (ideal_page_warps * ipr_pages).max(constant_ipr_pages))?;
        next.maximum(0x58, register_pages.max(2))?;
        next.maximum(0x5c, scratch_pages.max(constant_scratch_pages))?;
        next.maximum(0x60, ipr_pages.max(constant_ipr_pages))?;
        next.maximum(0x64, local_pages * groups.min(6))?;
        next.maximum(0x68, image_pages * groups.min(6))?;
        next.maximum(0x70, local_pages)?;
        next.maximum(0x74, image_pages)?;
        *self = next;
        Some(())
    }

    /// Original endComputePass contributes an all-zero, unknown-geometry job.
    pub(crate) fn finish(&mut self) -> Option<()> {
        self.merge(G18ComputeDemand::default(), G18_TGMEM_POLICY)
    }

    /// Compute-only AGXSMADescRec. Render/ray/protected-memory contributors
    /// remain zero and cannot be admitted through this interface.
    pub(crate) fn descriptor(&self) -> [u32; 23] {
        let w = |off: usize| self.words[off / 4];
        [w(0x44), w(0x0c) + w(0x10) + w(0x14) + w(0x24) + w(0x28), 0, 0,
         w(0x84), w(0x4c) + w(0x50) + w(0x54) + w(0x64) + w(0x68), 0, 0,
         w(0x58), w(0x5c), w(0x60), w(0x70), w(0x74), 0, 0, 0,
         w(0xc8), 0, 0, w(0xcc), w(0xc4), 0, 0]
    }

    /// Expose component state for the retained original-instruction audit.
    pub(crate) fn component_words(&self) -> &[u32; 58] {
        &self.words
    }
}
