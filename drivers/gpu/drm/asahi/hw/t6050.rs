// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware configuration for t6050 platforms: J775D (Mac Studio, M5 Ultra, two dies), on the
//! macOS 27.0 (26A428) GPU firmware, native: no SPTM, so the firmware publishes its kernel roots
//! (`UatFirmware::PublishedRoots`) as the A18 Pro's does, and the InitData graph is the M5 one
//! ([`crate::g17::fw::m5`]).
//!
//! Values: the J775D ADT and live measurements. The performance tables are this ADT's; probe
//! checks them against the device tree's operating points, which the boot loader fills from the
//! same ADT.

use crate::g17::fw::m5::initdata::{relative_frequency, J714RegisterWindowSpec, M5Platform};
use crate::g17::{Config, CoreLayout, CrashBuffers, FixedBuffer, Identity};
use crate::hw::{GpuGen, GpuVariant};
use crate::mmu::UatQuirks;

/// Static configuration of the T6050 (J775D) GPU.
pub(crate) const CONFIG: Config = Config {
    chip_id: CHIP_ID,
    identity: Identity {
        // ID register 0x0b06_1100: family 0xb (G17), variant 6 (G17D), revision 0x11.
        family: 0xb,
        variant: 6,
        num_dies: 2,
        gpu_variant: GpuVariant::D,
        usc_generation: 3,
        hal_generation: 300,
    },
    uat_ias: 42,
    uat_oas: 42,
    // GFX1 publishes its root 0x40000 into `pagetables`, as the A18 Pro's secondary does.
    secondary_root_offset: Some(0x40000),
    // Each role asks for its crash log at the PA of its __DATA_SHARED_RO, inside `fw-data`.
    crash_buffers: CrashBuffers::FirmwareData,
    // The M5 graph allocates its metrics itself; this is only the lower-root page the shared
    // boot path maps before the coprocessors start (the A18 Pro's address).
    pm_metrics: FixedBuffer {
        va: 0x10_0008_0000,
        size: 0x8000,
    },
    // The M5 graph takes its windows from M5_PLATFORM.
    windows: &[],
    reserved_slots: &[],
    perf: &crate::hw::t8140::PERF,
    opps: OPPS,
    // The tables are in no-map reservations outside /memory, as on the M4 Pro; the rest is the
    // conservative policy until a test shows otherwise.
    uat_quirks: UatQuirks {
        reserved_tables: true,
        clean_tables: true,
        page_tlbi: true,
        handoff_lock: false,
        // 26A428's values on T6050.
        handoff_slot: 0xffff,
        handoff_unk2: 1,
        native_gpu_permissions: false,
    },
    region_c: &crate::g17::fw::initdata::REGION_C_26A428,
    adopt_root_entries: true,
    // The A18 Pro's fabric bridge is not this SoC's: the M5 preparation (G+0xe90000) replaces it.
    fabric_bridge: false,
    recovery_slot_keys: None,
    gpu_generation: GpuGen::G17,
    firmware: crate::g17::fw::variant::Firmware::Fixed(&crate::g17::fw::variant::V26A428_M5),
    // Topology only; no M5 command-buffer layout is selected until submission is.
    core_layout: CoreLayout {
        cores: Some((8, 64)),
        operand_state: crate::g17::fw::compute::OperandState::ONE_BY_SIX,
        deflake: crate::g17::fw::render::Deflake::ONE_CLUSTER,
    },
    m5: Some(&M5_PLATFORM),
};

/// Chip identifier written to the hardware data.
pub(crate) const CHIP_ID: u32 = 0x6050;

/// Operating points the device tree lists (P0 included).
pub(crate) const OPPS: usize = 14;

const G: u64 = 0x23_0000_0000;
const D: u64 = 0x40_0000_0000;

const fn w(
    slot: usize,
    instances: &'static [u64],
    size: u32,
    unk_18: u64,
    flag: u32,
) -> J714RegisterWindowSpec {
    J714RegisterWindowSpec {
        slot,
        instances,
        size,
        unk_18,
        flag,
    }
}

