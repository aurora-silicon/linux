// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 (J873/T8152, 26A428 HAL400) TA and 3D firmware commands, the M6
//! channel configuration, SKSM kick-entry operands and tag14 kicks.
//!
//! Every byte follows the original AGXTAChannelSKSM::submitBuffer
//! (0xfffffe0008c837ac) and AGX3DChannelSKSM::submitBuffer (0xfffffe0008bbb500)
//! field writers, the register-list placement of the HAL400 generators
//! (TAChannelSKSM::generateRegisterList 0xfffffe0008c4e1b4,
//! 3DChannelSKSM::generateRegisterListFor3D 0xfffffe0008c3abe8) and the
//! KSMKickQueue TA/3D wrappers (0xfffffe0008c4a9e4 / 0xfffffe0008c4a794).
//! Parity is established by a native render-command audit,
//! which executes those functions unmodified.
//!
//! These are pure serializers. They never compute register values (the
//! programs come from g18_render), never allocate, map or publish, and never
//! choose an address: every kernel-owned address is an input that the job
//! must own and have mapped in the context that dereferences it. Native field
//! names are kept as descriptor offsets (`dNNNN` = native host descriptor
//! +0xNNNN) wherever a guessed meaning would be unproven.
//!
//! Command selectors (first u32 of every inner-ring command): TA 0, 3D 1,
//! 3D fast blit 2 (AGFIChannelCommandFastBlit, 0x9e0 bytes, emitted only when
//! 3D descriptor +0x840 bit 0 is set; not encoded here), compute 3,
//! tag15 channel configuration 15, tag14 add-kicks 14.

#![allow(dead_code)]

use crate::g18::fw::render_params::RegisterWrite;
use crate::g18::fw::sksm::{self as m5_sksm, M5SksmDependency, M5SksmOperands, M5SksmQueue, M5SksmRce};

pub(crate) const G18_TA_COMMAND_BYTES: usize = 0x980;
pub(crate) const G18_3D_COMMAND_BYTES: usize = 0x2240;
/// Selector-2 3D fast-blit form. Recorded for completeness; never emitted.
pub(crate) const G18_FAST_BLIT_COMMAND_BYTES: usize = 0x9e0;
pub(crate) const G18_CHANNEL_CONFIG_BYTES: usize = 0x98;
pub(crate) const G18_M5_CHANNEL_CONFIG_BYTES: usize = 0x86;
pub(crate) const G18_KICK_BYTES: usize = 21;

pub(crate) const G18_TA_SELECTOR: u32 = 0;
pub(crate) const G18_3D_SELECTOR: u32 = 1;
pub(crate) const G18_FAST_BLIT_SELECTOR: u32 = 2;
pub(crate) const G18_CONFIG_SELECTOR: u32 = 15;

/// TA register program: one list at command +0x60.
pub(crate) const G18_TA_PROGRAM_OFFSET: u32 = 0x60;
/// 3D main, partial store, partial resume and partial load programs.
pub(crate) const G18_3D_PROGRAM_OFFSETS: [u32; 4] = [0xa0, 0x7c0, 0xee0, 0x1600];
/// Each list slot: 0x700 bytes of 12-byte entries, then the GPU address of the
/// list (+0x700, u64) and its entry count (+0x708, u16).
const PROGRAM_AREA: usize = 0x700;
const REGISTER_BYTES: usize = 12;
pub(crate) const G18_PROGRAM_CAPACITY: usize = PROGRAM_AREA / REGISTER_BYTES;
/// The TA generator mirrors the first 0x1c898 value into command +0x85e and
/// later emits 0x1c899 with bit 0 set (0xfffffe0008c4f690 / 0x8c4fff0).
const TA_DISPATCH_REGISTER: u32 = 0x1c898;
/// generateRegisterListFor3D mirrors its 0x10038 value (appended as the
/// 64-bit entry 0x10039) into command +0x1f58 (0x8c3bfe8), then clears +0x1f60
/// (0x8c3c0d0). Register numbers are matched without the width bit 0.
const FRAGMENT_TILE_REGISTER: u32 = 0x10038;

/// Native KSMKickQueue QoS class table 0xfffffe00071524d8, index 3/4/5 for
/// TA/3D/CL when the scheduler has no override (+0xc68 == 0).
pub(crate) const G18_TA_QOS_CLASS: u8 = 12;
pub(crate) const G18_3D_QOS_CLASS: u8 = 24;
/// encodeTAKickEntry / encode3DKickEntry dependency-type argument.
pub(crate) const G18_TA_DEPENDENCY_TYPE: u8 = 2;
pub(crate) const G18_3D_DEPENDENCY_TYPE: u8 = 3;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum G18RenderCommandError {
    ProgramTooLong,
    ProgramOffset,
    RceCount,
    MissingDispatchMirror,
    AddressOverflow,
    InvalidCompletionState,
    Kick,
    Sksm,
}

