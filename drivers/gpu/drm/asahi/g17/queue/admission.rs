// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Logical queue and physical render-pair admission. A retained pair belongs
//! to one logical owner even while idle. Closing that owner makes it reusable
//! only after every active lease retires. Quarantine retains these leases until
//! a firmware drain witness or both processors stopping makes reuse safe.
//!
//! The pool lock protects host accounting only and is never held while building
//! a graph or waiting for GPU work. Capacity waiters sleep on the release event.

use crate::g17::{
    hazard::{self, Footprint, Plan},
    job::Packet,
};
use crate::{hw::t8140::queues as cfg, mmu::MAX_EXECUTION_CONTEXTS};
use core::sync::atomic::{AtomicU32, Ordering};
use kernel::{
    new_condvar, new_mutex,
    prelude::*,
    sync::{Arc, CondVar, CondVarTimeoutResult, Mutex},
};

#[derive(Clone, Copy)]
pub(crate) struct Request<'a> {
    footprint: &'a Arc<Footprint>,
    sequence: u64,
    prefix: Option<u64>,
}

impl<'a> Request<'a> {
    pub(crate) fn of(packet: &'a Packet) -> Option<Self> {
        packet.footprint.as_ref().map(|footprint| Self {
            footprint,
            sequence: packet.order.sequence,
            prefix: packet.order.wait_through[0],
        })
    }
}

struct InFlight {
    owner: u64,
    sequence: u64,
    slot: Option<u8>,
    ticket: u64,
    footprint: Arc<Footprint>,
}

const QUARANTINE_WAIT_MS: u32 = 1000;

/// A wake never renews admission's budget. Short observations also notice a
/// packet cancellation that has no corresponding pair-release notification.
fn wait_slice_ms(deadline_ns: u64, now_ns: u64) -> Result<u32> {
    let remaining = deadline_ns.saturating_sub(now_ns);
    if remaining == 0 {
        return Err(EBUSY);
    }
    Ok(remaining.div_ceil(1_000_000).min(10) as u32)
}

