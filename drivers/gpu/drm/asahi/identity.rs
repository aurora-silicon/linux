// SPDX-License-Identifier: GPL-2.0-only OR MIT


/// GPU generation enumeration. Note: Part of the UABI.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuGen {
    G13 = 13,
    G14 = 14,
    G15 = 15,
    G16 = 16,
    G17 = 17,
}

/// GPU variant enumeration. Note: Part of the UABI.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuVariant {
    P = 'P' as u32,
    G = 'G' as u32,
    S = 'S' as u32,
    C = 'C' as u32,
    D = 'D' as u32,
    A = 'A' as u32,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuHalGeneration {
    Legacy = 0,
    Hal200 = 200,
    Hal300 = 300,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum FirmwareRoleTopology {
    /// One GFX firmware role.
    Single,
    /// Separate GFX and GFX1 firmware roles.
    Dual,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionTransport {
    /// Classic device-control ring and SKU channels only.
    Classic,
    /// Both classic and 128-queue SKSM stacks are linked.
    ClassicAndSksm,
    /// 128-queue SKSM transport only.
    Sksm,
}

/// Host submission family selected from a complete decoded GPU identity.
///
/// This is intentionally narrower than [`SubmissionTransport`]. A binary may
/// link both classic and SKSM infrastructure while its target factories still
/// select exactly one data path.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionBackendFamily {
    /// Classic SKU channels used by G13-G15 and G16G.
    ClassicSku,
    /// G16 SKSM hybrid used by the G16X targets.
    G16Hal200Sksm,
    /// G16 SKSM machinery under G17's dual-firmware topology.
    G17Hal200Hybrid,
    /// PI_300 target SKSM channels used by G17G and G17X.
    G17Hal300Sksm,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionBackendBasis {
    /// The named target's own channel factories establish the selection.
    Established,
    /// An adjacent target shares the gate, but the named target is not pinned.
    CrossGenerationDerived,
}

/// Runtime implementation available in this driver checkout.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionBackendImplementation {
    /// Existing mature classic submission path; no G17 code is substituted.
    ExistingClassic,
    /// The bounded T8140/G17P first-partial path in this checkout.
    T8140G17PFirstPartial,
    NotImplemented,
    /// The family is only cross-generation-derived and cannot be enabled.
    Unconfirmed,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct SubmissionBackendSelection {
    pub(crate) family: SubmissionBackendFamily,
    pub(crate) basis: SubmissionBackendBasis,
    pub(crate) implementation: SubmissionBackendImplementation,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionBackendSelectionError {
    /// The generation/variant/HAL/role/transport axes contradict each other.
    InconsistentIdentity,
    UnconfirmedTarget,
    BackendNotImplemented,
}

pub(crate) const T8140_G17P_SUBMISSION_BACKEND: SubmissionBackendSelection =
    SubmissionBackendSelection {
        family: SubmissionBackendFamily::G17Hal200Hybrid,
        basis: SubmissionBackendBasis::Established,
        implementation: SubmissionBackendImplementation::T8140G17PFirstPartial,
    };

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionChannelClassFamily {
    /// `AGX{CL,3D,TA}ChannelG16_SKSM`.
    G16Sksm,
    /// `AGX::PI_300::G::A0::{CL,3D,TA}ChannelSKSM`.
    G17GSksm,
    /// `AGX::PI_300::X::A0::{CL,3D,TA}ChannelSKSM`.
    G17XSksm,
}

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionChannelSelection {
    /// Each factory directly allocates its SKSM subclass without consulting
    /// runtime state or branching to a classic SKU subclass.
    FixedSksm,
}

/// Exact class and allocation sizes selected by the three channel factories.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct SubmissionChannelSelectionConfig {
    pub(crate) selection: SubmissionChannelSelection,
    pub(crate) class_family: SubmissionChannelClassFamily,
    pub(crate) cl_object_size: u16,
    pub(crate) three_d_object_size: u16,
    pub(crate) ta_object_size: u16,
}

/// Exact host-reported bit layout used to insert a queue ID in an SKSM kick.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct SksmKickQueueIdConfig {
    pub(crate) shift: u8,
    pub(crate) mask: u8,
}

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionFirmwareRole {
    Gfx,
    Gfx1,
}

/// Exact role and wire word produced for one host firmware kick.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct FirmwareKickRoute {
    pub(crate) role: SubmissionFirmwareRole,
    pub(crate) message: u64,
}

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct FirmwareKickEndpointConfig {
    pub(crate) endpoint_service_id_offset: u16,
    pub(crate) message_endpoint_id: u8,
    pub(crate) message_endpoint_object_offset: u16,
    pub(crate) async_note_endpoint_id: u8,
    pub(crate) async_note_endpoint_object_offset: u16,
    pub(crate) sender_arg2: u8,
    pub(crate) endpoint_send_arg2: u8,
    pub(crate) endpoint_send_arg3: u8,
}

/// Exact host-side ordering around one data-master ring publication.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum DataMasterRingPublicationStep {
    /// Select the slot at the current write index after checking for full.
    SelectNextEntry,
    /// The opaque, target-specific command entry has been fully encoded.
    RecordEncoded,
    /// Execute the host's inner-shareable data memory barrier.
    DmbIsh,
    /// Read the current shared write index again.
    ReadWriteIndex,
    /// Increment the write index and truncate it to eight bits.
    IncrementAndWrap8,
    /// Store the new shared write index before notifying firmware.
    StoreWriteIndex,
}

/// Exact host-side storage used for the three data-master ring indices.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum DataMasterRingIndexBacking {
    FirmwareSharedData {
        resource_owner_object_offset: u16,
        cpu_object_offset: u8,
        firmware_object_offset: u8,
        read_index_offset: u8,
        cfi_index_offset: u8,
        write_index_offset: u8,
        slice_size: u8,
        caching_options: u8,
        allocate_shared_data_arg: bool,
        batch_ring_count: u8,
        release_zeroes_owner_slot: bool,
    },
    FenderScratchRam {
        scratch_object_offset: u8,
        read_cpu_pointer_offset: u8,
        cfi_cpu_pointer_offset: u8,
        write_cpu_pointer_offset: u8,
        read_firmware_pointer_offset: u8,
        cfi_firmware_pointer_offset: u8,
        write_firmware_pointer_offset: u8,
        entries_write_allocator_offset: u8,
        read_cfi_allocator_offset: u8,
        index_allocation_size: u8,
        entries_write_mapping_options: u32,
        read_cfi_mapping_options: u32,
        cleanup_frees_allocations_proven: bool,
    },
}

/// Exact construction path for the 256 data-master ring entries.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum DataMasterRingEntryBacking {
    FirmwareSharedData {
        resource_owner_object_offset: u16,
        slice_size: u16,
        caching_options: u8,
        allocate_shared_data_arg: bool,
        batch_ring_count: u8,
        release_zeroes_owner_slot: bool,
    },
    FenderScratchRam {
        allocator_offset: u8,
        allocation_size: u16,
        mapping_options: u32,
        cleanup_frees_allocation_proven: bool,
    },
}

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum DataMasterRingHostIndexMutation {
    /// Read, CFI, and write setters all store directly into shared data.
    DirectSharedStores,
    /// Read/CFI setters trap; the write setter stores while holding the
    /// Fender ScratchRAM lock.
    LockedScratchWriteOnly,
}

/// Exact host-visible geometry and publication contract of a data-master ring.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct DataMasterRingConfig {
    pub(crate) index_backing: DataMasterRingIndexBacking,
    pub(crate) entry_backing: DataMasterRingEntryBacking,
    pub(crate) entries_cpu_object_offset: u8,
    pub(crate) entries_firmware_object_offset: u8,
    pub(crate) host_index_mutation: DataMasterRingHostIndexMutation,
    pub(crate) entry_stride: u8,
    pub(crate) index_count: u16,
    pub(crate) usable_entry_count: u16,
    pub(crate) cold_init_ring_count: u8,
    pub(crate) shared_data_cacheability_names_proven: bool,
    pub(crate) firmware_consumer_ownership_proven: bool,
    pub(crate) publication_steps: [DataMasterRingPublicationStep; 6],
}

/// Pure result of applying the host's full/wrap calculation to one ring state.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct DataMasterRingNextEntry {
    /// Byte offset from the backing pointer loaded at ring object +0x18.
    pub(crate) entry_offset_from_entries_base: u16,
    pub(crate) published_write_index: u8,
}

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum DataMasterClass {
    Ta,
    ThreeD,
    Cl,
}

/// Host index handoff performed before publishing firmware ring pointers.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum DataMasterFirmwareIndexHandoff {
    /// For each priority, copy each ring's write index to read, then copy each
    /// ring's write index to CFI.
    MirrorReadAndCfiToWrite,
    NoHostReadOrCfiStore,
}

/// Exact host-side publication layout for firmware data-master ring pointers.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct DataMasterFirmwareTableConfig {
    pub(crate) descriptor_table_cpu_pointer_object_offset: u16,
    pub(crate) ta_ring_object_base_offset: u16,
    pub(crate) three_d_ring_object_base_offset: u16,
    pub(crate) cl_ring_object_base_offset: u16,
    pub(crate) ring_object_stride: u8,
    pub(crate) priority_count: u8,
    pub(crate) data_master_count: u8,
    pub(crate) descriptor_record_stride: u8,
    pub(crate) read_index_firmware_pointer_offset: u8,
    pub(crate) cfi_index_firmware_pointer_offset: u8,
    pub(crate) write_index_firmware_pointer_offset: u8,
    pub(crate) entries_firmware_pointer_offset: u8,
    pub(crate) index_handoff: DataMasterFirmwareIndexHandoff,
    pub(crate) firmware_read_index_acknowledgment_writer_proven: bool,
    pub(crate) dual_stack_runtime_selector_proven: bool,
}

#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct DataMasterFirmwareRingRoute {
    pub(crate) encoder_data_master_ordinal: u8,
    pub(crate) ring_object_offset: u16,
    pub(crate) descriptor_record_offset: u16,
    pub(crate) read_index_firmware_pointer_offset: u16,
    pub(crate) cfi_index_firmware_pointer_offset: u16,
    pub(crate) write_index_firmware_pointer_offset: u16,
    pub(crate) entries_firmware_pointer_offset: u16,
}

/// Data-master order used by the pinned G17S firmware recovery drain.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum G17sFirmwareRecoveryDataMaster {
    Ta,
    ThreeD,
    Compute,
}

/// Exact direct ordering of the recovery-status stores and barriers.
///
/// `RecoveryBody` deliberately does not flatten barriers in called helpers.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum G17sFirmwareRecoveryStatusStep {
    StoreActive,
    DsbSy,
    RecoveryBody,
    StoreInactive,
}

/// Exact G17S firmware-only recovery drain/discard contract.
///
/// This describes recovery handling, not normal submission consumption or
/// acknowledgment.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sFirmwareRecoveryDrainConfig {
    pub(crate) handler_text_offset: u32,
    pub(crate) runtime_pointer_global_image_offset: u32,
    pub(crate) recovery_status_pointer_global_image_offset: u32,
    pub(crate) recovery_status_field_offset: u8,
    pub(crate) recovery_status_active_value: u32,
    pub(crate) recovery_status_inactive_value: u32,
    pub(crate) descriptor_table_offset: u8,
    pub(crate) priority_count: u8,
    pub(crate) data_master_count: u8,
    pub(crate) priority_stride: u8,
    pub(crate) descriptor_record_stride: u8,
    pub(crate) read_index_pointer_offset: u8,
    pub(crate) cfi_index_pointer_offset: u8,
    pub(crate) write_index_pointer_offset: u8,
    pub(crate) entries_pointer_offset: u8,
    pub(crate) entry_stride: u8,
    pub(crate) read_index_mask: u8,
    pub(crate) maximum_read_advancements_per_master: u8,
    pub(crate) cfi_initialized_from_write_index: bool,
    pub(crate) cfi_initialization_order: [G17sFirmwareRecoveryDataMaster; 3],
    pub(crate) drain_attempt_order: [G17sFirmwareRecoveryDataMaster; 3],
    pub(crate) expected_entry_type_ordinals: [u8; 3],
    pub(crate) direct_status_steps: [G17sFirmwareRecoveryStatusStep; 5],
    pub(crate) normal_submission_acknowledgment_proven: bool,
}

/// Pure descriptor route for one G17S recovery priority/data-master pair.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sFirmwareRecoveryDrainRoute {
    pub(crate) record_offset_from_runtime_base: u16,
    pub(crate) read_index_pointer_offset_from_runtime_base: u16,
    pub(crate) cfi_index_pointer_offset_from_runtime_base: u16,
    pub(crate) write_index_pointer_offset_from_runtime_base: u16,
    pub(crate) entries_pointer_offset_from_runtime_base: u16,
    pub(crate) expected_entry_type_ordinal: u8,
}

