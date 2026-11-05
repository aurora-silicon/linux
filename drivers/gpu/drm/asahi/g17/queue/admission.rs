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

struct State {
    active: u64,
    refs: [u8; cfg::RENDER_SLOTS],
    owners: [u64; cfg::RENDER_SLOTS],
    epochs: [u64; cfg::RENDER_SLOTS],
    quarantined: u64,
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
                Err(ENOSPC)
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
        for slot in 0..cfg::RENDER_SLOTS {
            if self.owners[slot] == owner
                && self.refs[slot] == 0
                && self.quarantined & (1u64 << slot) == 0
            {
                if !(0..cfg::RENDER_SLOTS)
                    .any(|other| self.owners[other] == owner && self.refs[other] != 0)
                {
                    self.claim_sibling(owner);
                }
                return Some(self.take_idle(slot, owner));
            }
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
            self.advance(slot);
        }
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
            })
            .map(|slot| slot as u8)
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
}
impl Pool {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                state <- new_mutex!(State::new(), "G17 render admission"),
                released <- new_condvar!("G17 render pair release"),
                queues: AtomicU32::new(0),
            }),
            GFP_KERNEL,
        )
    }
    pub(crate) fn reserve_queue(self: &Arc<Self>) -> Result<QueueSlot> {
        let mut count = self.queues.load(Ordering::Acquire);
        loop {
            if count >= MAX_EXECUTION_CONTEXTS {
                return Err(ENOSPC);
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
    ) -> Result<RenderLease> {
        if owner == 0 {
            return Err(EINVAL);
        }
        let mut state = self.state.lock();
        let mut conflict_budget = kernel::time::msecs_to_jiffies(hazard::WAIT_MS);
        let mut quarantine_budget = kernel::time::msecs_to_jiffies(QUARANTINE_WAIT_MS);
        let mut expired = false;
        let mut ticket = None;
        let result = (|| loop {
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
                if quarantine_budget == 0 {
                    return Err(EIO);
                }
                quarantine_budget = match self
                    .released
                    .wait_interruptible_timeout(&mut state, quarantine_budget)
                {
                    CondVarTimeoutResult::Signal { .. } => return Err(ERESTARTSYS),
                    CondVarTimeoutResult::Timeout => 0,
                    CondVarTimeoutResult::Woken { jiffies } => jiffies,
                };
                continue;
            }
            state.refusal(owner)?;
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
                match self
                    .released
                    .wait_interruptible_timeout(&mut state, conflict_budget)
                {
                    CondVarTimeoutResult::Signal { .. } => return Err(ERESTARTSYS),
                    CondVarTimeoutResult::Timeout => {
                        conflict_budget = 0;
                        expired = true;
                    }
                    CondVarTimeoutResult::Woken { jiffies } => conflict_budget = jiffies,
                }
            } else if self.released.wait_interruptible(&mut state) {
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
    pub(crate) fn warmable_sibling(&self, owner: u64, publishing: u8) -> Option<u8> {
        self.state.lock().warmable_sibling(owner, publishing)
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
        state.kill_hold = 0;
        drop(state);
        self.released.notify_all();
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
    pub(crate) fn slot(&self) -> u8 {
        self.slot
    }
    pub(crate) fn quarantine(&self) {
        self.pool.state.lock().quarantined |= 1u64 << self.slot;
        self.pool.released.notify_all();
    }
    /// The VM pin must be retired first. Keep this lease until after clearing
    /// quarantine; its drop is what permits waiting publications to proceed.
    pub(crate) fn rehabilitate_after_processor_stop(&self) {
        self.pool.state.lock().quarantined &= !(1u64 << self.slot);
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