/// Register windows of both dies.
const WINDOWS: [J714RegisterWindowSpec; 32] = [
    w(0, &[0x2_8041_4000], 0x4000, 0, 2),
    // One per active MCC (ADT active-mcc-bit-vector 0xff), base 0x2220000000.
    w(
        3,
        &[
            0x2220104000,
            0x2222104000,
            0x2224104000,
            0x2226104000,
            0x2228104000,
            0x222a104000,
            0x222c104000,
            0x222e104000,
        ],
        0x18000,
        0,
        2,
    ),
    w(4, &[0x2_8046_8000], 0x4000, 0, 0),
    w(9, &[0x2_8425_0000], 0x4000, 0, 2),
    w(10, &[0x2_8424_0000], 0x4000, 0, 0),
    w(11, &[0x2_8425_8000], 0x4000, 0, 2),
    w(12, &[0x5_0965_c000], 0x4000, 0, 2),
    w(13, &[0x45_0965_c000], 0x4000, 0, 2),
    w(14, &[0x23_8030_0000], 0x8000, 0, 0),
    w(15, &[0x2_8cdc_0000], 0x24000, 0, 2),
    w(17, &[G], 0x21500, 0, 2),
    w(18, &[G + 0xeb_4000, G + 0xeb_4000 + D], 0x4000, 0, 2),
    w(
        20,
        &[
            G + 0x3000000,
            G + 0x3200000,
            G + 0x3400000,
            G + 0x3600000,
            G + 0x3800000,
            G + 0x3a00000,
            G + 0x3c00000,
            G + 0x3e00000,
            0x23_035a_8000,
            0x23_037a_8000,
            0x23_03da_8000,
            0x23_03fa_8000,
        ],
        0x40000,
        0,
        2,
    ),
    w(22, &[G + 0x100_0000], 0x8000, 0, 2),
    w(23, &[G + 0xf0_0000, G + 0xf0_0000 + D], 0x10000, 0, 2),
    w(
        24,
        &[
            G + 0xec_0000,
            G + 0xed_0000,
            G + 0xee_0000,
            G + 0xef_0000,
            G + 0xec_0000 + D,
            G + 0xed_0000 + D,
            G + 0xee_0000 + D,
            G + 0xef_0000 + D,
        ],
        0x10000,
        0,
        2,
    ),
    w(31, &[G + 0xd0_4000], 0x8000, 0xd0_4000, 2),
    w(33, &[G + 0xd5_0000], 0x10000, 0xd5_0000, 2),
    w(34, &[G + 0xd1_0000], 0x4000, 0xd1_0000, 2),
    w(36, &[G + 0xd4_0000], 0x4000, 0xd4_0000, 2),
    w(37, &[G + 0xd6_0000], 0x20000, 0xd6_0000, 2),
    w(38, &[G + 0xd4_4000], 0x4000, 0xd4_4000, 2),
    w(39, &[G + 0xd4_c000], 0x200, 0xd4_c000, 2),
    w(40, &[G + 0xe0_0000], 0x4000, 0xe0_0000, 2),
    w(42, &[G + 0xe4_0000], 0x4000, 0xe4_0000, 2),
    w(44, &[G + 0xe0_8000, G + 0xe1_0000], 0x8000, 0, 2),
    w(46, &[G + 0xe3_3800], 0x4000, 0, 2),
    w(48, &[G + 0xe6_0000], 0x58, 0xe6_0000, 2),
    w(52, &[G + 0xe4_8000], 0x8000, 0, 2),
    w(53, &[G + 0x2_3d00], 0x200, 0x2_3d00, 2),
    w(58, &[G + 0xe7_3800], 0x4000, 0, 2),
    w(
        59,
        &[
            0x23_0358_0000,
            0x23_0378_0000,
            0x23_03d8_0000,
            0x23_03f8_0000,
        ],
        0x60000,
        0,
        2,
    ),
];

const MHZ: [u32; 14] = [
    0, 338, 486, 636, 796, 888, 988, 1084, 1182, 1278, 1374, 1470, 1578, 1620,
];

