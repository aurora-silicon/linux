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
//!
//! A publication that succeeded returns its pair without waiting for the
//! device mutex: when the mutex is busy, the loan goes to the shared return
//! box instead ([`DirectReturns`]) and the next holder of the device mutex
//! that needs the pair settles it, exactly as the publisher would have. Every
//! such holder settles the box first: the next lend, the event pass, and the
//! stop paths once the preparation leases are drained. The publisher queues an
//! event pass after filling the box, so a returned loan never waits for more
//! than that pass. A contended publication then waits for the device mutex
//! once, for its lend, instead of twice.

use super::super::{Backend, DeferredBatch};
use crate::g17::{
    job::Packet,
    object::Allocator,
    port::{Port, RenderRoom},
    queue::render::{Pair, PublicationLease, RenderAddresses},
};
use crate::hw::t8140::queues::RENDER_SLOTS;
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

/// A published loan waiting in [`DirectReturns`] for the device mutex.
struct Returned {
    loan: Loan,
    activation: Option<(u16, u64)>,
    packet: Arc<Packet>,
}

/// Loans of successful direct publications, returned without the device
/// mutex, by slot. A slot holds at most one: its pair stays lent (its entry
/// keeps `preparing`) until the loan is settled, so it cannot be lent again
/// meanwhile. The lock nests inside the device mutex and is only held to move
/// one loan in or out.
pub(in crate::g17) struct DirectReturns {
    slots: KBox<[Option<Returned>; RENDER_SLOTS]>,
    pending: u64,
}

const _: () = assert!(RENDER_SLOTS <= u64::BITS as usize);

impl DirectReturns {
    pub(in crate::g17) fn new() -> Result<Self> {
        Ok(Self {
            slots: KBox::new([const { None }; RENDER_SLOTS], GFP_KERNEL)?,
            pending: 0,
        })
    }

    /// Puts `returned` in the cell of `slot`, or gives it back if the cell is not free.
    fn put(&mut self, slot: u8, returned: Returned) -> Option<Returned> {
        let index = usize::from(slot);
        match self.slots.get_mut(index) {
            Some(cell @ None) => {
                *cell = Some(returned);
                self.pending |= 1 << index;
                None
            }
            _ => Some(returned),
        }
    }

    fn take_next(&mut self) -> Option<(u8, Returned)> {
        if self.pending == 0 {
            return None;
        }
        let index = self.pending.trailing_zeros() as usize;
        self.pending &= !(1 << index);
        let returned = self.slots[index].take()?;
        Some((index as u8, returned))
    }
}

/// What settling the return box leaves for after the device mutex is dropped: dropping
/// this queues the growth the returned pairs unblocked and wakes their waiters, as the
/// publisher does after its own return. Declare it before the device-mutex guard, or drop
/// the guard first, so that this runs unlocked.
pub(in crate::g17) struct Settled {
    shared: Arc<crate::g17::Shared>,
    returned: bool,
    grow: bool,
}

impl Drop for Settled {
    fn drop(&mut self) {
        if self.grow {
            self.shared.queue_grow();
        }
        if self.returned {
            self.shared.note_changed();
        }
    }
}

impl crate::g17::Shared {
    /// Returns every loan in the return box to `firmware`, the device-mutex state of this
    /// device, as its publishers would have. Returns whether any came back and whether a
    /// return failed. The packets dropped here are never the last references: each was
    /// published, so its pair keeps it among its active commands, and a lent pair is never
    /// retired.
    fn return_boxed(&self, firmware: &mut crate::g17::Firmware) -> (bool, bool) {
        let (mut returned, mut failed) = (false, false);
        loop {
            let next = self.direct_returns.lock().take_next();
            let Some((slot, mut boxed)) = next else {
                return (returned, failed);
            };
            returned = true;
            failed |= firmware
                .return_render(slot, &mut boxed.loan, boxed.activation, &boxed.packet)
                .is_err();
        }
    }

    /// Settles the return box under the device mutex. A failed return loses the device, as
    /// on the publisher's own return.
    pub(in crate::g17) fn settle_direct_returns(
        self: &Arc<Self>,
        firmware: &mut crate::g17::Firmware,
    ) -> Settled {
        let (returned, failed) = self.return_boxed(firmware);
        if failed {
            self.lose_device_quietly();
        }
        Settled {
            shared: self.clone(),
            returned,
            grow: returned && firmware.render_grow_ready(),
        }
    }

    /// Settles the return box of stopped firmware before it is detached, so that its pairs
    /// are torn down with the rest of its state. Nothing waits for them any more.
    pub(in crate::g17) fn settle_stopped_returns(&self, firmware: &mut crate::g17::Firmware) {
        let _ = self.return_boxed(firmware);
    }
}

/// Publication host of a lent pair. Registration, first-publication and
/// growth paths never run here: the loan requires an installed, registered
/// graph that needs no growth.
struct DirectHost<'a> {
    port: &'a Port,
    render: &'a MutexGuard<'a, ()>,
    room: RenderRoom,
    generation: u64,
    activation: Option<(u16, u64)>,
}