struct State {
    active: u64,
    refs: [u8; cfg::RENDER_SLOTS],
    owners: [u64; cfg::RENDER_SLOTS],
    epochs: [u64; cfg::RENDER_SLOTS],
    quarantined: u64,
    /// Quarantined slots whose failed work is all spared (innocent): waiting publications fail
    /// their own fence with EBUSY instead of a device-loss error, so the VM stays usable.
    spared: u64,
    kill_hold: u64,
    released: u64,
    epoch: u64,
    footprints: KVec<InFlight>,
    next_ticket: u64,
}
impl State {
    const MASK: u64 = (u64::MAX >> (64 - cfg::RENDER_SLOTS)) & !1;
    const fn new() -> Self {
        Self {
            active: 0,
            refs: [0; cfg::RENDER_SLOTS],
            owners: [0; cfg::RENDER_SLOTS],
            epochs: [0; cfg::RENDER_SLOTS],
            quarantined: 0,
            spared: 0,
            kill_hold: 0,
            released: 0,
            epoch: 0,
            footprints: KVec::new(),
            next_ticket: 1,
        }
    }
    fn exhausted(&self) -> bool {
        self.quarantined & Self::MASK == Self::MASK
    }
    fn refusal(&self, owner: u64) -> Result {
        if self.exhausted() {
            return Err(EIO);
        }
        match self.owners.iter().position(|id| *id == owner) {
            Some(slot) if self.quarantined & (1u64 << slot) != 0 => Err(EIO),
            None if !self.owners[1..].contains(&0)
                && self.released
                    & !self.active
                    & !self.quarantined
                    & !self.kill_hold
                    & Self::MASK
                    == 0 =>
            {
                Err(crate::g17::capacity::refused(crate::g17::capacity::Site::RenderSlots))
            }
            _ => Ok(()),
        }
    }
    fn advance(&mut self, slot: usize) {
        self.epoch = self.epoch.wrapping_add(1);
        self.epochs[slot] = self.epoch;
    }
    fn take_idle(&mut self, slot: usize, owner: u64) -> u8 {
        self.refs[slot] += 1;
        self.active |= 1u64 << slot;
        self.released &= !(1u64 << slot);
        self.owners[slot] = owner;
        self.advance(slot);
        slot as u8
    }
    fn acquire(&mut self, owner: u64) -> Option<u8> {
        if owner == 0 {
            return None;
        }
        // Prefer the owner's idle pair. A missing sibling can be claimed
        // only while this owner has no live kick, before constructing it.
        let idle = (0..cfg::RENDER_SLOTS)
            .filter(|slot| {
                self.owners[*slot] == owner
                    && self.refs[*slot] == 0
                    && self.quarantined & (1u64 << slot) == 0
            })
            .max_by_key(|slot| (self.epochs[*slot], usize::MAX - *slot));
        if let Some(slot) = idle {
            if !(0..cfg::RENDER_SLOTS)
                .any(|other| self.owners[other] == owner && self.refs[other] != 0)
            {
                self.claim_sibling(owner);
            }
            return Some(self.take_idle(slot, owner));
        }
        if self.owners.contains(&owner) {
            // Consecutive kicks are ordered by firmware dependencies. Stack
            // onto the least-loaded retained pair without a retirement wait.
            let slot = (0..cfg::RENDER_SLOTS)
                .filter(|slot| {
                    self.owners[*slot] == owner
                        && self.quarantined & (1u64 << slot) == 0
                        && self.active & (1u64 << slot) != 0
                        && self.refs[*slot] < cfg::RENDER_DEPTH
                })
                .min_by_key(|slot| (self.refs[*slot], u64::MAX - self.epochs[*slot]))?;
            self.refs[slot] += 1;
            self.advance(slot);
            return Some(slot as u8);
        }
        let free = !self.active & Self::MASK & !self.quarantined & !self.kill_hold;
        let eligible = |slot: usize| free & (1u64 << slot) != 0;
        // Reuse a closed, exactly retired owner before consuming a new graph.
        let slot = (0..cfg::RENDER_SLOTS)
            .filter(|slot| eligible(*slot) && self.released & (1u64 << slot) != 0)
            .min_by_key(|slot| self.epochs[*slot])
            .or_else(|| {
                (0..cfg::RENDER_SLOTS).find(|slot| eligible(*slot) && self.owners[*slot] == 0)
            })?;
        let first = self.take_idle(slot, owner);
        self.claim_sibling(owner);
        Some(first)
    }
    fn claim_sibling(&mut self, owner: u64) {
        if self.owners.iter().filter(|id| **id == owner).count() >= cfg::RENDER_PAIRS_PER_OWNER {
            return;
        }
        let free = !self.active & Self::MASK & !self.quarantined & !self.kill_hold;
        let eligible = |slot: usize| free & (1u64 << slot) != 0;
        let sibling = (0..cfg::RENDER_SLOTS)
            .filter(|slot| {
                eligible(*slot)
                    && self.owners[*slot] != owner
                    && self.released & (1u64 << slot) != 0
            })
            .min_by_key(|slot| self.epochs[*slot])
            .or_else(|| {
                (0..cfg::RENDER_SLOTS).find(|slot| eligible(*slot) && self.owners[*slot] == 0)
            });
        if let Some(slot) = sibling {
            self.released &= !(1u64 << slot);
            self.owners[slot] = owner;
            // A speculative sibling has no GPU-use recency yet.
            self.epochs[slot] = 0;
        }
    }

