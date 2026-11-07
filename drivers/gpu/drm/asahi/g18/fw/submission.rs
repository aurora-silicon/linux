// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! Byte-confirmed subset of the G17 GPU submission-transport producer ABI (the
//! "SKSM producer").
//!
//! This module is a pure codec for the driver->firmware submission path — the
//! wire form that hands a compiled command buffer to the firmware for
//! execution. It allocates no memory, starts no ASC, sends no mailbox message,
//! and touches no MMIO. It models the EP 0x21 work-submission doorbell, the
//! channel/work-queue ring producer indices, the SKSM kick word/record, and the
//! host queue-timestamp plus `addKicks` MMIO producer. It also models the exact
//! host-side reset images for `_AGFIChannelState` and
//! `AGXUncachedFWChannelMem`, the A18 Pro SKSM hardware-bridge programming
//! equation, and its four completion-queue descriptors. The provenance ledger
//! explicitly separates byte-grounded G17 fields from inherited producer
//! records whose G17 consumer offsets still need first-hand confirmation. The
//! native G17 channel-command dispatcher is also modeled for the exact A18
//! Pro/G17P identity: it reads a little-endian u32 tag at command offset zero,
//! keeps unresolved and nested SKU meanings out of the top-level enum, and
//! separates the host's `Nop=17` name from the firmware's fatal handling of
//! that value.
//!
//! Grounding is first-hand from the on-disk M5 role macho `GFX.a010.macho`
//! (`g17s a0, SKSM Enabled`), all three pinned A18 Pro `g17p` firmware variants
//! (`a000`, `b000`, and `b100`), and the pinned G17P/G17G/G17X host kexts. The
//! native G17P channel-state ABI is admitted only for the A18 Pro identity;
//! the M5-family identities remain unsupported.
//! Firmware addresses are VAs (0xfffffc0000000000 + fileoff - 0x4000). See
//! the G17 submission notes.
//!
//! Two-signal discipline, mirroring `g17_rtkit`/`g17_boot`:
//! [`GROUNDED_SUBMISSION_PRODUCER_AVAILABLE`] states that the *codec* is real
//! and cited; [`EXECUTABLE_SUBMISSION_TRANSPORT_AVAILABLE`] states whether a
//! submission can be driven to completion offline. It is `false`: the SKSM
//! completion path is a hardware kick FIFO the firmware reads through indexed
//! MMIO scratch registers. The host publication form, runtime scratch geometry,
//! and resource-0 ordered writer are implemented. J700 has completed the
//! channel-doorbell round trip with exact compute output across a retained CL2
//! queue, and the measured first-partial path has executed graphics through
//! the classic channel transport. Linux still needs that contract wired into
//! its runtime and completion IRQ ingress. The native kext `addKicks` path is
//! tracked independently. See [`SUBMISSION_LIVE_GAPS`].

#[cfg(not(test))]
use crate::g18::compat::g17p::G17SksmScratchGeometry;
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
    RenderBarrierFuture,
    PriorBarrierFuture,
    RenderBarrierSequenceOverflow,
    RenderBarrierMissingToken,
    RenderBarrierPredecessorFailed,
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
    /// No SKSM queue geometry is proven for this target/backend pair.
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
    /// A retained queue static object is smaller than its native extent.
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
    /// This native channel layout is not grounded for the selected identity.
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

/// Dedicated FlushID classification is not sufficient for release: an absent
/// feature needs a proven represented fallback, while a present feature needs
/// its numeric allocator, lifetime, and command operands implemented.
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

/// Native target-bound active state required before TA/3D/CL submission.
///
/// D93 role 1 is managed and non-standalone. Linux retains its started CPU,
/// mailbox, and booted RTKit owners for the complete session, which is the
/// native target-1 state. Submit performs no management power request.
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

/// Direct CL `kickFirmware(4, 3, false)`, byte-proven as EP 0x21 `0x...10`.
pub(crate) const fn encode_g17p_direct_cl_kick() -> u64 {
    MSG_TX_DOORBELL | DOORBELL_KICKFW as u64
}

/// Exact J700 native EP21 lifecycle around firmware startup and the first real
/// userspace CL. Startup publishes no work or record-less device-control
/// announcement; the first changed priority-2 CL emits activation then the
/// direct kick.
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

// ---------------------------------------------------------------------------
// 2. Legacy AGX2 channel state. These constants are retained only to make the
//    inherited producer explicit; they are not a native-G17 admission.
// ---------------------------------------------------------------------------

/// AGX2 FW-owned consumer index.
pub(crate) const AGX2_CHANNEL_STATE_READ_PTR_OFFSET: u32 = 0x00;
/// AGX2 driver-owned producer index.
pub(crate) const AGX2_CHANNEL_STATE_WRITE_PTR_OFFSET: u32 = 0x20;
/// Total AGX2 `ChannelState` size.
pub(crate) const AGX2_CHANNEL_STATE_SIZE: u32 = 0x40;

// ---------------------------------------------------------------------------
// 2a. G17 runtime channel descriptors -- native AGX3 layout.
//
//     Grounded first-hand from the G17S firmware recovery scanner at GFX
//     0xfffffc000000ff8c-0xfffffc00000103dc. runtime_pointers+0x20 holds four
//     priority groups of three descriptors (vertex, fragment, compute). Each
//     descriptor is 0x20 bytes: the firmware dereferences cursor pointers at
//     +0x00/+0x08/+0x10 and obtains the command-entry array from +0x18. It
//     advances through that array in 0x18-byte records. The final descriptor
//     ends at runtime_pointers+0x1a0, immediately before the independent cursor
//     pair at +0x1a0/+0x1a8.
// ---------------------------------------------------------------------------

/// First G17 channel descriptor in `runtime_pointers`.
pub(crate) const G17_CHANNEL_DESCRIPTOR_TABLE_OFFSET: u32 = 0x20;
/// Vertex, fragment, and compute descriptors per priority group.
pub(crate) const G17_CHANNEL_DESCRIPTORS_PER_PRIORITY: u32 = 3;
/// Priority groups scanned by the G17 firmware.
pub(crate) const G17_CHANNEL_PRIORITY_COUNT: u8 = 4;
/// Total descriptor count: four priorities times three submission pipes.
pub(crate) const G17_CHANNEL_DESCRIPTOR_COUNT: u32 = 12;
/// Native G17 descriptor stride. AGX2 `ChannelRing` is only 0x10 bytes.
pub(crate) const G17_CHANNEL_DESCRIPTOR_STRIDE: u32 = 0x20;
/// G17 command-entry stride used by all three pipe rings.
pub(crate) const G17_CHANNEL_ENTRY_STRIDE: u32 = 0x18;

/// Pointer fields inside one native G17 channel descriptor.
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

/// Return the runtime-pointer byte offset of a native G17 pipe descriptor.
///
/// The table is priority-major and pipe-minor. Refuse priority values outside
/// the four groups that the firmware scans instead of aliasing the independent
/// cursor pair at `runtime_pointers+0x1a0`.
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

// ---------------------------------------------------------------------------
// 2b. G17 host channel-reset images -- native AGX3 layout.
//
//     Grounded first-hand from byte-identical `AGXChannel::resetChannelState`
//     implementations in the pinned G17P host at 0xfffffe00089f2a60, G17G at
//     0xfffffe0008b225ac, and G17X at 0xfffffe0008ae6a6c. The host clears all
//     0x80 bytes of `_AGFIChannelState`, then writes the fields below. Its
//     separate `AGXUncachedFWChannelMem` reset touches only seven u32 fields
//     through +0x63; bytes between them are deliberately preserved here.
//
//     These are independent host-reset records. They do not prove that the
//     inherited AGX2 ChannelState/RingState names in sections 2/3 describe the
//     G17 firmware consumer, so those ledger entries remain unsupported.
// ---------------------------------------------------------------------------

/// Full byte extent cleared by the G17 host for `_AGFIChannelState`.
pub(crate) const G17_HOST_CHANNEL_STATE_RESET_SIZE: usize = 0x80;
/// Last byte touched by the G17 host's uncached-channel reset stores.
pub(crate) const G17_UNCACHED_CHANNEL_RESET_WRITE_EXTENT: usize = 0x64;
/// The older pinned host builds share this reset layout. The live M5 25G76
/// driver instead resets 0xc0 bytes; see `encode_m5_25g76_channel_state_reset`.
pub(crate) const G17_HOST_CHANNEL_RESET_TARGETS_CONFIRMED: bool = true;

/// Offsets written after the host clears `_AGFIChannelState`.
///
/// Names are intentionally structural until the firmware consumer supplies
/// first-hand semantics for every field.
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

/// Fields touched in `AGXUncachedFWChannelMem` during a G17 channel reset.
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

/// M5 G17G / macOS 25G76 resets six pairs of 128-bit vectors, covering 0xc0
/// bytes. Its firmware consumes through +0xa7. The common 0x80-byte reset
/// would omit the moved fields and the firmware-visible tail.
pub(crate) const M5_25G76_CHANNEL_STATE_RESET_SIZE: usize = 0xc0;
pub(crate) const M5_25G76_CHANNEL_FIRMWARE_READ_EXTENT: usize = 0xa8;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ChannelStateResetValues {
    pub(crate) uncached_state: u64,
    pub(crate) command_pointer_ring: u64,
    pub(crate) gpu_va_10: u64,
    pub(crate) word_48: u32,
    /// Opaque host field +0xf8 copied to the unaligned firmware field +0x9c.
    pub(crate) gpu_va_9c: u64,
}

/// Exact reset stores from M5 AGXChannel::resetChannelState at
/// 0xfffffe000897d36c..0xfffffe000897d410 (AGXG17G 353.14, 25G76).
/// Uncached stores are separately shared with apply_g17_uncached_channel_reset.
/// This codec alone does not admit M5 submission or a Linux GPU runtime.
pub(crate) fn encode_m5_25g76_channel_state_reset(
    values: M5ChannelStateResetValues,
) -> [u8; M5_25G76_CHANNEL_STATE_RESET_SIZE] {
    let mut raw = [0u8; M5_25G76_CHANNEL_STATE_RESET_SIZE];
    put_u64_le(&mut raw, 0x00, values.uncached_state);
    put_u64_le(&mut raw, 0x08, values.command_pointer_ring);
    put_u64_le(&mut raw, 0x10, values.gpu_va_10);
    put_u32_le(&mut raw, 0x24, u32::MAX);
    put_u32_le(&mut raw, 0x28, 4);
    put_u32_le(&mut raw, 0x44, u32::MAX);
    put_u32_le(&mut raw, 0x48, values.word_48);
    put_u64_le(&mut raw, 0x9c, values.gpu_va_9c);
    raw
}

/// Apply only the seven uncached reset stores made by the G17 host.
///
/// The host does not clear the intervening bytes in `resetChannelState`; a
/// caller-provided image therefore stays intact outside the proven stores.
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

// ---------------------------------------------------------------------------
// 2c. A18 Pro / G17P native channel state.
//
//     This is deliberately identity-scoped. The producer is the pinned G17P
//     host `AGXChannel::writeChannelCommandPointer` at
//     0xfffffe00089f5a88-0xfffffe00089f5b58. The matching firmware consumers
//     are `armfw_g17p.a000` 0xfffffc0000012388-0xfffffc00000124f4 and the
//     resume path 0xfffffc0000012514-0xfffffc0000012690. The matching b000
//     and b100 channel blocks are byte-identical to each other and implement
//     the same accesses at 0xfffffc00000122f4-0xfffffc0000012460 and
//     0xfffffc00000124a4-0xfffffc0000012664.
//
//     `_AGFIChannelState` is the cached indirection record: +0x00 points to
//     the uncached state, +0x08 points to the u64 command-pointer ring, and
//     +0x1c holds the cached coherency cursor. `AGXUncachedFWChannelMem` holds
//     u32 cursors at +0x00/+0x30/+0x40/+0x50 and a u32 ring entry count at
//     +0x60. This is not AGX2's read@0/write@0x20 `ChannelState`.
// ---------------------------------------------------------------------------

/// Silicon/host identity used to admit native G17 submission ABIs.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17ChannelAbiTarget {
    /// A18 Pro: G17P host and `g17p` firmware.
    A18ProG17P,
    /// M5: G17G host. No native channel consumer is admitted by this pass.
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
    /// Host producer cursor, published after a `dmb ish`.
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

/// The host executes `dmb ish` after storing the u64 command pointer and
/// before publishing the next u32 producer cursor.
pub(crate) const G17P_CHANNEL_PRODUCER_REQUIRES_DMB_ISH: bool = true;

/// Return the native channel ABI only for the exact identity whose producer
/// and consumer were both inspected. M5-family targets remain unsupported.
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

/// Validate and calculate the write performed by the G17P host producer.
///
/// This pure helper does not execute the required barrier; callers must store
/// the u64 command GPU VA, execute `dmb ish`, then store `next_producer` to
/// uncached offset +0x40.
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

// ---------------------------------------------------------------------------
// 2d. Proven G17P first-partial graphics publication.
//
//     The measured J700 first-partial source contract executes graphics through
//     the native partial graph. It publishes a one-record scheduler page and
//     an eight-group primary index, presents five primary status fields before
//     control 0x84, then exposes 3D before TA and sends one EP 0x21/0x83 kick
//     for transport pair 0 on channel 0. This is the classic channel path and
//     remains separate from the kext's SKSM queue-entry plus `addKicks` path.
// ---------------------------------------------------------------------------

pub(crate) const G17P_PARTIAL_OPENING_PAGE_SIZE: usize = 0x4000;
/// Firmware-high zero page consumed at +0xe00 after the two instances exchange
/// their first peer messages. It is not named by an initdata descriptor, but
/// its fixed mapping is part of the native first-partial topology.
pub(crate) const G17P_PARTIAL_OPENING_COMPUTED_GPU_VA: u64 = 0xffff_fc20_015d_8000;
pub(crate) const G17P_PARTIAL_OPENING_SCHEDULER_GPU_VA: u64 = 0xffff_fc20_015e_0000;
pub(crate) const G17P_PARTIAL_OPENING_SCHEDULER_WORD: u64 = 0x0000_0020_0001_9000;
/// Firmware-high owner of the four-page primary-index run in the clean
/// first-partial capture. The render TTB0 aliases this at
/// [`G17P_PARTIAL_OPENING_PRIMARY_INDEX_GPU_VA`].
pub(crate) const G17P_PARTIAL_OPENING_PRIMARY_INDEX_FIRMWARE_GPU_VA: u64 =
    0xffff_fc20_c084_8000;
pub(crate) const G17P_PARTIAL_OPENING_PRIMARY_INDEX_GPU_VA: u64 = 0x0000_0010_0019_0000;
pub(crate) const G17P_PARTIAL_OPENING_PRIMARY_INDEX_SIZE: usize =
    4 * G17P_PARTIAL_OPENING_PAGE_SIZE;
pub(crate) const G17P_PARTIAL_OPENING_FWCTL_OFFSET: u64 = 0x001c_0000;
pub(crate) const G17P_PARTIAL_OPENING_CONTROL_MESSAGE_SIZE: u64 = 0x40;
/// Legacy Linux render work-submission doorbell channel.
///
/// `encode_tx_doorbell` (this file) gives the layout as
/// `MSG_TX_DOORBELL | pipe_type | (index << 2)`, `index` being the queue
/// priority. The three actual GPU pipes are:
///
///   TA      = Vertex(0)
///   3D      = Fragment(1)
///   compute = Compute(2)
///
/// Linux historically sent only Vertex/priority 0 after publishing both render
/// queues. A gate probe proved that message wakes the cores, not that it
/// dispatches both pipes. Mac17P `AGXArmFirmware::submit3DChannel` at
/// `0x8a43aa0` sends Fragment first (tail `0x8a43c6c`), then
/// `submitTAChannel` at `0x8a43cdc` sends Vertex (tail `0x8a43e10`). The
/// byte-complete boot trace confirms ordinary userspace queues use priority 2;
/// the earlier priority-0 pairs are a distinct system bring-up class.
pub(crate) const G17P_PARTIAL_OPENING_WORK_DOORBELL_CHANNEL: u8 = 0;
pub(crate) const G17P_PARTIAL_OPENING_PAIRED_DOORBELL: u64 =
    MSG_TX_DOORBELL | G17P_PARTIAL_OPENING_WORK_DOORBELL_CHANNEL as u64;
// Default-off build diagnostic: KRUSTFLAGS="--cfg g17p_render_priority_probe".
// Native ordinary user queues captured priority/subpriority 2/2. This does
// not establish a Linux UAPI mapping or make the profile a production fix.
pub(crate) const G17P_NATIVE_RENDER_DOORBELL_PRIORITY: u8 =
    if cfg!(g17p_render_priority_probe) { 2 } else { 0 };
// HardwareBufferID is independently allocated by AGXHardwareBufferIDManager,
// not derived from priority (native captures have priority 2 with IDs 5/6).
// Preserve the existing Linux identity for this diagnostic.
pub(crate) const G17P_NATIVE_RENDER_QOS_HARDWARE_BUFFER_ID: u8 = 0;
pub(crate) const G17P_NATIVE_RENDER_POLICY: u16 =
    if G17P_NATIVE_RENDER_DOORBELL_PRIORITY == 0 { 1 } else { 2 };
/// Mac17P submits 3D first: Fragment(1) | (priority 2 << 2) = 0x09.
pub(crate) const G17P_NATIVE_3D_DOORBELL: u64 = MSG_TX_DOORBELL
    | PipeType::Fragment as u64
    | ((G17P_NATIVE_RENDER_DOORBELL_PRIORITY as u64) << DOORBELL_INDEX_SHIFT);
/// Mac17P then submits TA: Vertex(0) | (priority 2 << 2) = 0x08.
pub(crate) const G17P_NATIVE_TA_DOORBELL: u64 = MSG_TX_DOORBELL
    | PipeType::Vertex as u64
    | ((G17P_NATIVE_RENDER_DOORBELL_PRIORITY as u64) << DOORBELL_INDEX_SHIFT);
pub(crate) const G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE: usize = 0x18;
pub(crate) const G17P_PARTIAL_OPENING_OUTER_SLOT_COUNT: u32 = 0x100;
/// Work-channel table index is `priority * 3 + pipe`, with pipe Vertex(TA)=0,
/// Fragment(3D)=1, Compute(CL)=2. That is exactly the grouping
/// [`g17_initdata::WORK_STATE_GRID_OFFSETS`] documents: indices 0,1,2 are
/// TA_0/3D_0/CL_0 and indices 6,7,8 are TA_2/3D_2/CL_2.
///
/// The render must publish into the rings for the priority it actually
/// configures and announces. This profile follows native's initial priority-0
/// qid-0/1 pair consistently through table selection, queue records, tags,
/// QoS and the paired doorbell. Later priority-2 userspace queues use indices
/// 6/7 and must likewise remain internally coherent.
///
/// Compute is the worked example of the rule: index 8 is CL at priority 2, it
/// agrees with its own priority-2 doorbell 0x0a, and it is validated by 2000
/// consecutive passing computes.
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
const _: () = assert!(G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX ==
    G17P_NATIVE_RENDER_DOORBELL_PRIORITY * 3);
const _: () = assert!(G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX ==
    G17P_NATIVE_RENDER_DOORBELL_PRIORITY * 3 + 1);
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
    /// PID of the process that owns the paired TA/3D channels. Native stores
    /// the same u32 in `_AGFIChannelState +0x44` and ConfigUpdate +0x5a.
    pub(crate) owner_pid: u32,
    /// TA HardwareBufferID owned by the command being published.
    ///
    /// Native copies one allocator result to five firmware-visible sinks: TA
    /// command +0x18, register 0x1c830, ConfigUpdate +0x7e, PBState +0x0c, and
    /// the PBDesc ring index. Keeping the ConfigUpdate copy beside its object
    /// pointer prevents those fields from being staged from different command
    /// lifetimes.
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

/// Root, queue, and scheduler identities at the proven first-work boundary.
/// The successful run used the two-entry macOS context table: slot/tag 0 and
/// slot/tag 1 both retain their low roots while their high roots are replaced
/// with distinct empty pages. Linux owns those pages, so their measured
/// physical addresses are intentionally not part of this contract.
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

/// One proven CL_2 outer-ring publication and its matching RTKit doorbell.
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

/// Build the eight-group primary index used by the proven opening.
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

