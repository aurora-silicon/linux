// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


#[cfg(not(test))]
use crate::g17_manager::G17SksmScratchGeometry;
#[cfg(test)]
#[path = "g17_manager.rs"]
mod g17_manager;
#[cfg(test)]
use g17_manager::G17SksmScratchGeometry;

/// The submission producer subset is byte-grounded against the M5 firmware.
/// See [`SUBMISSION_ABI_LEDGER`] for the inherited, still-unconfirmed pieces.
pub(crate) const GROUNDED_SUBMISSION_PRODUCER_AVAILABLE: bool = true;

/// Whether a submission can be driven to completion. `false`: see
/// [`SUBMISSION_LIVE_GAPS`]. This keeps `SUBMISSION_TRANSPORT` out of
/// `g17_boot::pre_handoff_gate` admission until completion is implemented.
pub(crate) const EXECUTABLE_SUBMISSION_TRANSPORT_AVAILABLE: bool =
    SUBMISSION_LIVE_GAPS.all_proven();

// ---------------------------------------------------------------------------
// 1. EP 0x21 TX work-submission doorbell — byte-identical to AGX2.
//    Grounded from the firmware doorbell decoder at GFX 0xfffffc00000251cc /
//    the TX case 0xfffffc0000025278 (see the research note §1).
// ---------------------------------------------------------------------------

/// GPU doorbell endpoint (gfx_ints). Matches `gpu.rs` `EP_DOORBELL`.
pub(crate) const EP_DOORBELL: u8 = 0x21;

/// TX (channel-kick) doorbell tag, `0x83 << 48`. Matches `gpu.rs`
/// `MSG_TX_DOORBELL`.
const MSG_TX_DOORBELL: u64 = 0x83 << 48;

/// The firmware reads a 6-bit doorbell type from bits [53:48]
/// (`ubfx x8, x1, #48, #6` @ 0x251dc).
const DOORBELL_TYPE_SHIFT: u32 = 48;
const DOORBELL_TYPE_BITS: u32 = 6;
const DOORBELL_TYPE_MASK: u64 = (1 << DOORBELL_TYPE_BITS) - 1;

/// `0x83 & 0x3f = 3`: the channel-kick (TX) doorbell case (`b.eq 0x25278`).
pub(crate) const DOORBELL_TYPE_TX: u8 = 3;
/// `0x84 & 0x3f = 4`: the firmware-control doorbell case (`b.eq 0x25268`).
pub(crate) const DOORBELL_TYPE_FWCTL: u8 = 4;
/// `0x85 & 0x3f = 5`: the halt doorbell case.
pub(crate) const DOORBELL_TYPE_HALT: u8 = 5;

/// TX pipe field is `msg & 0x3` (`and w8, w1, #0x3` @ 0x25278).
const DOORBELL_PIPE_MASK: u64 = 0x3;
/// TX index field is bits [4:2] (`tst x1, #0x1c` @ 0x2527c; `index << 2`).
const DOORBELL_INDEX_SHIFT: u32 = 2;
const DOORBELL_INDEX_MASK: u64 = 0x7;

/// Well-known TX-doorbell payloads used for firmware wakeups, matching
/// `gpu.rs` `DOORBELL_KICKFW` / `DOORBELL_DEVCTRL`.
pub(crate) const DOORBELL_KICKFW: u8 = 0x10;
pub(crate) const DOORBELL_DEVCTRL: u8 = 0x11;

/// The three GPU submission pipes. The firmware treats `pipe_type == 3` as invalid
/// (`cmp w8, #0x3 ; b.eq 0x252f4` — no such pipe).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum PipeType {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
}

/// Errors from the submission codec.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum SubmissionError {
    /// A doorbell index outside the 3-bit [4:2] field.
    DoorbellIndexOutOfRange,
    /// A doorbell whose type field is not the TX (channel-kick) type.
    NotATxDoorbell,
    /// A doorbell that decodes to the reserved pipe value 3.
    ReservedPipe,
    /// A kick word with bits set outside the modeled 47-bit form.
    KickWordUnmodeledBits,
    /// A queue id outside the 7-bit [46:40] field / 128-slot table.
    QueueIdOutOfRange,
    /// A SKSM timestamp outside the 40-bit [39:0] field.
    KickTimestampOutOfRange,
    /// The runtime queue geometry must contain between 1 and 256 stamps.
    KickStampCountOutOfRange,
    KickQueueGeometryUnconfirmed,
    /// The queue stride/count/alignment/extent/seed tuple is inconsistent.
    KickQueueGeometryInvalid,
    /// The timestamp's low-byte slot does not exist in this queue geometry.
    KickStampIndexOutOfRange,
    /// A zero-byte entry stride would alias every queue entry.
    KickEntryStrideZero,
    /// Multiplying the slot by the runtime entry stride overflowed u32.
    KickEntryOffsetOverflow,
    /// The queue already has `num_stamps` entries awaiting completion.
    KickQueueSubmittedFull,
    /// A completion observed no submitted entries or exceeded their count.
    KickCompletionUnderflow,
    /// A provider-owned pre-B2 state object is smaller than one 16 KiB page.
    KickB2PrimaryStateBufferTooSmall,
    /// The retained queue already published its one-time pre-B2 state.
    KickB2PrimaryStateAlreadyPublished,
    KickRetainedStorageBufferTooSmall,
    /// A CL entry has one implicit dependency and at most 31 explicit ones.
    KickDependencyCountOutOfRange,
    /// A CL dependency names a queue outside the seven-bit entry field.
    KickDependencyQueueIdOutOfRange,
    /// The MCache aperture index or count cannot be represented in its field.
    KickMcacheFieldOutOfRange,
    /// The MCache aperture address is unaligned or exceeds its 38-bit field.
    KickMcacheAddressUnencodable,
    /// The RCE kind is outside the two-bit entry field.
    KickRceKindOutOfRange,
    /// An RCE tag is outside its seven-bit entry field.
    KickRceTagOutOfRange,
    /// An RCE address is unaligned or exceeds its 38-bit entry field.
    KickRceAddressUnencodable,
    /// A compute descriptor-relative RCE address overflowed u64.
    ComputeRceAddressOverflow,
    /// A compute event counter overflowed its 64-bit entry field.
    ComputeEventCounterOverflow,
    /// Required first-bind command publication facts are still unresolved.
    FirstBindCommandPublicationIncomplete,
    /// G17P target-1 requires both retained CPU and booted RTKit owners.
    RunningClientPowerTransitionUnavailable,
    /// The middle QoS field is wider than the five bits copied by the host.
    KickQosClassOutOfRange,
    /// The runtime MMIO aperture selector or base offset overflowed u32.
    KickMmioOffsetOverflow,
    /// The A18 Pro SKSM queue data-master field is limited to TA/3D/CL.
    KickQueueDataMasterOutOfRange,
    /// The A18 Pro SKSM completion-queue selector is outside the host range.
    KickQueueCompletionSelectorOutOfRange,
    /// The A18 Pro SKSM entry address cannot be represented by the host mask.
    KickQueueEntryAddressUnencodable,
    /// The A18 Pro completion descriptor ordinal is outside its four slots.
    CompletionQueueOrdinalOutOfRange,
    /// A present A18 Pro completion queue cannot have zero entries.
    CompletionQueueCapacityZero,
    /// A ring with zero entries cannot advance a producer index.
    RingSizeZero,
    /// A producer index must identify an entry inside its ring.
    RingIndexOutOfRange,
    /// Advancing the producer would make it collide with the consumer.
    RingFull,
    /// G17 exposes four priority groups in its runtime channel table.
    ChannelPriorityOutOfRange,
    ChannelAbiUnconfirmed,
    /// The top-level channel-command tag ABI is not grounded for this target.
    CommandAbiUnconfirmed,
    /// A top-level channel command must expose its complete four-byte tag.
    CommandHeaderTooShort,
    /// A first-render status pointer overflowed while deriving the FWCtl VAs.
    PartialOpeningAddressOverflow,
    /// A first-render scheduler or index mapping is not exactly one 16 KiB page.
    PartialOpeningPageSize,
    /// A first-render outer-channel counter has bits outside the 8-bit ring index.
    PartialOpeningChannelCounterOutOfRange,
    /// A first-render queue head does not fit the outer slot's 16-bit field.
    PartialOpeningQueueHeadOutOfRange,
    /// A cold-opening support object does not match its exact encoded extent.
    PartialOpeningGraphObjectSize,
    /// A first-partial descriptor cannot be represented by its context locators.
    PartialOpeningDescriptorLocatorUnencodable,
    /// The host/firmware meaning of this in-range top-level tag is unresolved.
    CommandTagUnresolved,
    /// This value is not a grounded G17P top-level command tag.
    CommandTagInvalid,
    /// The host names this tag, but the matched firmware handles it fatally.
    CommandTagInvalidForFirmware,
    /// A tag-14 AddKicks command must publish at least one new kick entry.
    KsmAddKicksCountZero,
    /// The tag-14 AddKicks count does not fit the record's 16-bit field.
    KsmAddKicksCountOutOfRange,
}

/// Encode a work-submission doorbell exactly as `gpu.rs::run_job` does:
/// `MSG_TX_DOORBELL | pipe_type | (index << 2)`.
///
/// `index` is the queue priority (0..=7 fits the firmware's [4:2] field).
pub(crate) const fn encode_tx_doorbell(pipe: PipeType, index: u8) -> Result<u64, SubmissionError> {
    if index as u64 > DOORBELL_INDEX_MASK {
        return Err(SubmissionError::DoorbellIndexOutOfRange);
    }
    Ok(MSG_TX_DOORBELL | (pipe as u64) | ((index as u64) << DOORBELL_INDEX_SHIFT))
}

/// Host state which decides the two distinct EP 0x21 notifications around a
/// direct G17P CL submit. Activation is first-or-changed-channel state; the
/// direct kick has its own feature, suppression, and command-enable gates.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PDirectClNotificationState {
    pub(crate) cached_channel_identity: u64,
    pub(crate) submitted_channel_identity: u64,
    pub(crate) firmware_ready: bool,
    pub(crate) direct_feature: bool,
    pub(crate) submission_suppressed: bool,
    pub(crate) command_submission_enabled: bool,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PDirectClNotifications {
    pub(crate) activation: Option<u64>,
    pub(crate) direct_kick: Option<u64>,
}

/// Submission backend identity. This keeps the T8140 G17P HAL200 hybrid
/// distinct from both classic channels and the unimplemented HAL300 family
/// without changing mature generic paths.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17ClSubmissionBackend {
    Classic,
    G17PHal200Hybrid,
    Hal300,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClBackendConfig {
    pub(crate) backend: G17ClSubmissionBackend,
    pub(crate) direct_feature: bool,
    pub(crate) submission_suppressed: bool,
    pub(crate) command_submission_enabled: bool,
}

