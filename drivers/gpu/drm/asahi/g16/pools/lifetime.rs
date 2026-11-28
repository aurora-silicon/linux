// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded displacement ledger of the retained owners of one hardware UMA slot. A completion
//! alone never makes an M4 UMA owner reusable: a later, fully retired publication must have
//! replaced it in the same hardware slot.
//!
//! The firmware runs one queue's commands strictly in order, each to its Finalize, so a
//! publication that retired started after every earlier one finished and displaced it. Owners
//! therefore retire in publication order and an owner is reusable once any owner published
//! after it retired. With `pipelined`, owners may be published while earlier ones are still
//! in flight (the compute queue); without it, publication stays serialized against every
//! owner (the render pass slots).
//!
//! Core-only so the transition system is executable in host tests.

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum State {
    Fresh,
    Preparing,
    Published,
    Retired,
    Quarantined,
}

#[derive(Copy, Clone)]
struct Unit {
    state: State,
    previous: State,
    generation: u8,
    pipe: u8,
    publication: u64,
}

/// Accounting of physical owners, including permanently retained abort storage.
#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub(crate) struct Counts {
    pub(crate) fresh: u32,
    pub(crate) preparing: u32,
    pub(crate) published: u32,
    pub(crate) retired: u32,
    pub(crate) quarantined: u32,
    pub(crate) reusable: u32,
}

