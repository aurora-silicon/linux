// SPDX-License-Identifier: GPL-2.0-only OR MIT


#[cfg(not(test))]
use crate::{
    agx_power_recovery, g17_firmware, g17_initdata, g17_resources, g17_rtkit, g17_submission,
};
#[cfg(test)]
#[path = "agx_power_recovery.rs"]
mod agx_power_recovery;
#[cfg(test)]
#[path = "g17_firmware.rs"]
mod g17_firmware;
#[cfg(test)]
#[path = "g17_initdata.rs"]
mod g17_initdata;
#[cfg(test)]
#[path = "g17_resources.rs"]
mod g17_resources;
#[cfg(test)]
#[path = "g17_rtkit.rs"]
mod g17_rtkit;
#[cfg(test)]
#[path = "g17_submission.rs"]
mod g17_submission;

/// Design generation that requires the dual-role contract.
const G17_GENERATION: u32 = 17;

pub(crate) mod missing {
    pub(crate) const FIRMWARE_IDENTITY: u16 = 1 << 0;
    pub(crate) const ROLE_RESOURCES: u16 = 1 << 1;
    pub(crate) const TOPOLOGY_REGISTERS: u16 = 1 << 2;
    pub(crate) const UAT_HANDOFF: u16 = 1 << 3;
    pub(crate) const INITDATA_GRAPH: u16 = 1 << 4;
    pub(crate) const RTKIT_TRANSPORT: u16 = 1 << 5;
    pub(crate) const GFX1_POWER_SEQUENCE: u16 = 1 << 6;
    pub(crate) const RECOVERY_HANDSHAKE: u16 = 1 << 7;
    pub(crate) const SUBMISSION_TRANSPORT: u16 = 1 << 8;
}

const REQUIRED_PRE_HANDOFF_EVIDENCE: u16 = missing::FIRMWARE_IDENTITY
    | missing::ROLE_RESOURCES
    | missing::TOPOLOGY_REGISTERS
    | missing::UAT_HANDOFF
    | missing::INITDATA_GRAPH
    | missing::RTKIT_TRANSPORT
    | missing::GFX1_POWER_SEQUENCE
    | missing::RECOVERY_HANDSHAKE
    | missing::SUBMISSION_TRANSPORT;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct MissingEvidence(u16);

impl MissingEvidence {
    pub(crate) const fn bits(self) -> u16 {
        self.0
    }

    pub(crate) const fn contains(self, evidence: u16) -> bool {
        self.0 & evidence != 0
    }
}

pub(crate) const AGX3_TOPOLOGY_REGISTERS_IMPLEMENTED: bool = true;

const fn implemented_pre_handoff_evidence(
    expected_variant: Option<G17FirmwareVariant>,
    firmware: Option<&g17_firmware::G17FirmwareIdentityAdmission>,
) -> u16 {
    let mut implemented = 0;

    // Only the opaque result of atomic GFX/GFX1 byte validation may satisfy
    // firmware identity. Selecting the expected static table is insufficient,
    // and an admission for one GPU variant cannot satisfy another variant.
    if let (Some(expected), Some(admission)) = (expected_variant, firmware) {
        if expected as u8 == admission.variant() as u8 {
            implemented |= missing::FIRMWARE_IDENTITY;
        }
    }

    // These constants deliberately distinguish useful offline codecs from an
    // executable firmware ABI. When either module becomes complete, this gate
    // changes with the implementation rather than a disconnected probe flag.
    if AGX3_TOPOLOGY_REGISTERS_IMPLEMENTED {
        implemented |= missing::TOPOLOGY_REGISTERS;
    }
    // The old M5 role model below remains diagnostic. T8140 has its own
    // source-backed allocator and absent-handoff UAT constructor.
    if GROUNDED_ROLE_RESOURCE_OWNERSHIP_AVAILABLE && EXECUTABLE_ROLE_RESOURCES_AVAILABLE {
        implemented |= missing::ROLE_RESOURCES;
    }
    if let Some(G17FirmwareVariant::G17P) = expected_variant {
        if g17_resources::EXECUTABLE_T8140_ROLE_RESOURCES_AVAILABLE {
            implemented |= missing::ROLE_RESOURCES;
        }
        if g17_resources::EXECUTABLE_T8140_SHARED_UAT_AVAILABLE {
            implemented |= missing::UAT_HANDOFF;
        }
    }
    if g17_initdata::GROUNDED_LAYOUT_AVAILABLE && g17_initdata::PARSE_ACCEPTED_GRAPH_AVAILABLE {
        implemented |= missing::INITDATA_GRAPH;
    }
    if g17_rtkit::GROUNDED_CODEC_AVAILABLE && g17_rtkit::EXECUTABLE_TRANSPORT_AVAILABLE {
        implemented |= missing::RTKIT_TRANSPORT;
    }
    if agx_power_recovery::GROUNDED_POWER_RECOVERY_MODEL_AVAILABLE
        && agx_power_recovery::EXECUTABLE_GFX1_POWER_SEQUENCE_AVAILABLE
    {
        implemented |= missing::GFX1_POWER_SEQUENCE;
    }
    if agx_power_recovery::GROUNDED_POWER_RECOVERY_MODEL_AVAILABLE
        && agx_power_recovery::EXECUTABLE_G17_RECOVERY_HANDSHAKE_AVAILABLE
    {
        implemented |= missing::RECOVERY_HANDSHAKE;
    }
    if g17_submission::GROUNDED_SUBMISSION_PRODUCER_AVAILABLE
        && g17_submission::EXECUTABLE_SUBMISSION_TRANSPORT_AVAILABLE
    {
        implemented |= missing::SUBMISSION_TRANSPORT;
    }

    implemented
}

