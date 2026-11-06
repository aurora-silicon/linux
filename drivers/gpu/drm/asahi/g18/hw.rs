// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18G revision and identity types, and the T8152 SoC entry.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten. The SoC entry is in `hw::t8152`.
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

// The T8152 SoC entry and its registry facts live with the other SoCs.
pub(crate) use crate::hw::t8152::{G18RegistryFacts, SocConfig, T8152, T8152_REGISTRY};
