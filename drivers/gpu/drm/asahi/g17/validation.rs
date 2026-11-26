// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Reuse address-coverage proofs only while the VM's page tables are unchanged.

use super::command::{Access, AddressSpace, Payload, UscWindow, Validated};
use crate::mmu;
use core::{cell::RefCell, ops::Range};
use kernel::prelude::*;

const MAX_RANGES: usize = 48;
// The persistent proof must retain a deep queue's rotating command buffers.
// Keep the transient parser request set small to bound its stack frame.
const MAX_CACHED_RANGES: usize = 128;
type Request = (u64, u64, u8);

/// Match the page footprint already checked by `Vm::covers_range_locked`.
fn pages((address, size, access): Request) -> Option<Request> {
    if size == 0 || access == 0 || access & !3 != 0 {
        return None;
    }
    let mask = mmu::UAT_PGSZ as u64 - 1;
    let end = address.checked_add(size)?;
    let end = end.checked_add(mask)? & !mask;
    let start = address & !mask;
    Some((start, end.checked_sub(start)?, access))
}

fn contains((start, size, access): Request, (other, length, needed): Request) -> bool {
    access & needed == needed
        && other >= start
        && other.checked_add(length).is_some_and(|end|
            start.checked_add(size).is_some_and(|limit| end <= limit))
}

#[derive(Clone, Copy)]
struct Requests<const CAPACITY: usize = MAX_RANGES> {
    ranges: [Request; CAPACITY],
    count: usize,
    overflow: bool,
}

impl<const CAPACITY: usize> Requests<CAPACITY> {
    const fn new() -> Self {
        Self {
            ranges: [(0, 0, 0); CAPACITY],
            count: 0,
            overflow: false,
        }
    }

    fn as_slice(&self) -> &[Request] {
        &self.ranges[..self.count]
    }

    fn covers_bucket(&self, bucket: u8, request: Request) -> bool {
        let slice = self.as_slice();
        let pos = slice.partition_point(|r| (r.2, r.0) <= (bucket, request.0));
        pos > 0 && slice[pos - 1].2 == bucket && contains(slice[pos - 1], request)
    }

    fn covers(&self, request: Request) -> bool {
        let needed = request.2;
        if needed == 0 || needed & !3 != 0 {
            return false;
        }
        if self.covers_bucket(3, request) {
            return true;
        }
        needed != 3 && self.covers_bucket(needed, request)
    }

    fn insert(&mut self, request: Request) {
        let Some(request) = pages(request) else {
            self.overflow = true;
            return;
        };
        if self.covers(request) {
            return;
        }
        // Keep ranges sorted by (access, start) with disjoint, non-adjacent
        // intervals per access bucket so lookups are O(log N) and adjacent
        // equal-permission ranges merge without granting access across pages.
        let access = request.2;
        let mut merged_start = request.0;
        let mut merged_end = request.0 + request.1;
        let pos = self
            .as_slice()
            .partition_point(|r| (r.2, r.0) <= (access, merged_start));
        let mut lo = pos;
        if pos > 0 {
            let prev = self.ranges[pos - 1];
            if prev.2 == access && prev.0 + prev.1 >= merged_start {
                lo = pos - 1;
                merged_start = prev.0;
                merged_end = merged_end.max(prev.0 + prev.1);
            }
        }
        let mut hi = pos;
        while hi < self.count {
            let next = self.ranges[hi];
            if next.2 != access || next.0 > merged_end {
                break;
            }
            merged_end = merged_end.max(next.0 + next.1);
            hi += 1;
        }
        if access == 3 {
            // Prune strictly weaker Read/Write intervals fully covered by the
            // merged ReadWrite interval, removing bucket 2 before bucket 1 so
            // earlier indices remain stable.
            for bucket in [2u8, 1u8] {
                let p = self
                    .as_slice()
                    .partition_point(|r| (r.2, r.0) < (bucket, merged_start));
                let mut q = p;
                while q < self.count {
                    let r = self.ranges[q];
                    if r.2 != bucket || r.0 + r.1 > merged_end {
                        break;
                    }
                    q += 1;
                }
                if q > p {
                    let removed = q - p;
                    self.ranges.copy_within(q..self.count, p);
                    self.count -= removed;
                    lo -= removed;
                    hi -= removed;
                }
            }
        }
        let merged = (merged_start, merged_end - merged_start, access);
        if lo == hi {
            if self.count == CAPACITY {
                self.overflow = true;
                return;
            }
            self.ranges.copy_within(lo..self.count, lo + 1);
            self.ranges[lo] = merged;
            self.count += 1;
        } else {
            self.ranges[lo] = merged;
            let extra = hi - lo - 1;
            if extra != 0 {
                self.ranges.copy_within(hi..self.count, lo + 1);
                self.count -= extra;
            }
        }
    }

}

