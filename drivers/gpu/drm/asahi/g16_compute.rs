// SPDX-License-Identifier: GPL-2.0-only OR MIT


#[derive(Clone, Copy)]
pub(crate) struct Addresses {
    pub(crate) command: u64,
    pub(crate) command_gpu: u64,
    pub(crate) cdm_gpu: u64,
    pub(crate) preemption_gpu: u64,
    pub(crate) metrics: u64,
    pub(crate) auxiliary_gpu: u64,
    pub(crate) work_queue: u64,
    pub(crate) stats: u64,
    pub(crate) context_store_gpu: u64,
    pub(crate) pool_state_gpu: u64,
    pub(crate) page_list_gpu: u64,
    pub(crate) counter: u64,
    pub(crate) context: u32,
    pub(crate) generation: u8,
    pub(crate) stamp: u32,
    pub(crate) event_slot: u8,
}


pub(crate) mod layout {
    pub(crate) const KIND: usize = 0;
    pub(crate) const CONTEXT: usize = 0xc;
    pub(crate) const NOTIFIER_POINTER: usize = 0x10;
    pub(crate) const REGISTER_STRIDE: usize = 12;
    pub(crate) const REGISTERS: usize = 0x20;
    pub(crate) const REGISTER_VALUE: usize = 0x24;
    pub(crate) const REGISTER_POINTER: usize = 0x720;
    pub(crate) const REGISTER_COUNT: usize = 0x728;
    pub(crate) const SEQUENCE_POINTER: usize = 0x760;
    pub(crate) const SEQUENCE_LENGTH: usize = 0x768;
    pub(crate) const ENGINE_STATE: usize = 0x76c;
    pub(crate) const PREEMPTION: usize = 0x798;
    pub(crate) const CDM_LAST: usize = 0x7a0;
    pub(crate) const CONTROL: usize = 0x7c8;
    pub(crate) const CONTEXT_TIME: usize = 0x7dc;
    pub(crate) const UNKNOWN_7E8: usize = 0x7e8;
    pub(crate) const USER_STAMP_POINTER: usize = 0x800;
    pub(crate) const FIRMWARE_STAMP_POINTER: usize = 0x808;
    pub(crate) const STAMP_VALUE: usize = 0x810;
    pub(crate) const EVENT: usize = 0x814;
    pub(crate) const EVENT_SLOT: usize = 0x818;
    pub(crate) const FLUSH_STAMPS: usize = 0x81c;
    pub(crate) const UUID: usize = 0x820;
    pub(crate) const EVENT_SEQUENCE: usize = 0x824;
    pub(crate) const TIME_CONTROL: usize = 0x82c;
    pub(crate) const TIME_PAIR: usize = 0x834;
    pub(crate) const TIME_PAIR_END: usize = 0x83c;
    pub(crate) const USER_TIME_PAIR: usize = 0x844;
    pub(crate) const USER_TIME_PAIR_END: usize = 0x84c;
    pub(crate) const UMA_POINTER: usize = 0x86a;
    pub(crate) const UMA_FLAG: usize = 0x872;
    pub(crate) const UNKNOWN_873: usize = 0x873;
    pub(crate) const UNKNOWN_87B: usize = 0x87b;
    pub(crate) const METRICS: usize = 0x883;
    pub(crate) const ASID_GENERATION: usize = 0x88b;
    pub(crate) const UNKNOWN_88C: usize = 0x88c;
    pub(crate) const TIME_AUXILIARY: usize = 0x89c;
    pub(crate) const STATUS: usize = 0x8ac;
    pub(crate) const TIME_START_CELL: usize = 0x8c0;
    pub(crate) const TIME_END_CELL: usize = 0x8c8;
    pub(crate) const SEQUENCE: usize = 0x900;
    pub(crate) const START_CONTEXT: usize = 0x92c;
    pub(crate) const START_CONTEXT_CONTROL: usize = 0x930;
    pub(crate) const START_EVENT_GENERATION: usize = 0x934;
    pub(crate) const START_EVENT_SEQUENCE: usize = 0x938;
    pub(crate) const START_UNKNOWN_950: usize = 0x950;
    pub(crate) const ATTACHMENTS: usize = 0x958;
    pub(crate) const START_AUXILIARY: usize = 0xa78;
    pub(crate) const START_CONTEXT_STORE: usize = 0xa88;
    pub(crate) const START_EVENT: usize = 0xab8;
    pub(crate) const TIMESTAMP_START: usize = 0xabc;
    pub(crate) const WAIT_IDLE: usize = 0xb08;
    pub(crate) const TIMESTAMP_END: usize = 0xb0c;
    pub(crate) const FINALIZE: usize = 0xb58;
    pub(crate) const FINALIZE_CONTEXT: usize = 0xb6c;
    pub(crate) const FINALIZE_STAMP: usize = 0xb8c;
    pub(crate) const FINALIZE_RESTART: usize = 0xbbc;
    pub(crate) const FINALIZE_HAS_ATTACHMENTS: usize = 0xbc0;
    pub(crate) const FINISH: usize = 0xbd4;
    pub(crate) const NOTIFIER: usize = 0xc00;
    pub(crate) const UMA: usize = 0xe00;
    pub(crate) const UMA_END: usize = 0xea0;
    pub(crate) const FIRMWARE_STAMP: usize = 0xee0;
    pub(crate) const USER_STAMP: usize = 0xee4;
    pub(crate) const THRESHOLD: usize = 0xf00;
    pub(crate) const DEPENDENCY: usize = 0xf40;
    pub(crate) const FANOUT_SEQUENCE: usize = 0x1000;
    pub(crate) const FANOUT_PAIRS: usize = 0x1800;
    pub(crate) const ALLOCATION_SIZE: usize = 0x2000;
    pub(crate) const SEQUENCE_SIZE: usize = 0x300;
    pub(crate) const FINALIZE_DELTA: usize = FINALIZE - SEQUENCE;
    pub(crate) const FINALIZE_RESTART_DELTA: usize = FINALIZE_RESTART - FINALIZE;
}
mod start {
    pub(crate) const REGISTERS: usize = 0x14;
    pub(crate) const STATS: usize = 0x1c;
    pub(crate) const QUEUE: usize = 0x24;
    pub(crate) const ENGINE_STATE: usize = 0x44;
    pub(crate) const UMA: usize = 0x160;
    pub(crate) const STATUS: usize = 0x1a8;
}
mod finalize {
    pub(crate) const STATS: usize = 4;
    pub(crate) const QUEUE: usize = 12;
    pub(crate) const ENGINE_STATE: usize = 24;
    pub(crate) const UNKNOWN_24: usize = 36;
    pub(crate) const STAMP: usize = 44;
    pub(crate) const UMA_POINTER: usize = 0x5c;
    pub(crate) const STATUS: usize = 0x69;
    pub(crate) const UNKNOWN_71: usize = 0x71;
}

