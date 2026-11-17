// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G18 compute command serialization, derived from the original 26A428 HAL400.
//!
//! This encodes the submission fields and registers of the 0x1020-byte command.
//! It does not create resource owners, publish a queue entry or execute a kick.
//! The caller must own and validate all GPU addresses before publication. Native
//! offsets retained in field names are deliberately not assigned guessed meanings.
//! Register overrides, optional SMAPP preemption objects and random/alternating
//! tuning policies are outside this initial contract.

#![allow(dead_code)]

pub(crate) const G18_COMPUTE_COMMAND_BYTES: usize = 0x1020;

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18ComputeRegisterInputs {
    pub command_gpu: u64,
    /// Native descriptor +4d8, +480, +4e8, +4f0, +4f8, +500, in emission order.
    pub program: [u64; 6],
    /// Native descriptor +578 through +5a0, in emission order.
    pub aliases: [u64; 6],
    pub d550: u64,
    pub d558: u64,
    pub d548: u64,
    pub d5a8: u64,
    pub d5b0: u64,
    pub d540: u8,
    pub d5d8: u8,
    pub d5d9: u8,
    pub d62a: u8,
    pub d62b: u8,
    pub d420: u8,
    pub d418: u32,
    pub d774: u32,
    pub d5c0: u32,
    pub d5c4: u32,
    pub d5c8: u32,
    pub d50c: u32,
    pub d510: u32,
    pub d514: u32,
    pub d630: u32,
    pub context_id: u32,
    pub pool_slot: u32,
    /// 0x10241[63:16]: AGXUMADataRec+0x100, the per-dispatch SMA result
    /// (kext 8c4bdb8 kind 2). Not the pool binding's FList generation.
    pub sma_units: u64,
    pub operands: [u64; 2],
    pub support_control: u64,
    pub clusters: u32,
    pub cluster_width: u32,
    pub a6b7: u8,
    pub a6b9: u8,
    pub a6d0: u8,
    pub a758: u32,
    pub a10aac: u64,
    pub a10ab4: u64,
    pub a10aec: u32,
    pub a10af0: u8,
    pub a10af4: u32,
    pub a10b04: u8,
    pub a10b08: u32,
    pub a10b18: u32,
    pub policy54: u8,
    pub policy2c: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum G18ComputeRegisterError {
    InvalidTopology,
    AddressOverflow,
    InvalidCompletionState,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct G18ComputeRegisterSummary {
    pub main_gpu: u64,
    pub main_count: u16,
    pub total_main_count: u16,
    pub secondary_gpu: u64,
    pub secondary_count: u16,
}

/// Inputs to the native per-queue completion calculation. `generation` is the
/// selected stamp half's u64 generation; selection happens in the owner layer.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18ComputeCompletionState {
    pub stamp: u32,
    pub index: u32,
    pub entries: u32,
    pub generation: u64,
    pub base: u64,
}

/// The remaining native submission inputs. Offset names refer to the native
/// host descriptor, not firmware command offsets. Ownership is a caller duty.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct G18ComputeCommandInputs {
    pub sequence: u64,
    pub completion: G18ComputeCompletionState,
    pub stamp_addresses: [u64; 2],
    pub channel_ordinal: u32,
    pub has_wait: bool,
    pub d100: u64,
    pub d150: u32,
    pub d158: u64,
    pub d198: u64,
    pub d1b0: u64,
    pub d1b8: u64,
    pub d488: u64,
    pub d490: u64,
    pub d498: u64,
    pub d4a0: u64,
    pub d4a8: u64,
    pub d4b0: u32,
    pub d4b8: u64,
    pub d4c0: u32,
    pub d4d0: u32,
    pub d4e0: u64,
    pub d508: u8,
    pub d518: u64,
    pub d520: u32,
    pub d524: u32,
    pub d628: u8,
    pub d629: u8,
    pub d770: u8,
    pub d778: u64,
    pub d780: u64,
    pub d788: u64,
    pub d790: u64,
    pub d7b8: u64,
    pub d7d4: u8,
    pub d8b0: u8,
    pub extra_addresses: [u64; 4],
    pub extra_policy: u8,
}

