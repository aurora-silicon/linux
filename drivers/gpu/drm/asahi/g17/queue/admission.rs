// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Logical queue and physical render-pair admission. A retained pair belongs
//! to one logical owner even while idle. Closing that owner makes it reusable
//! only after every active lease retires. Quarantine keeps the slot from any new
//! lease until the runtime's drain witness of its pair or both processors stopping
//! makes reuse safe.
//!
//! The pool lock protects host accounting only and is never held while building
//! a graph or waiting for GPU work. Capacity waiters sleep on the release event.

use crate::g17::{
    fw::kick::KickDependency,
    hazard::{self, Footprint, Plan},
    job::{Order, Packet},
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
    /// Fragment kick of the published render; a later same-VM overlap of
    /// another pair waits for it in firmware.
    fragment: Option<KickDependency>,
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
    /// The owner of each slot. Written only by [`Self::set_owner`], which keeps `wide` with it.
    owners: [u64; cfg::RENDER_SLOTS],
    epochs: [u64; cfg::RENDER_SLOTS],
    quarantined: u64,
    /// Quarantined slots whose failed work is all spared (innocent): waiting publications fail
    /// their own fence with EBUSY instead of a device-loss error, so the VM stays usable.
    spared: u64,
    /// Quarantined slots whose pair the runtime's drain witness released while leases of the
    /// slot were still held ([`Pool::pair_drained`]): the quarantine ends when the last of them
    /// is dropped. A quarantined slot takes no new lease, and a new failure clears the bit.
    drained: u64,
    kill_hold: u64,
    /// Slots whose kill hold ended at their killed context's consumed scheduler release,
    /// and when. A new owner takes such a pair last, the one released longest ago first.
    killed: u64,
    killed_at: [u64; cfg::RENDER_SLOTS],
    /// Slots whose pair no other owner can bind: it keeps work nobody else can bind past (a
    /// quarantine, or a group whose stamps are still owed), or the scheduler release of the
    /// context it is bound to is pending. The slot's own live owner keeps using its pair, but no
    /// new or other owner is given it, even once it is released and has no lease left, until
    /// the runtime clears the bit (the pair drained, or the release was consumed and the pair
    /// parked) or both processors stop.
    held: u64,
    /// The physical queue table still has room for a pair built in an unowned slot. The
    /// runtime updates this whenever the table changes.
    build_room: bool,
    /// Slots that take their owner's renders that run on one of a two-pair queue's pairs
    /// ([`crate::g17::pairs::for_window`]), so the objects of a layout too large for two pairs
    /// exist in that pair only: at most one per owner ([`Self::mark_wide`]). A mark belongs to its
    /// owner's tenure of the slot: [`Self::set_owner`] drops it whenever the slot changes hands.
    wide: u64,
    /// Total dedicated render pairs the QID table can hold (installed plus buildable).
    /// Updated alongside `build_room` so sibling admission preserves first-pair capacity.
    render_capacity: usize,
    /// Live slots on which the current owner has published (or started) a
    /// firmware-visible render graph; unpublished siblings yield before
    /// published ones, and reclaiming a published sibling keeps its installed
    /// QIDs in the `released` state for `bind_owner()` reuse.
    published: u64,
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
            drained: 0,
            kill_hold: 0,
            killed: 0,
            killed_at: [0; cfg::RENDER_SLOTS],
            held: 0,
            build_room: true,
            wide: 0,
            render_capacity: cfg::RENDER_SLOTS - 1,
            published: 0,
            released: 0,
            epoch: 0,
            footprints: KVec::new(),
            next_ticket: 1,
        }
    }
    fn owns_live(&self, owner: u64, slot: usize) -> bool {
        self.owners.get(slot) == Some(&owner) && self.released & (1u64 << slot) == 0
    }
    fn has_live_owner(&self, owner: u64) -> bool {
        (1..cfg::RENDER_SLOTS).any(|slot| self.owns_live(owner, slot))
    }
    fn mark_published(&mut self, slot: u8) {
        let index = usize::from(slot);
        if index > 0 && index < cfg::RENDER_SLOTS {
            let bit = 1u64 << index;
            if self.owners[index] != 0 && self.released & bit == 0 {
                self.published |= bit;
            }
        }
    }
    fn live_owners(&self) -> usize {
        let mut count = 0usize;
        for slot in 1..cfg::RENDER_SLOTS {
            let owner = self.owners[slot];
            if owner == 0 || self.released & (1u64 << slot) != 0 {
                continue;
            }
            if !(1..slot).any(|prev| {
                self.owners[prev] == owner && self.released & (1u64 << prev) == 0
            }) {
                count += 1;
            }
        }
        count
    }
    fn live_siblings(&self) -> usize {
        let mut siblings = 0usize;
        for slot in 1..cfg::RENDER_SLOTS {
            let owner = self.owners[slot];
            if owner == 0 || self.released & (1u64 << slot) != 0 {
                continue;
            }
            if (1..slot).any(|prev| {
                self.owners[prev] == owner && self.released & (1u64 << prev) == 0
            }) {
                siblings += 1;
            }
        }
        siblings
    }
    fn exhausted(&self) -> bool {
        self.quarantined & Self::MASK == Self::MASK
    }
    /// The owner's quarantined pairs, if any: `Some(true)` when every one of
    /// them is spared (quarantined behind another VM's recovery). One
    /// quarantined pair stops the owner on all of its pairs, so a healthy
    /// sibling never bypasses it.
    fn owner_quarantine(&self, owner: u64) -> Option<bool> {
        let owned = (0..cfg::RENDER_SLOTS)
            .filter(|slot| self.owns_live(owner, *slot))
            .fold(0u64, |mask, slot| mask | 1u64 << slot);
        let quarantined = self.quarantined & owned;
        (quarantined != 0).then_some(quarantined & !self.spared == 0)
    }
    /// Released means closed: by its client, or by the kernel for a client that keeps it open
    /// (`Firmware::close_failed_owners`), which then never releases it again. A pair taken
    /// back, or a sibling claimed, would clear `released` and leak. Owner IDs are never reused.
    /// Checked where a lease is made only, so `admits_render` lets such a job fail at once.
    fn owner_released(&self, owner: u64) -> bool {
        (0..cfg::RENDER_SLOTS).any(|slot| self.owners[slot] == owner && self.released & (1u64 << slot) != 0)
    }
    fn refusal(&self, owner: u64) -> Result {
        if self.exhausted() {
            return Err(EIO);
        }
        if self.owner_quarantine(owner).is_some() {
            return Err(EIO);
        }
        if !self.has_live_owner(owner)
            && !self.owners[1..].contains(&0)
            && self.released
                & !self.active
                & !self.quarantined
                & !self.kill_hold
                    & !self.held
                & Self::MASK
                == 0
        {
            return Err(crate::g17::capacity::refused(crate::g17::capacity::Site::RenderSlots));
        }
        Ok(())
    }
    fn advance(&mut self, slot: usize) {
        self.epoch = self.epoch.wrapping_add(1);
        self.epochs[slot] = self.epoch;
    }
    /// Gives `slot` to `owner` (0: to nobody): the one writer of `owners`. A new owner starts
    /// without the slot's one-pair mark.
    fn set_owner(&mut self, slot: usize, owner: u64) {
        if self.owners[slot] != owner {
            self.wide &= !(1u64 << slot);
        }
        self.owners[slot] = owner;
    }
    fn take_idle(&mut self, slot: usize, owner: u64) -> u8 {
        if self.owners[slot] != owner || self.released & (1u64 << slot) != 0 {
            if self.owners[slot] != owner {
                crate::g17::trace::teardown(
                    6,
                    slot as u64,
                    self.owners[slot],
                    owner | u64::from(self.killed & (1u64 << slot) != 0) << 63,
                );
            }
            self.killed &= !(1u64 << slot);
            self.published &= !(1u64 << slot);
        }
        self.refs[slot] += 1;
        self.active |= 1u64 << slot;
        self.released &= !(1u64 << slot);
        self.set_owner(slot, owner);
        self.advance(slot);
        slot as u8
    }
    #[cfg(CONFIG_KUNIT = "y")]
    fn acquire(&mut self, owner: u64, pairs: usize) -> Option<u8> {
        self.acquire_for(owner, pairs, 0)
    }
    fn acquire_for(&mut self, owner: u64, pairs: usize, open_queues: usize) -> Option<u8> {
        if owner == 0 {
            return None;
        }
        // A shallow queue stays on its busy pair: a second pair pays off only
        // once the first has a backlog (at depth 2 a switch on every render
        // cost a 1080p fill about 45 us a submit on M5 Ultra).
        if pairs > 1 {
            let busy = (0..cfg::RENDER_SLOTS)
                .filter(|slot| {
                    self.owners[*slot] == owner
                        && self.refs[*slot] != 0
                        && self.active & (1u64 << slot) != 0
                        && self.usable(owner, *slot)
                })
                .min_by_key(|slot| (self.refs[*slot], u64::MAX - self.epochs[*slot]));
            if let Some(slot) = busy.filter(|slot| self.refs[*slot] <= crate::g17::pairs::sibling_backlog()) {
                self.refs[slot] += 1;
                self.advance(slot);
                return Some(slot as u8);
            }
        }
        // Prefer the owner's idle pair. A missing sibling can be claimed
        // only while this owner has no live kick, before constructing it.
        let idle = (0..cfg::RENDER_SLOTS)
            .filter(|slot| {
                self.owns_live(owner, *slot)
                    && self.refs[*slot] == 0
                    && self.quarantined & (1u64 << slot) == 0
                    && self.can_lease_slot(owner, *slot, open_queues)
            })
            .max_by_key(|slot| (self.epochs[*slot], usize::MAX - *slot));
        if let Some(slot) = idle {
            if !(0..cfg::RENDER_SLOTS)
                .any(|other| self.owns_live(owner, other) && self.refs[other] != 0)
            {
                self.claim_sibling(owner, pairs, open_queues);
            }
            return Some(self.take_idle(slot, owner));
        }
        if self.has_live_owner(owner) {
            // Consecutive kicks are ordered by firmware dependencies. Stack
            // onto the least-loaded retained pair without a retirement wait.
            let slot = (0..cfg::RENDER_SLOTS)
                .filter(|slot| {
                    self.owns_live(owner, *slot)
                        && self.quarantined & (1u64 << slot) == 0
                        && self.active & (1u64 << slot) != 0
                        && self.refs[*slot] < cfg::RENDER_DEPTH
                })
                .min_by_key(|slot| (self.refs[*slot], u64::MAX - self.epochs[*slot]))?;
            self.refs[slot] += 1;
            self.advance(slot);
            return Some(slot as u8);
        }
        let free = !self.active & Self::MASK & !self.quarantined & !self.kill_hold & !self.held;
        let eligible = |slot: usize| free & (1u64 << slot) != 0;
        let released = |slot: usize| self.released & (1u64 << slot) != 0;
        let unowned = || (0..cfg::RENDER_SLOTS).find(|slot| eligible(*slot) && self.owners[*slot] == 0);
        // Reuse a closed, exactly retired owner before consuming a new graph. A killed
        // context's pair comes after a new graph while one can still be built: on Neo, new
        // owners of such pairs had renders that executed but never stored their stamps.
        // Once the queue table is full it is reused, the one released longest ago first,
        // so killed pairs never strand physical queues.
        let slot = (0..cfg::RENDER_SLOTS)
            .filter(|slot| eligible(*slot) && released(*slot) && self.killed & (1u64 << slot) == 0)
            .min_by_key(|slot| self.epochs[*slot])
            .or_else(|| if self.build_room { unowned() } else { None })
            .or_else(|| {
                (0..cfg::RENDER_SLOTS)
                    .filter(|slot| eligible(*slot) && released(*slot))
                    .min_by_key(|slot| self.killed_at[*slot])
            })
            .or_else(unowned)?;
        let first = self.take_idle(slot, owner);
        self.claim_sibling(owner, pairs, open_queues);
        Some(first)
    }
    fn unavailable_released(&self) -> usize {
        (self.released
            & (self.active | self.quarantined | self.kill_hold | self.held)
            & Self::MASK)
            .count_ones() as usize
    }

    /// Idle live siblings in `eligible` (unpublished or drained published)
    /// that can be given back without taking any live owner's last usable pair.
    fn reclaimable_siblings_in(&self, eligible: u64) -> usize {
        let unavailable = self.quarantined | self.kill_hold | self.held;
        let mut reclaimable = 0usize;
        for slot in 1..cfg::RENDER_SLOTS {
            let owner = self.owners[slot];
            if owner == 0 || self.released & (1u64 << slot) != 0 {
                continue;
            }
            if (1..slot).any(|prev| {
                self.owners[prev] == owner && self.released & (1u64 << prev) == 0
            }) {
                continue;
            }
            let mut usable = 0usize;
            let mut candidates = 0usize;
            for s in slot..cfg::RENDER_SLOTS {
                let bit = 1u64 << s;
                if self.owners[s] != owner || self.released & bit != 0 {
                    continue;
                }
                if unavailable & bit == 0 {
                    usable += 1;
                    if eligible & bit != 0
                        && self.refs[s] == 0
                        && (self.active | self.killed) & bit == 0
                    {
                        candidates += 1;
                    }
                }
            }
            reclaimable += candidates.min(usable.saturating_sub(1));
        }
        reclaimable
    }

    fn reclaimable_siblings(&self) -> usize {
        self.reclaimable_siblings_in(Self::MASK)
    }

    /// Whether leasing `slot` for live `owner` would turn a reclaimable
    /// sibling into protected (non-reclaimable) demand.
    fn promotes_reclaimable_sibling(&self, owner: u64, slot: usize) -> bool {
        if slot == 0 || slot >= cfg::RENDER_SLOTS || owner == 0 {
            return false;
        }
        let bit = 1u64 << slot;
        let unavailable = self.quarantined | self.kill_hold | self.held;
        if self.owners[slot] != owner
            || self.released & bit != 0
            || self.refs[slot] != 0
            || (unavailable | self.active | self.killed) & bit != 0
        {
            return false;
        }
        let mut usable = 0usize;
        let mut candidates = 0usize;
        for s in 1..cfg::RENDER_SLOTS {
            let sbit = 1u64 << s;
            if self.owners[s] != owner || self.released & sbit != 0 {
                continue;
            }
            if unavailable & sbit == 0 {
                usable += 1;
                if self.refs[s] == 0 && (self.active | self.killed) & sbit == 0 {
                    candidates += 1;
                }
            }
        }
        usable > 1 && candidates < usable
    }

    /// Whether `owner` may take a lease on `slot` without pushing protected
    /// demand above `render_capacity`.
    fn can_lease_slot(&self, owner: u64, slot: usize, open_queues: usize) -> bool {
        if !self.promotes_reclaimable_sibling(owner, slot) {
            return true;
        }
        let cap = self.render_capacity.min(cfg::RENDER_SLOTS - 1);
        self.pair_demand(open_queues).saturating_add(1) <= cap
    }

    /// All render pairs currently spoken for, including reclaimable siblings
    /// claimed from surplus.
    fn total_pair_demand(&self, open_queues: usize) -> usize {
        open_queues
            .max(self.live_owners())
            .saturating_add(self.live_siblings())
            .saturating_add(self.unavailable_released())
    }

    /// Render pairs that cannot be given back: one first pair per open logical
    /// queue, non-reclaimable live siblings (busy, quarantined, or held), and
    /// teardown-held released pairs.
    fn pair_demand(&self, open_queues: usize) -> usize {
        open_queues
            .max(self.live_owners())
            .saturating_add(
                self.live_siblings()
                    .saturating_sub(self.reclaimable_siblings()),
            )
            .saturating_add(self.unavailable_released())
    }

    /// Claims another pair for an owner that may hold `pairs`, while keeping
    /// first-pair capacity for every open logical queue.
    fn claim_sibling(&mut self, owner: u64, pairs: usize, open_queues: usize) {
        if (1..cfg::RENDER_SLOTS)
            .filter(|slot| self.owns_live(owner, *slot))
            .count()
            >= pairs
        {
            return;
        }
        let needed = self.total_pair_demand(open_queues).saturating_add(1);
        if needed > self.render_capacity.min(cfg::RENDER_SLOTS - 1) {
            return;
        }
        // A speculative sibling never takes a killed context's pair.
        let free =
            !self.active & Self::MASK & !self.quarantined & !self.kill_hold & !self.killed & !self.held;
        let eligible = |slot: usize| free & (1u64 << slot) != 0;
        let sibling = (0..cfg::RENDER_SLOTS)
            .filter(|slot| {
                eligible(*slot)
                    && self.owners[*slot] != owner
                    && self.released & (1u64 << slot) != 0
            })
            .min_by_key(|slot| self.epochs[*slot])
            .or_else(|| {
                // Nor an unowned slot once no pair can be built there.
                if !self.build_room {
                    return None;
                }
                (0..cfg::RENDER_SLOTS).find(|slot| eligible(*slot) && self.owners[*slot] == 0)
            });
        if let Some(slot) = sibling {
            self.released &= !(1u64 << slot);
            self.published &= !(1u64 << slot);
            self.set_owner(slot, owner);
            // A speculative sibling has no GPU-use recency yet.
            self.epochs[slot] = 0;
        }
    }

    /// Whether [`Self::reassign_unused_spare`] would accept `slot` right now.
    fn can_reassign_unused_spare(
        &self,
        slot: u8,
        expected: Option<u64>,
        owner: Option<u64>,
    ) -> bool {
        let slot = usize::from(slot);
        if slot == 0
            || slot >= cfg::RENDER_SLOTS
            || owner.is_some_and(|owner| owner == 0 || self.has_live_owner(owner))
        {
            return false;
        }
        let bit = 1u64 << slot;
        let old = self.owners[slot];
        !(old == 0
            || self.killed & bit != 0
            || owner == Some(old)
            || expected.is_some_and(|expected| expected != old)
            || self.refs[slot] != 0
            || (self.active | self.quarantined | self.kill_hold | self.held) & bit != 0
            || (self.released & bit == 0
                && !self.owners.iter().enumerate().any(|(other, value)| {
                    other != slot
                        && *value == old
                        && self.released & (1u64 << other) == 0
                        && (self.quarantined | self.kill_hold | self.held) & (1u64 << other) == 0
                })))
    }
    /// The device lock separately proves the physical graph was never published.
    /// An open owner keeps another usable pair. Exclude every live borrower,
    /// including off-lock sibling construction; a closed owner needs no spare.
    /// A spare goes only to an owner without a pair: a second pair comes only
    /// from surplus, through claim_sibling.
    fn reassign_unused_spare(
        &mut self,
        slot: u8,
        expected: Option<u64>,
        owner: Option<u64>,
    ) -> bool {
        if !self.can_reassign_unused_spare(slot, expected, owner) {
            return false;
        }
        let slot = usize::from(slot);
        let bit = 1u64 << slot;
        self.set_owner(slot, owner.unwrap_or(0));
        self.released &= !bit;
        self.published &= !bit;
        self.epochs[slot] = 0;
        true
    }
    /// Reclaims an idle live sibling. When `keep_installed` is true, its
    /// physical slot retains installed render QIDs for reuse, so `owners[slot]`
    /// stays set with `released` marked (the same invariant as `release_owner`);
    /// otherwise the slot returns to the unowned state.
    fn release_unused_sibling(
        &mut self,
        slot: u8,
        expected: Option<u64>,
        keep_installed: bool,
    ) -> bool {
        let index = usize::from(slot);
        if index == 0 || index >= cfg::RENDER_SLOTS {
            return false;
        }
        let bit = 1u64 << index;
        if self.released & bit != 0 {
            return false;
        }
        if !self.can_reassign_unused_spare(slot, expected, None) {
            return false;
        }
        if keep_installed {
            self.released |= bit;
        } else {
            self.set_owner(index, 0);
            self.released &= !bit;
        }
        self.wide &= !bit;
        self.published &= !bit;
        self.epochs[index] = 0;
        true
    }
    /// Takes back one idle live sibling in `eligible`, preferring unpublished
    /// siblings before published ones and non-wide siblings before wide-marked
    /// ones. Slots in `installed` or `published` keep `released` set so their
    /// installed QIDs are reused first.
    fn reclaim_sibling(&mut self, eligible: u64, installed: u64) -> Option<u8> {
        for allow_published in [false, true] {
            for allow_wide in [false, true] {
                for slot in 1..cfg::RENDER_SLOTS {
                    let bit = 1u64 << slot;
                    if eligible & bit == 0
                        || (!allow_published && self.published & bit != 0)
                        || (!allow_wide && self.wide & bit != 0)
                    {
                        continue;
                    }
                    let keep_installed = (installed | self.published) & bit != 0;
                    if self.release_unused_sibling(slot as u8, None, keep_installed) {
                        return Some(slot as u8);
                    }
                }
            }
        }
        None
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
            && (self.quarantined | self.kill_hold | self.held) & bit == 0
        {
            self.set_owner(slot, 0);
            self.released &= !bit;
            self.published &= !bit;
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

    /// Whether `pick` would return a slot, without taking it.
    #[cfg(CONFIG_KUNIT = "y")]
    fn can_pick(&self, owner: u64, plan: Plan) -> bool {
        self.can_pick_for(owner, 0, plan)
    }

    /// Whether `pick_for` would return a slot, without taking it.
    fn can_pick_for(&self, owner: u64, open_queues: usize, plan: Plan) -> bool {
        let usable =
            |slot: usize| self.usable(owner, slot) && self.can_lease_slot(owner, slot, open_queues);
        match plan {
            Plan::Wait => false,
            Plan::Pair(slot) => usable(usize::from(slot)),
            Plan::Join(_) => (0..cfg::RENDER_SLOTS).any(usable),
            Plan::Any if owner == 0 => false,
            Plan::Any if self.has_live_owner(owner) => (0..cfg::RENDER_SLOTS).any(usable),
            Plan::Any => {
                let free = !self.active & Self::MASK & !self.quarantined & !self.kill_hold & !self.held;
                (0..cfg::RENDER_SLOTS).any(|slot| {
                    free & (1u64 << slot) != 0
                        && (self.released & (1u64 << slot) != 0 || self.owners[slot] == 0)
                })
            }
        }
    }

    /// The one pair a one-pair render (`pairs` 1) of an owner holding two pairs goes to: the pair
    /// its previous one-pair render took, while the owner holds it (`wide`). Its layout's objects
    /// then exist in that pair only.
    fn wide(&self, owner: u64, pairs: usize) -> Option<usize> {
        if pairs != 1
            || (1..cfg::RENDER_SLOTS)
                .filter(|slot| self.owns_live(owner, *slot))
                .count()
                < 2
        {
            return None;
        }
        (1..cfg::RENDER_SLOTS)
            .find(|slot| self.wide & (1u64 << slot) != 0 && self.owns_live(owner, *slot))
    }

    /// Marks `slot` as taking its owner's one-pair renders, instead of any other slot.
    fn mark_wide(&mut self, slot: usize) {
        for other in 1..cfg::RENDER_SLOTS {
            if self.owners[other] == self.owners[slot] {
                self.wide &= !(1u64 << other);
            }
        }
        self.wide |= 1u64 << slot;
    }

    /// [`Self::can_pick`] for a render that may use `pairs` of the owner's pairs.
    #[cfg(CONFIG_KUNIT = "y")]
    fn can_pick_render(&self, owner: u64, pairs: usize, plan: Plan) -> bool {
        self.can_pick_render_for(owner, pairs, 0, plan)
    }

    /// [`Self::can_pick_for`] for a render that may use `pairs` of the owner's pairs.
    fn can_pick_render_for(
        &self,
        owner: u64,
        pairs: usize,
        open_queues: usize,
        plan: Plan,
    ) -> bool {
        match (self.wide(owner, pairs), plan) {
            (None, plan) => self.can_pick_for(owner, open_queues, plan),
            (Some(_), Plan::Wait) => false,
            (Some(wide), Plan::Pair(slot)) if usize::from(slot) != wide => false,
            (Some(wide), _) => {
                self.usable(owner, wide) && self.can_lease_slot(owner, wide, open_queues)
            }
        }
    }

    #[cfg(CONFIG_KUNIT = "y")]
    fn pick(&mut self, owner: u64, pairs: usize, plan: Plan) -> Option<u8> {
        self.pick_for(owner, pairs, 0, plan)
    }

    fn pick_for(
        &mut self,
        owner: u64,
        pairs: usize,
        open_queues: usize,
        plan: Plan,
    ) -> Option<u8> {
        // A one-pair render of an owner holding two pairs goes only to the pair its previous
        // one-pair render took, while the owner holds it (see `wide`).
        if let Some(wide) = self.wide(owner, pairs) {
            return match plan {
                Plan::Wait => None,
                Plan::Pair(slot) if usize::from(slot) != wide => None,
                // The publication barrier orders a Join's overlap from this pair.
                Plan::Any | Plan::Pair(_) | Plan::Join(_) => {
                    self.join_for(owner, wide, open_queues)
                }
            };
        }
        let slot = match plan {
            Plan::Any => self.acquire_for(owner, pairs, open_queues),
            Plan::Wait => None,
            Plan::Pair(slot) => self.join_for(owner, usize::from(slot), open_queues),
            // The preferred pair, else the owner's least-loaded other pair:
            // the publication barrier orders the overlap from there.
            Plan::Join(slot) => self
                .join_for(owner, usize::from(slot), open_queues)
                .or_else(|| {
                    let other = (0..cfg::RENDER_SLOTS)
                        .filter(|other| {
                            *other != usize::from(slot)
                                && self.usable(owner, *other)
                                && self.can_lease_slot(owner, *other, open_queues)
                        })
                        .min_by_key(|other| (self.refs[*other], u64::MAX - self.epochs[*other]))?;
                    self.join_for(owner, other, open_queues)
                }),
        }?;
        if pairs == 1 {
            self.mark_wide(usize::from(slot));
        }
        Some(slot)
    }
    fn usable(&self, owner: u64, slot: usize) -> bool {
        self.owns_live(owner, slot)
            && (self.quarantined | self.kill_hold) & (1u64 << slot) == 0
            && self.refs[slot] < cfg::RENDER_DEPTH
    }
    #[cfg(CONFIG_KUNIT = "y")]
    fn join(&mut self, owner: u64, index: usize) -> Option<u8> {
        self.join_for(owner, index, 0)
    }
    fn join_for(&mut self, owner: u64, index: usize, open_queues: usize) -> Option<u8> {
        if !self.usable(owner, index) || !self.can_lease_slot(owner, index, open_queues) {
            return None;
        }
        if self.refs[index] == 0 {
            Some(self.take_idle(index, owner))
        } else {
            self.refs[index] += 1;
            self.advance(index);
            Some(index as u8)
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
                    fragment: None,
                },
                GFP_KERNEL,
            )
            .ok()?;
        Some((owner, request.sequence))
    }

    fn position(&self, slot: u8, (owner, sequence): (u64, u64)) -> Option<usize> {
        self.footprints.iter().position(|entry| {
            entry.slot == Some(slot) && entry.owner == owner && entry.sequence == sequence
        })
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
            let bit = 1u64 << slot;
            self.active &= !bit;
            // The last lease of a slot whose pair has drained ends its quarantine
            // (see `pair_drained`).
            if self.drained & bit != 0 {
                self.quarantined &= !bit;
                self.spared &= !bit;
                self.drained &= !bit;
            }
        }
    }

    /// Ends the quarantine of `slot`.
    fn rehabilitate(&mut self, slot: usize) {
        let bit = 1u64 << slot;
        self.quarantined &= !bit;
        self.spared &= !bit;
        self.drained &= !bit;
    }

    /// The runtime's drain witness released every group of the pair in `slot`. A quarantine
    /// that no lease of the slot is left to end ends now; returns whether it did. While leases
    /// remain, it ends with the last of them, so that a drained command still gives its VM
    /// pin back before its slot returns ([`RenderLease::rehabilitate_after_firmware_drain`]).
    fn pair_drained(&mut self, slot: usize) -> bool {
        let bit = (1u64 << slot) & Self::MASK;
        if self.quarantined & bit == 0 {
            return false;
        }
        if self.refs[slot] != 0 {
            self.drained |= bit;
            return false;
        }
        self.rehabilitate(slot);
        true
    }

    /// Returns whether the slot stopped being held.
    fn set_held(&mut self, slot: usize, held: bool) -> bool {
        if slot >= cfg::RENDER_SLOTS {
            return false;
        }
        let bit = (1u64 << slot) & Self::MASK;
        let was = self.held & bit != 0;
        if held {
            self.held |= bit;
        } else {
            self.held &= !bit;
        }
        was && !held
    }

    /// [`Self::set_held`] for every slot in `slots`, whose bits in `held` are the new ones.
    /// Returns whether a slot stopped being held.
    fn sync_held(&mut self, slots: u64, held: u64) -> bool {
        let slots = slots & Self::MASK;
        let cleared = self.held & slots & !held != 0;
        self.held = (self.held & !slots) | (held & slots);
        cleared
    }

    fn retained(&self) -> u64 {
        self.owners.iter().enumerate().fold(0u64, |bits, (slot, owner)| {
            bits | if *owner != 0 { 1u64 << slot } else { 0 }
        }) & Self::MASK
    }

    /// Slots a new owner cannot take now: leased, quarantined, kill-held, held, or retained by
    /// an open owner.
    fn used(&self) -> u64 {
        (self.active | self.quarantined | self.kill_hold | self.held | (self.retained() & !self.released))
            & Self::MASK
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
    /// Logical-queue creation waiting for a returned slot.
    waiters: Option<Arc<crate::g17::availability::Availability>>,
    /// Render publication waiting for render capacity: woken with every
    /// release event above.
    renders: Option<Arc<crate::g17::availability::Availability>>,
}
impl Pool {
    #[cfg(CONFIG_KUNIT = "y")]
    pub(crate) fn new(max_queues: u32) -> Result<Arc<Self>> {
        Self::with_waiters(max_queues, None, None)
    }
    pub(crate) fn with_waiters(
        max_queues: u32,
        waiters: Option<Arc<crate::g17::availability::Availability>>,
        renders: Option<Arc<crate::g17::availability::Availability>>,
    ) -> Result<Arc<Self>> {
        if max_queues > MAX_EXECUTION_CONTEXTS {
            return Err(EINVAL);
        }
        Arc::pin_init(
            pin_init!(Self {
                state <- new_mutex!(State::new(), "G17 render admission"),
                released <- new_condvar!("G17 render pair release"),
                queues: AtomicU32::new(0),
                max_queues,
                waiters,
                renders,
            }),
            GFP_KERNEL,
        )
    }
    /// Every pair or lease return, owner close and quarantine change.
    fn returned(&self) {
        self.released.notify_all();
        if let Some(renders) = &self.renders {
            renders.notify();
        }
    }
    /// Logical queues currently counted, including one being created.
    pub(crate) fn queues(&self) -> u32 {
        self.queues.load(Ordering::Acquire)
    }
    /// Whether one more logical queue fits, without reserving it. Creators
    /// check before reserving, so a refusal never returns (and notifies) a slot.
    pub(crate) fn has_room(&self) -> bool {
        self.queues() < self.max_queues
    }
    /// Render pairs needed so every open logical queue (plus `extra_queues`)
    /// keeps a first pair alongside non-reclaimable sibling and teardown-held
    /// pairs.
    pub(crate) fn render_demand(&self, extra_queues: usize) -> usize {
        let state = self.state.lock();
        let open = self.queues.load(Ordering::Acquire) as usize;
        state.pair_demand(open).saturating_add(extra_queues)
    }
    /// Whether admitting one more logical queue requires reclaiming a warmed
    /// (built, unpublished) sibling outside `reclaimable` before
    /// [`Self::try_reserve_queue_reclaiming`] can succeed.
    pub(crate) fn needs_warmed_sibling_reclaim(
        &self,
        render_capacity: usize,
        reclaimable: u64,
    ) -> bool {
        let state = self.state.lock();
        let cap = render_capacity.min(cfg::RENDER_SLOTS - 1);
        let open = self.queues.load(Ordering::Acquire) as usize;
        let total = state.total_pair_demand(open).saturating_add(1);
        total > cap && total.saturating_sub(state.reclaimable_siblings_in(reclaimable)) > cap
    }
    #[cfg(CONFIG_KUNIT = "y")]
    pub(crate) fn reserve_queue(self: &Arc<Self>) -> Result<QueueSlot> {
        self.try_reserve_queue(cfg::RENDER_SLOTS - 1)
    }
    #[cfg(CONFIG_KUNIT = "y")]
    pub(crate) fn try_reserve_queue(
        self: &Arc<Self>,
        render_capacity: usize,
    ) -> Result<QueueSlot> {
        self.try_reserve_queue_reclaiming(render_capacity, State::MASK, 0)
    }
    /// Reserves a logical queue slot if every open queue (including this one),
    /// every non-reclaimable sibling, and every teardown-held released pair
    /// fits within `render_capacity`, reclaiming idle unpublished siblings in
    /// `reclaimable` first so eager sibling claims never reduce queue capacity.
    pub(crate) fn try_reserve_queue_reclaiming(
        self: &Arc<Self>,
        render_capacity: usize,
        reclaimable: u64,
        installed: u64,
    ) -> Result<QueueSlot> {
        let mut state = self.state.lock();
        state.render_capacity = render_capacity;
        let cap = render_capacity.min(cfg::RENDER_SLOTS - 1);
        let mut count = self.queues.load(Ordering::Acquire);
        let mut reclaimed = false;
        loop {
            if count >= self.max_queues {
                drop(state);
                if reclaimed {
                    self.returned();
                }
                return Err(crate::g17::capacity::refused(
                    crate::g17::capacity::Site::LogicalQueues,
                ));
            }
            while state.total_pair_demand(count as usize).saturating_add(1) > cap {
                if state.reclaim_sibling(reclaimable, installed).is_none() {
                    break;
                }
                reclaimed = true;
            }
            let needed = state.total_pair_demand(count as usize).saturating_add(1);
            if needed > cap {
                drop(state);
                if reclaimed {
                    self.returned();
                }
                return Err(crate::g17::capacity::refused(
                    crate::g17::capacity::Site::LogicalQueues,
                ));
            }
            match self.queues.compare_exchange(
                count,
                count + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => {
                    drop(state);
                    if reclaimed {
                        self.returned();
                    }
                    return Ok(QueueSlot { pool: self.clone() });
                }
                Err(current) => count = current,
            }
        }
    }
    /// Reserves a compute QID while holding admission's state lock so sibling
    /// admission cannot race ahead of the updated render-pair capacity.
    pub(crate) fn reserve_compute_with_budget<T>(
        &self,
        reclaimable: u64,
        installed: u64,
        reserve: impl FnOnce(usize) -> Result<(T, bool, usize)>,
    ) -> Result<T> {
        let mut state = self.state.lock();
        let open = self.queues.load(Ordering::Acquire) as usize;
        let demand = state.pair_demand(open);
        let (value, room, capacity) = reserve(demand)?;
        let grew = (room && !state.build_room) || capacity > state.render_capacity;
        state.build_room = room;
        state.render_capacity = capacity;
        let cap = capacity.min(cfg::RENDER_SLOTS - 1);
        let mut reclaimed = false;
        while state.total_pair_demand(open) > cap {
            if state.reclaim_sibling(reclaimable, installed).is_none() {
                break;
            }
            reclaimed = true;
        }
        drop(state);
        if grew || reclaimed {
            self.returned();
        }
        Ok(value)
    }
    /// The owner's pair is quarantined behind another VM's recovery (spared) and drains.
    pub(crate) fn spared_quarantined(&self, owner: u64) -> bool {
        self.state.lock().owner_quarantine(owner) == Some(true)
    }