    /// The device lock separately proves the physical graph was never published.
    /// An open owner keeps another usable pair. Exclude every live borrower,
    /// including off-lock sibling construction; a closed owner needs no spare.
    fn reassign_unused_spare(
        &mut self,
        slot: u8,
        expected: Option<u64>,
        owner: Option<u64>,
    ) -> bool {
        let slot = usize::from(slot);
        if slot == 0
            || slot >= cfg::RENDER_SLOTS
            || owner.is_some_and(|owner| {
                owner == 0
                    || self.owners.iter().filter(|value| **value == owner).count()
                        >= cfg::RENDER_PAIRS_PER_OWNER
            })
        {
            return false;
        }
        let bit = 1u64 << slot;
        let old = self.owners[slot];
        if old == 0
            || owner == Some(old)
            || expected.is_some_and(|expected| expected != old)
            || self.refs[slot] != 0
            || (self.active | self.quarantined | self.kill_hold) & bit != 0
            || (self.released & bit == 0
                && !self.owners.iter().enumerate().any(|(other, value)| {
                    other != slot
                        && *value == old
                        && (self.quarantined | self.kill_hold) & (1u64 << other) == 0
                }))
        {
            return false;
        }
        self.owners[slot] = owner.unwrap_or(0);
        self.released &= !bit;
        self.epochs[slot] = 0;
        true
    }
    fn warmable_sibling(&self, owner: u64, publishing: u8) -> Option<u8> {
        if self.refs.get(usize::from(publishing)) != Some(&1) {
            return None;
        }
        (0..cfg::RENDER_SLOTS)
            .find(|slot| {
                *slot != usize::from(publishing)
                    && self.owners[*slot] == owner
                    && self.refs[*slot] == 0
                    && self.quarantined & (1u64 << slot) == 0
                    && self.released & (1u64 << slot) == 0
            })
            .map(|slot| slot as u8)
    }
    fn borrow_warm_sibling(&mut self, owner: u64, publishing: u8) -> Option<u8> {
        let slot = self.warmable_sibling(owner, publishing)?;
        self.refs[usize::from(slot)] += 1;
        self.active |= 1u64 << slot;
        Some(slot)
    }
    fn cancel_empty_slot(&mut self, slot: u8, owner: u64) -> bool {
        let slot = usize::from(slot);
        let bit = 1u64 << slot;
        if self.owners[slot] == owner
            && self.refs[slot] == 1
            && (self.quarantined | self.kill_hold) & bit == 0
        {
            self.owners[slot] = 0;
            self.released &= !bit;
            self.epochs[slot] = 0;
            true
        } else {
            false
        }
    }
    fn plan(&self, owner: u64, request: Option<Request<'_>>, ticket: Option<u64>) -> Plan {
        let Some(request) = request else {
            return Plan::Any;
        };
        hazard::plan(
            owner,
            request.prefix,
            ticket,
            request.footprint,
            self.footprints.iter().map(|entry| hazard::Live {
                owner: entry.owner,
                sequence: entry.sequence,
                slot: entry.slot,
                ticket: entry.ticket,
                footprint: &entry.footprint,
                quarantined: entry
                    .slot
                    .is_some_and(|slot| self.quarantined & (1u64 << slot) != 0),
            }),
        )
    }

    fn pick(&mut self, owner: u64, plan: Plan) -> Option<u8> {
        match plan {
            Plan::Any => self.acquire(owner),
            Plan::Wait => None,
            Plan::Pair(slot) => {
                let index = usize::from(slot);
                if self.owners.get(index) != Some(&owner)
                    || (self.quarantined | self.kill_hold) & (1u64 << slot) != 0
                    || self.refs[index] >= cfg::RENDER_DEPTH
                {
                    return None;
                }
                if self.refs[index] == 0 {
                    Some(self.take_idle(index, owner))
                } else {
                    self.refs[index] += 1;
                    self.advance(index);
                    Some(slot)
                }
            }
        }
    }

    fn record(
        &mut self,
        owner: u64,
        slot: Option<u8>,
        ticket: u64,
        request: Request<'_>,
    ) -> Option<(u64, u64)> {
        self.footprints
            .push(
                InFlight {
                    owner,
                    sequence: request.sequence,
                    slot,
                    ticket,
                    footprint: request.footprint.clone(),
                },
                GFP_KERNEL,
            )
            .ok()?;
        Some((owner, request.sequence))
    }

    fn forget_waiter(&mut self, ticket: Option<u64>) {
        if let Some(ticket) = ticket {
            self.footprints
                .retain(|entry| entry.slot.is_some() || entry.ticket != ticket);
        }
    }

    fn release(&mut self, slot: u8) {
        let slot = usize::from(slot);
        if self.refs[slot] != 0 {
            self.refs[slot] -= 1;
        }
        if self.refs[slot] == 0 {
            self.active &= !(1u64 << slot);
        }
    }
}

