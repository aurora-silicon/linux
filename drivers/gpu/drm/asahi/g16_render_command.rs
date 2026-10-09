// SPDX-License-Identifier: GPL-2.0-only
//! 25G83 render envelopes and firmware sequences.

use crate::g16_render::{BuildError, Stage};

#[derive(Clone, Copy)]
pub(crate) struct Args {
    pub(crate) command: u64, pub sku: u64, pub queue: u64, pub stats: u64,
    pub(crate) pb_slot: u64, pub manager: u64, pub pb_aux: u64,
    pub(crate) notifier: u64,
    /// JobMeta first pointer: shared stamp published by the ready notifier.
    pub(crate) user_stamp: u64,
    /// JobMeta second pointer and Finalize: internal dependency stamp.
    pub(crate) fw_stamp: u64,
    pub(crate) uma: u64, pub uma_aux: u64, pub timestamp_storage: u64,
    pub(crate) context: u32, pub generation: u8, pub counter: u64,
    pub(crate) uuid: u32, pub stamp: u32, pub event_slot: u32,
    pub(crate) fragment_slot: u32, pub fragment_stamp: u32,
    pub(crate) pb_id: u32, pub pass_id: u32,
}
const RENDER_MODE: u8 = 1;

pub(crate) use crate::g16_compute::timestamp;

pub(crate) mod metadata {
    pub(crate) const SIZE: usize = 0x4000;
    pub(crate) const SCRATCH_GPU: usize = 0;
    pub(crate) const SCRATCH_FW: usize = 8;
    pub(crate) const COMPACT_SCENE_LIST: usize = 0x28;
    pub(crate) const STATISTICS_POINTER: usize = 0x40;
    pub(crate) const PB_AUX: usize = 0x100;
    pub(crate) const STATISTICS: usize = 0x200;
    pub(crate) const INIT_BUFFER: usize = 0x300;
    pub(crate) const TILER_DEPENDENCY: usize = 0x340;
    pub(crate) const COMPUTE_DEPENDENCY: usize = 0x380;
    pub(crate) const LEGACY_FW_STAMPS: [usize; 2] = [0x400, 0x410];
    pub(crate) const LEGACY_USER_STAMPS: [usize; 2] = [0x408, 0x418];
    pub(crate) const PRIOR_FRAGMENT: usize = 0x440;
    pub(crate) const UMA_SUBMITTED: usize = 0x500;
    pub(crate) const TIME_PAIRS: [usize; 2] = [0x600, 0x620];
    pub(crate) const USER_TIME_PAIRS: [usize; 2] = [0x610, 0x630];
    pub(crate) const FANOUT_PAIRS: usize = 0x700;
    pub(crate) const UMA_AUX: [usize; 2] = [0x100, 0x200];
}
pub(crate) mod parameter {
    pub(crate) const SIZE: usize = 0xc0;
    pub(crate) const HARDWARE_ID: usize = 0xc;
    pub(crate) const ID: usize = 0x10;
    pub(crate) const PAGES_FW: usize = 0x20;
    pub(crate) const PAGES_GPU: usize = 0x28;
    pub(crate) const LIST_BYTES: usize = 0x30;
    pub(crate) const PAGE_COUNT: usize = 0x34;
    pub(crate) const BLOCK_CAPACITY: usize = 0x38;
    pub(crate) const WRITE: usize = 0x3c;
    pub(crate) const READ: usize = 0x40;
    pub(crate) const BLOCKS: usize = 0x44;
    pub(crate) const RING: usize = 0x4c;
    pub(crate) const LAST_PAGE: usize = 0x54;
    pub(crate) const BLOCK_BYTES: usize = 0x58;
    pub(crate) const COUNTER: usize = 0x64;
    pub(crate) const MAX_PAGES: usize = 0x7c;
    pub(crate) const MIN_PAGES: usize = 0x80;
    pub(crate) const DISCARD: usize = 0x84;
}
pub(crate) mod dependency {
    pub(crate) const SIZE: usize = 0x40;
    pub(crate) const KIND: usize = 0;
    pub(crate) const STAMP: usize = 4;
    pub(crate) const STAMP_COPY: usize = 0xc;
    pub(crate) const WAIT_VALUE: usize = 0x14;
    pub(crate) const WAIT_SLOT: usize = 0x20;
    pub(crate) const SELF_VALUE: usize = 0x24;
    pub(crate) const INTERNAL: usize = 0x3c;
}
pub(crate) mod init_buffer {
    pub(crate) const SIZE: usize = 0x20;
    pub(crate) const KIND: usize = 0;
    pub(crate) const CONTEXT: usize = 4;
    pub(crate) const ID: usize = 8;
    pub(crate) const CURSOR: usize = 0xc;
    pub(crate) const STAGED: usize = 0x10;
    pub(crate) const MANAGER: usize = 0x14;
    pub(crate) const STAMP: usize = 0x1c;
}

