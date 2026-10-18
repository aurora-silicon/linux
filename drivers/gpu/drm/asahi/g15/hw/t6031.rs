// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T6031 (M3 Max, G15C: J514C, J516C) configuration.
//!
//! One die, four clusters of ten core slots. The firmware is the same RTKit 2419 generation as
//! the T6030's and the initdata graph keeps the T6030 canonical VAs; what differs is the SoC
//! identity, the 40-bit enabled-core mask (two words), the source of the GPU regions (the
//! device tree node the boot loader fills, not an ADT export) and the GPU power (its power
//! domain; there is no PMP vote). The identity words and the firmware identity were read on a
//! J516C.

use super::super::Config;
use super::{FirmwareBuild, HwId};

pub(crate) const CONFIG: Config = Config {
    name: "T6031 G15C",
    chip_id: 0x6031,
    gpu_variant: crate::hw::GpuVariant::C,
    gpu_revision: 0x12,
    uat_ias: 42,
    uat_oas: 42,
    num_clusters: 4,
    num_cores_per_cluster: 10,
    hw_ids: &[HwId {
        version: 0x07041100,
        counts: 0x0021040a,
    }],
    // The stub the macOS 14.7 boot chain of this J516C leaves loaded (RTKit-2419.140.12). A
    // J516C started through a macOS 14.8.x chain carries another stub, not pinned yet
    // (`g15_accept_text_prefix` admits it for one boot and logs its digest).
    firmware: &[FirmwareBuild {
        name: "macOS 14.7 stub firmware (iBoot-10151.140.19.700.2), J516C",
        text_size: 0x64000,
        data_size: 0x140000,
        marker: 0x5cf28,
        text_sha256: [
            0xc2, 0x6e, 0xc8, 0x9c, 0x11, 0x50, 0xeb, 0x65, 0x49, 0xcf, 0x60, 0x2e, 0x01, 0x2d,
            0x0c, 0x5f, 0x49, 0xeb, 0x75, 0x6e, 0x89, 0x6d, 0x6b, 0xb8, 0x34, 0x2c, 0xb2, 0xc5,
            0xd9, 0x8b, 0x33, 0x2c,
        ],
    }],
    machine: kernel::c_str!("apple,t6031"),
    platform: super::Platform::DeviceTree,
    // The T6030 layout and board data do not fit this SoC (see the initdata commit); until
    // then admission stops before anything firmware-visible.
    layout: &super::t6030::LAYOUT,
    board: super::Board::None,
    compute: super::t6030::COMPUTE,
};