/// One logical queue's bounded cache of validated page ranges and permissions.
/// Mapping mutations make the epoch unavailable until their final page-table write.
pub(crate) struct Proof {
    epoch: Option<u64>,
    requests: Requests<MAX_CACHED_RANGES>,
}

impl Proof {
    pub(crate) const fn new() -> Self {
        Self {
            epoch: None,
            requests: Requests::new(),
        }
    }

    fn covers(&self, epoch: Option<u64>, requests: &Requests) -> bool {
        self.epoch.is_some()
            && self.epoch == epoch
            && requests
                .as_slice()
                .iter()
                .all(|&request| self.requests.covers(request))
    }

    /// The requested ranges this proof does not already cover.
    fn uncovered(&self, requests: &Requests) -> Requests {
        let mut missing = Requests::new();
        for &request in requests.as_slice() {
            if !self.requests.covers(request) {
                missing.insert(request);
            }
        }
        missing
    }

    fn record(&mut self, epoch: Option<u64>, requests: Requests) {
        // Alternating in-flight buffers must not evict each other's proofs.
        if epoch.is_some() && self.epoch == epoch && !requests.overflow {
            for &request in requests.as_slice() {
                self.requests.insert(request);
            }
            if !self.requests.overflow {
                return;
            }
        }
        // An incomplete union cannot be reused; retain the latest complete set.
        self.epoch = if requests.overflow { None } else { epoch };
        self.requests = Requests::new();
        for &request in requests.as_slice() {
            self.requests.insert(request);
        }
    }

    /// The VM-job guard must already pin the mappings for this command. Keep the
    /// range arrays off the caller's frame before descending into publication.
    #[inline(never)]
    pub(crate) fn validate(
        &mut self,
        vm: &mmu::Vm,
        range: Range<u64>,
        window: &UscWindow,
        variant: &crate::g17::fw::Variant,
        payload: &Payload,
    ) -> Result<Validated> {
        let requests = RefCell::new(Requests::new());
        let optimistic = payload.validate(window, variant, &Space::Record(&range, &requests));
        let requests = requests.into_inner();
        if optimistic.is_ok() && !requests.overflow {
            let epoch = vm.mapping_validation_epoch();
            if self.covers(epoch, &requests) {
                return optimistic;
            }
            // Proofs from an unchanged epoch still hold: walk only the ranges they do not
            // cover. A command buffer at a fresh address then no longer re-walks every
            // render target it names.
            let partial = self.epoch.is_some() && self.epoch == epoch;
            let walk = if partial { self.uncovered(&requests) } else { requests };
            let (mut covered, mut walked) = vm.covers_ranges_batch(walk.as_slice());
            if covered && partial && walked != self.epoch {
                // A mapping changed since the cached proofs were taken: walk everything.
                (covered, walked) = vm.covers_ranges_batch(requests.as_slice());
            }
            if covered {
                self.record(walked, requests);
                return optimistic;
            }
            self.epoch = None;
        }
        // Preserve per-range validation's failure and concurrent-mutation behavior.
        payload.validate(window, variant, &Space::Checked(&range, vm))
    }
}

enum Space<'a> {
    Record(&'a Range<u64>, &'a RefCell<Requests>),
    Checked(&'a Range<u64>, &'a mmu::Vm),
}