pub(crate) struct Layout {
    pub(crate) command_sequence: usize,
    pub(crate) command_sequence_size: usize,
    pub(crate) timestamp_start: usize,
    pub(crate) finalize: usize,
    pub(crate) finalize_restart: usize,
    pub(crate) finalize_uma_pointer: usize,
    pub(crate) finalize_opcode: u32,
    pub(crate) uma_header: usize,
    pub(crate) sequence_size: usize,
    pub(crate) command_flush_stamps: usize,
    pub(crate) command_status: usize,
    pub(crate) attachments: usize,
    pub(crate) finalize_has_attachments: usize,
    finish: usize,
    job_meta: usize,
    command_time_control: usize,
    command_time_pair: usize,
    command_time_auxiliary: usize,
    command_context_time: usize,
}

pub(crate) const fn layout(stage: Stage) -> Layout {
    match stage {
        Stage::Tiling => Layout {
            command_sequence: 0x770, command_sequence_size: 0x778,
            timestamp_start: 0x1cc, finalize: 0x268, finalize_restart: 0x80,
            finalize_uma_pointer: 0x78, finalize_opcode: 6, uma_header: 0x190,
            command_flush_stamps: 0x878 + 0x1c, command_status: 0x918,
            attachments: 0x88, finalize_has_attachments: 0x2ec,
            sequence_size: 0x300, finish: 0x94, job_meta: 0x878,
            command_time_control: 0x8c0, command_time_pair: 0x8c8,
            command_time_auxiliary: 0x928, command_context_time: 0x83c,
        },
        Stage::Fragment => Layout {
            command_sequence: 0x14, command_sequence_size: 0x1c,
            timestamp_start: 0x1ec, finalize: 0x288, finalize_restart: 0xa0,
            finalize_uma_pointer: 0x98, finalize_opcode: 8, uma_header: 0x1b0,
            command_flush_stamps: 0xbc0 + 0x1c, command_status: 0xc60,
            attachments: 0xa8, finalize_has_attachments: 0x32c,
            sequence_size: 0x380, finish: 0xbc, job_meta: 0xbc0,
            command_time_control: 0xc08, command_time_pair: 0xc10,
            command_time_auxiliary: 0xc70, command_context_time: 0xb84,
        },
    }
}

fn put(out: &mut [u8], offset: usize, value: u64, size: usize) {
    out[offset..offset + size].copy_from_slice(&value.to_le_bytes()[..size]);
}
fn valid_fw(address: u64) -> bool {
    address >= 0xffff_fc00_0000_0000 && address <= u64::MAX - 0x4000 && address & 3 == 0
}

pub(crate) fn sequence_size(stage: Stage) -> usize {
    layout(stage).sequence_size
}

