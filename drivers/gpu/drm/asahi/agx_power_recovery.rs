// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


/// The grounded, pure validation model is available.
pub(crate) const GROUNDED_POWER_RECOVERY_MODEL_AVAILABLE: bool = true;

/// Exact targets whose host binaries establish the shared recovery grammar.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RecoveryEvidenceTarget {
    G15G,
    G15S,
    G15C,
    G15D,
    G16G,
    G16S,
    G16C,
    G17P,
    G17G,
    G17S,
}

pub(crate) const RECOVERY_EVIDENCE_TARGETS: [RecoveryEvidenceTarget; 10] = [
    RecoveryEvidenceTarget::G15G,
    RecoveryEvidenceTarget::G15S,
    RecoveryEvidenceTarget::G15C,
    RecoveryEvidenceTarget::G15D,
    RecoveryEvidenceTarget::G16G,
    RecoveryEvidenceTarget::G16S,
    RecoveryEvidenceTarget::G16C,
    RecoveryEvidenceTarget::G17P,
    RecoveryEvidenceTarget::G17G,
    RecoveryEvidenceTarget::G17S,
];

/// G17 GFX1 images establish the internal transition grammar and one idle
/// poll. Only G17S establishes the direct PIO prefix and intervening
/// firmware-operation order. All three targets establish physical type-0
/// mappings. G17G/G17P leave type 0x0b unbound, while G17S installs it in a
/// target override; none of those facts proves Linux MMIO ownership.
pub(crate) const EXECUTABLE_GFX1_POWER_SEQUENCE_AVAILABLE: bool = false;

/// The firmware writes and cross-role 0x22/0x23 protocol remain unproved, so
/// Linux must not execute G17 recovery from this host-write subset.
pub(crate) const EXECUTABLE_G17_RECOVERY_HANDSHAKE_AVAILABLE: bool = false;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PmConfigEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

pub(crate) const G17_PM_CONFIG_EVIDENCE_TARGETS: [G17PmConfigEvidenceTarget; 3] = [
    G17PmConfigEvidenceTarget::G17P,
    G17PmConfigEvidenceTarget::G17G,
    G17PmConfigEvidenceTarget::G17S,
];

pub(crate) const G17_HAL200_PM_CONFIG_OPAQUE: [u64; 6] = [0x4, 0x4, 0x1800, 0x8, 0x8, 0x10];
pub(crate) const G17_HAL300_PM_CONFIG_OPAQUE: [u64; 6] = [0x10, 0x40, 0x1800, 0x8, 0x8, 0x40];

pub(crate) const fn g17_pm_config_for_evidence_target(
    target: G17PmConfigEvidenceTarget,
) -> &'static [u64; 6] {
    match target {
        G17PmConfigEvidenceTarget::G17P => &G17_HAL200_PM_CONFIG_OPAQUE,
        G17PmConfigEvidenceTarget::G17G | G17PmConfigEvidenceTarget::G17S => {
            &G17_HAL300_PM_CONFIG_OPAQUE
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T6050PerfState {
    pub(crate) freq_hz: u32,
    pub(crate) volt_mv: u32,
}

pub(crate) const T6050_G17S_PERF_STATE_COUNT: u32 = 14;
pub(crate) const T6050_G17S_PERF_STATE_TABLE_COUNT: u32 = 2;
const T6050_G17S_MAIN_PERF_STATE_RECORDS: usize =
    T6050_G17S_PERF_STATE_COUNT as usize * T6050_G17S_PERF_STATE_TABLE_COUNT as usize;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T6050PerfStateMirrorError {
    UnexpectedPerfStateCount,
    UnexpectedPerfStateTableCount,
    UnexpectedMainCoreShape,
    UnexpectedMainSramShape,
    UnexpectedCoreMirrorShape,
    UnexpectedSramMirrorShape,
    CoreMirrorMismatch,
    SramMirrorMismatch,
}

pub(crate) fn validate_t6050_g17s_perf_state_mirrors(
    perf_state_count: u32,
    perf_state_table_count: u32,
    perf_states: &[T6050PerfState],
    perf_states_sram: &[T6050PerfState],
    perf_states1: &[T6050PerfState],
    perf_states_sram1: &[T6050PerfState],
) -> Result<(), T6050PerfStateMirrorError> {
    if perf_state_count != T6050_G17S_PERF_STATE_COUNT {
        return Err(T6050PerfStateMirrorError::UnexpectedPerfStateCount);
    }
    if perf_state_table_count != T6050_G17S_PERF_STATE_TABLE_COUNT {
        return Err(T6050PerfStateMirrorError::UnexpectedPerfStateTableCount);
    }
    if perf_states.len() != T6050_G17S_MAIN_PERF_STATE_RECORDS {
        return Err(T6050PerfStateMirrorError::UnexpectedMainCoreShape);
    }
    if perf_states_sram.len() != T6050_G17S_MAIN_PERF_STATE_RECORDS {
        return Err(T6050PerfStateMirrorError::UnexpectedMainSramShape);
    }
    if perf_states1.len() != T6050_G17S_PERF_STATE_COUNT as usize {
        return Err(T6050PerfStateMirrorError::UnexpectedCoreMirrorShape);
    }
    if perf_states_sram1.len() != T6050_G17S_PERF_STATE_COUNT as usize {
        return Err(T6050PerfStateMirrorError::UnexpectedSramMirrorShape);
    }

    let table1_start = T6050_G17S_PERF_STATE_COUNT as usize;
    if perf_states1 != &perf_states[table1_start..] {
        return Err(T6050PerfStateMirrorError::CoreMirrorMismatch);
    }
    if perf_states_sram1 != &perf_states_sram[table1_start..] {
        return Err(T6050PerfStateMirrorError::SramMirrorMismatch);
    }

    Ok(())
}

/// G17 GFX1 internal power states whose numeric names are string-confirmed.
pub(crate) const GFX1_POWER_SLEEP: u8 = 0x08;
pub(crate) const GFX1_POWER_NAP: u8 = 0x10;

/// Mandatory intermediate state between SLEEP and NAP.
///
/// Its private Apple enum name is not known, so the numeric value is kept
/// without assigning a semantic name.
pub(crate) const GFX1_POWER_INTERMEDIATE_21: u8 = 0x21;

/// G17 targets with independently pinned GFX1 transition and idle-poll bytes.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17Gfx1PowerEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

pub(crate) const G17_GFX1_POWER_EVIDENCE_TARGETS: [G17Gfx1PowerEvidenceTarget; 3] = [
    G17Gfx1PowerEvidenceTarget::G17P,
    G17Gfx1PowerEvidenceTarget::G17G,
    G17Gfx1PowerEvidenceTarget::G17S,
];

/// Exact target-specific call site for the shared pre-idle CAS helper.
///
/// The three helper bodies have the same 53-instruction normalized dataflow:
/// their only body-local atomic access compares `1` and replaces it with `0`
/// at the passed firmware-global object. G17S executes its direct PIO prefix
/// before the same helper, while G17G/G17P reach the helper without that
/// prefix. Thus the helper cannot account for the missing target PIO writes.
/// Its private name and the ownership of its scheduler slow path stay opaque.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17Gfx1PreIdleEntryEvidence {
    pub(crate) helper_text_offset: u32,
    pub(crate) helper_object_vm_offset: u32,
    pub(crate) direct_pio_prefix_before_helper: bool,
}

pub(crate) const G17_GFX1_PRE_IDLE_HELPER_WINDOW_SIZE: u32 = 0xd4;
pub(crate) const G17_GFX1_PRE_IDLE_HELPER_INSTRUCTION_COUNT: u32 = 53;
pub(crate) const G17_GFX1_PRE_IDLE_HELPER_CAS_COMPARE: u64 = 1;
pub(crate) const G17_GFX1_PRE_IDLE_HELPER_CAS_REPLACEMENT: u64 = 0;
/// No helper body directly resolves or accesses a PIO descriptor.
pub(crate) const G17_GFX1_PRE_IDLE_HELPER_CONTAINS_DIRECT_PIO_ACCESS: bool = false;

pub(crate) const fn g17_gfx1_pre_idle_entry_evidence(
    target: G17Gfx1PowerEvidenceTarget,
) -> G17Gfx1PreIdleEntryEvidence {
    match target {
        G17Gfx1PowerEvidenceTarget::G17P => G17Gfx1PreIdleEntryEvidence {
            helper_text_offset: 0x0003_5f5c,
            helper_object_vm_offset: 0x000f_90f0,
            direct_pio_prefix_before_helper: false,
        },
        G17Gfx1PowerEvidenceTarget::G17G => G17Gfx1PreIdleEntryEvidence {
            helper_text_offset: 0x0003_5db4,
            helper_object_vm_offset: 0x000f_3c30,
            direct_pio_prefix_before_helper: false,
        },
        G17Gfx1PowerEvidenceTarget::G17S => G17Gfx1PreIdleEntryEvidence {
            helper_text_offset: 0x0003_b1d8,
            helper_object_vm_offset: 0x0010_0df0,
            direct_pio_prefix_before_helper: true,
        },
    }
}

/// Semantic G17 PIO types used by the pinned G17S GFX1 transition callback.
///
/// The firmware's 52-entry name/type table and type-indexed 0x28-byte PIO
/// descriptor copier directly pair these names and numeric values. Independently
/// pinned G17G/G17P/G17S host mappings are modeled below.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum G17PioSlot {
    GpcAicSwInt = 0x00,
    PushTelemetryDashboardConfigSpace = 0x0b,
    FenderRegsMcw = 0x24,
}

