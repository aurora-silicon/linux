// SPDX-License-Identifier: GPL-2.0-only OR MIT


#[cfg(not(test))]
use crate::identity::{GpuGen, GpuVariant};
#[cfg(test)]
#[path = "../identity.rs"]
mod identity;
#[cfg(test)]
use identity::{GpuGen, GpuVariant};

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

pub(crate) const T8122: SocConfig = agx3(0x8122, GpuGen::G15, GpuVariant::G, 0x7, 2, 1);
/// Apple M3 Pro (G15S).
pub(crate) const T6030: SocConfig = agx3(0x6030, GpuGen::G15, GpuVariant::S, 0x7, 3, 1);
/// Apple M3 Max (G15C).
pub(crate) const T6031: SocConfig = agx3(0x6031, GpuGen::G15, GpuVariant::C, 0x7, 4, 1);
/// Apple M3 Ultra (G15D, two dies).
pub(crate) const T6032: SocConfig = agx3(0x6032, GpuGen::G15, GpuVariant::D, 0x7, 4, 2);

pub(crate) const T8140: SocConfig = agx3(0x8140, GpuGen::G17, GpuVariant::P, 0xa, 0, 1);
/// Apple M4 (G16G).
pub(crate) const T8132: SocConfig = agx3(0x8132, GpuGen::G16, GpuVariant::G, 0xa, 2, 1);
/// Apple M4 Pro (G16S).
pub(crate) const T6040: SocConfig = agx3(0x6040, GpuGen::G16, GpuVariant::S, 0xa, 3, 1);
/// Apple M4 Max (G16C).
pub(crate) const T6041: SocConfig = agx3(0x6041, GpuGen::G16, GpuVariant::C, 0xa, 4, 1);

pub(crate) const T8142: SocConfig = agx3(0x8142, GpuGen::G17, GpuVariant::G, 0xb, 2, 1);
/// Apple M5 Pro (G17S).
pub(crate) const T6050: SocConfig = agx3(0x6050, GpuGen::G17, GpuVariant::S, 0xb, 3, 1);
/// Apple M5 Max (G17C).
pub(crate) const T6051: SocConfig = agx3(0x6051, GpuGen::G17, GpuVariant::C, 0xb, 4, 1);


// -------------------------------------------------------------------------
// T8140 HwConfig grounding ledger.
//
// The legacy `super::HwConfig` carries ~40 fields. Only the identity and
// geometry axes below are grounded for T8140 (from `SocConfig`, the live
// J700 registers, and the G17P init-data model). The rest are AGX2
// firmware-ABI power/tuning constants with no T8140 derivation, so no full
// `HwConfig` can be constructed without inventing them. This ledger names
// exactly which field groups are missing and lets the probe path fail
// closed with a precise reason instead of fabricating a config.
// -------------------------------------------------------------------------

#[cfg(not(test))]
#[allow(dead_code)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140GroundedHwConfig {
    pub(crate) chip_id: u32,
    /// Base timekeeping clock: the 24 MHz `clkref`, verified in the J700 DT.
    pub(crate) base_clock_hz: u32,
    /// 42-bit UAT output address space.
    pub(crate) uat_oas: u32,
    /// 42-bit UAT input address space per root.
    pub(crate) uat_ias: u8,
    /// Single die (live `0xd04010` read = `0x00110106`).
    pub(crate) num_dies: u32,
    /// Firmware `GpuCore` id, always `None` for T8140 (see above).
    pub(crate) gpu_core: Option<super::GpuCore>,
}

/// The exact grounded T8140 HwConfig subset.
#[cfg(not(test))]
pub(crate) const T8140_GROUNDED_HWCONFIG: T8140GroundedHwConfig = T8140GroundedHwConfig {
    chip_id: 0x8140,
    base_clock_hz: 24_000_000,
    uat_oas: 42,
    uat_ias: 42,
    num_dies: 1,
    gpu_core: None,
};

