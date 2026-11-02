// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M5 tiling and fragment command encoders, with shared kick bindings (linux-aurora
//! t6050-gpu's g17/fw/m5/render.rs).

#![allow(dead_code)]

use super::super::kick::{KickTimestamp, RegisterArrayBinding, RegisterWrite};

use super::render_tail::{self as tail, RenderStage, RenderTail};

pub(crate) const M5_TA_COMMAND_BYTES: usize = 0x990;
pub(crate) const M5_3D_COMMAND_BYTES: usize = 0x2240;
pub(crate) const M5_TA_SELECTOR: u32 = 0;
pub(crate) const M5_3D_SELECTOR: u32 = 1;

pub(crate) const M5_TA_PROGRAM_OFFSET: u32 = 0x60;

pub(crate) const M5_3D_PROGRAM_OFFSETS: [u32; 4] = [0xa0, 0x7c0, 0xee0, 0x1600];

const PROGRAM_AREA: usize = 0x700;
const REGISTER_BYTES: usize = 12;
pub(crate) const M5_PROGRAM_CAPACITY: usize = PROGRAM_AREA / REGISTER_BYTES;

const TA_DISPATCH_REGISTER: u32 = 0x1c898;

const FRAGMENT_TILE_REGISTER: u32 = 0x10038;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum M5RenderCommandError {
    ProgramTooLong,
    ProgramOffset,
    RceCount,
    MissingDispatchMirror,
    AddressOverflow,
    InvalidCompletionState,
    InvalidUsageHint,
    Kick,
    Sksm,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct M5ProgramRef<'a> {
    pub registers: &'a [RegisterWrite],

    pub rce_count: u16,
    pub command_offset: u32,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct M5FragmentProgramRefs<'a> {
    pub main: M5ProgramRef<'a>,

    pub main_mid_count: u16,
    pub store: M5ProgramRef<'a>,
    pub resume: M5ProgramRef<'a>,
    pub load: M5ProgramRef<'a>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct M5StageCompletion {
    pub stamp_addresses: [u64; 2],

    pub stamp: u32,

    pub queue_index: u32,

    pub ordinal: u32,

    pub has_wait: bool,

    pub completion: u64,

    pub key: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct M5HardwareStamp {
    pub stamp: u32,

    pub entries: u32,

    pub generation: u64,

    pub base: u64,
}

pub(crate) fn m5_hardware_stamp(p: &M5HardwareStamp) -> Result<u64, M5RenderCommandError> {
    if p.entries == 0 {
        return Err(M5RenderCommandError::InvalidCompletionState);
    }
    let value = (u64::from(p.stamp >> 8) | p.generation << 24).wrapping_sub(1);
    let quotient = (value / u64::from(p.entries)) as u32;
    let remainder = (value as u32).wrapping_sub(quotient.wrapping_mul(p.entries));
    let slot = (p.base as u32 & 0xff) + (remainder & 0xff);
    let carry = slot / p.entries;
    let index = (slot - carry * p.entries) & 0xff;
    let phase = ((p.base >> 8) as u32)
        .wrapping_add(quotient)
        .wrapping_add(carry);
    Ok(u64::from(index) | u64::from(phase) << 8)
}

pub(crate) fn m5_render_completion(
    p: &M5HardwareStamp,
    pass: u8,
) -> Result<u64, M5RenderCommandError> {
    Ok(m5_hardware_stamp(p)? | u64::from(pass) << 40)
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct M5TaLinked3d {
    pub queue_index: u32,

    pub stamp: u32,

    pub flag: u8,

    pub completion: u64,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct M5TaPolicy {
    pub a6c0: u32,
    pub d15f8: u8,
    pub d1168: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct M5ExtraAddresses {
    pub count: u32,
    pub addresses: [u64; 4],

    pub policy: u8,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct M5TaCommandFields {
    pub sequence: u64,
    pub completion: M5StageCompletion,

    pub pb_slot: u32,

    pub pm_1d4: u32,

    pub pb_state: u64,

    pub pm_scene: u64,

    pub pm_b8: u64,
    pub d12a0: u32,
    pub d1290: u32,
    pub d1298: u64,
    pub d1448: u64,
    pub resolved_tail_value: u64,
    pub d1188: u64,
    pub d11a8: u64,
    pub linked_3d: M5TaLinked3d,
    pub d1038: u64,
    pub d1040: u64,
    pub d10c8: u64,
    pub d08a0: u8,
    pub d08a4: u32,

    pub d08a8: u32,
    pub d08ac: u32,
    pub d08b0: u64,

    pub diagnostic_ta_timestamp_upper: u64,
    pub d13e8: [u64; 2],

    pub timestamp_counts: [u32; 2],

    pub d1400: u64,
    /// Legacy seven-byte shifted hint within the TA descriptor value at 0x937.
    /// The following byte at 0x93f belongs to the resolved usage pointer.
    pub d0938: u64,

    pub uma_state: u64,

    pub d1444: u8,
    /// TA +0x892: at an overflow of the parameter buffer, the firmware asks the host for blocks
    /// (the type-6 growth request) instead of rendering partially.
    pub host_growth: u8,
    pub d1177: u8,
    pub policy: M5TaPolicy,
    pub extra: M5ExtraAddresses,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct M53dCommandFields {
    pub sequence: u64,
    pub completion: M5StageCompletion,

    pub pass: u8,
    pub d158: u64,
    pub dbc8: u64,
    pub resolved_tail_value: Option<u64>,
    pub d150: u32,

    pub d980_is_2: bool,

    pub d08cf: u8,

    pub dbc4: u8,

    pub pm_state: u64,

    pub pm_scene: u64,

    pub pm_b8: u64,
    pub d08d8: u64,
    pub d08f0: u64,
    pub d08f8: u32,
    pub d0900: u32,
    pub d0908: u32,
    pub d0910: [u64; 2],
    pub d0920: u64,
    pub d0930: u32,
    pub d04e0: u64,
    pub d04e8: u64,
    pub d04f0: u64,
    pub d0510: u64,

    pub d0518: [[u64; 2]; 23],
    pub d0690: u64,
    pub d0698: u64,
    pub d06a0: [u64; 2],
    pub d06b0: u64,
    pub d06b8: [u64; 2],
    pub d06d8: u64,
    pub d06f0: u64,
    pub d06f8: u64,
    pub d0708: u64,
    pub d0728: u64,
    pub d0730: u64,
    pub d0750: u64,
    pub d0758: u64,
    pub d0770: [u64; 2],
    pub d0780: [u64; 2],
    pub d0790: [u64; 2],
    pub d07b0: [u64; 2],
    pub d07c0: u64,
    pub d07c8: u32,
    pub d07cc: u64,
    pub d07d4: u32,
    pub d07d8: u64,
    pub d07e8: u64,
    pub d07f8: u32,
    pub d04d0: u32,
    pub d08c4: u8,
    pub dc68: u32,
    pub dc6c: u8,
    pub d08a0: u8,
    pub d08a4: u32,

    pub d08a8: u32,
    pub d08ac: u32,
    pub dad0: [u64; 2],

    pub timestamp_counts: [u32; 2],

    pub dae8: u64,

    pub uma_state: u64,

    pub d08b0: [u64; 2],

    pub d08c5_flags: [u8; 4],
    pub d08cd: u8,
    pub extra: M5ExtraAddresses,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct M5RenderCommandInputs {
    pub context_id: u32,

    pub scheduler_record: u64,
    pub ta: M5TaCommandFields,
    pub fragment: M53dCommandFields,
}

impl Default for M5RenderCommandInputs {
    fn default() -> Self {
        Self {
            context_id: 0,
            scheduler_record: 0,
            ta: M5TaCommandFields {
                d08ac: u32::MAX,
                ..Default::default()
            },
            fragment: M53dCommandFields {
                d08ac: u32::MAX,
                ..Default::default()
            },
        }
    }
}

fn put8(out: &mut [u8], offset: usize, value: u8) {
    out[offset] = value;
}

fn put16(out: &mut [u8], offset: usize, value: u16) {
    out[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put32(out: &mut [u8], offset: usize, value: u32) {
    out[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put64(out: &mut [u8], offset: usize, value: u64) {
    out[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn put128(out: &mut [u8], offset: usize, value: [u64; 2]) {
    put64(out, offset, value[0]);
    put64(out, offset + 8, value[1]);
}

fn read32(out: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(out[offset..offset + 4].try_into().unwrap())
}

fn completion_tail(out: &mut [u8], base: usize, c: &M5StageCompletion) {
    put64(out, base, c.stamp_addresses[0]);
    put64(out, base + 8, c.stamp_addresses[1]);
    put32(out, base + 0x10, c.stamp);
    put32(out, base + 0x14, c.queue_index);
    put32(out, base + 0x18, c.ordinal);
    put32(out, base + 0x1c, u32::from(c.has_wait));
}

/// Fill publication-time completion values without replacing the retained
/// resource, lifecycle or register-program fields of either command.
pub(crate) fn link_completions(
    ta: &mut [u8; M5_TA_COMMAND_BYTES],
    fragment: &mut [u8; M5_3D_COMMAND_BYTES],
    kicks: [KickTimestamp; 2],
    pass: u8,
    fragment_busy: bool,
) {
    put32(ta, 0x7a4, u32::from(fragment_busy));
    put64(ta, 0x7a8, kicks[1].completion_value(pass) & 0xff_ffff_ffff);
    put8(ta, 0x7b0, pass);
    put8(ta, 0x7b1, pass);
    put64(ta, 0x8c6, kicks[0].completion_value(pass));
    put8(fragment, 0x90, pass);
    put64(fragment, 0x2160, kicks[1].completion_value(pass));
}

/// Bind the command-state slot in each engine's completion tail.
pub(crate) fn bind_work_slot(
    ta: &mut [u8; M5_TA_COMMAND_BYTES],
    fragment: &mut [u8; M5_3D_COMMAND_BYTES],
    slot: u32,
) -> Result<(), M5RenderCommandError> {
    if slot >= 4 {
        return Err(M5RenderCommandError::InvalidCompletionState);
    }
    put32(ta, 0x8be, slot);
    put32(fragment, 0x2158, slot);
    Ok(())
}

fn extra_tail(out: &mut [u8], stage: RenderStage, e: &M5ExtraAddresses) {
    tail::encode_extra(
        out,
        stage,
        &tail::ExtraAddresses {
            count: e.count,
            policy: e.policy,
            addresses: e.addresses,
        },
    )
    .expect("fixed render command extent");
}

fn check_program(p: &M5ProgramRef<'_>, offset: u32) -> Result<(), M5RenderCommandError> {
    if p.command_offset != offset {
        return Err(M5RenderCommandError::ProgramOffset);
    }
    if p.registers.len() > M5_PROGRAM_CAPACITY {
        return Err(M5RenderCommandError::ProgramTooLong);
    }
    if usize::from(p.rce_count) > p.registers.len() || p.rce_count > 0x7f {
        return Err(M5RenderCommandError::RceCount);
    }
    Ok(())
}

fn place_program(
    out: &mut [u8],
    low_alias: u64,
    p: &M5ProgramRef<'_>,
) -> Result<u64, M5RenderCommandError> {
    let offset = p.command_offset as usize;
    let address = low_alias
        .checked_add(u64::from(p.command_offset))
        .ok_or(M5RenderCommandError::AddressOverflow)?;
    put16(out, offset + PROGRAM_AREA + 8, 0);
    put64(out, offset + PROGRAM_AREA, address);
    for (index, register) in p.registers.iter().enumerate() {
        let entry = offset + index * REGISTER_BYTES;
        let header = (read32(out, entry) & 0xfffc0006) | (register.reg & 0x3fff9);
        put32(out, entry, header);
        put64(out, entry + 4, register.value);
    }
    put16(out, offset + PROGRAM_AREA + 8, p.registers.len() as u16);
    Ok(address)
}

/// Per-command external-write metadata and the existing diagnostic.
pub(crate) const fn shader_write_class(declared: bool, diagnostic: u32) -> u8 {
    // The compiler's declared write class goes to d1177/command89a.
    // Preserve the existing explicit force-on diagnostic; do not infer policy89e.
    (declared || diagnostic == 1) as u8
}

/// The shifted hint at TA command +0x938 by programmed mesh class (S's default): the
/// apparent high-bit "pass rotation" in an unaligned u64 read at 0x938 comes from the usage
/// pointer at 0x93f; it is not a separate field.
pub(crate) fn ta_word_938(mesh_class: u8) -> u64 {
    match mesh_class {
        0 => 0xc0c0,
        1 => 0x10ac0,
        _ => 0x12b40,
    }
}

pub(crate) fn encode_m5_ta_command(
    out: &mut [u8; M5_TA_COMMAND_BYTES],
    low_alias: u64,
    i: &M5RenderCommandInputs,
    ta: &M5ProgramRef<'_>,
) -> Result<[Option<RegisterArrayBinding>; 4], M5RenderCommandError> {
    check_program(ta, M5_TA_PROGRAM_OFFSET)?;
    let dispatch = ta
        .registers
        .iter()
        .find(|r| r.reg & !1 == TA_DISPATCH_REGISTER)
        .ok_or(M5RenderCommandError::MissingDispatchMirror)?
        .value;
    low_alias
        .checked_add(u64::from(ta.command_offset))
        .ok_or(M5RenderCommandError::AddressOverflow)?;
    let t = &i.ta;
    let descriptor_value = tail::tiling_descriptor_value(t.d1448, t.d0938)
        .ok_or(M5RenderCommandError::InvalidUsageHint)?;
    put32(out, 0, M5_TA_SELECTOR);
    put64(out, 4, t.sequence);
    put32(out, 0xc, i.context_id);
    tail::encode(
        out,
        RenderStage::Tiling,
        &RenderTail {
            descriptor_value,
            resolved_value: Some(t.resolved_tail_value),
            trailing_value: t.d1298,
        },
    )
    .expect("fixed TA command extent");
    put32(out, 0x8d6, t.d1290);
    put32(out, 0x48, t.d12a0);
    put32(out, 0x18, t.pb_slot);
    put32(out, 0x1c, t.pm_1d4);
    put64(out, 0x20, t.pb_state);
    put64(out, 0x28, t.pm_scene);
    put64(out, 0x30, t.pm_b8);
    put64(out, 0x10, i.scheduler_record);
    put64(out, 0x780, t.d1188);
    put64(out, 0x788, t.d11a8);
    put32(out, 0x79c, t.linked_3d.queue_index);
    put32(out, 0x7a0, t.linked_3d.stamp);
    put32(out, 0x7a4, u32::from(t.linked_3d.flag & 1));
    put64(out, 0x7a8, t.linked_3d.completion & 0xff_ffff_ffff);
    let pass = (t.linked_3d.completion >> 40) as u8;
    put8(out, 0x7b1, pass);
    put8(out, 0x7b0, pass);
    put64(out, 0x7b6, t.d1038);
    put64(out, 0x7be, t.d1040);
    put64(out, 0x7d6, t.d10c8);
    put128(out, 0x7de, [0, 0]);
    put64(out, 0x7ee, 0);
    put32(out, 0x7b2, 0);
    put32(out, 0x86a, u32::from(t.d08a0 & 1));
    put32(out, 0x86e, t.d08a4);
    put32(out, 0x872, t.d08a8);
    put32(out, 0x876, t.d08ac);
    put128(out, 0x87a, [0, 0]);
    put128(out, 0x90e, t.d13e8);
    put32(out, 0x91e, t.timestamp_counts[0]);
    put32(out, 0x922, t.timestamp_counts[1]);
    put64(out, 0x926, t.d1400);
    put64(out, 0x92e, t.uma_state);
    put8(out, 0x936, t.d1444 & 1);
    put64(out, 0x8fe, t.d08b0);
    put64(out, 0x906, t.diagnostic_ta_timestamp_upper);
    put32(out, 0x892, u32::from(t.host_growth & 1));
    put32(out, 0x89a, u32::from(t.d1177 & 1));
    put32(out, 0x89e, u32::from(m5_ta_policy_bit(&t.policy, t.d1177)));
    put32(out, 0x8a2, 0);
    put64(out, 0x8f6, 0);
    completion_tail(out, 0x8a6, &t.completion);
    put64(out, 0x8c6, t.completion.completion);
    put64(out, 0x8ce, t.completion.key);

    put32(out, 0x38, 0);
    put16(out, 0x3c, 0);
    put32(out, 0x40, 0);
    let address = place_program(out, low_alias, ta)?;
    put16(out, 0x38, ta.rce_count);
    put16(out, 0x3a, ta.registers.len() as u16);
    put16(out, 0x3c, ta.registers.len() as u16);
    put64(out, 0x85e, dispatch);
    extra_tail(out, RenderStage::Tiling, &t.extra);
    let empty = None;
    Ok([
        Some(
            RegisterArrayBinding::new(address, ta.rce_count as u8)
                .map_err(|_| M5RenderCommandError::ProgramOffset)?,
        ),
        empty,
        empty,
        empty,
    ])
}

pub(crate) fn encode_m5_3d_command(
    out: &mut [u8; M5_3D_COMMAND_BYTES],
    low_alias: u64,
    i: &M5RenderCommandInputs,
    f: &M5FragmentProgramRefs<'_>,
) -> Result<[Option<RegisterArrayBinding>; 4], M5RenderCommandError> {
    let programs = [f.main, f.store, f.resume, f.load];
    for (program, offset) in programs.iter().zip(M5_3D_PROGRAM_OFFSETS) {
        check_program(program, offset)?;
        low_alias
            .checked_add(u64::from(program.command_offset))
            .ok_or(M5RenderCommandError::AddressOverflow)?;
    }
    for partial in &programs[1..] {
        if usize::from(partial.rce_count) != partial.registers.len() {
            return Err(M5RenderCommandError::RceCount);
        }
    }
    if f.main_mid_count < f.main.rce_count || usize::from(f.main_mid_count) > f.main.registers.len()
    {
        return Err(M5RenderCommandError::RceCount);
    }
    let tile = f
        .main
        .registers
        .iter()
        .find(|r| r.reg & !1 == FRAGMENT_TILE_REGISTER)
        .ok_or(M5RenderCommandError::MissingDispatchMirror)?
        .value;
    let t = &i.fragment;
    put32(out, 0, M5_3D_SELECTOR);
    put64(out, 4, t.sequence);
    put32(out, 0xc, i.context_id);
    tail::encode(
        out,
        RenderStage::Fragment,
        &RenderTail {
            descriptor_value: t.dbc8,
            resolved_value: t.resolved_tail_value,
            trailing_value: t.d158,
        },
    )
    .expect("fixed fragment command extent");
    put32(out, 0x2170, t.d150);
    put32(out, 0x2174, u32::from(t.d980_is_2));
    put32(out, 0x2100, u32::from(t.d08cf & 1));
    put32(out, 0x20f8, 0);

    put128(out, 0x2178, [0, 0]);
    put64(out, 0x2188, 0);
    put8(out, 0x21d0, t.dbc4 & 1);
    put64(out, 0x20, i.scheduler_record);
    put64(out, 0x28, t.pm_state);
    put64(out, 0x30, t.pm_scene);
    put64(out, 0x38, t.pm_b8);
    put32(out, 0x58, t.d08f8);
    put64(out, 0x40, t.d08d8);
    put64(out, 0x48, t.d08f0);
    put32(out, 0x50, t.d0900);
    put32(out, 0x54, t.d0908);
    put128(out, 0x60, t.d0910);
    put64(out, 0x70, t.d0920);
    put32(out, 0x78, t.d0930);
    put64(out, 0x1d30, t.d04e8);
    put64(out, 0x1d20, t.d04e0);
    put64(out, 0x1d40, t.d04f0);
    for (index, pair) in t.d0518.iter().enumerate() {
        put128(out, 0x1d58 + index * 16, *pair);
    }
    put64(out, 0x1ec8, t.d0698);
    put64(out, 0x1ed0, t.d06f8);
    put64(out, 0x1ed8, t.d0730);
    put64(out, 0x1f28, t.d06d8);
    put128(out, 0x1ee0, t.d06a0);
    put64(out, 0x1ef0, t.d0728);
    put64(out, 0x1ef8, t.d06b0);
    put64(out, 0x1f00, t.d0708);
    put64(out, 0x1f08, t.d0758);
    put64(out, 0x1f30, t.d06f0);
    put128(out, 0x1f10, t.d06b8);
    put64(out, 0x1f20, t.d0750);
    put64(out, 0x1f68, t.d07c0);
    put32(out, 0x1f70, t.d07c8);
    put128(out, 0x1f48, t.d0780);
    put128(out, 0x1f74, t.d0790);
    put64(out, 0x1f88, t.d07cc);
    put32(out, 0x1f90, t.d07d4);
    put128(out, 0x1f94, t.d07b0);
    put64(out, 0x1fa8, t.d07d8);
    put32(out, 0x1fb0, t.d04d0);
    put128(out, 0x1f38, t.d0770);
    put64(out, 0x1d50, t.d0510);
    put64(out, 0x1fc0, t.d07e8);
    put32(out, 0x1fc8, t.d07f8);
    put64(out, 0x1fd0, t.d0690);
    out[0x1fdc..0x20dc].fill(0);
    put32(out, 0x20dc, 0);
    put128(out, 0x20e0, [0, 0]);
    put32(out, 0x1fd8, 0);
    put32(out, 0x20f0, 0);
    put8(out, 0x90, t.pass);
    put32(out, 0x20fc, u32::from(t.d08c4 & 1));
    put32(out, 0x2209, t.dc68);
    put8(out, 0x220d, t.dc6c);
    put32(out, 0x2104, u32::from(t.d08a0 & 1));
    put32(out, 0x2108, t.d08a4);
    put32(out, 0x210c, t.d08a8);
    put32(out, 0x2110, t.d08ac);
    put128(out, 0x2114, [0, 0]);
    put128(out, 0x21a8, t.dad0);
    put32(out, 0x21b8, t.timestamp_counts[0]);
    put32(out, 0x21bc, t.timestamp_counts[1]);
    put64(out, 0x21c0, t.dae8);
    put64(out, 0x21c8, t.uma_state);
    put128(out, 0x2198, t.d08b0);
    completion_tail(out, 0x2140, &t.completion);
    put64(out, 0x2160, t.completion.completion);
    put64(out, 0x2190, 0);
    for (index, flag) in t.d08c5_flags.iter().enumerate() {
        put32(out, 0x2124 + index * 4, u32::from(flag & 1));
    }
    put32(out, 0x2138, u32::from(t.d08cd & 1));
    put64(out, 0x2168, t.completion.key);
    extra_tail(out, RenderStage::Fragment, &t.extra);

    put32(out, 0x80, 0);
    put16(out, 0x84, 0);
    put32(out, 0x88, 0);
    let mut rce = [None; 4];
    for (slot, program) in rce.iter_mut().zip(programs.iter()) {
        let address = place_program(out, low_alias, program)?;
        *slot = Some(
            RegisterArrayBinding::new(address, program.rce_count as u8)
                .map_err(|_| M5RenderCommandError::ProgramOffset)?,
        );
    }
    put64(out, 0x1f58, tile);
    put64(out, 0x1f60, 0);
    put16(out, 0x80, f.main.rce_count);
    put16(out, 0x82, f.main_mid_count);
    put16(out, 0x84, f.main_mid_count);
    put16(out, 0x88, f.main.registers.len() as u16);
    Ok(rce)
}

fn m5_ta_policy_bit(p: &M5TaPolicy, d1177: u8) -> bool {
    if p.a6c0 == 0 || !(1..=2).contains(&p.d15f8) || p.d1168 == 3 {
        return false;
    }
    match p.a6c0 {
        1 | 4 => d1177 & 1 == 0,
        3 => true,
        _ => false,
    }
}