/// Exact PIO descriptor layout used by the pinned firmware's type copier.
pub(crate) const G17S_PIO_DESCRIPTOR_SIZE: u32 = 0x28;
pub(crate) const G17S_PIO_DESCRIPTOR_POINTER_OFFSET: u32 = 0x08;

/// Offset of one type's descriptor from the firmware PIO descriptor mirror.
pub(crate) const fn g17s_pio_descriptor_offset(slot: G17PioSlot) -> u32 {
    slot as u32 * G17S_PIO_DESCRIPTOR_SIZE
}

/// Offset of the callback's mapped-pointer field from the descriptor mirror.
pub(crate) const fn g17s_pio_mapped_pointer_offset(slot: G17PioSlot) -> u32 {
    g17s_pio_descriptor_offset(slot) + G17S_PIO_DESCRIPTOR_POINTER_OFFSET
}

/// One exact 32-bit PIO operation in firmware execution order.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PioOp {
    Write {
        slot: G17PioSlot,
        offset: u32,
        value: u32,
    },
    SetBits {
        slot: G17PioSlot,
        offset: u32,
        mask: u32,
    },
}

/// Ordered PIO prefix for G17S GFX1 `0x21 -> SLEEP`.
///
/// Firmware-internal synchronization and state checks occur after this prefix
/// and before the idle poll. Consequently this is not an executable sequence.
pub(crate) const G17S_GFX1_21_TO_SLEEP_PIO_PREFIX: [G17PioOp; 2] = [
    G17PioOp::Write {
        slot: G17PioSlot::GpcAicSwInt,
        offset: 0x430,
        value: 0x0800_0000,
    },
    G17PioOp::Write {
        slot: G17PioSlot::PushTelemetryDashboardConfigSpace,
        offset: 0x354,
        value: 0x0020_0000,
    },
];

/// Ordered PIO prefix for G17S GFX1 `SLEEP/NAP -> 0x21`.
pub(crate) const G17S_GFX1_LOW_POWER_TO_21_PIO_PREFIX: [G17PioOp; 3] = [
    G17PioOp::Write {
        slot: G17PioSlot::GpcAicSwInt,
        offset: 0x630,
        value: 0x0800_0000,
    },
    G17PioOp::SetBits {
        slot: G17PioSlot::PushTelemetryDashboardConfigSpace,
        offset: 0x758,
        mask: 0x0000_0b00,
    },
    G17PioOp::Write {
        slot: G17PioSlot::PushTelemetryDashboardConfigSpace,
        offset: 0x254,
        value: 0x0020_0000,
    },
];

/// Compatibility alias retained for the original, reversed direction name.
pub(crate) const G17S_GFX1_SLEEP_TO_21_PIO_PREFIX: [G17PioOp; 2] =
    G17S_GFX1_21_TO_SLEEP_PIO_PREFIX;
/// Compatibility alias retained for the original, reversed direction name.
pub(crate) const G17S_GFX1_21_TO_LOW_POWER_PIO_PREFIX: [G17PioOp; 3] =
    G17S_GFX1_LOW_POWER_TO_21_PIO_PREFIX;

/// Validate only the exact observed `0x21 -> SLEEP` PIO prefix.
pub(crate) fn validate_g17s_gfx1_21_to_sleep_pio_prefix(ops: &[G17PioOp]) -> bool {
    ops == G17S_GFX1_21_TO_SLEEP_PIO_PREFIX
}

/// Validate only the exact observed `SLEEP/NAP -> 0x21` PIO prefix.
pub(crate) fn validate_g17s_gfx1_low_power_to_21_pio_prefix(ops: &[G17PioOp]) -> bool {
    ops == G17S_GFX1_LOW_POWER_TO_21_PIO_PREFIX
}

/// Compatibility wrapper retained for the original, reversed direction name.
pub(crate) fn validate_g17s_gfx1_sleep_to_21_pio_prefix(ops: &[G17PioOp]) -> bool {
    validate_g17s_gfx1_21_to_sleep_pio_prefix(ops)
}

/// Compatibility wrapper retained for the original, reversed direction name.
pub(crate) fn validate_g17s_gfx1_21_to_low_power_pio_prefix(ops: &[G17PioOp]) -> bool {
    validate_g17s_gfx1_low_power_to_21_pio_prefix(ops)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17SFirmwareOp {
    OpaqueHelperCall {
        helper_text_offset: u32,
        object_global_offset: u32,
    },
    AtomicAdd32 {
        global_offset: u32,
        addend: i32,
    },
    RejectBothNonZero32 {
        root_global_offset: u32,
        pointer_offset: u32,
        field_offset: u32,
        global_offset: u32,
    },
    AtomicSetBits64 {
        global_offset: u32,
        mask: u64,
    },
}

/// Ordered G17S operations after the `0x21 -> SLEEP` PIO writes and before the
/// type-0x24 idle poll.
pub(crate) const G17S_GFX1_PRE_IDLE_FIRMWARE_OPS: [G17SFirmwareOp; 6] = [
    G17SFirmwareOp::OpaqueHelperCall {
        helper_text_offset: 0x0003_b1d8,
        object_global_offset: 0x0000_0df0,
    },
    G17SFirmwareOp::AtomicAdd32 {
        global_offset: 0x0000_21e0,
        addend: -1,
    },
    G17SFirmwareOp::RejectBothNonZero32 {
        root_global_offset: 0x0000_0e08,
        pointer_offset: 0x481,
        field_offset: 0x38,
        global_offset: 0x0000_2d10,
    },
    G17SFirmwareOp::AtomicSetBits64 {
        global_offset: 0x0000_11a0,
        mask: 0x0800_0000,
    },
    G17SFirmwareOp::OpaqueHelperCall {
        helper_text_offset: 0x0003_a8e8,
        object_global_offset: 0x0000_11a0,
    },
    G17SFirmwareOp::OpaqueHelperCall {
        helper_text_offset: 0x0003_b1d8,
        object_global_offset: 0x0000_0dd8,
    },
];

/// Validate only the byte-confirmed G17S pre-idle firmware-operation order.
pub(crate) fn validate_g17s_gfx1_pre_idle_firmware_ops(ops: &[G17SFirmwareOp]) -> bool {
    ops == G17S_GFX1_PRE_IDLE_FIRMWARE_OPS
}

/// G17 host targets with the byte-identical slot-0x24 relative-map record.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PioMapEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

pub(crate) const G17_PIO_SLOT_24_MAP_EVIDENCE_TARGETS: [G17PioMapEvidenceTarget; 3] = [
    G17PioMapEvidenceTarget::G17P,
    G17PioMapEvidenceTarget::G17G,
    G17PioMapEvidenceTarget::G17S,
];

/// Exact common slot-0x24 relative mapping; the complete family tables differ.
pub(crate) const G17_PIO_SLOT_24_RELATIVE_OFFSET: u32 = 0x00d4_0000;
pub(crate) const G17_PIO_SLOT_24_SIZE: u32 = 0x4000;

/// Complete ordered type list of the pinned G17S 19-record relative table.
///
/// Type 0x0b is absent; G17S installs that descriptor later in its target
/// configure override instead of obtaining it from the SGX-relative table.
pub(crate) const G17S_HOST_PIO_RELATIVE_TABLE_TYPES: [u8; 19] = [
    0x11, 0x35, 0x1f, 0x22, 0x24, 0x26, 0x27, 0x21, 0x25, 0x28, 0x2a, 0x30, 0x14, 0x15,
    0x12, 0x13, 0x18, 0x17, 0x32,
];

/// G17 targets whose physical SGX and host-created PIO bindings are pinned.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17HostPioPhysicalEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

pub(crate) const G17_HOST_PIO_PHYSICAL_EVIDENCE_TARGETS: [G17HostPioPhysicalEvidenceTarget; 3] =
    [
        G17HostPioPhysicalEvidenceTarget::G17P,
        G17HostPioPhysicalEvidenceTarget::G17G,
        G17HostPioPhysicalEvidenceTarget::G17S,
    ];

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17MetaSwInterruptEvidence {
    pub(crate) first_u64: u64,
    pub(crate) second_u64: u64,
    pub(crate) trailing_u32: u32,
}

/// One address/size pair from the Apple DeviceTree `reg` property.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AdtRegisterRange {
    pub(crate) phys_base: u64,
    pub(crate) size: u32,
}

