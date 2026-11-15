// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Bounded two-owner displacement ledger. A completion alone never makes an M4 UMA owner
//! reusable: a distinct, fully retired owner must replace it in the same hardware slot.
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
pub(crate) struct Lifetime {
    units: [Unit; 2],
    sequence: u64,
    last_owner: Option<usize>,
}
impl Lifetime {
    pub(crate) const fn new() -> Self {
        let unit = Unit {
            state: State::Fresh,
            previous: State::Fresh,
            generation: 0,
            publication: 0,
        };
        Self {
            units: [unit; 2],
            sequence: 0,
            last_owner: None,
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
    pub(crate) fn available(&self, k: usize) -> bool {
        let Some(u) = self.units.get(k) else {
            return false;
        };
        match u.state {
            State::Fresh => true,
            State::Retired => self.last_owner.is_some_and(|b| {
                b != k
                    && self.units[b].state == State::Retired
                    && self.units[b].publication > u.publication
            }),
            _ => false,
        }
    }
    /// No amount of firmware progress can make a unit claimable. The caller must fail
    /// pending work rather than wait forever after abort/quarantine or sequence exhaustion.
    pub(crate) fn exhausted(&self) -> bool {
        (0..2).all(|k| !self.available(k))
            && !self
                .units
                .iter()
                .any(|u| matches!(u.state, State::Preparing | State::Published))
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
        if self.units[k].state != State::Preparing || self.sequence == u64::MAX {
            return false;
        }
        // Publication must be serialized against every owner of this hardware slot.
        if self.units.iter().any(|u| u.state == State::Published) {
            return false;
        }
        self.sequence += 1;
        self.units[k].publication = self.sequence;
        self.units[k].state = State::Published;
        self.last_owner = Some(k);
        true
    }
    /// Called only after the caller has established complete, exact wire retirement.
    pub(crate) fn retire(&mut self, k: usize) -> bool {
        if self.units[k].state != State::Published || self.last_owner != Some(k) {
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
        let mut l = Lifetime::new();
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
        let mut l = Lifetime::new();
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
        let mut l = Lifetime::new();
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
        let mut l = Lifetime::new();
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
}
