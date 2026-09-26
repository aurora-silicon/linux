// SPDX-License-Identifier: GPL-2.0-only

use crate::g16_render::{BuildError, Stage};

#[derive(Clone, Copy)]
pub(crate) struct Args {
    pub command: u64, pub sku: u64, pub queue: u64, pub stats: u64,
    pub pb_slot: u64, pub manager: u64, pub pb_aux: u64,
    pub notifier: u64, pub shared_stamp: u64, pub stamp_address: u64,
    pub uma: u64, pub uma_aux: u64, pub timestamp_storage: u64,
    pub context: u32, pub generation: u8, pub counter: u64,
    pub uuid: u32, pub stamp: u32, pub event_slot: u32,
    pub fragment_slot: u32, pub fragment_stamp: u32,
    pub pb_id: u32, pub pass_id: u32,
}
const RENDER_MODE: u8 = 1;

fn put(out: &mut [u8], offset: usize, value: u64, size: usize) {
    out[offset..offset + size].copy_from_slice(&value.to_le_bytes()[..size]);
}
fn valid_fw(address: u64) -> bool {
    address >= 0xffff_fc00_0000_0000 && address <= u64::MAX - 0x4000 && address & 3 == 0
}

pub(crate) fn sequence_size(stage: Stage) -> usize {
    match stage { Stage::Tiling => 0x300, Stage::Fragment => 0x380 }
}

pub(crate) fn encode(command: &mut [u8], sku: &mut [u8], stage: Stage, a: Args) -> Result<(), BuildError> {
    let ta = stage == Stage::Tiling;
    let (start, finish, uma, meta) = if ta { (0x1cc, 0x94, 0x190, 0x878) }
                                   else { (0x1ec, 0xbc, 0x1b0, 0xbc0) };
    let total = sequence_size(stage);
    if command.len() < stage.command_size() || sku.len() < total { return Err(BuildError::BufferTooSmall); }
    if a.context >= 64 || a.generation == 0 || a.event_slot >= 128 || a.stamp == 0
        || a.fragment_slot >= 128 || a.fragment_stamp == 0
        || ![a.command, a.sku, a.queue, a.stats, a.pb_slot, a.manager, a.pb_aux,
              a.notifier, a.shared_stamp, a.stamp_address, a.uma,
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
        (0, a.shared_stamp, 8), (8, a.stamp_address, 8), (0x10, a.stamp as u64, 4),
        (0x14, a.event_slot as u64, 4), (0x20, a.uuid as u64, 4)] {
        put(command, meta + off, value, size);
    }
    if ta {
        for (off, value, size) in [
            (0x10, a.notifier, 8), (0x18, a.pb_id as u64, 4),
            (0x20, a.manager, 8), (0x28, a.pb_slot, 8), (0x30, a.pb_aux, 8),
            (0x770, a.sku, 8), (0x778, total as u64, 4), (0x8a0, a.pass_id as u64, 4)] {
            put(command, off, value, size);
        }
        command[0x917] = a.generation;
        put(command, 0x77c, a.fragment_slot as u64, 4);
        put(command, 0x780, a.fragment_stamp as u64, 4);
    } else {
        for (off, value, size) in [
            (0x14, a.sku, 8), (0x1c, total as u64, 4), (0x20, a.notifier, 8),
            (0x28, a.manager, 8), (0x30, a.pb_slot, 8), (0x38, a.pb_aux, 8),
            (0xbe8, a.pass_id as u64, 4)] { put(command, off, value, size); }
        command[0xc5f] = a.generation;
        command[0xc84] = RENDER_MODE;
    }
    put(sku, 0, if ta { 5 } else { 7 }, 4);
    // Completion must flush the same cache generation that starts this work.
    put(sku, if ta { 0x4c } else { 0x64 }, crate::g16_compute::EVENT_GENERATION as u64, 4);
    put(sku, 0x14, a.command + if ta { 0x40 } else { 0x80 }, 8);
    put(sku, uma, a.uma, 8);
    put(sku, uma + 0x18, a.uma_aux, 8);
    put(sku, uma + 0x28, a.counter, 8);
    let (control, pointers, auxiliary, context_time) = if ta {
        (0x8c0, 0x8c8, 0x928, 0x83c)
    } else {
        (0xc08, 0xc10, 0xc70, 0xb84)
    };
    put(command, pointers, a.timestamp_storage, 8);
    put(command, pointers + 8, a.timestamp_storage + 8, 8);
    for (offset, opcode, selected) in [(start, 0x80000003, pointers),
                                      (start + 0x50, 3, pointers + 8)] {
        let timestamp = &mut sku[offset..offset + 0x4c];
        put(timestamp, 0, opcode, 4);
        for (off, value) in [(4, a.command + control as u64),
            (0xc, a.command + pointers as u64),
            (0x14, a.command + selected as u64), (0x1c, a.queue),
            (0x2c, a.command + auxiliary as u64),
            (0x34, a.command + context_time as u64), (0x44, a.uuid as u64)] {
            put(timestamp, off, value, 8);
        }
    }
    put(sku, start + 0x4c, 1, 4);
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
    let finalize = start + 0x9c;
    let f = &mut sku[finalize..];
    put(f, 0, if ta { 6 } else { 8 }, 4);
    if ta {
        for (off,value,size) in [
            (4,a.pb_slot,8), (0xc,a.manager,8), (0x14,a.stats,8), (0x1c,a.queue,8),
            (0x24,a.command+0x85c,8), (0x2c,a.context as u64,4),
            (0x34,a.command+0x784,8), (0x40,a.uuid as u64,4), (0x48,a.stamp_address,8),
            (0x50,a.stamp as u64,4), (0x78,a.sku+uma as u64,8),
            (0x80,(-(finalize as i32)) as u32 as u64,4), (0x85,a.command+0x918,8)] { put(f,off,value,size); }
    } else {
        for (off,value,size) in [
            (4,a.uuid as u64,4), (0xc,a.stamp_address,8), (0x14,a.stamp as u64,4),
            (0x1c,a.pb_slot,8), (0x24,a.manager,8), (0x2c,1,4), (0x30,a.stats,8),
            (0x38,a.command+0xbb0,8), (0x40,a.command+0xbb4,8), (0x48,a.command+0xb70,8),
            (0x50,a.queue,8), (0x58,a.command,8), (0x60,a.context as u64,4),
            (0x64,a.command+0xa58,8), (0x98,a.sku+uma as u64,8),
            (0xa0,(-(finalize as i32)) as u32 as u64,4), (0xa5,a.command+0xc60,8),
            (0xad,1,4), (0xb2,a.event_slot as u64,4)] { put(f,off,value,size); }
    }
    f[if ta { 0x8d } else { 0xb1 }] = RENDER_MODE;
    put(f,finish,0x40000002,4);
    Ok(())
}