/// Literal power-ownership property on one D93AP RTBuddy nub.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17AdtPowerManagementProperty {
    UserPowerManaged,
    PowerManaged,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PAdtTopology {
    pub(crate) sgx_regs: [G17AdtRegisterRange; 2],
    pub(crate) sgx_power_gates: [u32; 2],
    pub(crate) sgx_power_gate_parents: [Option<u32>; 2],
    pub(crate) gfx_power_domain: u32,
    pub(crate) gfx_power_domain_parents: [u32; 2],
    pub(crate) clock_id: u32,
    pub(crate) sgx_interrupts: [u32; 8],
    pub(crate) sgx_interrupts_valid: u32,
    pub(crate) gfx_asc_regs: [G17AdtRegisterRange; 2],
    pub(crate) gfx_asc_power_gate: u32,
    pub(crate) gfx_asc_power_gate_parent: u32,
    pub(crate) gfx_asc_interrupts: [u32; 4],
    pub(crate) gfx_power_management: G17AdtPowerManagementProperty,
    pub(crate) gfx1_asc_regs: [G17AdtRegisterRange; 2],
    pub(crate) gfx1_asc_power_gate: u32,
    pub(crate) gfx1_asc_power_gate_parent: u32,
    pub(crate) gfx1_asc_interrupts: [u32; 4],
    pub(crate) gfx1_power_management: G17AdtPowerManagementProperty,
    pub(crate) shared_iommu_mapper: bool,
    pub(crate) meta_sw_interrupt: G17MetaSwInterruptEvidence,
    pub(crate) perf_state_count: u32,
    pub(crate) gpu_num_perf_states: u32,
    pub(crate) perf_state_record_count: u32,
    pub(crate) perf_state_records_all_zero: bool,
    pub(crate) perf_states_sram_present: bool,
}

/// One role-local ASCWrap v6 provider decoded from exact D93AP topology.
///
/// `wrapper_regs` contains the mailbox block at
/// [`T8140_ASCWRAP_V6_MAILBOX_OFFSET`]. `iorvbar_regs` is the independent
/// eight-byte IORVBAR window and is deliberately not part of mailbox MMIO.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PAscProvider {
    pub(crate) role: T8140G17PAscRole,
    pub(crate) wrapper_regs: G17AdtRegisterRange,
    pub(crate) iorvbar_regs: G17AdtRegisterRange,
    pub(crate) send_empty_irq: u32,
    pub(crate) recv_not_empty_irq: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T8140G17PAscRole {
    Gfx,
    Gfx1,
}

/// Named PMGR device selected by one D93AP GPU power-gate ID.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T8140G17PPmgrDeviceName {
    GfxSgx,
    GfxBusy,
    GfxAsc,
    GfxAsc1,
}

/// Exact PMGR membership for one D93AP GPU power-gate ID.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PPmgrGate {
    pub(crate) device_id: u32,
    pub(crate) name: T8140G17PPmgrDeviceName,
    pub(crate) flags: u32,
    pub(crate) index: u32,
    pub(crate) alias: Option<u32>,
}

/// D93AP's SGX and two ASC nubs select these named `pmgr1,t8140` devices.
/// This proves membership only; it does not prove a Linux enable sequence or
/// whether RTBuddy firmware retains ownership of any gate.
pub(crate) const D93AP_T8140_G17P_PMGR_GATES: [T8140G17PPmgrGate; 4] = [
    T8140G17PPmgrGate {
        device_id: 235,
        name: T8140G17PPmgrDeviceName::GfxSgx,
        flags: 16,
        index: 0,
        alias: Some(40),
    },
    T8140G17PPmgrGate {
        device_id: 236,
        name: T8140G17PPmgrDeviceName::GfxBusy,
        flags: 16,
        index: 0,
        alias: None,
    },
    T8140G17PPmgrGate {
        device_id: 237,
        name: T8140G17PPmgrDeviceName::GfxAsc,
        flags: 16,
        index: 0,
        alias: Some(40),
    },
    T8140G17PPmgrGate {
        device_id: 321,
        name: T8140G17PPmgrDeviceName::GfxAsc1,
        flags: 16,
        index: 0,
        alias: Some(40),
    },
];

pub(crate) const T8140_G17P_PMGR_GATE_TOPOLOGY_PROVEN: bool = true;
pub(crate) const T8140_G17P_LIVE_POWER_SEQUENCE_PROVEN: bool = false;

/// ASCWrap v6 retains the ASC mailbox-v4 register ABI at wrapper + 0x8000.
pub(crate) const T8140_ASCWRAP_V6_MAILBOX_OFFSET: u32 = 0x8000;

pub(crate) const D93AP_T8140_G17P_ASC_PROVIDERS: [T8140G17PAscProvider; 2] = [
    T8140G17PAscProvider {
        role: T8140G17PAscRole::Gfx,
        wrapper_regs: G17AdtRegisterRange {
            phys_base: 0x2_7260_0000,
            size: 0x0008_8000,
        },
        iorvbar_regs: G17AdtRegisterRange {
            phys_base: 0x2_7205_0000,
            size: 8,
        },
        send_empty_irq: 993,
        recv_not_empty_irq: 996,
    },
    T8140G17PAscProvider {
        role: T8140G17PAscRole::Gfx1,
        wrapper_regs: G17AdtRegisterRange {
            phys_base: 0x2_72e0_0000,
            size: 0x0008_8000,
        },
        iorvbar_regs: G17AdtRegisterRange {
            phys_base: 0x2_7285_0000,
            size: 8,
        },
        send_empty_irq: 997,
        recv_not_empty_irq: 1000,
    },
];

pub(crate) const D93AP_T8140_G17P_ADT_TOPOLOGY: T8140G17PAdtTopology = T8140G17PAdtTopology {
    sgx_regs: [
        G17AdtRegisterRange {
            phys_base: 0x2_7000_0000,
            size: 0x0400_0000,
        },
        G17AdtRegisterRange {
            phys_base: 0x2_70d0_0000,
            size: 0x0012_c000,
        },
    ],
    sgx_power_gates: [235, 236],
    sgx_power_gate_parents: [Some(40), None],
    gfx_power_domain: 40,
    gfx_power_domain_parents: [29, 31],
    clock_id: 351,
    sgx_interrupts: [975, 976, 977, 978, 1001, 1003, 990, 992],
    sgx_interrupts_valid: 0xdf,
    gfx_asc_regs: [
        G17AdtRegisterRange {
            phys_base: 0x2_7260_0000,
            size: 0x0008_8000,
        },
        G17AdtRegisterRange {
            phys_base: 0x2_7205_0000,
            size: 8,
        },
    ],
    gfx_asc_power_gate: 237,
    gfx_asc_power_gate_parent: 40,
    gfx_asc_interrupts: [994, 993, 996, 995],
    gfx_power_management: G17AdtPowerManagementProperty::UserPowerManaged,
    gfx1_asc_regs: [
        G17AdtRegisterRange {
            phys_base: 0x2_72e0_0000,
            size: 0x0008_8000,
        },
        G17AdtRegisterRange {
            phys_base: 0x2_7285_0000,
            size: 8,
        },
    ],
    gfx1_asc_power_gate: 321,
    gfx1_asc_power_gate_parent: 40,
    gfx1_asc_interrupts: [998, 997, 1000, 999],
    gfx1_power_management: G17AdtPowerManagementProperty::PowerManaged,
    shared_iommu_mapper: true,
    meta_sw_interrupt: G17MetaSwInterruptEvidence {
        first_u64: 0x3_0101_4048,
        second_u64: 0x3_0101_4248,
        trailing_u32: 0x4000,
    },
    perf_state_count: 0,
    gpu_num_perf_states: 2,
    perf_state_record_count: 16,
    perf_state_records_all_zero: true,
    perf_states_sram_present: false,
};

/// Do not begin A18 Pro live bring-up against an inferred or partial topology.
pub(crate) fn validate_t8140_g17p_adt_topology(observed: &T8140G17PAdtTopology) -> bool {
    observed == &D93AP_T8140_G17P_ADT_TOPOLOGY
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum T8140G17PSgxHostAction {
    FirmwareEventZero,
    GpcHostInterrupts,
    DrainFirmwareRings,
    McwInterrupt,
    AcceleratorVirtualC60,
}

/// Physical D93AP interrupt selected by one host event-source index.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PSgxHostIrqRoute {
    pub(crate) source_index: u8,
    pub(crate) aic_irq: u32,
    pub(crate) action: T8140G17PSgxHostAction,
}

pub(crate) const D93AP_T8140_G17P_SGX_HOST_IRQ_ROUTES: [T8140G17PSgxHostIrqRoute; 7] = [
    T8140G17PSgxHostIrqRoute {
        source_index: 0,
        aic_irq: 975,
        action: T8140G17PSgxHostAction::FirmwareEventZero,
    },
    T8140G17PSgxHostIrqRoute {
        source_index: 1,
        aic_irq: 976,
        action: T8140G17PSgxHostAction::GpcHostInterrupts,
    },
    T8140G17PSgxHostIrqRoute {
        source_index: 2,
        aic_irq: 977,
        action: T8140G17PSgxHostAction::GpcHostInterrupts,
    },
    T8140G17PSgxHostIrqRoute {
        source_index: 3,
        aic_irq: 978,
        action: T8140G17PSgxHostAction::GpcHostInterrupts,
    },
    T8140G17PSgxHostIrqRoute {
        source_index: 4,
        aic_irq: 1001,
        action: T8140G17PSgxHostAction::DrainFirmwareRings,
    },
    T8140G17PSgxHostIrqRoute {
        source_index: 6,
        aic_irq: 990,
        action: T8140G17PSgxHostAction::McwInterrupt,
    },
    T8140G17PSgxHostIrqRoute {
        source_index: 7,
        aic_irq: 992,
        action: T8140G17PSgxHostAction::AcceleratorVirtualC60,
    },
];

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PFirmwareEventIngress {
    pub(crate) physical_aic_irq: u32,
    pub(crate) source_index: u8,
    pub(crate) rtbuddy_akf_message_type: u8,
}

