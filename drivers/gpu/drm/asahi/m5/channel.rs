// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! T8142/25G76 channel configuration, separate from Neo's HAL200 path.
//!
//! Native AGXCLChannelSKSM::submitBuffer inlines this packed 0x86-byte
//! record at 0xfffffe000898488c..0x8984bd8. The controlled native sequence
//! is a selector-3 register program, this tag-15 record, then a tag-14 kick.
//! These builders do not map memory, install a queue or publish a producer.
//! Every pointer and event identifier must belong to the retained session.

pub(crate) const M5_CHANNEL_CONFIG_BYTES: usize = 0x86;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum M5ChannelError {
    QueueId,
    Priority,
    Address,
    Context,
    DataMaster,
}

/// Optional TA parameter-buffer event, separate from the first event tuple.
/// The native TA writer at 25G76 0x8a249ac..0x8a249e4 obtains these values
/// from its retained PM manager and command descriptor.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ParameterBufferEvent {
    pub(crate) address: u64,
    pub(crate) timestamp: u64,
    /// TA descriptor +0x1218, independently selected by the owned PB table.
    pub(crate) hardware_buffer_id: u32,
    /// Native PM manager +0x79c. This is not the SKSM queue ID.
    pub(crate) manager_id: u32,
}

/// Optional retained event-machine object, with the native timestamp and
/// index. None emits the native disabled branch's full-width sentinels.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ChannelEvent {
    pub(crate) address: u64,
    pub(crate) timestamp: u64,
    pub(crate) index: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ChannelConfig {
    pub(crate) ring_low: u64,
    pub(crate) ring_high: u64,
    pub(crate) queue_id: u8,
    pub(crate) priority: u8,
    /// Native descriptor +0x181 bit 0. Mode 3 validates the first queue
    /// installation on M5; an already-installed native capture has 0.
    pub(crate) install_queue: bool,
    /// Native descriptor +0x144, independently matched to the UAT context.
    pub(crate) gpu_context: u8,
    pub(crate) event: Option<M5ChannelEvent>,
    /// AGXCommandQueue +0x8b8: retained scheduler/control object.
    pub(crate) channel_control: u64,
    /// Native descriptor +0x2dc bit 0.
    pub(crate) update_scheduler: bool,
    /// AGXCommandQueue +0x898 object's +0x10. Not the GPU context ID.
    pub(crate) scheduler_index: u32,
    /// getStampIndex on descriptor +0x170. Not a fixed target constant.
    pub(crate) channel_stamp_index: u32,
    /// Native descriptor +0x180 bit 0.
    pub(crate) update_context: bool,
    /// Descriptor +0x58 owner's virtual context value. Distinct from UAT.
    pub(crate) owner_context: u32,
    /// Native channel-state +0x84 is nonzero.
    pub(crate) extended_command: bool,
}

fn firmware_address(address: u64) -> bool {
    address >= 0xffff_fc00_0000_0000 && address & 7 == 0
}

fn put32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

pub(crate) fn encode_m5_channel_config(
    p: M5ChannelConfig,
) -> Result<[u8; M5_CHANNEL_CONFIG_BYTES], M5ChannelError> {
    encode_m5_channel_config_for(p, 2, 2, None, None)
}

/// The two selectors are independent: native TA/fragment use 0/1 at +0x22
/// while both retain 2 at +0x5e. Changing both by stage breaks that contract.
/// gpu_state_update retains the separate +0x26 flag/+0x2a value pair;
/// the native TA writer copies descriptor +0xe41/+0xe48 to these fields.
pub(crate) fn encode_m5_channel_config_for(
    p: M5ChannelConfig,
    data_master: u32,
    channel_data_master: u32,
    gpu_state_update: Option<u64>,
    parameter_buffer: Option<M5ParameterBufferEvent>,
) -> Result<[u8; M5_CHANNEL_CONFIG_BYTES], M5ChannelError> {
    if data_master > 2
        || channel_data_master > 2
        || (parameter_buffer.is_some() && data_master != 0)
    {
        return Err(M5ChannelError::DataMaster);
    }
    if p.queue_id > 127 {
        return Err(M5ChannelError::QueueId);
    }
    if p.priority > 3 {
        return Err(M5ChannelError::Priority);
    }
    if p.gpu_context == 0 {
        return Err(M5ChannelError::Context);
    }
    if p.ring_low == 0
        || p.ring_low >= (1 << 42)
        || p.ring_low & 0x3fff != 0
        || !firmware_address(p.ring_high)
        || p.ring_high & 0x3fff != 0
        || !firmware_address(p.channel_control)
    {
        return Err(M5ChannelError::Address);
    }
    if let Some(event) = p.event {
        if !firmware_address(event.address) {
            return Err(M5ChannelError::Address);
        }
    }
    if let Some(event) = parameter_buffer {
        if !firmware_address(event.address) {
            return Err(M5ChannelError::Address);
        }
    }
    let mut bytes = [0; M5_CHANNEL_CONFIG_BYTES];
    put32(&mut bytes, 0, 15);
    put64(&mut bytes, 8, p.ring_low);
    put64(&mut bytes, 0x10, p.ring_high);
    bytes[0x18..0x1a].copy_from_slice(&u16::from(p.queue_id).to_le_bytes());
    for (offset, value) in [
        (0x1a, u32::from(p.install_queue)),
        (0x1e, u32::from(p.priority)),
        (0x22, data_master),
        (0x32, u32::from(p.gpu_context)),
        (0x52, u32::from(p.update_scheduler)),
        (0x56, p.scheduler_index),
        (0x5a, p.channel_stamp_index),
        (0x5e, channel_data_master),
        (0x62, u32::from(p.update_context)),
        (0x66, p.owner_context),
        (0x6a, u32::from(p.extended_command)),
    ] {
        put32(&mut bytes, offset, value);
    }
    if let Some(value) = gpu_state_update {
        put32(&mut bytes, 0x26, 1);
        put64(&mut bytes, 0x2a, value);
    }
    match p.event {
        Some(event) => {
            put64(&mut bytes, 0x36, event.address);
            put64(&mut bytes, 0x3e, event.timestamp);
            put32(&mut bytes, 0x46, event.index);
        }
        None => {
            put64(&mut bytes, 0x3e, u64::MAX);
            put32(&mut bytes, 0x46, u32::MAX);
        }
    }
    put64(&mut bytes, 0x4a, p.channel_control);
    if let Some(event) = parameter_buffer {
        put64(&mut bytes, 0x6e, event.address);
        put64(&mut bytes, 0x76, event.timestamp);
        put32(&mut bytes, 0x7e, event.hardware_buffer_id);
        put32(&mut bytes, 0x82, event.manager_id);
    } else {
        bytes[0x76..0x86].fill(0xff);
    }
    Ok(bytes)
}

