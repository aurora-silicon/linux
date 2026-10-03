// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! QoS accounting object. Queue rows and submission counts use distinct queue
//! and logical-owner indices. Firmware may observe each live word separately.

use kernel::prelude::*;

pub(crate) const QUEUES: usize = 128;
pub(crate) const DATA_MASTERS: usize = 4;

#[repr(C)]
pub(crate) struct Qos {
    pub(crate) rows: [[u8; 8]; QUEUES],
    pub(crate) queue_submitted: [u32; QUEUES],
    pub(crate) owner_submitted: [u32; QUEUES],
    pub(crate) scheduler: [u64; QUEUES],
    pub(crate) share: [u32; QUEUES],
    pub(crate) intervals: u64,
    pub(crate) busy_start: [u64; 3],
    pub(crate) dm_submitted: [u64; DATA_MASTERS],
}
// SAFETY: Qos consists entirely of integer fields and arrays.
unsafe impl Zeroable for Qos {}

const _: () = {
    assert!(core::mem::size_of::<Qos>() == 0xe40);
    assert!(core::mem::offset_of!(Qos, queue_submitted) == 0x400);
    assert!(core::mem::offset_of!(Qos, owner_submitted) == 0x600);
    assert!(core::mem::offset_of!(Qos, scheduler) == 0x800);
    assert!(core::mem::offset_of!(Qos, share) == 0xc00);
    assert!(core::mem::offset_of!(Qos, intervals) == 0xe00);
    assert!(core::mem::offset_of!(Qos, busy_start) == 0xe08);
    assert!(core::mem::offset_of!(Qos, dm_submitted) == 0xe20);
};