pub(crate) const D93AP_T8140_G17P_FIRMWARE_EVENT_INGRESS: T8140G17PFirmwareEventIngress =
    T8140G17PFirmwareEventIngress {
        physical_aic_irq: 1001,
        source_index: 4,
        rtbuddy_akf_message_type: 2,
    };

/// The platform event-source and firmware-ring ingress mapping is exact; the
/// live SKSM completion handshake still must remain fail-closed.
pub(crate) const T8140_G17P_SGX_EVENT_SOURCE_MAP_PROVEN: bool = true;
pub(crate) const T8140_G17P_FIRMWARE_EVENT_INGRESS_PROVEN: bool = true;
pub(crate) const T8140_G17P_SKSM_COMPLETION_IRQ_PROVEN: bool = false;

/// Resolve only a valid interrupt from an exact D93AP topology.
///
/// Raw slot 5 contains IRQ 1003 but bit 5 is clear in `interrupts-valid`, so
/// it is deliberately rejected rather than exposed as a Linux resource.
pub(crate) fn decode_t8140_g17p_sgx_host_irq_route(
    observed: &T8140G17PAdtTopology,
    source_index: u8,
) -> Option<T8140G17PSgxHostIrqRoute> {
    if !validate_t8140_g17p_adt_topology(observed) || source_index >= 8 {
        return None;
    }

    let valid = (observed.sgx_interrupts_valid & (1u32 << u32::from(source_index))) != 0;
    if !valid {
        return None;
    }

    D93AP_T8140_G17P_SGX_HOST_IRQ_ROUTES
        .iter()
        .copied()
        .find(|route| route.source_index == source_index)
}

pub(crate) fn decode_t8140_g17p_asc_providers(
    observed: &T8140G17PAdtTopology,
) -> Option<[T8140G17PAscProvider; 2]> {
    if !validate_t8140_g17p_adt_topology(observed) {
        return None;
    }

    Some([
        T8140G17PAscProvider {
            role: T8140G17PAscRole::Gfx,
            wrapper_regs: observed.gfx_asc_regs[0],
            iorvbar_regs: observed.gfx_asc_regs[1],
            send_empty_irq: observed.gfx_asc_interrupts[1],
            recv_not_empty_irq: observed.gfx_asc_interrupts[2],
        },
        T8140G17PAscProvider {
            role: T8140G17PAscRole::Gfx1,
            wrapper_regs: observed.gfx1_asc_regs[0],
            iorvbar_regs: observed.gfx1_asc_regs[1],
            send_empty_irq: observed.gfx1_asc_interrupts[1],
            recv_not_empty_irq: observed.gfx1_asc_interrupts[2],
        },
    ])
}

// -------------------------------------------------------------------------
// T8140 topology as a *Linux* device tree observes it.
//
// The pinned D93AP topology above records ADT `reg` values, which are
// arm-io child-bus addresses. The J700 Linux device tree (and the live J700
// bring-up) shows the same devices at CPU physical addresses shifted by the
// arm-io `ranges` translation: SGX child 0x2_7000_0000 <-> CPU 0x4_8000_0000,
// GFX ASC child 0x2_7260_0000 <-> CPU 0x4_8260_0000. A Linux platform
// decoder therefore observes CPU addresses and must translate them back
// before they can be compared against the pinned child-bus constants.
// -------------------------------------------------------------------------

/// The J700/T8140 arm-io `ranges` translation from ADT child-bus addresses to
/// CPU physical addresses (CPU = child + offset). Verified on the live J700
/// (Mac17,5) against both the SGX and both ASC wrapper windows.
pub(crate) const T8140_ARM_IO_CPU_TRANSLATION_OFFSET: u64 = 0x2_1000_0000;

/// One ASCWrap v6 provider as observed from a Linux device tree: the
/// mailbox node's two `reg` windows (CPU physical) and its two named
/// interrupts. `role_is_gfx1` comes from the node's `apple,firmware-role`.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PLinuxDtAscObservation {
    pub(crate) role_is_gfx1: bool,
    pub(crate) wrapper_cpu_base: u64,
    pub(crate) wrapper_size: u64,
    pub(crate) iorvbar_cpu_base: u64,
    pub(crate) iorvbar_size: u64,
    pub(crate) send_empty_irq: u32,
    pub(crate) recv_not_empty_irq: u32,
}

/// The dual-provider observation a Linux platform decoder can actually make
/// from the T8140 GPU node's `mboxes` at probe time.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct T8140G17PLinuxDtTopology {
    pub(crate) gfx: T8140G17PLinuxDtAscObservation,
    pub(crate) gfx1: T8140G17PLinuxDtAscObservation,
}

fn linux_dt_asc_provider(
    role: T8140G17PAscRole,
    observed: &T8140G17PLinuxDtAscObservation,
) -> Option<T8140G17PAscProvider> {
    let expected_gfx1 = matches!(role, T8140G17PAscRole::Gfx1);
    if observed.role_is_gfx1 != expected_gfx1 {
        return None;
    }

    Some(T8140G17PAscProvider {
        role,
        wrapper_regs: G17AdtRegisterRange {
            phys_base: observed
                .wrapper_cpu_base
                .checked_sub(T8140_ARM_IO_CPU_TRANSLATION_OFFSET)?,
            size: observed.wrapper_size.try_into().ok()?,
        },
        iorvbar_regs: G17AdtRegisterRange {
            phys_base: observed
                .iorvbar_cpu_base
                .checked_sub(T8140_ARM_IO_CPU_TRANSLATION_OFFSET)?,
            size: observed.iorvbar_size.try_into().ok()?,
        },
        send_empty_irq: observed.send_empty_irq,
        recv_not_empty_irq: observed.recv_not_empty_irq,
    })
}

/// Decode the two mailbox providers from a Linux DT observation.
///
/// This only reshapes and bus-translates what the device tree actually says;
/// it deliberately performs no comparison against the pinned D93AP constants.
/// Admission stays with the caller (`g17_power_preflight`), which compares
/// the decoded providers against the pinned expectation, so an inferred or
/// partial observation can never substitute for the exact one.
pub(crate) fn decode_t8140_g17p_asc_providers_from_linux_dt(
    observed: &T8140G17PLinuxDtTopology,
) -> Option<[T8140G17PAscProvider; 2]> {
    Some([
        linux_dt_asc_provider(T8140G17PAscRole::Gfx, &observed.gfx)?,
        linux_dt_asc_provider(T8140G17PAscRole::Gfx1, &observed.gfx1)?,
    ])
}

/// Source from which the pinned host constructs one physical PIO binding.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17HostPioBindingSource {
    MetaSwInterrupt,
    SgxRelativeOffsetTable,
    TargetConfigureConstant,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17HostPioPhysicalBinding {
    pub(crate) phys_base: u64,
    pub(crate) size: u32,
    pub(crate) source: G17HostPioBindingSource,
}

/// Exact absolute PIO assignments made by a target configure override.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17HostPioTargetConfigureEvidence {
    pub(crate) slot_0_phys_base: u64,
    pub(crate) slot_0b_phys_base: u64,
    pub(crate) slot_size: u32,
}

/// Exact target input used to derive the pinned host's physical PIO map.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17HostPioPhysicalEvidence {
    pub(crate) sgx_phys_base: u64,
    pub(crate) host_io_page_size: u32,
    pub(crate) meta_sw_interrupt: G17MetaSwInterruptEvidence,
    pub(crate) target_configure: Option<G17HostPioTargetConfigureEvidence>,
}

pub(crate) const G17_HOST_PIO_LINUX_MMIO_OWNERSHIP_PROVEN: bool = false;

pub(crate) const fn g17_host_pio_physical_evidence(
    target: G17HostPioPhysicalEvidenceTarget,
) -> G17HostPioPhysicalEvidence {
    match target {
        G17HostPioPhysicalEvidenceTarget::G17P => G17HostPioPhysicalEvidence {
            sgx_phys_base: D93AP_T8140_G17P_ADT_TOPOLOGY.sgx_regs[0].phys_base,
            host_io_page_size: 0x4000,
            meta_sw_interrupt: D93AP_T8140_G17P_ADT_TOPOLOGY.meta_sw_interrupt,
            target_configure: None,
        },
        G17HostPioPhysicalEvidenceTarget::G17G => G17HostPioPhysicalEvidence {
            sgx_phys_base: 0x3_7000_0000,
            host_io_page_size: 0x4000,
            meta_sw_interrupt: G17MetaSwInterruptEvidence {
                first_u64: 0x3_8101_4048,
                second_u64: 0x3_8101_4248,
                trailing_u32: 0x4000,
            },
            target_configure: None,
        },
        G17HostPioPhysicalEvidenceTarget::G17S => G17HostPioPhysicalEvidence {
            sgx_phys_base: G17S_SGX_PHYS_BASE,
            host_io_page_size: 0x4000,
            meta_sw_interrupt: G17MetaSwInterruptEvidence {
                first_u64: 0x2_8041_4084,
                second_u64: 0x2_8041_4284,
                trailing_u32: 0x80,
            },
            target_configure: Some(G17HostPioTargetConfigureEvidence {
                slot_0_phys_base: 0x2_8041_4000,
                slot_0b_phys_base: 0x2_8425_8000,
                slot_size: 0x4000,
            }),
        },
    }
}

