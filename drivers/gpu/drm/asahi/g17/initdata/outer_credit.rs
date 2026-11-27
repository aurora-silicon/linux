// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Pending capacity claims for M5's per-priority shared outer ring. Firmware
//! consumption frees posted slots; RAII cancellation frees unposted claims.

use crate::g17::{availability::Availability, fw::channels::WORK_RING_SLOTS};
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc};

const PRIORITIES: usize = 4;

pub(crate) struct Credits {
    reserved: [AtomicU32; PRIORITIES],
    published: [AtomicU64; PRIORITIES],
    consumers: [AtomicU64; PRIORITIES],
    scoped: AtomicBool,
    available: Arc<Availability>,
}

pub(crate) struct OuterClaim {
    domain: Arc<Credits>,
    priority: usize,
    remaining: u32,
}

/// Borrows the claim only. The caller must hold the device mutex from entry
/// through Drop and serialize entry and Drop under `Port::m5_outer` (via
/// `Port::outer_scope`): its temporarily lent credits are available to that publisher.
pub(crate) struct OuterScope<'a> {
    claim: &'a mut OuterClaim,
    before: u64,
}

pub(super) fn free([a, b, producer]: [u32; 3]) -> Result<u32> {
    if [a, b, producer].iter().any(|&v| v >= WORK_RING_SLOTS) {
        return Err(EINVAL);
    }
    Ok(((a + WORK_RING_SLOTS - producer - 1) % WORK_RING_SLOTS)
        .min((b + WORK_RING_SLOTS - producer - 1) % WORK_RING_SLOTS))
}

impl Credits {
    pub(super) fn new() -> Result<Arc<Self>> {
        Ok(Arc::new(Self {
            reserved: [const { AtomicU32::new(0) }; PRIORITIES],
            published: [const { AtomicU64::new(0) }; PRIORITIES],
            consumers: [const { AtomicU64::new(u64::MAX) }; PRIORITIES],
            scoped: AtomicBool::new(false),
            available: Availability::new()?,
        }, GFP_KERNEL)?)
    }

    pub(crate) fn available(&self) -> Arc<Availability> { self.available.clone() }

    pub(super) fn waiting(&self) -> bool { self.available.has_waiters() }

    pub(crate) fn is_scoped(&self) -> bool {
        self.scoped.load(Ordering::Acquire)
    }

    pub(crate) fn capacity(&self, priority: u8, cursors: [u32; 3]) -> Result<u32> {
        let pending = self.reserved.get(usize::from(priority)).ok_or(EINVAL)?
            .load(Ordering::Acquire);
        Ok(free(cursors)?.saturating_sub(pending))
    }

    /// Reserve under the outer-ring lock. Off-lock cancellation may only
    /// reduce reserved counts, so the sampled capacity is conservative.
    pub(crate) fn reserve(self: &Arc<Self>, priority: u8, count: u32,
        cursors: [u32; 3]) -> Result<OuterClaim>
    {
        if count == 0 || count >= WORK_RING_SLOTS { return Err(EINVAL); }
        if self.scoped.load(Ordering::Acquire) { return Err(EBUSY); }
        let index = usize::from(priority);
        let counter = self.reserved.get(index).ok_or(EINVAL)?;
        let seen = (u64::from(cursors[0]) << 32) | u64::from(cursors[1]);
        self.consumers[index].compare_exchange(u64::MAX, seen, Ordering::AcqRel, Ordering::Acquire).ok();
        if self.capacity(priority, cursors)? < count { return Err(EBUSY); }
        counter.fetch_add(count, Ordering::AcqRel);
        Ok(OuterClaim { domain: self.clone(), priority: index, remaining: count })
    }

    /// Consumes one reserved slot for a direct M5 publication without touching
    /// `published[]`, so an enclosing device-mutex `OuterScope` never debits a direct slot.
    pub(crate) fn consume_direct(&self, claim: &mut OuterClaim) -> Result {
        self.consume_direct_count(claim, 1)
    }