    /// For an owner that may hold `pairs` render pairs: its queue's count, or 1 for a render
    /// that runs on one of a two-pair queue's pairs ([`crate::g17::pairs::for_window`]).
    pub(crate) fn acquire_render(
        self: &Arc<Self>,
        owner: u64,
        pairs: usize,
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
            // A retained pair's quarantine is not bypassed through its
            // healthy sibling. Reclamation wakes this bounded admission wait.
            if let Some(spared) = state.owner_quarantine(owner) {
                // Behind an innocent pair's quarantine (another VM wedged the GPU) wait for the
                // pair to drain until the caller's deadline, which the caller extends while it
                // drains (RECOVERY_WAIT_MS); a guilty pair's publication fails after a second.
                if now >= quarantine_deadline && !spared {
                    return Err(EIO);
                }
                let slice = if spared {
                    remaining_ms
                } else {
                    remaining_ms.min(wait_slice_ms(quarantine_deadline, now)?)
                };
                if matches!(self.released.wait_interruptible_timeout(
                    &mut state, kernel::time::msecs_to_jiffies(slice)
                ), CondVarTimeoutResult::Signal { .. }) {
                    return Err(ERESTARTSYS);
                }
                continue;
            }
            if state.owner_released(owner) {
                return Err(ECANCELED);
            }
            state.refusal(owner)?;
            expired |= now >= conflict_deadline;
            let plan = if expired {
                Plan::Any
            } else {
                state.plan(owner, request, ticket)
            };
            let open_queues = self.queues.load(Ordering::Acquire) as usize;
            if let Some(slot) = state.pick_for(owner, pairs, open_queues, plan) {
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
        pairs: usize,
        request: Option<Request<'_>>,
    ) -> Result<Option<RenderLease>> {
        if owner == 0 {
            return Ok(None);
        }
        let mut state = self.state.lock();
        if state.owner_released(owner) || state.refusal(owner).is_err() || !state.has_live_owner(owner) {
            return Ok(None);
        }
        let plan = state.plan(owner, request, None);
        let open_queues = self.queues.load(Ordering::Acquire) as usize;
        let Some(slot) = state.pick_for(owner, pairs, open_queues, plan) else {
            return Ok(None);
        };
        let record = request.and_then(|request| state.record(owner, Some(slot), 0, request));
        Ok(Some(RenderLease {
            pool: self.clone(),
            slot,
            record,
        }))
    }
    /// Whether `acquire_render` would admit this request now, without
    /// admitting it: the scheduler's prepare step waits for this.
    pub(crate) fn admits_render(&self, owner: u64, pairs: usize, request: Option<Request<'_>>) -> bool {
        let state = self.state.lock();
        if owner == 0 || state.refusal(owner).is_err() {
            return false;
        }
        if state.owner_quarantine(owner).is_some() {
            return false;
        }
        let plan = state.plan(owner, request, None);
        let open_queues = self.queues.load(Ordering::Acquire) as usize;
        state.can_pick_render_for(owner, pairs, open_queues, plan)
    }
    pub(crate) fn warmable_sibling(
        self: &Arc<Self>,
        owner: u64,
        publishing: u8,
    ) -> Option<RenderLease> {
        let mut state = self.state.lock();
        let open_queues = self.queues.load(Ordering::Acquire) as usize;
        let candidate = state.warmable_sibling(owner, publishing)?;
        if !state.can_lease_slot(owner, usize::from(candidate), open_queues) {
            return None;
        }
        let slot = state.borrow_warm_sibling(owner, publishing)?;
        // This is a host construction borrower, not a GPU-use epoch. In
        // particular it must not clear a logical close or age an unused spare.
        Some(RenderLease {
            pool: self.clone(),
            slot,
            record: None,
        })
    }
    pub(crate) fn can_reassign_unused_spare(
        &self,
        slot: u8,
        expected: Option<u64>,
        owner: Option<u64>,
    ) -> bool {
        self.state
            .lock()
            .can_reassign_unused_spare(slot, expected, owner)
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
    pub(crate) fn release_unused_sibling(
        &self,
        slot: u8,
        expected: Option<u64>,
        keep_installed: bool,
    ) -> bool {
        let released = self
            .state
            .lock()
            .release_unused_sibling(slot, expected, keep_installed);
        if released {
            self.returned();
        }
        released
    }
    pub(crate) fn owns_slot(&self, owner: u64, slot: usize) -> bool {
        self.state.lock().owns_live(owner, slot)
    }
    pub(crate) fn owns_live_slot(&self, owner: u64) -> bool {
        self.state.lock().has_live_owner(owner)
    }
    pub(crate) fn is_wide(&self, slot: u8) -> bool {
        let index = usize::from(slot);
        index < cfg::RENDER_SLOTS && self.state.lock().wide & (1u64 << index) != 0
    }
    pub(crate) fn note_published(&self, slot: u8) {
        self.state.lock().mark_published(slot);
    }
    pub(crate) fn release_owner(&self, owner: u64) {
        let mut state = self.state.lock();
        for slot in 0..cfg::RENDER_SLOTS {
            if state.owners[slot] == owner {
                let bit = 1u64 << slot;
                state.released |= bit;
                state.wide &= !bit;
                state.published &= !bit;
            }
        }
        drop(state);
        self.returned();
    }
    /// Active leases survive this transition. Their later drop is still the
    /// event which releases capacity, so a stopped owner is never double-freed.
    pub(crate) fn release_retained_owners_after_processor_stop(&self) {
        let mut state = self.state.lock();
        let released = state.released & !state.active;
        for slot in 1..cfg::RENDER_SLOTS {
            if released & (1u64 << slot) != 0 {
                state.set_owner(slot, 0);
                state.epochs[slot] = 0;
            }
        }
        state.released &= !released;
        state.published &= !released;
        state.quarantined &= !released;
        state.spared &= !released;
        state.kill_hold = 0;
        state.killed = 0;
        // The stop settled every pair's work.
        state.held = 0;
        drop(state);
        self.returned();
    }

    /// Under the device mutex, each time the runtime puts a pair back: `held` while no other
    /// owner can bind the pair in `slot` (`render_hold` in the runtime). Set before that pass's
    /// retirements drop their leases, and before a closing owner's slots are released, so the
    /// slot is never free for another owner in between; clearing it wakes the waiters.
    pub(crate) fn set_held(&self, slot: usize, held: bool) {
        let cleared = self.state.lock().set_held(slot, held);
        if cleared {
            self.returned();
        }
    }

    /// [`Self::set_held`] for every slot in `slots` under one lock: the runtime re-derives
    /// them all where a scheduler release is armed or consumed, and on housekeeping passes.
    pub(crate) fn sync_held(&self, slots: u64, held: u64) {
        let cleared = self.state.lock().sync_held(slots, held);
        if cleared {
            self.returned();
        }
    }

    /// Under the device mutex, once the runtime's drain witness released every group of the
    /// pair in `slot` (`Pair::release_witnessed`): the firmware consumed the pair's ring
    /// entries and returned its free-list blocks, and either stored stamps that cover its last
    /// publication or acknowledged the kill of its context. It runs nothing of the pair and
    /// stores through none of its records, so the slot's quarantine ends.
    ///
    /// Otherwise only a lease a drained command still holds ends it
    /// ([`RenderLease::rehabilitate_after_firmware_drain`]). A group that retired on its pass
    /// record (`stamps_owed`) gave its lease back with its result, and a quarantine can reach
    /// the slot through that lease while the retirement is still deferred (a watchdog expiry
    /// retires the group, then fails its pair, in one pass): the quarantine then outlived every
    /// lease, and the slot stayed out of service until the processors stopped.
    pub(crate) fn pair_drained(&self, slot: usize) {
        if slot >= cfg::RENDER_SLOTS {
            return;
        }
        let ended = self.state.lock().pair_drained(slot);
        if ended {
            self.returned();
        }
    }

    /// Whether the physical queue table can hold another pair and how many
    /// dedicated render pairs fit in total (see `State::build_room` and
    /// `State::render_capacity`).
    pub(crate) fn note_build_room(&self, room: bool, capacity: usize) {
        let mut state = self.state.lock();
        if state.build_room != room || state.render_capacity != capacity {
            state.build_room = room;
            state.render_capacity = capacity;
            drop(state);
            self.returned();
        }
    }

    /// A killed context's pair is free for a new owner.
    pub(crate) fn killed_pair_free(&self) -> bool {
        let state = self.state.lock();
        state.killed & state.released & !state.active & !state.quarantined & !state.kill_hold & !state.held
            & State::MASK
            != 0
    }

}

#[kunit_tests(asahi_g17_admission_accounting)]
mod accounting_tests {
    use super::*;

    #[test]
    fn every_logical_owner_fits_without_speculative_spares() {
        let mut state = State::new();
        for owner in 1..=u64::from(MAX_EXECUTION_CONTEXTS) {
            let slot = state.acquire(owner, 1).unwrap();
            assert_eq!(state.owners.iter().filter(|id| **id == owner).count(), 1);
            state.release(slot);
        }
        assert_eq!(state.active, 0);
        assert_eq!(state.owners.iter().filter(|id| **id != 0).count(), MAX_EXECUTION_CONTEXTS as usize);
    }

    #[test]
    fn close_does_not_reuse_an_active_lease() {
        let mut state = State::new();
        let slot = state.acquire(1, 1).unwrap();
        state.released |= 1u64 << slot;
        assert!(!state.reassign_unused_spare(slot, Some(1), None));
        state.release(slot);
        assert!(state.reassign_unused_spare(slot, Some(1), None));
        assert_eq!(state.owners[usize::from(slot)], 0);
        assert_eq!(state.refs[usize::from(slot)], 0);
        assert_eq!(state.active & (1u64 << slot), 0);
    }

    #[test]
    fn a_drained_pair_ends_a_quarantine_no_lease_is_left_to_end() {
        let mut state = State::new();
        let slot = state.acquire(1, 1).unwrap();
        let (index, bit) = (usize::from(slot), 1u64 << slot);
        // Its lease carried a failure into admission, then went with a retirement on the
        // pass record; the owner closed.
        state.quarantined |= bit;
        state.release(slot);
        state.released |= bit;
        assert_ne!(state.used() & bit, 0);
        assert!(state.pair_drained(index));
        assert_eq!(state.used() & bit, 0);
        assert!(!state.pair_drained(index));
        // With leases left, the last of them ends it.
        state.refs[index] = 2;
        state.active |= bit;
        state.quarantined |= bit;
        assert!(!state.pair_drained(index));
        state.release(slot);
        assert_ne!(state.quarantined & bit, 0);
        state.release(slot);
        assert_eq!(state.quarantined | state.drained, 0);
    }

    #[test]
    fn warming_quarantine_and_exit_hold_exclude_reassignment() {
        let mut state = State::new();
        let primary = state.acquire(1, 1).unwrap();
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
    fn a_held_slot_goes_to_no_other_owner() {
        let mut state = State::new();
        state.build_room = false;
        for slot in 1..cfg::RENDER_SLOTS {
            state.owners[slot] = 100 + slot as u64;
        }
        // Two closed owners' pairs; the held one was released first.
        state.released = (1u64 << 5) | (1u64 << 9);
        state.epochs[5] = 1;
        state.epochs[9] = 2;
        assert!(!state.set_held(5, true));
        assert_ne!(state.used() & (1u64 << 5), 0);
        assert_eq!(state.acquire(1, 1), Some(9));
        assert!(state.refusal(2).is_err());
        assert_eq!(state.acquire(2, 1), None);
        assert!(state.set_held(5, false));
        assert_eq!(state.acquire(2, 1), Some(5));
    }

    #[test]
    fn a_sync_changes_only_the_slots_it_names() {
        let mut state = State::new();
        state.held = (1u64 << 3) | (1u64 << 4);
        // Slot 4 is lent out (not named): its bit stays. Slot 0 is no render slot.
        assert!(!state.sync_held((1u64 << 3) | (1u64 << 7) | 1, (1u64 << 3) | (1u64 << 7) | 1));
        assert_eq!(state.held, (1u64 << 3) | (1u64 << 4) | (1u64 << 7));
        assert!(state.sync_held((1u64 << 3) | (1u64 << 7), 1u64 << 7));
        assert_eq!(state.held, (1u64 << 4) | (1u64 << 7));
        assert!(!state.sync_held(1u64 << 3, 0));
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
impl QueueSlot {
    /// The device's admission pool, which lives as long as the firmware session.
    pub(crate) fn pool(&self) -> &Arc<Pool> {
        &self.pool
    }
    /// Gives back a slot no queue was published in, for a creator that sampled the waiters'
    /// domain as `observed` before claiming it. Returns the sample that creator waits on next
    /// ([`Availability::notify_own`]): its own return still wakes the other waiters, but not
    /// itself.
    ///
    /// [`Availability::notify_own`]: crate::g17::availability::Availability::notify_own
    pub(crate) fn cancel(self, observed: u64) -> u64 {
        let this = core::mem::ManuallyDrop::new(self);
        // SAFETY: `this` is never used or dropped again, so the pool reference moves out once.
        let pool = unsafe { core::ptr::read(&this.pool) };
        pool.queues.fetch_sub(1, Ordering::AcqRel);
        pool.waiters.as_ref().map_or(observed, |waiters| waiters.notify_own(observed))
    }
}
impl Drop for QueueSlot {
    fn drop(&mut self) {
        self.pool.queues.fetch_sub(1, Ordering::AcqRel);
        if let Some(waiters) = &self.pool.waiters {
            waiters.notify();
        }
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
        // A drain witnessed before this failure does not cover it.
        state.drained &= !bit;
        drop(state);
        self.pool.returned();
    }
    /// The VM pin must be retired first. Keep this lease until after clearing
    /// quarantine; its drop is what permits waiting publications to proceed.
    pub(crate) fn rehabilitate_after_processor_stop(&self) {
        self.pool.state.lock().rehabilitate(usize::from(self.slot));
    }
    /// Requires the runtime's closed-recovery, exact pair-drain witness.
    pub(crate) fn rehabilitate_after_firmware_drain(&self) {
        self.rehabilitate_after_processor_stop();
    }
}
impl RenderLease {
    /// Called under the device mutex once the render's kicks are committed,
    /// including a replay, which supersedes the earlier kick.
    pub(crate) fn note_publication(&self, fragment: KickDependency) {
        let mut state = self.pool.state.lock();
        state.mark_published(self.slot);
        let Some(record) = self.record else {
            return;
        };
        if let Some(index) = state.position(self.slot, record) {
            state.footprints[index].fragment = Some(fragment);
        }
    }
}

impl Pool {
    /// Under the device mutex, which orders every publication: the newest
    /// published fragment kick of each other pair whose live same-VM render
    /// overlaps `footprint`. Quarantined pairs are skipped as in admission.
    ///
    /// That includes the owner's own other pair, for its renders that `prefix`
    /// (the render's queue prefix) does not already order: an owner's renders
    /// on its two pairs run on different queues (see [`hazard::plan`]). An
    /// owner publishes in submission order (the backlog keeps inline
    /// publication behind queued commands, and the ordered run_job worker
    /// publishes one command at a time), so its earlier renders already carry
    /// their fragment kick here.
    pub(crate) fn publication_barriers(
        &self,
        owner: u64,
        slot: u8,
        footprint: &Footprint,
        prefix: Option<u64>,
        barriers: &mut [KickDependency; hazard::PUBLICATION_BARRIERS],
    ) -> Result<usize> {
        let state = self.state.lock();
        let mut slots = [0u8; hazard::PUBLICATION_BARRIERS];
        let mut count = 0;
        for entry in state.footprints.iter() {
            let (Some(other), Some(fragment)) = (entry.slot, entry.fragment) else {
                continue;
            };
            if other == slot
                || (entry.owner == owner && Order::contains(prefix, entry.sequence))
                || state.quarantined & (1u64 << other) != 0
                || !footprint.same_vm(&entry.footprint)
                || !footprint.overlaps(&entry.footprint)
            {
                continue;
            }
            match slots[..count].iter().position(|known| *known == other) {
                // One fragment ring completes in kick order; same QID, so the
                // word orders by timestamp.
                Some(index) => {
                    if fragment.word() > barriers[index].word() {
                        barriers[index] = fragment;
                    }
                }
                // Admission bounds the pairs; see hazard::plan.
                None if count == hazard::PUBLICATION_BARRIERS => return Err(EBUSY),
                None => {
                    slots[count] = other;
                    barriers[count] = fragment;
                    count += 1;
                }
            }
        }
        Ok(count)
    }
}

impl Drop for RenderLease {
    fn drop(&mut self) {
        let mut state = self.pool.state.lock();
        if let Some(record) = self.record {
            if let Some(index) = state.position(self.slot, record) {
                state.footprints.swap_remove(index);
            }
        }
        state.release(self.slot);
        drop(state);
        self.pool.returned();
    }
}

impl State {
    fn hold_for_exit_kill(&mut self, owner: u64, slots: u64, executed: u64) -> bool {
        if owner == 0 || slots & !Self::MASK != 0 {
            return false;
        }
        for slot in 0..cfg::RENDER_SLOTS {
            let bit = 1u64 << slot;
            if slots & bit == 0 {
                continue;
            }
            // A pair that only the kill releases is held with its commands (see
            // `Pair::releasable_by_kill`: they all ran, or they are a guilty context's).
            if self.owners[slot] != owner
                || (executed & bit == 0
                    && (self.refs[slot] != 0 || self.active & bit != 0 || self.released & bit == 0))
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
    /// Successful publication retains these bits until the owner's scheduler release is
    /// consumed (`release_killed_slots` at detach), or until both processors stop.
    pub(crate) fn hold_render_slots_for_exit_kill(&self, owner: u64, slots: u64, executed: u64) -> bool {
        self.state.lock().hold_for_exit_kill(owner, slots, executed)
    }

    /// Undo a hold whose control record was not published.
    pub(crate) fn release_exit_kill_hold(&self, slots: u64) {
        if slots == 0 {
            return;
        }
        self.state.lock().kill_hold &= !slots;
        self.returned();
    }

    /// Keep `slots` out of service until both processors stop: the killed guilty pairs
    /// `detach_context` kept there still have their queues installed. The kill held only the
    /// slots whose queues the scheduler page listed at the kill, so the hold is set here too.
    pub(crate) fn hold_kept_slots(&self, slots: u64) {
        if slots == 0 {
            return;
        }
        self.state.lock().kill_hold |= slots & State::MASK;
    }

    /// End the kill hold of `slots` once their killed context's scheduler release was
    /// consumed (the firmware no longer names these pairs). A new owner takes them last.
    pub(crate) fn release_killed_slots(&self, slots: u64) {
        let mut state = self.state.lock();
        let held = state.kill_hold & slots & State::MASK;
        if held == 0 {
            return;
        }
        let now = crate::g17::now_ns();
        state.kill_hold &= !held;
        state.killed |= held;
        for slot in 0..cfg::RENDER_SLOTS {
            if held & (1u64 << slot) != 0 {
                state.killed_at[slot] = now;
            }
        }
        drop(state);
        self.returned();
    }
}

impl Pool {
    pub(crate) fn write_capacity(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "logical_queues used={} total={}", self.queues.load(Ordering::Acquire), self.max_queues)?;
        let state = self.state.lock();
        writeln!(f, "render_slots used={} total={} retained={} active={} quarantined={} kill_held={} held={}",
                 state.used().count_ones(), State::MASK.count_ones(), state.retained().count_ones(),
                 (state.active & State::MASK).count_ones(), (state.quarantined & State::MASK).count_ones(),
                 (state.kill_hold & State::MASK).count_ones(), (state.held & State::MASK).count_ones())
    }
}

#[kunit_tests(asahi_g17_queue_slot_cancel)]
mod queue_slot_cancel_tests {
    use super::*;
    use crate::g17::availability::Availability;

    #[test]
    fn a_creator_does_not_wake_itself_by_giving_back_its_slot() -> Result {
        let waiters = Availability::new()?;
        let pool = Pool::with_waiters(MAX_EXECUTION_CONTEXTS, Some(waiters.clone()), None)?;
        let observed = waiters.observe();
        let slot = pool.reserve_queue()?;
        let next = slot.cancel(observed);
        assert_eq!(pool.queues(), 0);
        // The return advanced the domain, so other waiters wake, but the creator's next
        // sample is current: its wait registers instead of returning at once.
        assert_eq!(next, waiters.observe());
        assert_ne!(next, observed);
        assert!(waiters.wait(next)?.is_some());
        // A return by anyone else after the sample still ends the creator's wait.
        let observed = waiters.observe();
        let slot = pool.reserve_queue()?;
        drop(pool.reserve_queue()?);
        assert_eq!(slot.cancel(observed), observed);
        assert!(waiters.wait(observed)?.is_none());
        waiters.notify();
        Ok(())
    }
}

#[kunit_tests(asahi_g17_fault_admission)]
mod fault_capacity_tests {
    use super::*;

    #[test]
    fn a_guilty_failure_makes_a_spared_quarantine_guilty() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let queue = pool.reserve_queue().unwrap();
        let lease = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        let bit = 1u64 << lease.slot();
        assert!(!pool.spared_quarantined(1));
        lease.quarantine(true);
        assert_ne!(pool.state.lock().spared & bit, 0);
        assert!(pool.spared_quarantined(1));
        lease.quarantine(false);
        assert_eq!(pool.state.lock().spared & bit, 0);
        assert!(!pool.spared_quarantined(1));
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
    fn a_released_owner_takes_no_pair_again() {
        // The kernel closed a guilty owner for its open client, and its pair drained: an
        // early publication that passed the status check before the failure asks again.
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let queue = pool.reserve_queue().unwrap();
        let lease = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        let slot = lease.slot();
        drop(lease);
        pool.release_owner(1);
        assert!(pool.try_acquire_render(1, 2, None).unwrap().is_none());
        assert!(matches!(pool.acquire_render(1, 2, None, u64::MAX, || Ok(())), Err(ECANCELED)));
        // The scheduler's prepare step does not wait for it: publication fails the job at once.
        assert!(pool.admits_render(1, 2, None));
        let state = pool.state.lock();
        // Its pair stays released, and it claims no sibling.
        assert_ne!(state.released & (1u64 << slot), 0);
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 1);
        drop(state);
        // Another owner takes the pair.
        assert_eq!(pool.acquire_render(2, 1, None, u64::MAX, || Ok(())).unwrap().slot(), slot);
        drop(queue);
    }

    #[test]
    fn closed_faulting_clients_return_leases_after_drain() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        for owner in 1..=512 {
            let queue = pool.reserve_queue().unwrap();
            let lease = pool.acquire_render(owner, 1, None, u64::MAX, || Ok(())).unwrap();
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
        let lease = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        let bit = 1u64 << lease.slot();
        pool.release_owner(1);
        drop(lease);
        assert!(pool.hold_render_slots_for_exit_kill(1, bit, 0));
        assert!(!pool.reassign_unused_spare(bit.trailing_zeros() as u8, Some(1), Some(2)));
        // A failed (unpublished) control may roll back its hold. Published kills
        // keep the existing processor-stop rule; this test must not weaken it.
        pool.release_retained_owners_after_processor_stop();
        assert_eq!(pool.state.lock().kill_hold, 0);
    }

    #[test]
    fn a_kept_slot_stays_out_of_service_without_a_kill_hold() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let lease = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        let slot = lease.slot();
        let bit = 1u64 << slot;
        pool.release_owner(1);
        drop(lease);
        // The kill held nothing here (the scheduler page did not list the pair's queues).
        assert_eq!(pool.state.lock().kill_hold, 0);
        pool.hold_kept_slots(bit);
        pool.release_killed_slots(0);
        assert_eq!(pool.state.lock().kill_hold, bit);
        assert!(!pool.reassign_unused_spare(slot, Some(1), Some(2)));
        let next = pool.acquire_render(2, 1, None, u64::MAX, || Ok(())).unwrap();
        assert_ne!(next.slot(), slot);
        drop(next);
        pool.release_retained_owners_after_processor_stop();
        assert_eq!(pool.state.lock().kill_hold, 0);
    }

    #[test]
    fn consumed_scheduler_release_ends_the_kill_hold() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let lease = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        let slot = lease.slot();
        let bit = 1u64 << slot;
        pool.release_owner(1);
        drop(lease);
        assert!(pool.hold_render_slots_for_exit_kill(1, bit, 0));
        assert!(!pool.reassign_unused_spare(slot, Some(1), Some(2)));
        pool.release_killed_slots(bit);
        assert_eq!(pool.state.lock().kill_hold, 0);
        assert_eq!(pool.state.lock().killed, bit);
        assert!(pool.killed_pair_free());
        // The killed context's pair is never reassigned as a spare, and a new owner takes
        // a new graph before it.
        assert!(!pool.reassign_unused_spare(slot, Some(1), Some(2)));
        let next = pool.acquire_render(2, 1, None, u64::MAX, || Ok(())).unwrap();
        assert_ne!(next.slot(), slot);
        drop(next);
        pool.release_retained_owners_after_processor_stop();
        assert_eq!(pool.state.lock().killed, 0);
    }

    #[test]
    fn killed_pairs_are_taken_last_oldest_first() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        {
            let mut state = pool.state.lock();
            for slot in 1..cfg::RENDER_SLOTS {
                state.owners[slot] = 100 + slot as u64;
                state.released |= 1u64 << slot;
                state.killed |= 1u64 << slot;
                state.killed_at[slot] = 1000 - slot as u64;
            }
            // One pair released without a kill.
            state.killed &= !(1u64 << 7);
        }
        let first = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(first.slot(), 7);
        let second = pool.acquire_render(2, 1, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(usize::from(second.slot()), cfg::RENDER_SLOTS - 1);
        assert_eq!(pool.state.lock().killed & (1u64 << second.slot()), 0);
    }

    #[test]
    fn a_full_queue_table_reuses_killed_pairs_before_unowned_slots() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        {
            let mut state = pool.state.lock();
            for slot in 1..8 {
                state.owners[slot] = 100 + slot as u64;
                state.released |= 1u64 << slot;
                state.killed |= 1u64 << slot;
                state.killed_at[slot] = 1000 - slot as u64;
            }
        }
        // With room in the queue table a new owner builds a pair in an unowned slot.
        let built = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        assert!(usize::from(built.slot()) >= 8);
        // Without room it takes the killed pair released longest ago.
        pool.note_build_room(false, cfg::RENDER_SLOTS - 1);
        let reused = pool.acquire_render(2, 1, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(reused.slot(), 7);
        assert_eq!(pool.state.lock().killed & (1u64 << 7), 0);
        // It claims no sibling: neither another killed pair nor an unowned slot.
        let state = pool.state.lock();
        for slot in 1..7 {
            assert_eq!(state.owners[slot], 100 + slot as u64);
        }
        assert_eq!(state.owners.iter().filter(|owner| **owner == 2).count(), 1);
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
        let first = state.acquire(1, 1).unwrap();
        for _ in 1..cfg::RENDER_DEPTH { assert_eq!(state.acquire(1, 1), Some(first)); }
        assert_eq!(state.acquire(1, 1), None);
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 1);
        state.release(first);
        assert_eq!(state.acquire(1, 1), Some(first));
    }
}

#[kunit_tests(asahi_g17_owner_pairs)]
mod owner_pair_tests {
    use super::*;
    use crate::g17::fw::kick::KickTimestamp;

    fn kick(qid: u8, timestamp: u64) -> KickDependency {
        KickDependency::new(qid, KickTimestamp::new(timestamp).unwrap()).unwrap()
    }

    #[test]
    fn a_full_preferred_pair_moves_to_the_owners_other_pair() {
        let mut state = State::new();
        state.owners[1] = 1;
        state.owners[2] = 1;
        for _ in 0..cfg::RENDER_DEPTH {
            assert_eq!(state.pick(1, 2, Plan::Pair(1)), Some(1));
        }
        assert!(!state.can_pick(1, Plan::Pair(1)));
        assert_eq!(state.pick(1, 2, Plan::Pair(1)), None);
        assert!(state.can_pick(1, Plan::Join(1)));
        assert_eq!(state.pick(1, 2, Plan::Join(1)), Some(2));
        // With one pair a preference is the pair.
        state.owners[2] = 0;
        assert!(!state.can_pick(1, Plan::Join(1)));
        assert_eq!(state.pick(1, 2, Plan::Join(1)), None);
    }

    #[test]
    fn any_quarantined_pair_stops_its_owner() {
        let mut state = State::new();
        state.owners[1] = 1;
        state.owners[2] = 1;
        assert!(state.owner_quarantine(1).is_none());
        state.quarantined |= 1u64 << 2;
        assert_eq!(state.owner_quarantine(1), Some(false));
        assert!(state.refusal(1).is_err());
        state.spared |= 1u64 << 2;
        assert_eq!(state.owner_quarantine(1), Some(true));
        state.quarantined |= 1u64 << 1;
        assert_eq!(state.owner_quarantine(1), Some(false));
    }

    #[test]
    fn an_uncovered_own_render_on_the_other_pair_is_a_barrier() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        let mut footprint = Footprint::new(1, false);
        footprint.push_vm(0, 0x1000);
        let footprint = Arc::new(footprint, GFP_KERNEL).unwrap();
        {
            let mut state = pool.state.lock();
            for (slot, sequence, timestamp) in [(1u8, 4u64, 7u64), (1, 5, 9), (2, 6, 3)] {
                state
                    .footprints
                    .push(
                        InFlight {
                            owner: 1,
                            sequence,
                            slot: Some(slot),
                            ticket: 0,
                            footprint: footprint.clone(),
                            fragment: Some(kick(10 + slot, timestamp)),
                        },
                        GFP_KERNEL,
                    )
                    .unwrap();
            }
        }
        let mut barriers = [KickDependency::ZERO; hazard::PUBLICATION_BARRIERS];
        // On pair 2: pair 1's newest overlapping fragment kick, never its own.
        assert_eq!(pool.publication_barriers(1, 2, &footprint, None, &mut barriers).unwrap(), 1);
        assert!(barriers[0] == kick(11, 9));
        assert_eq!(pool.publication_barriers(1, 2, &footprint, Some(4), &mut barriers).unwrap(), 1);
        assert!(barriers[0] == kick(11, 9));
        // The queue prefix already orders them.
        assert_eq!(pool.publication_barriers(1, 2, &footprint, Some(5), &mut barriers).unwrap(), 0);
        // Another owner's render is a barrier whatever the prefix.
        assert_eq!(pool.publication_barriers(3, 2, &footprint, Some(9), &mut barriers).unwrap(), 1);
        assert_eq!(pool.publication_barriers(3, 3, &footprint, Some(9), &mut barriers).unwrap(), 2);
    }
}

#[kunit_tests(asahi_g17_two_pairs)]
mod two_pair_tests {
    use super::*;

