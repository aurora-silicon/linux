// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]


pub(crate) const REPORT_ENTRY_SIZE: usize = 0x40;
pub(crate) const REPORT_CHANNEL_EVENT: u8 = 13;
pub(crate) const REPORT_CHANNEL_TRACE: u8 = 14;
pub(crate) const G17P_COMPUTE_COMPLETION_SELECTOR: u8 = 2;
pub(crate) const KSM_COMPLETION_ENTRY_SIZE: usize = 0x40;
pub(crate) const D93AP_G17P_COMPLETION_IRQ: u32 = 1001;
pub(crate) const G17P_NORMAL_CL_COMPLETION_IRQ_BIT: u8 = 21;
pub(crate) const G17P_NORMAL_CL_COMPLETION_ORDINAL: u8 = 0;
pub(crate) const G17P_NORMAL_CL_COMPLETION_CAPACITY: u32 = 0x800;
pub(crate) const G17P_NORMAL_CL_COMPLETION_DESCRIPTOR_TABLE_OFFSET: u32 = 0x2614;
pub(crate) const G17P_NORMAL_CL_COMPLETION_DESCRIPTOR_STRIDE: u32 = 0x20;
pub(crate) const G17P_NORMAL_CL_COMPLETION_PROCESSED_OFFSET: u32 = 0x1c;
pub(crate) const G17P_KSM_COMPLETION_NOTIFY_BASE: u32 = 0x21168;
pub(crate) const G17P_KSM_COMPLETION_ACK_BASE: u32 = 0x21188;
pub(crate) const G17P_KSM_COMPLETION_MMIO_STRIDE: u32 = 8;
pub(crate) const G17P_FIRMWARE_EVENT_ENDPOINT: u8 = 0x20;
pub(crate) const G17P_FIRMWARE_EVENT_MESSAGE: u64 = 0x0042_0000_0000_0000;
pub(crate) const G17P_FIRMWARE_EVENT_HOST_TYPE: u8 = 2;
pub(crate) const G17P_FIRMWARE_EVENT_HOST_TYPE_MASK: u8 = 0x3f;
pub(crate) const G17P_FIRMWARE_EVENT_ENTRY_SIZE: usize = 0x48;
pub(crate) const G17P_FIRMWARE_EVENT_CAPACITY: u32 = 0x100;
pub(crate) const G17P_FIRMWARE_EVENT_CONSUMER_OFFSET: u32 = 0;
pub(crate) const G17P_FIRMWARE_EVENT_PRODUCER_OFFSET: u32 = 0x20;
/// Work-completion record. The firmware publishes one of these when a
/// submission retires, carrying the event stamp (which advances by 0x100 per
/// completion, as on M1/M2), the queue index, the command UUID and the context.
pub(crate) const G17P_FIRMWARE_EVENT_COMPLETION_TYPE: u32 = 1;
/// `kAGFIFirmwareEventTypeStampSignal`: a bitmask of the stamp indices the
/// firmware has just signalled, in the same 128-bit space the completion
/// records index. Both the working compute path and the failing render path
/// emit one, so it is the firmware's own account of which stamps moved.
pub(crate) const G17P_FIRMWARE_EVENT_STAMP_TYPE: u32 = 1;
pub(crate) const G17P_FIRMWARE_EVENT_RECOVERY_TYPE: u32 = 4;
pub(crate) const G17P_FIRMWARE_EVENT_UMA_GROW_TYPE: u32 = 13;
pub(crate) const D93AP_G17P_SGX_SOURCE_INDEX: u8 = 4;
pub(crate) const D93AP_G17P_SGX_INTERRUPTS_VALID: u32 = 0xdf;


/// Channel-table index carrying the event state/ring pointer pair.
pub(crate) const G17P_EVENT_CHANNEL_TABLE_INDEX: usize = 13;
pub(crate) const G17P_EVENT_STATE_STATUS_A_OFFSET: u64 = 0x0_0040;
pub(crate) const G17P_EVENT_RING_STATUS_A_OFFSET: u64 = 0x0_02c0;
/// Firmware-log ring-0 control block. Read only to show, in the same line,
/// that it is *not* the event producer.
pub(crate) const G17P_FWLOG_CONTROL_STATUS_A_OFFSET: u64 = 0x0_0080;
/// 0x100 entries. Same capacity as `G17P_FIRMWARE_EVENT_CAPACITY`, which
/// describes this same ring.
pub(crate) const G17P_EVENT_CAPACITY: u32 = G17P_FIRMWARE_EVENT_CAPACITY;
/// 0x48 bytes per entry -- see the block comment above. Deliberately NOT
/// `REPORT_ENTRY_SIZE`.
pub(crate) const G17P_EVENT_ENTRY_SIZE: usize = G17P_FIRMWARE_EVENT_ENTRY_SIZE;
/// Firmware event type names, for logging. Type 4 is the one that matters:
/// the firmware halted and is asking the host to run the restart handshake.
pub(crate) fn g17p_firmware_event_name(event_type: u32) -> &'static str {
    match event_type {
        0 => "fault",
        1 => "stamp-flags",
        4 => "gpu-restart",
        7 => "uma-grow-request",
        8 => "channel-error",
        13 => "uma-async-grow-complete",
        _ => "unmodelled",
    }
}
/// Host-written read pointer, `state + 0x00`.
pub(crate) const G17P_EVENT_CONSUMER_OFFSET: u64 = 0x00;
/// Firmware-written write pointer, `state + 0x20`.
pub(crate) const G17P_EVENT_PRODUCER_OFFSET: u64 = 0x20;

impl Channel13EventRecord {
    /// Short stable name for logging.
    pub(crate) fn name(&self) -> &'static str {
        match self {
            Channel13EventRecord::Fault(_) => "fault",
            Channel13EventRecord::Flag(_) => "flag",
            Channel13EventRecord::Timeout(_) => "timeout",
            Channel13EventRecord::GrowTilingBuffer(_) => "grow-tiling-buffer",
            Channel13EventRecord::ChannelError => "channel-error",
        }
    }
}