/// Select the Pool-A record used by one render pass. Native G17P moves the cold
/// first pass from record 0 to the captured record 1. Retained native
/// scenes can reuse an index across multiple sequence values, so the traces do
/// not justify replacing Linux's retained `ordinal * 2` policy globally.
pub(crate) const fn g17p_render_pool_a_record_index(
    submission_ordinal: u64,
    native_first_record: bool,
) -> usize {
    if native_first_record && submission_ordinal == 0 {
        1
    } else {
        ((submission_ordinal % G17P_COLD_OPENING_RECORD_POOL_A_COUNT as u64) as usize * 2)
            % G17P_COLD_OPENING_RECORD_POOL_A_COUNT
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
    // Native numbers the slot words from the start of the page: cold record
    // index 1 points at page +0x04. The old extra +4 coupled index 1 to +0x08
    // and made the host graph one slot ahead of the valid first-render graph.
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

/// Write the legacy packed HWPB-shaped object used by TA and by the
/// established Fragment path. Native Fragment can instead name the dedicated
/// PM HWPB state; TA keeps this distinct object.
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
        // +0x56 is the AGXQOSQueue HardwareBufferID. Native gives each
        // retained TA/3D pair one ID (both stages share it), while different
        // pairs carry different IDs. It must match both the qid*8 QoS record
        // and the KSM dependency-control qword.
        // Byte-diffed against live ConfigUpdate records from two independent
        // Metal render passes on the target (macOS 2026-09-02,
        // `records-gfx.txt` and `everyobj-gfx.txt`):
        //
        //   +0x66 = 1 in both.
        //   +0x46 is written below as a u32 from the allocated FList hardware
        //           buffer id; it reads 0 in one capture and 2 in the other,
        //           so it is dynamic, not a constant.
        //   +0x62 read 1 in one capture and 0 in the other, so it is dynamic,
        //           not a constant; 1 is what this driver always sent and what
        //           the majority of captured records carry. An earlier claim
        //           that native always has 0 came from a single record and is
        //           contradicted by the wider capture.
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
            // +0x82 is the TA's FORWARD REFERENCE TO ITS 3D PARTNER, not a
            // flag. MEASURED ON macOS 2026-09-02 (recs2-run1.txt): across two
            // processes and six passes, a native TA ConfigUpdate carries the
            // partner FRAGMENT queue's id here -- 0x10 while its own +0x18 is
            // 0x0f -- and the matching 3D record fills +0x76..+0x85 with 0xff,
            // the "no forward reference" sentinel this encoder already writes
            // for the Fragment stage. The constant 1 published here before
            // named queue 1, which is neither stage.
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
        G17PColdOpeningStage::Fragment => crate::g18::fw::config::RENDER_3D_DM,
        G17PColdOpeningStage::Tiling => crate::g18::fw::config::RENDER_TA_DM,
    };
    if override_dm != 0xff {
        put_u16_le(out, 0x22, override_dm as u16);
    }
    // AGXChannel::resetChannelState copies channel+0x4c (the owning process
    // PID) to state+0x44. ConfigUpdate carries that same u32 at unaligned
    // +0x5a; the working Metal capture has 0x625 in both locations.
    put_u32_le(out, 0x5a, addresses.owner_pid);
    // Native copies the HardwareBufferID allocated for the FList named at
    // +0x36 into this tag-15 field. Keep the object pointer and its table slot
    // sourced from the same retained owner.
    put_u32_le(out, 0x46, addresses.usc_freelist_hardware_buffer_id);
    // +0x32 is the parameter-buffer binding context when +0x6e is nonzero.
    // It is a distinct kext allocation/source from TA command +0x0c even when
    // both happen to be context 1 in our single-client bring-up path.
    put_u16_le(out, 0x32, context_id);
    match stage {
        G17PColdOpeningStage::Tiling => {
            put_u64_le(out, 0x6e, addresses.tiling_shared_object);
            put_u64_le(out, 0x76, addresses.parameter_buffer_token);
            // AGXTAChannelSKSM::submitBuffer copies descriptor +0x1248 here
            // with a 32-bit store. The same source feeds TA command +0x18,
            // register 0x1c830, PBState +0x0c, and the PBDesc ring; firmware
            // uses it to index the 0x50-byte PB binding table.
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
    // +0x04 is [u16 queue_id][u16 1]: the firmware's own 0x41 tag14-addkicks
    // receipt reads the QID with `ldrh w21, [x19, #4]`, and the native capture
    // carries 0x11/0x12 there for TA/3D.
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
///
/// Publishes the per-slot ordinal as the FList generation at `+0x3e`, which
/// is the same number as the device-global FList sequence for as long as one
/// client renders. `apply_g17p_retained_optional_with_flist_generation` takes
/// the device-global value explicitly.
pub(crate) fn apply_g17p_retained_optional(
    stage: G17PColdOpeningStage,
    addresses: G17PColdOpeningOptionalAddresses,
    submission_ordinal: u64,
    bias_generation: bool,
    queue_id: u16,
    partner_queue_id: u16,
    qos_hardware_buffer_id: u16,
    install: u16,
    context_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    apply_g17p_retained_optional_with_flist_generation(
        stage,
        addresses,
        submission_ordinal,
        g17p_render_flist_generation(submission_ordinal),
        bias_generation,
        queue_id,
        partner_queue_id,
        qos_hardware_buffer_id,
        install,
        context_id,
        out,
    )
}

/// `apply_g17p_retained_optional` with the USC FList generation supplied by
/// the caller.
///
/// The FList is one device-global pool, so its generation advances with every
/// render on the device, while `submission_ordinal` counts only this slot's
/// renders. With one client the two coincide; the moment a second slot
/// publishes, its first tag-15 would otherwise announce generation 0 against a
/// pool the firmware last saw at N.
pub(crate) fn apply_g17p_retained_optional_with_flist_generation(
    stage: G17PColdOpeningStage,
    addresses: G17PColdOpeningOptionalAddresses,
    submission_ordinal: u64,
    flist_generation: u64,
    bias_generation: bool,
    queue_id: u16,
    partner_queue_id: u16,
    qos_hardware_buffer_id: u16,
    install: u16,
    context_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    apply_g17p_retained_optional_with_flist_generation_and_context_update(
        stage,
        addresses,
        submission_ordinal,
        flist_generation,
        bias_generation,
        queue_id,
        partner_queue_id,
        qos_hardware_buffer_id,
        install,
        context_id,
        submission_ordinal == 0,
        out,
    )
}

/// Retained ConfigUpdate with the GART-context branch supplied independently
/// from queue installation and QoS setup.
///
/// Native derives +0x62 from the descriptor's GART allocation generation
/// versus the channel's cached context. A graph rebuilt for another VM must
/// therefore request the context update even when its physical QID is already
/// installed and its global submission ordinal is nonzero. Conversely, a
/// same-context restage must leave it clear.
pub(crate) fn apply_g17p_retained_optional_with_flist_generation_and_context_update(
    stage: G17PColdOpeningStage,
    addresses: G17PColdOpeningOptionalAddresses,
    submission_ordinal: u64,
    flist_generation: u64,
    bias_generation: bool,
    queue_id: u16,
    partner_queue_id: u16,
    qos_hardware_buffer_id: u16,
    install: u16,
    context_id: u16,
    context_update: bool,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    // Payload bytes wrap independently of the full retained owner generation.
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
        for offset in [0x1a, 0x52] {
            put_u16_le(out, offset, 0);
        }
    }
    put_u16_le(out, 0x62, u16::from(context_update));
    // +0x2a is read by the firmware as a whole u64 (`ldur x27, [x19, #42]` in
    // the tag-15 ConfigUpdate handler) and stored into its per-queue table at
    // `0xfffffc0000125df0 + qid * 8`. Every consumer of that table uses
    // `lsr x9, x11, #40` + `uxtb` -- bits 47:40, which is byte 0x2f, the high
    // byte of the u16 written at +0x2e, i.e. `ordinal & 0xff`. And each one
    // compares it against a cached byte and SKIPS when they are equal:
    //
    //     ldr  x11, [table + qid*8]
    //     lsr  x9, x11, #40
    //     ldrb w10, [x19, #40]
    //     cmp  w10, w9, uxtb
    //     b.eq <skip>            ; unchanged -> do nothing
    //     strb w9, [x19, #40]
    //
    // It is a change-detected generation. The raw ordinal is 0 on the first
    // render submit, which matches the firmware's initial cached 0, so the
    // very first submission of a boot reads as "nothing changed" and the
    // firmware declines to act -- with no record emitted either way, which is
    // why no capture ever showed it.
    //
    // This is the same defect already fixed at +0x3e in this function, whose
    // comment records that native never publishes zero there. Apply the same
    // bias so the first submit publishes a generation the firmware sees as new.
    let generation = if bias_generation {
        (submission_ordinal as u16).wrapping_add(1)
    } else {
        submission_ordinal as u16
    };
    put_u16_le(out, 0x2a, generation);
    put_u16_le(out, 0x2e, generation << 8);
    // +0x3e is the AGXUSCPrivMemFList generation. Native copies FList+0xb0
    // here and copies the same value into opcode-0x20 +0x0c during
    // AsyncGrowEngine::updateSubmitInfo. Keep both Linux producers on the
    // shared helper below: changing this generation between registration and
    // the tag-15 kick makes B1 release and re-register the just-published USC
    // allocation before fragment engine service.
    // The kext uses a qword store at 8a98c48; +0x46 begins the independent
    // hardware-buffer ID and must remain untouched by this generation.
    put_u64_le(out, 0x3e, flist_generation);
    // Keep the pair-owned HardwareBufferID identical in ConfigUpdate, the
    // QID-indexed QoS row, and the KSM dependency-control qword.
    put_u16_le(out, 0x56, qos_hardware_buffer_id);
    // TA +0x76 is not the submission ordinal. Native copies the u64 at
    // AGXHardwareBufferBase +0x68 here. The cold encoder above copies the
    // captured PB owner lease token; leave it intact when refreshing queue
    // generations. In particular, do not overwrite +0x76/+0x7e with ordinal.
    Ok(())
}

/// Host-owned equivalent of AGXUSCPrivMemFList +0xb0 for a retained render
/// queue. Valid native cold records start at zero; each later retained restage
/// advances the same generation once.
pub(crate) const fn g17p_render_flist_generation(submission_ordinal: u64) -> u64 {
    submission_ordinal as u64
}

/// Refresh the canonical event record while retaining its physical address.
///
/// Tag-14 +0x09 is an unaligned u64 containing the 40-bit KSM timestamp.
/// 25G83 A000 reads it with `ldur x3, [x19, #9]` at 0x11730. Small cold
/// captures do not establish a 24-bit field. +0x08 remains queue priority;
/// the independently owned data-master field starts at +0x11.
pub(crate) fn apply_g17p_retained_event_with_stamp(
    stage: G17PColdOpeningStage,
    entry_stamp: u64,
    queue_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    if entry_stamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }
    apply_g17p_cold_opening_event(stage, queue_id, out)?;
    put_u64_le(out, 0x09, entry_stamp);
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
    submission_ordinal: u64,
    queue_id: u16,
    out: &mut [u8],
) -> Result<(), SubmissionError> {
    let group_number = submission_ordinal
        .checked_add(1)
        .ok_or(SubmissionError::PartialOpeningQueueHeadOutOfRange)?;
    apply_g17p_retained_event_with_stamp(stage, group_number & KICK_TS_MASK, queue_id, out)
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
    // +0x10. Native's per-queue ring records carry the SAME pointer here for
    // both render stages (ringrec-gfx.txt: 0xfffffc2000000060 in the TA record
    // at fwva 0xfffffc20c0000b40 and in the 3D record at 0xfffffc20c0000c00),
    // and it is the a5 argument of every AGXChannel::init. That looked like a
    // shared firmware constant we were failing to write. It is NOT.
    //
    // KEXT_PROVEN (25G83): cmdq+0x680 is an AGXTimeStampQueue whose +0x28 is
    // its firmware VA; AGXCommandQueue::allocate3DWorkQueueInner passes that
    // as a5 to the channel init, AGXChannel::init stores it at chan+0x98 and
    // resetChannelState copies it to state+0x10. The 0x18-byte
    // _AGFITimeStampQueue slots are host-allocated out of a firmware-shared
    // stack, and 0x60 is simply slot 4. The value repeats across boots only
    // because the Metal test app is always the fifth client. Writing that
    // literal from Linux would have published a pointer into someone else's
    // allocation.
    //
    // FW_PROVEN: nothing in the DM-ring consumption path (FUN_261a4 /
    // FUN_12638) nor in the job builder FUN_23004 reads state+0x10 at all. It
    // is a per-command-queue timestamp list head, used only by
    // AGFAcceleratorProcessTimeStampQueue on the completion side. It cannot
    // stop consumption or launch, so it is not our bug. Our per-graph value
    // stays.
    put_u64_le(out, 0x10, addresses.job_list);
    put_u32_le(out, 0x24, u32::MAX);
    // The live native render path reads state +0x28 through getPriority(),
    // copies it to 3D setup +0xe5c, and validates it against KSM queue +0x1c.
    // The captured ordinary Metal command carries 2. Channel +0x1c0 is only
    // context-priority bookkeeping and is not the live-state value.
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
    // MEASURED ON macOS 2026-09-02 (`ringrec-gfx.txt`): the live per-queue
    // record of a real Metal render pass, read through the kernel alias the
    // AGXChannel keeps at +0x58 for the firmware VA at +0x80, is
    //
    //   +0x24 0xffffffff   +0x28 2   +0x2c 2   +0x36 0xffff
    //   +0x40 2   +0x44 0xffffffff   +0x48 <pid>
    //   +0x9c <scheduler/channel-control pointer>
    //
    // This driver had that {2, 0xffffffff, pid} block four bytes early, at
    // +0x3c/+0x40/+0x44, and the pointer at +0x70. The pointer had once been
    // at +0x9c and was moved on the theory that the object is 0x80 bytes; the
    // ring slots name records on an exact 0xc0 stride (base + qid * 0xc0), so
    // 0x9c is inside the object and was right all along.
    put_u32_le(out, 0x40, u32::from(G17P_NATIVE_RENDER_POLICY));
    put_u32_le(out, 0x44, u32::MAX);
    // Native reset copies the channel owner's process PID here. The same
    // value is published in ConfigUpdate +0x5a.
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

/// Keep the fixed operand table zero at the proven cold first-work boundary.
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
    // +0x14 is [u16 write_index][u8 queue_id][u8 1]. The middle byte is the
    // QUEUE ID, not a separate "grid index" -- macOS's own
    // AGFIAcceleratorDataMasterEntry slot carries 0x11 (TA) and 0x12 (3D)
    // there, matching the QIDs its tag-15 publishes, and the rest of the
    // 0x18-byte entry matches this encoder field for field:
    //
    //   +0x00 zero                       +0x00 zero
    //   +0x08 queue record VA            +0x08 state.queue_gpu_va
    //   +0x10 data master (TA 0 / 3D 1)  +0x10 kind (PipeType Vertex/Fragment)
    //   +0x14 0x01_11_0003 / 0x01_12_0003  write_index 3, qid, bit24 set
    //
    // This byte must agree with the QID published in tag-15, tag-14 and the
    // SKSM registration. It follows the retained queue object even when a
    // recovered compute session advances past its first QID.
    //
    // +0x17 is NOT a constant. `AGXArmFirmware::encodeAcceleratorRingCommand`
    // fills this entry as `+0x08 chanstate fwva, +0x10 type, +0x14 info[0],
    // +0x16 qid, +0x17 !chan[0x3c]` -- the last byte is the logical NOT of a
    // channel state word, so it is 0 once that word is set. MEASURED on macOS
    // 2026-09-02: a live Metal render's data-master ring entries carry 0x00
    // there for BOTH stages (`00 3c 00 10 .. 39 01 08 00` for 3D and
    // `.. 43 01 07 00` for TA), across two independent captures with different
    // queue ids. This driver hardcoded bit 24, i.e. it told firmware the
    // channel state word was clear on every single submission.
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
    channel_started: bool,
) -> Result<PreparedG17PComputeChannelWork, SubmissionError> {
    // Match AGXArmFirmware::encodeAcceleratorRingCommand for every pipe:
    // +0x17 is !AGXChannel[0x3c], so it is one only for the first publication
    // of a physical channel and zero after that channel has started.
    let outer = prepare_g17p_partial_opening_outer_slot(
        state,
        G17P_COMPUTE_CHANNEL_TABLE_INDEX,
        queue_id,
        PipeType::Compute as u32,
        channel_started,
    )?;
    Ok(PreparedG17PComputeChannelWork {
        outer,
        doorbell: G17P_COMPUTE_WORK_DOORBELL,
    })
}

/// Couple the fragment-first/tiling-second outer slots to their one channel-0
/// 0x83 notification. These are the initdata work rings at table indices 1 and
/// 0, not the kext's separate 8-byte command-pointer rings. The caller performs
/// the barriers in [`G17P_PARTIAL_OPENING_PUBLICATION_ORDER`].
pub(crate) fn prepare_g17p_partial_opening_work_pair(
    fragment: G17PPartialOpeningOuterChannelState,
    tiling: G17PPartialOpeningOuterChannelState,
    fragment_queue_id: u8,
    tiling_queue_id: u8,
    first_submit: bool,
) -> Result<PreparedG17PPartialOpeningWorkPair, SubmissionError> {
    // `+0x17` is `!AGXChannel[0x3c]`: one on the first publication of a
    // physical queue and zero after it has started. Every valid native queue
    // follows 1 -> 0; a module-wide constant encoded one side incorrectly.
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

// ---------------------------------------------------------------------------
// 3. Legacy AGX2 work-queue RingState. The A18 Pro ARM firmware directly
//    confirms only done@0; the SKSM hardware consumer keeps the remaining
//    rptr/wptr/size offsets unconfirmed, so this record is not admitted as a
//    native G17P layout.
// ---------------------------------------------------------------------------

pub(crate) const AGX2_RING_STATE_GPU_DONEPTR_OFFSET: u32 = 0x00;
pub(crate) const AGX2_RING_STATE_GPU_RPTR_OFFSET: u32 = 0x30;
pub(crate) const AGX2_RING_STATE_CPU_WPTR_OFFSET: u32 = 0x40;
pub(crate) const AGX2_RING_STATE_RB_SIZE_OFFSET: u32 = 0x50;

/// AGX2 work-queue entries are u64 command GPU virtual addresses.
pub(crate) const AGX2_RING_ENTRY_BYTES: u32 = 8;

/// The shipping producer's maximum outstanding event slots
/// (`workqueue.rs::MAX_JOB_SLOTS`). This is not the command-ring depth: the
/// current Linux ring has `WQ_SIZE == 0x500`. Its numerical equality with the
/// 7-bit QID maximum is only a coincidence and carries no ABI meaning.
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

/// TS occupies bits [39:0] (`and x0,x5,#0xffffffffff` at 25G83 A000 0xfd18). 40-bit, per
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

// ---------------------------------------------------------------------------
// 5. Host SKSM kick-queue producer transaction (AGX3-specific).
//
//    Grounded from G17S KSMKickQueue::encodeKickEntry at host text
//    0xfffffe0008b51f74-0xfffffe0008b521c4. The current timestamp at queue
//    +0x48 selects `entry_base + low8(timestamp) * entry_stride`; the current
//    40-bit timestamp is returned for the doorbell, then the low-byte slot is
//    advanced modulo runtime `num_stamps`. Wrapping the slot to zero increments
//    the 32-bit stamp in bits [39:8]. This coupling is load-bearing: choosing a
//    different entry slot from the one encoded in the doorbell desynchronizes
//    the queue and can reach the firmware's out-of-order fatal path.
// ---------------------------------------------------------------------------

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

/// Pure result of preparing one host SKSM kick-queue producer transaction.
///
/// The caller must still encode the complete target-specific entry, execute
/// the recovered `dmb ish`, update the submitted counter, and perform the live
/// target-specific `addKicks` MMIO publication. This structure only makes the
/// entry selection, queue identity, and next producer timestamp indivisible.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedKickSubmission {
    /// Slot selected from the current timestamp's low byte.
    pub(crate) entry_index: u8,
    /// Byte offset from the runtime queue entry base.
    pub(crate) entry_offset: u32,
    /// Current 40-bit timestamp consumed by this entry.
    pub(crate) timestamp: u64,
    /// Queue whose target-specific `addKicks` tag must be published later.
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

/// Low-byte payload of the retained render channel's next submission.
///
/// In the inspected 25G83 kext, FRG submit increments channel +0x204
/// (8837ae0..e8); getter 88b4ee8 returns its low byte. The counter follows
/// channel-object lifetime, independently of its physical timestamp base.
/// Linux retains one ordinal per physical pair across its storage handoffs,
/// advancing once per publication; outstanding submissions need not retire
/// before this counter advances. The byte wraps independently of the 40-bit
/// SKSM timestamp and is neither a QID nor an address-space tag.
pub(crate) const fn g17p_serialized_render_scratch_payload(submission_ordinal: u64) -> u8 {
    submission_ordinal.wrapping_add(1) as u8
}

/// Pack `AGX3DCommandDescriptor::getSKSMCompletionScratchRegValue()`.
///
/// The kext stores this value in both its host descriptor and the emitted GPU
/// command at +0x2160. Bits [39:0] are the command's SKSM queue timestamp and
/// bits [47:40] are the 3D-channel scratch payload. The native full-command
/// witness `0x0000010000000f76` therefore decodes as payload one and timestamp
/// `0xf76`; it is categorically not a USC FList pointer.
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

/// ConfigUpdate +0x26 gates the u64 completion seed at +0x2a (B1
/// 11434..11470). Busy channels preserve firmware's evolving scratch value.
/// Native emits both fields with u32/u64 stores; validate before mutation.
pub(crate) fn apply_g17p_idle_scratch_seed(
    out: &mut [u8], seed: Option<u64>,
) -> Result<(), SubmissionError> {
    if out.len() < 0x32 {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }
    put_u32_le(out, 0x26, u32::from(seed.is_some()));
    put_u64_le(out, 0x2a, seed.unwrap_or(0));
    Ok(())
}

/// Prepare the exact entry-slot/doorbell/next-timestamp tuple for one kick.
///
/// `num_stamps` and `entry_stride` are intentionally runtime inputs: Apple's
/// host loads both from the per-queue geometry rather than hard-coding them.
/// Invalid geometry returns an error before an entry address or doorbell is
/// produced. No shared memory, counter, barrier, MMIO, or endpoint state is
/// mutated here.
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

// ---------------------------------------------------------------------------
// 5a. A18 Pro / G17P CL kick-entry body and host queue-state transition.
//
//     `AGXKSMKickQueueG16::encodeCLKickEntry` at
//     0xfffffe0008a56848-0xfffffe0008a56a64 gathers the CL descriptor inputs.
//     Its shared encoder at 0xfffffe0008a56a68-0xfffffe0008a56eb4 writes the
//     complete 0x180-byte body described below. The recovered G17S encoder at
//     0xfffffe0008b51f74-0xfffffe0008b521c4 independently confirms the slot,
//     timestamp, barrier, and counter tail; scratch tags remain target-specific.
//
//     The body is the first 0x180 bytes of each 0x200-spaced queue-context
//     record captured by m1n1. The host zeroes the runtime stride before these
//     stores, so every byte not named here stays zero.
// ---------------------------------------------------------------------------

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
    /// Already converted native predecessor stamp. Never reinterpret it using
    /// the consumer queue's base timestamp or ring geometry.
    pub(crate) native_timestamp: Option<u64>,
    pub(crate) event_word: u32,
    pub(crate) shared_stamp: u64,
    pub(crate) queue_id: u8,
}

/// Represent a render dependency using the native event word plus epoch.
/// The event value advances by 0x100 and wraps as on M1/M2. Its high timestamp
/// bits are carried separately; dropping them makes a dependency at 2^24
/// name the wrong kick even if AddKicks itself preserves all forty bits.
pub(crate) fn g17p_render_barrier_from_timestamp(
    timestamp: u64,
    queue_id: u8,
) -> Result<G17PClBarrierDependency, SubmissionError> {
    if timestamp & !KICK_TS_MASK != 0 {
        return Err(SubmissionError::KickTimestampOutOfRange);
    }
    if queue_id > KICK_QID_MASK {
        return Err(SubmissionError::KickDependencyQueueIdOutOfRange);
    }
    Ok(G17PClBarrierDependency {
        native_timestamp: None,
        event_word: (timestamp as u32).wrapping_shl(8),
        shared_stamp: timestamp >> 24,
        queue_id,
    })
}

/// Logical render prefix, resolved at ioctl admission before asynchronous work.
#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub(crate) struct G17PRenderOrder {
    pub(crate) sequence: u64,
    pub(crate) wait_through: Option<u64>,
}

/// Zero snapshots all preceding ioctls; N adds the first N renders of this
/// ioctl. NONE remains independent. Sequence zero is reserved for internal work.
pub(crate) fn g17p_render_order(
    ioctl_base: u64, preceding: u64, barrier: u16,
) -> Result<G17PRenderOrder, SubmissionError> {
    let sequence = ioctl_base.checked_add(preceding)
        .and_then(|n| n.checked_add(1))
        .ok_or(SubmissionError::RenderBarrierSequenceOverflow)?;
    let wait_through = if barrier == u16::MAX {
        None
    } else {
        if u64::from(barrier) > preceding {
            return Err(SubmissionError::RenderBarrierFuture);
        }
        Some(ioctl_base.checked_add(u64::from(barrier))
            .ok_or(SubmissionError::RenderBarrierSequenceOverflow)?)
    };
    Ok(G17PRenderOrder { sequence, wait_through })
}

/// Add an exact historical prefix without broadening it to the ioctl base.
/// The base is frozen for the whole ioctl, before any of its jobs publish;
/// even an earlier command in this ioctl cannot satisfy a historical ordinal.
/// Zero is absent, including when the legacy header is NONE.
pub(crate) fn g17p_render_order_with_prior(
    ioctl_base: u64, preceding: u64, barrier: u16, prior: u64,
) -> Result<G17PRenderOrder, SubmissionError> {
    if prior > ioctl_base {
        return Err(SubmissionError::PriorBarrierFuture);
    }
    let mut order = g17p_render_order(ioctl_base, preceding, barrier)?;
    if prior != 0 {
        order.wait_through = Some(order.wait_through.map_or(prior, |p| p.max(prior)));
    }
    Ok(order)
}

/// Resolve a still-owned predecessor. A successful retirement needs no GPU
/// dependency; errors and an unassigned token must never satisfy the prefix.
/// The caller filters owner identity and supplies every live predecessor in
/// the prefix under the same runtime lock as publication and retirement.
pub(crate) fn g17p_live_render_dependency(
    order: G17PRenderOrder, predecessor_sequence: u64,
    predecessor_status: i32, native_word: u64,
) -> Result<Option<G17PClBarrierDependency>, SubmissionError> {
    if !g17p_render_prefix_contains(order.wait_through, predecessor_sequence) {
        return Ok(None);
    }
    if predecessor_status < 0 {
        return Err(SubmissionError::RenderBarrierPredecessorFailed);
    }
    if predecessor_status > 0 { return Ok(None); }
    if native_word == u64::MAX {
        return Err(SubmissionError::RenderBarrierMissingToken);
    }
    // Bit 46 is QID bit 6: the firmware takes the QID as bits [46:40] (ubfx
    // x1,x5,#40,#7) and the same bit as its valid-queue mask-word selector
    // (ubfx x6,x5,#46,#1). A predecessor on QID 64..127 sets it; its token is
    // fully modeled and the dependency encoder takes all seven QID bits.
    // decode_kick_word still refuses bits [63:47]. Refusing the selector made
    // every barrier on a 64..127 fragment QID fail the dependent render with
    // EIO (the dead-pair DeviceLost).
    let token = decode_kick_word(native_word)?;
    let mut dependency = g17p_render_barrier_from_timestamp(token.timestamp, token.queue_id)?;
    dependency.native_timestamp = Some(token.timestamp);
    Ok(Some(dependency))
}

pub(crate) fn g17p_render_prefix_contains(prefix: Option<u64>, sequence: u64) -> bool {
    sequence != 0 && prefix.is_some_and(|limit| sequence <= limit)
}