pub(crate) const fn pre_handoff_gate(gpu_generation: u32) -> Result<(), MissingEvidence> {
    apply_pre_handoff_gate(gpu_generation, None, None)
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn pre_handoff_gate_for_variant(
    gpu_generation: u32,
    expected_variant: G17FirmwareVariant,
) -> Result<(), MissingEvidence> {
    apply_pre_handoff_gate(gpu_generation, Some(expected_variant), None)
}

/// Apply the pre-handoff gate with an optional byte-validated role pair.
///
/// The diagnostic G17S probe supplies the opaque result of the exact versioned
/// m1n1 loaded-segment handoff validator here. Other variants remain on
/// [`pre_handoff_gate`]. This seam prevents a static expected-identity lookup
/// or unrelated firmware file from clearing the bit.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn pre_handoff_gate_with_identity(
    gpu_generation: u32,
    expected_variant: G17FirmwareVariant,
    firmware: Option<&g17_firmware::G17FirmwareIdentityAdmission>,
) -> Result<(), MissingEvidence> {
    apply_pre_handoff_gate(gpu_generation, Some(expected_variant), firmware)
}

const fn apply_pre_handoff_gate(
    gpu_generation: u32,
    expected_variant: Option<G17FirmwareVariant>,
    firmware: Option<&g17_firmware::G17FirmwareIdentityAdmission>,
) -> Result<(), MissingEvidence> {
    if gpu_generation != G17_GENERATION {
        return Ok(());
    }

    let missing_bits = REQUIRED_PRE_HANDOFF_EVIDENCE
        & !implemented_pre_handoff_evidence(expected_variant, firmware);
    if missing_bits == 0 {
        Ok(())
    } else {
        Err(MissingEvidence(missing_bits))
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum FirmwareRole {
    /// Scheduler and recovery-ingress role ("primary").
    Gfx = 0,
    /// Power-management / control-only peer role ("secondary").
    Gfx1 = 1,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u8)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum G17FirmwareVariant {
    G17S = b'S',
    G17C = b'C',
    G17P = b'P',
}

/// Strict variant decoding for diagnostic firmware selection.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum FirmwareIdentityError {
    UnknownVariant,
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn decode_g17_firmware_variant(
    raw: u8,
) -> Result<G17FirmwareVariant, FirmwareIdentityError> {
    match raw {
        b'S' => Ok(G17FirmwareVariant::G17S),
        b'C' => Ok(G17FirmwareVariant::G17C),
        b'P' => Ok(G17FirmwareVariant::G17P),
        _ => Err(FirmwareIdentityError::UnknownVariant),
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct G17NestedFirmwareIdentity {
    pub(crate) entry_tag: [u8; 4],
    pub(crate) size: u32,
    pub(crate) uuid: [u8; 16],
    pub(crate) sha256: [u8; 32],
}

#[cfg_attr(not(test), allow(dead_code))]
const G17S_GFX_FIRMWARE_IDENTITY: G17NestedFirmwareIdentity = G17NestedFirmwareIdentity {
    entry_tag: *b"a010",
    size: 0x11_9a90,
    uuid: [
        0x05, 0x23, 0x65, 0x20, 0x33, 0xf1, 0x3d, 0x41, 0x83, 0xcb, 0x16, 0x9f, 0xaa, 0xad, 0xbc,
        0xa7,
    ],
    sha256: [
        0x31, 0x7d, 0xe3, 0x5b, 0x44, 0x7e, 0x2a, 0x4c, 0xce, 0x0f, 0xb4, 0x67, 0xf0, 0xab, 0x04,
        0xba, 0xa4, 0x2f, 0xce, 0xee, 0x8a, 0x3f, 0x55, 0x99, 0x8e, 0xcf, 0x3c, 0x62, 0x4d, 0x30,
        0x03, 0x1b,
    ],
};

#[cfg_attr(not(test), allow(dead_code))]
const G17C_GFX_FIRMWARE_IDENTITY: G17NestedFirmwareIdentity = G17NestedFirmwareIdentity {
    entry_tag: *b"a000",
    size: 0x11_9a90,
    uuid: [
        0x21, 0xa1, 0x77, 0xa0, 0xc2, 0x7b, 0x30, 0x59, 0xa3, 0xb0, 0x05, 0x6c, 0xdd, 0xac, 0x65,
        0x8b,
    ],
    sha256: [
        0x61, 0xa3, 0x59, 0x3e, 0x78, 0x68, 0x67, 0x29, 0x40, 0x5b, 0x77, 0xe1, 0x67, 0xdd, 0x74,
        0xbf, 0xb2, 0x5b, 0x0c, 0xe5, 0x82, 0x29, 0x65, 0x08, 0x54, 0x40, 0x4b, 0x3f, 0xd5, 0x3f,
        0xd8, 0x9a,
    ],
};

#[cfg_attr(not(test), allow(dead_code))]
const G17S_GFX1_FIRMWARE_IDENTITY: G17NestedFirmwareIdentity = G17NestedFirmwareIdentity {
    entry_tag: *b"a010",
    size: 0x11_5aa8,
    uuid: [
        0x0e, 0x98, 0xaf, 0xb8, 0x06, 0x80, 0x3b, 0x15, 0x93, 0x2e, 0x91, 0x0d, 0x0a, 0x1c, 0xcd,
        0x78,
    ],
    sha256: [
        0x5d, 0x71, 0x92, 0x08, 0xe1, 0x2b, 0xe6, 0xbf, 0xa7, 0x54, 0xca, 0x84, 0x86, 0xf3, 0x67,
        0x39, 0xfc, 0xe7, 0x4b, 0x0c, 0x45, 0x61, 0x8a, 0xd9, 0xa0, 0xf4, 0x4a, 0x18, 0x9f, 0xee,
        0xb3, 0x45,
    ],
};

#[cfg_attr(not(test), allow(dead_code))]
const G17C_GFX1_FIRMWARE_IDENTITY: G17NestedFirmwareIdentity = G17NestedFirmwareIdentity {
    entry_tag: *b"a000",
    size: 0x11_5aa8,
    uuid: [
        0xfe, 0xb8, 0x1f, 0x4a, 0xf0, 0x75, 0x34, 0x49, 0xa6, 0x62, 0x3c, 0x01, 0xc0, 0x99, 0x4b,
        0x4f,
    ],
    sha256: [
        0xcf, 0x81, 0x13, 0xa8, 0xbc, 0x4a, 0x62, 0xc3, 0x76, 0xd1, 0x1a, 0x4d, 0xc5, 0xd5, 0xc5,
        0x42, 0x99, 0x14, 0xb9, 0x5a, 0xb2, 0xd2, 0x19, 0x0a, 0x0e, 0xba, 0xd3, 0x27, 0x8a, 0x32,
        0x34, 0x68,
    ],
};

/// Select only an exact role/variant nested-firmware identity.
///
/// This proves the contents of the pinned containers, not a live G17C target
/// or Linux ownership of FTAB extraction, fixups, or firmware loading.
/// Returns `None` for G17P: the pinned corpus has no role-paired nested
/// images for it (see [`G17FirmwareVariant`]), so no byte identity exists to
/// select, so no caller can admit the image.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn g17_nested_firmware_identity(
    role: FirmwareRole,
    variant: G17FirmwareVariant,
) -> Option<&'static G17NestedFirmwareIdentity> {
    match (role, variant) {
        (FirmwareRole::Gfx, G17FirmwareVariant::G17S) => Some(&G17S_GFX_FIRMWARE_IDENTITY),
        (FirmwareRole::Gfx, G17FirmwareVariant::G17C) => Some(&G17C_GFX_FIRMWARE_IDENTITY),
        (FirmwareRole::Gfx1, G17FirmwareVariant::G17S) => Some(&G17S_GFX1_FIRMWARE_IDENTITY),
        (FirmwareRole::Gfx1, G17FirmwareVariant::G17C) => Some(&G17C_GFX1_FIRMWARE_IDENTITY),
        (_, G17FirmwareVariant::G17P) => None,
    }
}

/// Validate tag, UUID, and hash together; no field is sufficient by itself.
/// A variant with no pinned identity (G17P) validates nothing.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn validate_g17_nested_firmware_identity(
    role: FirmwareRole,
    variant: G17FirmwareVariant,
    observed: &G17NestedFirmwareIdentity,
) -> bool {
    match g17_nested_firmware_identity(role, variant) {
        Some(expected) => observed == expected,
        None => false,
    }
}

/// The G17P/G17G/G17S host/RTBuddy firmware-start model is exact but
/// diagnostic only.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const GROUNDED_G17X_FIRMWARE_START_MODEL_AVAILABLE: bool = true;

/// Linux cannot execute this handshake until the opaque loader boundary and
/// its power, cache, interrupt, and completion ownership are recovered.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const EXECUTABLE_G17X_FIRMWARE_START_HANDSHAKE_AVAILABLE: bool = false;

/// Pinned host targets which prove the dual-role boot lifecycle.
///
/// G17P is the A18 Pro HAL200 target, G17G is M5, and G17S is M5 Pro/Max.
/// Requiring an explicit target is load-bearing: G17G shifts multiple host
/// object fields relative to G17P/G17S even though the control flow is the
/// same.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum G17HostBootEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17_HOST_BOOT_EVIDENCE_TARGETS: [G17HostBootEvidenceTarget; 3] = [
    G17HostBootEvidenceTarget::G17P,
    G17HostBootEvidenceTarget::G17G,
    G17HostBootEvidenceTarget::G17S,
];

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct G17HostBootLayout {
    pub(crate) role_record_base: u32,
    pub(crate) combined_started: u32,
    pub(crate) accelerator: u16,
    pub(crate) event_source: u16,
    pub(crate) host_booted: u16,
    pub(crate) cold_boot_role0: u16,
    pub(crate) cold_boot_role1: u16,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct G17HostBootTextEvidence {
    pub(crate) boot_firmware_va: u64,
    pub(crate) notify_started_va: u64,
    pub(crate) complete_cold_boot_va: u64,
    pub(crate) cold_boot_done_va: u64,
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn g17_host_boot_layout(target: G17HostBootEvidenceTarget) -> G17HostBootLayout {
    match target {
        G17HostBootEvidenceTarget::G17P | G17HostBootEvidenceTarget::G17S => G17HostBootLayout {
            role_record_base: 0x1980,
            combined_started: 0x1a6d,
            accelerator: 0x2a0,
            event_source: 0x3d0,
            host_booted: 0x2b0,
            cold_boot_role0: 0x0b78,
            cold_boot_role1: 0x0ca8,
        },
        G17HostBootEvidenceTarget::G17G => G17HostBootLayout {
            role_record_base: 0x19c8,
            combined_started: 0x1ab5,
            accelerator: 0x2a8,
            event_source: 0x3e0,
            host_booted: 0x2b8,
            cold_boot_role0: 0x0b80,
            cold_boot_role1: 0x0cb0,
        },
    }
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn g17_host_boot_text_evidence(
    target: G17HostBootEvidenceTarget,
) -> G17HostBootTextEvidence {
    match target {
        G17HostBootEvidenceTarget::G17P => G17HostBootTextEvidence {
            boot_firmware_va: 0xffff_fe00_08a4_4ce0,
            notify_started_va: 0xffff_fe00_08a2_e744,
            complete_cold_boot_va: 0xffff_fe00_08a2_368c,
            cold_boot_done_va: 0xffff_fe00_08a2_36c0,
        },
        G17HostBootEvidenceTarget::G17G => G17HostBootTextEvidence {
            boot_firmware_va: 0xffff_fe00_08b7_666c,
            notify_started_va: 0xffff_fe00_08b5_fe60,
            complete_cold_boot_va: 0xffff_fe00_08b5_4de4,
            cold_boot_done_va: 0xffff_fe00_08b5_4e18,
        },
        G17HostBootEvidenceTarget::G17S => G17HostBootTextEvidence {
            boot_firmware_va: 0xffff_fe00_08b3_9754,
            notify_started_va: 0xffff_fe00_08b2_25c0,
            complete_cold_boot_va: 0xffff_fe00_08b1_765c,
            cold_boot_done_va: 0xffff_fe00_08b1_7690,
        },
    }
}

/// Exact host-side offsets for one role record in a pinned G17 driver.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct G17xRoleHostOffsets {
    pub(crate) rtbuddy_object: u32,
    pub(crate) token_source_object: u32,
    pub(crate) firmware_map: u32,
    pub(crate) started: u32,
    pub(crate) timestamp: u32,
}

const G17X_ROLE_RECORD_STRIDE: u32 = 0x38;

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn g17_role_host_offsets(
    target: G17HostBootEvidenceTarget,
    role: FirmwareRole,
) -> G17xRoleHostOffsets {
    let record =
        g17_host_boot_layout(target).role_record_base + role as u32 * G17X_ROLE_RECORD_STRIDE;

    G17xRoleHostOffsets {
        rtbuddy_object: record,
        token_source_object: record + 0x10,
        firmware_map: record + 0x20,
        started: record + 0x28,
        timestamp: record + 0x30,
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum G17xHostBootOp {
    ClearRoleStarted {
        role: FirmwareRole,
        offset: u32,
    },
    ClearCombinedStarted {
        offset: u32,
    },
    StartRole {
        role: FirmwareRole,
        rtbuddy_object: u32,
        firmware_map: u32,
    },
    RequireBothRoleStarts,
    WaitForEitherRoleStarted {
        combined_offset: u32,
        accelerator_offset: u16,
        event_source_offset: u16,
        deadline_helper_arg0: u32,
        deadline_helper_arg1: u32,
        event_arg3: u32,
        continue_result: u32,
    },
    MarkHostBootedAndNotifyAccelerator {
        offset: u16,
    },
}

/// G17 adds a second host role; the role-start virtual slot itself is shared
/// with independently pinned G15C and G16X host implementations.
#[cfg_attr(not(test), allow(dead_code))]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G15C_G16X_BOOT_ROLE_COUNT: u8 = 1;
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17X_BOOT_ROLE_COUNT: u8 = 2;

/// Exact target-specific G17 boot order. The wait is satisfied by either
/// role's callback, but both role-start calls must first return nonzero.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn g17_host_boot_ops(target: G17HostBootEvidenceTarget) -> [G17xHostBootOp; 8] {
    let layout = g17_host_boot_layout(target);
    let gfx = g17_role_host_offsets(target, FirmwareRole::Gfx);
    let gfx1 = g17_role_host_offsets(target, FirmwareRole::Gfx1);

    [
        G17xHostBootOp::ClearRoleStarted {
            role: FirmwareRole::Gfx,
            offset: gfx.started,
        },
        G17xHostBootOp::ClearRoleStarted {
            role: FirmwareRole::Gfx1,
            offset: gfx1.started,
        },
        G17xHostBootOp::ClearCombinedStarted {
            offset: layout.combined_started,
        },
        G17xHostBootOp::StartRole {
            role: FirmwareRole::Gfx,
            rtbuddy_object: gfx.rtbuddy_object,
            firmware_map: gfx.firmware_map,
        },
        G17xHostBootOp::StartRole {
            role: FirmwareRole::Gfx1,
            rtbuddy_object: gfx1.rtbuddy_object,
            firmware_map: gfx1.firmware_map,
        },
        G17xHostBootOp::RequireBothRoleStarts,
        G17xHostBootOp::WaitForEitherRoleStarted {
            combined_offset: layout.combined_started,
            accelerator_offset: layout.accelerator,
            event_source_offset: layout.event_source,
            deadline_helper_arg0: 50,
            deadline_helper_arg1: 1_000_000,
            event_arg3: 0,
            continue_result: 1,
        },
        G17xHostBootOp::MarkHostBootedAndNotifyAccelerator {
            offset: layout.host_booted,
        },
    ]
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn validate_g17_host_boot_ops(
    target: G17HostBootEvidenceTarget,
    ops: &[G17xHostBootOp],
) -> bool {
    ops == g17_host_boot_ops(target)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct G17xRtBuddyEndpointBinding {
    pub(crate) service_id: u8,
    pub(crate) object_offset: u16,
}

/// These endpoint IDs and object offsets are also byte-identical in the
/// pinned G15C and G16X RTBuddy implementations. Arrival order is not fixed.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17X_RTBUDDY_ENDPOINT_BINDINGS: [G17xRtBuddyEndpointBinding; 2] = [
    G17xRtBuddyEndpointBinding {
        service_id: 0x20,
        object_offset: 0x128,
    },
    G17xRtBuddyEndpointBinding {
        service_id: 0x21,
        object_offset: 0x130,
    },
];

/// Ordered convergence after both endpoint objects are non-null.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum G17xRtBuddyStartOp {
    RequireBothEndpoints {
        first_object_offset: u16,
        second_object_offset: u16,
    },
    EnableEndpoint {
        service_id: u8,
        object_offset: u16,
    },
    NotifyHostRole {
        host_object_offset: u16,
        role_offset: u16,
    },
}

#[cfg_attr(not(test), allow(dead_code))]

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17X_RTBUDDY_START_OPS: [G17xRtBuddyStartOp; 4] = [
    G17xRtBuddyStartOp::RequireBothEndpoints {
        first_object_offset: 0x128,
        second_object_offset: 0x130,
    },
    G17xRtBuddyStartOp::EnableEndpoint {
        service_id: 0x20,
        object_offset: 0x128,
    },
    G17xRtBuddyStartOp::EnableEndpoint {
        service_id: 0x21,
        object_offset: 0x130,
    },
    G17xRtBuddyStartOp::NotifyHostRole {
        host_object_offset: 0x170,
        role_offset: 0x120,
    },
];

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn validate_g17x_rtbuddy_start_ops(ops: &[G17xRtBuddyStartOp]) -> bool {
    ops == G17X_RTBUDDY_START_OPS
}

/// Exact host callback operations, parameterized by the role-record base.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum G17xNotifyStartedOp {
    SetRoleStarted {
        relative_offset: u8,
    },
    StoreTimestamp {
        relative_offset: u8,
    },
    SetCombinedStarted {
        absolute_offset: u32,
    },
    WakeCombinedStarted {
        accelerator_offset: u16,
        event_source_offset: u16,
        argument: u32,
    },
    QueryRoleToken {
        source_relative_offset: u8,
        firmware_argument: u32,
    },
    EncodeStartedMessage {
        prefix: u64,
        token_bits: u8,
    },
    SendRoleMessage {
        endpoint_relative_offset: u8,
    },
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const fn g17_notify_started_ops(
    target: G17HostBootEvidenceTarget,
) -> [G17xNotifyStartedOp; 7] {
    let layout = g17_host_boot_layout(target);

    [
        G17xNotifyStartedOp::SetRoleStarted {
            relative_offset: 0x28,
        },
        G17xNotifyStartedOp::StoreTimestamp {
            relative_offset: 0x30,
        },
        G17xNotifyStartedOp::SetCombinedStarted {
            absolute_offset: layout.combined_started,
        },
        G17xNotifyStartedOp::WakeCombinedStarted {
            accelerator_offset: layout.accelerator,
            event_source_offset: layout.event_source,
            argument: 0,
        },
        G17xNotifyStartedOp::QueryRoleToken {
            source_relative_offset: 0x10,
            firmware_argument: 0,
        },
        G17xNotifyStartedOp::EncodeStartedMessage {
            prefix: 0x0081_0000_0000_0000,
            token_bits: 44,
        },
        G17xNotifyStartedOp::SendRoleMessage {
            endpoint_relative_offset: 0,
        },
    ]
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn validate_g17_notify_started_ops(
    target: G17HostBootEvidenceTarget,
    ops: &[G17xNotifyStartedOp],
) -> bool {
    ops == g17_notify_started_ops(target)
}

/// Exact remaining boundaries which keep the diagnostic model non-executable.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct G17xFirmwareStartGaps {
    pub(crate) load_firmware_mmio_and_power_order_proven: bool,
    pub(crate) load_firmware_cache_maintenance_proven: bool,
    pub(crate) power_callback_to_endpoint_causality_proven: bool,
    pub(crate) interrupt_and_ack_route_proven: bool,
    pub(crate) both_cold_boot_flags_to_submission_causality_proven: bool,
}

#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17X_FIRMWARE_START_GAPS: G17xFirmwareStartGaps = G17xFirmwareStartGaps {
    load_firmware_mmio_and_power_order_proven: false,
    load_firmware_cache_maintenance_proven: true,
    power_callback_to_endpoint_causality_proven: false,
    interrupt_and_ack_route_proven: false,
    both_cold_boot_flags_to_submission_causality_proven: false,
};

const ROLE_COUNT: usize = 2;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum KickOrder {
    PrimaryFirst,
    /// `GFX1` first, then `GFX`. Matches the published T8140 firmware ABI
    /// spec's startup sequence. Not hardware-refuted; not the default.
    SecondaryFirst,
}

impl KickOrder {
    pub(crate) const fn role_at(self, index: usize) -> FirmwareRole {
        match (self, index) {
            (Self::PrimaryFirst, 0) | (Self::SecondaryFirst, 1) => FirmwareRole::Gfx,
            _ => FirmwareRole::Gfx1,
        }
    }
}

impl FirmwareRole {
    const fn index(self) -> usize {
        self as usize
    }
}

/// One GPU allocator/UAT owner shared by both role records.
///
/// This is intentionally stored once in [`DualRoleBoot`]. A role never owns a
/// second UAT or allocator.
#[derive(Debug)]
pub(crate) struct SharedGpuUatOwner;

#[derive(Debug, Copy, Clone, Default)]
struct RoleState {
    initdata_root_ready: bool,
    kick_succeeded: bool,
    started_notified: bool,
    cold_boot_complete: bool,
}

/// Errors in the host-observed boot sequence.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum BootError {
    /// Both role roots must be built and validated before the first kick.
    InitdataRootsIncomplete,
    /// A role was kicked out of the exact order selected by [`KickOrder`].
    WrongKickOrder,
    /// A role start call failed; the boot attempt is permanently failed.
    KickFailed,
    /// A root was changed after the first role kick.
    RootMutationAfterKick,
    /// An earlier sequencing error already failed this boot attempt.
    AlreadyFailed,
}

/// Pure state model for one G17 dual-role cold boot.
///
/// Notifications may race with the host's start calls, so notification methods
/// only record flags. The three public gate predicates remain independent, and
/// [`Self::handoff_ready`] composes them before submission can be exposed.
#[cfg_attr(not(test), allow(dead_code))]
#[derive(Debug)]
pub(crate) struct DualRoleBoot {
    owner: SharedGpuUatOwner,
    roles: [RoleState; ROLE_COUNT],
    kick_order: KickOrder,
    next_kick: usize,
    failed: bool,
}

#[cfg_attr(not(test), allow(dead_code))]
impl DualRoleBoot {
    /// Create an empty boot attempt with one shared GPU/UAT owner, using the
    /// default primary-first kick order (see [`KickOrder`]).
    pub(crate) const fn new() -> Self {
        Self::new_with_order(KickOrder::PrimaryFirst)
    }

    /// Create an empty boot attempt with an explicit kick order.
    pub(crate) const fn new_with_order(kick_order: KickOrder) -> Self {
        Self {
            owner: SharedGpuUatOwner,
            roles: [
                RoleState {
                    initdata_root_ready: false,
                    kick_succeeded: false,
                    started_notified: false,
                    cold_boot_complete: false,
                },
                RoleState {
                    initdata_root_ready: false,
                    kick_succeeded: false,
                    started_notified: false,
                    cold_boot_complete: false,
                },
            ],
            kick_order,
            next_kick: 0,
            failed: false,
        }
    }

    /// The kick order this boot attempt enforces.
    pub(crate) const fn kick_order(&self) -> KickOrder {
        self.kick_order
    }

    /// Return the single address-space owner used by either role.
    pub(crate) const fn owner_for(&self, _role: FirmwareRole) -> &SharedGpuUatOwner {
        &self.owner
    }

    /// Mark one role-specific init-data root ready for the shared UAT.
    pub(crate) fn mark_initdata_root_ready(&mut self, role: FirmwareRole) -> Result<(), BootError> {
        if self.failed {
            return Err(BootError::AlreadyFailed);
        }
        if self.next_kick != 0 {
            self.failed = true;
            return Err(BootError::RootMutationAfterKick);
        }

        self.roles[role.index()].initdata_root_ready = true;
        Ok(())
    }

    /// Record the return value of the next role start call.
    ///
    /// Both roots must already exist, and calls must be recorded in the exact
    /// order selected at construction ([`KickOrder`]; primary-first by
    /// default). Any failure permanently closes all gates.
    pub(crate) fn record_kick(
        &mut self,
        role: FirmwareRole,
        succeeded: bool,
    ) -> Result<(), BootError> {
        if self.failed {
            return Err(BootError::AlreadyFailed);
        }
        if self.roles.iter().any(|state| !state.initdata_root_ready) {
            self.failed = true;
            return Err(BootError::InitdataRootsIncomplete);
        }
        if self.next_kick >= ROLE_COUNT || self.kick_order.role_at(self.next_kick) != role {
            self.failed = true;
            return Err(BootError::WrongKickOrder);
        }
        if !succeeded {
            self.failed = true;
            return Err(BootError::KickFailed);
        }

        self.roles[role.index()].kick_succeeded = true;
        self.next_kick += 1;
        Ok(())
    }

    pub(crate) fn notify_started(&mut self, role: FirmwareRole) {
        self.roles[role.index()].started_notified = true;
    }

    /// Record one role's later cold-boot-complete callback.
    pub(crate) fn notify_cold_boot_complete(&mut self, role: FirmwareRole) {
        self.roles[role.index()].cold_boot_complete = true;
    }

    /// Both ordered role start calls returned success.
    pub(crate) fn kick_gate_open(&self) -> bool {
        !self.failed
            && self.next_kick == ROLE_COUNT
            && self.roles.iter().all(|state| state.kick_succeeded)
    }

    /// At least one role delivered its started callback.
    pub(crate) fn early_start_gate_open(&self) -> bool {
        !self.failed && self.roles.iter().any(|state| state.started_notified)
    }

    /// Both per-role cold-boot flags are set.
    pub(crate) fn cold_boot_gate_open(&self) -> bool {
        !self.failed && self.roles.iter().all(|state| state.cold_boot_complete)
    }

    /// Full readiness requires every independent host-observed gate.
    pub(crate) fn handoff_ready(&self) -> bool {
        self.kick_gate_open() && self.early_start_gate_open() && self.cold_boot_gate_open()
    }
}


/// Firmware global that receives the copied init-data root header (FW-1).
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17_ROOT_HEADER_FW_GLOBAL: u64 = 0xffff_fc00_0010_4890;
/// Byte offset of the role selector inside the copied root header (root+0x28).
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17_ROOT_ROLE_SELECTOR_OFFSET: u32 = 0x28;
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17_GFX1_RUNTIME_RESOURCE_REFS_IN_GFX1: u32 = 78;
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) const G17_GFX1_RUNTIME_RESOURCE_REFS_IN_GFX: u32 = 0;

