// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Private 26A428 compute-add capture, with a poisoned Linux output buffer.
//! All records are user virtual pages, never captured physical/kernel mappings.
//! This fixed diagnostic format does not grant permission to submit a GPU job.

#![allow(dead_code)]

use crate::g18::fw::compute::{G18ComputeCommandInputs, G18ComputeRegisterInputs};
use crate::g18::fw::compute_state::{G18ComputeState, G18_COMPUTE_STATE_BYTES};

pub(crate) const HEADER_BYTES: usize = 64;
pub(crate) const PAGE_BYTES: usize = 0x4000;
pub(crate) const RECORD_BYTES: usize = PAGE_BYTES + 16;
pub(crate) const PAGE_COUNT: usize = 46;
pub(crate) const INPUT_A: u64 = 0x100_0003_0000;
pub(crate) const INPUT_B: u64 = 0x100_0003_8000;
pub(crate) const OUTPUT: u64 = 0x100_0004_0000;
pub(crate) const POISON: u32 = 0xdead_beef;

// Exact allocations in submission 1. Submission 2 is the post-compute blit.
const RANGES: [(u64, usize); 15] = [
    (0, 0x10000),
    (0x100_0000_0000, 0x10000), (0x100_0001_8000, 0x10000),
    (INPUT_A, PAGE_BYTES), (INPUT_B, PAGE_BYTES), (OUTPUT, PAGE_BYTES),
    (0x100_0004_8000, 0x20000), (0x100_0007_0000, 0x20000),
    (0x100_0009_8000, 0x8000), (0x100_000a_8000, 0x8000),
    (0x100_000b_8000, 0x8000), (0x100_000c_8000, 0x8000),
    (0x100_000d_8000, 0x8000), (0x100_000e_8000, 0x8000),
    (0x100_000f_8000, 0xc000),
];

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum PayloadError { Header, Length, Checksum, Page, Input, Output, State }

pub(crate) struct ComputePayload<'a> { bytes: &'a [u8] }
pub(crate) struct PayloadPage<'a> { pub(crate) address: u64, pub(crate) bytes: &'a [u8] }

fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
fn u64_at(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

/// Transfer corruption check only. Capture provenance is in the SHA256 receipt.
pub(crate) fn fnv64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, b|
        (h ^ u64::from(*b)).wrapping_mul(0x100_0000_01b3))
}

/// Build the captured addition's state and default device control fields.
/// Original 26A428 setup 8bf1280/8bf1414 supplies the spill inputs to 8bf16cc:
/// d518/d520/d524 and d5a8/d5b0/d5d8 are all zero for submission 1. Native
/// configureDevice stores at 8bc73fc, 8c492a0, 8bc93bc and 8bc7774 establish
/// the register defaults below. Optional tuning and debug policies stay off.
/// Context, pool, scratch, scheduler, completion and addresses belong to the
/// retained owners; this function never publishes or submits a command.
pub(crate) fn addition_inputs(state: &[u8])
    -> Result<(G18ComputeCommandInputs, G18ComputeRegisterInputs), PayloadError> {
    if state.len() != G18_COMPUTE_STATE_BYTES || fnv64(state) != 0x956a_f8e1_8848_5ce8 {
        return Err(PayloadError::State);
    }
    let mut command = G18ComputeCommandInputs::default();
    let mut registers = G18ComputeRegisterInputs {
        a10aac: 1,
        a6d0: 1,
        clusters: 2,
        cluster_width: 6,
        ..G18ComputeRegisterInputs::default()
    };
    G18ComputeState::parse(state).ok_or(PayloadError::State)?.apply(&mut command, &mut registers);
    Ok((command, registers))
}

/// Exact IEEE754 encoding of a small signed integer divided by four. Keeps
/// float operations out of kernel code; the qualified values fit in 13 bits.
fn quarter_bits(value: i32) -> u32 {
    if value == 0 { return 0; }
    let magnitude = value.unsigned_abs();
    let top = 31 - magnitude.leading_zeros();
    (u32::from(value < 0) << 31) | ((top + 125) << 23)
        | ((magnitude << (23 - top)) & 0x7f_ffff)
}