/// A borrowed register program, the slice form of g18_render's
/// `G18Program<N>` (registers, native RCE metadata count, list offset).
#[derive(Clone, Copy, Debug)]
pub(crate) struct G18ProgramRef<'a> {
    pub registers: &'a [RegisterWrite],
    /// Native generator metadata slot count, used as the SKSM RCE tag and the
    /// command's first boundary (TA: N-3; 3D main: N-4).
    pub rce_count: u16,
    pub command_offset: u32,
}

/// The four 3D programs. Native always encodes all four.
#[derive(Clone, Copy, Debug)]
pub(crate) struct G18FragmentProgramRefs<'a> {
    /// `rce_count` is the +0x80 boundary (native N-4).
    pub main: G18ProgramRef<'a>,
    /// Second main boundary, +0x82 and +0x84 (native N-2, after 0x16020).
    pub main_mid_count: u16,
    pub store: G18ProgramRef<'a>,
    pub resume: G18ProgramRef<'a>,
    pub load: G18ProgramRef<'a>,
}

/// Native per-stage completion identity copied into the command tail.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18StageCompletion {
    /// Firmware-written stamp words: stamp-memory A/B base plus
    /// IOGPUEventMachine::getStampOffset(queue) = queue * 4.
    pub stamp_addresses: [u64; 2],
    /// Event-machine stamp of this submission (vendevtCommandRec +0).
    pub stamp: u32,
    /// Kick-queue index (vendevtCommandRec +4; the qid on Linux).
    pub queue_index: u32,
    /// Channel +0x38.
    pub ordinal: u32,
    /// Host descriptor +0x48 nonzero.
    pub has_wait: bool,
    /// Hardware completion word, [`g18_render_completion`]. M5's
    /// `(1 << 40) | generation` is this value for pass 1, stamp 0x100.
    pub completion: u64,
    /// Submission key: TA descriptor +0xe68 / 3D descriptor +0x100. Also
    /// the key argument of the command-pointer publication.
    pub key: u64,
}

/// Hardware-stamp conversion shared by completion words and barrier stamps
/// (HAL400 convertEventMachineTSToHWTS arithmetic; identical to compute).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18HardwareStamp {
    pub stamp: u32,
    /// Kick-queue entry count (+0x10).
    pub entries: u32,
    /// Event-machine generation of the stamp's queue, half selected by the
    /// stamp's sign (+8 for stamp < 0x80000000, else +16).
    pub generation: u64,
    /// Kick-queue ring base (+0x90).
    pub base: u64,
}

pub(crate) fn g18_hardware_stamp(p: &G18HardwareStamp) -> Result<u64, G18RenderCommandError> {
    if p.entries == 0 {
        return Err(G18RenderCommandError::InvalidCompletionState);
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

/// Completion word: hardware stamp | completion scratch payload << 40. The
/// payload is the 3D channel's per-pass counter (+0x1d4, incremented by the
/// 3D submit); the TA of the same pass carries the same value.
pub(crate) fn g18_render_completion(
    p: &G18HardwareStamp,
    pass: u8,
) -> Result<u64, G18RenderCommandError> {
    Ok(g18_hardware_stamp(p)? | u64::from(pass) << 40)
}

/// TA fields for the TA's own linked 3D submission (AGX3DWorkQueue::
/// submitCommand 0x8c93938..0x8c93a50 writes them after the 3D submit).
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18TaLinked3d {
    /// 3D channel +0x18 (its queue index) -> TA +0x79c.
    pub queue_index: u32,
    /// 3D channel +0x1d0 (the 3D's event stamp) -> TA +0x7a0.
    pub stamp: u32,
    /// TA descriptor +0x1580 bit 0 -> TA +0x7a4.
    pub flag: u8,
    /// 3D completion word -> TA +0x7a8 (40 bits) and +0x7b0/+0x7b1 (pass).
    pub completion: u64,
}

/// Native TA +0x89e policy bit: accelerator +0x6c0 against descriptor
/// +0x1658/+0x1198/+0x11a7 (0x8c83f6c..0x8c83fa4, 0x8c84448..0x8c8446c).
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18TaPolicy {
    pub a6c0: u32,
    pub d1658: u8,
    pub d1198: u32,
}

/// Optional four-address tail (+0x95f..+0x97f), as compute's +0xfe5.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18ExtraAddresses {
    /// Descriptor count word (TA +0x13d8, 3D +0xa90).
    pub count: u32,
    pub addresses: [u64; 4],
    /// Accelerator +0x116b8 bits 0..1.
    pub policy: u8,
}

/// Every TA command field outside the register program.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18TaCommandFields {
    /// AGXCommandQueue +0x628 before its increment.
    pub sequence: u64,
    pub completion: G18StageCompletion,
    /// TA HardwareBufferID (PB descriptor-table slot), d1280.
    pub pb_slot: u32,
    /// Parameter management +0x1d4, d12a8.
    pub pm_1d4: u32,
    /// PB state object (firmware VA), d1288. Same value as tag15 +0x7a.
    pub pb_state: u64,
    /// Parameter management +0xa8[+0x2c], d1290.
    pub pm_scene: u64,
    /// Parameter management +0xb8, d12a0.
    pub pm_b8: u64,
    pub d12d0: u32,
    pub d12c0: u32,
    pub d12c8: u64,
    pub d11b8: u64,
    pub d11d8: u64,
    pub linked_3d: G18TaLinked3d,
    pub d1068: u64,
    pub d1070: u64,
    pub d10f8: u64,
    pub d08a0: u8,
    pub d08a4: u32,
    pub d08a8: u32,
    pub d08ac: u32,
    pub d08b0: u64,
    pub d1418: [u64; 2],
    pub d1428: u64,
    pub d1430: u64,
    /// UMA pool state firmware VA (UMA data record +0x20), d1458.
    pub uma_state: u64,
    pub d1474: u8,
    pub d11a4: u8,
    pub d11a7: u8,
    pub policy: G18TaPolicy,
    pub extra: G18ExtraAddresses,
}

