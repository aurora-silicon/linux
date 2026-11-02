// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 compute queue encodings (linux-aurora t6050-gpu's g17/fw/m5/queue.rs): the queue record,
//! the current-kick notification and the host command port transaction. The configuration record
//! itself is the common [`QueueConfig`](super::super::queue::QueueConfig).

use super::super::{
    kick::{KickTimestamp, QID_MAX},
    queue::{DataMaster, KickPredecessor, Policy, QueueRecordArgs},
};
use kernel::prelude::*;

/// The M5 queue record. The scheduler pointer moved from the Neo record's 0x9c to 0x70; the
/// policy and owner fields also moved.
#[repr(C, packed)]
#[derive(Clone, Copy)]
pub(crate) struct QueueRecord {
    pointers_va: u64,
    items_va: u64,
    job_list_va: u64,
    reserved_18: [u8; 0xc],
    sentinel_24: u32,
    priority: [u32; 2],
    reserved_30: [u8; 6],
    sentinel_36: u16,
    reserved_38: u32,
    policy: u32,
    sentinel_40: u32,
    owner_pid: u32,
    reserved_48: [u8; 0x28],
    scheduler_va: u64,
    reserved_78: [u8; 8],
}

static_assert!(size_of::<QueueRecord>() == 0x80);
static_assert!(core::mem::offset_of!(QueueRecord, policy) == 0x3c);
static_assert!(core::mem::offset_of!(QueueRecord, owner_pid) == 0x44);
static_assert!(core::mem::offset_of!(QueueRecord, scheduler_va) == 0x70);

impl QueueRecord {
    /// The class-specific words stored on a queue's record, for the profiles
    /// of its four priority classes.
    fn scheduling_words(policy: Policy) -> Result<([u8; 6], u16, u32)> {
        match (policy.priority(), policy.policy()) {
            (0, 1) => Ok(([0, 0, 0xff, 0xff, 0xff, 0xff], u16::MAX, 1)),
            (1, 0) => Ok(([0, 0, 0, 0, 0xff, 0xff], u16::MAX, 0)),
            (2, 2) => Ok(([0; 6], u16::MAX, 0)),
            (3, 0) => Ok(([0; 6], 0, 0)),
            _ => Err(EINVAL),
        }
    }

    /// Builds the record described by `args`.
    pub(crate) fn new(args: &QueueRecordArgs) -> Result<Self> {
        let (reserved_30, sentinel_36, reserved_38) = Self::scheduling_words(args.policy)?;
        Ok(Self {
            pointers_va: args.pointers_va,
            items_va: args.items_va,
            job_list_va: args.job_list_va,
            reserved_18: [0; 0xc],
            sentinel_24: u32::MAX,
            priority: [u32::from(args.policy.priority()); 2],
            reserved_30,
            sentinel_36,
            reserved_38,
            policy: u32::from(args.policy.policy()),
            sentinel_40: u32::MAX,
            owner_pid: args.owner_pid,
            reserved_48: [0; 0x28],
            scheduler_va: args.scheduler_va,
            reserved_78: [0; 8],
        })
    }
}

/// The M5 current-kick notification: the common tag-16 record naming the kick being published
/// (not its predecessor), zero-padded to 24 bytes. The descriptor is reached through the kick
/// entry.
#[repr(C, packed)]
#[derive(Clone, Copy)]
pub(crate) struct Notification {
    record: KickPredecessor,
    reserved: [u8; 6],
}

static_assert!(size_of::<Notification>() == 0x18);

impl Notification {
    /// Names `current`, the kick of queue `qid` being published.
    pub(crate) fn new(qid: u8, current: KickTimestamp) -> Result<Self> {
        Ok(Self {
            record: KickPredecessor::new(qid, current)?,
            reserved: [0; 6],
        })
    }
}

/// A compute queue transaction on the M5 host command port: configuration of the queue at its
/// first use, then the advance that makes the new kick runnable.
#[derive(Clone, Copy)]
pub(crate) struct HardwareSubmission {
    writes: [(usize, u64); 8],
    count: usize,
}

impl HardwareSubmission {
    const TAG_PORT: usize = 0x4090;
    const DATA_PORT: usize = 0x4098;
    const VALID_LOW: usize = 0x21068;
    const VALID_HIGH: usize = 0x21070;
    const INVALID_LOW: usize = 0x21078;
    const INVALID_HIGH: usize = 0x21080;
    const CONFIG_TAG: u64 = 0x0020_0800_0002_1008;
    const QID_SHIFT: u32 = 44;
    /// The advance command's offset from the queue's configuration tag.
    const ADVANCE: u64 = 0x10;
    const ADDRESS_MASK: u64 = 0x0000_07ff_ffff_ffe0;

    /// The transaction of compute queue `qid`, whose kick ring is at `va`, at priority class
    /// `priority`. With `configure` the queue is invalidated, configured and made valid first;
    /// each validity mask is stored high before low.
    pub(crate) fn compute(qid: u8, va: u64, priority: u8, configure: bool) -> Result<Self> {
        let tag = Self::configuration_tag(qid)?;
        if priority >= Policy::PRIORITIES || va & !Self::ADDRESS_MASK != 0 {
            return Err(EINVAL);
        }
        let mut writes = [(0, 0); 8];
        let mut count = 0;
        if configure {
            let (low, high) = if qid < 64 {
                (1u64 << qid, 0)
            } else {
                (0, 1u64 << (qid - 64))
            };
            let completion = (3 * (4 - u64::from(priority)) + 0x3fe).wrapping_shl(54);
            let payload = ((DataMaster::Compute as u64 + 1) << 60) | va | completion;
            writes[..6].copy_from_slice(&[
                (Self::INVALID_HIGH, high),
                (Self::INVALID_LOW, low),
                (Self::TAG_PORT, tag),
                (Self::DATA_PORT, payload),
                (Self::VALID_HIGH, high),
                (Self::VALID_LOW, low),
            ]);
            count = 6;
        }
        writes[count..count + 2]
            .copy_from_slice(&[(Self::TAG_PORT, tag + Self::ADVANCE), (Self::DATA_PORT, 1)]);
        Ok(Self {
            writes,
            count: count + 2,
        })
    }

    /// The tag-port value selecting queue `qid`'s configuration word.
    pub(crate) fn configuration_tag(qid: u8) -> Result<u64> {
        if qid > QID_MAX {
            return Err(EINVAL);
        }
        Ok(Self::CONFIG_TAG | (u64::from(qid) << Self::QID_SHIFT))
    }

    /// The 64-bit register writes, in order.
    pub(crate) fn writes(&self) -> &[(usize, u64)] {
        &self.writes[..self.count]
    }
}
