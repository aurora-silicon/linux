// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Context-local work-state identities. The low six bits address a node;
//! the complete sequence distinguishes its owner until exact retirement.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc};

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
    /// Slots retired without post-maintenance completion stamps (for example
    /// stampless render groups released via pair-wide drain or recovery-dropped
    /// groups). A pinned slot stays out of `allocate` for the rest of this
    /// context's life so the host never rewrites a cached work node the firmware
    /// may still hold dirty; `live[index]` returns to `FREE` so `older_live`
    /// never stalls behind it.
    pinned: AtomicU64,
    /// Nodes taken by each engine's admissions and not yet released.
    held: [AtomicU32; ENGINES],
    /// Indices claimed by every context sharing this one's execution context
    /// (`super::super::exec_share`), whose work keys would otherwise collide.
    claims: Option<Arc<AtomicU64>>,
}
impl WorkStates {
    pub(super) const fn new() -> Self {
        Self {
            // Start at generation 1: macOS 27.0 never uses generation 0 (its words run
            // 0x17c8.. 0x17ea on the J813).
            next: AtomicU32::new(0x40),
            live: [const { AtomicU32::new(FREE) }; 64],
            pinned: AtomicU64::new(0),
            held: [const { AtomicU32::new(0) }; ENGINES],
            claims: None,
        }
    }
    /// Work states whose indices are unique among `claims`' sharers.
    pub(super) fn shared(claims: Arc<AtomicU64>) -> Self {
        Self { claims: Some(claims), ..Self::new() }
    }
    /// Every slot of this context is pinned out of reuse, so no future release
    /// can ever free one.
    pub(super) fn exhausted(&self) -> bool {
        let mask = (1u64 << STATE_COUNT) - 1;
        (self.pinned.load(Ordering::Acquire) & mask).count_ones() as usize >= STATE_COUNT
    }
    /// Takes a node for `engine`, or none when [`may_take`] refuses. Callers serialize
    /// allocations of one context; releases may run concurrently.
    pub(super) fn allocate(&self, engine: usize) -> Option<u32> {
        let held = self.held.get(engine)?;
        if !may_take(STATE_COUNT.saturating_sub(self.used()), held.load(Ordering::Acquire)) {
            return None;
        }
        let pinned = self.pinned.load(Ordering::Acquire);
        for _ in 0..64 {
            let word = self.next.fetch_add(1, Ordering::Relaxed);
            let index = (word & 63) as usize;
            if index != STATE_COUNT
                && (pinned & (1u64 << index)) == 0
                && self.live[index]
                    .compare_exchange(FREE, word, Ordering::AcqRel, Ordering::Acquire)
                    .is_ok()
            {
                if (self.pinned.load(Ordering::Acquire) & (1u64 << index)) != 0 {
                    self.live[index].store(FREE, Ordering::Release);
                    continue;
                }
                if let Some(claims) = &self.claims {
                    if claims.fetch_or(1 << index, Ordering::AcqRel) & (1 << index) != 0 {
                        self.live[index].store(FREE, Ordering::Release);
                        continue;
                    }
                }
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
            if let Some(claims) = &self.claims {
                claims.fetch_and(!(1 << index), Ordering::AcqRel);
            }
            if let Some(held) = self.held.get(engine) {
                held.fetch_sub(1, Ordering::AcqRel);
            }
        }
        released
    }
    /// Retires `word` while keeping its slot index out of reuse for the rest of
    /// this context's life (cross-context pool reuse is separately prevented by
    /// `Context::retain_linked_work`).
    pub(super) fn pin(&self, word: u32, engine: usize) -> bool {
        let index = (word & 63) as usize;
        if index == STATE_COUNT || self.live[index].load(Ordering::Acquire) != word {
            return false;
        }
        let mask = 1u64 << index;
        // Set `pinned` before clearing `live[index]` so a concurrent `allocate`
        // cannot observe `FREE` without also observing the pin bit.
        self.pinned.fetch_or(mask, Ordering::AcqRel);
        let pinned = self.live[index]
            .compare_exchange(word, FREE, Ordering::AcqRel, Ordering::Acquire)
            .is_ok();
        if pinned {
            if let Some(claims) = &self.claims {
                claims.fetch_and(!mask, Ordering::AcqRel);
            }
            if let Some(held) = self.held.get(engine) {
                held.fetch_sub(1, Ordering::AcqRel);
            }
        }
        pinned
    }
}

/// How far past the oldest waiting ioctl a new ioctl may be ordered: the ordinals past the
/// turn remembered for ioctls that left early.
pub(super) const ORDER_WINDOW: u64 = 1024;

/// The order in which a context's ioctls take work-state nodes.
///
/// Each engine admits a context's ioctls in submission order, but the engines advance
/// independently: render admission can take nodes for ioctls far ahead of an older ioctl still
/// waiting for its turn on the compute queue. Under deep mixed load (128 compute and 64 render
/// ioctls in flight on one queue) the firmware then stops completing all of the context's work
/// within seconds. Native macOS schedules a queue's command buffers in commit order across
/// engines, at most 36 between scheduling and completion, and the same load runs there without
/// failure.
///
/// So an ioctl takes its node only at its turn, once every older ioctl of its context has taken
/// one or left without one. The firmware sorts a context's nodes by their words and flushes
/// from the head, and a node's word is drawn when the node is taken
/// ([`WorkStates::allocate`]): an ioctl taking its node ahead of an older one would hold a lower
/// word than that ioctl, whose completion stamp would then wait behind a node submitted after
/// it. Taking nodes strictly in turn keeps the words in submission order. Each engine admits
/// from its own scheduler, so the turn costs a scheduler round trip whenever the other engine's
/// ioctl is ready first.
pub(super) struct Order {
    /// The next ordinal to hand out.
    next: u64,
    /// The oldest ordinal that has neither taken a node nor left.
    turn: u64,
    /// Ordinals past the turn that left, as bits at ordinal % ORDER_WINDOW.
    done: [u64; (ORDER_WINDOW / 64) as usize],
    /// An ioctl was refused since the turn last moved: its publisher waits for the next move.
    refused: bool,
}
impl Order {
    pub(super) const fn new() -> Self {
        Self {
            next: 0,
            turn: 0,
            done: [0; (ORDER_WINDOW / 64) as usize],
            refused: false,
        }
    }
    /// The ordinal of a new ioctl, in submission order. None while [`ORDER_WINDOW`] ioctls of
    /// the context are past the oldest one still waiting for its node.
    pub(super) fn assign(&mut self) -> Option<u64> {
        if self.next - self.turn >= ORDER_WINDOW {
            return None;
        }
        let ordinal = self.next;
        self.next += 1;
        Some(ordinal)
    }
    /// Whether the ioctl `ordinal` may take its node now: only at its turn. A refusal is
    /// remembered until the turn next moves, so that move wakes the refused publisher.
    pub(super) fn admits(&mut self, ordinal: u64) -> bool {
        let admits = ordinal == self.turn;
        self.refused |= !admits;
        admits
    }
    /// The ioctl `ordinal` took its node at its turn. Returns whether a refused ioctl may now be
    /// admitted.
    pub(super) fn admit(&mut self, ordinal: u64) -> bool {
        self.finish(ordinal)
    }
    /// The ioctl `ordinal` left without taking a node (it failed or was cancelled first).
    /// Returns whether a refused ioctl may now be admitted.
    pub(super) fn leave(&mut self, ordinal: u64) -> bool {
        self.finish(ordinal)
    }
    fn finish(&mut self, ordinal: u64) -> bool {
        if ordinal < self.turn {
            return false;
        }
        if ordinal == self.turn {
            self.pass();
            return core::mem::take(&mut self.refused);
        }
        // assign() keeps every ordinal within the window past the turn.
        let bit = ordinal % ORDER_WINDOW;
        self.done[(bit / 64) as usize] |= 1 << (bit % 64);
        false
    }
    /// Moves the turn past the current ordinal and every following one already done.
    fn pass(&mut self) {
        loop {
            self.turn += 1;
            let bit = self.turn % ORDER_WINDOW;
            let (word, mask) = ((bit / 64) as usize, 1u64 << (bit % 64));
            if self.done[word] & mask == 0 {
                break;
            }
            self.done[word] &= !mask;
        }
    }
}

/// Two stable logical ordinals (render channels), independent of physical pair numbering. A
/// context's render work can move between physical pairs (sibling pairs, lending, relocation,
/// QID recycling), so a channel is rebound to a new pair once every publication this context
/// made through it has retired. A context's channel counts its passes up for the whole
/// process, so a rebound channel continues its pass sequence.
pub(super) struct RenderSlots {
    pairs: [Option<u8>; 2],
    origins: [Option<u64>; 2],
    /// Pass of the channel's last publication on its previous pair.
    bases: [u8; 2],
    /// QIDs and visibility target (ordinal + 1) of the last publication made through each
    /// channel.
    last: [Option<([u8; 2], u64)>; 2],
    /// Recency of each channel's last publication.
    used: [u64; 2],
    epoch: u64,
}
impl RenderSlots {
    pub(super) const fn new() -> Self {
        Self {
            pairs: [None; 2],
            origins: [None; 2],
            bases: [0; 2],
            last: [None; 2],
            used: [0; 2],
            epoch: 0,
        }
    }
    fn get(&mut self, pair: u8) -> Option<u32> {
        if let Some(index) = self.pairs.iter().position(|value| *value == Some(pair)) {
            return Some(index as u32);
        }
        let index = self.pairs.iter().position(Option::is_none)?;
        self.pairs[index] = Some(pair);
        Some(index as u32)
    }
    /// The channel of `pair`, rebinding the least recently used channel whose publications
    /// have all retired (`retired(qids, ordinal)`) when both are bound to other pairs. `None`:
    /// no channel can move yet.
    pub(super) fn bind(
        &mut self,
        pair: u8,
        retired: impl Fn([u8; 2], u64) -> bool,
    ) -> Option<(u32, Option<u8>)> {
        if let Some(index) = self.get(pair) {
            return Some((index, None));
        }
        let index = (0..2)
            .filter(|index| self.last[*index].is_none_or(|(qids, ordinal)| retired(qids, ordinal)))
            .min_by_key(|index| self.used[*index])?;
        let previous = self.pairs[index];
        // `last` holds the ordinal after the last publication (its visibility target), so that
        // publication's pass is base + (next - origin).
        if let (Some(origin), Some((_, next))) = (self.origins[index], self.last[index]) {
            self.bases[index] = self.bases[index].wrapping_add(next.wrapping_sub(origin) as u8);
        }
        self.pairs[index] = Some(pair);
        self.origins[index] = None;
        self.last[index] = None;
        Some((index as u32, previous))
    }
    pub(super) fn payload(&mut self, pair: u8, ordinal: u64) -> Option<u8> {
        let index = self.pairs.iter().position(|value| *value == Some(pair))?;
        let origin = *self.origins[index].get_or_insert(ordinal);
        Some(self.bases[index].wrapping_add(ordinal.checked_sub(origin)?.wrapping_add(1) as u8))
    }
    /// Records a publication that reached firmware through `pair`'s channel.
    pub(super) fn published(&mut self, pair: u8, qids: [u8; 2], ordinal: u64) -> bool {
        let Some(index) = self.pairs.iter().position(|value| *value == Some(pair)) else {
            return false;
        };
        self.epoch = self.epoch.wrapping_add(1);
        self.used[index] = self.epoch;
        self.last[index] = Some((qids, ordinal));
        true
    }
}

impl WorkStates {
    /// The oldest node in use that the firmware lists ahead of `word`: a live word older than
    /// it by the signed 32-bit difference the firmware sorts by. Its ioctl was submitted
    /// earlier ([`Order`]), and until that node drains the firmware withholds every stamp of
    /// `word`'s node behind it.
    pub(super) fn older_live(&self, word: u32) -> Option<u32> {
        self.live
            .iter()
            .map(|live| live.load(Ordering::Acquire))
            .filter(|&live| live != FREE && (live.wrapping_sub(word) as i32) < 0)
            .min_by_key(|&live| live.wrapping_sub(word) as i32)
    }

