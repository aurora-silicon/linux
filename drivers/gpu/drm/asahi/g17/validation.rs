// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Reuse address-coverage proofs only while the VM's page tables are unchanged.

use super::command::{Access, AddressSpace, Payload, UscWindow, Validated};
use crate::mmu;
use core::{cell::RefCell, ops::Range};
use kernel::prelude::*;

const MAX_RANGES: usize = 48;
type Request = (u64, u64, u8);

#[derive(Clone, Copy)]
struct Requests {
    ranges: [Request; MAX_RANGES],
    count: usize,
    overflow: bool,
}

impl Requests {
    const fn new() -> Self {
        Self {
            ranges: [(0, 0, 0); MAX_RANGES],
            count: 0,
            overflow: false,
        }
    }

    fn as_slice(&self) -> &[Request] {
        &self.ranges[..self.count]
    }

    fn insert(&mut self, request: Request) {
        if self.as_slice().contains(&request) {
            return;
        }
        if self.count == MAX_RANGES {
            self.overflow = true;
            return;
        }
        self.ranges[self.count] = request;
        self.count += 1;
    }
}

/// One logical queue's bounded cache of exact address, size and permission tuples.
/// Mapping mutations make the epoch unavailable until their final page-table write.
pub(crate) struct Proof {
    epoch: Option<u64>,
    requests: Requests,
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
                .all(|request| self.requests.as_slice().contains(request))
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
        self.requests = requests;
    }

    /// The VM-job guard must already pin the mappings for this command. Keep the
    /// range arrays off the caller's frame before descending into publication.
    #[inline(never)]
    pub(crate) fn validate(
        &mut self,
        vm: &mmu::Vm,
        range: Range<u64>,
        window: &UscWindow,
        payload: &Payload,
    ) -> Result<Validated> {
        let requests = RefCell::new(Requests::new());
        let optimistic = payload.validate(window, &Space::Record(&range, &requests));
        let requests = requests.into_inner();
        if optimistic.is_ok() && !requests.overflow {
            if self.covers(vm.mapping_validation_epoch(), &requests) {
                return optimistic;
            }
            let (covered, epoch) = vm.covers_ranges_batch(requests.as_slice());
            if covered {
                self.record(epoch, requests);
                return optimistic;
            }
            self.epoch = None;
        }
        // Preserve per-range validation's failure and concurrent-mutation behavior.
        payload.validate(window, &Space::Checked(&range, vm))
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
