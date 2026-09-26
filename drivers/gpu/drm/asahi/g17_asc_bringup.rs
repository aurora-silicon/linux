// SPDX-License-Identifier: GPL-2.0-only OR MIT


#![cfg_attr(not(test), allow(dead_code))]

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17AscEvidenceTarget {
    G17P,
    G17G,
    G17S,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AscTextEvidence {
    pub(crate) rtbuddy_load_firmware: u64,
    pub(crate) rtbuddy_force_power_off: u64,
    pub(crate) rtbuddy_copy_to_target: u64,
    pub(crate) rtbuddy_write_back_patch_bay: u64,
    pub(crate) a7iop_start_cpu_with_options: u64,
    pub(crate) a7iop_stop_cpu: u64,
    pub(crate) a7iop_reset_state: u64,
    pub(crate) a7iop_enable_power: u64,
    pub(crate) ascwrap_initialize: u64,
    pub(crate) ascwrap_set_iorvbar: u64,
    pub(crate) ascwrap_is_idle: u64,
    pub(crate) ascwrap_run_cpu: u64,
}

pub(crate) const fn g17_asc_text_evidence(target: G17AscEvidenceTarget) -> G17AscTextEvidence {
    match target {
        G17AscEvidenceTarget::G17P => G17AscTextEvidence {
            rtbuddy_load_firmware: 0xffff_fe00_0b1f_b288,
            rtbuddy_force_power_off: 0xffff_fe00_0b1f_13bc,
            rtbuddy_copy_to_target: 0xffff_fe00_0b1f_f028,
            rtbuddy_write_back_patch_bay: 0xffff_fe00_0b1f_fa00,
            a7iop_start_cpu_with_options: 0xffff_fe00_08ab_c6d0,
            a7iop_stop_cpu: 0xffff_fe00_08ab_ca30,
            a7iop_reset_state: 0xffff_fe00_08ab_dde0,
            a7iop_enable_power: 0xffff_fe00_08ab_c518,
            ascwrap_initialize: 0xffff_fe00_08ab_9f7c,
            ascwrap_set_iorvbar: 0xffff_fe00_08ab_a1f8,
            ascwrap_is_idle: 0xffff_fe00_08ab_a414,
            ascwrap_run_cpu: 0xffff_fe00_08ab_a4b4,
        },
        G17AscEvidenceTarget::G17G => G17AscTextEvidence {
            rtbuddy_load_firmware: 0xffff_fe00_0b5f_df08,
            rtbuddy_force_power_off: 0xffff_fe00_0b5f_403c,
            rtbuddy_copy_to_target: 0xffff_fe00_0b60_1ca8,
            rtbuddy_write_back_patch_bay: 0xffff_fe00_0b60_2680,
            a7iop_start_cpu_with_options: 0xffff_fe00_08be_fd20,
            a7iop_stop_cpu: 0xffff_fe00_08bf_0080,
            a7iop_reset_state: 0xffff_fe00_08bf_1430,
            a7iop_enable_power: 0xffff_fe00_08be_fb68,
            ascwrap_initialize: 0xffff_fe00_08be_c1cc,
            ascwrap_set_iorvbar: 0xffff_fe00_08be_c448,
            ascwrap_is_idle: 0xffff_fe00_08be_c664,
            ascwrap_run_cpu: 0xffff_fe00_08be_c704,
        },
        G17AscEvidenceTarget::G17S => G17AscTextEvidence {
            rtbuddy_load_firmware: 0xffff_fe00_0b2e_cab8,
            rtbuddy_force_power_off: 0xffff_fe00_0b2e_2bec,
            rtbuddy_copy_to_target: 0xffff_fe00_0b2f_0858,
            rtbuddy_write_back_patch_bay: 0xffff_fe00_0b2f_1230,
            a7iop_start_cpu_with_options: 0xffff_fe00_08bb_6a50,
            a7iop_stop_cpu: 0xffff_fe00_08bb_6db0,
            a7iop_reset_state: 0xffff_fe00_08bb_8160,
            a7iop_enable_power: 0xffff_fe00_08bb_6898,
            ascwrap_initialize: 0xffff_fe00_08bb_2efc,
            ascwrap_set_iorvbar: 0xffff_fe00_08bb_3178,
            ascwrap_is_idle: 0xffff_fe00_08bb_3394,
            ascwrap_run_cpu: 0xffff_fe00_08bb_3434,
        },
    }
}

pub(crate) const G17_ASC_BRIDGE_TARGET_PARITY_PROVEN: bool = true;

/// G17P-only text anchors for the teardown details recovered after the common
/// P/G/S bridge audit.  These are deliberately separate from
/// [`G17AscTextEvidence`]: equivalent G17G/G17S bodies have not been admitted.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PAscTeardownTextEvidence {
    pub(crate) a7iop_set_mapper_active: u64,
    pub(crate) a7iop_disable_power: u64,
    pub(crate) a7iop_turn_off_domain: u64,
    pub(crate) a7iop_enable_mailbox_interrupts: u64,
    pub(crate) ascwrap_unmap_firmware: u64,
    pub(crate) ascwrap_enable_outbox: u64,
    pub(crate) ascwrap_disable_all_interrupts: u64,
    pub(crate) ascwrap_enable_inbox_interrupt: u64,
    pub(crate) ascwrap_enable_outbox_interrupt: u64,
    pub(crate) armio_device_enable_clock: u64,
    pub(crate) armio_device_enable_power: u64,
    pub(crate) h17p_platformio_enable_clock: u64,
    pub(crate) h17p_platformio_enable_power: u64,
}

pub(crate) const G17P_ASC_TEARDOWN_TEXT_EVIDENCE: G17PAscTeardownTextEvidence =
    G17PAscTeardownTextEvidence {
        a7iop_set_mapper_active: 0xffff_fe00_08ab_d394,
        a7iop_disable_power: 0xffff_fe00_08ab_c648,
        a7iop_turn_off_domain: 0xffff_fe00_08ab_c69c,
        a7iop_enable_mailbox_interrupts: 0xffff_fe00_08ab_d580,
        ascwrap_unmap_firmware: 0xffff_fe00_08ab_a298,
        ascwrap_enable_outbox: 0xffff_fe00_08ab_a944,
        ascwrap_disable_all_interrupts: 0xffff_fe00_08ab_ab4c,
        ascwrap_enable_inbox_interrupt: 0xffff_fe00_08ab_ab54,
        ascwrap_enable_outbox_interrupt: 0xffff_fe00_08ab_ab5c,
        armio_device_enable_clock: 0xffff_fe00_08b0_b0d8,
        armio_device_enable_power: 0xffff_fe00_08b0_b1e8,
        h17p_platformio_enable_clock: 0xffff_fe00_09a8_9080,
        h17p_platformio_enable_power: 0xffff_fe00_09a8_9154,
    };

