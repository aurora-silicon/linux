// SPDX-License-Identifier: GPL-2.0-only OR MIT


use crate::f32;

use super::*;

/// SGX (RGX) register window base: ADT sgx reg[0] + /arm-io ranges.
const SGX_BASE: usize = 0x2_9000_0000;
/// Fender base: ADT sgx reg[1]; always SGX + 0xd00000.
const FENDER_BASE: usize = SGX_BASE + 0xd0_0000;

/// SRAM rail floor used when there is no `perf-states-sram` table, in microvolts.
/// The commonly used G15 default is 789 mV, but the J516S runtime ADT perf-states-sram table
/// floors SRAM at 810 mV; use the higher, safe value.
pub(crate) const MIN_SRAM_MICROVOLT: u32 = 810_000;

const fn iomaps() -> [Option<IOMapping>; 31] {
    [
        Some(IOMapping::new(FENDER_BASE, false, 1, 0x144000, 0, true)), // 0 Fender
        // 1 AICTimer: hard-coded 0x20e101000 & ~page for all SoCs.
        // TODO: whether this block exists on t6030 (AIC is at 0x3_5100_0000).
        Some(IOMapping::new(0x2_0e10_0000, false, 1, 0x4000, 0, false)), // 1 AICTimer
        Some(IOMapping::new(0x3_5101_4000, false, 1, 0x4000, 0, true)),  // 2 AICSWInt (AIC+0x14000)
        Some(IOMapping::new(SGX_BASE, false, 1, 0x20000, 0, true)),      // 3 RGX
        None,                                                            // 4 UVD (no 3rd ADT range)
        None,                                                            // 5 unused
        None,                                                            // 6 DisplayUnderrunWA
        Some(IOMapping::new(0x3_502b_c000, false, 1, 0x4000, 0, false)), // 7 AnalogTempSensorControllerRegs
        None,                                                            // 8 PMPDoorbell
        Some(IOMapping::new(
            SGX_BASE + 0xe0_8000,
            false,
            1,
            0x8000,
            0,
            true,
        )), // 9 MetrologySensorRegs
        None,                                                            // 10 GM GIFAF
        // 11 MCache: 2 instances, 0x98000 each, at 0x2_2000_0000 + i * 0x200_0000
        Some(IOMapping::new(
            0x2_2000_0000,
            false,
            2,
            0x98000,
            0x200_0000,
            true,
        )), // 11 MCache registers
        Some(IOMapping::new(0x3_5104_c000, false, 1, 0x4000, 0, false)), // 12 AICBankedRegisters
        None,                                                            // 13 PMGRScratch
        None, // 14 NIA Special agent idle register die 0
        None, // 15 NIA Special agent idle register die 1
        None, // 16 CRE registers (the CRE lives inside Fender on G15)
        None, // 17 Streaming codec registers
        Some(IOMapping::new(0x3_503d_0000, false, 1, 0x4000, 0, true)), // 18 PushTelemetryDashboardRegs
        Some(IOMapping::new(0x3_503c_0000, false, 1, 0x4000, 0, false)), // 19 PushTelemetryDashboardReadRegs
        Some(IOMapping::new(0x3_503d_8000, false, 1, 0x4000, 0, true)), // 20 PushTelemetryDashboardConfigRegs
        None, // 21 AFRClkGenRegs (G15C only)
        None, // 22 ?
        // 23 AFRRegs: SGX+0x3000000, G15S only; firmware PIO type 0x17
        Some(IOMapping::new(
            SGX_BASE + 0x300_0000,
            false,
            1,
            0x40_0000,
            0,
            true,
        )), // 23 AFRRegs
        None, // 24 GFXCAXI2AFRegs (G15C only)
        Some(IOMapping::new(0x3_0945_c000, false, 1, 0x4000, 0, true)), // 25 ANE0Doorbell
        Some(IOMapping::new(0x3_5028_0000, false, 1, 0x8000, 0, false)), // 26 PMSMetrologySensorRegs
        None,                                                            // 27 ?
        None, // 28 AONPTDSpace (G15C only)
        Some(IOMapping::new(
            SGX_BASE + 0xe5_c000,
            false,
            1,
            0x4000,
            0,
            false,
        )), // 29 GFXCLKGEN_MGPU
        None, // 30 ?
    ]
}

