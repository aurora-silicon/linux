// SPDX-License-Identifier: GPL-2.0-only OR MIT


use crate::g16_config::layout::{IoMap, PhysicalAddress};
use crate::hw;

/// Immutable T8122 MTR alarm wiring, independent of per-Mac fast-die calibration.
const T8122_MTR_ALARM_MASK: u32 = 0x4b48;

pub(crate) const MAX_IO_SLOTS: usize = 53;

#[derive(Debug, Clone)]
pub(crate) struct FirmwareImage {
    pub(crate) text_size: u64,
    pub(crate) data_size: u64,
    pub(crate) text_sha256: [u8; 32],
    pub(crate) tlv_header: [u8; 8],
    pub(crate) gkts: usize,
    pub(crate) ecap: usize,
    pub(crate) loader_iomaps: usize,
}

#[derive(Debug, Clone)]
pub(crate) struct Board {
    pub(crate) name: &'static str,
    pub(crate) root_compatible: [&'static [u8]; 2],
    pub(crate) gpu_compatible: &'static [u8],
    pub(crate) asc: (u64, u64),
    pub(crate) sgx: (u64, u64),
    pub(crate) mbox_compatible: &'static [u8],
    pub(crate) mbox_reg: [u32; 4],
    pub(crate) mbox_irqs: [u32; 12],
    pub(crate) handoff_version: &'static kernel::str::CStr,
    pub(crate) firmware: FirmwareImage,
    pub(crate) sgx_version: u32,
    pub(crate) sgx_counts: u32,
    pub(crate) core_mask: u32,
    pub(crate) pmp_device: u16,
    pub(crate) io_slots: usize,
    pub(crate) iomaps: &'static [IoMap],
    pub(crate) empty_mode2: &'static [usize],
    pub(crate) protected_slot: Option<usize>,
    pub(crate) scratch: u64,
    pub(crate) frequencies_mhz: [u32; 16],
    pub(crate) aux_frequencies_mhz: [u32; 16],
    pub(crate) voltages_mv: [u32; 16],
    pub(crate) sram_voltages_mv: [u32; 16],
    pub(crate) main_f24: u64,
    pub(crate) pstate_count: usize,
    pub(crate) powers_mw: [u32; 16],
    pub(crate) max_power_mw: u32,
    pub(crate) sram_k: u32,
    pub(crate) calibration: Option<crate::g16_profile::Tables>,
    pub(crate) cores: u32,
    pub(crate) main_2540: [(usize, u32); 7],
    pub(crate) main_overlay: &'static [(usize, u32)],
    pub(crate) power_overlay: &'static [(usize, u32)],
    pub(crate) cached_hw_objects: bool,
    pub(crate) gpu_gen: hw::GpuGen,
    pub(crate) gpu_revision: hw::GpuRevision,
    pub(crate) chip_id: u32,
    pub(crate) compute_self_test: bool,
}

impl Board {
    pub(crate) fn max_pstate(&self) -> u32 { (self.pstate_count - 1) as u32 }
}

const fn iomap(slot: usize, physical: u64, mapped: usize, requested: usize, tag: u64, mode: u64) -> IoMap {
    IoMap { slot, physical: PhysicalAddress(physical), mapped, requested, tag, mode }
}

const J613_IOMAPS: &[IoMap] = &[
    IoMap { slot: 0, physical: PhysicalAddress(0x2d1014000), mapped: 0x4000, requested: 0x4000, tag: 0x0, mode: 2 },
    IoMap { slot: 3, physical: PhysicalAddress(0x2201c4000), mapped: 0x30000, requested: 0x18000, tag: 0x0, mode: 2 },
    IoMap { slot: 9, physical: PhysicalAddress(0x2d03d0000), mapped: 0x1000, requested: 0x1000, tag: 0x0, mode: 2 },
    IoMap { slot: 10, physical: PhysicalAddress(0x2d03c0000), mapped: 0x2000, requested: 0x2000, tag: 0x0, mode: 0 },
    IoMap { slot: 12, physical: PhysicalAddress(0x31145c000), mapped: 0x4000, requested: 0x4000, tag: 0x0, mode: 2 },
    IoMap { slot: 14, physical: PhysicalAddress(0x2d0280000), mapped: 0x8000, requested: 0x8000, tag: 0x0, mode: 0 },
    IoMap { slot: 17, physical: PhysicalAddress(0x290000000), mapped: 0x20000, requested: 0x20000, tag: 0x0, mode: 2 },
    IoMap { slot: 26, physical: PhysicalAddress(0x290d04000), mapped: 0x8000, requested: 0x8000, tag: 0xd04000, mode: 2 },
    IoMap { slot: 27, physical: PhysicalAddress(0x290d0d000), mapped: 0x1000, requested: 0x1000, tag: 0xd0d000, mode: 2 },
    IoMap { slot: 28, physical: PhysicalAddress(0x290d50000), mapped: 0x8000, requested: 0x8000, tag: 0xd50000, mode: 2 },
    IoMap { slot: 29, physical: PhysicalAddress(0x290d10000), mapped: 0x4000, requested: 0x4000, tag: 0xd10000, mode: 2 },
    IoMap { slot: 31, physical: PhysicalAddress(0x290d40000), mapped: 0x4000, requested: 0x4000, tag: 0xd40000, mode: 2 },
    IoMap { slot: 32, physical: PhysicalAddress(0x290d60000), mapped: 0x8000, requested: 0x8000, tag: 0xd60000, mode: 2 },
    IoMap { slot: 35, physical: PhysicalAddress(0x290e00000), mapped: 0x4000, requested: 0x4000, tag: 0xe00000, mode: 2 },
    IoMap { slot: 39, physical: PhysicalAddress(0x290e08000), mapped: 0x8000, requested: 0x8000, tag: 0x0, mode: 2 },
    IoMap { slot: 40, physical: PhysicalAddress(0x290e1c000), mapped: 0x4000, requested: 0x4000, tag: 0xe1c000, mode: 2 },
];
const J613_MAIN_OVERLAY: &[(usize, u32)] = &[
    (0xe90, 0x8122),
    (0xe94, 0x2),
    (0xe9c, 0x4),
    (0xea4, 0x1),
    (0xec0, 0x1),
    (0xec8, 0x1),
    (0xee4, 0x1),
    (0xee8, 0x1),
    (0xf04, 0x1f),
    (0xf34, 0x1),
    (0xf38, 0x0),
    (0xf4c, 0x31),
    (0xf6c, 0x1),
    (0xf88, 0xa),
    (0xf8c, 0x3),
    (0xfac, 0x1),
    (0xfb8, 0x16),
    (0xfbc, 0x5),
    (0x258c, 0x160000),
    (0x2594, 0x1),
    (0x2598, 0x1),
    (0x25a4, 0x1),
    (0x25a8, 0x1),
    (0x25ac, 0x1),
    (0x25b4, 0xffffffff),
    (0x25b8, 0xffffffff),
    (0x25bc, 0xffffffff),
    (0x25c0, 0xffffffff),
    (0x25c4, 0xffffffff),
    (0x25c8, 0xffffffff),
    (0x25cc, 0xffffffff),
    (0x25d0, 0xffffffff),
    (0x25d4, 0xffffffff),
    (0x25d8, 0xffffffff),
    (0x25dc, 0xffffffff),
    (0x25e0, 0xffffffff),
    (0x2604, 0x1),
    (0x2634, 0x1),
];

const J613_POWER_OVERLAY: &[(usize, u32)] = &[
    (0x2224, 0x3f4ccccd), (0x2228, 0x3e4ccccd), 
    (0x2278, 0x4), (0x228c, 0x3f800000), (0x22a0, 0x47800000), (0x3b58, T8122_MTR_ALARM_MASK), (0x3b60, 0x1),
];

pub(crate) static J613: Board = Board {
    name: "J613",
    root_compatible: [b"apple,j613", b"apple,t8122"],
    gpu_compatible: b"apple,agx-t8122\0",
    asc: (0x292400000, 0x4000),
    sgx: (0x290000000, 0x4000000),
    mbox_compatible: b"apple,t8122-agx-asc-mailbox\0apple,asc-mailbox-v4\0",
    mbox_reg: [2, 0x92408000, 0, 0x4000],
    mbox_irqs: [0, 723, 4, 0, 724, 4, 0, 725, 4, 0, 726, 4],
    handoff_version: kernel::c_str!("apple,m3-handoff-version"),
    firmware: FirmwareImage {
        text_size: 0x60000,
        data_size: 0x12c000,
        text_sha256: [
            0x3a, 0x6b, 0x8b, 0x5e, 0x83, 0x38, 0x4d, 0x5c, 0xc1, 0x92, 0x9d, 0x91, 0xb6, 0x36, 0x40, 0xfe,
            0xe8, 0xf5, 0x52, 0xb5, 0x10, 0x8c, 0x4e, 0xa6, 0x20, 0xbe, 0xdb, 0x7b, 0xbc, 0xa0, 0x31, 0x64,
        ],
        tlv_header: [0x4c, 0xce, 5, 0, 0x31, 2, 0, 0],
        gkts: 0x5ce4c,
        ecap: 0x5cf2f,
        loader_iomaps: 0x1228e0,
    },
    sgx_version: 0x07022000,
    sgx_counts: 0x0011010a,
    core_mask: 0x3ff,
    pmp_device: 5,
    io_slots: MAX_IO_SLOTS,
    iomaps: J613_IOMAPS,
    empty_mode2: &[2, 5, 6, 7, 8, 30, 33, 34, 37, 38, 41, 42, 43, 46, 47, 48, 49, 52],
    protected_slot: None,
    scratch: 0x290d60000,
    frequencies_mhz: [0; 16],
    aux_frequencies_mhz: [0; 16],
    voltages_mv: [0; 16],
    sram_voltages_mv: [0; 16],
    main_f24: 0x4,
    pstate_count: 0,
    powers_mw: [0; 16],
    max_power_mw: 0,
    sram_k: 0,
    calibration: None,
    cores: 10,
    main_2540: [(4, 4), (8, 6), (12, 3), (16, 7), (20, 7), (40, 10), (48, 1)],
    main_overlay: J613_MAIN_OVERLAY,
    power_overlay: J613_POWER_OVERLAY,
    cached_hw_objects: true,
    gpu_gen: hw::GpuGen::G15,
    gpu_revision: hw::GpuRevision::C0,
    chip_id: 0x8122,
    compute_self_test: true,
};

#[cfg(not(test))]
static CURRENT: kernel::sync::SetOnce<Board> = kernel::sync::SetOnce::new();

#[cfg(not(test))]
pub(crate) fn initialize(pdev: &kernel::platform::Device<kernel::device::Core>) -> kernel::error::Result {
    use kernel::{c_str, prelude::*};
    if !crate::g16_profile::selected(pdev)? { return Err(ENODEV); }
    if CURRENT.as_ref().is_some() { return Ok(()); }
    let node = pdev.as_ref().of_node().ok_or(ENODEV)?;
    if node.get_property::<u32>(c_str!("apple,gpu-power-model"))? != 1 { return Err(EINVAL); }
    let opps = node.parse_phandle(c_str!("operating-points-v2"), 0).ok_or(ENODEV)?;
    let mut points = KVec::new();
    for opp in opps.children() {
        let hz = opp.get_property::<u64>(c_str!("opp-hz"))?;
        let core = opp.get_property::<KVec<u32>>(c_str!("opp-microvolt"))?;
        let sram = opp.get_property::<KVec<u32>>(c_str!("opp-microvolt-sram"))?;
        let power = opp.get_property::<u32>(c_str!("opp-microwatt"))?;
        if core.len() != 1 || sram.len() != 1 { return Err(EINVAL); }
        points.push((hz, core[0], sram[0], power), GFP_KERNEL)?;
    }
    let tables = crate::g16_profile::performance_tables(&points).ok_or(EINVAL)?;
    let mut board = J613.clone();
    board.pstate_count = tables.count;
    board.frequencies_mhz = tables.primary;
    board.aux_frequencies_mhz = tables.secondary;
    board.voltages_mv = tables.core_mv;
    board.sram_voltages_mv = tables.sram_mv;
    board.powers_mw = tables.power_mw;
    board.max_power_mw = tables.power_mw[..tables.count].iter().copied().max().ok_or(EINVAL)?;
    if board.max_power_mw == 0 { return Err(EINVAL); }
    board.calibration = Some(tables);
    board.sram_k = crate::hw::t8122::HWCONFIG_T8122.sram_k.to_bits();
    CURRENT.populate(board);
    Ok(())
}

#[cfg(not(test))]
pub(crate) fn get() -> kernel::error::Result<&'static Board> {
    CURRENT.as_ref().ok_or(kernel::error::code::ENODEV)
}
