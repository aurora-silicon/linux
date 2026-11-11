// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware configuration for t8152 platforms (M6, G18G).
//!
//! The M6 GPU runs on the G18 backend (`crate::g18`), which uses its own
//! firmware interface and therefore an identification-only SoC entry rather
//! than a [`super::HwConfig`].

use crate::g18::identity::{GpuGen, GpuVariant};

/// Identification-only configuration for one AGX3 SoC.
#[allow(dead_code)]
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