#[derive(Clone)]
pub(crate) struct Lifetime<const N: usize> {
    units: [Unit; N],
    sequence: u64,
    pipelined: bool,
}
impl<const N: usize> Lifetime<N> {
    pub(crate) const fn new(pipelined: bool) -> Self {
        let unit = Unit {
            state: State::Fresh,
            previous: State::Fresh,
            generation: 0,
            pipe: 0,
            publication: 0,
        };
        Self {
            units: [unit; N],
            sequence: 0,
            pipelined,
        }
    }
    #[allow(dead_code)]
    pub(crate) fn generation(&self, k: usize) -> u8 {
        self.units[k].generation
    }
    pub(crate) fn counts(&self) -> Counts {
        let mut counts = Counts::default();
        for (k, unit) in self.units.iter().enumerate() {
            match unit.state {
                State::Fresh => counts.fresh += 1,
                State::Preparing => counts.preparing += 1,
                State::Published => counts.published += 1,
                State::Retired => counts.retired += 1,
                State::Quarantined => counts.quarantined += 1,
            }
            counts.reusable += u32::from(self.available(k));
        }
        counts
    }
    /// Published owners.
    pub(crate) fn published(&self) -> u32 {
        self.units
            .iter()
            .filter(|u| u.state == State::Published)
            .count() as u32
    }
    pub(crate) fn available(&self, k: usize) -> bool {
        let Some(u) = self.units.get(k) else {
            return false;
        };
        match u.state {
            State::Fresh => true,
            // A later publication on the same hardware pipe retired: it started only after this
            // owner finished on that pipe, and its load displaced this owner from the slot.
            State::Retired => self.units.iter().enumerate().any(|(b, other)| {
                b != k
                    && other.state == State::Retired
                    && other.pipe == u.pipe
                    && other.publication > u.publication
            }),
            _ => false,
        }
    }
    /// No amount of firmware progress can make a unit claimable. The caller must fail
    /// pending work rather than wait forever after abort/quarantine or sequence exhaustion.
    pub(crate) fn exhausted(&self) -> bool {
        self.exhausted_within(N)
    }
    /// [`Self::exhausted`] when only owners `0..n` exist.
    pub(crate) fn exhausted_within(&self, n: usize) -> bool {
        (0..n.min(N)).all(|k| !self.available(k))
            && !self
                .units
                .iter()
                .any(|u| matches!(u.state, State::Preparing | State::Published))
    }
    /// [`Self::exhausted`] restricted to owners selected by `mask` (bit `k` -> owner `k`).
    pub(crate) fn exhausted_in_mask(&self, mask: u32) -> bool {
        let mut rem = mask;
        while rem != 0 {
            let k = rem.trailing_zeros() as usize;
            rem &= rem - 1;
            if k < N && self.available(k) {
                return false;
            }
        }
        !self
            .units
            .iter()
            .any(|u| matches!(u.state, State::Preparing | State::Published))
    }
    /// Bitmask of permanently quarantined owners in `0..N.min(32)`.
    pub(crate) fn quarantined_mask(&self) -> u32 {
        let mut mask = 0u32;
        for k in 0..N.min(32) {
            if self.units[k].state == State::Quarantined {
                mask |= 1u32 << k;
            }
        }
        mask
    }
    /// Bitmask of owners in `0..N.min(32)` currently held in `Preparing`, or `Published` /
    /// undisplaced `Retired` on any secondary pipe (`pipe != 0`).
    pub(crate) fn secondary_pinned_mask(&self) -> u32 {
        let mut mask = 0u32;
        for k in 0..N.min(32) {
            let u = &self.units[k];
            if u.state == State::Preparing
                || (u.pipe != 0
                    && (u.state == State::Published
                        || (u.state == State::Retired && !self.available(k))))
            {
                mask |= 1u32 << k;
            }
        }
        mask
    }
    /// Bitmask of owners in `0..N.min(32)` that are the newest non-quarantined (`Published` or
    /// `Retired`) publication on any secondary pipe (`pipe != 0`): an idle secondary pipe's
    /// undisplaced `Retired` unit, or an active secondary pipe's newest in-flight `Published`
    /// unit (which becomes undisplaced once that pipe drains).
    pub(crate) fn secondary_tail_mask(&self) -> u32 {
        let mut mask = 0u32;
        for k in 0..N.min(32) {
            let u = &self.units[k];
            if u.pipe == 0 || !matches!(u.state, State::Published | State::Retired) {
                continue;
            }
            let has_newer = self.units.iter().enumerate().any(|(b, other)| {
                b != k
                    && other.pipe == u.pipe
                    && matches!(other.state, State::Published | State::Retired)
                    && other.publication > u.publication
            });
            if !has_newer {
                mask |= 1u32 << k;
            }
        }
        mask
    }
    /// Whether any owner in `0..n` is permanently quarantined.
    pub(crate) fn any_quarantined_within(&self, n: usize) -> bool {
        (0..n.min(N)).any(|k| self.units[k].state == State::Quarantined)
    }
    /// Whether owner `k` is permanently quarantined.
    pub(crate) fn is_quarantined(&self, k: usize) -> bool {
        self.units
            .get(k)
            .is_some_and(|u| u.state == State::Quarantined)
    }
    pub(crate) fn claim(&mut self, k: usize) -> bool {
        if !self.available(k) {
            return false;
        }
        self.units[k].previous = self.units[k].state;
        self.units[k].state = State::Preparing;
        self.units[k].generation = 1;
        true
    }
    pub(crate) fn cancel(&mut self, k: usize) {
        if self.units[k].state == State::Preparing {
            self.units[k].state = self.units[k].previous;
            self.units[k].generation = if self.units[k].previous == State::Fresh {
                0
            } else {
                1
            };
        }
    }
    pub(crate) fn publish(&mut self, k: usize) -> bool {
        self.publish_on(k, 0)
    }
    /// Publishes unit `k` onto hardware queue `pipe` (`0` for single-pipe engines, or the CDM
    /// pipe index for dual-pipe compute). Each pipe enforces FIFO retirement and same-pipe
    /// displacement independently while sharing a monotonic publication sequence.
    pub(crate) fn publish_on(&mut self, k: usize, pipe: u8) -> bool {
        if k >= N || self.units[k].state != State::Preparing || self.sequence == u64::MAX {
            return false;
        }
        // Without pipelining, publication is serialized against every owner on the same pipe.
        if !self.pipelined
            && self
                .units
                .iter()
                .any(|u| u.state == State::Published && u.pipe == pipe)
        {
            return false;
        }
        self.sequence += 1;
        self.units[k].pipe = pipe;
        self.units[k].publication = self.sequence;
        self.units[k].state = State::Published;
        true
    }
    /// Called only after the caller has established complete, exact wire retirement. Owners
    /// retire in publication order per pipe: `k` must be the oldest published owner on its pipe.
    pub(crate) fn retire(&mut self, k: usize) -> bool {
        if k >= N || self.units[k].state != State::Published {
            return false;
        }
        let publication = self.units[k].publication;
        let pipe = self.units[k].pipe;
        if self
            .units
            .iter()
            .any(|u| u.state == State::Published && u.pipe == pipe && u.publication < publication)
        {
            return false;
        }
        self.units[k].state = State::Retired;
        true
    }
    /// A firmware-aborted owner is retained forever and cannot grant a displacement lease.
    pub(crate) fn quarantine(&mut self, k: usize) {
        self.units[k].state = State::Quarantined;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn distinct_retired_displacement_is_required() {
        let mut l = Lifetime::<2>::new(false);
        assert!(l.claim(0));
        assert!(l.publish(0));
        assert!(!l.claim(0));
        assert!(l.retire(0));
        assert!(!l.claim(0)); // Idle/completed alone grants no reuse.
        assert!(l.claim(1));
        assert!(l.publish(1));
        assert!(!l.claim(0)); // Replacement must itself retire.
        assert!(l.retire(1));
        assert!(l.claim(0));
        assert!(l.publish(0));
        assert!(l.retire(0));
        assert!(l.claim(1));
        assert_eq!(l.generation(0), 1);
        assert_eq!(l.generation(1), 1);
    }
    #[test]
    fn abort_does_not_recycle_or_authorize_displacement() {
        let mut l = Lifetime::<2>::new(false);
        assert!(l.claim(0));
        assert!(l.publish(0));
        assert!(l.retire(0));
        assert!(l.claim(1));
        assert!(l.publish(1));
        l.quarantine(1);
        assert!(!l.claim(1));
        assert!(!l.claim(0));
        assert!(l.exhausted());
        assert_eq!(
            l.counts(),
            Counts {
                retired: 1,
                quarantined: 1,
                ..Counts::default()
            }
        );
    }
    #[test]
    fn preparation_cancel_preserves_retirement() {
        let mut l = Lifetime::<2>::new(false);
        assert!(l.claim(0));
        l.cancel(0);
        assert_eq!(l.generation(0), 0);
        assert!(l.claim(0));
        assert!(l.publish(0));
        assert!(l.retire(0));
        assert!(l.claim(1));
        assert!(l.publish(1));
        assert!(l.retire(1));
        assert!(l.claim(0));
        l.cancel(0);
        assert_eq!(l.generation(0), 1);
        assert!(l.claim(0));
    }
    #[test]
    fn publication_is_serialized_and_command_count_is_not_binding_generation() {
        let mut l = Lifetime::<2>::new(false);
        assert!(l.claim(0));
        assert!(l.publish(0));
        assert!(l.claim(1));
        assert!(!l.publish(1));
        l.cancel(1);
        assert!(l.retire(0));
        for n in 1..10000 {
            let k = n % 2;
            assert!(l.claim(k));
            assert!(l.publish(k));
            assert!(l.retire(k));
        }
        assert_eq!(l.generation(0), 1);
        assert_eq!(l.generation(1), 1);
        assert!(l.claim(0));
    }
    #[test]
    fn pipelined_owners_keep_n_minus_one_in_flight() {
        let mut l = Lifetime::<4>::new(true);
        // Fresh owners publish back to back.
        for k in 0..4 {
            assert!(l.claim(k));
            assert!(l.publish(k));
        }
        assert_eq!(l.published(), 4);
        // Out of order retirement is refused.
        assert!(!l.retire(1));
        assert!(l.retire(0));
        // Owner 0 retired but nothing after it has: not displaced yet.
        assert!(!l.claim(0));
        assert!(l.retire(1));
        assert!(l.claim(0)); // displaced by 1
        assert!(!l.claim(1)); // 2 still in flight
        assert!(l.publish(0));
        assert!(l.retire(2));
        assert!(l.claim(1));
        assert!(l.publish(1));
        // Steady state: round robin with up to three in flight.
        let mut next = 2;
        for _ in 0..1000 {
            let oldest = (0..4)
                .filter(|&k| l.units[k].state == State::Published)
                .min_by_key(|&k| l.units[k].publication)
                .unwrap();
            assert!(l.retire(oldest));
            assert!(l.claim(next));
            assert!(l.publish(next));
            assert!(l.published() <= 3);
            next = (next + 1) % 4;
        }
    }
    #[test]
    fn pipelined_newest_retired_owner_is_never_reusable() {
        let mut l = Lifetime::<3>::new(true);
        for k in 0..3 {
            assert!(l.claim(k));
            assert!(l.publish(k));
        }
        for k in 0..3 {
            assert!(l.retire(k));
        }
        assert!(l.available(0) && l.available(1));
        assert!(!l.available(2));
        assert!(l.claim(1));
        // 2 still proves 0 displaced although 1 is being prepared again.
        assert!(l.available(0));
        assert!(!l.available(2));
    }
    #[test]
    fn two_pipes_retire_out_of_order_and_displace_independently() {
        let mut l = Lifetime::<4>::new(true);
        // Publish unit 0 on pipe 0, then unit 2 on pipe 1.
        assert!(l.claim(0) && l.publish_on(0, 0));
        assert!(l.claim(2) && l.publish_on(2, 1));
        // Pipe 1 finishes ahead of pipe 0: out-of-order retirement across pipes must succeed.
        assert!(l.retire(2));
        // Pipe 1's second publication (unit 3) retires and displaces unit 2 on pipe 1.
        assert!(l.claim(3) && l.publish_on(3, 1));
        assert!(l.retire(3));
        assert!(l.available(2));
        assert!(!l.available(3));
        // Pipe 0's unit 0 now retires: unit 3 (pipe 1, higher publication) must NOT displace it.
        assert!(l.retire(0));
        assert!(!l.available(0));
        // Pipe 0's next publication (unit 1) retires and displaces unit 0 on pipe 0.
        assert!(l.claim(1) && l.publish_on(1, 0));
        assert!(l.retire(1));
        assert!(l.available(0));
        assert!(!l.available(1));
        assert!(!l.available(3));
    }
    #[test]
    fn secondary_masks_track_retired_in_flight_and_quarantined_tails() {
        let mut l = Lifetime::<6>::new(true);
        assert_eq!(l.secondary_pinned_mask(), 0);
        assert_eq!(l.secondary_tail_mask(), 0);
        // Pipe 0 never enters secondary masks.
        assert!(l.claim(0) && l.publish_on(0, 0) && l.retire(0));
        assert_eq!(l.secondary_pinned_mask(), 0);
        assert_eq!(l.secondary_tail_mask(), 0);
        // Pipe 1 publishes unit 2 (in-flight tail).
        assert!(l.claim(2) && l.publish_on(2, 1));
        assert_eq!(l.secondary_pinned_mask(), 1 << 2);
        assert_eq!(l.secondary_tail_mask(), 1 << 2);
        // Pipe 1 retires unit 2 (idle undisplaced tail).
        assert!(l.retire(2));
        assert_eq!(l.secondary_pinned_mask(), 1 << 2);
        assert_eq!(l.secondary_tail_mask(), 1 << 2);
        // Pipe 1 publishes unit 3 behind retired unit 2: both are pinned, only 3 is the tail.
        assert!(l.claim(3) && l.publish_on(3, 1));
        assert_eq!(l.secondary_pinned_mask(), (1 << 2) | (1 << 3));
        assert_eq!(l.secondary_tail_mask(), 1 << 3);
        // If unit 3 quarantines instead of retiring, unit 2 remains pipe 1's undisplaced tail.
        l.quarantine(3);
        assert_eq!(l.secondary_pinned_mask(), 1 << 2);
        assert_eq!(l.secondary_tail_mask(), 1 << 2);
        // Pipe 1 publishes and retires unit 4: unit 2 is displaced and cleared from both masks.
        assert!(l.claim(4) && l.publish_on(4, 1) && l.retire(4));
        assert_eq!(l.secondary_pinned_mask(), 1 << 4);
        assert_eq!(l.secondary_tail_mask(), 1 << 4);
    }
}
