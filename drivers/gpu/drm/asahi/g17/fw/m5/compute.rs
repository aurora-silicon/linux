// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 compute register programs and descriptor (linux-aurora t6050-gpu's g17/fw/m5/compute.rs).
//! The operand and preemption layouts differ from the other G17 profile; sharing a register
//! count does not imply a shared register program.
//!
//! Only programs without a shader-context scratch (SCS) or matrix bracket and without
//! intersection tables are encoded: those brackets add writes and checkpoints, and the kick
//! entry would need the matrix flag.

use super::super::{
    kick::{RegisterArray, RegisterArrayBinding, RegisterWrite},
    offset_va, work_key,
};
use kernel::prelude::*;

pub(crate) const MAIN_COUNT: usize = 40;
pub(crate) const ALTERNATE_COUNT: usize = 4;
pub(crate) const PREEMPT_BYTES: usize = 0x4000;
pub(crate) const OPERAND_BYTES: usize = 0x40000;
/// Checkpoints of a program without scratch or matrix brackets.
pub(crate) const CHECKPOINTS: [u16; 4] = [37, 39, 39, 40];

/// The scratch registers of a program without shader-context scratch: no pages, the minimum
/// stack, no driver section and the empty window.
const NO_SCRATCH_SIZE: u64 = 0;
const NO_SCRATCH_STACK: u64 = 8;
const NO_SCRATCH_DRIVER: u64 = 0;
const NO_SCRATCH_WINDOW: u64 = 0xff_0000;

/// Addresses and policy owned by a queued compute command.
pub(crate) struct RegisterArgs {
    /// Opaque allocation word, independent of the SCS layout.
    pub(crate) auxiliary_allocation: u32,
    pub(crate) preempt_va: u64,
    pub(crate) cdm_va: u64,
    pub(crate) operand_va: u64,
    pub(crate) usage_va: u64,
    /// Private-memory pool slot, as named by the usage record.
    pub(crate) free_list_slot: u32,
    pub(crate) context_id: u16,
    pub(crate) work_state: u32,
    /// Packed predecessor/current object identifiers.
    pub(crate) object_ids: u64,
    /// Pool target and resource policy for the first 0x0a599 write.
    pub(crate) resource_policy: u64,
    /// Start of the four eight-byte state words in the preemption buffer.
    pub(crate) preempt_state_offset: u32,
    /// Size of one operand-state region; the program exchanges two regions.
    pub(crate) operand_region_bytes: u32,
    pub(crate) operand_aux_offset: u32,
    pub(crate) resource_control: u64,
    pub(crate) dispatch_control: u64,
    pub(crate) auxiliary_control: u64,
}

impl RegisterArgs {
    fn validate(&self) -> Result {
        if self.preempt_va & 0x3fff != 0
            || self.operand_va & 0x3fff != 0
            || self.cdm_va & 7 != 0
            || self.usage_va & 7 != 0
            || self.context_id >= 64
        {
            return Err(EINVAL);
        }
        offset_va(self.preempt_va, PREEMPT_BYTES as u64)?;
        offset_va(self.operand_va, OPERAND_BYTES as u64)?;
        if self.preempt_state_offset & 7 != 0
            || u64::from(self.preempt_state_offset) + 32 > PREEMPT_BYTES as u64
            || self.operand_region_bytes == 0
            || self.operand_region_bytes & 0xfff != 0
            || u64::from(self.operand_region_bytes) * 2 + 0x808 > OPERAND_BYTES as u64
            || self.operand_aux_offset & 7 != 0
            || u64::from(self.operand_aux_offset) + 8 > OPERAND_BYTES as u64
        {
            return Err(EINVAL);
        }
        Ok(())
    }
}