#[derive(Clone, Copy)]
pub(crate) struct ParameterManager {
    pub pages_fw: u64, pub pages_gpu: u64, pub blocks: u64, pub ring: u64,
    pub counter: u64, pub discard: u64, pub list_bytes: u32, pub pages: u32,
    pub block_capacity: u32, pub write: u32, pub read: u32,
    pub max_pages: u32, pub min_pages: u32, pub id: u32,
}
pub(crate) fn parameter_manager(out: &mut [u8], a: ParameterManager) -> Result<(), BuildError> {
    if out.len() < 0xc0 { return Err(BuildError::BufferTooSmall); }
    if ![a.pages_fw,a.blocks,a.ring,a.counter].iter().all(|p| valid_fw(*p))
        || a.pages_gpu == 0 || a.pages_gpu >= 1 << 43 || a.pages == 0 || a.pages & 3 != 0
        || a.pages > a.list_bytes / 4 || a.pages / 4 >= a.block_capacity
        || a.write >= a.block_capacity || a.read >= a.block_capacity
        || a.min_pages > a.max_pages || a.max_pages > a.list_bytes / 4 { return Err(BuildError::Address); }
    out[..0xc0].fill(0);
    for (off,value,size) in [
        (0x10,a.id as u64,4), (0x20,a.pages_fw,8), (0x28,a.pages_gpu,8),
        (0x30,a.list_bytes as u64,4), (0x34,a.pages as u64,4), (0x38,a.block_capacity as u64,4),
        (0x3c,a.write as u64,4), (0x40,a.read as u64,4), (0x44,a.blocks,8), (0x4c,a.ring,8),
        (0x54,(a.pages-1) as u64,4), (0x58,0x20000,4), (0x64,a.counter,8),
        (0x7c,a.max_pages as u64,4), (0x80,a.min_pages as u64,4), (0x84,a.discard,8)] { put(out,off,value,size); }
    Ok(())
}
pub(crate) fn init_parameter_buffer(out: &mut [u8], context: u32, id: u32,
    cursor: u32, staged: u32, manager: u64, stamp: u32) -> Result<(), BuildError> {
    if out.len() < 0x20 { return Err(BuildError::BufferTooSmall); }
    if context >= 64 || !valid_fw(manager) { return Err(BuildError::Address); }
    for (off,value,size) in [(0,6,4), (4,context as u64,4), (8,id as u64,4),
        (0xc,cursor as u64,4), (0x10,staged as u64,4), (0x14,manager,8), (0x1c,stamp as u64,4)] {
        put(out,off,value,size);
    }
    Ok(())
}

pub(crate) fn render_dependency(out: &mut [u8], stamp_address: u64,
    wait_slot: u8, wait_value: u32, stamp_self: u32) -> Result<(), BuildError> {
    if out.len() < 0x40 { return Err(BuildError::BufferTooSmall); }
    if !valid_fw(stamp_address) || wait_slot >= 128 || wait_value == 0 || stamp_self == 0 {
        return Err(BuildError::Address);
    }
    out[..0x40].fill(0);
    put(out, 0, 4, 4);
    put(out, 4, stamp_address, 8);
    put(out, 0xc, stamp_address, 8);
    put(out, 0x14, wait_value as u64, 4);
    put(out, 0x20, wait_slot as u64, 4);
    put(out, 0x24, stamp_self as u64, 4);
    Ok(())
}
