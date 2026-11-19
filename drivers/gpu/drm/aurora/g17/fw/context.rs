// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Scheduler and timestamp-queue state shared with firmware. Only a freshly
//! leased node may be initialized. After publication, the host writes its
//! separate submitted counter and reads the node completion word individually.

use kernel::prelude::*;

#[repr(C)]
pub(crate) struct Scheduler {
    unk0: u64,
    unk8: [u64; 3],
    unk20: u64,
    unk28: u64,
    unk30: u64,
    unk38: [u8; 0x3fc8],
}
impl Scheduler {
    /// Called on zeroed storage before its first publication.
    pub(crate) fn init(&mut self) {
        self.unk0 = 0x0000_0100_0000_ffff;
        self.unk20 = 0x0002_0000_0000_0000;
        self.unk30 = 0x0000_0000_ff00_0000;
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct WorkNode {
    pub(crate) submitted: u64,
    pub(crate) word: u32,
    pub(crate) completed: u32,
    pub(crate) threshold: u32,
    unk14: [u32; 45],
}
impl WorkNode {
    pub(crate) fn new(submitted: u64, word: u32) -> Option<Self> {
        if submitted == 0 || submitted & 3 != 0 || word & 63 == 63 {
            return None;
        }
        Some(Self {
            submitted,
            word,
            completed: 0,
            threshold: 0x50,
            unk14: [0; 45],
        })
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct WorkHead {
    pub(crate) next: u64,
    pub(crate) previous: u64,
    pub(crate) unk10: u32,
    pub(crate) unk14: u32,
}

const _: () = {
    assert!(core::mem::size_of::<Scheduler>() == 0x4000);
    assert!(core::mem::size_of::<WorkNode>() == 0xc8);
    assert!(core::mem::offset_of!(WorkNode, completed) == 0xc);
    assert!(core::mem::size_of::<WorkHead>() == 0x18);
};

// SAFETY: These shared records contain only integer fields and arrays.
unsafe impl Zeroable for Scheduler {}
// SAFETY: WorkNode contains only integers; zero is valid for every field.
unsafe impl Zeroable for WorkNode {}
// SAFETY: WorkHead contains only integer addresses and words.
unsafe impl Zeroable for WorkHead {}