impl AddressSpace for Space<'_> {
    fn covers(&self, address: u64, size: u64, access: Access) -> bool {
        let range = match self {
            Self::Record(range, _) | Self::Checked(range, _) => range,
        };
        let Some(end) = address.checked_add(size) else {
            return false;
        };
        if size == 0 || address < range.start || end > range.end {
            return false;
        }
        let permissions = match access {
            Access::Read => 1,
            Access::Write => 2,
            Access::ReadWrite => 3,
        };
        match self {
            Self::Record(_, requests) => {
                requests.borrow_mut().insert((address, size, permissions));
                true
            }
            Self::Checked(_, vm) => {
                vm.covers_range(address, size, permissions & 1 != 0, permissions & 2 != 0)
            }
        }
    }
}


#[kunit_tests(asahi_g17_validation_pages)]
mod tests {
    use super::*;
    const PAGE: u64 = mmu::UAT_PGSZ as u64;

    fn requests(range: Request) -> Requests {
        let mut result = Requests::new();
        result.insert(range);
        result
    }

    #[test]
    fn different_offsets_share_the_already_validated_page() {
        let mut proof = Proof::new();
        proof.record(Some(7), requests((PAGE + 4, 4, 3)));
        assert!(proof.covers(Some(7), &requests((PAGE + 0x100, 8, 1))));
        assert!(proof.covers(Some(7), &requests((2 * PAGE - 8, 8, 2))));
        assert!(!proof.covers(Some(7), &requests((2 * PAGE - 8, 16, 1))));
        assert!(!proof.covers(Some(8), &requests((PAGE + 4, 4, 1))));
        assert!(!proof.covers(None, &requests((PAGE + 4, 4, 1))));
    }

    #[test]
    fn adjacent_equal_permissions_merge_but_holes_do_not() {
        let mut ranges = requests((PAGE, 1, 1));
        ranges.insert((3 * PAGE, 1, 1));
        assert_eq!(ranges.count, 2);
        assert!(!ranges.covers((2 * PAGE, PAGE, 1)));
        ranges.insert((2 * PAGE, 1, 1));
        assert_eq!(ranges.count, 1);
        assert_eq!(ranges.as_slice(), &[(PAGE, 3 * PAGE, 1)]);
    }

    #[test]
    fn adjacent_permissions_never_grant_access_to_another_page() {
        let mut proof = Proof::new();
        let mut ranges = requests((PAGE, 1, 1));
        ranges.insert((2 * PAGE, 1, 3));
        proof.record(Some(1), ranges);
        assert!(!proof.covers(Some(1), &requests((PAGE, 1, 2))));
        assert!(proof.covers(Some(1), &requests((2 * PAGE, 1, 2))));
        assert!(!proof.covers(Some(1), &requests((PAGE, 2 * PAGE, 3))));
    }

    #[test]
    fn stronger_coverage_removes_only_fully_covered_ranges() {
        let mut ranges = requests((PAGE, PAGE, 1));
        ranges.insert((2 * PAGE, PAGE, 2));
        ranges.insert((PAGE, 2 * PAGE, 3));
        assert_eq!(ranges.as_slice(), &[(PAGE, 2 * PAGE, 3)]);
        assert!(!ranges.covers((0, PAGE, 1)));
    }

    #[test]
    fn persistent_cache_retains_a_deep_rotating_working_set() {
        let mut proof = Proof::new();
        for index in 0..64 {
            proof.record(Some(7), requests((index * 2 * PAGE, 8, 1)));
        }
        assert_eq!(proof.requests.count, 64);
        for index in 0..64 {
            assert!(proof.covers(Some(7), &requests((index * 2 * PAGE + 16, 8, 1))));
        }
        assert_eq!(core::mem::size_of::<Requests>(), core::mem::size_of::<Requests<48>>());
    }

    #[test]
    fn persistent_overflow_keeps_only_a_new_complete_proof() {
        let mut proof = Proof::new();
        for index in 0..=MAX_CACHED_RANGES {
            proof.record(Some(7), requests((index as u64 * 2 * PAGE, 8, 1)));
        }
        assert_eq!(proof.requests.count, 1);
        assert!(!proof.covers(Some(7), &requests((0, 8, 1))));
        assert!(proof.covers(Some(7),
            &requests((MAX_CACHED_RANGES as u64 * 2 * PAGE, 8, 1))));
    }

