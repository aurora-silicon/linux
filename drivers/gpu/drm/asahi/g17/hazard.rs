// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Attachment overlap ordering across physical render pairs and shared images.
//! Tile stores can overlap even when the application's pixel regions do not.
//! Keep independent attachments concurrent; order only conflicting live spans.
//!
//! Within one VM, a render of another logical owner, or of the same owner on
//! its other pair, is ordered at publication: the device mutex totally orders
//! publications, and the later render's fragment kick waits in firmware for the
//! newest overlapping fragment kick of every other pair. Admission never waits
//! for such an overlap. Only a
//! cross-VM overlap through a shared object waits on the host for retirement,
//! so that one VM's fault can never hold another VM's kick in firmware.

use super::job::Order;
use kernel::prelude::kunit_tests;

const MAX_SPANS: usize = 24;
pub(crate) const WAIT_MS: u32 = 1000;
/// Firmware barriers one render publication may add for same-VM overlaps,
/// one per other pair.
pub(crate) const PUBLICATION_BARRIERS: usize = 7;
/// Render pairs one logical owner can hold.
pub(crate) const MAX_OWNER_PAIRS: usize = crate::hw::t8140::queues::RENDER_PAIRS_MAX;

#[derive(Clone, Copy)]
struct Span {
    key: u64,
    start: u64,
    end: u64,
}

/// Up to sixteen attachment ranges and eight shared-object ranges.
pub(crate) struct Footprint {
    vm: u64,
    spans: [Span; MAX_SPANS],
    count: usize,
    shared: bool,
    /// The render writes whole attachments: its tile stores reach no pixel
    /// outside its own render area.
    whole: bool,
}

impl Footprint {
    pub(crate) fn new(vm: u64, whole: bool) -> Self {
        Self {
            vm,
            spans: [Span {
                key: 0,
                start: 0,
                end: 0,
            }; MAX_SPANS],
            count: 0,
            shared: false,
            whole,
        }
    }

    fn push(&mut self, key: u64, start: u64, end: u64) {
        if start < end && self.count < MAX_SPANS {
            self.spans[self.count] = Span { key, start, end };
            self.count += 1;
        }
    }

    pub(crate) fn push_vm(&mut self, start: u64, end: u64) {
        self.push(self.vm << 1, start, end);
    }

    /// An aligned GEM object address is an identity, never dereferenced here.
    pub(crate) fn push_object(&mut self, object: usize, start: u64, end: u64) {
        let before = self.count;
        self.push(object as u64 | 1, start, end);
        self.shared |= self.count != before;
    }

    /// Whether a publication orders an overlap with `other` in firmware.
    pub(crate) fn same_vm(&self, other: &Self) -> bool {
        self.vm == other.vm
    }

    pub(crate) fn overlaps(&self, other: &Self) -> bool {
        if self.vm != other.vm && !(self.shared && other.shared) {
            return false;
        }
        // Tile-granular stores of two whole-attachment renders write only
        // pixels both renders own; unordered, they race as the application
        // asked.
        if self.whole && other.whole {
            return false;
        }
        self.spans[..self.count].iter().any(|a| {
            other.spans[..other.count]
                .iter()
                .any(|b| a.key == b.key && a.start < b.end && b.start < a.end)
        })
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Plan {
    Any,
    /// Only this pair of the owner's.
    Pair(u8),
    /// This pair of the owner's, else another of its pairs: the publication
    /// barrier orders the overlap from either.
    Join(u8),
    Wait,
}

pub(crate) struct Live<'a> {
    pub(crate) owner: u64,
    pub(crate) sequence: u64,
    pub(crate) slot: Option<u8>,
    pub(crate) ticket: u64,
    pub(crate) footprint: &'a Footprint,
    pub(crate) quarantined: bool,
}

/// A queue prefix already orders covered predecessors. Pending arrivals order
/// different owners fairly without imposing order on unrelated attachments.
///
/// A same-VM overlap with another owner's admitted render needs no wait; its
/// publication carries one barrier per other pair. That count stays within
/// [`PUBLICATION_BARRIERS`] because such a render is admitted only while fewer
/// than `PUBLICATION_BARRIERS + 1` other pairs of its VM hold admitted renders:
/// of any set of renders overlapping one publication, the last admitted saw all
/// the others live.
///
/// An owner's own uncovered overlaps on one of its pairs prefer that pair: the
/// pair's queues run its commands in publication order. On any other pair of
/// the owner's (it holds at most [`MAX_OWNER_PAIRS`]) the render's fragment
/// kick waits in firmware for that pair's newest overlapping fragment kick, as
/// for another owner's render ([`Pool::publication_barriers`] names it); tile
/// loads and stores happen only in fragment work. So with overlaps on two of
/// its pairs the render prefers the pair of the newer overlap, and a full
/// preferred pair moves the render to the owner's other pair instead of waiting.
/// That barrier is one more than the foreign ones: without a slot for it, a
/// render with overlaps on one pair stays on that pair, and one with overlaps
/// on two waits as a crowded publication does.
///
/// [`Pool::publication_barriers`]: super::queue::admission::Pool::publication_barriers
pub(crate) fn plan<'a>(
    owner: u64,
    prefix: Option<u64>,
    ticket: Option<u64>,
    footprint: &Footprint,
    live: impl IntoIterator<Item = Live<'a>>,
) -> Plan {
    // The owner's uncovered overlaps: (pair, newest sequence) for each of at
    // most two of its pairs.
    let mut own: [Option<(u8, u64)>; MAX_OWNER_PAIRS] = [None; MAX_OWNER_PAIRS];
    let mut ordered = false;
    let mut pairs = [0u8; PUBLICATION_BARRIERS];
    let mut count = 0;
    let mut crowded = false;
    for entry in live {
        let foreign = entry.owner != owner && footprint.same_vm(entry.footprint);
        if let Some(slot) = entry.slot.filter(|_| foreign) {
            if !pairs[..count].contains(&slot) {
                if count == PUBLICATION_BARRIERS {
                    crowded = true;
                } else {
                    pairs[count] = slot;
                    count += 1;
                }
            }
        }
        if entry.quarantined || !footprint.overlaps(entry.footprint) {
            continue;
        }
        if foreign {
            ordered |= entry.slot.is_some();
            continue;
        }
        let Some(slot) = entry.slot else {
            if entry.owner != owner && ticket.is_none_or(|mine| entry.ticket < mine) {
                return Plan::Wait;
            }
            continue;
        };
        if entry.owner != owner {
            return Plan::Wait;
        }
        if Order::contains(prefix, entry.sequence) {
            continue;
        }
        match own.iter_mut().find(|known| known.is_none_or(|(pair, _)| pair == slot)) {
            Some(known) => {
                let newest = known.map_or(entry.sequence, |(_, sequence)| sequence.max(entry.sequence));
                *known = Some((slot, newest));
            }
            // More pairs than an owner can hold: not reachable.
            None => return Plan::Wait,
        }
    }
    // Foreign barriers come only with a foreign overlap, at most one per
    // counted pair: the own cross-pair barrier needs a slot beside them.
    let room = !(ordered && count == PUBLICATION_BARRIERS);
    if ordered && crowded || own[1].is_some() && !room {
        return Plan::Wait;
    }
    match own {
        [None, _] => Plan::Any,
        [Some((slot, _)), None] if room => Plan::Join(slot),
        [Some((slot, _)), None] => Plan::Pair(slot),
        [Some(a), Some(b)] => Plan::Join(if a.1 >= b.1 { a.0 } else { b.0 }),
    }
}