// ---------------------------------------------------------------------------
// KTrace ring -- channel-table entry 14, states[0]/states[1].
//
// `main_config + 0x1e0` is the KTrace state block (`status_a + 0x240`) and
// `main_config + 0x1e8` is the 0x200 x 0x48 KTrace ring (`status_a + 0xa6ac0`).
// The Stats channel shares the same table entry through `states[2]` and
// `ring`, which is why `REPORT_ENTRY_SIZE` / `REPORT_CURSOR_MASK` /
// `SplitReportCursor` must not be reused here: this ring has 0x200 entries of
// 0x48 bytes and cursors that run to 0x1ff.
//
// The firmware emitter writes every u64 field with `stur`, so all of them are
// unaligned; the byte-copy `get_u64` helper below already handles that. Bytes
// 0x34..0x47 of a record are never written by the firmware and must not be
// read.
// ---------------------------------------------------------------------------

/// Channel-table index carrying the KTrace state/ring pointer pair.
pub(crate) const G17P_KTRACE_CHANNEL_TABLE_INDEX: usize = 14;
pub(crate) const G17P_KTRACE_STATE_STATUS_A_OFFSET: u64 = 0x0_0240;
pub(crate) const G17P_KTRACE_RING_STATUS_A_OFFSET: u64 = 0xa_6ac0;
pub(crate) const G17P_KTRACE_ENTRY_SIZE: usize = 0x48;
pub(crate) const G17P_KTRACE_CAPACITY: u32 = 0x200;
/// Host-written read pointer, `state + 0x00`.
pub(crate) const G17P_KTRACE_CONSUMER_OFFSET: u64 = 0x00;
/// Firmware-written write pointer, `state + 0x20`.
pub(crate) const G17P_KTRACE_PRODUCER_OFFSET: u64 = 0x20;
/// `kAGFIFirmwareEventTypeFWTrace`.
pub(crate) const G17P_KTRACE_RECORD_TYPE: u32 = 5;
/// Channel 1 code 0x1c: records-lost marker. Emitted from inside the emitter
/// itself with no class-mask test, so it appears whatever the mask is.
pub(crate) const G17P_KTRACE_LOST_KEY: u16 = 0x011c;
/// Channel 0 code 0x41: tag-14 `KsmKickQueueAddKicks`. No branch separates the
/// trace call from the `sgx+0x1_03a8` / `sgx+0x2_1018` stores, so one of these
/// records proves the KSM was programmed.
pub(crate) const G17P_KTRACE_ADD_KICKS_KEY: u16 = 0x0041;
/// Channel 0 code 0x46: KSM scheduling-pause reason-mask change,
/// `(set?, reason_bit, resulting_mask)`.
pub(crate) const G17P_KTRACE_PAUSE_REASON_KEY: u16 = 0x0046;

/// B1's DM progress sampler only selects registers for this exact c020
/// predicate. A zero result must not be called an idle shader or completion.
pub(crate) const fn g17p_slot_has_progress_selector(state: u64) -> bool {
    state & 1 != 0 && state & 0x3c == 0x0c && (state >> 24) & 3 != 0
}

/// Exact DM1 sample ranges in B1 tables45580/4558c. The second tuple member
/// is the extended read namespace, not a value to write to a PDM selector.
pub(crate) const G17P_DM1_MONITOR_READS: [(u64, u64); 8] = [
    (0x1668, 0),
    (0x13020, (1u64 << 42) | (2u64 << 40)),
    (0x15150, (1u64 << 42) | (2u64 << 40)),
    (0x15388, (1u64 << 42) | (2u64 << 40)),
    (0x15390, (1u64 << 42) | (2u64 << 40)),
    (0x15510, (1u64 << 42) | (2u64 << 40)),
    (0x15518, (1u64 << 42) | (2u64 << 40)),
    (0xa068, (1u64 << 42) | (1u64 << 40)),
];

pub(crate) const fn g17p_dm1_monitor_selector_index(state: u64) -> Option<u64> {
    if g17p_slot_has_progress_selector(state) {
        Some(((state >> 24) & 3) - 1)
    } else {
        None
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PHaltedSlot0Gate {
    pub(crate) handshake_state: u32,
    pub(crate) epoch: u64,
    pub(crate) expected_epoch: u64,
    pub(crate) report_state: u32,
    pub(crate) host_requested: u32,
    pub(crate) slot_present: u32,
    pub(crate) slot: u32,
    pub(crate) data_master_present: u32,
    pub(crate) data_master: u32,
}

/// A reason-3 report is independent of the copied hardware-slot mask. B1
/// selects its first valid GMMU requestor before the state-1 host handshake.
pub(crate) const fn g17p_halted_gmmu_sample_allowed(
    handshake_state: u32,
    epoch: u64,
    expected_epoch: u64,
    report_state: u32,
    host_requested: u32,
    reason: u32,
) -> bool {
    handshake_state == 1 && epoch == expected_epoch && report_state == 2
        && host_requested == 0 && reason == 3
}

impl G17PHaltedSlot0Gate {
    pub(crate) const fn allows_sample(self) -> bool {
        self.handshake_state == 1 && self.epoch == self.expected_epoch
            && self.report_state == 2 && self.host_requested == 0
            && self.slot_present == 1 && self.slot == 0
            && self.data_master_present == 1 && self.data_master == 1
    }
}

/// Submission-path keys printed by default (`channel << 8 | code`).
pub(crate) const G17P_KTRACE_INTEREST_KEYS: [u16; 20] = [
    0x0002, // firmware job creation (new-work IRQ)
    0x0003, // tiling done (IRQ bit 3)
    0x001c, // DM launch strobe: a2=0 phase 1, a2=1 the 3D launch
    0x001f, // job waiting on a host reply to message type 6
    0x0045, // queue resume request
    0x0058,
    0x0060, // job freed via the flist fast path
    0x000a, // stamp update / retire
    0x000c, // stamp update / retire
    0x000e, // stamp update / retire
    0x0020, // device-control command retired
    0x0040, // tag 15 queue programmed (emitted at b1 0x11a84)
    0x0041, // tag 14 KsmKickQueueAddKicks
    0x0042, // queue stopped with pending kicks -> re-add
    0x0043, // completion processing entry
    0x0046, // KSM scheduling-pause reason-mask change
    0x0047, // classic work-channel drain
    0x004b, // kick-queue config value change (tag 15)
    0x0057, // tag 16 KsmKickQueueEntrySignal
    0x011c, // records lost
];

pub(crate) fn g17p_ktrace_key_is_interesting(key: u16) -> bool {
    key < 0x100 || G17P_KTRACE_INTEREST_KEYS.contains(&key)
        || g17p_ktrace_key_is_dm_monitor(key)
}

pub(crate) const fn g17p_ktrace_key_is_dm_monitor(key: u16) -> bool {
    matches!(key, 0x0110 | 0x0900..=0x0909)
}

#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct G17PKtracePrintBudget {
    pub(crate) ordinary_printed: u32,
    pub(crate) submission_printed: u32,
    pub(crate) submission_dropped: u32,
    pub(crate) monitor_printed: u32,
    pub(crate) monitor_dropped: u32,
}

impl G17PKtracePrintBudget {
    pub(crate) fn allow(&mut self, key: u16, ordinary_limit: u32) -> bool {
        if g17p_ktrace_key_is_dm_monitor(key) {
            if self.monitor_printed >= 64 {
                self.monitor_dropped = self.monitor_dropped.saturating_add(1);
                return false;
            }
            self.monitor_printed += 1;
            return true;
        }
        if key < 0x100 {
            if self.submission_printed >= 64 {
                self.submission_dropped = self.submission_dropped.saturating_add(1);
                return false;
            }
            self.submission_printed += 1;
            return true;
        }
        if key == G17P_KTRACE_LOST_KEY
            || ordinary_limit == 0
            || self.ordinary_printed < ordinary_limit
        {
            self.ordinary_printed = self.ordinary_printed.saturating_add(1);
            return true;
        }
        false
    }
}

/// Short human tail naming the argument shape of the fixed-shape records.
pub(crate) const fn g17p_ktrace_key_name(key: u16) -> &'static str {
    match key {
        0x0040 => "tag15-programmed qid/base/dm/prio",
        0x0041 => "tag14-addkicks qid/stamp/slot/kicks",
        0x0046 => "pause-reason set/bit/mask",
        0x004b => "cfg qid/old/new",
        0x0057 => "tag16-signal qid/stamp/value",
        0x000a | 0x000c | 0x000e => "stamp-update value/index/uuid/ctx",
        0x0020 => "devctl-retired",
        0x011c => "RECORDS-LOST count/window",
        0x0110 => "monitor-sample dm/slot/threshold",
        0x0900 => "monitor-time delta/now/last-completion-drain (ticks/24)",
        0x0901 => "monitor-classify state1",
        0x0902 => "monitor-classify state2",
        0x0903 => "monitor-state-change dm/slot/state",
        0x0904 => "monitor-key-changed dm/slot/previous-key",
        0x0909 => "monitor-recovery-scan total/state1/state2/raw-flags",
        _ => "",
    }
}

