// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Device ownership of retained render pairs and their shared free-list leases.
//! The device mutex protects identities and publication. A preparation gate
//! lease permits graph construction outside that mutex without session teardown.

mod exit;
mod dependencies;
mod grow;
mod observe;
mod pools;
mod prepare;
mod recovery;
mod settle;
mod submit;
pub(super) mod teardown;

use super::{DeferredBatch, Registry};
use crate::g17::{context::Context, fw::queue::DataMaster, job::Packet, kick, queue::render::Pair};
use crate::hw::t8140::queues::RENDER_SLOTS;
use kernel::{prelude::*, sync::Arc};

#[derive(Copy, Clone)]
struct Config {
    clusters: u32,
    descriptor_flags: [u32; 2],
}

#[derive(Copy, Clone)]
struct Reservation {
    slot: u8,
    ids: [kick::Id; 2],
    owner: u64,
    buffer: (u8, u64),
}

struct Entry {
    reservation: Reservation,
    pair: Option<KBox<Pair>>,
    parked: Option<crate::g17::queue::render::parked::ParkedPair>,
    preparing: Option<Arc<Packet>>,
    context: Option<Arc<Context>>,
    pool: Option<Arc<crate::g17::freelist::RenderPool>>,
    pending_owner: Option<u64>,
    pending_owner_closed: bool,
    borrowed_active: bool,
    borrowed_dependencies: Option<crate::g17::queue::render::Dependencies>,
    event_masks: [u64; 2],
    closed: bool,
    pending_error: Option<(Arc<crate::g17::status::VmStatus>, Error)>,
    pending_timeout: bool,
    constructing: bool,
    growing: bool,
    published: bool,
}

pub(super) struct RenderState {
    config: Config,
    entries: KVec<Option<Entry>>,
    pools: pools::Pools,
    grow: grow::Grow,
    retired: KVec<crate::g17::queue::render::RetiredBinding>,
    next_pm_generation: u64,
}

impl RenderState {
    pub(super) fn new(clusters: u32, descriptor_flags: [u32; 2]) -> Result<Self> {
        if clusters == 0 {
            return Err(EINVAL);
        }
        let mut entries = KVec::with_capacity(RENDER_SLOTS, GFP_KERNEL)?;
        for _ in 0..RENDER_SLOTS {
            entries.push(None, GFP_KERNEL)?;
        }
        Ok(Self {
            config: Config {
                clusters,
                descriptor_flags,
            },
            entries,
            pools: pools::Pools::new()?,
            grow: grow::Grow::new()?,
            retired: KVec::with_capacity(128, GFP_KERNEL)?,
            next_pm_generation: 1,
        })
    }

    fn reserve_pm_generation(&mut self) -> Result<u64> {
        let generation = self.next_pm_generation;
        self.next_pm_generation = generation.checked_add(1).ok_or(EOVERFLOW)?;
        Ok(generation)
    }

    fn collect_retired(&mut self, slot: usize) -> Result {
        loop {
            if self.retired.len() == self.retired.capacity() {
                return Ok(());
            }
            let retired = self.entries[slot]
                .as_mut()
                .and_then(|entry| entry.pair.as_mut())
                .and_then(|pair| pair.take_retired_binding());
            let Some(retired) = retired else {
                return Ok(());
            };
            self.retired
                .push_within_capacity(retired)
                .map_err(|_| EIO)?;
        }
    }

    fn entry(&self, slot: u8) -> Result<&Entry> {
        self.entries
            .get(usize::from(slot))
            .and_then(Option::as_ref)
            .ok_or(EIO)
    }

    fn entry_mut(&mut self, slot: u8) -> Result<&mut Entry> {
        self.entries
            .get_mut(usize::from(slot))
            .and_then(Option::as_mut)
            .ok_or(EIO)
    }

