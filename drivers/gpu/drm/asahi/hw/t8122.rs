// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware configuration for T8122 (M3, G15G): one die, one cluster of ten core slots.
//!
//! Only the M3 runtime reads this table (`m3_soc::T8122`), and it refuses a T8122 until the
//! rest of the SoC's runtime configuration is known (`m3_soc::Soc::require_complete`). The
//! G15 manager backend never starts on T8122.

use crate::f32;

use super::*;

/// Fender: ADT sgx reg[1] translated through the /arm-io ranges (SGX + 0xd00000).
const FENDER_BASE: usize = 0x2_90d0_0000;
/// The GPU register window: ADT sgx reg[0] translated through the /arm-io ranges.
const SGX_BASE: usize = 0x2_9000_0000;

/// The firmware IO mappings, one per HwDataB slot (31 on G15). A slot is `None` when T8122 has
/// no block for it; its record then stays zero. Every address is a CPU physical address.
const IO_MAPPINGS: [Option<IOMapping>; 31] = [
    // 0: Fender. The window is 0x104000 bytes: the ADT sgx reg[1] size (0x12c000) less 0x28000,
    // the same difference as on T6030.
    Some(IOMapping::new(FENDER_BASE, false, 1, 0x10_4000, 0, true)),
    // 1: AIC timer, the same 16 KiB page on every SoC.
    Some(IOMapping::new(0x2_0e10_0000, false, 1, 0x4000, 0, false)),
    // 2: AIC software interrupts: AIC (0x2d1000000) + 0x14000, the page that holds the ADT's
    // meta-sw-interrupt registers.
    Some(IOMapping::new(0x2_d101_4000, false, 1, 0x4000, 0, true)),
    // 3: RGX: the first 128 KiB of the GPU register window.
    Some(IOMapping::new(SGX_BASE, false, 1, 0x2_0000, 0, true)),
    None, // 4: UVD: the ADT sgx node has no third reg range
    None, // 5: unused
    None, // 6: display underrun workaround
    None, // 7: analog temperature sensors
    None, // 8: PMP doorbell
    // 9: metrology sensors, SGX + 0xe08000.
    Some(IOMapping::new(SGX_BASE + 0xe0_8000, false, 1, 0x8000, 0, true)),
    // 10: GM GIFAF registers, a 4 KiB page at Fender + 0xd000.
    Some(IOMapping::new(FENDER_BASE + 0xd000, false, 1, 0x1000, 0, true)),
    // 11: memory cache, two instances of 0x58000 bytes (a multiple of 16 KiB), at 0x220000000
    // and 0x222000000.
    Some(IOMapping::new(0x2_2000_0000, false, 2, 0x5_8000, 0x200_0000, true)),
    None, // 12: AIC banked registers
    None, // 13: PMGR scratch
    None, // 14: NIA idle register, die 0
    None, // 15: NIA idle register, die 1
    None, // 16: CRE registers
    None, // 17: streaming codec registers
    // 18: telemetry dashboard, written by the firmware: the 4 KiB at +0x10000 of the PMP's
    // telemetry window (0x2d03c0000, 0x14000 bytes).
    Some(IOMapping::new(0x2_d03d_0000, false, 1, 0x1000, 0, true)),
    // 19: telemetry dashboard, read by the firmware: the first 8 KiB of the same window.
    Some(IOMapping::new(0x2_d03c_0000, false, 1, 0x2000, 0, false)),
    None, // 20: telemetry dashboard configuration
    None, // 21
    None, // 22
    None, // 23: AFR registers
    None, // 24
    // 25: ANE doorbell, ANE + 0x45c000.
    Some(IOMapping::new(0x3_1145_c000, false, 1, 0x4000, 0, true)),
    // 26: PMS metrology sensors.
    Some(IOMapping::new(0x2_d028_0000, false, 1, 0x8000, 0, false)),
    None, // 27
    None, // 28
    // 29: GPU clock generator, SGX + 0xe1c000.
    Some(IOMapping::new(SGX_BASE + 0xe1_c000, false, 1, 0x4000, 0, false)),
    None, // 30
];

/// T8122 (M3, G15G): one die, one cluster of ten core slots, eight or ten of them active.
pub(crate) const HWCONFIG_T8122: super::HwConfig = HwConfig {
    chip_id: 0x8122,
    gpu_gen: GpuGen::G15,       // ID_VERSION[31:24] == 7
    gpu_variant: GpuVariant::G, // ID_VERSION[23:16] == 2
    gpu_core: Some(GpuCore::G15G),

    // The 24 MHz reference clock (HwDataB base_clock_khz = 24000).
    base_clock_hz: 24_000_000,
    // 42-bit UAT roots and output addresses, as on every AGX3 part (hw::agx3::T8122).
    uat_ias: 42,
    uat_oas: 42,
    num_dies: 1,         // ID_COUNTS_1[19:16]
    max_num_clusters: 1, // one cluster of ten core slots
    max_num_cores: 10,
    max_num_frags: 10, // one fragment unit per core slot
    max_num_gps: 4,    // ID_COUNTS_2[23:16]

    // Not read by the M3 runtime, which keeps its own buffer layout.
    preempt1_size: 0,
    preempt2_size: 0,
    preempt3_size: 0,
    // Compute preemption buffer: 0xd80 bytes plus 0x700 per cluster.
    compute_preempt1_size: 0x1480,
    clustering: None,
    render: HwRenderConfig { tiling_control: 0 },

    da: HwConfigA {
        // Cleared by the M3 runtime's InitData in any case.
        unk_87c: 0,
        // Fast-die temperature target, in centidegrees Celsius (110 C, as on T6030). The J613
        // ADT's gpu-fast-die0-target is 88; how the two relate is not known.
        unk_8cc: 11_000,
        unk_e24: 0,
    },
    db: HwConfigB {
        // The chip revision's major number: /arm-io chip-revision 0x20 >> 4.
        unk_454: 2,
        // The G15 InitData builders read none of the words below.
        unk_4e0: 0,
        unk_534: 0,
        unk_ab8: 0,
        unk_abc: 0,
        unk_b30: 1,
    },
    // G15 leaves the hws1/hws2/hws3 curves zero, as on T6030.
    shared1_tab: &[],
    shared1_a4: 0,
    shared2_tab: &[],
    shared2_unk_508: 0,
    shared2_curves: None,
    shared3_unk: 0,
    shared3_tab: &[],
    // Idle-off standby timer when the device tree gives none: the J613 ADT has no
    // gpu-idleoff-standby-timer.
    idle_off_standby_timer_default: 700,
    // No smart idle-off curve; those Globals words stay 0.
    unk_hws2_4: None,
    unk_hws2_24: 0,
    // The M3 runtime writes 0xffff to this Globals word in any case.
    global_unk_54: 0,
    // SRAM power scale 1.02 for every performance state, as on T6030.
    sram_k: f32!(1.02),
    unk_coef_a: &[],
    unk_coef_b: &[],
    // No FwUtil performance tunables are written.
    global_tab: None,
    has_csafr: false,
    // The fast-die sensor mask is each machine's: the M3 runtime takes it from the boot
    // loader's apple,fast-die0-sensor-mask (the ADT's gpu-fast-die0-sensor-mask), not from here.
    fast_sensor_mask: [0, 0],
    fast_sensor_mask_alt: [0, 0],
    fast_die0_sensor_present: 0,
    io_mappings: &IO_MAPPINGS,
    // No Fender scratch SRAM window.
    sram_base: None,
    sram_size: None,
};