/// The per-role resource-ownership *contract* is byte-grounded and encodable
/// offline (FW-1..FW-4). This does not assert that the driver has wired the
/// live allocation/mapping.
pub(crate) const GROUNDED_ROLE_RESOURCE_OWNERSHIP_AVAILABLE: bool = true;
/// Whether Linux may execute the per-role resource plumbing. It may not:
/// there is no firmware parse-time role check to satisfy (FW-3), so the
/// driver's validator is the safety boundary, and the actual sub-allocation
/// and UAT mapping is a live DMA activity that cannot be validated from the
/// machos. A wrong role/resource pairing is silently accepted by the firmware
/// and only fails as a functional fault a live M5 boot exposes.
pub(crate) const EXECUTABLE_ROLE_RESOURCES_AVAILABLE: bool = false;

/// Strict per-role resource-ownership description.
///
/// Both roles share exactly one GPU/UAT owner ([`SharedGpuUatOwner`]); only
/// GFX1 owns the runtime resource at runtime_pointers+0x481 (FW-4).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) struct RoleResourceOwnership {
    pub(crate) role: FirmwareRole,
    /// A non-zero GPU VA for the GFX1-only runtime resource (rt+0x481).
    /// Required for GFX1, must be zero for GFX.
    pub(crate) gfx1_runtime_resource: u64,
}

