// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Submission admission, preparation and the firmware visibility boundary.

use super::super::{Backend, DeferredBatch, Registry};
use crate::g17::{
    job::Packet,
    queue::admission::{RenderLease, Request},
};
use core::sync::atomic::Ordering;
use kernel::{
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, Delta},
};

impl Backend {
    pub(in crate::g17) fn render_packet(
        &self,
        packet: &Arc<Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        self.render_with_lease(packet, None, deferred)
    }

    fn render_with_lease(
        &self,
        packet: &Arc<Packet>,
        early: Option<RenderLease>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        packet.check_dependencies()?;
        if packet.cancel_requested() {
            return Err(ECANCELED);
        }
        if packet.completion.status().get() != 0 {
            return Err(EIO);
        }
        let interruptible = early.is_none();
        let admission = match early {
            Some(lease) => lease,
            None => {
                let pool = {
                    let state = self.shared.state.lock();
                    (*state).as_deref().ok_or(ENODEV)?.queues.admission.clone()
                };
                pool.acquire_render(self.owner, Request::of(packet))?
            }
        };
        let slot = admission.slot();
        packet.completion.attach_render_lease(admission)?;
        let mut state_polls = 0;
        let mut state_delay = 20i64;
        let mut takeover_polls = 0;
        let mut recovery_polls = 0;
        loop {
            if packet.cancel_requested() {
                return Err(ECANCELED);
            }
            if packet.completion.status().get() != 0 || self.shared.crashed.load(Ordering::Acquire)
            {
                return Err(EIO);
            }
            if !self.context.is_current() {
                return Err(EFAULT);
            }
            let work_state = {
                let preparation = self.shared.preparations.enter(interruptible)?;
                let result = packet.completion.work_state().and_then(|state| state.acquire());
                if preparation.is_current() {
                    result
                } else {
                    Err(EAGAIN)
                }
            };
            match work_state {
                Err(error) if error == EAGAIN => continue,
                Err(error) if error == EBUSY => {
                    if state_polls == 1000 {
                        return Err(ETIMEDOUT);
                    }
                    state_polls += 1;
                    fsleep(Delta::from_micros(state_delay));
                    state_delay = (state_delay * 2).min(1000);
                    continue;
                }
                result => {
                    result?;
                }
            }
            match self.ensure_render(packet, slot) {
                Err(error) if error == EAGAIN => continue,
                Err(error) if error == EBUSY => {
                    if takeover_polls == 5000 {
                        return Err(ETIMEDOUT);
                    }
                    takeover_polls += 1;
                    fsleep(Delta::from_micros(200));
                    continue;
                }
                result => result?,
            }
            match self.prepare_render(slot, packet, deferred) {
                Err(error) if !packet.is_published() && [EAGAIN, EBUSY].contains(&error) => {
                    let recovering = {
                        let state = self.shared.state.lock();
                        let firmware = (*state).as_deref().ok_or(ENODEV)?;
                        firmware.recovery.pending()
                            || crate::g17::recovery::Memory::recovery_state(&firmware.init)? != 0
                    };
                    if recovering {
                        if recovery_polls == super::super::RECOVERY_SETTLE_POLLS {
                            return Err(ETIMEDOUT);
                        }
                        recovery_polls += 1;
                        self.shared.queue_events();
                        fsleep(Delta::from_millis(1));
                    } else if error == EBUSY {
                        if takeover_polls == 5000 {
                            return Err(ETIMEDOUT);
                        }
                        takeover_polls += 1;
                        fsleep(Delta::from_micros(200));
                    }
                }
                result => return result,
            }
        }
    }

    pub(in crate::g17) fn try_render_early(&self, packet: &Arc<Packet>) -> bool {
        if self.backlog.load(Ordering::Acquire) != 0 || packet.has_dependencies() {
            return false;
        }
        let admission = {
            let state = self.shared.state.lock();
            let Some(firmware) = (*state).as_deref() else {
                return false;
            };
            match firmware
                .queues
                .admission
                .try_acquire_render(self.owner, Request::of(packet))
            {
                Ok(Some(lease)) => lease,
                _ => return false,
            }
        };
        if !packet.begin_publication() {
            packet.completion.fail_unpublished(ECANCELED);
            packet.set_early_result(Err(ECANCELED));
            return true;
        }
        let mut deferred = match DeferredBatch::publication() {
            Ok(deferred) => deferred,
            Err(error) => {
                if packet.completion.status().get() != 0 {
                    <Self as crate::g17::job::Backend>::fail_vm(self, error);
                }
                packet.completion.fail_unpublished(error);
                packet.set_early_result(Err(error));
                return true;
            }
        };
        let result = self.render_with_lease(packet, Some(admission), &mut deferred);
        let result = self
            .finish_publication(packet, result, &mut deferred)
            .map(|_| ());
        packet.set_early_result(result);
        true
    }
}