#[pin_data]
pub(crate) struct Pool {
    #[pin]
    state: Mutex<State>,
    #[pin]
    released: CondVar,
    queues: AtomicU32,
    /// Logical queues admitted at most: each must be able to hold a render pair.
    max_queues: u32,
}
impl Pool {
    pub(crate) fn new(max_queues: u32) -> Result<Arc<Self>> {
        if max_queues > MAX_EXECUTION_CONTEXTS {
            return Err(EINVAL);
        }
        Arc::pin_init(
            pin_init!(Self {
                state <- new_mutex!(State::new(), "G17 render admission"),
                released <- new_condvar!("G17 render pair release"),
                queues: AtomicU32::new(0),
                max_queues,
            }),
            GFP_KERNEL,
        )
    }
    /// Logical queues currently counted, including one being created.
    pub(crate) fn queues(&self) -> u32 {
        self.queues.load(Ordering::Acquire)
    }
    pub(crate) fn reserve_queue(self: &Arc<Self>) -> Result<QueueSlot> {
        let mut count = self.queues.load(Ordering::Acquire);
        loop {
            if count >= self.max_queues {
                return Err(crate::g17::capacity::refused(crate::g17::capacity::Site::LogicalQueues));
            }
            match self.queues.compare_exchange(
                count,
                count + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Ok(QueueSlot { pool: self.clone() }),
                Err(current) => count = current,
            }
        }
    }
    pub(crate) fn acquire_render(
        self: &Arc<Self>,
        owner: u64,
        request: Option<Request<'_>>,
        deadline_ns: u64,
        check: impl Fn() -> Result,
    ) -> Result<RenderLease> {
        if owner == 0 {
            return Err(EINVAL);
        }
        let mut state = self.state.lock();
        let started = crate::g17::now_ns();
        let conflict_deadline = started.saturating_add(u64::from(hazard::WAIT_MS) * 1_000_000);
        let quarantine_deadline = started.saturating_add(u64::from(QUARANTINE_WAIT_MS) * 1_000_000);
        let mut expired = false;
        let mut ticket = None;
        let result = (|| loop {
            check()?;
            let now = crate::g17::now_ns();
            let remaining_ms = wait_slice_ms(deadline_ns, now)?;
            if state.exhausted() {
                return Err(EIO);
            }
            // The first retained pair's quarantine is not bypassed through its
            // healthy sibling. Reclamation wakes this bounded admission wait.
            if state
                .owners
                .iter()
                .position(|id| *id == owner)
                .is_some_and(|slot| state.quarantined & (1u64 << slot) != 0)
            {
                if now >= quarantine_deadline {
                    // Behind an innocent pair's quarantine (another VM wedged the GPU) only this
                    // publication fails, not the VM: it recovers once the pair drains.
                    let spared = state
                        .owners
                        .iter()
                        .position(|id| *id == owner)
                        .is_some_and(|slot| state.spared & (1u64 << slot) != 0);
                    return Err(if spared { EBUSY } else { EIO });
                }
                let slice = remaining_ms.min(wait_slice_ms(quarantine_deadline, now)?);
                if matches!(self.released.wait_interruptible_timeout(
                    &mut state, kernel::time::msecs_to_jiffies(slice)
                ), CondVarTimeoutResult::Signal { .. }) {
                    return Err(ERESTARTSYS);
                }
                continue;
            }
            state.refusal(owner)?;
            expired |= now >= conflict_deadline;
            let plan = if expired {
                Plan::Any
            } else {
                state.plan(owner, request, ticket)
            };
            if let Some(slot) = state.pick(owner, plan) {
                state.forget_waiter(ticket.take());
                let record =
                    request.and_then(|request| state.record(owner, Some(slot), 0, request));
                return Ok(RenderLease {
                    pool: self.clone(),
                    slot,
                    record,
                });
            }
            if plan != Plan::Any {
                if ticket.is_none() {
                    if let Some(request) = request {
                        let next = state.next_ticket;
                        state.next_ticket = next.wrapping_add(1);
                        if state.record(owner, None, next, request).is_some() {
                            ticket = Some(next);
                        }
                    }
                }
            }
            // Keep the established hazard wait/fallback rule, but cap every
            // sleep with the caller's original deadline and cancellation poll.
            let slice = if plan != Plan::Any {
                remaining_ms.min(wait_slice_ms(conflict_deadline, now)?)
            } else {
                remaining_ms
            };
            if matches!(self.released.wait_interruptible_timeout(
                &mut state, kernel::time::msecs_to_jiffies(slice)
            ), CondVarTimeoutResult::Signal { .. }) {
                return Err(ERESTARTSYS);
            }
        })();
        state.forget_waiter(ticket);
        result
    }