pub(crate) fn encode(command: &mut [u8], sku: &mut [u8], stage: Stage, a: Args) -> Result<(), BuildError> {
    let ta = stage == Stage::Tiling;
    let wire = layout(stage);
    let (start, finish, uma, meta) = (wire.timestamp_start, wire.finish, wire.uma_header, wire.job_meta);
    let total = sequence_size(stage);
    if command.len() < stage.command_size() || sku.len() < total { return Err(BuildError::BufferTooSmall); }
    if a.context >= 64 || a.generation == 0 || a.event_slot >= 128 || a.stamp == 0
        || a.fragment_slot >= 128 || a.fragment_stamp == 0
        || ![a.command, a.sku, a.queue, a.stats, a.pb_slot, a.manager, a.pb_aux,
              a.notifier, a.user_stamp, a.fw_stamp, a.uma,
              a.timestamp_storage].iter().all(|p| valid_fw(*p))
        || a.uma_aux == 0 || a.uma_aux >= 1 << 42 {
        return Err(BuildError::Address);
    }
    command[..stage.command_size()].fill(0);
    sku[..total].fill(0);
    put(command, 0, u64::from(!ta), 4);
    put(command, 4, a.counter, 8);
    put(command, 0xc, a.context as u64, 4);
    for (off, value, size) in [
        (0, a.user_stamp, 8), (8, a.fw_stamp, 8), (0x10, a.stamp as u64, 4),
        (0x14, a.event_slot as u64, 4), (0x20, a.uuid as u64, 4)] {
        put(command, meta + off, value, size);
    }
    if ta {
        for (off, value, size) in [
            (0x10, a.notifier, 8), (0x18, a.pb_id as u64, 4),
            (0x20, a.manager, 8), (0x28, a.pb_slot, 8), (0x30, a.pb_aux, 8),
            (wire.command_sequence, a.sku, 8), (wire.command_sequence_size, total as u64, 4), (0x8a0, a.pass_id as u64, 4)] {
            put(command, off, value, size);
        }
        command[0x917] = a.generation;
        put(command, 0x77c, a.fragment_slot as u64, 4);
        put(command, 0x780, a.fragment_stamp as u64, 4);
    } else {
        for (off, value, size) in [
            (wire.command_sequence, a.sku, 8), (wire.command_sequence_size, total as u64, 4), (0x20, a.notifier, 8),
            (0x28, a.manager, 8), (0x30, a.pb_slot, 8), (0x38, a.pb_aux, 8),
            (0xbe8, a.pass_id as u64, 4)] { put(command, off, value, size); }
        command[0xc5f] = a.generation;
        command[0xc84] = RENDER_MODE;
    }
    put(sku, 0, if ta { 5 } else { 7 }, 4);
    put(sku, if ta { 0x4c } else { 0x64 }, crate::g16_compute::EVENT_GENERATION as u64, 4);
    put(sku, 0x14, a.command + if ta { 0x40 } else { 0x80 }, 8);
    put(sku, uma, a.uma, 8);
    put(sku, uma + 0x18, a.uma_aux, 8);
    put(sku, uma + 0x28, a.counter, 8);
    let (control, pointers, auxiliary, context_time) = (wire.command_time_control,
        wire.command_time_pair, wire.command_time_auxiliary, wire.command_context_time);
    put(command, pointers, a.timestamp_storage, 8);
    put(command, pointers + 8, a.timestamp_storage + 8, 8);
    for (offset, opcode, selected) in [(start, 0x80000003, pointers),
                                      (start + timestamp::END_DELTA, 3, pointers + 8)] {
        let timestamp = &mut sku[offset..offset + timestamp::SIZE];
        put(timestamp, 0, opcode, 4);
        for (off, value) in [(timestamp::CONTROL, a.command + control as u64),
            (timestamp::INTERNAL_PAIR, a.command + pointers as u64),
            (timestamp::SELECTED_CELL, a.command + selected as u64), (timestamp::QUEUE, a.queue),
            (timestamp::AUXILIARY, a.command + auxiliary as u64),
            (timestamp::CONTEXT_TIME, a.command + context_time as u64), (timestamp::UUID, a.uuid as u64)] {
            put(timestamp, off, value, 8);
        }
    }
    put(sku, start + timestamp::SIZE, 1, 4);
    if ta {
        for (off, value, size) in [
            (0x1c,a.manager,8), (0x24,a.pb_slot,8), (0x2c,a.stats,8),
            (0x34,a.queue,8), (0x3c,a.command+0x85c,8), (0x44,a.context as u64,4),
            (0x48,1,4), (0x50,a.pb_id as u64,4), (0x58,a.pass_id as u64,4),
            (0x64,a.command+0x784,8), (0x6c,a.command+0x8a8,8),
            (0x7c,a.uuid as u64,4), (0x1c0,a.event_slot as u64,4)] { put(sku,off,value,size); }
    } else {
        for (off, value, size) in [
            (0x1c,a.pb_slot,8), (0x24,a.stats,8), (0x2c,a.command+0xb70,8),
            (0x34,a.pb_slot+0x54,8), (0x3c,a.command+0xbb0,8), (0x44,a.command+0xbb4,8),
            (0x4c,a.queue,8), (0x54,a.command,8), (0x5c,a.context as u64,4),
            (0x60,1,4), (0x68,a.pb_id as u64,4), (0x70,a.pass_id as u64,4),
            (0x7c,a.command+0xa58,8), (0x84,a.command+0xbf0,8),
            (0xa0,a.uuid as u64,4), (0x1e0,a.event_slot as u64,4)] { put(sku,off,value,size); }
    }
    if !ta { sku[0x1e4] = RENDER_MODE; }
    let finalize = wire.finalize;
    let f = &mut sku[finalize..];
    put(f, 0, wire.finalize_opcode as u64, 4);
    if ta {
        for (off,value,size) in [
            (4,a.pb_slot,8), (0xc,a.manager,8), (0x14,a.stats,8), (0x1c,a.queue,8),
            (0x24,a.command+0x85c,8), (0x2c,a.context as u64,4),
            (0x34,a.command+0x784,8), (0x40,a.uuid as u64,4), (0x48,a.fw_stamp,8),
            (0x50,a.stamp as u64,4), (wire.finalize_uma_pointer,a.sku+uma as u64,8),
            (wire.finalize_restart,(-(finalize as i32)) as u32 as u64,4), (0x85,a.command+wire.command_status as u64,8)] { put(f,off,value,size); }
    } else {
        for (off,value,size) in [
            (4,a.uuid as u64,4), (0xc,a.fw_stamp,8), (0x14,a.stamp as u64,4),
            (0x1c,a.pb_slot,8), (0x24,a.manager,8), (0x2c,1,4), (0x30,a.stats,8),
            (0x38,a.command+0xbb0,8), (0x40,a.command+0xbb4,8), (0x48,a.command+0xb70,8),
            (0x50,a.queue,8), (0x58,a.command,8), (0x60,a.context as u64,4),
            (0x64,a.command+0xa58,8), (wire.finalize_uma_pointer,a.sku+uma as u64,8),
            (wire.finalize_restart,(-(finalize as i32)) as u32 as u64,4), (0xa5,a.command+wire.command_status as u64,8),
            (0xad,1,4), (0xb2,a.event_slot as u64,4)] { put(f,off,value,size); }
    }
    f[if ta { 0x8d } else { 0xb1 }] = RENDER_MODE;
    put(f,finish,0x40000002,4);
    Ok(())
}

