// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T6041 (M4 Max) on the 25G83 firmware build: the profile captured on the 32-core J616C.
//!
//! The J616C boots through a macOS 26.6.2 stub (iBoot mBoot-18000.161.10), which loads a T6041
//! image of the 25G83 build the M4 Pro runs ([`fw::variant::V25G83`]). [`super::t6041::CONFIG`]
//! selects this configuration when it identifies that image.
//!
//! The hardware data, power configuration, region C and AON table are the ones built on this
//! unit and captured from it ([`super::t6041_captured`]), with only the runtime words written
//! over them ([`Captured`]): the device tree the J616C boots with has no GPU operating-point,
//! CS or AFR tables and no calibration for the builders. The register windows are the captured
//! register map; the GPU is not started if the two differ.

use super::t604x::{instances, window};
use super::{t6041, t6041_captured as captured};
use crate::g17::fw::{
    self,
    initdata::{v25g83::Captured, PerfConfig, RegionCConfig, RegisterWindow, PERF_STATES},
};
use crate::g17::{Config, CoreLayout, CrashBuffers, FixedBuffer, Identity};
use crate::hw::{GpuGen, GpuVariant};
use crate::mmu::UatQuirks;

pub(super) const CONFIG: Config = Config {
    chip_id: t6041::CHIP_ID,
    identity: Identity {
        family: 0xa,
        // ID register 0x0a04_1100 (J616C): family 0xa, variant 4, revision 0x11 (B1).
        variant: 4,
        num_dies: 1,
        gpu_variant: GpuVariant::C,
        usc_generation: 3,
        hal_generation: 200,
    },
    uat_ias: 42,
    uat_oas: 42,
    secondary_root_offset: None,
    crash_buffers: CrashBuffers::FirmwareData,
    pm_metrics: FixedBuffer {
        va: 0x10_0008_0000,
        size: 0x8000,
    },
    windows: &REGISTER_WINDOWS,
    reserved_slots: &[],
    perf: &PERF,
    opps: OPPS,
    // The M4 Pro's policy, which the J616C has run with on this build.
    uat_quirks: UatQuirks {
        reserved_tables: true,
        clean_tables: true,
        page_tlbi: true,
        handoff_lock: false,
        handoff_slot: 0xffff_ffff,
        native_gpu_permissions: true,
    },
    region_c: &REGION_C,
    chip: None,
    captured: Some(&CAPTURED),
    adopt_root_entries: false,
    fabric_bridge: false,
    recovery_slot_keys: None,
    gpu_generation: GpuGen::G16,
    firmware: fw::variant::Firmware::Identified(&[BUILD]),
    // The cores the contents were captured with; other core counts are refused at probe.
    core_layout: CoreLayout {
        cores: Some((4, 32)),
        alternate_core_masks: None,
        // Banks for all forty core slots: the firmware indexes them by physical slot, and the
        // J616C's enabled slots run to 39 (core mask 0xf6_fdbf6fdb).
        operand_state: fw::compute::OperandState::FOUR_BY_TEN,
        deflake: fw::render::Deflake::FOUR_CLUSTERS,
        // The compute pool the J616C has run with since 23 Oct 2026; the M4 Pro's 21 blocks
        // hang this GPU's firmware on grids of more than ten workgroups.
        compute_blocks: 96,
    },
};

/// The 25G83 build as the J616C loads it.
pub(super) const BUILD: fw::variant::Build = fw::variant::Build {
    os_version: [26, 6, 2],
    name: "macOS 26.6.2 (25G83, iBoot mBoot-18000.161.10)",
    images: &[crate::fwid::FirmwareBuild {
        name: "Apple GFX 25G83 for T6041 (normalized loaded image)",
        text_size: 0x60000,
        data_size: 0x138000,
        entropy: [0x59839, 0x5991c],
        text_sha256: [
            0xaa, 0x11, 0x3e, 0xd6, 0x20, 0x79, 0xbc, 0xfd, 0xee, 0xf0, 0xf8, 0x6c, 0x2a, 0x8d,
            0x7e, 0xae, 0x9f, 0x90, 0x7b, 0x03, 0xbe, 0x28, 0xcc, 0xf2, 0x98, 0xc9, 0x43, 0x95,
            0x1d, 0x0e, 0xff, 0xc5,
        ],
    }],
    // Non-clustered tiling (TA mode 1), as for 25F84. Clustered tiling hangs the J616C unless
    // the cluster buffers are sized for four clusters, which this driver does not do yet.
    variant: &fw::variant::Variant {
        ta_mode_1: true,
        ..fw::variant::V25G83
    },
    config: None,
};

/// The captured register map: slot 3 holds the six memory controllers (ADT
/// `apple,sgx-active-mcc-bit-vector` 0x3f, 32 MiB apart), slot 20 the four AFR blocks. The
/// windows after slot 3 lie 0x30000 further than the M4 Pro's.
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

/// The ADT ladders of this unit (`apple,m4-index-a`, `apple,m4-index-b`, `apple,m4-relative-a`,
/// `apple,m4-relative-b`), which the captured performance tables hold. The builders, which
/// would use them, do not run for this configuration.
const PERF: PerfConfig = PerfConfig {
    states_a: [0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15],
    states_b: [0, 1, 2, 3, 4, 5, 7, 9, 11, 13, 15],
    relative_a: [0, 9, 16, 23, 29, 38, 49, 60, 69, 79, 100],
    relative_b: [0, 0, 22, 36, 47, 57, 68, 78, 84, 91, 100],
    sram_min_mv: 810,
};

const OPPS: usize = 16;

const _: () = {
    let mut index = 0;
    while index < PERF_STATES {
        assert!((PERF.states_a[index] as usize) < OPPS);
        assert!((PERF.states_b[index] as usize) < OPPS);
        index += 1;
    }
};

/// Region C as captured: the M4 Pro's with this unit's power limit (110586 mW,
/// `apple,m4-max-power-mw`). Only the builders would use it.
const REGION_C: RegionCConfig = RegionCConfig {
    unk_54: 0x0fa0,
    unk_78: [1, 0, 0],
    max_power_mw: 110_586,
    unk_9c: 0,
    unk_a0: 0,
    unk_7c4: 1500,
    unk_9e0: [
        1,
        0x0102_0200,
        0x010a_2801,
        0x2802_0101,
        0x0101_010a,
        0x010a_2802,
        0x2801_0101,
        0x0001_010a,
    ],
};

/// The contents captured on the J616C on 25 Oct 2026.
const CAPTURED: Captured = Captured {
    hw_data: captured::HW_DATA,
    power_config: captured::POWER_CONFIG,
    region_c: captured::REGION_C,
    aon_table: &captured::AON_TABLE,
    active_cores: 32,
    expected: captured::EXPECTED,
};