/// Every 3D command field outside the four register programs.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G183dCommandFields {
    pub sequence: u64,
    pub completion: G18StageCompletion,
    /// Completion scratch payload (3D channel +0x1d4 after this submit) -> +0x90.
    pub pass: u8,
    pub d158: u64,
    pub d150: u32,
    /// Descriptor +0x980 == 2 -> +0x2174.
    pub d980_is_2: bool,
    pub d08cf: u8,
    pub dbbc: u8,
    /// 3D side of the shared PM state (firmware VA of d940) -> +0x28.
    pub pm_state: u64,
    /// Parameter management +0xa8[+0x2c], d948 -> +0x30.
    pub pm_scene: u64,
    /// Parameter management +0xb8, d958 -> +0x38.
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
    /// d518..d688 as 23 16-byte pairs, copied to +0x1d58..+0x1ec8.
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
    pub dc98: u32,
    pub dc9c: u8,
    pub d08a0: u8,
    pub d08a4: u32,
    pub d08a8: u32,
    pub d08ac: u32,
    pub dad0: [u64; 2],
    pub dae0: u64,
    pub dae8: u64,
    /// UMA pool state firmware VA, dba0.
    pub uma_state: u64,
    /// Timestamp pair, d8b0/d8b8 -> +0x2198/+0x21a0.
    pub d08b0: [u64; 2],
    /// d8c5, d8c6, d8c7, d8cb bit 0 -> +0x2124..+0x2130.
    pub d08c5_flags: [u8; 4],
    pub d08cd: u8,
    pub extra: G18ExtraAddresses,
}

