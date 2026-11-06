// SPDX-License-Identifier: GPL-2.0-only OR MIT

#![cfg_attr(not(test), allow(dead_code))]

//! T8142/G17G C0 J813 compute register program for the 25G76 native profile.
//!
//! The native SKSM payload has selector 3 and its register array begins at
//! +0x40. The 40-write program below is separately captured on M5; it is not
//! the G17P program with a new chip ID. Queue registration, descriptor-owned
//! pointers, shader/CDM construction and completion still belong to the M5
//! runtime. Encoding these registers does not admit a Linux GPU job.

pub(crate) const M5_COMPUTE_REGISTER_COUNT: usize = 40;
pub(crate) const M5_COMPUTE_REGISTER_BYTES: usize = 12 * M5_COMPUTE_REGISTER_COUNT;
/// Measured descriptor prefix, not a claim about the host allocation size.
pub(crate) const M5_COMPUTE_DESCRIPTOR_BYTES: usize = 0x1000;

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum M5ComputeError {
    AddressOverflow,
    BufferTooSmall,
    CdmTerminatorBeforeStream,
}

/// Retained objects and per-command state for the measured simple CL path.
/// Samplers, user timestamps, indirect dispatch and extra spill modes are not
/// part of this profile. Pointer ownership belongs to the runtime caller.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ComputeDescriptor {
    pub(crate) registers: M5ComputeParameters,
    pub(crate) submit_sequence: u64,
    /// Host descriptor +0x188, copied to native command +0x10.
    pub(crate) scheduler_record: u64,
    pub(crate) descriptor_low_alias: u64,
    pub(crate) cdm_terminator: u64,
    pub(crate) dispatch_stamps: [u64; 2],
    pub(crate) event_machine_stamp: u32,
    pub(crate) queue_id: u32,
    pub(crate) queue_ordinal: u32,
    pub(crate) has_event: bool,
    pub(crate) submission_index: u32,
    pub(crate) status: [u64; 2],
    pub(crate) shared_control: u64,
    pub(crate) shared_control_enabled: bool,
    /// Sixteen bytes copied by the native host from the event machine's
    /// +0x70 record. Their bit meanings are not all qualified; keep opaque.
    pub(crate) event_metadata: [u8; 16],
    pub(crate) zero_page: u64,
    /// Host descriptor +0x148; not a fixed target constant.
    pub(crate) owner_id: u8,
    /// Full host descriptor +0x140. Register context packing uses only its
    /// low byte; the descriptor copy at +0xf70 must retain all 32 bits.
    pub(crate) descriptor_work_ordinal: u32,
}

