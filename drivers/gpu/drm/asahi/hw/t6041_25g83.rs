// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
//
// The power configuration words, leakage coefficients, power limit and AON rows of this
// profile are what the M4 GPU encoders of Gravity Linux (g16_fw.rs, g16_platform.rs and
// g16_power.rs, written by Cody Ho, with the bootloader power limit added by Niklas Sheth)
// built on a 32-core J616C:
// https://github.com/GravityLinux/linux/commit/bd25d194b0a295a75692a0e6caed3793603b9e65
// https://github.com/GravityLinux/linux/commit/3a2ad760d7316813f37c0ddc653ef783827f855b

//! T6041 (M4 Max) with 32 cores on the 25G83 firmware build, as the J616C runs it.
//!
//! The J616C boots through a macOS 26.6.2 stub (iBoot mBoot-18000.161.10), which loads the
//! 25G83 image [`t6041::BUILD_25G83`] admits. Its GPU has 32 of 40 cores in four clusters, so
//! the 40-core configuration of that build refuses it; this one is selected instead.
//!
//! What differs between units comes from the device tree, which the boot loader fills from the
//! ADT: the operating points with one voltage per cluster (`operating-points-v2`, from the
//! ADT `perf-states` tables) and the CS and AFR tables (`apple,cs-opp`, `apple,afr-opp`, from
//! `cs-perf-states` and `afr-perf-states`). The builders turn them into the performance, CS and
//! AFR tables of the hardware data. The enabled cores are probed from the GPU.
//!
//! Everything else is static, because the driver has no source for it on T6041:
//! - [`PERF`]: the selector ladders, see there.
//! - [`MAX_POWER_MW`] and the leakage coefficients of [`CHIP`]: not in the ADT. They are the
//!   M4 Pro's values (its per-core power budget for 32 cores, its per-cluster coefficients for
//!   four clusters), not values of this unit.
//! - [`AON_TABLE`]: the GPU cluster and SoC rows of the ADT's `/arm-io/aon-ptd`
//!   `ptd_data_table`, which the loader does not publish.
//! - [`REGISTER_WINDOWS`]: slot 3 holds this part's six memory controllers (ADT
//!   `/arm-io/sgx` `active-mcc-bit-vector` 0x3f).
//!
//! Built from the device tree of the J616C, the hardware data and region C equal what this
//! unit ran with since October 2026, and the power configuration does with [`CHIP`]'s words.

use super::t6041;
use super::t604x::{instances, window};
use crate::g17::fw::{
    self,
    initdata::{v25g83, PerfConfig, RegionCConfig, RegisterWindow, PERF_STATES},
};
use crate::g17::{Config, CoreLayout};

pub(super) const CONFIG: Config = Config {
    windows: &REGISTER_WINDOWS,
    // The empty register-map slots macOS 26.6.2 marks reserved on the J616C, as on the J614C.
    reserved_slots: &t6041::RESERVED_SLOTS,
    perf: Some(&PERF),
    region_c: Some(&REGION_C),
    chip: Some(&CHIP),
    firmware: fw::variant::Firmware::Identified(&[t6041::BUILD_25G83, t6041::BUILD_26B123]),
    core_layout: CoreLayout {
        cores: Some((4, 32)),
        alternate_core_masks: None,
        // Banks for all forty core slots: the firmware indexes them by physical slot, and the
        // J616C's enabled slots run to 39 (core mask 0xf6_fdbf6fdb). With banks for the 32
        // enabled cores only, a compute load beside rendering hit reason-0 recoveries.
        // macOS 26.6.2 on the J616C writes 0xf7 as the first compute PTC cap (0x0a599; 0x60
        // after the work, as here), where the M4 Pro writes 0x60 both times.
        operand_state: fw::compute::OperandState::FOUR_BY_TEN.with_ptc_first(0xf7_0040_0020),
        deflake: fw::render::Deflake::FOUR_CLUSTERS,
        // The compute pool the J616C has run with since 23 Oct 2026; the M4 Pro's 21 blocks
        // hang this GPU's firmware on grids of more than ten workgroups.
        compute_blocks: 96,
    },
    ..t6041::BASE
};

/// The register map of [`t6041::REGISTER_WINDOWS_25G83`] with six memory controller
/// instances in slot 3 instead of eight, laid out by the same rule, so every window after it
/// lies 0x30000 lower.
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

