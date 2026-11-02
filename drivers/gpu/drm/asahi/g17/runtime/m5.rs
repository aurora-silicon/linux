// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! DRM compute on the M5 profile: each logical queue owns one M5 channel
//! ([`crate::g17::queue::m5::Channel`]) from QUEUE_CREATE on, installed before the queue exists.
//! An installed lane (QID, kick ring, graph, support page and pool) stays with the firmware
//! session; logical queues lease it: the 26A428 firmware has no queue uninstall.
//! A closed queue's lane parks once its context's scheduler release (0x13) is consumed
//! (`runtime::teardown`, [`Lanes`]), and the next QUEUE_CREATE adopts it; a quarantined lane is
//! never reused, and past [`LEAKED_LANES`] of them creation fails with ENOSPC. Every compute
//! command, from the scheduler or published early from its ioctl, goes through
//! [`Backend::m5_packet`] and the channel's one publication transaction; the shared completion scan
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
        m5_render::{BindingArgs, Draw, Pair, PairArgs, ParkedPair},
    },
    status::VmStatus,
};
use core::sync::atomic::Ordering;
use kernel::{prelude::*, sync::Arc};

/// Quarantined compute lanes the session tolerates: each keeps its QID, kick ring, pool and its
/// owner's execution context, so past this many a new logical queue is refused (ENOSPC) rather
/// than losing its device at the first submission.
pub(super) const LEAKED_LANES: usize = 8;

/// The M5 lanes as the scheduler-release teardown sees them (`runtime::teardown::Service`):
/// compute channels and render pairs, bound and parked. Both kinds park through the same
/// release and are adopted by later queues.
pub(super) struct Lanes<'a> {
    pub(super) channels: &'a mut KVec<KBox<Channel>>,
    pub(super) parked: &'a mut KVec<crate::g17::queue::m5::ParkedLane>,
    pub(super) pairs: &'a mut KVec<KBox<Pair>>,
    pub(super) parked_pairs: &'a mut KVec<ParkedPair>,
}

