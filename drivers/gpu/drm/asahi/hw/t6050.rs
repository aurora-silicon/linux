// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware configuration for t6050 platforms: J775D (Mac Studio, M5 Ultra, two dies), on the
//! macOS 27.0 (26A428) GPU firmware, identified by its loaded image, native: no SPTM, so the
//! firmware publishes its kernel roots (`UatFirmware::PublishedRoots`) as the A18 Pro's does, and
//! the InitData graph is the M5 one ([`crate::g17::fw::m5`]).
//!
//! Values: the J775D ADT and live measurements. The performance tables are this ADT's; probe
//! checks them against the device tree's operating points, which the boot loader fills from the
//! same ADT.

use crate::g17::fw::m5::initdata::{J714RegisterWindowSpec, M5Board};
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
        va: crate::g17::m5_vm::fixed::BOOT_METRICS,
        size: crate::g17::m5_vm::fixed::BOOT_METRICS_SIZE,
    },
    // The M5 graph takes its windows from M5_BOARD.
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
    // The boot loader loads both roles' 26A428 TEXT back to back into `fw-text`: GFX's 0x50000,
    // then GFX1's 0x58000, each with its own GKTS/ECAP entropy records. Any other loaded image,
    // such as a 27.2 stub's, is refused before anything is written to the GPU.
    firmware: crate::g17::fw::variant::Firmware::Identified(&[crate::g17::fw::variant::Build {
        os_version: [27, 0, 0],
        name: "macOS 27.0 (26A428)",
        images: &[crate::fwid::FirmwareBuild {
            // The firmware's TEXT with the boot-filled ranges zeroed: GFX is the third image of
            // armfw_g17x, GFX1 the third of armfw1_g17x; a loaded copy normalizes to the same
            // digest.
            name: "Apple GFX 26A428 for T6050, GFX then GFX1 (normalized image)",
            text_size: 0xa8000,
            data_size: 0x184000,
            entropy: &[[0x49e60, 0x49f43], [0xa1918, 0xa19fb]],
            // Per role: the header's two load fields (+0x224, and the DATA address at +0x23c),
            // then `_rtk_tunables` and `_rtk_patchbay` (the boot arguments, entropy included).
            boot_filled: &[
                (0x224, 4),
                (0x23c, 8),
                (0x49720, 0x740),
                (0x49e60, 0x231),
                (0x50224, 4),
                (0x5023c, 8),
                (0xa11d8, 0x740),
                (0xa1918, 0x231),
            ],
            text_sha256: [
                0xc3, 0x53, 0x54, 0x40, 0x76, 0x02, 0x48, 0x44, 0x04, 0xaa, 0x6f, 0xc9, 0x46, 0xcd,
                0xd2, 0x4c, 0xb8, 0x2c, 0x64, 0xa8, 0xbb, 0xd1, 0x07, 0xa4, 0x94, 0x79, 0xa0, 0x8c,
                0xe9, 0xcc, 0x82, 0x51,
            ],
        }],
        variant: &crate::g17::fw::variant::V26A428_M5,
    }]),
    // Topology only; no M5 command-buffer layout is selected until submission is.
    core_layout: CoreLayout {
        cores: Some((8, 64)),
        operand_state: crate::g17::fw::compute::OperandState::ONE_BY_SIX,
        deflake: crate::g17::fw::render::Deflake::ONE_CLUSTER,
    },
    m5: Some(&M5_BOARD),
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

/// 2 dies x 4 clusters, ten core slots each (`ID_COUNTS_1`); 64 enabled, counted from the core
/// masks at probe.
const CLUSTERS: u32 = 8;
const CORE_SLOTS: u32 = 10;

/// The J775D board's fixed part of the M5 hardware data. Its performance, auxiliary and AON
/// tables come from the device tree, which the boot loader fills from the ADT.
pub(crate) const M5_BOARD: M5Board = M5Board {
    // Core type 0x1a (variant 6) and firmware revision 2 for raw revision 0x11.
    core_revision: (0x1a, 2),
    nominal_cores: CLUSTERS * CORE_SLOTS,
    // One voltage lane per cluster.
    lanes: 8,
    windows: &WINDOWS,
    // The operand layout and dispatch control follow the topology; the rest is the J714 tuple the arithmetic
    // qualification passes with on this GPU (see `Profile`).
    compute: crate::g17::fw::m5::compute::Profile {
        preempt_state_offset: 0x1480,
        operand: crate::g17::fw::m5::compute::OperandLayout::of(CLUSTERS, CORE_SLOTS),
        dispatch_control: crate::g17::fw::m5::compute::dispatch_control(0x10c0_8860, CLUSTERS),
        auxiliary_control: 0,
        no_storage: crate::g17::fw::m5::compute::Resources {
            control: 0x54b,
            policy: 0xf5_0040_0020,
            pool_blocks: 8,
        },
    },
};