/// Collapse a validated logical prefix to its newest kick per hardware QID.
/// Each QID's implicit previous-kick parent supplies the transitive order.
/// Use logical sequence, not wrapped firmware timestamps or scan order, to
/// choose the newest. Callers must check every covered predecessor for errors
/// before inserting it, including predecessors this frontier will subsume.
pub(crate) fn g17p_insert_prefix_dependency(
    dependencies: &mut [G17PClBarrierDependency],
    sequences: &mut [u64], count: &mut usize,
    sequence: u64, dependency: G17PClBarrierDependency,
) -> Result<(), SubmissionError> {
    if *count > dependencies.len() || *count > sequences.len() {
        return Err(SubmissionError::KickDependencyCountOutOfRange);
    }
    if let Some(index) = dependencies[..*count].iter()
        .position(|prior| prior.queue_id == dependency.queue_id)
    {
        if sequence > sequences[index] {
            dependencies[index] = dependency;
            sequences[index] = sequence;
        }
        return Ok(());
    }
    if *count == dependencies.len() || *count == sequences.len() {
        return Err(SubmissionError::KickDependencyCountOutOfRange);
    }
    dependencies[*count] = dependency;
    sequences[*count] = sequence;
    *count += 1;
    Ok(())
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

/// Command-relative CL fields witnessed on consecutive output-positive
/// compute entries. The descriptor slot stride remains a storage decision;
/// both RCE addresses are derived from the current slot's low alias.
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

/// Host-written extent of the tag-14 `KsmKickQueueAddKicks` channel command.
///
/// `AGXCLChannelSKSM::submitBuffer` writes exactly these 0x15 bytes inline at
/// mac17p `0xfffffe00089fa39c-0xfffffe00089fa3cc`, then publishes the record's
/// GPU address through `AGXChannel::writeChannelCommandPointer`.
pub(crate) const G17P_KSM_ADD_KICKS_COMMAND_SIZE: usize = 0x15;
/// Bytes the matched J700 A000 handler cache-invalidates before reading the
/// record (`mov w1, #0x18` at `0xfffffc0000011714`). The three bytes past the
/// host-written extent are never loaded, but must not be stale.
pub(crate) const G17P_KSM_ADD_KICKS_COMMAND_FETCH_SIZE: usize = 0x18;

/// Fields the native host copies into one tag-14 AddKicks record.
///
/// Native sources, all from `submitBuffer`:
/// * `queue_id`      = `AGXKSMKickQueue[0x0c]`, the QID.
/// * `add_count`     = `AGXCLCommandDescriptor[0x2e4]`, the number of kick
///   entries this submission published (NOPs plus the one CL entry). It is the
///   same value the suppressed MMIO variant passes to
///   `AGXKSMKickQueueG16::addKicks`.
/// * `queue_priority`= `AGXKSMKickQueue[0x1c]`, i.e. `AGXChannel::getPriority()`.
///   This queue's value is the retained completion selector.
/// * `entry_stamp`   = the queue's producer stamp **before** the CL entry
///   advanced it, returned by `AGXKSMKickQueueG16::encodeKickEntry`
///   (`ldr x0, [x19, #0x48]` at `0xfffffe0008a56e00`). Its low byte is the ring
///   entry index; A000 reads it only for its trace event.
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

/// Build the tag-14 `KsmKickQueueAddKicks` channel command.
///
/// The record replaces the host SKSM-port `addKicks` pair on J700, where every
/// host access to `sgx 0x0..0x2ffff` takes an asynchronous SError. The matched
/// A000 firmware dispatches type 14 to `0xfffffc0000011710`, which writes the
/// QID to `sgx+0x103a8` and this record's `add_count` to `sgx+0x21018` through
/// the firmware's own mapping of the block.
///
/// The A000 handler loads only `+0x04`, `+0x06` and `+0x09`; `+0x08` and `+0x11`
/// are written for host parity.
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
    submission_ordinal: u64,
    primary_checkpoint: u8,
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
                tag: primary_checkpoint,
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

/// Select the descriptor-embedded RegisterArrays consumed by a native render
/// kick. 3D's four complete arrays start at command +0xa0 and follow at the
/// 0x720-byte stride present in every working capture.
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
        match crate::g18::fw::config::RENDER_3D_RCE {
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
    /// Last timestamp reconciled through `submitCLChannel` addKicks.
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
    /// Result of `convertEventMachineTSToHWTS` for a queue with no predecessor.
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

/// G17P direct first-submit side-effect ledger. The tag/QID/SKSM/channel and
/// mailbox ordering is binary-verified against the native submit path.
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

    if let Some(timestamp) = barrier.native_timestamp {
        return encode_g17p_cl_dependency(timestamp, barrier.queue_id);
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

/// Encode the complete G17P CL entry body and calculate the exact host state
/// transition that follows the recovered `dmb ish`.
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
    // Native writes `(RCEInfo + 0x00) & 3` here. RCEInfo +0x04 is the active
    // RegisterArray count (1 for TA, 4 for 3D), not this field.
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
        && crate::g18::fw::config::NO_IMPLICIT_DEP != 0;
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

/// Apply the completed-kick count used by `AGXCLCommandDescriptor::complete`.
/// The live callback performs an acquire-release atomic subtraction at queue
/// +0x44. This pure form keeps every other producer field unchanged.
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

/// The one compute private-memory pool, distinct from render's pool ID zero.
/// B1 indexes the same registration table with tag15 +0x46 and FList +0x08.
pub(crate) const G17P_COMPUTE_FLIST_ID: u16 = crate::g18::fw::config::COMPUTE_FLIST_ID;

/// Initialize pool-owned state before its first firmware registration.
pub(crate) fn apply_g17p_cl_flist_static_state(
    shared_support: &mut [u8],
    support_state: &mut [u8],
    page_list_gpu_va: u64,
    run_table_gpu_va: u64,
    support_state_gpu_va: u64,
    pool_id: u16,
) -> Result<(), SubmissionError> {
    if shared_support.len() < 0x4000 || support_state.len() < 0x4000 {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }
    shared_support.fill(0);
    support_state.fill(0);
    put_u64_le(shared_support, 0x00, 2);
    put_u64_le(shared_support, 0x08, u64::from(pool_id));
    put_u64_le(shared_support, 0x10, 2);
    put_u64_le(shared_support, 0x14, page_list_gpu_va);
    put_u32_le(shared_support, 0x1c, 0x4_0000);
    put_u64_le(shared_support, 0x20, 0x0000_1500_0000_0000);
    put_u64_le(shared_support, 0x28, 0x0000_1500_0000_0000);
    put_u64_le(shared_support, 0x30, run_table_gpu_va);
    put_u64_le(shared_support, 0x40, 4);
    put_u32_le(shared_support, 0x48, 0xa8);
    put_u64_le(shared_support, 0x4c, support_state_gpu_va);
    // B1 compares *FList[+0x4c] (submitted) with FList[+0x54]
    // (completed). Both are pool-wide u64 counters, initially zero.
    put_u64_le(shared_support, 0x54, 0);
    put_u32_le(shared_support, 0x5c, 1);
    put_u32_le(shared_support, 0x60, 2);
    Ok(())
}

/// Historical captured per-queue fixture initializer. Production pool creation
/// uses `apply_g17p_cl_flist_static_state`; never call this on its shared pages.
pub(crate) fn apply_g17p_cl_queue_static_state(
    shared_support: &mut [u8],
    support_state: &mut [u8],
    scheduler_state: &mut [u8],
    page_list_gpu_va: u64,
    run_table_gpu_va: u64,
    support_state_gpu_va: u64,
    scheduler_state_gpu_va: u64,
) -> Result<(), SubmissionError> {
    if shared_support.len() < 0x4000
        || support_state.len() < 0x4000
        || scheduler_state.len() < 0x4000
    {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }

    shared_support.fill(0);
    support_state.fill(0);
    scheduler_state.fill(0);

    put_u64_le(shared_support, 0x00, 2);
    // AGXUSCPrivMemFList+0x08 is the pool ID copied into the firmware's
    // internal allocator entry and ultimately written to gUPM's pool-select
    // operation. All compute queues share this pool and UMA slot; QoS
    // HardwareBufferIDs remain per queue.
    put_u64_le(shared_support, 0x08, 1);
    put_u64_le(shared_support, 0x10, 2);
    // _AGFIUMAPagePoolState stores the full VA at unaligned +0x14.
    // Copying the native +0x18 qword leaves compute naming render's pool.
    put_u64_le(shared_support, 0x14, page_list_gpu_va);
    put_u32_le(shared_support, 0x1c, 0x4_0000);
    put_u64_le(shared_support, 0x20, 0x0000_1500_0000_0000);
    put_u64_le(shared_support, 0x28, 0x0000_1500_0000_0000);
    // Every queue names the run table that describes the shared backing.
    // A private empty table here is incoherent with the shared pool and leaves
    // overlapping spill allocation without the pool's initialized run map.
    put_u64_le(shared_support, 0x30, run_table_gpu_va);
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

    Ok(())
}

/// Initialize the one channel-control page retained by physical CL_2.
pub(crate) fn apply_g17p_cl_channel_control_static_state(
    channel_control: &mut [u8],
) -> Result<(), SubmissionError> {
    if channel_control.len() < 0x40 {
        return Err(SubmissionError::KickRetainedStorageBufferTooSmall);
    }
    channel_control.fill(0);

    put_u64_le(channel_control, 0x00, 0x0000_0100_0000_ffff);
    put_u64_le(channel_control, 0x20, 0x0002_0000_0000_0000);
    put_u64_le(channel_control, 0x30, 0x0000_0000_ff00_0000);
    Ok(())
}

/// Compatibility helper for byte-layout tests covering both ownership
/// domains. Production initializes the queue pages and CL_2 page separately.
pub(crate) fn apply_g17p_cl_retained_static_state(
    shared_support: &mut [u8],
    support_state: &mut [u8],
    scheduler_state: &mut [u8],
    channel_control: &mut [u8],
    page_list_gpu_va: u64,
    run_table_gpu_va: u64,
    support_state_gpu_va: u64,
    scheduler_state_gpu_va: u64,
) -> Result<(), SubmissionError> {
    apply_g17p_cl_queue_static_state(
        shared_support,
        support_state,
        scheduler_state,
        page_list_gpu_va,
        run_table_gpu_va,
        support_state_gpu_va,
        scheduler_state_gpu_va,
    )?;
    apply_g17p_cl_channel_control_static_state(channel_control)
}

/// Apply the exact host-owned pre-B2 values to three provider-owned pages.
/// Bytes outside the listed fields remain unchanged. `channel_control_gpu_va`
/// comes from the retained Linux mapping, not the captured macOS address.
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

    let activation = apply_g17p_cl_b2_computed_state(
        activation,
        computed,
        channel_control_gpu_va,
    )?;
    apply_g17p_cl_b2_descriptor_tables(region_1, region_2)?;
    Ok(activation)
}

/// Apply the session-wide CL_2 half of B2 activation. The pointer is pinned to
/// a dedicated bootstrap owner; logical queues have separate scheduler pages.
/// The exact consumer of this firmware-private slot remains unresolved. Never
/// retarget it during ordinary queue switching. PBDesc/UMA are separate state.
pub(crate) fn apply_g17p_cl_b2_computed_state(
    activation: G17PClB2ActivationState,
    computed: &mut [u8],
    channel_control_gpu_va: u64,
) -> Result<G17PClB2ActivationState, SubmissionError> {
    if activation == G17PClB2ActivationState::Published {
        return Err(SubmissionError::KickB2PrimaryStateAlreadyPublished);
    }
    let layout = G17P_CL_B2_PRIMARY_STATE_LAYOUT;
    if computed.len() < layout.object_size {
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

    Ok(G17PClB2ActivationState::Published)
}

/// Apply the device-global descriptor-table half of B2 initialization.
///
/// Native performs this before client halLoadPBDesc/halLoadUMADesc updates.
/// Reapplying these captured cold entries for every later compute queue would
/// overwrite whichever live render owner currently holds PBDesc index 1 and
/// UMA index 0.
pub(crate) fn apply_g17p_cl_b2_descriptor_tables(
    region_1: &mut [u8],
    region_2: &mut [u8],
) -> Result<(), SubmissionError> {
    let layout = G17P_CL_B2_PRIMARY_STATE_LAYOUT;
    if region_1.len() < layout.object_size || region_2.len() < layout.object_size {
        return Err(SubmissionError::KickB2PrimaryStateBufferTooSmall);
    }

    put_u64_le(region_1, 0x10, 0x0000_0080_0007_7000);
    put_u32_le(region_1, 0x18, 0x17);
    put_u32_le(region_1, 0x1c, 0x17);

    put_u64_le(region_2, 0x00, 0x0800_0000_e000_0000);
    put_u64_le(region_2, 0x08, 0x0000_3d40_0000_3400);
    put_u64_le(region_2, 0x10, 0x0000_0000_0000_1d00);
    Ok(())
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

/// One-time activation order proven by retained command B2.
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

/// Steady-state order proven by command B3 on the same physical queue.
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

// ---------------------------------------------------------------------------
// 6. Host addKicks MMIO publication (AGX3-specific).
//
//    The host does not write the firmware-side `timestamp | (qid << 40)` word
//    directly.  The pinned G17P/G17G/G17X implementations publish a
//    target-specific 64-bit queue tag followed by the zero-extended add count
//    as a second 64-bit store to two
//    runtime-selected scratch offsets. The host adds nine times the runtime
//    stride to both bases for every target. Publication never reads either
//    location and preserves the exact word-0 then word-1 order. The separate
//    host diagnostic path may read word 1; word 0 must never be read.
//    G17P `addKicks` performs `str x20` then `str x19` at
//    0xfffffe0008a57b5c/0xfffffe0008a57b60; the earlier `mov w19,w1`
//    zero-extends the add count.
// ---------------------------------------------------------------------------

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

/// Host producer family with an independently recovered `addKicks` tag.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum SksmAddKicksTarget {
    /// A18 Pro / HAL200 (`AGXKSMKickQueueG16`).
    G17P,
    /// M5 / HAL300 G target.
    G17G,
    /// M5 Pro/Max HAL300 X-family target (G17S/G17C host implementation).
    G17X,
}

/// Exact ordered values and offsets for one host `addKicks` publication.
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
        // mac17p 0x8a57b28-0x8a57b34: base bit 42, qid at bit 43.
        SksmAddKicksTarget::G17P => G17P_ADD_KICKS_TAG | ((queue_id as u64) << 43),
        // mac17g 0x8b91978-0x8b91984: base bit 43, qid at bit 44.
        SksmAddKicksTarget::G17G => G17G_ADD_KICKS_TAG | ((queue_id as u64) << 44),
        // mac17j 0x8b53144-0x8b53154: G17G form plus host-family bit 53.
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

// ---------------------------------------------------------------------------
// 7. A18 Pro / G17P SKSM hardware bridge and completion descriptors.
//
//    Grounded from the pinned G17P host. `AGXKSMKickQueueG16::configureHardware`
//    at 0xfffffe0008a56580-0xfffffe0008a5682c reads the runtime MMIO geometry
//    from accelerator-object fields +0x10730/+0x10734/+0x10738, writes a
//    64-bit queue tag and 64-bit queue payload, then publishes the QID bit at
//    MMIO +0x21068. The object fields contain base0=0x4000, base1=0x4008 and
//    stride=0x10; they are not MMIO locations. The same geometry is used by
//    `addKicks` (§6). Linux resource 0 (`sgx`) owns this window. Its two
//    scratch locations receive ordered stores; queue enable is a separate
//    read/modify/write register after the pair. The production publication
//    surface intentionally exposes no scratch read operation.
//
//    `AGXAcceleratorG16::populateKSMCompletionQueue` at
//    0xfffffe0008a53034-0xfffffe0008a532b8 emits four 0x20-byte descriptors.
//    Each present queue has 0x40-byte entries and a fixed ordinal 0..3. The
//    matched firmware consumes those queues. D93AP IRQ 1001 and RTBuddy AKF
//    message type 2 converge on source 4 and drain the event rings that carry
//    completion stamps. The completion path remains pending until an actual
//    SKSM kick proves that complete round trip on D93AP hardware.
// ---------------------------------------------------------------------------

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
/// `_AGFIDataMasterType` selected by `AGXCommandQueue::processComputeSetup`.
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

/// Prepare the queue-configuration transaction produced by the G17P compute
/// setup path.
///
/// `AGXCommandQueue::processComputeSetup` at
/// `0xfffffe0008a077a4..0xfffffe0008a078d4` selects the queue from the CL
/// channel QID, prepares its mapping resource, obtains the channel completion
/// selector, and writes the packed selector/data-master pair. The captured
/// native CL2 queue uses QID 4, selector 2, and entry VA `0x70004d8000`.
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

/// Prepare the queue-configuration transaction for one G17P *render* queue.
///
/// Identical to the compute helper except for the data master, which the
/// twelve captured macOS render passes pin to TA = 0 and 3D = 1 in three
/// independent places (outer ring slot `+0x10`, tag-14 `+0x11`, tag-15
/// `+0x22`). The compute data master is rejected here so a render caller
/// cannot silently configure a CL queue.
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

// ---------------------------------------------------------------------------
// 8. A18 Pro / G17P top-level channel-command dispatch.
//
//    This ABI is deliberately identity-scoped, just like §2c. The pinned G17P
//    host grounds the top-level names below through its command-request and
//    submit paths. The matching a000 firmware dispatcher is
//    `agfaProcessChannelCommand` at 0xfffffc0000010fa8 with the 19-entry table
//    at 0xfffffc000001233c. The b000/b100 dispatcher starts at
//    0xfffffc0000010f14 and uses the byte-identical table at
//    0xfffffc00000122a8.
//
//    All three firmware variants validate at least four bytes, load a u32 tag
//    from command offset zero, and treat tags above 18 as invalid. Their jump tables
//    process 0/1/2/3/4/5/7/9/12/14/15/16/18. Tag 6 reaches the dedicated
//    `PMRebuild must not be received in SKSM` fatal, while 8/10/11/13/17
//    reach the invalid-command fatal. The host nevertheless names 17 `Nop`;
//    that producer identity is preserved, but the admission helper marks it invalid
//    because the matched consumer cannot process it.
//
//    Tags 6/8/10/11 remain semantically unresolved instead of inheriting an
//    AGX2 or nested SKU-stream meaning. SKU-stream opcodes are a distinct
//    nested domain and intentionally have no representation in the top-level
//    enum. M5-family identities remain unsupported pending their own paired
//    producer/consumer pass.
//
//    The optional pre-dispatch filter uses literal mask 0x1d03f for tags <=
//    16. It is exposed separately so a future live producer can preserve that
//    gate instead of conflating it with complete jump-table dispatch.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// 9. Provenance ledger + live gaps.
// ---------------------------------------------------------------------------

/// How each submission-ABI component was grounded, so reuse-vs-fork is explicit.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum AbiProvenance {
    /// Byte-identical to the shipping AGX2 producer; reused unchanged.
    IdenticalToAgx2,
    /// AGX3-specific, grounded first-hand from matched host/firmware artifacts.
    ForkedAgx3Grounded,
    /// AGX2 layout the driver produces; the G17 firmware's *own* consumer
    /// offsets were not read first-hand this pass. Kept unsupported.
    Agx2InheritedUnconfirmed,
}

/// Per-component provenance for the submission transport.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SubmissionAbiLedger {
    pub(crate) tx_doorbell: AbiProvenance,
    pub(crate) channel_state: AbiProvenance,
    pub(crate) g17p_channel_state: AbiProvenance,
    pub(crate) ring_state: AbiProvenance,
    pub(crate) channel_descriptor: AbiProvenance,
    pub(crate) host_channel_state_reset: AbiProvenance,
    pub(crate) host_uncached_channel_reset: AbiProvenance,
    pub(crate) g17p_command_types: AbiProvenance,
    pub(crate) kick_word: AbiProvenance,
    pub(crate) kick_queue_producer: AbiProvenance,
    pub(crate) host_add_kicks_mmio: AbiProvenance,
    pub(crate) g17p_queue_config: AbiProvenance,
    pub(crate) g17p_completion_descriptors: AbiProvenance,
    pub(crate) kick_record: AbiProvenance,
}

/// The doorbell is byte-grounded (§1); the SKSM kick word/record and host
/// queue-timestamp producer are AGX3 forks grounded first-hand (§2-§5). The
/// generic channel/ring producer records remain inherited and unsupported.
/// A separate G17P-only channel-state entry records the matched A18 Pro host
/// producer and firmware consumer (§2c); it does not admit either M5 family.
/// The queue bridge/completion descriptors and command dispatch are grounded,
/// G17P-only AGX3 forks (§7-§8): the M5
/// identities remain unsupported; unresolved values do not inherit AGX2 or
/// nested SKU meanings, and the host's `Nop = 17` is not firmware-admitted.
pub(crate) const SUBMISSION_ABI_LEDGER: SubmissionAbiLedger = SubmissionAbiLedger {
    tx_doorbell: AbiProvenance::IdenticalToAgx2,
    channel_state: AbiProvenance::Agx2InheritedUnconfirmed,
    g17p_channel_state: AbiProvenance::ForkedAgx3Grounded,
    ring_state: AbiProvenance::Agx2InheritedUnconfirmed,
    channel_descriptor: AbiProvenance::ForkedAgx3Grounded,
    host_channel_state_reset: AbiProvenance::ForkedAgx3Grounded,
    host_uncached_channel_reset: AbiProvenance::ForkedAgx3Grounded,
    g17p_command_types: AbiProvenance::ForkedAgx3Grounded,
    kick_word: AbiProvenance::ForkedAgx3Grounded,
    kick_queue_producer: AbiProvenance::ForkedAgx3Grounded,
    host_add_kicks_mmio: AbiProvenance::ForkedAgx3Grounded,
    g17p_queue_config: AbiProvenance::ForkedAgx3Grounded,
    g17p_completion_descriptors: AbiProvenance::ForkedAgx3Grounded,
    kick_record: AbiProvenance::ForkedAgx3Grounded,
};

/// The pinned G17P/G17G/G17X hosts all use the target-specific §6 MMIO
/// publication for `KSMKickQueue::addKicks`. EP 0x21 remains the separate
/// channel/work-ring doorbell; the two producers are complementary, not an
/// unresolved selector.
pub(crate) const HOST_SKSM_ADD_KICKS_PATH_SELECTED: bool = true;

/// Whether the AGX3 per-pipe channel *descriptor* stride is proven. The G17S
/// firmware directly walks twelve 0x20-byte entries at
/// `runtime_pointers+0x020..0x19f` (§2a).
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
    /// A native SKSM queue entry followed by the host `addKicks` MMIO pair has
    /// executed real work. The shipping kext uses this path, but the classic
    /// first-partial graphics path is independently executable.
    pub(crate) sksm_add_kicks_round_trip_proven: bool,
    /// Apple's host builds platform interrupt event sources from runtime
    /// `interrupts`/`interrupts-valid` properties. D93AP physical IRQ 1001 and
    /// RTBuddy AKF type 2 both converge exactly on source 4 and drain the main
    /// firmware event rings, but an on-device SKSM completion round trip has
    /// not proven that this ingress is sufficient for Linux completion.
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

/// The host-side writer is implemented. The channel-doorbell execution result
/// comes from `20260827-g17p-compact-contract-queue-live12`: exact 64-float
/// add3 output, then two more submissions on the same CL2 queue at queue and
/// channel counters 6/6/6 and 9/9/9. That does not prove the separate native
/// SKSM `addKicks` FIFO used by the shipping graphics channels.
pub(crate) const SUBMISSION_LIVE_GAPS: SubmissionLiveGaps = SubmissionLiveGaps {
    sksm_resource0_ordered_write_implemented: true,
    channel_doorbell_round_trip_proven: true,
    partial_graphics_runtime_implemented: false,
    partial_graphics_round_trip_proven: true,
    sksm_add_kicks_round_trip_proven: false,
    completion_irq_route_proven: false,
    normal_cl_completion_consumer_implemented: true,
};

#[cfg(test)]
mod tests {
    use super::*;

    /// Witnessed against the macOS capture: a real Metal render leaves the
    /// parameter-buffer ring state at {7, 7, 0, 0}, and `PBState[+0x28]`
    /// holds the full tagged address `0x1000190000`. The encode and decode
    /// must be exact inverses -- that is what makes the field map from the two
    /// kernelcache functions trustworthy.
    #[test]
    fn g17p_pbdesc_round_trips_the_witnessed_ring_state() {
        assert_eq!(g17p_pbdesc_segment(0x10_0019_0000), 0);
        assert_eq!(
            g17p_pbdesc_reconstructed_address(0x10_0019_0000),
            0x10_0019_0000
        );
        assert_eq!(
            g17p_pbdesc_reconstructed_address(0x70_0247_8000),
            0x70_0247_8000
        );
        assert_eq!(
            g17p_pbdesc_reconstructed_address(0x0247_8000),
            0x80_0247_8000
        );
        let state = G17PPbDescRingState {
            counter_a: 7,
            counter_b: 7,
            flag_high: 0,
            flag_low: 0,
        };
        let descriptor = encode_g17p_pbdesc([0; 4], 0x10_0019_0000, 0, state);
        // dword0 carries the address >> 4; dword1 the segment >> 8 up top.
        assert_eq!(descriptor[0], 0x0001_9000);
        assert_eq!(descriptor[1] >> 28, 0x0);
        assert_eq!(decode_g17p_pbdesc(descriptor), state);

        // Bits the firmware owns survive a re-encode.
        let previous = [0x0000_0005u32, 0x0abc_defu32, 0xf000_0000u32, 0x4000_0000u32];
        let again = encode_g17p_pbdesc(previous, 0x10_0019_0000, 0x0012_3456, state);
        assert_eq!(again[0] & 0x6, previous[0] & 0x6);
        assert_eq!(again[1] & 0x003f_ffff, 0x0012_3456);
        assert_eq!(again[2] & !0x003f_ffff, previous[2] & !0x003f_ffff);
        assert_eq!(decode_g17p_pbdesc(again), state);

        assert_eq!(g17p_pbdesc_offset(2), 2 * G17P_PBDESC_ENTRY_SIZE);
    }

    fn get_u64_le(raw: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(raw[offset..offset + 8].try_into().unwrap())
    }

    fn get_u32_le(raw: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(raw[offset..offset + 4].try_into().unwrap())
    }

    fn get_u16_le(raw: &[u8], offset: usize) -> u16 {
        u16::from_le_bytes(raw[offset..offset + 2].try_into().unwrap())
    }

    fn source_fixture_fingerprint(raw: &[u8]) -> u64 {
        raw.iter().fold(0xcbf2_9ce4_8422_2325, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(0x0000_0100_0000_01b3)
        })
    }

    fn rce_binding_from_packed(word: u64) -> G17PClRceBinding {
        G17PClRceBinding {
            address: (word & G17P_CL_PACKED_ADDRESS_MASK) << 5,
            tag: ((word >> 43) & KICK_QID_MASK as u64) as u8,
        }
    }

    fn native_add3_compute_operands(
        descriptor: u64,
        queue: u64,
        converted_command_timestamp: u64,
        event_mask: [u64; 4],
        rce_words: [u64; 2],
    ) -> G17PClKickEntryOperands<'static> {
        let empty = G17PClRceBinding { address: 0, tag: 0 };
        G17PClKickEntryOperands {
            descriptor_flag_4c: false,
            descriptor_flag_5c8: false,
            converted_command_timestamp,
            barriers: &[],
            mcache: None,
            payload: [descriptor, queue],
            event_mask,
            rce_kind: 0,
            rce_bindings: [
                rce_binding_from_packed(rce_words[0]),
                rce_binding_from_packed(rce_words[1]),
                empty,
                empty,
            ],
            auxiliary: 0x003f_ffff_ffff_ffff,
        }
    }

    #[test]
    fn compute_command_state_tracks_each_descriptor_slot() {
        let first = prepare_g17p_compute_cl_command_state(0x70_0034_0000, 0, 0x26).unwrap();
        assert_eq!(first.event_mask, [0, 0, 4, 0]);
        assert_eq!(
            first.rce_bindings,
            [
                G17PClRceBinding {
                    address: 0x70_0034_0040,
                    tag: 0x26,
                },
                G17PClRceBinding {
                    address: 0x70_0034_0760,
                    tag: 0x04,
                },
                G17PClRceBinding { address: 0, tag: 0 },
                G17PClRceBinding { address: 0, tag: 0 },
            ]
        );

        let second = prepare_g17p_compute_cl_command_state(0x70_0034_4000, 1, 0x26).unwrap();
        assert_eq!(second.event_mask, [0, 2, 8, 0]);
        assert_eq!(second.rce_bindings[0].address, 0x70_0034_4040);
        assert_eq!(second.rce_bindings[1].address, 0x70_0034_4760);
    }

    #[test]
    fn render_command_state_selects_native_descriptor_register_arrays() {
        let ta = prepare_g17p_render_cl_command_state(0x70_0001_0740, true).unwrap();
        assert_eq!(ta.event_mask, [0; 4]);
        assert_eq!(
            ta.rce_bindings,
            [
                G17PClRceBinding {
                    address: 0x70_0001_07a0,
                    tag: 0x47,
                },
                G17PClRceBinding { address: 0, tag: 0 },
                G17PClRceBinding { address: 0, tag: 0 },
                G17PClRceBinding { address: 0, tag: 0 },
            ]
        );

        let fragment =
            prepare_g17p_render_cl_command_state(0x70_0016_5800, false).unwrap();
        assert_eq!(
            fragment.rce_bindings,
            [
                G17PClRceBinding {
                    address: 0x70_0016_58a0,
                    tag: 0x56,
                },
                G17PClRceBinding {
                    address: 0x70_0016_5fc0,
                    tag: 0x10,
                },
                G17PClRceBinding {
                    address: 0x70_0016_66e0,
                    tag: 0x17,
                },
                G17PClRceBinding {
                    address: 0x70_0016_6e00,
                    tag: 0x0a,
                },
            ]
        );
    }

    #[test]
    fn tx_doorbell_matches_the_shipping_agx2_wire_form() {
        // gpu.rs: MSG_TX_DOORBELL = 0x83 << 48; run_job kicks
        // MSG_TX_DOORBELL | pipe_type | (index << 2).
        const MSG_TX_DOORBELL: u64 = 0x83 << 48;
        assert_eq!(EP_DOORBELL, 0x21);
        for (pipe, raw) in [
            (PipeType::Vertex, 0u64),
            (PipeType::Fragment, 1),
            (PipeType::Compute, 2),
        ] {
            for index in 0u8..=7 {
                let msg = encode_tx_doorbell(pipe, index).unwrap();
                assert_eq!(msg, MSG_TX_DOORBELL | raw | ((index as u64) << 2));
                // Firmware type extraction: ubfx x8,x1,#48,#6 == 3.
                assert_eq!(doorbell_type(msg), DOORBELL_TYPE_TX);
                assert_eq!(decode_tx_doorbell(msg), Ok((pipe, index)));
            }
        }
        assert_eq!(
            encode_tx_doorbell(PipeType::Vertex, 8),
            Err(SubmissionError::DoorbellIndexOutOfRange)
        );
    }

    #[test]
    fn native_initial_render_uses_the_paired_priority_zero_doorbell() {
        // Pipe identity at priority zero: 3D is the low Fragment pipe 1 and
        // TA is the low Vertex pipe 0. Compute is the remaining pipe 2.
        assert_eq!(
            encode_tx_doorbell(PipeType::Fragment, 0),
            Ok(0x0083_0000_0000_0001)
        );
        assert_eq!(
            encode_tx_doorbell(PipeType::Vertex, 0),
            Ok(0x0083_0000_0000_0000)
        );
        assert_eq!(
            encode_tx_doorbell(PipeType::Compute, 0),
            Ok(0x0083_0000_0000_0002)
        );

        assert_eq!(G17P_NATIVE_RENDER_DOORBELL_PRIORITY, 0);
        assert_eq!(G17P_NATIVE_3D_DOORBELL, 0x0083_0000_0000_0001);
        assert_eq!(G17P_NATIVE_TA_DOORBELL, 0x0083_0000_0000_0000);
        assert_eq!(
            encode_tx_doorbell(PipeType::Fragment, 0),
            Ok(G17P_NATIVE_3D_DOORBELL)
        );
        assert_eq!(
            encode_tx_doorbell(PipeType::Vertex, 0),
            Ok(G17P_NATIVE_TA_DOORBELL)
        );
    }

    #[test]
    fn doorbell_types_come_from_the_firmware_dispatch() {
        // 0x83/0x84/0x85 & 0x3f -> 3/4/5, the firmware's cmp targets.
        assert_eq!(DOORBELL_TYPE_TX, (0x83u8 & 0x3f));
        assert_eq!(DOORBELL_TYPE_FWCTL, (0x84u8 & 0x3f));
        assert_eq!(DOORBELL_TYPE_HALT, (0x85u8 & 0x3f));
        // A FwCtl (0x84<<48) doorbell is not a TX doorbell.
        assert_eq!(
            decode_tx_doorbell(0x84u64 << 48),
            Err(SubmissionError::NotATxDoorbell)
        );
    }

    #[test]
    fn firmware_marks_pipe_three_invalid() {
        // The firmware `cmp w8,#0x3 ; b.eq done` path: pipe 3 is not a pipe.
        let msg = (0x83u64 << 48) | 0x3;
        assert_eq!(doorbell_type(msg), DOORBELL_TYPE_TX);
        assert_eq!(decode_tx_doorbell(msg), Err(SubmissionError::ReservedPipe));
    }

    #[test]
    fn wakeup_doorbell_payloads_decode_like_the_firmware() {
        // DOORBELL_KICKFW=0x10 -> pipe 0, index 4; DOORBELL_DEVCTRL=0x11 ->
        // pipe 1, index 4 (msg & 3, (msg>>2)&7).
        let kickfw = (0x83u64 << 48) | DOORBELL_KICKFW as u64;
        assert_eq!(decode_tx_doorbell(kickfw), Ok((PipeType::Vertex, 4)));
        let devctrl = (0x83u64 << 48) | DOORBELL_DEVCTRL as u64;
        assert_eq!(decode_tx_doorbell(devctrl), Ok((PipeType::Fragment, 4)));
    }

    #[test]
    fn g17p_running_client_power_gate_precedes_every_submit_mutation() {
        let contract = G17P_RUNNING_CLIENT_POWER_CONTRACT;
        assert_eq!(contract.role_index, 1);
        assert_eq!(contract.service_id, 0x20);
        assert_eq!(contract.target, 1);
        assert!(!contract.management_request_used);
        assert!(contract.managed);
        assert!(!contract.standalone);
        assert_eq!(
            G17P_COMPUTE_POWER_LEASE_ORDER,
            [
                G17PComputePowerLeaseStep::AcquireBeforeReadiness,
                G17PComputePowerLeaseStep::HoldThroughClaim,
                G17PComputePowerLeaseStep::HoldThroughCompletion,
                G17PComputePowerLeaseStep::ReleaseAfterCompletion,
            ]
        );

        let mut vm_bind_mutations = 0;
        let mut readiness_mutations = 0;
        let mut sksm_mutations = 0;
        let mut mailbox_mutations = 0;
        let gated: Result<(), SubmissionError> = (|| {
            require_g17p_running_client_power_transition(contract, true, false)?;
            vm_bind_mutations += 1;
            readiness_mutations += 1;
            sksm_mutations += 1;
            mailbox_mutations += 1;
            Ok(())
        })();
        assert_eq!(
            gated,
            Err(SubmissionError::RunningClientPowerTransitionUnavailable)
        );
        assert_eq!(vm_bind_mutations, 0);
        assert_eq!(readiness_mutations, 0);
        assert_eq!(sksm_mutations, 0);
        assert_eq!(mailbox_mutations, 0);
        assert_eq!(
            require_g17p_running_client_power_transition(contract, true, true),
            Ok(())
        );
    }

    #[test]
    fn g17p_native_boot_is_ep21_quiet_until_first_real_user_cl() {
        assert!(G17P_NATIVE_PRE_USER_EP21_MESSAGES.is_empty());
        assert_eq!(
            G17P_NATIVE_FIRST_USER_EP21_MESSAGES,
            [
                encode_g17p_cl_activation(2).unwrap(),
                encode_g17p_direct_cl_kick(),
            ]
        );
        assert_eq!(
            G17P_NATIVE_FIRST_USER_EP21_MESSAGES.map(doorbell_type),
            [3, 3]
        );
        assert!(!G17P_NATIVE_FIRST_USER_EP21_MESSAGES
            .iter()
            .any(|message| doorbell_type(*message) == 4));
    }

    #[test]
    fn g17p_direct_cl_notifications_match_first_and_repeat_fixtures() {
        const ACTIVATION: &[u8; 8] =
            include_bytes!("fixtures/g17p_first_submit/ep21-first-activation.bin");
        const DIRECT_KICK: &[u8; 8] =
            include_bytes!("fixtures/g17p_first_submit/ep21-direct-kick.bin");
        const FIRST_SEQUENCE: &[u8; 16] =
            include_bytes!("fixtures/g17p_first_submit/ep21-first-direct-sequence.bin");

        let first_state = G17PDirectClNotificationState {
            cached_channel_identity: 0,
            submitted_channel_identity: 0xffff_fc20_c07a_1dc0,
            firmware_ready: true,
            direct_feature: true,
            submission_suppressed: false,
            command_submission_enabled: true,
        };
        let first = prepare_g17p_direct_cl_notifications(first_state).unwrap();
        assert_eq!(first.activation.unwrap().to_le_bytes(), *ACTIVATION);
        assert_eq!(first.direct_kick.unwrap().to_le_bytes(), *DIRECT_KICK);
        let mut sequence = [0u8; 16];
        sequence[..8].copy_from_slice(&first.activation.unwrap().to_le_bytes());
        sequence[8..].copy_from_slice(&first.direct_kick.unwrap().to_le_bytes());
        assert_eq!(sequence, *FIRST_SEQUENCE);

        let repeat = prepare_g17p_direct_cl_notifications(G17PDirectClNotificationState {
            cached_channel_identity: first_state.submitted_channel_identity,
            ..first_state
        })
        .unwrap();
        assert_eq!(repeat.activation, None);
        assert_eq!(repeat.direct_kick.unwrap().to_le_bytes(), *DIRECT_KICK);

        let not_ready = prepare_g17p_direct_cl_notifications(G17PDirectClNotificationState {
            firmware_ready: false,
            ..first_state
        })
        .unwrap();
        assert_eq!(not_ready.activation, None);
        assert_eq!(not_ready.direct_kick.unwrap().to_le_bytes(), *DIRECT_KICK);

        for disabled in [
            G17PDirectClNotificationState {
                direct_feature: false,
                ..first_state
            },
            G17PDirectClNotificationState {
                submission_suppressed: true,
                ..first_state
            },
            G17PDirectClNotificationState {
                command_submission_enabled: false,
                ..first_state
            },
        ] {
            let prepared = prepare_g17p_direct_cl_notifications(disabled).unwrap();
            assert_eq!(prepared.activation.unwrap().to_le_bytes(), *ACTIVATION);
            assert_eq!(prepared.direct_kick, None);
        }

        assert_eq!(
            T8140_G17P_CL_BACKEND,
            G17PClBackendConfig {
                backend: G17ClSubmissionBackend::G17PHal200Hybrid,
                direct_feature: true,
                submission_suppressed: false,
                command_submission_enabled: true,
            }
        );
        assert!(g17p_first_bind_tag15_required(true));
        assert!(g17p_first_bind_tag16_required(
            T8140_G17P_CL_BACKEND,
            true
        ));
        assert!(!g17p_first_bind_tag16_required(
            G17PClBackendConfig {
                backend: G17ClSubmissionBackend::Classic,
                ..T8140_G17P_CL_BACKEND
            },
            true
        ));
        assert!(!g17p_first_bind_tag16_required(
            G17PClBackendConfig {
                direct_feature: false,
                ..T8140_G17P_CL_BACKEND
            },
            true
        ));
        assert!(G17P_FIRST_BIND_COMMAND_PLAN.hardware_release_ready());
        assert!(G17P_FIRST_BIND_DIRECT_HARDWARE_RELEASE_READY);

        let first_lifecycle = prepare_g17p_direct_cl_lifecycle(
            G17PDirectClLifecycleState {
                queue_config_dirty: true,
                cached_channel_identity: 0,
                next_correlation_token: 1,
            },
            first_state.submitted_channel_identity,
            true,
            T8140_G17P_CL_BACKEND,
        )
        .unwrap();
        assert!(first_lifecycle.publish_config_update);
        assert!(first_lifecycle.publish_entry_signal);
        assert_eq!(first_lifecycle.correlation_token, 1);
        assert_eq!(first_lifecycle.notifications, first);
        assert_eq!(
            first_lifecycle.next,
            G17PDirectClLifecycleState {
                queue_config_dirty: false,
                cached_channel_identity: first_state.submitted_channel_identity,
                next_correlation_token: 2,
            }
        );

        let repeat_lifecycle = prepare_g17p_direct_cl_lifecycle(
            first_lifecycle.next,
            first_state.submitted_channel_identity,
            true,
            T8140_G17P_CL_BACKEND,
        )
        .unwrap();
        assert!(!repeat_lifecycle.publish_config_update);
        assert!(!repeat_lifecycle.publish_entry_signal);
        assert_eq!(repeat_lifecycle.correlation_token, 2);
        assert_eq!(repeat_lifecycle.notifications.activation, None);
        assert_eq!(repeat_lifecycle.notifications.direct_kick, first.direct_kick);
        assert_eq!(repeat_lifecycle.next.next_correlation_token, 3);
        assert_eq!(
            prepare_g17p_direct_cl_lifecycle(
                G17PDirectClLifecycleState {
                    next_correlation_token: 0,
                    ..first_lifecycle.next
                },
                first_state.submitted_channel_identity,
                true,
                T8140_G17P_CL_BACKEND,
            ),
            Err(SubmissionError::ComputeEventCounterOverflow)
        );

        let mut command_ring_mutations = 0;
        let mut sksm_storage_mutations = 0;
        let mut sksm_mmio_mutations = 0;
        let mut outer_ring_mutations = 0;
        let mut mailbox_mutations = 0;
        let blocked_plan = G17PFirstBindCommandPlan {
            optional_correlation_ring: G17PCorrelationRingImplementationState::Pending,
            ..G17P_FIRST_BIND_COMMAND_PLAN
        };
        let gated: Result<(), SubmissionError> = (|| {
            require_g17p_first_bind_direct_hardware_release(blocked_plan)?;
            command_ring_mutations += 2;
            sksm_storage_mutations += 1;
            sksm_mmio_mutations += 1;
            outer_ring_mutations += 1;
            mailbox_mutations += 2;
            Ok(())
        })();
        assert_eq!(
            gated,
            Err(SubmissionError::FirstBindCommandPublicationIncomplete)
        );
        assert_eq!(command_ring_mutations, 0);
        assert_eq!(sksm_storage_mutations, 0);
        assert_eq!(sksm_mmio_mutations, 0);
        assert_eq!(outer_ring_mutations, 0);
        assert_eq!(mailbox_mutations, 0);

        let resolved = G17P_FIRST_BIND_COMMAND_PLAN;
        assert_eq!(resolved.fallback_logical_flush_id, 0);
        assert!(!resolved.flush_id_serialized_in_tag15);
        assert!(!resolved.flush_id_completion_release_required);
        assert!(resolved.hardware_release_ready());
        assert_eq!(
            require_g17p_first_bind_direct_hardware_release(resolved),
            Ok(())
        );
        for unresolved_implementation in [
            G17PFlushIdImplementationState::Pending,
            G17PFlushIdImplementationState::ProvenAbsentFallbackUnverified,
            G17PFlushIdImplementationState::ProvenPresentUnimplemented,
        ] {
            assert!(!G17PFirstBindCommandPlan {
                dedicated_flush_id: unresolved_implementation,
                ..resolved
            }
            .hardware_release_ready());
        }
        assert!(G17PFirstBindCommandPlan {
            dedicated_flush_id: G17PFlushIdImplementationState::ProvenPresentImplemented,
            ..resolved
        }
        .hardware_release_ready());
        assert!(!G17PFirstBindCommandPlan {
            optional_correlation_ring: G17PCorrelationRingImplementationState::Pending,
            ..resolved
        }
        .hardware_release_ready());
        assert!(!G17PFirstBindCommandPlan {
            native_command_pointer_ring: G17PContractPredicateState::Pending,
            ..resolved
        }
        .hardware_release_ready());
        assert!(G17PFirstBindCommandPlan {
            optional_correlation_ring: G17PCorrelationRingImplementationState::ProvenInactive,
            ..resolved
        }
        .hardware_release_ready());
        assert!(G17PFirstBindCommandPlan {
            optional_correlation_ring:
                G17PCorrelationRingImplementationState::ProvenRequiredImplemented,
            ..resolved
        }
        .hardware_release_ready());
        assert!(!g17p_first_bind_tag16_required(
            G17PClBackendConfig {
                backend: G17ClSubmissionBackend::Hal300,
                ..T8140_G17P_CL_BACKEND
            },
            true
        ));
    }

    #[test]
    fn pending_pipe_bit_matches_1_shl_pipe() {
        assert_eq!(pending_pipe_bit(PipeType::Vertex), 1);
        assert_eq!(pending_pipe_bit(PipeType::Fragment), 2);
        assert_eq!(pending_pipe_bit(PipeType::Compute), 4);
    }

    #[test]
    fn inherited_channel_and_ring_state_offsets_are_labeled_agx2() {
        // channel.rs raw::ChannelState: read_ptr@0x00, write_ptr@0x20.
        assert_eq!(AGX2_CHANNEL_STATE_READ_PTR_OFFSET, 0x00);
        assert_eq!(AGX2_CHANNEL_STATE_WRITE_PTR_OFFSET, 0x20);
        assert_eq!(AGX2_CHANNEL_STATE_SIZE, 0x40);
        // fw/workqueue.rs raw::RingState: doneptr@0x00, rptr@0x30, wptr@0x40,
        // size@0x50 (0x10-strided).
        assert_eq!(AGX2_RING_STATE_GPU_DONEPTR_OFFSET, 0x00);
        assert_eq!(AGX2_RING_STATE_GPU_RPTR_OFFSET, 0x30);
        assert_eq!(AGX2_RING_STATE_CPU_WPTR_OFFSET, 0x40);
        assert_eq!(AGX2_RING_STATE_RB_SIZE_OFFSET, 0x50);
        assert_eq!(AGX2_RING_ENTRY_BYTES, 8);
        assert_eq!(HOST_MAX_OUTSTANDING_JOB_SLOTS, 127);
    }

    #[test]
    fn g17p_native_channel_layout_is_identity_scoped() {
        assert_eq!(
            g17_channel_abi(G17ChannelAbiTarget::A18ProG17P),
            Ok(G17P_CHANNEL_ABI)
        );
        assert_eq!(
            g17_channel_abi(G17ChannelAbiTarget::M5G17G),
            Err(SubmissionError::ChannelAbiUnconfirmed)
        );
        assert_eq!(
            g17_channel_abi(G17ChannelAbiTarget::M5ProMaxG17X),
            Err(SubmissionError::ChannelAbiUnconfirmed)
        );

        assert_eq!(
            G17P_CHANNEL_ABI.cached,
            G17PCachedChannelStateLayout {
                uncached_state_gpu_va: 0x00,
                command_pointer_ring_gpu_va: 0x08,
                coherency_cursor: 0x1c,
                record_size: 0x80,
            }
        );
        assert_eq!(
            G17P_CHANNEL_ABI.uncached,
            G17PUncachedChannelStateLayout {
                consumer: 0x00,
                coherency_shadow: 0x30,
                producer: 0x40,
                recovery_target: 0x50,
                ring_entry_count: 0x60,
                cursor_width: 4,
                write_extent: 0x64,
            }
        );
        assert_eq!(G17P_CHANNEL_ABI.command_pointer_entry_bytes, 8);
        assert_eq!(G17P_CHANNEL_ABI.accelerator_target_width, 2);
        assert!(G17P_CHANNEL_PRODUCER_REQUIRES_DMB_ISH);
    }

    #[test]
    fn g17p_classic_work_slots_and_sksm_pointer_entries_are_distinct() {
        assert_eq!(G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE, 0x18);
        assert_eq!(G17P_CHANNEL_ABI.command_pointer_entry_bytes, 0x08);

        let sksm = prepare_g17p_channel_write(
            G17ChannelAbiTarget::A18ProG17P,
            1,
            3,
            4,
        )
        .unwrap();
        assert_eq!(sksm.entry_byte_offset, 0x08);

        let classic = prepare_g17p_partial_opening_outer_slot(
            G17PPartialOpeningOuterChannelState {
                consumers: [0, 0],
                producer: 1,
                queue_gpu_va: 0xffff_fc20_c000_00c0,
                queue_write_index: 3,
            },
            G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX,
            1,
            1,
        )
        .unwrap();
        assert_eq!(classic.slot_byte_offset, 0x18);
        assert_eq!(get_u64_le(&classic.slot, 0x08), 0xffff_fc20_c000_00c0);
    }

    #[test]
    fn g17p_channel_write_plan_matches_host_ordering_and_wrap() {
        assert_eq!(
            prepare_g17p_channel_write(G17ChannelAbiTarget::A18ProG17P, 2, 0, 4),
            Ok(G17PChannelWritePlan {
                entry_byte_offset: 0x10,
                next_producer: 3,
            })
        );
        assert_eq!(
            prepare_g17p_channel_write(G17ChannelAbiTarget::A18ProG17P, 3, 2, 4),
            Ok(G17PChannelWritePlan {
                entry_byte_offset: 0x18,
                next_producer: 0,
            })
        );
        assert_eq!(
            prepare_g17p_channel_write(G17ChannelAbiTarget::A18ProG17P, 1, 2, 4),
            Err(SubmissionError::RingFull)
        );
        assert_eq!(
            prepare_g17p_channel_write(G17ChannelAbiTarget::A18ProG17P, 0, 0, 0),
            Err(SubmissionError::RingSizeZero)
        );
        assert_eq!(
            prepare_g17p_channel_write(G17ChannelAbiTarget::M5ProMaxG17X, 0, 2, 4),
            Err(SubmissionError::ChannelAbiUnconfirmed)
        );
    }

    #[test]
    fn g17p_partial_opening_pages_and_context_match_the_successful_graph() {
        assert_eq!(G17P_PARTIAL_OPENING_PAGE_SIZE, 0x4000);
        assert_eq!(G17P_PARTIAL_OPENING_SCHEDULER_GPU_VA, 0xffff_fc20_015e_0000);
        assert_eq!(
            G17P_PARTIAL_OPENING_COMPUTED_GPU_VA,
            0xffff_fc20_015d_8000
        );
        assert_eq!(
            G17P_PARTIAL_OPENING_PRIMARY_INDEX_FIRMWARE_GPU_VA,
            0xffff_fc20_c084_8000
        );
        assert_eq!(
            G17P_PARTIAL_OPENING_PRIMARY_INDEX_GPU_VA,
            0x0000_0010_0019_0000
        );
        assert_eq!(G17P_PARTIAL_OPENING_PRIMARY_INDEX_SIZE, 0x10000);

        let scheduler = build_g17p_partial_opening_scheduler_page();
        assert_eq!(get_u64_le(&scheduler, 0), 0x0000_0020_0001_9000);
        assert!(scheduler[8..].iter().all(|value| *value == 0));
        let mut scheduler_slice = [0xa5; G17P_PARTIAL_OPENING_PAGE_SIZE];
        assert_eq!(
            apply_g17p_partial_opening_scheduler_page(&mut scheduler_slice),
            Ok(())
        );
        assert_eq!(scheduler_slice, scheduler);

        let primary = build_g17p_partial_opening_primary_index_page();
        let expected = [
            0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1d, 0x1e, 0x20, 0x21,
            0x22, 0x23, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x2b, 0x2c, 0x2d, 0x3c, 0x3d, 0x3e, 0x3f,
            0x41, 0x42, 0x43, 0x44,
        ];
        for (index, value) in expected.into_iter().enumerate() {
            assert_eq!(get_u32_le(&primary, index * 4), value);
        }
        assert!(primary[expected.len() * 4..]
            .iter()
            .all(|value| *value == 0));
        let mut primary_slice = [0xa5; G17P_PARTIAL_OPENING_PAGE_SIZE];
        assert_eq!(
            apply_g17p_partial_opening_primary_index_page(&mut primary_slice),
            Ok(())
        );
        assert_eq!(primary_slice, primary);
        assert_eq!(
            apply_g17p_partial_opening_scheduler_page(&mut []),
            Err(SubmissionError::PartialOpeningPageSize)
        );
        assert_eq!(
            apply_g17p_partial_opening_primary_index_page(&mut [0; 1]),
            Err(SubmissionError::PartialOpeningPageSize)
        );

        assert_eq!(
            G17P_PARTIAL_OPENING_CONTEXT,
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
                work_doorbell_channel: 0,
                context0_low_root_present_at_first_work: true,
                render_low_root_present_at_first_work: true,
                source_firmware_high_root_table_resident: false,
            }
        );
    }

    #[test]
    fn g17p_cold_opening_support_graph_matches_the_source_bytes() {
        let addresses = G17PColdOpeningGraphAddresses {
            primary_index: 0x70_0100_0000,
            secondary_index: 0x70_0100_4000,
            pool_a_slots: 0x70_0100_8000,
            pool_b_slots: 0x70_0100_c000,
            shared_slots: 0x70_0101_0000,
            flag: 0x70_0101_4000,
        };
        let mut page = [0xa5; G17P_PARTIAL_OPENING_PAGE_SIZE];
        apply_g17p_cold_opening_leaf_page(G17PColdOpeningLeaf::SecondaryIndex, &mut page).unwrap();
        for (index, expected) in [0x11, 0x16, 0x1b, 0x20, 0x25, 0x2a, 0x3c, 0x41]
            .into_iter()
            .enumerate()
        {
            assert_eq!(get_u64_le(&page, index * 8), expected);
        }
        assert_eq!(source_fixture_fingerprint(&page), 0x5538_4e84_8d5a_840b);
        assert!(page[0x40..].iter().all(|value| *value == 0));
        apply_g17p_cold_opening_leaf_page(G17PColdOpeningLeaf::PoolASlots, &mut page).unwrap();
        assert_eq!(get_u32_le(&page, 4), 2);
        assert_eq!(source_fixture_fingerprint(&page), 0xcfc1_218a_7364_2c07);
        apply_g17p_cold_opening_leaf_page(G17PColdOpeningLeaf::PoolBSlots, &mut page).unwrap();
        assert_eq!(source_fixture_fingerprint(&page), 0x9c1b_da7f_8c87_2325);
        apply_g17p_cold_opening_leaf_page(G17PColdOpeningLeaf::SharedSlots, &mut page).unwrap();
        assert_eq!(get_u32_le(&page, 0), 8);
        assert_eq!(get_u32_le(&page, 4), 8);
        assert_eq!(get_u32_le(&page, 0x60), 1);
        assert_eq!(source_fixture_fingerprint(&page), 0x53c2_7f11_bcfb_4124);
        apply_g17p_cold_opening_leaf_page(G17PColdOpeningLeaf::Flag, &mut page).unwrap();
        assert_eq!(get_u32_le(&page, 0), 1);
        assert_eq!(source_fixture_fingerprint(&page), 0xba85_253c_a046_2324);

        apply_g17p_cold_opening_leaf_page(G17PColdOpeningLeaf::PrimaryIndex, &mut page).unwrap();
        assert_eq!(source_fixture_fingerprint(&page), 0x85e8_d166_b202_3e2d);

        let mut pool_a = [0u8; G17P_COLD_OPENING_RECORD_POOL_A_SIZE];
        apply_g17p_cold_opening_record_pool_a(addresses, &mut pool_a).unwrap();
        assert_eq!(get_u64_le(&pool_a, 0), addresses.pool_a_slots);
        assert_eq!(get_u32_le(&pool_a, 0x10), 0x50);
        assert_eq!(
            get_u64_le(&pool_a, 34 * 0x100),
            addresses.pool_a_slots + 34 * 4
        );
        assert_eq!(source_fixture_fingerprint(&pool_a), 0x42cb_4f83_a145_54be);

        let mut pool_b = [0u8; G17P_COLD_OPENING_RECORD_POOL_B_SIZE];
        apply_g17p_cold_opening_record_pool_b(addresses, &mut pool_b).unwrap();
        assert_eq!(get_u32_le(&pool_b, 0), 0x80004);
        assert_eq!(get_u32_le(&pool_b, 0x28), 0x178020);
        assert_eq!(get_u32_le(&pool_b, 35 * 0x80 + 0x28), 0x178000);
        assert_eq!(get_u32_le(&pool_b, 36 * 0x80 + 0x28), 0x178020);
        assert_eq!(get_u64_le(&pool_b, 0x40), addresses.shared_slots + 0x40);
        assert_eq!(get_u32_le(&pool_b, 0x4c), 1);
        assert_eq!(source_fixture_fingerprint(&pool_b), 0xb98b_d0a4_1c25_9a6e);

        let mut shared = [0u8; G17P_COLD_OPENING_SHARED_OBJECT_SIZE];
        apply_g17p_cold_opening_shared_object(addresses, &mut shared).unwrap();
        assert_eq!(get_u32_le(&shared, 0x0c), 0);
        assert_eq!(
            get_u32_le(&shared, 0x0c),
            u32::from(G17P_PARTIAL_OPENING_CONTEXT.shared_queue_pair_namespace)
        );
        assert_eq!(get_u64_le(&shared, 0x20), addresses.primary_index);
        assert_eq!(get_u32_le(&shared, 0x34), 0x20);
        assert_eq!(get_u32_le(&shared, 0x3c), 8);
        assert_eq!(get_u64_le(&shared, 0x44), addresses.secondary_index);
        assert_eq!(get_u64_le(&shared, 0x4c), addresses.shared_slots);
        assert_eq!(get_u32_le(&shared, 0x54), 0x1f);
        assert_eq!(get_u64_le(&shared, 0x64), addresses.flag);
        assert_eq!(source_fixture_fingerprint(&shared), 0x5d97_133c_cbf5_a77c);

        let graph_range = 0x70_0100_0000..0x70_0101_8000;
        for address in [
            get_u64_le(&pool_a, 0),
            get_u64_le(&pool_a, 34 * 0x100),
            get_u64_le(&pool_b, 0x08),
            get_u64_le(&pool_b, 78 * 0x80 + 0x08),
            get_u64_le(&pool_b, 0x40),
            get_u64_le(&shared, 0x20),
            get_u64_le(&shared, 0x44),
            get_u64_le(&shared, 0x4c),
            get_u64_le(&shared, 0x64),
        ] {
            assert!(graph_range.contains(&address));
        }

        let mut control = [0u8; G17P_COLD_OPENING_SHARED_CONTROL_SIZE];
        apply_g17p_cold_opening_shared_control(0x70_0020_8000, 0x70_0102_0000, &mut control)
            .unwrap();
        assert_eq!(get_u32_le(&control, 0), 1);
        assert_eq!(get_u32_le(&control, 0x10), 2);
        assert_eq!(get_u64_le(&control, 0x20), 0x0000_1900_0000_0000);
        assert_eq!(get_u64_le(&control, 0x30), 0x70_0020_8000);
        assert_eq!(get_u32_le(&control, 0x48), 0xe0);
        assert_eq!(get_u64_le(&control, 0x4c), 0x70_0102_0000);
        assert_eq!(get_u32_le(&control, 0x60), 3);
        assert_eq!(source_fixture_fingerprint(&control), 0x9457_5ea8_9f02_1db4);
        assert!(G17P_COLD_OPENING_OPERAND_TABLE_GPU_VA >= 0x70_0000_0000);
        assert!(G17P_COLD_OPENING_OPERAND_TABLE_GPU_VA < 0x71_0000_0000);
        assert!((0x70_0102_0000..0x70_0102_4000).contains(&get_u64_le(&control, 0x4c)));
        let mut inner = [0u8; G17P_COLD_OPENING_SHARED_CONTROL_INNER_SIZE];
        apply_g17p_cold_opening_shared_control_inner(&mut inner).unwrap();
        assert_eq!(get_u32_le(&inner, 0), 2);
        assert_eq!(source_fixture_fingerprint(&inner), 0x8d1a_ce90_4a39_8d17);

        let mut channel = [0u8; G17P_COLD_OPENING_CHANNEL_CONTROL_SIZE];
        apply_g17p_cold_opening_channel_control(&mut channel).unwrap();
        assert_eq!(get_u64_le(&channel, 0), 0x0000_0100_0000_ffff);
        assert_eq!(get_u64_le(&channel, 0x20), 0x0002_0000_0000_0000);
        assert_eq!(get_u64_le(&channel, 0x30), 0x0000_0000_ff00_0000);
        assert_eq!(source_fixture_fingerprint(&channel), 0x448e_89f5_b330_26cb);
    }

    #[test]
    fn g17p_native_cold_pool_a_starts_at_initialized_record_one_only() {
        let addresses = G17PColdOpeningGraphAddresses {
            primary_index: 0x70_0100_0000,
            secondary_index: 0x70_0100_4000,
            pool_a_slots: 0x70_0100_8000,
            pool_b_slots: 0x70_0100_c000,
            shared_slots: 0x70_0101_0000,
            flag: 0x70_0101_4000,
        };
        assert_eq!(g17p_render_pool_a_record_index(0, false), 0);
        assert_eq!(g17p_render_pool_a_record_index(0, true), 1);
        assert_eq!(g17p_render_pool_a_record_index(1, false), 2);
        assert_eq!(g17p_render_pool_a_record_index(1, true), 2);

        let mut slots = [0xa5; G17P_PARTIAL_OPENING_PAGE_SIZE];
        apply_g17p_cold_opening_pool_a_start_slot(1, &mut slots).unwrap();
        assert_eq!(get_u32_le(&slots, 4), 1);
        assert_eq!(get_u32_le(&slots, 8), 0);
        assert!(slots[..4].iter().all(|byte| *byte == 0));
        assert!(slots[12..].iter().all(|byte| *byte == 0));

        let mut records = [0xa5; G17P_COLD_OPENING_RECORD_POOL_A_SIZE];
        apply_g17p_cold_opening_record_pool_a_at(addresses, 1, &mut records).unwrap();
        assert_eq!(get_u64_le(&records, 0), addresses.pool_a_slots);
        assert_eq!(get_u32_le(&records, 0x10), 0);
        assert_eq!(get_u64_le(&records, 0x100), addresses.pool_a_slots + 4);
        assert_eq!(get_u32_le(&records, 0x110), 0x50);
        assert_eq!(get_u32_le(&records, 0x108), 0);
        assert_eq!(
            get_u64_le(
                &records,
                (G17P_COLD_OPENING_RECORD_POOL_A_COUNT - 1)
                    * G17P_COLD_OPENING_RECORD_POOL_A_STRIDE,
            ),
            addresses.pool_a_slots
                + ((G17P_COLD_OPENING_RECORD_POOL_A_COUNT - 1) * 4) as u64
        );
    }

    #[test]
    fn g17p_cold_opening_work_records_keep_identity_context_and_relocatable_pointers() {
        let optional_addresses = G17PColdOpeningOptionalAddresses {
            context_scratch: 0x70_0200_0000,
            firmware_scratch: 0x70_0200_4000,
            usc_priv_mem_flist: 0x70_0200_8000,
            usc_freelist_hardware_buffer_id: 0,
            channel_control: 0x70_0200_c000,
            tiling_shared_object: 0x70_0201_0000,
            parameter_buffer_token: 0,
            owner_pid: 0x15,
            ta_hardware_buffer_id: 0,
        };
        let supplied = 0x70_0200_0000..0x70_0202_0000;
        for stage in [G17PColdOpeningStage::Tiling, G17PColdOpeningStage::Fragment] {
            let mut optional = [0u8; G17P_COLD_OPENING_OPTIONAL_SIZE];
            apply_g17p_cold_opening_optional(
                stage,
                optional_addresses,
                0,
                0,
                0,
                1,
                1,
                &mut optional,
            )
                .unwrap();
            assert_eq!(get_u32_le(&optional, 0), 0x0f);
            for offset in [0x08, 0x10, 0x36, 0x4a] {
                assert!(supplied.contains(&get_u64_le(&optional, offset)));
            }
            assert_eq!(get_u16_le(&optional, 0x1e), 2);
            assert_eq!(get_u16_le(&optional, 0x32), 1);
            assert_eq!(get_u16_le(&optional, 0x46), 0);
            assert_eq!(get_u16_le(&optional, 0x56), 0);
            assert_eq!(get_u32_le(&optional, 0x5a), 0x15);
            assert_eq!(get_u16_le(&optional, 0x5e), 2);
            assert_eq!(get_u16_le(&optional, 0x3e), 0);
            assert_eq!(
                source_fixture_fingerprint(&optional),
                match stage {
                    G17PColdOpeningStage::Tiling => 0x3511_6ec9_a9f3_7149,
                    G17PColdOpeningStage::Fragment => 0x2a62_5e4c_5170_22ef,
                }
            );

            let mut event = [0u8; G17P_COLD_OPENING_EVENT_SIZE];
            apply_g17p_cold_opening_event(stage, 0, &mut event).unwrap();
            assert_eq!(get_u32_le(&event, 0x08), 0x102);
            assert_eq!(
                source_fixture_fingerprint(&event),
                match stage {
                    G17PColdOpeningStage::Tiling => 0x9eb4_fa84_39f9_667b,
                    G17PColdOpeningStage::Fragment => 0x86a6_d8e2_4f16_eea1,
                }
            );

            let descriptor = if stage == G17PColdOpeningStage::Tiling {
                G17P_COLD_OPENING_TA_DESCRIPTOR_BASE
            } else {
                G17P_COLD_OPENING_3D_DESCRIPTOR_BASE
            };
            let queue = if stage == G17PColdOpeningStage::Tiling {
                0x70_0201_c000
            } else {
                0x70_0201_e000
            };
            let mut context = [0u8; G17P_COLD_OPENING_QUEUE_CONTEXT_SIZE];
            apply_g17p_cold_opening_queue_context(stage, descriptor, queue, &mut context).unwrap();
            assert_eq!(get_u64_le(&context, 0x210), descriptor);
            assert_eq!(get_u64_le(&context, 0x218), queue);
            assert_eq!(
                get_u64_le(&context, 0x200),
                match stage {
                    G17PColdOpeningStage::Tiling => 0x1000_0000_0000_0004,
                    G17PColdOpeningStage::Fragment => 0x1000_0400_0000_0004,
                }
            );
            if stage == G17PColdOpeningStage::Fragment {
                assert_eq!(get_u64_le(&context, 0x208), 0);
                assert_eq!(get_u64_le(&context, 0x358), 0x0000_8003_8000_4c3e);
                assert_eq!(get_u64_le(&context, 0x360), 0x0000_b803_8000_4c77);
                assert_eq!(get_u64_le(&context, 0x368), 0x0000_5003_8000_4cb0);
            } else {
                assert_eq!(get_u64_le(&context, 0x350), 0x0002_3803_8000_0003);
            }
            assert!(supplied.contains(&queue));
            assert_eq!(
                source_fixture_fingerprint(&context),
                match stage {
                    G17PColdOpeningStage::Tiling => 0xd30d_fd3c_7336_4025,
                    G17PColdOpeningStage::Fragment => 0xdfda_d80d_e0cc_292b,
                }
            );
        }

        let queue_addresses = G17PColdOpeningQueueAddresses {
            pointers: 0x70_0200_0000,
            item_ring: 0x70_0200_4000,
            job_list: 0x70_0200_8000,
            channel_control: 0x70_0200_c000,
            owner_pid: 0x15,
        };
        let mut queue = [0u8; G17P_COLD_OPENING_QUEUE_RECORD_SIZE];
        apply_g17p_cold_opening_queue_record(queue_addresses, &mut queue).unwrap();
        for offset in [0x00, 0x08, 0x10, 0x70] {
            assert!(supplied.contains(&get_u64_le(&queue, offset)));
        }
        assert_eq!(get_u32_le(&queue, 0x48), 0);
        assert_eq!(get_u32_le(&queue, 0x28), 2);
        assert_eq!(get_u32_le(&queue, 0x3c), 2);
        assert_eq!(get_u32_le(&queue, 0x40), u32::MAX);
        assert_eq!(get_u32_le(&queue, 0x44), 0x15);
        assert_eq!(source_fixture_fingerprint(&queue), 0x0704_bf11_9ee1_8084);

        let mut pointers = [0u8; G17P_COLD_OPENING_POINTER_BLOCK_SIZE];
        apply_g17p_cold_opening_pointer_block(&mut pointers).unwrap();
        assert_eq!(get_u32_le(&pointers, 0x50), u32::MAX);
        assert_eq!(get_u64_le(&pointers, 0x60), 0x500);
        assert_eq!(source_fixture_fingerprint(&pointers), 0x41b7_257d_6bc8_1cb2);

        let mut descriptor = [0u8; G17P_PARTIAL_OPENING_PAGE_SIZE];
        apply_g17p_cold_opening_descriptor_overrides(G17PColdOpeningStage::Tiling, &mut descriptor)
            .unwrap();
        assert_eq!(get_u32_le(&descriptor, 0x0c), 1);
        assert_eq!(descriptor[0x0789], 0x08);
        assert_eq!(&descriptor[0x093e..0x0940], &[0xd0, 0x91]);
        assert_eq!(
            source_fixture_fingerprint(&descriptor),
            0x0a67_4d00_ea32_e630
        );

        descriptor.fill(0);
        apply_g17p_cold_opening_descriptor_overrides(
            G17PColdOpeningStage::Fragment,
            &mut descriptor,
        )
        .unwrap();
        assert_eq!(
            source_fixture_fingerprint(&descriptor),
            0xaac5_6b92_e70a_d459
        );
    }

    #[test]
    fn g17p_cold_opening_context_locators_follow_descriptor_relocation() {
        for (stage, base, locator_offsets) in [
            (
                G17PColdOpeningStage::Tiling,
                G17P_COLD_OPENING_TA_DESCRIPTOR_BASE,
                &[0x350usize][..],
            ),
            (
                G17PColdOpeningStage::Fragment,
                G17P_COLD_OPENING_3D_DESCRIPTOR_BASE,
                &[0x350usize, 0x358, 0x360, 0x368][..],
            ),
        ] {
            let mut native = [0u8; G17P_COLD_OPENING_QUEUE_CONTEXT_SIZE];
            let mut relocated = [0u8; G17P_COLD_OPENING_QUEUE_CONTEXT_SIZE];
            apply_g17p_cold_opening_queue_context(stage, base, 0x70_0201_c000, &mut native)
                .unwrap();
            apply_g17p_cold_opening_queue_context(
                stage,
                base + 0x8000,
                0x70_0201_c000,
                &mut relocated,
            )
            .unwrap();

            for offset in locator_offsets {
                assert_eq!(
                    get_u64_le(&relocated, *offset),
                    get_u64_le(&native, *offset) + 0x400
                );
            }
            assert_eq!(get_u64_le(&relocated, 0x210), base + 0x8000);

            assert_eq!(
                apply_g17p_cold_opening_queue_context(
                    stage,
                    base - 0x20,
                    0x70_0201_c000,
                    &mut relocated,
                ),
                Err(SubmissionError::PartialOpeningDescriptorLocatorUnencodable)
            );
            assert_eq!(
                apply_g17p_cold_opening_queue_context(
                    stage,
                    base + 1,
                    0x70_0201_c000,
                    &mut relocated,
                ),
                Err(SubmissionError::PartialOpeningDescriptorLocatorUnencodable)
            );
        }
    }

    #[test]
    fn g17p_cold_opening_operand_graph_is_exact() {
        let mut table = [0u8; G17P_PARTIAL_OPENING_PAGE_SIZE];
        apply_g17p_cold_opening_operand_table(&mut table).unwrap();
        assert_eq!(source_fixture_fingerprint(&table), 0x9c1b_da7f_8c87_2325);
        assert!(table.iter().all(|value| *value == 0));

        apply_g17p_populated_operand_table(&mut table).unwrap();
        assert_eq!(
            get_u64_le(&table, 0),
            (1u64 << 60) | G17P_COLD_OPENING_OPERAND_BUFFER_BASE
        );
        assert_eq!(
            get_u64_le(&table, 27 * 0x40),
            (1u64 << 60)
                | (G17P_COLD_OPENING_OPERAND_BUFFER_BASE
                    + 27 * G17P_COLD_OPENING_OPERAND_BUFFER_STRIDE)
        );
        assert!(table[28 * 0x40..].iter().all(|value| *value == 0));
        assert_eq!(source_fixture_fingerprint(&table), 0x61a7_b063_3bea_e785);

        let mut directory = [0u8; G17P_COLD_OPENING_OPERAND_DIRECTORY_SIZE];
        apply_g17p_cold_opening_operand_directory(&mut directory).unwrap();
        assert_eq!(
            source_fixture_fingerprint(&directory),
            0xeb05_052e_a5b6_2325
        );
        assert!(directory.iter().all(|value| *value == 0));

        apply_g17p_populated_operand_directory(&mut directory).unwrap();
        assert_eq!(get_u64_le(&directory, 0), 0x70_0022_0000);
        assert_eq!(get_u64_le(&directory, 255 * 8), 0x70_0031_f000);
        assert_eq!(get_u64_le(&directory, 256 * 8), 0x70_0032_8000);
        assert_eq!(get_u64_le(&directory, (7168 - 1) * 8), 0x70_01ef_7000);
        assert!(directory[7168 * 8..].iter().all(|value| *value == 0));
        assert_eq!(
            source_fixture_fingerprint(&directory),
            0xd5c6_623f_4366_03a5
        );
    }

    #[test]
    fn g17p_partial_opening_status_is_exact_and_checked() {
        const KERNEL_VA_BASE: u64 = 0xffff_fc20_0000_0000;
        assert_eq!(
            prepare_g17p_partial_opening_pre_0x84_status(KERNEL_VA_BASE),
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
                    value: KERNEL_VA_BASE + 0x001c_0000,
                },
                G17PPartialOpeningStatusWrite {
                    offset: 0x48e8,
                    value: KERNEL_VA_BASE + 0x001c_0040,
                },
            ])
        );
        assert_eq!(
            prepare_g17p_partial_opening_pre_0x84_status(u64::MAX),
            Err(SubmissionError::PartialOpeningAddressOverflow)
        );
    }

    #[test]
    fn g17p_partial_opening_publishes_3d_then_ta_with_one_pair_kick() {
        assert_eq!(
            G17P_PARTIAL_OPENING_PUBLICATION_ORDER,
            [
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
            ]
        );
        let prepared = prepare_g17p_partial_opening_work_pair(
            G17PPartialOpeningOuterChannelState {
                consumers: [0, 0],
                producer: 0,
                queue_gpu_va: 0xffff_fc20_c000_00c0,
                queue_write_index: 3,
            },
            G17PPartialOpeningOuterChannelState {
                consumers: [0, 0],
                producer: 0,
                queue_gpu_va: 0xffff_fc20_c000_0000,
                queue_write_index: 3,
            },
            1,
            0,
            true,
        )
        .unwrap();
        assert_eq!(
            prepared.fragment,
            G17PPartialOpeningOuterSlotPlan {
                channel_table_index: 7,
                slot_index: 0,
                slot_byte_offset: 0,
                slot: [
                    0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0x00, 0x00, 0xc0, 0x20, 0xfc, 0xff, 0xff, 1, 0,
                    0, 0, 3, 0, 1, 1,
                ],
                next_producer: 1,
            },
            1,
            0
        );
        assert_eq!(
            prepared.tiling,
            G17PPartialOpeningOuterSlotPlan {
                channel_table_index: 6,
                slot_index: 0,
                slot_byte_offset: 0,
                slot: [
                    0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0xc0, 0x20, 0xfc, 0xff, 0xff, 0, 0,
                    0, 0, 3, 0, 0, 1,
                ],
                next_producer: 1,
            }
        );
        assert_eq!(G17P_PARTIAL_OPENING_OUTER_SLOT_SIZE, 0x18);
        assert_eq!(G17P_PARTIAL_OPENING_OUTER_SLOT_COUNT, 0x100);
        assert_eq!(G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX, 6);
        assert_eq!(G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX, 7);
        assert_eq!(G17P_PARTIAL_OPENING_WORK_DOORBELL_CHANNEL, 0);
        assert_eq!(prepared.paired_doorbell, 0x0083_0000_0000_0000);
        assert_eq!(
            prepared.paired_doorbell,
            encode_tx_doorbell(PipeType::Vertex, 0).unwrap()
        );

        let compute = prepare_g17p_compute_channel_work(
            G17PPartialOpeningOuterChannelState {
                consumers: [0, 0],
                producer: 0,
                queue_gpu_va: 0xffff_fc20_c100_0000,
                queue_write_index: 3,
            },
            4,
            false,
        )
        .unwrap();
        assert_eq!(compute.outer.channel_table_index, 8);
        assert_eq!(compute.outer.slot_index, 0);
        assert_eq!(compute.outer.next_producer, 1);
        assert_eq!(
            u64::from_le_bytes(compute.outer.slot[8..16].try_into().unwrap()),
            0xffff_fc20_c100_0000
        );
        assert_eq!(
            u32::from_le_bytes(compute.outer.slot[0x10..0x14].try_into().unwrap()),
            2
        );
        assert_eq!(
            u32::from_le_bytes(compute.outer.slot[0x14..0x18].try_into().unwrap()),
            0x0104_0003
        );
        assert_eq!(compute.doorbell, 0x0083_0000_0000_000a);

        let compute_repeat = prepare_g17p_compute_channel_work(
            G17PPartialOpeningOuterChannelState {
                consumers: [1, 1],
                producer: 1,
                queue_gpu_va: 0xffff_fc20_c100_0000,
                queue_write_index: 7,
            },
            4,
            true,
        )
        .unwrap();
        assert_eq!(
            u32::from_le_bytes(
                compute_repeat.outer.slot[0x14..0x18]
                    .try_into()
                    .unwrap()
            ),
            0x0004_0007
        );

        let full = G17PPartialOpeningOuterChannelState {
            consumers: [0, 7],
            producer: 6,
            queue_gpu_va: 0,
            queue_write_index: 3,
        };
        assert_eq!(
            prepare_g17p_partial_opening_work_pair(full, full, 1, 0, true),
            Err(SubmissionError::RingFull)
        );
        assert_eq!(
            prepare_g17p_partial_opening_work_pair(
                G17PPartialOpeningOuterChannelState {
                    producer: 0x100,
                    ..full
                },
                full,
                1,
                0,
                true,
            ),
            Err(SubmissionError::PartialOpeningChannelCounterOutOfRange)
        );
        assert_eq!(
            prepare_g17p_partial_opening_work_pair(
                G17PPartialOpeningOuterChannelState {
                    consumers: [0, 0],
                    producer: 0,
                    queue_write_index: 0x1_0000,
                    ..full
                },
                full,
                1,
                0,
                true,
            ),
            Err(SubmissionError::PartialOpeningQueueHeadOutOfRange)
        );
    }

    #[test]
    fn g17_channel_descriptor_geometry_matches_the_firmware_walk() {
        assert_eq!(G17_CHANNEL_DESCRIPTOR_TABLE_OFFSET, 0x20);
        assert_eq!(G17_CHANNEL_DESCRIPTORS_PER_PRIORITY, 3);
        assert_eq!(G17_CHANNEL_PRIORITY_COUNT, 4);
        assert_eq!(G17_CHANNEL_DESCRIPTOR_COUNT, 12);
        assert_eq!(G17_CHANNEL_DESCRIPTOR_STRIDE, 0x20);
        assert_eq!(G17_CHANNEL_ENTRY_STRIDE, 0x18);
        assert_eq!(
            G17_CHANNEL_DESCRIPTOR_LAYOUT,
            G17ChannelDescriptorLayout {
                dispatch_cursor: 0x00,
                coherency_cursor: 0x08,
                producer_cursor: 0x10,
                entries: 0x18,
            }
        );

        for priority in 0..G17_CHANNEL_PRIORITY_COUNT {
            for (pipe, pipe_index) in [
                (PipeType::Vertex, 0u32),
                (PipeType::Fragment, 1),
                (PipeType::Compute, 2),
            ] {
                let linear = priority as u32 * 3 + pipe_index;
                assert_eq!(
                    g17_channel_descriptor_offset(priority, pipe),
                    Ok(0x20 + linear * 0x20)
                );
            }
        }

        // Descriptor eleven occupies 0x180..0x19f. Priority four must not
        // alias the independent cursor pair beginning at +0x1a0.
        assert_eq!(
            g17_channel_descriptor_offset(3, PipeType::Compute),
            Ok(0x180)
        );
        assert_eq!(
            g17_channel_descriptor_offset(4, PipeType::Vertex),
            Err(SubmissionError::ChannelPriorityOutOfRange)
        );
    }

    #[test]
    fn g17_host_channel_state_reset_matches_all_three_pinned_hosts() {
        let values = G17HostChannelStateResetValues {
            gpu_va_00: 0x0011_2233_4455_6677,
            gpu_va_08: 0x8899_aabb_ccdd_eeff,
            gpu_va_10: 0x1020_3040_5060_7080,
            word_44: 0x7654_3210,
            gpu_va_70: 0xfedc_ba98_7654_3210,
        };
        let raw = encode_g17_host_channel_state_reset(values);
        let layout = G17_HOST_CHANNEL_STATE_RESET_LAYOUT;

        assert_eq!(raw.len(), 0x80);
        assert_eq!(
            &raw[layout.gpu_va_00..layout.gpu_va_00 + 8],
            &values.gpu_va_00.to_le_bytes()
        );
        assert_eq!(
            &raw[layout.gpu_va_08..layout.gpu_va_08 + 8],
            &values.gpu_va_08.to_le_bytes()
        );
        assert_eq!(
            &raw[layout.gpu_va_10..layout.gpu_va_10 + 8],
            &values.gpu_va_10.to_le_bytes()
        );
        assert_eq!(&raw[layout.zero_18..layout.zero_18 + 4], &[0; 4]);
        assert_eq!(&raw[layout.zero_1c..layout.zero_1c + 4], &[0; 4]);
        assert_eq!(&raw[layout.zero_20..layout.zero_20 + 4], &[0; 4]);
        assert_eq!(
            &raw[layout.sentinel_24..layout.sentinel_24 + 4],
            &u32::MAX.to_le_bytes()
        );
        assert_eq!(
            &raw[layout.fixed_4_28..layout.fixed_4_28 + 4],
            &4u32.to_le_bytes()
        );
        assert_eq!(
            &raw[layout.sentinel_40..layout.sentinel_40 + 4],
            &u32::MAX.to_le_bytes()
        );
        assert_eq!(
            &raw[layout.word_44..layout.word_44 + 4],
            &values.word_44.to_le_bytes()
        );
        assert_eq!(&raw[layout.zero_58..layout.zero_58 + 4], &[0; 4]);
        assert_eq!(
            &raw[layout.gpu_va_70..layout.gpu_va_70 + 8],
            &values.gpu_va_70.to_le_bytes()
        );

        // resetChannelState clears the entire record before its scalar stores.
        // Every byte outside the nonzero runtime/fixed ranges stays zero.
        for (index, byte) in raw.iter().enumerate() {
            let nonzero_field = (0x00..0x18).contains(&index)
                || (0x24..0x2c).contains(&index)
                || (0x40..0x48).contains(&index)
                || (0x70..0x78).contains(&index);
            if !nonzero_field {
                assert_eq!(*byte, 0, "unexpected reset byte at {index:#x}");
            }
        }
        assert!(G17_HOST_CHANNEL_RESET_TARGETS_CONFIRMED);
    }

    #[test]
    fn g17_uncached_channel_reset_preserves_unwritten_bytes() {
        let mut raw = [0xa5; G17_UNCACHED_CHANNEL_RESET_WRITE_EXTENT];
        let ring_entry_count = 0x800;
        apply_g17_uncached_channel_reset(&mut raw, ring_entry_count);
        let layout = G17_UNCACHED_CHANNEL_RESET_LAYOUT;

        for offset in [
            layout.zero_00,
            layout.zero_10,
            layout.zero_20,
            layout.zero_30,
            layout.zero_40,
        ] {
            assert_eq!(&raw[offset..offset + 4], &[0; 4]);
        }
        assert_eq!(
            &raw[layout.sentinel_50..layout.sentinel_50 + 4],
            &u32::MAX.to_le_bytes()
        );
        assert_eq!(
            &raw[layout.ring_entry_count_60..layout.ring_entry_count_60 + 4],
            &ring_entry_count.to_le_bytes()
        );

        for (index, byte) in raw.iter().enumerate() {
            let written = [0x00usize, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60]
                .iter()
                .any(|offset| (*offset..*offset + 4).contains(&index));
            if !written {
                assert_eq!(*byte, 0xa5, "unproven byte changed at {index:#x}");
            }
        }
    }

    /// Native tag-14 AddKicks, PROVEN on all 25 captured publishes:
    /// `+0x00 = 0x0e`, `+0x04 u16 = qid`, `+0x06 u16 = 1` kick, `+0x08 u8 = 2`
    /// queue priority, `+0x09 u64 = entry stamp`, `+0x11 u8 = data master`,
    /// everything else zero.
    #[test]
    fn g17p_retained_event_carries_native_addkicks_bytes() {
        for (stage, qid, data_master) in [
            (G17PColdOpeningStage::Tiling, 0u16, 0u8),
            (G17PColdOpeningStage::Fragment, 1u16, 1u8),
        ] {
            let mut event = [0xa5u8; G17P_COLD_OPENING_EVENT_SIZE];
            apply_g17p_retained_event_with_stamp(stage, 0x3d9, qid, &mut event).unwrap();
            assert_eq!(get_u32_le(&event, 0x00), 0x0e);
            assert_eq!(get_u16_le(&event, 0x04), qid);
            assert_eq!(get_u16_le(&event, 0x06), 1);
            assert_eq!(event[0x08], G17P_NATIVE_RENDER_DOORBELL_PRIORITY);
            assert_eq!(
                u32::from_le_bytes([event[0x09], event[0x0a], event[0x0b], 0]),
                0x3d9
            );
            assert_eq!(event[0x11], data_master);
            for (index, byte) in event.iter().enumerate() {
                if !(0x00..0x0c).contains(&index) && index != 0x11 {
                    assert_eq!(*byte, 0, "unproven byte set at {index:#x}");
                }
            }
        }
        // The native 40-bit producer wraps to zero. Values above its width
        // must be refused before any record is modified.
        let mut event = [0u8; G17P_COLD_OPENING_EVENT_SIZE];
        assert!(
            apply_g17p_retained_event_with_stamp(G17PColdOpeningStage::Tiling, 1u64 << 40, 0, &mut event)
                .is_err()
        );
    }

    /// The render pair configures TA on data master 0 and 3D on data master 1;
    /// the compute data master must be refused on this path.
    #[test]
    fn g17p_render_queue_config_admits_only_render_data_masters() {
        let geometry = T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY;
        let scratch = g17_manager::T8140_G17P_PLATFORM_CONFIG.sksm_scratch;
        for (queue_id, data_master, entry) in [
            (
                G17P_PARTIAL_OPENING_TILING_CHANNEL_TABLE_INDEX,
                G17P_TA_DATA_MASTER,
                0x70_0043_8000u64,
            ),
            (
                G17P_PARTIAL_OPENING_FRAGMENT_CHANNEL_TABLE_INDEX,
                G17P_3D_DATA_MASTER,
                0x70_0046_0000u64,
            ),
        ] {
            let routing = G17PComputeQueueRouting {
                queue_id,
                completion_selector: 2,
                entry_gpu_va: entry,
                geometry,
            };
            let config =
                prepare_g17p_render_sksm_queue_config(routing, data_master, scratch).unwrap();
            assert_eq!(config.payload & G17P_SKSM_QUEUE_ENTRY_ADDRESS_MASK, entry);
            assert_eq!(config.payload >> 60, data_master as u64 + 1);
            assert_eq!(config.enable_pair.word1, 1u64 << queue_id);
            assert_eq!(config.enable_pair.word0, 0);
            assert!(prepare_g17p_render_sksm_queue_config(
                routing,
                G17P_COMPUTE_DATA_MASTER,
                scratch
            )
            .is_err());
        }
    }

    #[test]
    fn g17p_secondary_render_pair_matches_native_configure_words() {
        let geometry = T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY;
        let scratch = g17_manager::T8140_G17P_PLATFORM_CONFIG.sksm_scratch;
        for (queue_id, data_master, entry, tag, payload, mask) in [
            (
                2,
                G17P_TA_DATA_MASTER,
                0x70_01f4_8000,
                0x0000_1400_0002_1008,
                0x1280_0070_01f4_8000,
                0x4,
            ),
            (
                3,
                G17P_3D_DATA_MASTER,
                0x70_01f7_0000,
                0x0000_1c00_0002_1008,
                0x2280_0070_01f7_0000,
                0x8,
            ),
        ] {
            let config = prepare_g17p_render_sksm_queue_config(
                G17PComputeQueueRouting {
                    queue_id,
                    completion_selector: 0,
                    entry_gpu_va: entry,
                    geometry,
                },
                data_master,
                scratch,
            )
            .unwrap();
            assert_eq!(config.tag, tag);
            assert_eq!(config.payload, payload);
            assert_eq!(config.enable_pair.word0, 0);
            assert_eq!(config.enable_pair.word1, mask);
        }
    }

    #[test]
    fn g17p_render_config_update_carries_context_hardware_buffer_id_and_pid() {
        let addresses = G17PColdOpeningOptionalAddresses {
            context_scratch: 0x70_0200_0000,
            firmware_scratch: 0x70_0200_4000,
            usc_priv_mem_flist: 0x70_0200_8000,
            usc_freelist_hardware_buffer_id: 5,
            channel_control: 0x70_0200_c000,
            tiling_shared_object: 0x70_0201_0000,
            parameter_buffer_token: 0,
            owner_pid: 0x625,
            ta_hardware_buffer_id: 7,
        };
        for context_id in [5, 62] {
            for stage in [
                G17PColdOpeningStage::Tiling,
                G17PColdOpeningStage::Fragment,
            ] {
                let mut optional = [0u8; G17P_COLD_OPENING_OPTIONAL_SIZE];
                apply_g17p_retained_optional(
                    stage,
                    addresses,
                    0,
                    false,
                    0,
                    1,
                    5,
                    1,
                    context_id,
                    &mut optional,
                )
                .unwrap();
                assert_eq!(get_u16_le(&optional, 0x32), context_id);
                assert_eq!(get_u32_le(&optional, 0x46), 5);
                assert_eq!(get_u16_le(&optional, 0x56), 5);
                assert_eq!(get_u32_le(&optional, 0x5a), 0x625);
                assert_eq!(
                    get_u32_le(&optional, 0x7e),
                    if stage == G17PColdOpeningStage::Tiling {
                        7
                    } else {
                        u32::MAX
                    }
                );
            }
        }
    }

    #[test]
    fn g17p_retained_canonical_records_advance_logically_without_moving() {
        let addresses = G17PColdOpeningOptionalAddresses {
            context_scratch: 0x70_0200_0000,
            firmware_scratch: 0x70_0200_4000,
            usc_priv_mem_flist: 0x70_0200_8000,
            usc_freelist_hardware_buffer_id: 0,
            channel_control: 0x70_0200_c000,
            tiling_shared_object: 0x70_0201_0000,
            parameter_buffer_token: 0,
            owner_pid: 0x15,
            ta_hardware_buffer_id: 0,
        };
        for stage in [G17PColdOpeningStage::Tiling, G17PColdOpeningStage::Fragment] {
            let mut optional = [0u8; G17P_COLD_OPENING_OPTIONAL_SIZE];
            apply_g17p_retained_optional(
                stage,
                addresses,
                1,
                false,
                0,
                0,
                0,
                1,
                1,
                &mut optional,
            )
                .unwrap();
            assert_eq!(get_u16_le(&optional, 0x1a), 0);
            assert_eq!(get_u16_le(&optional, 0x2a), 1);
            assert_eq!(get_u16_le(&optional, 0x2e), 0x100);
            assert_eq!(get_u16_le(&optional, 0x3e), 1);
            assert_eq!(get_u16_le(&optional, 0x52), 0);
            assert_eq!(get_u16_le(&optional, 0x62), 0);
            assert_eq!(
                get_u16_le(&optional, 0x76),
                if stage == G17PColdOpeningStage::Tiling {
                    0
                } else {
                    u16::MAX
                }
            );

            let mut event = [0u8; G17P_COLD_OPENING_EVENT_SIZE];
            apply_g17p_retained_event(stage, 1, 0, &mut event).unwrap();
            assert_eq!(get_u32_le(&event, 0x08), 0x202);
        }

        let pair = prepare_g17p_partial_opening_work_pair(
            G17PPartialOpeningOuterChannelState {
                consumers: [1, 1],
                producer: 1,
                queue_gpu_va: 0x70_0300_00c0,
                queue_write_index: 3,
            },
            G17PPartialOpeningOuterChannelState {
                consumers: [1, 1],
                producer: 1,
                queue_gpu_va: 0x70_0300_0000,
                queue_write_index: 3,
            },
            1,
            0,
            false,
        )
        .unwrap();
        assert_eq!(pair.fragment.slot_index, 1);
        assert_eq!(pair.tiling.slot_index, 1);
        assert_eq!(pair.fragment.next_producer, 2);
        assert_eq!(pair.tiling.next_producer, 2);
        assert_eq!(get_u32_le(&pair.fragment.slot, 0x14), 0x0001_0003);
        assert_eq!(get_u32_le(&pair.tiling.slot, 0x14), 0x0000_0003);
    }

    #[test]
    fn g17p_context_update_is_fresh_graph_state_not_global_ordinal_state() {
        let addresses = G17PColdOpeningOptionalAddresses {
            context_scratch: 0x70_0200_0000,
            firmware_scratch: 0x70_0200_4000,
            usc_priv_mem_flist: 0x70_0200_8000,
            usc_freelist_hardware_buffer_id: 0,
            channel_control: 0x70_0200_c000,
            tiling_shared_object: 0x70_0201_0000,
            parameter_buffer_token: 0,
            owner_pid: 0x15,
            ta_hardware_buffer_id: 0,
        };
        for stage in [G17PColdOpeningStage::Tiling, G17PColdOpeningStage::Fragment] {
            for (ordinal, context_update) in [(0, true), (195, true), (196, false)] {
                let mut optional = [0u8; G17P_COLD_OPENING_OPTIONAL_SIZE];
                apply_g17p_retained_optional_with_flist_generation_and_context_update(
                    stage, addresses, ordinal, 2022, false, 2, 3, 2, 0, 6,
                    context_update, &mut optional,
                )
                .unwrap();
                assert_eq!(get_u16_le(&optional, 0x1a), 0);
                assert_eq!(get_u16_le(&optional, 0x52), if ordinal == 0 { 1 } else { 0 });
                assert_eq!(get_u16_le(&optional, 0x62), u16::from(context_update));
                assert_eq!(get_u16_le(&optional, 0x32), 6);
            }
        }
    }

    #[test]
    fn ring_index_wraps_at_size() {
        assert_eq!(ring_next(0, 4), Ok(1));
        assert_eq!(ring_next(3, 4), Ok(0));
        assert_eq!(ring_next(0x4ff, 0x500), Ok(0));
        assert_eq!(ring_next(0, 0), Err(SubmissionError::RingSizeZero));
        assert_eq!(ring_next(4, 4), Err(SubmissionError::RingIndexOutOfRange));
    }

    #[test]
    fn kick_word_fields_match_the_firmware_extraction() {
        // and x0,x5,#0xffffffffff ; ubfx x1,x5,#40,#7 ; ubfx x6,x5,#46,#1.
        assert_eq!(KICK_TS_BITS, 40);
        assert_eq!(KICK_TS_MASK, 0xff_ffff_ffff);
        assert_eq!(KICK_QID_SHIFT, 40);
        assert_eq!(KICK_QID_MASK, 0x7f);
        assert_eq!(KICK_SELECTOR_BIT, 46);

        // Compose the exact word consumed from the firmware scratch FIFO;
        // QID bit 6 is the mask selector.
        let ts = 0x89_abcd_ef42u64;
        let word = encode_kick_word(ts, 0x7f).unwrap();
        assert_eq!(
            decode_kick_word(word),
            Ok(SksmKickWord {
                timestamp: ts,
                queue_id: 0x7f,
                mask_selector: 1,
            }),
            1,
            0
        );
        // A word with bit 47 set is outside the modeled form.
        assert_eq!(
            decode_kick_word(1 << 47),
            Err(SubmissionError::KickWordUnmodeledBits)
        );
        assert_eq!(
            encode_kick_word(KICK_TS_MASK + 1, 0),
            Err(SubmissionError::KickTimestampOutOfRange)
        );
        assert_eq!(
            encode_kick_word(0, 0x80),
            Err(SubmissionError::QueueIdOutOfRange)
        );
    }

    #[test]
    fn g17p_3d_completion_scratch_matches_native_full_command() {
        assert_eq!(
            encode_g17p_3d_completion_scratch(
                0xf76,
                1,
            ),
            Ok(0x0000_0100_0000_0f76),
        );
        assert_eq!(
            encode_g17p_3d_completion_scratch(
                KICK_TS_MASK + 1,
                1,
            ),
            Err(SubmissionError::KickTimestampOutOfRange),
        );
    }

    #[test]
    fn g17p_serialized_render_scratch_payload_advances_and_wraps_independently() {
        for (ordinal, timestamp, expected) in [
            (0, 1, 0x0000_0100_0000_0001),
            (1, 2, 0x0000_0200_0000_0002),
            (2, 3, 0x0000_0300_0000_0003),
            (254, 255, 0x0000_ff00_0000_00ff),
            (255, 256, 0x0000_0000_0000_0100),
            (256, 257, 0x0000_0100_0000_0101),
            // A channel payload is not inferred from the physical queue's
            // slot or timestamp: distinct valid values must remain distinct.
            (1, 0x1234_5678_9a, 0x0000_0212_3456_789a),
        ] {
            assert_eq!(
                encode_g17p_3d_completion_scratch(
                    timestamp, g17p_serialized_render_scratch_payload(ordinal),
                ),
                Ok(expected),
            );
        }
        assert_eq!(g17p_serialized_render_scratch_payload(u64::from(u32::MAX)), 0);
    }

    #[test]
    fn kick_queue_preparation_couples_slot_queue_and_next_timestamp() {
        let timestamp = encode_kick_timestamp(KickTimestamp {
            stamp: 0x1234_5678,
            stamp_index: 7,
        });
        let prepared = prepare_kick_submission(timestamp, 0x2a, 16, 0x180).unwrap();

        assert_eq!(prepared.entry_index, 7);
        assert_eq!(prepared.entry_offset, 7 * 0x180);
        assert_eq!(prepared.timestamp, 0x12_3456_7807);
        assert_eq!(prepared.queue_id, 0x2a);
        assert_eq!(
            decode_kick_timestamp(prepared.next_timestamp),
            Ok(KickTimestamp {
                stamp: 0x1234_5678,
                stamp_index: 8,
            })
        );
    }

    #[test]
    fn kick_queue_timestamp_wrap_matches_the_host_divide_remainder_sequence() {
        // Slot 15 wraps modulo 16, so the host adds 0x100 to the timestamp:
        // the low byte becomes zero and the 32-bit stamp increments.
        let timestamp = encode_kick_timestamp(KickTimestamp {
            stamp: 0x1234_5678,
            stamp_index: 15,
        });
        let prepared = prepare_kick_submission(timestamp, 0x7f, 16, 0x180).unwrap();
        assert_eq!(prepared.entry_index, 15);
        assert_eq!(prepared.entry_offset, 15 * 0x180);
        assert_eq!(
            decode_kick_timestamp(prepared.next_timestamp),
            Ok(KickTimestamp {
                stamp: 0x1234_5679,
                stamp_index: 0,
            })
        );

        // A 256-slot queue is representable despite num_stamps being larger
        // than u8; slot 255 wraps and the 32-bit stamp wraps exactly like the
        // host's masked add.
        let timestamp = encode_kick_timestamp(KickTimestamp {
            stamp: u32::MAX,
            stamp_index: 0xff,
        });
        let prepared = prepare_kick_submission(timestamp, 0x40, 256, 0x180).unwrap();
        assert_eq!(prepared.entry_index, 0xff);
        assert_eq!(
            decode_kick_timestamp(prepared.next_timestamp),
            Ok(KickTimestamp {
                stamp: 0,
                stamp_index: 0,
            })
        );
    }

    #[test]
    fn g17p_hal200_queue_geometry_is_exact_and_other_backends_stay_unconfirmed() {
        let geometry = T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY;
        assert_eq!(geometry.validate(), Ok(()));
        assert_eq!(geometry.allocation_alignment(), 0x100);
        assert_eq!(geometry.entry_stride(), 0x200);
        assert_eq!(geometry.stamp_count(), 0x100);
        assert_eq!(geometry.timestamp_seed(), 1);
        assert_eq!(geometry.backing_size(), 0x20000);
        assert_eq!(
            geometry.entry_stride() as usize * geometry.stamp_count() as usize,
            geometry.backing_size()
        );
        assert_eq!(
            g17_sksm_queue_geometry(
                G17ChannelAbiTarget::A18ProG17P,
                G17ClSubmissionBackend::G17PHal200Hybrid,
            ),
            Some(geometry)
        );
        for backend in [
            G17ClSubmissionBackend::Classic,
            G17ClSubmissionBackend::Hal300,
        ] {
            assert_eq!(
                g17_sksm_queue_geometry(G17ChannelAbiTarget::A18ProG17P, backend),
                None
            );
        }
        for target in [
            G17ChannelAbiTarget::M5G17G,
            G17ChannelAbiTarget::M5ProMaxG17X,
        ] {
            assert_eq!(
                g17_sksm_queue_geometry(target, G17ClSubmissionBackend::G17PHal200Hybrid),
                None
            );
        }

        let producer = geometry
            .initial_producer_state(
                4,
                2,
                G17PClQosConfig {
                    queue_byte_32: 1,
                    class_40: 8,
                    word_48: 0xffff,
                },
            )
            .unwrap();
        assert_eq!(producer.geometry, geometry);
        assert_eq!(producer.submitted, 0);
        assert_eq!(producer.current_timestamp, geometry.timestamp_seed());
        assert_eq!(producer.last_add_kicks_timestamp, geometry.timestamp_seed());
        assert_eq!(producer.base_timestamp, geometry.timestamp_seed());
        assert!(!producer.previous_valid);
    }

    #[test]
    fn g17p_hal200_queue_geometry_drives_last_slot_and_wrap() {
        let geometry = T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY;
        let timestamp = encode_kick_timestamp(KickTimestamp {
            stamp: 0x1234_5678,
            stamp_index: (geometry.stamp_count() - 1) as u8,
        });
        let prepared = prepare_kick_submission(
            timestamp,
            4,
            geometry.stamp_count(),
            geometry.entry_stride(),
        )
        .unwrap();

        assert_eq!(prepared.entry_index, 0xff);
        assert_eq!(
            prepared.entry_offset as usize + geometry.entry_stride() as usize,
            geometry.backing_size()
        );
        assert_eq!(
            decode_kick_timestamp(prepared.next_timestamp),
            Ok(KickTimestamp {
                stamp: 0x1234_5679,
                stamp_index: 0,
            })
        );
    }

    #[test]
    fn kick_queue_preparation_matches_all_runtime_stamp_geometries() {
        for num_stamps in 1u16..=KICK_NUM_STAMPS_MAX {
            for index in 0u16..num_stamps {
                let current = KickTimestamp {
                    stamp: 0x89ab_cdef,
                    stamp_index: index as u8,
                };
                let timestamp = encode_kick_timestamp(current);
                let prepared = prepare_kick_submission(timestamp, 3, num_stamps, 0x180).unwrap();
                let expected_index = (index + 1) % num_stamps;
                let expected_stamp = if expected_index == 0 {
                    current.stamp.wrapping_add(1)
                } else {
                    current.stamp
                };

                assert_eq!(prepared.entry_index, index as u8);
                assert_eq!(prepared.entry_offset, index as u32 * 0x180);
                assert_eq!(prepared.timestamp, timestamp);
                assert_eq!(
                    decode_kick_timestamp(prepared.next_timestamp),
                    Ok(KickTimestamp {
                        stamp: expected_stamp,
                        stamp_index: expected_index as u8,
                    })
                );
            }
        }
    }

    #[test]
    fn kick_queue_preparation_reports_unrepresentable_state_before_publication() {
        assert_eq!(
            prepare_kick_submission(1 << 40, 0, 1, 0x180),
            Err(SubmissionError::KickTimestampOutOfRange)
        );
        assert_eq!(
            prepare_kick_submission(0, 0, 0, 0x180),
            Err(SubmissionError::KickStampCountOutOfRange)
        );
        assert_eq!(
            prepare_kick_submission(0, 0, 257, 0x180),
            Err(SubmissionError::KickStampCountOutOfRange)
        );
        assert_eq!(
            prepare_kick_submission(4, 0, 4, 0x180),
            Err(SubmissionError::KickStampIndexOutOfRange)
        );
        assert_eq!(
            prepare_kick_submission(0, 0, 1, 0),
            Err(SubmissionError::KickEntryStrideZero)
        );
        assert_eq!(
            prepare_kick_submission(2, 0, 3, u32::MAX),
            Err(SubmissionError::KickEntryOffsetOverflow)
        );
        assert_eq!(
            prepare_kick_submission(0, 0x80, 1, 0x180),
            Err(SubmissionError::QueueIdOutOfRange)
        );
    }

    #[test]
    fn g17p_cl_kick_layout_and_publication_order_are_exact() {
        assert_eq!(G17P_CL_KICK_ENTRY_SIZE, 0x180);
        assert_eq!(G17P_CL_DEPENDENCIES_MAX, 32);
        assert_eq!(G17P_CL_EXPLICIT_DEPENDENCIES_MAX, 31);
        assert_eq!(
            G17P_CL_KICK_QUEUE_STATE_LAYOUT,
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
            }
        );
        assert_eq!(
            G17P_CL_KICK_ENTRY_LAYOUT,
            G17PClKickEntryLayout {
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
            }
        );
        assert_eq!(
            G17P_CL_KICK_PUBLICATION_ORDER,
            [
                G17PClKickPublicationStep::ZeroEntryStride,
                G17PClKickPublicationStep::EntryBody,
                G17PClKickPublicationStep::InnerShareableBarrier,
                G17PClKickPublicationStep::NextTimestamp,
                G17PClKickPublicationStep::SubmittedAtomicAdd,
                G17PClKickPublicationStep::PreviousValid,
                G17PClKickPublicationStep::PreviousTimestamp,
            ]
        );
        assert_eq!(
            G17P_FIRST_SUBMIT_PUBLICATION_ORDER,
            [
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
            ]
        );
    }

    #[test]
    fn g17p_conditional_transport_events_match_the_independent_fixtures() {
        const PRIMARY: &[u8; G17P_COMPUTE_PRIMARY_EVENT_SIZE] =
            include_bytes!("fixtures/g17p_first_submit/channel-event-primary.bin");
        const OPTIONAL: &[u8; G17P_COMPUTE_OPTIONAL_EVENT_SIZE] =
            include_bytes!("fixtures/g17p_first_submit/channel-event-optional.bin");

        let primary = prepare_g17p_compute_primary_event(0, 0, 4, 2, 1).unwrap();
        assert_eq!(primary.descriptor_generation, 1);
        assert_eq!(primary.bytes, *PRIMARY);
        assert_eq!(advance_g17p_compute_descriptor_generation(0, 0), Ok(1));
        assert_eq!(advance_g17p_compute_descriptor_generation(2, 3), Ok(6));

        let mut optional = [0u8; G17P_COMPUTE_OPTIONAL_EVENT_SIZE];
        encode_g17p_compute_optional_event(
            G17PComputeOptionalEvent {
                queue_id: 4,
                old_timestamp: 1,
            },
            &mut optional,
        )
        .unwrap();
        assert_eq!(optional, *OPTIONAL);

        assert_eq!(
            prepare_g17p_compute_primary_event(u32::MAX, 0, 4, 2, 1),
            Err(SubmissionError::ComputeEventCounterOverflow)
        );
        assert_eq!(
            prepare_g17p_compute_primary_event(u16::MAX as u32, 0, 4, 2, 1),
            Err(SubmissionError::ComputeEventCounterOverflow)
        );

        // The tag-14 transport builder reproduces the same captured record.
        // mac17p 0xfffffe00089fa39c writes type 14, QID 4, one kick, the queue
        // QoS priority 2, the pre-increment entry stamp 1, and data master 2.
        let add_kicks = prepare_g17p_ksm_add_kicks_command(G17PKsmAddKicksCommand {
            queue_id: 4,
            add_count: 1,
            queue_priority: 2,
            entry_stamp: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.timestamp_seed(),
        })
        .unwrap();
        assert_eq!(add_kicks.bytes, *PRIMARY);
        assert_eq!(add_kicks.bytes.len(), G17P_KSM_ADD_KICKS_COMMAND_SIZE);
        assert!(G17P_KSM_ADD_KICKS_COMMAND_SIZE <= G17P_KSM_ADD_KICKS_COMMAND_FETCH_SIZE);
        assert_eq!(add_kicks.bytes[0x08], 2);
        assert_eq!(
            u32::from_le_bytes(add_kicks.bytes[0x11..0x15].try_into().unwrap()),
            G17P_COMPUTE_DATA_MASTER
        );

        assert_eq!(
            prepare_g17p_ksm_add_kicks_command(G17PKsmAddKicksCommand {
                queue_id: 4,
                add_count: 0,
                queue_priority: 2,
                entry_stamp: 1,
            }),
            Err(SubmissionError::KsmAddKicksCountZero)
        );
        assert_eq!(
            prepare_g17p_ksm_add_kicks_command(G17PKsmAddKicksCommand {
                queue_id: KICK_QID_MASK + 1,
                add_count: 1,
                queue_priority: 2,
                entry_stamp: 1,
            }),
            Err(SubmissionError::QueueIdOutOfRange)
        );
        assert_eq!(
            prepare_g17p_ksm_add_kicks_command(G17PKsmAddKicksCommand {
                queue_id: 4,
                add_count: 1,
                queue_priority: 4,
                entry_stamp: 1,
            }),
            Err(SubmissionError::KickQueueCompletionSelectorOutOfRange)
        );
        assert_eq!(
            prepare_g17p_ksm_add_kicks_command(G17PKsmAddKicksCommand {
                queue_id: 4,
                add_count: 1,
                queue_priority: 2,
                entry_stamp: 1 << KICK_TS_BITS,
            }),
            Err(SubmissionError::KickTimestampOutOfRange)
        );
        assert_eq!(
            prepare_g17p_ksm_add_kicks_command(G17PKsmAddKicksCommand {
                queue_id: 4,
                add_count: u16::MAX as u32 + 1,
                queue_priority: 2,
                entry_stamp: 1,
            }),
            Err(SubmissionError::KsmAddKicksCountOutOfRange)
        );
    }

    #[test]
    fn g17p_cl_hardware_positive_retained_items_are_exact() {
        const QUEUE: u64 = 0xffff_fc20_c000_0300;
        const DESCRIPTOR: u64 = 0xffff_fc20_c035_8000;
        const DESCRIPTOR_STRIDE: u64 = 0x1040;
        let item0_queue = G17PClKickProducerState {
            queue_id: 4,
            geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            completion_selector: 2,
            qos: G17PClQosConfig {
                queue_byte_32: 1,
                class_40: 8,
                word_48: 0xffff,
            },
            submitted: 0,
            current_timestamp: 1,
            last_add_kicks_timestamp: 1,
            base_timestamp: 1,
            previous_valid: false,
            previous_timestamp: 0,
        };
        let item0 = prepare_g17p_cl_kick_entry(
            item0_queue,
            native_add3_compute_operands(
                DESCRIPTOR,
                QUEUE,
                1,
                [0, 0, 4, 0],
                [0x0001_1003_8001_a002, 0x0000_2003_8001_a03b],
            ),
        )
        .unwrap();
        let mut expected0 = [0u8; G17P_CL_KICK_ENTRY_SIZE];
        for (offset, value) in [
            (0x00, 0x1000_1000_0000_0004),
            (0x10, DESCRIPTOR),
            (0x18, QUEUE),
            (0x20, 0xffff_0801_0000_0001),
            (0x28, 0x0000_0400_0000_0000),
            (0x138, 4),
            (0x150, 0x0001_1003_8001_a002),
            (0x158, 0x0000_2003_8001_a03b),
            (0x178, 0x003f_ffff_ffff_ffff),
        ] {
            put_u64_le(&mut expected0, offset, value);
        }
        assert_eq!(item0.entry, expected0);
        assert_eq!(item0.zero_length, 0x200);
        assert_eq!(item0.entry_index, 1);
        assert_eq!(item0.entry_offset, 0x200);
        assert_eq!(item0.current_timestamp, 1);
        assert_eq!(item0.next_timestamp, 2);
        assert_eq!(item0.submitted_before, 0);
        assert_eq!(item0.submitted_after, 1);
        assert_eq!(item0.previous_timestamp_after, 1);
        assert_eq!(item0.producer_after.current_timestamp, 2);
        assert_eq!(item0.producer_after.submitted, 1);
        assert!(item0.producer_after.previous_valid);
        assert_eq!(item0.producer_after.previous_timestamp, 1);
        assert_eq!(item0.producer_after.qos, item0_queue.qos);

        let item1 = prepare_g17p_cl_kick_entry(
            item0.producer_after,
            native_add3_compute_operands(
                DESCRIPTOR + DESCRIPTOR_STRIDE,
                QUEUE,
                0,
                [0, 2, 8, 0],
                [0x0001_1003_8001_a084, 0x0000_2003_8001_a0bd],
            ),
        )
        .unwrap();
        let mut expected1 = [0u8; G17P_CL_KICK_ENTRY_SIZE];
        for (offset, value) in [
            (0x00, 0x1000_1000_0000_0008),
            (0x10, DESCRIPTOR + DESCRIPTOR_STRIDE),
            (0x18, QUEUE),
            (0x20, 0xffff_0801_0000_0001),
            (0x28, 0x0000_0400_0000_0001),
            (0x130, 2),
            (0x138, 8),
            (0x150, 0x0001_1003_8001_a084),
            (0x158, 0x0000_2003_8001_a0bd),
            (0x178, 0x003f_ffff_ffff_ffff),
        ] {
            put_u64_le(&mut expected1, offset, value);
        }
        assert_eq!(item1.entry, expected1);
        assert_eq!(item1.zero_length, 0x200);
        assert_eq!(item1.entry_index, 2);
        assert_eq!(item1.entry_offset, 0x400);
        assert_eq!(item1.current_timestamp, 2);
        assert_eq!(item1.next_timestamp, 3);
        assert_eq!(item1.submitted_before, 1);
        assert_eq!(item1.submitted_after, 2);
        assert_eq!(item1.previous_timestamp_after, 2);
        assert_eq!(item1.producer_after.current_timestamp, 3);
        assert_eq!(item1.producer_after.submitted, 2);
        assert_eq!(item1.producer_after.previous_timestamp, 2);
        assert_eq!(item1.producer_after.qos, item0_queue.qos);

        // Command two keeps physical queue 0's QoS word while advancing its
        // command-owned event mask from 4 to 8.
        assert_eq!(get_u64_le(&item1.entry, 0x00), 0x1000_1000_0000_0008);
        assert_eq!(get_u64_le(&item1.entry, 0x28), 0x0000_0400_0000_0001);
        assert_eq!(
            get_u64_le(&item1.entry, 0x20),
            get_u64_le(&item0.entry, 0x20)
        );
        assert_eq!(get_u64_le(&item0.entry, 0x138), 4);
        assert_eq!(get_u64_le(&item1.entry, 0x138), 8);
        assert_eq!(
            get_u64_le(&item1.entry, 0x178),
            get_u64_le(&item0.entry, 0x178)
        );
    }

    #[test]
    fn g17p_first_submit_matches_the_independent_full_stride_fixture() {
        const BEFORE: &[u8; 0x200] =
            include_bytes!("fixtures/g17p_first_submit/sksm-entry-before.bin");
        const AFTER: &[u8; 0x200] =
            include_bytes!("fixtures/g17p_first_submit/sksm-entry-after.bin");
        const RELOCATION_MASK: &[u8; 0x200] =
            include_bytes!("fixtures/g17p_first_submit/sksm-entry-relocation-mask.bin");
        const DESCRIPTOR_HIGH: u64 = 0xffff_fc20_c035_8000;
        const DESCRIPTOR_LOW: u64 = 0x70_0034_0000;
        const QUEUE: u64 = 0xffff_fc20_c000_0300;

        assert!(BEFORE.iter().all(|byte| *byte == 0));
        assert_eq!(G17P_FIRST_BIND_COMMAND_PLAN.fallback_logical_flush_id, 0);
        assert!(!G17P_FIRST_BIND_COMMAND_PLAN.flush_id_serialized_in_tag15);
        assert!(!G17P_FIRST_BIND_COMMAND_PLAN.flush_id_completion_release_required);
        assert_eq!(
            RELOCATION_MASK
                .iter()
                .map(|byte| byte.count_ones())
                .sum::<u32>(),
            204
        );

        let command = prepare_g17p_compute_cl_command_state(DESCRIPTOR_LOW, 0, 0x26).unwrap();
        let empty = G17PClRceBinding { address: 0, tag: 0 };
        assert_eq!(
            command.rce_bindings,
            [
                G17PClRceBinding {
                    address: DESCRIPTOR_LOW + 0x40,
                    tag: 0x26,
                },
                G17PClRceBinding {
                    address: DESCRIPTOR_LOW + 0x760,
                    tag: 0x04,
                },
                empty,
                empty,
            ]
        );
        let before = G17PClKickProducerState {
            queue_id: 4,
            geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            completion_selector: 2,
            qos: G17PClQosConfig {
                queue_byte_32: 1,
                class_40: 8,
                word_48: 0xffff,
            },
            submitted: 0,
            current_timestamp: 1,
            last_add_kicks_timestamp: 1,
            base_timestamp: 1,
            previous_valid: false,
            previous_timestamp: 0,
        };
        let prepared = prepare_g17p_cl_kick_entry(
            before,
            G17PClKickEntryOperands {
                descriptor_flag_4c: false,
                descriptor_flag_5c8: false,
                converted_command_timestamp: 1,
                barriers: &[],
                mcache: None,
                payload: [DESCRIPTOR_HIGH, QUEUE],
                event_mask: command.event_mask,
                rce_kind: 0,
                rce_bindings: command.rce_bindings,
                auxiliary: 0x003f_ffff_ffff_ffff,
            },
        )
        .unwrap();
        let mut actual = [0u8; 0x200];
        actual[..G17P_CL_KICK_ENTRY_SIZE].copy_from_slice(&prepared.entry);

        for index in 0..actual.len() {
            assert_eq!(
                actual[index] & !RELOCATION_MASK[index],
                AFTER[index] & !RELOCATION_MASK[index],
                "normalized fixture mismatch at offset {:#x}",
                index
            );
        }
        // The fixture uses the same captured addresses as this test, so the
        // relocation-bearing fields must also agree without normalization.
        assert_eq!(actual, *AFTER);
        assert_eq!(prepared.zero_length, 0x200);
        assert_eq!(prepared.entry_index, 1);
        assert_eq!(prepared.entry_offset, 0x200);
        assert_eq!(prepared.current_timestamp, 1);
        assert_eq!(prepared.next_timestamp, 2);
        assert_eq!(prepared.submitted_before, 0);
        assert_eq!(prepared.submitted_after, 1);
        assert_eq!(
            prepared.producer_after,
            G17PClKickProducerState {
                submitted: 1,
                current_timestamp: 2,
                previous_valid: true,
                previous_timestamp: 1,
                ..before
            }
        );

        const SECOND_AFTER: &[u8; 0x200] =
            include_bytes!("fixtures/g17p_first_submit/sksm-entry-second-after.bin");
        let second_command =
            prepare_g17p_compute_cl_command_state(DESCRIPTOR_LOW + 0x1040, 1, 0x26).unwrap();
        let second = prepare_g17p_cl_kick_entry(
            prepared.producer_after,
            G17PClKickEntryOperands {
                descriptor_flag_4c: false,
                descriptor_flag_5c8: false,
                converted_command_timestamp: 0,
                barriers: &[],
                mcache: None,
                payload: [DESCRIPTOR_HIGH + 0x1040, QUEUE],
                event_mask: second_command.event_mask,
                rce_kind: 0,
                rce_bindings: second_command.rce_bindings,
                auxiliary: 0x003f_ffff_ffff_ffff,
            },
        )
        .unwrap();
        let mut second_actual = [0u8; 0x200];
        second_actual[..G17P_CL_KICK_ENTRY_SIZE].copy_from_slice(&second.entry);
        assert_eq!(second_actual, *SECOND_AFTER);
        assert_eq!(second.entry_index, 2);
        assert_eq!(second.entry_offset, 0x400);
        assert_eq!(second.current_timestamp, 2);
        assert_eq!(second.next_timestamp, 3);
        assert_eq!(second.submitted_before, 1);
        assert_eq!(second.submitted_after, 2);
        assert_eq!(second.producer_after.current_timestamp, 3);
        assert_eq!(second.producer_after.previous_timestamp, 2);
        assert_eq!(second.producer_after.last_add_kicks_timestamp, 1);
    }

    #[test]
    fn g17p_cl_descriptor_flags_map_directly_to_header_bits() {
        let empty = G17PClRceBinding { address: 0, tag: 0 };
        let producer = G17PClKickProducerState {
            queue_id: 4,
            geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            completion_selector: 2,
            qos: G17PClQosConfig {
                queue_byte_32: 1,
                class_40: 8,
                word_48: 0xffff,
            },
            submitted: 0,
            current_timestamp: 1,
            last_add_kicks_timestamp: 1,
            base_timestamp: 1,
            previous_valid: false,
            previous_timestamp: 0,
        };
        let flag_mask = (1u64 << 1) | (1u64 << 61);

        for (flag_4c, flag_5c8, expected) in [
            (false, false, 0),
            (true, false, 1u64 << 1),
            (false, true, 1u64 << 61),
            (true, true, flag_mask),
        ] {
            let prepared = prepare_g17p_cl_kick_entry(
                producer,
                G17PClKickEntryOperands {
                    descriptor_flag_4c: flag_4c,
                    descriptor_flag_5c8: flag_5c8,
                    converted_command_timestamp: 1,
                    barriers: &[],
                    mcache: None,
                    payload: [0; 2],
                    event_mask: [0; 4],
                    rce_kind: 0,
                    rce_bindings: [empty; 4],
                    auxiliary: 0,
                },
            )
            .unwrap();

            assert_eq!(get_u64_le(&prepared.entry, 0) & flag_mask, expected);
        }
    }

    #[test]
    fn g17p_cl_completion_updates_only_the_submitted_count() {
        let producer = G17PClKickProducerState {
            queue_id: 4,
            geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            completion_selector: 2,
            qos: G17PClQosConfig {
                queue_byte_32: 1,
                class_40: 8,
                word_48: 0xffff,
            },
            submitted: 2,
            current_timestamp: 3,
            last_add_kicks_timestamp: 2,
            base_timestamp: 1,
            previous_valid: true,
            previous_timestamp: 2,
        };

        assert_eq!(complete_g17p_cl_kicks(producer, 0), Ok(producer));
        let after_one = complete_g17p_cl_kicks(producer, 1).unwrap();
        assert_eq!(
            after_one,
            G17PClKickProducerState {
                submitted: 1,
                ..producer
            }
        );
        let empty = complete_g17p_cl_kicks(producer, 2).unwrap();
        assert_eq!(empty.submitted, 0);
        assert_eq!(complete_g17p_cl_kicks(empty, 0), Ok(empty));
        assert_eq!(
            complete_g17p_cl_kicks(empty, 1),
            Err(SubmissionError::KickCompletionUnderflow)
        );
        assert_eq!(
            complete_g17p_cl_kicks(producer, 3),
            Err(SubmissionError::KickCompletionUnderflow)
        );
    }

    #[test]
    fn g17p_cl_retained_storage_overlap_and_order_are_exact() {
        let layout = G17P_CL_RETAINED_STORAGE_LAYOUT;
        assert_eq!(
            layout,
            G17PClRetainedStorageLayout {
                shared_support: G17PClStorageRange {
                    offset: 0,
                    size: 0x4000,
                },
                scheduler: G17PClStorageRange {
                    offset: 0x200,
                    size: 0x100,
                },
            }
        );
        assert_eq!(
            layout.scheduler.offset - layout.shared_support.offset,
            0x200
        );
        assert!(g17p_cl_range_contains(
            layout.shared_support,
            layout.scheduler
        ));
        assert!(g17p_cl_ranges_overlap(
            layout.shared_support,
            layout.scheduler
        ));
        assert!(g17p_cl_full_support_initialization_allowed(false));
        assert!(!g17p_cl_full_support_initialization_allowed(true));

        assert_eq!(
            G17P_CL_RETAINED_REGISTRATION_ORDER,
            [
                G17PClRetainedPublicationStep::InitializeSharedSupport,
                G17PClRetainedPublicationStep::WriteSchedulerRecord,
                G17PClRetainedPublicationStep::RegisterPhysicalQueue,
            ]
        );
        assert_eq!(
            G17P_CL_RETAINED_ACTIVATION_ORDER,
            [
                G17PClRetainedPublicationStep::PublishCommandOwnedState,
                G17PClRetainedPublicationStep::CleanCommandOwnedState,
                G17PClRetainedPublicationStep::WriteB2PrimaryState,
                G17PClRetainedPublicationStep::CleanB2PrimaryState,
                G17PClRetainedPublicationStep::SystemBarrierBeforeInnerPayload,
                G17PClRetainedPublicationStep::PublishInnerQueuePayload,
                G17PClRetainedPublicationStep::SystemBarrierBeforeInnerProducer,
                G17PClRetainedPublicationStep::PublishInnerQueueProducer,
                G17PClRetainedPublicationStep::SystemBarrierAfterInnerProducer,
            ]
        );
        assert_eq!(
            G17P_CL_RETAINED_SUBMISSION_ORDER,
            [
                G17PClRetainedPublicationStep::PublishCommandOwnedState,
                G17PClRetainedPublicationStep::CleanCommandOwnedState,
                G17PClRetainedPublicationStep::PublishInnerQueuePayload,
                G17PClRetainedPublicationStep::SystemBarrierBeforeInnerProducer,
                G17PClRetainedPublicationStep::PublishInnerQueueProducer,
                G17PClRetainedPublicationStep::SystemBarrierAfterInnerProducer,
            ]
        );
        assert_eq!(
            g17p_cl_retained_submission_order(G17PClB2ActivationState::Pending),
            G17P_CL_RETAINED_ACTIVATION_ORDER
        );
        assert_eq!(
            g17p_cl_retained_submission_order(G17PClB2ActivationState::Published),
            G17P_CL_RETAINED_SUBMISSION_ORDER
        );
    }

    #[test]
    fn g17p_cl_b2_primary_state_builder_is_byte_exact() {
        const FILL: u8 = 0xa5;
        const CHANNEL_CONTROL: u64 = 0xffff_fc20_1234_8000;
        let layout = G17P_CL_B2_PRIMARY_STATE_LAYOUT;
        assert_eq!(
            layout,
            G17PClB2PrimaryStateLayout {
                object_size: 0x4000,
                computed_write_extent: 0xe30,
                region_1_write_extent: 0x20,
                region_2_write_extent: 0x18,
            }
        );

        let mut computed = [FILL; 0x4000];
        let mut region_1 = [FILL; 0x4000];
        let mut region_2 = [FILL; 0x4000];
        let activation = apply_g17p_cl_b2_primary_state(
            G17PClB2ActivationState::Pending,
            &mut computed,
            &mut region_1,
            &mut region_2,
            CHANNEL_CONTROL,
        )
        .unwrap();
        assert_eq!(activation, G17PClB2ActivationState::Published);

        let mut expected_computed = [FILL; 0x4000];
        expected_computed[0x000] = 0xff;
        expected_computed[0x008] = 0xff;
        expected_computed[0x010..0x012].copy_from_slice(&0x0f01u16.to_le_bytes());
        expected_computed[0x018..0x01a].copy_from_slice(&0x0f01u16.to_le_bytes());
        for (offset, value) in [(0x408, 2), (0x40c, 2), (0x604, 4), (0xc04, 0x1000)] {
            put_u32_le(&mut expected_computed, offset, value);
        }
        for (offset, value) in [
            (0x808, CHANNEL_CONTROL),
            (0xe00, 7),
            (0xe08, 0x0000_0001_0e22_618a),
            (0xe20, 4),
            (0xe28, 4),
        ] {
            put_u64_le(&mut expected_computed, offset, value);
        }

        let mut expected_region_1 = [FILL; 0x4000];
        put_u64_le(&mut expected_region_1, 0x10, 0x0000_0080_0007_7000);
        put_u32_le(&mut expected_region_1, 0x18, 0x17);
        put_u32_le(&mut expected_region_1, 0x1c, 0x17);

        let mut expected_region_2 = [FILL; 0x4000];
        put_u64_le(&mut expected_region_2, 0x00, 0x0800_0000_e000_0000);
        put_u64_le(&mut expected_region_2, 0x08, 0x0000_3d40_0000_3400);
        put_u64_le(&mut expected_region_2, 0x10, 0x0000_0000_0000_1d00);

        assert_eq!(computed, expected_computed);
        assert_eq!(region_1, expected_region_1);
        assert_eq!(region_2, expected_region_2);
        assert_eq!(
            apply_g17p_cl_b2_primary_state(
                activation,
                &mut computed,
                &mut region_1,
                &mut region_2,
                CHANNEL_CONTROL,
            ),
            Err(SubmissionError::KickB2PrimaryStateAlreadyPublished)
        );
        assert_eq!(computed, expected_computed);
        assert_eq!(region_1, expected_region_1);
        assert_eq!(region_2, expected_region_2);

        let mut short = [0u8; 1];
        assert_eq!(
            apply_g17p_cl_b2_primary_state(
                G17PClB2ActivationState::Pending,
                &mut short,
                &mut region_1,
                &mut region_2,
                CHANNEL_CONTROL,
            ),
            Err(SubmissionError::KickB2PrimaryStateBufferTooSmall)
        );
    }

    #[test]
    fn g17p_cl_entry_encodes_every_optional_section() {
        let barriers = [G17PClBarrierDependency {
            native_timestamp: None,
            event_word: 0x100,
            shared_stamp: 0,
            queue_id: 3,
        }];
        let rce_bindings = [
            G17PClRceBinding {
                address: 0x2000,
                tag: 1,
            },
            G17PClRceBinding {
                address: 0x4000,
                tag: 2,
            },
            G17PClRceBinding {
                address: 0x6000,
                tag: 3,
            },
            G17PClRceBinding {
                address: 0x8000,
                tag: 4,
            },
        ];
        let queue = G17PClKickProducerState {
            queue_id: 4,
            geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            completion_selector: 2,
            qos: G17PClQosConfig {
                queue_byte_32: 0x12,
                class_40: 0x1b,
                word_48: 0x3456,
            },
            submitted: 2,
            current_timestamp: 2,
            last_add_kicks_timestamp: 1,
            base_timestamp: 1,
            previous_valid: true,
            previous_timestamp: 7,
        };
        let prepared = prepare_g17p_cl_kick_entry(
            queue,
            G17PClKickEntryOperands {
                descriptor_flag_4c: true,
                descriptor_flag_5c8: true,
                converted_command_timestamp: 0,
                barriers: &barriers,
                mcache: Some(G17PClMcacheAperture {
                    address: 0x12340,
                    index: 5,
                    count: 3,
                }),
                payload: [0x1111_2222_3333_4444, 0x5555_6666_7777_8888],
                event_mask: [0x10, 0x20, 0x30, 0x40],
                rce_kind: 2,
                rce_bindings,
                auxiliary: 0xa5a5_5a5a_1234_5678,
            },
        )
        .unwrap();
        let header =
            (2u64 << 2) | (4u64 << 42) | (1u64 << 58) | (2u64 << 59) | (1u64 << 1) | (1u64 << 61);
        assert_eq!(get_u64_le(&prepared.entry, 0x00), header);
        assert_eq!(
            get_u64_le(&prepared.entry, 0x08),
            5 | ((0x12340u64 >> 5) << 6) | (2u64 << 54)
        );
        assert_eq!(get_u64_le(&prepared.entry, 0x10), 0x1111_2222_3333_4444);
        assert_eq!(get_u64_le(&prepared.entry, 0x18), 0x5555_6666_7777_8888);
        assert_eq!(get_u64_le(&prepared.entry, 0x20), 0x3456_1b12_0000_0003);
        assert_eq!(get_u64_le(&prepared.entry, 0x28), (3u64 << 40) | 1);
        assert_eq!(get_u64_le(&prepared.entry, 0x30), (4u64 << 40) | 7);
        assert_eq!(
            [
                get_u64_le(&prepared.entry, 0x128),
                get_u64_le(&prepared.entry, 0x130),
                get_u64_le(&prepared.entry, 0x138),
                get_u64_le(&prepared.entry, 0x140),
            ],
            [0x10, 0x20, 0x30, 0x40]
        );
        assert_eq!(prepared.entry[0x148], 2);
        for (index, binding) in rce_bindings.into_iter().enumerate() {
            assert_eq!(
                get_u64_le(&prepared.entry, 0x150 + index * 8),
                (binding.address >> 5) | ((binding.tag as u64) << 43)
            );
        }
        assert_eq!(get_u64_le(&prepared.entry, 0x170), 0);
        assert_eq!(get_u64_le(&prepared.entry, 0x178), 0xa5a5_5a5a_1234_5678);
        assert_eq!(prepared.submitted_after, 3);
        assert_eq!(prepared.previous_timestamp_after, 2);
    }

    /// G17P's native graphics wrappers select stage-specific launch classes,
    /// while direct entry-ring captures show the same 0xffff word for both
    /// ordinary TA and 3D kicks. Keep those exact +0x20 qwords pinned
    /// independently of the CL profile.
    #[test]
    fn g17p_native_render_dependency_control_words_are_exact() {
        fn operands(barriers: &[G17PClBarrierDependency]) -> G17PClKickEntryOperands<'_> {
            let empty = G17PClRceBinding { address: 0, tag: 0 };
            G17PClKickEntryOperands {
                descriptor_flag_4c: false,
                descriptor_flag_5c8: false,
                converted_command_timestamp: 1,
                barriers,
                mcache: None,
                payload: [0; 2],
                event_mask: [0; 4],
                rce_kind: 0,
                rce_bindings: [empty; 4],
                auxiliary: 0,
            }
        }

        let ta = T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY
            .initial_producer_state(
                0,
                2,
                G17PClQosConfig {
                    queue_byte_32: 2,
                    class_40: 0x0c,
                    word_48: 0xffff,
                },
            )
            .unwrap();
        let ta = prepare_g17p_cl_kick_entry(ta, operands(&[])).unwrap();
        assert_eq!(get_u64_le(&ta.entry, 0x20), 0xffff_0c02_0000_0001);

        let fragment_barrier = [G17PClBarrierDependency {
            native_timestamp: None,
            event_word: 0x100,
            shared_stamp: 0,
            queue_id: 0,
        }];
        let fragment = T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY
            .initial_producer_state(
                1,
                2,
                G17PClQosConfig {
                    queue_byte_32: 2,
                    class_40: 0x18,
                    word_48: 0xffff,
                },
            )
            .unwrap();
        let fragment =
            prepare_g17p_cl_kick_entry(fragment, operands(&fragment_barrier)).unwrap();
        assert_eq!(
            get_u64_le(&fragment.entry, 0x20),
            0xffff_1802_0000_0003
        );
    }

    #[test]
    fn g17p_cl_entry_reports_invalid_geometry_and_fields() {
        let empty = G17PClRceBinding { address: 0, tag: 0 };
        let queue = G17PClKickProducerState {
            queue_id: 0,
            geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            completion_selector: 0,
            qos: G17PClQosConfig {
                queue_byte_32: 0,
                class_40: 0,
                word_48: 0,
            },
            submitted: 0,
            current_timestamp: 1,
            last_add_kicks_timestamp: 0,
            base_timestamp: 1,
            previous_valid: false,
            previous_timestamp: 0,
        };
        let operands = G17PClKickEntryOperands {
            descriptor_flag_4c: true,
            descriptor_flag_5c8: true,
            converted_command_timestamp: 1,
            barriers: &[],
            mcache: None,
            payload: [0; 2],
            event_mask: [0; 4],
            rce_kind: 0,
            rce_bindings: [empty; 4],
            auxiliary: 0,
        };

        assert_eq!(
            prepare_g17p_cl_kick_entry(
                G17PClKickProducerState {
                    geometry: G17SksmQueueGeometry {
                        entry_stride: 0,
                        ..T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY
                    },
                    ..queue
                },
                operands,
            ),
            Err(SubmissionError::KickQueueGeometryInvalid)
        );
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                G17PClKickProducerState {
                    geometry: G17SksmQueueGeometry {
                        entry_stride: 0x17f,
                        ..T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY
                    },
                    ..queue
                },
                operands,
            ),
            Err(SubmissionError::KickQueueGeometryInvalid)
        );
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                G17PClKickProducerState {
                    geometry: G17SksmQueueGeometry {
                        stamp_count: 0,
                        ..T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY
                    },
                    ..queue
                },
                operands,
            ),
            Err(SubmissionError::KickQueueGeometryInvalid)
        );
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                G17PClKickProducerState {
                    submitted: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY.stamp_count() as u32,
                    ..queue
                },
                operands,
            ),
            Err(SubmissionError::KickQueueSubmittedFull)
        );
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                G17PClKickProducerState {
                    qos: G17PClQosConfig {
                        class_40: 0x20,
                        ..queue.qos
                    },
                    ..queue
                },
                operands,
            ),
            Err(SubmissionError::KickQosClassOutOfRange)
        );
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                queue,
                G17PClKickEntryOperands {
                    mcache: Some(G17PClMcacheAperture {
                        address: 0x20,
                        index: 0x40,
                        count: 1,
                    }),
                    ..operands
                },
            ),
            Err(SubmissionError::KickMcacheFieldOutOfRange)
        );
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                queue,
                G17PClKickEntryOperands {
                    rce_bindings: [G17PClRceBinding { address: 1, tag: 0 }, empty, empty, empty,],
                    ..operands
                },
            ),
            Err(SubmissionError::KickRceAddressUnencodable)
        );

        let too_many = [G17PClBarrierDependency {
            native_timestamp: None,
            event_word: 0,
            shared_stamp: 0,
            queue_id: 0,
        }; G17P_CL_DEPENDENCIES_MAX];
        assert_eq!(
            prepare_g17p_cl_kick_entry(
                queue,
                G17PClKickEntryOperands {
                    barriers: &too_many,
                    ..operands
                },
            ),
            Err(SubmissionError::KickDependencyCountOutOfRange)
        );
    }

    #[test]
    fn host_add_kicks_tags_match_all_three_pinned_producers() {
        let geometry = G17SksmScratchGeometry {
            write0_base: 0x100,
            write1_base: 0x104,
            stride: 3,
        };

        for queue_id in 0u8..=0x7f {
            let p = prepare_sksm_add_kicks_mmio(SksmAddKicksTarget::G17P, queue_id, 5, geometry)
                .unwrap();
            assert_eq!(p.tag, 0x0000_0400_0002_1018 | ((queue_id as u64) << 43));
            assert_eq!(p.tag_offset, 0x11b);
            assert_eq!(p.count_offset, 0x11f);
            assert_eq!(p.add_count, 5);
            assert_eq!(
                p.write_pair(),
                G17SksmOrderedWritePair {
                    word0_offset: 0x11b,
                    word0: 0x0000_0400_0002_1018 | ((queue_id as u64) << 43),
                    word1_offset: 0x11f,
                    word1: 5,
                }
            );

            let g = prepare_sksm_add_kicks_mmio(
                SksmAddKicksTarget::G17G,
                queue_id,
                0x89ab_cdef,
                geometry,
            )
            .unwrap();
            assert_eq!(g.tag, 0x0000_0800_0002_1018 | ((queue_id as u64) << 44));

            let x =
                prepare_sksm_add_kicks_mmio(SksmAddKicksTarget::G17X, queue_id, u32::MAX, geometry)
                    .unwrap();
            assert_eq!(x.tag, 0x0020_0800_0002_1018 | ((queue_id as u64) << 44));
            assert_eq!(x.add_count, u32::MAX);
        }
    }

    #[test]
    fn pair_writer_publishes_two_64_bit_words_in_order() {
        use std::cell::RefCell;

        struct Recorder(RefCell<Vec<(u32, u64)>>);

        impl G17SksmPairWriter for Recorder {
            type Error = ();

            fn write64(&self, offset: u32, value: u64) -> core::result::Result<(), Self::Error> {
                self.0.borrow_mut().push((offset, value));
                Ok(())
            }
        }

        let pair = prepare_sksm_add_kicks_mmio(
            SksmAddKicksTarget::G17P,
            0x2a,
            0x89ab_cdef,
            g17_manager::T8140_G17P_PLATFORM_CONFIG.sksm_scratch,
        )
        .unwrap()
        .write_pair();
        let recorder = Recorder(RefCell::new(Vec::new()));

        assert_eq!(publish_g17p_sksm_write_pair(&recorder, pair), Ok(()));
        assert_eq!(
            recorder.0.into_inner(),
            vec![
                (0x4090, 0x0001_5400_0002_1018),
                (0x4098, 0x0000_0000_89ab_cdef),
            ]
        );
    }

    #[test]
    fn pair_writer_does_not_publish_word1_after_word0_error() {
        use std::cell::Cell;

        struct FirstWriteError(Cell<u32>);

        impl G17SksmPairWriter for FirstWriteError {
            type Error = u32;

            fn write64(&self, _offset: u32, _value: u64) -> core::result::Result<(), Self::Error> {
                let calls = self.0.get() + 1;
                self.0.set(calls);
                Err(calls)
            }
        }

        let writer = FirstWriteError(Cell::new(0));
        let pair = G17SksmOrderedWritePair {
            word0_offset: 0x4090,
            word0: 1,
            word1_offset: 0x4098,
            word1: 2,
        };
        assert_eq!(publish_g17p_sksm_write_pair(&writer, pair), Err(1));
        assert_eq!(writer.0.get(), 1);
    }

    #[test]
    fn host_add_kicks_mmio_offsets_report_overflow() {
        let valid = G17SksmScratchGeometry {
            write0_base: 0,
            write1_base: 0,
            stride: u32::MAX / 9,
        };
        let publication =
            prepare_sksm_add_kicks_mmio(SksmAddKicksTarget::G17G, 0, 1, valid).unwrap();
        assert_eq!(publication.tag_offset, (u32::MAX / 9) * 9);

        assert_eq!(
            prepare_sksm_add_kicks_mmio(
                SksmAddKicksTarget::G17P,
                0x80,
                1,
                G17SksmScratchGeometry {
                    write0_base: 0,
                    write1_base: 4,
                    stride: 0,
                },
            ),
            Err(SubmissionError::QueueIdOutOfRange)
        );
        for geometry in [
            G17SksmScratchGeometry {
                write0_base: 0,
                write1_base: 0,
                stride: u32::MAX / 9 + 1,
            },
            G17SksmScratchGeometry {
                write0_base: u32::MAX,
                write1_base: 0,
                stride: 1,
            },
            G17SksmScratchGeometry {
                write0_base: 0,
                write1_base: u32::MAX,
                stride: 1,
            },
        ] {
            assert_eq!(
                prepare_sksm_add_kicks_mmio(SksmAddKicksTarget::G17X, 0, 1, geometry),
                Err(SubmissionError::KickMmioOffsetOverflow)
            );
        }
    }

    #[test]
    fn g17p_sksm_queue_configuration_matches_the_host_store_sequence() {
        assert_eq!(
            G17P_SKSM_RUNTIME_GEOMETRY_OBJECT_LAYOUT,
            G17PSksmRuntimeGeometryObjectLayout {
                write0_base_field: 0x10730,
                write1_base_field: 0x10734,
                stride_field: 0x10738,
            }
        );
        assert_eq!(G17P_SKSM_QUEUE_ENABLE_MASK_BASE, 0x21068);
        assert_eq!(G17P_SKSM_QUEUE_ENTRY_ADDRESS_MASK, 0x0000_07ff_ffff_ffe0);

        let config = prepare_g17p_sksm_queue_config(
            G17ChannelAbiTarget::A18ProG17P,
            0x40,
            1,
            0,
            0x12340,
            g17_manager::T8140_G17P_PLATFORM_CONFIG.sksm_scratch,
        )
        .unwrap();

        // configureHardware: base + 9*stride, tag, payload, then QID mask.
        assert_eq!(config.tag_offset, 0x4090);
        assert_eq!(config.tag, 0x0002_0400_0002_1008);
        assert_eq!(config.payload_offset, 0x4098);
        // This hard-coded value catches both the 32-bit selector arithmetic
        // and the host's ADD of `(data_master + 1) << 60` before the OR.
        assert_eq!(config.payload, 0x2280_0000_0001_2340);
        assert_eq!(
            config.write_pair(),
            G17SksmOrderedWritePair {
                word0_offset: 0x4090,
                word0: 0x0002_0400_0002_1008,
                word1_offset: 0x4098,
                word1: 0x2280_0000_0001_2340,
            }
        );
        assert_eq!(
            config.enable_pair,
            G17SksmOrderedWritePair {
                word0_offset: 0x21070,
                word0: 1,
                word1_offset: 0x21068,
                word1: 0,
            }
        );
        assert_eq!(
            config.disable_pair,
            G17SksmOrderedWritePair {
                word0_offset: 0x21080,
                word0: 1,
                word1_offset: 0x21078,
                word1: 0,
            }
        );
    }

    #[test]
    fn g17p_compute_queue_matches_the_captured_native_cl2_tuple() {
        assert_eq!(G17P_TA_DATA_MASTER, 0);
        assert_eq!(G17P_3D_DATA_MASTER, 1);
        assert_eq!(G17P_COMPUTE_DATA_MASTER, 2);

        let config = prepare_g17p_compute_sksm_queue_config(
            G17PComputeQueueRouting {
                queue_id: 4,
                completion_selector: 2,
                entry_gpu_va: 0x0000_0070_004d_8000,
                geometry: T8140_G17P_HAL200_SKSM_QUEUE_GEOMETRY,
            },
            g17_manager::T8140_G17P_PLATFORM_CONFIG.sksm_scratch,
        )
        .unwrap();

        assert_eq!(config.tag_offset, 0x4090);
        assert_eq!(config.tag, 0x0000_2400_0002_1008);
        assert_eq!(config.payload_offset, 0x4098);
        assert_eq!(config.payload, 0x3100_0070_004d_8000);
        assert_eq!(
            config.enable_pair,
            G17SksmOrderedWritePair {
                word0_offset: 0x21070,
                word0: 0,
                word1_offset: 0x21068,
                word1: 1 << 4,
            }
        );
        assert_eq!(
            config.disable_pair,
            G17SksmOrderedWritePair {
                word0_offset: 0x21080,
                word0: 0,
                word1_offset: 0x21078,
                word1: 1 << 4,
            }
        );
    }

    #[test]
    fn g17p_sksm_queue_configuration_is_identity_and_range_scoped() {
        let geometry = g17_manager::T8140_G17P_PLATFORM_CONFIG.sksm_scratch;

        for target in [
            G17ChannelAbiTarget::M5G17G,
            G17ChannelAbiTarget::M5ProMaxG17X,
        ] {
            assert_eq!(
                prepare_g17p_sksm_queue_config(target, 0, 0, 0, 0x20, geometry),
                Err(SubmissionError::ChannelAbiUnconfirmed)
            );
        }
        assert_eq!(
            prepare_g17p_sksm_queue_config(
                G17ChannelAbiTarget::A18ProG17P,
                0x80,
                0,
                0,
                0x20,
                geometry,
            ),
            Err(SubmissionError::QueueIdOutOfRange)
        );
        assert_eq!(
            prepare_g17p_sksm_queue_config(
                G17ChannelAbiTarget::A18ProG17P,
                0,
                3,
                0,
                0x20,
                geometry,
            ),
            Err(SubmissionError::KickQueueDataMasterOutOfRange)
        );
        assert_eq!(
            prepare_g17p_sksm_queue_config(
                G17ChannelAbiTarget::A18ProG17P,
                0,
                0,
                5,
                0x20,
                geometry,
            ),
            Err(SubmissionError::KickQueueCompletionSelectorOutOfRange)
        );
        for address in [0x21, 0x0000_0800_0000_0000] {
            assert_eq!(
                prepare_g17p_sksm_queue_config(
                    G17ChannelAbiTarget::A18ProG17P,
                    0,
                    0,
                    0,
                    address,
                    geometry,
                ),
                Err(SubmissionError::KickQueueEntryAddressUnencodable)
            );
        }
        assert_eq!(
            prepare_g17p_sksm_queue_config(
                G17ChannelAbiTarget::A18ProG17P,
                0,
                0,
                0,
                0x20,
                G17SksmScratchGeometry {
                    write0_base: u32::MAX,
                    write1_base: 0,
                    stride: 1,
                },
            ),
            Err(SubmissionError::KickMmioOffsetOverflow)
        );
    }

    #[test]
    fn g17p_completion_descriptors_match_all_four_host_slots() {
        assert_eq!(G17P_KSM_COMPLETION_QUEUE_COUNT, 4);
        assert_eq!(G17P_KSM_COMPLETION_DESCRIPTOR_SIZE, 0x20);
        assert_eq!(G17P_KSM_COMPLETION_ENTRY_SIZE, 0x40);
        assert_eq!(G17P_KSM_COMPLETION_CAPACITY, 0x800);
        assert_eq!(G17P_KSM_COMPLETION_BACKING_SIZE, 0x20000);
        assert_eq!(G17P_KSM_COMPLETION_PRESENT_ORDINALS, [0, 2]);
        assert_eq!(G17P_NORMAL_CL_COMPLETION_ORDINAL, 0);
        assert_eq!(
            G17P_KSM_COMPLETION_DESCRIPTOR_LAYOUT,
            G17PKsmCompletionDescriptorLayout {
                gpu_va: 0x00,
                cpu_va: 0x08,
                capacity: 0x10,
                entry_size: 0x14,
                ordinal: 0x18,
                processed: 0x1c,
            }
        );

        for ordinal in 0..G17P_KSM_COMPLETION_QUEUE_COUNT {
            let gpu_va = 0x1234_0000 + ordinal as u64 * 0x1000;
            let cpu_va = 0xabcd_0000 + ordinal as u64 * 0x2000;
            let capacity = 0x80 + ordinal as u32;
            let raw = encode_g17p_ksm_completion_descriptor(
                G17ChannelAbiTarget::A18ProG17P,
                ordinal,
                gpu_va,
                cpu_va,
                capacity,
            )
            .unwrap();

            assert_eq!(&raw[0x00..0x08], &gpu_va.to_le_bytes());
            assert_eq!(&raw[0x08..0x10], &cpu_va.to_le_bytes());
            assert_eq!(&raw[0x10..0x14], &capacity.to_le_bytes());
            assert_eq!(&raw[0x14..0x18], &0x40u32.to_le_bytes());
            assert_eq!(&raw[0x18..0x1c], &(ordinal as u32).to_le_bytes());
            assert_eq!(&raw[0x1c..0x20], &[0; 4]);
        }
        assert!(G17P_COMPLETION_HOST_ABI_GROUNDED);
    }

    #[test]
    fn g17p_completion_descriptors_report_invalid_inputs() {
        assert_eq!(
            encode_g17p_ksm_completion_descriptor(
                G17ChannelAbiTarget::M5G17G,
                0,
                0x1000,
                0x2000,
                1,
            ),
            Err(SubmissionError::ChannelAbiUnconfirmed)
        );
        assert_eq!(
            encode_g17p_ksm_completion_descriptor(
                G17ChannelAbiTarget::A18ProG17P,
                4,
                0x1000,
                0x2000,
                1,
            ),
            Err(SubmissionError::CompletionQueueOrdinalOutOfRange)
        );
        assert_eq!(
            encode_g17p_ksm_completion_descriptor(
                G17ChannelAbiTarget::A18ProG17P,
                0,
                0x1000,
                0x2000,
                0,
            ),
            Err(SubmissionError::CompletionQueueCapacityZero)
        );
    }

    #[test]
    fn kick_record_table_uses_the_firmware_geometry() {
        // add x2,x22,#0x10 ; add x2,x2,QID,lsl#5 -> base 0x10, stride 0x20.
        assert_eq!(KICK_RECORD_TABLE_BASE, 0x10);
        assert_eq!(KICK_RECORD_STRIDE, 0x20);
        assert_eq!(KICK_RECORD_COUNT, 128);
        assert_eq!(kick_record_offset(0), Ok(0x10));
        assert_eq!(kick_record_offset(1), Ok(0x30));
        assert_eq!(kick_record_offset(0x7f), Ok(0x0ff0));
        assert_eq!(
            kick_record_offset(0x80),
            Err(SubmissionError::QueueIdOutOfRange)
        );
        // The whole 128-entry table fits in 0x10 + 128*0x20 = 0x1010 bytes.
        assert_eq!(
            KICK_RECORD_TABLE_BASE + KICK_RECORD_COUNT * KICK_RECORD_STRIDE,
            0x1010
        );
    }

    #[test]
    fn kick_record_internal_layout_is_exact() {
        // Written across 0x10cb0-0x10d2c.
        assert_eq!(
            KICK_RECORD_LAYOUT,
            KickRecordLayout {
                ts_first: 0x00,
                ts_last: 0x08,
                slot_mask: 0x10,
                valid_flag: 0x18,
                slot_index: 0x1c,
            }
        );
        // Every field lies inside one 0x20-byte record.
        for off in [
            KICK_RECORD_LAYOUT.ts_first,
            KICK_RECORD_LAYOUT.ts_last,
            KICK_RECORD_LAYOUT.slot_mask,
            KICK_RECORD_LAYOUT.valid_flag,
            KICK_RECORD_LAYOUT.slot_index,
        ] {
            assert!(off < KICK_RECORD_STRIDE);
        }
    }

    #[test]
    fn g17p_top_level_command_names_match_the_host_abi() {
        let known = [
            (G17PTopLevelCommandType::Ta, 0),
            (G17PTopLevelCommandType::ThreeD, 1),
            (G17PTopLevelCommandType::FastBlit, 2),
            (G17PTopLevelCommandType::Cl, 3),
            (G17PTopLevelCommandType::Barrier, 4),
            (G17PTopLevelCommandType::PmGrow, 5),
            (G17PTopLevelCommandType::RemoteNode, 7),
            (G17PTopLevelCommandType::SharedEventSignal, 9),
            (G17PTopLevelCommandType::AddFinalFrgGtpcKick, 12),
            (G17PTopLevelCommandType::KsmKickQueueAddKicks, 14),
            (G17PTopLevelCommandType::ConfigUpdate, 15),
            (G17PTopLevelCommandType::EntrySignal, 16),
            (G17PTopLevelCommandType::Nop, 17),
            (G17PTopLevelCommandType::LateEvalEventSignal, 18),
        ];
        for (command_type, raw) in known {
            assert_eq!(command_type as u32, raw);
        }

        assert_eq!(G17P_TOP_LEVEL_COMMAND_TAG_OFFSET, 0);
        assert_eq!(G17P_TOP_LEVEL_COMMAND_TAG_WIDTH, 4);
        assert_eq!(G17P_CHANNEL_COMMAND_TYPE_MAX, 18);
        assert_eq!(G17P_TOP_LEVEL_COMMAND_KNOWN_MASK, 0x0007_d2bf);
        assert_eq!(G17P_TOP_LEVEL_COMMAND_UNRESOLVED_MASK, 0x0000_0d40);
        assert_eq!(G17P_TOP_LEVEL_COMMAND_INVALID_MASK, 0x0000_2000);
        assert_eq!(
            G17P_TOP_LEVEL_COMMAND_KNOWN_MASK
                | G17P_TOP_LEVEL_COMMAND_UNRESOLVED_MASK
                | G17P_TOP_LEVEL_COMMAND_INVALID_MASK,
            (1 << (G17P_CHANNEL_COMMAND_TYPE_MAX + 1)) - 1
        );
    }

    #[test]
    fn g17p_top_level_command_decoder_reads_exact_little_endian_bytes() {
        let known = [
            G17PTopLevelCommandType::Ta,
            G17PTopLevelCommandType::ThreeD,
            G17PTopLevelCommandType::FastBlit,
            G17PTopLevelCommandType::Cl,
            G17PTopLevelCommandType::Barrier,
            G17PTopLevelCommandType::PmGrow,
            G17PTopLevelCommandType::RemoteNode,
            G17PTopLevelCommandType::SharedEventSignal,
            G17PTopLevelCommandType::AddFinalFrgGtpcKick,
            G17PTopLevelCommandType::KsmKickQueueAddKicks,
            G17PTopLevelCommandType::ConfigUpdate,
            G17PTopLevelCommandType::EntrySignal,
            G17PTopLevelCommandType::Nop,
            G17PTopLevelCommandType::LateEvalEventSignal,
        ];
        for command_type in known {
            let mut command = [0xa5; 9];
            command[..4].copy_from_slice(&(command_type as u32).to_le_bytes());
            assert_eq!(
                decode_g17p_top_level_command(G17ChannelAbiTarget::A18ProG17P, &command),
                Ok(command_type)
            );
        }

        for length in 0..4 {
            assert_eq!(
                decode_g17p_top_level_command(
                    G17ChannelAbiTarget::A18ProG17P,
                    &[0, 0, 0, 0][..length]
                ),
                Err(SubmissionError::CommandHeaderTooShort)
            );
        }

        // The dispatcher reads all four little-endian bytes at offset zero;
        // it neither truncates to byte zero nor scans for a nested opcode.
        assert_eq!(
            decode_g17p_top_level_command(
                G17ChannelAbiTarget::A18ProG17P,
                &[0, 0, 0, 1, 18, 0, 0, 0]
            ),
            Err(SubmissionError::CommandTagInvalid)
        );
    }

    #[test]
    fn g17p_top_level_command_decode_reports_bad_tag_and_identity() {
        for raw in [6u32, 8, 10, 11] {
            assert_eq!(
                decode_g17p_top_level_command(G17ChannelAbiTarget::A18ProG17P, &raw.to_le_bytes()),
                Err(SubmissionError::CommandTagUnresolved)
            );
        }
        for raw in [13u32, 19, u32::MAX] {
            assert_eq!(
                decode_g17p_top_level_command(G17ChannelAbiTarget::A18ProG17P, &raw.to_le_bytes()),
                Err(SubmissionError::CommandTagInvalid)
            );
        }
        for target in [
            G17ChannelAbiTarget::M5G17G,
            G17ChannelAbiTarget::M5ProMaxG17X,
        ] {
            assert_eq!(
                decode_g17p_top_level_command(target, &0u32.to_le_bytes()),
                Err(SubmissionError::CommandAbiUnconfirmed)
            );
            assert_eq!(
                classify_g17p_channel_command(target, 0),
                Err(SubmissionError::CommandAbiUnconfirmed)
            );
        }
    }

    #[test]
    fn g17p_command_dispatch_matches_the_firmware_jump_table() {
        assert_eq!(G17P_CHANNEL_COMMAND_PROCESS_MASK, 0x0005_d2bf);
        assert_eq!(G17P_CHANNEL_COMMAND_PM_REBUILD_MASK, 0x0000_0040);
        assert_eq!(G17P_CHANNEL_COMMAND_INVALID_MASK, 0x0002_2d00);
        assert_eq!(
            G17P_CHANNEL_COMMAND_PROCESS_MASK
                | G17P_CHANNEL_COMMAND_PM_REBUILD_MASK
                | G17P_CHANNEL_COMMAND_INVALID_MASK,
            (1 << (G17P_CHANNEL_COMMAND_TYPE_MAX + 1)) - 1
        );

        for raw in 0..=G17P_CHANNEL_COMMAND_TYPE_MAX {
            let expected = match raw {
                0 | 1 | 2 | 3 | 4 | 5 | 7 | 9 | 12 | 14 | 15 | 16 | 18 => {
                    G17PChannelCommandDisposition::Process
                }
                6 => G17PChannelCommandDisposition::PmRebuildForbiddenFatal,
                8 | 10 | 11 | 13 | 17 => G17PChannelCommandDisposition::InvalidCommandFatal,
                _ => unreachable!(),
            };
            assert_eq!(
                classify_g17p_channel_command(G17ChannelAbiTarget::A18ProG17P, raw),
                Ok(expected)
            );
        }
        for raw in [19, u32::MAX] {
            assert_eq!(
                classify_g17p_channel_command(G17ChannelAbiTarget::A18ProG17P, raw),
                Ok(G17PChannelCommandDisposition::InvalidCommandFatal)
            );
        }

        // Host ABI identity and matched-firmware admission intentionally split.
        let nop = (G17PTopLevelCommandType::Nop as u32).to_le_bytes();
        assert_eq!(
            decode_g17p_top_level_command(G17ChannelAbiTarget::A18ProG17P, &nop),
            Ok(G17PTopLevelCommandType::Nop)
        );
        assert_eq!(
            admit_g17p_top_level_command(G17ChannelAbiTarget::A18ProG17P, &nop),
            Err(SubmissionError::CommandTagInvalidForFirmware)
        );

        for command_type in [
            G17PTopLevelCommandType::Ta,
            G17PTopLevelCommandType::ThreeD,
            G17PTopLevelCommandType::FastBlit,
            G17PTopLevelCommandType::Cl,
            G17PTopLevelCommandType::Barrier,
            G17PTopLevelCommandType::PmGrow,
            G17PTopLevelCommandType::RemoteNode,
            G17PTopLevelCommandType::SharedEventSignal,
            G17PTopLevelCommandType::AddFinalFrgGtpcKick,
            G17PTopLevelCommandType::KsmKickQueueAddKicks,
            G17PTopLevelCommandType::ConfigUpdate,
            G17PTopLevelCommandType::EntrySignal,
            G17PTopLevelCommandType::LateEvalEventSignal,
        ] {
            assert_eq!(
                admit_g17p_top_level_command(
                    G17ChannelAbiTarget::A18ProG17P,
                    &(command_type as u32).to_le_bytes()
                ),
                Ok(command_type)
            );
        }
    }

    #[test]
    fn g17p_command_prefilter_matches_literal_mask() {
        assert_eq!(G17P_CHANNEL_COMMAND_PREFILTER_MASK, 0x0001_d03f);
        for raw in 0..=20 {
            let expected = matches!(raw, 0 | 1 | 2 | 3 | 4 | 5 | 12 | 14 | 15 | 16);
            assert_eq!(
                g17p_channel_command_prefilter_allows(G17ChannelAbiTarget::A18ProG17P, raw),
                Ok(expected)
            );
        }
        assert_eq!(
            g17p_channel_command_prefilter_allows(G17ChannelAbiTarget::M5G17G, 0),
            Err(SubmissionError::CommandAbiUnconfirmed)
        );
    }

    #[test]
    fn provenance_ledger_is_explicit_about_reuse_and_fork() {
        assert_eq!(
            SUBMISSION_ABI_LEDGER.tx_doorbell,
            AbiProvenance::IdenticalToAgx2
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.kick_word,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.kick_queue_producer,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.host_add_kicks_mmio,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.g17p_queue_config,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.g17p_completion_descriptors,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert!(HOST_SKSM_ADD_KICKS_PATH_SELECTED);
        assert_eq!(
            SUBMISSION_ABI_LEDGER.kick_record,
            AbiProvenance::ForkedAgx3Grounded
        );
        // The channel/ring state records stay inherited and unsupported. The
        // widened native descriptor geometry is grounded first-hand.
        assert_eq!(
            SUBMISSION_ABI_LEDGER.channel_state,
            AbiProvenance::Agx2InheritedUnconfirmed
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.g17p_channel_state,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.ring_state,
            AbiProvenance::Agx2InheritedUnconfirmed
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.channel_descriptor,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.host_channel_state_reset,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.host_uncached_channel_reset,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert_eq!(
            SUBMISSION_ABI_LEDGER.g17p_command_types,
            AbiProvenance::ForkedAgx3Grounded
        );
        assert!(CHANNEL_DESCRIPTOR_STRIDE_CONFIRMED);
    }

    #[test]
    fn transport_tracks_classic_graphics_and_native_sksm_independently() {
        assert!(GROUNDED_SUBMISSION_PRODUCER_AVAILABLE);
        assert!(!EXECUTABLE_SUBMISSION_TRANSPORT_AVAILABLE);
        assert!(!SUBMISSION_LIVE_GAPS.all_proven());
        assert!(!SUBMISSION_LIVE_GAPS.classic_graphics_proven());
        assert!(!SUBMISSION_LIVE_GAPS.native_sksm_proven());
        assert!(SUBMISSION_LIVE_GAPS.sksm_resource0_ordered_write_implemented);
        assert!(SUBMISSION_LIVE_GAPS.channel_doorbell_round_trip_proven);
        assert!(!SUBMISSION_LIVE_GAPS.partial_graphics_runtime_implemented);
        assert!(SUBMISSION_LIVE_GAPS.partial_graphics_round_trip_proven);
        assert!(!SUBMISSION_LIVE_GAPS.sksm_add_kicks_round_trip_proven);
        assert!(!SUBMISSION_LIVE_GAPS.completion_irq_route_proven);
        assert!(SUBMISSION_LIVE_GAPS.normal_cl_completion_consumer_implemented);

        let classic = SubmissionLiveGaps {
            sksm_resource0_ordered_write_implemented: true,
            channel_doorbell_round_trip_proven: true,
            partial_graphics_runtime_implemented: true,
            partial_graphics_round_trip_proven: true,
            sksm_add_kicks_round_trip_proven: false,
            completion_irq_route_proven: true,
            normal_cl_completion_consumer_implemented: true,
        };
        assert!(classic.classic_graphics_proven());
        assert!(!classic.native_sksm_proven());
        assert!(classic.all_proven());
        assert!(!classic.sksm_add_kicks_round_trip_proven);

        let native = SubmissionLiveGaps {
            sksm_resource0_ordered_write_implemented: true,
            channel_doorbell_round_trip_proven: false,
            partial_graphics_runtime_implemented: false,
            partial_graphics_round_trip_proven: false,
            sksm_add_kicks_round_trip_proven: true,
            completion_irq_route_proven: true,
            normal_cl_completion_consumer_implemented: true,
        };
        assert!(!native.classic_graphics_proven());
        assert!(native.native_sksm_proven());
        assert!(native.all_proven());
        assert!(!SubmissionLiveGaps {
            normal_cl_completion_consumer_implemented: false,
            ..native
        }
        .native_sksm_proven());
    }
}

// ---------------------------------------------------------------------------
// Parameter-buffer descriptor handshake
//
// Metal performs this around every render submit and this driver has never
// performed it at all. Decoded from the 25G83 host kernelcache, which carries
// both halves symbolised:
//
//   AGXAcceleratorG16::halLoadPBDesc  @ 0xfffffe0008a52890   before submit
//   AGXAcceleratorG16::halStorePBDesc @ 0xfffffe0008a527f0   on completion
//
// They are exact inverses, which is what makes the field map trustworthy: the
// store reads back precisely the four quantities the load wrote. Confirmed on
// hardware -- a real Metal render leaves the ring state {7, 7, 0, 0}.
//
// The ring lives at `[[accelerator + 0x11358] + 0x60]` with 16-byte entries
// indexed by the `uint` third argument, and `halStorePBDesc` runs on
// `kernel_task`, i.e. from the completion path rather than the submitting
// thread.
// ---------------------------------------------------------------------------

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

/// Encode the high segment of a parameter-buffer address.
///
/// `PBState[+0x28]` contains the full tagged address: native passes
/// `0x1000190000`. Hardware adds an implicit `0x1000000000` bias when it
/// rebuilds the address from this segment and dword0's low 36 bits. Bit 42
/// selects between the two kext encoding forms.
pub(crate) const fn g17p_pbdesc_segment(pb_address: u64) -> u64 {
    let base: u64 = if pb_address & 0x400_0000_0000 == 0 {
        0x70_0000_0000
    } else {
        0
    };
    let high = (pb_address >> 3) & 0x80_0000_0000;
    ((base.wrapping_add(pb_address)) & 0x70_0000_0000) | high
}

/// Address reconstructed by the first G17P PB page-list read. This models the
/// hardware bias witnessed by the exact `0x8002478000` fault when the host
/// incorrectly encoded stripped address `0x2478000`.
pub(crate) const fn g17p_pbdesc_reconstructed_address(pb_address: u64) -> u64 {
    (g17p_pbdesc_segment(pb_address).wrapping_add(0x10_0000_0000))
        | (pb_address & 0x0f_ffff_ffff)
}

/// `halLoadPBDesc`: fold the ring state and the parameter-buffer address into
/// the descriptor, preserving every bit the firmware owns.
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

/// `halStorePBDesc`: read the ring state back out. Exact inverse of the encode.
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

// ---------------------------------------------------------------------------
// UMA page-pool descriptor handshake
//
// `AGXAcceleratorG16::halLoadUMADesc` publishes one 0x20-byte descriptor at
// `[[accelerator + 0x11360] + 0x60] + HardwareBufferID * 0x20` before the
// command descriptors become visible. `halStoreUMADesc` is the inverse on
// completion. The fourth qword belongs to the firmware and is not touched by
// either load or store.
// ---------------------------------------------------------------------------

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

/// Exact `halLoadUMADesc` packing. The source fields live at unaligned
/// offsets `+0x14/+0x1c/+0x20/+0x24/+0x28/+0x2c` in
/// `_AGFIUMAPagePoolState`.
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

/// Recover the mutable pool state before its hardware slot changes owners.
/// The address and allocation unit belong to the retained pool; word three
/// is firmware-owned and is not part of the load/store handshake.
pub(crate) const fn decode_g17p_umadesc(
    descriptor: [u64; 4],
    mut state: G17PUmaPagePoolState,
) -> G17PUmaPagePoolState {
    state.counter_a = ((descriptor[1] >> 5) & 0x0fff_ffff) as u32;
    state.counter_b = ((descriptor[1] >> 33) & 0x0fff_ffff) as u32;
    state.empty = (descriptor[1] >> 61) as u32;
    state.total_pages = descriptor[2] as u32;
    state
}

#[cfg(test)]
mod uma_descriptor_tests {
    use super::*;

    #[test]
    fn first_render_uma_descriptor_matches_the_native_packer() {
        let encoded = encode_g17p_umadesc(
            [0, 0, 0, 0xfeed_face_dead_beef],
            G17PUmaPagePoolState {
                page_list: 0x70_0000_0000,
                unit_pages: 0x4_0000,
                counter_a: 0,
                counter_b: 0x1600,
                empty: 0,
                total_pages: 0x1600,
            },
        );
        assert_eq!(
            encoded,
            [
                0x0800_0000_e000_0000,
                0x0000_2c00_0000_0000,
                0x0000_0000_0000_1600,
                0xfeed_face_dead_beef,
            ]
        );
        assert_eq!(g17p_umadesc_offset(0), Some(0));
        assert_eq!(g17p_umadesc_offset(0xff), Some(0x1fe0));
        assert_eq!(g17p_umadesc_offset(0x100), None);
    }
}

/// Opcode of the USC private-memory free-list device-control record.
///
/// MISIDENTIFIED ONCE, corrected 2026-08-31. This was briefly taken for the
/// parameter-buffer descriptor-ring publication because it is the only
/// device-control record in a whole process lifetime that carries low VAs, and
/// it appears once per queue right after `allocParameterManagement`. That is
/// adjacency, not provenance. The record is built by
/// `AGXUSCPrivMemFList::updateSubmitInfo(AGXUMADataRec*, int)`, which belongs
/// to the USC/UMA page-pool family (`halLoadUMADesc`/`halStoreUMADesc`), not
/// the parameter-buffer family (`halLoadPBDesc`/`halStorePBDesc`).
///
/// The real owner of the PBDesc ring is `AGXAccelerator::allocPBDescriptor`,
/// which is the ONLY site in the image that stores to `accelerator + 0x11358`
/// -- the pointer `halLoadPBDesc` dereferences. It allocates the object; it
/// publishes no device-control record, so the ring reaches firmware by some
/// other route we have not yet found.
pub(crate) const G17P_DEVCTL_USC_FREELIST: u32 = 0x20;

/// Span of the USC free-list ring, as published by the opcode-0x20 record.
///
/// Stable across four independent captures, so it is a fixed address rather
/// than an allocation. It is NOT the parameter-buffer descriptor ring -- see
/// `G17P_DEVCTL_USC_FREELIST`. Kept because if the USC free list is a third
/// thing this driver does not maintain, this is where it lives.
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

/// Build the opcode-0x20 (USC free-list) device-control record.
///
/// Only the first 0x40 bytes of an `AGFIAcceleratorDeviceControlEntry` ever
/// reach firmware: `AGXFirmware::submitDeviceControl` copies exactly four
/// 16-byte SIMD pairs (`ldp q0,q1` / `ldp q2,q3` then `stp`) into a ring whose
/// stride is `1 << 6` and whose bound is `cmp w8, #0x100`. Our
/// `CONTROL_RECORD_SIZE` (0x40) and `CONTROL_RING_ENTRIES` (0x100) already
/// match, so the fields the capture shows at +0x40/+0x44/+0x4c are kext-side
/// bookkeeping that never crosses the interface.
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
    put_u32_le(out, 0x08, 0x3f); // constant across all three captures
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

#[cfg(test)]
mod usc_freelist_tests {
    use super::*;

    fn u32_at(raw: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(raw[offset..offset + 4].try_into().unwrap())
    }

    fn u64_at(raw: &[u8], offset: usize) -> u64 {
        u64::from_le_bytes(raw[offset..offset + 8].try_into().unwrap())
    }

    #[test]
    fn linux_owned_first_render_usc_freelist_record_is_exact() {
        let mut record = [0xa5; 0x40];
        encode_g17p_usc_freelist_control(
            0xffff_fc20_c082_8000,
            G17P_USC_FREELIST_LOW_VA,
            G17P_USC_FREELIST_LOW_END,
            G17PUscFreelistSequence {
                word_04: 1,
                word_0c: 0,
                word_30: 0,
            },
            &mut record,
        )
        .unwrap();
        assert_eq!(u32_at(&record, 0x00), G17P_DEVCTL_USC_FREELIST);
        assert_eq!(u32_at(&record, 0x04), 1);
        assert_eq!(u32_at(&record, 0x08), 0x3f);
        assert_eq!(u32_at(&record, 0x0c), 0);
        assert_eq!(u64_at(&record, 0x14), 0xffff_fc20_c082_8000);
        assert_eq!(u64_at(&record, 0x1c), G17P_USC_FREELIST_LOW_VA);
        assert_eq!(u64_at(&record, 0x24), G17P_USC_FREELIST_LOW_END);
        assert_eq!(
            u32_at(&record, 0x2c),
            G17P_USC_FREELIST_GROW_DESCRIPTOR_BYTES
        );
        assert_eq!(u32_at(&record, 0x30), 0);
        assert_eq!(u32_at(&record, 0x34), 1);
        assert!(record[0x38..].iter().all(|byte| *byte == 0));
    }
}


#[cfg(test)]
mod exact_prior_tests {
    use super::*;

    #[test]
    fn absent_prior_preserves_every_legacy_header_form() {
        for base in [0, 1, 100, u64::MAX - 10] {
            for preceding in 0..5 {
                for barrier in [u16::MAX, 0, 1, 3, 5] {
                    assert_eq!(g17p_render_order_with_prior(base, preceding, barrier, 0),
                               g17p_render_order(base, preceding, barrier));
                }
            }
        }
    }

    #[test]
    fn exact_old_prefix_does_not_grow_with_newer_unrelated_commands() {
        for base in [7, 8, 1000] {
            for preceding in [0, 1, 17] {
                let order = g17p_render_order_with_prior(base, preceding, u16::MAX, 7).unwrap();
                assert_eq!(order.wait_through, Some(7));
                assert_eq!(order.sequence, base + preceding + 1);
                assert!(g17p_render_prefix_contains(order.wait_through, 7));
                assert!(!g17p_render_prefix_contains(order.wait_through, 8));
            }
        }
    }

    #[test]
    fn historical_max_preserves_zero_and_positive_relative_prefixes() {
        assert_eq!(g17p_render_order_with_prior(100, 4, 0, 7).unwrap().wait_through, Some(100));
        assert_eq!(g17p_render_order_with_prior(100, 4, 3, 100).unwrap().wait_through, Some(103));
        assert_eq!(g17p_render_order_with_prior(100, 4, u16::MAX, 100).unwrap().wait_through, Some(100));
        assert_eq!(g17p_render_order_with_prior(0, 0, u16::MAX, 0).unwrap().wait_through, None);
    }

    #[test]
    fn earlier_current_ioctl_commands_are_not_historical() {
        for prior in [11, 12, 100, u64::MAX] {
            assert_eq!(g17p_render_order_with_prior(10, 100, u16::MAX, prior),
                       Err(SubmissionError::PriorBarrierFuture));
        }
        assert_eq!(g17p_render_order_with_prior(0, 1, u16::MAX, 1),
                   Err(SubmissionError::PriorBarrierFuture));
    }

    #[test]
    fn relative_future_and_counter_overflow_still_reject() {
        assert_eq!(g17p_render_order_with_prior(10, 1, 2, 7),
                   Err(SubmissionError::RenderBarrierFuture));
        assert_eq!(g17p_render_order_with_prior(u64::MAX, 0, u16::MAX, 1),
                   Err(SubmissionError::RenderBarrierSequenceOverflow));
        assert_eq!(g17p_render_order_with_prior(u64::MAX - 1, 1, 1, 1),
                   Err(SubmissionError::RenderBarrierSequenceOverflow));
    }

    #[test]
    fn split_ioctl_prefix_has_same_members_as_original_relative_prefix() {
        let original = g17p_render_order_with_prior(20, 6, 3, 0).unwrap();
        let later_slice = g17p_render_order_with_prior(25, 1, u16::MAX, 23).unwrap();
        assert_eq!(original, later_slice);
        for sequence in 0..30 {
            assert_eq!(g17p_render_prefix_contains(original.wait_through, sequence),
                       g17p_render_prefix_contains(later_slice.wait_through, sequence));
        }
    }

    #[test]
    fn physical_siblings_remain_independent_prefix_members() {
        let order = g17p_render_order_with_prior(50, 0, u16::MAX, 12).unwrap();
        let mut deps = [g17p_render_barrier_from_timestamp(0, 0).unwrap(); 4];
        let mut seqs = [0; 4];
        let mut count = 0;
        for (sequence, qid, timestamp) in [(10, 2, 99), (11, 3, 55), (12, 2, 100)] {
            assert!(g17p_render_prefix_contains(order.wait_through, sequence));
            let dep = g17p_render_barrier_from_timestamp(timestamp, qid).unwrap();
            g17p_insert_prefix_dependency(&mut deps, &mut seqs, &mut count, sequence, dep).unwrap();
        }
        assert_eq!(count, 2);
        assert_eq!(&seqs[..count], &[12, 11]);
        assert_eq!((deps[0].queue_id, deps[1].queue_id), (2, 3));
        // Consuming one destination does not change another's exact prefix.
        assert_eq!(g17p_render_order_with_prior(51, 0, u16::MAX, 12).unwrap().wait_through,
                   order.wait_through);
    }
}