pub(crate) const T8140_G17P_CL_BACKEND: G17PClBackendConfig = G17PClBackendConfig {
    backend: G17ClSubmissionBackend::G17PHal200Hybrid,
    direct_feature: true,
    submission_suppressed: false,
    command_submission_enabled: true,
};

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PContractPredicateState {
    Pending,
    ProvenPresent,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PFlushIdImplementationState {
    Pending,
    ProvenAbsentFallbackUnverified,
    ProvenAbsentFallbackRepresented,
    ProvenPresentUnimplemented,
    ProvenPresentImplemented,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PCorrelationRingImplementationState {
    Pending,
    ProvenInactive,
    ProvenRequiredImplemented,
}

/// Required first-bind commands and publication facts. FlushID uses represented
/// fallback zero, the command-pointer ring is implemented, and the taken
/// correlation branch is represented by a host-only nonzero job token.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirstBindCommandPlan {
    pub(crate) config_update_required: bool,
    pub(crate) entry_signal_required: bool,
    pub(crate) logical_flush_id_required: bool,
    pub(crate) fallback_logical_flush_id: u32,
    pub(crate) flush_id_serialized_in_tag15: bool,
    pub(crate) flush_id_completion_release_required: bool,
    pub(crate) dedicated_flush_id: G17PFlushIdImplementationState,
    pub(crate) native_command_pointer_ring: G17PContractPredicateState,
    pub(crate) optional_correlation_ring: G17PCorrelationRingImplementationState,
}

impl G17PFirstBindCommandPlan {
    pub(crate) const fn hardware_release_ready(self) -> bool {
        self.config_update_required
            && self.entry_signal_required
            && self.logical_flush_id_required
            && self.fallback_logical_flush_id == 0
            && !self.flush_id_serialized_in_tag15
            && !self.flush_id_completion_release_required
            && matches!(
                self.dedicated_flush_id,
                G17PFlushIdImplementationState::ProvenAbsentFallbackRepresented
                    | G17PFlushIdImplementationState::ProvenPresentImplemented
            )
            && matches!(
                self.native_command_pointer_ring,
                G17PContractPredicateState::ProvenPresent
            )
            && matches!(
                self.optional_correlation_ring,
                G17PCorrelationRingImplementationState::ProvenInactive
                    | G17PCorrelationRingImplementationState::ProvenRequiredImplemented
            )
    }
}

pub(crate) const G17P_FIRST_BIND_COMMAND_PLAN: G17PFirstBindCommandPlan =
    G17PFirstBindCommandPlan {
        config_update_required: true,
        entry_signal_required: true,
        logical_flush_id_required: true,
        fallback_logical_flush_id: 0,
        flush_id_serialized_in_tag15: false,
        flush_id_completion_release_required: false,
        dedicated_flush_id:
            G17PFlushIdImplementationState::ProvenAbsentFallbackRepresented,
        native_command_pointer_ring: G17PContractPredicateState::ProvenPresent,
        optional_correlation_ring:
            G17PCorrelationRingImplementationState::ProvenRequiredImplemented,
    };

/// Compile-time release gate consumed by the live first-add3 boundary. Keep
/// true only because the host-only correlation token, tag-15/tag-16 pointer
/// publisher, and FlushID fallback are all represented for this profile.
pub(crate) const G17P_FIRST_BIND_DIRECT_HARDWARE_RELEASE_READY: bool =
    G17P_FIRST_BIND_COMMAND_PLAN.hardware_release_ready();

pub(crate) const fn require_g17p_first_bind_direct_hardware_release(
    plan: G17PFirstBindCommandPlan,
) -> Result<(), SubmissionError> {
    if plan.hardware_release_ready() {
        Ok(())
    } else {
        Err(SubmissionError::FirstBindCommandPublicationIncomplete)
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PRunningClientPowerContract {
    pub(crate) role_index: u8,
    pub(crate) service_id: u8,
    pub(crate) target: u8,
    pub(crate) management_request_used: bool,
    pub(crate) managed: bool,
    pub(crate) standalone: bool,
}

pub(crate) const G17P_RUNNING_CLIENT_POWER_CONTRACT: G17PRunningClientPowerContract =
    G17PRunningClientPowerContract {
        role_index: 1,
        service_id: 0x20,
        target: 1,
        management_request_used: false,
        managed: true,
        standalone: false,
    };

pub(crate) const fn require_g17p_running_client_power_transition(
    contract: G17PRunningClientPowerContract,
    retained_cpu_owner: bool,
    retained_rtkit_owner: bool,
) -> Result<(), SubmissionError> {
    if contract.role_index == 1
        && contract.service_id == 0x20
        && contract.target == 1
        && !contract.management_request_used
        && contract.managed
        && !contract.standalone
        && retained_cpu_owner
        && retained_rtkit_owner
    {
        Ok(())
    } else {
        Err(SubmissionError::RunningClientPowerTransitionUnavailable)
    }
}

/// Required lifetime of the eventual target-1 active lease.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PComputePowerLeaseStep {
    AcquireBeforeReadiness,
    HoldThroughClaim,
    HoldThroughCompletion,
    ReleaseAfterCompletion,
}

pub(crate) const G17P_COMPUTE_POWER_LEASE_ORDER: [G17PComputePowerLeaseStep; 4] = [
    G17PComputePowerLeaseStep::AcquireBeforeReadiness,
    G17PComputePowerLeaseStep::HoldThroughClaim,
    G17PComputePowerLeaseStep::HoldThroughCompletion,
    G17PComputePowerLeaseStep::ReleaseAfterCompletion,
];

pub(crate) const fn g17p_first_bind_tag15_required(queue_config_dirty: bool) -> bool {
    queue_config_dirty
}

pub(crate) const fn g17p_first_bind_tag16_required(
    backend: G17PClBackendConfig,
    queue_config_dirty: bool,
) -> bool {
    matches!(backend.backend, G17ClSubmissionBackend::G17PHal200Hybrid)
        && backend.direct_feature
        && queue_config_dirty
}

/// Host-only first/repeat state for the direct CL submission contract.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PDirectClLifecycleState {
    pub(crate) queue_config_dirty: bool,
    pub(crate) cached_channel_identity: u64,
    /// Nonzero host-only descriptor sequence used for job ownership/fencing.
    pub(crate) next_correlation_token: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PDirectClLifecycle {
    pub(crate) publish_config_update: bool,
    pub(crate) publish_entry_signal: bool,
    pub(crate) correlation_token: u64,
    pub(crate) notifications: PreparedG17PDirectClNotifications,
    pub(crate) next: G17PDirectClLifecycleState,
}

/// Derive first-bind and repeat transitions without selecting publication
/// storage. First bind publishes tags 15/16; a clean repeat omits both and the
/// unchanged channel identity omits activation, while direct kick remains.
pub(crate) const fn prepare_g17p_direct_cl_lifecycle(
    current: G17PDirectClLifecycleState,
    submitted_channel_identity: u64,
    firmware_ready: bool,
    backend: G17PClBackendConfig,
) -> Result<PreparedG17PDirectClLifecycle, SubmissionError> {
    if current.next_correlation_token == 0 {
        return Err(SubmissionError::ComputeEventCounterOverflow);
    }
    let next_correlation_token = match current.next_correlation_token.checked_add(1) {
        Some(token) => token,
        None => return Err(SubmissionError::ComputeEventCounterOverflow),
    };
    let publish_config_update = g17p_first_bind_tag15_required(current.queue_config_dirty);
    let publish_entry_signal =
        g17p_first_bind_tag16_required(backend, current.queue_config_dirty);
    let notifications = match prepare_g17p_direct_cl_notifications(
        G17PDirectClNotificationState {
            cached_channel_identity: current.cached_channel_identity,
            submitted_channel_identity,
            firmware_ready,
            direct_feature: backend.direct_feature,
            submission_suppressed: backend.submission_suppressed,
            command_submission_enabled: backend.command_submission_enabled,
        },
    ) {
        Ok(notifications) => notifications,
        Err(error) => return Err(error),
    };
    Ok(PreparedG17PDirectClLifecycle {
        publish_config_update,
        publish_entry_signal,
        correlation_token: current.next_correlation_token,
        notifications,
        next: G17PDirectClLifecycleState {
            queue_config_dirty: false,
            cached_channel_identity: submitted_channel_identity,
            next_correlation_token,
        },
    })
}

/// First/new compute-channel activation. Priority 2 encodes as `0x...0a`;
/// this is independent of SKSM QID 4 and outer work-channel index 10.
pub(crate) const fn encode_g17p_cl_activation(priority: u8) -> Result<u64, SubmissionError> {
    encode_tx_doorbell(PipeType::Compute, priority)
}

pub(crate) const fn encode_g17p_direct_cl_kick() -> u64 {
    MSG_TX_DOORBELL | DOORBELL_KICKFW as u64
}

pub(crate) const G17P_NATIVE_PRE_USER_EP21_MESSAGES: [u64; 0] = [];
pub(crate) const G17P_NATIVE_FIRST_USER_EP21_MESSAGES: [u64; 2] = [
    0x0083_0000_0000_000a,
    0x0083_0000_0000_0010,
];

/// Pure predicate/encoding boundary. A repeat submit omits activation when the
/// cached identity already matches, but an enabled, direct, unsuppressed submit
/// still emits the separate direct kick.
pub(crate) const fn prepare_g17p_direct_cl_notifications(
    state: G17PDirectClNotificationState,
) -> Result<PreparedG17PDirectClNotifications, SubmissionError> {
    let activation = if state.cached_channel_identity != state.submitted_channel_identity
        && state.firmware_ready
    {
        match encode_g17p_cl_activation(2) {
            Ok(message) => Some(message),
            Err(error) => return Err(error),
        }
    } else {
        None
    };
    let direct_kick = if state.direct_feature
        && !state.submission_suppressed
        && state.command_submission_enabled
    {
        Some(encode_g17p_direct_cl_kick())
    } else {
        None
    };
    Ok(PreparedG17PDirectClNotifications {
        activation,
        direct_kick,
    })
}

/// Extract the firmware's 6-bit doorbell type from bits [53:48].
pub(crate) const fn doorbell_type(message: u64) -> u8 {
    ((message >> DOORBELL_TYPE_SHIFT) & DOORBELL_TYPE_MASK) as u8
}

/// Decode a TX doorbell to `(pipe_type, index)`, applying the firmware's exact
/// masks and its invalid `pipe == 3` outcome.
pub(crate) const fn decode_tx_doorbell(message: u64) -> Result<(PipeType, u8), SubmissionError> {
    if doorbell_type(message) != DOORBELL_TYPE_TX {
        return Err(SubmissionError::NotATxDoorbell);
    }
    let pipe = match message & DOORBELL_PIPE_MASK {
        0 => PipeType::Vertex,
        1 => PipeType::Fragment,
        2 => PipeType::Compute,
        _ => return Err(SubmissionError::ReservedPipe),
    };
    let index = ((message >> DOORBELL_INDEX_SHIFT) & DOORBELL_INDEX_MASK) as u8;
    Ok((pipe, index))
}

/// The firmware's pending-pipe bit for a submitted pipe: `1 << pipe_type`
/// (`mov w9,#1 ; lsl w8,w9,w8` in the pinned J700 A000 type-3 handler). This
/// path is distinct from the type-4 device-control bit 17 at scheduler root
/// `0xfffffc0000177190`.
pub(crate) const fn pending_pipe_bit(pipe: PipeType) -> u32 {
    1u32 << (pipe as u32)
}


/// AGX2 FW-owned consumer index.
pub(crate) const AGX2_CHANNEL_STATE_READ_PTR_OFFSET: u32 = 0x00;
/// AGX2 driver-owned producer index.
pub(crate) const AGX2_CHANNEL_STATE_WRITE_PTR_OFFSET: u32 = 0x20;
/// Total AGX2 `ChannelState` size.
pub(crate) const AGX2_CHANNEL_STATE_SIZE: u32 = 0x40;


/// First G17 channel descriptor in `runtime_pointers`.
pub(crate) const G17_CHANNEL_DESCRIPTOR_TABLE_OFFSET: u32 = 0x20;
/// Vertex, fragment, and compute descriptors per priority group.
pub(crate) const G17_CHANNEL_DESCRIPTORS_PER_PRIORITY: u32 = 3;
/// Priority groups scanned by the G17 firmware.
pub(crate) const G17_CHANNEL_PRIORITY_COUNT: u8 = 4;
/// Total descriptor count: four priorities times three submission pipes.
pub(crate) const G17_CHANNEL_DESCRIPTOR_COUNT: u32 = 12;
pub(crate) const G17_CHANNEL_DESCRIPTOR_STRIDE: u32 = 0x20;
/// G17 command-entry stride used by all three pipe rings.
pub(crate) const G17_CHANNEL_ENTRY_STRIDE: u32 = 0x18;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17ChannelDescriptorLayout {
    /// Pointer to the cursor advanced after an entry has been dispatched.
    pub(crate) dispatch_cursor: u32,
    /// Pointer to the cursor advanced after the entry range is made coherent.
    pub(crate) coherency_cursor: u32,
    /// Pointer to the producer cursor sampled before scanning the ring.
    pub(crate) producer_cursor: u32,
    /// GPU pointer to the first 0x18-byte command entry.
    pub(crate) entries: u32,
}

pub(crate) const G17_CHANNEL_DESCRIPTOR_LAYOUT: G17ChannelDescriptorLayout =
    G17ChannelDescriptorLayout {
        dispatch_cursor: 0x00,
        coherency_cursor: 0x08,
        producer_cursor: 0x10,
        entries: 0x18,
    };

pub(crate) const fn g17_channel_descriptor_offset(
    priority: u8,
    pipe: PipeType,
) -> Result<u32, SubmissionError> {
    if priority >= G17_CHANNEL_PRIORITY_COUNT {
        return Err(SubmissionError::ChannelPriorityOutOfRange);
    }

    let index = priority as u32 * G17_CHANNEL_DESCRIPTORS_PER_PRIORITY + pipe as u32;
    Ok(G17_CHANNEL_DESCRIPTOR_TABLE_OFFSET + index * G17_CHANNEL_DESCRIPTOR_STRIDE)
}


/// Full byte extent cleared by the G17 host for `_AGFIChannelState`.
pub(crate) const G17_HOST_CHANNEL_STATE_RESET_SIZE: usize = 0x80;
/// Last byte touched by the G17 host's uncached-channel reset stores.
pub(crate) const G17_UNCACHED_CHANNEL_RESET_WRITE_EXTENT: usize = 0x64;
/// All three in-scope host families use the identical reset layout.
pub(crate) const G17_HOST_CHANNEL_RESET_TARGETS_CONFIRMED: bool = true;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17HostChannelStateResetLayout {
    pub(crate) gpu_va_00: usize,
    pub(crate) gpu_va_08: usize,
    pub(crate) gpu_va_10: usize,
    pub(crate) zero_18: usize,
    pub(crate) zero_1c: usize,
    pub(crate) zero_20: usize,
    pub(crate) sentinel_24: usize,
    pub(crate) fixed_4_28: usize,
    pub(crate) sentinel_40: usize,
    pub(crate) word_44: usize,
    pub(crate) zero_58: usize,
    pub(crate) gpu_va_70: usize,
}

pub(crate) const G17_HOST_CHANNEL_STATE_RESET_LAYOUT: G17HostChannelStateResetLayout =
    G17HostChannelStateResetLayout {
        gpu_va_00: 0x00,
        gpu_va_08: 0x08,
        gpu_va_10: 0x10,
        zero_18: 0x18,
        zero_1c: 0x1c,
        zero_20: 0x20,
        sentinel_24: 0x24,
        fixed_4_28: 0x28,
        sentinel_40: 0x40,
        word_44: 0x44,
        zero_58: 0x58,
        gpu_va_70: 0x70,
    };

/// Runtime values copied by the host into the structural G17 reset image.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17HostChannelStateResetValues {
    pub(crate) gpu_va_00: u64,
    pub(crate) gpu_va_08: u64,
    pub(crate) gpu_va_10: u64,
    pub(crate) word_44: u32,
    pub(crate) gpu_va_70: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17UncachedChannelResetLayout {
    pub(crate) zero_00: usize,
    pub(crate) zero_10: usize,
    pub(crate) zero_20: usize,
    pub(crate) zero_30: usize,
    pub(crate) zero_40: usize,
    pub(crate) sentinel_50: usize,
    pub(crate) ring_entry_count_60: usize,
}

pub(crate) const G17_UNCACHED_CHANNEL_RESET_LAYOUT: G17UncachedChannelResetLayout =
    G17UncachedChannelResetLayout {
        zero_00: 0x00,
        zero_10: 0x10,
        zero_20: 0x20,
        zero_30: 0x30,
        zero_40: 0x40,
        sentinel_50: 0x50,
        ring_entry_count_60: 0x60,
    };

fn put_u32_le(raw: &mut [u8], offset: usize, value: u32) {
    raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u16_le(raw: &mut [u8], offset: usize, value: u16) {
    raw[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u64_le(raw: &mut [u8], offset: usize, value: u64) {
    raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

/// Build the exact 0x80-byte `_AGFIChannelState` reset image.
pub(crate) fn encode_g17_host_channel_state_reset(
    values: G17HostChannelStateResetValues,
) -> [u8; G17_HOST_CHANNEL_STATE_RESET_SIZE] {
    let mut raw = [0u8; G17_HOST_CHANNEL_STATE_RESET_SIZE];
    let layout = G17_HOST_CHANNEL_STATE_RESET_LAYOUT;

    put_u64_le(&mut raw, layout.gpu_va_00, values.gpu_va_00);
    put_u64_le(&mut raw, layout.gpu_va_08, values.gpu_va_08);
    put_u64_le(&mut raw, layout.gpu_va_10, values.gpu_va_10);
    put_u32_le(&mut raw, layout.zero_18, 0);
    put_u32_le(&mut raw, layout.zero_1c, 0);
    put_u32_le(&mut raw, layout.zero_20, 0);
    put_u32_le(&mut raw, layout.sentinel_24, u32::MAX);
    put_u32_le(&mut raw, layout.fixed_4_28, 4);
    put_u32_le(&mut raw, layout.sentinel_40, u32::MAX);
    put_u32_le(&mut raw, layout.word_44, values.word_44);
    put_u32_le(&mut raw, layout.zero_58, 0);
    put_u64_le(&mut raw, layout.gpu_va_70, values.gpu_va_70);
    raw
}

pub(crate) fn apply_g17_uncached_channel_reset(
    raw: &mut [u8; G17_UNCACHED_CHANNEL_RESET_WRITE_EXTENT],
    ring_entry_count: u32,
) {
    let layout = G17_UNCACHED_CHANNEL_RESET_LAYOUT;

    put_u32_le(raw, layout.zero_00, 0);
    put_u32_le(raw, layout.zero_10, 0);
    put_u32_le(raw, layout.zero_20, 0);
    put_u32_le(raw, layout.zero_30, 0);
    put_u32_le(raw, layout.zero_40, 0);
    put_u32_le(raw, layout.sentinel_50, u32::MAX);
    put_u32_le(raw, layout.ring_entry_count_60, ring_entry_count);
}


#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17ChannelAbiTarget {
    /// A18 Pro: G17P host and `g17p` firmware.
    A18ProG17P,
    M5G17G,
    /// M5 Pro/Max: G17X host with `g17s` firmware. Kept separate from G17P.
    M5ProMaxG17X,
}

/// Cached `_AGFIChannelState` fields consumed by the A18 Pro firmware.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PCachedChannelStateLayout {
    pub(crate) uncached_state_gpu_va: u32,
    pub(crate) command_pointer_ring_gpu_va: u32,
    pub(crate) coherency_cursor: u32,
    pub(crate) record_size: u32,
}

/// Uncached channel fields shared by the A18 Pro host and firmware.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PUncachedChannelStateLayout {
    /// Firmware dispatch/consumer cursor.
    pub(crate) consumer: u32,
    /// Cursor published after the command-pointer range is made coherent.
    pub(crate) coherency_shadow: u32,
    pub(crate) producer: u32,
    /// Recovery target selected instead of the accelerator-entry snapshot.
    pub(crate) recovery_target: u32,
    /// Runtime ring depth in 8-byte command-pointer entries.
    pub(crate) ring_entry_count: u32,
    pub(crate) cursor_width: u32,
    pub(crate) write_extent: u32,
}

/// Complete admitted A18 Pro channel layout.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PChannelAbiLayout {
    pub(crate) cached: G17PCachedChannelStateLayout,
    pub(crate) uncached: G17PUncachedChannelStateLayout,
    pub(crate) command_pointer_entry_bytes: u32,
    pub(crate) accelerator_target_width: u32,
}

pub(crate) const G17P_CHANNEL_ABI: G17PChannelAbiLayout = G17PChannelAbiLayout {
    cached: G17PCachedChannelStateLayout {
        uncached_state_gpu_va: 0x00,
        command_pointer_ring_gpu_va: 0x08,
        coherency_cursor: 0x1c,
        record_size: 0x80,
    },
    uncached: G17PUncachedChannelStateLayout {
        consumer: 0x00,
        coherency_shadow: 0x30,
        producer: 0x40,
        recovery_target: 0x50,
        ring_entry_count: 0x60,
        cursor_width: 4,
        write_extent: 0x64,
    },
    command_pointer_entry_bytes: 8,
    accelerator_target_width: 2,
};

pub(crate) const G17P_CHANNEL_PRODUCER_REQUIRES_DMB_ISH: bool = true;

pub(crate) const fn g17_channel_abi(
    target: G17ChannelAbiTarget,
) -> Result<G17PChannelAbiLayout, SubmissionError> {
    match target {
        G17ChannelAbiTarget::A18ProG17P => Ok(G17P_CHANNEL_ABI),
        G17ChannelAbiTarget::M5G17G | G17ChannelAbiTarget::M5ProMaxG17X => {
            Err(SubmissionError::ChannelAbiUnconfirmed)
        }
    }
}

/// A host write plan for one G17P command-pointer ring publication.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PChannelWritePlan {
    /// Byte offset of the u64 slot written before the barrier.
    pub(crate) entry_byte_offset: u64,
    /// u32 producer value published after the barrier.
    pub(crate) next_producer: u32,
}

pub(crate) const fn prepare_g17p_channel_write(
    target: G17ChannelAbiTarget,
    producer: u32,
    consumer: u32,
    ring_entry_count: u32,
) -> Result<G17PChannelWritePlan, SubmissionError> {
    match g17_channel_abi(target) {
        Ok(_) => {}
        Err(error) => return Err(error),
    }

    if consumer >= ring_entry_count {
        return Err(if ring_entry_count == 0 {
            SubmissionError::RingSizeZero
        } else {
            SubmissionError::RingIndexOutOfRange
        });
    }

    let next_producer = match ring_next(producer, ring_entry_count) {
        Ok(next) => next,
        Err(error) => return Err(error),
    };
    if next_producer == consumer {
        return Err(SubmissionError::RingFull);
    }

    Ok(G17PChannelWritePlan {
        entry_byte_offset: producer as u64 * G17P_CHANNEL_ABI.command_pointer_entry_bytes as u64,
        next_producer,
    })
}


pub(crate) const G17P_PARTIAL_OPENING_PAGE_SIZE: usize = 0x4000;
pub(crate) const G17P_PARTIAL_OPENING_COMPUTED_GPU_VA: u64 = 0xffff_fc20_015d_8000;
pub(crate) const G17P_PARTIAL_OPENING_SCHEDULER_GPU_VA: u64 = 0xffff_fc20_015e_0000;
pub(crate) const G17P_PARTIAL_OPENING_SCHEDULER_WORD: u64 = 0x0000_0020_0001_9000;
pub(crate) const G17P_PARTIAL_OPENING_PRIMARY_INDEX_FIRMWARE_GPU_VA: u64 =
    0xffff_fc20_c084_8000;
pub(crate) const G17P_PARTIAL_OPENING_PRIMARY_INDEX_GPU_VA: u64 = 0x0000_0010_0019_0000;
pub(crate) const G17P_PARTIAL_OPENING_PRIMARY_INDEX_SIZE: usize =
    4 * G17P_PARTIAL_OPENING_PAGE_SIZE;
pub(crate) const G17P_PARTIAL_OPENING_FWCTL_OFFSET: u64 = 0x001c_0000;
pub(crate) const G17P_PARTIAL_OPENING_CONTROL_MESSAGE_SIZE: u64 = 0x40;
pub(crate) const G17P_PARTIAL_OPENING_WORK_DOORBELL_CHANNEL: u8 = 0;
pub(crate) const G17P_PARTIAL_OPENING_PAIRED_DOORBELL: u64 =
    MSG_TX_DOORBELL | G17P_PARTIAL_OPENING_WORK_DOORBELL_CHANNEL as u64;
pub(crate) const G17P_NATIVE_RENDER_DOORBELL_PRIORITY: u8 = 0;
pub(crate) const G17P_NATIVE_RENDER_QOS_HARDWARE_BUFFER_ID: u8 =
    if G17P_NATIVE_RENDER_DOORBELL_PRIORITY == 0 { 0 } else { 2 };
pub(crate) const G17P_NATIVE_RENDER_POLICY: u16 =
    if G17P_NATIVE_RENDER_DOORBELL_PRIORITY == 0 { 1 } else { 2 };
pub(crate) const G17P_NATIVE_3D_DOORBELL: u64 = MSG_TX_DOORBELL
    | PipeType::Fragment as u64
    | ((G17P_NATIVE_RENDER_DOORBELL_PRIORITY as u64) << DOORBELL_INDEX_SHIFT);
pub(crate) const G17P_NATIVE_TA_DOORBELL: u64 = MSG_TX_DOORBELL
    | PipeType::Vertex as u64
    | ((G17P_NATIVE_RENDER_DOORBELL_PRIORITY as u64) << DOORBELL_INDEX_SHIFT);
pub(crate) const G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE: usize = 0x18;
pub(crate) const G17P_PARTIAL_OPENING_OUTER_SLOT_COUNT: u32 = 0x100;
const fn g17p_work_channel_table_index(priority: u8, pipe: PipeType) -> u8 {
    priority * 3 + pipe as u8
}
pub(crate) const G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX: u8 =
    g17p_work_channel_table_index(G17P_NATIVE_RENDER_DOORBELL_PRIORITY, PipeType::Vertex);
pub(crate) const G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX: u8 =
    g17p_work_channel_table_index(G17P_NATIVE_RENDER_DOORBELL_PRIORITY, PipeType::Fragment);
pub(crate) const G17P_COMPUTE_CHANNEL_TABLE_INDEX: u8 =
    g17p_work_channel_table_index(2, PipeType::Compute);
// The compute index is the validated anchor for the rule above: it must stay 8.
const _: () = assert!(G17P_COMPUTE_CHANNEL_TABLE_INDEX == 8);
const _: () = assert!(G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX == 0);
const _: () = assert!(G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX == 1);
/// Compute work-submission doorbell channel. VALIDATED by 2000 consecutive
/// passing computes. Decodes under `encode_tx_doorbell` as
/// `pipe Compute(2) | (priority 2 << 2)` = 0x0a, which is why it is not simply
/// the channel-table index (8).
pub(crate) const G17P_COMPUTE_WORK_DOORBELL_CHANNEL: u8 = 0x0a;
pub(crate) const G17P_COMPUTE_WORK_DOORBELL: u64 =
    MSG_TX_DOORBELL | G17P_COMPUTE_WORK_DOORBELL_CHANNEL as u64;
pub(crate) const G17P_COLD_OPENING_RECORD_POOL_A_COUNT: usize = 35;
pub(crate) const G17P_COLD_OPENING_RECORD_POOL_A_STRIDE: usize = 0x100;
pub(crate) const G17P_COLD_OPENING_RECORD_POOL_A_SIZE: usize =
    G17P_COLD_OPENING_RECORD_POOL_A_COUNT * G17P_COLD_OPENING_RECORD_POOL_A_STRIDE;
pub(crate) const G17P_COLD_OPENING_RECORD_POOL_B_SIZE: usize = 79 * 0x80;
pub(crate) const G17P_COLD_OPENING_SHARED_OBJECT_SIZE: usize = 0x88;
pub(crate) const G17P_COLD_OPENING_SHARED_CONTROL_SIZE: usize = 0x70;
pub(crate) const G17P_COLD_OPENING_SHARED_CONTROL_INNER_SIZE: usize = 4;
pub(crate) const G17P_COLD_OPENING_CHANNEL_CONTROL_SIZE: usize = 0x40;
pub(crate) const G17P_COLD_OPENING_OPTIONAL_SIZE: usize = 0xc0;
pub(crate) const G17P_COLD_OPENING_EVENT_SIZE: usize = 0x40;
pub(crate) const G17P_COLD_OPENING_QUEUE_CONTEXT_SIZE: usize = 0x4000;
pub(crate) const G17P_COLD_OPENING_TA_DESCRIPTOR_BASE: u64 = 0xffff_fc20_c001_8000;
pub(crate) const G17P_COLD_OPENING_3D_DESCRIPTOR_BASE: u64 = 0xffff_fc20_c00b_0000;
pub(crate) const G17P_COLD_OPENING_QUEUE_RECORD_SIZE: usize = 0xc0;
pub(crate) const G17P_COLD_OPENING_POINTER_BLOCK_SIZE: usize = 0x80;
pub(crate) const G17P_COLD_OPENING_OPERAND_DIRECTORY_GPU_VA: u64 = 0x70_0000_0000;
pub(crate) const G17P_COLD_OPENING_OPERAND_DIRECTORY_SIZE: usize = 4 * 0x4000;
pub(crate) const G17P_COLD_OPENING_OPERAND_TABLE_GPU_VA: u64 = 0x70_0020_8000;
pub(crate) const G17P_COLD_OPENING_OPERAND_BUFFER_BASE: u64 = 0x70_0022_0000;
pub(crate) const G17P_COLD_OPENING_OPERAND_BUFFER_COUNT: usize = 28;
pub(crate) const G17P_COLD_OPENING_OPERAND_BUFFER_SIZE: u64 = 0x10_0000;
pub(crate) const G17P_COLD_OPENING_OPERAND_BUFFER_STRIDE: u64 = 0x10_8000;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PColdOpeningLeaf {
    PrimaryIndex,
    SecondaryIndex,
    PoolASlots,
    PoolBSlots,
    SharedSlots,
    Flag,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PColdOpeningStage {
    Tiling,
    Fragment,
}

/// Relocatable leaf addresses used by the cold-opening pool and shared graph.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PColdOpeningGraphAddresses {
    pub(crate) primary_index: u64,
    pub(crate) secondary_index: u64,
    pub(crate) pool_a_slots: u64,
    pub(crate) pool_b_slots: u64,
    pub(crate) shared_slots: u64,
    pub(crate) flag: u64,
}

/// Relocatable pointers carried by one cold-opening optional record.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PColdOpeningOptionalAddresses {
    pub(crate) context_scratch: u64,
    pub(crate) firmware_scratch: u64,
    pub(crate) usc_priv_mem_flist: u64,
    /// HardwareBufferID of `usc_priv_mem_flist`, copied to tag-15 +0x46.
    pub(crate) usc_freelist_hardware_buffer_id: u32,
    pub(crate) channel_control: u64,
    pub(crate) tiling_shared_object: u64,
    /// HWPB owner lease token (HardwareBufferBase +0x68), not an ordinal.
    /// Reacquiring the TA HardwareBufferID from zero references assigns it.
    pub(crate) parameter_buffer_token: u64,
    pub(crate) owner_pid: u32,
    pub(crate) ta_hardware_buffer_id: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PColdOpeningQueueAddresses {
    pub(crate) pointers: u64,
    pub(crate) item_ring: u64,
    pub(crate) job_list: u64,
    pub(crate) channel_control: u64,
    /// PID of the process that owns this channel state.
    pub(crate) owner_pid: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PPartialOpeningContextContract {
    pub(crate) context0_root_slot: u8,
    pub(crate) context0_context_id: u8,
    pub(crate) render_root_slot: u8,
    pub(crate) render_context_id: u8,
    pub(crate) context0_empty_high_root_slot: u8,
    pub(crate) render_empty_high_root_slot: u8,
    pub(crate) transport_pair: u8,
    pub(crate) descriptor_pair: u8,
    pub(crate) shared_queue_pair_namespace: u8,
    pub(crate) optional_identity_context_id: u8,
    pub(crate) descriptor_context_id: u8,
    pub(crate) scheduler_class: u8,
    pub(crate) optional_grids: [u8; 2],
    pub(crate) queue_identity: u8,
    pub(crate) optional_submission_ordinal: u16,
    pub(crate) event_counter: u32,
    pub(crate) work_doorbell_channel: u8,
    pub(crate) context0_low_root_present_at_first_work: bool,
    pub(crate) render_low_root_present_at_first_work: bool,
    pub(crate) source_firmware_high_root_table_resident: bool,
}

pub(crate) const G17P_PARTIAL_OPENING_CONTEXT: G17PPartialOpeningContextContract =
    G17PPartialOpeningContextContract {
        context0_root_slot: 0,
        context0_context_id: 0,
        render_root_slot: 1,
        render_context_id: 1,
        context0_empty_high_root_slot: 0,
        render_empty_high_root_slot: 1,
        transport_pair: 0,
        descriptor_pair: 0,
        shared_queue_pair_namespace: 0,
        optional_identity_context_id: 1,
        descriptor_context_id: 1,
        scheduler_class: 2,
        optional_grids: [0, 1],
        queue_identity: 0x15,
        optional_submission_ordinal: 0,
        event_counter: 0x102,
        work_doorbell_channel: G17P_PARTIAL_OPENING_WORK_DOORBELL_CHANNEL,
        context0_low_root_present_at_first_work: true,
        render_low_root_present_at_first_work: true,
        source_firmware_high_root_table_resident: false,
    };

/// One exact 64-bit write into the primary status-B object before control 0x84.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PPartialOpeningStatusWrite {
    pub(crate) offset: u32,
    pub(crate) value: u64,
}

/// State required to prepare one half of the paired outer-channel publish.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PPartialOpeningOuterChannelState {
    pub(crate) consumers: [u32; 2],
    pub(crate) producer: u32,
    pub(crate) queue_gpu_va: u64,
    pub(crate) queue_write_index: u32,
}