/// One decoded KTrace record.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PKtraceRecord {
    pub(crate) timestamp: u64,
    pub(crate) args: [u64; 4],
    pub(crate) code: u8,
    pub(crate) channel: u8,
    pub(crate) thread: u8,
    pub(crate) flag: u32,
}

impl G17PKtraceRecord {
    /// `channel << 8 | code`, the key used by the interest filter.
    pub(crate) const fn key(&self) -> u16 {
        ((self.channel as u16) << 8) | self.code as u16
    }
}

pub(crate) fn decode_g17p_ktrace(raw: &[u8]) -> Result<G17PKtraceRecord, CompletionDecodeError> {
    if raw.len() != G17P_KTRACE_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }
    if get_u32(raw, 0x00) != G17P_KTRACE_RECORD_TYPE {
        return Err(CompletionDecodeError::TraceShape);
    }
    let code_thread = get_u32(raw, 0x2c);
    Ok(G17PKtraceRecord {
        timestamp: get_u64(raw, 0x04),
        args: [
            get_u64(raw, 0x0c),
            get_u64(raw, 0x14),
            get_u64(raw, 0x1c),
            get_u64(raw, 0x24),
        ],
        code: (code_thread & 0xff) as u8,
        channel: ((code_thread >> 8) & 0xff) as u8,
        thread: (code_thread >> 24) as u8,
        flag: get_u32(raw, 0x30),
    })
}

/// Wrap/empty logic for the trace ring. Returns the byte offset of the record
/// at `consumer` and the read pointer to publish once it has been copied.
///
/// Both cursors are indices already reduced mod 0x200; the firmware publishes
/// `(producer + 1) & 0x1ff`, so a difference must never be re-masked before
/// being used as a count.
pub(crate) const fn prepare_g17p_ktrace_read(
    consumer: u32,
    producer: u32,
) -> Result<(u32, u32), CompletionDecodeError> {
    if consumer >= G17P_KTRACE_CAPACITY || producer >= G17P_KTRACE_CAPACITY {
        return Err(CompletionDecodeError::FirmwareEventCursor);
    }
    if consumer == producer {
        return Err(CompletionDecodeError::FirmwareEventEmpty);
    }
    Ok((
        consumer * G17P_KTRACE_ENTRY_SIZE as u32,
        (consumer + 1) & (G17P_KTRACE_CAPACITY - 1),
    ))
}

const REPORT_CURSOR_MASK: u32 = 0xff;
const KSM_TIMESTAMP_MASK: u64 = 0x00ff_ffff_ffff;
const KSM_QID_MASK: u8 = 0x7f;
const KSM_PARENT_MASK: u64 = 0x003f_ffff_ffff_ffff;

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

fn get_i32(raw: &[u8], offset: usize) -> i32 {
    i32::from_le_bytes(raw[offset..offset + 4].try_into().unwrap())
}