    fn reserved(&mut self, reservation: Reservation) -> Result<&mut Entry> {
        let entry = self.entry_mut(reservation.slot)?;
        if entry.reservation.ids != reservation.ids
            || entry.reservation.owner != reservation.owner
            || entry.reservation.buffer != reservation.buffer
            || entry.pair.is_some()
            || entry.preparing.is_some()
            || entry.context.is_some()
            || !entry.constructing
        {
            return Err(EIO);
        }
        Ok(entry)
    }

    /// Keep metadata while graph preparation owns the pair outside the mutex.
    /// Completion scans skip it; cancellation is recorded and applied on return.
    fn take_pair(&mut self, slot: u8, owner: u64, packet: &Arc<Packet>) -> Result<KBox<Pair>> {
        let entry = self.entry_mut(slot)?;
        if entry.closed
            || entry.reservation.owner != owner
            || entry.preparing.is_some()
            || entry.pending_error.is_some()
            || entry.pending_timeout
        {
            return Err(EBUSY);
        }
        let pair = entry.pair.as_ref().ok_or(EIO)?;
        if !pair.matches(owner, &packet.context)? {
            return Err(EIO);
        }
        entry.borrowed_active = pair.published_in_flight();
        entry.borrowed_dependencies = Some(pair.dependency_snapshot());
        entry.preparing = Some(packet.clone());
        entry.pair.take().ok_or(EIO)
    }

    /// Validate before taking the owner from the caller. Even rejected returns
    /// keep their backing alive until the caller resolves the reservation.
    fn return_pair(&mut self, slot: u8, pair: &mut Option<KBox<Pair>>) -> Result {
        let returning = pair.as_ref().ok_or(EINVAL)?;
        let entry = self.entry_mut(slot)?;
        if entry.pair.is_some()
            || entry.preparing.is_none()
            || returning.slot() != slot
            || returning.qids() != entry.reservation.ids.map(kick::Id::qid)
            || returning.owner() != Some(entry.reservation.owner)
        {
            return Err(EIO);
        }
        entry.pair = pair.take();
        entry.borrowed_dependencies = None;
        entry.preparing = None;
        Ok(())
    }

    fn save_detaching_pool(&self, init: &crate::g17::initdata::InitData, slot: u8) -> Result {
        let pool = self.entry(slot)?.pool.as_ref().ok_or(EIO)?;
        let shared = self.entries.iter().enumerate().any(|(index, entry)| {
            index != usize::from(slot)
                && entry.as_ref().is_some_and(|entry| {
                    entry.pool.as_ref().is_some_and(|other| {
                        other.id() == pool.id() && other.control_va() == pool.control_va()
                    })
                })
        });
        if !shared {
            self.pools.save(init, pool)?;
        }
        Ok(())
    }
}

impl Registry {
    fn ensure_render_ids(&mut self, slot: u8, owner: u64) -> Result {
        if owner == 0 || slot == 0 || usize::from(slot) >= RENDER_SLOTS {
            return Err(EINVAL);
        }
        if self.render.entries[usize::from(slot)].is_none() {
            let identity = self.identities.reserve(2)?.start;
            let tiling = self.qids.reserve(identity, DataMaster::Tiling, None)?;
            let fragment = match self.qids.reserve(identity + 1, DataMaster::Fragment, None) {
                Ok(id) => id,
                Err(error) => {
                    self.qids.cancel_unpublished(tiling)?;
                    return Err(error);
                }
            };
            self.render.entries[usize::from(slot)] = Some(Entry {
                reservation: Reservation {
                    slot,
                    ids: [tiling, fragment],
                    owner,
                    buffer: (0, 0),
                },
                pair: None,
                parked: None,
                preparing: None,
                context: None,
                pool: None,
                pending_owner: None,
                pending_owner_closed: false,
                borrowed_active: false,
                borrowed_dependencies: None,
                event_masks: [0; 2],
                closed: false,
                pending_error: None,
                pending_timeout: false,
                constructing: false,
                growing: false,
                published: false,
            });
        }
        Ok(())
    }