    /// Early publication never waits for overlap; the scheduler retries with
    /// the same footprint after its dependencies have become ready.
    pub(crate) fn try_acquire_render(
        self: &Arc<Self>,
        owner: u64,
        request: Option<Request<'_>>,
    ) -> Result<Option<RenderLease>> {
        if owner == 0 {
            return Ok(None);
        }
        let mut state = self.state.lock();
        if state.refusal(owner).is_err() || !state.owners.contains(&owner) {
            return Ok(None);
        }
        let plan = state.plan(owner, request, None);
        let Some(slot) = state.pick(owner, plan) else {
            return Ok(None);
        };
        let record = request.and_then(|request| state.record(owner, Some(slot), 0, request));
        Ok(Some(RenderLease {
            pool: self.clone(),
            slot,
            record,
        }))
    }
    pub(crate) fn warmable_sibling(
        self: &Arc<Self>,
        owner: u64,
        publishing: u8,
    ) -> Option<RenderLease> {
        let mut state = self.state.lock();
        let slot = state.borrow_warm_sibling(owner, publishing)?;
        // This is a host construction borrower, not a GPU-use epoch. In
        // particular it must not clear a logical close or age an unused spare.
        Some(RenderLease {
            pool: self.clone(),
            slot,
            record: None,
        })
    }
    pub(crate) fn reassign_unused_spare(
        &self,
        slot: u8,
        expected: Option<u64>,
        owner: Option<u64>,
    ) -> bool {
        self.state
            .lock()
            .reassign_unused_spare(slot, expected, owner)
    }
    pub(crate) fn owns_slot(&self, owner: u64, slot: usize) -> bool {
        self.state.lock().owners.get(slot) == Some(&owner)
    }
    pub(crate) fn release_owner(&self, owner: u64) {
        let mut state = self.state.lock();
        for slot in 0..cfg::RENDER_SLOTS {
            if state.owners[slot] == owner {
                state.released |= 1u64 << slot;
            }
        }
        drop(state);
        self.released.notify_all();
    }
    /// Active leases survive this transition. Their later drop is still the
    /// event which releases capacity, so a stopped owner is never double-freed.
    pub(crate) fn release_retained_owners_after_processor_stop(&self) {
        let mut state = self.state.lock();
        let released = state.released & !state.active;
        for slot in 1..cfg::RENDER_SLOTS {
            if released & (1u64 << slot) != 0 {
                state.owners[slot] = 0;
                state.epochs[slot] = 0;
            }
        }
        state.released &= !released;
        state.quarantined &= !released;
        state.spared &= !released;
        state.kill_hold = 0;
        drop(state);
        self.released.notify_all();
    }
}

#[kunit_tests(asahi_g17_admission_accounting)]
mod accounting_tests {
    use super::*;

    #[test]
    fn every_logical_owner_fits_without_speculative_spares() {
        let mut state = State::new();
        for owner in 1..=u64::from(MAX_EXECUTION_CONTEXTS) {
            let slot = state.acquire(owner).unwrap();
            assert_eq!(state.owners.iter().filter(|id| **id == owner).count(), 1);
            state.release(slot);
        }
        assert_eq!(state.active, 0);
        assert_eq!(state.owners.iter().filter(|id| **id != 0).count(), MAX_EXECUTION_CONTEXTS as usize);
    }

    #[test]
    fn close_does_not_reuse_an_active_lease() {
        let mut state = State::new();
        let slot = state.acquire(1).unwrap();
        state.released |= 1u64 << slot;
        assert!(!state.reassign_unused_spare(slot, Some(1), None));
        state.release(slot);
        assert!(state.reassign_unused_spare(slot, Some(1), None));
        assert_eq!(state.owners[usize::from(slot)], 0);
        assert_eq!(state.refs[usize::from(slot)], 0);
        assert_eq!(state.active & (1u64 << slot), 0);
    }

