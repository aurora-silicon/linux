// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Off-lock render graph construction and command preparation. Every borrowed
//! installed pair returns to its device owner before its preparation lease ends.

use super::{Config, Reservation};
use crate::g17::{
    buffer::MetricsLease,
    job::Packet,
    object::{Allocator, KernelObject, Pool},
    queue::render::{Pair, RenderAddresses},
};
use kernel::{prelude::*, sync::Arc};

struct BuildResources {
    uat: Arc<crate::mmu::Uat>,
    objects: Arc<Pool>,
    ids: Arc<crate::g17::freelist::RenderIds>,
    global: crate::mmu::Vm,
    metrics: Arc<KernelObject>,
    metrics_ids: Arc<crate::g17::buffer::MetricsIds>,
    metrics_bases: [u64; 2],
    config: Config,
}
impl BuildResources {
    fn new(firmware: &crate::g17::Firmware) -> Result<Self> {
        Ok(Self {
            uat: firmware.uat.clone(),
            objects: firmware.pool.clone(),
            ids: firmware.render_ids.clone(),
            global: firmware._render_global.clone(),
            metrics: firmware._pm_metrics.clone(),
            metrics_ids: firmware.queues.metrics.clone(),
            metrics_bases: [
                crate::hw::t8140::CONFIG.pm_metrics.va,
                firmware.init.metrics_fw_va()?,
            ],
            config: firmware.queues.render.config,
        })
    }
    fn metrics(&self) -> Result<MetricsLease> {
        MetricsLease::new(&self.metrics_ids, &self.metrics, self.metrics_bases)
    }
}

enum Build {
    Fresh(Reservation, u64, MetricsLease),
    Parked {
        parked: crate::g17::queue::render::parked::ParkedPair,
        buffer: (u8, u64),
        pm_generation: u64,
        metrics: MetricsLease,
    },
    Rebind {
        pair: KBox<Pair>,
        buffer: Option<(u8, u64)>,
        pm_generation: Option<u64>,
        metrics: Option<MetricsLease>,
    },
}

