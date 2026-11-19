// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5 (T8142: MacBook Air J813 and J815, MacBook Pro 14-inch J704) on the macOS 27.0 (26A428)
//! and 27.2 (26B123) GPU firmware.
//!
//! The SoC's part of the GPU (identity, core layout, firmware builds, the M5 graph's
//! [`T8142`](crate::g17::fw::m5::initdata::T8142) template) is here; each board's power data
//! (performance states and voltages, relative power, AON rows, HW data, region C and power
//! policy words, idle standby timer) is its GPU node's, which [`board`] reads at probe. A node
//! without it is refused, so no board runs with another's power values.
//!
//! [`PROBE`] stops at the identity check, after `get_g17_id` has logged the ID registers and
//! core masks, before the UAT or the firmware are touched. [`select`] picks it when the
//! `m5_probe_only` module parameter is set (`aurora.m5_probe_only=1` with the driver built in),
//! otherwise [`CONFIG`].
//!
//! J813 (2026-11-07): ID registers 0xb022000 0xb0800 0x11010a 0x40404 0x1320311 0x10030302,
//! family 0xb, variant 2, one die, one cluster of ten core slots; core mask 0x3bd (eight
//! enabled, slots 1 and 6 fused off).
//!
//! J704 (m1n1 proxy reads of SGX, before this driver): the same ID registers, core mask 0x3ff
//! (all ten slots enabled).
use kernel::{c_str, device, of, prelude::*, sync::SetOnce};
use crate::g17::fw::m5::initdata::{Board, BoardPower, BoardPowerCells};
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

/// The T8142 on the M5 graph: [`PROBE`] with the GPU's identity and the T8142 board template,
/// which the probe completes with the board's GPU node ([`board`]). It runs either firmware
/// build of [`BUILDS`]; 26B123 runs [`CONFIG_26B123`].
pub(crate) const CONFIG: Config = Config {
    firmware: crate::g17::fw::variant::Firmware::Identified(&BUILDS),
    ..BASE
};

/// The T8142 board template and identity, before a firmware build is chosen.
const BASE: Config = Config {
    identity: Identity {
        family: 0xb,
        variant: 2,
        num_dies: 1,
        gpu_variant: GpuVariant::G,
        usc_generation: 3,
        hal_generation: 300,
    },
    m5: Some(&crate::g17::fw::m5::initdata::T8142),
    // One cluster of ten core slots; the buffers are sized for all ten.
    core_layout: CoreLayout {
        cores: None,
        alternate_core_masks: None,
        operand_state: crate::g17::fw::compute::OperandState::ONE_BY_TEN,
        deflake: crate::g17::fw::render::Deflake::ONE_CLUSTER,
        compute_blocks: crate::g17::fw::freelist::COMPUTE_BLOCKS,
    },
    // TEST: the sleep's commit stops the firmware and a real wake restarts it warm, as on
    // T8140. Not yet run through a real sleep on the J813.
    sleep_restart: true,
    ..PROBE
};

/// The T8142 on the 27.2 firmware: the board template on the common 26B123 graph.
pub(crate) const CONFIG_26B123: Config = Config {
    m5: Some(&crate::g17::fw::m5::initdata::T8142_26B123),
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
        == crate::g17::fw::m5::initdata::T8142.gpu_cores * 0x1800
);

/// The configuration the 26A428 (G17) path of a T8142 starts with: [`PROBE`] when the
/// `m5_probe_only` module parameter is set, else [`CONFIG`], whose board the probe reads from
/// the GPU node.
pub(crate) fn select() -> &'static Config {
    if crate::module_parameters::m5_probe_only.value() != 0 {
        &PROBE
    } else {
        &CONFIG
    }
}

/// The board's power data, read from its GPU node by the first probe.
static POWER: SetOnce<BoardPower> = SetOnce::new();
/// The board: the firmware build's template ([`CONFIG`] or [`CONFIG_26B123`]) with [`POWER`].
static BOARD: SetOnce<Board> = SetOnce::new();

/// `name`, a u32 array property of `node`; an empty property is an empty array.
fn cells(node: &of::Node, name: &CStr) -> Result<KVec<u32>> {
    node.get_property(name)
}