/// Derive only the physical bindings established by the pinned target host.
///
/// G17G/G17P type 0 comes from `meta-sw-interrupt`; the G17S generic path does
/// the same, then its target override reasserts the same aligned address as an
/// absolute constant. Type 0x24 comes from the target SGX base plus its exact
/// relative-table record. Type 0x0b remains `None` on G17G/G17P, but the G17S
/// target override installs its exact absolute address after the 19-record
/// relative table omits it.
pub(crate) const fn g17_host_pio_physical_binding(
    target: G17HostPioPhysicalEvidenceTarget,
    slot: G17PioSlot,
) -> Option<G17HostPioPhysicalBinding> {
    let evidence = g17_host_pio_physical_evidence(target);

    match slot {
        G17PioSlot::GpcAicSwInt => match evidence.target_configure {
            Some(target_configure) => Some(G17HostPioPhysicalBinding {
                phys_base: target_configure.slot_0_phys_base,
                size: target_configure.slot_size,
                source: G17HostPioBindingSource::TargetConfigureConstant,
            }),
            None => Some(G17HostPioPhysicalBinding {
                phys_base: evidence.meta_sw_interrupt.first_u64
                    & !((evidence.host_io_page_size as u64) - 1),
                size: evidence.host_io_page_size,
                source: G17HostPioBindingSource::MetaSwInterrupt,
            }),
        },
        G17PioSlot::PushTelemetryDashboardConfigSpace => match evidence.target_configure {
            Some(target_configure) => Some(G17HostPioPhysicalBinding {
                phys_base: target_configure.slot_0b_phys_base,
                size: target_configure.slot_size,
                source: G17HostPioBindingSource::TargetConfigureConstant,
            }),
            None => None,
        },
        G17PioSlot::FenderRegsMcw => Some(G17HostPioPhysicalBinding {
            phys_base: evidence.sgx_phys_base + G17_PIO_SLOT_24_RELATIVE_OFFSET as u64,
            size: G17_PIO_SLOT_24_SIZE,
            source: G17HostPioBindingSource::SgxRelativeOffsetTable,
        }),
    }
}

pub(crate) fn validate_g17_host_pio_physical_binding(
    target: G17HostPioPhysicalEvidenceTarget,
    slot: G17PioSlot,
    binding: Option<G17HostPioPhysicalBinding>,
) -> bool {
    binding == g17_host_pio_physical_binding(target, slot)
}

pub(crate) const G17S_SGX_PHYS_BASE: u64 = 0x21_0000_0000;
pub(crate) const G17S_GFX1_IDLE_STATUS_PHYS: u64 =
    G17S_SGX_PHYS_BASE + G17_PIO_SLOT_24_RELATIVE_OFFSET as u64;

/// Exact common G17 GFX1 idle-poll fields.
pub(crate) const G17_GFX1_IDLE_POLL_MASK: u32 = 1;
pub(crate) const G17_GFX1_IDLE_POLL_EXPECTED: u32 = 1;
pub(crate) const G17_GFX1_IDLE_POLL_TIMEOUT_TICKS: u32 = 0x1770;
/// Maximum printed by the pinned firmware for the `0x1770`-tick deadline.
pub(crate) const G17_GFX1_IDLE_POLL_REPORTED_MAX_US: u32 = 250;

pub(crate) const G17S_GFX1_IDLE_POLL_MASK: u32 = G17_GFX1_IDLE_POLL_MASK;
pub(crate) const G17S_GFX1_IDLE_POLL_EXPECTED: u32 = G17_GFX1_IDLE_POLL_EXPECTED;
pub(crate) const G17S_GFX1_IDLE_POLL_TIMEOUT_TICKS: u32 = G17_GFX1_IDLE_POLL_TIMEOUT_TICKS;
pub(crate) const G17S_GFX1_IDLE_POLL_REPORTED_MAX_US: u32 = G17_GFX1_IDLE_POLL_REPORTED_MAX_US;

pub(crate) const fn g17_gfx1_report_timebase_delta_us(delta_ticks: u32) -> u32 {
    ((delta_ticks as u64 * 0xaaaa_aaab) >> 36) as u32
}

pub(crate) const fn g17s_gfx1_report_timebase_delta_us(delta_ticks: u32) -> u32 {
    g17_gfx1_report_timebase_delta_us(delta_ticks)
}

/// Test the exact success predicate used by the common G17 GFX1 idle poll.
pub(crate) const fn g17_gfx1_idle_poll_satisfied(
    _target: G17Gfx1PowerEvidenceTarget,
    status: u32,
) -> bool {
    status & G17_GFX1_IDLE_POLL_MASK == G17_GFX1_IDLE_POLL_EXPECTED
}

pub(crate) const fn g17s_gfx1_idle_poll_satisfied(status: u32) -> bool {
    g17_gfx1_idle_poll_satisfied(G17Gfx1PowerEvidenceTarget::G17S, status)
}

/// Fail-closed result for the bounded G17 GFX1 transition grammar.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum PowerTransitionError {
    /// Direct SLEEP-to-NAP and NAP-to-SLEEP paths reach firmware aborts.
    DirectSleepNapIsFatal,
    /// No other edge was established by the pinned transition callback.
    UnprovenTransition,
}

pub(crate) const fn validate_g17_gfx1_power_transition(
    _target: G17Gfx1PowerEvidenceTarget,
    old: u8,
    new: u8,
) -> Result<(), PowerTransitionError> {
    if (old == GFX1_POWER_SLEEP && new == GFX1_POWER_NAP)
        || (old == GFX1_POWER_NAP && new == GFX1_POWER_SLEEP)
    {
        return Err(PowerTransitionError::DirectSleepNapIsFatal);
    }

    if ((old == GFX1_POWER_SLEEP || old == GFX1_POWER_NAP) && new == GFX1_POWER_INTERMEDIATE_21)
        || (old == GFX1_POWER_INTERMEDIATE_21 && (new == GFX1_POWER_SLEEP || new == GFX1_POWER_NAP))
    {
        Ok(())
    } else {
        Err(PowerTransitionError::UnprovenTransition)
    }
}

pub(crate) const fn validate_g17s_gfx1_power_transition(
    old: u8,
    new: u8,
) -> Result<(), PowerTransitionError> {
    validate_g17_gfx1_power_transition(G17Gfx1PowerEvidenceTarget::G17S, old, new)
}

/// Target-specific locations independently establishing the callback-to-ACK
/// prefix in each pinned G17 GFX1 image.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17Gfx1CallbackAckEvidence {
    pub(crate) worker_text_offset: u32,
    pub(crate) release_helper_text_offset: u32,
    pub(crate) payload_mapper_text_offset: u32,
    pub(crate) sender_text_offset: u32,
}

pub(crate) const fn g17_gfx1_callback_ack_evidence(
    target: G17Gfx1PowerEvidenceTarget,
) -> G17Gfx1CallbackAckEvidence {
    match target {
        G17Gfx1PowerEvidenceTarget::G17P => G17Gfx1CallbackAckEvidence {
            worker_text_offset: 0x0003_b030,
            release_helper_text_offset: 0x0003_5484,
            payload_mapper_text_offset: 0x0003_8dc8,
            sender_text_offset: 0x0003_dc00,
        },
        G17Gfx1PowerEvidenceTarget::G17G => G17Gfx1CallbackAckEvidence {
            worker_text_offset: 0x0003_ae34,
            release_helper_text_offset: 0x0003_52e8,
            payload_mapper_text_offset: 0x0003_8bdc,
            sender_text_offset: 0x0003_da48,
        },
        G17Gfx1PowerEvidenceTarget::G17S => G17Gfx1CallbackAckEvidence {
            worker_text_offset: 0x0004_0258,
            release_helper_text_offset: 0x0003_a70c,
            payload_mapper_text_offset: 0x0003_e000,
            sender_text_offset: 0x0004_2f00,
        },
    }
}

/// Exact ordered prefix from one firmware power callback through its ACK.
///
/// Optional callbacks are skipped when their pointers are null. The release
/// helper's CASL body is exact, but its matching acquisition and ownership
/// contract are not. The worker can also run a target-state hook after the ACK;
/// neither that hook nor any later worker action is represented here.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17Gfx1CallbackAckStep {
    InvokeOptionalPreCallback,
    InvokeRegisteredCallbacks,
    InvokeOptionalPostCallback,
    ReleaseFirmwareLocalObjectCasl,
    MapCompletionPayload,
    SendType0bCompletionMessage,
}

pub(crate) const G17_GFX1_CALLBACK_ACK_PREFIX: [G17Gfx1CallbackAckStep; 6] = [
    G17Gfx1CallbackAckStep::InvokeOptionalPreCallback,
    G17Gfx1CallbackAckStep::InvokeRegisteredCallbacks,
    G17Gfx1CallbackAckStep::InvokeOptionalPostCallback,
    G17Gfx1CallbackAckStep::ReleaseFirmwareLocalObjectCasl,
    G17Gfx1CallbackAckStep::MapCompletionPayload,
    G17Gfx1CallbackAckStep::SendType0bCompletionMessage,
];