/// Inputs shared by both commands of a render pass (contract §6.2).
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18RenderCommandInputs {
    /// TA descriptor +0x12c4 / 3D descriptor +0x154 (the GPU context ID).
    pub context_id: u32,
    /// Descriptor +0x198: the 0xc8-byte scheduler record (TA +0x10, 3D +0x20).
    pub scheduler_record: u64,
    pub ta: G18TaCommandFields,
    pub fragment: G183dCommandFields,
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

fn completion_tail(out: &mut [u8], base: usize, c: &G18StageCompletion) {
    put64(out, base, c.stamp_addresses[0]);
    put64(out, base + 8, c.stamp_addresses[1]);
    put32(out, base + 0x10, c.stamp);
    put32(out, base + 0x14, c.queue_index);
    put32(out, base + 0x18, c.ordinal);
    put32(out, base + 0x1c, u32::from(c.has_wait));
}

fn extra_tail(out: &mut [u8], flags: usize, addresses: usize, e: &G18ExtraAddresses) {
    out[flags] &= 0xf2;
    if e.count != 0 || e.addresses.iter().any(|&value| value != 0) {
        out[flags] |= 1 | ((e.policy & 3) << 2);
        for (index, value) in e.addresses.iter().enumerate() {
            put64(out, addresses + index * 8, *value);
        }
    }
}

/// Validate one program slot before any byte is written.
fn check_program(p: &G18ProgramRef<'_>, offset: u32) -> Result<(), G18RenderCommandError> {
    if p.command_offset != offset {
        return Err(G18RenderCommandError::ProgramOffset);
    }
    if p.registers.len() > G18_PROGRAM_CAPACITY {
        return Err(G18RenderCommandError::ProgramTooLong);
    }
    if usize::from(p.rce_count) > p.registers.len() || p.rce_count > 0x7f {
        return Err(G18RenderCommandError::RceCount);
    }
    Ok(())
}

/// Native RCEBufferEncoder entry: the header keeps the destination word's
/// bits 0xfffc0006, then the 64-bit value follows unaligned at +4.
fn place_program(out: &mut [u8], low_alias: u64, p: &G18ProgramRef<'_>) -> Result<u64, G18RenderCommandError> {
    let offset = p.command_offset as usize;
    let address = low_alias
        .checked_add(u64::from(p.command_offset))
        .ok_or(G18RenderCommandError::AddressOverflow)?;
    put16(out, offset + PROGRAM_AREA + 8, 0);
    put64(out, offset + PROGRAM_AREA, address);
    for (index, register) in p.registers.iter().enumerate() {
        let entry = offset + index * REGISTER_BYTES;
        let header = (read32(out, entry) & 0xfffc0006) | (register.number & 0x3fff9);
        put32(out, entry, header);
        put64(out, entry + 4, register.value);
    }
    put16(out, offset + PROGRAM_AREA + 8, p.registers.len() as u16);
    Ok(address)
}

/// Encode the TA firmware command (selector 0) and return the SKSM RCE list.
///
/// `out` must hold the command allocation's current contents: like native,
/// untouched bytes and header bits 0xfffc0006 of each list entry are kept.
/// `low_alias` is the command's GPU address in the job context (native
/// descriptor +0x1008); the RCE list and +0x760 name it.
pub(crate) fn encode_g18_ta_command(
    out: &mut [u8; G18_TA_COMMAND_BYTES],
    low_alias: u64,
    i: &G18RenderCommandInputs,
    ta: &G18ProgramRef<'_>,
) -> Result<[M5SksmRce; 4], G18RenderCommandError> {
    check_program(ta, G18_TA_PROGRAM_OFFSET)?;
    let dispatch = ta
        .registers
        .iter()
        .find(|r| r.number & !1 == TA_DISPATCH_REGISTER)
        .ok_or(G18RenderCommandError::MissingDispatchMirror)?
        .value;
    let t = &i.ta;
    put32(out, 0, G18_TA_SELECTOR);
    put64(out, 4, t.sequence);
    put32(out, 0xc, i.context_id);
    put64(out, 0x937, t.d12c8);
    put32(out, 0x8d6, t.d12c0);
    put32(out, 0x48, t.d12d0);
    put32(out, 0x18, t.pb_slot);
    put32(out, 0x1c, t.pm_1d4);
    put64(out, 0x20, t.pb_state);
    put64(out, 0x28, t.pm_scene);
    put64(out, 0x30, t.pm_b8);
    put64(out, 0x10, i.scheduler_record);
    put64(out, 0x780, t.d11b8);
    put64(out, 0x788, t.d11d8);
    put32(out, 0x79c, t.linked_3d.queue_index);
    put32(out, 0x7a0, t.linked_3d.stamp);
    put32(out, 0x7a4, u32::from(t.linked_3d.flag & 1));
    put64(out, 0x7a8, t.linked_3d.completion & 0xff_ffff_ffff);
    let pass = (t.linked_3d.completion >> 40) as u8;
    put8(out, 0x7b1, pass);
    put8(out, 0x7b0, pass);
    put64(out, 0x7b6, t.d1068);
    put64(out, 0x7be, t.d1070);
    put64(out, 0x7d6, t.d10f8);
    put128(out, 0x7de, [0, 0]);
    put64(out, 0x7ee, 0);
    put32(out, 0x7b2, 0);
    put32(out, 0x86a, u32::from(t.d08a0 & 1));
    put32(out, 0x86e, t.d08a4);
    put32(out, 0x872, t.d08a8);
    put32(out, 0x876, t.d08ac);
    put128(out, 0x87a, [0, 0]);
    put128(out, 0x90e, t.d1418);
    put64(out, 0x91e, t.d1428);
    put64(out, 0x926, t.d1430);
    put64(out, 0x92e, t.uma_state);
    put8(out, 0x936, t.d1474 & 1);
    put64(out, 0x8fe, t.d08b0);
    put32(out, 0x892, u32::from(t.d11a4 & 1));
    put32(out, 0x89a, u32::from(t.d11a7 & 1));
    put32(out, 0x89e, u32::from(g18_ta_policy_bit(&t.policy, t.d11a7)));
    put32(out, 0x8a2, 0);
    put64(out, 0x8f6, 0);
    completion_tail(out, 0x8a6, &t.completion);
    put64(out, 0x8c6, t.completion.completion);
    put64(out, 0x8ce, t.completion.key);
    // Register generator placement (zeroing first, as 0x8c4fd28..0x8c4fd30).
    put32(out, 0x38, 0);
    put16(out, 0x3c, 0);
    put32(out, 0x40, 0);
    let address = place_program(out, low_alias, ta)?;
    put16(out, 0x38, ta.rce_count);
    put16(out, 0x3a, ta.registers.len() as u16);
    put16(out, 0x3c, ta.registers.len() as u16);
    put64(out, 0x85e, dispatch);
    extra_tail(out, 0x97f, 0x95f, &t.extra);
    let empty = M5SksmRce { address: 0, tag: 0 };
    Ok([M5SksmRce { address, tag: ta.rce_count as u8 }, empty, empty, empty])
}

/// Encode the 3D firmware command (selector 1) with all four programs and
/// return the SKSM RCE list (main, store, resume, load).
///
/// `low_alias` is the command's GPU address in the job context (native
/// descriptor +0x470). Untouched bytes of `out` are kept, as native does.
pub(crate) fn encode_g18_3d_command(
    out: &mut [u8; G18_3D_COMMAND_BYTES],
    low_alias: u64,
    i: &G18RenderCommandInputs,
    f: &G18FragmentProgramRefs<'_>,
) -> Result<[M5SksmRce; 4], G18RenderCommandError> {
    let programs = [f.main, f.store, f.resume, f.load];
    for (program, offset) in programs.iter().zip(G18_3D_PROGRAM_OFFSETS) {
        check_program(program, offset)?;
    }
    for partial in &programs[1..] {
        if usize::from(partial.rce_count) != partial.registers.len() {
            return Err(G18RenderCommandError::RceCount);
        }
    }
    if f.main_mid_count < f.main.rce_count || usize::from(f.main_mid_count) > f.main.registers.len() {
        return Err(G18RenderCommandError::RceCount);
    }
    let tile = f
        .main
        .registers
        .iter()
        .find(|r| r.number & !1 == FRAGMENT_TILE_REGISTER)
        .ok_or(G18RenderCommandError::MissingDispatchMirror)?
        .value;
    let t = &i.fragment;
    put32(out, 0, G18_3D_SELECTOR);
    put64(out, 4, t.sequence);
    put32(out, 0xc, i.context_id);
    put64(out, 0x21d1, t.d158);
    put32(out, 0x2170, t.d150);
    put32(out, 0x2174, u32::from(t.d980_is_2));
    put32(out, 0x2100, u32::from(t.d08cf & 1));
    put32(out, 0x20f8, 0);
    // generateFrgBarrierRegister is a G18 stub (0x8c39224): 24 zero bytes.
    put128(out, 0x2178, [0, 0]);
    put64(out, 0x2188, 0);
    put8(out, 0x21d0, t.dbbc & 1);
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
    put32(out, 0x21f9, t.dc98);
    put8(out, 0x21fd, t.dc9c);
    put32(out, 0x2104, u32::from(t.d08a0 & 1));
    put32(out, 0x2108, t.d08a4);
    put32(out, 0x210c, t.d08a8);
    put32(out, 0x2110, t.d08ac);
    put128(out, 0x2114, [0, 0]);
    put128(out, 0x21a8, t.dad0);
    put64(out, 0x21b8, t.dae0);
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
    extra_tail(out, 0x221e, 0x21fe, &t.extra);
    put32(out, 0x221f, 0);
    // generateRegisterListFor3D placement (zeroing at 0x8c3c7e4..0x8c3c7ec).
    put32(out, 0x80, 0);
    put16(out, 0x84, 0);
    put32(out, 0x88, 0);
    let mut rce = [M5SksmRce { address: 0, tag: 0 }; 4];
    for (slot, program) in rce.iter_mut().zip(programs.iter()) {
        let address = place_program(out, low_alias, program)?;
        *slot = M5SksmRce { address, tag: program.rce_count as u8 };
    }
    put64(out, 0x1f58, tile);
    put64(out, 0x1f60, 0);
    put16(out, 0x80, f.main.rce_count);
    put16(out, 0x82, f.main_mid_count);
    put16(out, 0x84, f.main_mid_count);
    put16(out, 0x88, f.main.registers.len() as u16);
    Ok(rce)
}

fn g18_ta_policy_bit(p: &G18TaPolicy, d11a7: u8) -> bool {
    if p.a6c0 == 0 || !(1..=2).contains(&p.d1658) || p.d1198 == 3 {
        return false;
    }
    match p.a6c0 {
        1 | 4 => d11a7 & 1 == 0,
        3 => true,
        _ => false,
    }
}

// ---- M6 tag15 channel configuration ----------------------------------------

/// Channel-configuration layout. M6 inserts a u32 flag at +0x6e and a u64 at
/// +0x72, moving the parameter-buffer tuple from +0x6e..+0x86 to +0x7a..+0x92.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ChannelForm {
    /// T8142/25G76, 0x86 bytes (m5_channel.rs).
    M5,
    /// T8152/26A428, 0x98 bytes (GFX b000 tag15 handler 0x12824 reads 0x98).
    M6,
}