pub(crate) mod timestamp {
    pub(crate) const SIZE: usize = 0x4c;
    pub(crate) const END_DELTA: usize = SIZE + 4; // intervening WaitForIdle opcode
    pub(crate) const CONTROL: usize = 0x04;
    pub(crate) const INTERNAL_PAIR: usize = 0x0c;
    pub(crate) const SELECTED_CELL: usize = 0x14;
    pub(crate) const QUEUE: usize = 0x1c;
    pub(crate) const USER_PAIR: usize = 0x24;
    pub(crate) const AUXILIARY: usize = 0x2c;
    pub(crate) const CONTEXT_TIME: usize = 0x34;
    pub(crate) const UUID: usize = 0x44;
}

pub(crate) const SIZE: usize = 0x1000;
pub(crate) const SLOT: u8 = 15;
pub(crate) const STAMP: u32 = 0x100;

pub(crate) const EVENT_GENERATION: u32 = 0;

pub(crate) mod notification_layout {
    pub(crate) const SIZE: usize = 0x200;
    pub(crate) const THRESHOLD: usize = 0;
    pub(crate) const GENERATION: usize = 8;
    pub(crate) const UNKNOWN_10: usize = 0x10;
    pub(crate) const CONTEXT: usize = 0x24;
    pub(crate) const SLOT_SENTINELS: usize = 0xa8;
    pub(crate) const SLOT_SENTINELS_SIZE: usize = 8;
    pub(crate) const UNKNOWN_114: usize = 0x114;
    pub(crate) const QUALIFIED_UNKNOWN_10: u32 = 0x50;
    pub(crate) const QUALIFIED_UNKNOWN_114: u32 = 1;
}


#[derive(Clone, Copy)]
pub(crate) struct Notification {
    pub(crate) address: u64,
    pub(crate) threshold: u64,
    pub(crate) stamp: u64,
    pub(crate) fw_stamp: u64,
    pub(crate) event_seq: u64,
}