    /// Only an optional sibling's unpublished QID-capacity refusal is soft.
    /// ensure_render_ids cancels a first ID if the second cannot be reserved;
    /// validation, identity overflow, and cancellation errors still propagate.
    fn prepare_render_ids(&mut self, slot: u8, owner: u64, optional: bool) -> Result<bool> {
        match self.ensure_render_ids(slot, owner) {
            Ok(()) => Ok(true),
            Err(error) if optional && error == ENOSPC => Ok(false),
            Err(error) => Err(error),
        }
    }

    fn reserve_render(&mut self, slot: u8, owner: u64) -> Result<Reservation> {
        self.ensure_render_ids(slot, owner)?;
        let entry = self.render.entry(slot)?;
        if entry.constructing
            || entry.pair.is_some()
            || entry.parked.is_some()
            || entry.preparing.is_some()
            || entry.growing
        {
            return Err(EBUSY);
        }
        let ids = self.render.entry(slot)?.reservation.ids;
        // Physical queue identities survive failed graph construction. Only
        // failure to acquire the second identity can return the first one.
        for id in ids {
            self.qids.publish(id)?;
        }
        let buffer = self.buffers.allocate_render()?;
        let entry = self.render.entry_mut(slot)?;
        entry.reservation.owner = owner;
        entry.reservation.buffer = buffer;
        entry.constructing = true;
        entry.closed = false;
        Ok(entry.reservation)
    }

    fn install_render(
        &mut self,
        reservation: Reservation,
        pair: &mut Option<KBox<Pair>>,
    ) -> Result {
        let built = pair.as_ref().ok_or(EINVAL)?;
        if built.slot() != reservation.slot
            || built.qids() != reservation.ids.map(kick::Id::qid)
            || built.owner() != Some(reservation.owner)
            || built.buffer_id() != reservation.buffer.0
        {
            return Err(EIO);
        }
        if self.render.reserved(reservation)?.closed {
            return Err(ECANCELED);
        }
        // Construction holds one PB reference; retain the owner independently
        // and return that reference before the first command acquires its own.
        self.buffers.reserve_owner(reservation.buffer.0)?;
        self.buffers.release(reservation.buffer.0)?;
        let entry = self.render.reserved(reservation)?;
        entry.context = Some(built.context().clone());
        entry.pool = Some(built.pool().clone());
        entry.pair = pair.take();
        entry.constructing = false;
        Ok(())
    }

    /// Drop the rejected graph outside the device mutex before returning its
    /// temporary PB reference. The physical queue identities remain reserved.
    fn cancel_render_reservation(&mut self, reservation: Reservation) -> Result {
        self.render.reserved(reservation)?;
        self.buffers.release(reservation.buffer.0)?;
        self.render.entry_mut(reservation.slot)?.constructing = false;
        Ok(())
    }

    pub(super) fn fail_render_vm(
        &mut self,
        status: &Arc<crate::g17::status::VmStatus>,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        for entry in self.render.entries.iter_mut().flatten() {
            if !entry
                .context
                .as_ref()
                .is_some_and(|context| Arc::ptr_eq(context.status(), status))
            {
                continue;
            }
            if let Some(pair) = entry.pair.as_mut() {
                pair.quarantine(error, &mut |event| deferred.push(event))?;
            } else if entry.preparing.is_some() || entry.growing {
                entry.pending_error = Some((status.clone(), error));
            }
        }
        Ok(())
    }
}

