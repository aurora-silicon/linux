// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Completion ownership for the bounded M5 TA/fragment pair.
//!
//! 25G76 GFX 0x17544 compares scheduler.completed with *scheduler.submitted;
//! 0x1763c unlinks the completed scheduler. The submitted total is host-owned,
//! not a firmware-decremented pending count. 0x17a68/0x17ae0 then write the
//! first stamp pointer and 0x17b98 publishes the corresponding event mask.

// The 32-bit completion value reserves its low byte. Stop before it wraps;
// storage-ring wrap is independent of this monotonically increasing stamp.
pub(crate) const MAX_GRAPHICS_GENERATION: u32 = 0x00ff_ffff;
pub(crate) const GRAPHICS_POINTER_COUNT: u32 = 0x500;

pub(crate) const fn graphics_pointer_head(generation: u32) -> u32 {
    (1 + generation * 3) % GRAPHICS_POINTER_COUNT
}

#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct M5GraphicsCompletion {
    pub(crate) submitted: u32,
    pub(crate) completed: u32,
    pub(crate) linked: u32,
    pub(crate) list: [u64; 2],
    pub(crate) stamps: [u32; 2],
    pub(crate) signalled: u128,
}

/// The same native scheduler retirement contract for one compute record.
#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct M5ComputeCompletion {
    pub(crate) submitted: u32,
    pub(crate) completed: u32,
    pub(crate) linked: u32,
    pub(crate) list: [u64; 2],
    pub(crate) stamp: u32,
    pub(crate) signalled: u128,
}

impl M5ComputeCompletion {
    pub(crate) fn complete_at(self, list_address: u64, queue_id: u8, stamp: u32) -> bool {
        list_address != 0 && queue_id < 128 && stamp != 0 && stamp & 0xff == 0
            && self.submitted == 1 && self.completed == 1 && self.linked == 0
            && self.list == [0, list_address] && self.stamp == stamp
            && self.signalled & (1u128 << queue_id) != 0
    }
}

impl M5GraphicsCompletion {

    pub(crate) fn pair_complete_at(self, list_address: u64, stamp: u32) -> bool {
        list_address != 0
            && stamp != 0 && stamp & 0xff == 0
            && self.submitted == 2
            && self.completed == self.submitted
            && self.linked == 0
            && self.list == [0, list_address]
            && self.stamps == [stamp; 2]
            && self.signalled & 0xc00 == 0xc00
    }
}

