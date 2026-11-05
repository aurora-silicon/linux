// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Context-local work-state identities. The low six bits address a node;
//! the complete sequence distinguishes its owner until exact retirement.

use core::sync::atomic::{AtomicU32, Ordering};

pub(super) const FREE: u32 = u32::MAX;
pub(super) const STATE_COUNT: usize = 63;
pub(super) const NODE_STRIDE: usize = 0x100;

pub(super) struct WorkStates {
    next: AtomicU32,
    live: [AtomicU32; 64],
}
impl WorkStates {
    pub(super) const fn new() -> Self {
        Self {
            next: AtomicU32::new(0),
            live: [const { AtomicU32::new(FREE) }; 64],
        }
    }
    pub(super) fn allocate(&self) -> Option<u32> {
        for _ in 0..64 {
            let word = self.next.fetch_add(1, Ordering::Relaxed);
            let index = (word & 63) as usize;
            if index != STATE_COUNT
                && self.live[index]
                    .compare_exchange(FREE, word, Ordering::AcqRel, Ordering::Acquire)
                    .is_ok()
            {
                return Some(word);
            }
        }
        None
    }
    pub(super) fn release(&self, word: u32) -> bool {
        let index = (word & 63) as usize;
        index != STATE_COUNT
            && self.live[index]
                .compare_exchange(word, FREE, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
    }
}

/// Two stable logical ordinals, independent of physical pair numbering.
pub(super) struct RenderSlots {
    pairs: [Option<u8>; 2],
    origins: [Option<u64>; 2],
}
impl RenderSlots {
    pub(super) const fn new() -> Self {
        Self {
            pairs: [None; 2],
            origins: [None; 2],
        }
    }
    pub(super) fn get(&mut self, pair: u8) -> Option<u32> {
        if let Some(index) = self.pairs.iter().position(|value| *value == Some(pair)) {
            return Some(index as u32);
        }
        let index = self.pairs.iter().position(Option::is_none)?;
        self.pairs[index] = Some(pair);
        Some(index as u32)
    }
    pub(super) fn payload(&mut self, pair: u8, ordinal: u64) -> Option<u8> {
        let index = self.get(pair)? as usize;
        let origin = *self.origins[index].get_or_insert(ordinal);
        Some(ordinal.checked_sub(origin)?.wrapping_add(1) as u8)
    }
}
