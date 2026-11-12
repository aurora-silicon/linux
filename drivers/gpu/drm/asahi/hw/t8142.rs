// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5 (T8142, MacBook Air J813) on the macOS 27.0 (26A428) and 27.2 (26B123) GPU firmware.
//!
//! Bring-up only: the M5 graph ([`crate::g17::fw::m5::initdata`]) has no J813 board yet, so
//! [`PROBE`] stops at the identity check, after `get_g17_id` has logged the ID registers and
//! core masks, before the UAT or the firmware are touched.
//!
//! J813 (2026-11-07): ID registers 0xb022000 0xb0800 0x11010a 0x40404 0x1320311 0x10030302,
//! family 0xb, variant 2, one die, one cluster of ten core slots; core mask 0x3bd (eight
//! enabled, slots 1 and 6 fused off).
use crate::g17::{Config, CoreLayout, CrashBuffers, FirmwareProfile, Identity};
use crate::hw::{GpuGen, GpuVariant};


/// Refuses the GPU after logging its identity: no variant byte is 0xff.
pub(crate) const PROBE: Config = Config {
    chip_id: 0x8142,
    identity: Identity {
        family: 0xb,
        variant: 0xff,
        num_dies: 1,
        gpu_variant: GpuVariant::G,
        usc_generation: 3,
        hal_generation: 300,
    },
    uat_ias: 42,
    uat_oas: 42,
    // As on 25G76: each role publishes its own kernel root, the secondary 0x40000 into
    // `pagetables`.
    secondary_root_offset: Some(0x40000),
    // Each role asks for its crash log at the physical address of its __DATA_SHARED_RO
    // (0x100_0250_4000, 0x100_025c_0000 under 27.0), inside the `fw-data` region.
    crash_buffers: CrashBuffers::FirmwareData,
    profile: FirmwareProfile::M5PublishedRoots,
    pm_metrics: None,
    windows: &[],
    reserved_slots: &[],
    perf: None,
    opps: 0,
    uat_quirks: crate::mmu::UatQuirks::DEFAULT,
    region_c: None,
    chip: None,
    // Never reached: the identity check refuses first.
    m5: Some(&crate::g17::fw::m5::initdata::J714),
    adopt_root_entries: true,
    fabric_bridge: false,
    sleep_restart: false,
    recovery_slot_keys: None,
    gpu_generation: GpuGen::G17,
    firmware: crate::g17::fw::variant::Firmware::Fixed(&crate::g17::fw::variant::V26A428_M5),
    core_layout: CoreLayout {
        cores: None,
        alternate_core_masks: None,
        operand_state: crate::g17::fw::compute::OperandState::TWO_BY_TEN,
        deflake: crate::g17::fw::render::Deflake::TWO_CLUSTERS,
        compute_blocks: crate::g17::fw::freelist::COMPUTE_BLOCKS,
    },
};

/// The J813 on the M5 graph: [`PROBE`] with the GPU's identity and the J813 board. It runs
/// either firmware build of [`BUILDS`]; 26B123 runs [`CONFIG_26B123`].
pub(crate) const CONFIG: Config = Config {
    firmware: crate::g17::fw::variant::Firmware::Identified(&BUILDS),
    ..BASE
};

/// The J813 board and identity, before a firmware build is chosen.
const BASE: Config = Config {
    identity: Identity {
        family: 0xb,
        variant: 2,
        num_dies: 1,
        gpu_variant: GpuVariant::G,
        usc_generation: 3,
        hal_generation: 300,
    },
    m5: Some(&crate::g17::fw::m5::initdata::J813),
    // One cluster of ten core slots; the buffers are sized for all ten.
    core_layout: CoreLayout {
        cores: None,
        alternate_core_masks: None,
        operand_state: crate::g17::fw::compute::OperandState::ONE_BY_TEN,
        deflake: crate::g17::fw::render::Deflake::ONE_CLUSTER,
        compute_blocks: crate::g17::fw::freelist::COMPUTE_BLOCKS,
    },
    ..PROBE
};