/// The GPU node's operating points, off state first: Hz and one core voltage cell, uV.
fn opps(node: &of::Node) -> Result<KVec<(u64, u32)>> {
    let table = node.parse_phandle(c_str!("operating-points-v2"), 0).ok_or(ENOENT)?;
    let mut opps = KVec::new();
    for opp in table.children() {
        let hz: u64 = opp.get_property(c_str!("opp-hz"))?;
        let microvolt: u32 = opp.get_property(c_str!("opp-microvolt"))?;
        opps.push((hz, microvolt), GFP_KERNEL)?;
    }
    Ok(opps)
}

/// The M5 board of a T8142 GPU: `template`, the firmware build's, with the power data of the
/// board's GPU node. Fails closed: a node without every property of its power data, or with one
/// [`BoardPower::new`] or [`Board::with_power`] refuses, fails the probe.
pub(crate) fn board(dev: &device::Device, template: &'static Board) -> Result<&'static Board> {
    let node = dev.of_node().ok_or(ENODEV)?;
    // Each property the board must state, read in this order.
    let read = |name: &'static CStr| {
        cells(&node, name).inspect_err(|e| {
            dev_err!(dev, "M5: GPU node has no valid {} ({:?}); the board's GPU power data must \
                           come from the device tree\n", name.to_str().unwrap_or("?"), e)
        })
    };
    let opps = opps(&node).inspect_err(|e| {
        dev_err!(dev, "M5: GPU node has no valid operating-points-v2 table ({:?}); the board's \
                       GPU power data must come from the device tree\n", e)
    })?;
    let sram_microvolt = read(c_str!("apple,sram-microvolt"))?;
    let relative_power = read(c_str!("apple,m5-relative-power"))?;
    let aon_records = read(c_str!("apple,m5-aon-records"))?;
    let hw_words = read(c_str!("apple,m5-hw-words"))?;
    let region_c_words = read(c_str!("apple,m5-region-c-words"))?;
    let policy_words = read(c_str!("apple,m5-policy-words"))?;
    let standby = c_str!("apple,idleoff-standby-timer");
    let idle_standby_us: u32 = node.get_property(standby).inspect_err(|e| {
        dev_err!(dev, "M5: GPU node has no valid {} ({:?}); the board's GPU power data must \
                       come from the device tree\n", standby.to_str().unwrap_or("?"), e)
    })?;
    let power = BoardPower::new(&BoardPowerCells {
        opps: &opps,
        sram_microvolt: &sram_microvolt,
        relative_power: &relative_power,
        aon_records: &aon_records,
        hw_words: &hw_words,
        region_c_words: &region_c_words,
        policy_words: &policy_words,
        idle_standby_us,
    })
    .map_err(|reason| {
        dev_err!(dev, "M5: invalid GPU power data in the device tree: {}\n", reason);
        EINVAL
    })?;
    // The first probe keeps the board for the rest of the boot; a later one (a rebind) must
    // read the same.
    match POWER.as_ref() {
        Some(kept) if *kept != power => {
            dev_err!(dev, "M5: the GPU node's power data changed since the first probe\n");
            return Err(EINVAL);
        }
        Some(_) => {}
        None => {
            POWER.populate(power);
        }
    }
    let kept = POWER.as_ref().ok_or(EBUSY)?;
    let board = template.with_power(kept).map_err(|reason| {
        dev_err!(dev, "M5: invalid GPU power data in the device tree: {}\n", reason);
        EINVAL
    })?;
    if BOARD.as_ref().is_none() {
        BOARD.populate(board);
    }
    let kept = BOARD.as_ref().ok_or(EBUSY)?;
    if !core::ptr::eq(kept.graph, template.graph) {
        dev_err!(dev, "M5: the GPU firmware build changed since the first probe\n");
        return Err(EINVAL);
    }
    dev_info!(dev, "M5: board power data from the device tree: {} performance states, top {} MHz\n",
              kept.perf_state_count(), kept.perf_states[13].0);
    Ok(kept)
}