#[derive(Clone, Copy)]
pub(crate) struct Control {
    pub(crate) base: u64,
    pub(crate) end: u64,
    pub(crate) usc_base: u64,
    pub(crate) wide_visibility: bool,
}

pub(crate) fn encode(out: &mut [u8], a: Addresses, pool_qwords: u32, pages: u32) -> Result<(), ()> {
    encode_dispatch(out, a, pool_qwords, pages,
        Control { base: a.cdm_gpu, end: a.cdm_gpu + 0x30, usc_base: 0, wide_visibility: false }, None)
}

pub(crate) fn encode_dispatch(out: &mut [u8], a: Addresses, pool_qwords: u32,
    pages: u32, control: Control, notification: Option<Notification>) -> Result<(), ()> {
    if control.base == 0 || control.base & 3 != 0 || control.end & 3 != 0
        || control.end <= control.base || control.end >= (1 << 42)
        || control.usc_base & 0xffff_ffff != 0 || control.usc_base >= (1 << 42)
        || out.len() < SIZE || a.command < 0xffff_fc00_0000_0000
        || a.command.checked_add(SIZE as u64).is_none()
        || a.context >= 64 || (a.context != 0 && a.generation == 0)
        || a.command & 0x3fff != 0 || a.work_queue & 7 != 0
        || a.stats & 7 != 0 || a.stats < 0xffff_fc00_0000_0000
        || a.counter & 7 != 0 || pool_qwords == 0
        || pages >= pool_qwords || pages % 512 != 0
        || [a.command_gpu, a.auxiliary_gpu, a.preemption_gpu, a.context_store_gpu, a.pool_state_gpu, a.page_list_gpu]
            .iter().any(|va| *va == 0 || *va & 0x3fff != 0 || *va >= (1 << 42) - 0x20000)
        || a.metrics < 0xffff_fc00_0000_0000 || a.metrics & 7 != 0
        || a.work_queue < 0xffff_fc00_0000_0000 || a.counter < 0xffff_fc00_0000_0000 {
        return Err(());
    }
    out[..SIZE].fill(0);
    let c = a.command;
    let g = a.command_gpu;
    let sku = c + layout::SEQUENCE as u64;
    let threshold = notification.map_or(c + layout::THRESHOLD as u64, |n| n.threshold);
    let uma = c + layout::UMA as u64;
    let u32_at = |b: &mut [u8], off, value: u32| b[off..off + 4].copy_from_slice(&value.to_le_bytes());
    let u64_at = |b: &mut [u8], off, value: u64| b[off..off + 8].copy_from_slice(&value.to_le_bytes());
    u32_at(out, layout::KIND, 3);
    u64_at(out, layout::NOTIFIER_POINTER, notification.map_or(c + layout::NOTIFIER as u64, |n| n.address));
    u32_at(out, layout::CONTEXT, a.context);
    out[layout::ASID_GENERATION] = a.generation;
    let registers: &[(u32, u64)] = &[
        (0x1a510, a.preemption_gpu), (0x1a420, control.base),
        (0x1a4d0, a.preemption_gpu + 0x1480), (0x1a4d8, a.preemption_gpu + 0x1488),
        (0x1a4e0, a.preemption_gpu + 0x1490), (0x1a4e8, a.preemption_gpu + 0x1498),
        (0x1a440, 0x154024201), (0x1a458, 0x10c08860),
        (0x101d9, 0x1c), (0x1a089, 0), (0x1a091, 0),
        (0x1a059, 0), (0x1a061, 0), (0x1a0b9, 0), (0x1a0c1, 0),
        (0x101d1, 0), (0xd479, 0), (0x1a0e9, 8),
        (0x107a1, 0xff0000), (0xa599, 0x13200400020),
        (0xd411, 0x200000001), (0x1a540, 1), (0x014a9, 1), (0x0a351, 1),
    ];
    for (i, (register, value)) in registers.iter().enumerate() {
        u32_at(out, layout::REGISTERS + i * layout::REGISTER_STRIDE, *register);
        u64_at(out, layout::REGISTER_VALUE + i * layout::REGISTER_STRIDE, *value);
    }
    let mut count = registers.len() as u32;
    if control.usc_base != 0 {
        u32_at(out, layout::REGISTERS + count as usize * layout::REGISTER_STRIDE, 0x10071);
        u64_at(out, layout::REGISTER_VALUE + count as usize * layout::REGISTER_STRIDE, control.usc_base);
        count += 1;
    }
    u64_at(out, layout::REGISTER_POINTER, g + layout::REGISTERS as u64);
    u32_at(out, layout::REGISTER_COUNT, count | ((count * layout::REGISTER_STRIDE as u32) << 16));
    u64_at(out, layout::SEQUENCE_POINTER, sku);
    u32_at(out, layout::SEQUENCE_LENGTH, layout::SEQUENCE_SIZE as u32);
    u64_at(out, layout::PREEMPTION, a.preemption_gpu);
    u64_at(out, layout::CDM_LAST, control.end - 4);
    u64_at(out, layout::CONTROL, 0x154024201);
    u32_at(out, layout::UNKNOWN_7E8, u32::MAX);
    u64_at(out, layout::USER_STAMP_POINTER, notification.map_or(c + layout::USER_STAMP as u64, |n| n.stamp));
    u64_at(out, layout::FIRMWARE_STAMP_POINTER, notification.map_or(c + layout::FIRMWARE_STAMP as u64, |n| n.fw_stamp));
    if a.event_slot >= 4 { return Err(()); }
    u32_at(out, layout::STAMP_VALUE, a.stamp);
    u32_at(out, layout::EVENT, SLOT as u32);
    u32_at(out, layout::EVENT_SLOT, a.event_slot as u32);
    u32_at(out, layout::FLUSH_STAMPS, 1);
    u32_at(out, layout::UUID, 1);
    u32_at(out, layout::EVENT_SEQUENCE, notification.map_or(0, |n| n.event_seq as u32));
    u64_at(out, layout::TIME_PAIR, c + layout::TIME_START_CELL as u64);
    u64_at(out, layout::TIME_PAIR_END, c + layout::TIME_END_CELL as u64);
    u64_at(out, layout::UMA_POINTER, uma);
    out[layout::UMA_FLAG] = 1;
    u64_at(out, layout::UNKNOWN_873, 0x1588000);
    u64_at(out, layout::UNKNOWN_87B, 0xef8000);
    u64_at(out, layout::METRICS, a.metrics);
    u32_at(out, layout::SEQUENCE, 0xb);
    for (offset, value) in [(start::REGISTERS, c + layout::REGISTERS as u64), (start::STATS, a.stats), (start::QUEUE, a.work_queue),
        (start::ENGINE_STATE, c + layout::ENGINE_STATE as u64), (start::UMA, uma), (start::STATUS, c + layout::STATUS as u64)] {
        u64_at(out, layout::SEQUENCE + offset, value);
    }
    u32_at(out, layout::START_CONTEXT, a.context);
    u32_at(out, layout::START_CONTEXT_CONTROL, 1);
    u32_at(out, layout::START_EVENT_GENERATION, EVENT_GENERATION);
    u64_at(out, layout::START_EVENT_SEQUENCE, notification.map_or(0, |n| n.event_seq));
    u32_at(out, layout::START_UNKNOWN_950, 1);
    u64_at(out, layout::START_AUXILIARY, a.auxiliary_gpu);
    for (i, delta) in [0, 0xf400, 0x1e800, 0x1f000].iter().enumerate() {
        u64_at(out, layout::START_CONTEXT_STORE + i * 8, a.context_store_gpu + delta);
    }
    u32_at(out, layout::START_EVENT, SLOT as u32);
    for (base, opcode, selected) in [(layout::TIMESTAMP_START, 0x80000003, layout::TIME_PAIR), (layout::TIMESTAMP_END, 3, layout::TIME_PAIR_END)] {
        u32_at(out, base, opcode);
        for (off, value) in [(timestamp::CONTROL, c + layout::TIME_CONTROL as u64), (timestamp::INTERNAL_PAIR, c + layout::TIME_PAIR as u64), (timestamp::SELECTED_CELL, c + selected as u64),
            (timestamp::QUEUE, a.work_queue), (timestamp::AUXILIARY, c + layout::TIME_AUXILIARY as u64), (timestamp::CONTEXT_TIME, c + layout::CONTEXT_TIME as u64), (timestamp::UUID, 1)] {
            u64_at(out, base + off, value);
        }
    }
    u32_at(out, layout::WAIT_IDLE, 1); // compute WFI
    u32_at(out, layout::FINALIZE, 0xc);
    for (off, value) in [(finalize::STATS, a.stats), (finalize::QUEUE, a.work_queue), (finalize::ENGINE_STATE, c + layout::ENGINE_STATE as u64),
        (finalize::UNKNOWN_24, 1), (finalize::STAMP, notification.map_or(c + layout::FIRMWARE_STAMP as u64, |n| n.fw_stamp)), (finalize::UMA_POINTER, sku + start::UMA as u64),
        (finalize::STATUS, c + layout::STATUS as u64), (finalize::UNKNOWN_71, c + layout::UNKNOWN_88C as u64)] {
        u64_at(out, layout::FINALIZE + off, value);
    }
    u32_at(out, layout::FINALIZE_CONTEXT, a.context);
    u32_at(out, layout::FINALIZE_STAMP, a.stamp);
    u32_at(out, layout::FINALIZE_RESTART, (-(layout::FINALIZE_DELTA as i32)) as u32);
    u32_at(out, layout::FINISH, 0x40000002);
    let notifier = layout::NOTIFIER;
    u64_at(out, notifier + notification_layout::THRESHOLD, threshold);
    u32_at(out, notifier + notification_layout::GENERATION, EVENT_GENERATION);
    u32_at(out, notifier + notification_layout::UNKNOWN_10, notification_layout::QUALIFIED_UNKNOWN_10);
    out[notifier + notification_layout::SLOT_SENTINELS..notifier + notification_layout::SLOT_SENTINELS + notification_layout::SLOT_SENTINELS_SIZE].fill(255);
    u32_at(out, notifier + notification_layout::UNKNOWN_114, notification_layout::QUALIFIED_UNKNOWN_114);
    u32_at(out, layout::FIRMWARE_STAMP, u32::MAX);
    u32_at(out, layout::USER_STAMP, u32::MAX);
    u64_at(out, layout::THRESHOLD, 1);
    encode_uma(&mut out[layout::UMA..layout::UMA_END], a.pool_state_gpu, pool_qwords, pages, a.page_list_gpu, a.counter, UmaEngine::Compute)?;
    Ok(())
}