/// One complete 0x18-byte outer slot followed by its producer publication.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PPartialOpeningOuterSlotPlan {
    pub(crate) channel_table_index: u8,
    pub(crate) slot_index: u8,
    pub(crate) slot_byte_offset: u32,
    pub(crate) slot: [u8; G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE],
    pub(crate) next_producer: u32,
}

/// The two producer writes and the single mailbox kick for first graphics.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PPartialOpeningWorkPair {
    pub(crate) fragment: G17PPartialOpeningOuterSlotPlan,
    pub(crate) tiling: G17PPartialOpeningOuterSlotPlan,
    pub(crate) paired_doorbell: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PComputeChannelWork {
    pub(crate) outer: G17PPartialOpeningOuterSlotPlan,
    pub(crate) doorbell: u64,
}

/// Side-effect order measured at the successful first-partial boundary.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PPartialOpeningPublicationStep {
    PrimarySchedulerPage,
    PrimaryIndexPage,
    PreControlStatus,
    ControlDone0x84,
    FragmentState,
    EmptyHighRoots,
    FragmentProducer,
    LateTilingState,
    SystemBarrierBeforeTilingProducer,
    TilingProducer,
    SystemBarrierBeforePairedDoorbell,
    PairedWorkDoorbell0x83,
}

pub(crate) const G17P_PARTIAL_OPENING_PUBLICATION_ORDER: [G17PPartialOpeningPublicationStep; 12] = [
    G17PPartialOpeningPublicationStep::PrimarySchedulerPage,
    G17PPartialOpeningPublicationStep::PrimaryIndexPage,
    G17PPartialOpeningPublicationStep::PreControlStatus,
    G17PPartialOpeningPublicationStep::ControlDone0x84,
    G17PPartialOpeningPublicationStep::FragmentState,
    G17PPartialOpeningPublicationStep::EmptyHighRoots,
    G17PPartialOpeningPublicationStep::FragmentProducer,
    G17PPartialOpeningPublicationStep::LateTilingState,
    G17PPartialOpeningPublicationStep::SystemBarrierBeforeTilingProducer,
    G17PPartialOpeningPublicationStep::TilingProducer,
    G17PPartialOpeningPublicationStep::SystemBarrierBeforePairedDoorbell,
    G17PPartialOpeningPublicationStep::PairedWorkDoorbell0x83,
];

/// Build the computed one-record scheduler page that decided graphics entry in
/// the successful post-control graft.
pub(crate) fn apply_g17p_partial_opening_scheduler_page(
    page: &mut [u8],
) -> Result<(), SubmissionError> {
    if page.len() != G17P_PARTIAL_OPENING_PAGE_SIZE {
        return Err(SubmissionError::PartialOpeningPageSize);
    }
    page.fill(0);
    put_u64_le(page, 0, G17P_PARTIAL_OPENING_SCHEDULER_WORD);
    Ok(())
}

/// Array-returning form retained for host tests. Kernel mappings should use
/// [`apply_g17p_partial_opening_scheduler_page`] to avoid a 16 KiB temporary.
pub(crate) fn build_g17p_partial_opening_scheduler_page() -> [u8; G17P_PARTIAL_OPENING_PAGE_SIZE] {
    let mut page = [0u8; G17P_PARTIAL_OPENING_PAGE_SIZE];
    let _ = apply_g17p_partial_opening_scheduler_page(&mut page);
    page
}

pub(crate) fn apply_g17p_partial_opening_primary_index_page(
    page: &mut [u8],
) -> Result<(), SubmissionError> {
    if page.len() != G17P_PARTIAL_OPENING_PAGE_SIZE {
        return Err(SubmissionError::PartialOpeningPageSize);
    }
    page.fill(0);
    let mut index = 0usize;
    for (start, groups) in [(0x11u32, 6u32), (0x3cu32, 2u32)] {
        for group in 0..groups {
            let base = start + group * 5;
            for member in 0..4u32 {
                put_u32_le(page, index * 4, base + member);
                index += 1;
            }
        }
    }
    Ok(())
}

/// Array-returning form retained for host tests. Kernel mappings should use
/// [`apply_g17p_partial_opening_primary_index_page`] directly.
pub(crate) fn build_g17p_partial_opening_primary_index_page() -> [u8; G17P_PARTIAL_OPENING_PAGE_SIZE]
{
    let mut page = [0u8; G17P_PARTIAL_OPENING_PAGE_SIZE];
    let _ = apply_g17p_partial_opening_primary_index_page(&mut page);
    page
}

/// Write one of the six source-built leaf pages used by the cold opening.
pub(crate) fn apply_g17p_cold_opening_leaf_page(
    leaf: G17PColdOpeningLeaf,
    page: &mut [u8],
) -> Result<(), SubmissionError> {
    if page.len() != G17P_PARTIAL_OPENING_PAGE_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    page.fill(0);
    match leaf {
        G17PColdOpeningLeaf::PrimaryIndex => {
            apply_g17p_partial_opening_primary_index_page(page)?;
        }
        G17PColdOpeningLeaf::SecondaryIndex => {
            let mut index = 0usize;
            for (start, groups) in [(0x11u32, 6u32), (0x3cu32, 2u32)] {
                for group in 0..groups {
                    put_u64_le(page, index * 8, (start + group * 5) as u64);
                    index += 1;
                }
            }
        }
        G17PColdOpeningLeaf::PoolASlots => put_u32_le(page, 0x04, 2),
        G17PColdOpeningLeaf::PoolBSlots => {}
        G17PColdOpeningLeaf::SharedSlots => {
            put_u32_le(page, 0x00, 8);
            put_u32_le(page, 0x04, 8);
            put_u32_le(page, 0x60, 1);
        }
        G17PColdOpeningLeaf::Flag => put_u32_le(page, 0x00, 1),
    }
    Ok(())
}

pub(crate) const fn g17p_render_pool_a_record_index(
    submission_ordinal: u32,
    native_first_record: bool,
) -> usize {
    if native_first_record && submission_ordinal == 0 {
        1
    } else {
        (submission_ordinal as usize * 2) % G17P_COLD_OPENING_RECORD_POOL_A_COUNT
    }
}

/// Initialize only the Pool-A slot corresponding to the selected first
/// record. The slot page has no other modeled contents.
pub(crate) fn apply_g17p_cold_opening_pool_a_start_slot(
    record_index: usize,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_PARTIAL_OPENING_PAGE_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    if record_index >= G17P_COLD_OPENING_RECORD_POOL_A_COUNT {
        return Err(SubmissionError::PartialOpeningQueueHeadOutOfRange);
    }
    out.fill(0);
    put_u32_le(out, record_index * 4, 1);
    Ok(())
}

/// Write the 35-record Pool-A array with relocatable slot pointers and select
/// the record whose first-pass state word is initialized to 0x50.
pub(crate) fn apply_g17p_cold_opening_record_pool_a_at(
    addresses: G17PColdOpeningGraphAddresses,
    active_record_index: usize,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if active_record_index >= G17P_COLD_OPENING_RECORD_POOL_A_COUNT {
        return Err(SubmissionError::PartialOpeningQueueHeadOutOfRange);
    }
    if out.len() != G17P_COLD_OPENING_RECORD_POOL_A_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    let slot_base = addresses.pool_a_slots;
    for index in 0..G17P_COLD_OPENING_RECORD_POOL_A_COUNT {
        let slot = slot_base
            .checked_add((index * 4) as u64)
            .ok_or(SubmissionError::PartialOpeningAddressOverflow)?;
        let base = index * G17P_COLD_OPENING_RECORD_POOL_A_STRIDE;
        put_u64_le(out, base, slot);
        if index == active_record_index {
            put_u32_le(out, base + 0x10, 0x50);
        }
    }
    Ok(())
}

/// Established record-zero wrapper retained for callers and fixture tests.
pub(crate) fn apply_g17p_cold_opening_record_pool_a(
    addresses: G17PColdOpeningGraphAddresses,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    apply_g17p_cold_opening_record_pool_a_at(addresses, 0, out)
}

/// Write the 79-record Pool-B array in the cold pair-zero namespace.
pub(crate) fn apply_g17p_cold_opening_record_pool_b(
    addresses: G17PColdOpeningGraphAddresses,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_RECORD_POOL_B_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    let slot_base = addresses
        .pool_b_slots
        .checked_add(4)
        .ok_or(SubmissionError::PartialOpeningAddressOverflow)?;
    let shared = addresses
        .shared_slots
        .checked_add(0x40)
        .ok_or(SubmissionError::PartialOpeningAddressOverflow)?;
    for index in 0..79usize {
        let base = index * 0x80;
        put_u32_le(out, base, 0x80004 + index as u32 * 4);
        put_u32_le(out, base + 0x04, 0x10);
        put_u64_le(
            out,
            base + 0x08,
            slot_base
                .checked_add((index * 4) as u64)
                .ok_or(SubmissionError::PartialOpeningAddressOverflow)?,
        );
        let phase = index % 36;
        let cycle = if phase == 35 {
            0x178000
        } else {
            0x178020 + phase as u32 * 0x20
        };
        put_u32_le(out, base + 0x28, cycle);
        put_u64_le(out, base + 0x40, shared);
        if index == 0 {
            put_u32_le(out, base + 0x4c, 1);
        }
    }
    Ok(())
}

pub(crate) fn apply_g17p_cold_opening_shared_object(
    addresses: G17PColdOpeningGraphAddresses,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_SHARED_OBJECT_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    for (offset, address) in [
        (0x20, addresses.primary_index),
        (0x44, addresses.secondary_index),
        (0x4c, addresses.shared_slots),
        (0x64, addresses.flag),
    ] {
        put_u64_le(out, offset, address);
    }
    for (offset, value) in [
        (0x28, 0x0019_0000),
        (0x2c, 0x10),
        (0x30, 0x0001_0000),
        (0x34, 0x20),
        (0x38, 0x0c18),
        (0x3c, 8),
        (0x54, 0x1f),
        (0x58, 0x0002_0000),
        (0x7c, 0x3060),
        (0x80, 0x1020),
        (0x84, 0x0018_0000),
    ] {
        put_u32_le(out, offset, value);
    }
    Ok(())
}

/// Write the 0x70-byte class-2 control object used before cold first work.
pub(crate) fn apply_g17p_cold_opening_shared_control(
    operand_table: u64,
    inner_state: u64,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_SHARED_CONTROL_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    put_u32_le(out, 0x00, 1);
    put_u32_le(out, 0x10, 2);
    put_u64_le(out, 0x18, 0x0004_0000_0000_0070);
    put_u64_le(out, 0x20, 0x0000_1900_0000_0000);
    put_u64_le(out, 0x28, 0x0000_1900_0000_0000);
    put_u64_le(out, 0x30, operand_table);
    put_u32_le(out, 0x40, 4);
    put_u32_le(out, 0x48, 0xe0);
    put_u64_le(out, 0x4c, inner_state);
    put_u32_le(out, 0x60, 3);
    Ok(())
}

pub(crate) fn apply_g17p_cold_opening_shared_control_inner(
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_SHARED_CONTROL_INNER_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    put_u32_le(out, 0, 2);
    Ok(())
}

/// Write the fresh channel-control record used by both cold queues.
pub(crate) fn apply_g17p_cold_opening_channel_control(
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_CHANNEL_CONTROL_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    put_u64_le(out, 0x00, 0x0000_0100_0000_ffff);
    put_u64_le(out, 0x20, 0x0002_0000_0000_0000);
    put_u64_le(out, 0x30, 0x0000_0000_ff00_0000);
    Ok(())
}

/// Write one exact grid-0/1 optional record from the cold partial opening.
pub(crate) fn apply_g17p_cold_opening_optional(
    stage: G17PColdOpeningStage,
    addresses: G17PColdOpeningOptionalAddresses,
    queue_id: u16,
    partner_queue_id: u16,
    qos_hardware_buffer_id: u16,
    install: u16,
    context_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_OPTIONAL_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    put_u32_le(out, 0, 0x0f);
    for (offset, address) in [
        (0x08, addresses.context_scratch),
        (0x10, addresses.firmware_scratch),
        (0x36, addresses.usc_priv_mem_flist),
        (0x4a, addresses.channel_control),
    ] {
        put_u64_le(out, offset, address);
    }
    let fields: &[(usize, u16)] = match stage {
        G17PColdOpeningStage::Tiling => &[
            (0x18, queue_id),
            (0x1a, install),
            (0x1e, G17P_NATIVE_RENDER_DOORBELL_PRIORITY as u16),
            (0x26, 1),
            (0x52, 1),
            (0x56, qos_hardware_buffer_id),
            (0x5e, G17P_NATIVE_RENDER_POLICY),
            (0x62, 1),
            (0x66, 1),
            (0x82, partner_queue_id),
        ],
        G17PColdOpeningStage::Fragment => &[
            (0x18, queue_id),
            (0x1a, install),
            (0x1e, G17P_NATIVE_RENDER_DOORBELL_PRIORITY as u16),
            (0x22, 1),
            (0x26, 1),
            (0x52, 1),
            (0x56, qos_hardware_buffer_id),
            (0x5e, G17P_NATIVE_RENDER_POLICY),
            (0x62, 1),
            (0x66, 1),
        ],
    };
    for (offset, value) in fields {
        put_u16_le(out, *offset, *value);
    }
    // DIAGNOSTIC. +0x22 is the ConfigUpdate's data-master field, hardcoded to
    // 1 for the fragment. The tiler (DM0) and compute (DM2) both dispatch and
    // execute; the fragment is retired with KTrace 0x43 status 4 without ever
    // running, with an identically shaped descriptor, a satisfied barrier and
    // geometry already tiled. Overriding this isolates the data master itself.
    let override_dm = match stage {
        G17PColdOpeningStage::Fragment => *crate::module_parameters::g17p_render_3d_dm.value(),
        G17PColdOpeningStage::Tiling => *crate::module_parameters::g17p_render_ta_dm.value(),
    };
    if override_dm != 0xff {
        put_u16_le(out, 0x22, override_dm as u16);
    }
    put_u32_le(out, 0x5a, addresses.owner_pid);
    put_u32_le(out, 0x46, addresses.usc_freelist_hardware_buffer_id);
    put_u16_le(out, 0x32, context_id);
    match stage {
        G17PColdOpeningStage::Tiling => {
            put_u64_le(out, 0x6e, addresses.tiling_shared_object);
            put_u64_le(out, 0x76, addresses.parameter_buffer_token);
            put_u32_le(out, 0x7e, addresses.ta_hardware_buffer_id);
        }
        G17PColdOpeningStage::Fragment => out[0x76..0x86].fill(0xff),
    }
    Ok(())
}

/// Write one cold event record. Both halves use the measured counter 0x102.
pub(crate) fn apply_g17p_cold_opening_event(
    stage: G17PColdOpeningStage,
    queue_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_EVENT_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    put_u32_le(out, 0x00, 0x0e);
    put_u32_le(out, 0x04, 0x0001_0000 | queue_id as u32);
    put_u32_le(
        out,
        0x08,
        0x100 | u32::from(G17P_NATIVE_RENDER_DOORBELL_PRIORITY),
    );
    if stage == G17PColdOpeningStage::Fragment {
        put_u32_le(out, 0x10, 0x100);
    }
    Ok(())
}