/// High message-type bits ORed with the mapped payload before the send.
pub(crate) const G17_GFX1_CALLBACK_ACK_MESSAGE_TYPE_BITS: u64 = 0x00b0_0000_0000_0000;
/// The only sender status for which the worker retries the ACK.
pub(crate) const G17_GFX1_CALLBACK_ACK_RETRY_STATUS: u32 = 0x8000_000e;

pub(crate) fn validate_g17_gfx1_callback_ack_prefix(
    _target: G17Gfx1PowerEvidenceTarget,
    steps: &[G17Gfx1CallbackAckStep],
) -> bool {
    steps == G17_GFX1_CALLBACK_ACK_PREFIX
}

/// Fail-closed results from the common internal-state-to-payload mapper.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17Gfx1CallbackAckError {
    /// Internal state 3 branches to a firmware abort instead of returning.
    FatalInternalState,
    /// The firmware's default is zero, but Linux must not accept an unknown.
    UnprovenInternalState,
}

/// Decode only independently established internal states and table entries.
///
/// The pinned mapper returns zero for other numeric inputs. Rejecting that
/// default prevents an unknown state from becoming an apparently valid ACK.
pub(crate) const fn g17_gfx1_callback_ack_payload(
    _target: G17Gfx1PowerEvidenceTarget,
    internal_state: u8,
) -> Result<u16, G17Gfx1CallbackAckError> {
    match internal_state {
        3 => Err(G17Gfx1CallbackAckError::FatalInternalState),
        4 => Ok(3),
        5 => Ok(0x203),
        6 | 7 => Ok(0),
        GFX1_POWER_SLEEP => Ok(0x201),
        GFX1_POWER_NAP => Ok(0),
        0x20 => Ok(0x10),
        GFX1_POWER_INTERMEDIATE_21 => Ok(0x20),
        _ => Err(G17Gfx1CallbackAckError::UnprovenInternalState),
    }
}

/// Host-side management-message routes established in all three pinned
/// generic RTBuddy images.
///
/// Despite the firmware-side callback-ACK terminology above, type 0x0b is
/// not the host's type-7 power ACK. The two messages wake different fields
/// belonging to different objects.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17HostManagementMessageRoute {
    /// `_handlePowerAck()` updates and wakes `RTBuddy + 0x158`.
    IopStatusPowerAck,
    /// The handler stores the low word and wakes management endpoint +0x108.
    Gfx1CallbackRemoteState,
}

/// Object containing the field used as the matching sleep/wakeup event.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17HostManagementWakeObject {
    RtBuddy,
    ManagementEndpoint,
}

/// Fail-closed result for an incoming management-message type not covered by
/// this bounded power-route model.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17HostManagementMessageError {
    UnprovenMessageType,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17HostManagementRouteEvidence {
    pub(crate) management_handler_text_offset: u32,
    pub(crate) type0b_remote_state_branch_text_offset: u32,
    pub(crate) wait_remote_state_text_offset: u32,
    pub(crate) handle_power_ack_text_offset: u32,
    pub(crate) perform_power_change_text_offset: u32,
    pub(crate) set_iop_status_text_offset: u32,
}

pub(crate) const G17_HOST_MANAGEMENT_MESSAGE_TYPE_SHIFT: u32 = 52;
pub(crate) const G17_HOST_MANAGEMENT_MESSAGE_TYPE_MASK: u64 = 0xf;
pub(crate) const G17_HOST_POWER_ACK_MESSAGE_TYPE: u8 = 0x07;
pub(crate) const G17_HOST_GFX1_CALLBACK_MESSAGE_TYPE: u8 = 0x0b;
pub(crate) const G17_HOST_MANAGEMENT_REMOTE_STATE_WORD_OFFSET: u32 = 0x108;
pub(crate) const G17_HOST_RTBUDDY_IOP_STATUS_WORD_OFFSET: u32 = 0x158;

/// Target-specific GFX1 text offsets proving the callback endpoint dataflow.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17Gfx1CallbackEndpointEvidence {
    pub(crate) endpoint_request_store_text_offset: u32,
    pub(crate) endpoint_allocator_call_text_offset: u32,
    pub(crate) endpoint_allocator_sentinel_compare_text_offset: u32,
    pub(crate) endpoint_allocator_dynamic_store_text_offset: u32,
    pub(crate) endpoint_result_load_text_offset: u32,
    pub(crate) endpoint_handle_shift_text_offset: u32,
    pub(crate) callback_endpoint_store_text_offset: u32,
    pub(crate) callback_registration_load_text_offset: u32,
    pub(crate) callback_registration_context_text_offset: u32,
    pub(crate) callback_handler_text_offset: u32,
    pub(crate) callback_context_store_text_offset: u32,
    pub(crate) completion_context_load_text_offset: u32,
    pub(crate) completion_endpoint_load_text_offset: u32,
    pub(crate) completion_send_call_text_offset: u32,
}

pub(crate) const G17_GFX1_CALLBACK_REQUESTED_ENDPOINT_SLOT: u8 = 0;
pub(crate) const G17_GFX1_CALLBACK_DYNAMIC_ENDPOINT_SENTINEL: u8 = 0xff;
pub(crate) const G17_GFX1_CALLBACK_ENDPOINT_HANDLE_SHIFT: u32 = 16;
pub(crate) const G17_GFX1_CALLBACK_ENDPOINT_FIELD_OFFSET: u32 = 8;
pub(crate) const G17_GFX1_CALLBACK_CONTEXT_FIELD_OFFSET: u32 = 0x40;

/// The exact constructors request endpoint slot 0, not the allocator's 0xff
/// dynamic-slot sentinel. The successful path therefore retains zero, shifts
/// it by 16, stores it at callback-object +8, registers the same object as the
/// handler context, and later reloads that zero for the completion sender.
pub(crate) const G17_GFX1_CALLBACK_MANAGEMENT_ENDPOINT_LINK_PROVEN: bool = true;

pub(crate) const fn g17_gfx1_callback_endpoint_evidence(
    target: G17Gfx1PowerEvidenceTarget,
) -> G17Gfx1CallbackEndpointEvidence {
    match target {
        G17Gfx1PowerEvidenceTarget::G17P => G17Gfx1CallbackEndpointEvidence {
            endpoint_request_store_text_offset: 0x0003_2d7c,
            endpoint_allocator_call_text_offset: 0x0003_2d80,
            endpoint_allocator_sentinel_compare_text_offset: 0x0003_6c70,
            endpoint_allocator_dynamic_store_text_offset: 0x0003_6cc4,
            endpoint_result_load_text_offset: 0x0003_2da4,
            endpoint_handle_shift_text_offset: 0x0003_2e84,
            callback_endpoint_store_text_offset: 0x0003_2ea0,
            callback_registration_load_text_offset: 0x0003_2f20,
            callback_registration_context_text_offset: 0x0003_e088,
            callback_handler_text_offset: 0x0003_89e4,
            callback_context_store_text_offset: 0x0003_8af0,
            completion_context_load_text_offset: 0x0003_b184,
            completion_endpoint_load_text_offset: 0x0003_b198,
            completion_send_call_text_offset: 0x0003_b1a0,
        },
        G17Gfx1PowerEvidenceTarget::G17G => G17Gfx1CallbackEndpointEvidence {
            endpoint_request_store_text_offset: 0x0003_2be8,
            endpoint_allocator_call_text_offset: 0x0003_2bec,
            endpoint_allocator_sentinel_compare_text_offset: 0x0003_6a84,
            endpoint_allocator_dynamic_store_text_offset: 0x0003_6ad8,
            endpoint_result_load_text_offset: 0x0003_2c10,
            endpoint_handle_shift_text_offset: 0x0003_2cec,
            callback_endpoint_store_text_offset: 0x0003_2d08,
            callback_registration_load_text_offset: 0x0003_2d88,
            callback_registration_context_text_offset: 0x0003_df28,
            callback_handler_text_offset: 0x0003_87f8,
            callback_context_store_text_offset: 0x0003_8904,
            completion_context_load_text_offset: 0x0003_af88,
            completion_endpoint_load_text_offset: 0x0003_af9c,
            completion_send_call_text_offset: 0x0003_afa4,
        },
        G17Gfx1PowerEvidenceTarget::G17S => G17Gfx1CallbackEndpointEvidence {
            endpoint_request_store_text_offset: 0x0003_800c,
            endpoint_allocator_call_text_offset: 0x0003_8010,
            endpoint_allocator_sentinel_compare_text_offset: 0x0003_bea8,
            endpoint_allocator_dynamic_store_text_offset: 0x0003_befc,
            endpoint_result_load_text_offset: 0x0003_8034,
            endpoint_handle_shift_text_offset: 0x0003_8110,
            callback_endpoint_store_text_offset: 0x0003_812c,
            callback_registration_load_text_offset: 0x0003_81ac,
            callback_registration_context_text_offset: 0x0004_33c0,
            callback_handler_text_offset: 0x0003_dc1c,
            callback_context_store_text_offset: 0x0003_dd28,
            completion_context_load_text_offset: 0x0004_03ac,
            completion_endpoint_load_text_offset: 0x0004_03c0,
            completion_send_call_text_offset: 0x0004_03c8,
        },
    }
}

