// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713/G16G 25G83 compute packet layout. All offsets are bytes.

pub(crate) const WORK_SIZE: usize = 0x2000;
/// Active head span of a compute work image: header (`0x00..0x18`) plus up to 29 register
/// records (`0x20..0x17c`, including optional 0x12091, USC base and RT/SCS records), 16-byte
/// aligned. Bytes between `HEAD_ACTIVE_SIZE` and `BODY_ACTIVE_START` remain zeroed after a full
/// write.
pub(crate) const HEAD_ACTIVE_SIZE: usize = 0x180;
/// First active body byte of a compute work image (`work_gva + 0x20` pointer at `0x720`).
pub(crate) const BODY_ACTIVE_START: usize = 0x720;
/// Exclusive end of the active body span of a compute work image (`0xf00..0xf08` tail).
/// Bytes `BODY_ACTIVE_END..WORK_SIZE` remain zeroed after a full write.
pub(crate) const BODY_ACTIVE_END: usize = 0xf08;
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
/// u32 of the StartCompute command: nonzero when the work's register list enables shader-context
/// scratch (SCS). The firmware then records the work's slot as an SCS user, appends
/// `0x017d9 = 1` to the kick's register list, and keeps the SCS state (`0x017e0`, the compute
/// and TA windows) through its idle teardown while the slot is busy.
pub(crate) const START_SCS: usize = 0x40;
pub(crate) const EVENT: u32 = 15;
/// Firmware completion events for concurrent compute pipes `0..COMPUTE_PIPES`.
/// All events reside in type-1 event word 1 (`15..32`) so event draining reads only words 1 and 4.
pub(crate) const EVENTS: [u32; 16] = [
    EVENT, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
];
const _: () = assert!(EVENT as usize + EVENTS.len() <= 32);

/// Resolves a firmware compute completion event back to its compute pipe index (`0..EVENTS.len()`).
pub(crate) const fn pipe_of_event(event: u32) -> Option<usize> {
    if event >= EVENT && (event - EVENT) < EVENTS.len() as u32 {
        Some((event - EVENT) as usize)
    } else {
        None
    }
}
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
    /// Optional `(start, end)` u64 tick offsets inside the work object when the firmware writes
    /// compute GPU timestamps into work rather than the unit's ITS pair.
    pub(crate) work_ticks: Option<(usize, usize)>,
    /// Offset of the four preemption saved-state pointers (`0x1a4d0..0x1a4e8`) in the
    /// preemption buffer.
    pub(crate) preempt_tail: u64,
    /// Whether to emit G15 register `0x12091 = 0x1c` immediately before `0x101d9`.
    pub(crate) has_reg_12091: bool,
    /// Whether to emit HAL200 USC-base register `0x10071`.
    pub(crate) has_usc_base_reg: bool,
    /// Four sub-offsets inside `context_store_gva` written at `StartCompute +0x188..+0x1a0`.
    pub(crate) context_store_offsets: [u64; 4],
    /// Pool threshold words written at compute work `+0x873` and `+0x87b`.
    pub(crate) pool_words: [u64; 2],
}

pub(crate) const V25G83: Layout = Layout {
    generation_bytes: 1,
    completion: 0x88c,
    auxiliary: 0x89c,
    target: 0x8ac,
    cached: 0x8c0,
    work_ticks: None,
    preempt_tail: 0x1480,
    has_reg_12091: false,
    has_usc_base_reg: true,
    context_store_offsets: [0, 0xf400, 0x1e800, 0x1f000],
    pool_words: [0x1588000, 0xef8000],
};

pub(crate) const V26B123: Layout = Layout {
    generation_bytes: 8,
    completion: 0x893,
    auxiliary: 0x8a3,
    target: 0x8b3,
    cached: 0x8e0,
    work_ticks: Some((0x7ec, 0x7f4)),
    preempt_tail: 0x1480,
    has_reg_12091: false,
    has_usc_base_reg: true,
    context_store_offsets: [0, 0xf400, 0x1e800, 0x1f000],
    pool_words: [0x1588000, 0xef8000],
};

pub(crate) const V26B123_G15S: Layout = Layout {
    generation_bytes: 8,
    completion: 0x893,
    auxiliary: 0x8a3,
    target: 0x8b3,
    cached: 0x8e0,
    work_ticks: Some((0x7ec, 0x7f4)),
    preempt_tail: 0x1b80,
    has_reg_12091: true,
    has_usc_base_reg: false,
    context_store_offsets: [0, 0x1e400, 0x3c800, 0x3d800],
    pool_words: [0x31cc000, 0x2308000],
};

const fn valid(l: &Layout) -> bool {
    l.completion == GENERATION + l.generation_bytes
        && l.auxiliary == l.completion + 0x10
        && l.target == l.completion + 0x20
        && l.target + 4 <= l.cached
        && l.cached <= SEQ_EMBEDDED
        && SEQ_EMBEDDED + SEQ_SIZE <= NOTIFIER
        && l.preempt_tail + 0x20 <= PREEMPT_SIZE as u64
        && l.context_store_offsets[3] + 0x1000 <= CONTEXT_STORE_SIZE as u64
}
const _: () = assert!(
    valid(&V25G83)
        && valid(&V26B123)
        && valid(&V26B123_G15S)
        && REGS + (SEL.len() + 5) * 12 <= HEAD_ACTIVE_SIZE
        && HEAD_ACTIVE_SIZE <= BODY_ACTIVE_START
        && BODY_ACTIVE_START <= GENERATION
        && DESCRIPTOR + DESCRIPTOR_SIZE <= BODY_ACTIVE_END
        && 0xf08 <= BODY_ACTIVE_END
        && BODY_ACTIVE_END <= WORK_SIZE
);

pub(crate) const SEL: [u32; 24] = [
    0x1a510, 0x1a420, 0x1a4d0, 0x1a4d8, 0x1a4e0, 0x1a4e8, 0x1a440, 0x1a458, 0x101d9, 0x1a089,
    0x1a091, 0x1a059, 0x1a061, 0x1a0b9, 0x1a0c1, 0x101d1, 0xd479, 0x1a0e9, 0x107a1, 0xa599, 0xd411,
    0x1a540, 0x14a9, 0xa351,
];