impl<'a> ComputePayload<'a> {
    pub(crate) fn parse(bytes: &'a [u8]) -> Result<Self, PayloadError> {
        let expected = HEADER_BYTES + G18_COMPUTE_STATE_BYTES + PAGE_COUNT * RECORD_BYTES;
        if bytes.len() != expected { return Err(PayloadError::Length); }
        if &bytes[..8] != b"G18CMP01" || u32_at(bytes, 8) != 1
            || u32_at(bytes, 12) != HEADER_BYTES as u32
            || u32_at(bytes, 16) != PAGE_BYTES as u32
            || u32_at(bytes, 20) != PAGE_COUNT as u32
            || u32_at(bytes, 24) != G18_COMPUTE_STATE_BYTES as u32
            || u32_at(bytes, 28) != 4096
            || u64_at(bytes, 32) != INPUT_A || u64_at(bytes, 40) != INPUT_B
            || u64_at(bytes, 48) != OUTPUT
        { return Err(PayloadError::Header); }
        if fnv64(&bytes[HEADER_BYTES..]) != u64_at(bytes, 56) {
            return Err(PayloadError::Checksum);
        }
        let payload = Self { bytes };
        let mut index = 0;
        for (base, size) in RANGES {
            for offset in (0..size).step_by(PAGE_BYTES) {
                let record = HEADER_BYTES + G18_COMPUTE_STATE_BYTES + index * RECORD_BYTES;
                let page = payload.page(index).ok_or(PayloadError::Page)?;
                if page.address != base + offset as u64 || u32_at(bytes, record + 8) != 3
                    || u32_at(bytes, record + 12) != PAGE_BYTES as u32
                { return Err(PayloadError::Page); }
                if page.address == OUTPUT {
                    if page.bytes.chunks_exact(4).any(|v| u32_at(v, 0) != POISON) {
                        return Err(PayloadError::Output);
                    }
                } else if page.address == INPUT_A || page.address == INPUT_B {
                    for (i, word) in page.bytes.chunks_exact(4).enumerate() {
                        let value = if page.address == INPUT_A { 2 * i as i32 } else { 4000 - i as i32 };
                        if u32_at(word, 0) != quarter_bits(value) { return Err(PayloadError::Input); }
                    }
                }
                index += 1;
            }
        }
        if index != PAGE_COUNT { return Err(PayloadError::Page); }
        let mut c = G18ComputeCommandInputs::default();
        let mut r = G18ComputeRegisterInputs::default();
        payload.state().apply(&mut c, &mut r);
        if r.program != [0x100_000f_8000, 0x100_000c_8000, 0x100_000f_9b80,
                         0x100_000f_9b88, 0x100_000f_9b90, 0x100_000f_9b98]
            || c.d4e0 != 0x100_000c_802c || r.aliases != [0; 6] || r.d548 != 0
        { return Err(PayloadError::State); }
        // All six program pointers and the command-stream end must be backed.
        for address in r.program.into_iter().chain([c.d4e0]) {
            if !RANGES.iter().any(|(base, size)| *base <= address && address < *base + *size as u64) {
                return Err(PayloadError::State);
            }
        }
        Ok(payload)
    }

    pub(crate) fn page_count(&self) -> usize { PAGE_COUNT }

    pub(crate) fn state(&self) -> G18ComputeState<'a> {
        G18ComputeState::parse(&self.bytes[HEADER_BYTES..HEADER_BYTES + G18_COMPUTE_STATE_BYTES]).unwrap()
    }

    pub(crate) fn state_bytes(&self) -> &[u8] {
        &self.bytes[HEADER_BYTES..HEADER_BYTES + G18_COMPUTE_STATE_BYTES]
    }

    pub(crate) fn page(&self, index: usize) -> Option<PayloadPage<'a>> {
        if index >= PAGE_COUNT { return None; }
        let offset = HEADER_BYTES + G18_COMPUTE_STATE_BYTES + index * RECORD_BYTES;
        Some(PayloadPage { address: u64_at(self.bytes, offset),
            bytes: &self.bytes[offset + 16..offset + RECORD_BYTES] })
    }
}

/// Byte evidence only: callers additionally require a completed, fault-free
/// live submission before interpreting a correct output as GPU execution.
#[derive(Debug, Default, Copy, Clone)]
pub(crate) struct ComputeResult {
    pub(crate) checked: usize,
    pub(crate) correct: usize,
    pub(crate) untouched: usize,
}

impl ComputeResult {
    pub(crate) fn inspect(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != PAGE_BYTES { return None; }
        let mut result = Self::default();
        for (i, word) in bytes.chunks_exact(4).enumerate() {
            let value = u32_at(word, 0);
            result.checked += 1;
            result.correct += usize::from(value == quarter_bits(4000 + i as i32));
            result.untouched += usize::from(value == POISON);
        }
        Some(result)
    }
}
