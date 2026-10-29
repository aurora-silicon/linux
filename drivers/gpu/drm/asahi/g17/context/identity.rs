// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Context-local work-state identities. The low six bits address a node;
//! the complete sequence distinguishes its owner until exact retirement.

use core::sync::atomic::{AtomicU32, Ordering};
use kernel::prelude::*;

pub(super) const FREE: u32 = u32::MAX;
pub(super) const STATE_COUNT: usize = 63;
pub(super) const NODE_STRIDE: usize = 0x100;

/// Engines whose admissions take work-state nodes: render (0) and compute (1).
pub(super) const ENGINES: usize = 2;

/// Whether an engine that holds `held` nodes may take one of `free` nodes.
///
/// All engines of one ioctl share its node, and the node returns only once every command of
/// the ioctl has retired. So render admission can keep taking nodes for newer ioctls while
/// their compute commands still wait in the compute queue behind an older ioctl that has no
/// node yet; once render held every node, nothing could retire and both queues stalled. An
/// engine therefore never takes the last free node while it already holds one. The other
/// engine can then always take a node, and an engine refused here waits only for nodes it
/// took itself, for ioctls ahead of its waiting command. Each engine admits a context's
/// ioctls in submission order, so those waits cannot form a cycle.
pub(super) const fn may_take(free: usize, held: u32) -> bool {
    free > 1 || (free == 1 && held == 0)
}

pub(super) struct WorkStates {
    next: AtomicU32,
    live: [AtomicU32; 64],
    /// Nodes taken by each engine's admissions and not yet released.
    held: [AtomicU32; ENGINES],
}
impl WorkStates {
    pub(super) const fn new() -> Self {
        Self {
            next: AtomicU32::new(0),
            live: [const { AtomicU32::new(FREE) }; 64],
            held: [const { AtomicU32::new(0) }; ENGINES],
        }
    }
    /// Takes a node for `engine`, or none when [`may_take`] refuses. Callers serialize
    /// allocations of one context; releases may run concurrently.
    pub(super) fn allocate(&self, engine: usize) -> Option<u32> {
        let held = self.held.get(engine)?;
        if !may_take(STATE_COUNT.saturating_sub(self.used()), held.load(Ordering::Acquire)) {
            return None;
        }
        for _ in 0..64 {
            let word = self.next.fetch_add(1, Ordering::Relaxed);
            let index = (word & 63) as usize;
            if index != STATE_COUNT
                && self.live[index]
                    .compare_exchange(FREE, word, Ordering::AcqRel, Ordering::Acquire)
                    .is_ok()
            {
                held.fetch_add(1, Ordering::AcqRel);
                return Some(word);
            }
        }
        None
    }
    pub(super) fn release(&self, word: u32, engine: usize) -> bool {
        let index = (word & 63) as usize;
        let released = index != STATE_COUNT
            && self.live[index]
                .compare_exchange(word, FREE, Ordering::AcqRel, Ordering::Acquire)
                .is_ok();
        if released {
            if let Some(held) = self.held.get(engine) {
                held.fetch_sub(1, Ordering::AcqRel);
            }
        }
        released
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

impl WorkStates {
    pub(super) fn used(&self) -> usize {
        self.live.iter().filter(|word| word.load(Ordering::Acquire) != FREE).count()
    }
}
impl RenderSlots {
    pub(super) fn used(&self) -> usize { self.pairs.iter().flatten().count() }
}

#[kunit_tests(asahi_g17_work_states)]
mod tests {
    use super::*;

    #[test]
    fn last_node_stays_with_an_engine_holding_none() {
        assert!(!may_take(0, 0));
        assert!(may_take(1, 0));
        assert!(!may_take(1, 3));
        assert!(may_take(2, 3));
    }

    #[test]
    fn render_cannot_starve_compute_of_a_node() {
        let states = WorkStates::new();
        let mut render = KVec::new();
        while let Some(word) = states.allocate(0) {
            render.push(word, GFP_KERNEL).unwrap();
        }
        // Render stops one short; compute takes the last node, then nothing is left.
        assert_eq!(render.len(), STATE_COUNT - 1);
        let compute = states.allocate(1).unwrap();
        assert_eq!(states.used(), STATE_COUNT);
        assert!(states.allocate(0).is_none());
        assert!(states.allocate(1).is_none());
        // A lone free node goes to the engine holding none.
        assert!(states.release(compute, 1));
        assert!(states.allocate(0).is_none());
        let compute = states.allocate(1).unwrap();
        // With both engines holding nodes, a lone free node waits for one of their returns.
        assert!(states.release(render.pop().unwrap(), 0));
        assert!(states.allocate(0).is_none());
        assert!(states.allocate(1).is_none());
        // Two free nodes: render may take one again, never the last.
        assert!(states.release(render.pop().unwrap(), 0));
        assert!(states.allocate(0).is_some());
        assert!(states.allocate(0).is_none());
        assert!(states.release(compute, 1));
        assert!(states.allocate(1).is_some());
    }

    #[test]
    fn releases_return_nodes_to_their_engine() {
        let states = WorkStates::new();
        let word = states.allocate(1).unwrap();
        assert!(states.release(word, 1));
        assert!(!states.release(word, 1));
        assert_eq!(states.used(), 0);
        assert_eq!(states.held[1].load(Ordering::Acquire), 0);
        // An engine outside the table takes nothing.
        assert!(states.allocate(ENGINES).is_none());
    }
}