/// Refresh the canonical optional record for a later logical submission.
/// The queue-context record itself remains in locator slot zero.
pub(crate) fn apply_g17p_retained_optional(
    stage: G17PColdOpeningStage,
    addresses: G17PColdOpeningOptionalAddresses,
    submission_ordinal: u32,
    bias_generation: bool,
    queue_id: u16,
    partner_queue_id: u16,
    qos_hardware_buffer_id: u16,
    install: u16,
    context_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    // The announced event stamp is ordinal + 1 on this serialized path and
    // still has a 24-bit interface. Payload bytes are allowed to wrap; the
    // retained owner generation below must not truncate with them.
    if submission_ordinal >= 0x00ff_ffff {
        return Err(SubmissionError::PartialOpeningQueueHeadOutOfRange);
    }
    apply_g17p_cold_opening_optional(
        stage,
        addresses,
        queue_id,
        partner_queue_id,
        qos_hardware_buffer_id,
        install,
        context_id,
        out,
    )?;
    if submission_ordinal != 0 {
        for offset in [0x1a, 0x52, 0x62] {
            put_u16_le(out, offset, 0);
        }
    }
    let generation = if bias_generation {
        (submission_ordinal as u16).wrapping_add(1)
    } else {
        submission_ordinal as u16
    };
    put_u16_le(out, 0x2a, generation);
    put_u16_le(out, 0x2e, generation << 8);
    put_u64_le(out, 0x3e, g17p_render_flist_generation(submission_ordinal));
    // Keep the pair-owned HardwareBufferID identical in ConfigUpdate, the
    // QID-indexed QoS row, and the KSM dependency-control qword.
    put_u16_le(out, 0x56, qos_hardware_buffer_id);
    Ok(())
}

pub(crate) const fn g17p_render_flist_generation(submission_ordinal: u32) -> u64 {
    submission_ordinal as u64
}

pub(crate) fn apply_g17p_retained_event_with_stamp(
    stage: G17PColdOpeningStage,
    entry_stamp: u32,
    queue_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if entry_stamp == 0 || entry_stamp > 0x00ff_ffff {
        return Err(SubmissionError::PartialOpeningQueueHeadOutOfRange);
    }
    apply_g17p_cold_opening_event(stage, queue_id, out)?;
    put_u32_le(
        out,
        0x08,
        (entry_stamp << 8) | u32::from(G17P_NATIVE_RENDER_DOORBELL_PRIORITY),
    );
    Ok(())
}

/// Refresh the canonical event record while retaining its physical address.
///
/// Without the SKSM half there is no KSM producer to read a stamp from, so
/// the stamp is derived as `submission_ordinal + 1` -- which is exactly what
/// the producer would have yielded for the first `stamp_count` submissions,
/// since the geometry seeds `current_timestamp` at 1 and advances it by one
/// per kick.
pub(crate) fn apply_g17p_retained_event(
    stage: G17PColdOpeningStage,
    submission_ordinal: u32,
    queue_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    let group_number = submission_ordinal
        .checked_add(1)
        .ok_or(SubmissionError::PartialOpeningQueueHeadOutOfRange)?;
    apply_g17p_retained_event_with_stamp(stage, group_number, queue_id, out)
}

/// Write the cold class-2 queue record for the owning process.
pub(crate) fn apply_g17p_cold_opening_queue_record(
    addresses: G17PColdOpeningQueueAddresses,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_QUEUE_RECORD_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    put_u64_le(out, 0x00, addresses.pointers);
    put_u64_le(out, 0x08, addresses.item_ring);
    put_u64_le(out, 0x10, addresses.job_list);
    put_u32_le(out, 0x24, u32::MAX);
    let priority = u32::from(G17P_NATIVE_RENDER_DOORBELL_PRIORITY);
    put_u32_le(out, 0x28, priority);
    put_u32_le(out, 0x2c, priority);
    if priority == 0 {
        out[0x32..0x38].fill(0xff);
        put_u32_le(out, 0x38, 1);
    } else {
        out[0x36..0x38].fill(0xff);
        put_u32_le(out, 0x38, 0);
    }
    put_u32_le(out, 0x40, u32::from(G17P_NATIVE_RENDER_POLICY));
    put_u32_le(out, 0x44, u32::MAX);
    put_u32_le(out, 0x48, addresses.owner_pid);
    put_u64_le(out, 0x9c, addresses.channel_control);
    Ok(())
}

pub(crate) fn apply_g17p_cold_opening_pointer_block(out: &mut [u8]) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_POINTER_BLOCK_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    put_u32_le(out, 0x50, u32::MAX);
    put_u64_le(out, 0x60, 0x500);
    Ok(())
}

/// Apply the measured first-publication bytes after descriptor construction.
pub(crate) fn apply_g17p_cold_opening_descriptor_overrides(
    stage: G17PColdOpeningStage,
    descriptor: &mut [u8],
) -> Result<(), SubmissionError> {
    if descriptor.len() != G17P_PARTIAL_OPENING_PAGE_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    let fields: &[(usize, u8)] = match stage {
        G17PColdOpeningStage::Tiling => &[
            (0x38, 0x47),
            (0x3a, 0x49),
            (0x3c, 0x49),
            (0x0789, 0x08),
            (0x093e, 0xd0),
            (0x093f, 0x91),
        ],
        G17PColdOpeningStage::Fragment => &[
            (0x50, 0x01),
            (0x80, 0x56),
            (0x82, 0x57),
            (0x84, 0x57),
            (0x88, 0x59),
            (0x90, 0x01),
            (0x215c, 0x00),
            (0x222d, 0x00),
        ],
    };
    for (offset, value) in fields {
        descriptor[*offset] = *value;
    }
    put_u32_le(descriptor, 0x0c, 1);
    Ok(())
}

pub(crate) fn apply_g17p_cold_opening_operand_table(out: &mut [u8]) -> Result<(), SubmissionError> {
    if out.len() != G17P_PARTIAL_OPENING_PAGE_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    Ok(())
}

/// Write the optional 28-entry table used by a later populated control state.
pub(crate) fn apply_g17p_populated_operand_table(out: &mut [u8]) -> Result<(), SubmissionError> {
    apply_g17p_cold_opening_operand_table(out)?;
    for index in 0..G17P_COLD_OPENING_OPERAND_BUFFER_COUNT {
        let address = G17P_COLD_OPENING_OPERAND_BUFFER_BASE
            .checked_add(index as u64 * G17P_COLD_OPENING_OPERAND_BUFFER_STRIDE)
            .ok_or(SubmissionError::PartialOpeningAddressOverflow)?;
        put_u64_le(out, index * 0x40, address | (1u64 << 60));
    }
    Ok(())
}

/// Keep the four-page operand directory zero through cold first work.
pub(crate) fn apply_g17p_cold_opening_operand_directory(
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_OPERAND_DIRECTORY_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    Ok(())
}

/// Write the optional populated directory for the 28 operand buffers.
pub(crate) fn apply_g17p_populated_operand_directory(
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    apply_g17p_cold_opening_operand_directory(out)?;
    let pages_per_buffer = G17P_COLD_OPENING_OPERAND_BUFFER_SIZE / 0x1000;
    let mut qword = 0usize;
    for buffer in 0..G17P_COLD_OPENING_OPERAND_BUFFER_COUNT {
        let base = G17P_COLD_OPENING_OPERAND_BUFFER_BASE
            .checked_add(buffer as u64 * G17P_COLD_OPENING_OPERAND_BUFFER_STRIDE)
            .ok_or(SubmissionError::PartialOpeningAddressOverflow)?;
        for page in 0..pages_per_buffer {
            put_u64_le(out, qword * 8, base + page * 0x1000);
            qword += 1;
        }
    }
    Ok(())
}

/// Write the queue-context page for the clean first-partial TA/3D pair.
///
/// This is the new local grid-0/1 generation selected by
/// `G17P_PARTIAL_OPENING_GRAPH=1`. Its namespace tag and fragment locators are
/// distinct from the untagged full-opening pair-zero context.
pub(crate) fn apply_g17p_cold_opening_queue_context(
    stage: G17PColdOpeningStage,
    descriptor: u64,
    queue: u64,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != G17P_COLD_OPENING_QUEUE_CONTEXT_SIZE {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    out.fill(0);
    let (descriptor_base, words, locators): (u64, &[(usize, u64)], &[(usize, u64)]) = match stage {
        G17PColdOpeningStage::Tiling => (
            G17P_COLD_OPENING_TA_DESCRIPTOR_BASE,
            &[
                (0x200, 0x1000_0000_0000_0004),
                (0x220, 0xffff_0c00_0000_0001),
                (0x378, 0x003f_ffff_ffff_ffff),
            ],
            &[(0x350, 0x0002_3803_8000_0003)],
        ),
        G17PColdOpeningStage::Fragment => (
            G17P_COLD_OPENING_3D_DESCRIPTOR_BASE,
            &[
                (0x200, 0x1000_0400_0000_0004),
                (0x220, 0xffff_1800_0000_0003),
                (0x228, 0x0000_0000_0000_0001),
                (0x230, 0x0000_0100_0000_0000),
                (0x378, 0x003f_ffff_ffff_ffff),
            ],
            &[
                (0x350, 0x0002_b003_8000_4c05),
                (0x358, 0x0000_8003_8000_4c3e),
                (0x360, 0x0000_b803_8000_4c77),
                (0x368, 0x0000_5003_8000_4cb0),
            ],
        ),
    };
    let descriptor_delta = descriptor
        .checked_sub(descriptor_base)
        .ok_or(SubmissionError::PartialOpeningDescriptorLocatorUnencodable)?;
    if descriptor_delta & 0x1f != 0 {
        return Err(SubmissionError::PartialOpeningDescriptorLocatorUnencodable);
    }
    let locator_delta = descriptor_delta / 0x20;
    for (offset, value) in words {
        put_u64_le(out, *offset, *value);
    }
    for (offset, value) in locators {
        put_u64_le(
            out,
            *offset,
            value
                .checked_add(locator_delta)
                .ok_or(SubmissionError::PartialOpeningDescriptorLocatorUnencodable)?,
        );
    }
    put_u64_le(out, 0x210, descriptor);
    put_u64_le(out, 0x218, queue);
    Ok(())
}

/// Derive the five stable status-B writes presented before control 0x84.
pub(crate) const fn prepare_g17p_partial_opening_pre_0x84_status(
    kernel_va_base: u64,
) -> Result<[G17PPartialOpeningStatusWrite; 5], SubmissionError> {
    let fwctl_va = match kernel_va_base.checked_add(G17P_PARTIAL_OPENING_FWCTL_OFFSET) {
        Some(value) => value,
        None => return Err(SubmissionError::PartialOpeningAddressOverflow),
    };
    let fwctl_next = match fwctl_va.checked_add(G17P_PARTIAL_OPENING_CONTROL_MESSAGE_SIZE) {
        Some(value) => value,
        None => return Err(SubmissionError::PartialOpeningAddressOverflow),
    };
    Ok([
        G17PPartialOpeningStatusWrite {
            offset: 0x4018,
            value: 0x0005_0601_0000_0000,
        },
        G17PPartialOpeningStatusWrite {
            offset: 0x4020,
            value: 61,
        },
        G17PPartialOpeningStatusWrite {
            offset: 0x40b0,
            value: 5,
        },
        G17PPartialOpeningStatusWrite {
            offset: 0x48e0,
            value: fwctl_va,
        },
        G17PPartialOpeningStatusWrite {
            offset: 0x48e8,
            value: fwctl_next,
        },
    ])
}

fn prepare_g17p_partial_opening_outer_slot(
    state: G17PPartialOpeningOuterChannelState,
    channel_table_index: u8,
    queue_grid_index: u8, // the QID this slot dispatches to
    kind: u32,
    channel_started: bool,
) -> Result<G17PPartialOpeningOuterSlotPlan, SubmissionError> {
    if state.producer >= G17P_PARTIAL_OPENING_OUTER_SLOT_COUNT
        || state
            .consumers
            .iter()
            .any(|consumer| *consumer >= G17P_PARTIAL_OPENING_OUTER_SLOT_COUNT)
    {
        return Err(SubmissionError::PartialOpeningChannelCounterOutOfRange);
    }
    if state.queue_write_index > u16::MAX as u32 {
        return Err(SubmissionError::PartialOpeningQueueHeadOutOfRange);
    }

    let next_producer = (state.producer + 1) & 0xff;
    if state.consumers.contains(&next_producer) {
        return Err(SubmissionError::RingFull);
    }

    let mut slot = [0u8; G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE];
    put_u64_le(&mut slot, 0x08, state.queue_gpu_va);
    put_u32_le(&mut slot, 0x10, kind);
    put_u32_le(
        &mut slot,
        0x14,
        state.queue_write_index
            | ((queue_grid_index as u32) << 16)
            | (if channel_started { 0 } else { 1 << 24 }),
    );

    Ok(G17PPartialOpeningOuterSlotPlan {
        channel_table_index,
        slot_index: state.producer as u8,
        slot_byte_offset: state.producer * G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE as u32,
        slot,
        next_producer,
    })
}

pub(crate) fn prepare_g17p_compute_channel_work(
    state: G17PPartialOpeningOuterChannelState,
    queue_id: u8,
) -> Result<PreparedG17PComputeChannelWork, SubmissionError> {
    let outer = prepare_g17p_partial_opening_outer_slot(
        state,
        G17P_COMPUTE_CHANNEL_TABLE_INDEX,
        queue_id,
        PipeType::Compute as u32,
        false,
    )?;
    Ok(PreparedG17PComputeChannelWork {
        outer,
        doorbell: G17P_COMPUTE_WORK_DOORBELL,
    })
}

pub(crate) fn prepare_g17p_partial_opening_work_pair(
    fragment: G17PPartialOpeningOuterChannelState,
    tiling: G17PPartialOpeningOuterChannelState,
    fragment_queue_id: u8,
    tiling_queue_id: u8,
    first_submit: bool,
) -> Result<PreparedG17PPartialOpeningWorkPair, SubmissionError> {
    let render_channel_started = !first_submit;
    let fragment = match prepare_g17p_partial_opening_outer_slot(
        fragment,
        G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX,
        fragment_queue_id,
        1,
        render_channel_started,
    ) {
        Ok(value) => value,
        Err(error) => return Err(error),
    };
    let tiling = match prepare_g17p_partial_opening_outer_slot(
        tiling,
        G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX,
        tiling_queue_id,
        0,
        render_channel_started,
    ) {
        Ok(value) => value,
        Err(error) => return Err(error),
    };
    Ok(PreparedG17PPartialOpeningWorkPair {
        fragment,
        tiling,
        paired_doorbell: G17P_PARTIAL_OPENING_PAIRED_DOORBELL,
    })
}

/// Layout-only forms used to make accidental field drift a compile-time
/// error. The intervening words remain unnamed because their semantics are
/// outside the admitted producer/consumer subset.
#[allow(dead_code)]
#[repr(C)]
struct G17PCachedChannelStateLayoutAssert {
    uncached_state_gpu_va: u64,
    command_pointer_ring_gpu_va: u64,
    _unknown_10: u64,
    _unknown_18: u32,
    coherency_cursor: u32,
    _unknown_20: [u8; 0x60],
}

#[allow(dead_code)]
#[repr(C)]
struct G17PUncachedChannelStateLayoutAssert {
    consumer: u32,
    _unknown_04: [u8; 0x0c],
    _unknown_10: u32,
    _unknown_14: [u8; 0x0c],
    _unknown_20: u32,
    _unknown_24: [u8; 0x0c],
    coherency_shadow: u32,
    _unknown_34: [u8; 0x0c],
    producer: u32,
    _unknown_44: [u8; 0x0c],
    recovery_target: u32,
    _unknown_54: [u8; 0x0c],
    ring_entry_count: u32,
}

const _: () = {
    assert!(core::mem::size_of::<G17PCachedChannelStateLayoutAssert>() == 0x80);
    assert!(
        core::mem::offset_of!(G17PCachedChannelStateLayoutAssert, uncached_state_gpu_va) == 0x00
    );
    assert!(
        core::mem::offset_of!(
            G17PCachedChannelStateLayoutAssert,
            command_pointer_ring_gpu_va
        ) == 0x08
    );
    assert!(core::mem::offset_of!(G17PCachedChannelStateLayoutAssert, coherency_cursor) == 0x1c);

    assert!(core::mem::size_of::<G17PUncachedChannelStateLayoutAssert>() == 0x64);
    assert!(core::mem::offset_of!(G17PUncachedChannelStateLayoutAssert, consumer) == 0x00);
    assert!(core::mem::offset_of!(G17PUncachedChannelStateLayoutAssert, coherency_shadow) == 0x30);
    assert!(core::mem::offset_of!(G17PUncachedChannelStateLayoutAssert, producer) == 0x40);
    assert!(core::mem::offset_of!(G17PUncachedChannelStateLayoutAssert, recovery_target) == 0x50);
    assert!(core::mem::offset_of!(G17PUncachedChannelStateLayoutAssert, ring_entry_count) == 0x60);
};


pub(crate) const AGX2_RING_STATE_GPU_DONEPTR_OFFSET: u32 = 0x00;
pub(crate) const AGX2_RING_STATE_GPU_RPTR_OFFSET: u32 = 0x30;
pub(crate) const AGX2_RING_STATE_CPU_WPTR_OFFSET: u32 = 0x40;
pub(crate) const AGX2_RING_STATE_RB_SIZE_OFFSET: u32 = 0x50;

/// AGX2 work-queue entries are u64 command GPU virtual addresses.
pub(crate) const AGX2_RING_ENTRY_BYTES: u32 = 8;

pub(crate) const HOST_MAX_OUTSTANDING_JOB_SLOTS: u32 = 127;

/// Advance a producer/consumer index within a ring of `size` entries.
///
/// Return an error for a zero-sized ring or a producer outside the ring. A
/// valid `index` is always below `u32::MAX`, so `index + 1` cannot overflow
/// after these checks.
pub(crate) const fn ring_next(index: u32, size: u32) -> Result<u32, SubmissionError> {
    if size == 0 {
        return Err(SubmissionError::RingSizeZero);
    }
    if index >= size {
        return Err(SubmissionError::RingIndexOutOfRange);
    }
    Ok((index + 1) % size)
}

// ---------------------------------------------------------------------------
// 4. SKSM kick word + per-QID kick-record table (AGX3-specific).
//    Grounded from GFX 0x10c88-0x10d2c. The canonical kick-word *encoder* lives
//    in `g17_rtkit::KickWord`; these constants are the same firmware bytes,
//    re-cited so this module stands alone. `g17_boot` cross-checks them against
//    `g17_rtkit` to prove no drift.
// ---------------------------------------------------------------------------

/// TS occupies bits [39:0] (`and x0,x5,#0xffffffffff` @ 0x10c8c). 40-bit, per
/// the firmware print `"TS=0x%010llx"`.
pub(crate) const KICK_TS_BITS: u32 = 40;
pub(crate) const KICK_TS_MASK: u64 = (1u64 << KICK_TS_BITS) - 1;
/// QID occupies bits [46:40] (`ubfx x1,x5,#40,#7` @ 0x10c90).
pub(crate) const KICK_QID_SHIFT: u32 = 40;
pub(crate) const KICK_QID_BITS: u32 = 7;
pub(crate) const KICK_QID_MASK: u8 = (1u8 << KICK_QID_BITS) - 1;
/// Bit 46 also selects one of two 64-bit valid-queue mask words
/// (`ubfx x6,x5,#46,#1` @ 0x10c9c).
pub(crate) const KICK_SELECTOR_BIT: u32 = 46;
/// Bits [63:47] are unmodeled and must be zero.
pub(crate) const KICK_RESERVED_MASK: u64 = !((1u64 << (KICK_SELECTOR_BIT + 1)) - 1);

/// Per-QID kick-record table base offset from the valid-mask base
/// (`add x2,x22,#0x10` @ 0x10c94).
pub(crate) const KICK_RECORD_TABLE_BASE: u32 = 0x10;
/// Per-QID record stride (`add x2,x2,x1,lsl#5` @ 0x10c98).
pub(crate) const KICK_RECORD_STRIDE: u32 = 0x20;
/// 128 queue slots: 7-bit QID, two 64-bit valid-mask words selected by bit 46.
pub(crate) const KICK_RECORD_COUNT: u32 = 128;

/// Fields of one 0x20-byte per-QID kick record (written across 0x10cb0-0x10d2c).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct KickRecordLayout {
    /// `+0x00` u64: first/min TS seen for this QID.
    pub(crate) ts_first: u32,
    /// `+0x08` u64: last/max TS seen for this QID.
    pub(crate) ts_last: u32,
    /// `+0x10` u64: pending kick-slot bitmask.
    pub(crate) slot_mask: u32,
    /// `+0x18` u32: valid/first flag.
    pub(crate) valid_flag: u32,
    /// `+0x1c` u32: winning kick-slot index.
    pub(crate) slot_index: u32,
}

/// The exact offsets, so a change to the firmware layout fails a test.
pub(crate) const KICK_RECORD_LAYOUT: KickRecordLayout = KickRecordLayout {
    ts_first: 0x00,
    ts_last: 0x08,
    slot_mask: 0x10,
    valid_flag: 0x18,
    slot_index: 0x1c,
};

