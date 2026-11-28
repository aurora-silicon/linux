// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Steady-state render publication without the device mutex.
//!
//! The device mutex lends the owner's installed pair out, together with its
//! free-list preparation and a parameter-buffer reference, and takes it back.
//! Between the two, command preparation and publication take only the port:
//! its render publication lock (the render work rings' single producer and
//! the publication order of attachment hazards), QoS accounting, the stamps
//! and the doorbell. While the pair is lent, the event worker skips it, so no
//! retirement can complete its commands or their free-list references.

use super::super::{Backend, DeferredBatch};
use super::observe::FastRenderOutcome;
use crate::g17::{
    job::Packet,
    object::Allocator,
    port::{M5RenderRoom, Port, RenderRoom},
    queue::render::{Pair, PublicationLease, RenderAddresses},
};
use core::sync::atomic::Ordering;
use kernel::{
    prelude::*,
    sync::{Arc, MutexGuard},
};

/// A pair lent out for one publication, with what that publication holds.
pub(in crate::g17::runtime) struct Loan {
    pair: Option<KBox<Pair>>,
    preparation: Option<super::pools::Preparation>,
    buffer: u8,
    lease: PublicationLease,
    addresses: RenderAddresses,
    uat: Arc<crate::mmu::Uat>,
}

enum DirectTarget {
    Legacy(RenderRoom),
    M5(M5RenderRoom),
}

/// Publication host of a lent pair. Registration, first-publication and
/// growth paths never run here: the loan requires an installed, registered
/// graph that needs no growth.
struct DirectHost<'a> {
    port: &'a Port,
    render: Option<MutexGuard<'a, ()>>,
    m5_outer: Option<MutexGuard<'a, Option<(u32, u64, u64)>>>,
    m5_claim: Option<&'a mut crate::g17::initdata::OuterClaim>,
    target: DirectTarget,
    generation: u64,
    activation: Option<(u16, u64)>,
}

impl crate::g17::queue::render::Host for DirectHost<'_> {
    fn generation(&self) -> u64 {
        self.generation
    }
    fn next_publication(&mut self) -> Result<u64> {
        Ok(self.port.next_publication())
    }
    fn epoch(&self) -> Result<(u64, u32)> {
        Ok((self.generation, self.port.recovery_state()?))
    }
    fn stamps(&self, qids: [u8; 2]) -> Result<[u32; 2]> {
        Ok([self.port.stamp(qids[0])?, self.port.stamp(qids[1])?])
    }
    fn qos_publish_pair(
        &mut self,
        owners: [crate::g17::qos::Owner; 2],
        scheduler: u64,
        policy: crate::g17::fw::queue::Policy,
    ) -> Result<[crate::g17::qos::Publication; 2]> {
        self.port.qos_publish_pair(owners, scheduler, policy)
    }
    fn qos_cancel(&mut self, publication: crate::g17::qos::Publication) -> Result {
        self.port.qos_cancel(publication)
    }
    fn install_pair(&mut self, _registration: &crate::g17::queue::render::Registration) -> Result {
        Err(EAGAIN)
    }
    fn publish_qid(&mut self, _id: crate::g17::kick::Id) -> Result {
        // An installed graph published its identities with its first command.
        Ok(())
    }
    fn publish_outer_pair(
        &mut self,
        priority: u8,
        fragment: &crate::g17::fw::channels::WorkSlot,
        tiling: &crate::g17::fw::channels::WorkSlot,
        commit: impl FnOnce() -> Result,
        late_tiling: impl FnOnce(),
    ) -> Result<[u32; 2]> {
        match &self.target {
            DirectTarget::Legacy(room) => {
                if priority != room.priority() {
                    return Err(EINVAL);
                }
                let render = self.render.as_ref().ok_or(EIO)?;
                self.port.write_render_pair(
                    render,
                    room,
                    fragment,
                    tiling,
                    commit,
                    late_tiling,
                )
            }
            DirectTarget::M5(room) => {
                if priority != room.priority() {
                    return Err(EINVAL);
                }
                let render = self.render.as_ref().ok_or(EIO)?;
                let m5_outer = self.m5_outer.as_ref().ok_or(EIO)?;
                self.port.write_m5_render_pair(
                    render,
                    m5_outer,
                    room,
                    fragment,
                    tiling,
                    commit,
                    late_tiling,
                )
            }
        }
    }
    fn publish_configuration_pair(
        &mut self,
        _priority: u8,
        _fragment: &crate::g17::fw::channels::WorkSlot,
        _tiling: &crate::g17::fw::channels::WorkSlot,
        _prepare: impl FnOnce(),
    ) -> Result {
        // Cold-install configuration pairs never run on an already-registered pair.
        Err(EINVAL)
    }
    fn activate_render_pool(&mut self, id: u16, generation: u64) -> Result {
        // The lease leaves preparation when the pair is returned; nothing can
        // observe it in between because the pair is lent.
        self.activation = Some((id, generation));
        Ok(())
    }
    fn publish_retained_pair(
        &mut self,
        priority: u8,
        _fragment: &crate::g17::fw::channels::WorkSlot,
        _tiling: &crate::g17::fw::channels::WorkSlot,
        next: [u32; 2],
    ) -> Result {
        match &self.target {
            DirectTarget::Legacy(room) => {
                if priority != room.priority() {
                    return Err(EINVAL);
                }
                let render = self.render.as_ref().ok_or(EIO)?;
                self.port.expose_render_pair(render, room, next)
            }
            DirectTarget::M5(room) => {
                if priority != room.priority() {
                    return Err(EINVAL);
                }
                let render = self.render.as_ref().ok_or(EIO)?;
                let m5_outer = self.m5_outer.as_ref().ok_or(EIO)?;
                let claim = self.m5_claim.as_deref_mut().ok_or(EIO)?;
                self.port
                    .expose_m5_render_pair(render, m5_outer, room, claim, next)
            }
        }
    }
    /// Rings the shared render doorbell contract ([`super::render_doorbell_message`]).
    fn notify_render(&mut self, priority: u8) -> Result {
        let m5 = matches!(self.target, DirectTarget::M5(_));
        if m5 {
            let geometry = self.m5_outer.as_mut().ok_or(EIO)?;
            if let Err(error) = self.port.note_m5_submission_history_locked(geometry, 2) {
                pr_warn!("asahi: M5 submission history skipped: {:?}\n", error);
            }
            // Drop `m5_outer` before ringing the mailbox doorbell so competing M5 submitters
            // do not wait behind mailbox I/O.
            self.m5_outer = None;
        }
        // Outer ring producers, credits, and hazard order are committed; drop the render
        // publication lock before mailbox I/O as well.
        self.render = None;
        self.port.notify(super::render_doorbell_message(m5, priority))
    }
    fn note_submission(&mut self) -> Result {
        // Counted, with full effort witnessed, before the pair was lent.
        Ok(())
    }
    fn complete_render(
        &mut self,
        _owners: Option<[crate::g17::qos::Owner; 2]>,
        _pool_id: u16,
        _generation: u64,
        _buffer_id: u8,
    ) -> Result {
        Err(EINVAL)
    }
}