fn get_u64(raw: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(raw[offset..offset + 8].try_into().unwrap())
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct SplitReportCursor {
    pub(crate) host: u8,
    pub(crate) firmware: u8,
}

impl SplitReportCursor {
    pub(crate) fn from_words(host: u32, firmware: u32) -> Result<Self, CompletionDecodeError> {
        if host & !REPORT_CURSOR_MASK != 0 || firmware & !REPORT_CURSOR_MASK != 0 {
            return Err(CompletionDecodeError::CursorRange);
        }
        Ok(Self {
            host: host as u8,
            firmware: firmware as u8,
        })
    }

    pub(crate) const fn pending(self) -> u8 {
        self.firmware.wrapping_sub(self.host)
    }

    pub(crate) const fn acknowledgement(self) -> u32 {
        self.firmware as u32
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ReportCounters {
    pub(crate) state: [u32; 3],
}

impl ReportCounters {
    pub(crate) const fn visible_record_count(self) -> u32 {
        let first = if self.state[0] > self.state[1] {
            self.state[0]
        } else {
            self.state[1]
        };
        if first > self.state[2] {
            first
        } else {
            self.state[2]
        }
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct Channel13FaultRecord {
    pub(crate) payload: [u8; 0x34],
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct Channel13FlagRecord {
    pub(crate) firing: [u64; 2],
    pub(crate) value_14: u16,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct Channel13TimeoutRecord {
    pub(crate) counter: u64,
    pub(crate) stamp_index: i32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct Channel13GrowthRecord {
    pub(crate) vm_id: u32,
    pub(crate) buffer_manager_id: u32,
    pub(crate) counter: u32,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum Channel13EventRecord {
    Fault(Channel13FaultRecord),
    Flag(Channel13FlagRecord),
    Timeout(Channel13TimeoutRecord),
    GrowTilingBuffer(Channel13GrowthRecord),
    ChannelError,
}

pub(crate) fn decode_channel13_event(
    channel: u8,
    raw: &[u8],
) -> Result<Channel13EventRecord, CompletionDecodeError> {
    if channel != REPORT_CHANNEL_EVENT {
        return Err(CompletionDecodeError::ChannelNumber);
    }
    if raw.len() != REPORT_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }
    match get_u32(raw, 0) {
        0 => {
            let mut payload = [0u8; 0x34];
            payload.copy_from_slice(&raw[4..0x38]);
            Ok(Channel13EventRecord::Fault(Channel13FaultRecord {
                payload,
            }))
        }
        1 => Ok(Channel13EventRecord::Flag(Channel13FlagRecord {
            firing: [get_u64(raw, 4), get_u64(raw, 12)],
            value_14: get_u16(raw, 0x14),
        })),
        4 => Ok(Channel13EventRecord::Timeout(Channel13TimeoutRecord {
            counter: get_u64(raw, 4),
            stamp_index: get_i32(raw, 12),
        })),
        7 => Ok(Channel13EventRecord::GrowTilingBuffer(
            Channel13GrowthRecord {
                vm_id: get_u32(raw, 4),
                buffer_manager_id: get_u32(raw, 8),
                counter: get_u32(raw, 12),
            },
        )),
        8 => Ok(Channel13EventRecord::ChannelError),
        _ => Err(CompletionDecodeError::EventType),
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum Channel14TraceRecord {
    Marker11At00 {
        tick: u64,
    },
    Marker19At08 {
        tick: u64,
        state_28: u32,
        value_34: u32,
    },
    Marker1cAt10 {
        tick: u64,
        state_1c: u32,
    },
    Marker19At18 {
        tick: u64,
        prior_tick: u64,
        state_2c: u32,
        state_38: u32,
    },
    Marker1cAt20 {
        value_04: u64,
        tick: u64,
        state_2c: u32,
    },
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum FirmwareReportRecord {
    Event(Channel13EventRecord),
    Trace(Channel14TraceRecord),
}

pub(crate) fn decode_firmware_report(
    channel: u8,
    raw: &[u8],
) -> Result<FirmwareReportRecord, CompletionDecodeError> {
    match channel {
        REPORT_CHANNEL_EVENT => Ok(FirmwareReportRecord::Event(decode_channel13_event(
            channel, raw,
        )?)),
        REPORT_CHANNEL_TRACE => Ok(FirmwareReportRecord::Trace(decode_channel14_trace(
            channel, raw,
        )?)),
        _ => Err(CompletionDecodeError::ChannelNumber),
    }
}

pub(crate) fn decode_channel14_trace(
    channel: u8,
    raw: &[u8],
) -> Result<Channel14TraceRecord, CompletionDecodeError> {
    if channel != REPORT_CHANNEL_TRACE {
        return Err(CompletionDecodeError::ChannelNumber);
    }
    if raw.len() != REPORT_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }

    if get_u32(raw, 0x00) == 0x11 {
        return Ok(Channel14TraceRecord::Marker11At00 {
            tick: get_u64(raw, 0x04),
        });
    }
    if get_u32(raw, 0x08) == 0x19 {
        return Ok(Channel14TraceRecord::Marker19At08 {
            tick: get_u64(raw, 0x0c),
            state_28: get_u32(raw, 0x28),
            value_34: get_u32(raw, 0x34),
        });
    }
    if get_u32(raw, 0x10) == 0x1c {
        return Ok(Channel14TraceRecord::Marker1cAt10 {
            tick: get_u64(raw, 0x14),
            state_1c: get_u32(raw, 0x1c),
        });
    }
    if get_u32(raw, 0x18) == 0x19 {
        return Ok(Channel14TraceRecord::Marker19At18 {
            tick: get_u64(raw, 0x1c),
            prior_tick: get_u64(raw, 0x24),
            state_2c: get_u32(raw, 0x2c),
            state_38: get_u32(raw, 0x38),
        });
    }
    if get_u32(raw, 0x20) == 0x1c {
        return Ok(Channel14TraceRecord::Marker1cAt20 {
            value_04: get_u64(raw, 0x04),
            tick: get_u64(raw, 0x24),
            state_2c: get_u32(raw, 0x2c),
        });
    }
    Err(CompletionDecodeError::TraceShape)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum KsmCompletionCommandType {
    Type1,
    Type2,
    Type3,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17pKsmCompletionEntry {
    pub(crate) completion_selector: u8,
    pub(crate) command_type: KsmCompletionCommandType,
    pub(crate) queue_id: u8,
    pub(crate) timestamp: u64,
    pub(crate) word_08: u64,
    pub(crate) payload: [u64; 2],
    pub(crate) parents: [u64; 2],
    pub(crate) tagged_object: u64,
    pub(crate) object: u64,
    pub(crate) metadata: u64,
}

pub(crate) fn decode_ksm_completion_entry(
    completion_selector: u8,
    raw: &[u8],
) -> Result<G17pKsmCompletionEntry, CompletionDecodeError> {
    if completion_selector > 4 {
        return Err(CompletionDecodeError::CompletionSelector);
    }
    if raw.len() != KSM_COMPLETION_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }

    let control = get_u64(raw, 0x00);
    let command_type = match (control >> 4) & 0x3 {
        1 => KsmCompletionCommandType::Type1,
        2 => KsmCompletionCommandType::Type2,
        3 => KsmCompletionCommandType::Type3,
        _ => return Err(CompletionDecodeError::CompletionCommandType),
    };
    let timestamp = ((control >> 8) & 0xffff_ffff_00) | ((control >> 48) & 0xff);
    let tagged_object = get_u64(raw, 0x30);
    Ok(G17pKsmCompletionEntry {
        completion_selector,
        command_type,
        queue_id: ((control >> 9) as u8) & KSM_QID_MASK,
        timestamp: timestamp & KSM_TIMESTAMP_MASK,
        word_08: get_u64(raw, 0x08),
        payload: [get_u64(raw, 0x10), get_u64(raw, 0x18)],
        parents: [
            get_u64(raw, 0x20) & KSM_PARENT_MASK,
            get_u64(raw, 0x28) & KSM_PARENT_MASK,
        ],
        tagged_object,
        object: tagged_object & !1,
        metadata: get_u64(raw, 0x38),
    })
}

pub(crate) fn decode_compute_completion_entry(
    raw: &[u8],
) -> Result<G17pKsmCompletionEntry, CompletionDecodeError> {
    decode_ksm_completion_entry(G17P_COMPUTE_COMPLETION_SELECTOR, raw)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct KsmCompletionCursor {
    pub(crate) capacity: u32,
    pub(crate) processed: u32,
}

impl KsmCompletionCursor {
    pub(crate) fn entry_offset(self) -> Result<u64, CompletionDecodeError> {
        if self.capacity == 0 {
            return Err(CompletionDecodeError::CompletionCapacity);
        }
        if self.processed >= self.capacity {
            return Err(CompletionDecodeError::CompletionCursor);
        }
        Ok(self.processed as u64 * KSM_COMPLETION_ENTRY_SIZE as u64)
    }

    pub(crate) fn advance(self) -> Result<Self, CompletionDecodeError> {
        self.entry_offset()?;
        Ok(Self {
            capacity: self.capacity,
            processed: (self.processed + 1) % self.capacity,
        })
    }
}

/// Owner of one normal-CL completion transport operation.
///
/// Source bit 21 and the `+0x21168/+0x21188` notification pair are consumed
/// by GFX firmware. They are not a Linux interrupt-status/acknowledgement
/// pair. Linux first participates when the firmware publishes an ordinary
/// event-ring record and reports it through AIC source 4 or the equivalent
/// RTKit notification.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PCompletionOwner {
    GfxFirmware,
    LinuxPlatform,
}

/// Exact firmware-owned descriptor/MMIO drain for normal CL.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareCompletionDrain {
    pub(crate) source_bit: u8,
    pub(crate) descriptor_table_offset: u32,
    pub(crate) descriptor_stride: u32,
    pub(crate) descriptor_ordinal: u8,
    pub(crate) processed_offset: u32,
    pub(crate) notification_offset: u32,
    pub(crate) acknowledgement_offset: u32,
    pub(crate) owner: G17PCompletionOwner,
}

pub(crate) const G17P_FIRMWARE_COMPLETION_DRAIN: G17PFirmwareCompletionDrain =
    G17PFirmwareCompletionDrain {
        source_bit: G17P_NORMAL_CL_COMPLETION_IRQ_BIT,
        descriptor_table_offset: G17P_NORMAL_CL_COMPLETION_DESCRIPTOR_TABLE_OFFSET,
        descriptor_stride: G17P_NORMAL_CL_COMPLETION_DESCRIPTOR_STRIDE,
        descriptor_ordinal: G17P_NORMAL_CL_COMPLETION_ORDINAL,
        processed_offset: G17P_NORMAL_CL_COMPLETION_PROCESSED_OFFSET,
        notification_offset: G17P_KSM_COMPLETION_NOTIFY_BASE,
        acknowledgement_offset: G17P_KSM_COMPLETION_ACK_BASE,
        owner: G17PCompletionOwner::GfxFirmware,
    };

/// Exact D93AP host ingress facts and the current Linux ownership state.
///
/// The pinned ADT selects raw SGX source 4 (valid in mask `0xdf`) and AIC
/// hardware interrupt 1001. The checked-in T8140 Linux GPU node exposes no
/// SGX interrupt resource, so 1001 must not be passed directly to Linux's IRQ
/// registration API: that API expects a DT-translated Linux virtual IRQ. The
/// retained RTKit client does receive endpoint 0x20 and can demultiplex the
/// exact type-2 notification, but no role-local event-ring pointer graph is
/// mapped yet.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PNormalClCompletionRoute {
    pub(crate) physical_aic_irq: u32,
    pub(crate) sgx_source_index: u8,
    pub(crate) sgx_interrupts_valid: u32,
    pub(crate) firmware_event_endpoint: u8,
    pub(crate) firmware_event_message: u64,
    pub(crate) firmware_completion_owner: G17PCompletionOwner,
    pub(crate) linux_gpu_dt_irq_resource: bool,
    pub(crate) physical_irq_registered: bool,
    pub(crate) rtkit_notification_demultiplexed: bool,
    pub(crate) firmware_event_ring_source_mapped: bool,
    pub(crate) firmware_event_shared_indices_mapped: bool,
    pub(crate) firmware_event_entries_mapped: bool,
    pub(crate) firmware_event_consumer_writer_available: bool,
}

pub(crate) const G17P_NORMAL_CL_COMPLETION_ROUTE: G17PNormalClCompletionRoute =
    G17PNormalClCompletionRoute {
        physical_aic_irq: D93AP_G17P_COMPLETION_IRQ,
        sgx_source_index: D93AP_G17P_SGX_SOURCE_INDEX,
        sgx_interrupts_valid: D93AP_G17P_SGX_INTERRUPTS_VALID,
        firmware_event_endpoint: G17P_FIRMWARE_EVENT_ENDPOINT,
        firmware_event_message: G17P_FIRMWARE_EVENT_MESSAGE,
        firmware_completion_owner: G17PCompletionOwner::GfxFirmware,
        linux_gpu_dt_irq_resource: false,
        physical_irq_registered: false,
        rtkit_notification_demultiplexed: true,
        firmware_event_ring_source_mapped: false,
        firmware_event_shared_indices_mapped: false,
        firmware_event_entries_mapped: false,
        firmware_event_consumer_writer_available: false,
    };

pub(crate) const fn require_g17p_normal_cl_completion_route(
    route: G17PNormalClCompletionRoute,
) -> Result<(), CompletionDecodeError> {
    let physical_ingress = route.linux_gpu_dt_irq_resource && route.physical_irq_registered;
    if route.physical_aic_irq == D93AP_G17P_COMPLETION_IRQ
        && route.sgx_source_index == D93AP_G17P_SGX_SOURCE_INDEX
        && route.sgx_interrupts_valid == D93AP_G17P_SGX_INTERRUPTS_VALID
        && route.firmware_event_endpoint == G17P_FIRMWARE_EVENT_ENDPOINT
        && route.firmware_event_message == G17P_FIRMWARE_EVENT_MESSAGE
        && matches!(
            route.firmware_completion_owner,
            G17PCompletionOwner::GfxFirmware
        )
        && (physical_ingress || route.rtkit_notification_demultiplexed)
        && route.firmware_event_ring_source_mapped
        && route.firmware_event_shared_indices_mapped
        && route.firmware_event_entries_mapped
        && route.firmware_event_consumer_writer_available
    {
        Ok(())
    } else {
        Err(CompletionDecodeError::CompletionRouteUnavailable)
    }
}

/// Recognize the exact firmware-event notification delivered to the retained
/// RTKit endpoint. Bit 6 is a transport flag; the host-visible message type is
/// the low six bits (`0x42 & 0x3f == 2`).
pub(crate) const fn decode_g17p_firmware_event_notification(
    endpoint: u8,
    message: u64,
) -> Result<(), CompletionDecodeError> {
    if endpoint != G17P_FIRMWARE_EVENT_ENDPOINT {
        return Err(CompletionDecodeError::FirmwareEventEndpoint);
    }
    if message != G17P_FIRMWARE_EVENT_MESSAGE
        || ((message >> 48) as u8 & G17P_FIRMWARE_EVENT_HOST_TYPE_MASK)
            != G17P_FIRMWARE_EVENT_HOST_TYPE
    {
        return Err(CompletionDecodeError::FirmwareEventMessage);
    }
    Ok(())
}

/// Shared indices for one ordinary firmware-event ring.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareEventCursor {
    pub(crate) consumer: u32,
    pub(crate) producer: u32,
}

/// Pure read/publish plan for one host-consumed event record.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareEventRead {
    pub(crate) entry_offset: u32,
    pub(crate) published_consumer: u32,
    pub(crate) has_more_from_snapshot: bool,
}

/// Required host ordering for one role-local firmware-event record.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G17PFirmwareEventReadStep {
    SnapshotIndices,
    ValidateIndices,
    CopyEntry,
    DecodeType,
    AdvanceConsumer,
    DmbIsh,
    PublishConsumer,
    CorrelateStamp,
}

pub(crate) const G17P_FIRMWARE_EVENT_READ_ORDER: [G17PFirmwareEventReadStep; 8] = [
    G17PFirmwareEventReadStep::SnapshotIndices,
    G17PFirmwareEventReadStep::ValidateIndices,
    G17PFirmwareEventReadStep::CopyEntry,
    G17PFirmwareEventReadStep::DecodeType,
    G17PFirmwareEventReadStep::AdvanceConsumer,
    G17PFirmwareEventReadStep::DmbIsh,
    G17PFirmwareEventReadStep::PublishConsumer,
    G17PFirmwareEventReadStep::CorrelateStamp,
];

pub(crate) const fn prepare_g17p_firmware_event_read(
    cursor: G17PFirmwareEventCursor,
) -> Result<G17PFirmwareEventRead, CompletionDecodeError> {
    if cursor.consumer >= G17P_FIRMWARE_EVENT_CAPACITY
        || cursor.producer >= G17P_FIRMWARE_EVENT_CAPACITY
    {
        return Err(CompletionDecodeError::FirmwareEventCursor);
    }
    if cursor.consumer == cursor.producer {
        return Err(CompletionDecodeError::FirmwareEventEmpty);
    }
    let published_consumer = (cursor.consumer + 1) & (G17P_FIRMWARE_EVENT_CAPACITY - 1);
    Ok(G17PFirmwareEventRead {
        entry_offset: cursor.consumer * G17P_FIRMWARE_EVENT_ENTRY_SIZE as u32,
        published_consumer,
        has_more_from_snapshot: published_consumer != cursor.producer,
    })
}

/// One decoded notification for a present completion descriptor.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PKsmCompletionNotification {
    pub(crate) ordinal: u8,
    pub(crate) notification_offset: u32,
    pub(crate) acknowledgement_offset: u32,
    pub(crate) entry_count: u32,
}

pub(crate) const fn decode_g17p_normal_cl_notification(
    irq_sources: u64,
    ordinal: u8,
    notification_word: u32,
) -> Result<G17PKsmCompletionNotification, CompletionDecodeError> {
    if irq_sources & (1u64 << G17P_NORMAL_CL_COMPLETION_IRQ_BIT) == 0 {
        return Err(CompletionDecodeError::CompletionIrqSource);
    }
    if ordinal != G17P_NORMAL_CL_COMPLETION_ORDINAL {
        return Err(CompletionDecodeError::CompletionOrdinal);
    }
    let entry_count = if notification_word < 0x1000 {
        0
    } else {
        notification_word >> 16
    };
    Ok(G17PKsmCompletionNotification {
        ordinal,
        notification_offset: G17P_KSM_COMPLETION_NOTIFY_BASE
            + ordinal as u32 * G17P_KSM_COMPLETION_MMIO_STRIDE,
        acknowledgement_offset: G17P_KSM_COMPLETION_ACK_BASE
            + ordinal as u32 * G17P_KSM_COMPLETION_MMIO_STRIDE,
        entry_count,
    })
}

/// Firmware event type 1, delivered through the ordinary EP0x20 event ring.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareStampEvent {
    pub(crate) masks: [u64; 2],
    pub(crate) diagnostic_pair_count: u16,
}

impl G17PFirmwareStampEvent {
    pub(crate) const fn signals(self, stamp_index: u8) -> bool {
        if stamp_index >= 128 {
            return false;
        }
        let word = stamp_index as usize / 64;
        let bit = stamp_index as usize % 64;
        self.masks[word] & (1u64 << bit) != 0
    }
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

/// Firmware event type 4 (`kAGFIFirmwareEventTypeGPURestart`).
///
/// Exact J700 A000 publishes the current recovery generation unaligned at
/// `+0x04` and the affected stamp index at `+0x0c`. A stamp index of `-1` is
/// valid and denotes a recovery with no command stamp to attribute.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareRecoveryEvent {
    pub(crate) generation: u64,
    pub(crate) stamp_index: i32,
}

pub(crate) fn decode_g17p_firmware_recovery_event(
    raw: &[u8],
) -> Result<G17PFirmwareRecoveryEvent, CompletionDecodeError> {
    if raw.len() != G17P_FIRMWARE_EVENT_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }
    if get_u32(raw, 0x00) != G17P_FIRMWARE_EVENT_RECOVERY_TYPE {
        return Err(CompletionDecodeError::FirmwareEventType);
    }
    Ok(G17PFirmwareRecoveryEvent {
        generation: get_u64(raw, 0x04),
        stamp_index: get_i32(raw, 0x0c),
    })
}

/// Firmware event type 13
/// (`kAGFIFirmwareEventTypeUMAAsyncGrowRequestComplete`).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PFirmwareUmaGrowEvent {
    pub(crate) stamp_index: i32,
    pub(crate) pool_index: u32,
    pub(crate) shared_control: u64,
    pub(crate) operand_table: u64,
    pub(crate) cookie: u64,
    pub(crate) result: u32,
}

pub(crate) fn decode_g17p_firmware_uma_grow_event(
    raw: &[u8],
) -> Result<G17PFirmwareUmaGrowEvent, CompletionDecodeError> {
    if raw.len() != G17P_FIRMWARE_EVENT_ENTRY_SIZE {
        return Err(CompletionDecodeError::RecordSize);
    }
    if get_u32(raw, 0x00) != G17P_FIRMWARE_EVENT_UMA_GROW_TYPE {
        return Err(CompletionDecodeError::FirmwareEventType);
    }
    let stamp_index = get_i32(raw, 0x04);
    if stamp_index < -1 {
        return Err(CompletionDecodeError::FirmwareEventStampIndex);
    }
    let pool_index = get_u32(raw, 0x08);
    if pool_index >= 0x100 {
        return Err(CompletionDecodeError::FirmwareEventUmaPoolIndex);
    }
    let shared_control = get_u64(raw, 0x0c);
    let operand_table = get_u64(raw, 0x14);
    if shared_control == 0 || operand_table == 0 {
        return Err(CompletionDecodeError::FirmwareEventUmaAddress);
    }
    Ok(G17PFirmwareUmaGrowEvent {
        stamp_index,
        pool_index,
        shared_control,
        operand_table,
        cookie: get_u64(raw, 0x1c),
        result: get_u32(raw, 0x24),
    })
}

/// Firmware's per-QID timestamp ordering state.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PCompletionQidState {
    pub(crate) valid: bool,
    pub(crate) last_completed_timestamp: u64,
    pub(crate) dependency_timestamp: u64,
}

pub(crate) const fn advance_g17p_completion_qid_state(
    state: G17PCompletionQidState,
    timestamp: u64,
) -> Result<G17PCompletionQidState, CompletionDecodeError> {
    if timestamp & !KSM_TIMESTAMP_MASK != 0 {
        return Err(CompletionDecodeError::CompletionTimestamp);
    }
    if state.valid
        && timestamp.wrapping_sub(state.last_completed_timestamp) & KSM_TIMESTAMP_MASK != 1
    {
        return Err(CompletionDecodeError::CompletionTimestampOrder);
    }
    Ok(G17PCompletionQidState {
        valid: true,
        last_completed_timestamp: timestamp,
        dependency_timestamp: state.dependency_timestamp,
    })
}

/// Host-owned state required to validate one first normal-CL completion.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G17PNormalClCompletionState {
    pub(crate) cursor: KsmCompletionCursor,
    pub(crate) qid_state: G17PCompletionQidState,
    pub(crate) queue_id: u8,
    pub(crate) expected_timestamp: u64,
    pub(crate) expected_stamp_index: u8,
    pub(crate) outstanding: u32,
    pub(crate) descriptor_generation: u32,
}