/// Encode all 4096 bytes of the native simple-compute descriptor prefix.
/// The register arrays and their aliases are generated from owned inputs.
/// This function performs no allocation, cache operation or publication.
pub(crate) fn encode_m5_compute_descriptor(
    p: M5ComputeDescriptor,
    raw: &mut [u8],
) -> Result<(), M5ComputeError> {
    if raw.len() < M5_COMPUTE_DESCRIPTOR_BYTES {
        return Err(M5ComputeError::BufferTooSmall);
    }
    if p.cdm_terminator < p.registers.cdm_base {
        return Err(M5ComputeError::CdmTerminatorBeforeStream);
    }
    let registers = build_m5_compute_registers(p.registers)?;
    let primary_alias = add(p.descriptor_low_alias, 0x40)?;
    let secondary_alias = add(p.descriptor_low_alias, 0x760)?;
    let secondary = [
        (0x10099u32, add(p.registers.operand_state_base, 5)?),
        (0x10091, add(p.registers.operand_state_base, 0xf000)?),
        (0x0a5c1, add(p.registers.operand_state_base, 0xf405)?),
        (0x0a5c9, add(p.registers.operand_state_base, 0x1e400)?),
    ];
    let raw = &mut raw[..M5_COMPUTE_DESCRIPTOR_BYTES];
    raw.fill(0);
    for (offset, value) in [
        (0x00, 3u32), (0x0c, u32::from(p.registers.context_id)),
        (0x748, 0x01e0_0028), (0xe68, 0x0030_0004),
        (0xf20, (p.registers.dispatch_identity >> 32) as u32),
        (0xf28, u32::MAX), (0xf50, p.event_machine_stamp),
        (0xf54, p.queue_id), (0xf58, p.queue_ordinal),
        (0xf5c, u32::from(p.has_event)), (0xf60, p.submission_index),
        (0xf70, p.descriptor_work_ordinal),
    ] {
        raw[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    for (offset, value) in [
        (4, p.submit_sequence), (0x10, p.scheduler_record),
        (0x740, primary_alias), (0xe60, secondary_alias),
        (0xed8, p.registers.preempt_base), (0xee0, p.cdm_terminator),
        (0xf08, 0x0000_0001_5402_4201),
        (0xf40, p.dispatch_stamps[0]), (0xf48, p.dispatch_stamps[1]),
        (0xf68, p.registers.dispatch_identity & 0xffff_ffff),
        (0xf7c, p.status[0]), (0xf84, p.status[1]),
        (0xfb2, p.shared_control), (0xfcb, p.zero_page),
    ] {
        raw[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    }
    for (index, count) in [37u16, 39, 39, 40].into_iter().enumerate() {
        raw[0x18 + index * 2..0x1a + index * 2].copy_from_slice(&count.to_le_bytes());
    }
    for (index, register) in registers.into_iter().enumerate() {
        let offset = 0x40 + index * 12;
        raw[offset..offset + 4].copy_from_slice(&register.number.to_le_bytes());
        raw[offset + 4..offset + 12].copy_from_slice(&register.value.to_le_bytes());
    }
    for (index, (number, value)) in secondary.into_iter().enumerate() {
        let offset = 0x760 + index * 12;
        raw[offset..offset + 4].copy_from_slice(&number.to_le_bytes());
        raw[offset + 4..offset + 12].copy_from_slice(&value.to_le_bytes());
    }
    raw[0xfba] = u8::from(p.shared_control_enabled);
    raw[0xfbb..0xfcb].copy_from_slice(&p.event_metadata);
    raw[0xfd3] = p.owner_id;
    Ok(())
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ComputeRegister {
    pub(crate) number: u32,
    pub(crate) value: u64,
}

impl M5ComputeRegister {
    const fn new(number: u32, value: u64) -> Self {
        Self { number, value }
    }
}

/// Command-owned addresses and identities; the runtime must retain their
/// backing and validate its mappings before publishing the complete command.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct M5ComputeParameters {
    pub(crate) preempt_base: u64,
    pub(crate) cdm_base: u64,
    pub(crate) dispatch_identity: u64,
    pub(crate) context_id: u8,
    pub(crate) work_ordinal: u8,
    pub(crate) robustness: u64,
    pub(crate) operand_state_base: u64,
    /// Owned UMA pool slot, copied from native host descriptor +0x788 by
    /// 25G76 generateRegisterList at 0x89e38dc..0x89e391c. This must match
    /// shared-control +8 and the populated hardware UMA descriptor slot.
    pub(crate) private_pool_hw_id: u64,
    /// Native command descriptor +0x530, copied to register 0xd4a1.
    pub(crate) descriptor_control_530: u64,
    /// Descriptor +0x538; zero selects the native 0x60 default. The native
    /// encoder shifts the selected value left 32 bits, discarding high bits.
    pub(crate) descriptor_control_538: u64,
    /// Native descriptor +0x60a/+0x60b bit 0. Their broader semantics are
    /// still under investigation; neither is a fixed M5 device property.
    pub(crate) descriptor_flag_60a: bool,
    pub(crate) descriptor_flag_60b: bool,
}

fn add(base: u64, offset: u64) -> Result<u64, M5ComputeError> {
    base.checked_add(offset)
        .ok_or(M5ComputeError::AddressOverflow)
}

/// Encode the measured J813 profile, including its M5 control words and
/// enlarged operand-state offsets. Other G17G configurations are not implied.
pub(crate) fn build_m5_compute_registers(
    p: M5ComputeParameters,
) -> Result<[M5ComputeRegister; M5_COMPUTE_REGISTER_COUNT], M5ComputeError> {
    let context = ((p.context_id as u64) << 8) | p.work_ordinal as u64;
    // 25G76 generateRegisterList +0x5d4..0x670. The final 0xa599 write
    // restores 0x60 irrespective of the command-specific initial value.
    let control_538 = if p.descriptor_control_538 == 0 {
        0x60
    } else {
        p.descriptor_control_538
    };
    // 25G76 +0x644..0x66c; additional device-dependent bits used by other
    // profiles are outside this measured J813 configuration.
    let mode = if p.descriptor_flag_60a {
        0x14c0_8a60
    } else {
        0x10c0_8860
    } | if p.descriptor_flag_60b { 0x80 } else { 0 };
    Ok([
        M5ComputeRegister::new(0x1a510, p.preempt_base),
        M5ComputeRegister::new(0x1a420, p.cdm_base),
        M5ComputeRegister::new(0x1a4d0, add(p.preempt_base, 0x1480)?),
        M5ComputeRegister::new(0x1a4d8, add(p.preempt_base, 0x1488)?),
        M5ComputeRegister::new(0x1a4e0, add(p.preempt_base, 0x1490)?),
        M5ComputeRegister::new(0x1a4e8, add(p.preempt_base, 0x1498)?),
        M5ComputeRegister::new(0x1a440, 0x0000_0001_5402_4201),
        M5ComputeRegister::new(0x0d4a1, p.descriptor_control_530),
        M5ComputeRegister::new(0x0d411, 0x0000_0002_0000_0001),
        M5ComputeRegister::new(0x0a599, (control_538 << 32) | 0x0040_0020),
        M5ComputeRegister::new(0x1a458, mode),
        M5ComputeRegister::new(0x101d9, 0x1c),
        M5ComputeRegister::new(0x1a089, 0),
        M5ComputeRegister::new(0x1a091, 0),
        M5ComputeRegister::new(0x1a059, 0),
        M5ComputeRegister::new(0x1a061, 0),
        M5ComputeRegister::new(0x1a0b9, 0),
        M5ComputeRegister::new(0x1a0c1, 0),
        M5ComputeRegister::new(0x101d1, 0),
        M5ComputeRegister::new(0x0d479, 0),
        M5ComputeRegister::new(0x1a0e9, 8),
        M5ComputeRegister::new(0x1a141, 0),
        M5ComputeRegister::new(0x107a1, 0xff_0000),
        M5ComputeRegister::new(0x1a540, p.dispatch_identity),
        M5ComputeRegister::new(0x014a9, p.dispatch_identity),
        M5ComputeRegister::new(0x0a351, p.dispatch_identity),
        M5ComputeRegister::new(0x0a731, 0),
        M5ComputeRegister::new(0x10201, context),
        M5ComputeRegister::new(0x10428, context),
        M5ComputeRegister::new(0x14028, p.private_pool_hw_id),
        M5ComputeRegister::new(0x14070, p.robustness | 1),
        M5ComputeRegister::new(0x10229, add(p.operand_state_base, 0x1e800)?),
        M5ComputeRegister::new(0x140a8, add(p.operand_state_base, 0x1f000)?),
        M5ComputeRegister::new(0x10099, add(p.operand_state_base, 0x0f405)?),
        M5ComputeRegister::new(0x10091, add(p.operand_state_base, 0x1e400)?),
        M5ComputeRegister::new(0x0a5c1, add(p.operand_state_base, 0x00005)?),
        M5ComputeRegister::new(0x0a5c9, add(p.operand_state_base, 0x0f000)?),
        M5ComputeRegister::new(0x0a731, 0),
        M5ComputeRegister::new(0x1a440, 0x0000_0001_5402_4209),
        M5ComputeRegister::new(0x0a599, 0x0000_0060_0040_0020),
    ])
}

/// Write only the register array. No descriptor header or publication state
/// is manufactured here, and an invalid request leaves the destination alone.
pub(crate) fn encode_m5_compute_registers(
    p: M5ComputeParameters,
    bytes: &mut [u8],
) -> Result<(), M5ComputeError> {
    if bytes.len() < M5_COMPUTE_REGISTER_BYTES {
        return Err(M5ComputeError::BufferTooSmall);
    }
    let registers = build_m5_compute_registers(p)?;
    for (index, register) in registers.iter().enumerate() {
        let offset = index * 12;
        bytes[offset..offset + 4].copy_from_slice(&register.number.to_le_bytes());
        bytes[offset + 4..offset + 12].copy_from_slice(&register.value.to_le_bytes());
    }
    Ok(())
}