impl crate::g17::Firmware {
    /// Lends `owner`'s pair in `slot` for a publication of `packet`, or None if
    /// the pair is not in the steady state the direct path requires.
    fn lend_render(
        &mut self,
        slot: u8,
        owner: u64,
        packet: &Arc<Packet>,
        priority: u8,
        generation: u64,
    ) -> Result<Option<Loan>> {
        if self.recovery.pending() || self.recovery.generation() != generation {
            return Ok(None);
        }
        let Ok(entry) = self.queues.render.entry_mut(slot) else {
            return Ok(None);
        };
        if !entry.published
            || entry.closed
            || entry.constructing
            || entry.preparing.is_some()
            || entry.growing
            || entry.parked.is_some()
            || entry.pending_error.is_some()
            || entry.pending_timeout
        {
            return Ok(None);
        }
        let Some(pair) = entry.pair.as_mut() else {
            return Ok(None);
        };
        if !pair.matches(owner, &packet.context)?
            || !pair.direct_ready(&packet.context, priority)
            || pair.pool().vacant()
            || !matches!(pair.host_growth_target(packet), Ok(None))
        {
            return Ok(None);
        }
        let qids = pair.qids();
        let pool = pair.pool().clone();
        let buffer_id = pair.buffer_id();
        // An M5 PB token is bound to the hardware context and PB state it names.
        let buffer_context = pair
            .m5_pb_state_va()
            .map(|pb_va| (packet.context.id(), pb_va));
        let addresses = RenderAddresses {
            stamps: [
                self.init.stamp_addresses(qids[0])?,
                self.init.stamp_addresses(qids[1])?,
            ],
        };
        let (buffer, buffer_token) = match buffer_context {
            Some((context, pb_va)) => {
                self.queues
                    .buffers
                    .acquire_owner_bound(buffer_id, context, pb_va)?
            }
            None => self.queues.buffers.acquire_owner(buffer_id)?,
        };
        let preparation = match self.queues.render.pools.prepare(&self.init, &pool) {
            Ok(preparation) => preparation,
            Err(error) => {
                self.queues.buffers.release(buffer)?;
                return if error == EAGAIN || error == EBUSY {
                    Ok(None)
                } else {
                    Err(error)
                };
            }
        };
        let lease = PublicationLease {
            free_list_generation: preparation.generation,
            buffer_token,
            buffer_context: buffer_context.map(|(context, _)| context),
        };
        let pair = match self.queues.render.take_pair(slot, owner, packet) {
            Ok(pair) => pair,
            Err(error) => {
                let rollback = self.rollback_render_pool(preparation);
                self.queues.buffers.release(buffer)?;
                rollback?;
                return if error == EBUSY { Ok(None) } else { Err(error) };
            }
        };
        // The selected scheduler is retained through release acknowledgement,
        // as on the device-mutex path.
        packet.context.mark_published();
        Ok(Some(Loan {
            pair: Some(pair),
            preparation: Some(preparation),
            buffer,
            lease,
            addresses,
            uat: self.uat.clone(),
        }))
    }

