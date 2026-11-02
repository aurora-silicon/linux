// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! DRM compute on the M5 profile: each logical queue owns one M5 channel
//! ([`crate::g17::queue::m5::Channel`]) from QUEUE_CREATE on, installed before the queue exists
//! and retained by the registry for the rest of the firmware session. Every compute command,
//! from the scheduler or published early from its ioctl, goes through [`Backend::m5_packet`] and
//! the channel's one publication transaction; the shared completion scan
//! (`Firmware::scan_compute`) retires it with the channel's strict predicate.

use super::{render::FALLBACK, Backend, DeferredBatch, Registry, WORK_RING_WAIT_NS};
use crate::g17::{
    completion::Deferred,
    context::Context,
    fw::queue::DataMaster,
    job::{Engine, Packet},
    object::Allocator,
    preparation::Admission,
    queue::{
        compute::retirement::Batch,
        m5::{Channel, Dispatch, Host, Retired},
    },
    status::VmStatus,
};
use core::sync::atomic::Ordering;
use kernel::{prelude::*, sync::Arc};

/// Queues the result of one M5 kick for signalling off the device mutex. A failed kick stays
/// owned by its channel: its fence errors while its command stays pinned.
fn defer(deferred: &mut DeferredBatch, retired: Retired) -> Result {
    let Some(packet) = retired.packet else {
        return Ok(());
    };
    deferred.push(match retired.result {
        Ok(times) => Deferred::Retired(packet.completion.clone(), Ok([times[0], times[1], 0, 0])),
        Err(error) => Deferred::FailedOwned(packet.completion.clone(), error),
    })
}

impl Registry {
    fn m5_channel(&mut self, owner: u64) -> Option<&mut Channel> {
        self.m5
            .iter_mut()
            .find(|channel| channel.owner() == owner)
            .map(|channel| &mut **channel)
    }

    /// Whether any M5 kick is in flight: the event worker polls their retirement.
    pub(super) fn m5_in_flight(&self) -> bool {
        self.m5.iter().any(|channel| channel.in_flight())
    }

    /// Stages every M5 kick not yet observed into the shared completion scan.
    pub(super) fn request_m5(&mut self) -> Result<bool> {
        let mut requested = false;
        for channel in &self.m5 {
            requested |= channel.request(&mut self.observations)?;
        }
        Ok(requested)
    }

    /// Retires the M5 kicks the shared scan completed.
    pub(super) fn retire_m5(
        &mut self,
        port: &crate::g17::port::Port,
        generation: u64,
        state: u32,
        deferred: &mut DeferredBatch,
    ) -> Result {
        let observations: &mut Batch = &mut self.observations;
        for channel in &mut self.m5 {
            channel.retire(
                observations,
                generation,
                state,
                &mut |owner| port.qos_complete(&[owner]),
                &mut |retired| defer(deferred, retired),
            )?;
        }
        Ok(())
    }

    /// Fails the M5 kicks of a failed VM.
    pub(super) fn fail_m5_vm(
        &mut self,
        status: &Arc<VmStatus>,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        for channel in &mut self.m5 {
            if channel.in_flight() && Arc::ptr_eq(channel.context().status(), status) {
                channel.fail(error, &mut |retired| defer(deferred, retired))?;
            }
        }
        Ok(())
    }

    /// The firmware failed a kick of `qid` ([`crate::g17::fw::channels::Event::ComputeFault`]).
    /// `window` is the raw fault window read as the event was serviced (`regs.rs`
    /// `m5_fault_registers`: 0xd800, the fault status 0xd8c0 and address 0xd8c8, 0xc078, 0xc008,
    /// 0xc028), logged for each M5 fault: nothing else says what the GPU faulted on.
    pub(super) fn m5_fault(&mut self, qid: u8, stamp: u32, window: Result<[u64; 6]>) {
        for channel in self.m5.iter_mut().filter(|channel| channel.qid() == qid) {
            pr_warn!(
                "G17 M5: compute fault on queue {} (stamp {:#x}, {} kicks published): fault window {:x?}\n",
                qid,
                stamp,
                channel.kicks(),
                window
            );
            channel.note_fault();
        }
    }

    /// Closes `owner`'s channel; it stays installed and retained.
    pub(super) fn close_m5(&mut self, owner: u64) {
        if let Some(channel) = self.m5_channel(owner) {
            channel.close();
        }
    }

