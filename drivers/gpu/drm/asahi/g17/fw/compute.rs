// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Compute work descriptor.
//!
//! A compute kick launches one [`ComputeDescriptor`]. The descriptor embeds
//! the two register arrays its kick entry binds: the main array and a
//! four-entry array that writes the operand-state registers with their
//! regions exchanged.

use kernel::prelude::*;

use super::kick::{KickTimestamp, RegisterArray, RegisterArrayBinding, RegisterWrite};
use super::queue::CommandTag;
use super::{
    clear, offset_va, sampler_max, stamp_value, timestamp_end_va, work_key, ObjectIds, UNK_0A599,
    UNK_0D411, UNK_101D9, UNK_10791, UNK_1A0E9, UNK_1C8F8,
};

/// Size of a compute descriptor.
pub(crate) const COMPUTE_DESCRIPTOR_SIZE: usize = 0x4000;
/// QoS class of compute kicks.
pub(crate) const COMPUTE_QOS_CLASS: u8 = 0x08;
/// Event mask of compute kicks.
pub(crate) const COMPUTE_KICK_EVENT_MASK: [u64; 4] = [0, 0, 4, 0];

const COMPUTE_REGISTERS: usize = 40;
const ALT_REGISTERS: usize = 4;

/// Object IDs of every compute kick.
const OBJECT_IDS: ObjectIds = ObjectIds {
    predecessor: 0x0100_01d7,
    current: 0x0200_01dc,
};
/// Values of the first and second write of register 0x1a440.
const UNK_1A440: u64 = 0x0000_0001_5402_4201;
const UNK_1A440_FINAL: u64 = 0x0000_0001_5402_4209;
/// Value of register 0x1a458: [`UNK_1C8F8`] below bits 31:20 = 0x10c.
const UNK_1A458: u64 = 0x10c0_0000 | UNK_1C8F8;

/// Offsets in the preempt buffer of the four state words the program names.
const PREEMPT_STATE: [u64; 4] = [0x1480, 0x1488, 0x1490, 0x1498];
/// Offsets in the operand-state buffer of the regions the program names.
const OPERAND_STATE_10229: u64 = 0x12800;
const OPERAND_STATE_140A8: u64 = 0x13000;
const OPERAND_STATE_10099: u64 = 0x9405;
const OPERAND_STATE_10091: u64 = 0x12400;
const OPERAND_STATE_0A5C1: u64 = 0x0005;
const OPERAND_STATE_0A5C9: u64 = 0x9000;

/// Checkpoints into the main register array, as entry counts: after the
/// first 0x0a5c9 write, after the final 0x1a440 write (twice), and the full
/// count.
const CHECKPOINTS: [u16; 4] = [38, 39, 39, COMPUTE_REGISTERS as u16];

/// Inputs of one compute descriptor.
pub(crate) struct ComputeArgs {
    /// Base of the queue's USC window.
    pub(crate) usc_base: u64,
    /// GPU address of the control stream.
    pub(crate) cdm_va: u64,
    /// GPU address of the control-stream terminator.
    pub(crate) cdm_end_va: u64,
    /// GPU address of the sampler heap, or zero.
    pub(crate) sampler_heap_va: u64,
    /// Number of samplers in the heap.
    pub(crate) sampler_count: u32,
    /// GPU address of this descriptor, as seen by the work's VM.
    pub(crate) descriptor_va: u64,
    /// UAT context the work runs in.
    pub(crate) context_id: u16,
    /// Allocation generation of that context.
    pub(crate) context_generation: u8,
    /// State word of the logical queue's work-state node.
    pub(crate) work_state: u32,
    /// GPU address of the logical queue's work-state node.
    pub(crate) work_state_va: u64,
    /// Queue the work is kicked on.
    pub(crate) qid: u8,
    /// Number of earlier kicks of the queue.
    pub(crate) kick_count: u32,
    /// Timestamp the work is kicked at.
    pub(crate) kick: KickTimestamp,
    /// GPU address of the preempt buffer, as seen by the work's VM.
    pub(crate) preempt_va: u64,
    /// GPU address of the operand-state buffer, as seen by the work's VM.
    pub(crate) operand_state_va: u64,
    /// GPU address of the private-memory usage record, as seen by the work's
    /// VM.
    pub(crate) usage_va: u64,
    /// Firmware alias of the private-memory usage record.
    pub(crate) usage_fw_va: u64,
    /// Buffer slot of the USC private-memory free list.
    pub(crate) free_list_slot: u32,
    /// GPU address of the free list's shared control object.
    pub(crate) free_list_control_va: u64,
    /// GPU address the completion stamp value is stored to.
    pub(crate) stamp_va: u64,
    /// GPU address of the word following the completion stamp.
    pub(crate) aux_stamp_va: u64,
    /// GPU addresses of the two status words.
    pub(crate) status_va: [u64; 2],
    /// GPU address of the start/end timestamp pair.
    pub(crate) timestamp_va: u64,
}

