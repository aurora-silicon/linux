// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! T8140/G17P compute descriptor construction.
//!
//! The descriptor is the one-page selector-3 object executed by the CL path.
//! Every field here comes from the hardware-tested m1n1 compute builder. The
//! caller supplies queue-owned object addresses and command-owned UAPI values;
//! this module only validates and encodes them.

pub(crate) const COMPUTE_DESCRIPTOR_SIZE: usize = 0x4000;
pub(crate) const COMPUTE_REGISTER_COUNT: usize = 40;
pub(crate) const COMPUTE_REGISTER_CAPACITY: usize = 128;
pub(crate) const COMPUTE_REGISTER_SIZE: usize = 0x0c;
pub(crate) const COMPUTE_QUEUE_GRAPH_SIZE: usize = 0x8000;
pub(crate) const COMPUTE_QUEUE_CONTEXT_SIZE: usize = 0x4000;
pub(crate) const COMPUTE_QUEUE_CONTEXT_EXTENT: usize = 8 * COMPUTE_QUEUE_CONTEXT_SIZE;
pub(crate) const COMPUTE_OPTIONAL_SIZE: usize = 0xc0;
pub(crate) const COMPUTE_EVENT_SIZE: usize = 0x400;
pub(crate) const COMPUTE_CHANNEL_WRITE_INDEX: u32 = 3;
pub(crate) const COMPUTE_QUEUE_POINTERS: usize = 0x100;
const COMPUTE_QUEUE_CONTEXT_ITEM: usize = 0x200;
/// Same offset, exported so a repeat can refresh the item the first bind wrote.
pub(crate) const COMPUTE_QUEUE_CONTEXT_ITEM_OFFSET: usize = COMPUTE_QUEUE_CONTEXT_ITEM;
pub(crate) const COMPUTE_ITEM_RING: usize = 0x4000;
pub(crate) const COMPUTE_ITEM_RING_SIZE: usize = 0x500 * 8;
pub(crate) const COMPUTE_OPTIONAL: usize = 0x6800;
pub(crate) const COMPUTE_EVENT: usize = 0x6c00;
pub(crate) const COMPUTE_REPEAT_RING_RECORD_COUNT: usize = 4;
pub(crate) const COMPUTE_ADD_KICKS: usize = 0x7000;
pub(crate) const COMPUTE_ADD_KICKS_SIZE: usize = 0x40;
const _: () = assert!(COMPUTE_ITEM_RING + COMPUTE_ITEM_RING_SIZE <= COMPUTE_OPTIONAL);
const _: () = assert!(COMPUTE_OPTIONAL + COMPUTE_OPTIONAL_SIZE <= COMPUTE_EVENT);
const _: () = assert!(COMPUTE_EVENT + COMPUTE_EVENT_SIZE <= COMPUTE_ADD_KICKS);
const _: () = assert!(COMPUTE_ADD_KICKS + COMPUTE_ADD_KICKS_SIZE <= COMPUTE_QUEUE_GRAPH_SIZE);
const _: () = assert!(COMPUTE_REGISTER_COUNT <= COMPUTE_REGISTER_CAPACITY);