/// The selector ladders, as macOS 26.6.2 writes them on the J616C (hardware data captured under
/// a hypervisor): `states_a` and `states_b` pick the J616C's operating points in ADT order, with
/// OPP 6 for state 5 of `states_b` as on the J614C; `relative_a` is the J616C's own.
const PERF: PerfConfig = PerfConfig {
    states_a: [0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15],
    states_b: [0, 1, 2, 3, 4, 6, 7, 9, 11, 13, 15],
    relative_a: [0, 10, 18, 25, 31, 40, 51, 62, 69, 78, 100],
    relative_b: [0, 0, 22, 36, 47, 57, 68, 78, 84, 91, 100],
    sram_min_mv: 810,
};

const _: () = {
    let mut index = 0;
    while index < PERF_STATES {
        assert!((PERF.states_a[index] as usize) < t6041::OPPS);
        assert!((PERF.states_b[index] as usize) < t6041::OPPS);
        index += 1;
    }
};

/// The power ceiling macOS 26.6.2 writes on the J616C.
const MAX_POWER_MW: u32 = 113_169;

const REGION_C: RegionCConfig = RegionCConfig {
    max_power_mw: MAX_POWER_MW,
    ..t6041::REGION_C
};

/// The AON temperature-source rows (12 bytes each) the power configuration names: GPU clusters
/// 0 to 3 and the SoC, as in the ADT's `ptd_data_table`.
const AON_TABLE: [u32; 15] = [
    u32::from_be_bytes(*b"g0TR"),
    0x00c2_0001,
    0x0000_1401,
    u32::from_be_bytes(*b"g1TR"),
    0x0097_0001,
    0x0000_1401,
    u32::from_be_bytes(*b"g2TR"),
    0x0092_0001,
    0x0000_1401,
    u32::from_be_bytes(*b"g3TR"),
    0x00c7_0001,
    0x0000_1401,
    u32::from_be_bytes(*b"r0TR"),
    0x012c_0001,
    0x0000_1401,
];

const CHIP: v25g83::Chip = v25g83::Chip {
    max_power_mw: MAX_POWER_MW,
    // The per-cluster core, SRAM, CS and AFR leakage coefficients macOS 26.6.2 writes on the
    // J616C.
    leakage: {
        let mut words = [0; 56];
        words[0] = 0x4509_b800;
        words[1] = 0x4513_7800;
        words[2] = 0x4506_e800;
        words[3] = 0x4500_5800;
        words[32] = 0x43f3_0000;
        words[33] = 0x43f3_0000;
        words[34] = 0x43f3_0000;
        words[35] = 0x43f3_0000;
        words[52] = 0x440d_4000;
        words[54] = 0x43e6_4000;
        words
    },
    aon_table: &AON_TABLE,
    hw_data_words: HW_DATA_WORDS,
    power_config_words: POWER_CONFIG_WORDS,
    // macOS fills every voltage lane of table A and leaves the CS table empty, as on the J614C.
    perf_a_replicate_lanes: true,
    cs_perf_table: false,
};

/// Hardware data that differs from the M4 Pro's, as macOS 26.6.2 writes it on the J616C: the
/// J614C's words with six memory cache controllers (`+0xf8c`, the ADT's active-mcc-bit-vector)
/// and 40 at `+0xf88`/`+0xfc0` (macOS writes the die's core count there, 32 at `+0x2570`).
///
/// macOS also has 1 at `+0x2610`; this driver keeps 0 there, as with 1 the GPU takes work but
/// never runs it (see `t6041::CHIP_25G83`).
const HW_DATA_WORDS: &[(usize, u32, usize)] = &[
    (0xe0, 0x2008, 1),
    (0xe8, 0x20080000, 1),
    (0xf4, 0x2008, 1),
    (0xf8, 0x24cb, 1),
    (0xfc, 0xc98a2cfa, 1),
    (0x100, 0xf4f624cb, 1),
    (0x104, 0x1877e917, 1),
    (0x108, 0x38d924cb, 1),
    (0x10c, 0xbdab0000, 1),
    (0x2d8, 0xfff82000, 1),
    (0x2dc, 0xd32d2cdb, 1),
    (0x2e0, 0xf5002000, 1),
    (0x2e4, 0x21dae926, 1),
    (0x2e8, 0x38b62000, 1),
    (0x2ec, 0xc7420008, 1),
    (0x3e0, 0x7fe0, 1),
    (0x3e8, 0x7fe00000, 1),
    (0x3f4, 0x7fe0, 1),
    (0x3f8, 0x8000, 1),
    (0x400, 0x80000000, 1),
    (0x40c, 0x8000, 1),
    (0x5d8, 0x4b232645, 1),
    (0x5dc, 0xe98, 1),
    (0x5e0, 0xd5a2ea5f, 1),
    (0x5e4, 0x40003fff, 1),
    (0x5e8, 0xca5e4000, 1),
    (0x5ec, 0x4000f5a2, 1),
    (0xf6c, 0x1, 1),
    (0xf88, 0x28, 1),
    (0xf8c, 0x3f, 1),
    (0xfac, 0x1, 1),
    (0xfb8, 0x1d, 1),
    (0xfc0, 0x28, 1),
    (0x2594, 0x2e0000, 1),
    (0x3d98, 0xffff_ffff, 1),
    (0x3db0, 0xffff_ffff, 1),
];