    #[test]
    fn a_two_pair_owner_spreads_its_depth_over_a_surplus_sibling() {
        crate::g17::pairs::set_sibling_backlog(0);
        let mut state = State::new();
        let first = state.acquire(1, 2).unwrap();
        // The sibling is claimed with the first pair, from surplus.
        let sibling = (0..cfg::RENDER_SLOTS)
            .find(|slot| *slot != usize::from(first) && state.owners[*slot] == 1)
            .unwrap();
        // With the first pair busy, the next render takes the idle sibling, then
        // the least-loaded of the two.
        assert_eq!(usize::from(state.acquire(1, 2).unwrap()), sibling);
        let third = state.acquire(1, 2).unwrap();
        assert!(third == first || usize::from(third) == sibling);
        for _ in 3..2 * usize::from(cfg::RENDER_DEPTH) {
            assert!(state.acquire(1, 2).is_some());
        }
        assert_eq!(state.refs[usize::from(first)], cfg::RENDER_DEPTH);
        assert_eq!(state.refs[sibling], cfg::RENDER_DEPTH);
        assert_eq!(state.acquire(1, 2), None);
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 2);
        crate::g17::pairs::set_sibling_backlog(1);
    }

    #[test]
    fn a_shallow_two_pair_queue_stays_on_its_busy_pair() {
        crate::g17::pairs::set_sibling_backlog(1);
        let mut state = State::new();
        let first = state.acquire(1, 2).unwrap();
        // At depth 2 the second render stacks on the busy pair.
        assert_eq!(state.acquire(1, 2), Some(first));
        // With a backlog there, the third takes the idle sibling.
        let third = state.acquire(1, 2).unwrap();
        assert_ne!(third, first);
        assert_eq!(state.owners[usize::from(third)], 1);
        // Retired back to one render, the pair keeps the next one.
        state.release(first);
        state.release(third);
        assert_eq!(state.acquire(1, 2), Some(first));
    }