/// The `HwConfig` field groups that are ungrounded for T8140.
pub(crate) mod hwconfig_gap {
    /// Firmware `GpuCore` id (per-firmware-build ABI; not recovered).
    pub(crate) const GPU_CORE_ID: u16 = 1 << 0;
    /// Preemption buffer sizes (`preempt{1,2,3}_size`, `compute_preempt1`).
    pub(crate) const PREEMPT_SIZES: u16 = 1 << 1;
    /// `HwConfigA`/`HwConfigB` unknown tuning words.
    pub(crate) const HWCONFIG_A_B: u16 = 1 << 2;
    /// Shared power curve tables (`shared1/2/3`, `unk_coef_a/b`, `sram_k`).
    pub(crate) const POWER_CURVES: u16 = 1 << 3;
    /// Thermal sensor masks (`fast_sensor_mask*`, `fast_die0_sensor_present`).
    pub(crate) const SENSOR_MASKS: u16 = 1 << 4;
    /// Firmware MMIO mapping table (`io_mappings`) and SRAM window.
    pub(crate) const IO_MAPPINGS: u16 = 1 << 5;
    /// Clustering metadata block sizes (`HwClusteringConfig`).
    pub(crate) const CLUSTERING: u16 = 1 << 6;
    /// Global misc fields (`idle_off_standby_timer_default`, `unk_hws2_*`,
    /// `global_unk_54`, `global_tab`, `has_csafr`, render tiling control).
    pub(crate) const GLOBAL_MISC: u16 = 1 << 7;
}

/// Every `HwConfig` field group that T8140 does not yet ground.
pub(crate) const T8140_HWCONFIG_MISSING: u16 = hwconfig_gap::GPU_CORE_ID
    | hwconfig_gap::PREEMPT_SIZES
    | hwconfig_gap::HWCONFIG_A_B
    | hwconfig_gap::POWER_CURVES
    | hwconfig_gap::SENSOR_MASKS
    | hwconfig_gap::IO_MAPPINGS
    | hwconfig_gap::CLUSTERING
    | hwconfig_gap::GLOBAL_MISC;

/// Opaque fail-closed result of a T8140 HwConfig construction attempt.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct HwConfigGap(u16);

impl HwConfigGap {
    pub(crate) const fn bits(self) -> u16 {
        self.0
    }

    /// Whether a specific field group is among the missing ones. Used by the
    /// gap tests and the future per-group grounding path.
    #[allow(dead_code)]
    pub(crate) const fn contains(self, group: u16) -> bool {
        self.0 & group != 0
    }
}

/// Attempt to construct a full T8140 `HwConfig`.
///
/// This assembles the grounded subset ([`T8140_GROUNDED_HWCONFIG`]) and then
/// **fails closed**: the ~32 remaining AGX2 firmware-ABI power/tuning fields
/// have no T8140 derivation, so returning a complete `HwConfig` would require
/// inventing them. The returned gap names exactly which field groups are
/// missing, so the probe path can report a precise wall rather than a bare
/// error. When those groups are eventually grounded, this becomes the place
/// that returns `Ok(&'static HwConfig)`.
#[cfg(not(test))]
pub(crate) fn t8140_hwconfig() -> Result<&'static super::HwConfig, HwConfigGap> {
    // Grounded fields exist ([`T8140_GROUNDED_HWCONFIG`]); the rest do not.
    let _grounded = &T8140_GROUNDED_HWCONFIG;
    Err(HwConfigGap(T8140_HWCONFIG_MISSING))
}

#[cfg(test)]
mod tests {
    use super::identity::{
        decode_gpu_identity, FirmwareRoleTopology, GpuGen, GpuHalGeneration, SubmissionTransport,
    };
    use super::*;

    const ALL: [&SocConfig; 11] = [
        &T8122, &T6030, &T6031, &T6032, &T8140, &T8132, &T6040, &T6041, &T8142, &T6050, &T6051,
    ];