/// Exact per-entry order in the pinned G17S SKSM completion consumer.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum G17sSksmCompletionEntryStep {
    ValidateEntry,
    HandleIfAccepted,
    AdvanceReadIndexModuloCapacity,
}

/// Exact G17S SKSM completion descriptor and consumer contract.
///
/// This is separate from the classic submission rings and does not prove
/// normal classic-ring firmware acknowledgment.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sSksmCompletionConsumerConfig {
    pub(crate) consumer_text_offset: u32,
    pub(crate) entry_validator_text_offset: u32,
    pub(crate) entry_handler_text_offset: u32,
    pub(crate) cache_clean_invalidate_text_offset: u32,
    pub(crate) descriptor_table_offset: u16,
    pub(crate) host_resource_table_accelerator_offset: u32,
    pub(crate) host_capacity_accelerator_offset: u32,
    pub(crate) host_present_pointer_source_subobject_offset: u8,
    pub(crate) host_entries_pointer_source_subobject_offset: u8,
    pub(crate) host_initial_read_index: u32,
    pub(crate) host_resource_count: u8,
    pub(crate) firmware_consumed_descriptor_count: u8,
    pub(crate) descriptor_record_stride: u8,
    pub(crate) present_pointer_offset: u8,
    pub(crate) entries_pointer_offset: u8,
    pub(crate) capacity_offset: u8,
    pub(crate) entry_stride_offset: u8,
    pub(crate) mmio_selector_offset: u8,
    pub(crate) read_index_offset: u8,
    pub(crate) host_entry_stride: u8,
    pub(crate) host_selector_by_record: [u8; 4],
    pub(crate) firmware_record_order: [u8; 3],
    pub(crate) producer_mmio_base_offset: u32,
    pub(crate) processed_count_mmio_base_offset: u32,
    pub(crate) mmio_selector_stride: u8,
    pub(crate) minimum_raw_producer_value: u32,
    pub(crate) producer_count_shift: u8,
    pub(crate) processed_count_mmio_store_width: u8,
    pub(crate) supported_modes: [u8; 2],
    pub(crate) entry_steps: [G17sSksmCompletionEntryStep; 3],
    pub(crate) cache_clean_invalidate_precedes_entry_reads: bool,
    pub(crate) handler_return_precedes_read_index_store: bool,
    pub(crate) current_descriptor_work_precedes_its_mmio_publication: bool,
    pub(crate) direct_barrier_before_mmio_publication: bool,
    pub(crate) mmio_publication_is_release_store: bool,
    pub(crate) host_prepare_mappings_covers_all_resources: bool,
    pub(crate) host_release_zeroes_all_resource_slots: bool,
    pub(crate) normal_classic_submission_acknowledgment_proven: bool,
}

/// Pure route for one descriptor visited by the G17S SKSM completion consumer.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sSksmCompletionRoute {
    pub(crate) descriptor_record: u8,
    pub(crate) descriptor_offset: u8,
    pub(crate) mmio_selector: u8,
    pub(crate) producer_mmio_offset: u32,
    pub(crate) processed_count_mmio_offset: u32,
}

/// Exact G17S firmware order for publishing one host event.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum G17sFirmwareEventProducerStep {
    LoadSharedWriteIndex,
    AdvanceAndWrap8,
    WaitUntilNextDiffersFromSharedRead,
    ReloadSharedWriteIndex,
    CopyEntry,
    DsbSyBeforeWriteIndex,
    PublishSharedWriteIndex,
    DsbSyBeforeNotification,
    NotifyHost,
}

/// Role-specific entry points and runtime globals in the pinned G17S bundle.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sFirmwareEventProducerRoleConfig {
    pub(crate) role: SubmissionFirmwareRole,
    pub(crate) enqueue_text_offset: u32,
    pub(crate) runtime_pointer_global_image_offset: u32,
    pub(crate) notification_send_text_offset: u32,
}

/// Exact G17S firmware-event producer and host interrupt bridge.
///
/// This proves how an already-formed event reaches the host event consumer.
/// It does not prove that the SKSM processed-count store creates such an event.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sFirmwareEventProducerConfig {
    pub(crate) roles: [G17sFirmwareEventProducerRoleConfig; 2],
    pub(crate) ring_pointer_pair_offset: u16,
    pub(crate) shared_indices_pointer_pair_offset: u8,
    pub(crate) entries_pointer_pair_offset: u8,
    pub(crate) shared_read_index_offset: u8,
    pub(crate) shared_write_index_offset: u8,
    pub(crate) entry_stride: u8,
    pub(crate) copied_entry_bytes: u8,
    pub(crate) index_mask: u8,
    pub(crate) notification_endpoint_service_id: u8,
    pub(crate) notification_message: u64,
    pub(crate) notification_message_type_shift: u8,
    pub(crate) notification_message_type_mask: u8,
    pub(crate) notification_argument2: u64,
    pub(crate) rtbuddy_received_message_text_offset: u32,
    pub(crate) host_received_message_text_offset: u32,
    pub(crate) host_message_type: u8,
    pub(crate) host_event_source_setup_text_offset: u32,
    pub(crate) host_event_source_array_offset: u16,
    pub(crate) host_interrupt_selector_offset: u16,
    pub(crate) host_interrupt_indices: [u8; 2],
    pub(crate) accelerator_firmware_pointer_offset: u16,
    pub(crate) accelerator_handle_interrupt_text_offset: u32,
    pub(crate) firmware_handle_event_text_offset: u32,
    pub(crate) steps: [G17sFirmwareEventProducerStep; 9],
    pub(crate) direct_cache_maintenance_before_publish: bool,
    pub(crate) sksm_processed_count_causal_link_proven: bool,
    pub(crate) normal_classic_submission_acknowledgment_proven: bool,
}

/// Pure producer-side ring slot and next shared write index.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sFirmwareEventProducerNextEntry {
    pub(crate) entry_offset_from_entries_base: u16,
    pub(crate) published_write_index: u8,
}

/// Pure host interrupt route selected by the pinned type-2 notification path.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sFirmwareEventInterruptRoute {
    pub(crate) raw_interrupt_index: u8,
    pub(crate) drains_firmware_event_rings: bool,
}

/// Exact host order for consuming one pinned G17S firmware-event entry.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum G17sHostFirmwareEventStep {
    SnapshotSharedIndices,
    ValidateSnapshotBounds,
    CopyEntry,
    ValidateTypeMask,
    AdvanceReadIndexModuloCapacity,
    DmbIsh,
    PublishSharedReadIndex,
    DispatchIfSupported,
}

/// Exact G17S host firmware-event-ring interrupt and consumer contract.
///
/// This describes the host consumer of firmware events. The firmware-side
/// producer is independently pinned below, but a causal link from SKSM
/// processed-count MMIO to these interrupts is not proved.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sHostFirmwareEventConsumerConfig {
    pub(crate) handle_event_text_offset: u32,
    pub(crate) clear_outstanding_interrupts_text_offset: u32,
    pub(crate) drain_firmware_rings_text_offset: u32,
    pub(crate) drain_all_event_rings_text_offset: u32,
    pub(crate) drain_role_event_ring_text_offset: u32,
    pub(crate) fetch_next_entry_text_offset: u32,
    pub(crate) draining_interrupt_indices: [u8; 2],
    pub(crate) role_order: [SubmissionFirmwareRole; 2],
    pub(crate) role_validator_base_offset: u16,
    pub(crate) role_validator_stride: u16,
    pub(crate) shared_indices_pointer_offset: u8,
    pub(crate) entries_pointer_offset: u8,
    pub(crate) cached_read_index_offset: u8,
    pub(crate) cached_write_index_offset: u8,
    pub(crate) valid_type_mask_offset: u8,
    pub(crate) capacity_offset: u8,
    pub(crate) shared_read_index_offset: u8,
    pub(crate) shared_write_index_offset: u8,
    pub(crate) entry_stride: u8,
    pub(crate) entry_copy_word_count: u8,
    pub(crate) event_type_offset: u8,
    pub(crate) valid_type_mask_bits: u8,
    pub(crate) maximum_dispatched_event_type: u8,
    pub(crate) entry_steps: [G17sHostFirmwareEventStep; 8],
    pub(crate) firmware_event_producer_ordering_proven: bool,
    pub(crate) sksm_processed_count_interrupt_link_proven: bool,
    pub(crate) normal_classic_submission_acknowledgment_proven: bool,
}

/// Pure per-role validator location in the G17S host firmware object.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sHostFirmwareEventRingRoute {
    pub(crate) role: SubmissionFirmwareRole,
    pub(crate) validator_object_offset: u16,
}

/// Pure result of the pinned host's firmware-event fetch and publication.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct G17sHostFirmwareEventNextEntry {
    pub(crate) entry_offset_from_entries_base: u64,
    pub(crate) published_read_index: u32,
    pub(crate) dispatch_event_type: Option<u8>,
    pub(crate) has_more_from_snapshot: bool,
}

/// Refusals made by the target-pinned SKSM submission preflight.
#[allow(dead_code)]
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) enum SubmissionConfigError {
    UnsupportedTarget,
    /// The supplied configuration differs from the target's exact host values.
    UnexpectedSksmKickQueueIdConfig,
    /// The supplied channel selection differs from the target's factory code.
    UnexpectedSubmissionChannelSelectionConfig,
    /// The kick index does not fit the three-bit host field.
    KickIndexOutOfRange,
    /// The opaque firmware message type does not fit the six-bit host field.
    FirmwareMessageTypeOutOfRange,
    /// The supplied endpoint binding differs from the target's exact host values.
    UnexpectedFirmwareKickEndpointConfig,
    /// The supplied data-master ring contract differs from exact host values.
    UnexpectedDataMasterRingConfig,
    /// The supplied firmware ring-table contract differs from exact host values.
    UnexpectedDataMasterFirmwareTableConfig,
    /// The supplied G17S recovery drain differs from the pinned firmware.
    UnexpectedG17sFirmwareRecoveryDrainConfig,
    /// The supplied G17S SKSM completion consumer differs from pinned code.
    UnexpectedG17sSksmCompletionConsumerConfig,
    /// The supplied G17S firmware-event producer differs from pinned code.
    UnexpectedG17sFirmwareEventProducerConfig,
    /// The supplied G17S host firmware-event consumer differs from pinned code.
    UnexpectedG17sHostFirmwareEventConsumerConfig,
    /// The priority is outside the four published data-master ring groups.
    DataMasterPriorityOutOfRange,
    /// The priority is outside the four G17S firmware recovery groups.
    G17sFirmwareRecoveryPriorityOutOfRange,
    /// The ordinal is outside the three G17S completion descriptors consumed.
    G17sSksmCompletionDescriptorOutOfRange,
    /// The G17S SKSM completion descriptor has an invalid zero capacity.
    G17sSksmCompletionCapacityZero,
    /// The low producer/status word does not indicate a completion entry.
    G17sSksmCompletionProducerNotReady,
    /// The G17S host firmware-event ring has an invalid zero capacity.
    G17sHostFirmwareEventCapacityZero,
    /// The G17S host firmware-event read index is outside the ring capacity.
    G17sHostFirmwareEventReadIndexOutOfRange,
    /// The G17S host firmware-event write index is outside the ring capacity.
    G17sHostFirmwareEventWriteIndexOutOfRange,
    /// The G17S host firmware-event ring has no snapshotted entry to consume.
    G17sHostFirmwareEventRingEmpty,
    /// The event-type bit selected by the host's 64-bit mask is disabled.
    G17sHostFirmwareEventTypeDisabled,
    /// The firmware-event producer's shared read index is outside 8 bits.
    G17sFirmwareEventReadIndexOutOfRange,
    /// The firmware-event producer's shared write index is outside 8 bits.
    G17sFirmwareEventWriteIndexOutOfRange,
    /// The next firmware-event write index would collide with shared read.
    G17sFirmwareEventRingFull,
    /// The host bridge selector is neither recovered interrupt index 0 nor 4.
    G17sFirmwareEventInterruptIndexUnsupported,
    /// The shared read index is outside the host's eight-bit ring index space.
    DataMasterReadIndexOutOfRange,
    /// The shared write index is outside the host's eight-bit ring index space.
    DataMasterWriteIndexOutOfRange,
    /// Advancing the write index would collide with the shared read index.
    DataMasterRingFull,
}