    #[test]
    fn warming_quarantine_and_exit_hold_exclude_reassignment() {
        let mut state = State::new();
        let primary = state.acquire(1).unwrap();
        // A retained legacy sibling still obeys all quarantine/kill guards.
        state.owners[2] = 1;
        let spare = state.borrow_warm_sibling(1, primary).unwrap();
        assert!(!state.reassign_unused_spare(spare, Some(1), Some(2)));
        state.release(spare);
        state.quarantined |= 1u64 << spare;
        assert!(!state.reassign_unused_spare(spare, Some(1), Some(2)));
        state.quarantined = 0;
        state.kill_hold |= 1u64 << spare;
        assert!(!state.reassign_unused_spare(spare, Some(1), Some(2)));
        state.kill_hold = 0;
        assert!(!state.reassign_unused_spare(spare, Some(3), Some(2)));
        assert!(state.reassign_unused_spare(spare, Some(1), Some(2)));
        assert!(!state.reassign_unused_spare(primary, Some(1), None));
        state.release(primary);
        assert!(!state.reassign_unused_spare(primary, Some(1), None));
    }

    #[test]
    fn logical_queue_count_returns_on_drop() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let mut leases = KVec::with_capacity(MAX_EXECUTION_CONTEXTS as usize, GFP_KERNEL).unwrap();
        for _ in 0..MAX_EXECUTION_CONTEXTS {
            leases.push(pool.reserve_queue().unwrap(), GFP_KERNEL).unwrap();
        }
        assert!(matches!(pool.reserve_queue(), Err(ENOSPC)));
        assert_eq!(pool.queues.load(Ordering::Acquire), MAX_EXECUTION_CONTEXTS);
        drop(leases.pop());
        let replacement = pool.reserve_queue().unwrap();
        drop(leases);
        assert_eq!(pool.queues.load(Ordering::Acquire), 1);
        drop(replacement);
        assert_eq!(pool.queues.load(Ordering::Acquire), 0);
    }
}

/// Counted once per userspace logical queue, independently of installed graphs.
pub(crate) struct QueueSlot {
    pool: Arc<Pool>,
}
impl Drop for QueueSlot {
    fn drop(&mut self) {
        self.pool.queues.fetch_sub(1, Ordering::AcqRel);
    }
}

pub(crate) struct RenderLease {
    pool: Arc<Pool>,
    slot: u8,
    record: Option<(u64, u64)>,
}
impl RenderLease {
    /// The device lock proves this slot has no physical identities or graph.
    /// Give back an unbound reservation before retrying on a retained channel.
    pub(crate) fn cancel_empty(self, owner: u64) -> bool {
        let cancelled = {
            let mut state = self.pool.state.lock();
            state.cancel_empty_slot(self.slot, owner)
        };
        drop(self);
        cancelled
    }

    pub(crate) fn slot(&self) -> u8 {
        self.slot
    }
    /// `spared`: the failing work is innocent. Any guilty failure makes the slot's quarantine
    /// guilty for good; only a quarantine that starts spared and stays spared is marked spared.
    pub(crate) fn quarantine(&self, spared: bool) {
        let bit = 1u64 << self.slot;
        let mut state = self.pool.state.lock();
        if !spared {
            state.spared &= !bit;
        } else if state.quarantined & bit == 0 {
            state.spared |= bit;
        }
        state.quarantined |= bit;
        drop(state);
        self.pool.released.notify_all();
    }
    /// The VM pin must be retired first. Keep this lease until after clearing
    /// quarantine; its drop is what permits waiting publications to proceed.
    pub(crate) fn rehabilitate_after_processor_stop(&self) {
        let mut state = self.pool.state.lock();
        state.quarantined &= !(1u64 << self.slot);
        state.spared &= !(1u64 << self.slot);
    }
    /// Requires the runtime's closed-recovery, exact pair-drain witness.
    pub(crate) fn rehabilitate_after_firmware_drain(&self) {
        self.rehabilitate_after_processor_stop();
    }
}
impl Drop for RenderLease {
    fn drop(&mut self) {
        let mut state = self.pool.state.lock();
        if let Some((owner, sequence)) = self.record {
            if let Some(index) = state.footprints.iter().position(|entry| {
                entry.slot == Some(self.slot) && entry.owner == owner && entry.sequence == sequence
            }) {
                state.footprints.swap_remove(index);
            }
        }
        state.release(self.slot);
        drop(state);
        self.pool.released.notify_all();
    }
}