#[kunit_tests(asahi_g17_hazard_plan)]
mod plan_tests {
    use super::*;

    fn footprint(vm: u64, start: u64) -> Footprint {
        let mut footprint = Footprint::new(vm, false);
        footprint.push_vm(start, start + 0x1000);
        footprint
    }

    fn live(owner: u64, sequence: u64, slot: u8, footprint: &Footprint) -> Live<'_> {
        Live { owner, sequence, slot: Some(slot), ticket: 0, footprint, quarantined: false }
    }

    #[test]
    fn an_own_overlap_prefers_its_pair() {
        let mine = footprint(1, 0);
        let earlier = footprint(1, 0x800);
        assert!(plan(7, None, None, &mine, [live(7, 3, 4, &earlier)]) == Plan::Join(4));
        // A covered predecessor needs nothing.
        assert!(plan(7, Some(3), None, &mine, [live(7, 3, 4, &earlier)]) == Plan::Any);
        // Disjoint attachments stay free.
        let other = footprint(1, 0x10000);
        assert!(plan(7, None, None, &mine, [live(7, 3, 4, &other)]) == Plan::Any);
    }

    #[test]
    fn overlaps_on_two_own_pairs_prefer_the_newer_and_never_wait() {
        let mine = footprint(1, 0);
        let a = footprint(1, 0x800);
        let b = footprint(1, 0x400);
        assert!(plan(7, None, None, &mine, [live(7, 5, 2, &a), live(7, 9, 3, &b)]) == Plan::Join(3));
        assert!(plan(7, None, None, &mine, [live(7, 9, 2, &a), live(7, 5, 3, &b)]) == Plan::Join(2));
        assert!(plan(7, Some(5), None, &mine, [live(7, 5, 2, &a), live(7, 9, 3, &b)]) == Plan::Join(3));
        // A foreign same-VM overlap beside them is a barrier too.
        assert!(
            plan(7, None, None, &mine, [live(7, 5, 2, &a), live(7, 9, 3, &b), live(8, 1, 6, &b)])
                == Plan::Join(3)
        );
    }

    #[test]
    fn the_cross_pair_barrier_keeps_a_slot() {
        let mine = footprint(1, 0);
        let near = footprint(1, 0x800);
        let far = footprint(1, 0x10000);
        // Another owner of the VM holds `foreign` pairs, one with an overlap,
        // then the owner's own overlaps on pair 2 and, if `across`, pair 3.
        fn entries<'a>(
            near: &'a Footprint,
            far: &'a Footprint,
            foreign: usize,
            across: bool,
        ) -> impl Iterator<Item = Live<'a>> + 'a {
            (0..foreign)
                .map(move |index| live(9, 1, 20 + index as u8, if index == 0 { near } else { far }))
                .chain(core::iter::once(live(7, 5, 2, near)))
                .chain(across.then(|| live(7, 6, 3, near)))
        }
        let full = PUBLICATION_BARRIERS;
        assert!(plan(7, None, None, &mine, entries(&near, &far, full - 1, false)) == Plan::Join(2));
        assert!(plan(7, None, None, &mine, entries(&near, &far, full - 1, true)) == Plan::Join(3));
        // No slot left: one pair's overlap stays on that pair; two pairs' wait.
        assert!(plan(7, None, None, &mine, entries(&near, &far, full, false)) == Plan::Pair(2));
        assert!(plan(7, None, None, &mine, entries(&near, &far, full, true)) == Plan::Wait);
    }
}