pub(crate) const ASC_CPU_CONTROL_OFFSET: u32 = 0x44;
pub(crate) const ASC_CPU_RUN_BIT: u32 = 0x10;
/// `_runCPU(false)` clears this second bit after clearing RUN and rereading.
/// Apple does not name its meaning in the recovered method, so neither do we.
pub(crate) const ASC_CPU_STOP_SECOND_CLEAR_BIT: u32 = 0x20;
pub(crate) const ASC_CPU_STATUS_OFFSET: u32 = 0x48;
pub(crate) const ASC_CPU_STATUS_RUNNING_BIT: u32 = 1 << 0;
pub(crate) const ASC_CPU_STATUS_STOPPED_BIT: u32 = 1 << 1;
pub(crate) const ASC_CPU_STATUS_IDLE_BIT: u32 = 1 << 5;
pub(crate) const ASC_IORVBAR_VALID_BIT: u64 = 1;
pub(crate) const ASC_CPU_CTRL_FILTERED_PROPERTY: &str = "cpu-ctrl-filtered";
pub(crate) const ASC_IDLE_CTRL_CHECK_PROPERTY: &str = "idle-ctrl-check";
pub(crate) const ASCWRAP_V6_OUTBOX_CONTROL_OFFSET: u32 = 0x8114;
pub(crate) const ASCWRAP_V6_MAILBOX_ENABLE_BIT: u32 = 1;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AscProviderProperties {
    pub(crate) cpu_ctrl_filtered: bool,
    pub(crate) idle_ctrl_check: bool,
}

pub(crate) const G17_ASC_PROVIDER_PROPERTIES: [G17AscProviderProperties; 2] =
    [G17AscProviderProperties {
        cpu_ctrl_filtered: false,
        idle_ctrl_check: false,
    }; 2];

pub(crate) const fn wrapper_mmio_enabled(properties: G17AscProviderProperties) -> bool {
    !properties.cpu_ctrl_filtered
}