pub(crate) fn omit_timestamps(sequence: &mut [u8], start: usize,
    total: usize, restart_offset: usize) -> Result<usize, ()> {
    let finalize = start + timestamp::END_DELTA + timestamp::SIZE;
    let new_finalize = start + 4;
    if sequence.len() < total || finalize + restart_offset + 4 > total ||
        sequence[start..start+4] != 0x80000003u32.to_le_bytes() ||
        sequence[start+timestamp::SIZE..start+timestamp::END_DELTA] != 1u32.to_le_bytes() ||
        sequence[start+timestamp::END_DELTA..start+timestamp::END_DELTA+4] != 3u32.to_le_bytes() {
        return Err(());
    }
    sequence.copy_within(start+timestamp::SIZE..start+timestamp::END_DELTA, start);
    sequence.copy_within(finalize..total, new_finalize);
    let length = total - 2 * timestamp::SIZE;
    let restart = new_finalize + restart_offset;
    sequence[restart..restart+4].copy_from_slice(&(-(new_finalize as i32)).to_le_bytes());
    sequence[length..total].fill(0);
    Ok(length)
}

pub(crate) fn timestamp_copies(sequence: &mut [u8], pointers: &[u64]) -> Result<usize, ()> {
    const END: usize = layout::TIMESTAMP_END - layout::SEQUENCE;
    const FINALIZE: usize = layout::FINALIZE_DELTA;
    const RECORD: usize = timestamp::SIZE;
    const BASE: usize = layout::SEQUENCE_SIZE;
    if pointers.len() > 8 || pointers.iter().any(|p| *p < 0xffff_fc00_0000_0000 || p & 7 != 0) {
        return Err(());
    }
    let extra = pointers.len() * RECORD;
    if sequence.len() < BASE + extra { return Err(()); }
    if pointers.is_empty() { return Ok(BASE); }
    let mut timestamp = [0; RECORD];
    timestamp.copy_from_slice(&sequence[END..FINALIZE]);
    sequence.copy_within(FINALIZE..BASE, FINALIZE + extra);
    for (i, pointer) in pointers.iter().enumerate() {
        let record = &mut sequence[FINALIZE + i * RECORD..FINALIZE + (i + 1) * RECORD];
        record.copy_from_slice(&timestamp);
        record[timestamp::USER_PAIR..timestamp::USER_PAIR + 8].copy_from_slice(&pointer.to_le_bytes());
    }
    let restart = FINALIZE + extra + layout::FINALIZE_RESTART_DELTA;
    sequence[restart..restart + 4].copy_from_slice(&(-(FINALIZE as i32 + extra as i32)).to_le_bytes());
    Ok(BASE + extra)
}