impl ChannelForm {
    pub(crate) const fn bytes(self) -> usize {
        match self {
            ChannelForm::M5 => G18_M5_CHANNEL_CONFIG_BYTES,
            ChannelForm::M6 => G18_CHANNEL_CONFIG_BYTES,
        }
    }
}

/// UMA pool tuple (+0x36 state, +0x3e generation, +0x46 slot).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18ConfigUma {
    pub state: u64,
    /// FList +0x1c0 when the descriptor asks for it, else None (u64::MAX).
    pub generation: Option<u64>,
    pub slot: u32,
}

/// Parameter-buffer tuple. Firmware b000 0x13708..0x137c0 caches 0x90
/// bytes of `state` into its slot table (0x71420 + slot * 0x50) when the
/// generation changes, stores the GPU context at state +0x08 and sets the
/// queue/manager bits. Native TA writer 0x8c83a34..0x8c83a9c.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18ConfigParameterBuffer {
    /// PB state object firmware VA (TA descriptor +0x1288).
    pub state: u64,
    /// Descriptor +0xdc8 object's +0x68 (PB generation).
    pub generation: u64,
    /// TA HardwareBufferID / PB table slot (+0x1280).
    pub slot: u32,
    /// TA descriptor +0x1578: the linked 3D queue index.
    pub manager: u32,
}