pub(crate) const ASC_IDLE_PREREQUISITE_OFFSET: u32 = 0x40;
pub(crate) const ASC_IDLE_STATUS_OFFSET: u32 = 0x8000;
pub(crate) const ASC_IDLE_STATUS_MASK: u32 = 0x3;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscIdleOp {
    ReadPrerequisite { offset: u32 },
    ReadStatus { offset: u32, idle_mask: u32 },
    None,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct AscIdlePlan {
    pub(crate) len: u8,
    pub(crate) ops: [AscIdleOp; 2],
}

/// Exact `_isIdle` read plan. If `idle-ctrl-check` exists, a zero read at
/// +0x40 returns busy without touching +0x8000. Otherwise idle is exactly
/// `(status & 3) != 0`.
pub(crate) const fn asc_idle_plan(properties: G17AscProviderProperties) -> AscIdlePlan {
    if properties.idle_ctrl_check {
        AscIdlePlan {
            len: 2,
            ops: [
                AscIdleOp::ReadPrerequisite {
                    offset: ASC_IDLE_PREREQUISITE_OFFSET,
                },
                AscIdleOp::ReadStatus {
                    offset: ASC_IDLE_STATUS_OFFSET,
                    idle_mask: ASC_IDLE_STATUS_MASK,
                },
            ],
        }
    } else {
        AscIdlePlan {
            len: 1,
            ops: [
                AscIdleOp::ReadStatus {
                    offset: ASC_IDLE_STATUS_OFFSET,
                    idle_mask: ASC_IDLE_STATUS_MASK,
                },
                AscIdleOp::None,
            ],
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscIdleResult {
    Busy,
    Idle,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscIdleError {
    MissingPrerequisiteRead,
    MissingStatusRead,
    UnexpectedPrerequisiteRead,
}

pub(crate) const fn classify_asc_idle(
    properties: G17AscProviderProperties,
    prerequisite: Option<u32>,
    status: Option<u32>,
) -> Result<AscIdleResult, AscIdleError> {
    if properties.idle_ctrl_check {
        match prerequisite {
            None => return Err(AscIdleError::MissingPrerequisiteRead),
            Some(0) => return Ok(AscIdleResult::Busy),
            Some(_) => {}
        }
    } else if prerequisite.is_some() {
        return Err(AscIdleError::UnexpectedPrerequisiteRead);
    }

    match status {
        Some(raw) if raw & ASC_IDLE_STATUS_MASK != 0 => Ok(AscIdleResult::Idle),
        Some(_) => Ok(AscIdleResult::Busy),
        None => Err(AscIdleError::MissingStatusRead),
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscMmioOp {
    ReadControl { offset: u32 },
    WriteControlSet { offset: u32, bits: u32 },
    WriteControlClear { offset: u32, bits: u32 },
    None,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct AscMmioPlan {
    pub(crate) len: u8,
    pub(crate) ops: [AscMmioOp; 4],
}

/// Exact ASCWrap-v6 `_runCPU` register transform.
///
/// The wrapper first tests its object flag at +0x132.  When that flag is
/// clear the method returns without MMIO; this is modeled explicitly instead
/// of assuming the aperture is live.
pub(crate) const fn asc_run_cpu_plan(run: bool, wrapper_mmio_enabled: bool) -> AscMmioPlan {
    if !wrapper_mmio_enabled {
        return AscMmioPlan {
            len: 0,
            ops: [AscMmioOp::None; 4],
        };
    }

    if run {
        AscMmioPlan {
            len: 2,
            ops: [
                AscMmioOp::ReadControl {
                    offset: ASC_CPU_CONTROL_OFFSET,
                },
                AscMmioOp::WriteControlSet {
                    offset: ASC_CPU_CONTROL_OFFSET,
                    bits: ASC_CPU_RUN_BIT,
                },
                AscMmioOp::None,
                AscMmioOp::None,
            ],
        }
    } else {
        AscMmioPlan {
            len: 4,
            ops: [
                AscMmioOp::ReadControl {
                    offset: ASC_CPU_CONTROL_OFFSET,
                },
                AscMmioOp::WriteControlClear {
                    offset: ASC_CPU_CONTROL_OFFSET,
                    bits: ASC_CPU_RUN_BIT,
                },
                AscMmioOp::ReadControl {
                    offset: ASC_CPU_CONTROL_OFFSET,
                },
                AscMmioOp::WriteControlClear {
                    offset: ASC_CPU_CONTROL_OFFSET,
                    bits: ASC_CPU_STOP_SECOND_CLEAR_BIT,
                },
            ],
        }
    }
}

/// Exact ASCWrap-v6 `_enableOutbox(bool)` transform.  The method reads through
/// the wrapper accessor at +0x8114, changes only bit zero, and writes the result
/// to the provider-owned wrapper mapping.
pub(crate) const fn asc_outbox_control_value(current: u32, enabled: bool) -> u32 {
    (current & !ASCWRAP_V6_MAILBOX_ENABLE_BIT)
        | if enabled {
            ASCWRAP_V6_MAILBOX_ENABLE_BIT
        } else {
            0
        }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct A7IopMailboxInterruptContract {
    pub(crate) skip_event_sources_flag_offset: u16,
    pub(crate) event_source_offsets: [u16; 2],
    pub(crate) ascwrap_hardware_hooks_perform_mmio: bool,
}

pub(crate) const G17_A7IOP_MAILBOX_INTERRUPT_CONTRACT: A7IopMailboxInterruptContract =
    A7IopMailboxInterruptContract {
        skip_event_sources_flag_offset: 0x121,
        event_source_offsets: [0x180, 0xf0],
        ascwrap_hardware_hooks_perform_mmio: false,
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum IorvbarError {
    AddressAlreadyMarkedValid,
    WrongEncodedValue,
}

/// Encode the exact `_setIORVBAR` write while refusing an already-tagged
/// input address at the Linux validation boundary.
pub(crate) const fn encode_iorvbar(firmware_address: u64) -> Result<u64, IorvbarError> {
    if firmware_address & ASC_IORVBAR_VALID_BIT != 0 {
        Err(IorvbarError::AddressAlreadyMarkedValid)
    } else {
        Ok(firmware_address | ASC_IORVBAR_VALID_BIT)
    }
}

pub(crate) const fn validate_iorvbar_write(
    firmware_address: u64,
    observed: u64,
) -> Result<(), IorvbarError> {
    match encode_iorvbar(firmware_address) {
        Ok(expected) if expected == observed => Ok(()),
        Ok(_) => Err(IorvbarError::WrongEncodedValue),
        Err(e) => Err(e),
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct AscIorvbarOwnerContract {
    pub(crate) provider_resource_index: u8,
    pub(crate) map_options: u32,
    pub(crate) obtains_virtual_address: bool,
    pub(crate) write_width_bytes: u8,
}

pub(crate) const G17_IORVBAR_OWNER_CONTRACT: AscIorvbarOwnerContract = AscIorvbarOwnerContract {
    provider_resource_index: 1,
    map_options: 0,
    obtains_virtual_address: true,
    write_width_bytes: 8,
};

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17FirmwareVisibilityContract {
    pub(crate) map_bypass_option_mask: u32,
    pub(crate) iboot_uses_dart_mapper: bool,
    pub(crate) non_iboot_requires_valid_iorvbar: bool,
    pub(crate) ascwrap_performs_cache_maintenance: bool,
    pub(crate) rtbuddy_publication_cache_operation_proven: bool,
}

pub(crate) const G17_FIRMWARE_VISIBILITY_CONTRACT: G17FirmwareVisibilityContract =
    G17FirmwareVisibilityContract {
        map_bypass_option_mask: 1 << 1,
        iboot_uses_dart_mapper: true,
        non_iboot_requires_valid_iorvbar: true,
        ascwrap_performs_cache_maintenance: false,
        rtbuddy_publication_cache_operation_proven: true,
    };

pub(crate) const IOMEMORY_INCOHERENT_IO_STORE: u32 = 2;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RtBuddyPublicationLength {
    FirmwareDescriptorLength,
    PatchBayMappingLength,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum RtBuddyPublicationOp {
    FullImageCpuCopy,
    DescriptorStore {
        option: u32,
        offset: u64,
        length: RtBuddyPublicationLength,
    },
    FullImageMapRelease,
    PatchBayCpuCopy,
    PatchBayMapRelease,
}

pub(crate) const G17_RTBUDDY_PUBLICATION_ORDER: [RtBuddyPublicationOp; 6] = [
    RtBuddyPublicationOp::FullImageCpuCopy,
    RtBuddyPublicationOp::DescriptorStore {
        option: IOMEMORY_INCOHERENT_IO_STORE,
        offset: 0,
        length: RtBuddyPublicationLength::FirmwareDescriptorLength,
    },
    RtBuddyPublicationOp::FullImageMapRelease,
    RtBuddyPublicationOp::PatchBayCpuCopy,
    RtBuddyPublicationOp::DescriptorStore {
        option: IOMEMORY_INCOHERENT_IO_STORE,
        offset: 0,
        length: RtBuddyPublicationLength::PatchBayMappingLength,
    },
    RtBuddyPublicationOp::PatchBayMapRelease,
];

pub(crate) const G17_RTBUDDY_PUBLICATION_TARGET_PARITY_PROVEN: bool = true;
pub(crate) const G17_RTBUDDY_FULL_IMAGE_PUBLICATION_WINDOW_LEN: usize = 0x88;
pub(crate) const G17_RTBUDDY_FULL_IMAGE_PUBLICATION_WINDOW_SHA256: &str =
    "42b961f82d5677694401daed755734168d91574e53ef1d1b9c5e709de569c60b";
pub(crate) const G17_RTBUDDY_PATCH_BAY_PUBLICATION_WINDOW_LEN: usize = 0xbc;
pub(crate) const G17_RTBUDDY_PATCH_BAY_PUBLICATION_WINDOW_SHA256: &str =
    "4fcbf48f0ea6a4054874d60292ac6cb6c600e7886fb369dcd59fa2c6d93ce8de";

pub(crate) fn validate_rtbuddy_publication_order(observed: &[RtBuddyPublicationOp]) -> bool {
    observed == G17_RTBUDDY_PUBLICATION_ORDER
}

/// Linux ownership boundary for ASCWrap-v6. `apple-mailbox` is the platform
/// driver already bound to the ASC provider node and exclusively claims
/// resource zero. It must therefore become the sole low-level lifecycle owner:
/// resource zero contains CPU control and mailbox state, resource one is the
/// independent IORVBAR aperture, and the same owner must bracket both with
/// power/idle/stop. DRM owns the firmware DMA buffer and requests the lifecycle
/// transition, but must never map either resource beside the mailbox driver.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum LinuxAscOwner {
    AppleMailboxProvider,
    DrmAsahi,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17LinuxAscOwnerContract {
    pub(crate) wrapper_resource0: LinuxAscOwner,
    pub(crate) iorvbar_resource1: LinuxAscOwner,
    pub(crate) power_enable: LinuxAscOwner,
    pub(crate) idle_probe: LinuxAscOwner,
    pub(crate) cpu_control: LinuxAscOwner,
    pub(crate) stop_cpu_teardown: LinuxAscOwner,
    pub(crate) firmware_dma_publication: LinuxAscOwner,
    pub(crate) lifecycle_client: LinuxAscOwner,
}

pub(crate) const G17_LINUX_ASC_OWNER_CONTRACT: G17LinuxAscOwnerContract =
    G17LinuxAscOwnerContract {
        wrapper_resource0: LinuxAscOwner::AppleMailboxProvider,
        iorvbar_resource1: LinuxAscOwner::AppleMailboxProvider,
        power_enable: LinuxAscOwner::AppleMailboxProvider,
        idle_probe: LinuxAscOwner::AppleMailboxProvider,
        cpu_control: LinuxAscOwner::AppleMailboxProvider,
        stop_cpu_teardown: LinuxAscOwner::AppleMailboxProvider,
        firmware_dma_publication: LinuxAscOwner::DrmAsahi,
        lifecycle_client: LinuxAscOwner::DrmAsahi,
    };

pub(crate) fn validate_linux_asc_owner_contract(observed: G17LinuxAscOwnerContract) -> bool {
    observed.wrapper_resource0 == LinuxAscOwner::AppleMailboxProvider
        && observed.iorvbar_resource1 == LinuxAscOwner::AppleMailboxProvider
        && observed.power_enable == LinuxAscOwner::AppleMailboxProvider
        && observed.idle_probe == LinuxAscOwner::AppleMailboxProvider
        && observed.cpu_control == LinuxAscOwner::AppleMailboxProvider
        && observed.stop_cpu_teardown == LinuxAscOwner::AppleMailboxProvider
        && observed.firmware_dma_publication == LinuxAscOwner::DrmAsahi
        && observed.lifecycle_client == LinuxAscOwner::DrmAsahi
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AscFirmwareUnmapContract {
    pub(crate) retained_map_offset: u16,
    pub(crate) unmap_argument: bool,
    pub(crate) clears_retained_pointer: bool,
}

pub(crate) const G17_ASC_FIRMWARE_UNMAP_CONTRACT: G17AscFirmwareUnmapContract =
    G17AscFirmwareUnmapContract {
        retained_map_offset: 0x1a0,
        unmap_argument: true,
        clears_retained_pointer: true,
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AscMapperActiveContract {
    pub(crate) mapper_object_offset: u16,
    pub(crate) function_name: &'static str,
    pub(crate) wait_for_function: bool,
    pub(crate) active_parameter_index: u8,
    pub(crate) trailing_null_parameters: u8,
    pub(crate) opt_out_property: &'static str,
}

pub(crate) const G17_ASC_MAPPER_ACTIVE_CONTRACT: G17AscMapperActiveContract =
    G17AscMapperActiveContract {
        mapper_object_offset: 0x138,
        function_name: "setActive",
        wait_for_function: false,
        active_parameter_index: 1,
        trailing_null_parameters: 3,
        opt_out_property: "no-activate-mapper",
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PAscRole {
    Gfx,
    Gfx1,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PAscPowerOperands {
    pub(crate) role: G17PAscRole,
    pub(crate) index: u8,
    pub(crate) clock_gate_id: u32,
    pub(crate) power_gate_id: u32,
    pub(crate) clock_id_metadata: u32,
    pub(crate) pmgr_device_name: &'static str,
    pub(crate) pmgr_device_has_power_state_register: bool,
    pub(crate) pmgr_parent_device_id: u16,
}

pub(crate) const G17P_ASC_POWER_OPERANDS: [G17PAscPowerOperands; 2] = [
    G17PAscPowerOperands {
        role: G17PAscRole::Gfx,
        index: 0,
        clock_gate_id: 0xed,
        power_gate_id: 0xed,
        clock_id_metadata: 0x15f,
        pmgr_device_name: "GFX-ASC",
        pmgr_device_has_power_state_register: false,
        pmgr_parent_device_id: 40,
    },
    G17PAscPowerOperands {
        role: G17PAscRole::Gfx1,
        index: 0,
        clock_gate_id: 0x141,
        power_gate_id: 0x141,
        clock_id_metadata: 0x15f,
        pmgr_device_name: "GFX-ASC1",
        pmgr_device_has_power_state_register: false,
        pmgr_parent_device_id: 40,
    },
];

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PSharedGfxPowerTopology {
    pub(crate) device_tree_im4p_sha256: &'static str,
    pub(crate) role_device_ids: [u16; 2],
    pub(crate) role_device_raw_flags: [u8; 2],
    pub(crate) shared_parent_device_id: u16,
    pub(crate) shared_parent_name: &'static str,
    pub(crate) shared_parent_parents: [u16; 2],
    pub(crate) pmgr_group: u8,
    pub(crate) pmgr_group_base: u64,
    pub(crate) parent_register_offset: u32,
    pub(crate) parent_register_address: u64,
    pub(crate) linux_genpd_label: &'static str,
}

pub(crate) const G17P_SHARED_GFX_POWER_TOPOLOGY: G17PSharedGfxPowerTopology =
    G17PSharedGfxPowerTopology {
        device_tree_im4p_sha256: "290a36da7dbf24d8ab45681bee4896352eee4f103c3a0d4b116bac6d6e74f41a",
        role_device_ids: [237, 321],
        role_device_raw_flags: [0x10, 0x10],
        shared_parent_device_id: 40,
        shared_parent_name: "GFX",
        shared_parent_parents: [29, 31],
        pmgr_group: 0,
        pmgr_group_base: 0x3007_00000,
        parent_register_offset: 0x1f0,
        parent_register_address: 0x3007_001f0,
        linux_genpd_label: "gfx",
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PAscPowerCallContract {
    pub(crate) provider_object_offset: u16,
    pub(crate) clock_count_offset: u16,
    pub(crate) clock_gate_array_offset: u16,
    pub(crate) power_count_offset: u16,
    pub(crate) power_gate_array_offset: u16,
}

pub(crate) const G17P_ASC_POWER_CALL_CONTRACT: G17PAscPowerCallContract =
    G17PAscPowerCallContract {
        provider_object_offset: 0x88,
        clock_count_offset: 0xa0,
        clock_gate_array_offset: 0xa8,
        power_count_offset: 0xb0,
        power_gate_array_offset: 0xb8,
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17AscPowerOffStep {
    ProviderEnableDeviceClockFalseIndexZero,
    ProviderEnableDevicePowerFalseNullStatusIndexZero,
}

/// `_disablePower()` calls the provider clock edge first and the provider
/// power edge second. Both select index zero; the power status pointer is null.
pub(crate) const G17_ASC_POWER_OFF_ORDER: [G17AscPowerOffStep; 2] = [
    G17AscPowerOffStep::ProviderEnableDeviceClockFalseIndexZero,
    G17AscPowerOffStep::ProviderEnableDevicePowerFalseNullStatusIndexZero,
];

pub(crate) fn validate_g17_asc_power_off_order(observed: &[G17AscPowerOffStep]) -> bool {
    observed == G17_ASC_POWER_OFF_ORDER
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17LinuxAscTeardownStep {
    ProviderDisableOutbox,
    ProviderDisableMailboxIrqSources,
    ProviderBoundedIdleProbe,
    ProviderRunCpuFalse,
    ClientRevokeFirmwareDmaPublication,
    ProviderSetMapperInactive,
    ProviderReleaseSharedGfxRuntimeReference,
    ProviderVerifySharedGfxParentPowerGatedIfLastConsumer,
    ProviderClearRunningState,
}

/// Reverse of the currently admitted primary-then-secondary acquisition.
/// A coordinated DRM client must stop both providers in this order before
/// either provider can release the shared GFX power-domain reference.
pub(crate) const G17P_LINUX_ROLE_TEARDOWN_ORDER: [G17PAscRole; 2] =
    [G17PAscRole::Gfx1, G17PAscRole::Gfx];

pub(crate) const G17_LINUX_ASC_TEARDOWN_ORDER: [G17LinuxAscTeardownStep; 9] = [
    G17LinuxAscTeardownStep::ProviderDisableOutbox,
    G17LinuxAscTeardownStep::ProviderDisableMailboxIrqSources,
    G17LinuxAscTeardownStep::ProviderBoundedIdleProbe,
    G17LinuxAscTeardownStep::ProviderRunCpuFalse,
    G17LinuxAscTeardownStep::ClientRevokeFirmwareDmaPublication,
    G17LinuxAscTeardownStep::ProviderSetMapperInactive,
    G17LinuxAscTeardownStep::ProviderReleaseSharedGfxRuntimeReference,
    G17LinuxAscTeardownStep::ProviderVerifySharedGfxParentPowerGatedIfLastConsumer,
    G17LinuxAscTeardownStep::ProviderClearRunningState,
];

pub(crate) fn validate_g17_linux_asc_teardown_order(observed: &[G17LinuxAscTeardownStep]) -> bool {
    observed == G17_LINUX_ASC_TEARDOWN_ORDER
}

pub(crate) const G17_PMGR_AUTO_ENABLE_BIT: u32 = 1 << 28;
pub(crate) const G17_PMGR_PS_ACTUAL_MASK: u32 = 0xf0;
pub(crate) const G17_PMGR_PS_TARGET_MASK: u32 = 0x0f;
pub(crate) const G17_PMGR_PS_PWRGATE: u32 = 0;

pub(crate) const fn g17_pmgr_parent_is_power_gated(register: u32) -> bool {
    register & G17_PMGR_AUTO_ENABLE_BIT == 0
        && register & G17_PMGR_PS_ACTUAL_MASK == G17_PMGR_PS_PWRGATE
        && register & G17_PMGR_PS_TARGET_MASK == G17_PMGR_PS_PWRGATE
}

/// Observations needed before a role may be called hardware-cold. Register
/// values alone are insufficient: the firmware mapping, mapper, runtime-PM
/// ownership, and the shared GFX parent's exact state need positive readback.
/// `None` therefore fails closed. There is deliberately no per-role clock or
/// power register: IDs 237 and 321 are virtual children of the same parent.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AscColdObservation {
    pub(crate) cpu_control: u32,
    pub(crate) cpu_status: u32,
    pub(crate) outbox_control: u32,
    pub(crate) irq_event_sources_disabled: [Option<bool>; 2],
    pub(crate) firmware_dma_unpublished: Option<bool>,
    pub(crate) mapper_inactive: Option<bool>,
    pub(crate) provider_runtime_reference_released: Option<bool>,
    pub(crate) all_shared_gfx_consumers_released: Option<bool>,
    pub(crate) shared_gfx_parent_register: Option<u32>,
}

pub(crate) fn g17_asc_hardware_cold(observed: G17AscColdObservation) -> bool {
    let status_mask =
        ASC_CPU_STATUS_RUNNING_BIT | ASC_CPU_STATUS_STOPPED_BIT | ASC_CPU_STATUS_IDLE_BIT;
    let stopped_idle = ASC_CPU_STATUS_STOPPED_BIT | ASC_CPU_STATUS_IDLE_BIT;

    observed.cpu_control & ASC_CPU_RUN_BIT == 0
        && observed.cpu_status & status_mask == stopped_idle
        && observed.outbox_control & ASCWRAP_V6_MAILBOX_ENABLE_BIT == 0
        && observed.irq_event_sources_disabled == [Some(true), Some(true)]
        && observed.firmware_dma_unpublished == Some(true)
        && observed.mapper_inactive == Some(true)
        && observed.provider_runtime_reference_released == Some(true)
        && observed.all_shared_gfx_consumers_released == Some(true)
        && observed
            .shared_gfx_parent_register
            .is_some_and(g17_pmgr_parent_is_power_gated)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PmgrTextEvidence {
    pub(crate) enable_device: u64,
    pub(crate) enable_device_gated: u64,
    pub(crate) sync_device_status_change: u64,
    pub(crate) was_device_disabled: u64,
    pub(crate) enable_cluster: u64,
    pub(crate) read_reg32: u64,
    pub(crate) wait_reg32: u64,
    pub(crate) platform_write_reg32: u64,
}

pub(crate) const fn g17_pmgr_text_evidence(target: G17AscEvidenceTarget) -> G17PmgrTextEvidence {
    match target {
        G17AscEvidenceTarget::G17P => G17PmgrTextEvidence {
            enable_device: 0xffff_fe00_0972_4980,
            enable_device_gated: 0xffff_fe00_0972_4b18,
            sync_device_status_change: 0xffff_fe00_0972_575c,
            was_device_disabled: 0xffff_fe00_0972_6288,
            enable_cluster: 0xffff_fe00_0971_a3e4,
            read_reg32: 0xffff_fe00_0971_473c,
            wait_reg32: 0xffff_fe00_0971_497c,
            platform_write_reg32: 0xffff_fe00_09b3_c80c,
        },
        G17AscEvidenceTarget::G17G => G17PmgrTextEvidence {
            enable_device: 0xffff_fe00_09b2_a920,
            enable_device_gated: 0xffff_fe00_09b2_aab8,
            sync_device_status_change: 0xffff_fe00_09b2_b6fc,
            was_device_disabled: 0xffff_fe00_09b2_c228,
            enable_cluster: 0xffff_fe00_09b2_0384,
            read_reg32: 0xffff_fe00_09b1_a6dc,
            wait_reg32: 0xffff_fe00_09b1_a91c,
            platform_write_reg32: 0xffff_fe00_09f7_c230,
        },
        G17AscEvidenceTarget::G17S => G17PmgrTextEvidence {
            enable_device: 0xffff_fe00_097b_9c90,
            enable_device_gated: 0xffff_fe00_097b_9e28,
            sync_device_status_change: 0xffff_fe00_097b_aa6c,
            was_device_disabled: 0xffff_fe00_097b_b598,
            enable_cluster: 0xffff_fe00_097a_f6f4,
            read_reg32: 0xffff_fe00_097a_9a4c,
            wait_reg32: 0xffff_fe00_097a_9c8c,
            platform_write_reg32: 0xffff_fe00_09bf_238c,
        },
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17AscPowerOnStep {
    ProviderEnableDeviceClockIndexZero,
    PmgrEnableDevice,
    ResolveFlag16ClusterDevice,
    SyncDeviceStatusChange,
    ResolvePhysicalCluster,
    ReadClusterRegister,
    WriteClusterRegisterThroughPlatformBackend,
    WaitClusterRegister,
    ReadClusterPowerState,
    ProviderEnableDevicePowerStatusQueryIndexZero,
}

/// Cold enable route for the scoped flag-16 ASC PMGR devices when no cluster
/// object is already active. Warm/refcounted and disable routes are distinct
/// branches and are intentionally not inferred from this sequence.
pub(crate) const G17_ASC_POWER_ON_ORDER: [G17AscPowerOnStep; 10] = [
    G17AscPowerOnStep::ProviderEnableDeviceClockIndexZero,
    G17AscPowerOnStep::PmgrEnableDevice,
    G17AscPowerOnStep::ResolveFlag16ClusterDevice,
    G17AscPowerOnStep::SyncDeviceStatusChange,
    G17AscPowerOnStep::ResolvePhysicalCluster,
    G17AscPowerOnStep::ReadClusterRegister,
    G17AscPowerOnStep::WriteClusterRegisterThroughPlatformBackend,
    G17AscPowerOnStep::WaitClusterRegister,
    G17AscPowerOnStep::ReadClusterPowerState,
    G17AscPowerOnStep::ProviderEnableDevicePowerStatusQueryIndexZero,
];

pub(crate) const G17_CLUSTER_WAIT_MASK: u32 = 0x10;
pub(crate) const G17_CLUSTER_WAIT_VALUE: u32 = 0;

pub(crate) fn validate_g17_asc_power_on_order(observed: &[G17AscPowerOnStep]) -> bool {
    observed == G17_ASC_POWER_ON_ORDER
}

/// The cluster table supplies these operands at runtime. DeviceTree gate IDs
/// alone cannot derive them, so an executor must return an error for an
/// incomplete set.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PmgrClusterOperands {
    pub(crate) cluster_index: Option<u8>,
    pub(crate) register_map: Option<u8>,
    pub(crate) register_offset: Option<u32>,
    pub(crate) timeout: Option<u32>,
}

pub(crate) const fn g17_pmgr_cluster_operands_complete(operands: G17PmgrClusterOperands) -> bool {
    operands.cluster_index.is_some()
        && operands.register_map.is_some()
        && operands.register_offset.is_some()
        && operands.timeout.is_some()
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum A7IopStartStep {
    RtBuddySetPowerState3,
    RequireNotRunning,
    IfNormalEnablePowerLateRequireZero,
    IfMapperFlagSetMapperActive,
    IfFirmwareAndNormalMapFirmware,
    SetRunningFlag,
    IfMailboxSetupQueryApInitializes,
    IfMailboxSetupAndApInitializesDisableAllInterrupts,
    IfMailboxSetupAndApInitializesEnableInboxInterrupt,
    IfMailboxSetupAndApInitializesEnableOutboxInterrupt,
    IfMailboxSetupEnableInboxEventSource,
    IfMailboxSetupEnableOutboxEventSource,
    IfMailboxSetupAndApInitializesEnableOutbox,
    IfRequestedSyncIopTimebase,
    IfNormalCreateBootCompletion,
    IfNormalRunCpu,
}

pub(crate) const G17_A7IOP_START_ORDER: [A7IopStartStep; 16] = [
    A7IopStartStep::RtBuddySetPowerState3,
    A7IopStartStep::RequireNotRunning,
    A7IopStartStep::IfNormalEnablePowerLateRequireZero,
    A7IopStartStep::IfMapperFlagSetMapperActive,
    A7IopStartStep::IfFirmwareAndNormalMapFirmware,
    A7IopStartStep::SetRunningFlag,
    A7IopStartStep::IfMailboxSetupQueryApInitializes,
    A7IopStartStep::IfMailboxSetupAndApInitializesDisableAllInterrupts,
    A7IopStartStep::IfMailboxSetupAndApInitializesEnableInboxInterrupt,
    A7IopStartStep::IfMailboxSetupAndApInitializesEnableOutboxInterrupt,
    A7IopStartStep::IfMailboxSetupEnableInboxEventSource,
    A7IopStartStep::IfMailboxSetupEnableOutboxEventSource,
    A7IopStartStep::IfMailboxSetupAndApInitializesEnableOutbox,
    A7IopStartStep::IfRequestedSyncIopTimebase,
    A7IopStartStep::IfNormalCreateBootCompletion,
    A7IopStartStep::IfNormalRunCpu,
];

pub(crate) fn validate_a7iop_start_order(observed: &[A7IopStartStep]) -> bool {
    observed == G17_A7IOP_START_ORDER
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum A7IopStopStep {
    RequireRunning,
    IfMailboxSetupDisableOutbox,
    IfMailboxSetupDisableMailboxInterrupts,
    IfWrapperMmioBoundedIdleProbe,
    RunCpuFalse,
    UnmapFirmware,
    IfMapperFlagClearMapperActive,
    UnlessPowerRetainedTurnOffDomain,
    ClearRunningFlag,
}

/// Owner-invoked `stopCPU(bool, bool)` teardown order. When wrapper MMIO is
/// enabled, `_isIdle` is called once and then up to ten more times after 1 ms
/// delays. A still-busy result emits a diagnostic but teardown continues.
pub(crate) const G17_A7IOP_STOP_ORDER: [A7IopStopStep; 9] = [
    A7IopStopStep::RequireRunning,
    A7IopStopStep::IfMailboxSetupDisableOutbox,
    A7IopStopStep::IfMailboxSetupDisableMailboxInterrupts,
    A7IopStopStep::IfWrapperMmioBoundedIdleProbe,
    A7IopStopStep::RunCpuFalse,
    A7IopStopStep::UnmapFirmware,
    A7IopStopStep::IfMapperFlagClearMapperActive,
    A7IopStopStep::UnlessPowerRetainedTurnOffDomain,
    A7IopStopStep::ClearRunningFlag,
];

pub(crate) const A7IOP_STOP_IDLE_PROBE_MAX_CALLS: u8 = 11;
pub(crate) const A7IOP_STOP_IDLE_RETRY_DELAY_US: u32 = 1_000;

pub(crate) fn validate_a7iop_stop_order(observed: &[A7IopStopStep]) -> bool {
    observed == G17_A7IOP_STOP_ORDER
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RtBuddyStopOwnerContract {
    pub(crate) provider_object_offset: u16,
    pub(crate) stop_argument: bool,
}

pub(crate) const G17_RTBUDDY_STOP_OWNER_CONTRACT: RtBuddyStopOwnerContract =
    RtBuddyStopOwnerContract {
        provider_object_offset: 0xb0,
        stop_argument: true,
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct A7IopSoftwareResetState {
    pub(crate) state_offset: u16,
    pub(crate) state_value: u8,
    pub(crate) running_offset: u16,
    pub(crate) completion_offset: u16,
    pub(crate) performs_mmio: bool,
}

pub(crate) const G17_A7IOP_SOFTWARE_RESET_STATE: A7IopSoftwareResetState =
    A7IopSoftwareResetState {
        state_offset: 0x148,
        state_value: 8,
        running_offset: 0x108,
        completion_offset: 0x160,
        performs_mmio: false,
    };

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PResetFunctionEvidence {
    pub(crate) constructor_call_site: u64,
    pub(crate) property_name: &'static str,
    pub(crate) object_offset: u16,
    pub(crate) role_property_present: [bool; 2],
    pub(crate) stop_invokes_function: bool,
    pub(crate) reset_state_invokes_function: bool,
}

pub(crate) const G17P_RESET_FUNCTION_EVIDENCE: G17PResetFunctionEvidence =
    G17PResetFunctionEvidence {
        constructor_call_site: 0xffff_fe00_08ab_ba64,
        property_name: "function-device_reset",
        object_offset: 0x110,
        role_property_present: [false, false],
        stop_invokes_function: false,
        reset_state_invokes_function: false,
    };

pub(crate) const EXECUTABLE_G17_HARDWARE_RESET_AVAILABLE: bool = false;

pub(crate) const G17_HOST_WAIT_QUANTUM_ARG0: u32 = 50;
pub(crate) const G17_HOST_WAIT_QUANTUM_ARG1: u32 = 1_000_000;
pub(crate) const G17_HOST_WAIT_CONTINUE_RESULT: u32 = 1;
pub(crate) const G17_HOST_WAIT_SCALE: u64 = 1_000;
pub(crate) const G17_HOST_WAIT_MINIMUM: u64 = 50;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum HostWaitError {
    CounterOverflow,
}

/// Return the exact host counter limit.  A non-zero diagnostic override
/// replaces the configured value before multiplication and selects the fatal
/// deadline behavior below.
pub(crate) const fn host_wait_counter_limit(
    configured: u64,
    diagnostic_override: u64,
) -> Result<u64, HostWaitError> {
    let selected = if diagnostic_override != 0 {
        diagnostic_override
    } else {
        configured
    };
    match selected.checked_mul(G17_HOST_WAIT_SCALE) {
        Some(value) if value >= G17_HOST_WAIT_MINIMUM => Ok(value),
        Some(_) => Ok(G17_HOST_WAIT_MINIMUM),
        None => Err(HostWaitError::CounterOverflow),
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum HostWaitAction {
    Started,
    Continue,
    ReturnFalse,
    FatalTimeout,
}

pub(crate) const fn classify_host_wait_result(
    combined_started: bool,
    event_wait_result: u32,
) -> HostWaitAction {
    if combined_started {
        HostWaitAction::Started
    } else if event_wait_result == G17_HOST_WAIT_CONTINUE_RESULT {
        HostWaitAction::Continue
    } else {
        HostWaitAction::ReturnFalse
    }
}

pub(crate) const fn classify_host_wait_deadline(diagnostic_override: u64) -> HostWaitAction {
    if diagnostic_override == 0 {
        HostWaitAction::ReturnFalse
    } else {
        HostWaitAction::FatalTimeout
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscBringupPhase {
    Off,
    PowerStateRequested,
    FirmwarePublished,
    PowerLateAccepted,
    IorvbarProgrammed,
    RunningMarked,
    CpuReleased,
    Failed,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscBringupError {
    WrongOrder,
    WrongPowerState,
    FirmwarePublicationTraceMismatch,
    EnablePowerLateFailed(u32),
    Iorvbar(IorvbarError),
    RunCpuTraceMismatch,
    WrapperMmioDisabled,
    AlreadyFailed,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AscRollbackRequirement {
    None,
    /// The AGX host failure paths do not call stopCPU.  An owner must invoke
    /// the separately recovered stop sequence before retrying or releasing
    /// resources.
    OwnerStopCpuRequired,
}

/// Essential ASC launch observer. It records already-observed
/// actions and permanently fails on an ordering or value mismatch.
#[derive(Debug, Copy, Clone)]
pub(crate) struct G17AscBringup {
    phase: AscBringupPhase,
    mutated: bool,
}

impl G17AscBringup {
    pub(crate) const fn new() -> Self {
        Self {
            phase: AscBringupPhase::Off,
            mutated: false,
        }
    }

    pub(crate) const fn phase(&self) -> AscBringupPhase {
        self.phase
    }

    fn fail(&mut self, error: AscBringupError) -> Result<(), AscBringupError> {
        self.phase = AscBringupPhase::Failed;
        Err(error)
    }

    pub(crate) fn observe_rtbuddy_power_state(&mut self, state: u8) -> Result<(), AscBringupError> {
        if self.phase == AscBringupPhase::Failed {
            return Err(AscBringupError::AlreadyFailed);
        }
        if self.phase != AscBringupPhase::Off {
            return self.fail(AscBringupError::WrongOrder);
        }
        if state != 3 {
            return self.fail(AscBringupError::WrongPowerState);
        }
        self.mutated = true;
        self.phase = AscBringupPhase::PowerStateRequested;
        Ok(())
    }

    pub(crate) fn record_enable_power_late(&mut self, result: u32) -> Result<(), AscBringupError> {
        if self.phase == AscBringupPhase::Failed {
            return Err(AscBringupError::AlreadyFailed);
        }
        if self.phase != AscBringupPhase::FirmwarePublished {
            return self.fail(AscBringupError::WrongOrder);
        }
        if result != 0 {
            return self.fail(AscBringupError::EnablePowerLateFailed(result));
        }
        self.phase = AscBringupPhase::PowerLateAccepted;
        Ok(())
    }

    pub(crate) fn observe_firmware_publication(
        &mut self,
        observed: &[RtBuddyPublicationOp],
    ) -> Result<(), AscBringupError> {
        if self.phase == AscBringupPhase::Failed {
            return Err(AscBringupError::AlreadyFailed);
        }
        if self.phase != AscBringupPhase::PowerStateRequested {
            return self.fail(AscBringupError::WrongOrder);
        }
        if !validate_rtbuddy_publication_order(observed) {
            return self.fail(AscBringupError::FirmwarePublicationTraceMismatch);
        }
        self.phase = AscBringupPhase::FirmwarePublished;
        Ok(())
    }

    pub(crate) fn observe_iorvbar_write(
        &mut self,
        firmware_address: u64,
        observed: u64,
    ) -> Result<(), AscBringupError> {
        if self.phase == AscBringupPhase::Failed {
            return Err(AscBringupError::AlreadyFailed);
        }
        if self.phase != AscBringupPhase::PowerLateAccepted {
            return self.fail(AscBringupError::WrongOrder);
        }
        if let Err(e) = validate_iorvbar_write(firmware_address, observed) {
            return self.fail(AscBringupError::Iorvbar(e));
        }
        self.phase = AscBringupPhase::IorvbarProgrammed;
        Ok(())
    }

    pub(crate) fn observe_running_marked(&mut self) -> Result<(), AscBringupError> {
        if self.phase == AscBringupPhase::Failed {
            return Err(AscBringupError::AlreadyFailed);
        }
        if self.phase != AscBringupPhase::IorvbarProgrammed {
            return self.fail(AscBringupError::WrongOrder);
        }
        self.phase = AscBringupPhase::RunningMarked;
        Ok(())
    }

    pub(crate) fn observe_run_cpu_trace(
        &mut self,
        wrapper_mmio_enabled: bool,
        observed: &AscMmioPlan,
    ) -> Result<(), AscBringupError> {
        if self.phase == AscBringupPhase::Failed {
            return Err(AscBringupError::AlreadyFailed);
        }
        if self.phase != AscBringupPhase::RunningMarked {
            return self.fail(AscBringupError::WrongOrder);
        }
        if !wrapper_mmio_enabled {
            return self.fail(AscBringupError::WrapperMmioDisabled);
        }
        if *observed != asc_run_cpu_plan(true, true) {
            return self.fail(AscBringupError::RunCpuTraceMismatch);
        }
        self.phase = AscBringupPhase::CpuReleased;
        Ok(())
    }

    pub(crate) fn record_host_wait_failure(&mut self) {
        if self.phase != AscBringupPhase::Off {
            self.phase = AscBringupPhase::Failed;
        }
    }

    pub(crate) const fn rollback_requirement(&self) -> AscRollbackRequirement {
        if self.mutated {
            AscRollbackRequirement::OwnerStopCpuRequired
        } else {
            AscRollbackRequirement::None
        }
    }
}

/// Named live gaps.  Static recovery of a register transform is not ownership
/// to execute it from Linux.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17AscLiveGaps {
    pub(crate) pmgr_set_power_state_gate_order_owned: bool,
    pub(crate) pmgr_cluster_runtime_operands_proven: bool,
    pub(crate) asc_control_aperture_owned: bool,
    pub(crate) iorvbar_aperture_and_dma_visibility_owned: bool,
    pub(crate) firmware_publication_cache_operation_proven: bool,
    pub(crate) wrapper_mmio_enable_source_proven: bool,
    pub(crate) teardown_owner_identified: bool,
    pub(crate) outbox_disable_transform_proven: bool,
    pub(crate) irq_event_source_disable_order_proven: bool,
    pub(crate) mapper_inactive_call_abi_proven: bool,
    pub(crate) power_off_call_operands_proven: bool,
    pub(crate) shared_gfx_pmgr_topology_proven: bool,
    pub(crate) firmware_dma_owner_handoff_wired: bool,
    pub(crate) power_off_linux_owner_wired: bool,
    pub(crate) cold_state_readback_complete: bool,
    pub(crate) hardware_reset_function_available: bool,
    pub(crate) stop_cpu_teardown_owner_wired: bool,
}

pub(crate) const G17_ASC_LIVE_GAPS: G17AscLiveGaps = G17AscLiveGaps {
    pmgr_set_power_state_gate_order_owned: false,
    pmgr_cluster_runtime_operands_proven: false,
    asc_control_aperture_owned: true,
    iorvbar_aperture_and_dma_visibility_owned: false,
    firmware_publication_cache_operation_proven: true,
    wrapper_mmio_enable_source_proven: true,
    teardown_owner_identified: true,
    outbox_disable_transform_proven: true,
    irq_event_source_disable_order_proven: true,
    mapper_inactive_call_abi_proven: true,
    power_off_call_operands_proven: true,
    shared_gfx_pmgr_topology_proven: true,
    firmware_dma_owner_handoff_wired: false,
    power_off_linux_owner_wired: false,
    cold_state_readback_complete: false,
    hardware_reset_function_available: false,
    stop_cpu_teardown_owner_wired: true,
};

pub(crate) const EXECUTABLE_G17_ASC_BRINGUP_AVAILABLE: bool = false;