fn compute_registers(args: &ComputeArgs) -> Result<[RegisterWrite; COMPUTE_REGISTERS]> {
    let preempt = args.preempt_va;
    let operand = args.operand_state_va;
    let key = work_key(args.context_id, args.work_state);
    let ids = OBJECT_IDS.word();

    Ok([
        RegisterWrite::new(0x1a510, preempt),
        RegisterWrite::new(0x1a420, args.cdm_va),
        RegisterWrite::new(0x1a4d0, offset_va(preempt, PREEMPT_STATE[0])?),
        RegisterWrite::new(0x1a4d8, offset_va(preempt, PREEMPT_STATE[1])?),
        RegisterWrite::new(0x1a4e0, offset_va(preempt, PREEMPT_STATE[2])?),
        RegisterWrite::new(0x1a4e8, offset_va(preempt, PREEMPT_STATE[3])?),
        RegisterWrite::new(0x10071, args.usc_base),
        // Helper program binary, data and configuration: none.
        RegisterWrite::new(0x11841, 0),
        RegisterWrite::new(0x11849, 0),
        RegisterWrite::new(0x11f81, 0),
        RegisterWrite::new(0x1a440, UNK_1A440),
        RegisterWrite::new(0x1a458, UNK_1A458),
        RegisterWrite::new(0x101d9, UNK_101D9),
        RegisterWrite::new(0x1a089, 0),
        RegisterWrite::new(0x1a091, 0),
        RegisterWrite::new(0x1a059, 0),
        RegisterWrite::new(0x1a061, 0),
        RegisterWrite::new(0x1a0b9, 0),
        RegisterWrite::new(0x1a0c1, 0),
        RegisterWrite::new(0x101d1, 0),
        RegisterWrite::new(0x0d479, 0),
        RegisterWrite::new(0x1a0e9, UNK_1A0E9),
        RegisterWrite::new(0x107a1, UNK_10791),
        RegisterWrite::new(0x0a599, UNK_0A599),
        RegisterWrite::new(0x0d411, UNK_0D411),
        RegisterWrite::new(0x1a540, ids),
        RegisterWrite::new(0x014a9, ids),
        RegisterWrite::new(0x0a351, ids),
        RegisterWrite::new(0x10201, key),
        RegisterWrite::new(0x10428, key),
        RegisterWrite::new(0x14028, args.free_list_slot as u64),
        RegisterWrite::new(0x14070, args.usage_va | 1),
        RegisterWrite::new(0x10229, offset_va(operand, OPERAND_STATE_10229)?),
        RegisterWrite::new(0x140a8, offset_va(operand, OPERAND_STATE_140A8)?),
        RegisterWrite::new(0x10099, offset_va(operand, OPERAND_STATE_10099)?),
        RegisterWrite::new(0x10091, offset_va(operand, OPERAND_STATE_10091)?),
        RegisterWrite::new(0x0a5c1, offset_va(operand, OPERAND_STATE_0A5C1)?),
        RegisterWrite::new(0x0a5c9, offset_va(operand, OPERAND_STATE_0A5C9)?),
        RegisterWrite::new(0x1a440, UNK_1A440_FINAL),
        RegisterWrite::new(0x0a599, UNK_0A599),
    ])
}

/// The second array exchanges the operand-state regions of registers
/// 0x10099/0x0a5c1 and 0x10091/0x0a5c9.
fn alt_registers(args: &ComputeArgs) -> Result<[RegisterWrite; ALT_REGISTERS]> {
    let operand = args.operand_state_va;
    Ok([
        RegisterWrite::new(0x10099, offset_va(operand, OPERAND_STATE_0A5C1)?),
        RegisterWrite::new(0x10091, offset_va(operand, OPERAND_STATE_0A5C9)?),
        RegisterWrite::new(0x0a5c1, offset_va(operand, OPERAND_STATE_10099)?),
        RegisterWrite::new(0x0a5c9, offset_va(operand, OPERAND_STATE_10091)?),
    ])
}