/// Byte offset of one firmware kick record: `0x10 + QID * 0x20`. Mirrors
/// `g17_rtkit::kick_queue_record_offset`.
pub(crate) const fn kick_record_offset(queue_id: u8) -> Result<u32, SubmissionError> {
    if queue_id > KICK_QID_MASK {
        Err(SubmissionError::QueueIdOutOfRange)
    } else {
        Ok(KICK_RECORD_TABLE_BASE + queue_id as u32 * KICK_RECORD_STRIDE)
    }
}

/// The firmware-visible fields of a decoded SKSM kick word.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SksmKickWord {
    pub(crate) timestamp: u64,
    pub(crate) queue_id: u8,
    pub(crate) mask_selector: u8,
}

/// Encode the exact raw SKSM kick fields, leaving bits [63:47] clear.
pub(crate) const fn encode_kick_word(timestamp: u64, queue_id: u8) -> Result<u64, SubmissionError> {
    if timestamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }
    if queue_id > KICK_QID_MASK {
        return Err(SubmissionError::QueueIdOutOfRange);
    }
    Ok(timestamp | ((queue_id as u64) << KICK_QID_SHIFT))
}

/// Decode a kick word with the firmware's exact bitfields, refusing the
/// unmodeled upper bits. Kept parallel to `g17_rtkit::KickWord::decode_strict`.
pub(crate) const fn decode_kick_word(word: u64) -> Result<SksmKickWord, SubmissionError> {
    if word & KICK_RESERVED_MASK != 0 {
        return Err(SubmissionError::KickWordUnmodeledBits);
    }
    Ok(SksmKickWord {
        timestamp: word & KICK_TS_MASK,
        queue_id: ((word >> KICK_QID_SHIFT) as u8) & KICK_QID_MASK,
        mask_selector: ((word >> KICK_SELECTOR_BIT) & 1) as u8,
    })
}


/// Stamp sequence in bits [39:8] of a host SKSM timestamp.
pub(crate) const KICK_STAMP_SHIFT: u32 = 8;
pub(crate) const KICK_STAMP_MASK: u64 = u32::MAX as u64;
/// Entry/stamp slot in bits [7:0] of a host SKSM timestamp.
pub(crate) const KICK_STAMP_INDEX_MASK: u64 = 0xff;
/// The low-byte slot representation permits at most 256 runtime stamps.
pub(crate) const KICK_NUM_STAMPS_MAX: u16 = 256;

/// Decoded host-side timestamp state before a kick is published.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct KickTimestamp {
    pub(crate) stamp: u32,
    pub(crate) stamp_index: u8,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedKickSubmission {
    /// Slot selected from the current timestamp's low byte.
    pub(crate) entry_index: u8,
    /// Byte offset from the runtime queue entry base.
    pub(crate) entry_offset: u32,
    /// Current 40-bit timestamp consumed by this entry.
    pub(crate) timestamp: u64,
    pub(crate) queue_id: u8,
    /// Timestamp stored at queue +0x48 for the following submission.
    pub(crate) next_timestamp: u64,
}

/// Decode the exact host timestamp split: stamp[31:0] followed by slot[7:0].
pub(crate) const fn decode_kick_timestamp(
    timestamp: u64,
) -> Result<KickTimestamp, SubmissionError> {
    if timestamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }

    Ok(KickTimestamp {
        stamp: ((timestamp >> KICK_STAMP_SHIFT) & KICK_STAMP_MASK) as u32,
        stamp_index: (timestamp & KICK_STAMP_INDEX_MASK) as u8,
    })
}

/// Encode the host timestamp layout without a queue ID.
pub(crate) const fn encode_kick_timestamp(timestamp: KickTimestamp) -> u64 {
    ((timestamp.stamp as u64) << KICK_STAMP_SHIFT) | timestamp.stamp_index as u64
}

pub(crate) const fn g17p_serialized_render_scratch_payload(submission_ordinal: u32) -> u8 {
    submission_ordinal.wrapping_add(1) as u8
}

pub(crate) const fn encode_g17p_3d_completion_scratch(
    timestamp: u64,
    payload: u8,
) -> Result<u64, SubmissionError> {
    if timestamp & !KICK_TS_MASK != 0 {
        Err(SubmissionError::KickTimestampOutOfRange)
    } else {
        Ok(timestamp | ((payload as u64) << KICK_QID_SHIFT))
    }
}

pub(crate) const fn prepare_kick_submission(
    current_timestamp: u64,
    queue_id: u8,
    num_stamps: u16,
    entry_stride: u32,
) -> Result<PreparedKickSubmission, SubmissionError> {
    let current = match decode_kick_timestamp(current_timestamp) {
        Ok(timestamp) => timestamp,
        Err(err) => return Err(err),
    };

    if num_stamps == 0 || num_stamps > KICK_NUM_STAMPS_MAX {
        return Err(SubmissionError::KickStampCountOutOfRange);
    }
    if current.stamp_index as u16 >= num_stamps {
        return Err(SubmissionError::KickStampIndexOutOfRange);
    }
    if entry_stride == 0 {
        return Err(SubmissionError::KickEntryStrideZero);
    }

    let index = current.stamp_index as u32;
    if index != 0 && entry_stride > u32::MAX / index {
        return Err(SubmissionError::KickEntryOffsetOverflow);
    }
    let entry_offset = index * entry_stride;

    if queue_id > KICK_QID_MASK as u8 {
        return Err(SubmissionError::QueueIdOutOfRange);
    }

    let next_index = ((current.stamp_index as u16 + 1) % num_stamps) as u8;
    let next_stamp = if next_index == 0 {
        current.stamp.wrapping_add(1)
    } else {
        current.stamp
    };
    let next_timestamp = encode_kick_timestamp(KickTimestamp {
        stamp: next_stamp,
        stamp_index: next_index,
    });

    Ok(PreparedKickSubmission {
        entry_index: current.stamp_index,
        entry_offset,
        timestamp: current_timestamp,
        queue_id,
        next_timestamp,
    })
}


pub(crate) const G17P_CL_KICK_ENTRY_SIZE: usize = 0x180;
pub(crate) const G17P_CL_DEPENDENCIES_MAX: usize = 32;
pub(crate) const G17P_CL_EXPLICIT_DEPENDENCIES_MAX: usize = G17P_CL_DEPENDENCIES_MAX - 1;
const G17P_CL_PACKED_ADDRESS_MASK: u64 = (1u64 << 38) - 1;

/// Proven allocation and timestamp geometry for one target/backend pair.
///
/// Only A18 Pro's G17P HAL200 hybrid has an admitted tuple. Classic and
/// HAL300 remain deliberately absent until their own host contracts are
/// recovered.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmQueueGeometry {
    target: G17ChannelAbiTarget,
    backend: G17ClSubmissionBackend,
    allocation_alignment: u64,
    entry_stride: u32,
    stamp_count: u16,
    timestamp_seed: u64,
    backing_size: usize,
}

impl G17SksmQueueGeometry {
    pub(crate) const fn validate(self) -> Result<(), SubmissionError> {
        if !matches!(
            (self.target, self.backend),
            (
                G17ChannelAbiTarget::A18ProG17P,
                G17ClSubmissionBackend::G17PHal200Hybrid
            )
        ) {
            return Err(SubmissionError::KickQueueGeometryUnconfirmed);
        }
        if self.allocation_alignment == 0
            || self.allocation_alignment & (self.allocation_alignment - 1) != 0
            || self.entry_stride < G17P_CL_KICK_ENTRY_SIZE as u32
            || self.stamp_count == 0
            || self.stamp_count > KICK_NUM_STAMPS_MAX
            || self.timestamp_seed & !KICK_TS_MASK != 0
            || (self.timestamp_seed & KICK_STAMP_INDEX_MASK) >= self.stamp_count as u64
            || self.backing_size as u64
                != self.entry_stride as u64 * self.stamp_count as u64
            || self.backing_size as u64 % self.allocation_alignment != 0
        {
            return Err(SubmissionError::KickQueueGeometryInvalid);
        }
        Ok(())
    }

    pub(crate) const fn allocation_alignment(self) -> u64 {
        self.allocation_alignment
    }

    pub(crate) const fn mapping_alignment(self, minimum: u64) -> u64 {
        if self.allocation_alignment > minimum {
            self.allocation_alignment
        } else {
            minimum
        }
    }

    pub(crate) const fn entry_stride(self) -> u32 {
        self.entry_stride
    }

    pub(crate) const fn stamp_count(self) -> u16 {
        self.stamp_count
    }

    pub(crate) const fn timestamp_seed(self) -> u64 {
        self.timestamp_seed
    }

    pub(crate) const fn backing_size(self) -> usize {
        self.backing_size
    }
}

pub(crate) const T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY: G17SksmQueueGeometry =
    G17SksmQueueGeometry {
        target: G17ChannelAbiTarget::A18ProG17P,
        backend: G17ClSubmissionBackend::G17PHal200Hybrid,
        allocation_alignment: 0x100,
        entry_stride: 0x200,
        stamp_count: 0x100,
        timestamp_seed: 1,
        backing_size: 0x20000,
    };

pub(crate) const fn g17_sksm_queue_geometry(
    target: G17ChannelAbiTarget,
    backend: G17ClSubmissionBackend,
) -> Option<G17SksmQueueGeometry> {
    match (target, backend) {
        (G17ChannelAbiTarget::A18ProG17P, G17ClSubmissionBackend::G17PHal200Hybrid) => {
            Some(T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY)
        }
        _ => None,
    }
}

const _: () = assert!(T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.entry_stride as usize
    * T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.stamp_count as usize
    == T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.backing_size);
const _: () = assert!(T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.entry_stride as usize
    >= G17P_CL_KICK_ENTRY_SIZE);
const _: () = assert!(T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.allocation_alignment == 0x100);
const _: () = assert!(T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.timestamp_seed == 1);

/// Host-object fields used while selecting, publishing, and retiring one CL
/// kick entry. These are CPU-object offsets, not firmware queue-record offsets.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClKickQueueStateLayout {
    pub(crate) queue_id: usize,
    pub(crate) num_stamps: usize,
    pub(crate) entry_stride: usize,
    pub(crate) completion_selector: usize,
    pub(crate) submitted: usize,
    pub(crate) current_timestamp: usize,
    pub(crate) last_add_kicks_timestamp: usize,
    pub(crate) entry_cpu_base: usize,
    pub(crate) entry_gpu_base: usize,
    pub(crate) base_timestamp: usize,
    pub(crate) previous_valid: usize,
    pub(crate) previous_timestamp: usize,
}

pub(crate) const G17P_CL_KICK_QUEUE_STATE_LAYOUT: G17PClKickQueueStateLayout =
    G17PClKickQueueStateLayout {
        queue_id: 0x0c,
        num_stamps: 0x10,
        entry_stride: 0x14,
        completion_selector: 0x1c,
        submitted: 0x44,
        current_timestamp: 0x48,
        last_add_kicks_timestamp: 0x50,
        entry_cpu_base: 0x78,
        entry_gpu_base: 0x80,
        base_timestamp: 0x90,
        previous_valid: 0x98,
        previous_timestamp: 0xa0,
    };

/// Fixed byte positions inside the 0x180-byte G17P CL entry body.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClKickEntryLayout {
    pub(crate) header: usize,
    pub(crate) mcache: usize,
    pub(crate) payload: [usize; 2],
    pub(crate) dependency_control: usize,
    pub(crate) dependencies: usize,
    pub(crate) event_mask: [usize; 4],
    pub(crate) rce_kind: usize,
    pub(crate) rce_bindings: [usize; 4],
    pub(crate) zero_170: usize,
    pub(crate) auxiliary: usize,
}

pub(crate) const G17P_CL_KICK_ENTRY_LAYOUT: G17PClKickEntryLayout = G17PClKickEntryLayout {
    header: 0x00,
    mcache: 0x08,
    payload: [0x10, 0x18],
    dependency_control: 0x20,
    dependencies: 0x28,
    event_mask: [0x128, 0x130, 0x138, 0x140],
    rce_kind: 0x148,
    rce_bindings: [0x150, 0x158, 0x160, 0x168],
    zero_170: 0x170,
    auxiliary: 0x178,
};

/// One explicit barrier dependency after its descriptor-selected shared stamp
/// has been loaded. `event_word` is barrier-record word zero; record word one
/// selects the shared-stamp source and has no independent wire field.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClBarrierDependency {
    pub(crate) event_word: u32,
    pub(crate) shared_stamp: u64,
    pub(crate) queue_id: u8,
}

/// Optional MCache aperture descriptor copied into entry +0x08.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClMcacheAperture {
    pub(crate) address: u64,
    pub(crate) index: u8,
    pub(crate) count: u8,
}

/// One of the four address/tag bindings written at +0x150..+0x168.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClRceBinding {
    pub(crate) address: u64,
    pub(crate) tag: u8,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PComputeClCommandState {
    pub(crate) event_mask: [u64; 4],
    pub(crate) rce_bindings: [G17PClRceBinding; 4],
}

/// Host-written prefix of the conditional tag-14 fallback command.
pub(crate) const G17P_COMPUTE_PRIMARY_EVENT_SIZE: usize = 0x15;
/// Host-written prefix of the descriptor-selected tag-16 command.
pub(crate) const G17P_COMPUTE_OPTIONAL_EVENT_SIZE: usize = 0x12;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PComputePrimaryEvent {
    pub(crate) queue_id: u16,
    pub(crate) descriptor_generation: u16,
    pub(crate) data_master: u8,
    pub(crate) old_timestamp: u64,
}

/// Descriptor accounting and bytes prepared only for the tag-14 fallback.
/// Direct-feature G17P skips this transport command.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PComputePrimaryEvent {
    pub(crate) descriptor_generation: u32,
    pub(crate) bytes: [u8; G17P_COMPUTE_PRIMARY_EVENT_SIZE],
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PComputeOptionalEvent {
    pub(crate) queue_id: u16,
    pub(crate) old_timestamp: u64,
}

/// Encode the tag-14 fallback command used when the direct feature is absent.
pub(crate) fn encode_g17p_compute_primary_event(
    event: G17PComputePrimaryEvent,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() < G17P_COMPUTE_PRIMARY_EVENT_SIZE {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }
    out[..G17P_COMPUTE_PRIMARY_EVENT_SIZE].fill(0);
    put_u32_le(out, 0x00, 0x0e);
    put_u16_le(out, 0x04, event.queue_id);
    put_u16_le(out, 0x06, event.descriptor_generation);
    out[0x08] = event.data_master;
    put_u64_le(out, 0x09, event.old_timestamp);
    put_u32_le(out, 0x11, 2);
    Ok(())
}

/// Advance the host descriptor's submitted-entry count for one CL entry.
pub(crate) fn advance_g17p_compute_descriptor_generation(
    generation_before: u32,
    nop_count: u32,
) -> Result<u32, SubmissionError> {
    generation_before
        .checked_add(nop_count)
        .and_then(|generation| generation.checked_add(1))
        .ok_or(SubmissionError::ComputeEventCounterOverflow)
}

/// Prepare the tag-14 fallback bytes. Required NOPs are accounted before the
/// one CL entry; the direct-feature path advances the same generation but
/// does not call this fallback encoder or publish its pointer.
pub(crate) fn prepare_g17p_compute_primary_event(
    generation_before: u32,
    nop_count: u32,
    queue_id: u8,
    data_master: u8,
    old_timestamp: u64,
) -> Result<PreparedG17PComputePrimaryEvent, SubmissionError> {
    let descriptor_generation =
        advance_g17p_compute_descriptor_generation(generation_before, nop_count)?;
    let event_generation = u16::try_from(descriptor_generation)
        .map_err(|_| SubmissionError::ComputeEventCounterOverflow)?;
    let mut bytes = [0u8; G17P_COMPUTE_PRIMARY_EVENT_SIZE];
    encode_g17p_compute_primary_event(
        G17PComputePrimaryEvent {
            queue_id: u16::from(queue_id),
            descriptor_generation: event_generation,
            data_master,
            old_timestamp,
        },
        &mut bytes,
    )?;
    Ok(PreparedG17PComputePrimaryEvent {
        descriptor_generation,
        bytes,
    })
}

pub(crate) const G17P_KSM_ADD_KICKS_COMMAND_SIZE: usize = 0x15;
/// Bytes the matched J700 A000 handler cache-invalidates before reading the
/// record (`mov w1, #0x18` at `0xfffffc0000011714`). The three bytes past the
/// host-written extent are never loaded, but must not be stale.
pub(crate) const G17P_KSM_ADD_KICKS_COMMAND_FETCH_SIZE: usize = 0x18;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PKsmAddKicksCommand {
    pub(crate) queue_id: u8,
    pub(crate) add_count: u32,
    pub(crate) queue_priority: u8,
    pub(crate) entry_stamp: u64,
}

/// Bytes for one tag-14 record, ready to copy into retained queue storage.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PKsmAddKicks {
    pub(crate) bytes: [u8; G17P_KSM_ADD_KICKS_COMMAND_SIZE],
}

pub(crate) fn prepare_g17p_ksm_add_kicks_command(
    command: G17PKsmAddKicksCommand,
) -> Result<PreparedG17PKsmAddKicks, SubmissionError> {
    if command.queue_id > KICK_QID_MASK {
        return Err(SubmissionError::QueueIdOutOfRange);
    }
    if command.queue_priority > 3 {
        return Err(SubmissionError::KickQueueCompletionSelectorOutOfRange);
    }
    if command.entry_stamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }
    if command.add_count == 0 {
        return Err(SubmissionError::KsmAddKicksCountZero);
    }
    let add_count = match u16::try_from(command.add_count) {
        Ok(add_count) => add_count,
        Err(_) => return Err(SubmissionError::KsmAddKicksCountOutOfRange),
    };

    let mut bytes = [0u8; G17P_KSM_ADD_KICKS_COMMAND_SIZE];
    put_u32_le(
        &mut bytes,
        0x00,
        G17PTopLevelCommandType::KsmKickQueueAddKicks as u32,
    );
    put_u16_le(&mut bytes, 0x04, u16::from(command.queue_id));
    put_u16_le(&mut bytes, 0x06, add_count);
    bytes[0x08] = command.queue_priority;
    put_u64_le(&mut bytes, 0x09, command.entry_stamp);
    put_u32_le(&mut bytes, 0x11, G17P_COMPUTE_DATA_MASTER);
    Ok(PreparedG17PKsmAddKicks { bytes })
}

/// Encode tag 16 only when the host descriptor carries its optional pointer.
/// The simple direct first add3 profile does not publish this record.
pub(crate) fn encode_g17p_compute_optional_event(
    event: G17PComputeOptionalEvent,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() < G17P_COMPUTE_OPTIONAL_EVENT_SIZE {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }
    out[..G17P_COMPUTE_OPTIONAL_EVENT_SIZE].fill(0);
    put_u32_le(out, 0x00, 0x10);
    put_u16_le(out, 0x04, event.queue_id);
    put_u64_le(out, 0x06, event.old_timestamp);
    put_u32_le(out, 0x0e, 0xfe);
    Ok(())
}

pub(crate) fn prepare_g17p_compute_cl_command_state(
    descriptor_low: u64,
    submission_ordinal: u32,
) -> Result<G17PComputeClCommandState, SubmissionError> {
    let next = submission_ordinal
        .checked_add(1)
        .ok_or(SubmissionError::ComputeEventCounterOverflow)?;
    let event_previous = u64::from(submission_ordinal)
        .checked_mul(2)
        .ok_or(SubmissionError::ComputeEventCounterOverflow)?;
    let event_current = u64::from(next)
        .checked_mul(4)
        .ok_or(SubmissionError::ComputeEventCounterOverflow)?;
    let rce_a = descriptor_low
        .checked_add(0x40)
        .ok_or(SubmissionError::ComputeRceAddressOverflow)?;
    let rce_b = descriptor_low
        .checked_add(0x760)
        .ok_or(SubmissionError::ComputeRceAddressOverflow)?;
    let empty = G17PClRceBinding { address: 0, tag: 0 };
    Ok(G17PComputeClCommandState {
        event_mask: [0, event_previous, event_current, 0],
        rce_bindings: [
            G17PClRceBinding {
                address: rce_a,
                tag: 0x22,
            },
            G17PClRceBinding {
                address: rce_b,
                tag: 0x04,
            },
            empty,
            empty,
        ],
    })
}