/// All native tag15 fields. Offsets are for the M6 form.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct G18ChannelConfig {
    /// Kick queue +0x80/+0x88: SKSM entry ring low and firmware addresses.
    pub ring_low: u64,
    pub ring_high: u64,
    pub queue_id: u16,
    /// +0x1a: install the queue (TA d+0xe9d, 3D d+0x191, compute d+0x181).
    pub install: bool,
    /// Kick queue +0x1c.
    pub priority: u32,
    /// +0x22: stage word, TA 0 / 3D 1 / compute 2. Copied to the firmware
    /// queue slot +0x20 (b000 0x12240/0x1241c).
    pub data_master: u32,
    /// +0x26 flag and +0x2a value (TA d+0xeaa/+0xeb0, 3D d+0x308/+0x310).
    /// Native stores the value even when the flag is clear.
    pub state_update_flag: bool,
    pub state_update: u64,
    pub context_id: u32,
    pub uma: G18ConfigUma,
    /// AGXCommandQueue +0x618 -> +0xe0: the scheduler control record.
    pub channel_control: u64,
    pub update_scheduler: bool,
    /// AGXCommandQueue +0x868 -> +0x10.
    pub scheduler_index: u32,
    /// Channel +0x58 -> +0x44 (AGXChannel::getPid).
    pub pid: u32,
    /// Channel +0x58 -> +0x3c (AGXChannel::getSubPriority). Compute uses 2.
    pub sub_priority: u32,
    pub update_context: bool,
    /// Descriptor +0x58 gart's +0x2b8 bit 0 (b000 sets/clears context bit).
    pub gart_flag: bool,
    /// M6 +0x6e flag and +0x72 value (TA d+0xe9f/+0xea0, 3D d+0x2fa/
    /// +0x300); the value is stored regardless of the flag. Absent on M5.
    pub extension_flag: bool,
    pub extension: u64,
    pub parameter_buffer: Option<G18ConfigParameterBuffer>,
}

