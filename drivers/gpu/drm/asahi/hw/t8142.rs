// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5 (T8142, MacBook Air J813) on the macOS 27.0 (26A428) GPU firmware.
//!
//! Bring-up only: the M5 graph ([`crate::g17::fw::m5::initdata`]) has no J813 board yet, so
//! [`PROBE`] stops at the identity check, after `get_g17_id` has logged the ID registers and
//! core masks, before the UAT or the firmware are touched.
//!
//! J813 (2026-11-07): ID registers 0xb022000 0xb0800 0x11010a 0x40404 0x1320311 0x10030302,
//! family 0xb, variant 2, one die, one cluster of ten core slots; core mask 0x3bd (eight
//! enabled, slots 1 and 6 fused off).
use crate::g17::{Config, CoreLayout, CrashBuffers, FirmwareProfile, FixedBuffer, Identity};
use crate::hw::{GpuGen, GpuVariant};

// The ADT sgx gfx-data-shared-ro-base and gfx1-data-shared-ro-base (each 0x8000) under 27.0,
// where the M5 Pro's coprocessors ask for their crash logs.
const J813_CRASH_BUFFERS: [FixedBuffer; 2] = [
    FixedBuffer { va: 0x100_0250_4000, size: 0x8000 },
    FixedBuffer { va: 0x100_025c_0000, size: 0x8000 },
];

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
    crash_buffers: CrashBuffers::Fixed(J813_CRASH_BUFFERS),
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
