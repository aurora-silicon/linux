// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
//
// The four-cluster 25G83 register layout and power configuration words of this profile derive
// from the M4 GPU encoders of Gravity Linux (g16_fw.rs, g16_platform.rs and g16_power.rs,
// written by Cody Ho):
// https://github.com/GravityLinux/linux/commit/bd25d194b0a295a75692a0e6caed3793603b9e65
// https://github.com/GravityLinux/linux/commit/3a2ad760d7316813f37c0ddc653ef783827f855b

//! T6041 (M4 Max) with 32 cores on the 25G83 and 26B123 firmware builds, as the J616C runs it.
//!
//! The J616C boots through a macOS 26.6.2 stub (iBoot mBoot-18000.161.10) or macOS 27.2 stub
//! (iBoot mBoot-20457.41.3). Its GPU has 32 of 40 cores in four clusters, so the 40-core
//! configuration refuses it; this one is selected instead.
//!
//! All per-unit calibration values come from the device tree, published by the boot loader under
//! `apple,t6041-handoff-version = 2`: operating points (`operating-points-v2`), CS and AFR tables
//! (`apple,cs-opp`, `apple,afr-opp`), performance selector and relative ladders
//! (`apple,perf-map-a/b`, `apple,perf-relative-a/b`, `apple,perf-base-pstate`), power ceiling
//! (`apple,max-power-mw`), four-cluster leakage coefficients (`apple,core-leak-coef`,
//! `apple,sram-leak-coef`, `apple,cs-leak-coef`, `apple,afr-leak-coef`), AON temperature-source
//! rows (`apple,aon-table`), core mask (`apple,core-mask`), and active MCC bit vector
//! (`apple,active-mcc-bit-vector`). There is no static power or leakage fallback.

use super::t6041;
use super::t604x::{instances, window};
use crate::g17::fw::{
    self,
    initdata::{v25g83, RegionCConfig, RegisterWindow},
};
use crate::g17::{Config, CoreLayout};

pub(super) const CONFIG: Config = Config {
    windows: &REGISTER_WINDOWS,
    reserved_slots: &[],
    perf: None,
    region_c: Some(&REGION_C),
    chip: Some(&CHIP),
    firmware: fw::variant::Firmware::Identified(&[t6041::BUILD_25G83_32CORE, t6041::BUILD_26B123]),
    core_layout: CoreLayout {
        cores: Some((4, 32)),
        alternate_core_masks: None,
        // Banks for all forty core slots: the firmware indexes them by physical slot, and the
        // J616C's enabled slots run to 39 (core mask 0xf6_fdbf6fdb). With banks for the 32
        // enabled cores only, a compute load beside rendering hit reason-0 recoveries.
        operand_state: fw::compute::OperandState::FOUR_BY_TEN,
        deflake: fw::render::Deflake::FOUR_CLUSTERS,
        // The compute pool the J616C has run with since 23 Oct 2026; the M4 Pro's 21 blocks
        // hang this GPU's firmware on grids of more than ten workgroups.
        compute_blocks: 96,
    },
    // The GPU is restarted after a real sleep, as on T8140.
    sleep_restart: true,
    ..t6041::BASE
};