/// A pure submission configuration validator is available for the exact
/// G16X/G17 host targets below.
#[allow(dead_code)]
pub(crate) const GROUNDED_SKSM_KICK_QUEUE_ID_CONFIG_AVAILABLE: bool = true;
#[allow(dead_code)]
pub(crate) const GROUNDED_SUBMISSION_CHANNEL_SELECTION_CONFIG_AVAILABLE: bool = true;
/// A pure endpoint-binding validator is available for every target supported
/// by the firmware-kick word encoder.
#[allow(dead_code)]
pub(crate) const GROUNDED_FIRMWARE_KICK_ENDPOINT_CONFIG_AVAILABLE: bool = true;
/// A pure data-master ring geometry and publication validator is available for
/// every target supported by the firmware-kick word encoder.
#[allow(dead_code)]
pub(crate) const GROUNDED_DATA_MASTER_RING_CONFIG_AVAILABLE: bool = true;
/// A pure firmware ring-table publication and classic class/priority selector
/// is available for the same independently pinned G15-G17 host corpus.
#[allow(dead_code)]
pub(crate) const GROUNDED_DATA_MASTER_FIRMWARE_TABLE_CONFIG_AVAILABLE: bool = true;
/// A pure model of the G17S firmware recovery drain/discard writer is
/// available. It is deliberately not a normal submission acknowledgment.
#[allow(dead_code)]
pub(crate) const GROUNDED_G17S_FIRMWARE_RECOVERY_DRAIN_CONFIG_AVAILABLE: bool = true;
/// A pure model of the distinct G17S SKSM completion consumer is available.
/// It does not establish classic submission-ring acknowledgment.
#[allow(dead_code)]
pub(crate) const GROUNDED_G17S_SKSM_COMPLETION_CONSUMER_CONFIG_AVAILABLE: bool = true;
/// A pure model of the two pinned G17S firmware-event producers and their
/// type-2 host interrupt bridge is available. It does not connect SKSM's
/// processed-count store to event production.
#[allow(dead_code)]
pub(crate) const GROUNDED_G17S_FIRMWARE_EVENT_PRODUCER_CONFIG_AVAILABLE: bool = true;
/// A pure model of the distinct G17S host firmware-event consumer is
/// available. Its producer is pinned, while the SKSM-completion causal link
/// remains unproved.
#[allow(dead_code)]
pub(crate) const GROUNDED_G17S_HOST_FIRMWARE_EVENT_CONSUMER_CONFIG_AVAILABLE: bool = true;
/// The symbolic meaning of the firmware shared-data caching-option numbers,
/// firmware consumer ownership, and transport selection on dual-stack targets
/// are not proved, so command submission must remain disconnected from these
/// helpers.
#[allow(dead_code)]
pub(crate) const EXECUTABLE_G15_G17_SUBMISSION_AVAILABLE: bool = false;

const SKSM_KICK_QUEUE_ID_CONFIG: SksmKickQueueIdConfig = SksmKickQueueIdConfig {
    shift: 40,
    mask: 0x7f,
};
const SUBMISSION_CHANNEL_SELECTION_G16_SKSM: SubmissionChannelSelectionConfig =
    SubmissionChannelSelectionConfig {
        selection: SubmissionChannelSelection::FixedSksm,
        class_family: SubmissionChannelClassFamily::G16Sksm,
        cl_object_size: 0x1d0,
        three_d_object_size: 0x1d8,
        ta_object_size: 0x1e0,
    };
const SUBMISSION_CHANNEL_SELECTION_G17G_SKSM: SubmissionChannelSelectionConfig =
    SubmissionChannelSelectionConfig {
        selection: SubmissionChannelSelection::FixedSksm,
        class_family: SubmissionChannelClassFamily::G17GSksm,
        cl_object_size: 0x1d0,
        three_d_object_size: 0x1d8,
        ta_object_size: 0x1e0,
    };
const SUBMISSION_CHANNEL_SELECTION_G17X_SKSM: SubmissionChannelSelectionConfig =
    SubmissionChannelSelectionConfig {
        selection: SubmissionChannelSelection::FixedSksm,
        class_family: SubmissionChannelClassFamily::G17XSksm,
        cl_object_size: 0x1d0,
        three_d_object_size: 0x1d8,
        ta_object_size: 0x1e0,
    };
const FIRMWARE_KICK_TAG: u64 = 0x0080_0000_0000_0000;
const FIRMWARE_KICK_INDEX_MASK: u8 = 0x07;
const FIRMWARE_KICK_INDEX_SHIFT: u32 = 2;
const FIRMWARE_MESSAGE_TYPE_MASK: u8 = 0x3f;
const FIRMWARE_MESSAGE_TYPE_SHIFT: u32 = 48;
const G17_GFX1_FIRMWARE_MESSAGE_TYPE: u8 = 0x07;
const FIRMWARE_KICK_ENDPOINT_CONFIG: FirmwareKickEndpointConfig = FirmwareKickEndpointConfig {
    endpoint_service_id_offset: 0x88,
    message_endpoint_id: 0x20,
    message_endpoint_object_offset: 0x128,
    async_note_endpoint_id: 0x21,
    async_note_endpoint_object_offset: 0x130,
    sender_arg2: 0,
    endpoint_send_arg2: 0,
    endpoint_send_arg3: 1,
};
const DATA_MASTER_RING_PUBLICATION_STEPS: [DataMasterRingPublicationStep; 6] = [
    DataMasterRingPublicationStep::SelectNextEntry,
    DataMasterRingPublicationStep::RecordEncoded,
    DataMasterRingPublicationStep::DmbIsh,
    DataMasterRingPublicationStep::ReadWriteIndex,
    DataMasterRingPublicationStep::IncrementAndWrap8,
    DataMasterRingPublicationStep::StoreWriteIndex,
];

const G17S_FIRMWARE_RECOVERY_DRAIN_CONFIG: G17sFirmwareRecoveryDrainConfig =
    G17sFirmwareRecoveryDrainConfig {
        handler_text_offset: 0x2649c,
        runtime_pointer_global_image_offset: 0x104888,
        recovery_status_pointer_global_image_offset: 0x1047b8,
        recovery_status_field_offset: 0x0c,
        recovery_status_active_value: 1,
        recovery_status_inactive_value: 0,
        descriptor_table_offset: 0x20,
        priority_count: 4,
        data_master_count: 3,
        priority_stride: 0x60,
        descriptor_record_stride: 0x20,
        read_index_pointer_offset: 0x00,
        cfi_index_pointer_offset: 0x08,
        write_index_pointer_offset: 0x10,
        entries_pointer_offset: 0x18,
        entry_stride: 0x18,
        read_index_mask: 0xff,
        maximum_read_advancements_per_master: 0x14,
        cfi_initialized_from_write_index: true,
        cfi_initialization_order: [
            G17sFirmwareRecoveryDataMaster::Ta,
            G17sFirmwareRecoveryDataMaster::ThreeD,
            G17sFirmwareRecoveryDataMaster::Compute,
        ],
        drain_attempt_order: [
            G17sFirmwareRecoveryDataMaster::Ta,
            G17sFirmwareRecoveryDataMaster::ThreeD,
            G17sFirmwareRecoveryDataMaster::Compute,
        ],
        expected_entry_type_ordinals: [0, 1, 2],
        direct_status_steps: [
            G17sFirmwareRecoveryStatusStep::StoreActive,
            G17sFirmwareRecoveryStatusStep::DsbSy,
            G17sFirmwareRecoveryStatusStep::RecoveryBody,
            G17sFirmwareRecoveryStatusStep::StoreInactive,
            G17sFirmwareRecoveryStatusStep::DsbSy,
        ],
        normal_submission_acknowledgment_proven: false,
    };

const G17S_SKSM_COMPLETION_CONSUMER_CONFIG: G17sSksmCompletionConsumerConfig =
    G17sSksmCompletionConsumerConfig {
        consumer_text_offset: 0xe730,
        entry_validator_text_offset: 0xbd44,
        entry_handler_text_offset: 0xbfa8,
        cache_clean_invalidate_text_offset: 0x2bf24,
        descriptor_table_offset: 0x2714,
        host_resource_table_accelerator_offset: 0x1e5b8,
        host_capacity_accelerator_offset: 0x11228,
        host_present_pointer_source_subobject_offset: 0x58,
        host_entries_pointer_source_subobject_offset: 0x50,
        host_initial_read_index: 0,
        host_resource_count: 4,
        firmware_consumed_descriptor_count: 3,
        descriptor_record_stride: 0x20,
        present_pointer_offset: 0x00,
        entries_pointer_offset: 0x08,
        capacity_offset: 0x10,
        entry_stride_offset: 0x14,
        mmio_selector_offset: 0x18,
        read_index_offset: 0x1c,
        host_entry_stride: 0x40,
        host_selector_by_record: [0, 1, 2, 3],
        firmware_record_order: [1, 3, 0],
        producer_mmio_base_offset: 0x21168,
        processed_count_mmio_base_offset: 0x21188,
        mmio_selector_stride: 8,
        minimum_raw_producer_value: 0x1_0000,
        producer_count_shift: 16,
        processed_count_mmio_store_width: 8,
        supported_modes: [0, 2],
        entry_steps: [
            G17sSksmCompletionEntryStep::ValidateEntry,
            G17sSksmCompletionEntryStep::HandleIfAccepted,
            G17sSksmCompletionEntryStep::AdvanceReadIndexModuloCapacity,
        ],
        cache_clean_invalidate_precedes_entry_reads: true,
        handler_return_precedes_read_index_store: true,
        current_descriptor_work_precedes_its_mmio_publication: true,
        direct_barrier_before_mmio_publication: false,
        mmio_publication_is_release_store: false,
        host_prepare_mappings_covers_all_resources: true,
        host_release_zeroes_all_resource_slots: true,
        normal_classic_submission_acknowledgment_proven: false,
    };

const G17S_FIRMWARE_EVENT_PRODUCER_CONFIG: G17sFirmwareEventProducerConfig =
    G17sFirmwareEventProducerConfig {
        roles: [
            G17sFirmwareEventProducerRoleConfig {
                role: SubmissionFirmwareRole::Gfx,
                enqueue_text_offset: 0x23560,
                runtime_pointer_global_image_offset: 0x104888,
                notification_send_text_offset: 0x35a90,
            },
            G17sFirmwareEventProducerRoleConfig {
                role: SubmissionFirmwareRole::Gfx1,
                enqueue_text_offset: 0x290f0,
                runtime_pointer_global_image_offset: 0x100e08,
                notification_send_text_offset: 0x3c804,
            },
        ],
        ring_pointer_pair_offset: 0x1c0,
        shared_indices_pointer_pair_offset: 0x00,
        entries_pointer_pair_offset: 0x08,
        shared_read_index_offset: 0x00,
        shared_write_index_offset: 0x20,
        entry_stride: 0x48,
        copied_entry_bytes: 0x48,
        index_mask: 0xff,
        notification_endpoint_service_id: 0x20,
        notification_message: 0x0042_0000_0000_0000,
        notification_message_type_shift: 48,
        notification_message_type_mask: 0x3f,
        notification_argument2: 0,
        rtbuddy_received_message_text_offset: 0x2430,
        host_received_message_text_offset: 0x5f2a0,
        host_message_type: 2,
        host_event_source_setup_text_offset: 0x150bc,
        host_event_source_array_offset: 0x5b8,
        host_interrupt_selector_offset: 0x72c,
        host_interrupt_indices: [0, 4],
        accelerator_firmware_pointer_offset: 0x598,
        accelerator_handle_interrupt_text_offset: 0x757c,
        firmware_handle_event_text_offset: 0x4c388,
        steps: [
            G17sFirmwareEventProducerStep::LoadSharedWriteIndex,
            G17sFirmwareEventProducerStep::AdvanceAndWrap8,
            G17sFirmwareEventProducerStep::WaitUntilNextDiffersFromSharedRead,
            G17sFirmwareEventProducerStep::ReloadSharedWriteIndex,
            G17sFirmwareEventProducerStep::CopyEntry,
            G17sFirmwareEventProducerStep::DsbSyBeforeWriteIndex,
            G17sFirmwareEventProducerStep::PublishSharedWriteIndex,
            G17sFirmwareEventProducerStep::DsbSyBeforeNotification,
            G17sFirmwareEventProducerStep::NotifyHost,
        ],
        direct_cache_maintenance_before_publish: false,
        sksm_processed_count_causal_link_proven: false,
        normal_classic_submission_acknowledgment_proven: false,
    };