/// Role-resource validation errors.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) enum RoleResourceError {
    /// GFX carried a GFX1-only resource, or GFX1 omitted its required one.
    RoleSpecificResourceViolation,
    /// A required resource pointer was null.
    NullRequiredResource,
    /// The two roles did not resolve to one shared GPU/UAT owner.
    SharedOwnerAliasViolation,
}

/// Validate one role's resource ownership.
///
/// Because the firmware never self-validates the role (FW-3), this is the only
/// place a role/resource mismatch is caught before handoff. It refuses a null
/// required resource and any cross-role resource ownership.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn validate_role_resource_ownership(
    ownership: &RoleResourceOwnership,
) -> Result<(), RoleResourceError> {
    match ownership.role {
        FirmwareRole::Gfx => {
            if ownership.gfx1_runtime_resource != 0 {
                return Err(RoleResourceError::RoleSpecificResourceViolation);
            }
        }
        FirmwareRole::Gfx1 => {
            if ownership.gfx1_runtime_resource == 0 {
                return Err(RoleResourceError::NullRequiredResource);
            }
        }
    }
    Ok(())
}

/// Validate that the two roles in a boot resolve to one GPU/UAT owner and hold
/// mutually consistent, role-correct resources.
#[cfg_attr(not(test), allow(dead_code))]
pub(crate) fn validate_dual_role_resources(
    boot: &DualRoleBoot,
    ownership: &[RoleResourceOwnership; ROLE_COUNT],
) -> Result<(), RoleResourceError> {
    if !core::ptr::eq(
        boot.owner_for(FirmwareRole::Gfx),
        boot.owner_for(FirmwareRole::Gfx1),
    ) {
        return Err(RoleResourceError::SharedOwnerAliasViolation);
    }
    if ownership[FirmwareRole::Gfx.index()].role != FirmwareRole::Gfx
        || ownership[FirmwareRole::Gfx1.index()].role != FirmwareRole::Gfx1
    {
        return Err(RoleResourceError::RoleSpecificResourceViolation);
    }
    for one in ownership {
        validate_role_resource_ownership(one)?;
    }
    Ok(())
}