pub(crate) fn prepare_g17p_render_cl_command_state(
    descriptor_low: u64,
    tiling: bool,
) -> Result<G17PComputeClCommandState, SubmissionError> {
    const FRAGMENT_RCE_PROGRAM_STRIDE: u64 = 0x720;
    const FRAGMENT_RCE_PROGRAM_OFFSET: u64 = 0xa0;
    let binding = |offset: u64, tag: u8| {
        descriptor_low
            .checked_add(offset)
            .map(|address| G17PClRceBinding { address, tag })
            .ok_or(SubmissionError::ComputeRceAddressOverflow)
    };
    let empty = G17PClRceBinding { address: 0, tag: 0 };

    let rce_bindings = if tiling {
        [binding(0x60, 0x47)?, empty, empty, empty]
    } else {
        let fragment_rce = descriptor_low
            .checked_add(FRAGMENT_RCE_PROGRAM_OFFSET)
            .ok_or(SubmissionError::ComputeRceAddressOverflow)?;
        let external_binding = |index: u64, tag: u8| {
            fragment_rce
                .checked_add(index * FRAGMENT_RCE_PROGRAM_STRIDE)
                .map(|address| G17PClRceBinding { address, tag })
                .ok_or(SubmissionError::ComputeRceAddressOverflow)
        };
        let full = [
            external_binding(0, 0x56)?,
            external_binding(1, 0x10)?,
            external_binding(2, 0x17)?,
            external_binding(3, 0x0a)?,
        ];
        match *crate::module_parameters::g17p_render_3d_rce.value() {
            0 => [empty, empty, empty, empty],
            1 => [full[0], empty, empty, empty],
            2 => [full[0], full[1], empty, empty],
            3 => [full[0], full[1], full[2], empty],
            _ => full,
        }
    };

    Ok(G17PComputeClCommandState {
        event_mask: [0; 4],
        rce_bindings,
    })
}

/// Physical-queue QoS fields packed above the 32-bit dependency mask at entry
/// +0x20. These remain with the registered SKSM queue when later descriptors
/// publish new per-command event masks.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClQosConfig {
    pub(crate) queue_byte_32: u8,
    pub(crate) class_40: u8,
    pub(crate) word_48: u16,
}

/// Physical queue configuration and mutable producer state read before one CL
/// entry. The QoS fields belong to this retained queue, while the event mask in
/// [`G17PClKickEntryOperands`] belongs to the command being submitted.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClKickProducerState {
    pub(crate) queue_id: u8,
    pub(crate) geometry: G17SksmQueueGeometry,
    pub(crate) completion_selector: u8,
    pub(crate) qos: G17PClQosConfig,
    pub(crate) submitted: u32,
    pub(crate) current_timestamp: u64,
    pub(crate) last_add_kicks_timestamp: u64,
    pub(crate) base_timestamp: u64,
    pub(crate) previous_valid: bool,
    pub(crate) previous_timestamp: u64,
}

impl G17SksmQueueGeometry {
    pub(crate) const fn initial_producer_state(
        self,
        queue_id: u8,
        completion_selector: u8,
        qos: G17PClQosConfig,
    ) -> Result<G17PClKickProducerState, SubmissionError> {
        match self.validate() {
            Ok(()) => {}
            Err(error) => return Err(error),
        }
        if queue_id > KICK_QID_MASK {
            return Err(SubmissionError::QueueIdOutOfRange);
        }
        if completion_selector > 3 {
            return Err(SubmissionError::KickQueueCompletionSelectorOutOfRange);
        }
        Ok(G17PClKickProducerState {
            queue_id,
            geometry: self,
            completion_selector,
            qos,
            submitted: 0,
            current_timestamp: self.timestamp_seed,
            last_add_kicks_timestamp: self.timestamp_seed,
            base_timestamp: self.timestamp_seed,
            previous_valid: false,
            previous_timestamp: 0,
        })
    }
}

/// Descriptor and call-site operands consumed by `encodeCLKickEntry`.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClKickEntryOperands<'a> {
    /// Descriptor +0x4c bit 0; true sets entry-header bit 1.
    pub(crate) descriptor_flag_4c: bool,
    /// Descriptor +0x5c8 bit 0; true sets entry-header bit 61.
    pub(crate) descriptor_flag_5c8: bool,
    pub(crate) converted_command_timestamp: u64,
    pub(crate) barriers: &'a [G17PClBarrierDependency],
    pub(crate) mcache: Option<G17PClMcacheAperture>,
    /// The two qwords reached through `AGXKSMKickQueueEntryPayload`.
    pub(crate) payload: [u64; 2],
    /// Per-command mask copied independently to +0x128..+0x140.
    pub(crate) event_mask: [u64; 4],
    pub(crate) rce_kind: u8,
    pub(crate) rce_bindings: [G17PClRceBinding; 4],
    /// Descriptor +0x178 when nonzero, otherwise the descriptor-indexed host
    /// table value selected by +0x61c.
    pub(crate) auxiliary: u64,
}

/// Pure result of one entry encode and the host state values published after
/// the entry barrier. No shared memory or MMIO is touched by this function.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PClKickEntry {
    /// Bytes the live publisher must clear before copying `entry`.
    pub(crate) zero_length: u32,
    pub(crate) entry: [u8; G17P_CL_KICK_ENTRY_SIZE],
    pub(crate) entry_index: u8,
    pub(crate) entry_offset: u32,
    pub(crate) current_timestamp: u64,
    pub(crate) next_timestamp: u64,
    pub(crate) submitted_before: u32,
    pub(crate) submitted_after: u32,
    pub(crate) previous_timestamp_after: u64,
    /// Producer state to use for the next entry on this physical queue.
    pub(crate) producer_after: G17PClKickProducerState,
}

/// Ordering at the side-effect boundary of the host encoder.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PClKickPublicationStep {
    ZeroEntryStride,
    EntryBody,
    InnerShareableBarrier,
    NextTimestamp,
    SubmittedAtomicAdd,
    PreviousValid,
    PreviousTimestamp,
}

pub(crate) const G17P_CL_KICK_PUBLICATION_ORDER: [G17PClKickPublicationStep; 7] = [
    G17PClKickPublicationStep::ZeroEntryStride,
    G17PClKickPublicationStep::EntryBody,
    G17PClKickPublicationStep::InnerShareableBarrier,
    G17PClKickPublicationStep::NextTimestamp,
    G17PClKickPublicationStep::SubmittedAtomicAdd,
    G17PClKickPublicationStep::PreviousValid,
    G17PClKickPublicationStep::PreviousTimestamp,
];

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PFirstSubmitPublicationStep {
    AcquireGfx1RunningPower,
    PublishB2,
    PublishConfigUpdatePointer,
    ConfigureQid4Hardware,
    ZeroSksmStride,
    WriteSksmEntry,
    EntryBarrier,
    PublishSksmProducer,
    AdvanceDescriptorGeneration,
    RecordHostCorrelationToken,
    PublishEntrySignalPointer,
    AddKicks,
    PublishCl2,
    SendActivation,
    DirectKickBarrier,
    SendDirectKick,
}

pub(crate) const G17P_FIRST_SUBMIT_PUBLICATION_ORDER: [G17PFirstSubmitPublicationStep; 16] = [
    G17PFirstSubmitPublicationStep::AcquireGfx1RunningPower,
    G17PFirstSubmitPublicationStep::PublishB2,
    G17PFirstSubmitPublicationStep::PublishConfigUpdatePointer,
    G17PFirstSubmitPublicationStep::ConfigureQid4Hardware,
    G17PFirstSubmitPublicationStep::ZeroSksmStride,
    G17PFirstSubmitPublicationStep::WriteSksmEntry,
    G17PFirstSubmitPublicationStep::EntryBarrier,
    G17PFirstSubmitPublicationStep::PublishSksmProducer,
    G17PFirstSubmitPublicationStep::AdvanceDescriptorGeneration,
    G17PFirstSubmitPublicationStep::RecordHostCorrelationToken,
    G17PFirstSubmitPublicationStep::PublishEntrySignalPointer,
    G17PFirstSubmitPublicationStep::AddKicks,
    G17PFirstSubmitPublicationStep::PublishCl2,
    G17PFirstSubmitPublicationStep::SendActivation,
    G17PFirstSubmitPublicationStep::DirectKickBarrier,
    G17PFirstSubmitPublicationStep::SendDirectKick,
];

fn encode_g17p_cl_dependency(timestamp: u64, queue_id: u8) -> Result<u64, SubmissionError> {
    if timestamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }
    if queue_id > KICK_QID_MASK as u8 {
        return Err(SubmissionError::KickDependencyQueueIdOutOfRange);
    }
    Ok(timestamp | ((queue_id as u64) << KICK_QID_SHIFT))
}

fn encode_g17p_cl_barrier_dependency(
    queue: G17PClKickProducerState,
    barrier: G17PClBarrierDependency,
) -> Result<u64, SubmissionError> {
    if barrier.queue_id > KICK_QID_MASK as u8 {
        return Err(SubmissionError::KickDependencyQueueIdOutOfRange);
    }

    let stamp_count = queue.geometry.stamp_count() as u64;
    let event_timestamp = (((barrier.event_word as u64) >> 8)
        | barrier.shared_stamp.wrapping_shl(24))
    .wrapping_sub(1);
    let event_stamp = event_timestamp / stamp_count;
    let event_index = event_timestamp % stamp_count;
    let index_sum = (queue.base_timestamp & KICK_STAMP_INDEX_MASK) + event_index;
    let index_carry = index_sum / stamp_count;
    let stamp = ((queue.base_timestamp >> KICK_STAMP_SHIFT) as u32)
        .wrapping_add(event_stamp as u32)
        .wrapping_add(index_carry as u32);
    let index = (index_sum % stamp_count) as u8;

    encode_g17p_cl_dependency(
        encode_kick_timestamp(KickTimestamp {
            stamp,
            stamp_index: index,
        }),
        barrier.queue_id,
    )
}

fn encode_g17p_cl_mcache(aperture: G17PClMcacheAperture) -> Result<u64, SubmissionError> {
    if aperture.index > 0x3f || aperture.count == 0 || aperture.count > 0x40 {
        return Err(SubmissionError::KickMcacheFieldOutOfRange);
    }
    if aperture.address & 0x1f != 0 || aperture.address >> 5 > G17P_CL_PACKED_ADDRESS_MASK {
        return Err(SubmissionError::KickMcacheAddressUnencodable);
    }

    Ok((aperture.index as u64)
        | ((aperture.address >> 5) << 6)
        | (((aperture.count as u64 - 1) & 0x3f) << 54))
}

fn encode_g17p_cl_rce(binding: G17PClRceBinding) -> Result<u64, SubmissionError> {
    if binding.tag > KICK_QID_MASK as u8 {
        return Err(SubmissionError::KickRceTagOutOfRange);
    }
    if binding.address & 0x1f != 0 || binding.address >> 5 > G17P_CL_PACKED_ADDRESS_MASK {
        return Err(SubmissionError::KickRceAddressUnencodable);
    }

    Ok((binding.address >> 5) | ((binding.tag as u64) << 43))
}

pub(crate) fn prepare_g17p_cl_kick_entry(
    queue: G17PClKickProducerState,
    operands: G17PClKickEntryOperands<'_>,
) -> Result<PreparedG17PClKickEntry, SubmissionError> {
    queue.geometry.validate()?;
    if queue.completion_selector > 3 {
        return Err(SubmissionError::KickQueueCompletionSelectorOutOfRange);
    }
    if queue.base_timestamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }
    if operands.barriers.len() > G17P_CL_EXPLICIT_DEPENDENCIES_MAX {
        return Err(SubmissionError::KickDependencyCountOutOfRange);
    }
    if queue.qos.class_40 > 0x1f {
        return Err(SubmissionError::KickQosClassOutOfRange);
    }
    if operands.rce_kind > 3 {
        return Err(SubmissionError::KickRceKindOutOfRange);
    }

    let prepared = prepare_kick_submission(
        queue.current_timestamp,
        queue.queue_id,
        queue.geometry.stamp_count(),
        queue.geometry.entry_stride(),
    )?;
    if queue.submitted >= queue.geometry.stamp_count() as u32 {
        return Err(SubmissionError::KickQueueSubmittedFull);
    }

    let mut entry = [0u8; G17P_CL_KICK_ENTRY_SIZE];
    let layout = G17P_CL_KICK_ENTRY_LAYOUT;

    let mut header = queue.current_timestamp << 2;
    header |= (queue.queue_id as u64) << 42;
    header |= (queue.completion_selector as u64) << 59;
    if operands.descriptor_flag_4c {
        header |= 1 << 1;
    }
    if let Some(aperture) = operands.mcache {
        header |= 1 << 58;
        put_u64_le(&mut entry, layout.mcache, encode_g17p_cl_mcache(aperture)?);
    }
    if operands.descriptor_flag_5c8 {
        header |= 1 << 61;
    }
    put_u64_le(&mut entry, layout.header, header);

    for (offset, value) in layout.payload.into_iter().zip(operands.payload) {
        put_u64_le(&mut entry, offset, value);
    }

    for (index, barrier) in operands.barriers.iter().copied().enumerate() {
        put_u64_le(
            &mut entry,
            layout.dependencies + index * 8,
            encode_g17p_cl_barrier_dependency(queue, barrier)?,
        );
    }

    let implicit_timestamp = if queue.previous_valid {
        queue.previous_timestamp
    } else {
        operands.converted_command_timestamp.wrapping_sub(1)
    };
    let implicit_index = operands.barriers.len();
    // DIAGNOSTIC. On a first submit this declares a self-dependency on stamp 0
    // (`converted_command_timestamp` is 1, minus one). Measured on hardware at
    // the stall: the TA entry's dependency word is 0x0000050000000000, i.e.
    // {qid 5, stamp 0}, while the hardware's completed register for that queue,
    // `0x21028[5]`, reads 0x862cde4910 -- not zero, and not the firmware's
    // "never completed = base-1 = 0" convention. Firmware assumes one value and
    // the hardware register holds another, so this dependency may be
    // unsatisfiable from cold. Omitting it isolates whether the SKSM front-end
    // is waiting on it.
    let omit_implicit = !queue.previous_valid
        && *crate::module_parameters::g17p_no_implicit_dep.value() != 0;
    if !omit_implicit {
        put_u64_le(
            &mut entry,
            layout.dependencies + implicit_index * 8,
            encode_g17p_cl_dependency(implicit_timestamp, queue.queue_id)?,
        );
    }

    let dependency_count = implicit_index + usize::from(!omit_implicit);
    let dependency_mask = if dependency_count == G17P_CL_DEPENDENCIES_MAX {
        u32::MAX
    } else if dependency_count == 0 {
        0
    } else {
        (1u32 << dependency_count) - 1
    };
    let dependency_control = dependency_mask as u64
        | ((queue.qos.queue_byte_32 as u64) << 32)
        | ((queue.qos.class_40 as u64) << 40)
        | ((queue.qos.word_48 as u64) << 48);
    put_u64_le(&mut entry, layout.dependency_control, dependency_control);

    for (offset, value) in layout.event_mask.into_iter().zip(operands.event_mask) {
        put_u64_le(&mut entry, offset, value);
    }
    entry[layout.rce_kind] = operands.rce_kind;
    for (offset, binding) in layout.rce_bindings.into_iter().zip(operands.rce_bindings) {
        put_u64_le(&mut entry, offset, encode_g17p_cl_rce(binding)?);
    }
    put_u64_le(&mut entry, layout.auxiliary, operands.auxiliary);

    let mut producer_after = queue;
    producer_after.current_timestamp = prepared.next_timestamp;
    producer_after.submitted = queue.submitted.wrapping_add(1);
    producer_after.previous_valid = true;
    producer_after.previous_timestamp = prepared.timestamp;

    Ok(PreparedG17PClKickEntry {
        zero_length: queue.geometry.entry_stride(),
        entry,
        entry_index: prepared.entry_index,
        entry_offset: prepared.entry_offset,
        current_timestamp: prepared.timestamp,
        next_timestamp: prepared.next_timestamp,
        submitted_before: queue.submitted,
        submitted_after: producer_after.submitted,
        previous_timestamp_after: prepared.timestamp,
        producer_after,
    })
}

pub(crate) fn complete_g17p_cl_kicks(
    mut producer: G17PClKickProducerState,
    completed_count: u32,
) -> Result<G17PClKickProducerState, SubmissionError> {
    if completed_count == 0 {
        return Ok(producer);
    }
    if producer.submitted == 0 || completed_count > producer.submitted {
        return Err(SubmissionError::KickCompletionUnderflow);
    }

    producer.submitted -= completed_count;
    Ok(producer)
}

/// One range inside provider-owned queue storage.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClStorageRange {
    pub(crate) offset: usize,
    pub(crate) size: usize,
}

/// The scheduler record at +0x200 aliases the registered queue's 16 KiB
/// shared-support page. A second full-page support write would erase it.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClRetainedStorageLayout {
    pub(crate) shared_support: G17PClStorageRange,
    pub(crate) scheduler: G17PClStorageRange,
}

pub(crate) const G17P_CL_RETAINED_STORAGE_LAYOUT: G17PClRetainedStorageLayout =
    G17PClRetainedStorageLayout {
        shared_support: G17PClStorageRange {
            offset: 0,
            size: 0x4000,
        },
        scheduler: G17PClStorageRange {
            offset: 0x200,
            size: 0x100,
        },
    };

/// Required provider-owned storage for the measured pre-B2 state writes.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PClB2PrimaryStateLayout {
    pub(crate) object_size: usize,
    pub(crate) computed_write_extent: usize,
    pub(crate) region_1_write_extent: usize,
    pub(crate) region_2_write_extent: usize,
}

pub(crate) const G17P_CL_B2_PRIMARY_STATE_LAYOUT: G17PClB2PrimaryStateLayout =
    G17PClB2PrimaryStateLayout {
        object_size: 0x4000,
        computed_write_extent: 0xe30,
        region_1_write_extent: 0x20,
        region_2_write_extent: 0x18,
    };

pub(crate) const fn g17p_cl_ranges_overlap(
    left: G17PClStorageRange,
    right: G17PClStorageRange,
) -> bool {
    let left_end = match left.offset.checked_add(left.size) {
        Some(end) => end,
        None => return false,
    };
    let right_end = match right.offset.checked_add(right.size) {
        Some(end) => end,
        None => return false,
    };
    left.offset < right_end && right.offset < left_end
}

pub(crate) const fn g17p_cl_range_contains(
    outer: G17PClStorageRange,
    inner: G17PClStorageRange,
) -> bool {
    let outer_end = match outer.offset.checked_add(outer.size) {
        Some(end) => end,
        None => return false,
    };
    let inner_end = match inner.offset.checked_add(inner.size) {
        Some(end) => end,
        None => return false,
    };
    outer.offset <= inner.offset && inner_end <= outer_end
}

/// A whole-page support initializer is valid only before queue registration.
pub(crate) const fn g17p_cl_full_support_initialization_allowed(registered: bool) -> bool {
    !registered
}