const G17S_HOST_FIRMWARE_EVENT_CONSUMER_CONFIG: G17sHostFirmwareEventConsumerConfig =
    G17sHostFirmwareEventConsumerConfig {
        handle_event_text_offset: 0x4c388,
        clear_outstanding_interrupts_text_offset: 0x7f8a8,
        drain_firmware_rings_text_offset: 0x4c658,
        drain_all_event_rings_text_offset: 0x48fd4,
        drain_role_event_ring_text_offset: 0x49014,
        fetch_next_entry_text_offset: 0x5e82c,
        draining_interrupt_indices: [0, 4],
        role_order: [SubmissionFirmwareRole::Gfx, SubmissionFirmwareRole::Gfx1],
        role_validator_base_offset: 0x7e0,
        role_validator_stride: 0x240,
        shared_indices_pointer_offset: 0x08,
        entries_pointer_offset: 0x10,
        cached_read_index_offset: 0x18,
        cached_write_index_offset: 0x1c,
        valid_type_mask_offset: 0x20,
        capacity_offset: 0x28,
        shared_read_index_offset: 0x00,
        shared_write_index_offset: 0x20,
        entry_stride: 0x48,
        entry_copy_word_count: 18,
        event_type_offset: 0x00,
        valid_type_mask_bits: 64,
        maximum_dispatched_event_type: 0x0f,
        entry_steps: [
            G17sHostFirmwareEventStep::SnapshotSharedIndices,
            G17sHostFirmwareEventStep::ValidateSnapshotBounds,
            G17sHostFirmwareEventStep::CopyEntry,
            G17sHostFirmwareEventStep::ValidateTypeMask,
            G17sHostFirmwareEventStep::AdvanceReadIndexModuloCapacity,
            G17sHostFirmwareEventStep::DmbIsh,
            G17sHostFirmwareEventStep::PublishSharedReadIndex,
            G17sHostFirmwareEventStep::DispatchIfSupported,
        ],
        firmware_event_producer_ordering_proven: true,
        sksm_processed_count_interrupt_link_proven: false,
        normal_classic_submission_acknowledgment_proven: false,
    };

const fn make_data_master_firmware_table_config(
    descriptor_table_cpu_pointer_object_offset: u16,
    ta_ring_object_base_offset: u16,
    three_d_ring_object_base_offset: u16,
    cl_ring_object_base_offset: u16,
    ring_object_stride: u8,
    index_handoff: DataMasterFirmwareIndexHandoff,
) -> DataMasterFirmwareTableConfig {
    DataMasterFirmwareTableConfig {
        descriptor_table_cpu_pointer_object_offset,
        ta_ring_object_base_offset,
        three_d_ring_object_base_offset,
        cl_ring_object_base_offset,
        ring_object_stride,
        priority_count: 4,
        data_master_count: 3,
        descriptor_record_stride: 0x20,
        read_index_firmware_pointer_offset: 0x00,
        cfi_index_firmware_pointer_offset: 0x08,
        write_index_firmware_pointer_offset: 0x10,
        entries_firmware_pointer_offset: 0x18,
        index_handoff,
        firmware_read_index_acknowledgment_writer_proven: false,
        dual_stack_runtime_selector_proven: false,
    }
}

const fn data_master_ring_shared_indices(
    resource_owner_object_offset: u16,
) -> DataMasterRingIndexBacking {
    DataMasterRingIndexBacking::FirmwareSharedData {
        resource_owner_object_offset,
        cpu_object_offset: 0x08,
        firmware_object_offset: 0x10,
        read_index_offset: 0x00,
        cfi_index_offset: 0x10,
        write_index_offset: 0x20,
        slice_size: 0x30,
        caching_options: 0,
        allocate_shared_data_arg: false,
        batch_ring_count: 12,
        release_zeroes_owner_slot: true,
    }
}

const fn data_master_ring_shared_entries(
    resource_owner_object_offset: u16,
) -> DataMasterRingEntryBacking {
    DataMasterRingEntryBacking::FirmwareSharedData {
        resource_owner_object_offset,
        slice_size: 0x1800,
        caching_options: 1,
        allocate_shared_data_arg: false,
        batch_ring_count: 12,
        release_zeroes_owner_slot: true,
    }
}

const DATA_MASTER_RING_SCRATCH_INDICES_CLEANUP: DataMasterRingIndexBacking =
    DataMasterRingIndexBacking::FenderScratchRam {
        scratch_object_offset: 0x28,
        read_cpu_pointer_offset: 0x30,
        cfi_cpu_pointer_offset: 0x38,
        write_cpu_pointer_offset: 0x40,
        read_firmware_pointer_offset: 0x48,
        cfi_firmware_pointer_offset: 0x50,
        write_firmware_pointer_offset: 0x58,
        entries_write_allocator_offset: 0x98,
        read_cfi_allocator_offset: 0xa0,
        index_allocation_size: 0x04,
        entries_write_mapping_options: 0x0400_0101,
        read_cfi_mapping_options: 0x0400_1101,
        cleanup_frees_allocations_proven: true,
    };
const DATA_MASTER_RING_SCRATCH_INDICES_NO_PROVEN_CLEANUP: DataMasterRingIndexBacking =
    DataMasterRingIndexBacking::FenderScratchRam {
        scratch_object_offset: 0x28,
        read_cpu_pointer_offset: 0x30,
        cfi_cpu_pointer_offset: 0x38,
        write_cpu_pointer_offset: 0x40,
        read_firmware_pointer_offset: 0x48,
        cfi_firmware_pointer_offset: 0x50,
        write_firmware_pointer_offset: 0x58,
        entries_write_allocator_offset: 0x98,
        read_cfi_allocator_offset: 0xa0,
        index_allocation_size: 0x04,
        entries_write_mapping_options: 0x0400_0101,
        read_cfi_mapping_options: 0x0400_1101,
        cleanup_frees_allocations_proven: false,
    };
const DATA_MASTER_RING_SCRATCH_ENTRIES: DataMasterRingEntryBacking =
    DataMasterRingEntryBacking::FenderScratchRam {
        allocator_offset: 0x98,
        allocation_size: 0x1800,
        mapping_options: 0x0400_0101,
        cleanup_frees_allocation_proven: true,
    };

const fn data_master_ring_shared_config(
    entry_resource_owner_object_offset: u16,
    index_resource_owner_object_offset: u16,
    cold_init_ring_count: u8,
) -> DataMasterRingConfig {
    DataMasterRingConfig {
        index_backing: data_master_ring_shared_indices(index_resource_owner_object_offset),
        entry_backing: data_master_ring_shared_entries(entry_resource_owner_object_offset),
        entries_cpu_object_offset: 0x18,
        entries_firmware_object_offset: 0x20,
        host_index_mutation: DataMasterRingHostIndexMutation::DirectSharedStores,
        entry_stride: 0x18,
        index_count: 0x100,
        usable_entry_count: 0xff,
        cold_init_ring_count,
        shared_data_cacheability_names_proven: false,
        firmware_consumer_ownership_proven: false,
        publication_steps: DATA_MASTER_RING_PUBLICATION_STEPS,
    }
}

const DATA_MASTER_RING_CONFIG_G15G: DataMasterRingConfig =
    data_master_ring_shared_config(0x310, 0x548, 12);
const DATA_MASTER_RING_CONFIG_G16X: DataMasterRingConfig =
    data_master_ring_shared_config(0x350, 0x588, 12);
const DATA_MASTER_RING_CONFIG_G17P_G17X: DataMasterRingConfig =
    data_master_ring_shared_config(0x350, 0x588, 24);
const DATA_MASTER_RING_CONFIG_G17G: DataMasterRingConfig =
    data_master_ring_shared_config(0x358, 0x590, 24);
const DATA_MASTER_RING_CONFIG_SCRATCH_ENTRIES: DataMasterRingConfig = DataMasterRingConfig {
    index_backing: DATA_MASTER_RING_SCRATCH_INDICES_CLEANUP,
    entry_backing: DATA_MASTER_RING_SCRATCH_ENTRIES,
    entries_cpu_object_offset: 0x18,
    entries_firmware_object_offset: 0x20,
    host_index_mutation: DataMasterRingHostIndexMutation::LockedScratchWriteOnly,
    entry_stride: 0x18,
    index_count: 0x100,
    usable_entry_count: 0xff,
    cold_init_ring_count: 12,
    shared_data_cacheability_names_proven: false,
    firmware_consumer_ownership_proven: false,
    publication_steps: DATA_MASTER_RING_PUBLICATION_STEPS,
};
const DATA_MASTER_RING_CONFIG_SCRATCH_INDICES: DataMasterRingConfig = DataMasterRingConfig {
    index_backing: DATA_MASTER_RING_SCRATCH_INDICES_NO_PROVEN_CLEANUP,
    entry_backing: data_master_ring_shared_entries(0x318),
    host_index_mutation: DataMasterRingHostIndexMutation::LockedScratchWriteOnly,
    ..DATA_MASTER_RING_CONFIG_G16X
};

const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G15G: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0x800,
        0x368,
        0x408,
        0x4a8,
        0x28,
        DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G15S: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0xbc0,
        0x368,
        0x548,
        0x728,
        0x78,
        DataMasterFirmwareIndexHandoff::NoHostReadOrCfiStore,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G15C_D: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0xbc0,
        0x368,
        0x548,
        0x728,
        0x78,
        DataMasterFirmwareIndexHandoff::NoHostReadOrCfiStore,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G16G: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0xbc8,
        0x370,
        0x550,
        0x730,
        0x78,
        DataMasterFirmwareIndexHandoff::NoHostReadOrCfiStore,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G16X: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0x840,
        0x3a8,
        0x448,
        0x4e8,
        0x28,
        DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G17P: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0xa80,
        0x3a8,
        0x448,
        0x4e8,
        0x28,
        DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G17G: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0xa88,
        0x3b0,
        0x450,
        0x4f0,
        0x28,
        DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
    );
const DATA_MASTER_FIRMWARE_TABLE_CONFIG_G17X: DataMasterFirmwareTableConfig =
    make_data_master_firmware_table_config(
        0xa80,
        0x3a8,
        0x448,
        0x4e8,
        0x28,
        DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
    );

/// Identity and userspace-visible architecture axes decoded from GPU ID MMIO.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct GpuIdentity {
    pub(crate) gpu_gen: GpuGen,
    pub(crate) gpu_variant: GpuVariant,
    pub(crate) usc_generation: u32,
    pub(crate) gpu_hal_generation: GpuHalGeneration,
    pub(crate) firmware_roles: FirmwareRoleTopology,
    pub(crate) submission_transport: SubmissionTransport,
    /// Input-address bits per UAT root.
    pub(crate) uat_input_address_bits: u8,
}

/// Decode the hardware family/variant/die tuple reported at 0xd04000/0xd04010.
///
/// This deliberately does not imply that firmware, initdata, command
/// submission, or a core-mask register layout is implemented for the returned
/// identity.
pub(crate) fn decode_gpu_identity(family: u8, variant: u8, num_dies: u8) -> Option<GpuIdentity> {
    use GpuGen::*;
    use GpuHalGeneration::*;
    use GpuVariant::*;

    let (gpu_gen, gpu_variant, usc_generation, gpu_hal_generation) = match (family, variant) {
        (0x4, 0) => (G13, P, 2, Legacy),
        (0x4, 2) => (G13, G, 2, Legacy),
        (0x4, 3) => (G13, S, 2, Legacy),
        // G13 has no D core identity; C covers both Max and Ultra.
        (0x4, 4) => (G13, C, 2, Legacy),

        (0x5, 0) => (G14, P, 2, Legacy),
        (0x5, 2) => (G14, G, 2, Legacy),
        (0x6, 3) => (G14, S, 2, Legacy),
        (0x6, 4) => match num_dies {
            1 => (G14, C, 2, Legacy),
            2 => (G14, D, 2, Legacy),
            _ => return None,
        },

        (0x7, 0) => (G15, P, 2, Legacy),
        (0x7, 2) => (G15, G, 3, Legacy),
        (0x7, 3) => (G15, S, 3, Legacy),
        (0x7, 4) => match num_dies {
            1 => (G15, C, 3, Legacy),
            2 => (G15, D, 3, Legacy),
            _ => return None,
        },

        (0x8, 0) => (G16, P, 3, Hal200),

        // Family 0xa crosses the design-generation boundary.
        (0xa, 0) => (G17, P, 3, Hal200),
        (0xa, 2) => (G16, G, 3, Hal200),
        (0xa, 3) => (G16, S, 3, Hal200),
        (0xa, 4) => (G16, C, 3, Hal200),
        (0xa, 5) => (G17, A, 3, Hal200),

        (0xb, 2) => (G17, G, 3, Hal300),
        (0xb, 3) => (G17, S, 3, Hal300),
        (0xb, 4) => (G17, C, 3, Hal300),
        _ => return None,
    };

    let firmware_roles = if gpu_gen == G17 {
        FirmwareRoleTopology::Dual
    } else {
        FirmwareRoleTopology::Single
    };

    let submission_transport = match (family, variant) {
        // G16X and G17P/G17A carry both stacks. The runtime selector is not
        // yet known, so callers must not silently choose one.
        (0xa, 0 | 3 | 4 | 5) => SubmissionTransport::ClassicAndSksm,
        // HAL300 removes the classic SKU channel stack.
        (0xb, 2 | 3 | 4) => SubmissionTransport::Sksm,
        _ => SubmissionTransport::Classic,
    };

    let uat_input_address_bits = if matches!(gpu_gen, G13 | G14) { 39 } else { 42 };

    Some(GpuIdentity {
        gpu_gen,
        gpu_variant,
        usc_generation,
        gpu_hal_generation,
        firmware_roles,
        submission_transport,
        uat_input_address_bits,
    })
}