pub(crate) fn bind_uma_slot(bytes: &mut [u8], saved: Option<&[u8; uma_layout::SIZE]>,
    slot: u8, owner: u64) {
    let uma = &mut bytes[layout::UMA..layout::UMA_END];
    if let Some(saved) = saved { uma.copy_from_slice(saved); }
    uma[uma_layout::SLOT..uma_layout::SLOT + 4].copy_from_slice(&u32::from(slot).to_le_bytes());
    uma[uma_layout::OWNER..uma_layout::OWNER + 8].copy_from_slice(&owner.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rebind_preserves_completed_allocator() {
        let saved: [u8; uma_layout::SIZE] = core::array::from_fn(|i| (i * 37 + 19) as u8);
        for slot in 1..=32 {
            let mut bytes = [0x5a; SIZE];
            bind_uma_slot(&mut bytes, Some(&saved), slot, 0xfffffc0012340000);
            assert_eq!(&bytes[..layout::UMA], &[0x5a; layout::UMA]);
            assert!(bytes[layout::UMA_END..].iter().all(|v| *v == 0x5a));
            let mut expected = saved;
            expected[uma_layout::SLOT..uma_layout::SLOT + 4].copy_from_slice(&(slot as u32).to_le_bytes());
            expected[uma_layout::OWNER..uma_layout::OWNER + 8].copy_from_slice(&0xfffffc0012340000u64.to_le_bytes());
            assert_eq!(&bytes[layout::UMA..layout::UMA_END], &expected);
        }
    }
    fn addresses() -> Addresses {
        Addresses { command: 0xfffffc2000014000, command_gpu: 0x7000100000, cdm_gpu: 0x7000500000, preemption_gpu: 0x7000600000, metrics: 0xfffffc2000050000, auxiliary_gpu: 0x7000700000,
            work_queue: 0xfffffc2000030000, stats: 0xfffffc2000060000, context_store_gpu: 0x7000800000,
            pool_state_gpu: 0x7000200000, page_list_gpu: 0x7000300000,
            counter: 0xfffffc2000040000, context: 1, generation: 1, stamp: STAMP, event_slot: 0 }
    }
    #[test]
    fn repeated_end_timestamps_preserve_sequence_and_restart() {
        let mut command = [0; SIZE];
        encode(&mut command, addresses(), 16384, 512).unwrap();
        let original = &command[0x900..0xc00];
        for count in 0..=8 {
            let mut seq = [0x5a; 0x600];
            seq[..0x300].copy_from_slice(original);
            let pointers: [u64; 8] = core::array::from_fn(|i| 0xfffffc2000021800 + i as u64 * 16);
            let length = timestamp_copies(&mut seq, &pointers[..count]).unwrap();
            let extra = count * 0x4c;
            assert_eq!(length, 0x300 + extra);
            assert_eq!(&seq[..0x258], &original[..0x258]);
            for i in 0..count {
                let record = &seq[0x258 + i * 0x4c..0x258 + (i+1) * 0x4c];
                assert_eq!(&record[..0x24], &original[0x20c..0x230]);
                assert_eq!(&record[0x24..0x2c], &pointers[i].to_le_bytes());
                assert_eq!(&record[0x2c..], &original[0x238..0x258]);
            }
            assert_eq!(&seq[0x258 + extra..0x2bc + extra], &original[0x258..0x2bc]);
            assert_eq!(&seq[0x2bc + extra..0x2c0 + extra], &(-(0x258 + extra as i32)).to_le_bytes());
            assert_eq!(&seq[0x2c0 + extra..length], &original[0x2c0..]);
            assert!(seq[length..].iter().all(|b| *b == 0x5a));
        }
        let mut short = [0x5a; 0x300];
        assert!(timestamp_copies(&mut short, &[0xfffffc2000021800]).is_err());
        assert!(short.iter().all(|b| *b == 0x5a));
        let mut seq = [0; 0x600];
        assert!(timestamp_copies(&mut seq, &[0; 1]).is_err());
        assert!(timestamp_copies(&mut seq, &[0xfffffc2000021801]).is_err());
        assert!(timestamp_copies(&mut seq, &[0xfffffc2000021800; 9]).is_err());
    }

    #[test]
    fn packed_abi_roles_and_end_record() {
        let mut bytes = [0; SIZE];
        encode(&mut bytes, addresses(), 16384, 512).unwrap();
        let u64_at = |off| u64::from_le_bytes(bytes[off..off + 8].try_into().unwrap());
        assert_eq!(&bytes[0xc..0x10], &[1, 0, 0, 0]);
        assert_eq!(bytes[0x88b], 1);
        assert_eq!(&bytes[0x92c..0x934], &[1, 0, 0, 0, 1, 0, 0, 0]);
        assert_eq!(u64_at(0x7a0), 0x700050002c);
        assert_eq!(u64_at(0x30), 0x7000500000);
        assert_eq!(u64_at(0x720), 0x7000100020);
        assert_eq!(u64_at(0x914), 0xfffffc2000014020);
        assert_eq!(&bytes[0x728..0x72c], &[24, 0, 0x20, 1]);
        assert_eq!(u64_at(0x86a), 0xfffffc2000014e00);
        assert_eq!(u64_at(0x873), 0x1588000);
        assert_eq!(u64_at(0xb84), 0xfffffc2000014ee0);
        assert_eq!(&bytes[0xb8c..0xb90], &[0, 1, 0, 0]);
        assert_eq!(u64_at(0xbb4), 0xfffffc2000014a60);
        assert_eq!(&bytes[0xbbc..0xbc0], &(-0x258i32).to_le_bytes());
        assert_eq!(u64_at(0xbc1), 0xfffffc20000148ac);
        assert_eq!(u64_at(0xbc9), 0xfffffc200001488c);
        assert_eq!(&bytes[0xbd4..0xbd8], &[2, 0, 0, 0x40]);
        assert!(bytes[0x140..0x720].iter().all(|byte| *byte == 0));
    }
    #[test]
    fn engine_state_queue_and_notifier_are_distinct() {
        let a = addresses();
        let n = Notification { address: 0xfffffc2000070000, threshold: 0xfffffc2000074000,
            stamp: 0xfffffc2000078000, fw_stamp: 0xfffffc200007c000, event_seq: 0x100000003 };
        let mut bytes = [0; SIZE];
        encode_dispatch(&mut bytes, a, 16384, 512,
            Control { base: a.cdm_gpu, end: a.cdm_gpu + 0x30, usc_base: 0, wide_visibility: false }, Some(n)).unwrap();
        let ptr = |off| u64::from_le_bytes(bytes[off..off + 8].try_into().unwrap());
        for off in [0x91c, 0xb5c] { assert_eq!(ptr(off), a.stats); }
        for off in [0x924, 0xad8, 0xb28, 0xb64] { assert_eq!(ptr(off), a.work_queue); }
        assert_eq!(ptr(0x10), n.address);
        assert_eq!(ptr(0xc00), n.threshold);
        assert_eq!(ptr(0x800), n.stamp);
        assert_eq!(ptr(0x808), n.fw_stamp);
        assert_eq!(ptr(0xb84), n.fw_stamp);
        assert_eq!(ptr(0x938), n.event_seq);
        assert_eq!(&bytes[0x824..0x828], &3u32.to_le_bytes());
    }
    #[test]
    fn dispatch_and_finish_carry_the_same_event_value() {
        let mut a = addresses();
        a.stamp = 0x12300;
        a.generation = 7;
        let mut bytes = [0; SIZE];
        encode(&mut bytes, a, 16384, 512).unwrap();
        assert_eq!(bytes[0x88b], 7);
        assert_eq!(&bytes[0x934..0x938], &[0; 4]);
        assert_eq!(&bytes[0xc08..0xc0c], &[0; 4]);
        for offset in [0x810, 0xb8c] {
            assert_eq!(&bytes[offset..offset + 4], &0x12300u32.to_le_bytes());
        }
    }
    #[test]
    fn populated_freelist_requires_headroom() {
        let mut bytes = [0xa5; SIZE];
        assert!(encode(&mut bytes, addresses(), 512, 512).is_err());
        assert_eq!(bytes, [0xa5; SIZE]);
        assert!(encode(&mut bytes, addresses(), 1024, 512).is_ok());
    }
    #[test]
    fn render_pool_allows_peer_fragment_while_tiler_is_active() {
        let a = addresses();
        for pages in [0, 512, 8192] {
            for (engine, expected) in [(UmaEngine::Render, 0u32),
                (UmaEngine::Compute, u32::from(pages != 0))] {
                let mut bytes = [0xa5; 0xa0];
                encode_uma(&mut bytes, a.pool_state_gpu, 16384, pages,
                    a.page_list_gpu, a.counter, engine).unwrap();
                assert_eq!(&bytes[0x5c..0x60], &expected.to_le_bytes());
                assert_eq!(&bytes[0x60..0x64], &[0; 4]);
                assert_eq!(&bytes[0x24..0x28], &pages.to_le_bytes());
            }
        }
    }
    #[test]
    fn invalid_address_is_transactional() {
        let mut bytes = [0x55; SIZE];
        let mut a = addresses(); a.command_gpu |= 1;
        assert!(encode(&mut bytes, a, 16384, 512).is_err());
        assert!(bytes.iter().all(|byte| *byte == 0x55));
        assert!(encode(&mut bytes[..SIZE - 1], addresses(), 16384, 512).is_err());
    }
}

pub(crate) mod uma_layout {
    pub(crate) const SIZE: usize = 0xa0;
    pub(crate) const OWNER: usize = 0;
    pub(crate) const SLOT: usize = 8;
    pub(crate) const EMPTY: usize = 12;
    pub(crate) const UNKNOWN_10: usize = 16;
    pub(crate) const POOL: usize = 0x14;
    pub(crate) const CAPACITY: usize = 0x1c;
    pub(crate) const READ_CURSOR: usize = 0x20;
    pub(crate) const PAGES: usize = 0x24;
    pub(crate) const EMPTY_28: usize = 0x28;
    pub(crate) const PAGES_2C: usize = 0x2c;
    pub(crate) const PAGE_LIST: usize = 0x30;
    pub(crate) const UNKNOWN_40: usize = 0x40;
    pub(crate) const UNKNOWN_44: usize = 0x44;
    pub(crate) const PAGE_GROUP_BYTES: usize = 0x48;
    pub(crate) const SUBMITTED_POINTER: usize = 0x4c;
    pub(crate) const COMPLETED: usize = 0x54;
    pub(crate) const COMPUTE_EXCLUSIVE: usize = 0x5c;
    pub(crate) const POPULATED: usize = 0x84;
    pub(crate) const UNKNOWN_94: usize = 0x94;
}

#[derive(Clone, Copy)]
pub(crate) enum UmaEngine { Compute, Render }

pub(crate) fn encode_uma(out: &mut [u8], pool: u64, pool_qwords: u32,
    pages: u32, list: u64, counter: u64, engine: UmaEngine) -> Result<(), ()> {
    if out.len() < uma_layout::SIZE || pool_qwords == 0 || pages >= pool_qwords || pages % 512 != 0
        || [pool,list].iter().any(|p| *p == 0 || *p & 0x3fff != 0 || *p >= 1<<42)
        || counter < 0xfffffc0000000000 || counter & 7 != 0 { return Err(()); }
    out[..uma_layout::SIZE].fill(0);
    let u32_at = |b: &mut [u8], off, value: u32| b[off..off + 4].copy_from_slice(&value.to_le_bytes());
    let u64_at = |b: &mut [u8], off, value: u64| b[off..off + 8].copy_from_slice(&value.to_le_bytes());
    u64_at(out, uma_layout::OWNER, 1);
    let exclusive = matches!(engine, UmaEngine::Compute) && pages != 0;
    for (off, value) in [(uma_layout::SLOT, 1), (uma_layout::EMPTY, u32::from(pages == 0)), (uma_layout::UNKNOWN_10, 2), (uma_layout::CAPACITY, pool_qwords),
        (uma_layout::PAGES, pages), (uma_layout::EMPTY_28, u32::from(pages == 0)), (uma_layout::PAGES_2C, pages),
        (uma_layout::UNKNOWN_40, 4), (uma_layout::PAGE_GROUP_BYTES, pages / 512 * 8), (uma_layout::COMPUTE_EXCLUSIVE, u32::from(exclusive)),
        (uma_layout::POPULATED, u32::from(pages != 0)), (uma_layout::UNKNOWN_94, 2)] {
        u32_at(out, off, value);
    }
    u64_at(out, uma_layout::POOL, pool);
    u64_at(out, uma_layout::PAGE_LIST, list);
    u64_at(out, uma_layout::SUBMITTED_POINTER, counter);
    Ok(())
}