    /// Consumes `count` reserved slots for a direct M5 publication without touching
    /// `published[]`, so an enclosing device-mutex `OuterScope` never debits direct slots.
    pub(crate) fn consume_direct_count(&self, claim: &mut OuterClaim, count: u32) -> Result {
        if count == 0 || !core::ptr::eq(self, &*claim.domain) {
            return Err(EINVAL);
        }
        if claim.remaining < count {
            return Err(EIO);
        }
        claim.remaining -= count;
        self.reserved[claim.priority].fetch_sub(count, Ordering::AcqRel);
        Ok(())
    }

    pub(super) fn scope<'a>(self: &Arc<Self>, claim: &'a mut OuterClaim) -> Result<OuterScope<'a>> {
        if !Arc::ptr_eq(self, &claim.domain) { return Err(EINVAL); }
        if self.scoped.swap(true, Ordering::AcqRel) { return Err(EBUSY); }
        self.reserved[claim.priority].fetch_sub(claim.remaining, Ordering::AcqRel);
        let before = self.published[claim.priority].load(Ordering::Acquire);
        Ok(OuterScope { claim, before })
    }

    pub(super) fn counter(&self, priority: u8) -> Result<&AtomicU64> {
        self.published.get(usize::from(priority)).ok_or(EINVAL)
    }

    pub(super) fn progressed(&self, priority: u8, cursors: [u32; 3]) -> Result<bool> {
        free(cursors)?;
        let counter = self.consumers.get(usize::from(priority)).ok_or(EINVAL)?;
        let current = (u64::from(cursors[0]) << 32) | u64::from(cursors[1]);
        let previous = counter.swap(current, Ordering::AcqRel);
        Ok(previous != u64::MAX && previous != current)
    }
}

impl Drop for OuterScope<'_> {
    fn drop(&mut self) {
        let domain = &self.claim.domain;
        let published = domain.published[self.claim.priority].load(Ordering::Acquire)
            .wrapping_sub(self.before);
        // Every publication also checks unreserved physical capacity. If an
        // enclosing transaction posts extra records, only this claim's own
        // credits are consumed here; another claim can never be debited.
        self.claim.remaining -= published.min(u64::from(self.claim.remaining)) as u32;
        domain.reserved[self.claim.priority].fetch_add(self.claim.remaining, Ordering::AcqRel);
        domain.scoped.store(false, Ordering::Release);
    }
}

impl OuterClaim {
    /// Credits not yet consumed by a publication. A claim with none left can be dropped
    /// anywhere: only returned credits notify waiters.
    pub(crate) fn remaining(&self) -> u32 {
        self.remaining
    }

    /// Priority class of the reserved outer work ring.
    pub(crate) fn priority(&self) -> usize {
        self.priority
    }
}

impl Drop for OuterClaim {
    /// As for physical queue claims, drop outside the device mutex because
    /// availability notification can run DMA-fence callbacks.
    fn drop(&mut self) {
        if self.remaining != 0 {
            self.domain.reserved[self.priority].fetch_sub(self.remaining, Ordering::AcqRel);
            self.domain.available.notify();
        }
    }
}

#[kernel::macros::kunit_tests(asahi_g17_outer_credit)]
mod tests {
    use super::*;

    #[test]
    fn direct_consumption_does_not_debit_outer_scope() {
        let credits = Credits::new().unwrap();
        let cursors = [0, 0, 0];
        let full = free(cursors).unwrap();
        let mut scoped_claim = credits.reserve(1, 2, cursors).unwrap();
        let mut direct_claim = credits.reserve(1, 1, cursors).unwrap();
        let mut direct_pair = credits.reserve(1, 2, cursors).unwrap();
        assert_eq!(credits.capacity(1, cursors).unwrap(), full - 5);

        // Direct publish consumes only its own reserved slots and leaves published[] untouched.
        assert!(!credits.is_scoped());
        credits.consume_direct(&mut direct_claim).unwrap();
        assert_eq!(direct_claim.remaining(), 0);
        credits.consume_direct_count(&mut direct_pair, 2).unwrap();
        assert_eq!(direct_pair.remaining(), 0);
        assert_eq!(credits.capacity(1, cursors).unwrap(), full - 2);
        assert_eq!(credits.counter(1).unwrap().load(Ordering::Acquire), 0);

        // An OuterScope taken afterward keeps its full 2 credits when nothing published via it.
        {
            let _scope = credits.scope(&mut scoped_claim).unwrap();
            assert!(credits.is_scoped());
            assert!(credits.reserve(1, 1, cursors).is_err());
        }
        assert!(!credits.is_scoped());
        assert_eq!(scoped_claim.remaining(), 2);
        drop(direct_claim);
        drop(direct_pair);
        drop(scoped_claim);
        assert_eq!(credits.capacity(1, cursors).unwrap(), full);
    }