/// t6030 (M3 Pro): one die, two MGPU clusters of ten core slots.
pub(crate) const HWCONFIG_T6030: super::HwConfig = HwConfig {
    chip_id: 0x6030,
    gpu_gen: GpuGen::G15,       // ID_VERSION[31:24] == 7
    gpu_variant: GpuVariant::S, // ID_VERSION[23:16] == 3
    gpu_core: Some(GpuCore::G15S), // firmware core type 0x17

    // HwDataB base_clock_khz = 24000 (HwDataB +0xa68)
    base_clock_hz: 24_000_000,
    // DRAM starts at 0x100_0000_0000; physical addresses are 42 bits (mask 0x3ff_ffff_c000)
    uat_oas: 42,
    uat_ias: 42,
    num_dies: 1,         // num_agcs = ID_COUNTS_1[19:16], expected 1
    max_num_clusters: 2, // 20 core slots / 10 per MGPU
    max_num_cores: 10,   // G15S has 10 core slots per MGPU
    max_num_frags: 10,   // num_frags == num_cores
    // TODO: real ID_COUNTS_2[23:16]; default from t602x
    max_num_gps: 4,

    // TODO: preemption buffer sizes; defaults from t602x
    preempt1_size: 0x540,
    preempt2_size: 0x280,
    preempt3_size: 0x40,
    compute_preempt1_size: 0x25980,
    // TODO: clustering metadata sizes; defaults from t602x
    clustering: Some(HwClusteringConfig {
        meta1_blocksize: 0x44,
        meta2_size: 0xc0 * 16,
        meta3_size: 0x280 * 16,
        meta4_size: 0x10 * 128,
        max_splits: 64,
    }),

    // TODO: tiling control register value; default from t602x
    render: HwRenderConfig {
        tiling_control: 0x180340,
    },

    // TODO: HwDataA values; defaults from t6020 (G15 HwDataA follows the G13
    // 13.5 layout, values not traced)
    da: HwConfigA {
        unk_87c: 500,
        unk_8cc: 11000,
        unk_e24: 125,
    },
    // TODO: HwDataB values. unk_ab8 (+0xab8) = 0; unk_abc is a board value (not known).
    // Others default from t6020.
    db: HwConfigB {
        unk_454: 0,
        unk_4e0: 4,
        unk_534: 0,
        unk_ab8: 0,
        unk_abc: 0,
        unk_b30: 0,
    },
    // G13's hws1/hws2/hws3 static curves are not written by the G15 host; the firmware starts
    // from a zeroed buffer (0x1294..0x3e34). Leave them zero.
    shared1_tab: &[],
    shared1_a4: 0,
    shared2_tab: &[],
    shared2_unk_508: 0,
    shared2_curves: None,
    shared3_unk: 0,
    shared3_tab: &[],
    // ADT sgx gpu-idleoff-standby-timer = 1500 (J516S)
    idle_off_standby_timer_default: 1500,
    // TODO: Globals hws2 values; defaults from t6020
    unk_hws2_4: Some(f32!([1.0, 0.8, 0.2, 0.9, 0.1, 0.25, 0.7, 0.9])),
    unk_hws2_24: 6,
    // TODO: Globals unk_54; default from t602x
    global_unk_54: 4000,
    // SRAM power scale 1.02 (0x3f828f5c) for every pstate
    sram_k: f32!(1.02),
    // HwDataA 0x970-0xb6f (unk_coef_a1/a2) and unk_coef_b1/b2 are left zero by the G15 host.
    unk_coef_a: &[],
    unk_coef_b: &[],
    // FwUtil perf tunables: u32 1, then these 21 bytes. The builder writes the
    // leading 1 into unk_118e8 and the bytes from unk_118ec, which is the same shape.
    // TODO: confirm where these land in the re-packed 0xe00-byte G15 Globals.
    global_tab: Some(&[
        0x00, 0x01, 0x02, 0x01, 0x01, 0x5a, 0x4b, 0x01, 0x01, 0x01, 0x02, 0x5a, 0x4b, 0x01, 0x01,
        0x01, 0x01, 0x5a, 0x4b, 0x01, 0x01,
    ]),
    // G15S has no CS/AFR clock domains
    has_csafr: false,
    // G15 encodes the fast-die sensors as a 64-bit MTR sensor bitmask. G15S requires 0x402a002b
    // (bits 0,1,3,5,17,19,21,30); HwDataA +0x8ac takes it. Single die.
    fast_sensor_mask: [0x402a002b, 0],
    // TODO: HwDataA +0x1a98 is a separate board value; default = MTR mask
    fast_sensor_mask_alt: [0x402a002b, 0],
    // TODO: not traced; t602x leaves this unused
    fast_die0_sensor_present: 0,
    io_mappings: &iomaps(),
    // Fender scratch SRAM: Fender+0x60000, 128 KiB; holds the pipe rings and host->FW ring
    // indices on G15 (IO-mapping entry 31)
    sram_base: Some(FENDER_BASE + 0x6_0000),
    sram_size: Some(0x20000),
};