impl super::super::Backend {
    fn build_render_pair(&self, slot: u8, first: &Arc<Packet>) -> Result {
        let lease = self.shared.preparations.enter(false)?;
        let (resources, build) = {
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() {
                return Err(EAGAIN);
            }
            firmware.queues.ensure_render_ids(slot, self.owner)?;
            let resources = BuildResources::new(firmware)?;
            let build =
                if let Some(entry) = firmware.queues.render.entries[usize::from(slot)].as_ref() {
                    if entry.pending_error.is_some() || entry.pending_timeout {
                        self.shared.queue_events();
                        return Err(EBUSY);
                    }
                    if entry.constructing || entry.preparing.is_some() || entry.growing {
                        return Err(EBUSY);
                    }
                    if entry.parked.is_some() {
                        let metrics = resources.metrics()?;
                        let pm_generation = firmware.queues.render.reserve_pm_generation()?;
                        let buffer = firmware.queues.buffers.allocate_render()?;
                        let entry = firmware.queues.render.entry_mut(slot)?;
                        entry.preparing = Some(first.clone());
                        entry.pending_owner = Some(self.owner);
                        entry.borrowed_active = false;
                        Build::Parked {
                            parked: entry.parked.take().ok_or(EIO)?,
                            buffer,
                            pm_generation,
                            metrics,
                        }
                    } else if entry.pair.is_none() {
                        let metrics = resources.metrics()?;
                        let pm_generation = firmware.queues.render.reserve_pm_generation()?;
                        Build::Fresh(
                            firmware.queues.reserve_render(slot, self.owner)?,
                            pm_generation,
                            metrics,
                        )
                    } else {
                        let pair = entry.pair.as_ref().ok_or(EBUSY)?;
                        if pair.matches(self.owner, &self.context)? {
                            return Ok(());
                        }
                        if !pair.reusable()? {
                            return Err(EBUSY);
                        }
                        let foreign = !Arc::ptr_eq(pair.context().status(), self.context.status());
                        if foreign {
                            firmware
                                .queues
                                .render
                                .save_detaching_pool(&firmware.init, slot)?;
                        }
                        let metrics = if foreign {
                            Some(resources.metrics()?)
                        } else {
                            None
                        };
                        let pm_generation = if foreign {
                            Some(firmware.queues.render.reserve_pm_generation()?)
                        } else {
                            None
                        };
                        let buffer = if foreign {
                            Some(firmware.queues.buffers.allocate_render()?)
                        } else {
                            None
                        };
                        let entry = firmware.queues.render.entry_mut(slot)?;
                        entry.preparing = Some(first.clone());
                        entry.pending_owner = Some(self.owner);
                        entry.borrowed_active = entry
                            .pair
                            .as_ref()
                            .is_some_and(|pair| pair.published_in_flight());
                        entry.borrowed_dependencies =
                            entry.pair.as_ref().map(|pair| pair.dependency_snapshot());
                        Build::Rebind {
                            pair: entry.pair.take().ok_or(EIO)?,
                            buffer,
                            pm_generation,
                            metrics,
                        }
                    }
                } else {
                    let metrics = resources.metrics()?;
                    let pm_generation = firmware.queues.render.reserve_pm_generation()?;
                    Build::Fresh(
                        firmware.queues.reserve_render(slot, self.owner)?,
                        pm_generation,
                        metrics,
                    )
                };
            (resources, build)
        };
        let alloc = Allocator {
            dev: &self.shared.dev,
            uat: &resources.uat,
        };
        let fresh_graph = match &build {
            Build::Fresh(..) | Build::Parked { .. } => true,
            Build::Rebind { buffer, .. } => buffer.is_some(),
        };
        let result = match build {
            Build::Fresh(reservation, pm_generation, metrics) => {
                let built = (|| -> Result<KBox<Pair>> {
                    let free_list =
                        self.context
                            .render_pool(&alloc, &resources.ids, &resources.global)?;
                    Pair::new(
                        &alloc,
                        &resources.objects,
                        reservation.ids,
                        slot,
                        self.owner,
                        self.context.clone(),
                        free_list,
                        metrics,
                        reservation.buffer,
                        pm_generation,
                        first,
                        resources.config.clusters,
                        resources.config.descriptor_flags,
                    )
                })();
                let (mut built, error) = match built {
                    Ok(pair) => (Some(pair), None),
                    Err(error) => (None, Some(error)),
                };
                let result = (|| {
                    let mut state = self.shared.state.lock();
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if !lease.is_current() {
                        return Err(EAGAIN);
                    }
                    if let Some(error) = error {
                        return Err(error);
                    }
                    if !self.context.is_current() || self.context.status().get() != 0 {
                        return Err(EFAULT);
                    }
                    if firmware.queues.render.reserved(reservation)?.closed {
                        return Err(ECANCELED);
                    }
                    built
                        .as_mut()
                        .ok_or(EIO)?
                        .finish_install(&alloc, firmware.init.pb_descriptor_table()?)?;
                    firmware.queues.install_render(reservation, &mut built)
                })();
                // Fresh backing disappears before its temporary PB reference returns.
                drop(built);
                if result.is_err() {
                    let mut state = self.shared.state.lock();
                    (*state)
                        .as_deref_mut()
                        .ok_or(ENODEV)?
                        .queues
                        .cancel_render_reservation(reservation)?;
                }
                result
            }
            Build::Parked {
                mut parked,
                buffer,
                pm_generation,
                metrics,
            } => {
                let built = (|| -> Result<_> {
                    let free_list =
                        self.context
                            .render_pool(&alloc, &resources.ids, &resources.global)?;
                    let prepared = parked.prepare(
                        &alloc,
                        &resources.objects,
                        self.owner,
                        self.context.clone(),
                        free_list,
                        metrics,
                        buffer,
                        pm_generation,
                        first,
                    )?;
                    let storage = KBox::<Pair>::new_uninit(GFP_KERNEL)?;
                    Ok((prepared, storage))
                })();
                let (mut prepared, mut storage, error) = match built {
                    Ok((prepared, storage)) => (Some(prepared), Some(storage), None),
                    Err(error) => (None, None, Some(error)),
                };
                let result = {
                    let mut state = self.shared.state.lock();
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    (|| {
                        if !lease.is_current() {
                            return Err(EAGAIN);
                        }
                        if let Some(error) = error {
                            return Err(error);
                        }
                        if !self.context.is_current() || self.context.status().get() != 0 {
                            return Err(EFAULT);
                        }
                        let entry = firmware.queues.render.entry(slot)?;
                        if entry.pending_owner != Some(self.owner)
                            || entry.pending_owner_closed
                            || entry.pair.is_some()
                            || entry.parked.is_some()
                        {
                            return Err(ECANCELED);
                        }
                        let storage = storage.take().ok_or(EIO)?;
                        firmware.queues.buffers.can_release(buffer.0)?;
                        prepared.as_mut().ok_or(EIO)?.finish_install(
                            &parked,
                            &alloc,
                            firmware.init.pb_descriptor_table()?,
                        )?;
                        let pair = parked.commit(&mut prepared, storage)?;
                        firmware.queues.buffers.reserve_owner(buffer.0)?;
                        firmware.queues.buffers.release(buffer.0)?;
                        let entry = firmware.queues.render.entry_mut(slot)?;
                        entry.reservation.owner = self.owner;
                        entry.reservation.buffer = buffer;
                        entry.context = Some(self.context.clone());
                        entry.pool = Some(pair.pool().clone());
                        entry.pair = Some(pair);
                        entry.closed = false;
                        entry.published = false;
                        Ok(())
                    })()
                };
                // Rejected candidate mappings disappear before the PB identity
                // returns, while the original installed kick owner stays live.
                drop(prepared);
                drop(storage);
                {
                    let mut state = self.shared.state.lock();
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    if result.is_err() {
                        firmware.queues.render.entry_mut(slot)?.parked = Some(parked);
                        firmware.queues.buffers.release(buffer.0)?;
                    } else {
                        drop(state);
                        drop(parked);
                        state = self.shared.state.lock();
                    }
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    let entry = firmware.queues.render.entry_mut(slot)?;
                    entry.preparing = None;
                    entry.pending_owner = None;
                    entry.pending_owner_closed = false;
                }
                self.shared.changed.notify_all();
                result
            }
            Build::Rebind {
                mut pair,
                buffer,
                pm_generation,
                metrics,
            } => {
                let prepared = (|| {
                    let free_list =
                        self.context
                            .render_pool(&alloc, &resources.ids, &resources.global)?;
                    pair.prepare_bind(
                        &alloc,
                        &resources.objects,
                        self.owner,
                        self.context.clone(),
                        free_list,
                        metrics,
                        buffer,
                        pm_generation,
                        first,
                    )
                })();
                let (mut prepared, error) = match prepared {
                    Ok(binding) => (Some(binding), None),
                    Err(error) => (None, Some(error)),
                };
                let result = {
                    let mut state = self.shared.state.lock();
                    let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                    let result = (|| {
                        if !lease.is_current() {
                            return Err(EAGAIN);
                        }
                        if let Some(error) = error {
                            return Err(error);
                        }
                        if !self.context.is_current() || self.context.status().get() != 0 {
                            return Err(EFAULT);
                        }
                        let entry = firmware.queues.render.entry(slot)?;
                        if entry.pending_owner != Some(self.owner) || entry.pending_owner_closed {
                            return Err(ECANCELED);
                        }
                        if firmware.queues.render.retired.len()
                            == firmware.queues.render.retired.capacity()
                        {
                            return Err(ENOSPC);
                        }
                        firmware.queues.teardown.displace(
                            pair.context(),
                            true,
                            super::super::now_ns(),
                        )?;
                        pair.bind_owner(
                            prepared.as_mut().ok_or(EIO)?,
                            &alloc,
                            firmware.init.pb_descriptor_table()?,
                        )?;
                        if let Some((id, _)) = buffer {
                            firmware.queues.buffers.reserve_owner(id)?;
                            firmware.queues.buffers.release(id)?;
                        }
                        let entry = firmware.queues.render.entry_mut(slot)?;
                        entry.reservation.owner = self.owner;
                        if let Some(buffer) = buffer {
                            entry.reservation.buffer = buffer;
                        }
                        entry.context = Some(self.context.clone());
                        entry.pool = Some(pair.pool().clone());
                        entry.closed = false;
                        Ok(())
                    })();
                    // The original installed pair remains owned on every error.
                    let entry = firmware.queues.render.entry_mut(slot)?;
                    entry.preparing = None;
                    entry.pending_owner = None;
                    entry.pending_owner_closed = false;
                    entry.pair = Some(pair);
                    entry.borrowed_dependencies = None;
                    result
                };
                drop(prepared);
                if result.is_err() {
                    if let Some((id, _)) = buffer {
                        let mut state = self.shared.state.lock();
                        (*state)
                            .as_deref_mut()
                            .ok_or(ENODEV)?
                            .queues
                            .buffers
                            .release(id)?;
                    }
                }
                self.shared.changed.notify_all();
                result
            }
        };
        result?;
        let mut state = self.shared.state.lock();
        let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
        if !lease.is_current() {
            return Err(EAGAIN);
        }
        if firmware.queues.render.entry(slot)?.closed {
            return Err(ECANCELED);
        }
        if !self.context.is_current() || self.context.status().get() != 0 {
            return Err(EFAULT);
        }
        // From this point every error leaves the complete graph retained for
        // retry. Physical queue identities and installed mappings stay owned.
        if fresh_graph {
            firmware
                .queues
                .render
                .entry_mut(slot)?
                .pair
                .as_mut()
                .ok_or(EIO)?
                .rebase_fresh_graph()?;
        }
        let pool = firmware
            .queues
            .render
            .entry(slot)?
            .pool
            .clone()
            .ok_or(EIO)?;
        firmware.queues.render.pools.warm(&firmware.init, &pool)
    }