/// Compute work descriptor (tag 3).
#[repr(C, packed)]
#[derive(Debug)]
pub(crate) struct ComputeDescriptor {
    /// [`CommandTag::Compute`].
    pub(crate) tag: u32,
    pub(crate) unk_04: u64,
    /// UAT context of the work.
    pub(crate) context_id: u32,
    /// GPU address of the logical queue's work-state node.
    pub(crate) work_state_va: u64,
    /// Checkpoints into the main register array.
    pub(crate) checkpoints: [u16; 4],
    pub(crate) unk_20: [u8; 0x20],
    /// Main register array.
    pub(crate) registers: RegisterArray,
    /// Second register array.
    pub(crate) alt_registers: RegisterArray,
    pub(crate) unk_e80: [u8; 0x58],
    /// GPU address of the preempt buffer.
    pub(crate) preempt_va: u64,
    /// GPU address of the control-stream terminator.
    pub(crate) cdm_end_va: u64,
    pub(crate) unk_ee8: [u8; 0x20],
    /// First value of register 0x1a440.
    pub(crate) unk_f08: u64,
    pub(crate) unk_f10: [u8; 0x10],
    /// Predecessor object ID.
    pub(crate) predecessor_id: u32,
    pub(crate) unk_f24: u32,
    pub(crate) unk_f28: u32,
    /// GPU address of the sampler heap.
    pub(crate) sampler_heap_va: u64,
    /// Number of samplers.
    pub(crate) sampler_count: u32,
    /// Number of samplers plus one, or zero without samplers.
    pub(crate) sampler_max: u32,
    pub(crate) unk_f3c: u32,
    /// GPU address the firmware stores `stamp_value` to on completion.
    pub(crate) stamp_va: u64,
    /// GPU address of the word following the completion stamp.
    pub(crate) aux_stamp_va: u64,
    /// Completion stamp value of the kick timestamp.
    pub(crate) stamp_value: u32,
    /// QID of the compute queue.
    pub(crate) qid: u32,
    pub(crate) unk_f58: u32,
    pub(crate) unk_f5c: u32,
    /// Kick timestamp.
    pub(crate) kick_timestamp: u64,
    /// Current object ID.
    pub(crate) current_id: u64,
    /// Number of earlier kicks of the queue.
    pub(crate) kick_count: u32,
    pub(crate) unk_f74: [u8; 8],
    /// GPU addresses of the two status words.
    pub(crate) status_va: [u64; 2],
    /// GPU address the start timestamp is written to.
    pub(crate) timestamp_start_va: u64,
    /// GPU address the end timestamp is written to.
    pub(crate) timestamp_end_va: u64,
    pub(crate) unk_f9c: [u8; 0x14],
    pub(crate) unk_fb0: u16,
    /// GPU address of the USC free list's shared control object.
    pub(crate) free_list_control_va: u64,
    pub(crate) unk_fba: u32,
    pub(crate) unk_fbe: [u8; 7],
    pub(crate) unk_fc5: u8,
    pub(crate) unk_fc6: [u8; 5],
    /// Firmware alias of the private-memory usage record.
    pub(crate) usage_va: u64,
    /// Allocation generation of the UAT context.
    pub(crate) context_generation: u8,
    pub(crate) unk_fd4: [u8; 0x302c],
}

static_assert!(size_of::<ComputeDescriptor>() == COMPUTE_DESCRIPTOR_SIZE);
static_assert!(core::mem::offset_of!(ComputeDescriptor, registers) == 0x40);
static_assert!(core::mem::offset_of!(ComputeDescriptor, alt_registers) == 0x760);
static_assert!(core::mem::offset_of!(ComputeDescriptor, preempt_va) == 0xed8);
static_assert!(core::mem::offset_of!(ComputeDescriptor, unk_f08) == 0xf08);
static_assert!(core::mem::offset_of!(ComputeDescriptor, predecessor_id) == 0xf20);
static_assert!(core::mem::offset_of!(ComputeDescriptor, sampler_heap_va) == 0xf2c);
static_assert!(core::mem::offset_of!(ComputeDescriptor, stamp_va) == 0xf40);
static_assert!(core::mem::offset_of!(ComputeDescriptor, kick_timestamp) == 0xf60);
static_assert!(core::mem::offset_of!(ComputeDescriptor, current_id) == 0xf68);
static_assert!(core::mem::offset_of!(ComputeDescriptor, status_va) == 0xf7c);
static_assert!(core::mem::offset_of!(ComputeDescriptor, free_list_control_va) == 0xfb2);
static_assert!(core::mem::offset_of!(ComputeDescriptor, usage_va) == 0xfcb);
static_assert!(core::mem::offset_of!(ComputeDescriptor, context_generation) == 0xfd3);

// SAFETY: `ComputeDescriptor` consists of integers and arrays of integers only.
unsafe impl Zeroable for ComputeDescriptor {}

impl ComputeDescriptor {
    const REGISTERS_OFFSET: u64 = core::mem::offset_of!(Self, registers) as u64;
    const ALT_REGISTERS_OFFSET: u64 = core::mem::offset_of!(Self, alt_registers) as u64;
    const UNK_F28: u32 = u32::MAX;
    const UNK_FB0: u16 = 0x1a;
    const UNK_FBA: u32 = 0xe0a0_0001;
    const UNK_FC5: u8 = 0x9f;

