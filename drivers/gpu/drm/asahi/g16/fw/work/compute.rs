// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713/G16G 25G83 compute packet layout. All offsets are bytes.

pub(crate) const WORK_SIZE: usize = 0x2000;
pub(crate) const SEQ_SIZE: usize = 0x300;
pub(crate) const SEQ_OBJECT: usize = 0x1000;
pub(crate) const REGS: usize = 0x20;
pub(crate) const SEQ_EMBEDDED: usize = 0x900;
pub(crate) const ATTACHMENTS: usize = 0x58;
pub(crate) const ATTACHMENT_STRIDE: usize = 16;
pub(crate) const MAX_ATTACHMENTS: usize = 16;
pub(crate) const TS_START: usize = 0x1bc;
pub(crate) const TS_END: usize = 0x20c;
pub(crate) const TS_SIZE: usize = 0x4c;
pub(crate) const TS_CELLS: u64 = 0x82c;
pub(crate) const FINALIZE: usize = 0x258;
pub(crate) const END: usize = 0x2d4;
pub(crate) const DESCRIPTOR: usize = 0xe00;
pub(crate) const DESCRIPTOR_SIZE: usize = 0xa0;
pub(crate) const DESCRIPTOR_COUNTER: usize = DESCRIPTOR + super::super::graph::manager::COUNTER;
pub(crate) const DESCRIPTOR_COMPLETED: usize = DESCRIPTOR + super::super::graph::manager::COMPLETED;
pub(crate) const NOTIFIER: usize = 0xc00;
pub(crate) const PREEMPT_SIZE: usize = 0x4000;
pub(crate) const AUX_SIZE: usize = 0x4000;
pub(crate) const CONTEXT_STORE_SIZE: usize = 0x40000;
pub(crate) const POOL_SIZE: usize = 0x2000000;
pub(crate) const POOL_STATE_SIZE: usize = 0x20000;
pub(crate) const PAGE_LIST_SIZE: usize = 0x4000;
pub(crate) const EVENT: u32 = 15;
/// The binding generation.
pub(crate) const GENERATION: usize = 0x88b;

/// Compute work fields that move between firmware ABIs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Layout {
    /// Width of the binding generation at [`GENERATION`].
    pub(crate) generation_bytes: usize,
    /// Target of the finalize record's +0x71 pointer.
    pub(crate) completion: usize,
    /// Target of the timestamp records' +0x2c ("auxiliary") pointer.
    pub(crate) auxiliary: usize,
    /// u32 target of the start record's +0x1a8 and the finalize record's +0x69 pointers.
    pub(crate) target: usize,
    /// Bytes of the work the firmware caches; the host's private parts lie beyond them.
    pub(crate) cached: usize,
}

pub(crate) const V25G83: Layout = Layout {
    generation_bytes: 1,
    completion: 0x88c,
    auxiliary: 0x89c,
    target: 0x8ac,
    cached: 0x8c0,
};

pub(crate) const V26B123: Layout = Layout {
    generation_bytes: 8,
    completion: 0x893,
    auxiliary: 0x8a3,
    target: 0x8b3,
    cached: 0x8e0,
};

const fn valid(l: &Layout) -> bool {
    l.completion == GENERATION + l.generation_bytes
        && l.auxiliary == l.completion + 0x10
        && l.target == l.completion + 0x20
        && l.target + 4 <= l.cached
        && l.cached <= SEQ_EMBEDDED
        && SEQ_EMBEDDED + SEQ_SIZE <= NOTIFIER
}
const _: () = assert!(valid(&V25G83) && valid(&V26B123));

pub(crate) const SEL: [u32; 24] = [
    0x1a510, 0x1a420, 0x1a4d0, 0x1a4d8, 0x1a4e0, 0x1a4e8, 0x1a440, 0x1a458, 0x101d9, 0x1a089,
    0x1a091, 0x1a059, 0x1a061, 0x1a0b9, 0x1a0c1, 0x101d1, 0xd479, 0x1a0e9, 0x107a1, 0xa599, 0xd411,
    0x1a540, 0x14a9, 0xa351,
];