    pub(super) fn ensure_render(&self, packet: &Arc<Packet>, slot: u8) -> Result {
        self.build_render_pair(slot, packet)?;
        let sibling = {
            let state = self.shared.state.lock();
            (*state)
                .as_deref()
                .ok_or(ENODEV)?
                .queues
                .admission
                .warmable_sibling(self.owner, slot)
        };
        if let Some(sibling) = sibling {
            self.build_render_pair(sibling, packet)?;
        }
        Ok(())
    }

    pub(super) fn prepare_render(
        &self,
        slot: u8,
        packet: &Arc<Packet>,
        deferred: &mut super::super::DeferredBatch,
    ) -> Result {
        let lease = self.shared.preparations.enter(false)?;
        let (uat, mut pair, addresses, growth_target) = {
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() {
                return Err(EAGAIN);
            }
            let qids = firmware
                .queues
                .render
                .entry(slot)?
                .pair
                .as_ref()
                .ok_or(EBUSY)?
                .qids();
            let addresses = RenderAddresses {
                stamps: [
                    firmware.init.stamp_addresses(qids[0])?,
                    firmware.init.stamp_addresses(qids[1])?,
                ],
            };
            let pair = firmware
                .queues
                .render
                .entry_mut(slot)?
                .pair
                .as_mut()
                .ok_or(EIO)?;
            if !pair.matches(self.owner, &packet.context)? {
                return Err(EIO);
            }
            // The selected scheduler is retained through release acknowledgement
            // even if later private command allocation fails. Sibling warming
            // does not cross this publication boundary.
            packet.context.mark_published();
            let target = pair.host_growth_target(packet)?;
            let buffer_id = pair.buffer_id();
            let growth_target = if let Some(target) = target {
                let table = firmware.init.pb_descriptor_table()?;
                let offset = usize::from(buffer_id) * 16;
                core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
                let mut descriptor = [0; 4];
                for (index, word) in descriptor.iter_mut().enumerate() {
                    *word = table
                        .word(offset + index * 4)?
                        .load(core::sync::atomic::Ordering::Relaxed);
                }
                Some((target, descriptor))
            } else {
                None
            };
            let pair = firmware.queues.render.take_pair(slot, self.owner, packet)?;
            (firmware.uat.clone(), pair, addresses, growth_target)
        };
        let alloc = Allocator {
            dev: &self.shared.dev,
            uat: &uat,
        };
        let mut growth = None;
        let result = (|| {
            if let Some((target, descriptor)) = growth_target {
                let vm = pair.context().vm().clone();
                growth = pair.manager().prepare_growth(
                    &self.shared.dev,
                    &vm,
                    target,
                    Some(descriptor),
                )?;
            }
            pair.prepare(&alloc, packet, addresses)
        })();
        let mut returned = Some(pair);
        let mut state = self.shared.state.lock();
        let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
        firmware.queues.render.return_pair(slot, &mut returned)?;
        if firmware.queues.render.entry(slot)?.closed {
            firmware
                .queues
                .render
                .entry_mut(slot)?
                .pair
                .as_mut()
                .ok_or(EIO)?
                .release_owner();
        }
        self.shared.changed.notify_all();
        let published = (|| {
            let entry = firmware.queues.render.entry(slot)?;
            if entry.pending_error.is_some() || entry.pending_timeout {
                // Final timeout observations can settle unrelated queues. The
                // event worker owns the full collector needed for that sweep.
                self.shared.queue_events();
                return Err(EBUSY);
            }
            if !lease.is_current()
                || firmware.recovery.pending()
                || crate::g17::recovery::Memory::recovery_state(&firmware.init)? != 0
            {
                return Err(EAGAIN);
            }
            // Private restaging can fail after preparing a larger inventory.
            // Finish its descriptor before discarding the command preparation.
            firmware.commit_render_growth(slot, &mut growth)?;
            result?;
            if packet.cancel_requested() || firmware.queues.render.entry(slot)?.closed {
                return Err(ECANCELED);
            }
            firmware.publish_render(self.owner, slot, packet, deferred)
        })();
        if published.is_err() && !packet.is_published() {
            firmware
                .queues
                .render
                .entry_mut(slot)?
                .pair
                .as_mut()
                .ok_or(EIO)?
                .discard_prepared(packet)?;
        }
        published
    }
}