/// Original HAL400 completion expression. Preserve the native u64 division,
/// u32 arithmetic and final 8-bit index independently; a wide modulo gives
/// different bytes around counter wrap. G18's native scratch payload is zero.
pub(crate) fn g18_compute_completion(
    p: &G18ComputeCompletionState,
) -> Result<u64, G18ComputeRegisterError> {
    if p.entries == 0 || p.index >= 128 {
        return Err(G18ComputeRegisterError::InvalidCompletionState);
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

/// Encode the native submission field writer and register program. This does
/// not publish a ring entry or imply that any pointer is backed. The caller
/// initializes the command allocation, rejects errors and retains all owners.
/// Aperture entries and optional SMAPP preemption resources are not supported.
pub(crate) fn encode_g18_compute_command(
    out: &mut [u8; G18_COMPUTE_COMMAND_BYTES],
    p: &G18ComputeCommandInputs,
    registers: &G18ComputeRegisterInputs,
) -> Result<G18ComputeRegisterSummary, G18ComputeRegisterError> {
    let completion = g18_compute_completion(&p.completion)?;
    put32(out, 0, 3);
    put64(out, 4, p.sequence);
    put32(out, 0xc, registers.context_id);
    for (offset, value) in [
        (0x10, p.d198),
        (0xfb5, p.d158),
        (0xe80, p.d518),
        (0xeb8, p.d488),
        (0xec0, p.d490),
        (0xec8, p.d498),
        (0xed0, p.d4a0),
        (0xed8, registers.program[0]),
        (0xee0, p.d4e0),
        (0xee8, p.d4a8),
        (0xef8, p.d4b8),
        (0xf2c, 0),
        (0xf34, 0),
        (0xf40, p.stamp_addresses[0]),
        (0xf48, p.stamp_addresses[1]),
        (0xf60, completion),
        (0xf68, p.d100),
        (0xf74, 0),
        (0xf7c, p.d1b0),
        (0xf84, p.d1b8),
        (0xf8c, p.d778),
        (0xf94, p.d780),
        (0xf9c, p.d788),
        (0xfa4, p.d790),
        (0xfac, p.d7b8),
    ] {
        put64(out, offset, value);
    }
    for (offset, value) in [
        (0xf70, p.d150),
        (0xe88, p.d520),
        (0xe90, p.d524),
        (0xeac, 0),
        (0xef0, p.d4b0),
        (0xf00, p.d4c0),
        (0xf10, p.d4d0),
        (0xf1c, u32::from(p.d508 & 1)),
        (0xf20, registers.d50c),
        (0xf24, registers.d510),
        (0xf28, registers.d514),
        (0xf50, p.completion.stamp),
        (0xf54, p.completion.index),
        (0xf58, p.channel_ordinal),
        (0xf5c, u32::from(p.has_wait)),
        (0xfdd, 0),
        (0xfe1, 0),
    ] {
        put32(out, offset, value);
    }
    out[0xfb4] = p.d7d4 & 1;
    out[0xf3c] = p.d628;
    out[0xf3d] = p.d629 & 1;
    out[0xf3e] = p.d770 & 1;
    out[0xf3f] = p.d8b0 & 1;
    let summary = encode_g18_compute_registers(out, registers)?;
    out[0x1005] &= 0xf2;
    if p.extra_addresses.iter().any(|&value| value != 0) {
        out[0x1005] |= 1 | ((p.extra_policy & 3) << 2);
        for (index, value) in p.extra_addresses.iter().enumerate() {
            put64(out, 0xfe5 + index * 8, *value);
        }
    }
    Ok(summary)
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

fn read32(out: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(out[offset..offset + 4].try_into().unwrap())
}

fn append(out: &mut [u8], base: usize, count: &mut u16, number: u32, value: u64) {
    let offset = base + usize::from(*count) * 12;
    let header = (read32(out, offset) & 0xfffc0006) | (number & 0x3fff9);
    put32(out, offset, header);
    put64(out, offset + 4, value);
    *count += 1;
}

/// Fill only native generateRegisterList outputs; leave all other command bytes
/// untouched. The full-command builder sets command+0xf68 before this call.
pub(crate) fn encode_g18_compute_registers(
    out: &mut [u8; G18_COMPUTE_COMMAND_BYTES],
    p: &G18ComputeRegisterInputs,
) -> Result<G18ComputeRegisterSummary, G18ComputeRegisterError> {
    if p.clusters == 0 || p.clusters > 16 || p.cluster_width == 0 || p.cluster_width > 16 {
        return Err(G18ComputeRegisterError::InvalidTopology);
    }
    let main_gpu = p
        .command_gpu
        .checked_add(0x40)
        .ok_or(G18ComputeRegisterError::AddressOverflow)?;
    let secondary_gpu = p
        .command_gpu
        .checked_add(0x760)
        .ok_or(G18ComputeRegisterError::AddressOverflow)?;
    let mode_table = [
        0x154006201u64,
        0x154004201,
        0x154002201,
        0x154002201,
        0x154002201,
        0x154002201,
        0x154000201,
    ];
    let mut control = 0x154024200u64;
    if p.a10b04 & 1 != 0 {
        control = mode_table
            .get(p.a10b08.wrapping_sub(1) as usize)
            .copied()
            .unwrap_or(0x154002201);
        control = (control | (u64::from(p.a10b18) << 17)) & 0x1ffffffee6200;
    }
    let cs_mode = if p.a10af0 & 1 == 0 {
        p.a10aec
    } else {
        u32::from(p.a10af4 == 0)
    };
    control = ((p.a10aac >> 28 | p.a10aac << 9) & 0x1e1fffffe30)
        | ((control | u64::from(cs_mode) << 20) & 0xfffe1fffe6001)
        | u64::from((!p.d540) & 1)
        | ((p.a10ab4 & 0x87ffffff) << 6)
        | ((p.a10ab4 >> 32) << 33);
    if p.a6d0 == 1 {
        control |= 0x30;
    }
    let mut count = 0u16;
    macro_rules! emit {
        ($number:expr, $value:expr) => {
            append(out, 0x40, &mut count, $number, $value)
        };
    }
    if p.d5d8 & 1 != 0 {
        emit!(0x81e9, 1);
    }
    if p.d5d9 & 1 != 0 {
        emit!(0x81f9, 1);
    }
    for (number, value) in [0x1a510, 0x1a420, 0x1a4d0, 0x1a4d8, 0x1a4e0, 0x1a4e8]
        .into_iter()
        .zip(p.program)
    {
        emit!(number, value);
    }
    emit!(0x1a440, control);
    put64(out, 0xf08, control);
    emit!(0xa5e1, p.d550 | p.d558 << 16);
    if p.d548 != 0 {
        emit!(0xa0f1, p.d548 & !0xfff);
    }
    let mut mode = if p.d62a & 1 == 0 {
        0x10c08860
    } else {
        0x14c08a60
    };
    if p.d62b & 1 != 0 {
        mode |= 0x80;
    }
    if p.clusters > 2 || p.a6b9 & 1 != 0 || p.d548 != 0 {
        mode |= 0x1c;
    }
    emit!(0x1a458, mode);
    emit!(0x101d9, u64::from(p.d774 & 0x7f));
    emit!(0xa111, 0xffff00000000);
    emit!(0xa131, 0x7000000000);
    emit!(0xa151, 0x7100000000);
    for (number, value) in [0x1a079, 0x1a081, 0x1a049, 0x1a051, 0x1a0a9, 0x1a0b1]
        .into_iter()
        .zip(p.aliases)
    {
        emit!(number, value);
    }
    emit!(0x101d1, (p.d5a8 & 0xffffffffc00fffff) | p.d5b0 << 20);
    emit!(0x1a0d9, u64::from(p.d5c4 & 0xfffffff8));
    let next = p.d5c8.wrapping_add(1);
    emit!(0x1a109, u64::from(if next < 32 { next } else { 0 }));
    let mut dispatch = if p.d5d8 & 1 == 0 {
        0xff0004
    } else {
        (u64::from(p.d5c0) << 16) | 0x1000004
    };
    if p.a6b7 & 1 == 0 {
        dispatch = (dispatch & 0xfffffffb) | (u64::from(p.a758) << 32);
    } else {
        dispatch &= 0xffffffff;
    }
    dispatch |= u64::from(p.d5d9 & 1) << 25;
    emit!(0x107a1, dispatch);
    put64(out, 0x1006, dispatch);
    put32(out, 0x100e, 0);
    let identity = u64::from(read32(out, 0xf68)) | u64::from(p.d50c) << 32;
    emit!(0x1a540, identity);
    emit!(0x11209, identity);
    emit!(0xa351, identity);
    if p.policy54 & 1 != 0 {
        if p.policy2c & 8 != 0 {
            emit!(0x1a430, 0x3c0f);
        }
        let dimensions = p.d510.min(0x3fff) | p.d514.min(0x3fff) << 16;
        for number in [0x1a4f0, 0x1a4f8, 0x1a500, 0x1a508] {
            emit!(number, u64::from(dimensions));
        }
    }
    put32(out, 0x20, 0);
    let mode = if p.d420 & 1 == 0 {
        0
    } else if p.d418 == 1 {
        3
    } else {
        1
    };
    emit!(0xa731, mode);
    let context = u64::from((p.d630 & 0x3f) | p.context_id.wrapping_shl(8));
    emit!(0x10201, context);
    emit!(0x10428, context);
    emit!(
        0x10241,
        (p.pool_slot as i32 as i64 as u64) | p.sma_units << 16
    );
    emit!(0xa611, 0);
    emit!(
        0x10229,
        (p.support_control & 0x7ffffffffc0) | 0x1000000000000000
    );
    if p.d5d8 & 1 != 0 {
        emit!(0x81e1, 1);
    }
    if p.d5d9 & 1 != 0 {
        emit!(0x81f1, 1);
    }
    let extent = u64::from(p.cluster_width + 10 * p.clusters - 10) * 0x1800;
    let a = p.operands[0] & 0x7fffffffc00;
    let b = p.operands[1] & 0x7fffffffc00;
    emit!(0x10099, a | 5);
    emit!(0x10091, a + extent);
    emit!(0xa5c1, b | 5);
    emit!(0xa5c9, b + extent);
    let main_count = count;
    put16(out, 0x18, main_count);
    emit!(0xa731, 0);
    emit!(0x1a440, control | 8);
    put16(out, 0x1a, count);
    put16(out, 0x1c, count);
    put64(out, 0x740, main_gpu);
    put16(out, 0x748, count);
    let mut secondary_count = 0;
    for (number, value) in [
        (0x10099, b | 5),
        (0x10091, b + extent),
        (0xa5c1, a | 5),
        (0xa5c9, a + extent),
    ] {
        append(out, 0x760, &mut secondary_count, number, value);
    }
    put64(out, 0xe60, secondary_gpu);
    put16(out, 0xe68, secondary_count);
    Ok(G18ComputeRegisterSummary {
        main_gpu,
        main_count,
        total_main_count: count,
        secondary_gpu,
        secondary_count,
    })
}