/// Initialize the queue-owned pages exactly once, before SKSM registration.
pub(crate) fn apply_g17p_cl_retained_static_state(
    shared_support: &mut [u8],
    support_state: &mut [u8],
    scheduler_state: &mut [u8],
    channel_control: &mut [u8],
    operand_table_gpu_va: u64,
    support_state_gpu_va: u64,
    scheduler_state_gpu_va: u64,
) -> Result<(), SubmissionError> {
    if shared_support.len() < 0x4000
        || support_state.len() < 0x4000
        || scheduler_state.len() < 0x4000
        || channel_control.len() < 0x40
    {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }

    shared_support.fill(0);
    support_state.fill(0);
    scheduler_state.fill(0);
    channel_control.fill(0);

    put_u64_le(shared_support, 0x00, 2);
    put_u64_le(shared_support, 0x08, 1);
    put_u64_le(shared_support, 0x10, 2);
    put_u64_le(shared_support, 0x18, 0x0004_0000_0000_0070);
    put_u64_le(shared_support, 0x20, 0x0000_1500_0000_0000);
    put_u64_le(shared_support, 0x28, 0x0000_1500_0000_0000);
    put_u64_le(shared_support, 0x30, operand_table_gpu_va);
    put_u64_le(shared_support, 0x40, 4);
    put_u32_le(shared_support, 0x48, 0xa8);
    put_u64_le(shared_support, 0x4c, support_state_gpu_va);
    put_u32_le(shared_support, 0x54, 0);
    put_u32_le(shared_support, 0x5c, 1);
    put_u32_le(shared_support, 0x60, 2);

    let scheduler = G17P_CL_RETAINED_STORAGE_LAYOUT.scheduler.offset;
    put_u64_le(shared_support, scheduler, scheduler_state_gpu_va + 8);
    put_u32_le(shared_support, scheduler + 0x08, 1);
    put_u32_le(shared_support, scheduler + 0x0c, 0);
    put_u32_le(shared_support, scheduler + 0x10, 0x50);
    put_u32_le(support_state, 0, 1);
    put_u32_le(scheduler_state, 8, 1);

    put_u64_le(channel_control, 0x00, 0x0000_0100_0000_ffff);
    put_u64_le(channel_control, 0x20, 0x0002_0000_0000_0000);
    put_u64_le(channel_control, 0x30, 0x0000_0000_ff00_0000);
    Ok(())
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PClB2ActivationState {
    Pending,
    Published,
}

pub(crate) fn apply_g17p_cl_b2_primary_state(
    activation: G17PClB2ActivationState,
    computed: &mut [u8],
    region_1: &mut [u8],
    region_2: &mut [u8],
    channel_control_gpu_va: u64,
) -> Result<G17PClB2ActivationState, SubmissionError> {
    if activation == G17PClB2ActivationState::Published {
        return Err(SubmissionError::KickB2PrimaryStateAlreadyPublished);
    }
    let layout = G17P_CL_B2_PRIMARY_STATE_LAYOUT;
    if computed.len() < layout.object_size
        || region_1.len() < layout.object_size
        || region_2.len() < layout.object_size
    {
        return Err(SubmissionError::KickB2PrimaryStateBufferTooSmall);
    }

    computed[0x000] = 0xff;
    computed[0x008] = 0xff;
    computed[0x010..0x012].copy_from_slice(&0x0f01u16.to_le_bytes());
    computed[0x018..0x01a].copy_from_slice(&0x0f01u16.to_le_bytes());
    put_u32_le(computed, 0x408, 2);
    put_u32_le(computed, 0x40c, 2);
    put_u32_le(computed, 0x604, 4);
    put_u64_le(computed, 0x808, channel_control_gpu_va);
    put_u32_le(computed, 0xc04, 0x1000);
    put_u64_le(computed, 0xe00, 7);
    put_u64_le(computed, 0xe08, 0x0000_0001_0e22_618a);
    put_u64_le(computed, 0xe20, 4);
    put_u64_le(computed, 0xe28, 4);

    put_u64_le(region_1, 0x10, 0x0000_0080_0007_7000);
    put_u32_le(region_1, 0x18, 0x17);
    put_u32_le(region_1, 0x1c, 0x17);

    put_u64_le(region_2, 0x00, 0x0800_0000_e000_0000);
    put_u64_le(region_2, 0x08, 0x0000_3d40_0000_3400);
    put_u64_le(region_2, 0x10, 0x0000_0000_0000_1d00);
    Ok(G17PClB2ActivationState::Published)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PClRetainedPublicationStep {
    InitializeSharedSupport,
    WriteSchedulerRecord,
    RegisterPhysicalQueue,
    PublishCommandOwnedState,
    CleanCommandOwnedState,
    WriteB2PrimaryState,
    CleanB2PrimaryState,
    SystemBarrierBeforeInnerPayload,
    PublishInnerQueuePayload,
    SystemBarrierBeforeInnerProducer,
    PublishInnerQueueProducer,
    SystemBarrierAfterInnerProducer,
}

/// One-time order that makes the scheduler alias valid before registration.
pub(crate) const G17P_CL_RETAINED_REGISTRATION_ORDER: [G17PClRetainedPublicationStep; 3] = [
    G17PClRetainedPublicationStep::InitializeSharedSupport,
    G17PClRetainedPublicationStep::WriteSchedulerRecord,
    G17PClRetainedPublicationStep::RegisterPhysicalQueue,
];

pub(crate) const G17P_CL_RETAINED_ACTIVATION_ORDER: [G17PClRetainedPublicationStep; 9] = [
    G17PClRetainedPublicationStep::PublishCommandOwnedState,
    G17PClRetainedPublicationStep::CleanCommandOwnedState,
    G17PClRetainedPublicationStep::WriteB2PrimaryState,
    G17PClRetainedPublicationStep::CleanB2PrimaryState,
    G17PClRetainedPublicationStep::SystemBarrierBeforeInnerPayload,
    G17PClRetainedPublicationStep::PublishInnerQueuePayload,
    G17PClRetainedPublicationStep::SystemBarrierBeforeInnerProducer,
    G17PClRetainedPublicationStep::PublishInnerQueueProducer,
    G17PClRetainedPublicationStep::SystemBarrierAfterInnerProducer,
];

pub(crate) const G17P_CL_RETAINED_SUBMISSION_ORDER: [G17PClRetainedPublicationStep; 6] = [
    G17PClRetainedPublicationStep::PublishCommandOwnedState,
    G17PClRetainedPublicationStep::CleanCommandOwnedState,
    G17PClRetainedPublicationStep::PublishInnerQueuePayload,
    G17PClRetainedPublicationStep::SystemBarrierBeforeInnerProducer,
    G17PClRetainedPublicationStep::PublishInnerQueueProducer,
    G17PClRetainedPublicationStep::SystemBarrierAfterInnerProducer,
];

pub(crate) const fn g17p_cl_retained_submission_order(
    activation: G17PClB2ActivationState,
) -> &'static [G17PClRetainedPublicationStep] {
    match activation {
        G17PClB2ActivationState::Pending => &G17P_CL_RETAINED_ACTIVATION_ORDER,
        G17PClB2ActivationState::Published => &G17P_CL_RETAINED_SUBMISSION_ORDER,
    }
}


/// One ordered transaction for the two SKSM scratch FIFO ports.
///
/// `word0` must be stored first and `word1` second. There is deliberately no
/// read operation in this publication interface.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17SksmOrderedWritePair {
    pub(crate) word0_offset: u32,
    pub(crate) word0: u64,
    pub(crate) word1_offset: u32,
    pub(crate) word1: u64,
}

/// Minimal backend for an ordered pair of 64-bit FIFO stores.
pub(crate) trait G17SksmPairWriter {
    type Error;

    fn write64(&self, offset: u32, value: u64) -> core::result::Result<(), Self::Error>;
}

/// Publish one pair in the only order accepted by the pinned host sequence.
pub(crate) fn publish_g17p_sksm_write_pair<P: G17SksmPairWriter>(
    port: &P,
    pair: G17SksmOrderedWritePair,
) -> core::result::Result<(), P::Error> {
    port.write64(pair.word0_offset, pair.word0)?;
    port.write64(pair.word1_offset, pair.word1)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum SksmAddKicksTarget {
    G17P,
    /// M5 / HAL300 G target.
    G17G,
    /// M5 Pro/Max HAL300 X-family target (G17S/G17C host implementation).
    G17X,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedSksmAddKicksMmio {
    /// First store: 64-bit target tag to `tag_offset`.
    pub(crate) tag_offset: u32,
    pub(crate) tag: u64,
    /// Second store: zero-extended number of newly published kicks.
    pub(crate) count_offset: u32,
    pub(crate) add_count: u32,
}

impl PreparedSksmAddKicksMmio {
    /// Convert the decoded host values to the exact two 64-bit FIFO stores.
    pub(crate) const fn write_pair(self) -> G17SksmOrderedWritePair {
        G17SksmOrderedWritePair {
            word0_offset: self.tag_offset,
            word0: self.tag,
            word1_offset: self.count_offset,
            word1: self.add_count as u64,
        }
    }
}

const fn sksm_add_kicks_tag(target: SksmAddKicksTarget, queue_id: u8) -> u64 {
    match target {
        SksmAddKicksTarget::G17P => G17P_ADD_KICKS_TAG | ((queue_id as u64) << 43),
        SksmAddKicksTarget::G17G => G17G_ADD_KICKS_TAG | ((queue_id as u64) << 44),
        SksmAddKicksTarget::G17X => G17X_ADD_KICKS_TAG | ((queue_id as u64) << 44),
    }
}

const G17P_ADD_KICKS_TAG: u64 = 0x0000_0400_0002_1018;
const G17G_ADD_KICKS_TAG: u64 = 0x0000_0800_0002_1018;
const G17X_ADD_KICKS_TAG: u64 = 0x0020_0800_0002_1018;
/// Prepare the host's two ordered MMIO stores without touching the aperture.
pub(crate) const fn prepare_sksm_add_kicks_mmio(
    target: SksmAddKicksTarget,
    queue_id: u8,
    add_count: u32,
    geometry: G17SksmScratchGeometry,
) -> Result<PreparedSksmAddKicksMmio, SubmissionError> {
    if queue_id > KICK_QID_MASK as u8 {
        return Err(SubmissionError::QueueIdOutOfRange);
    }

    let offsets = match geometry.fifo_offsets() {
        Ok(offsets) => offsets,
        Err(_) => return Err(SubmissionError::KickMmioOffsetOverflow),
    };

    Ok(PreparedSksmAddKicksMmio {
        tag_offset: offsets.write0,
        tag: sksm_add_kicks_tag(target, queue_id),
        count_offset: offsets.write1,
        add_count,
    })
}


/// Accelerator-object fields holding the three runtime geometry values.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PSksmRuntimeGeometryObjectLayout {
    pub(crate) write0_base_field: u32,
    pub(crate) write1_base_field: u32,
    pub(crate) stride_field: u32,
}

pub(crate) const G17P_SKSM_RUNTIME_GEOMETRY_OBJECT_LAYOUT: G17PSksmRuntimeGeometryObjectLayout =
    G17PSksmRuntimeGeometryObjectLayout {
        write0_base_field: 0x10730,
        write1_base_field: 0x10734,
        stride_field: 0x10738,
    };

/// QID-valid mask base written after the two queue-configuration stores.
pub(crate) const G17P_SKSM_QUEUE_ENABLE_MASK_BASE: u32 = 0x21068;
/// QID-disable mask base written before changing a registered queue's routing.
pub(crate) const G17P_SKSM_QUEUE_DISABLE_MASK_BASE: u32 = 0x21078;
/// Queue-entry address bits preserved by the host payload encoder.
pub(crate) const G17P_SKSM_QUEUE_ENTRY_ADDRESS_MASK: u64 = 0x0000_07ff_ffff_ffe0;
/// Fixed portion of the first G17P queue-configuration store.
const G17P_SKSM_QUEUE_CONFIG_TAG: u64 = 0x0000_0400_0002_1008;
const G17P_SKSM_QUEUE_COMPLETION_SELECTOR_MAX: u32 = 4;
/// `_AGFIDataMasterType` selected by the G17P TA channel.
pub(crate) const G17P_TA_DATA_MASTER: u32 = 0;
/// `_AGFIDataMasterType` selected by the G17P 3D channel.
pub(crate) const G17P_3D_DATA_MASTER: u32 = 1;
pub(crate) const G17P_COMPUTE_DATA_MASTER: u32 = 2;

/// Runtime routing state that the compute-channel setup owns for one queue.
///
/// The pinned host selects the queue by the CL channel QID, prepares the
/// queue's mapping resource, obtains a completion selector from the channel,
/// and stores `selector | (2 << 32)` at queue `+0x1c`. Keeping the selector
/// and queue address together prevents a caller from supplying a non-compute
/// data-master value to the production compute path.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PComputeQueueRouting {
    pub(crate) queue_id: u8,
    pub(crate) completion_selector: u32,
    pub(crate) entry_gpu_va: u64,
    pub(crate) geometry: G17SksmQueueGeometry,
}

/// Pure description of the ordered host-side publications that make one A18
/// Pro SKSM queue visible to the hardware bridge.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PSksmQueueConfig {
    /// First store: 64-bit QID tag.
    pub(crate) tag_offset: u32,
    pub(crate) tag: u64,
    /// Second store: 64-bit queue address/data-master/completion selector.
    pub(crate) payload_offset: u32,
    pub(crate) payload: u64,
    /// Final stores: high 64 QIDs first, then low 64 QIDs.
    pub(crate) enable_pair: G17SksmOrderedWritePair,
    /// Stores used before changing an already-enabled queue's routing.
    pub(crate) disable_pair: G17SksmOrderedWritePair,
}

impl PreparedG17PSksmQueueConfig {
    /// Convert the queue configuration to its two ordered write-only stores.
    pub(crate) const fn write_pair(self) -> G17SksmOrderedWritePair {
        G17SksmOrderedWritePair {
            word0_offset: self.tag_offset,
            word0: self.tag,
            word1_offset: self.payload_offset,
            word1: self.payload,
        }
    }
}

/// Prepare the exact G17P host queue-configuration stores without touching
/// MMIO. Only the paired A18 Pro identity is admitted.
pub(crate) const fn prepare_g17p_sksm_queue_config(
    target: G17ChannelAbiTarget,
    queue_id: u8,
    data_master: u32,
    completion_selector: u32,
    entry_gpu_va: u64,
    geometry: G17SksmScratchGeometry,
) -> Result<PreparedG17PSksmQueueConfig, SubmissionError> {
    match target {
        G17ChannelAbiTarget::A18ProG17P => {}
        G17ChannelAbiTarget::M5G17G | G17ChannelAbiTarget::M5ProMaxG17X => {
            return Err(SubmissionError::ChannelAbiUnconfirmed)
        }
    }
    if queue_id > KICK_QID_MASK {
        return Err(SubmissionError::QueueIdOutOfRange);
    }
    if data_master >= 3 {
        return Err(SubmissionError::KickQueueDataMasterOutOfRange);
    }
    if completion_selector > G17P_SKSM_QUEUE_COMPLETION_SELECTOR_MAX {
        return Err(SubmissionError::KickQueueCompletionSelectorOutOfRange);
    }
    if entry_gpu_va & !G17P_SKSM_QUEUE_ENTRY_ADDRESS_MASK != 0 {
        return Err(SubmissionError::KickQueueEntryAddressUnencodable);
    }

    let offsets = match geometry.fifo_offsets() {
        Ok(offsets) => offsets,
        Err(_) => return Err(SubmissionError::KickMmioOffsetOverflow),
    };

    // Host sequence: 3 * (4 - selector) + 0x3fe, shifted into bit 54.
    let completion_field = (3 * (4 - completion_selector) + 0x3fe) as u64;
    let payload =
        (((data_master as u64) + 1) << 60) | entry_gpu_va | completion_field.wrapping_shl(54);
    let low_mask = if queue_id < 64 { 1u64 << queue_id } else { 0 };
    let high_mask = if queue_id >= 64 {
        1u64 << (queue_id - 64)
    } else {
        0
    };
    let enable_pair = G17SksmOrderedWritePair {
        word0_offset: G17P_SKSM_QUEUE_ENABLE_MASK_BASE + 8,
        word0: high_mask,
        word1_offset: G17P_SKSM_QUEUE_ENABLE_MASK_BASE,
        word1: low_mask,
    };
    let disable_pair = G17SksmOrderedWritePair {
        word0_offset: G17P_SKSM_QUEUE_DISABLE_MASK_BASE + 8,
        word0: high_mask,
        word1_offset: G17P_SKSM_QUEUE_DISABLE_MASK_BASE,
        word1: low_mask,
    };

    Ok(PreparedG17PSksmQueueConfig {
        tag_offset: offsets.write0,
        tag: G17P_SKSM_QUEUE_CONFIG_TAG | ((queue_id as u64) << 43),
        payload_offset: offsets.write1,
        payload,
        enable_pair,
        disable_pair,
    })
}

pub(crate) const fn prepare_g17p_compute_sksm_queue_config(
    routing: G17PComputeQueueRouting,
    geometry: G17SksmScratchGeometry,
) -> Result<PreparedG17PSksmQueueConfig, SubmissionError> {
    match routing.geometry.validate() {
        Ok(()) => {}
        Err(error) => return Err(error),
    }
    if routing.entry_gpu_va & (routing.geometry.allocation_alignment() - 1) != 0 {
        return Err(SubmissionError::KickQueueEntryAddressUnencodable);
    }
    prepare_g17p_sksm_queue_config(
        G17ChannelAbiTarget::A18ProG17P,
        routing.queue_id,
        G17P_COMPUTE_DATA_MASTER,
        routing.completion_selector,
        routing.entry_gpu_va,
        geometry,
    )
}

pub(crate) const fn prepare_g17p_render_sksm_queue_config(
    routing: G17PComputeQueueRouting,
    data_master: u32,
    geometry: G17SksmScratchGeometry,
) -> Result<PreparedG17PSksmQueueConfig, SubmissionError> {
    match routing.geometry.validate() {
        Ok(()) => {}
        Err(error) => return Err(error),
    }
    if data_master != G17P_TA_DATA_MASTER && data_master != G17P_3D_DATA_MASTER {
        return Err(SubmissionError::KickQueueDataMasterOutOfRange);
    }
    if routing.entry_gpu_va & (routing.geometry.allocation_alignment() - 1) != 0 {
        return Err(SubmissionError::KickQueueEntryAddressUnencodable);
    }
    prepare_g17p_sksm_queue_config(
        G17ChannelAbiTarget::A18ProG17P,
        routing.queue_id,
        data_master,
        routing.completion_selector,
        routing.entry_gpu_va,
        geometry,
    )
}

pub(crate) const G17P_KSM_COMPLETION_QUEUE_COUNT: u8 = 4;
pub(crate) const G17P_KSM_COMPLETION_DESCRIPTOR_SIZE: usize = 0x20;
pub(crate) const G17P_KSM_COMPLETION_ENTRY_SIZE: u32 = 0x40;
pub(crate) const G17P_KSM_COMPLETION_CAPACITY: u32 = 0x800;
pub(crate) const G17P_KSM_COMPLETION_BACKING_SIZE: usize =
    G17P_KSM_COMPLETION_CAPACITY as usize * G17P_KSM_COMPLETION_ENTRY_SIZE as usize;
pub(crate) const G17P_KSM_COMPLETION_PRESENT_ORDINALS: [u8; 2] = [0, 2];
/// Normal direct CL completion uses ordinal 0, independently of the compute
/// SKSM queue's numeric completion selector 2.
pub(crate) const G17P_NORMAL_CL_COMPLETION_ORDINAL: u8 = 0;

/// Exact field offsets in one host-produced G17P completion descriptor.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PKsmCompletionDescriptorLayout {
    pub(crate) gpu_va: usize,
    pub(crate) cpu_va: usize,
    pub(crate) capacity: usize,
    pub(crate) entry_size: usize,
    pub(crate) ordinal: usize,
    pub(crate) processed: usize,
}

pub(crate) const G17P_KSM_COMPLETION_DESCRIPTOR_LAYOUT: G17PKsmCompletionDescriptorLayout =
    G17PKsmCompletionDescriptorLayout {
        gpu_va: 0x00,
        cpu_va: 0x08,
        capacity: 0x10,
        entry_size: 0x14,
        ordinal: 0x18,
        processed: 0x1c,
    };

/// Encode one present completion queue exactly as the G17P host populates it.
pub(crate) fn encode_g17p_ksm_completion_descriptor(
    target: G17ChannelAbiTarget,
    ordinal: u8,
    gpu_va: u64,
    cpu_va: u64,
    capacity: u32,
) -> Result<[u8; G17P_KSM_COMPLETION_DESCRIPTOR_SIZE], SubmissionError> {
    if target != G17ChannelAbiTarget::A18ProG17P {
        return Err(SubmissionError::ChannelAbiUnconfirmed);
    }
    if ordinal >= G17P_KSM_COMPLETION_QUEUE_COUNT {
        return Err(SubmissionError::CompletionQueueOrdinalOutOfRange);
    }
    if capacity == 0 {
        return Err(SubmissionError::CompletionQueueCapacityZero);
    }

    let mut raw = [0u8; G17P_KSM_COMPLETION_DESCRIPTOR_SIZE];
    let layout = G17P_KSM_COMPLETION_DESCRIPTOR_LAYOUT;
    put_u64_le(&mut raw, layout.gpu_va, gpu_va);
    put_u64_le(&mut raw, layout.cpu_va, cpu_va);
    put_u32_le(&mut raw, layout.capacity, capacity);
    put_u32_le(&mut raw, layout.entry_size, G17P_KSM_COMPLETION_ENTRY_SIZE);
    put_u32_le(&mut raw, layout.ordinal, ordinal as u32);
    put_u32_le(&mut raw, layout.processed, 0);
    Ok(raw)
}