pub(crate) const fn submission_backend_selection(
    identity: GpuIdentity,
) -> Result<SubmissionBackendSelection, SubmissionBackendSelectionError> {
    use FirmwareRoleTopology::*;
    use GpuGen::*;
    use GpuHalGeneration::*;
    use GpuVariant::*;
    use SubmissionBackendImplementation::*;
    use SubmissionBackendBasis::*;
    use SubmissionTransport::*;

    match identity {
        GpuIdentity {
            gpu_gen: G13,
            gpu_variant: P | G | S | C,
            usc_generation: 2,
            gpu_hal_generation: Legacy,
            firmware_roles: Single,
            submission_transport: Classic,
            uat_input_address_bits: 39,
        }
        | GpuIdentity {
            gpu_gen: G14,
            gpu_variant: P | G | S | C | D,
            usc_generation: 2,
            gpu_hal_generation: Legacy,
            firmware_roles: Single,
            submission_transport: Classic,
            uat_input_address_bits: 39,
        }
        | GpuIdentity {
            gpu_gen: G15,
            gpu_variant: P,
            usc_generation: 2,
            gpu_hal_generation: Legacy,
            firmware_roles: Single,
            submission_transport: Classic,
            uat_input_address_bits: 42,
        }
        | GpuIdentity {
            gpu_gen: G15,
            gpu_variant: G | S | C | D,
            usc_generation: 3,
            gpu_hal_generation: Legacy,
            firmware_roles: Single,
            submission_transport: Classic,
            uat_input_address_bits: 42,
        }
        | GpuIdentity {
            gpu_gen: G16,
            gpu_variant: G,
            usc_generation: 3,
            gpu_hal_generation: Hal200,
            firmware_roles: Single,
            submission_transport: Classic,
            uat_input_address_bits: 42,
        } => Ok(SubmissionBackendSelection {
            family: SubmissionBackendFamily::ClassicSku,
            basis: Established,
            implementation: ExistingClassic,
        }),
        GpuIdentity {
            gpu_gen: G16,
            gpu_variant: S | C,
            usc_generation: 3,
            gpu_hal_generation: Hal200,
            firmware_roles: Single,
            submission_transport: ClassicAndSksm,
            uat_input_address_bits: 42,
        } => Ok(SubmissionBackendSelection {
            family: SubmissionBackendFamily::G16Hal200Sksm,
            basis: Established,
            implementation: NotImplemented,
        }),
        GpuIdentity {
            gpu_gen: G17,
            gpu_variant: P,
            usc_generation: 3,
            gpu_hal_generation: Hal200,
            firmware_roles: Dual,
            submission_transport: ClassicAndSksm,
            uat_input_address_bits: 42,
        } => Ok(T8140_G17P_SUBMISSION_BACKEND),
        GpuIdentity {
            gpu_gen: G17,
            gpu_variant: A,
            usc_generation: 3,
            gpu_hal_generation: Hal200,
            firmware_roles: Dual,
            submission_transport: ClassicAndSksm,
            uat_input_address_bits: 42,
        } => Ok(SubmissionBackendSelection {
            family: SubmissionBackendFamily::G17Hal200Hybrid,
            basis: CrossGenerationDerived,
            implementation: Unconfirmed,
        }),
        GpuIdentity {
            gpu_gen: G17,
            gpu_variant: G | S | C,
            usc_generation: 3,
            gpu_hal_generation: Hal300,
            firmware_roles: Dual,
            submission_transport: Sksm,
            uat_input_address_bits: 42,
        } => Ok(SubmissionBackendSelection {
            family: SubmissionBackendFamily::G17Hal300Sksm,
            basis: Established,
            implementation: NotImplemented,
        }),
        // G16P is represented by the identity decoder, but the recovered
        // factory-family contract does not independently admit it here.
        GpuIdentity {
            gpu_gen: G16,
            gpu_variant: P,
            usc_generation: 3,
            gpu_hal_generation: Hal200,
            firmware_roles: Single,
            submission_transport: Classic,
            uat_input_address_bits: 42,
        } => Err(SubmissionBackendSelectionError::UnconfirmedTarget),
        _ => Err(SubmissionBackendSelectionError::InconsistentIdentity),
    }
}

/// Require a backend that this checkout can actually execute.
pub(crate) const fn require_executable_submission_backend(
    identity: GpuIdentity,
) -> Result<SubmissionBackendSelection, SubmissionBackendSelectionError> {
    let selection = match submission_backend_selection(identity) {
        Ok(selection) => selection,
        Err(error) => return Err(error),
    };
    match selection.implementation {
        SubmissionBackendImplementation::ExistingClassic
        | SubmissionBackendImplementation::T8140G17PFirstPartial => Ok(selection),
        SubmissionBackendImplementation::NotImplemented => {
            Err(SubmissionBackendSelectionError::BackendNotImplemented)
        }
        SubmissionBackendImplementation::Unconfirmed => {
            Err(SubmissionBackendSelectionError::UnconfirmedTarget)
        }
    }
}

#[allow(dead_code)]
pub(crate) const fn submission_channel_selection_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<SubmissionChannelSelectionConfig> {
    use GpuGen::*;
    use GpuVariant::*;

    match (gpu_gen, gpu_variant) {
        (G16, S | C) | (G17, P) => Some(SUBMISSION_CHANNEL_SELECTION_G16_SKSM),
        (G17, G) => Some(SUBMISSION_CHANNEL_SELECTION_G17G_SKSM),
        (G17, S | C) => Some(SUBMISSION_CHANNEL_SELECTION_G17X_SKSM),
        _ => None,
    }
}

/// Validate an externally recovered target channel factory selection.
#[allow(dead_code)]
pub(crate) const fn validate_submission_channel_selection_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: SubmissionChannelSelectionConfig,
) -> Result<SubmissionChannelSelectionConfig, SubmissionConfigError> {
    let Some(expected) = submission_channel_selection_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if !matches!(
        (supplied.selection, expected.selection),
        (
            SubmissionChannelSelection::FixedSksm,
            SubmissionChannelSelection::FixedSksm,
        )
    ) || !matches!(
        (supplied.class_family, expected.class_family),
        (
            SubmissionChannelClassFamily::G16Sksm,
            SubmissionChannelClassFamily::G16Sksm,
        ) | (
            SubmissionChannelClassFamily::G17GSksm,
            SubmissionChannelClassFamily::G17GSksm,
        ) | (
            SubmissionChannelClassFamily::G17XSksm,
            SubmissionChannelClassFamily::G17XSksm,
        )
    ) || supplied.cl_object_size != expected.cl_object_size
        || supplied.three_d_object_size != expected.three_d_object_size
        || supplied.ta_object_size != expected.ta_object_size
    {
        Err(SubmissionConfigError::UnexpectedSubmissionChannelSelectionConfig)
    } else {
        Ok(expected)
    }
}

#[allow(dead_code)]
pub(crate) const fn sksm_kick_queue_id_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<SksmKickQueueIdConfig> {
    use GpuGen::*;
    use GpuVariant::*;

    match (gpu_gen, gpu_variant) {
        (G16, S | C) | (G17, P | G | S | C) => Some(SKSM_KICK_QUEUE_ID_CONFIG),
        _ => None,
    }
}

/// Validate externally recovered SKSM queue-ID properties for one target.
#[allow(dead_code)]
pub(crate) const fn validate_sksm_kick_queue_id_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    shift: u8,
    mask: u8,
) -> Result<SksmKickQueueIdConfig, SubmissionConfigError> {
    let Some(expected) = sksm_kick_queue_id_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if shift != expected.shift || mask != expected.mask {
        Err(SubmissionConfigError::UnexpectedSksmKickQueueIdConfig)
    } else {
        Ok(expected)
    }
}

/// Encode the exact cross-generation host firmware-kick word and role route.
///
/// Pinned G15/G16 hosts always send through their single `GFX` role. Every
/// pinned G17 host instead selects `GFX1` only for opaque message type seven;
/// all other six-bit message types use `GFX`. The destination endpoint and
/// the complete queue publication/notification lifecycle remain deliberately
/// outside this model.
#[allow(dead_code)]
pub(crate) const fn encode_firmware_kick_route(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    kick_index: u8,
    message_type: u8,
) -> Result<FirmwareKickRoute, SubmissionConfigError> {
    use GpuGen::*;
    use GpuVariant::*;

    let target_is_proven = matches!(
        (gpu_gen, gpu_variant),
        (G15, G | S | C | D) | (G16, G | S | C) | (G17, P | G | S | C)
    );
    if !target_is_proven {
        return Err(SubmissionConfigError::UnsupportedTarget);
    }
    if kick_index > FIRMWARE_KICK_INDEX_MASK {
        return Err(SubmissionConfigError::KickIndexOutOfRange);
    }
    if message_type > FIRMWARE_MESSAGE_TYPE_MASK {
        return Err(SubmissionConfigError::FirmwareMessageTypeOutOfRange);
    }

    let role = match (gpu_gen, message_type) {
        (G17, G17_GFX1_FIRMWARE_MESSAGE_TYPE) => SubmissionFirmwareRole::Gfx1,
        _ => SubmissionFirmwareRole::Gfx,
    };
    let message = FIRMWARE_KICK_TAG
        | ((message_type as u64) << FIRMWARE_MESSAGE_TYPE_SHIFT)
        | ((kick_index as u64) << FIRMWARE_KICK_INDEX_SHIFT);

    Ok(FirmwareKickRoute { role, message })
}

#[allow(dead_code)]
pub(crate) const fn firmware_kick_endpoint_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<FirmwareKickEndpointConfig> {
    use GpuGen::*;
    use GpuVariant::*;

    match (gpu_gen, gpu_variant) {
        (G15, G | S | C | D) | (G16, G | S | C) | (G17, P | G | S | C) => {
            Some(FIRMWARE_KICK_ENDPOINT_CONFIG)
        }
        _ => None,
    }
}

/// Validate externally recovered RTBuddy endpoint-binding properties.
#[allow(dead_code)]
pub(crate) const fn validate_firmware_kick_endpoint_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: FirmwareKickEndpointConfig,
) -> Result<FirmwareKickEndpointConfig, SubmissionConfigError> {
    let Some(expected) = firmware_kick_endpoint_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if supplied.endpoint_service_id_offset != expected.endpoint_service_id_offset
        || supplied.message_endpoint_id != expected.message_endpoint_id
        || supplied.message_endpoint_object_offset != expected.message_endpoint_object_offset
        || supplied.async_note_endpoint_id != expected.async_note_endpoint_id
        || supplied.async_note_endpoint_object_offset != expected.async_note_endpoint_object_offset
        || supplied.sender_arg2 != expected.sender_arg2
        || supplied.endpoint_send_arg2 != expected.endpoint_send_arg2
        || supplied.endpoint_send_arg3 != expected.endpoint_send_arg3
    {
        Err(SubmissionConfigError::UnexpectedFirmwareKickEndpointConfig)
    } else {
        Ok(expected)
    }
}

#[allow(dead_code)]
pub(crate) const fn data_master_ring_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<DataMasterRingConfig> {
    use GpuGen::*;
    use GpuVariant::*;

    match (gpu_gen, gpu_variant) {
        (G15, G) => Some(DATA_MASTER_RING_CONFIG_G15G),
        (G15, S | C | D) => Some(DATA_MASTER_RING_CONFIG_SCRATCH_ENTRIES),
        (G16, G) => Some(DATA_MASTER_RING_CONFIG_SCRATCH_INDICES),
        (G16, S | C) => Some(DATA_MASTER_RING_CONFIG_G16X),
        (G17, P | S | C) => Some(DATA_MASTER_RING_CONFIG_G17P_G17X),
        (G17, G) => Some(DATA_MASTER_RING_CONFIG_G17G),
        _ => None,
    }
}

#[allow(dead_code)]
pub(crate) const fn data_master_firmware_table_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<DataMasterFirmwareTableConfig> {
    use GpuGen::*;
    use GpuVariant::*;

    match (gpu_gen, gpu_variant) {
        (G15, G) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G15G),
        (G15, S) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G15S),
        (G15, C | D) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G15C_D),
        (G16, G) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G16G),
        (G16, S | C) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G16X),
        (G17, P) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G17P),
        (G17, G) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G17G),
        (G17, S | C) => Some(DATA_MASTER_FIRMWARE_TABLE_CONFIG_G17X),
        _ => None,
    }
}