impl Lanes<'_> {
    /// Every compute channel of `context` is settled: closed and idle, or quarantined with the
    /// firmware idle on it (`Channel::settled`).
    pub(super) fn compute_idle(&self, context: &Arc<Context>) -> bool {
        self.channels
            .iter()
            .filter(|channel| Arc::ptr_eq(channel.context(), context))
            .all(|channel| channel.settled().unwrap_or(false))
    }

    /// Every render pair of `context` is settled (`Pair::settled`). A failed pair does not hold
    /// back the release of the context's healthy lanes.
    pub(super) fn render_idle(&self, context: &Arc<Context>) -> bool {
        self.pairs
            .iter()
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
            .all(|pair| pair.settled().unwrap_or(false))
    }

    /// The number of `context`'s healthy channels and pairs that park at its release; its
    /// quarantined ones stay.
    pub(super) fn parkable(&self, context: &Arc<Context>) -> Result<usize> {
        let mut count = 0;
        for channel in self
            .channels
            .iter()
            .filter(|channel| Arc::ptr_eq(channel.context(), context))
        {
            match channel.quarantined() {
                Some(_) if channel.settled()? => continue,
                None if channel.parkable()? => count += 1,
                _ => return Err(EBUSY),
            }
        }
        for pair in self
            .pairs
            .iter()
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
        {
            match pair.quarantined() {
                Some(_) if pair.settled()? => continue,
                None if pair.parkable()? => count += 1,
                _ => return Err(EBUSY),
            }
        }
        Ok(count)
    }

    /// Logs each of `context`'s lanes' park witnesses (a stalled release's diagnostic).
    pub(super) fn report(&self, context: &Arc<Context>) {
        for channel in self
            .channels
            .iter()
            .filter(|channel| Arc::ptr_eq(channel.context(), context))
        {
            channel.report_park();
        }
        for pair in self
            .pairs
            .iter()
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
        {
            pair.report_park();
        }
    }

    /// A quarantined lane of `context` remains: it keeps the context, and with it the
    /// execution context the firmware may still name, for the session (bounded by
    /// [`LEAKED_LANES`]).
    pub(super) fn retains(&self, context: &Arc<Context>) -> bool {
        self.channels.iter().any(|channel| {
            Arc::ptr_eq(channel.context(), context) && channel.quarantined().is_some()
        }) || self
            .pairs
            .iter()
            .any(|pair| Arc::ptr_eq(pair.context(), context) && pair.quarantined().is_some())
    }

    /// Parks every channel of `context`: its lane joins the parked lanes, its binding goes to
    /// `binding` to be dropped off the device mutex. The firmware consumed the context's
    /// scheduler release, and every channel is idle ([`Self::parkable`]).
    pub(super) fn park(
        &mut self,
        context: &Arc<Context>,
        buffers: &mut crate::g17::buffer::BufferIds,
        binding: &mut impl FnMut(crate::g17::queue::m5::Binding) -> Result,
    ) -> Result {
        while let Some(index) = self.channels.iter().position(|channel| {
            Arc::ptr_eq(channel.context(), context) && channel.quarantined().is_none()
        }) {
            self.parked.reserve(1, GFP_KERNEL)?;
            let channel = self.channels.remove(index).map_err(|_| EIO)?;
            let (lane, owner) = channel.park();
            if crate::module_parameters::m5_lane_trace.value() != 0 {
                pr_info!(
                    "M5 lane {}: parked (owner {} closed, {} parked)\n",
                    lane.qid(),
                    owner.owner(),
                    self.parked.len() + 1
                );
            }
            self.parked.push_within_capacity(lane).map_err(|_| ENOMEM)?;
            binding(owner)?;
        }
        // Its render pair parks as a pair; the binding's parameter-buffer ID returns.
        while let Some(index) = self
            .pairs
            .iter()
            .position(|pair| Arc::ptr_eq(pair.context(), context) && pair.quarantined().is_none())
        {
            self.parked_pairs.reserve(1, GFP_KERNEL)?;
            buffers.can_release(self.pairs[index].buffer_id())?;
            let pair = self.pairs.remove(index).map_err(|_| EIO)?;
            buffers.release(pair.buffer_id())?;
            let (lane, owner) = pair.park();
            if crate::module_parameters::m5_lane_trace.value() != 0 {
                pr_info!(
                    "M5 pair {:?}: parked (owner {} closed, {} parked)\n",
                    lane.qids(),
                    owner.owner(),
                    self.parked_pairs.len() + 1
                );
            }
            self.parked_pairs
                .push_within_capacity(lane)
                .map_err(|_| ENOMEM)?;
            binding(owner)?;
        }
        Ok(())
    }
}