impl State {
    fn hold_for_exit_kill(&mut self, owner: u64, slots: u64) -> bool {
        if owner == 0 || slots & !Self::MASK != 0 {
            return false;
        }
        for slot in 0..cfg::RENDER_SLOTS {
            let bit = 1u64 << slot;
            if slots & bit == 0 {
                continue;
            }
            if self.owners[slot] != owner
                || self.refs[slot] != 0
                || self.active & bit != 0
                || self.released & bit == 0
            {
                return false;
            }
        }
        self.kill_hold |= slots;
        true
    }
}
impl Pool {
    /// Hold the departed owner's idle pairs before publishing its exit record.
    /// Successful publication retains these bits until both processors stop.
    pub(crate) fn hold_render_slots_for_exit_kill(&self, owner: u64, slots: u64) -> bool {
        self.state.lock().hold_for_exit_kill(owner, slots)
    }

    /// Undo only a hold whose control record was not published.
    pub(crate) fn release_exit_kill_hold(&self, slots: u64) {
        if slots == 0 {
            return;
        }
        self.state.lock().kill_hold &= !slots;
        self.released.notify_all();
    }
}

impl Pool {
    pub(crate) fn write_capacity(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "logical_queues used={} total={}", self.queues.load(Ordering::Acquire), self.max_queues)?;
        let state = self.state.lock();
        let retained = state.owners.iter().enumerate().fold(0u64, |bits, (slot, owner)| {
            bits | if *owner != 0 { 1u64 << slot } else { 0 }
        }) & State::MASK;
        let used = (state.active | state.quarantined | state.kill_hold | (retained & !state.released)) & State::MASK;
        writeln!(f, "render_slots used={} total={} retained={} active={} quarantined={} kill_held={}",
                 used.count_ones(), State::MASK.count_ones(), retained.count_ones(),
                 (state.active & State::MASK).count_ones(), (state.quarantined & State::MASK).count_ones(),
                 (state.kill_hold & State::MASK).count_ones())
    }
}

#[kunit_tests(asahi_g17_fault_admission)]
mod fault_capacity_tests {
    use super::*;

    #[test]
    fn a_guilty_failure_makes_a_spared_quarantine_guilty() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let queue = pool.reserve_queue().unwrap();
        let lease = pool.acquire_render(1, None, u64::MAX, || Ok(())).unwrap();
        let bit = 1u64 << lease.slot();
        lease.quarantine(true);
        assert_ne!(pool.state.lock().spared & bit, 0);
        lease.quarantine(false);
        assert_eq!(pool.state.lock().spared & bit, 0);
        // Once guilty, a later spared failure does not make it spared again.
        lease.quarantine(true);
        assert_eq!(pool.state.lock().spared & bit, 0);
        assert_ne!(pool.state.lock().quarantined & bit, 0);
        pool.release_owner(1);
        lease.rehabilitate_after_firmware_drain();
        drop(lease);
        drop(queue);
        let state = pool.state.lock();
        assert_eq!(state.quarantined | state.spared, 0);
    }

    #[test]
    fn closed_faulting_clients_return_leases_after_drain() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        for owner in 1..=512 {
            let queue = pool.reserve_queue().unwrap();
            let lease = pool.acquire_render(owner, None, u64::MAX, || Ok(())).unwrap();
            lease.quarantine(false);
            pool.release_owner(owner);
            let bit = 1u64 << lease.slot();
            assert_ne!(pool.state.lock().active & bit, 0);
            assert_ne!(pool.state.lock().quarantined & bit, 0);
            // The runtime, not an error fence, supplies this exact drain witness.
            lease.rehabilitate_after_firmware_drain();
            drop(lease);
            drop(queue);
            let state = pool.state.lock();
            assert_eq!(state.active, 0);
            assert_eq!(state.quarantined, 0);
            assert_eq!(state.owners.iter().enumerate().filter(|(slot, id)| **id != 0 && state.released & (1u64 << slot) == 0).count(), 0);
            assert_eq!(pool.queues.load(Ordering::Acquire), 0);
        }
    }

    #[test]
    fn published_exit_kill_holds_survive_close_and_drain() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let lease = pool.acquire_render(1, None, u64::MAX, || Ok(())).unwrap();
        let bit = 1u64 << lease.slot();
        pool.release_owner(1);
        drop(lease);
        assert!(pool.hold_render_slots_for_exit_kill(1, bit));
        assert!(!pool.reassign_unused_spare(bit.trailing_zeros() as u8, Some(1), Some(2)));
        // A failed (unpublished) control may roll back its hold. Published kills
        // keep the existing processor-stop rule; this test must not weaken it.
        pool.release_retained_owners_after_processor_stop();
        assert_eq!(pool.state.lock().kill_hold, 0);
    }
}