    #[test]
    fn every_soc_decodes_to_its_stated_identity() {
        for soc in ALL {
            let identity = decode_gpu_identity(soc.hw_family, soc.hw_variant, soc.num_dies as u8)
                .expect("static AGX3 identity must decode");
            assert_eq!(identity.gpu_gen, soc.gpu_gen, "chip {:#x}", soc.chip_id);
            assert_eq!(
                identity.gpu_variant, soc.gpu_variant,
                "chip {:#x}",
                soc.chip_id
            );
            // Every Mac AGX3 part is USC generation 3 with 42-bit UAT roots.
            assert_eq!(identity.usc_generation, 3, "chip {:#x}", soc.chip_id);
            assert_eq!(
                identity.uat_input_address_bits, soc.uat_ias,
                "chip {:#x}",
                soc.chip_id
            );
            assert_eq!(soc.uat_ias, 42);
            assert_eq!(soc.uat_oas, 42);
        }
    }

    #[test]
    fn firmware_role_topology_follows_the_generation() {
        for soc in ALL {
            let identity =
                decode_gpu_identity(soc.hw_family, soc.hw_variant, soc.num_dies as u8).unwrap();
            let expected = if soc.gpu_gen == GpuGen::G17 {
                FirmwareRoleTopology::Dual
            } else {
                FirmwareRoleTopology::Single
            };
            assert_eq!(identity.firmware_roles, expected, "chip {:#x}", soc.chip_id);
        }
    }

    #[test]
    fn hal300_parts_are_sksm_only() {
        for soc in ALL {
            let identity =
                decode_gpu_identity(soc.hw_family, soc.hw_variant, soc.num_dies as u8).unwrap();
            if identity.gpu_hal_generation == GpuHalGeneration::Hal300 {
                assert_eq!(
                    identity.submission_transport,
                    SubmissionTransport::Sksm,
                    "chip {:#x}",
                    soc.chip_id
                );
            }
        }
    }

    #[test]
    fn chip_ids_are_unique_and_match_their_family_tier() {
        for (i, a) in ALL.iter().enumerate() {
            for b in ALL.iter().skip(i + 1) {
                assert_ne!(a.chip_id, b.chip_id);
            }
            // Mac 0x8xxx chips are single-cluster consumer parts (G variant),
            // except mobile T8140 whose independently decoded tier is P.
            // 0x6xxx chips are the S/C/D tiers.
            if a.chip_id == 0x8140 {
                assert_eq!(a.gpu_variant, GpuVariant::P);
            } else if a.chip_id & 0xf000 == 0x8000 {
                assert_eq!(a.gpu_variant, GpuVariant::G);
            } else {
                assert_ne!(a.gpu_variant, GpuVariant::G);
            }
        }
    }

    #[test]
    fn t8140_is_the_exact_a18_pro_g17p_diagnostic_identity() {
        assert_eq!(T8140.chip_id, 0x8140);
        assert_eq!(T8140.hw_family, 0xa);
        assert_eq!(T8140.hw_variant, 0);
        assert_eq!(T8140.num_dies, 1);
        let identity =
            decode_gpu_identity(T8140.hw_family, T8140.hw_variant, T8140.num_dies as u8).unwrap();
        assert_eq!(identity.gpu_gen, GpuGen::G17);
        assert_eq!(identity.gpu_variant, GpuVariant::P);
    }

    #[test]
    fn t8140_hwconfig_gap_enumerates_every_ungrounded_field_group() {
        // The gap must name every ungrounded field group and nothing else.
        let gap = HwConfigGap(T8140_HWCONFIG_MISSING);
        for group in [
            hwconfig_gap::GPU_CORE_ID,
            hwconfig_gap::PREEMPT_SIZES,
            hwconfig_gap::HWCONFIG_A_B,
            hwconfig_gap::POWER_CURVES,
            hwconfig_gap::SENSOR_MASKS,
            hwconfig_gap::IO_MAPPINGS,
            hwconfig_gap::CLUSTERING,
            hwconfig_gap::GLOBAL_MISC,
        ] {
            assert!(gap.contains(group));
        }
        // Eight distinct single-bit groups.
        assert_eq!(T8140_HWCONFIG_MISSING.count_ones(), 8);
        assert_eq!(gap.bits(), T8140_HWCONFIG_MISSING);
    }
}
