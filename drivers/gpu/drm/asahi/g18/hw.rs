// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T8152 SoC configuration and the native G18G registry facts.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.
use kernel::prelude::*;
pub(crate) use crate::g18::identity::{GpuGen, GpuHalGeneration, GpuVariant};

/// The `hw::agx3` paths used by the carried module-1134 code.
#[allow(unused_imports)]
pub(crate) mod agx3 {
    pub(crate) use super::{G18RegistryFacts, SocConfig, T8152, T8152_REGISTRY};
}

/// GPU revision enumeration. Note: Part of the UABI.
#[derive(Debug, PartialEq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuRevision {
    A0 = 0x00,
    A1 = 0x01,
    B0 = 0x10,
    B1 = 0x11,
    C0 = 0x20,
    C1 = 0x21,
}

/// GPU revision ID. Note: Part of the firmware ABI.
#[derive(Debug, PartialEq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuRevisionID {
    // Unknown = 0,
    A0 = 1,
    A1 = 2,
    B0 = 3,
    B1 = 4,
    C0 = 5,
    C1 = 6,
}

/// Specific GPU ID configuration fetched from SGX MMIO registers.
#[derive(Debug)]
pub(crate) struct GpuIdConfig {
    /// GPU generation (should match static config).
    pub(crate) gpu_gen: GpuGen,
    /// GPU variant type (should match static config).
    pub(crate) gpu_variant: GpuVariant,
    /// Unified shader core generation.
    pub(crate) usc_generation: u32,
    /// Apple GPU HAL generation (0 for the pre-numbered generations).
    pub(crate) gpu_hal_generation: GpuHalGeneration,
    /// GPU silicon revision.
    pub(crate) gpu_rev: GpuRevision,
    /// GPU silicon revision ID (firmware enum).
    pub(crate) gpu_rev_id: GpuRevisionID,
    /// Number of GPU dies read from the ID register.
    pub(crate) num_dies: u32,
    /// Total number of GPU clusters.
    pub(crate) num_clusters: u32,
    /// Maximum number of GPU cores per cluster.
    pub(crate) num_cores: u32,
    /// Number of frags per cluster.
    pub(crate) num_frags: u32,
    /// Number of GPs per cluster.
    pub(crate) num_gps: u32,
    /// Total number of active cores for the whole GPU.
    pub(crate) total_active_cores: u32,
    /// Mask of active cores per cluster.
    pub(crate) core_masks: KVec<u32>,
    /// Packed mask of all active cores.
    pub(crate) core_masks_packed: KVec<u32>,
    /// GPC performance-state map nibble sampled during the safe SGX ID probe.
    /// G17P emits this cached high nibble in fragment register 0x1c838. The
    /// 3D descriptor tail at +0x2110 has an independent render-setup source.
    pub(crate) gpc_perf_state_map: u32,
    /// Low nibble of the same SGX 0xe01480 word, written to 3D tail +0x210c.
    pub(crate) gpc_perf_state_map_low: u32,
    /// Bit 0 of SGX 0xe0141c, written to 3D tail +0x2104.
    pub(crate) gpc_perf_state_control: u32,
}

/// Identification-only configuration for one AGX3 SoC.
#[derive(Debug)]
pub(crate) struct SocConfig {
    /// Chip ID in hex format (e.g. 0x6050 for t6050).
    pub(crate) chip_id: u32,
    /// Decoded design generation for this chip.
    pub(crate) gpu_gen: GpuGen,
    /// Decoded variant for this chip.
    pub(crate) gpu_variant: GpuVariant,
    /// Hardware family byte expected at `0xd04000[31:24]`.
    pub(crate) hw_family: u8,
    /// Hardware variant byte expected at `0xd04000[23:16]`.
    pub(crate) hw_variant: u8,
    /// Die count expected at `0xd04010[19:16]`.
    pub(crate) num_dies: u32,
    /// Input-address bits per UAT root (42 on every AGX3 part).
    pub(crate) uat_ias: u8,
    /// Output address space (42-bit on every AGX3 part).
    pub(crate) uat_oas: u32,
}

const fn agx3(
    chip_id: u32,
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    hw_family: u8,
    hw_variant: u8,
    num_dies: u32,
) -> SocConfig {
    SocConfig {
        chip_id,
        gpu_gen,
        gpu_variant,
        hw_family,
        hw_variant,
        num_dies,
        uat_ias: 42,
        uat_oas: 42,
    }
}

// G18 (M6 family). Family byte 0xc, variant 2: 26A428
// `AGX·PI_400·G·A0·Accelerator::readChipInfo` @ 0xfffffe0008c3d3a0 compares
// the family byte with 0xc @ 0xfffffe0008c3d49c and gives variant 2 the
// 12-core G configuration @ 0xfffffe0008c3d4bc..0x4c8. Native J873 registry
// (AGXAcceleratorG18G): gpu_gen=18, gpu_var=G, usc_gen=3, is_sksm=1,
// num_cores=12, num_gps=6, num_mgpus=2, core_mask_list=(63,63),
// num_frags=12, kickid_qid_shift=40, kickid_qid_mask=127. The live ID
// registers have not been read yet (the GPU PSD reads idle at cold boot), so
// the T8152 probe identifies the part by chip id and compatible, and only
// logs a mismatching register decode. The ADT uat-vaddr-size is 43 on both
// J813 and J873: 42 bits per root plus the TTBR-select bit, as on T8142.
/// Apple M6 (G18G, J873 Mac mini).
pub(crate) const T8152: SocConfig = agx3(0x8152, GpuGen::G18, GpuVariant::G, 0xc, 2, 1);

/// Native J873 `AGXAcceleratorG18G` registry facts (macOS 26A428 ioreg).
///
/// These are identity facts, not firmware tuning: they size the core-mask
/// decode and cross-check the live ID registers once the GPU is powered.
#[allow(dead_code)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RegistryFacts {
    pub(crate) num_cores: u32,
    pub(crate) num_gps: u32,
    pub(crate) num_mgpus: u32,
    /// Per-mGPU (cluster) core masks, `core_mask_list`.
    pub(crate) core_mask_list: [u32; 2],
    pub(crate) num_frags: u32,
    pub(crate) usc_generation: u32,
    pub(crate) kickid_qid_shift: u32,
    pub(crate) kickid_qid_mask: u32,
}

/// J873 registry facts (PROVEN from the native ioreg).
#[allow(dead_code)]
pub(crate) const T8152_REGISTRY: G18RegistryFacts = G18RegistryFacts {
    num_cores: 12,
    num_gps: 6,
    num_mgpus: 2,
    core_mask_list: [0x3f, 0x3f],
    num_frags: 12,
    usc_generation: 3,
    kickid_qid_shift: 40,
    kickid_qid_mask: 127,
};