const fn data_master_firmware_index_handoff_matches(
    supplied: DataMasterFirmwareIndexHandoff,
    expected: DataMasterFirmwareIndexHandoff,
) -> bool {
    matches!(
        (supplied, expected),
        (
            DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
            DataMasterFirmwareIndexHandoff::MirrorReadAndCfiToWrite,
        ) | (
            DataMasterFirmwareIndexHandoff::NoHostReadOrCfiStore,
            DataMasterFirmwareIndexHandoff::NoHostReadOrCfiStore,
        )
    )
}

/// Validate an externally recovered firmware ring-pointer table contract.
#[allow(dead_code)]
pub(crate) const fn validate_data_master_firmware_table_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: DataMasterFirmwareTableConfig,
) -> Result<DataMasterFirmwareTableConfig, SubmissionConfigError> {
    let Some(expected) = data_master_firmware_table_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if supplied.descriptor_table_cpu_pointer_object_offset
        != expected.descriptor_table_cpu_pointer_object_offset
        || supplied.ta_ring_object_base_offset != expected.ta_ring_object_base_offset
        || supplied.three_d_ring_object_base_offset != expected.three_d_ring_object_base_offset
        || supplied.cl_ring_object_base_offset != expected.cl_ring_object_base_offset
        || supplied.ring_object_stride != expected.ring_object_stride
        || supplied.priority_count != expected.priority_count
        || supplied.data_master_count != expected.data_master_count
        || supplied.descriptor_record_stride != expected.descriptor_record_stride
        || supplied.read_index_firmware_pointer_offset
            != expected.read_index_firmware_pointer_offset
        || supplied.cfi_index_firmware_pointer_offset != expected.cfi_index_firmware_pointer_offset
        || supplied.write_index_firmware_pointer_offset
            != expected.write_index_firmware_pointer_offset
        || supplied.entries_firmware_pointer_offset != expected.entries_firmware_pointer_offset
        || !data_master_firmware_index_handoff_matches(
            supplied.index_handoff,
            expected.index_handoff,
        )
        || supplied.firmware_read_index_acknowledgment_writer_proven
            != expected.firmware_read_index_acknowledgment_writer_proven
        || supplied.dual_stack_runtime_selector_proven
            != expected.dual_stack_runtime_selector_proven
    {
        Err(SubmissionConfigError::UnexpectedDataMasterFirmwareTableConfig)
    } else {
        Ok(expected)
    }
}

/// Select the exact ring object and firmware descriptor record for one class
/// and priority without reading or modifying any shared state.
#[allow(dead_code)]
pub(crate) const fn data_master_firmware_ring_route(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    data_master: DataMasterClass,
    priority: u8,
) -> Result<DataMasterFirmwareRingRoute, SubmissionConfigError> {
    let Some(config) = data_master_firmware_table_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if priority >= config.priority_count {
        return Err(SubmissionConfigError::DataMasterPriorityOutOfRange);
    }

    let (encoder_data_master_ordinal, ring_object_base_offset) = match data_master {
        DataMasterClass::Ta => (0u8, config.ta_ring_object_base_offset),
        DataMasterClass::ThreeD => (1, config.three_d_ring_object_base_offset),
        DataMasterClass::Cl => (2, config.cl_ring_object_base_offset),
    };
    let ring_object_offset =
        ring_object_base_offset + priority as u16 * config.ring_object_stride as u16;
    let descriptor_record_offset = (priority as u16 * config.data_master_count as u16
        + encoder_data_master_ordinal as u16)
        * config.descriptor_record_stride as u16;

    Ok(DataMasterFirmwareRingRoute {
        encoder_data_master_ordinal,
        ring_object_offset,
        descriptor_record_offset,
        read_index_firmware_pointer_offset: descriptor_record_offset
            + config.read_index_firmware_pointer_offset as u16,
        cfi_index_firmware_pointer_offset: descriptor_record_offset
            + config.cfi_index_firmware_pointer_offset as u16,
        write_index_firmware_pointer_offset: descriptor_record_offset
            + config.write_index_firmware_pointer_offset as u16,
        entries_firmware_pointer_offset: descriptor_record_offset
            + config.entries_firmware_pointer_offset as u16,
    })
}

/// Return the exact recovery drain/discard contract from the pinned G17S
/// firmware bundle. Other G17 variants, including G17C, are intentionally not
/// inferred from the G17S image.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_recovery_drain_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<G17sFirmwareRecoveryDrainConfig> {
    if matches!((gpu_gen, gpu_variant), (GpuGen::G17, GpuVariant::S)) {
        Some(G17S_FIRMWARE_RECOVERY_DRAIN_CONFIG)
    } else {
        None
    }
}

/// Validate an externally recovered G17S recovery drain/discard contract.
#[allow(dead_code)]
pub(crate) const fn validate_g17s_firmware_recovery_drain_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: G17sFirmwareRecoveryDrainConfig,
) -> Result<G17sFirmwareRecoveryDrainConfig, SubmissionConfigError> {
    let Some(expected) = g17s_firmware_recovery_drain_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if supplied.handler_text_offset != expected.handler_text_offset
        || supplied.runtime_pointer_global_image_offset
            != expected.runtime_pointer_global_image_offset
        || supplied.recovery_status_pointer_global_image_offset
            != expected.recovery_status_pointer_global_image_offset
        || supplied.recovery_status_field_offset != expected.recovery_status_field_offset
        || supplied.recovery_status_active_value != expected.recovery_status_active_value
        || supplied.recovery_status_inactive_value != expected.recovery_status_inactive_value
        || supplied.descriptor_table_offset != expected.descriptor_table_offset
        || supplied.priority_count != expected.priority_count
        || supplied.data_master_count != expected.data_master_count
        || supplied.priority_stride != expected.priority_stride
        || supplied.descriptor_record_stride != expected.descriptor_record_stride
        || supplied.read_index_pointer_offset != expected.read_index_pointer_offset
        || supplied.cfi_index_pointer_offset != expected.cfi_index_pointer_offset
        || supplied.write_index_pointer_offset != expected.write_index_pointer_offset
        || supplied.entries_pointer_offset != expected.entries_pointer_offset
        || supplied.entry_stride != expected.entry_stride
        || supplied.read_index_mask != expected.read_index_mask
        || supplied.maximum_read_advancements_per_master
            != expected.maximum_read_advancements_per_master
        || !supplied.cfi_initialized_from_write_index
        || !matches!(
            supplied.cfi_initialization_order,
            [
                G17sFirmwareRecoveryDataMaster::Ta,
                G17sFirmwareRecoveryDataMaster::ThreeD,
                G17sFirmwareRecoveryDataMaster::Compute,
            ]
        )
        || !matches!(
            supplied.drain_attempt_order,
            [
                G17sFirmwareRecoveryDataMaster::Ta,
                G17sFirmwareRecoveryDataMaster::ThreeD,
                G17sFirmwareRecoveryDataMaster::Compute,
            ]
        )
        || !matches!(supplied.expected_entry_type_ordinals, [0, 1, 2])
        || !matches!(
            supplied.direct_status_steps,
            [
                G17sFirmwareRecoveryStatusStep::StoreActive,
                G17sFirmwareRecoveryStatusStep::DsbSy,
                G17sFirmwareRecoveryStatusStep::RecoveryBody,
                G17sFirmwareRecoveryStatusStep::StoreInactive,
                G17sFirmwareRecoveryStatusStep::DsbSy,
            ]
        )
        || supplied.normal_submission_acknowledgment_proven
    {
        Err(SubmissionConfigError::UnexpectedG17sFirmwareRecoveryDrainConfig)
    } else {
        Ok(expected)
    }
}

/// Select one descriptor in the G17S recovery table without touching firmware
/// memory.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_recovery_drain_route(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    priority: u8,
    data_master: G17sFirmwareRecoveryDataMaster,
) -> Result<G17sFirmwareRecoveryDrainRoute, SubmissionConfigError> {
    let Some(config) = g17s_firmware_recovery_drain_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if priority >= config.priority_count {
        return Err(SubmissionConfigError::G17sFirmwareRecoveryPriorityOutOfRange);
    }

    let ordinal = match data_master {
        G17sFirmwareRecoveryDataMaster::Ta => 0u16,
        G17sFirmwareRecoveryDataMaster::ThreeD => 1,
        G17sFirmwareRecoveryDataMaster::Compute => 2,
    };
    let record_offset = config.descriptor_table_offset as u16
        + priority as u16 * config.priority_stride as u16
        + ordinal * config.descriptor_record_stride as u16;

    Ok(G17sFirmwareRecoveryDrainRoute {
        record_offset_from_runtime_base: record_offset,
        read_index_pointer_offset_from_runtime_base: record_offset
            + config.read_index_pointer_offset as u16,
        cfi_index_pointer_offset_from_runtime_base: record_offset
            + config.cfi_index_pointer_offset as u16,
        write_index_pointer_offset_from_runtime_base: record_offset
            + config.write_index_pointer_offset as u16,
        entries_pointer_offset_from_runtime_base: record_offset
            + config.entries_pointer_offset as u16,
        expected_entry_type_ordinal: ordinal as u8,
    })
}

/// Apply the exact firmware recovery read-index update. The firmware uses a
/// 32-bit load/add/mask/store, so malformed upper bits are discarded rather
/// than rejected by this diagnostic.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_recovery_advance_read_index(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    read_index: u32,
) -> Result<u32, SubmissionConfigError> {
    let Some(config) = g17s_firmware_recovery_drain_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    Ok(read_index.wrapping_add(1) & config.read_index_mask as u32)
}

/// Return the exact CFI value installed before G17S recovery begins draining
/// one priority. The recovered firmware copies the snapshotted write index
/// without truncation.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_recovery_cfi_initial_value(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    write_index_snapshot: u32,
) -> Result<u32, SubmissionConfigError> {
    if g17s_firmware_recovery_drain_config(gpu_gen, gpu_variant).is_none() {
        Err(SubmissionConfigError::UnsupportedTarget)
    } else {
        Ok(write_index_snapshot)
    }
}

/// Return the exact SKSM completion consumer contract from the pinned G17S
/// host and GFX firmware. Other identities are deliberately unsupported.
#[allow(dead_code)]
pub(crate) const fn g17s_sksm_completion_consumer_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<G17sSksmCompletionConsumerConfig> {
    if matches!((gpu_gen, gpu_variant), (GpuGen::G17, GpuVariant::S)) {
        Some(G17S_SKSM_COMPLETION_CONSUMER_CONFIG)
    } else {
        None
    }
}

/// Validate an externally recovered G17S SKSM completion consumer contract.
#[allow(dead_code)]
pub(crate) const fn validate_g17s_sksm_completion_consumer_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: G17sSksmCompletionConsumerConfig,
) -> Result<G17sSksmCompletionConsumerConfig, SubmissionConfigError> {
    let Some(expected) = g17s_sksm_completion_consumer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if supplied.consumer_text_offset != expected.consumer_text_offset
        || supplied.entry_validator_text_offset != expected.entry_validator_text_offset
        || supplied.entry_handler_text_offset != expected.entry_handler_text_offset
        || supplied.cache_clean_invalidate_text_offset
            != expected.cache_clean_invalidate_text_offset
        || supplied.descriptor_table_offset != expected.descriptor_table_offset
        || supplied.host_resource_table_accelerator_offset
            != expected.host_resource_table_accelerator_offset
        || supplied.host_capacity_accelerator_offset != expected.host_capacity_accelerator_offset
        || supplied.host_present_pointer_source_subobject_offset
            != expected.host_present_pointer_source_subobject_offset
        || supplied.host_entries_pointer_source_subobject_offset
            != expected.host_entries_pointer_source_subobject_offset
        || supplied.host_initial_read_index != expected.host_initial_read_index
        || supplied.host_resource_count != expected.host_resource_count
        || supplied.firmware_consumed_descriptor_count
            != expected.firmware_consumed_descriptor_count
        || supplied.descriptor_record_stride != expected.descriptor_record_stride
        || supplied.present_pointer_offset != expected.present_pointer_offset
        || supplied.entries_pointer_offset != expected.entries_pointer_offset
        || supplied.capacity_offset != expected.capacity_offset
        || supplied.entry_stride_offset != expected.entry_stride_offset
        || supplied.mmio_selector_offset != expected.mmio_selector_offset
        || supplied.read_index_offset != expected.read_index_offset
        || supplied.host_entry_stride != expected.host_entry_stride
        || !matches!(supplied.host_selector_by_record, [0, 1, 2, 3])
        || !matches!(supplied.firmware_record_order, [1, 3, 0])
        || supplied.producer_mmio_base_offset != expected.producer_mmio_base_offset
        || supplied.processed_count_mmio_base_offset != expected.processed_count_mmio_base_offset
        || supplied.mmio_selector_stride != expected.mmio_selector_stride
        || supplied.minimum_raw_producer_value != expected.minimum_raw_producer_value
        || supplied.producer_count_shift != expected.producer_count_shift
        || supplied.processed_count_mmio_store_width != expected.processed_count_mmio_store_width
        || !matches!(supplied.supported_modes, [0, 2])
        || !matches!(
            supplied.entry_steps,
            [
                G17sSksmCompletionEntryStep::ValidateEntry,
                G17sSksmCompletionEntryStep::HandleIfAccepted,
                G17sSksmCompletionEntryStep::AdvanceReadIndexModuloCapacity,
            ]
        )
        || !supplied.cache_clean_invalidate_precedes_entry_reads
        || !supplied.handler_return_precedes_read_index_store
        || !supplied.current_descriptor_work_precedes_its_mmio_publication
        || supplied.direct_barrier_before_mmio_publication
        || supplied.mmio_publication_is_release_store
        || !supplied.host_prepare_mappings_covers_all_resources
        || !supplied.host_release_zeroes_all_resource_slots
        || supplied.normal_classic_submission_acknowledgment_proven
    {
        Err(SubmissionConfigError::UnexpectedG17sSksmCompletionConsumerConfig)
    } else {
        Ok(expected)
    }
}