pub(crate) fn j613_fragment_program(sequence: &mut [u8]) -> Result<usize, BuildError> {
    const OLD: usize = 0x380;
    const SIZE: usize = OLD - 16;
    if sequence.len() < OLD { return Err(BuildError::BufferTooSmall); }
    let word = |o| u32::from_le_bytes(sequence[o..o+4].try_into().unwrap());
    if sequence.len() != OLD || word(0) != 7 || word(0x1ec) != 0x80000003
        || word(0x238) != 1 || word(0x23c) != 3 || word(0x288) != 8
        || word(0x328) != (-0x288i32) as u32 || word(0x344) != 0x40000002 {
        return Err(BuildError::Address);
    }
    let mut image = [0u8; OLD];
    image[..0x1e4].copy_from_slice(&sequence[..0x1e4]);
    image[0x1e4..0x334].copy_from_slice(&sequence[0x1ec..0x33c]);
    image[0x334..SIZE].copy_from_slice(&sequence[0x344..OLD]);
    put(&mut image, 0x280 + 0xa0, (-0x280i32) as u32 as u64, 4);
    sequence.copy_from_slice(&image);
    Ok(SIZE)
}

pub(crate) fn relocate_program(sequence: &mut [u8], stage: Stage, address: u64) -> Result<(), BuildError> {
    let base = sequence_size(stage);
    if !valid_fw(address) || sequence.len() < base || sequence.len() > 0x800 ||
        (sequence.len() - base) % timestamp::SIZE != 0 ||
        (stage == Stage::Tiling && sequence.len() != base) { return Err(BuildError::Address); }
    let extra = sequence.len() - base;
    let wire = layout(stage);
    let (finalize, restart, pointer, uma, opcode) = (wire.finalize + extra,
        wire.finalize_restart, wire.finalize_uma_pointer, wire.uma_header as u64, wire.finalize_opcode);
    if sequence[finalize..finalize+4] != opcode.to_le_bytes() ||
        sequence[finalize+restart..finalize+restart+4] != (-(finalize as i32)).to_le_bytes() {
        return Err(BuildError::Address);
    }
    put(sequence, finalize + pointer, address + uma, 8);
    Ok(())
}