/// Validate the complete static value chain without registering an endpoint
/// or sending a management message.
pub(crate) const fn g17_gfx1_callback_endpoint_link_is_exact(
    _target: G17Gfx1PowerEvidenceTarget,
    requested_slot: u8,
    allocator_result_slot: u8,
    callback_endpoint_handle: u32,
    registration_endpoint_handle: u32,
    completion_endpoint_handle: u32,
) -> bool {
    requested_slot == G17_GFX1_CALLBACK_REQUESTED_ENDPOINT_SLOT
        && requested_slot != G17_GFX1_CALLBACK_DYNAMIC_ENDPOINT_SENTINEL
        && allocator_result_slot == requested_slot
        && callback_endpoint_handle
            == (allocator_result_slot as u32) << G17_GFX1_CALLBACK_ENDPOINT_HANDLE_SHIFT
        && registration_endpoint_handle == callback_endpoint_handle
        && completion_endpoint_handle == callback_endpoint_handle
}

pub(crate) const fn g17_host_management_route_evidence(
    target: G17Gfx1PowerEvidenceTarget,
) -> G17HostManagementRouteEvidence {
    match target {
        G17Gfx1PowerEvidenceTarget::G17P
        | G17Gfx1PowerEvidenceTarget::G17G
        | G17Gfx1PowerEvidenceTarget::G17S => G17HostManagementRouteEvidence {
            management_handler_text_offset: 0x0002_cda4,
            type0b_remote_state_branch_text_offset: 0x0002_ce90,
            wait_remote_state_text_offset: 0x0002_d908,
            handle_power_ack_text_offset: 0x0002_dea0,
            perform_power_change_text_offset: 0x0001_abf8,
            set_iop_status_text_offset: 0x0001_ca64,
        },
    }
}

/// Decode only the two incoming host power routes relevant to the GFX1
/// callback-completion linkage question.
pub(crate) const fn g17_host_management_message_route(
    _target: G17Gfx1PowerEvidenceTarget,
    message: u64,
) -> Result<G17HostManagementMessageRoute, G17HostManagementMessageError> {
    let message_type = ((message >> G17_HOST_MANAGEMENT_MESSAGE_TYPE_SHIFT)
        & G17_HOST_MANAGEMENT_MESSAGE_TYPE_MASK) as u8;

    match message_type {
        G17_HOST_POWER_ACK_MESSAGE_TYPE => Ok(G17HostManagementMessageRoute::IopStatusPowerAck),
        G17_HOST_GFX1_CALLBACK_MESSAGE_TYPE => {
            Ok(G17HostManagementMessageRoute::Gfx1CallbackRemoteState)
        }
        _ => Err(G17HostManagementMessageError::UnprovenMessageType),
    }
}

pub(crate) const fn g17_host_management_route_wake_field(
    route: G17HostManagementMessageRoute,
) -> (G17HostManagementWakeObject, u32) {
    match route {
        G17HostManagementMessageRoute::IopStatusPowerAck => (
            G17HostManagementWakeObject::RtBuddy,
            G17_HOST_RTBUDDY_IOP_STATUS_WORD_OFFSET,
        ),
        G17HostManagementMessageRoute::Gfx1CallbackRemoteState => (
            G17HostManagementWakeObject::ManagementEndpoint,
            G17_HOST_MANAGEMENT_REMOTE_STATE_WORD_OFFSET,
        ),
    }
}


pub(crate) const G17_REMOTE_POWER_KICK_DECODER_VA: u64 = 0xffff_fc00_0002_37e4;
/// Firmware VA of the decoder's `_RTK_abort` default ("Unrecognized ...").
pub(crate) const G17_REMOTE_POWER_KICK_ABORT_VA: u64 = 0xffff_fc00_0002_3970;
/// The only three remote-power-kick bit values the decoder accepts.
pub(crate) const G17_REMOTE_POWER_KICK_ACCEPT: [u64; 3] =
    [0x0010_0000, 0x0100_0000, 0x8000_0000];
/// The seven bit values the decoder explicitly self-panics on.
pub(crate) const G17_REMOTE_POWER_KICK_FORBIDDEN: [u64; 7] = [
    0x40,
    0x80,
    0x100,
    0x200,
    0x2_0000_0000,
    0x20_0000_0000,
    0x100_0000_0000,
];
/// Recognized scheduler power-event bits (`AGFASchedulerProcessPowerKick`).
pub(crate) const G17_SCHEDULER_POWER_EVENT_ACCEPT: [u64; 6] = [
    0x0010_0000,
    0x0400_0000,
    0x0800_0000,
    0x1000_0000,
    0x2000_0000,
    0x4000_0000,
];
/// Allowed power-server pending-mask bits (highest set-bit index must be < 2).
pub(crate) const G17_POWER_SERVER_KICK_MASK_ALLOWED: u32 = 0b11;

/// The scheduler-role power-kick command acceptance contract is byte-grounded.
/// This is a necessary condition for a correct kick; it does not by itself
/// make the GFX1 power sequence executable (see the module-level note).
pub(crate) const GROUNDED_POWER_KICK_COMMAND_DECODE_AVAILABLE: bool = true;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RemotePowerKickClass {
    /// One of the three accepted bits: causes a real power effect.
    Accepted,
    /// One of the seven forbidden bits: the firmware calls `_RTK_abort`.
    ForbiddenPanic,
    /// Any other value: the decoder returns via `retab` with no effect.
    IgnoredNoOp,
}

/// Mirror the exact-value equality tree at 0xfffffc00000237e4.
pub(crate) const fn classify_remote_power_kick(value: u64) -> RemotePowerKickClass {
    let mut i = 0;
    while i < G17_REMOTE_POWER_KICK_ACCEPT.len() {
        if value == G17_REMOTE_POWER_KICK_ACCEPT[i] {
            return RemotePowerKickClass::Accepted;
        }
        i += 1;
    }
    let mut j = 0;
    while j < G17_REMOTE_POWER_KICK_FORBIDDEN.len() {
        if value == G17_REMOTE_POWER_KICK_FORBIDDEN[j] {
            return RemotePowerKickClass::ForbiddenPanic;
        }
        j += 1;
    }
    RemotePowerKickClass::IgnoredNoOp
}

/// Fail-closed reasons a remote power kick must not be emitted.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum PowerKickEmitError {
    /// The value self-panics the firmware (`_RTK_abort`).
    ForbiddenPanicBit,
    /// The value is silently ignored, so emitting it is a driver bug.
    UnrecognizedNoOp,
}

/// Fail-closed: only permit emitting a remote power kick the firmware accepts.
pub(crate) const fn validate_remote_power_kick_emit(
    value: u64,
) -> Result<(), PowerKickEmitError> {
    match classify_remote_power_kick(value) {
        RemotePowerKickClass::Accepted => Ok(()),
        RemotePowerKickClass::ForbiddenPanic => Err(PowerKickEmitError::ForbiddenPanicBit),
        RemotePowerKickClass::IgnoredNoOp => Err(PowerKickEmitError::UnrecognizedNoOp),
    }
}

/// Whether a scheduler power-event bit is one the firmware recognizes.
pub(crate) const fn scheduler_power_event_recognized(event: u64) -> bool {
    let mut i = 0;
    while i < G17_SCHEDULER_POWER_EVENT_ACCEPT.len() {
        if event == G17_SCHEDULER_POWER_EVENT_ACCEPT[i] {
            return true;
        }
        i += 1;
    }
    false
}

/// Whether a power-server pending mask stays within the firmware bound
/// (no set bit at index >= 2). A zero mask has nothing to dispatch.
pub(crate) const fn power_server_kick_mask_in_bounds(mask: u32) -> bool {
    mask & !G17_POWER_SERVER_KICK_MASK_ALLOWED == 0
}

/// Exact values observed in the shared G15-G17 host recovery word.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum RecoveryWordState {
    RunningOrPostRecoveryResumed = 0,
    FirmwareStoppedForPreRecovery = 1,
    HostResumedPreRecovery = 2,
    FirmwareStoppedForPostRecovery = 3,
}

/// G15/G16 host targets with an independently pinned `+0x5190` recovery word.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G15G16RecoveryLayoutEvidenceTarget {
    G15G,
    G15S,
    G15C,
    G15D,
    G16G,
    G16S,
    G16C,
}

pub(crate) const G15_G16_RECOVERY_LAYOUT_EVIDENCE_TARGETS:
    [G15G16RecoveryLayoutEvidenceTarget; 7] = [
        G15G16RecoveryLayoutEvidenceTarget::G15G,
        G15G16RecoveryLayoutEvidenceTarget::G15S,
        G15G16RecoveryLayoutEvidenceTarget::G15C,
        G15G16RecoveryLayoutEvidenceTarget::G15D,
        G15G16RecoveryLayoutEvidenceTarget::G16G,
        G15G16RecoveryLayoutEvidenceTarget::G16S,
        G15G16RecoveryLayoutEvidenceTarget::G16C,
    ];

pub(crate) const G15_AND_G16G_RECOVERY_WORD_OFFSET: u32 = 0x5190;

pub(crate) const G16X_RECOVERY_WORD_OFFSET: u32 = 0x4ae0;