    #[test]
    fn only_uncovered_ranges_are_walked_again() {
        let mut proof = Proof::new();
        let mut proven = requests((PAGE, 4 * PAGE, 1));
        proven.insert((8 * PAGE, PAGE, 3));
        proof.record(Some(5), proven);

        let mut next = requests((PAGE + 16, 8, 1));
        next.insert((8 * PAGE, 8, 2));
        next.insert((20 * PAGE, PAGE, 1));
        next.insert((4 * PAGE, 2 * PAGE, 1));
        let missing = proof.uncovered(&next);
        assert_eq!(missing.as_slice(), &[(4 * PAGE, 2 * PAGE, 1), (20 * PAGE, PAGE, 1)]);

        // Stronger access than proven is walked again.
        assert_eq!(proof.uncovered(&requests((PAGE, 8, 2))).count, 1);
        assert_eq!(proof.uncovered(&requests((8 * PAGE, 8, 3))).count, 0);
    }

    #[test]
    fn a_partial_walk_extends_the_proof() {
        let mut proof = Proof::new();
        proof.record(Some(5), requests((PAGE, PAGE, 1)));
        let mut next = requests((PAGE, 8, 1));
        next.insert((10 * PAGE, 8, 1));
        assert!(!proof.covers(Some(5), &next));
        // validate() walks proof.uncovered(&next), then records the whole request set.
        assert_eq!(proof.uncovered(&next).as_slice(), &[(10 * PAGE, PAGE, 1)]);
        proof.record(Some(5), next);
        assert!(proof.covers(Some(5), &next));
        assert!(proof.covers(Some(5), &requests((PAGE + 8, 8, 1))));
        assert!(!proof.covers(Some(6), &next));
    }

    #[test]
    fn invalid_rounding_and_full_cache_disable_reuse() {
        for range in [(0, 0, 1), (u64::MAX - 1, 1, 1), (0, 1, 0), (0, 1, 4)] {
            assert!(pages(range).is_none());
            let mut proof = Proof::new();
            proof.record(Some(1), requests(range));
            assert!(!proof.covers(Some(1), &requests((0, 1, 1))));
        }
        let mut ranges = Requests::new();
        for index in 0..=MAX_RANGES {
            ranges.insert(((2 * index) as u64 * PAGE, PAGE, 1));
        }
        assert!(ranges.overflow);
        let mut proof = Proof::new();
        proof.record(Some(1), ranges);
        assert!(!proof.covers(Some(1), &requests((0, PAGE, 1))));
    }

    #[test]
    fn sorted_requests_preserve_overlap_merge_and_subsumption() {
        let mut ranges: Requests<48> = Requests::new();
        ranges.insert((PAGE, 4 * PAGE, 1));
        ranges.insert((2 * PAGE, PAGE, 2));
        // Overlapping weaker Write range must not mask the wider Read range.
        assert!(ranges.covers((3 * PAGE, PAGE, 1)));
        assert!(ranges.covers((2 * PAGE, PAGE, 2)));
        assert!(!ranges.covers((2 * PAGE, PAGE, 3)));
        // Adjacent equal-permission merge across a gap.
        ranges.insert((6 * PAGE, PAGE, 1));
        ranges.insert((5 * PAGE, PAGE, 1));
        assert!(ranges.covers((PAGE, 6 * PAGE, 1)));
        // Partial overlap with a stronger ReadWrite range keeps the uncovered weaker range.
        ranges.insert((PAGE, 2 * PAGE, 3));
        assert!(ranges.covers((PAGE, 6 * PAGE, 1)));
        assert!(ranges.covers((2 * PAGE, PAGE, 3)));
        // Stronger ReadWrite range subsumes fully covered Read/Write ranges.
        ranges.insert((PAGE, 6 * PAGE, 3));
        assert_eq!(ranges.as_slice(), &[(PAGE, 6 * PAGE, 3)]);
        assert!(ranges.covers((2 * PAGE, PAGE, 1)));
        assert!(ranges.covers((2 * PAGE, PAGE, 2)));
        assert!(ranges.covers((PAGE, 6 * PAGE, 3)));
    }
}
