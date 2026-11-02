// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! DRM compute on the M5 profile: each logical queue owns one M5 channel, a lane that is Neo's
//! compute queue with its `m5` arms ([`crate::g17::queue::m5`]), from QUEUE_CREATE on, installed
//! before the queue exists. A bound channel lives in a compute entry of the registry
//! ([`ComputeEntry::m5`]), the table Neo's compute queues live in, on the QID it reserved.
//! An installed lane (QID, kick ring, graph, support page and pool) stays with the firmware
//! session; logical queues lease it: the 26A428 firmware has no queue uninstall.
//! A closed queue's lane parks once its context's scheduler release (0x13) is consumed
//! (`runtime::teardown`, [`Lanes`]), and the next QUEUE_CREATE adopts it. A quarantined lane is
//! not reused until a witness ends its quarantine (a settled failure's drain, or a killed
//! context's drain, which recycles it: `Queue::settle_exit`, `Queue::recycle_killed`); past
//! [`LEAKED_LANES`] of them creation fails with ENOSPC. Every compute command, from the scheduler
//! or published early from its ioctl, goes through [`Backend::m5_packet`] and
//! the channel's one publication transaction; the shared completion scan
//! (`Firmware::scan_compute`) retires it with the channel's strict predicate.

use super::{
    render::FALLBACK, Backend, ComputeEntry, ComputeReservation, ComputeSlot, DeferredBatch,
    Registry, WORK_RING_WAIT_NS,
};
use crate::g17::{
    completion::Deferred,
    context::Context,
    fw::{channels::TvbGrowRequest, queue::DataMaster},
    job::{Engine, Packet},
    object::Allocator,
    preparation::Admission,
    queue::{
        compute::{retirement::Batch, Failure, Queue},
        m5::{Dispatch, Host},
        m5_render::{BindingArgs, Draw, Pair, PairArgs, PairGrowth, ParkedPair},
    },
    status::VmStatus,
};
use core::sync::atomic::Ordering;
use kernel::{prelude::*, sync::Arc};

/// Quarantined compute lanes the session tolerates: each keeps its QID, kick ring, pool and its
/// owner's execution context, so past this many a new logical queue is refused (ENOSPC) rather
/// than losing its device at the first submission. A timed-out lane whose
/// context was killed is recycled and does not count; what stays is a quarantine no witness
/// ends (the release consumed before the drain, a timed-out render pass).
pub(super) const LEAKED_LANES: usize = 8;

/// The M5 compute channels of the compute entries `compute` ([`ComputeEntry::m5`]); none off
/// the M5 Ultra.
pub(super) fn channels(compute: &[Option<ComputeEntry>]) -> impl Iterator<Item = &Queue> {
    compute
        .iter()
        .flatten()
        .filter_map(|entry| entry.m5.as_deref())
}

/// [`channels`], mutable.
pub(super) fn channels_mut(
    compute: &mut [Option<ComputeEntry>],
) -> impl Iterator<Item = &mut Queue> {
    compute
        .iter_mut()
        .flatten()
        .filter_map(|entry| entry.m5.as_deref_mut())
}

/// Every compute channel of `context` among the compute entries is settled: closed and idle, or
/// quarantined with the firmware idle on it (`Queue::settled`).
pub(super) fn compute_idle(compute: &[Option<ComputeEntry>], context: &Arc<Context>) -> bool {
    channels(compute)
        .filter(|channel| channel.binds(context))
        .all(|channel| channel.settled().unwrap_or(false))
}

/// The M5 lanes besides the bound compute channels, as the scheduler-release teardown sees them
/// (`runtime::teardown::Service`): parked compute lanes and render pairs, bound and parked. The
/// bound channels are read from the compute entries the teardown already walks. Both kinds park
/// through the same release and are adopted by later queues.
pub(super) struct Lanes<'a> {
    pub(super) parked: &'a mut KVec<crate::g17::queue::m5::ParkedLane>,
    pub(super) pairs: &'a mut KVec<KBox<Pair>>,
    pub(super) parked_pairs: &'a mut KVec<ParkedPair>,
}

/// What a render publication may grow its pair's pool with (`Firmware::publish_m5_render`): the
/// allocator, the GPU's bounds and the slot for the old backing.
type M5Growth<'a, 'b> = (
    &'a Allocator<'b>,
    &'a crate::g17::fw::m5::compute::Gpu,
    &'a mut Option<crate::g17::queue::m5::Grown>,
);

/// How [`crate::g17::Firmware::bind_m5_channel`] bound a new logical queue.
pub(super) enum M5Bind {
    /// A parked lane was adopted: the channel is bound and placed, with nothing to install.
    Adopted,
    /// A compute entry and a fresh QID are reserved for a channel to build and install.
    Fresh(ComputeReservation),
}