/// The register map of [`t6041::REGISTER_WINDOWS_25G83`] with six memory controller
/// instances in slot 3 instead of eight, laid out by the same rule, so every window after it
/// lies 0x30000 lower. At boot, slot 3's instance count (and subsequent window addresses) are
/// resolved from the loader's `apple,active-mcc-bit-vector`.
const REGISTER_WINDOWS: [RegisterWindow; 25] = [
    window(0, 0x5_0241_4000, 0x20_0000, 0x4000, 0, 2),
    instances(
        window(3, 0x2_201c_4000, 0x20_8000, 0x1_8000, 0, 2),
        6,
        0x200_0000,
    ),
    window(4, 0x5_0244_c000, 0x29_c000, 0x4000, 0, 0),
    window(9, 0x5_003d_0000, 0x2a_4000, 0x4000, 0, 2),
    window(10, 0x5_003c_0000, 0x2a_c000, 0x4000, 0, 0),
    window(11, 0x5_003d_8000, 0x2b_4000, 0x4000, 0, 2),
    window(12, 0x4_8565_c000, 0x2b_c000, 0x4000, 0, 2),
    window(14, 0x5_0040_0000, 0x2c_4000, 0x1_0000, 0, 0),
    window(15, 0x5_0cdc_0000, 0x2d_8000, 0x4000, 0, 0),
    window(17, 0x2_8800_0000, 0x2e_0000, 0x2_1400, 0, 2),
    instances(
        window(20, 0x2_8b00_0000, 0x30_8000, 0x4_0000, 0, 2),
        4,
        0x20_0000,
    ),
    window(22, 0x2_8900_0000, 0x40_c000, 0x8000, 0, 2),
    window(26, 0x2_88d0_4000, 0x41_8000, 0x8000, 0xd0_4000, 2),
    window(28, 0x2_88d5_8000, 0x42_4000, 0x8000, 0xd5_8000, 2),
    window(29, 0x2_88d1_0000, 0x43_0000, 0x4000, 0xd1_0000, 2),
    window(31, 0x2_88d4_0000, 0x43_8000, 0x4000, 0xd4_0000, 2),
    window(32, 0x2_88d6_0000, 0x44_0000, 0x2_0000, 0xd6_0000, 2),
    window(33, 0x2_88d4_4000, 0x46_4000, 0x4000, 0xd4_4000, 2),
    window(34, 0x2_88d4_c000, 0x46_c000, 0x200, 0xd4_c000, 2),
    window(35, 0x2_88e0_0000, 0x47_4000, 0x4000, 0xe0_0000, 2),
    window(37, 0x2_88e4_0000, 0x47_c000, 0x4000, 0xe4_0000, 2),
    window(39, 0x2_88e0_8000, 0x48_4000, 0x8000, 0, 2),
    window(40, 0x2_88e1_c000, 0x49_0000, 0x4000, 0xe1_c000, 2),
    window(41, 0x2_88e1_f800, 0x49_b800, 0x4000, 0, 2),
    window(43, 0x2_88e6_0000, 0x4a_4000, 0x58, 0xe6_0000, 2),
];

// The windows of the 40-core 25G83 map, with six controllers, laid out by its rule.
const _: () = {
    assert!(super::t604x::laid_out_25g83(&REGISTER_WINDOWS));
    let mut index = 0;
    while index < REGISTER_WINDOWS.len() {
        let (max, ours) = (
            &t6041::REGISTER_WINDOWS_25G83[index],
            &REGISTER_WINDOWS[index],
        );
        assert!(
            max.slot == ours.slot
                && max.phys == ours.phys
                && max.size == ours.size
                && max.unk_18 == ours.unk_18
                && max.unk_20 == ours.unk_20
                && max.phys_stride == ours.phys_stride
                && (max.instances == ours.instances || (max.slot == 3 && ours.instances == 6))
        );
        index += 1;
    }
};

const REGION_C: RegionCConfig = RegionCConfig {
    max_power_mw: 0,
    ..t6041::REGION_C
};

const CHIP: v25g83::Chip = v25g83::Chip {
    max_power_mw: None,
    leakage: None,
    aon_table: true,
    hw_data_words: &[],
    power_config_words: POWER_CONFIG_WORDS,
    perf_a_replicate_lanes: false,
    cs_perf_table: true,
};

/// Four-cluster T6041 power configuration words that differ from the two-cluster 25G83 base.
const POWER_CONFIG_WORDS: &[(usize, u32, usize)] = &[
    // The second temperature-sensor mask word.
    (0x3b5c, 0x2a22_2aa2, 1),
    // The temperature sources of GPU clusters 2 and 3.
    (0x6328, u32::from_be_bytes(*b"g2TR"), 1),
    (0x632c, u32::from_be_bytes(*b"g3TR"), 1),
    (0x6360, 0x5, 1),
];