/// `+0x2a04` is left to the driver (the power ceiling): macOS had 44.3 (0x42314aff) there at the
/// time of the capture, a runtime budget; with it the J616C's ALU test lost a clock state.
///
/// The power configuration macOS 26.6.2 writes on the J616C (captured 16 Nov 2026 under a
/// hypervisor), where it differs from the M4 Pro base, besides the power limit, the leakage and
/// the addresses and entropy the driver fills in. Like the J614C's words, it includes what
/// macOS had written by then (per-sensor blocks at `+0x3140`, timestamps).
const POWER_CONFIG_WORDS: &[(usize, u32, usize)] = &[
    // +0x0 stays 0: with macOS's 1 the J616C lost half its geometry throughput.
    (0x44, 0xc, 1),
    (0x9c, 0x43a90000, 1),
    (0x200, 0x1, 1),
    (0x208, 0x76, 1),
    (0x210, 0xb23a11d4, 1),
    (0x218, 0xaa7a, 1),
    (0x698, 0xb23a2338, 1),
    (0x6a8, 0xae38, 1),
    (0x728, 0x2b928, 1),
    (0x7a8, 0x2b928, 1),
    (0x920, 0x3dd00000, 1),
    (0x9ec, 0xbc10c64e, 1),
    (0x9f0, 0x28, 1),
    (0xa70, 0x4, 1),
    (0xa7c, 0x0, 1),
    (0xa84, 0x3f800000, 1),
    (0xa8c, 0x3e0b4396, 1),
    (0xa9c, 0x3e99999a, 1),
    (0xaa8, 0x28, 1),
    (0xab8, 0x42ec0000, 1),
    (0xac0, 0x76, 1),
    (0xad0, 0x0, 1),
    (0xae0, 0xc7dccd80, 1),
    (0xaf0, 0x3e8, 1),
    (0xafc, 0x3f800000, 1),
    (0xb00, 0x3e0b4396, 1),
    (0xb04, 0x3e99999a, 1),
    (0xb08, 0x76, 1),
    (0xb2c, 0x2, 1),
    (0xb88, 0x42a00000, 1),
    (0xb90, 0x1, 1),
    (0xbc8, 0x3f7eb852, 1),
    (0xbcc, 0x3ba3d70a, 1),
    (0xbd0, 0x3f64c986, 1),
    (0xbd4, 0x403ea7f0, 1),
    (0x21f4, 0x42200000, 1),
    (0x2210, 0x28, 1),
    (0x2264, 0x15a3, 1),
    (0x226c, 0x2a222aa2, 1),
    (0x2294, 0x40baaaaa, 1),
    (0x22a4, 0x42080000, 1),
    (0x22b0, 0x28, 1),
    (0x22c8, 0x15a3, 1),
    (0x2868, 0x2b7e4, 1),
    (0x2874, 0x154, 1),
    (0x28d8, 0x47dd0880, 1),
    (0x29bc, 0x74, 1),
    (0x29c8, 0x3f772c23, 1),
    (0x29d0, 0x3d0d3dcb, 1),
    (0x29d8, 0x3cf5c290, 1),
    (0x29e8, 0x3f19999a, 1),
    (0x2a0c, 0x76, 1),
    (0x2a2c, 0xc7dcf257, 1),
    (0x2a44, 0x3f772c23, 1),
    (0x2a48, 0x3d0d3dcb, 1),
    (0x2a4c, 0x3cf5c290, 1),
    (0x2a50, 0x3f19999a, 1),
    (0x2a54, 0x76, 1),
    (0x2b18, 0x2d688, 2),
    (0x2b20, 0x1ff1, 1),
    (0x2b24, 0x1ff5, 1),
    (0x2b28, 0x29b, 1),
    (0x2b40, 0x2, 1),
    (0x2b94, 0x46023400, 1),
    (0x2b9c, 0x1ff6, 1),
    (0x2bc4, 0x47800000, 1),
    (0x2bcc, 0x3e8, 1),
    (0x2be8, 0x208d, 1),
    (0x2c40, 0x28, 1),
    (0x2c88, 0x3e8, 1),
    (0x2cec, 0x1, 1),
    (0x2cf0, 0xf, 1),
    (0x2cf4, 0x3, 1),
    (0x2cfc, 0x6, 1),
    (0x2f0c, 0x2a222aa2, 1),
    (0x3140, 0x36, 1),
    (0x3144, 0x193e, 1),
    (0x3148, 0x2, 1),
    (0x314c, 0x2e, 1),
    (0x3150, 0x36, 1),
    (0x3154, 0x235c0dd, 1),
    (0x315c, 0x2a9c, 1),
    (0x31e0, 0x37, 1),
    (0x31e4, 0x1944, 1),
    (0x31e8, 0x2, 1),
    (0x31ec, 0x2e, 1),
    (0x31f0, 0x37, 1),
    (0x31f4, 0x23735cf, 1),
    (0x31fc, 0x2aaf, 1),
    (0x3230, 0x37, 1),
    (0x3234, 0x193a, 1),
    (0x3238, 0x2, 1),
    (0x323c, 0x2e, 1),
    (0x3240, 0x37, 1),
    (0x3244, 0x236d2d2, 1),
    (0x324c, 0x2ab8, 1),
    (0x3280, 0x36, 1),
    (0x3284, 0x18f3, 1),
    (0x3288, 0x2, 1),
    (0x328c, 0x2d, 1),
    (0x3290, 0x36, 1),
    (0x3294, 0x2305ffe, 1),
    (0x329c, 0x2ac3, 1),
    (0x32d0, 0x37, 1),
    (0x32d4, 0x1946, 1),
    (0x32d8, 0x2, 1),
    (0x32dc, 0x2e, 1),
    (0x32e0, 0x37, 1),
    (0x32e4, 0x2379314, 1),
    (0x32ec, 0x2acd, 1),
    (0x3320, 0x36, 1),
    (0x3324, 0x194b, 1),
    (0x3328, 0x2, 1),
    (0x332c, 0x2e, 1),
    (0x3330, 0x37, 1),
    (0x3334, 0x23aef65, 1),
    (0x333c, 0x2adc, 1),
    (0x33c0, 0x36, 1),
    (0x33c4, 0x193e, 1),
    (0x33c8, 0x2, 1),
    (0x33cc, 0x2e, 1),
    (0x33d0, 0x36, 1),
    (0x33d4, 0x23a5542, 1),
    (0x33dc, 0x2b14, 1),
    (0x3460, 0x37, 1),
    (0x3464, 0x195a, 1),
    (0x3468, 0x2, 1),
    (0x346c, 0x2e, 1),
    (0x3470, 0x37, 1),
    (0x3474, 0x23ca7f2, 1),
    (0x347c, 0x2b23, 1),
    (0x3500, 0x37, 1),
    (0x3504, 0x197a, 1),
    (0x3508, 0x2, 1),
    (0x350c, 0x2e, 1),
    (0x3510, 0x37, 1),
    (0x3514, 0x23f8ec7, 1),
    (0x351c, 0x2b2d, 1),
    (0x3550, 0x37, 1),
    (0x3554, 0x1948, 1),
    (0x3558, 0x2, 1),
    (0x355c, 0x2e, 1),
    (0x3560, 0x37, 1),
    (0x3564, 0x23d592f, 1),
    (0x356c, 0x2b33, 1),
    (0x35a0, 0x37, 1),
    (0x35a4, 0x1963, 1),
    (0x35a8, 0x2, 1),
    (0x35ac, 0x2e, 1),
    (0x35b0, 0x37, 1),
    (0x35b4, 0x23e7e07, 1),
    (0x35bc, 0x2b40, 1),
    (0x3640, 0x37, 1),
    (0x3644, 0x1954, 1),
    (0x3648, 0x2, 1),
    (0x364c, 0x2e, 1),
    (0x3650, 0x37, 1),
    (0x3654, 0x23f8703, 1),
    (0x365c, 0x2b42, 1),
    (0x36e0, 0x37, 1),
    (0x36e4, 0x1931, 1),
    (0x36e8, 0x2, 1),
    (0x36ec, 0x2e, 1),
    (0x36f0, 0x37, 1),
    (0x36f4, 0x23f0d34, 1),
    (0x36fc, 0x2b51, 1),
    (0x3730, 0x36, 1),
    (0x3734, 0x18f8, 1),
    (0x3738, 0x2, 1),
    (0x373c, 0x2d, 1),
    (0x3740, 0x36, 1),
    (0x3744, 0x23c8abc, 1),
    (0x374c, 0x2b5a, 1),
    (0x3780, 0x36, 1),
    (0x3784, 0x193c, 1),
    (0x3788, 0x2, 1),
    (0x378c, 0x2e, 1),
    (0x3790, 0x37, 1),
    (0x3794, 0x23f7532, 1),
    (0x379c, 0x2b65, 1),
    (0x37d0, 0x36, 1),
    (0x37d4, 0x1930, 1),
    (0x37d8, 0x2, 1),
    (0x37dc, 0x2e, 1),
    (0x37e0, 0x36, 1),
    (0x37e4, 0x23fdb06, 1),
    (0x37ec, 0x2b71, 1),
    (0x3820, 0x37, 1),
    (0x3824, 0x193d, 1),
    (0x3828, 0x2, 1),
    (0x382c, 0x2e, 1),
    (0x3830, 0x37, 1),
    (0x3834, 0x2406a28, 1),
    (0x383c, 0x2b79, 1),
    (0x38c0, 0x36, 1),
    (0x38c4, 0x18ed, 1),
    (0x38c8, 0x2, 1),
    (0x38cc, 0x2d, 1),
    (0x38d0, 0x36, 1),
    (0x38d4, 0x23e456c, 1),
    (0x38dc, 0x2ba4, 1),
    (0x3960, 0x36, 1),
    (0x3964, 0x1924, 1),
    (0x3968, 0x2, 1),
    (0x396c, 0x2e, 1),
    (0x3970, 0x36, 1),
    (0x3974, 0x243e621, 1),
    (0x397c, 0x2baa, 1),
    (0x3a00, 0x37, 1),
    (0x3a04, 0x1972, 1),
    (0x3a08, 0x2, 1),
    (0x3a0c, 0x2e, 1),
    (0x3a10, 0x37, 1),
    (0x3a14, 0x247c9f9, 1),
    (0x3a1c, 0x2bb3, 1),
    (0x3a50, 0x36, 1),
    (0x3a54, 0x1912, 1),
    (0x3a58, 0x2, 1),
    (0x3a5c, 0x2e, 1),
    (0x3a60, 0x36, 1),
    (0x3a64, 0x23fabcd, 1),
    (0x3a6c, 0x2bb7, 1),
    (0x3aa0, 0x36, 1),
    (0x3aa4, 0x1943, 1),
    (0x3aa8, 0x2, 1),
    (0x3aac, 0x2e, 1),
    (0x3ab0, 0x37, 1),
    (0x3ab4, 0x24446ea, 1),
    (0x3abc, 0x2bc4, 1),
    (0x3b1c, 0x197a, 1),
    (0x3b20, 0x2, 1),
    (0x3b24, 0x2d, 1),
    (0x3b28, 0x37, 1),
    (0x3b2c, 0x244fe8f, 1),
    (0x3b34, 0x2b5e, 1),
    (0x3b40, 0x34, 1),
    (0x3b44, 0x3f, 1),
    // The second temperature-sensor mask word.
    (0x3b5c, 0x2a22_2aa2, 1),
    (0x62c4, 0x1000000, 1),
    (0x62cc, 0x1010202, 1),
    (0x62d0, 0x1010a28, 1),
    (0x62d4, 0xa280201, 1),
    (0x62d8, 0x2010101, 1),
    (0x62dc, 0x1010a28, 1),
    (0x62e0, 0xa280101, 1),
    (0x62e4, 0x4b000101, 1),
    (0x62f4, 0x4a000000, 1),
    (0x62f8, 0x100, 1),
    (0x6304, 0xff000000, 1),
    (0x6308, 0x7, 1),
    // The temperature sources of GPU clusters 2 and 3.
    (0x6328, u32::from_be_bytes(*b"g2TR"), 1),
    (0x632c, u32::from_be_bytes(*b"g3TR"), 1),
    (0x6360, 0xf, 1),
    (0x63c8, 0x1, 1),
    (0x63cc, 0xb23620ed, 1),
    (0x63dc, 0xb23b9988, 1),
    (0x63e4, 0xb0ec8907, 1),
    (0x63ec, 0xb0ed0d1a, 1),
    (0x63f4, 0xb23965fd, 1),
    (0x63fc, 0xb23a11d4, 1),
    (0x6404, 0xb23b9988, 1),
    (0x6414, 0x1d32e3, 1),
    (0x6418, 0x1, 1),
    (0x641c, 0x30d40, 1),
    (0x6428, 0x1, 1),
    (0x650c, 0x16c648b6, 1),
];