    /// Takes a lent pair back and settles what its publication held: the
    /// free-list preparation is activated once published, rolled back with the
    /// parameter-buffer reference otherwise. True when a completion of the pair
    /// arrived while it was lent: the caller owes the pair that observation.
    fn return_render(
        &mut self,
        slot: u8,
        loan: &mut Loan,
        activation: Option<(u16, u64)>,
        packet: &Arc<Packet>,
    ) -> Result<bool> {
        self.queues.render.return_pair(slot, &mut loan.pair)?;
        let missed = self.queues.render.take_missed_event(slot);
        let entry = self.queues.render.entry_mut(slot)?;
        // A failure or timeout recorded against the lent pair is settled by the
        // event worker's collector, as after any other loan.
        if entry.pending_error.is_some() || entry.pending_timeout {
            self.primary.state.shared.queue_events();
        }
        let closed = entry.closed;
        let pair = entry.pair.as_mut().ok_or(EIO)?;
        if closed {
            pair.release_owner();
        }
        if !packet.is_published() {
            pair.discard_prepared(packet)?;
        }
        let published = packet.is_published() || pair.graph_started();
        // A failed publication may have quarantined the lent pair, and its owner may have
        // closed while it was lent.
        self.queues.hold_render_slot(usize::from(slot));
        if published && !closed {
            self.queues.admission.note_published(slot);
        }
        if let Some((id, generation)) = activation {
            if self.queues.render.pools.activate(id, generation).is_err() {
                self.primary.state.shared.lose_device_quietly();
            }
        }
        let preparation = loan.preparation.take().ok_or(EIO)?;
        if packet.is_published() {
            self.queues.render.entry_mut(slot)?.published = true;
        } else {
            let rollback = self.rollback_render_pool(preparation);
            self.queues.buffers.release(loan.buffer)?;
            rollback?;
        }
        Ok(missed)
    }
}