/// Queues the result of one M5 kick for signalling off the device mutex. A failed kick stays
/// owned by its channel: its fence errors while its command stays pinned.
/// An M5 kick has nowhere to carry a firmware wait on another engine's completion: submission
/// gives M5 packets completion dependencies instead (`queue::Queue::enqueue`). One that still
/// carries such a wait would publish unordered.
fn refuse_firmware_waits(packet: &Packet) -> Result {
    if packet.has_firmware_waits() {
        pr_warn!("G17 M5: command with a firmware wait on another engine refused\n");
        return Err(EINVAL);
    }
    Ok(())
}

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

    /// Whether any M5 kick or render pass is in flight: the event worker polls their retirement.
    pub(super) fn m5_in_flight(&self) -> bool {
        self.m5.iter().any(|channel| channel.in_flight())
            || self.m5_render.iter().any(|pair| pair.in_flight())
    }

    fn m5_pair(&mut self, owner: u64) -> Option<&mut Pair> {
        self.m5_render
            .iter_mut()
            .find(|pair| pair.owner() == owner)
            .map(|pair| &mut **pair)
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
        for pair in &mut self.m5_render {
            match pair.retire(&mut |owners| port.qos_complete(owners)) {
                Ok(Some(retired)) => {
                    if let Some(packet) = retired.packet {
                        deferred.push(Deferred::Retired(
                            packet.completion.clone(),
                            Ok(retired.times),
                        ))?;
                    }
                }
                Ok(None) => {}
                // One pair's broken witness fails that pair, not every channel's retirement.
                Err(error) => {
                    pr_warn!(
                        "G17 M5: render pair of queue {} failed to retire: {:?}: {:x?}\n",
                        pair.owner(),
                        error,
                        pair.report()
                    );
                    Self::fail_pair(pair, error, deferred)?;
                }
            }
        }
        Ok(())
    }

    /// Fails `pair`'s pass in flight with `error`: its fence errors while the pair keeps the
    /// command pinned. Its render lease goes back now. An M5 admission slot names no physical
    /// pair, so nothing it guards outlives the failure: the pair stays quarantined and refuses
    /// its owner's later passes itself, and the failed pass no longer orders others (as a
    /// quarantined lease does not on the other paths). A lease kept with the pinned command,
    /// or quarantined, would never return: the pass never retires.
    fn fail_pair(pair: &mut Pair, error: Error, deferred: &mut DeferredBatch) -> Result {
        if let Some(packet) = pair.fail(error) {
            drop(packet.completion.detach_render_lease());
            deferred.push(Deferred::FailedOwned(packet.completion.clone(), error))?;
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
        for pair in &mut self.m5_render {
            if pair.in_flight() && Arc::ptr_eq(pair.context().status(), status) {
                Self::fail_pair(pair, error, deferred)?;
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

    /// Fails `owner`'s render pass with ETIMEDOUT when `packet` is it. Returns whether it was.
    pub(super) fn timeout_m5_render(
        &mut self,
        owner: u64,
        packet: &Arc<Packet>,
        deferred: &mut DeferredBatch,
    ) -> Result<bool> {
        let Some(pair) = self.m5_pair(owner) else {
            return Ok(false);
        };
        if !pair.owns_packet(packet) {
            return Ok(false);
        }
        pr_warn!(
            "G17 M5: render pass of queue {} timed out: {:x?}\n",
            owner,
            pair.report()
        );
        Self::fail_pair(pair, ETIMEDOUT, deferred)?;
        Ok(true)
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
        pr_warn!(
            "G17 M5: compute kick of queue {} timed out: stamp {:x?}, cursors {:x?}, front {:x?}\n",
            owner,
            channel.stamp(),
            channel.cursors(),
            channel.front_witnesses()
        );
        channel.fail(ETIMEDOUT, &mut |retired| defer(deferred, retired))?;
        if let Some(pair) = self.m5_pair(owner) {
            pr_warn!(
                "G17 M5: render pass of queue {} at that timeout: {:x?}\n",
                owner,
                pair.report()
            );
        }
        Ok(true)
    }
}

impl crate::g17::Firmware {
    /// Quarantined lanes, leaked deliberately: a compute channel's one, a
    /// render pair's two.
    fn quarantined_lanes(&self) -> usize {
        self.queues
            .m5
            .iter()
            .filter(|channel| channel.quarantined().is_some())
            .count()
            + 2 * self
                .queues
                .m5_render
                .iter()
                .filter(|pair| pair.quarantined().is_some())
                .count()
    }

    /// Below the quarantine cap: a fresh lane or pair may be built.
    fn fresh_lanes_allowed(&self) -> bool {
        self.quarantined_lanes() < LEAKED_LANES
    }

    /// Builds and installs the channel of logical queue `owner`, or adopts a parked lane. From
    /// its installation on the lane is retained whatever happens.
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
        // A parked lane of the same class first (no outer-ring change), else any, once its
        // previous binding is unmapped.
        let class = context.policy().priority();
        let lanes = &self.queues.m5_parked;
        let parked = lanes
            .iter()
            .position(|lane| lane.adoptable() && lane.class() == class)
            .or_else(|| lanes.iter().position(|lane| lane.adoptable()));
        // Quarantined lanes are leaked deliberately; past the cap no fresh lane is built, while
        // parked ones stay in use.
        let fresh = self.fresh_lanes_allowed();
        if parked.is_none() && !fresh {
            if self.queues.m5_refused != owner {
                self.queues.m5_refused = owner;
                dev_warn!(
                    dev.as_ref(),
                    "M5 queue {} refused: {} quarantined lanes (cap {}), no parked lane\n",
                    owner,
                    self.quarantined_lanes(),
                    LEAKED_LANES
                );
            }
            return Err(ENOSPC);
        }
        // Every open logical queue keeps a render pair within reach, as off M5
        // (`Qids::render_fits`): one already built, a parked one, or two free QIDs. A queue
        // refused here fails at QUEUE_CREATE, not at its first pass.
        let free = match fresh {
            true => self
                .queues
                .qids
                .free()
                .saturating_sub(usize::from(parked.is_none())),
            false => 0,
        };
        let reach = self.queues.m5_parked_pairs.len() + free / 2;
        let waiting = 1 + self
            .queues
            .m5
            .iter()
            .filter(|channel| channel.open() && channel.quarantined().is_none())
            .filter(|channel| {
                !self
                    .queues
                    .m5_render
                    .iter()
                    .any(|pair| pair.owner() == channel.owner())
            })
            .count();
        if reach < waiting {
            return Err(crate::g17::capacity::refused(
                crate::g17::capacity::Site::PhysicalQids,
            ));
        }
        if let Some(index) = parked {
            self.queues.m5.reserve(1, GFP_KERNEL)?;
            let channel = Channel::adopt(alloc, &mut self.queues.m5_parked, index, context, owner)
                .map_err(|error| refused("adoption", error))?;
            if crate::module_parameters::m5_lane_trace.value() != 0 {
                pr_info!(
                    "M5 lane {}: bound to queue {} class {} ({}), next kick {:#x}\n",
                    channel.qid(),
                    owner,
                    class,
                    if channel.class_changed() {
                        "class changed"
                    } else {
                        "same class"
                    },
                    channel.next_kick()
                );
            }
            return self
                .queues
                .m5
                .push_within_capacity(channel)
                .map_err(|_| ENOMEM);
        }
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
        self.m5_publication(packet, wait)
            .inspect_err(|error| self.note_m5_failure("compute", *error))
    }

    /// A command this queue fails at publication: logged, since its fence and the VM's status
    /// carry only the errno. Fallback, contention and cancellation are not failures.
    fn note_m5_failure(&self, engine: &str, error: Error) {
        if ![FALLBACK, EAGAIN, ECANCELED].contains(&error) {
            pr_warn!(
                "G17 M5: {} command of queue {} failed at publication: {:?}\n",
                engine,
                self.owner,
                error
            );
        }
    }

    fn m5_publication(&self, packet: &Arc<Packet>, wait: bool) -> Result {
        refuse_firmware_waits(packet)?;
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

impl crate::g17::Firmware {
    /// Builds and installs the render pair of logical queue `owner`, its target first sized for
    /// `pass`. From its installation on the pair is retained whatever happens.
    fn create_m5_pair(
        &mut self,
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        owner: u64,
        gpu: &crate::g17::fw::m5::compute::Gpu,
        clusters: u32,
        pass: &crate::g17::fw::render::RenderPass,
    ) -> Result {
        let dev = alloc.dev;
        let refused = |step: &str, error: Error| {
            dev_warn!(
                dev.as_ref(),
                "M5 render pair of queue {}: {} failed: {:?}\n",
                owner,
                step,
                error
            );
            error
        };
        let metrics = self.m5_metrics.clone().ok_or(EINVAL)?;
        let view = metrics
            .alias_in(
                context.vm(),
                alloc.domain(crate::g17::vm_layout::Domain::Compact),
                crate::mmu::UAT_PGSZ as u64,
                crate::mmu::PROT_GPU_SHARED_RW,
            )
            .map_err(|error| refused("metrics view", error))?;
        let lease = crate::g17::buffer::MetricsLease::new(
            &self.queues.metrics,
            &metrics,
            crate::g17::fw::buffer::MetricsAddresses {
                firmware: metrics.gpu_va(),
                client: view.iova(),
            },
        )?;
        // A parked pair of the same class first (no outer-ring change), else any: its
        // lanes, pool and command storage stay; this binding gets its own parameter
        // buffer, target and tiling state.
        let class = context.policy().priority();
        let pairs = &self.queues.m5_parked_pairs;
        let parked = pairs
            .iter()
            .position(|pair| pair.adoptable() && pair.class() == class)
            .or_else(|| pairs.iter().position(|pair| pair.adoptable()));
        if parked.is_none() && (!pairs.is_empty() || !self.fresh_lanes_allowed()) {
            // A parked pair still unmapping its previous binding is adoptable shortly; past the
            // quarantine cap no fresh pair is built.
            return Err(if pairs.is_empty() { ENOSPC } else { EAGAIN });
        }
        if let Some(index) = parked {
            self.queues.m5_render.reserve(1, GFP_KERNEL)?;
            let pb = self.queues.buffers.allocate_render()?;
            let binding = BindingArgs {
                owner,
                metrics: lease,
                metrics_view: view,
                pb,
                pm_generation: self.queues.render.reserve_pm_generation()?,
                pass,
                clusters,
                programs: self.variant.programs,
            };
            let pair = match Pair::adopt(
                alloc,
                &mut self.queues.m5_parked_pairs,
                index,
                context,
                binding,
            ) {
                Ok(pair) => pair,
                Err(error) => {
                    self.queues.buffers.release(pb.0)?;
                    return Err(refused("adoption", error));
                }
            };
            if let Err(error) = pair.install_pb(&self.init) {
                // The lanes stay installed: back to the parked pairs, never dropped.
                let (lane, _binding) = pair.park();
                self.queues
                    .m5_parked_pairs
                    .push_within_capacity(lane)
                    .map_err(|_| ENOMEM)?;
                self.queues.buffers.release(pb.0)?;
                return Err(refused("parameter buffer", error));
            }
            if crate::module_parameters::m5_lane_trace.value() != 0 {
                pr_info!(
                    "M5 pair {:?}: bound to queue {} class {} ({}), next pass {}\n",
                    pair.qids(),
                    owner,
                    class,
                    if pair.class_changed() {
                        "class changed"
                    } else {
                        "same class"
                    },
                    pair.next_ordinal()
                );
            }
            return self
                .queues
                .m5_render
                .push_within_capacity(pair)
                .map_err(|_| ENOMEM);
        }
        let identities = [
            self.queues.reserve_identity()?,
            self.queues.reserve_identity()?,
        ];
        let qids = &mut self.queues.qids;
        let tiling = qids
            .reserve_for(identities[0], DataMaster::Tiling, None, 0)
            .map_err(|error| refused("tiling QID", error))?;
        let fragment = match qids.reserve_for(identities[1], DataMaster::Fragment, None, 0) {
            Ok(id) => id,
            Err(error) => {
                qids.cancel_unpublished(tiling)?;
                return Err(refused("fragment QID", error));
            }
        };
        let pb = self.queues.buffers.allocate_render()?;
        let pm_generation = self.queues.render.reserve_pm_generation()?;
        let built = Pair::new(
            alloc,
            context,
            PairArgs {
                ids: [tiling, fragment],
                gpu,
                pointers: &self.variant.pointers,
                binding: BindingArgs {
                    owner,
                    metrics: lease,
                    metrics_view: view,
                    pb,
                    pm_generation,
                    pass,
                    clusters,
                    programs: self.variant.programs,
                },
            },
        );
        let mut pair = match built {
            Ok(pair) => pair,
            Err(error) => {
                let qids = &mut self.queues.qids;
                qids.cancel_unpublished(fragment)?;
                qids.cancel_unpublished(tiling)?;
                return Err(refused("construction", error));
            }
        };
        self.queues.m5_render.reserve(1, GFP_KERNEL)?;
        let result = pair.install(&mut Host {
            init: &self.init,
            port: &self.port,
            qids: &mut self.queues.qids,
            crashed: &self.primary.state.shared.crashed,
        });
        if pair.installed() {
            dev_info!(
                dev.as_ref(),
                "M5 render pair of queue {}: queues {:?}\n",
                owner,
                pair.qids()
            );
            self.queues
                .m5_render
                .push_within_capacity(pair)
                .map_err(|_| ENOMEM)?;
        } else {
            let qids = &mut self.queues.qids;
            qids.cancel_unpublished(fragment)?;
            qids.cancel_unpublished(tiling)?;
        }
        result.map_err(|error| refused("installation", error))
    }

    /// Publishes `packet`'s pass on `owner`'s pair. EAGAIN: the pair has no room yet.
    fn publish_m5_render(
        &mut self,
        dev: &crate::driver::AsahiDrmDevice,
        owner: u64,
        packet: &Arc<Packet>,
        draw: &Draw<'_>,
    ) -> Result {
        let work = packet.completion.work_state()?;
        let pair = self
            .queues
            .m5_render
            .iter_mut()
            .find(|pair| pair.owner() == owner)
            .ok_or(EIO)?;
        if let Some(error) = pair.quarantined() {
            return Err(error);
        }
        if !pair.has_room() {
            return Err(EAGAIN);
        }
        let mut host = Host {
            init: &self.init,
            port: &self.port,
            qids: &mut self.queues.qids,
            crashed: &self.primary.state.shared.crashed,
        };
        let result = pair.publish(dev, &mut host, draw, &work, Some(packet.clone()));
        if packet.is_published() {
            self.note_submission()?;
        }
        result
    }
}

impl Backend {
    /// Publishes an M5 render command on this queue's pair, building and installing the pair at
    /// its first pass. Each condition that clears with retirement or with recovery closing is
    /// waited for, bounded, as for M5 compute ([`Self::m5_packet`]).
    pub(in crate::g17) fn m5_render_packet(&self, packet: &Arc<Packet>) -> Result {
        self.m5_render_publication(packet)
            .inspect_err(|error| self.note_m5_failure("render", *error))
    }

    fn m5_render_publication(&self, packet: &Arc<Packet>) -> Result {
        refuse_firmware_waits(packet)?;
        packet.check_dependencies()?;
        let crate::g17::command::Validated::Render {
            pass,
            plan: Some(plan),
            ..
        } = &packet.command
        else {
            return Err(EINVAL);
        };
        let draw = Draw { pass, plan: *plan };
        let gpu = self.shared.m5.ok_or(EINVAL)?;
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
        // Render admission orders this pass against every other live render whose attachments
        // overlap (`queue::admission`, `hazard::plan`), as on every other path. M5 has no render
        // barriers (`Variant::render_barriers`), so another queue's overlapping render, in this
        // VM or through a shared object, is waited for here, off the device lock, until it
        // retires. The completion holds the lease until this pass retires, or until its pair
        // fails it ([`Registry::fail_pair`]).
        let render_lease = self.admission.acquire_render(
            self.owner,
            crate::g17::queue::admission::Request::of(packet),
            deadline,
            &check,
        )?;
        packet.completion.attach_render_lease(render_lease)?;
        let admission = Admission::new(deadline, &check);
        let progress = &self.shared.progress;
        loop {
            admission.check()?;
            let observed = progress.observe();
            let retry = |lease| -> Result {
                drop(lease);
                self.shared.queue_events();
                self.wait_release(progress, observed, &admission)
            };
            let lease = self.shared.preparations.enter_until(&admission)?;
            match packet.completion.work_state()?.acquire(Engine::Render) {
                Err(error) if error == EBUSY => {
                    retry(lease)?;
                    continue;
                }
                result => {
                    result?;
                }
            }
            let dev = self.shared.drm()?;
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
            if firmware.queues.m5_pair(self.owner).is_none() {
                let uat = firmware.uat.clone();
                let alloc = Allocator {
                    dev: &dev,
                    uat: &uat,
                    layout: firmware.variant.vm_layout,
                };
                match firmware.create_m5_pair(
                    &alloc,
                    self.context.clone(),
                    self.owner,
                    &gpu,
                    self.shared.clusters,
                    pass,
                ) {
                    // A parked pair is still unmapping its previous binding (`Pair::adopt`):
                    // the reclaim that finishes it wakes this wait, bounded as a full ring's.
                    Err(error) if error == EAGAIN => {
                        drop(state);
                        retry(lease)?;
                        continue;
                    }
                    result => result?,
                }
            }
            match firmware.publish_m5_render(&dev, self.owner, packet, &draw) {
                Err(error) if error == EAGAIN && !packet.is_published() => {
                    drop(state);
                    retry(lease)?;
                }
                result => {
                    if let Err(error) = result {
                        pr_warn!(
                            "G17 M5: render pass of queue {} not published: {:?}\n",
                            self.owner,
                            error
                        );
                    }
                    return result;
                }
            }
        }
    }
}