const COMPUTE_SELECTOR: u32 = 3;
const COMPUTE_REGISTER_START: usize = 0x40;
const COMPUTE_SECONDARY_REGISTER_START: usize = 0x760;
const COMPUTE_PRIMARY_COUNT: usize = 0x748;
const COMPUTE_ENCODER_PARAMS: usize = 0xf14;
const COMPUTE_SAMPLER_ARRAY: usize = COMPUTE_ENCODER_PARAMS + 0x18;
const COMPUTE_SAMPLER_COUNT: usize = COMPUTE_ENCODER_PARAMS + 0x20;
const COMPUTE_SAMPLER_MAX: usize = COMPUTE_ENCODER_PARAMS + 0x24;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum ComputeBuildError {
    BufferTooSmall,
    AddressOverflow,
    CdmTerminatorBeforeStream,
    SubmissionIndexZero,
    QueueSubmissionZero,
    InvalidSksmEntryAliases,
    SamplerCountOutOfRange,
    SamplerPair,
    SamplerAlignment,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeSksmEntryAliases {
    low: u64,
    high: u64,
    extent: usize,
}

impl ComputeSksmEntryAliases {
    pub(crate) const fn new(
        low: u64,
        high: u64,
        extent: usize,
        alignment: u64,
    ) -> Result<Self, ComputeBuildError> {
        if low == 0
            || high == 0
            || low == high
            || extent == 0
            || alignment == 0
            || alignment & (alignment - 1) != 0
            || low & (alignment - 1) != 0
            || high & (alignment - 1) != 0
            || low.checked_add(extent as u64).is_none()
            || high.checked_add(extent as u64).is_none()
        {
            return Err(ComputeBuildError::InvalidSksmEntryAliases);
        }
        Ok(Self { low, high, extent })
    }

    pub(crate) const fn low(self) -> u64 {
        self.low
    }

    pub(crate) const fn high(self) -> u64 {
        self.high
    }

    pub(crate) const fn extent(self) -> usize {
        self.extent
    }

    /// Return corresponding addresses within both aliases of the one backing.
    pub(crate) const fn range_at(
        self,
        offset: usize,
        size: usize,
    ) -> Result<[u64; 2], ComputeBuildError> {
        let end = match offset.checked_add(size) {
            Some(end) => end,
            None => return Err(ComputeBuildError::InvalidSksmEntryAliases),
        };
        if end > self.extent {
            return Err(ComputeBuildError::InvalidSksmEntryAliases);
        }
        Ok([self.low + offset as u64, self.high + offset as u64])
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeRegister {
    pub(crate) number: u32,
    pub(crate) value: u64,
}

impl ComputeRegister {
    const fn new(number: u32, value: u64) -> Self {
        Self { number, value }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeRegisterParameters {
    pub(crate) preempt_base: u64,
    pub(crate) cdm_base: u64,
    pub(crate) usc_exec_base: u64,
    pub(crate) helper_binary: u64,
    pub(crate) helper_data: u64,
    pub(crate) helper_config: u64,
    pub(crate) dispatch_identity: u64,
    pub(crate) context_id: u32,
    pub(crate) work_ordinal: u32,
    pub(crate) robustness: u64,
    pub(crate) operand_state_base: u64,
    pub(crate) execution_gate: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeDescriptorObjects {
    pub(crate) scheduler_record: u64,
    pub(crate) descriptor_low_alias: u64,
    pub(crate) dispatch_a: u64,
    pub(crate) dispatch_b: u64,
    pub(crate) status_a: u64,
    pub(crate) status_b: u64,
    pub(crate) shared_control: u64,
    pub(crate) zero_page: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeDescriptorMetadata {
    pub(crate) submit_sequence: u64,
    pub(crate) context_id: u32,
    pub(crate) grid_index: u32,
    pub(crate) work_ordinal: u32,
    pub(crate) queue_submission: u32,
    pub(crate) queue_ordinal: u32,
    pub(crate) submission_index: u32,
    pub(crate) support_control: u32,
    pub(crate) support_flags: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeDescriptorCommand {
    pub(crate) cdm_terminator: u64,
    pub(crate) sampler_array: u64,
    pub(crate) sampler_count: u32,
    pub(crate) user_timestamp_start: u64,
    pub(crate) user_timestamp_end: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeQueueGraphAddresses {
    pub(crate) queue_record: u64,
    pub(crate) pointers: u64,
    pub(crate) job_list: u64,
    pub(crate) item_ring: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeChannelGroupAddresses {
    pub(crate) queue: u64,
    pub(crate) optional: u64,
    pub(crate) event: u64,
    pub(crate) write_index: u32,
}

fn add(address: u64, offset: u64) -> Result<u64, ComputeBuildError> {
    address
        .checked_add(offset)
        .ok_or(ComputeBuildError::AddressOverflow)
}

/// Build the complete 40-write compute register program.
pub(crate) fn build_compute_registers(
    p: ComputeRegisterParameters,
) -> Result<[ComputeRegister; COMPUTE_REGISTER_COUNT], ComputeBuildError> {
    let context_word = ((p.context_id as u64) << 8) | p.work_ordinal as u64;

    Ok([
        ComputeRegister::new(0x1a510, p.preempt_base),
        ComputeRegister::new(0x1a420, p.cdm_base),
        ComputeRegister::new(0x1a4d0, add(p.preempt_base, 0x1480)?),
        ComputeRegister::new(0x1a4d8, add(p.preempt_base, 0x1488)?),
        ComputeRegister::new(0x1a4e0, add(p.preempt_base, 0x1490)?),
        ComputeRegister::new(0x1a4e8, add(p.preempt_base, 0x1498)?),
        ComputeRegister::new(0x10071, p.usc_exec_base),
        ComputeRegister::new(0x11841, p.helper_binary),
        ComputeRegister::new(0x11849, p.helper_data),
        ComputeRegister::new(0x11f81, p.helper_config),
        ComputeRegister::new(0x1a440, 0x0000_0001_5402_4201),
        ComputeRegister::new(0x1a458, 0x0000_0000_10c0_8860),
        ComputeRegister::new(0x101d9, 0x1c),
        ComputeRegister::new(0x1a089, 0),
        ComputeRegister::new(0x1a091, 0),
        ComputeRegister::new(0x1a059, 0),
        ComputeRegister::new(0x1a061, 0),
        ComputeRegister::new(0x1a0b9, 0),
        ComputeRegister::new(0x1a0c1, 0),
        ComputeRegister::new(0x101d1, 0),
        ComputeRegister::new(0x0d479, 0),
        ComputeRegister::new(0x1a0e9, 8),
        ComputeRegister::new(0x107a1, 0xff_0000),
        ComputeRegister::new(0x0a599, 0x0000_0132_0040_0020),
        ComputeRegister::new(0x0d411, 0x0000_0002_0000_0001),
        ComputeRegister::new(0x1a540, p.dispatch_identity),
        ComputeRegister::new(0x014a9, p.dispatch_identity),
        ComputeRegister::new(0x0a351, p.dispatch_identity),
        ComputeRegister::new(0x10201, context_word),
        ComputeRegister::new(0x10428, context_word),
        ComputeRegister::new(0x14028, p.execution_gate),
        ComputeRegister::new(0x14070, p.robustness | 1),
        ComputeRegister::new(0x10229, add(p.operand_state_base, 0x12800)?),
        ComputeRegister::new(0x140a8, add(p.operand_state_base, 0x13000)?),
        ComputeRegister::new(0x10099, add(p.operand_state_base, 0x9405)?),
        ComputeRegister::new(0x10091, add(p.operand_state_base, 0x12400)?),
        ComputeRegister::new(0x0a5c1, add(p.operand_state_base, 0x0005)?),
        ComputeRegister::new(0x0a5c9, add(p.operand_state_base, 0x9000)?),
        ComputeRegister::new(0x1a440, 0x0000_0001_5402_4209),
        ComputeRegister::new(0x0a599, 0x0000_0060_0040_0020),
    ])
}

fn put_u8(raw: &mut [u8], offset: usize, value: u8) {
    raw[offset] = value;
}

fn put_u16(raw: &mut [u8], offset: usize, value: u16) {
    raw[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(raw: &mut [u8], offset: usize, value: u32) {
    raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(raw: &mut [u8], offset: usize, value: u64) {
    raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

/// Build the retained queue record named by every compute CL entry.
pub(crate) fn build_compute_queue_graph(
    base: u64,
    channel_control: u64,
    raw: &mut [u8],
) -> Result<ComputeQueueGraphAddresses, ComputeBuildError> {
    if raw.len() < COMPUTE_QUEUE_GRAPH_SIZE {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    let addresses = ComputeQueueGraphAddresses {
        queue_record: base,
        pointers: add(base, 0x100)?,
        job_list: add(base, 0x200)?,
        item_ring: add(base, 0x4000)?,
    };
    let raw = &mut raw[..COMPUTE_QUEUE_GRAPH_SIZE];
    raw.fill(0);
    put_u64(raw, 0x00, addresses.pointers);
    put_u64(raw, 0x08, addresses.item_ring);
    put_u64(raw, 0x10, addresses.job_list);
    put_u32(raw, 0x24, u32::MAX);
    put_u32(raw, 0x28, 2);
    put_u32(raw, 0x2c, 2);
    raw[0x36..0x38].fill(0xff);
    put_u32(raw, 0x40, 2);
    put_u32(raw, 0x44, u32::MAX);
    put_u32(raw, 0x48, 0x1aa);
    put_u64(raw, 0x9c, channel_control);
    put_u32(raw, 0x100 + 0x50, u32::MAX);
    put_u32(raw, 0x100 + 0x60, compute_item_ring_entries());
    put_u64(raw, 0x200 + 0x08, addresses.job_list);
    Ok(addresses)
}

pub(crate) fn compute_item_ring_entries() -> u32 {
    const FULL: u32 = 0x500;
    let requested = *crate::module_parameters::g17p_ring_limit.value();
    if requested == 0 {
        return FULL;
    }
    // Must stay a multiple of the per-submission record count so a batch never
    // straddles the wrap differently than it would at full size, and must leave
    // room for at least two submissions plus the always-empty slot.
    let clamped = requested.clamp(
        2 * COMPUTE_REPEAT_RING_RECORD_COUNT as u32 + 4,
        FULL,
    );
    clamped - (clamped % COMPUTE_REPEAT_RING_RECORD_COUNT as u32)
}

pub(crate) fn encode_compute_config_update(
    graph: &mut [u8],
    sksm_entries: ComputeSksmEntryAliases,
    shared_control: u64,
    channel_control: u64,
    queue_id: u8,
    install_queue: bool,
) -> Result<(), ComputeBuildError> {
    let flushid_fix = *crate::module_parameters::g17p_flushid_fix.value() != 0;
    let (flushid_object, flushid_slot, flushid_index) = if flushid_fix {
        (0u64, 0xffffu16, 0xffffu16)
    } else {
        (shared_control, 0x3fu16, 0u16)
    };
    let optional_raw = graph
        .get_mut(COMPUTE_OPTIONAL..COMPUTE_OPTIONAL + COMPUTE_OPTIONAL_SIZE)
        .ok_or(ComputeBuildError::BufferTooSmall)?;
    optional_raw.fill(0);
    put_u32(optional_raw, 0x00, 0x0f);
    put_u64(optional_raw, 0x08, sksm_entries.low());
    put_u64(optional_raw, 0x10, sksm_entries.high());
    for (offset, value) in [
        (0x18, u16::from(queue_id)),
        (0x1a, if install_queue { 1 } else { 0 }),
        (0x1e, 2),
        (0x22, 2),
        (0x32, 2),
        (0x3e, flushid_slot),
        (0x46, flushid_index),
        (0x52, 1),
        (0x56, 1),
        (0x5a, 0x170),
        (0x5e, 2),
        (0x62, 1),
        (0x66, 1),
    ] {
        put_u16(optional_raw, offset, value);
    }
    put_u64(optional_raw, 0x36, flushid_object);
    put_u64(optional_raw, 0x4a, channel_control);
    optional_raw[0x76..0x86].fill(0xff);
    Ok(())
}

pub(crate) fn encode_compute_queue_context_item(
    queue_context: &mut [u8],
    item_index: usize,
    descriptor: u64,
    queue_base: u64,
    queue_id: u8,
) -> Result<(), ComputeBuildError> {
    let offset = item_index
        .checked_mul(COMPUTE_QUEUE_CONTEXT_ITEM)
        .ok_or(ComputeBuildError::AddressOverflow)?;
    let end = offset
        .checked_add(COMPUTE_QUEUE_CONTEXT_ITEM)
        .ok_or(ComputeBuildError::AddressOverflow)?;
    if end > queue_context.len() {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    let context = &mut queue_context[offset..end];
    context.fill(0);
    put_u64(context, 0x00, 0x1000_1000_0000_0004);
    put_u64(context, 0x10, descriptor);
    put_u64(context, 0x18, queue_base);
    put_u64(context, 0x20, 0xffff_0801_0000_0001);
    put_u64(context, 0x28, u64::from(queue_id) << 40);
    put_u64(context, 0x130, 0);
    put_u64(context, 0x138, 4);
    put_u64(context, 0x150, 0x0001_1003_8001_a002);
    put_u64(context, 0x158, 0x0000_2003_8001_a03b);
    put_u64(context, 0x178, 0x003f_ffff_ffff_ffff);
    Ok(())
}

pub(crate) fn build_initial_compute_channel_group(
    base: u64,
    descriptor: u64,
    sksm_entries: ComputeSksmEntryAliases,
    shared_control: u64,
    channel_control: u64,
    queue_id: u8,
    graph: &mut [u8],
    queue_context: &mut [u8],
    install_queue: bool,
) -> Result<ComputeChannelGroupAddresses, ComputeBuildError> {
    if graph.len() < COMPUTE_QUEUE_GRAPH_SIZE
        || queue_context.len() < COMPUTE_QUEUE_CONTEXT_SIZE
    {
        return Err(ComputeBuildError::BufferTooSmall);
    }

    let optional = add(base, COMPUTE_OPTIONAL as u64)?;
    let event = add(base, COMPUTE_EVENT as u64)?;

    put_u32(graph, 0x48, 0x170);
    for (index, address) in [descriptor, optional, event].into_iter().enumerate() {
        put_u64(graph, COMPUTE_ITEM_RING + index * 8, address);
    }
    put_u32(
        graph,
        COMPUTE_QUEUE_POINTERS + 0x40,
        COMPUTE_CHANNEL_WRITE_INDEX,
    );

    encode_compute_config_update(
        graph,
        sksm_entries,
        shared_control,
        channel_control,
        queue_id,
        install_queue,
    )?;

    let event_raw = &mut graph[COMPUTE_EVENT..COMPUTE_EVENT + COMPUTE_EVENT_SIZE];
    event_raw.fill(0);
    put_u32(event_raw, 0x00, 0x0e);
    put_u32(event_raw, 0x04, (1u32 << 16) | u32::from(queue_id));
    put_u32(event_raw, 0x08, 0x0102);
    put_u32(event_raw, 0x10, 0x0200);

    queue_context[..COMPUTE_QUEUE_CONTEXT_SIZE].fill(0);
    // Item index 1, which is also the stamp index the first bind consumes
    // (the geometry seeds `current_timestamp` at 1). See
    // `encode_compute_queue_context_item` for why that coincidence matters.
    encode_compute_queue_context_item(queue_context, 1, descriptor, base, queue_id)?;

    Ok(ComputeChannelGroupAddresses {
        queue: base,
        optional,
        event,
        write_index: COMPUTE_CHANNEL_WRITE_INDEX,
    })
}

fn encode_registers(raw: &mut [u8], offset: usize, registers: &[ComputeRegister]) {
    for (index, register) in registers.iter().enumerate() {
        let entry = offset + index * COMPUTE_REGISTER_SIZE;
        put_u32(raw, entry, register.number);
        put_u64(raw, entry + 4, register.value);
    }
}

fn register_value(registers: &[ComputeRegister], number: u32, occurrence: usize) -> u64 {
    registers
        .iter()
        .filter(|register| register.number == number)
        .nth(occurrence)
        .expect("fixed G17P register program lost a required register")
        .value
}

/// Encode a selector-3 descriptor without dropping sampler, timestamp, or CDM
/// command state.
pub(crate) fn build_compute_descriptor(
    registers: &[ComputeRegister; COMPUTE_REGISTER_COUNT],
    objects: ComputeDescriptorObjects,
    metadata: ComputeDescriptorMetadata,
    command: ComputeDescriptorCommand,
    raw: &mut [u8],
) -> Result<(), ComputeBuildError> {
    if raw.len() < COMPUTE_DESCRIPTOR_SIZE {
        return Err(ComputeBuildError::BufferTooSmall);
    }
    if command.cdm_terminator < register_value(registers, 0x1a420, 0) {
        return Err(ComputeBuildError::CdmTerminatorBeforeStream);
    }
    if metadata.submission_index == 0 {
        return Err(ComputeBuildError::SubmissionIndexZero);
    }
    if metadata.queue_submission == 0 {
        return Err(ComputeBuildError::QueueSubmissionZero);
    }
    if command.sampler_count == u32::MAX {
        return Err(ComputeBuildError::SamplerCountOutOfRange);
    }
    if (command.sampler_array == 0) != (command.sampler_count == 0) {
        return Err(ComputeBuildError::SamplerPair);
    }
    if command.sampler_array & 7 != 0 {
        return Err(ComputeBuildError::SamplerAlignment);
    }

    let raw = &mut raw[..COMPUTE_DESCRIPTOR_SIZE];
    raw.fill(0);
    put_u32(raw, 0x00, COMPUTE_SELECTOR);
    put_u64(raw, 0x04, metadata.submit_sequence);
    put_u32(raw, 0x0c, metadata.context_id);
    put_u64(raw, 0x10, objects.scheduler_record);
    for (index, value) in [0x22u16, 0x23, 0x23, 0x24].into_iter().enumerate() {
        put_u16(raw, 0x18 + index * 2, value);
    }

    encode_registers(raw, COMPUTE_REGISTER_START, registers);
    put_u64(
        raw,
        0x740,
        add(objects.descriptor_low_alias, COMPUTE_REGISTER_START as u64)?,
    );
    put_u32(
        raw,
        COMPUTE_PRIMARY_COUNT,
        ((COMPUTE_REGISTER_COUNT * COMPUTE_REGISTER_SIZE) as u32) << 16
            | COMPUTE_REGISTER_COUNT as u32,
    );

    let secondary = [
        ComputeRegister::new(0x10099, register_value(registers, 0x0a5c1, 0)),
        ComputeRegister::new(0x10091, register_value(registers, 0x0a5c9, 0)),
        ComputeRegister::new(0x0a5c1, register_value(registers, 0x10099, 0)),
        ComputeRegister::new(0x0a5c9, register_value(registers, 0x10091, 0)),
    ];
    encode_registers(raw, COMPUTE_SECONDARY_REGISTER_START, &secondary);
    put_u64(
        raw,
        0x0e60,
        add(
            objects.descriptor_low_alias,
            COMPUTE_SECONDARY_REGISTER_START as u64,
        )?,
    );
    put_u32(raw, 0x0e68, 0x0030_0004);
    put_u32(raw, 0x0f28, u32::MAX);

    put_u64(raw, 0x0ed8, register_value(registers, 0x1a510, 0));
    put_u64(raw, 0x0ee0, command.cdm_terminator);
    put_u64(raw, 0x0f08, register_value(registers, 0x1a440, 0));
    put_u32(
        raw,
        0x0f20,
        (register_value(registers, 0x1a540, 0) >> 32) as u32,
    );
    put_u64(raw, COMPUTE_SAMPLER_ARRAY, command.sampler_array);
    put_u32(raw, COMPUTE_SAMPLER_COUNT, command.sampler_count);
    put_u32(
        raw,
        COMPUTE_SAMPLER_MAX,
        if command.sampler_count == 0 {
            0
        } else {
            command.sampler_count + 1
        },
    );
    put_u64(raw, 0x0f40, objects.dispatch_a);
    put_u64(raw, 0x0f48, objects.dispatch_b);
    put_u32(raw, 0x0f50, metadata.queue_submission << 8);
    put_u32(raw, 0x0f54, metadata.grid_index);
    put_u32(raw, 0x0f58, metadata.queue_ordinal);
    put_u32(raw, 0x0f60, metadata.submission_index);
    put_u64(
        raw,
        0x0f68,
        register_value(registers, 0x1a540, 0) & 0xffff_ffff,
    );
    put_u32(raw, 0x0f70, metadata.work_ordinal);
    put_u64(raw, 0x0f7c, objects.status_a);
    put_u64(raw, 0x0f84, objects.status_b);
    put_u64(raw, 0x0f8c, command.user_timestamp_start);
    put_u64(raw, 0x0f94, command.user_timestamp_end);

    put_u16(raw, 0x0fb0, 0x001a);
    put_u64(raw, 0x0fb2, objects.shared_control);
    put_u32(raw, 0x0fba, metadata.support_control);
    put_u32(raw, 0x0fbe, metadata.support_flags);
    put_u8(raw, 0x0fc5, 0x9f);
    put_u32(raw, 0x0fc8, (metadata.work_ordinal & 3) << 30);
    put_u64(raw, 0x0fcb, objects.zero_page);
    put_u8(raw, 0x0fd3, 1);
    Ok(())
}