impl Pool {
    pub(crate) fn owner_active(&self, owner: u64) -> bool {
        let state = self.state.lock();
        state.owners.iter().zip(state.refs.iter()).any(|(id, refs)| *id == owner && *refs != 0)
    }
}

#[kunit_tests(asahi_g17_shared_render_depth)]
mod shared_depth_tests {
    use super::*;
    #[test]
    fn a_full_pair_waits_for_retirement_instead_of_allocating_a_spare() {
        let mut state = State::new();
        let first = state.acquire(1).unwrap();
        for _ in 1..cfg::RENDER_DEPTH { assert_eq!(state.acquire(1), Some(first)); }
        assert_eq!(state.acquire(1), None);
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 1);
        state.release(first);
        assert_eq!(state.acquire(1), Some(first));
    }
}

#[kunit_tests(asahi_g17_admission_deadline)]
mod deadline_tests {
    use super::*;

    #[test]
    fn all_pairs_busy_no_progress_expires_without_release() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        {
            let mut state = pool.state.lock();
            // No pair has room, including the requesting owner's retained pair.
            for slot in 1..cfg::RENDER_SLOTS {
                state.owners[slot] = slot as u64;
                state.refs[slot] = cfg::RENDER_DEPTH;
            }
            state.active = State::MASK;
        }
        let start = crate::g17::now_ns();
        let result = pool.acquire_render(1, None, start + 20_000_000, || Ok(()));
        assert!(matches!(result, Err(error) if error == EBUSY));
        assert!(crate::g17::now_ns() - start < 500_000_000);
        assert!(pool.state.lock().footprints.is_empty());
    }

    #[test]
    fn busy_pair_observes_cancellation_without_a_release_event() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        {
            let mut state = pool.state.lock();
            for slot in 1..cfg::RENDER_SLOTS {
                state.owners[slot] = slot as u64;
                state.refs[slot] = cfg::RENDER_DEPTH;
            }
            state.active = State::MASK;
        }
        let start = crate::g17::now_ns();
        let result = pool.acquire_render(1, None, start + 2_000_000_000, || {
            if crate::g17::now_ns() - start >= 10_000_000 { Err(ECANCELED) } else { Ok(()) }
        });
        assert!(matches!(result, Err(error) if error == ECANCELED));
        assert!(crate::g17::now_ns() - start < 500_000_000);
        assert!(pool.state.lock().footprints.is_empty());
    }

    #[test]
    fn cancellation_precedes_available_capacity() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        assert!(matches!(pool.acquire_render(1, None, u64::MAX, || Err(ECANCELED)),
                         Err(error) if error == ECANCELED));
        assert_eq!(pool.state.lock().active, 0);
    }

    #[test]
    fn unrelated_wake_storm_never_renews_absolute_deadline() {
        let deadline = 2_000_000_000;
        // Each wake is allowed to report the full jiffy budget remaining. Only
        // monotonic time, not that report or a wake count, bounds admission.
        for now in (0..deadline).step_by(100_000) {
            assert!(wait_slice_ms(deadline, now).unwrap() <= 10);
        }
        assert_eq!(wait_slice_ms(deadline, deadline), Err(EBUSY));
        assert_eq!(wait_slice_ms(deadline, deadline + 1), Err(EBUSY));
    }
}