struct Host<'a> {
    firmware: &'a mut crate::g17::Firmware,
    registration_powered: bool,
}
impl Host<'_> {
    fn release_registration(&mut self) {
        if !self.registration_powered && !self.firmware.registration_power_release_pending {
            return;
        }
        if let Err(error) = self.firmware.release_registration_power() {
            dev_warn!(
                self.firmware.primary.state.shared.dev.as_ref(),
                "Could not restore render idle policy: {:?}\n",
                error
            );
        } else {
            self.registration_powered = false;
        }
    }
}
impl crate::g17::queue::render::Host for Host<'_> {
    fn epoch(&self) -> Result<(u64, u32)> {
        Ok((
            self.firmware.recovery.generation(),
            crate::g17::recovery::Memory::recovery_state(&self.firmware.init)?,
        ))
    }
    fn stamps(&self, qids: [u8; 2]) -> Result<[u32; 2]> {
        use core::sync::atomic::Ordering;
        let stamps = [
            self.firmware.init.stamp(qids[0])?.load(Ordering::Relaxed),
            self.firmware.init.stamp(qids[1])?.load(Ordering::Relaxed),
        ];
        core::sync::atomic::fence(Ordering::Acquire);
        Ok(stamps)
    }
    fn qos_publish(
        &mut self,
        owner: crate::g17::qos::Owner,
        scheduler: u64,
        policy: crate::g17::fw::queue::Policy,
    ) -> Result<crate::g17::qos::Publication> {
        use crate::g17::qos;
        self.firmware.queues.accounting.publish(
            &qos::View::new(self.firmware.init.qos()?)?,
            owner,
            scheduler,
            policy,
            qos::clock,
        )
    }
    fn qos_cancel(&mut self, publication: crate::g17::qos::Publication) -> Result {
        use crate::g17::qos;
        self.firmware
            .queues
            .accounting
            .cancel(&qos::View::new(self.firmware.init.qos()?)?, publication)
    }
    /// Idle descent is held for the whole first install. A point-in-time power
    /// check let the firmware gate the cores between the host's configuration
    /// writes while several clients installed their first pairs at once.
    fn install_pair(&mut self, registration: &crate::g17::fw::kick::RenderRegistration) -> Result {
        if !self.registration_powered {
            self.firmware.acquire_registration_power()?;
            self.registration_powered = true;
        }
        for _ in 0..3 {
            if self
                .firmware
                .bridge
                .regs
                .install_render_pair(registration)?
            {
                return Ok(());
            }
        }
        Err(EAGAIN)
    }
    fn publish_qid(&mut self, id: kick::Id) -> Result {
        self.firmware.queues.qids.publish(id)
    }
    fn publish_outer_pair(
        &mut self,
        priority: u8,
        fragment: &crate::g17::fw::channels::WorkSlot,
        tiling: &crate::g17::fw::channels::WorkSlot,
        commit: impl FnOnce() -> Result,
        late_tiling: impl FnOnce(),
    ) -> Result<[u32; 2]> {
        self.firmware
            .init
            .publish_work_pair(priority, fragment, tiling, true, commit, late_tiling)
    }
    fn activate_render_pool(&mut self, id: u16, generation: u64) -> Result {
        self.release_registration();
        let result = self.firmware.queues.render.pools.activate(id, generation);
        if result.is_err() {
            self.firmware.primary.state.shared.lose_device_quietly();
        }
        result
    }
    fn publish_retained_pair(
        &mut self,
        priority: u8,
        fragment: &crate::g17::fw::channels::WorkSlot,
        tiling: &crate::g17::fw::channels::WorkSlot,
        next: [u32; 2],
    ) -> Result {
        let result = self
            .firmware
            .init
            .publish_retained_work_pair(priority, fragment, tiling, next);
        if result.is_err() {
            self.firmware.primary.state.shared.lose_device_quietly();
        }
        result
    }
    /// Pipe 0 (tiling) in bits 1:0, priority class in bits 3:2.
    fn notify_render(&mut self, priority: u8) -> Result {
        self.release_registration();
        let result = self
            .firmware
            .primary
            .notify((0x83 << crate::g17::MSG_TYPE_SHIFT) | u64::from(priority) << 2);
        if result.is_err() {
            self.firmware.primary.state.shared.lose_device_quietly();
        }
        result
    }
    fn note_submission(&mut self) -> Result {
        self.firmware.note_submission()
    }
    fn complete_render(
        &mut self,
        owners: Option<[crate::g17::qos::Owner; 2]>,
        pool_id: u16,
        generation: u64,
        buffer_id: u8,
    ) -> Result {
        use crate::g17::qos;
        let queues = &mut self.firmware.queues;
        let next = queues.render.pools.completed(pool_id, generation)?;
        queues.buffers.can_release(buffer_id)?;
        let view = qos::View::new(self.firmware.init.qos()?)?;
        if let Some(owners) = owners {
            queues.accounting.complete(&view, &owners)?;
        }
        queues.render.pools.commit_completed(pool_id, next);
        queues.buffers.release(buffer_id)
    }
}

