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
            // A later publication retired: it started only after this owner finished, and
            // its load displaced this owner from the hardware slot.
            State::Retired => self.units.iter().enumerate().any(|(b, other)| {
                b != k && other.state == State::Retired && other.publication > u.publication
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
    /// Whether `k`'s owner retired (reusable by the same storage; see `claim_retired`).
    pub(crate) fn retired(&self, k: usize) -> bool {
        self.units.get(k).is_some_and(|u| u.state == State::Retired)
    }
    /// Claims a fresh or retired `k` without the displacement proof: the caller proved its
    /// last owner is the storage being claimed.
    pub(crate) fn claim_retired(&mut self, k: usize) -> bool {
        if !matches!(
            self.units.get(k).map(|u| u.state),
            Some(State::Fresh | State::Retired)
        ) {
            return false;
        }
        self.units[k].previous = self.units[k].state;
        self.units[k].state = State::Preparing;
        self.units[k].generation = 1;
        true
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
        if k >= N || self.units[k].state != State::Preparing || self.sequence == u64::MAX {
            return false;
        }
        // Without pipelining, publication is serialized against every owner of the slot.
        if !self.pipelined && self.units.iter().any(|u| u.state == State::Published) {
            return false;
        }
        self.sequence += 1;
        self.units[k].publication = self.sequence;
        self.units[k].state = State::Published;
        true
    }
    /// Called only after the caller has established complete, exact wire retirement. Owners
    /// retire in publication order: `k` must be the oldest published owner.
    pub(crate) fn retire(&mut self, k: usize) -> bool {
        if k >= N || self.units[k].state != State::Published {
            return false;
        }
        let publication = self.units[k].publication;
        if self
            .units
            .iter()
            .any(|u| u.state == State::Published && u.publication < publication)
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
    fn same_storage_reclaims_its_retired_slot_while_the_other_is_in_flight() {
        // Two pass slots in their own hardware slots: render n+2 reuses n's slot while n+1
        // is in flight, which only the same storage may do (`claim_retired`).
        let mut l = Lifetime::<2>::new(true);
        assert!(l.claim(0));
        assert!(l.publish(0));
        assert!(l.claim(1));
        assert!(l.publish(1));
        assert!(l.retire(0));
        assert!(!l.available(0)); // no later publication retired: no displacement proof
        assert!(l.retired(0));
        assert!(l.claim_retired(0));
        assert!(l.publish(0));
        assert!(!l.retire(0)); // in publication order only
        assert!(l.retire(1));
        assert!(l.retire(0));
        // Published, preparing and quarantined owners are never reclaimed.
        assert!(l.claim_retired(1));
        assert!(!l.claim_retired(1));
        l.cancel(1);
        l.quarantine(1);
        assert!(!l.claim_retired(1));
        assert!(!l.retired(1));
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
}