pub(crate) const fn g15_g16_recovery_word_offset(
    target: G15G16RecoveryLayoutEvidenceTarget,
) -> u32 {
    match target {
        G15G16RecoveryLayoutEvidenceTarget::G15G
        | G15G16RecoveryLayoutEvidenceTarget::G15S
        | G15G16RecoveryLayoutEvidenceTarget::G15C
        | G15G16RecoveryLayoutEvidenceTarget::G15D
        | G15G16RecoveryLayoutEvidenceTarget::G16G => G15_AND_G16G_RECOVERY_WORD_OFFSET,
        G15G16RecoveryLayoutEvidenceTarget::G16S
        | G15G16RecoveryLayoutEvidenceTarget::G16C => G16X_RECOVERY_WORD_OFFSET,
    }
}

/// G17 host targets with an independently pinned `+0x4ae0` recovery word.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17RecoveryLayoutEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

pub(crate) const G17_RECOVERY_LAYOUT_EVIDENCE_TARGETS: [G17RecoveryLayoutEvidenceTarget; 3] = [
    G17RecoveryLayoutEvidenceTarget::G17P,
    G17RecoveryLayoutEvidenceTarget::G17G,
    G17RecoveryLayoutEvidenceTarget::G17S,
];

pub(crate) const G17_RECOVERY_WORD_OFFSET: u32 = 0x4ae0;

pub(crate) const fn g17_recovery_word_offset(
    target: G17RecoveryLayoutEvidenceTarget,
) -> u32 {
    match target {
        G17RecoveryLayoutEvidenceTarget::G17P
        | G17RecoveryLayoutEvidenceTarget::G17G
        | G17RecoveryLayoutEvidenceTarget::G17S => G17_RECOVERY_WORD_OFFSET,
    }
}

/// Host-owned recovery phase whose write is directly established.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum HostResumePhase {
    PreRecovery,
    PostRecovery,
}

/// Fail-closed recovery-word validation errors.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RecoveryError {
    UnexpectedWordOffset,
    UnknownWordValue,
    FirmwareNotStoppedForPhase,
}

pub(crate) const fn decode_recovery_word(raw: u32) -> Result<RecoveryWordState, RecoveryError> {
    match raw {
        0 => Ok(RecoveryWordState::RunningOrPostRecoveryResumed),
        1 => Ok(RecoveryWordState::FirmwareStoppedForPreRecovery),
        2 => Ok(RecoveryWordState::HostResumedPreRecovery),
        3 => Ok(RecoveryWordState::FirmwareStoppedForPostRecovery),
        _ => Err(RecoveryError::UnknownWordValue),
    }
}

/// Decode a G15/G16 recovery word only at the exact target-backed location.
pub(crate) const fn decode_g15_g16_recovery_word_at_offset(
    target: G15G16RecoveryLayoutEvidenceTarget,
    observed_offset: u32,
    raw: u32,
) -> Result<RecoveryWordState, RecoveryError> {
    if observed_offset != g15_g16_recovery_word_offset(target) {
        return Err(RecoveryError::UnexpectedWordOffset);
    }

    decode_recovery_word(raw)
}

/// Decode a G17 recovery word only at the exact target-backed location.
pub(crate) const fn decode_g17_recovery_word_at_offset(
    target: G17RecoveryLayoutEvidenceTarget,
    observed_offset: u32,
    raw: u32,
) -> Result<RecoveryWordState, RecoveryError> {
    if observed_offset != g17_recovery_word_offset(target) {
        return Err(RecoveryError::UnexpectedWordOffset);
    }

    decode_recovery_word(raw)
}

/// Return the exact host value for one directly established resume edge.
///
/// This models only the two HOST-owned edges. The firmware-owned edges are
/// now byte-grounded separately (see [`firmware_recovery_word_write`]): the
/// firmware writes 1 at recovery start and 0 at recovery complete, and never
/// writes 2 or 3 — so the earlier `2 -> 3` inference is refuted. The caller
/// must observe the matching stopped value first.
pub(crate) const fn host_resume_recovery(
    phase: HostResumePhase,
    observed_raw: u32,
) -> Result<RecoveryWordState, RecoveryError> {
    let observed = match decode_recovery_word(observed_raw) {
        Ok(value) => value,
        Err(error) => return Err(error),
    };

    match (phase, observed) {
        (HostResumePhase::PreRecovery, RecoveryWordState::FirmwareStoppedForPreRecovery) => {
            Ok(RecoveryWordState::HostResumedPreRecovery)
        }
        (HostResumePhase::PostRecovery, RecoveryWordState::FirmwareStoppedForPostRecovery) => {
            Ok(RecoveryWordState::RunningOrPostRecoveryResumed)
        }
        _ => Err(RecoveryError::FirmwareNotStoppedForPhase),
    }
}

pub(crate) const HOST_RESET_FOR_RECOVERY_IS_NOOP: bool = true;

/// Recovery inspection enters through role 0/GFX and drains both role logs.
/// These role facts are specific to the dual-role G17 implementation.
pub(crate) const G17_RECOVERY_INSPECTION_INGRESS_ROLE: u8 = 0;
pub(crate) const G17_RECOVERY_LOG_ROLE_MASK: u8 = 0b11;

/// Only these two values reach recognized recovery power-cycle paths in the
/// pinned G17S GFX1 image. Their private names and wire ordering are unknown.
pub(crate) const G17S_GFX1_RECOVERY_POWER_EVENTS: [u8; 2] = [0x22, 0x23];

pub(crate) const fn validate_g17s_gfx1_recovery_power_event(event: u8) -> bool {
    event == G17S_GFX1_RECOVERY_POWER_EVENTS[0] || event == G17S_GFX1_RECOVERY_POWER_EVENTS[1]
}


/// Firmware VA of the `AGFASchedulerProcessRecovery` outer decoder.
pub(crate) const G17_RECOVERY_DISPATCH_VA: u64 = 0xffff_fc00_0002_6274;
/// Firmware VA of the "Unexpected recovery type" abort pad.
pub(crate) const G17_RECOVERY_TYPE_ABORT_VA: u64 = 0xffff_fc00_0002_647c;
/// The only three recovery-cause bits the outer decoder accepts.
pub(crate) const G17_RECOVERY_TYPE_ACCEPT: [u64; 3] =
    [0x0080_0000, 0x0100_0000, 0x0200_0000];

/// Firmware BSS pointer global holding the host-shared status struct.
pub(crate) const G17_FW_STATUS_STRUCT_PTR_GLOBAL_VA: u64 = 0xffff_fc00_0010_47b8;
/// Byte offset of the recovery word inside the host-shared status struct.
pub(crate) const G17_RECOVERY_WORD_STRUCT_OFFSET: u32 = 0xc;
/// Firmware store site that writes 1 at recovery start.
pub(crate) const G17_RECOVERY_WORD_FW_WRITE_BEGIN_VA: u64 = 0xffff_fc00_0002_6728;
/// Firmware store site that writes 0 at recovery complete.
pub(crate) const G17_RECOVERY_WORD_FW_WRITE_COMPLETE_VA: u64 = 0xffff_fc00_0002_6d58;
/// The only two values the firmware ever writes to the recovery word.
pub(crate) const G17_RECOVERY_WORD_FW_WRITTEN_VALUES: [u32; 2] = [1, 0];

/// Recovery-type acceptance and the recovery-word firmware-write side are
/// byte-grounded. This does not make the handshake executable: the cross-role
/// 0x22/0x23 delivery and the host-visible DRAM are live-only (see the note).
pub(crate) const GROUNDED_RECOVERY_WORD_FIRMWARE_WRITE_SIDE_AVAILABLE: bool = true;

/// The two firmware-driven recovery-word edges, grounded from the two stores.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum FirmwareRecoveryEdge {
    /// Recovery start: firmware writes 1 (`FirmwareStoppedForPreRecovery`).
    Begin,
    /// Recovery complete: firmware writes 0 (`RunningOrPostRecoveryResumed`).
    Complete,
}

/// The state the firmware writes to the recovery word for a given edge.
pub(crate) const fn firmware_recovery_word_write(
    edge: FirmwareRecoveryEdge,
) -> RecoveryWordState {
    match edge {
        FirmwareRecoveryEdge::Begin => RecoveryWordState::FirmwareStoppedForPreRecovery,
        FirmwareRecoveryEdge::Complete => RecoveryWordState::RunningOrPostRecoveryResumed,
    }
}

/// Whether the firmware ever writes a given raw value to the recovery word.
/// Grounded to exactly {0, 1}; a model claiming the firmware writes 2 or 3 is
/// refuted by the exhaustive store scan.
pub(crate) const fn firmware_writes_recovery_value(raw: u32) -> bool {
    raw == 0 || raw == 1
}

/// Fail-closed recovery-type validation error.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RecoveryTypeError {
    UnexpectedRecoveryType,
}

/// Fail-closed: only the three accepted recovery-cause bits avoid the
/// firmware's "Unexpected recovery type" abort.
pub(crate) const fn validate_recovery_type(raw: u64) -> Result<(), RecoveryTypeError> {
    let mut i = 0;
    while i < G17_RECOVERY_TYPE_ACCEPT.len() {
        if raw == G17_RECOVERY_TYPE_ACCEPT[i] {
            return Ok(());
        }
        i += 1;
    }
    Err(RecoveryTypeError::UnexpectedRecoveryType)
}

