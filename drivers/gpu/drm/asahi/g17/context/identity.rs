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

/// Work-state words of the contexts sharing one execution context. Commands name that context
/// with a word, so the sharers draw their words from one sequence and no two of their live nodes
/// have the same index (a word's low six bits): an identity and word pair stays unique, as it is
/// for an unshared context.
pub(crate) struct SharedStates {
    next: AtomicU32,
    /// Indices of the sharers' live nodes.
    claims: AtomicU64,
    /// The sharers' live words by index, for [`WorkStates::older_live`].
    words: [AtomicU32; 64],
}
impl SharedStates {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Ok(Arc::new(
            Self {
                next: AtomicU32::new(0x40),
                claims: AtomicU64::new(0),
                words: [const { AtomicU32::new(FREE) }; 64],
            },
            GFP_KERNEL,
        )?)
    }
}

/// The outcome of claiming a node index among the sharers.
enum Claim {
    Taken,
    /// Another sharer holds the index.
    Busy,
    /// [`may_take`] refuses at the claim.
    Refused,
}

pub(super) struct WorkStates {
    next: AtomicU32,
    live: [AtomicU32; 64],
    /// Nodes taken by each engine's admissions and not yet released.
    held: [AtomicU32; ENGINES],
    /// Set when the execution context is shared with other contexts.
    shared: Option<Arc<SharedStates>>,
}
impl WorkStates {
    pub(super) const fn new() -> Self {
        Self {
            // Start at generation 1: macOS 27.0 never uses generation 0 (its words run
            // 0x17c8.. 0x17ea on the J813).
            next: AtomicU32::new(0x40),
            live: [const { AtomicU32::new(FREE) }; 64],
            held: [const { AtomicU32::new(0) }; ENGINES],
            shared: None,
        }
    }
    /// Work states of a context whose execution context `shared` describes.
    pub(super) fn shared(shared: Arc<SharedStates>) -> Self {
        Self {
            shared: Some(shared),
            ..Self::new()
        }
    }
    /// Claims `index` among the sharers for an engine holding `held` nodes; always taken for an
    /// unshared context. Sharers allocate concurrently, so [`may_take`] is decided again on the
    /// claims as they were at this claim: two sharers holding nodes never both take the last
    /// free ones.
    fn claim(&self, index: usize, held: u32) -> Claim {
        let Some(shared) = &self.shared else {
            return Claim::Taken;
        };
        let bit = 1u64 << index;
        let before = shared.claims.fetch_or(bit, Ordering::AcqRel);
        if before & bit != 0 {
            return Claim::Busy;
        }
        if !may_take(STATE_COUNT.saturating_sub(before.count_ones() as usize), held) {
            shared.claims.fetch_and(!bit, Ordering::AcqRel);
            return Claim::Refused;
        }
        Claim::Taken
    }
    fn unclaim(&self, index: usize) {
        if let Some(shared) = &self.shared {
            shared.claims.fetch_and(!(1u64 << index), Ordering::AcqRel);
        }
    }
    /// Takes a node for `engine`, or none when [`may_take`] refuses. Callers serialize
    /// allocations of one context; releases may run concurrently.
    pub(super) fn allocate(&self, engine: usize) -> Option<u32> {
        let held = self.held.get(engine)?;
        if !may_take(STATE_COUNT.saturating_sub(self.used()), held.load(Ordering::Acquire)) {
            return None;
        }
        let next = self.shared.as_ref().map_or(&self.next, |shared| &shared.next);
        for _ in 0..64 {
            let word = next.fetch_add(1, Ordering::Relaxed);
            let index = (word & 63) as usize;
            if index == STATE_COUNT {
                continue;
            }
            match self.claim(index, held.load(Ordering::Acquire)) {
                Claim::Taken => {}
                Claim::Busy => continue,
                Claim::Refused => return None,
            }
            if self.live[index]
                .compare_exchange(FREE, word, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
            {
                if let Some(shared) = &self.shared {
                    shared.words[index].store(word, Ordering::Release);
                }
                held.fetch_add(1, Ordering::AcqRel);
                return Some(word);
            }
            self.unclaim(index);
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
            if let Some(shared) = &self.shared {
                shared.words[index].store(FREE, Ordering::Release);
            }
            self.unclaim(index);
            if let Some(held) = self.held.get(engine) {
                held.fetch_sub(1, Ordering::AcqRel);
            }
        }
        released
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
    /// `word`'s node behind it. With a shared execution context, the nodes of every sharer
    /// count: their words come from one sequence and name the one context.
    pub(super) fn older_live(&self, word: u32) -> Option<u32> {
        let words = self.shared.as_ref().map_or(&self.live, |shared| &shared.words);
        words
            .iter()
            .map(|live| live.load(Ordering::Acquire))
            .filter(|&live| live != FREE && (live.wrapping_sub(word) as i32) < 0)
            .min_by_key(|&live| live.wrapping_sub(word) as i32)
    }

    /// Nodes in use, by every sharer of the execution context.
    pub(super) fn used(&self) -> usize {
        if let Some(shared) = &self.shared {
            return shared.claims.load(Ordering::Acquire).count_ones() as usize;
        }
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
    fn sharers_never_hold_one_index() {
        let shared = SharedStates::new().unwrap();
        let sharers = [WorkStates::shared(shared.clone()), WorkStates::shared(shared)];
        let mut words: [KVec<u32>; 2] = [KVec::new(), KVec::new()];
        let mut taken = 0u64;
        for _ in 0..STATE_COUNT {
            for (states, own) in sharers.iter().zip(words.iter_mut()) {
                if let Some(word) = states.allocate(1) {
                    let bit = 1u64 << (word & 63);
                    assert_eq!(taken & bit, 0);
                    taken |= bit;
                    own.push(word, GFP_KERNEL).unwrap();
                }
            }
        }
        // Both hold nodes, so the last free node stays (may_take) and is counted for both.
        assert_eq!(taken.count_ones() as usize, STATE_COUNT - 1);
        assert_eq!(sharers[0].used(), STATE_COUNT - 1);
        assert_eq!(sharers[1].used(), STATE_COUNT - 1);
        // A node is returned only by its owner, and then either sharer may take its index.
        let word = words[0].pop().unwrap();
        assert!(!sharers[1].release(word, 1));
        assert!(sharers[0].release(word, 1));
        let again = sharers[1].allocate(1).unwrap();
        assert_eq!(taken & (1u64 << (again & 63)) & !(1u64 << (word & 63)), 0);
    }

    #[test]
    fn sharers_see_each_others_older_nodes() {
        let shared = SharedStates::new().unwrap();
        let sharers = [WorkStates::shared(shared.clone()), WorkStates::shared(shared)];
        let first = sharers[0].allocate(1).unwrap();
        let second = sharers[1].allocate(1).unwrap();
        assert_eq!(sharers[1].older_live(second), Some(first));
        assert_eq!(sharers[0].older_live(first), None);
        assert!(sharers[0].release(first, 1));
        assert_eq!(sharers[1].older_live(second), None);
    }

    #[test]
    fn a_sharer_holding_nodes_leaves_the_last_free_one() {
        let shared = SharedStates::new().unwrap();
        let sharers = [WorkStates::shared(shared.clone()), WorkStates::shared(shared)];
        // Sharer 0 takes every node but the last two, with its render engine.
        for _ in 0..STATE_COUNT - 2 {
            assert!(sharers[0].allocate(0).is_some());
        }
        // Sharer 1's compute engine holds none: it takes one of the two.
        assert!(sharers[1].allocate(1).is_some());
        // Each now holds a node, so neither takes the last one.
        assert!(sharers[0].allocate(0).is_none());
        assert!(sharers[1].allocate(1).is_none());
        // An engine holding none still may.
        assert!(sharers[1].allocate(0).is_some());
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
}