    /// Writes the complete descriptor.
    pub(crate) fn write(&mut self, args: &ComputeArgs) -> Result {
        if args.cdm_end_va < args.cdm_va
            || (args.sampler_heap_va == 0) != (args.sampler_count == 0)
            || args.sampler_heap_va & 7 != 0
        {
            return Err(EINVAL);
        }
        let registers = compute_registers(args)?;
        let alt_registers = alt_registers(args)?;
        let registers_va = offset_va(args.descriptor_va, Self::REGISTERS_OFFSET)?;
        let alt_registers_va = offset_va(args.descriptor_va, Self::ALT_REGISTERS_OFFSET)?;
        let timestamp_end_va = timestamp_end_va(args.timestamp_va)?;
        let sampler_max = sampler_max(args.sampler_count)?;

        clear(self);
        self.tag = CommandTag::Compute as u32;
        self.context_id = args.context_id.into();
        self.work_state_va = args.work_state_va;
        self.checkpoints = CHECKPOINTS;
        self.registers.set(registers_va, &registers)?;
        self.alt_registers.set(alt_registers_va, &alt_registers)?;
        self.preempt_va = args.preempt_va;
        self.cdm_end_va = args.cdm_end_va;
        self.unk_f08 = UNK_1A440;
        self.predecessor_id = OBJECT_IDS.predecessor;
        self.unk_f28 = Self::UNK_F28;
        self.sampler_heap_va = args.sampler_heap_va;
        self.sampler_count = args.sampler_count;
        self.sampler_max = sampler_max;
        self.stamp_va = args.stamp_va;
        self.aux_stamp_va = args.aux_stamp_va;
        self.stamp_value = stamp_value(args.kick.get() as u32);
        self.qid = args.qid.into();
        self.kick_timestamp = args.kick.get();
        self.current_id = OBJECT_IDS.current.into();
        self.kick_count = args.kick_count;
        self.status_va = args.status_va;
        self.timestamp_start_va = args.timestamp_va;
        self.timestamp_end_va = timestamp_end_va;
        self.unk_fb0 = Self::UNK_FB0;
        self.free_list_control_va = args.free_list_control_va;
        self.unk_fba = Self::UNK_FBA;
        self.unk_fc5 = Self::UNK_FC5;
        self.usage_va = args.usage_fw_va;
        self.context_generation = args.context_generation;
        Ok(())
    }

    /// Returns the register arrays a compute kick binds; `descriptor_va` is
    /// the descriptor's address as seen by the work's VM.
    pub(crate) fn register_bindings(
        descriptor_va: u64,
    ) -> Result<[Option<RegisterArrayBinding>; 4]> {
        let registers = RegisterArrayBinding::new(
            offset_va(descriptor_va, Self::REGISTERS_OFFSET)?,
            CHECKPOINTS[0] as u8,
        )?;
        let alt_registers = RegisterArrayBinding::new(
            offset_va(descriptor_va, Self::ALT_REGISTERS_OFFSET)?,
            ALT_REGISTERS as u8,
        )?;
        Ok([Some(registers), Some(alt_registers), None, None])
    }
}

/// One retained queue-context record. The descriptor pointer is refreshed for each compute
/// publication; the remaining fields are installed once with the queue.
#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct QueueContextItem {
    unk_00: u64,
    unk_08: u64,
    descriptor_va: u64,
    queue_va: u64,
    unk_20: u64,
    qid: u64,
    unk_30: [u8; 0x100],
    unk_130: u64,
    unk_138: u64,
    unk_140: [u8; 0x10],
    unk_150: u64,
    unk_158: u64,
    unk_160: [u8; 0x18],
    unk_178: u64,
    unk_180: [u8; 0x80],
}
impl QueueContextItem {
    pub(crate) fn new(descriptor_va: u64, queue_va: u64, qid: u8) -> Self {
        Self {
            unk_00: 0x1000_1000_0000_0004,
            unk_08: 0,
            descriptor_va,
            queue_va,
            unk_20: 0xffff_0801_0000_0001,
            qid: u64::from(qid) << 40,
            unk_30: [0; 0x100],
            unk_130: 0,
            unk_138: 4,
            unk_140: [0; 0x10],
            unk_150: 0x0001_1003_8001_a002,
            unk_158: 0x0000_2003_8001_a03b,
            unk_160: [0; 0x18],
            unk_178: 0x003f_ffff_ffff_ffff,
            unk_180: [0; 0x80],
        }
    }
}
static_assert!(size_of::<QueueContextItem>() == 0x200);