/// The host-side completion descriptor and SKSM bridge equations are grounded;
/// this does not assert that Linux knows the physical IRQ source or has
/// observed an interrupt round trip.
pub(crate) const G17P_COMPLETION_HOST_ABI_GROUNDED: bool = true;


/// Offset and width of the little-endian top-level command tag.
pub(crate) const G17P_TOP_LEVEL_COMMAND_TAG_OFFSET: u32 = 0;
pub(crate) const G17P_TOP_LEVEL_COMMAND_TAG_WIDTH: u32 = 4;

/// Host-grounded G17P top-level command types.
///
/// This type intentionally excludes unresolved tags and every nested
/// SKU-stream opcode. `Nop` records the host ABI but is invalid for
/// [`admit_g17p_top_level_command`] because all matched firmware variants send
/// tag 17 to the invalid-command fatal path.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum G17PTopLevelCommandType {
    Ta = 0,
    ThreeD = 1,
    FastBlit = 2,
    Cl = 3,
    Barrier = 4,
    PmGrow = 5,
    RemoteNode = 7,
    SharedEventSignal = 9,
    AddFinalFrgGtpcKick = 12,
    KsmKickQueueAddKicks = 14,
    ConfigUpdate = 15,
    EntrySignal = 16,
    Nop = 17,
    LateEvalEventSignal = 18,
}

/// Highest tag admitted by the G17P jump-table bounds check.
pub(crate) const G17P_CHANNEL_COMMAND_TYPE_MAX: u32 = 18;

/// Tags named by the pinned host's top-level ABI, including invalid `Nop`.
pub(crate) const G17P_TOP_LEVEL_COMMAND_KNOWN_MASK: u32 = 0x0007_d2bf;
/// In-range top-level tags whose host/firmware semantics remain unresolved.
pub(crate) const G17P_TOP_LEVEL_COMMAND_UNRESOLVED_MASK: u32 = 0x0000_0d40;
/// Tag 13 is in range but is neither a grounded host type nor unresolved.
pub(crate) const G17P_TOP_LEVEL_COMMAND_INVALID_MASK: u32 = 0x0000_2000;

/// Tags whose firmware jump-table entries reach a normal processing case.
pub(crate) const G17P_CHANNEL_COMMAND_PROCESS_MASK: u32 = 0x0005_d2bf;
/// Tag 6 has a dedicated PM-rebuild-in-SKSM fatal path.
pub(crate) const G17P_CHANNEL_COMMAND_PM_REBUILD_MASK: u32 = 0x0000_0040;
/// In-range tags whose jump-table entries reach the invalid-command fatal.
pub(crate) const G17P_CHANNEL_COMMAND_INVALID_MASK: u32 = 0x0002_2d00;
/// Literal mask used by the optional pre-dispatch filter for tags <= 16.
pub(crate) const G17P_CHANNEL_COMMAND_PREFILTER_MASK: u32 = 0x0001_d03f;

/// Firmware disposition for one raw command tag.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PChannelCommandDisposition {
    Process,
    PmRebuildForbiddenFatal,
    InvalidCommandFatal,
}

/// Admit the command ABI only for the exact paired G17P identity.
const fn ensure_g17p_command_abi(target: G17ChannelAbiTarget) -> Result<(), SubmissionError> {
    match target {
        G17ChannelAbiTarget::A18ProG17P => Ok(()),
        G17ChannelAbiTarget::M5G17G | G17ChannelAbiTarget::M5ProMaxG17X => {
            Err(SubmissionError::CommandAbiUnconfirmed)
        }
    }
}

/// Read the complete little-endian u32 tag at top-level command offset zero.
const fn g17p_raw_top_level_command_tag(command: &[u8]) -> Result<u32, SubmissionError> {
    let offset = G17P_TOP_LEVEL_COMMAND_TAG_OFFSET as usize;
    let end = offset + G17P_TOP_LEVEL_COMMAND_TAG_WIDTH as usize;
    if command.len() < end {
        return Err(SubmissionError::CommandHeaderTooShort);
    }
    Ok(u32::from_le_bytes([
        command[offset],
        command[offset + 1],
        command[offset + 2],
        command[offset + 3],
    ]))
}

/// Decode a host-grounded top-level G17P command name.
///
/// This records producer identity only. Call
/// [`admit_g17p_top_level_command`] before treating a decoded tag as
/// consumable by the matched firmware.
pub(crate) const fn decode_g17p_top_level_command(
    target: G17ChannelAbiTarget,
    command: &[u8],
) -> Result<G17PTopLevelCommandType, SubmissionError> {
    match ensure_g17p_command_abi(target) {
        Ok(()) => {}
        Err(error) => return Err(error),
    }

    let raw = match g17p_raw_top_level_command_tag(command) {
        Ok(raw) => raw,
        Err(error) => return Err(error),
    };
    match raw {
        0 => Ok(G17PTopLevelCommandType::Ta),
        1 => Ok(G17PTopLevelCommandType::ThreeD),
        2 => Ok(G17PTopLevelCommandType::FastBlit),
        3 => Ok(G17PTopLevelCommandType::Cl),
        4 => Ok(G17PTopLevelCommandType::Barrier),
        5 => Ok(G17PTopLevelCommandType::PmGrow),
        7 => Ok(G17PTopLevelCommandType::RemoteNode),
        9 => Ok(G17PTopLevelCommandType::SharedEventSignal),
        12 => Ok(G17PTopLevelCommandType::AddFinalFrgGtpcKick),
        14 => Ok(G17PTopLevelCommandType::KsmKickQueueAddKicks),
        15 => Ok(G17PTopLevelCommandType::ConfigUpdate),
        16 => Ok(G17PTopLevelCommandType::EntrySignal),
        17 => Ok(G17PTopLevelCommandType::Nop),
        18 => Ok(G17PTopLevelCommandType::LateEvalEventSignal),
        6 | 8 | 10 | 11 => Err(SubmissionError::CommandTagUnresolved),
        _ => Err(SubmissionError::CommandTagInvalid),
    }
}

/// Classify a raw tag exactly like the matched G17P firmware jump table.
pub(crate) const fn classify_g17p_channel_command(
    target: G17ChannelAbiTarget,
    raw: u32,
) -> Result<G17PChannelCommandDisposition, SubmissionError> {
    match ensure_g17p_command_abi(target) {
        Ok(()) => {}
        Err(error) => return Err(error),
    }

    if raw > G17P_CHANNEL_COMMAND_TYPE_MAX {
        return Ok(G17PChannelCommandDisposition::InvalidCommandFatal);
    }

    let bit = 1u32 << raw;
    if bit & G17P_CHANNEL_COMMAND_PROCESS_MASK != 0 {
        Ok(G17PChannelCommandDisposition::Process)
    } else if bit & G17P_CHANNEL_COMMAND_PM_REBUILD_MASK != 0 {
        Ok(G17PChannelCommandDisposition::PmRebuildForbiddenFatal)
    } else {
        Ok(G17PChannelCommandDisposition::InvalidCommandFatal)
    }
}

/// Decode a top-level tag and require a normal matched-firmware dispatch case.
///
/// In particular, host-grounded `Nop = 17` is invalid here because the
/// firmware sends it to the invalid-command fatal path.
pub(crate) const fn admit_g17p_top_level_command(
    target: G17ChannelAbiTarget,
    command: &[u8],
) -> Result<G17PTopLevelCommandType, SubmissionError> {
    let command_type = match decode_g17p_top_level_command(target, command) {
        Ok(command_type) => command_type,
        Err(error) => return Err(error),
    };
    match classify_g17p_channel_command(target, command_type as u32) {
        Ok(G17PChannelCommandDisposition::Process) => Ok(command_type),
        Ok(
            G17PChannelCommandDisposition::PmRebuildForbiddenFatal
            | G17PChannelCommandDisposition::InvalidCommandFatal,
        ) => Err(SubmissionError::CommandTagInvalidForFirmware),
        Err(error) => Err(error),
    }
}

/// Apply the firmware's optional `tag <= 16 && bit(tag) & 0x1d03f` filter.
pub(crate) const fn g17p_channel_command_prefilter_allows(
    target: G17ChannelAbiTarget,
    raw: u32,
) -> Result<bool, SubmissionError> {
    match ensure_g17p_command_abi(target) {
        Ok(()) => {}
        Err(error) => return Err(error),
    }
    Ok(raw <= 16 && ((1u32 << raw) & G17P_CHANNEL_COMMAND_PREFILTER_MASK) != 0)
}


/// How each submission-ABI component was grounded, so reuse-vs-fork is explicit.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AbiOrigin {
    /// Same layout as the AGX2 producer; reused unchanged.
    IdenticalToAgx2,
    /// AGX3-specific layout.
    ForkedAgx3Grounded,
    /// AGX2 layout the driver produces, not confirmed for G17. Kept unsupported.
    Agx2InheritedUnconfirmed,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SubmissionAbiLedger {
    pub(crate) tx_doorbell: AbiOrigin,
    pub(crate) channel_state: AbiOrigin,
    pub(crate) g17p_channel_state: AbiOrigin,
    pub(crate) ring_state: AbiOrigin,
    pub(crate) channel_descriptor: AbiOrigin,
    pub(crate) host_channel_state_reset: AbiOrigin,
    pub(crate) host_uncached_channel_reset: AbiOrigin,
    pub(crate) g17p_command_types: AbiOrigin,
    pub(crate) kick_word: AbiOrigin,
    pub(crate) kick_queue_producer: AbiOrigin,
    pub(crate) host_add_kicks_mmio: AbiOrigin,
    pub(crate) g17p_queue_config: AbiOrigin,
    pub(crate) g17p_completion_descriptors: AbiOrigin,
    pub(crate) kick_record: AbiOrigin,
}

pub(crate) const SUBMISSION_ABI_LEDGER: SubmissionAbiLedger = SubmissionAbiLedger {
    tx_doorbell: AbiOrigin::IdenticalToAgx2,
    channel_state: AbiOrigin::Agx2InheritedUnconfirmed,
    g17p_channel_state: AbiOrigin::ForkedAgx3Grounded,
    ring_state: AbiOrigin::Agx2InheritedUnconfirmed,
    channel_descriptor: AbiOrigin::ForkedAgx3Grounded,
    host_channel_state_reset: AbiOrigin::ForkedAgx3Grounded,
    host_uncached_channel_reset: AbiOrigin::ForkedAgx3Grounded,
    g17p_command_types: AbiOrigin::ForkedAgx3Grounded,
    kick_word: AbiOrigin::ForkedAgx3Grounded,
    kick_queue_producer: AbiOrigin::ForkedAgx3Grounded,
    host_add_kicks_mmio: AbiOrigin::ForkedAgx3Grounded,
    g17p_queue_config: AbiOrigin::ForkedAgx3Grounded,
    g17p_completion_descriptors: AbiOrigin::ForkedAgx3Grounded,
    kick_record: AbiOrigin::ForkedAgx3Grounded,
};

pub(crate) const HOST_SKSM_ADD_KICKS_PATH_SELECTED: bool = true;

pub(crate) const CHANNEL_DESCRIPTOR_STRIDE_CONFIRMED: bool = true;

/// The offline-unprovable pieces that keep the transport non-executable. Each
/// needs a live target device.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SubmissionLiveGaps {
    /// Linux has an ordered pair writer on resource 0, including fixed-slot
    /// geometry and serialization across queue owners.
    pub(crate) sksm_resource0_ordered_write_implemented: bool,
    /// EP 0x21 plus the retained CL2 channel queue has executed real work.
    pub(crate) channel_doorbell_round_trip_proven: bool,
    /// Linux allocates and publishes every object in the measured first-partial
    /// graphics contract through its runtime path.
    pub(crate) partial_graphics_runtime_implemented: bool,
    /// The measured first-partial channel path executed paired graphics work.
    pub(crate) partial_graphics_round_trip_proven: bool,
    pub(crate) sksm_add_kicks_round_trip_proven: bool,
    pub(crate) completion_irq_route_proven: bool,
    /// Ordinal-0 notification, entry, QID/timestamp, and EP0x20 stamp
    /// correlation is implemented as a pure transaction before outstanding
    /// subtraction. Physical IRQ/event-ring/ACK ownership remains separate.
    pub(crate) normal_cl_completion_consumer_implemented: bool,
}

impl SubmissionLiveGaps {
    pub(crate) const fn classic_graphics_proven(&self) -> bool {
        self.channel_doorbell_round_trip_proven
            && self.partial_graphics_runtime_implemented
            && self.partial_graphics_round_trip_proven
            && self.completion_irq_route_proven
    }

    pub(crate) const fn native_sksm_proven(&self) -> bool {
        self.sksm_resource0_ordered_write_implemented
            && self.sksm_add_kicks_round_trip_proven
            && self.normal_cl_completion_consumer_implemented
            && self.completion_irq_route_proven
    }

    /// True when either independently modeled transport can complete work.
    pub(crate) const fn all_proven(&self) -> bool {
        self.classic_graphics_proven() || self.native_sksm_proven()
    }
}

pub(crate) const SUBMISSION_LIVE_GAPS: SubmissionLiveGaps = SubmissionLiveGaps {
    sksm_resource0_ordered_write_implemented: true,
    channel_doorbell_round_trip_proven: true,
    partial_graphics_runtime_implemented: false,
    partial_graphics_round_trip_proven: true,
    sksm_add_kicks_round_trip_proven: false,
    completion_irq_route_proven: false,
    normal_cl_completion_consumer_implemented: true,
};


/// One parameter-buffer descriptor ring entry.
pub(crate) const G17P_PBDESC_ENTRY_SIZE: usize = 16;

/// The four quantities the handshake exchanges: two 22-bit counters and two
/// flags. This is `AGXSPBDescRingState`.
#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct G17PPbDescRingState {
    /// Descriptor dword2, low 22 bits.
    pub(crate) counter_a: u32,
    /// Descriptor dword3, low 22 bits.
    pub(crate) counter_b: u32,
    /// Descriptor dword3 bit 31.
    pub(crate) flag_high: u32,
    /// Descriptor dword0 bit 0.
    pub(crate) flag_low: u32,
}

pub(crate) const fn g17p_pbdesc_segment(pb_address: u64) -> u64 {
    let base: u64 = if pb_address & 0x400_0000_0000 == 0 {
        0x70_0000_0000
    } else {
        0
    };
    let high = (pb_address >> 3) & 0x80_0000_0000;
    ((base.wrapping_add(pb_address)) & 0x70_0000_0000) | high
}

pub(crate) const fn g17p_pbdesc_reconstructed_address(pb_address: u64) -> u64 {
    (g17p_pbdesc_segment(pb_address).wrapping_add(0x10_0000_0000))
        | (pb_address & 0x0f_ffff_ffff)
}

pub(crate) const fn encode_g17p_pbdesc(
    previous: [u32; 4],
    pb_address: u64,
    pb_state_word_34: u32,
    state: G17PPbDescRingState,
) -> [u32; 4] {
    let segment = g17p_pbdesc_segment(pb_address);

    // dword0: address >> 4, low 3 bits kept, then bit 0 = flag_low.
    let mut dword0 = ((pb_address >> 4) as u32 & !0x7) | (previous[0] & 0x7);
    dword0 = (dword0 & !1) | if state.flag_low != 0 { 1 } else { 0 };

    // dword1: segment >> 8 in the top nibble, low 28 kept, then low 22 from
    // PBState[+0x34].
    let mut dword1 = ((segment >> 8) as u32 & !0x0fff_ffff) | (previous[1] & 0x0fff_ffff);
    dword1 = (dword1 & !0x003f_ffff) | (pb_state_word_34 & 0x003f_ffff);

    // dword2/dword3: the two 22-bit counters, plus bit 31.
    let dword2 = (previous[2] & !0x003f_ffff) | (state.counter_a & 0x003f_ffff);
    let mut dword3 = (previous[3] & !0x003f_ffff) | (state.counter_b & 0x003f_ffff);
    dword3 = (dword3 & !0x8000_0000) | ((state.flag_high & 1) << 31);

    [dword0, dword1, dword2, dword3]
}

pub(crate) const fn decode_g17p_pbdesc(descriptor: [u32; 4]) -> G17PPbDescRingState {
    G17PPbDescRingState {
        counter_a: descriptor[2] & 0x003f_ffff,
        counter_b: descriptor[3] & 0x003f_ffff,
        flag_high: descriptor[3] >> 31,
        flag_low: descriptor[0] & 1,
    }
}

/// Byte offset of one descriptor within the ring.
pub(crate) const fn g17p_pbdesc_offset(index: u32) -> usize {
    index as usize * G17P_PBDESC_ENTRY_SIZE
}


pub(crate) const G17P_UMA_DESC_ENTRY_SIZE: usize = 0x20;
pub(crate) const G17P_UMA_DESC_ENTRY_COUNT: u32 = 0x100;

#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct G17PUmaPagePoolState {
    pub(crate) page_list: u64,
    pub(crate) unit_pages: u32,
    pub(crate) counter_a: u32,
    pub(crate) counter_b: u32,
    pub(crate) empty: u32,
    pub(crate) total_pages: u32,
}

pub(crate) const fn encode_g17p_umadesc(
    previous: [u64; 4],
    state: G17PUmaPagePoolState,
) -> [u64; 4] {
    [
        ((state.unit_pages as u64) << 41) | (state.page_list >> 7),
        ((state.counter_b as u64) << 33)
            | ((state.counter_a as u64) << 5)
            | ((state.empty as u64) << 61),
        state.total_pages as u64,
        previous[3],
    ]
}

pub(crate) const fn g17p_umadesc_offset(hardware_buffer_id: u32) -> Option<usize> {
    if hardware_buffer_id >= G17P_UMA_DESC_ENTRY_COUNT {
        return None;
    }
    Some(hardware_buffer_id as usize * G17P_UMA_DESC_ENTRY_SIZE)
}


pub(crate) const G17P_DEVCTL_USC_FREELIST: u32 = 0x20;

pub(crate) const G17P_USC_FREELIST_LOW_VA: u64 = 0x70_0020_8000;
pub(crate) const G17P_USC_FREELIST_LOW_END: u64 = 0x70_0020_8440;
pub(crate) const G17P_USC_FREELIST_GROW_DESCRIPTOR_BYTES: u32 = 0x28;

/// The three fields that move between boots. 256x256 and 1024x1024 gave identical values for `word_04`, and
/// `word_0c` moved by 2 between them, so none of these encode geometry -- they
/// read as per-submission or per-boot sequence counters.
#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct G17PUscFreelistSequence {
    /// +0x04. Observed 1, 1, 6.
    pub(crate) word_04: u32,
    /// +0x0c. Observed 0x8df, 0x8e1, 0x65e.
    pub(crate) word_0c: u32,
    /// +0x30. Observed 2, 1, 3.
    pub(crate) word_30: u32,
}

pub(crate) fn encode_g17p_usc_freelist_control(
    firmware_half_va: u64,
    ring_low_va: u64,
    ring_low_end: u64,
    sequence: G17PUscFreelistSequence,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if out.len() != 0x40 {
        return Err(SubmissionError::PartialOpeningGraphObjectSize);
    }
    if ring_low_end <= ring_low_va {
        return Err(SubmissionError::PartialOpeningAddressOverflow);
    }
    out.fill(0);
    put_u32_le(out, 0x00, G17P_DEVCTL_USC_FREELIST);
    put_u32_le(out, 0x04, sequence.word_04);
    put_u32_le(out, 0x08, 0x3f);
    put_u32_le(out, 0x0c, sequence.word_0c);
    put_u32_le(out, 0x10, 0);
    put_u64_le(out, 0x14, firmware_half_va);
    put_u64_le(out, 0x1c, ring_low_va);
    put_u64_le(out, 0x24, ring_low_end);
    put_u32_le(out, 0x2c, G17P_USC_FREELIST_GROW_DESCRIPTOR_BYTES);
    put_u32_le(out, 0x30, sequence.word_30);
    put_u32_le(out, 0x34, 1); // constant
    Ok(())
}