#[derive(Clone, Copy)]
pub(crate) struct ParameterManager {
    pub(crate) pages_fw: u64, pub pages_gpu: u64, pub blocks: u64, pub ring: u64,
    pub(crate) counter: u64, pub discard: u64, pub list_bytes: u32, pub pages: u32,
    pub(crate) block_capacity: u32, pub write: u32, pub read: u32,
    pub(crate) max_pages: u32, pub min_pages: u32, pub id: u32,
}
#[derive(Clone, Copy)]
pub(crate) enum ParameterLayout { M4, J613 }
impl ParameterLayout {
    pub(crate) fn for_chip(chip: u32) -> Self {
        if chip == 0x8122 { Self::J613 } else { Self::M4 }
    }
    pub(crate) fn last_page(self) -> usize {
        match self { Self::M4 => parameter::LAST_PAGE, Self::J613 => 0x50 }
    }
    fn offsets(self) -> [usize; 17] {
        match self {
            Self::M4 => [0x0c,0x10,0x20,0x28,0x30,0x34,0x38,0x3c,0x40,
                0x44,0x4c,0x54,0x58,0x64,0x7c,0x80,0x84],
            Self::J613 => [0x08,0x0c,0x1c,0x24,0x2c,0x30,0x34,0x38,0x3c,
                0x40,0x48,0x50,0x54,0x60,0x78,0x7c,0x80],
        }
    }
}
pub(crate) fn parameter_manager(out: &mut [u8], a: ParameterManager) -> Result<(), BuildError> {
    parameter_manager_for_layout(out, a, ParameterLayout::M4)
}
pub(crate) fn parameter_manager_for_layout(out: &mut [u8], a: ParameterManager,
    layout: ParameterLayout) -> Result<(), BuildError> {

    if out.len() < parameter::SIZE { return Err(BuildError::BufferTooSmall); }
    if ![a.pages_fw,a.blocks,a.ring,a.counter].iter().all(|p| valid_fw(*p))
        || a.pages_gpu == 0 || a.pages_gpu >= 1 << 43 || a.pages == 0 || a.pages & 3 != 0
        || a.pages > a.list_bytes / 4 || a.pages / 4 >= a.block_capacity
        || a.write >= a.block_capacity || a.read >= a.block_capacity
        || a.min_pages > a.max_pages || a.max_pages > a.list_bytes / 4 { return Err(BuildError::Address); }
    out[..parameter::SIZE].fill(0);
    let values = [
        (a.id as u64,4), (a.id as u64,4), (a.pages_fw,8), (a.pages_gpu,8),
        (a.list_bytes as u64,4), (a.pages as u64,4), (a.block_capacity as u64,4),
        (a.write as u64,4), (a.read as u64,4), (a.blocks,8), (a.ring,8),
        ((a.pages-1) as u64,4), (0x20000,4), (a.counter,8),
        (a.max_pages as u64,4), (a.min_pages as u64,4), (a.discard,8)];
    for (off, (value,size)) in layout.offsets().into_iter().zip(values) {
        put(out,off,value,size);
    }
    Ok(())
}
pub(crate) fn init_parameter_buffer(out: &mut [u8], context: u32, id: u32,
    cursor: u32, staged: u32, manager: u64, stamp: u32) -> Result<(), BuildError> {
    if out.len() < init_buffer::SIZE { return Err(BuildError::BufferTooSmall); }
    if context >= 64 || !valid_fw(manager) { return Err(BuildError::Address); }
    for (off,value,size) in [(init_buffer::KIND,6,4), (init_buffer::CONTEXT,context as u64,4), (init_buffer::ID,id as u64,4),
        (init_buffer::CURSOR,cursor as u64,4), (init_buffer::STAGED,staged as u64,4), (init_buffer::MANAGER,manager,8), (init_buffer::STAMP,stamp as u64,4)] {
        put(out,off,value,size);
    }
    Ok(())
}