/// Select one of the three completion descriptors visited by the G17S GFX
/// firmware, without reading shared memory or MMIO.
#[allow(dead_code)]
pub(crate) const fn g17s_sksm_completion_route(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    visit_ordinal: u8,
) -> Result<G17sSksmCompletionRoute, SubmissionConfigError> {
    let Some(config) = g17s_sksm_completion_consumer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if visit_ordinal >= config.firmware_consumed_descriptor_count {
        return Err(SubmissionConfigError::G17sSksmCompletionDescriptorOutOfRange);
    }

    let record = config.firmware_record_order[visit_ordinal as usize];
    let selector = config.host_selector_by_record[record as usize];
    Ok(G17sSksmCompletionRoute {
        descriptor_record: record,
        descriptor_offset: record * config.descriptor_record_stride,
        mmio_selector: selector,
        producer_mmio_offset: config.producer_mmio_base_offset
            + selector as u32 * config.mmio_selector_stride as u32,
        processed_count_mmio_offset: config.processed_count_mmio_base_offset
            + selector as u32 * config.mmio_selector_stride as u32,
    })
}

/// Decode the processed-count field read from the G17S producer/status MMIO
/// word. The recovered code compares and shifts only its low 32 bits.
#[allow(dead_code)]
pub(crate) const fn g17s_sksm_completion_producer_count(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    raw_status: u64,
) -> Result<u32, SubmissionConfigError> {
    let Some(config) = g17s_sksm_completion_consumer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    let low_status = raw_status as u32;
    if low_status < config.minimum_raw_producer_value {
        return Err(SubmissionConfigError::G17sSksmCompletionProducerNotReady);
    }

    Ok(low_status >> config.producer_count_shift)
}

/// Apply one exact G17S SKSM completion read-index advancement. The firmware
/// performs 32-bit add/divide/remainder/store after each processed entry.
#[allow(dead_code)]
pub(crate) const fn g17s_sksm_completion_advance_read_index(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    read_index: u32,
    capacity: u32,
) -> Result<u32, SubmissionConfigError> {
    if g17s_sksm_completion_consumer_config(gpu_gen, gpu_variant).is_none() {
        return Err(SubmissionConfigError::UnsupportedTarget);
    }
    if capacity == 0 {
        return Err(SubmissionConfigError::G17sSksmCompletionCapacityZero);
    }

    Ok(read_index.wrapping_add(1) % capacity)
}

/// Return the exact two-role G17S firmware-event producer and host bridge.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_event_producer_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<G17sFirmwareEventProducerConfig> {
    if matches!((gpu_gen, gpu_variant), (GpuGen::G17, GpuVariant::S)) {
        Some(G17S_FIRMWARE_EVENT_PRODUCER_CONFIG)
    } else {
        None
    }
}

/// Select the exact producer entry point for one G17S firmware role.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_event_producer_role_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    role: SubmissionFirmwareRole,
) -> Result<G17sFirmwareEventProducerRoleConfig, SubmissionConfigError> {
    let Some(config) = g17s_firmware_event_producer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    Ok(match role {
        SubmissionFirmwareRole::Gfx => config.roles[0],
        SubmissionFirmwareRole::Gfx1 => config.roles[1],
    })
}

/// Validate an externally recovered G17S event producer and interrupt bridge.
#[allow(dead_code)]
pub(crate) const fn validate_g17s_firmware_event_producer_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: G17sFirmwareEventProducerConfig,
) -> Result<G17sFirmwareEventProducerConfig, SubmissionConfigError> {
    let Some(expected) = g17s_firmware_event_producer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if !matches!(
        supplied.roles,
        [
            G17sFirmwareEventProducerRoleConfig {
                role: SubmissionFirmwareRole::Gfx,
                enqueue_text_offset: 0x23560,
                runtime_pointer_global_image_offset: 0x104888,
                notification_send_text_offset: 0x35a90,
            },
            G17sFirmwareEventProducerRoleConfig {
                role: SubmissionFirmwareRole::Gfx1,
                enqueue_text_offset: 0x290f0,
                runtime_pointer_global_image_offset: 0x100e08,
                notification_send_text_offset: 0x3c804,
            },
        ]
    ) || supplied.ring_pointer_pair_offset != expected.ring_pointer_pair_offset
        || supplied.shared_indices_pointer_pair_offset
            != expected.shared_indices_pointer_pair_offset
        || supplied.entries_pointer_pair_offset != expected.entries_pointer_pair_offset
        || supplied.shared_read_index_offset != expected.shared_read_index_offset
        || supplied.shared_write_index_offset != expected.shared_write_index_offset
        || supplied.entry_stride != expected.entry_stride
        || supplied.copied_entry_bytes != expected.copied_entry_bytes
        || supplied.index_mask != expected.index_mask
        || supplied.notification_endpoint_service_id != expected.notification_endpoint_service_id
        || supplied.notification_message != expected.notification_message
        || supplied.notification_message_type_shift != expected.notification_message_type_shift
        || supplied.notification_message_type_mask != expected.notification_message_type_mask
        || supplied.notification_argument2 != expected.notification_argument2
        || supplied.rtbuddy_received_message_text_offset
            != expected.rtbuddy_received_message_text_offset
        || supplied.host_received_message_text_offset != expected.host_received_message_text_offset
        || supplied.host_message_type != expected.host_message_type
        || supplied.host_event_source_setup_text_offset
            != expected.host_event_source_setup_text_offset
        || supplied.host_event_source_array_offset != expected.host_event_source_array_offset
        || supplied.host_interrupt_selector_offset != expected.host_interrupt_selector_offset
        || !matches!(supplied.host_interrupt_indices, [0, 4])
        || supplied.accelerator_firmware_pointer_offset
            != expected.accelerator_firmware_pointer_offset
        || supplied.accelerator_handle_interrupt_text_offset
            != expected.accelerator_handle_interrupt_text_offset
        || supplied.firmware_handle_event_text_offset != expected.firmware_handle_event_text_offset
        || !matches!(
            supplied.steps,
            [
                G17sFirmwareEventProducerStep::LoadSharedWriteIndex,
                G17sFirmwareEventProducerStep::AdvanceAndWrap8,
                G17sFirmwareEventProducerStep::WaitUntilNextDiffersFromSharedRead,
                G17sFirmwareEventProducerStep::ReloadSharedWriteIndex,
                G17sFirmwareEventProducerStep::CopyEntry,
                G17sFirmwareEventProducerStep::DsbSyBeforeWriteIndex,
                G17sFirmwareEventProducerStep::PublishSharedWriteIndex,
                G17sFirmwareEventProducerStep::DsbSyBeforeNotification,
                G17sFirmwareEventProducerStep::NotifyHost,
            ]
        )
        || supplied.direct_cache_maintenance_before_publish
        || supplied.sksm_processed_count_causal_link_proven
        || supplied.normal_classic_submission_acknowledgment_proven
        || ((supplied.notification_message >> supplied.notification_message_type_shift)
            & supplied.notification_message_type_mask as u64)
            != supplied.host_message_type as u64
    {
        Err(SubmissionConfigError::UnexpectedG17sFirmwareEventProducerConfig)
    } else {
        Ok(expected)
    }
}

/// Apply the exact producer-side G17S event-ring index calculation. This pure
/// helper does not copy the entry, execute either barrier, store, or notify.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_event_producer_next_entry(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    shared_read_index: u32,
    shared_write_index: u32,
) -> Result<G17sFirmwareEventProducerNextEntry, SubmissionConfigError> {
    let Some(config) = g17s_firmware_event_producer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if shared_read_index > config.index_mask as u32 {
        return Err(SubmissionConfigError::G17sFirmwareEventReadIndexOutOfRange);
    }
    if shared_write_index > config.index_mask as u32 {
        return Err(SubmissionConfigError::G17sFirmwareEventWriteIndexOutOfRange);
    }

    let published_write_index = shared_write_index.wrapping_add(1) & config.index_mask as u32;
    if published_write_index == shared_read_index {
        return Err(SubmissionConfigError::G17sFirmwareEventRingFull);
    }

    Ok(G17sFirmwareEventProducerNextEntry {
        entry_offset_from_entries_base: shared_write_index as u16 * config.entry_stride as u16,
        published_write_index: published_write_index as u8,
    })
}

/// Validate the raw interrupt selected by the G17S type-2 host notification.
/// Both recovered indices converge on the firmware-event drain path.
#[allow(dead_code)]
pub(crate) const fn g17s_firmware_event_interrupt_route(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    raw_interrupt_index: u8,
) -> Result<G17sFirmwareEventInterruptRoute, SubmissionConfigError> {
    let Some(config) = g17s_firmware_event_producer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if raw_interrupt_index != config.host_interrupt_indices[0]
        && raw_interrupt_index != config.host_interrupt_indices[1]
    {
        return Err(SubmissionConfigError::G17sFirmwareEventInterruptIndexUnsupported);
    }

    Ok(G17sFirmwareEventInterruptRoute {
        raw_interrupt_index,
        drains_firmware_event_rings: true,
    })
}

/// Return the exact host firmware-event consumer contract from the pinned
/// G17S host. Other identities are deliberately unsupported.
#[allow(dead_code)]
pub(crate) const fn g17s_host_firmware_event_consumer_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
) -> Option<G17sHostFirmwareEventConsumerConfig> {
    if matches!((gpu_gen, gpu_variant), (GpuGen::G17, GpuVariant::S)) {
        Some(G17S_HOST_FIRMWARE_EVENT_CONSUMER_CONFIG)
    } else {
        None
    }
}

/// Validate an externally recovered G17S host firmware-event contract.
#[allow(dead_code)]
pub(crate) const fn validate_g17s_host_firmware_event_consumer_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: G17sHostFirmwareEventConsumerConfig,
) -> Result<G17sHostFirmwareEventConsumerConfig, SubmissionConfigError> {
    let Some(expected) = g17s_host_firmware_event_consumer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if supplied.handle_event_text_offset != expected.handle_event_text_offset
        || supplied.clear_outstanding_interrupts_text_offset
            != expected.clear_outstanding_interrupts_text_offset
        || supplied.drain_firmware_rings_text_offset != expected.drain_firmware_rings_text_offset
        || supplied.drain_all_event_rings_text_offset != expected.drain_all_event_rings_text_offset
        || supplied.drain_role_event_ring_text_offset != expected.drain_role_event_ring_text_offset
        || supplied.fetch_next_entry_text_offset != expected.fetch_next_entry_text_offset
        || !matches!(supplied.draining_interrupt_indices, [0, 4])
        || !matches!(
            supplied.role_order,
            [SubmissionFirmwareRole::Gfx, SubmissionFirmwareRole::Gfx1]
        )
        || supplied.role_validator_base_offset != expected.role_validator_base_offset
        || supplied.role_validator_stride != expected.role_validator_stride
        || supplied.shared_indices_pointer_offset != expected.shared_indices_pointer_offset
        || supplied.entries_pointer_offset != expected.entries_pointer_offset
        || supplied.cached_read_index_offset != expected.cached_read_index_offset
        || supplied.cached_write_index_offset != expected.cached_write_index_offset
        || supplied.valid_type_mask_offset != expected.valid_type_mask_offset
        || supplied.capacity_offset != expected.capacity_offset
        || supplied.shared_read_index_offset != expected.shared_read_index_offset
        || supplied.shared_write_index_offset != expected.shared_write_index_offset
        || supplied.entry_stride != expected.entry_stride
        || supplied.entry_copy_word_count != expected.entry_copy_word_count
        || supplied.event_type_offset != expected.event_type_offset
        || supplied.valid_type_mask_bits != expected.valid_type_mask_bits
        || supplied.maximum_dispatched_event_type != expected.maximum_dispatched_event_type
        || !matches!(
            supplied.entry_steps,
            [
                G17sHostFirmwareEventStep::SnapshotSharedIndices,
                G17sHostFirmwareEventStep::ValidateSnapshotBounds,
                G17sHostFirmwareEventStep::CopyEntry,
                G17sHostFirmwareEventStep::ValidateTypeMask,
                G17sHostFirmwareEventStep::AdvanceReadIndexModuloCapacity,
                G17sHostFirmwareEventStep::DmbIsh,
                G17sHostFirmwareEventStep::PublishSharedReadIndex,
                G17sHostFirmwareEventStep::DispatchIfSupported,
            ]
        )
        || !supplied.firmware_event_producer_ordering_proven
        || supplied.sksm_processed_count_interrupt_link_proven
        || supplied.normal_classic_submission_acknowledgment_proven
    {
        Err(SubmissionConfigError::UnexpectedG17sHostFirmwareEventConsumerConfig)
    } else {
        Ok(expected)
    }
}