/// Construct the complete primary program before publishing its descriptor.
pub(crate) fn main(args: &RegisterArgs) -> Result<[RegisterWrite; MAIN_COUNT]> {
    args.validate()?;
    let preempt = args.preempt_va;
    let operand = args.operand_va;
    let ids = args.object_ids;
    let state = u64::from(args.preempt_state_offset);
    let region = u64::from(args.operand_region_bytes);
    let key = work_key(args.context_id, args.work_state);
    Ok([
        RegisterWrite::new(0x1a510, preempt),
        RegisterWrite::new(0x1a420, args.cdm_va),
        RegisterWrite::new(0x1a4d0, offset_va(preempt, state)?),
        RegisterWrite::new(0x1a4d8, offset_va(preempt, state + 8)?),
        RegisterWrite::new(0x1a4e0, offset_va(preempt, state + 16)?),
        RegisterWrite::new(0x1a4e8, offset_va(preempt, state + 24)?),
        RegisterWrite::new(0x1a440, 0x154024201),
        RegisterWrite::new(0x0d4a1, args.resource_control),
        RegisterWrite::new(0x0d411, 0x200000001),
        RegisterWrite::new(0x0a599, args.resource_policy),
        RegisterWrite::new(0x1a458, args.dispatch_control),
        RegisterWrite::new(0x101d9, 0x1c),
        // No intersection tables.
        RegisterWrite::new(0x1a089, 0),
        RegisterWrite::new(0x1a091, 0),
        RegisterWrite::new(0x1a059, 0),
        RegisterWrite::new(0x1a061, 0),
        RegisterWrite::new(0x1a0b9, 0),
        RegisterWrite::new(0x1a0c1, 0),
        RegisterWrite::new(0x101d1, NO_SCRATCH_SIZE),
        RegisterWrite::new(0x0d479, u64::from(args.auxiliary_allocation)),
        RegisterWrite::new(0x1a0e9, NO_SCRATCH_STACK),
        RegisterWrite::new(0x1a141, NO_SCRATCH_DRIVER),
        // Bit 25 would request the matrix bracket.
        RegisterWrite::new(0x107a1, NO_SCRATCH_WINDOW),
        RegisterWrite::new(0x1a540, ids),
        RegisterWrite::new(0x014a9, ids),
        RegisterWrite::new(0x0a351, ids),
        RegisterWrite::new(0x0a731, args.auxiliary_control),
        RegisterWrite::new(0x10201, key),
        RegisterWrite::new(0x10428, key),
        RegisterWrite::new(0x14028, args.free_list_slot.into()),
        RegisterWrite::new(0x14070, args.usage_va | 1),
        RegisterWrite::new(0x10229, offset_va(operand, region * 2 + 0x800)?),
        RegisterWrite::new(0x140a8, offset_va(operand, args.operand_aux_offset.into())?),
        RegisterWrite::new(0x10099, offset_va(operand, region + 0x405)?),
        RegisterWrite::new(0x10091, offset_va(operand, region * 2 + 0x400)?),
        RegisterWrite::new(0x0a5c1, offset_va(operand, 5)?),
        RegisterWrite::new(0x0a5c9, offset_va(operand, region)?),
        RegisterWrite::new(0x0a731, 0),
        RegisterWrite::new(0x1a440, 0x154024209),
        RegisterWrite::new(0x0a599, 0x6000400020),
    ])
}

/// Operand-state exchange used by the alternate program.
pub(crate) fn alternate(args: &RegisterArgs) -> Result<[RegisterWrite; ALTERNATE_COUNT]> {
    args.validate()?;
    let operand = args.operand_va;
    let region = u64::from(args.operand_region_bytes);
    Ok([
        RegisterWrite::new(0x10099, offset_va(operand, 5)?),
        RegisterWrite::new(0x10091, offset_va(operand, region)?),
        RegisterWrite::new(0x0a5c1, offset_va(operand, region + 0x405)?),
        RegisterWrite::new(0x0a5c9, offset_va(operand, region * 2 + 0x400)?),
    ])
}

/// Register-array bindings in a compute kick. The first checkpoint follows
/// the initial 0x0a5c9 write, before the second 0x0a731 write.
pub(crate) fn bindings(descriptor_va: u64) -> Result<[Option<RegisterArrayBinding>; 4]> {
    Ok([
        Some(RegisterArrayBinding::new(
            offset_va(descriptor_va, 0x40)?,
            CHECKPOINTS[0] as u8,
        )?),
        Some(RegisterArrayBinding::new(
            offset_va(descriptor_va, 0x760)?,
            ALTERNATE_COUNT as u8,
        )?),
        None,
        None,
    ])
}

/// The described prefix of a compute descriptor. Storage beyond this prefix is
/// owned and initialized by the runtime allocation, not described here.
pub(crate) const DESCRIPTOR_PREFIX_BYTES: usize = 0x1000;

/// Per-command fields outside the two register programs. Fields whose bit
/// meanings are not established remain explicit opaque values.
pub(crate) struct DescriptorArgs {
    pub(crate) registers: RegisterArgs,
    pub(crate) descriptor_va: u64,
    pub(crate) submit_sequence: u64,
    pub(crate) work_state_va: u64,
    pub(crate) cdm_end_va: u64,
    pub(crate) stamp_va: [u64; 2],
    pub(crate) stamp_value: u32,
    pub(crate) qid: u32,
    pub(crate) kick_timestamp: u64,
    pub(crate) completion_ordinal: u32,
    pub(crate) status_va: [u64; 2],
    pub(crate) timestamp_va: [u64; 2],
    pub(crate) shared_control_va: u64,
    pub(crate) event_metadata: [u8; 16],
    pub(crate) usage_fw_va: u64,
    pub(crate) completion_identity: u64,
}