pub(crate) fn render_dependency(out: &mut [u8], stamp_address: u64,
    wait_slot: u8, wait_value: u32, stamp_self: u32) -> Result<(), BuildError> {
    if out.len() < dependency::SIZE { return Err(BuildError::BufferTooSmall); }
    if !valid_fw(stamp_address) || wait_slot >= 128 || wait_value == 0 || stamp_self == 0 {
        return Err(BuildError::Address);
    }
    out[..dependency::SIZE].fill(0);
    put(out, dependency::KIND, 4, 4);
    put(out, dependency::STAMP, stamp_address, 8);
    put(out, dependency::STAMP_COPY, stamp_address, 8);
    put(out, dependency::WAIT_VALUE, wait_value as u64, 4);
    put(out, dependency::WAIT_SLOT, wait_slot as u64, 4);
    put(out, dependency::SELF_VALUE, stamp_self as u64, 4);
    Ok(())
}

pub(crate) fn fragment_timestamp_copies(sequence: &mut [u8], pointers: &[u64])
    -> Result<usize, BuildError> {
    const END: usize = layout(Stage::Fragment).timestamp_start + timestamp::END_DELTA;
    const FINALIZE: usize = layout(Stage::Fragment).finalize;
    const RECORD: usize = timestamp::SIZE;
    let base = sequence_size(Stage::Fragment);
    let extra = pointers.len() * RECORD;
    if pointers.len() > 8 || !pointers.iter().all(|p| valid_fw(*p)) {
        return Err(BuildError::Address);
    }
    if sequence.len() < base + extra { return Err(BuildError::BufferTooSmall); }
    if pointers.is_empty() { return Ok(base); }
    let mut timestamp = [0; RECORD];
    timestamp.copy_from_slice(&sequence[END..FINALIZE]);
    sequence.copy_within(FINALIZE..base, FINALIZE + extra);
    for (i, pointer) in pointers.iter().enumerate() {
        let record = &mut sequence[FINALIZE + i * RECORD..FINALIZE + (i + 1) * RECORD];
        record.copy_from_slice(&timestamp);
        put(record, timestamp::USER_PAIR, *pointer, 8);
    }
    put(sequence, FINALIZE + extra + layout(Stage::Fragment).finalize_restart,
        (-(FINALIZE as i32 + extra as i32)) as u32 as u64, 4);
    Ok(base + extra)
}