/// Select the exact per-role host validator without reading shared memory.
#[allow(dead_code)]
pub(crate) const fn g17s_host_firmware_event_ring_route(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    role: SubmissionFirmwareRole,
) -> Result<G17sHostFirmwareEventRingRoute, SubmissionConfigError> {
    let Some(config) = g17s_host_firmware_event_consumer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    let ordinal = match role {
        SubmissionFirmwareRole::Gfx => 0,
        SubmissionFirmwareRole::Gfx1 => 1,
    };

    Ok(G17sHostFirmwareEventRingRoute {
        role,
        validator_object_offset: config.role_validator_base_offset
            + ordinal * config.role_validator_stride,
    })
}

/// Apply the exact index/type calculation performed before the G17S host
/// publishes one consumed firmware-event read index. This is a pure model: it
/// does not copy memory, execute the barrier, or write the shared index.
#[allow(dead_code)]
pub(crate) const fn g17s_host_firmware_event_next_entry(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    read_index: u32,
    write_index_snapshot: u32,
    capacity: u64,
    event_type: u32,
    valid_type_mask: u64,
) -> Result<G17sHostFirmwareEventNextEntry, SubmissionConfigError> {
    let Some(config) = g17s_host_firmware_event_consumer_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if capacity == 0 {
        return Err(SubmissionConfigError::G17sHostFirmwareEventCapacityZero);
    }
    if read_index as u64 >= capacity {
        return Err(SubmissionConfigError::G17sHostFirmwareEventReadIndexOutOfRange);
    }
    if write_index_snapshot as u64 >= capacity {
        return Err(SubmissionConfigError::G17sHostFirmwareEventWriteIndexOutOfRange);
    }
    if read_index == write_index_snapshot {
        return Err(SubmissionConfigError::G17sHostFirmwareEventRingEmpty);
    }

    // AArch64 LSRV uses the low six bits for a 64-bit source register.
    let type_mask_shift = event_type & (config.valid_type_mask_bits as u32 - 1);
    if (valid_type_mask >> type_mask_shift) & 1 == 0 {
        return Err(SubmissionConfigError::G17sHostFirmwareEventTypeDisabled);
    }

    let published_read_index = (read_index.wrapping_add(1) as u64 % capacity) as u32;
    Ok(G17sHostFirmwareEventNextEntry {
        entry_offset_from_entries_base: read_index as u64 * config.entry_stride as u64,
        published_read_index,
        dispatch_event_type: if event_type <= config.maximum_dispatched_event_type as u32 {
            Some(event_type as u8)
        } else {
            None
        },
        has_more_from_snapshot: published_read_index != write_index_snapshot,
    })
}

const fn data_master_ring_index_backing_matches(
    supplied: DataMasterRingIndexBacking,
    expected: DataMasterRingIndexBacking,
) -> bool {
    use DataMasterRingIndexBacking::*;

    match (supplied, expected) {
        (
            FirmwareSharedData {
                resource_owner_object_offset: supplied_owner,
                cpu_object_offset: supplied_cpu,
                firmware_object_offset: supplied_firmware,
                read_index_offset: supplied_read,
                cfi_index_offset: supplied_cfi,
                write_index_offset: supplied_write,
                slice_size: supplied_size,
                caching_options: supplied_caching,
                allocate_shared_data_arg: supplied_arg,
                batch_ring_count: supplied_count,
                release_zeroes_owner_slot: supplied_release,
            },
            FirmwareSharedData {
                resource_owner_object_offset: expected_owner,
                cpu_object_offset: expected_cpu,
                firmware_object_offset: expected_firmware,
                read_index_offset: expected_read,
                cfi_index_offset: expected_cfi,
                write_index_offset: expected_write,
                slice_size: expected_size,
                caching_options: expected_caching,
                allocate_shared_data_arg: expected_arg,
                batch_ring_count: expected_count,
                release_zeroes_owner_slot: expected_release,
            },
        ) => {
            supplied_owner == expected_owner
                && supplied_cpu == expected_cpu
                && supplied_firmware == expected_firmware
                && supplied_read == expected_read
                && supplied_cfi == expected_cfi
                && supplied_write == expected_write
                && supplied_size == expected_size
                && supplied_caching == expected_caching
                && supplied_arg == expected_arg
                && supplied_count == expected_count
                && supplied_release == expected_release
        }
        (
            FenderScratchRam {
                scratch_object_offset: supplied_scratch,
                read_cpu_pointer_offset: supplied_read_cpu,
                cfi_cpu_pointer_offset: supplied_cfi_cpu,
                write_cpu_pointer_offset: supplied_write_cpu,
                read_firmware_pointer_offset: supplied_read_firmware,
                cfi_firmware_pointer_offset: supplied_cfi_firmware,
                write_firmware_pointer_offset: supplied_write_firmware,
                entries_write_allocator_offset: supplied_write_allocator,
                read_cfi_allocator_offset: supplied_read_cfi_allocator,
                index_allocation_size: supplied_size,
                entries_write_mapping_options: supplied_write_options,
                read_cfi_mapping_options: supplied_read_cfi_options,
                cleanup_frees_allocations_proven: supplied_cleanup,
            },
            FenderScratchRam {
                scratch_object_offset: expected_scratch,
                read_cpu_pointer_offset: expected_read_cpu,
                cfi_cpu_pointer_offset: expected_cfi_cpu,
                write_cpu_pointer_offset: expected_write_cpu,
                read_firmware_pointer_offset: expected_read_firmware,
                cfi_firmware_pointer_offset: expected_cfi_firmware,
                write_firmware_pointer_offset: expected_write_firmware,
                entries_write_allocator_offset: expected_write_allocator,
                read_cfi_allocator_offset: expected_read_cfi_allocator,
                index_allocation_size: expected_size,
                entries_write_mapping_options: expected_write_options,
                read_cfi_mapping_options: expected_read_cfi_options,
                cleanup_frees_allocations_proven: expected_cleanup,
            },
        ) => {
            supplied_scratch == expected_scratch
                && supplied_read_cpu == expected_read_cpu
                && supplied_cfi_cpu == expected_cfi_cpu
                && supplied_write_cpu == expected_write_cpu
                && supplied_read_firmware == expected_read_firmware
                && supplied_cfi_firmware == expected_cfi_firmware
                && supplied_write_firmware == expected_write_firmware
                && supplied_write_allocator == expected_write_allocator
                && supplied_read_cfi_allocator == expected_read_cfi_allocator
                && supplied_size == expected_size
                && supplied_write_options == expected_write_options
                && supplied_read_cfi_options == expected_read_cfi_options
                && supplied_cleanup == expected_cleanup
        }
        _ => false,
    }
}

const fn data_master_ring_entry_backing_matches(
    supplied: DataMasterRingEntryBacking,
    expected: DataMasterRingEntryBacking,
) -> bool {
    use DataMasterRingEntryBacking::*;

    match (supplied, expected) {
        (
            FirmwareSharedData {
                resource_owner_object_offset: supplied_owner,
                slice_size: supplied_size,
                caching_options: supplied_caching,
                allocate_shared_data_arg: supplied_arg,
                batch_ring_count: supplied_count,
                release_zeroes_owner_slot: supplied_release,
            },
            FirmwareSharedData {
                resource_owner_object_offset: expected_owner,
                slice_size: expected_size,
                caching_options: expected_caching,
                allocate_shared_data_arg: expected_arg,
                batch_ring_count: expected_count,
                release_zeroes_owner_slot: expected_release,
            },
        ) => {
            supplied_owner == expected_owner
                && supplied_size == expected_size
                && supplied_caching == expected_caching
                && supplied_arg == expected_arg
                && supplied_count == expected_count
                && supplied_release == expected_release
        }
        (
            FenderScratchRam {
                allocator_offset: supplied_allocator,
                allocation_size: supplied_size,
                mapping_options: supplied_options,
                cleanup_frees_allocation_proven: supplied_cleanup,
            },
            FenderScratchRam {
                allocator_offset: expected_allocator,
                allocation_size: expected_size,
                mapping_options: expected_options,
                cleanup_frees_allocation_proven: expected_cleanup,
            },
        ) => {
            supplied_allocator == expected_allocator
                && supplied_size == expected_size
                && supplied_options == expected_options
                && supplied_cleanup == expected_cleanup
        }
        _ => false,
    }
}

/// Validate an externally recovered host-side data-master ring contract.
#[allow(dead_code)]
pub(crate) const fn validate_data_master_ring_config(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    supplied: DataMasterRingConfig,
) -> Result<DataMasterRingConfig, SubmissionConfigError> {
    let Some(expected) = data_master_ring_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };

    if !data_master_ring_index_backing_matches(supplied.index_backing, expected.index_backing)
        || !data_master_ring_entry_backing_matches(supplied.entry_backing, expected.entry_backing)
        || supplied.entries_cpu_object_offset != expected.entries_cpu_object_offset
        || supplied.entries_firmware_object_offset != expected.entries_firmware_object_offset
        || !matches!(
            (supplied.host_index_mutation, expected.host_index_mutation),
            (
                DataMasterRingHostIndexMutation::DirectSharedStores,
                DataMasterRingHostIndexMutation::DirectSharedStores,
            ) | (
                DataMasterRingHostIndexMutation::LockedScratchWriteOnly,
                DataMasterRingHostIndexMutation::LockedScratchWriteOnly,
            )
        )
        || supplied.entry_stride != expected.entry_stride
        || supplied.index_count != expected.index_count
        || supplied.usable_entry_count != expected.usable_entry_count
        || supplied.cold_init_ring_count != expected.cold_init_ring_count
        || supplied.shared_data_cacheability_names_proven
            != expected.shared_data_cacheability_names_proven
        || supplied.firmware_consumer_ownership_proven
            != expected.firmware_consumer_ownership_proven
        || !matches!(
            supplied.publication_steps,
            [
                DataMasterRingPublicationStep::SelectNextEntry,
                DataMasterRingPublicationStep::RecordEncoded,
                DataMasterRingPublicationStep::DmbIsh,
                DataMasterRingPublicationStep::ReadWriteIndex,
                DataMasterRingPublicationStep::IncrementAndWrap8,
                DataMasterRingPublicationStep::StoreWriteIndex,
            ]
        )
    {
        Err(SubmissionConfigError::UnexpectedDataMasterRingConfig)
    } else {
        Ok(expected)
    }
}

/// Apply the exact host full/wrap and entry-offset calculation to ring state.
///
/// This is a pure diagnostic. It neither reserves an entry nor publishes an
/// index, and therefore carries no shared-memory ownership claim.
#[allow(dead_code)]
pub(crate) const fn data_master_ring_next_entry(
    gpu_gen: GpuGen,
    gpu_variant: GpuVariant,
    read_index: u16,
    write_index: u16,
) -> Result<DataMasterRingNextEntry, SubmissionConfigError> {
    let Some(config) = data_master_ring_config(gpu_gen, gpu_variant) else {
        return Err(SubmissionConfigError::UnsupportedTarget);
    };
    if read_index >= config.index_count {
        return Err(SubmissionConfigError::DataMasterReadIndexOutOfRange);
    }
    if write_index >= config.index_count {
        return Err(SubmissionConfigError::DataMasterWriteIndexOutOfRange);
    }

    let published_write_index = write_index.wrapping_add(1) as u8;
    if published_write_index == read_index as u8 {
        return Err(SubmissionConfigError::DataMasterRingFull);
    }

    Ok(DataMasterRingNextEntry {
        entry_offset_from_entries_base: write_index * config.entry_stride as u16,
        published_write_index,
    })
}

