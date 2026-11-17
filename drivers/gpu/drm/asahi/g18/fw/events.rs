// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Decoder of the firmware completion-stamp event that G18 reads from its report ring.
//!
//! Items copied verbatim from the J873 module-1134 sources (provenance in
//! the port's extraction ledger); only module paths
//! are rewritten.

pub(crate) const G17P_FIRMWARE_EVENT_ENTRY_SIZE: usize = 0x48;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum CompletionDecodeError {
    RecordSize,
    ChannelNumber,
    EventType,
    TraceShape,
    CompletionSelector,
    CompletionCommandType,
    CompletionCapacity,
    CompletionCursor,
    CursorRange,
    QueueCounterOrder,
    QueueCounterRegression,
    PublishedPrefixBeyondWrite,
    RenderWindow,
    RenderCursorOutsideWindow,
    CompletionIrqSource,
    CompletionOrdinal,
    CompletionNotificationCount,
    CompletionQueueId,
    CompletionTimestamp,
    CompletionTimestampOrder,
    FirmwareEventType,
    FirmwareEventDiagnosticCount,
    FirmwareEventStampIndex,
    FirmwareEventStampMissing,
    FirmwareEventUmaPoolIndex,
    FirmwareEventTvbRequest,
    FirmwareEventUmaAddress,
    CompletionOutstanding,
    CompletionRouteUnavailable,
    FirmwareEventEndpoint,
    FirmwareEventMessage,
    FirmwareEventCursor,
    FirmwareEventEmpty,
}

fn get_u16(raw: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(raw[offset..offset + 2].try_into().unwrap())
}

fn get_u32(raw: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(raw[offset..offset + 4].try_into().unwrap())
}

fn get_u64(raw: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(raw[offset..offset + 8].try_into().unwrap())
}

/// Firmware event type 1, delivered through the ordinary EP0x20 event ring.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareStampEvent {
    pub(crate) masks: [u64; 2],
    pub(crate) diagnostic_pair_count: u16,
}

pub(crate) fn decode_g17p_firmware_stamp_event(
    raw: &[u8],
) -> Result<G17PFirmwareStampEvent, CompletionDecodeError> {
    if raw.len() != G17P_FIRMWARE_EVENT_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }
    if get_u32(raw, 0x00) != 1 {
        return Err(CompletionDecodeError::FirmwareEventType);
    }
    let diagnostic_pair_count = get_u16(raw, 0x14);
    if diagnostic_pair_count >= 6 {
        return Err(CompletionDecodeError::FirmwareEventDiagnosticCount);
    }
    Ok(G17PFirmwareStampEvent {
        masks: [get_u64(raw, 0x04), get_u64(raw, 0x0c)],
        diagnostic_pair_count,
    })
}