    #[test]
    fn a_sibling_needs_room_to_build_and_never_takes_a_spare() {
        let mut state = State::new();
        state.build_room = false;
        // A released pair of a closed owner is reused for the first pair; no
        // other free slot is claimed without room to build.
        state.owners[5] = 9;
        state.released |= 1u64 << 5;
        assert_eq!(state.acquire(1, 2), Some(5));
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 1);
        // An unused spare goes only to an owner without a pair.
        state.owners[6] = 3;
        state.released |= 1u64 << 6;
        assert!(!state.reassign_unused_spare(6, Some(3), Some(1)));
        assert!(state.reassign_unused_spare(6, Some(3), Some(2)));
    }

    #[test]
    fn a_one_pair_render_of_a_two_pair_owner_stays_on_one_pair() {
        crate::g17::pairs::set_sibling_backlog(1);
        let mut state = State::new();
        // Small renders take the first pair and, with a backlog there, the sibling.
        let first = state.pick(1, 2, Plan::Any).unwrap();
        assert_eq!(state.pick(1, 2, Plan::Any), Some(first));
        let sibling = state.pick(1, 2, Plan::Any).unwrap();
        assert_ne!(sibling, first);
        // A render whose layout the window holds once takes one of them, and every later such
        // render goes there too, even when the other pair is idle or less loaded.
        let wide = state.pick(1, 1, Plan::Any).unwrap();
        let other = if wide == first { sibling } else { first };
        state.release(other);
        for _ in state.refs[usize::from(wide)]..cfg::RENDER_DEPTH {
            assert_eq!(state.pick(1, 1, Plan::Any), Some(wide));
        }
        // At full depth it waits for that pair instead of taking the other.
        assert!(state.can_pick(1, Plan::Any));
        assert!(!state.can_pick_render(1, 1, Plan::Any));
        assert!(state.can_pick_render(1, 2, Plan::Any));
        assert_eq!(state.pick(1, 1, Plan::Any), None);
        assert_eq!(state.pick(1, 1, Plan::Join(other)), None);
        state.release(wide);
        assert_eq!(state.pick(1, 1, Plan::Join(other)), Some(wide));
        assert_eq!(state.pick(1, 1, Plan::Pair(other)), None);
        state.release(wide);
        assert_eq!(state.pick(1, 1, Plan::Pair(wide)), Some(wide));
        // A small render may still use either pair.
        assert_eq!(state.pick(1, 2, Plan::Pair(other)), Some(other));
    }

    #[test]
    fn a_two_pair_owner_whose_first_render_needs_one_pair_claims_no_sibling() {
        let mut state = State::new();
        // Two large renders in flight on a new queue share its one pair.
        let first = state.pick(1, 1, Plan::Any).unwrap();
        assert_eq!(state.pick(1, 1, Plan::Any), Some(first));
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 1);
    }

    #[test]
    fn a_slot_lost_and_claimed_back_drops_its_one_pair_mark() {
        let mut state = State::new();
        // A one-pair render of owner 1 takes slot `a`; its build fails before publication and
        // the slot goes back unowned.
        let a = state.pick(1, 1, Plan::Any).unwrap();
        assert!(state.cancel_empty_slot(a, 1));
        state.release(a);
        // Owner 2 takes `a` meanwhile, so owner 1's next one-pair render, which builds its
        // large layout's objects there, takes a slot `b` above it.
        state.build_room = false;
        assert_eq!(state.pick(2, 2, Plan::Any), Some(a));
        state.build_room = true;
        let b = state.pick(1, 1, Plan::Any).unwrap();
        assert!(b > a);
        state.release(b);
        // Owner 2 closes, and a small render of owner 1 claims `a` back as its sibling.
        state.release(a);
        state.released |= 1u64 << a;
        assert_eq!(state.pick(1, 2, Plan::Any), Some(b));
        assert_eq!(state.owners[usize::from(a)], 1);
        state.release(b);
        // The large layout's objects are in `b` alone, and its one-pair renders stay there.
        assert_eq!(state.pick(1, 1, Plan::Any), Some(b));
        assert!(state.can_pick_render(1, 1, Plan::Pair(b)));
        assert!(!state.can_pick_render(1, 1, Plan::Pair(a)));
        assert_eq!(state.pick(1, 1, Plan::Join(a)), Some(b));
    }

    #[test]
    fn a_slot_given_to_another_owner_drops_its_one_pair_mark() {
        let mut state = State::new();
        // Owner 1 holds two pairs, and its one-pair renders went to the lower one.
        state.owners[1] = 1;
        state.owners[2] = 1;
        assert_eq!(state.pick(1, 1, Plan::Pair(1)), Some(1));
        state.release(1);
        // That pair is given to owner 2 as its spare, and owner 1's next one-pair render, with
        // one pair left, builds its layout in the other.
        assert!(state.reassign_unused_spare(1, Some(1), Some(2)));
        assert_eq!(state.pick(1, 1, Plan::Any), Some(2));
        state.release(2);
        // Owner 2 closes, owner 1 claims the pair back, and its one-pair renders stay on the
        // pair that holds the layout.
        state.released |= 1u64 << 1;
        assert_eq!(state.pick(1, 2, Plan::Any), Some(2));
        assert_eq!(state.owners[1], 1);
        state.release(2);
        assert_eq!(state.pick(1, 1, Plan::Any), Some(2));
        assert!(!state.can_pick_render(1, 1, Plan::Pair(1)));
    }

    #[test]
    fn reclaim_prefers_non_wide_sibling_and_clears_wide_on_detach() {
        let mut state = State::new();
        // Owner 1 holds two published pairs; slot 1 holds its one-pair wide mark.
        state.set_owner(1, 1);
        state.set_owner(2, 1);
        state.published = (1u64 << 1) | (1u64 << 2);
        assert_eq!(state.pick(1, 1, Plan::Pair(1)), Some(1));
        state.release(1);
        assert_ne!(state.wide & (1u64 << 1), 0);
        // Reclaiming a sibling takes the non-wide slot (slot 2) instead of slot 1.
        assert_eq!(state.reclaim_sibling(State::MASK, State::MASK), Some(2));
        assert_ne!(state.released & (1u64 << 2), 0);
        assert_ne!(state.wide & (1u64 << 1), 0);
        // Reattach slot 2 and explicitly detach the wide slot with keep_installed=true:
        // the wide mark clears so reclaiming/re-acquiring slot 1 does not leave a stale mark.
        state.released &= !(1u64 << 2);
        assert!(state.release_unused_sibling(1, Some(1), true));
        assert_eq!(state.wide & (1u64 << 1), 0);
        assert_ne!(state.released & (1u64 << 1), 0);
        assert_eq!(state.owners[1], 1);
        // With only slot 2 live, the next one-pair render moves to slot 2.
        assert_eq!(state.pick(1, 1, Plan::Any), Some(2));
        state.release(2);
        // Detach with keep_installed=false also clears the wide mark and owner.
        state.set_owner(1, 1);
        state.released &= !(1u64 << 1);
        assert!(state.release_unused_sibling(2, Some(1), false));
        assert_eq!(state.owners[2], 0);
        assert_eq!(state.wide & (1u64 << 2), 0);
    }

    #[test]
    fn wide_render_respects_open_queue_capacity_gate_and_release_owner() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 4);
        let q1 = pool.try_reserve_queue(4).unwrap();
        let q2 = pool.try_reserve_queue(4).unwrap();
        let q3 = pool.try_reserve_queue(4).unwrap();
        // Owner 1 gets two pairs; mark `wide_slot` for one-pair renders, then leave it idle
        // while the other pair stays busy and capacity shrinks to 3 (= open queues).
        let a = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        let b = pool.acquire_render(1, 1, None, u64::MAX, || Ok(())).unwrap();
        let wide_slot = b.slot();
        assert!(pool.is_wide(wide_slot));
        drop(b);
        drop(a);
        let other = {
            let mut state = pool.state.lock();
            let other = (1..cfg::RENDER_SLOTS)
                .find(|s| *s != usize::from(wide_slot) && state.owns_live(1, *s))
                .unwrap();
            state.refs[other] = 1;
            state.active |= 1u64 << other;
            other as u8
        };
        pool.note_build_room(false, 3);
        {
            let mut state = pool.state.lock();
            // Promoting idle `wide_slot` would push pair_demand from 3 to 4 > cap 3.
            assert!(!state.can_pick_render_for(1, 1, 3, Plan::Any));
            assert_eq!(state.pick_for(1, 1, 3, Plan::Any), None);
            // With capacity 4, the marked slot is picked again.
            state.render_capacity = 4;
            assert!(state.can_pick_render_for(1, 1, 3, Plan::Any));
            assert_eq!(state.pick_for(1, 1, 3, Plan::Any), Some(wide_slot));
            state.release(wide_slot);
            state.release(other);
        }
        // release_owner clears the wide mark.
        pool.release_owner(1);
        assert!(!pool.is_wide(wide_slot));
        drop(q3);
        drop(q2);
        drop(q1);
    }

    #[test]
    fn a_one_pair_owner_claims_no_sibling() {
        let mut state = State::new();
        let first = state.acquire(1, 1).unwrap();
        assert_eq!(state.owners.iter().filter(|id| **id == 1).count(), 1);
        assert_eq!(state.acquire(1, 1), Some(first));
    }

    #[test]
    fn sibling_claim_preserves_first_pair_capacity_for_open_queues() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 2);
        let _q1 = pool.try_reserve_queue(2).unwrap();
        let _q2 = pool.try_reserve_queue(2).unwrap();
        // Two open queues and capacity for two pairs: neither an unowned slot
        // nor a released pair may be claimed as a sibling.
        {
            let mut state = pool.state.lock();
            state.owners[5] = 99;
            state.released |= 1u64 << 5;
        }
        let l1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(
            pool.state.lock().owners.iter().filter(|id| **id == 1).count(),
            1
        );
        // The second open queue still gets its guaranteed first pair.
        let l2 = pool.acquire_render(2, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_ne!(l1.slot(), l2.slot());
        assert_eq!(
            pool.state.lock().owners.iter().filter(|id| **id == 2).count(),
            1
        );
        drop(l1);
        drop(l2);
        // Once capacity exceeds open queues, a sibling is claimed from surplus.
        pool.note_build_room(true, 3);
        let l1_again = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(
            pool.state.lock().owners.iter().filter(|id| **id == 1).count(),
            2
        );
        // The idle unpublished sibling can still yield to a new queue.
        assert_eq!(pool.render_demand(0), 2);
        assert_eq!(pool.state.lock().total_pair_demand(2), 3);
        drop(l1_again);
    }

    #[test]
    fn idle_siblings_yield_to_open_queues_up_to_full_capacity() {
        use crate::g17::fw::kick::KickTimestamp;

        crate::g17::pairs::set_sibling_backlog(0);
        let kick = KickDependency::new(10, KickTimestamp::new(1).unwrap()).unwrap();
        let cap = 8usize;
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, cap);
        let mut queues = KVec::with_capacity(cap, GFP_KERNEL).unwrap();
        // First cap / 2 owners each publish on BOTH their primary and sibling
        // pairs, filling all `cap` slots with published graphs, then go idle.
        for owner in 1..=(cap / 2) as u64 {
            queues.push(pool.try_reserve_queue(cap).unwrap(), GFP_KERNEL).unwrap();
            let first = pool
                .acquire_render(owner, 2, None, u64::MAX, || Ok(()))
                .unwrap();
            first.note_publication(kick);
            let sibling = pool
                .acquire_render(owner, 2, None, u64::MAX, || Ok(()))
                .unwrap();
            assert_ne!(first.slot(), sibling.slot());
            sibling.note_publication(kick);
            {
                let state = pool.state.lock();
                assert_eq!(
                    (1..cfg::RENDER_SLOTS)
                        .filter(|slot| state.owns_live(owner, *slot))
                        .count(),
                    2
                );
                assert_ne!(state.published & (1u64 << sibling.slot()), 0);
            }
            drop(sibling);
            drop(first);
        }
        assert_eq!(pool.state.lock().total_pair_demand(cap / 2), cap);
        assert_eq!(pool.render_demand(0), cap / 2);
        // Opening queues beyond cap / 2 detaches idle published siblings (marking
        // them released while keeping their previous owner recorded) until full
        // capacity is reached and each new owner reuses a detached slot.
        for owner in (cap / 2 + 1) as u64..=cap as u64 {
            assert!(pool.render_demand(1) <= cap);
            queues.push(pool.try_reserve_queue(cap).unwrap(), GFP_KERNEL).unwrap();
            {
                let state = pool.state.lock();
                assert_ne!(state.released & State::MASK, 0);
                // Released slots retain their prior owner until rebound.
                for slot in 1..cfg::RENDER_SLOTS {
                    if state.released & (1u64 << slot) != 0 {
                        assert_ne!(state.owners[slot], 0);
                        assert_eq!(state.published & (1u64 << slot), 0);
                    }
                }
            }
            let lease = pool
                .acquire_render(owner, 2, None, u64::MAX, || Ok(()))
                .unwrap();
            lease.note_publication(kick);
            {
                let state = pool.state.lock();
                assert_eq!(
                    (1..cfg::RENDER_SLOTS)
                        .filter(|slot| state.owns_live(owner, *slot))
                        .count(),
                    1
                );
            }
            drop(lease);
        }
        assert_eq!(pool.queues() as usize, cap);
        assert!(matches!(pool.try_reserve_queue(cap), Err(ENOSPC)));
        for owner in 1..=cap as u64 {
            pool.release_owner(owner);
        }
        drop(queues);
        crate::g17::pairs::set_sibling_backlog(1);
    }

    #[test]
    fn queue_and_compute_admission_account_for_non_reclaimable_siblings() {
        crate::g17::pairs::set_sibling_backlog(0);
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 2);
        let q1 = pool.try_reserve_queue(2).unwrap();
        let l1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(
            pool.state.lock().owners.iter().filter(|id| **id == 1).count(),
            2
        );
        // An idle, unpublished sibling is reclaimable and does not inflate pair_demand.
        assert_eq!(pool.render_demand(0), 1);
        assert_eq!(pool.render_demand(1), 2);
        // Once owner 1 actually acquires its sibling, it is busy/used and cannot be reclaimed.
        let l2 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_ne!(l1.slot(), l2.slot());
        assert_eq!(pool.render_demand(0), 2);
        assert_eq!(pool.render_demand(1), 3);
        assert!(matches!(pool.try_reserve_queue(2), Err(ENOSPC)));
        // Compute reservation observes demand including the non-reclaimable sibling.
        let observed = pool
            .reserve_compute_with_budget(State::MASK, 0, |demand| Ok((demand, false, 2)))
            .unwrap();
        assert_eq!(observed, 2);
        assert!(!pool.state.lock().build_room);
        // Closing the sibling owner releases both pairs so a new queue fits.
        drop(l2);
        drop(l1);
        pool.release_owner(1);
        drop(q1);
        assert_eq!(pool.render_demand(0), 0);
        let _q2 = pool.try_reserve_queue(2).unwrap();
        crate::g17::pairs::set_sibling_backlog(1);
    }

    #[test]
    fn queue_admission_accounts_for_teardown_held_released_pairs() {
        crate::g17::pairs::set_sibling_backlog(0);
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 3);
        let q1 = pool.try_reserve_queue(3).unwrap();
        let l1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        // Owner 1 now holds a primary pair and an idle sibling; acquire the sibling so it is busy.
        let l2 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_ne!(l1.slot(), l2.slot());
        // Simulate a third pair that was released but is still held for teardown.
        let held_slot = (1..cfg::RENDER_SLOTS)
            .find(|slot| pool.state.lock().owners[*slot] == 0)
            .unwrap();
        {
            let mut state = pool.state.lock();
            state.owners[held_slot] = 99;
            state.released |= 1u64 << held_slot;
            state.held |= 1u64 << held_slot;
        }
        assert_eq!(pool.render_demand(0), 3);
        assert_eq!(pool.render_demand(1), 4);
        // All 3 pairs are non-reclaimable (2 busy + 1 teardown-held): queue 2 is refused.
        assert!(matches!(pool.try_reserve_queue(3), Err(ENOSPC)));
        // Compute reservation observes demand including the teardown-held pair.
        let observed = pool
            .reserve_compute_with_budget(State::MASK, 0, |demand| Ok((demand, false, 3)))
            .unwrap();
        assert_eq!(observed, 3);
        // Once teardown finishes and clears `held`, the released pair becomes reusable.
        pool.state.lock().held &= !(1u64 << held_slot);
        assert_eq!(pool.render_demand(0), 2);
        let _q2 = pool.try_reserve_queue(3).unwrap();
        drop(l2);
        drop(l1);
        drop(q1);
        crate::g17::pairs::set_sibling_backlog(1);
    }

    #[test]
    fn unpublished_sibling_lease_drop_stays_reclaimable() {
        use crate::g17::fw::kick::KickTimestamp;

        crate::g17::pairs::set_sibling_backlog(0);
        let kick = KickDependency::new(10, KickTimestamp::new(1).unwrap()).unwrap();
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 2);
        let q1 = pool.try_reserve_queue(2).unwrap();
        let l1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        l1.note_publication(kick);
        // Taking a sibling lease advances its epoch and protects it while busy.
        let l2 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        let sib = l2.slot();
        assert_ne!(l1.slot(), sib);
        assert_eq!(pool.render_demand(0), 2);
        assert!(matches!(pool.try_reserve_queue(2), Err(ENOSPC)));
        // Dropping the lease before publication leaves the sibling reclaimable.
        drop(l2);
        assert_ne!(pool.state.lock().epochs[usize::from(sib)], 0);
        assert_eq!(pool.state.lock().published & (1u64 << sib), 0);
        assert_eq!(pool.render_demand(0), 1);
        let q2 = pool.try_reserve_queue(2).unwrap();
        assert_eq!(
            pool.state.lock().owners.iter().filter(|id| **id == 1).count(),
            1
        );
        drop(q2);
        drop(l1);
        // A published sibling stays protected while busy, then yields once idle
        // by detaching its owner into `released` so a new queue reuses its QIDs.
        let l1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        let l2 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        let pub_sib = l2.slot();
        assert_ne!(l1.slot(), pub_sib);
        l2.note_publication(kick);
        assert_eq!(pool.render_demand(0), 2);
        assert!(matches!(pool.try_reserve_queue(2), Err(ENOSPC)));
        drop(l2);
        assert_eq!(pool.render_demand(0), 1);
        let q3 = pool.try_reserve_queue(2).unwrap();
        {
            let state = pool.state.lock();
            assert_eq!(state.owners[usize::from(pub_sib)], 1);
            assert_ne!(state.released & (1u64 << pub_sib), 0);
            assert_eq!(state.published & (1u64 << pub_sib), 0);
        }
        // note_published() must not re-set `published` on a released slot.
        pool.note_published(pub_sib);
        assert_eq!(pool.state.lock().published & (1u64 << pub_sib), 0);
        let l3 = pool.acquire_render(2, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(l3.slot(), pub_sib);
        drop(l3);
        drop(l1);
        pool.release_owner(1);
        pool.release_owner(2);
        drop(q3);
        drop(q1);
        assert_eq!(pool.render_demand(0), 0);
        crate::g17::pairs::set_sibling_backlog(1);
    }

    #[test]
    fn compute_shrink_gates_warmed_sibling_promotion() {
        use crate::g17::fw::kick::KickTimestamp;

        crate::g17::pairs::set_sibling_backlog(0);
        let kick = KickDependency::new(10, KickTimestamp::new(1).unwrap()).unwrap();
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 5);
        let q1 = pool.try_reserve_queue(5).unwrap();
        let q2 = pool.try_reserve_queue(5).unwrap();
        let q3 = pool.try_reserve_queue(5).unwrap();
        // Owners 1 and 2 publish primaries and claim siblings; owner 3 has not rendered yet.
        let p1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        p1.note_publication(kick);
        drop(p1);
        let p2 = pool.acquire_render(2, 2, None, u64::MAX, || Ok(())).unwrap();
        p2.note_publication(kick);
        drop(p2);
        assert_eq!(pool.state.lock().total_pair_demand(3), 5);
        assert_eq!(pool.render_demand(0), 3);
        // Compute shrinks render_capacity from 5 to 4 while both siblings stay warmed
        // (outside the lock-only reclaimable mask).
        pool.reserve_compute_with_budget(0, 0, |demand| {
            assert_eq!(demand, 3);
            Ok(((), false, 4))
        })
        .unwrap();
        // One sibling can still be promoted up to the shrunk capacity of 4.
        let p1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        let s1 = pool.acquire_render(1, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_ne!(p1.slot(), s1.slot());
        s1.note_publication(kick);
        assert_eq!(pool.render_demand(0), 4);
        // While owner 1's sibling is busy, owner 2's warmed sibling cannot promote
        // past capacity 4 and stays reclaimable for owner 3's guaranteed first pair.
        let p2 = pool.acquire_render(2, 2, None, u64::MAX, || Ok(())).unwrap();
        let s2 = (1..cfg::RENDER_SLOTS)
            .find(|slot| {
                *slot != usize::from(p2.slot()) && pool.state.lock().owns_live(2, *slot)
            })
            .unwrap() as u8;
        assert!(pool.warmable_sibling(2, p2.slot()).is_none());
        let p2_stacked = pool.acquire_render(2, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(p2_stacked.slot(), p2.slot());
        {
            let mut state = pool.state.lock();
            assert!(!state.can_pick_for(2, 3, Plan::Pair(s2)));
            assert_eq!(state.pick_for(2, 2, 3, Plan::Pair(s2)), None);
            assert_eq!(state.pick_for(2, 2, 3, Plan::Join(s2)), Some(p2.slot()));
            state.release(p2.slot());
        }
        drop(p2_stacked);
        drop(p2);
        assert_eq!(pool.render_demand(0), 4);
        assert!(pool.release_unused_sibling(s2, Some(2), true));
        assert_eq!(pool.state.lock().owners[usize::from(s2)], 2);
        assert_ne!(pool.state.lock().released & (1u64 << s2), 0);
        let p3 = pool.acquire_render(3, 2, None, u64::MAX, || Ok(())).unwrap();
        assert_eq!(p3.slot(), s2);
        drop(p3);
        drop(s1);
        drop(p1);
        for owner in 1..=3 {
            pool.release_owner(owner);
        }
        drop(q3);
        drop(q2);
        drop(q1);
        crate::g17::pairs::set_sibling_backlog(1);
    }

    #[test]
    fn render_lease_note_publication_ignores_released_slot() {
        use crate::g17::fw::kick::KickTimestamp;

        let kick = KickDependency::new(10, KickTimestamp::new(1).unwrap()).unwrap();
        let mut footprint = Footprint::new(1, false);
        footprint.push_vm(0, 0x1000);
        let footprint = Arc::new(footprint, GFP_KERNEL).unwrap();
        let request = Request {
            footprint: &footprint,
            sequence: 1,
            prefix: None,
        };
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        pool.note_build_room(true, 2);
        let q1 = pool.try_reserve_queue(2).unwrap();
        let lease = pool
            .acquire_render(1, 1, Some(request), u64::MAX, || Ok(()))
            .unwrap();
        let slot = lease.slot();
        let bit = 1u64 << slot;
        // Concurrent owner release while the pair is lent out off-lock must not
        // have `published` re-set when `lease.note_publication()` acquires the lock.
        pool.release_owner(1);
        lease.note_publication(kick);
        {
            let state = pool.state.lock();
            assert_eq!(state.published & bit, 0);
            assert_ne!(state.released & bit, 0);
            assert_eq!(state.owners[usize::from(slot)], 1);
            assert!(state
                .footprints
                .iter()
                .any(|entry| entry.slot == Some(slot) && entry.fragment.is_some()));
        }
        drop(lease);
        drop(q1);
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
        let result = pool.acquire_render(1, 1, None, start + 20_000_000, || Ok(()));
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
        let result = pool.acquire_render(1, 1, None, start + 2_000_000_000, || {
            if crate::g17::now_ns() - start >= 10_000_000 { Err(ECANCELED) } else { Ok(()) }
        });
        assert!(matches!(result, Err(error) if error == ECANCELED));
        assert!(crate::g17::now_ns() - start < 500_000_000);
        assert!(pool.state.lock().footprints.is_empty());
    }

    #[test]
    fn cancellation_precedes_available_capacity() {
        let pool = Pool::new(MAX_EXECUTION_CONTEXTS).unwrap();
        assert!(matches!(pool.acquire_render(1, 1, None, u64::MAX, || Err(ECANCELED)),
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