/// The M5 descriptor has a shorter reserved region before shared-control
/// state. Register-array offsets are shared with G17, tail offsets are not.
#[repr(C, packed)]
pub(crate) struct Descriptor {
    tag: u32,
    submit_sequence: u64,
    context_id: u32,
    work_state_va: u64,
    checkpoints: [u16; 4],
    reserved_20: [u8; 0x20],
    registers: RegisterArray,
    alternate: RegisterArray,
    reserved_e80: [u8; 0x58],
    preempt_va: u64,
    cdm_end_va: u64,
    reserved_ee8: [u8; 0x20],
    control: u64,
    reserved_f10: [u8; 0x10],
    predecessor_id: u32,
    reserved_f24: u32,
    unk_f28: u32,
    firmware_timestamps: [u64; 2],
    reserved_f3c: u32,
    stamp_va: [u64; 2],
    stamp_value: u32,
    qid: u32,
    reserved_f58: u64,
    kick_timestamp: u64,
    current_id: u64,
    completion_ordinal: u32,
    reserved_f74: [u8; 8],
    status_va: [u64; 2],
    timestamp_va: [u64; 2],
    reserved_f9c: [u8; 0x10],
    shared_control_va: u64,
    shared_control_enabled: u8,
    event_metadata: [u8; 16],
    usage_fw_va: u64,
    completion_identity: u64,
    reserved_fd5: [u8; 0x2b],
}

static_assert!(size_of::<Descriptor>() == DESCRIPTOR_PREFIX_BYTES);
static_assert!(core::mem::offset_of!(Descriptor, registers) == 0x40);
static_assert!(core::mem::offset_of!(Descriptor, alternate) == 0x760);
static_assert!(core::mem::offset_of!(Descriptor, stamp_va) == 0xf40);
static_assert!(core::mem::offset_of!(Descriptor, completion_ordinal) == 0xf70);
static_assert!(core::mem::offset_of!(Descriptor, shared_control_va) == 0xfac);
static_assert!(core::mem::offset_of!(Descriptor, usage_fw_va) == 0xfc5);
static_assert!(core::mem::offset_of!(Descriptor, completion_identity) == 0xfcd);

// SAFETY: Every field consists only of integer values and their arrays.
unsafe impl Zeroable for Descriptor {}

impl Descriptor {
    /// Fill the entire prefix; no address or register becomes visible before
    /// validation completes. The owner publishes only after this returns.
    pub(crate) fn write(&mut self, args: &DescriptorArgs) -> Result {
        if args.cdm_end_va < args.registers.cdm_va || args.qid >= 128 {
            return Err(EINVAL);
        }
        let main = main(&args.registers)?;
        let alternate = alternate(&args.registers)?;
        let main_va = offset_va(args.descriptor_va, 0x40)?;
        let alternate_va = offset_va(args.descriptor_va, 0x760)?;
        // Also validate the kick's encodable addresses before mutating storage.
        bindings(args.descriptor_va)?;
        super::super::clear(self);
        self.tag = 3;
        self.submit_sequence = args.submit_sequence;
        self.context_id = args.registers.context_id.into();
        self.work_state_va = args.work_state_va;
        self.checkpoints = CHECKPOINTS;
        super::set_register_array(&mut self.registers, main_va, &main)?;
        super::set_register_array(&mut self.alternate, alternate_va, &alternate)?;
        self.preempt_va = args.registers.preempt_va;
        self.cdm_end_va = args.cdm_end_va;
        self.control = 0x154024201;
        self.predecessor_id = (args.registers.object_ids >> 32) as u32;
        self.unk_f28 = u32::MAX;
        self.stamp_va = args.stamp_va;
        self.stamp_value = args.stamp_value;
        self.qid = args.qid;
        self.kick_timestamp = args.kick_timestamp;
        self.current_id = args.registers.object_ids as u32 as u64;
        self.completion_ordinal = args.completion_ordinal;
        self.status_va = args.status_va;
        self.timestamp_va = args.timestamp_va;
        self.shared_control_va = args.shared_control_va;
        self.shared_control_enabled = u8::from(args.shared_control_va != 0);
        self.event_metadata = args.event_metadata;
        self.usage_fw_va = args.usage_fw_va;
        self.completion_identity = args.completion_identity;
        Ok(())
    }
}