/// The J813 on the 27.2 firmware: the board on the common 26B123 graph.
pub(crate) const CONFIG_26B123: Config = Config {
    m5: Some(&crate::g17::fw::m5::initdata::J813_26B123),
    firmware: crate::g17::fw::variant::Firmware::Identified(&[BUILD_26B123]),
    ..BASE
};

/// The firmware in `fw-text` must be one of these builds. iBoot loads the G17G c000 image of the
/// primary (TEXT 0x50000) and the c010 image of the secondary (0x4c000) back to back. The digest
/// is that of both TEXTs with the entropy records and the ranges iBoot fills zeroed (the image
/// header's load fields at +0x224 and +0x23c, the 0x740 bytes of boot arguments before each
/// role's GKTS record and the 0x231 bytes of records from it); it is that of the stock images
/// in the IPSW with the same ranges zeroed, and was measured on two 27.0 boots and one 27.2 boot.
const BUILDS: [crate::g17::fw::variant::Build; 2] = [
    BUILD_26A428,
    crate::g17::fw::variant::Build {
        config: Some(&CONFIG_26B123),
        ..BUILD_26B123
    },
];

const BUILD_26A428: crate::g17::fw::variant::Build = crate::g17::fw::variant::Build {
    os_version: [27, 0, 0],
    name: "27.0 (26A428), G17G",
    images: &[crate::fwid::FirmwareBuild {
        name: "26A428 for T8142 G17G, primary then secondary",
        text_size: 0x9c000,
        data_size: 0x184000,
        entropy: &[[0x49d10, 0x49df3], [0x997d0, 0x998b3]],
        boot_filled: &[
            (0x224, 4), (0x23c, 8), (0x495d0, 0x740), (0x49d10, 0x231),
            (0x50224, 4), (0x5023c, 8), (0x99090, 0x740), (0x997d0, 0x231),
        ],
        text_sha256: [
            0xec, 0xff, 0x40, 0xe3, 0xad, 0xb8, 0x9c, 0x90, 0x62, 0xe9, 0x1d, 0xb6, 0x3f,
            0x31, 0xba, 0xaf, 0x75, 0xeb, 0x5d, 0x3f, 0x97, 0x22, 0xe3, 0x06, 0x95, 0x45,
            0x3f, 0xad, 0x02, 0xfc, 0xf5, 0x0c,
        ],
    }],
    variant: &crate::g17::fw::variant::V26A428_M5,
    config: None,
};

/// RTKit 3514.41.1: the 26A428 protocol on the 26B123 InitData graph.
const BUILD_26B123: crate::g17::fw::variant::Build = crate::g17::fw::variant::Build {
    os_version: [27, 2, 0],
    name: "27.2 (26B123), G17G",
    images: &[crate::fwid::FirmwareBuild {
        name: "26B123 for T8142 G17G, primary then secondary",
        text_size: 0x9c000,
        data_size: 0x184000,
        entropy: &[[0x49d54, 0x49e37], [0x997e4, 0x998c7]],
        boot_filled: &[
            (0x224, 4), (0x23c, 8), (0x49614, 0x740), (0x49d54, 0x231),
            (0x50224, 4), (0x5023c, 8), (0x990a4, 0x740), (0x997e4, 0x231),
        ],
        text_sha256: [
            0xe3, 0xab, 0x32, 0x7b, 0x73, 0x1b, 0xf2, 0xf1, 0x74, 0x9b, 0x04, 0x44, 0x1f,
            0x5c, 0xb5, 0xab, 0xfc, 0x23, 0xf3, 0x41, 0xc3, 0x99, 0x72, 0x8e, 0x2c, 0xb4,
            0x6f, 0x68, 0xb1, 0x7a, 0x39, 0xb9,
        ],
    }],
    variant: &crate::g17::fw::variant::V26A428_M5,
    config: None,
};

// Each compute operand bank holds 6 KiB per core slot of the board.
const _: () = assert!(
    CONFIG.core_layout.operand_state.bank_bytes()
        == crate::g17::fw::m5::initdata::J813.gpu_cores * 0x1800
);