impl crate::g17::queue::render::Host for DirectHost<'_> {
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
    fn install_pair(&mut self, _registration: &crate::g17::fw::kick::RenderRegistration) -> Result {
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
        if priority != self.room.priority() {
            return Err(EINVAL);
        }
        self.port
            .write_render_pair(self.render, &self.room, fragment, tiling, commit, late_tiling)
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
        if priority != self.room.priority() {
            return Err(EINVAL);
        }
        self.port.expose_render_pair(self.render, &self.room, next)
    }
    /// Pipe 0 (tiling) in bits 1:0, priority class in bits 3:2.
    fn notify_render(&mut self, priority: u8) -> Result {
        self.port
            .notify((0x83 << crate::g17::MSG_TYPE_SHIFT) | u64::from(priority) << 2)
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
        let addresses = RenderAddresses {
            stamps: [
                self.init.stamp_addresses(qids[0])?,
                self.init.stamp_addresses(qids[1])?,
            ],
        };
        let (buffer, buffer_token) = self.queues.buffers.acquire_owner(buffer_id)?;
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
        };
        let pair = match self.queues.render.take_pair(slot, owner, packet) {
            Ok(pair) => pair,
            Err(error) => {
                self.queues.render.pools.rollback(preparation);
                self.queues.buffers.release(buffer)?;
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
    /// parameter-buffer reference otherwise.
    fn return_render(
        &mut self,
        slot: u8,
        loan: &mut Loan,
        activation: Option<(u16, u64)>,
        packet: &Arc<Packet>,
    ) -> Result {
        self.queues.render.return_pair(slot, &mut loan.pair)?;
        if self.queues.render.take_missed_event(slot) {
            // The record is retired by the next pass's observation, like any completion.
            self.primary.state.shared.queue_event_records();
        }
        let entry = self.queues.render.entry_mut(slot)?;
        // A failure or timeout recorded against the lent pair is settled by the
        // event worker's collector, as after any other loan.
        if entry.pending_error.is_some() || entry.pending_timeout {
            self.primary.state.shared.queue_events();
        }
        let pair = entry.pair.as_mut().ok_or(EIO)?;
        if entry.closed {
            pair.release_owner();
        }
        if !packet.is_published() {
            pair.discard_prepared(packet)?;
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
            self.queues.render.pools.rollback(preparation);
            self.queues.buffers.release(loan.buffer)?;
        }
        Ok(())
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
        match packet.completion.work_state().ok()?.acquire(crate::g17::job::Engine::Render) {
            Ok(_) => {}
            Err(error) if error == EBUSY => return None,
            Err(error) => return Some(Err(error)),
        }
        let priority = packet.context.policy().priority();
        let generation = port.generation();
        let dev = self.shared.drm().ok()?;
        let mut loan = {
            let _settled;
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut()?;
            // This owner's previous publication may have left its pair in the return box.
            _settled = self.shared.settle_direct_returns(firmware);
            let lent = if lease.is_current() {
                firmware.lend_render(slot, self.owner, packet, priority, generation)
            } else {
                Ok(None)
            };
            drop(state);
            match lent {
                Ok(Some(loan)) => loan,
                Ok(None) => return None,
                Err(error) => return Some(Err(error)),
            }
        };
        let mut activation = None;
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
            let room = port.observe_render_room(&render, priority)?.ok_or(EBUSY)?;
            let mut dependencies = pair.own_dependencies(self.owner, packet)?;
            if let Some(footprint) = packet.footprint.as_ref() {
                dependencies.hazard_count = self.admission.publication_barriers(
                    self.owner,
                    slot,
                    footprint,
                    &mut dependencies.hazards,
                )?;
            }
            let mut host = DirectHost {
                port,
                render: &render,
                room,
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
            result?;
            // A recovery that started meanwhile may have discarded the commands.
            if port.generation() != generation || port.recovery_state()? != 0 {
                return Err(EIO);
            }
            Ok(())
        })();
        // A published loan does not wait for a busy device mutex: it goes to the return box,
        // whose next device-mutex holder that needs the pair settles it, and the queued event
        // pass bounds how long it waits there. It goes in before the preparation lease is
        // dropped, so a stop path that has drained the leases finds it there. A failed
        // publication is returned here, since the device-mutex path may retry it at once.
        let mut state = match self.shared.state.try_lock() {
            Some(state) => state,
            None if result.is_ok() => {
                let boxed = Returned {
                    loan,
                    activation,
                    packet: packet.clone(),
                };
                let stored = self.shared.direct_returns.lock().put(slot, boxed);
                match stored {
                    None => {
                        drop(lease);
                        self.shared.queue_event_records();
                        self.direct_slot.store(u32::from(slot), Ordering::Relaxed);
                        return Some(Ok(()));
                    }
                    // A slot's pair is lent once at a time, so its cell is always empty; if
                    // not, return the loan here.
                    Some(boxed) => loan = boxed.loan,
                }
                self.shared.state.lock()
            }
            None => self.shared.state.lock(),
        };
        let returned = match (*state).as_deref_mut() {
            Some(firmware) => {
                let returned = firmware.return_render(slot, &mut loan, activation, packet);
                let grow_ready = firmware.render_grow_ready();
                drop(state);
                if grow_ready {
                    self.shared.queue_grow();
                }
                returned
            }
            // Shutdown joins every preparation lease before detaching the
            // session, so this cannot happen while `lease` is held.
            None => {
                drop(state);
                Err(ENODEV)
            }
        };
        self.shared.note_changed();
        drop(lease);
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