    /// Fails `owner`'s kicks with ETIMEDOUT when `packet` is one of them. Returns whether it was.
    pub(super) fn timeout_m5(
        &mut self,
        owner: u64,
        packet: &Arc<Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result<bool> {
        let Some(channel) = self.m5_channel(owner) else {
            return Ok(false);
        };
        if !channel.owns_packet(packet) {
            return Ok(false);
        }
        channel.fail(ETIMEDOUT, &mut |retired| defer(deferred, retired))?;
        Ok(true)
    }
}

impl crate::g17::Firmware {
    /// Builds and installs the channel of logical queue `owner`. From its installation on the
    /// channel is retained whatever happens.
    pub(super) fn create_m5_channel(
        &mut self,
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        owner: u64,
        gpu: crate::g17::fw::m5::compute::Gpu,
    ) -> Result {
        let dev = alloc.dev;
        let refused = |step: &str, error: Error| {
            dev_warn!(
                dev.as_ref(),
                "M5 channel of queue {}: {} failed: {:?}\n",
                owner,
                step,
                error
            );
            error
        };
        // QIDs are owned by host identities, not logical queues (whose numbers the bootstrap
        // render reservations share). M5 refuses render: no logical queue reserves a render pair
        // beside its channel.
        let identity = self.queues.reserve_identity()?;
        let id = self
            .queues
            .qids
            .reserve_for(identity, DataMaster::Compute, None, 0)
            .map_err(|error| refused("QID", error))?;
        let mut channel = match Channel::new(alloc, context, id, owner, gpu, &self.variant.pointers)
        {
            Ok(channel) => channel,
            Err(error) => {
                self.queues.qids.cancel_unpublished(id)?;
                return Err(refused("construction", error));
            }
        };
        self.queues.m5.reserve(1, GFP_KERNEL)?;
        let result = channel.install(&mut Host {
            init: &self.init,
            port: &self.port,
            qids: &mut self.queues.qids,
            crashed: &self.primary.state.shared.crashed,
        });
        if channel.installed() {
            self.queues
                .m5
                .push_within_capacity(channel)
                .map_err(|_| ENOMEM)?;
        } else if self.queues.qids.can_cancel_unpublished(id) {
            self.queues.qids.cancel_unpublished(id)?;
        }
        result.map_err(|error| refused("installation", error))
    }

    /// Publishes `packet` on `owner`'s channel. EAGAIN: the channel has no room yet.
    fn publish_m5(&mut self, owner: u64, packet: &Arc<Packet>, dispatch: &Dispatch) -> Result {
        let work = packet.completion.work_state()?;
        let epoch = self.recovery.generation();
        let channel = self
            .queues
            .m5
            .iter_mut()
            .find(|channel| channel.owner() == owner)
            .ok_or(EIO)?;
        if let Some(error) = channel.quarantined() {
            return Err(error);
        }
        if !channel.has_room() {
            return Err(EAGAIN);
        }
        let mut host = Host {
            init: &self.init,
            port: &self.port,
            qids: &mut self.queues.qids,
            crashed: &self.primary.state.shared.crashed,
        };
        let result = channel.publish(
            &mut host,
            dispatch,
            &work,
            packet.order.sequence,
            epoch,
            Some(packet.clone()),
        );
        if packet.is_published() {
            self.note_submission()?;
        }
        result.map(|_| ())
    }
}

impl Backend {
    /// Publishes an M5 compute command. With `wait` (the scheduler's publication) each condition
    /// that clears with retirement or with recovery closing is waited for, bounded; without it
    /// (early publication from the ioctl) each returns FALLBACK, leaving the command unpublished
    /// for its scheduler.
    pub(super) fn m5_packet(&self, packet: &Arc<Packet>, wait: bool) -> Result {
        packet.check_dependencies()?;
        let dispatch = Dispatch::of(&packet.command)?;
        let deadline = super::now_ns().saturating_add(WORK_RING_WAIT_NS);
        let check = || {
            if packet.cancel_requested() {
                return Err(ECANCELED);
            }
            self.admission_open()?;
            if packet.completion.status().get() != 0 {
                return Err(EIO);
            }
            Ok(())
        };
        let admission = Admission::new(deadline, &check);
        let progress = &self.shared.progress;
        loop {
            if let Err(error) = admission.check() {
                return Err(if wait { error } else { FALLBACK });
            }
            let observed = progress.observe();
            let retry = |lease| -> Result {
                if !wait {
                    return Err(FALLBACK);
                }
                drop(lease);
                self.shared.queue_events();
                self.wait_release(progress, observed, &admission)
            };
            let lease = if wait {
                self.shared.preparations.enter_until(&admission)?
            } else {
                self.enter_preparation(false)?
            };
            // Work states free as this context's earlier commands retire.
            match packet.completion.work_state()?.acquire(Engine::Compute) {
                Err(error) if error == EBUSY => {
                    retry(lease)?;
                    continue;
                }
                result => {
                    result?;
                }
            }
            let mut state = self.shared.state.lock();
            let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
            if !lease.is_current() {
                drop(state);
                retry(lease)?;
                continue;
            }
            if self.shared.crashed.load(Ordering::Acquire) {
                return Err(EIO);
            }
            if firmware.recovery.pending()
                || crate::g17::recovery::Memory::recovery_state(&firmware.init)? != 0
            {
                drop(state);
                retry(lease)?;
                continue;
            }
            match firmware.publish_m5(self.owner, packet, &dispatch) {
                Err(error) if error == EAGAIN && !packet.is_published() => {
                    drop(state);
                    retry(lease)?;
                }
                result => return result,
            }
        }
    }
}