impl Registry {
    fn render_dependencies(
        &self,
        owner: u64,
        packet: &Arc<Packet>,
        state: u32,
    ) -> Result<crate::g17::queue::render::RenderDependencies> {
        let mut dependencies = crate::g17::queue::render::RenderDependencies::EMPTY;
        let frontier = self.render_prefix(owner, packet)?;
        for dependency in frontier.as_slice() {
            *dependencies
                .render
                .get_mut(dependencies.render_count)
                .ok_or(EOVERFLOW)? = *dependency;
            dependencies.render_count += 1;
        }
        if let Some(queue) = self.compute(owner) {
            dependencies.compute = queue.dependency(
                owner,
                packet.completion.status(),
                packet.order.wait_through[1],
                state,
            )?;
        }
        Ok(dependencies)
    }
}

impl crate::g17::Firmware {
    pub(super) fn apply_render_deferred(
        &mut self,
        slot: u8,
        deferred: &mut DeferredBatch,
    ) -> Result {
        let observe_before_failure = {
            let entry = self.queues.render.entry_mut(slot)?;
            let pair = entry.pair.as_ref().ok_or(EIO)?;
            entry.pending_error.as_ref().is_some_and(|(status, _)| {
                Arc::ptr_eq(pair.context().status(), status)
            })
        };
        if observe_before_failure {
            // VM failure could not observe this pair while preparation owned
            // it. Restore the same final-observation boundary used by
            // settle_render_vm before quarantining any still-owned commands.
            // Keep pending_error intact if observation itself fails.
            self.retire_render_slot(usize::from(slot), None, deferred)?;
        }
        let entry = self.queues.render.entry_mut(slot)?;
        let pair = entry.pair.as_mut().ok_or(EIO)?;
        if let Some((status, error)) = entry.pending_error.take() {
            if Arc::ptr_eq(pair.context().status(), &status) {
                pair.quarantine(error, &mut |event| deferred.push(event))?;
            }
        }
        if entry.closed {
            pair.release_owner();
        }
        if !entry.pending_timeout {
            return Ok(());
        }
        for _ in 0..crate::hw::t8140::queues::RENDER_DEPTH {
            if self.recovery.pending() || self.primary.state.shared.crashed.load(Ordering::Acquire)
            {
                return Ok(());
            }
            let Some((packet, error)) = self
                .queues
                .render
                .entry(slot)?
                .pair
                .as_ref()
                .ok_or(EIO)?
                .take_deferred_timeout()
            else {
                self.queues.render.entry_mut(slot)?.pending_timeout = false;
                return Ok(());
            };
            if let Err(failure) = self.fail_render_packet(&packet, error, deferred) {
                packet.completion.defer_timeout(error);
                return Err(failure);
            }
        }
        self.queues.render.entry_mut(slot)?.pending_timeout = false;
        Ok(())
    }

    pub(super) fn publish_render(
        &mut self,
        owner: u64,
        slot: u8,
        packet: &Arc<Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        use crate::g17::queue::render::PublicationLease;
        let state = crate::g17::recovery::Memory::recovery_state(&self.init)?;
        if self.recovery.pending() || state != 0 {
            return Err(EAGAIN);
        }
        if packet.completion.status().get() != 0 {
            return Err(EIO);
        }
        // Both outer destinations must have room before pool accounting or
        // either inner queue becomes visible. All host producers share this lock.
        if !self.init.work_ready(crate::g17::fw::queue::DataMaster::Fragment)?
            || !self.init.work_ready(crate::g17::fw::queue::DataMaster::Tiling)?
        {
            return Err(EBUSY);
        }
        let dependencies = self.queues.render_dependencies(owner, packet, state)?;
        let pair = self.queues.render.entry(slot)?.pair.as_ref().ok_or(EIO)?;
        let pool = pair.pool().clone();
        let buffer_id = pair.buffer_id();
        let (buffer_id, buffer_token) = self.queues.buffers.acquire_owner(buffer_id)?;
        let preparation = match self.queues.render.pools.prepare(&self.init, &pool) {
            Ok(preparation) => preparation,
            Err(error) => {
                self.queues.buffers.release(buffer_id)?;
                return Err(error);
            }
        };
        let lease = PublicationLease {
            free_list_generation: preparation.generation,
            buffer_token,
        };
        let result = self.with_render(usize::from(slot), |pair, host| {
            pair.publish(
                packet.clone(),
                &dependencies,
                lease,
                host,
                &mut |event| deferred.push(event),
            )
        });
        if packet.is_published() {
            self.queues.render.entry_mut(slot)?.published = true;
        } else if result.is_err() {
            self.queues.render.pools.rollback(preparation);
            self.queues.buffers.release(buffer_id)?;
            self.queues
                .render
                .entry_mut(slot)?
                .pair
                .as_mut()
                .ok_or(EIO)?
                .discard_prepared(packet)?;
        }
        result
    }
}