impl Lanes<'_> {
    /// Every render pair of `context` is settled (`Pair::settled`). A failed pair does not hold
    /// back the release of the context's healthy lanes.
    pub(super) fn render_idle(&self, context: &Arc<Context>) -> bool {
        self.pairs
            .iter()
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
            .all(|pair| pair.settled().unwrap_or(false))
    }

    /// The number of `context`'s healthy channels (in `compute`) and pairs that park at its
    /// release; its quarantined ones stay.
    pub(super) fn parkable(
        &self,
        compute: &[Option<ComputeEntry>],
        context: &Arc<Context>,
    ) -> Result<usize> {
        let mut count = 0;
        for channel in channels(compute).filter(|channel| channel.binds(context)) {
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
    pub(super) fn report(&self, compute: &[Option<ComputeEntry>], context: &Arc<Context>) {
        for channel in channels(compute).filter(|channel| channel.binds(context)) {
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
    pub(super) fn retains(&self, compute: &[Option<ComputeEntry>], context: &Arc<Context>) -> bool {
        channels(compute).any(|channel| channel.binds(context) && channel.quarantined().is_some())
            || self
                .pairs
                .iter()
                .any(|pair| Arc::ptr_eq(pair.context(), context) && pair.quarantined().is_some())
    }

    /// Parks every channel of `context`: its lane joins the parked lanes and leaves its compute
    /// entry, which is freed (the lane keeps its published QID), and its binding goes to
    /// `binding` to be dropped off the device mutex. The firmware consumed the context's
    /// scheduler release, and every channel is idle ([`Self::parkable`]).
    pub(super) fn park(
        &mut self,
        compute: &mut [Option<ComputeEntry>],
        context: &Arc<Context>,
        buffers: &mut crate::g17::buffer::BufferIds,
        binding: &mut impl FnMut(crate::g17::queue::m5::Binding) -> Result,
    ) -> Result {
        for slot in compute.iter_mut() {
            let parks = slot
                .as_ref()
                .and_then(|entry| entry.m5.as_deref())
                .is_some_and(|channel| channel.binds(context) && channel.quarantined().is_none());
            if !parks {
                continue;
            }
            self.parked.reserve(1, GFP_KERNEL)?;
            let channel = slot.take().and_then(|entry| entry.m5).ok_or(EIO)?;
            let (lane, owner) = channel.park_m5()?;
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
        // What stays is quarantined. A drain it still awaited can no longer be witnessed: the
        // firmware, idle on its rings (`settled`), consumed the context's release. It stays
        // quarantined for good, counted toward the cap, as the old rule had it.
        for channel in channels_mut(compute).filter(|channel| channel.binds(context)) {
            channel.end_witness_wait();
        }
        for pair in self
            .pairs
            .iter_mut()
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
        {
            pair.end_witness_wait();
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

/// Recovery's settlements of M5 work, as the queues' `Retained` methods
/// (`runtime::events::Retained`): Neo's own queues settle through `Firmware::settle_blamed_render`,
/// `settle_cancelled_renders` and the firmware's compute-fault events.
impl Pair {
    /// The firmware's blame of this pair's QID was found guilty: its pass in flight fails with
    /// `error`, guilty unless an earlier recovery spared it, as on Neo
    /// (`Firmware::settle_blamed_render`).
    pub(super) fn settle_blamed(&mut self, error: Error, deferred: &mut DeferredBatch) -> Result {
        let Some(packet) = self.oldest_packet() else {
            return Ok(());
        };
        packet.completion.settle_blamed();
        Registry::fail_pair(self, error, Failure::Settled, deferred)
    }

    /// The recovery table cancelled one of this pair's QIDs (`qids`): its spared pass in flight
    /// fails with ECANCELED, as on Neo (`Firmware::settle_cancelled_renders`), unless the restart
    /// cancelled it before it started (`Pair::hold_unstarted`): that one runs again once the
    /// firmware has retired it (`Pair::retire`, [`crate::g17::Firmware::replay_m5_render`]).
    pub(super) fn settle_cancelled(&mut self, qids: u128, deferred: &mut DeferredBatch) -> Result {
        if self.held() {
            return Ok(());
        }
        if self
            .qids()
            .into_iter()
            .any(|qid| qids & (1u128 << qid) != 0)
            && self.oldest_spared() == Some(true)
        {
            return Registry::fail_pair(self, ECANCELED, Failure::Settled, deferred);
        }
        Ok(())
    }
}

impl Queue {
    /// The firmware's blame of this lane's QID was found guilty: its kicks fail with `error`, the
    /// oldest guilty unless an earlier recovery spared it, as the render twin does
    /// ([`Pair::settle_blamed`]). Neo leaves a blamed compute queue to the firmware's
    /// compute-fault event; M5's restart is the only report a lost dispatch makes (no fault
    /// event came), so the settlement is explicit (`recovery::Queues::settle_blamed_compute`).
    pub(super) fn settle_blamed(&mut self, error: Error, deferred: &mut DeferredBatch) -> Result {
        let Some(packet) = self.oldest_packet() else {
            return Ok(());
        };
        packet.completion.settle_blamed();
        self.fail(error, Failure::Settled, &mut |event| deferred.push(event))
    }
}

impl Registry {
    /// The M5 compute channels, each in its compute entry ([`channels`]).
    pub(super) fn m5_channels(&self) -> impl Iterator<Item = &Queue> {
        channels(&self.compute)
    }

    /// `owner`'s M5 compute channel.
    pub(super) fn m5_channel(&self, owner: u64) -> Option<&Queue> {
        self.m5_channels()
            .find(|channel| channel.owner() == Some(owner))
    }

    fn m5_channel_mut(&mut self, owner: u64) -> Option<&mut Queue> {
        channels_mut(&mut self.compute).find(|channel| channel.owner() == Some(owner))
    }

    /// Every M5 render pair of `context` is settled (`Pair::settled`): its kill waits for them,
    /// as a Neo context's waits for its render side.
    pub(super) fn m5_render_settled(&self, context: &Arc<Context>) -> bool {
        self.m5_render
            .iter()
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
            .all(|pair| pair.settled().unwrap_or(false))
    }

    /// The QIDs of `context`'s M5 lanes (its compute channels and render pairs), and those of
    /// every other bound lane, for the scope check of its kill (`exit::State::scope`).
    pub(super) fn m5_exit_scope(&self, context: &Arc<Context>) -> (u128, u128) {
        let (mut own, mut foreign) = (0u128, 0u128);
        let mut note = |bound: &Arc<Context>, bits: u128| match Arc::ptr_eq(bound, context) {
            true => own |= bits,
            false => foreign |= bits,
        };
        for channel in self.m5_channels() {
            if let Some(bound) = channel.context() {
                note(bound, 1u128 << channel.qid());
            }
        }
        for pair in self.m5_render.iter() {
            let [tiling, fragment] = pair.qids();
            note(pair.context(), 1u128 << tiling | 1u128 << fragment);
        }
        (own, foreign)
    }

    /// Another context's M5 kick or render pass is in flight on a healthy lane: a kill waits
    /// for a moment without it, as for Neo's queues (`Firmware::kill_unsafe`). None off the M5
    /// Ultra.
    pub(super) fn m5_foreign_in_flight(&self, context: &Arc<Context>) -> bool {
        self.m5_channels()
            .any(|channel| channel.in_flight() && !channel.binds(context))
            || self
                .m5_render
                .iter()
                .any(|pair| pair.polls() && !Arc::ptr_eq(pair.context(), context))
    }

    /// Whether any M5 kick or render pass is in flight: the event worker polls their retirement.
    /// Quarantined lanes and pairs are not in flight, as quarantined Neo queues are not
    /// (`queue::compute::Queue::in_flight`).
    pub(super) fn m5_in_flight(&self) -> bool {
        self.m5_channels().any(|channel| channel.in_flight())
            || self.m5_render.iter().any(|pair| pair.polls())
    }

    /// Whether the event worker polls M5 retirement: a compute channel's finished kick ahead of
    /// its witnesses (completion events cover the rest, `Queue::retirement_polling`), or a
    /// render pass in flight on a healthy pair. A quarantined lane or pair awaiting its drain is
    /// not polled, as [`Self::m5_in_flight`] leaves it out; each event pass still checks it.
    pub(super) fn m5_polling(&self) -> bool {
        self.m5_channels()
            .any(|channel| channel.retirement_polling())
            || self.m5_render.iter().any(|pair| pair.polls())
    }

    /// The M5 pair growth request `request` names, started serving (`Pair::begin_growth`):
    /// None when no pair is named, as `RenderState::grow_owner` finds no render entry. A twin
    /// of that lookup while M5 pairs are not render entries (`Pair::grows`).
    pub(super) fn m5_grow_begin(&mut self, request: &TvbGrowRequest) -> Option<Result<PairGrowth>> {
        self.m5_render
            .iter_mut()
            .find(|pair| pair.grows(request))
            .map(|pair| pair.begin_growth())
    }

    /// Ends serving `growth` on its pair (`Pair::finish_growth`); EIO if the pair is gone,
    /// which its `growing` mark rules out.
    pub(super) fn m5_grow_finish(
        &mut self,
        growth: &PairGrowth,
        blocks: &mut Option<crate::g17::buffer::TvbGrowth>,
        prepared: &mut Option<crate::g17::buffer::PreparedGrowth>,
        commit: bool,
    ) -> Result<bool> {
        self.m5_pair(growth.owner)
            .ok_or(EIO)?
            .finish_growth(growth, blocks, prepared, commit)
    }

    fn m5_pair(&mut self, owner: u64) -> Option<&mut Pair> {
        self.m5_render
            .iter_mut()
            .find(|pair| pair.owner() == owner)
            .map(|pair| &mut **pair)
    }

    /// A free compute entry for an M5 channel and its (empty) cell, taken before the channel is
    /// bound or installed so that placing it ([`Self::place_m5`]) cannot fail on capacity or
    /// memory. The compute entries outnumber the QIDs, and a parked lane holds none.
    fn m5_entry(&self) -> Result<(usize, ComputeSlot)> {
        let slot = self
            .compute
            .iter()
            .position(Option::is_none)
            .ok_or_else(|| {
                crate::g17::capacity::refused(crate::g17::capacity::Site::ComputeSlots)
            })?;
        Ok((slot, ComputeSlot::new()?))
    }

    /// Reserves a compute entry and a fresh QID for `owner`'s channel, as [`Self::reserve_compute`]
    /// does for a Neo queue: the entry holds the reservation, with an empty cell and no channel,
    /// while the channel is built off the device mutex; `Firmware::install_m5_channel` installs
    /// it there, or [`Self::cancel_compute`] returns the entry and the QID. The QID is M5's
    /// (`qids.reserve_for` with no preferred QID and no render reservation: the caller's reach
    /// check keeps the render pairs within reach) and its refusals are returned as they are.
    fn reserve_m5(&mut self, owner: u64) -> Result<ComputeReservation> {
        if owner == 0
            || self
                .compute
                .iter()
                .flatten()
                .any(|entry| entry.reservation.owner == owner)
        {
            return Err(EINVAL);
        }
        let (slot, queue) = self.m5_entry()?;
        // QIDs are owned by host identities, not logical queues (whose numbers the bootstrap
        // render reservations share).
        let identity = self.reserve_identity()?;
        let id = self
            .qids
            .reserve_for(identity, DataMaster::Compute, None, 0)?;
        let reservation = ComputeReservation { slot, id, owner };
        self.compute[slot] = Some(ComputeEntry {
            reservation,
            queue,
            m5: None,
        });
        Ok(reservation)
    }

    /// M5 channels being built: reserved entries ([`Self::reserve_m5`]) with neither a queue nor a
    /// channel yet. On the M5 Ultra every such entry is one, since Neo's builder does not run.
    fn m5_building(&self) -> usize {
        self.compute
            .iter()
            .flatten()
            .filter(|entry| entry.m5.is_none() && entry.queue.is_none())
            .count()
    }

    /// Places a bound channel in the entry [`Self::m5_entry`] took, under the channel's QID and
    /// owner. The device mutex is held from that call on, so the entry is still free.
    fn place_m5(&mut self, (slot, queue): (usize, ComputeSlot), channel: KBox<Queue>) -> Result {
        let entry = self.compute.get_mut(slot).ok_or(EIO)?;
        if entry.is_some() {
            return Err(EIO);
        }
        let reservation = ComputeReservation {
            slot,
            id: channel.id(),
            owner: channel.owner().ok_or(EIO)?,
        };
        *entry = Some(ComputeEntry {
            reservation,
            queue,
            m5: Some(channel),
        });
        Ok(())
    }

    /// Stages every M5 kick not yet observed into the shared completion scan, through Neo's
    /// `Queue::request_observations` as `Firmware::scan_compute` stages a Neo queue's.
    pub(super) fn request_m5(&mut self) -> Result<bool> {
        let mut requested = false;
        for channel in channels(&self.compute) {
            requested |= channel.request_observations(&mut self.observations)?;
        }
        Ok(requested)
    }

    /// Retires the M5 kicks the shared scan completed: each lane in `Firmware::scan_compute`'s
    /// order for a Neo queue (its records latched, its retirement, then its quarantine's release
    /// on the witness), through Neo's methods and their `m5` arms, with the scan's `masks` and
    /// recovery generation `epoch`.
    pub(super) fn retire_m5(
        &mut self,
        port: &crate::g17::port::Port,
        masks: Option<[u64; 2]>,
        epoch: u64,
        state: u32,
        deferred: &mut DeferredBatch,
    ) -> Result {
        // Neo's rule for scans over every queue (`recovery::Queues::publish_recovery_targets`):
        // a failed one must not skip the queues after it. A broken witness fails only its own
        // lane or pair (Ambiguous); any error left over is returned once all were scanned.
        let mut failure = None;
        let observations: &mut Batch = &mut self.observations;
        for channel in channels_mut(&mut self.compute) {
            let result = channel
                .latch_observations(observations)
                .and_then(|()| {
                    channel.retire(
                        masks,
                        epoch,
                        state,
                        |owner| port.qos_complete(&[owner]),
                        &mut |event| deferred.push(event),
                    )
                })
                .and_then(|()| {
                    channel.release_witnessed(
                        state,
                        |owner| port.qos_complete(&[owner]),
                        &mut |event| deferred.push(event),
                    )
                });
            if let Err(error) = result {
                // A channel already past its failure (Final or Ambiguous) is only reported once.
                if channel.quarantined().is_none() || channel.awaiting_witness() {
                    pr_warn!(
                        "G17 M5: lane {} failed to retire: {:?}\n",
                        channel.qid(),
                        error
                    );
                    let result =
                        channel.fail(error, Failure::Ambiguous, &mut |event| deferred.push(event));
                    failure = failure.or(result.err());
                }
            }
        }
        for pair in &mut self.m5_render {
            // A failed pass that drained is released with its error, in Neo's order for a
            // quarantined command (`Deferred::Drained`): its fence signals that error if the
            // failure never reached it.
            let result = pair.retire(
                state,
                &mut |owners| port.qos_complete(owners),
                &mut |retired| {
                    let Some(packet) = retired.packet else {
                        return Ok(());
                    };
                    deferred.push(match retired.failed {
                        Some(error) => Deferred::Drained(packet.completion.clone(), Err(error)),
                        None => Deferred::Retired(packet.completion.clone(), Ok(retired.times)),
                    })
                },
            );
            if let Err(error) = result {
                if pair.quarantined().is_none() || pair.awaiting_witness() {
                    pr_warn!(
                        "G17 M5: render pair of queue {} failed to retire: {:?}: {:x?}\n",
                        pair.owner(),
                        error,
                        pair.report()
                    );
                    failure =
                        failure
                            .or(Self::fail_pair(pair, error, Failure::Ambiguous, deferred).err());
                }
            }
        }
        failure.map_or(Ok(()), Err)
    }

    /// Fails `pair`'s pass in flight with `error`: its fence errors while the pair keeps the
    /// command pinned with its render lease, quarantined (`Pair::fail`, Neo's
    /// `Completion::quarantine_render`), and the pair refuses its owner's later passes. When the
    /// pass retires, [`Self::retire_m5`] releases the command and the lease as Neo's exact drain
    /// does (`Deferred::Drained`, `Completion::release_after_retirement`), and a settled failure
    /// also ends the pair's quarantine: it parks rather than leaking.
    /// Settled failures are recovery's (blame, cancellation): the firmware restarted and resumed
    /// the pass. A timed-out pass is not one, nor is another pass of its failed VM: a 2.8 s
    /// pass timed out and drained, and 1.5 s later the firmware restarted on that pair's QID,
    /// failing the next owner's pass. Those pairs stay quarantined and count toward the cap
    /// (`Failure::Final`). A repeat after a settled failure changes nothing: recoveries
    /// failed the blamed pass, then its VM's failure failed it again.
    /// A pass awaiting its replay (`Pair::retire`) was retired by the firmware and holds nothing
    /// of the pair: it fails as the restart would have failed it, released at once, and the pair
    /// stays healthy.
    pub(super) fn fail_pair(
        pair: &mut Pair,
        error: Error,
        failure: Failure,
        deferred: &mut DeferredBatch,
    ) -> Result {
        if let Some((packet, _)) = pair.take_replay() {
            packet.completion.note_interrupted();
            return deferred.push(Deferred::Drained(packet.completion.clone(), Err(error)));
        }
        if let Some(packet) = pair.fail(error, failure) {
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
        for channel in channels_mut(&mut self.compute) {
            if channel.unfailed_kicks()
                && channel
                    .context()
                    .is_some_and(|bound| Arc::ptr_eq(bound.status(), status))
            {
                channel.quarantine(error, &mut |event| deferred.push(event))?;
            }
        }
        for pair in &mut self.m5_render {
            if pair.in_flight() && Arc::ptr_eq(pair.context().status(), status) {
                Self::fail_pair(pair, error, Failure::Final, deferred)?;
            }
        }
        Ok(())
    }

    /// The firmware failed a kick of `qid` ([`crate::g17::fw::channels::Event::ComputeFault`]).
    /// `window` is the raw fault window read as the event was serviced (`regs.rs`
    /// `m5_fault_registers`: 0xd800, the fault status 0xd8c0 and address 0xd8c8, 0xc078, 0xc008,
    /// 0xc028), logged for each M5 fault: nothing else says what the GPU faulted on.
    pub(super) fn m5_fault(&mut self, qid: u8, stamp: u32, window: Result<[u64; 6]>) {
        for channel in channels_mut(&mut self.compute).filter(|channel| channel.qid() == qid) {
            pr_warn!(
                "G17 M5: compute fault on queue {} (stamp {:#x}, {} kicks published): fault window {:x?}\n",
                qid,
                stamp,
                channel.kicks(),
                window
            );
            channel.note_fault(stamp);
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
        Self::fail_pair(pair, ETIMEDOUT, Failure::Final, deferred)?;
        Ok(true)
    }

    /// Closes `owner`'s channel; it stays installed and retained.
    pub(super) fn close_m5(&mut self, owner: u64) {
        if let Some(channel) = self.m5_channel_mut(owner) {
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
        let Some(channel) = self.m5_channel_mut(owner) else {
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
        channel.quarantine(ETIMEDOUT, &mut |event| deferred.push(event))?;
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
            .m5_channels()
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

    /// Binds logical queue `owner` to an M5 lane under the device mutex: adopts a parked lane, or
    /// reserves a compute entry and a fresh QID for a channel the caller builds off the device
    /// mutex and installs there ([`Self::install_m5_channel`]), as `Backend::build_compute` does
    /// for a Neo queue. Refusals (the quarantine cap, a render pair out of reach, no QID) come
    /// before anything is reserved.
    pub(super) fn bind_m5_channel(
        &mut self,
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        owner: u64,
    ) -> Result<M5Bind> {
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
        // A lane whose pool is still backed before a vacated one (`Registry::vacate_idle_pools`).
        let pick = |backed: bool, same: bool| {
            lanes.iter().position(|lane| {
                lane.adoptable() && (!backed || lane.backed()) && (!same || lane.class() == class)
            })
        };
        let parked = pick(true, true)
            .or_else(|| pick(true, false))
            .or_else(|| pick(false, true))
            .or_else(|| pick(false, false));
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
        // refused here fails at QUEUE_CREATE, not at its first pass. A channel still being built
        // (a reserved entry, [`Registry::reserve_m5`]) counts as an open queue.
        let free = match fresh {
            true => self
                .queues
                .qids
                .free()
                .saturating_sub(usize::from(parked.is_none())),
            false => 0,
        };
        let reach = self.queues.m5_parked_pairs.len() + free / 2;
        let waiting = 1
            + self.queues.m5_building()
            + self
                .queues
                .m5_channels()
                .filter(|channel| channel.open() && channel.quarantined().is_none())
                .filter(|channel| {
                    !self
                        .queues
                        .m5_render
                        .iter()
                        .any(|pair| Some(pair.owner()) == channel.owner())
                })
                .count();
        if reach < waiting {
            return Err(crate::g17::capacity::refused(
                crate::g17::capacity::Site::PhysicalQids,
            ));
        }
        let Some(index) = parked else {
            return self
                .queues
                .reserve_m5(owner)
                .map(M5Bind::Fresh)
                .map_err(|error| refused("QID", error));
        };
        // The channel's compute entry first: once bound it must have a place.
        let entry = self.queues.m5_entry()?;
        let channel = Queue::adopt_m5(&mut self.queues.m5_parked, index, context, owner)
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
        self.queues.place_m5(entry, channel)?;
        Ok(M5Bind::Adopted)
    }

    /// Installs `built`, the channel of `reservation` built off the device mutex, and places it in
    /// the reserved entry. From its installation on the lane is retained whatever happens: an
    /// installed channel leaves `built`, whatever the result. Otherwise `built` keeps it and the
    /// reservation stands: EAGAIN (no outer slot yet, nothing published) is the caller's to wait
    /// out; after another failure the caller drops the channel, then cancels the reservation.
    pub(super) fn install_m5_channel(
        &mut self,
        dev: &crate::driver::AsahiDrmDevice,
        reservation: ComputeReservation,
        built: &mut Option<KBox<Queue>>,
    ) -> Result {
        self.queues.reserved(reservation)?;
        let channel = built.as_mut().ok_or(EINVAL)?;
        if channel.id() != reservation.id || channel.owner() != Some(reservation.owner) {
            return Err(EIO);
        }
        let result = channel.install(&mut Host {
            init: &self.init,
            port: &self.port,
            qids: &mut self.queues.qids,
            crashed: &self.primary.state.shared.crashed,
        });
        if channel.installed() {
            let entry = self
                .queues
                .compute
                .get_mut(reservation.slot)
                .and_then(Option::as_mut)
                .ok_or(EIO)?;
            entry.m5 = built.take();
        }
        result.inspect_err(|error| {
            if *error != EAGAIN {
                dev_warn!(
                    dev.as_ref(),
                    "M5 channel of queue {}: installation failed: {:?}\n",
                    reservation.owner,
                    error
                );
            }
        })
    }

    /// Reinstalls `owner`'s adopted channel when its lane was recycled after a kill
    /// ([`Queue::reinstall`]); nothing for any other channel. EAGAIN: no outer slot yet.
    pub(super) fn m5_reinstall(&mut self, owner: u64) -> Result {
        let queues = &mut self.queues;
        let channel = channels_mut(&mut queues.compute)
            .find(|channel| channel.owner() == Some(owner))
            .ok_or(EIO)?;
        channel.reinstall(&mut Host {
            init: &self.init,
            port: &self.port,
            qids: &mut queues.qids,
            crashed: &self.primary.state.shared.crashed,
        })
    }

    /// Whether the firmware consumed the installation of `owner`'s channel
    /// ([`Queue::install_consumed`]). EIO: the channel is gone (a recovery closed it).
    pub(super) fn m5_install_consumed(&mut self, owner: u64, expired: bool) -> Result<bool> {
        let crashed = &self.primary.state.shared.crashed;
        let channel = self.queues.m5_channel_mut(owner).ok_or(EIO)?;
        channel.install_consumed(crashed, expired)
    }

    /// Publishes `packet` on `owner`'s channel, its pool grown first when the command's plan
    /// needs more ([`Queue::grow`]; the old backing lands in `grown`, for the caller to drop
    /// off the device mutex). EAGAIN: the channel has no room yet, or is not idle for a growth.
    fn publish_m5(
        &mut self,
        alloc: &Allocator<'_>,
        owner: u64,
        packet: &Arc<Packet>,
        dispatch: &Dispatch,
        grown: &mut Option<crate::g17::queue::m5::Grown>,
    ) -> Result {
        let work = packet.completion.work_state()?;
        let epoch = self.recovery.generation();
        let channel = channels_mut(&mut self.queues.compute)
            .find(|channel| channel.owner() == Some(owner))
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
        if channel.needs_pool(dispatch.resources.pool_blocks) {
            let primary = &self.primary;
            *grown = channel.grow(
                alloc,
                &mut host,
                || primary.notify(crate::g17::MSG_CONTROL_NOTIFY),
                dispatch.resources.pool_blocks,
            )?;
        }
        let result = channel.publish_m5(&mut host, dispatch, &work, epoch, packet.clone());
        if packet.is_published() {
            self.note_submission()?;
        }
        result
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
            let uat = firmware.uat.clone();
            let alloc = Allocator {
                dev: &dev,
                uat: &uat,
                layout: firmware.variant.vm_layout,
            };
            let mut grown = None;
            let result = firmware.publish_m5(&alloc, self.owner, packet, &dispatch, &mut grown);
            drop(state);
            drop(grown);
            match result {
                Err(error) if error == EAGAIN && !packet.is_published() => {
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
        // A pair whose pool is still backed before a vacated one (`Registry::vacate_idle_pools`),
        // as for a lane (`Self::bind_m5_channel`).
        let pick = |backed: bool, same: bool| {
            pairs.iter().position(|pair| {
                pair.adoptable() && (!backed || pair.backed()) && (!same || pair.class() == class)
            })
        };
        let parked = pick(true, true)
            .or_else(|| pick(true, false))
            .or_else(|| pick(false, true))
            .or_else(|| pick(false, false));
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

    /// Publishes again each M5 pass a restart cancelled before it started and the firmware then
    /// retired without running it (`Pair::retire`), on its own pair and ahead of its queue's
    /// later passes, which wait for the pair's room. Neo's rules (`replay_render_front`): a
    /// cancelled or failed command, or one whose context is gone, is not run again, nor one past
    /// `MAX_REPLAYS`; a replay with no room waits, bounded as a publication's wait for ring room.
    /// One that cannot be published fails as the restart would have failed it (ECANCELED).
    /// Called only at the event worker's outer boundary, after recovery has settled. None off
    /// the M5 Ultra: there are no M5 pairs.
    pub(in crate::g17) fn replay_m5_render(&mut self, deferred: &mut DeferredBatch) -> Result {
        if self
            .queues
            .m5_render
            .iter()
            .all(|pair| pair.replay().is_none())
        {
            return Ok(());
        }
        if self.recovery.pending() || crate::g17::recovery::Memory::recovery_state(&self.init)? != 0
        {
            return Ok(());
        }
        let dev = self.primary.state.shared.drm()?;
        for index in 0..self.queues.m5_render.len() {
            let pair = &mut self.queues.m5_render[index];
            let Some((packet, since)) = pair.take_replay() else {
                continue;
            };
            let (owner, qids) = (pair.owner(), pair.qids());
            let completion = &packet.completion;
            let refused = if packet.cancel_requested() || !packet.context.is_current() {
                Some(ECANCELED)
            } else if completion.status().get() != 0 {
                Some(EIO)
            } else if completion.replay_count() >= crate::g17::completion::MAX_REPLAYS {
                Some(ECANCELED)
            } else {
                None
            };
            let result = match (refused, &packet.command) {
                (Some(error), _) => Err(error),
                (
                    None,
                    crate::g17::command::Validated::Render {
                        pass,
                        plan: Some(plan),
                        ..
                    },
                ) => {
                    let draw = Draw { pass, plan: *plan };
                    // Its pool was grown for it before its first publication.
                    self.publish_m5_render(&dev, owner, &packet, &draw, None)
                }
                (None, _) => Err(EINVAL),
            };
            let pair = self.queues.m5_render.get_mut(index).ok_or(EIO)?;
            match result {
                Ok(()) => {
                    // Published from the start: it reports its own result
                    // (`Completion::begin_replay` clears the restart's interruption).
                    completion.begin_replay();
                    pr_info!("G17 M5: render pair {:?}: the pass the restart cancelled before it started runs again as pass {} (replay {})\n",
                        qids, pair.next_ordinal().wrapping_sub(1), completion.replay_count());
                }
                // Published, then refused: the pair failed it, and its retirement reports it.
                Err(_) if pair.owns_packet(&packet) => {
                    completion.begin_replay();
                }
                Err(error)
                    if error == EAGAIN
                        && super::now_ns().saturating_sub(since) < WORK_RING_WAIT_NS =>
                {
                    pair.restore_replay((packet, since));
                }
                Err(error) => {
                    pr_warn!("G17 M5: render pair {:?}: the pass the restart cancelled before it started could not run again ({:?}); it fails\n",
                        qids, error);
                    deferred.push(Deferred::Drained(completion.clone(), Err(ECANCELED)))?;
                }
            }
        }
        Ok(())
    }

    /// Publishes `packet`'s pass on `owner`'s pair, its pool grown first when the pass's plan
    /// needs more and `growth` allows it ([`Pair::grow`], the compute channel's rule; the old
    /// backing lands in its slot, for the caller to drop off the device mutex). Without
    /// `growth` a pass the pool is too small for is refused (EINVAL, `Pair::publish`). EAGAIN:
    /// the pair has no room yet, or is not idle for a growth.
    fn publish_m5_render(
        &mut self,
        dev: &crate::driver::AsahiDrmDevice,
        owner: u64,
        packet: &Arc<Packet>,
        draw: &Draw<'_>,
        growth: Option<M5Growth<'_, '_>>,
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
        if let Some((alloc, gpu, grown)) = growth.filter(|_| pair.needs_pool(draw.plan.pool_blocks))
        {
            let primary = &self.primary;
            *grown = pair.grow(
                alloc,
                &mut host,
                || primary.notify(crate::g17::MSG_CONTROL_NOTIFY),
                gpu,
                draw.plan.pool_blocks,
            )?;
        }
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
        // A pre-growth allocated off the device mutex ([`Pair::pregrowth`]), with the recovery
        // generation it was planned in, for the next hold to commit.
        let mut pregrowth: Option<(PairGrowth, u64, Option<crate::g17::buffer::TvbGrowth>)> = None;
        loop {
            admission.check()?;
            // What a pre-growth leaves unpublished, dropped after the device mutex (declared
            // before its guard).
            let mut spare: (
                Option<crate::g17::buffer::TvbGrowth>,
                Option<crate::g17::buffer::PreparedGrowth>,
            ) = (None, None);
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
            // The pass's parameter buffer, grown first when the binding's (sized for its first
            // pass) holds less than the pass needs (S's pre-growth): the blocks are allocated off
            // the device mutex, then committed under it while the pair is still idle with that
            // binding, in a recovery generation unchanged since the plan.
            if let Some((growth, epoch, blocks)) = pregrowth.take() {
                spare.0 = blocks;
                if epoch == firmware.recovery.generation() {
                    let init = &firmware.init;
                    let pair = firmware.queues.m5_pair(self.owner).ok_or(EIO)?;
                    if pair.finish_pregrowth(init, &growth, &mut spare.0, &mut spare.1)
                        .inspect_err(|error| pr_warn!("G17 M5: render pair of queue {}: parameter buffer pre-growth from {} to {} blocks refused: {:?}\n",
                            self.owner, growth.from, growth.target, error))?
                    {
                        pr_info!("G17 M5: render pair of queue {}: parameter buffer pre-grew from {} to {} blocks for a {}x{}x{} pass\n",
                            self.owner, growth.from, growth.target, pass.width, pass.height, pass.layers);
                    }
                }
            }
            if let Some(growth) = firmware
                .queues
                .m5_pair(self.owner)
                .ok_or(EIO)?
                .pregrowth(pass)?
            {
                let epoch = firmware.recovery.generation();
                drop(state);
                let blocks = crate::g17::buffer::Tvb::allocate_growth(&dev, &growth.vm, growth.geometry,
                    growth.from, growth.target)
                    .inspect_err(|error| pr_warn!("G17 M5: render pair of queue {}: parameter buffer pre-growth from {} to {} blocks failed: {:?}\n",
                        self.owner, growth.from, growth.target, error))?;
                pregrowth = Some((growth, epoch, blocks));
                continue;
            }
            let uat = firmware.uat.clone();
            let alloc = Allocator {
                dev: &dev,
                uat: &uat,
                layout: firmware.variant.vm_layout,
            };
            let mut grown = None;
            let result = firmware.publish_m5_render(
                &dev,
                self.owner,
                packet,
                &draw,
                Some((&alloc, &gpu, &mut grown)),
            );
            match result {
                Err(error) if error == EAGAIN && !packet.is_published() => {
                    drop(state);
                    drop(grown);
                    retry(lease)?;
                }
                result => {
                    // The old backing goes off the device mutex, after its view.
                    drop(state);
                    drop(grown);
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