/// Encode a tag15 record. Native writes bytes 0..form.bytes() except the
/// trailing M6 padding +0x92..+0x98, which is left to the caller's buffer.
pub(crate) fn encode_channel_config(
    form: ChannelForm,
    p: &G18ChannelConfig,
    out: &mut [u8],
) -> Result<usize, G18RenderCommandError> {
    let bytes = form.bytes();
    if out.len() < bytes {
        return Err(G18RenderCommandError::AddressOverflow);
    }
    put64(out, 0, u64::from(G18_CONFIG_SELECTOR));
    put64(out, 8, p.ring_low);
    put64(out, 0x10, p.ring_high);
    put16(out, 0x18, p.queue_id);
    put32(out, 0x1a, u32::from(p.install));
    put32(out, 0x1e, p.priority);
    put32(out, 0x22, p.data_master);
    put32(out, 0x26, u32::from(p.state_update_flag));
    put64(out, 0x2a, p.state_update);
    put32(out, 0x32, p.context_id);
    put64(out, 0x36, p.uma.state);
    put64(out, 0x3e, p.uma.generation.unwrap_or(u64::MAX));
    put32(out, 0x46, p.uma.slot);
    put64(out, 0x4a, p.channel_control);
    put32(out, 0x52, u32::from(p.update_scheduler));
    put32(out, 0x56, p.scheduler_index);
    put32(out, 0x5a, p.pid);
    put32(out, 0x5e, p.sub_priority);
    put32(out, 0x62, u32::from(p.update_context));
    put32(out, 0x66, u32::from(p.gart_flag));
    put32(out, 0x6a, 0);
    let pb = match form {
        ChannelForm::M6 => {
            put32(out, 0x6e, u32::from(p.extension_flag));
            put64(out, 0x72, p.extension);
            0x7a
        }
        ChannelForm::M5 => 0x6e,
    };
    match p.parameter_buffer {
        Some(b) => {
            put64(out, pb, b.state);
            put64(out, pb + 8, b.generation);
            put32(out, pb + 0x10, b.slot);
            put32(out, pb + 0x14, b.manager);
        }
        None => {
            put64(out, pb, 0);
            put64(out, pb + 8, u64::MAX);
            put64(out, pb + 0x10, u64::MAX);
        }
    }
    Ok(bytes)
}

// ---- SKSM kick entry and tag14 kick -----------------------------------------

/// Graphics stage of a kick queue.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum G18RenderStage {
    Ta,
    ThreeD,
}

impl G18RenderStage {
    const fn dependency_type(self) -> u8 {
        match self {
            G18RenderStage::Ta => G18_TA_DEPENDENCY_TYPE,
            G18RenderStage::ThreeD => G18_3D_DEPENDENCY_TYPE,
        }
    }

    pub(crate) const fn qos_class(self) -> u8 {
        match self {
            G18RenderStage::Ta => G18_TA_QOS_CLASS,
            G18RenderStage::ThreeD => G18_3D_QOS_CLASS,
        }
    }

    const fn data_master(self) -> m5_sksm::M5DataMaster {
        match self {
            G18RenderStage::Ta => m5_sksm::M5DataMaster::Tiling,
            G18RenderStage::ThreeD => m5_sksm::M5DataMaster::Fragment,
        }
    }
}

/// One vendevtBarrierRec entry, before hardware-stamp conversion.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct G18Barrier {
    /// Event-machine stamp of the producer (e.g. the TA's 0x100).
    pub stamp: u32,
    /// Producer's queue index; also its qid on Linux.
    pub queue_index: u8,
    /// Barrier record flag word: 1 selects the stage dependency type.
    pub flag: u32,
    /// Event-machine generation of the producer queue for this stamp.
    pub generation: u64,
}

/// AGX3DChannelSKSM::submitBuffer 0x8bbba14..0x8bbba80: move the first
/// barrier on the paired TA queue (3D descriptor +0xe60; 0x80 for fast blit)
/// to the front of the list, swapping it with entry 0.
pub(crate) fn g18_order_3d_barriers(barriers: &mut [G18Barrier], ta_queue_index: u32) {
    if let Some(index) = barriers.iter().position(|b| u32::from(b.queue_index) == ta_queue_index) {
        barriers.swap(0, index);
    }
}