    #[test]
    fn mixed_render_compute_wrap_and_generation_change_with_queued_claim() {
        let credits = Credits::new().unwrap();
        let n = WORK_RING_SLOTS;
        // Producer sits two slots before wrap while both consumers lag 4 slots behind
        // (free = WORK_RING_SLOTS - 5).
        let mut consumer_a = n - 6;
        let mut consumer_b = n - 6;
        let mut producer = n - 2;
        let cursors = [consumer_a, consumer_b, producer];
        let initial_free = free(cursors).unwrap();
        assert_eq!(initial_free, n - 5);

        // Interleave a queued 2-slot render claim with 1-slot compute claims across ring wrap.
        let mut queued_render = credits.reserve(2, 2, cursors).unwrap();
        let mut compute_a = credits.reserve(2, 1, cursors).unwrap();
        credits.consume_direct(&mut compute_a).unwrap();
        producer = (producer + 1) % n; // n - 1
        assert_eq!(
            credits.capacity(2, [consumer_a, consumer_b, producer]).unwrap(),
            initial_free - 3,
        );

        let mut compute_b = credits.reserve(2, 1, [consumer_a, consumer_b, producer]).unwrap();
        credits.consume_direct(&mut compute_b).unwrap();
        producer = (producer + 1) % n; // 0 (wrapped!)
        assert_eq!(producer, 0);

        // Fill remaining capacity so exactly 1 free slot remains; 2-slot render must fail EBUSY
        // while 1-slot compute still succeeds across the wrap boundary.
        let rem = credits.capacity(2, [consumer_a, consumer_b, producer]).unwrap();
        let filler = credits.reserve(2, rem - 1, [consumer_a, consumer_b, producer]).unwrap();
        assert_eq!(credits.capacity(2, [consumer_a, consumer_b, producer]).unwrap(), 1);
        assert_eq!(credits.reserve(2, 2, [consumer_a, consumer_b, producer]).err(), Some(EBUSY));
        let mut edge_compute = credits.reserve(2, 1, [consumer_a, consumer_b, producer]).unwrap();
        credits.consume_direct(&mut edge_compute).unwrap();
        producer = (producer + 1) % n; // 1
        assert_eq!(credits.capacity(2, [consumer_a, consumer_b, producer]).unwrap(), 0);
        drop(filler);

        // Consumers advance across the wrap boundary and report progress.
        consumer_a = 1;
        consumer_b = 1;
        let wrapped = [consumer_a, consumer_b, producer];
        assert!(credits.progressed(2, wrapped).unwrap());
        assert_eq!(credits.capacity(2, wrapped).unwrap(), (n - 1) - 2);

        // Simulate locked render / recovery entering OuterScope while a queued compute claim
        // waits across a generation change: new reserves fail EBUSY and direct consumption never
        // debits OuterScope's published counter; dropping the queued claim after the generation
        // change releases its reserved credits cleanly.
        let queued_compute = credits.reserve(2, 1, wrapped).unwrap();
        {
            let _scope = credits.scope(&mut queued_render).unwrap();
            assert!(credits.is_scoped());
            assert_eq!(credits.reserve(2, 1, wrapped).err(), Some(EBUSY));
            credits.counter(2).unwrap().fetch_add(2, Ordering::AcqRel);
        }
        assert!(!credits.is_scoped());
        assert_eq!(queued_render.remaining(), 0);
        // Generation changed before queued_compute published: dropping it returns its credit.
        assert_eq!(queued_compute.remaining(), 1);
        drop(queued_compute);
        drop(queued_render);
        assert_eq!(credits.capacity(2, wrapped).unwrap(), n - 1);
    }
}