/// A fully correlated transaction. Only this type carries an outstanding
/// decrement to the manager boundary.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PreparedG17PNormalClCompletion {
    pub(crate) entry: G17pKsmCompletionEntry,
    pub(crate) notification: G17PKsmCompletionNotification,
    pub(crate) firmware_event: G17PFirmwareStampEvent,
    pub(crate) cursor_after: KsmCompletionCursor,
    pub(crate) qid_state_after: G17PCompletionQidState,
    pub(crate) outstanding_after: u32,
    pub(crate) completed_generation: u32,
}

pub(crate) fn prepare_g17p_first_normal_cl_completion(
    state: G17PNormalClCompletionState,
    irq_sources: u64,
    notification_word: u32,
    completion_entry: &[u8],
    firmware_event: &[u8],
) -> Result<PreparedG17PNormalClCompletion, CompletionDecodeError> {
    let notification = decode_g17p_normal_cl_notification(
        irq_sources,
        G17P_NORMAL_CL_COMPLETION_ORDINAL,
        notification_word,
    )?;
    if notification.entry_count != 1 {
        return Err(CompletionDecodeError::CompletionNotificationCount);
    }
    if state.cursor.capacity != G17P_NORMAL_CL_COMPLETION_CAPACITY {
        return Err(CompletionDecodeError::CompletionCapacity);
    }
    state.cursor.entry_offset()?;
    let entry = decode_compute_completion_entry(completion_entry)?;
    if entry.queue_id != state.queue_id {
        return Err(CompletionDecodeError::CompletionQueueId);
    }
    if entry.timestamp != state.expected_timestamp {
        return Err(CompletionDecodeError::CompletionTimestamp);
    }
    let qid_state_after = advance_g17p_completion_qid_state(state.qid_state, entry.timestamp)?;
    if state.expected_stamp_index >= 128 {
        return Err(CompletionDecodeError::FirmwareEventStampIndex);
    }
    let firmware_event = decode_g17p_firmware_stamp_event(firmware_event)?;
    if !firmware_event.signals(state.expected_stamp_index) {
        return Err(CompletionDecodeError::FirmwareEventStampMissing);
    }
    if state.descriptor_generation == 0 || state.descriptor_generation > state.outstanding {
        return Err(CompletionDecodeError::CompletionOutstanding);
    }
    Ok(PreparedG17PNormalClCompletion {
        entry,
        notification,
        firmware_event,
        cursor_after: state.cursor.advance()?,
        qid_state_after,
        outstanding_after: state.outstanding - state.descriptor_generation,
        completed_generation: state.descriptor_generation,
    })
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct QueueIndices {
    pub(crate) done: u32,
    pub(crate) read: u32,
    pub(crate) write: u32,
}

impl QueueIndices {
    fn validate(self) -> Result<(), CompletionDecodeError> {
        if self.done > self.write || self.read > self.write {
            return Err(CompletionDecodeError::QueueCounterOrder);
        }
        Ok(())
    }

    pub(crate) const fn idle(self) -> bool {
        self.done == self.read && self.read == self.write
    }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct QueuePrefixState {
    pub(crate) indices: QueueIndices,
    pub(crate) published_prefix: u32,
    pub(crate) accepted: bool,
    pub(crate) complete: bool,
    pub(crate) idle: bool,
}

pub(crate) fn observe_queue_prefix(
    previous: QueueIndices,
    current: QueueIndices,
    published_prefix: u32,
) -> Result<QueuePrefixState, CompletionDecodeError> {
    previous.validate()?;
    current.validate()?;
    if current.done < previous.done
        || current.read < previous.read
        || current.write < previous.write
    {
        return Err(CompletionDecodeError::QueueCounterRegression);
    }
    if published_prefix > current.write {
        return Err(CompletionDecodeError::PublishedPrefixBeyondWrite);
    }
    Ok(QueuePrefixState {
        indices: current,
        published_prefix,
        accepted: current.read >= published_prefix,
        complete: current.done >= published_prefix,
        idle: current.idle(),
    })
}

pub(crate) fn observe_compute_completion(
    previous: QueueIndices,
    current: QueueIndices,
    published_prefix: u32,
) -> Result<QueuePrefixState, CompletionDecodeError> {
    observe_queue_prefix(previous, current, published_prefix)
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct ComputeCompletionReport {
    pub(crate) entry: G17pKsmCompletionEntry,
    pub(crate) queue: QueuePrefixState,
}

pub(crate) fn observe_compute_completion_report(
    raw: &[u8],
    previous: QueueIndices,
    current: QueueIndices,
    published_prefix: u32,
) -> Result<ComputeCompletionReport, CompletionDecodeError> {
    Ok(ComputeCompletionReport {
        entry: decode_compute_completion_entry(raw)?,
        queue: observe_compute_completion(previous, current, published_prefix)?,
    })
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct PairedRenderCompletion {
    pub(crate) tiling: QueuePrefixState,
    pub(crate) fragment: QueuePrefixState,
    pub(crate) execution: RenderExecutionState,
    pub(crate) accepted: bool,
    pub(crate) retired: bool,
    pub(crate) complete: bool,
    pub(crate) idle: bool,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RenderExecutionState {
    pub(crate) job_list_empty: bool,
    pub(crate) tiling_timestamps: [u64; 2],
    pub(crate) fragment_timestamps: [u64; 2],
}

impl RenderExecutionState {
    pub(crate) fn observed(self) -> bool {
        self.job_list_empty
            && self.tiling_timestamps.iter().all(|value| *value != 0)
            && self.fragment_timestamps.iter().all(|value| *value != 0)
    }
}

pub(crate) const fn render_gpu_interval(
    execution: RenderExecutionState,
) -> Option<[u64; 2]> {
    let start = execution.tiling_timestamps[0];
    let end = execution.fragment_timestamps[1];
    if execution.job_list_empty
        && start != 0
        && execution.tiling_timestamps[1] == start
        && execution.fragment_timestamps[0] == start
        && start < end
    {
        Some([start, end])
    } else {
        None
    }
}

pub(crate) fn observe_paired_render_completion(
    tiling_previous: QueueIndices,
    tiling_current: QueueIndices,
    tiling_prefix: u32,
    fragment_previous: QueueIndices,
    fragment_current: QueueIndices,
    fragment_prefix: u32,
    execution: RenderExecutionState,
) -> Result<PairedRenderCompletion, CompletionDecodeError> {
    let tiling = observe_queue_prefix(tiling_previous, tiling_current, tiling_prefix)?;
    let fragment = observe_queue_prefix(fragment_previous, fragment_current, fragment_prefix)?;
    let retired = tiling.complete && fragment.complete;
    let complete = retired && execution.observed();
    Ok(PairedRenderCompletion {
        accepted: tiling.accepted && fragment.accepted,
        retired,
        complete,
        idle: tiling.idle && fragment.idle && execution.job_list_empty,
        tiling,
        fragment,
        execution,
    })
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct RenderQueueWindow {
    base: u32,
    count: u32,
    capacity: u32,
}

impl RenderQueueWindow {
    pub(crate) fn new(base: u32, count: u32, capacity: u32)
        -> Result<Self, CompletionDecodeError>
    {
        if base >= capacity || !matches!(count, 3 | 4) || count >= capacity {
            return Err(CompletionDecodeError::RenderWindow);
        }
        Ok(Self { base, count, capacity })
    }

    pub(crate) fn target(self) -> u32 {
        ((u64::from(self.base) + u64::from(self.count)) % u64::from(self.capacity)) as u32
    }

    fn normalize(self, indices: QueueIndices) -> Result<QueueIndices, CompletionDecodeError> {
        let distance = |value: u32| {
            if value >= self.capacity {
                return Err(CompletionDecodeError::RenderCursorOutsideWindow);
            }
            let distance = ((u64::from(value) + u64::from(self.capacity)
                - u64::from(self.base)) % u64::from(self.capacity)) as u32;
            if distance > self.count {
                return Err(CompletionDecodeError::RenderCursorOutsideWindow);
            }
            Ok(distance)
        };
        Ok(QueueIndices {
            done: distance(indices.done)?,
            read: distance(indices.read)?,
            write: distance(indices.write)?,
        })
    }
}

pub(crate) fn observe_paired_render_window(
    window: RenderQueueWindow,
    tiling_previous: QueueIndices,
    tiling_current: QueueIndices,
    fragment_previous: QueueIndices,
    fragment_current: QueueIndices,
    execution: RenderExecutionState,
) -> Result<PairedRenderCompletion, CompletionDecodeError> {
    let mut result = observe_paired_render_completion(
        window.normalize(tiling_previous)?, window.normalize(tiling_current)?, window.count,
        window.normalize(fragment_previous)?, window.normalize(fragment_current)?, window.count,
        execution,
    )?;
    result.tiling.indices = tiling_current;
    result.fragment.indices = fragment_current;
    result.tiling.published_prefix = window.target();
    result.fragment.published_prefix = window.target();
    Ok(result)
}