impl crate::g17::Firmware {
    /// The pair is returned even when transport, accounting or collection fails.
    /// No off-lock operation or callback which can re-enter the device runs here.
    fn with_render<R>(
        &mut self,
        slot: usize,
        operation: impl FnOnce(&mut Pair, &mut Host<'_>) -> Result<R>,
    ) -> Result<R> {
        let mut pair = self
            .queues
            .render
            .entries
            .get_mut(slot)
            .and_then(Option::as_mut)
            .and_then(|entry| entry.pair.take())
            .ok_or(EBUSY)?;
        let mut host = Host {
            firmware: self,
            registration_powered: false,
        };
        let result = operation(&mut pair, &mut host);
        host.release_registration();
        self.queues.render.entries[slot].as_mut().ok_or(EIO)?.pair = Some(pair);
        result
    }

    fn release_render_lists(&mut self) -> Result {
        if self.queues.render.grow.reply.is_some() {
            return Err(EAGAIN);
        }
        let primary = &mut self.primary;
        self.queues.render.pools.publish_releases(&self.init, || {
            primary.notify(crate::g17::MSG_CONTROL_NOTIFY)
        })
    }

    /// Events have already been consumed by the caller. Each healthy completion
    /// services its final pool release before examining the next retained job.
    fn retire_render_slot(
        &mut self,
        slot: usize,
        masks: Option<[u64; 2]>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        let generation = self.recovery.generation();
        let state = crate::g17::recovery::Memory::recovery_state(&self.init)?;
        loop {
            let progressed = self.with_render(slot, |pair, host| {
                pair.retire(masks, generation, state, host, &mut |event| {
                    deferred.push(event)
                })
            })?;
            if !progressed {
                break;
            }
            match self.release_render_lists() {
                Err(error) if error == EAGAIN => break,
                result => result?,
            }
        }
        Ok(())
    }

    pub(in crate::g17) fn settle_render_vm(
        &mut self,
        status: &Arc<crate::g17::status::VmStatus>,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        for slot in 1..self.queues.render.entries.len() {
            if self.queues.render.entries[slot]
                .as_ref()
                .is_some_and(|entry| {
                    entry.pair.is_some()
                        && entry
                            .context
                            .as_ref()
                            .is_some_and(|context| Arc::ptr_eq(context.status(), status))
                })
            {
                self.retire_render_slot(slot, None, deferred)?;
            }
        }
        self.queues.fail_render_vm(status, error, deferred)
    }

    pub(in crate::g17) fn stop_render(&mut self) {
        for entry in self.queues.render.entries.iter_mut().flatten() {
            if let Some(pair) = entry.pair.as_mut() {
                if let Err(error) = pair.stopped(&mut |event| {
                    event.finish();
                    Ok(())
                }) {
                    dev_err!(
                        self.primary.state.shared.dev.as_ref(),
                        "Could not settle stopped render pair: {:?}\n",
                        error
                    );
                }
            }
        }
    }
}

impl Registry {
    pub(in crate::g17) fn render_oldest_spared(&self, qid: u8) -> Option<bool> {
        self.render
            .entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.pair.as_ref())
            .find(|pair| pair.qids().contains(&qid))
            .and_then(|pair| pair.oldest_spared())
    }
    pub(in crate::g17) fn visit_render_owners(
        &self,
        qid: u8,
        visit: &mut dyn FnMut(&Arc<crate::g17::status::VmStatus>),
    ) {
        for pair in self
            .render
            .entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.pair.as_ref())
        {
            if pair.qids().contains(&qid) {
                pair.visit_owners(visit);
            }
        }
    }
    pub(in crate::g17) fn classify_render(
        &self,
        classify: &dyn Fn(&Arc<crate::g17::status::VmStatus>) -> crate::g17::recovery::Class,
    ) -> u128 {
        let mut spared = 0;
        for pair in self
            .render
            .entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.pair.as_ref())
        {
            pair.classify(classify);
            if pair.oldest_spared() == Some(true) {
                for qid in pair.qids() {
                    spared |= 1u128 << qid;
                }
            }
        }
        spared
    }

    pub(in crate::g17) fn render_scheduler_active(&self) -> bool {
        self.render.entries.iter().any(Option::is_some)
            || !self.render.retired.is_empty()
            || !self.teardown.is_empty()
    }
    /// Gives back the USC backing of render pools whose client VM pressure
    /// reclaim evicted (an idle VM) and whose final free-list release was
    /// consumed. The vacated backings go to the worker's deferred batch, which
    /// frees them after the device mutex; a later job of that client re-arms
    /// the pool first.
    pub(in crate::g17) fn vacate_evicted_render_pools(
        &mut self,
        init: &crate::g17::initdata::InitData,
        deferred: &mut super::DeferredBatch,
    ) -> Result {
        let render = &mut self.render;
        render.pools.observe(init)?;
        for entry in render.entries.iter().flatten() {
            if !deferred.render_backing_room() {
                break;
            }
            if entry.constructing || entry.growing || entry.preparing.is_some() {
                continue;
            }
            let Some(pool) = entry.pool.as_ref() else {
                continue;
            };
            if pool.vacant() || !render.pools.idle(pool.id()) || !pool.client_evicted() {
                continue;
            }
            if let Some(backing) = pool.vacate() {
                deferred.defer_render_backing(backing);
            }
        }
        Ok(())
    }
    pub(in crate::g17) fn render_in_flight(&self) -> bool {
        self.render.entries.iter().flatten().any(|entry| {
            entry.pair.as_ref().map_or(
                (entry.preparing.is_some() || entry.growing) && entry.borrowed_active,
                |pair| pair.published_in_flight(),
            )
        })
    }
    pub(in crate::g17) fn retain_render_completions(&mut self, masks: [u64; 2]) {
        for entry in self.render.entries.iter_mut().flatten() {
            for id in entry.reservation.ids {
                let qid = usize::from(id.qid());
                entry.event_masks[qid / 64] |= masks[qid / 64] & (1u64 << (qid % 64));
            }
        }
    }
    pub(in crate::g17) fn render_needs_rescan(&self) -> bool {
        self.render.entries.iter().flatten().any(|entry| {
            let active = entry
                .pair
                .as_ref()
                .map_or(
                    (entry.preparing.is_some() || entry.growing) && entry.borrowed_active,
                    |pair| pair.published_in_flight(),
                );
            active
                && entry.reservation.ids.into_iter().all(|id| {
                    let qid = usize::from(id.qid());
                    entry.event_masks[qid / 64] & (1u64 << (qid % 64)) != 0
                })
        })
    }
    pub(in crate::g17) fn defer_render_timeout(
        &mut self,
        packet: &Arc<Packet>,
        error: Error,
    ) -> bool {
        // Its tracker may already be gone while off-lock completion still
        // owns the fence and admission lease. A loan for a subsequent packet
        // cannot turn this exact accepted retirement into a missing-owner error.
        if packet.completion.retirement_queued() {
            return true;
        }
        let Some(slot) = packet.completion.render_slot() else {
            return false;
        };
        let Ok(entry) = self.render.entry_mut(slot) else {
            return false;
        };
        if entry.preparing.is_none() && !entry.growing {
            return false;
        }
        if !entry
            .borrowed_dependencies
            .as_ref()
            .is_some_and(|dependencies| dependencies.contains_packet(packet))
        {
            return false;
        }
        packet.completion.defer_timeout(error);
        entry.pending_timeout = true;
        true
    }
}