    pub(super) fn used(&self) -> usize {
        let pinned = self.pinned.load(Ordering::Acquire);
        if let Some(claims) = &self.claims {
            return (claims.load(Ordering::Acquire) | pinned).count_ones() as usize;
        }
        self.live
            .iter()
            .enumerate()
            .filter(|&(i, word)| {
                i != STATE_COUNT
                    && (word.load(Ordering::Acquire) != FREE || (pinned & (1u64 << i)) != 0)
            })
            .count()
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
    fn ioctls_take_nodes_only_at_their_turn() {
        let mut order = Order::new();
        let ordinals: [u64; 4] = core::array::from_fn(|_| order.assign().unwrap());
        // Only the oldest waiting ioctl may take its node; the others are refused.
        for &ordinal in &ordinals[1..] {
            assert!(!order.admits(ordinal));
        }
        assert!(order.admits(ordinals[0]));
        // Its admission moves the turn and wakes the refused.
        assert!(order.admit(ordinals[0]));
        assert!(order.admits(ordinals[1]));
        // Nobody was refused since, so the next admission wakes nobody.
        assert!(!order.admit(ordinals[1]));
        assert!(!order.admits(ordinals[3]));
        assert!(order.admits(ordinals[2]));
        assert!(order.admit(ordinals[2]));
        assert!(order.admits(ordinals[3]));
    }

    #[test]
    fn ioctls_leaving_without_a_node_pass_the_turn() {
        let mut order = Order::new();
        let (a, b, c, d) = (
            order.assign().unwrap(),
            order.assign().unwrap(),
            order.assign().unwrap(),
            order.assign().unwrap(),
        );
        // b and c fail before their turn; while a waits, d waits too.
        assert!(!order.leave(b));
        assert!(!order.leave(c));
        assert!(!order.admits(d));
        // Once a takes its node, the turn passes b and c as well, and the refused one may retry.
        assert!(order.admit(a));
        assert!(order.admits(d));
        // An ordinal already passed leaves without effect.
        assert!(!order.leave(a));
    }

    #[test]
    fn no_ioctl_is_ordered_past_the_window() {
        let mut order = Order::new();
        let first = order.assign().unwrap();
        let mut last = first;
        for _ in 1..ORDER_WINDOW {
            last = order.assign().unwrap();
        }
        assert!(order.assign().is_none());
        // An ioctl leaving far ahead is remembered within the window.
        assert!(!order.leave(last));
        assert!(order.admits(first));
        assert!(!order.admit(first));
        assert!(order.assign().is_some());
    }

    #[test]
    fn single_engine_admission_wakes_nobody() {
        let mut order = Order::new();
        for _ in 0..24 {
            let ordinal = order.assign().unwrap();
            assert!(order.admits(ordinal));
            assert!(!order.admit(ordinal));
        }
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

    #[test]
    fn pinned_slot_is_never_reused_and_never_stalls_older_live() {
        let states = WorkStates::new();
        let first = states.allocate(0).unwrap();
        let pinned_index = first & 63;
        let second = states.allocate(0).unwrap();
        assert_eq!(states.older_live(second), Some(first));
        assert!(states.pin(first, 0));
        assert!(!states.pin(first, 0));
        assert_eq!(states.older_live(second), None);
        assert_eq!(states.used(), 2);
        assert_eq!(states.held[0].load(Ordering::Acquire), 1);
        assert!(states.release(second, 0));
        assert_eq!(states.used(), 1);
        assert_eq!(states.held[0].load(Ordering::Acquire), 0);
        // Cycling well past STATE_COUNT never hands back the pinned slot.
        for _ in 0..(STATE_COUNT * 3) {
            let word = states.allocate(0).unwrap();
            assert_ne!(word & 63, pinned_index);
            assert!(states.release(word, 0));
        }
        assert!(!states.exhausted());
        // Pinning the remaining slots exhausts the context cleanly.
        for _ in 1..STATE_COUNT {
            let word = states.allocate(0).unwrap();
            assert!(states.pin(word, 0));
        }
        assert!(states.exhausted());
        assert_eq!(states.used(), STATE_COUNT);
        assert!(states.allocate(0).is_none());
        assert!(states.allocate(1).is_none());
    }
}
