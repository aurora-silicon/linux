// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! T6031 (M3 Max, G15C: J514C, J516C) configuration.
//!
//! One die, four clusters of ten core slots. The firmware is the same RTKit 2419 generation as
//! the T6030's and the initdata graph keeps the T6030 canonical VAs; what differs is the SoC
//! identity, the 40-bit enabled-core mask (two words), the source of the GPU regions (the
//! device tree node the boot loader fills, not an ADT export) and the GPU power (its power
//! domain; there is no PMP vote). The identity words were read on a J516C.

use super::super::Config;
use super::HwId;

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
    // No build is pinned yet: admission prints the measured TEXT identity and refuses.
    firmware: &[],
    machine: kernel::c_str!("apple,t6031"),
    platform: super::Platform::DeviceTree,
    // The T6030 layout and board data do not fit this SoC (see the initdata commit); until
    // then admission stops before anything firmware-visible.
    layout: &super::t6030::LAYOUT,
    board: super::Board::None,
    compute: super::t6030::COMPUTE,
};