/// Inputs of encodeTAKickEntry / encode3DKickEntry beyond the kick queue.
#[derive(Clone, Copy, Debug)]
pub(crate) struct G18KickEntryInputs<'a> {
    pub stage: G18RenderStage,
    /// Kick queue id/priority/current stamp/previous stamp (+0xc/+0x1c/
    /// +0x48/+0x98/+0xa0); `submitted` counts entries in flight.
    pub queue: M5SksmQueue,
    /// Kick queue +0x10 entries and +0x90 base (barrier conversion).
    pub entries: u32,
    pub base: u64,
    /// This submission's event stamp and its queue's event generation, for
    /// the implicit predecessor when the queue has no previous stamp.
    pub stamp: u32,
    pub generation: u64,
    pub barriers: &'a [G18Barrier],
    /// TA descriptor +0xfdc / 3D +0x444 (AGXSPipelineDependencyType).
    pub pipeline_dependency: u32,
    /// AGXCommandQueue +0x868 -> +0x10 (QoS byte 32).
    pub scheduler_index: u8,
    /// Per-stage QoS word (0xffff without a scheduler override).
    pub qos_word: u16,
    /// Command firmware VA and the channel's +0x80 queue address.
    pub payload: [u64; 2],
    pub rce: [M5SksmRce; 4],
    /// Descriptor +0x4c bit 0 (TA) or (3D +0xc9c == 3 ? 1 : +0x4c) -> bit 1.
    pub flag_bit1: bool,
    /// TA +0x15f0 / 3D +0xb78 -> bit 61; TA +0x15f1 / 3D +0x838 -> bit 63.
    pub flag_bit61: bool,
    pub flag_bit63: bool,
    /// Event flag word 3: bit 63 config +0xe20, bit 62 extra command
    /// present, bit 61 accelerator +0x6b6 & 0xc0.
    pub event_flags: u64,
    /// Descriptor +0x178, or accelerator +0x11f28[index & 0x7f].
    pub auxiliary: u64,
}

/// Build the HAL400 kick-entry operands for m5_sksm::prepare_m5_sksm_entry.
/// The dependency list follows encodeDependencyList 0x8c4a5a4.
pub(crate) fn g18_kick_entry_operands<'a>(
    p: &G18KickEntryInputs<'_>,
    barriers: &'a mut [M5SksmDependency],
) -> Result<(M5SksmOperands<'a>, u64), G18RenderCommandError> {
    if barriers.len() < p.barriers.len() || p.barriers.len() >= 32 {
        return Err(G18RenderCommandError::Sksm);
    }
    let kind = |flag: bool| if flag { p.stage.dependency_type() } else { 1 };
    for (out, b) in barriers.iter_mut().zip(p.barriers.iter()) {
        let stamp = g18_hardware_stamp(&G18HardwareStamp {
            stamp: b.stamp, entries: p.entries, generation: b.generation, base: p.base,
        })?;
        *out = M5SksmDependency { queue_id: b.queue_index & 0x7f, stamp, kind: kind(b.flag == 1) };
    }
    let first = g18_hardware_stamp(&G18HardwareStamp {
        stamp: p.stamp, entries: p.entries, generation: p.generation, base: p.base,
    })?
    .wrapping_sub(1);
    let count = p.barriers.len();
    Ok((M5SksmOperands {
        barriers: &barriers[..count],
        first_predecessor: first,
        implicit_dependency_type: kind(p.pipeline_dependency == 1),
        mcache: None,
        flag_bit1: p.flag_bit1,
        flag_bit61: p.flag_bit61,
        flag_bit63: p.flag_bit63,
        payload: p.payload,
        qos_byte_32: p.scheduler_index,
        qos_class_40: p.stage.qos_class(),
        qos_word_48: p.qos_word,
        event_mask: [0, 0, 0, p.event_flags],
        rce_kind: 0,
        rce: p.rce,
        auxiliary: p.auxiliary,
    }, first))
}

/// Encode the kick entry into its 0x200-byte slot image.
pub(crate) fn encode_g18_kick_entry(
    p: &G18KickEntryInputs<'_>,
) -> Result<m5_sksm::PreparedM5SksmEntry, G18RenderCommandError> {
    let mut storage = [M5SksmDependency { queue_id: 0, stamp: 0, kind: 0 }; 31];
    let (operands, _) = g18_kick_entry_operands(p, &mut storage)?;
    m5_sksm::prepare_m5_sksm_entry(p.queue, operands).map_err(|_| G18RenderCommandError::Sksm)
}

/// Native tag14 add-kicks record (TA 0x8c8430c..0x8c84334, 3D 0x8bbc8f0..
/// 0x8bbc91c): count is the descriptor's submitted-kick total (NOP entries
/// plus this one), DM 0 for TA and 1 for 3D.
pub(crate) fn encode_g18_kick(
    stage: G18RenderStage,
    queue_id: u8,
    priority: u8,
    stamp: u64,
    count: u16,
) -> Result<[u8; G18_KICK_BYTES], G18RenderCommandError> {
    m5_sksm::encode_m5_sksm_add_kicks_for(stage.data_master(), queue_id, priority, stamp, count)
        .map_err(|_| G18RenderCommandError::Kick)
}