/// The J775D platform of the M5 hardware data.
pub(crate) const M5_PLATFORM: M5Platform = M5Platform {
    // Core type 0x1a (variant 6) and firmware revision 2 for raw revision 0x11.
    core_revision: (0x1a, 2),
    // 2 dies x 4 clusters x 10 slots; 64 enabled, counted from the core masks at probe.
    nominal_cores: 80,
    perf_states: 13,
    // ADT sgx perf-states (one table per cluster) and perf-states-sram, mV.
    perf: [
        (
            0,
            [125, 125, 125, 125, 125, 125, 125, 125],
            [775, 775, 775, 775, 775, 775, 775, 775],
        ),
        (
            338,
            [615, 615, 615, 615, 615, 610, 610, 615],
            [775, 775, 775, 775, 775, 775, 775, 775],
        ),
        (
            486,
            [670, 655, 655, 670, 650, 665, 665, 650],
            [775, 775, 775, 775, 775, 775, 775, 775],
        ),
        (
            636,
            [695, 690, 690, 695, 690, 690, 690, 690],
            [775, 775, 775, 775, 775, 775, 775, 775],
        ),
        (
            796,
            [760, 745, 745, 760, 745, 740, 740, 745],
            [775, 775, 775, 775, 775, 775, 775, 775],
        ),
        (
            888,
            [770, 755, 755, 770, 750, 750, 750, 750],
            [775, 775, 775, 775, 775, 775, 775, 775],
        ),
        (
            988,
            [795, 780, 780, 795, 775, 775, 775, 775],
            [795, 780, 780, 795, 775, 775, 775, 775],
        ),
        (
            1084,
            [805, 805, 805, 805, 815, 800, 800, 815],
            [805, 805, 805, 805, 815, 800, 800, 815],
        ),
        (
            1182,
            [815, 810, 810, 815, 825, 820, 820, 825],
            [815, 810, 810, 815, 825, 820, 820, 825],
        ),
        (
            1278,
            [860, 845, 845, 860, 870, 855, 855, 870],
            [860, 845, 845, 860, 870, 855, 855, 870],
        ),
        (
            1374,
            [885, 885, 885, 885, 895, 895, 895, 895],
            [885, 885, 885, 885, 895, 895, 895, 895],
        ),
        (
            1470,
            [930, 930, 930, 930, 940, 935, 935, 940],
            [930, 930, 930, 930, 940, 935, 935, 940],
        ),
        (
            1578,
            [1020, 1005, 1005, 1020, 1030, 1015, 1015, 1030],
            [1020, 1005, 1005, 1020, 1030, 1015, 1015, 1030],
        ),
        (
            1620,
            [1045, 1040, 1040, 1045, 1065, 1050, 1050, 1065],
            [1045, 1040, 1040, 1045, 1065, 1050, 1050, 1065],
        ),
    ],
    lanes: 8,
    relative: relative_frequency(&MHZ),
    // ADT afr-perf-states / cs-perf-states (one lane per die), mV; SRAM floor 775 mV.
    aux: [
        (24, [125, 125], [775, 775]),
        (338, [600, 600], [775, 775]),
        (486, [660, 655], [775, 775]),
        (636, [670, 665], [775, 775]),
        (796, [710, 705], [775, 775]),
        (888, [720, 720], [775, 775]),
        (988, [755, 765], [775, 775]),
        (1084, [800, 795], [800, 795]),
        (1182, [810, 820], [810, 820]),
        (1278, [850, 860], [850, 860]),
        (1374, [905, 915], [905, 915]),
        (1470, [945, 955], [945, 955]),
        (1578, [1030, 1040], [1030, 1040]),
        (1620, [1075, 1085], [1075, 1085]),
    ],
    // The Ultra's ADT AON power-telemetry records.
    aon: &[
        [0x6730_5452, 0x019f_0001, 0x0000_1401], // g0TR
        [0x6731_5452, 0x01bb_0001, 0x0000_1401], // g1TR
        [0x6732_5452, 0x00a7_0001, 0x0000_1401], // g2TR
        [0x6733_5452, 0x00c3_0001, 0x0000_1401], // g3TR
        [0x7230_5452, 0x0113_0001, 0x0000_1401], // r0TR
        [0x7231_5452, 0x001b_0001, 0x0000_1401], // r1TR
    ],
    windows: &WINDOWS,
};