impl Backend {
    /// Publishes `packet` into the owner's installed pair with the device
    /// mutex held only to lend the pair out and take it back. None leaves the
    /// packet unpublished for the device-mutex path.
    pub(in crate::g17::runtime) fn render_direct(
        &self,
        packet: &Arc<Packet>,
        slot: u8,
        deferred: &mut DeferredBatch,
    ) -> Option<Result> {
        // A compute prefix needs the compute queue's state; leave it to the
        // device-mutex path.
        if packet.is_published()
            || packet.cancel_requested()
            || packet.completion.status().get() != 0
            || packet.order.wait_through[1].unwrap_or(0) != 0
            // Only the device-mutex path resolves a staged render's tiling prefixes.
            || matches!(&packet.command, crate::g17::command::Validated::Render { pass, .. }
                if pass.stage_prefixes)
            || self.admission_open().is_err()
        {
            return None;
        }
        let lease = self.shared.preparations.try_enter().ok()?;
        let port = self.shared.port(&lease)?;
        if self.shared.crashed.load(Ordering::Acquire) || port.recovery_state().ok()? != 0 {
            return None;
        }
        // Full effort is applied by the device-mutex path when it is not already.
        if !port.note_submission() {
            return None;
        }
        self.shared.note_submitter();
        self.shared.note_gpu_activity();
        match packet.completion.work_state().ok()?.acquire(crate::g17::job::Engine::Render) {
            Ok(_) => {}
            Err(error) if error == EBUSY => return None,
            Err(error) => return Some(Err(error)),
        }
        let m5 = packet.context.is_m5();
        let priority = packet.context.policy().priority();
        let generation = port.generation();
        let dev = self.shared.drm().ok()?;
        let mut loan = {
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut()?;
            if !lease.is_current() {
                return None;
            }
            match firmware.lend_render(slot, self.owner, packet, priority, generation) {
                Ok(Some(loan)) => loan,
                Ok(None) => return None,
                Err(error) => return Some(Err(error)),
            }
        };
        let mut activation = None;
        let mut recovery_started = false;
        // Declared outside the publication closure so any unspent M5 outer claim drops
        // after `m5_outer` and `render` are released (dropping an unspent claim can wake fences).
        let mut m5_claim = None;
        let result = (|| {
            let pair = loan.pair.as_mut().ok_or(EIO)?;
            let alloc = Allocator {
                dev: &dev,
                uat: &loan.uat,
            };
            pair.prepare(&alloc, packet, loan.addresses)?;
            let render = port.render_lock();
            if !lease.is_current() {
                return Err(EBUSY);
            }
            let m5_outer = m5.then(|| port.m5_outer_lock());
            let target = if let Some(guard) = m5_outer.as_ref() {
                let (room, claim) = port.reserve_m5_render_locked(guard, priority)?;
                m5_claim = Some(claim);
                DirectTarget::M5(room)
            } else {
                DirectTarget::Legacy(port.observe_render_room(&render, priority)?.ok_or(EBUSY)?)
            };
            let mut dependencies = pair.own_dependencies(self.owner, packet)?;
            if let Some(footprint) = packet.footprint.as_ref() {
                dependencies.hazard_count = self.admission.publication_barriers(
                    self.owner,
                    slot,
                    footprint,
                    packet.order.wait_through[0],
                    &mut dependencies.hazards,
                )?;
            }
            let mut host = DirectHost {
                port,
                render: Some(render),
                m5_outer,
                m5_claim: m5_claim.as_mut(),
                target,
                generation,
                activation: None,
            };
            let result = pair.publish(
                packet.clone(),
                &dependencies,
                loan.lease,
                &mut host,
                &mut |event| deferred.push(event),
            );
            activation = host.activation;
            drop(host);
            result?;
            // A generation advance while lent means the commands belong to a closed epoch.
            if port.generation() != generation {
                return Err(EIO);
            }
            // If recovery started during publication while our preparation lease still held
            // the same generation, return_render() hands the published pair back before lease
            // drops so the existing recovery pass settles or replays it once without failing
            // an unrelated client's VM.
            if port.recovery_state()? != 0 {
                recovery_started = true;
            }
            Ok(())
        })();
        drop(m5_claim);
        // The collector of the observation the return may owe, taken without waiting.
        let mut collector = None;
        let returned = {
            let mut state = self.shared.state.lock();
            match (*state).as_deref_mut() {
                Some(firmware) => {
                    let returned = firmware.return_render(slot, &mut loan, activation, packet);
                    let returned = match returned {
                        // A completion arrived while the pair was lent. The event pass it would
                        // request takes the device mutex from another CPU after this owner's
                        // next lend and finds the pair lent again: observe it here, in this
                        // hold, unless recovery, a crash or another borrower's observation
                        // leaves it to that pass.
                        Ok(true) => {
                            if !recovery_started
                                && lease.is_current()
                                && !self.shared.crashed.load(Ordering::Acquire)
                            {
                                collector = self.shared.render_collector.try_lock();
                            }
                            match collector.as_deref_mut() {
                                Some(batch) => firmware.observe_returned_render(slot, batch),
                                None => Ok(FastRenderOutcome::Fallback),
                            }
                            .map(|outcome| match outcome {
                                // Retired by the next pass's observation, like any completion.
                                FastRenderOutcome::Fallback => self.shared.queue_event_records(),
                                FastRenderOutcome::Rescan => self.shared.queue_poll(),
                                FastRenderOutcome::Done | FastRenderOutcome::Idle => {}
                            })
                        }
                        Ok(false) => Ok(()),
                        Err(error) => Err(error),
                    };
                    let grow_ready = firmware.render_grow_ready();
                    drop(state);
                    // Fences signal off the device mutex, before another borrower can collect.
                    if let Some(batch) = collector.as_deref_mut() {
                        batch.finish();
                    }
                    drop(collector);
                    if grow_ready {
                        self.shared.queue_grow();
                    }
                    returned
                }
                // Shutdown joins every preparation lease before detaching the
                // session, so this cannot happen while `lease` is held.
                None => Err(ENODEV),
            }
        };
        self.shared.note_changed();
        drop(lease);
        if recovery_started {
            self.shared.queue_events();
        }
        if let Err(error) = returned {
            self.shared.lose_device_quietly();
            return Some(Err(error));
        }
        match result {
            Err(error) if (error == EAGAIN || error == EBUSY) && !packet.is_published() => None,
            result => {
                if result.is_ok() {
                    self.direct_slot.store(u32::from(slot), Ordering::Relaxed);
                }
                Some(result)
            }
        }
    }
}
