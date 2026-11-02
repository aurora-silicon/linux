// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! DRM compute on the M5 profile: each logical queue owns one M5 channel, a lane that is Neo's
//! compute queue with its `m5` arms ([`crate::g17::queue::m5`]), leased at the queue's first
//! command ([`Backend::m5_lease_lane`]) and installed before that command publishes. A bound
//! channel lives in the cell of a compute entry of the registry (`ComputeSlot::lane`), the table
//! and cells Neo's compute queues live in, on the QID it reserved. An installed lane (QID, kick
//! ring, graph, support page and pool) stays with the firmware session; logical queues lease it:
//! the 26A428 firmware has no queue uninstall. A closed queue's lane is detached from it in its
//! compute entry once its context's scheduler release (0x13) is consumed (`runtime::teardown`,
//! [`Lanes`]), as Neo detaches an exited owner, and the next queue's first command binds it there
//! (Neo's `Queue::bind_owner`). A quarantined lane is not reused until a witness ends its
//! quarantine (a settled failure's drain, or a killed context's drain, which recycles it:
//! `Queue::settle_exit`, `Queue::recycle_killed`); past [`LEAKED_LANES`] of them no fresh lane
//! is built, and a queue's first command waits for a detached one up to QUEUE_CREATE's deadline,
//! then fails with ENOSPC. Every compute command, from the scheduler or published early from its
//! ioctl, goes through [`Backend::m5_packet`] and the channel's one publication transaction; the
//! shared completion scan (`Firmware::scan_compute`) retires it with the channel's strict
//! predicate.

use super::{
    render::FALLBACK, Backend, ComputeEntry, ComputeReservation, ComputeSlot, DeferredBatch,
    QueueGuard, Registry, QUEUE_CREATE_WAIT_NS, WORK_RING_WAIT_NS,
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
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};

/// Quarantined compute lanes the session tolerates: each keeps its QID, kick ring, pool and its
/// owner's execution context, so past this many no fresh lane is built: a queue's first command
/// waits for a detached lane up to QUEUE_CREATE's deadline, then fails with ENOSPC. A timed-out
/// lane whose context was killed is recycled and does not count; what stays is a quarantine no
/// witness ends (the release consumed before the drain, a timed-out render pass).
pub(super) const LEAKED_LANES: usize = 8;

/// The M5 compute channels of the compute entries `compute` (lanes in their entries' cells,
/// [`ComputeSlot::lane`]), each bound to a logical queue's context; none off the M5 Ultra. A lane
/// detached from its last owner ([`detached`]) is not one. Each comes locked: one at a time.
pub(super) fn channels(compute: &[Option<ComputeEntry>]) -> impl Iterator<Item = QueueGuard<'_>> {
    lanes(compute).filter(|lane| lane.context().is_some())
}

/// [`channels`], for changing them.
pub(super) fn channels_mut(
    compute: &mut [Option<ComputeEntry>],
) -> impl Iterator<Item = QueueGuard<'_>> {
    channels(compute)
}

/// The M5 lanes detached from their last owner (`Queue::detach_m5`), each in its compute entry
/// until a new logical queue binds it; none off the M5 Ultra.
pub(super) fn detached(compute: &[Option<ComputeEntry>]) -> impl Iterator<Item = QueueGuard<'_>> {
    lanes(compute).filter(|lane| lane.m5_detached())
}

/// Every M5 lane of the compute entries, bound or detached.
pub(super) fn lanes(compute: &[Option<ComputeEntry>]) -> impl Iterator<Item = QueueGuard<'_>> {
    compute
        .iter()
        .flatten()
        .filter_map(|entry| entry.queue.lane())
}

/// Every compute channel of `context` among the compute entries is settled: closed and idle, or
/// quarantined with the firmware idle on it (`Queue::settled`).
pub(super) fn compute_idle(compute: &[Option<ComputeEntry>], context: &Arc<Context>) -> bool {
    channels(compute)
        .filter(|channel| channel.binds(context))
        .all(|channel| channel.settled().unwrap_or(false))
}

/// A render slot's M5 pair: bound to its owner, parked for the slot's next owner, or the
/// reservation of a fresh one its owner builds off the device mutex (`Firmware::bind_m5_pair`),
/// as a Neo slot's entry holds its pair, its parked pair (`Entry::parked`) or its reservation
/// while `install_fresh_pair` builds.
pub(super) enum PairCell {
    Reserved(PairReservation),
    Built(KBox<Pair>),
    Parked(ParkedPair),
}

/// What a fresh pair's build holds: its slot and owner, the QIDs and parameter-buffer ID
/// reserved for it, and its graph's PM generation.
#[derive(Copy, Clone, PartialEq, Eq)]
pub(super) struct PairReservation {
    slot: u8,
    owner: u64,
    ids: [crate::g17::kick::Id; 2],
    pb: (u8, u64),
    pm_generation: u64,
}

/// The device resources a fresh pair's off-mutex build maps from, taken with its reservation.
pub(super) struct PairResources {
    uat: Arc<crate::mmu::Uat>,
    metrics: Arc<crate::g17::object::KernelObject>,
    metrics_ids: Arc<crate::g17::buffer::MetricsIds>,
}

/// How [`crate::g17::Firmware::bind_m5_pair`] bound a queue's render pair.
pub(super) enum PairBind {
    /// A parked pair was adopted at the slot.
    Adopted,
    /// The slot, QIDs and parameter buffer are reserved for a pair to build and install.
    Fresh(PairReservation, PairResources),
}

/// The M5 render pairs at their admission slots (`RenderState::m5`); none off the M5 Ultra.
pub(super) fn pairs(cells: &[Option<PairCell>]) -> impl Iterator<Item = &Pair> {
    cells.iter().filter_map(|cell| match cell {
        Some(PairCell::Built(pair)) => Some(&**pair),
        _ => None,
    })
}

/// The parked M5 render pairs, each at the slot it parked in.
pub(super) fn parked_pairs(cells: &[Option<PairCell>]) -> impl Iterator<Item = &ParkedPair> {
    cells.iter().filter_map(|cell| match cell {
        Some(PairCell::Parked(pair)) => Some(pair),
        _ => None,
    })
}

/// [`parked_pairs`], mutable.
pub(super) fn parked_pairs_mut(
    cells: &mut [Option<PairCell>],
) -> impl Iterator<Item = &mut ParkedPair> {
    cells.iter_mut().filter_map(|cell| match cell {
        Some(PairCell::Parked(pair)) => Some(pair),
        _ => None,
    })
}

/// The pair in `cells` at render slot `slot`, `owner`'s ([`Registry::m5_slot`]): None while the
/// slot has none bound (empty, or a parked pair to adopt). EAGAIN while a pair is being built
/// there.
fn slot_pair(cells: &mut [Option<PairCell>], slot: u8, owner: u64) -> Result<Option<&mut Pair>> {
    let cell = cells.get_mut(usize::from(slot)).ok_or(EINVAL)?;
    match cell {
        Some(PairCell::Built(pair)) if pair.owner() != owner => {
            pr_warn!(
                "G17 M5: render slot {} leased to queue {} holds queue {}'s pair {:?}\n",
                slot,
                owner,
                pair.owner(),
                pair.qids()
            );
            Err(EIO)
        }
        Some(PairCell::Built(pair)) => Ok(Some(&mut **pair)),
        // A build that its closed queue cancels, or this queue's own: either ends shortly.
        Some(PairCell::Reserved(_)) => Err(EAGAIN),
        Some(PairCell::Parked(_)) | None => Ok(None),
    }
}

/// [`pairs`], mutable.
pub(super) fn pairs_mut(cells: &mut [Option<PairCell>]) -> impl Iterator<Item = &mut Pair> {
    cells.iter_mut().filter_map(|cell| match cell {
        Some(PairCell::Built(pair)) => Some(&mut **pair),
        _ => None,
    })
}

/// The M5 Ultra's side of the scheduler-release teardown (`runtime::teardown::Service`), over
/// the compute entries and render slots the teardown holds (`compute`, `cells`, the render
/// slots' M5 pairs). Both kinds are given up at the same release and taken by later queues where
/// they stand: a lane detached in its compute entry, a pair parked at its slot.
pub(super) struct Lanes;

/// What a render publication may grow its pair's pool with (`Firmware::publish_m5_render`): the
/// allocator, the GPU's bounds and the slot for the old backing.
type M5Growth<'a, 'b> = (
    &'a Allocator<'b>,
    &'a crate::g17::fw::m5::compute::Gpu,
    &'a mut Option<crate::g17::queue::m5::Grown>,
);

/// How [`crate::g17::Firmware::bind_m5_channel`] bound a new logical queue.
pub(super) enum M5Bind {
    /// A detached lane was bound where it stands: nothing to install.
    Adopted,
    /// A compute entry and a fresh QID are reserved for a channel to build and install.
    Fresh(ComputeReservation),
}

impl Lanes {
    /// Every render pair of `context` is settled (`Pair::settled`). A failed pair does not hold
    /// back the release of the context's healthy lanes.
    pub(super) fn render_idle(&self, cells: &[Option<PairCell>], context: &Arc<Context>) -> bool {
        pairs(cells)
            .filter(|pair| Arc::ptr_eq(pair.context(), context))
            .all(|pair| pair.settled().unwrap_or(false))
    }

    /// The number of `context`'s healthy channels (in `compute`) and pairs that park at its
    /// release; its quarantined ones stay.
    pub(super) fn parkable(
        &self,
        compute: &[Option<ComputeEntry>],
        cells: &[Option<PairCell>],
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
        for pair in pairs(cells).filter(|pair| Arc::ptr_eq(pair.context(), context)) {
            match pair.quarantined() {
                Some(_) if pair.settled()? => continue,
                None if pair.parkable()? => count += 1,
                _ => return Err(EBUSY),
            }
        }
        Ok(count)
    }

    /// Logs each of `context`'s lanes' park witnesses (a stalled release's diagnostic).
    pub(super) fn report(
        &self,
        compute: &[Option<ComputeEntry>],
        cells: &[Option<PairCell>],
        context: &Arc<Context>,
    ) {
        for channel in channels(compute).filter(|channel| channel.binds(context)) {
            channel.report_park();
        }
        for pair in pairs(cells).filter(|pair| Arc::ptr_eq(pair.context(), context)) {
            pair.report_park();
        }
    }

    /// A quarantined lane of `context` remains: it keeps the context, and with it the
    /// execution context the firmware may still name, for the session (bounded by
    /// [`LEAKED_LANES`]).
    pub(super) fn retains(
        &self,
        compute: &[Option<ComputeEntry>],
        cells: &[Option<PairCell>],
        context: &Arc<Context>,
    ) -> bool {
        channels(compute).any(|channel| channel.binds(context) && channel.quarantined().is_some())
            || pairs(cells)
                .any(|pair| Arc::ptr_eq(pair.context(), context) && pair.quarantined().is_some())
    }

    /// Detaches every healthy channel of `context` in its compute entry (`Queue::detach_m5`,
    /// as Neo's `detach_exited_owner`): the lane keeps its entry and published QID until a new
    /// queue binds it, and its binding goes to `binding` to be dropped off the device mutex. Its
    /// render pair parks at its slot, as a Neo pair parks at its entry
    /// (`RenderState::detach_context`), and admission releases the slot to the next owner, which
    /// adopts the pair there (`Backend::release_queue` left the slot to this park). The firmware
    /// consumed the context's scheduler release, and every channel and pair is idle
    /// ([`Self::parkable`]).
    pub(super) fn park(
        &mut self,
        compute: &mut [Option<ComputeEntry>],
        cells: &mut [Option<PairCell>],
        admission: &crate::g17::queue::admission::Pool,
        context: &Arc<Context>,
        buffers: &mut crate::g17::buffer::BufferIds,
        binding: &mut impl FnMut(crate::g17::queue::m5::Binding) -> Result,
    ) -> Result {
        for mut channel in channels_mut(compute)
            .filter(|channel| channel.binds(context) && channel.quarantined().is_none())
        {
            let owner = channel.detach_m5()?;
            if crate::module_parameters::m5_lane_trace.value() != 0 {
                pr_info!(
                    "M5 lane {}: detached (owner {} closed)\n",
                    channel.qid(),
                    owner.owner()
                );
            }
            binding(owner)?;
        }
        // Its render pair parks as a pair; the binding's parameter-buffer ID returns.
        for cell in cells.iter_mut() {
            let Some(PairCell::Built(pair)) = cell else {
                continue;
            };
            if !Arc::ptr_eq(pair.context(), context) || pair.quarantined().is_some() {
                continue;
            }
            buffers.can_release(pair.buffer_id())?;
            let Some(PairCell::Built(pair)) = cell.take() else {
                return Err(EIO);
            };
            buffers.release(pair.buffer_id())?;
            let (lane, owner) = pair.park();
            if crate::module_parameters::m5_lane_trace.value() != 0 {
                pr_info!(
                    "M5 pair {:?}: parked (owner {} closed)\n",
                    lane.qids(),
                    owner.owner()
                );
            }
            let closed = owner.owner();
            *cell = Some(PairCell::Parked(lane));
            binding(owner)?;
            admission.release_owner(closed);
        }
        // What stays is quarantined. A drain it still awaited can no longer be witnessed: the
        // firmware, idle on its rings (`settled`), consumed the context's release. It stays
        // quarantined for good, counted toward the cap, as the old rule had it.
        for mut channel in channels_mut(compute).filter(|channel| channel.binds(context)) {
            channel.end_witness_wait();
        }
        for pair in pairs_mut(cells).filter(|pair| Arc::ptr_eq(pair.context(), context)) {
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

impl Registry {
    /// The M5 compute channels, each in its compute entry ([`channels`]).
    pub(super) fn m5_channels(&self) -> impl Iterator<Item = QueueGuard<'_>> {
        channels(&self.compute)
    }

    /// The M5 Ultra's arm of Neo's compute readiness (`Backend::compute_readiness`). `owner`'s
    /// lane waits for retirement while its ring is full or its pool is being grown
    /// (`Queue::m5_awaits_progress`). An owner with no lane yet (it is leased at the first
    /// command, `Backend::m5_lease_lane`) waits for progress while no detached lane is reusable
    /// and no QID is free: a lane detaches, and a QID frees, in the event worker's passes.
    /// Anything else is the publication's to judge.
    pub(super) fn m5_readiness(&self, owner: u64) -> super::Readiness {
        use super::Readiness;
        if let Some(lane) = self.m5_channel(owner) {
            return if lane.m5_awaits_progress() {
                Readiness::Progress
            } else {
                Readiness::Ready
            };
        }
        if lanes(&self.compute).any(|lane| lane.reusable()) || self.qids.free() != 0 {
            Readiness::Ready
        } else {
            Readiness::Progress
        }
    }

    /// `owner`'s M5 compute channel, locked.
    pub(super) fn m5_channel(&self, owner: u64) -> Option<QueueGuard<'_>> {
        self.m5_channels()
            .find(|channel| channel.owner() == Some(owner))
    }

    fn m5_channel_mut(&mut self, owner: u64) -> Option<QueueGuard<'_>> {
        self.m5_channel(owner)
    }

    /// The M5 render pairs, each at its admission slot ([`pairs`]).
    pub(super) fn m5_pairs(&self) -> impl Iterator<Item = &Pair> {
        pairs(&self.render.m5)
    }

    /// Whether `owner` still has its render pair at its slot: the slot stays its own until the
    /// pair parks ([`Lanes::park`]). A pair still being built is not: its build sees the
    /// queue closed and cancels it (`Backend::build_m5_pair`).
    pub(super) fn m5_pair_retained(&self, owner: u64) -> bool {
        self.m5_pairs().any(|pair| pair.owner() == owner)
    }

    /// Whether `owner` has a render pair at a slot, built or being built.
    fn m5_pair_held(&self, owner: u64) -> bool {
        self.render.m5.iter().flatten().any(|cell| match cell {
            PairCell::Built(pair) => pair.owner() == owner,
            PairCell::Reserved(reservation) => reservation.owner == owner,
            PairCell::Parked(_) => false,
        })
    }

    /// Admission's side of closing `owner` on the M5 Ultra (`Backend::release_queue`): a slot
    /// still holding its pair is released when the pair parks ([`Lanes::park`]); otherwise
    /// [`crate::g17::queue::admission::Pool::close_owner`] releases the owner's slots that hold
    /// a parked pair, and returns the empty ones unowned, so a next owner leases a slot with a
    /// parked pair to adopt before an empty one.
    pub(super) fn close_m5_render(&self, owner: u64) {
        if self.m5_pair_retained(owner) {
            return;
        }
        let cells = &self.render.m5;
        self.admission.close_owner(owner, |slot| {
            matches!(cells.get(slot), Some(Some(PairCell::Parked(_))))
        });
    }

    /// The pair at render slot `slot`, `owner`'s: None while the slot has none yet. EIO when
    /// another owner's pair is there, which admission rules out: it gives the slot to another
    /// owner only once the pair there has parked.
    fn m5_slot(&mut self, slot: u8, owner: u64) -> Result<Option<&mut Pair>> {
        slot_pair(&mut self.render.m5, slot, owner)
    }

    /// Every M5 render pair of `context` is settled (`Pair::settled`): its kill waits for them,
    /// as a Neo context's waits for its render side.
    pub(super) fn m5_render_settled(&self, context: &Arc<Context>) -> bool {
        self.m5_pairs()
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
        for pair in self.m5_pairs() {
            let [tiling, fragment] = pair.qids();
            note(pair.context(), 1u128 << tiling | 1u128 << fragment);
        }
        (own, foreign)
    }

    /// Another context's M5 render pass is in flight on a healthy pair: a kill waits for a
    /// moment without it, as for Neo's queues (`Firmware::kill_unsafe`, whose walk over the
    /// compute queues covers the M5 Ultra's lanes). None off the M5 Ultra.
    pub(super) fn m5_foreign_in_flight(&self, context: &Arc<Context>) -> bool {
        self.m5_pairs()
            .any(|pair| pair.polls() && !Arc::ptr_eq(pair.context(), context))
    }

    /// Whether any M5 render pass is in flight on a healthy pair (the compute lanes are among
    /// the compute queues the event worker's idle check walks). Quarantined pairs are not in
    /// flight, as quarantined Neo queues are not (`queue::compute::Queue::in_flight`).
    pub(super) fn m5_in_flight(&self) -> bool {
        self.m5_pairs().any(|pair| pair.polls())
    }

    /// Whether the event worker polls M5 retirement: a compute channel's finished kick ahead of
    /// its witnesses (completion events cover the rest, `Queue::retirement_polling`), or a
    /// render pass in flight on a healthy pair. A quarantined lane or pair awaiting its drain is
    /// not polled, as `Queue::in_flight` and [`Self::m5_in_flight`] leave it out; each event pass
    /// still checks it.
    pub(super) fn m5_polling(&self) -> bool {
        self.m5_channels()
            .any(|channel| channel.retirement_polling())
            || self.m5_pairs().any(|pair| pair.polls())
    }

    /// The M5 pair growth request `request` names, started serving (`Pair::begin_growth`):
    /// None when no pair is named, as `RenderState::grow_owner` finds no render entry. A twin
    /// of that lookup while M5 pairs are not render entries (`Pair::grows`).
    pub(super) fn m5_grow_begin(&mut self, request: &TvbGrowRequest) -> Option<Result<PairGrowth>> {
        pairs_mut(&mut self.render.m5)
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
        pairs_mut(&mut self.render.m5).find(|pair| pair.owner() == owner)
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
        // A free compute entry and its (empty) cell. There are as many entries as QIDs, and each
        // lane, bound or detached, holds its QID's.
        let slot = self
            .compute
            .iter()
            .position(Option::is_none)
            .ok_or_else(|| {
                crate::g17::capacity::refused(crate::g17::capacity::Site::ComputeSlots)
            })?;
        let queue = ComputeSlot::new()?;
        // QIDs are owned by host identities, not logical queues (whose numbers the bootstrap
        // render reservations share).
        let identity = self.reserve_identity()?;
        let id = self
            .qids
            .reserve_for(identity, DataMaster::Compute, None, 0)?;
        let reservation = ComputeReservation { slot, id, owner };
        self.compute[slot] = Some(ComputeEntry { reservation, queue });
        Ok(reservation)
    }

    /// M5 channels being built: reserved entries ([`Self::reserve_m5`]) with neither a queue nor a
    /// channel yet. On the M5 Ultra every such entry is one, since Neo's builder does not run.
    fn m5_building(&self) -> usize {
        self.compute
            .iter()
            .flatten()
            .filter(|entry| entry.queue.is_none())
            .count()
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
        for mut channel in channels_mut(&mut self.compute) {
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
        for pair in pairs_mut(&mut self.render.m5) {
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
        for mut channel in channels_mut(&mut self.compute) {
            if channel.unfailed_kicks()
                && channel
                    .context()
                    .is_some_and(|bound| Arc::ptr_eq(bound.status(), status))
            {
                channel.quarantine(error, &mut |event| deferred.push(event))?;
            }
        }
        for pair in pairs_mut(&mut self.render.m5) {
            if pair.in_flight() && Arc::ptr_eq(pair.context().status(), status) {
                Self::fail_pair(pair, error, Failure::Final, deferred)?;
            }
        }
        Ok(())
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
        if let Some(mut channel) = self.m5_channel_mut(owner) {
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
        let Some(mut channel) = self.m5_channel_mut(owner) else {
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
        drop(channel);
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
                .m5_pairs()
                .filter(|pair| pair.quarantined().is_some())
                .count()
    }

    /// Below the quarantine cap: a fresh lane or pair may be built.
    fn fresh_lanes_allowed(&self) -> bool {
        self.quarantined_lanes() < LEAKED_LANES
    }

    /// Binds logical queue `owner` to an M5 lane under the device mutex: binds a detached lane
    /// where it stands, through Neo's `Queue::reusable` and `Queue::bind_owner` and their `m5`
    /// arms, or reserves a compute entry and a fresh QID for a channel the caller builds off the
    /// device mutex and installs there ([`Self::install_m5_channel`]), as `Backend::build_compute`
    /// does for a Neo queue. Refusals (the quarantine cap, a render pair out of reach, no QID) come
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
        // A reusable detached lane (its previous binding unmapped), one whose pool is still
        // backed before a vacated one (`Registry::vacate_idle_pools`). The class does not pick
        // (no class effect was measured), as for pairs.
        let compute = &self.queues.compute;
        let pick = |backed: bool| {
            compute.iter().position(|entry| {
                entry
                    .as_ref()
                    .and_then(|entry| entry.queue.lane())
                    .is_some_and(|lane| lane.reusable() && (!backed || lane.m5_backed()))
            })
        };
        let parked = pick(true).or_else(|| pick(false));
        // Quarantined lanes are leaked deliberately; past the cap no fresh lane is built, while
        // detached ones stay in use.
        let fresh = self.fresh_lanes_allowed();
        if parked.is_none() && !fresh {
            if self.queues.m5_refused != owner {
                self.queues.m5_refused = owner;
                dev_warn!(
                    dev.as_ref(),
                    "M5 queue {} refused: {} quarantined lanes (cap {}), no detached lane\n",
                    owner,
                    self.quarantined_lanes(),
                    LEAKED_LANES
                );
            }
            return Err(ENOSPC);
        }
        // Every open logical queue keeps a render pair within reach, as off M5
        // (`Qids::render_fits`): one already built, a parked one, or two free QIDs. A queue
        // refused here waits at its first command, bounded, and then fails it. A channel still
        // being built (a reserved entry, [`Registry::reserve_m5`]) counts as an open queue.
        let free = match fresh {
            true => self
                .queues
                .qids
                .free()
                .saturating_sub(usize::from(parked.is_none())),
            false => 0,
        };
        let reach = parked_pairs(&self.queues.render.m5).count() + free / 2;
        let waiting = 1
            + self.queues.m5_building()
            + self
                .queues
                .m5_channels()
                .filter(|channel| channel.open() && channel.quarantined().is_none())
                .filter(|channel| {
                    !channel
                        .owner()
                        .is_some_and(|owner| self.queues.m5_pair_held(owner))
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
        // The lane binds in its compute entry, which now names the new owner.
        let class = context.policy().priority();
        let entry = self
            .queues
            .compute
            .get_mut(index)
            .and_then(Option::as_mut)
            .ok_or(EIO)?;
        let mut channel = entry.queue.lane().ok_or(EIO)?;
        channel
            .bind_owner(owner, context)
            .map_err(|error| refused("adoption", error))?;
        entry.reservation.owner = owner;
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
            entry.queue.put(built.take());
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
        let mut channel = channels_mut(&mut queues.compute)
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
        let mut channel = self.queues.m5_channel_mut(owner).ok_or(EIO)?;
        channel.install_consumed(crashed, expired)
    }

    /// Publishes `packet` on `owner`'s channel. Its pool must already back the command's plan
    /// (`Queue::pool_fits`; [`Backend::grow_m5`] grows it first). EAGAIN: the channel has no room
    /// yet, or its pool is being grown.
    fn publish_m5(&mut self, owner: u64, packet: &Arc<Packet>, dispatch: &Dispatch) -> Result {
        let work = packet.completion.work_state()?;
        let epoch = self.recovery.generation();
        let mut channel = channels_mut(&mut self.queues.compute)
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
        if !channel.pool_fits(dispatch.resources.pool_blocks) {
            return Err(EAGAIN);
        }
        let result = channel.publish_m5(&mut host, dispatch, &work, epoch, packet.clone());
        drop(channel);
        if packet.is_published() {
            self.note_submission()?;
        }
        result
    }
}

impl Backend {
    /// Publishes an M5 compute command. The scheduler's admission (`Backend::prepare_compute`)
    /// already held it, as a fence, while its lane's ring was full or its pool was being grown,
    /// as Neo's compute is held. With `wait` (the scheduler's publication) each condition
    /// that still clears with retirement or with recovery closing is waited for, bounded; without
    /// it (early publication from the ioctl) each returns FALLBACK, leaving the command unpublished
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

    /// Leases this queue's lane for its first command, as S claims a physical queue at
    /// preparation: a detached lane is bound in place, or a fresh one is built and installed
    /// (`Backend::install_m5`, which waits for the installation's consumption, bounded). With no
    /// lane to be had (no reusable lane, no QID, the quarantine cap) it waits for the event
    /// worker's progress, bounded by QUEUE_CREATE's deadline ([`QUEUE_CREATE_WAIT_NS`]), then fails
    /// with the refusal. Without `wait` (an early publication from the ioctl) the command is left
    /// to its scheduler (FALLBACK).
    fn m5_lease_lane(&self, packet: &Arc<Packet>, wait: bool) -> Result {
        let bound = |backend: &Self| -> Result<bool> {
            let state = backend.shared.state.lock();
            let bound = (*state)
                .as_deref()
                .ok_or(ENODEV)?
                .queues
                .m5_channel(backend.owner)
                .is_some();
            Ok(bound)
        };
        if bound(self)? {
            return Ok(());
        }
        if !wait {
            return Err(FALLBACK);
        }
        let gpu = self.shared.m5.as_ref().ok_or(EIO)?;
        let deadline = super::now_ns().saturating_add(QUEUE_CREATE_WAIT_NS);
        let check = || {
            if packet.cancel_requested() {
                return Err(ECANCELED);
            }
            self.admission_open()?;
            Ok(())
        };
        let admission = Admission::new(deadline, &check);
        let progress = &self.shared.progress;
        loop {
            admission.check()?;
            let observed = progress.observe();
            let lease = self.shared.preparations.enter_until(&admission)?;
            let dev = self.shared.drm()?;
            let uat = self.shared.state.lock().as_ref().ok_or(ENODEV)?.uat.clone();
            let alloc = Allocator {
                dev: &dev,
                uat: &uat,
                layout: self.shared.variant.vm_layout,
            };
            let result =
                Self::install_m5(&self.shared, &lease, &alloc, &self.context, self.owner, gpu);
            drop(lease);
            match result {
                Ok(()) => return Ok(()),
                // Nothing to lease yet, or the session moved on: wait for the event worker's
                // progress (a lane detaching, a QID freeing), then try again.
                Err(error) if error == ENOSPC || error == EAGAIN => {
                    if bound(self)? {
                        return Ok(());
                    }
                    self.shared.queue_events();
                    self.wait_release(progress, observed, &admission)?;
                }
                Err(error) => return Err(error),
            }
        }
    }

    fn m5_publication(&self, packet: &Arc<Packet>, wait: bool) -> Result {
        refuse_firmware_waits(packet)?;
        packet.check_dependencies()?;
        let dispatch = Dispatch::of(&packet.command)?;
        self.m5_lease_lane(packet, wait)?;
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
            // A plan the pool doesn't back grows it first, off the device mutex as Neo's
            // `grow_compute` does ([`Self::grow_m5`]), then publishes.
            let blocks = dispatch.resources.pool_blocks;
            if firmware
                .queues
                .m5_channel(self.owner)
                .is_some_and(|channel| !channel.pool_fits(blocks))
            {
                drop(state);
                drop(lease);
                match self.grow_m5(blocks, &admission, wait) {
                    Ok(()) => continue,
                    // The pool's backing was refused (nothing was published, and the pool is as
                    // it was): idle pools go back as for a Neo render (`reclaim_render_capacity`),
                    // and the command waits while a release is in flight, bounded as a full
                    // ring's. With nothing to give back it fails, as before.
                    Err(error) if error == ENOMEM => {
                        let releasing = self
                            .shared
                            .state
                            .lock()
                            .as_deref()
                            .is_some_and(|firmware| firmware.queues.m5_pool_release_pending());
                        if !wait || (!self.reclaim_render_capacity()? && !releasing) {
                            return Err(if wait { error } else { FALLBACK });
                        }
                        self.shared.queue_events();
                        self.wait_release(progress, observed, &admission)?;
                        continue;
                    }
                    Err(error) => return Err(error),
                }
            }
            let result = firmware.publish_m5(self.owner, packet, &dispatch);
            drop(state);
            match result {
                Err(error) if error == EAGAIN && !packet.is_published() => {
                    retry(lease)?;
                }
                result => return result,
            }
        }
    }
}

impl Backend {
    /// Grows this queue's M5 Ultra lane's pool for a command whose plan needs `blocks`, as Neo's
    /// `grow_compute` grows a compute queue's: the fresh backing and its client view are built off
    /// the device mutex under the preparation lease (`queue::m5::build_backing`); then, under the
    /// mutex, the growth begins once the lane is idle (`Queue::begin_growth`, which publishes the
    /// pool's release) and finishes once the firmware consumed it (`Queue::finish_growth_m5`),
    /// waiting on the event worker's progress in between. An abandoned growth ends
    /// (`Queue::abort_growth`), its release left to the vacate pass's observation. Without `wait`
    /// (an early publication from the ioctl) a growth that must wait returns FALLBACK. ENOMEM:
    /// the backing could not be allocated; the pool is as it was.
    fn grow_m5(&self, blocks: usize, admission: &Admission<'_>, wait: bool) -> Result {
        admission.check()?;
        let (qid, geometry) = {
            let state = self.shared.state.lock();
            let firmware = state.as_deref().ok_or(ENODEV)?;
            let channel = firmware.queues.m5_channel(self.owner).ok_or(EIO)?;
            if channel.pool_fits(blocks) {
                return Ok(());
            }
            channel.m5_growth_plan(blocks)?
        };
        let lease = match wait {
            true => self.shared.preparations.enter_until(admission)?,
            false => self.enter_preparation(false)?,
        };
        let dev = self.shared.drm()?;
        let uat = self.shared.state.lock().as_ref().ok_or(ENODEV)?.uat.clone();
        let alloc = Allocator {
            dev: &dev,
            uat: &uat,
            layout: self.shared.variant.vm_layout,
        };
        let mut fresh = Some(crate::g17::queue::m5::build_backing(
            &alloc,
            qid,
            geometry,
            self.context.vm(),
        )?);
        let mut deferred = DeferredBatch::publication()?;
        let mut growth = None;
        let progress = &self.shared.progress;
        let result = (|| loop {
            let observed = progress.observe();
            let result = (|| {
                let mut state = self.shared.state.lock();
                admission.check()?;
                let firmware = (*state).as_deref_mut().ok_or(ENODEV)?;
                if !lease.is_current() {
                    return Err(EAGAIN);
                }
                if self.shared.crashed.load(Ordering::Acquire) {
                    return Err(EIO);
                }
                firmware.scan_compute(None, &mut deferred)?;
                let mut channel = firmware.queues.m5_channel_mut(self.owner).ok_or(EIO)?;
                if growth.is_none() {
                    if channel.pool_fits(blocks) {
                        return Ok(None);
                    }
                    let notify = channel.begin_growth(&firmware.init, blocks)?;
                    // The lane's identity before anything else can fail: a close may take the
                    // owner's lookup.
                    growth = channel.growth_token();
                    if notify {
                        fence(Ordering::SeqCst);
                        firmware.primary.notify(crate::g17::MSG_CONTROL_NOTIFY)?;
                    }
                }
                channel
                    .finish_growth_m5(&firmware.init, &mut fresh, blocks)
                    .map(Some)
            })();
            deferred.finish();
            match result {
                Ok(old) => {
                    drop(old);
                    return Ok(());
                }
                Err(error) if error == EAGAIN && lease.is_current() => {
                    if !wait {
                        return Err(FALLBACK);
                    }
                    self.shared.queue_events();
                    self.wait_release(progress, observed, admission)?;
                }
                Err(error) => return Err(error),
            }
        })();
        if result.is_err() {
            if let Some(token) = growth {
                let mut state = self.shared.state.lock();
                if let Some(firmware) = state.as_deref_mut() {
                    for mut channel in channels_mut(&mut firmware.queues.compute) {
                        if channel.abort_growth(token)?.is_some() {
                            break;
                        }
                    }
                }
                drop(state);
                self.shared.queue_events();
            }
        }
        // An unused backing and its view go off the device mutex: the view first.
        if let Some((backing, view)) = fresh.take() {
            drop(view);
            drop(backing);
        }
        result
    }
}

impl crate::g17::Firmware {
    /// Binds logical queue `owner`'s render pair at its render slot `slot` (its admission
    /// lease's) under the device mutex, its target first sized for `pass`: adopts the pair
    /// parked there, as `build_render_pair`'s `Build::Parked` adopts a Neo slot's, or reserves
    /// the slot, two fresh QIDs and a parameter buffer for a pair the caller builds off the
    /// device mutex and installs there ([`Backend::build_m5_pair`]), as its `Build::Fresh`
    /// does. Admission leases a released slot, whose pair parked there, before an empty one,
    /// so a fresh pair is built only when no parked pair is free. Refusals (the quarantine cap)
    /// come before anything is reserved.
    fn bind_m5_pair(
        &mut self,
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        slot: u8,
        owner: u64,
        clusters: u32,
        pass: &crate::g17::fw::render::RenderPass,
    ) -> Result<PairBind> {
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
        // A parked pair whose pool is still backed first, as before the pairs parked at their
        // slots: the vacate pass releases the least recently parked pools first
        // (`Registry::vacate_idle_pools`), and admission leases the least recently used released
        // slot. When this slot's parked pair is vacated or still unmapping, it trades cells with
        // another slot's that is backed and adoptable; both stay parked, so admission's view of
        // the slots is unchanged.
        let cells = &mut self.queues.render.m5;
        let here = usize::from(slot);
        let ready = |cell: &Option<PairCell>| matches!(cell, Some(PairCell::Parked(pair)) if pair.adoptable() && pair.backed());
        if matches!(cells.get(here), Some(Some(PairCell::Parked(_)))) && !ready(&cells[here]) {
            if let Some(other) =
                (0..cells.len()).find(|other| *other != here && ready(&cells[*other]))
            {
                cells.swap(here, other);
            }
        }
        // The pair's place: the slot, empty or with a parked pair (`Registry::m5_slot`).
        let parked = match self.queues.render.m5.get(usize::from(slot)).ok_or(EINVAL)? {
            None => false,
            // A parked pair still unmapping its previous binding is adoptable shortly.
            Some(PairCell::Parked(pair)) if !pair.adoptable() => return Err(EAGAIN),
            Some(PairCell::Parked(_)) => true,
            Some(_) => return Err(EIO),
        };
        let metrics = self.m5_metrics.clone().ok_or(EINVAL)?;
        // Past the quarantine cap no fresh pair is built.
        if !parked && !self.fresh_lanes_allowed() {
            return Err(ENOSPC);
        }
        if !parked {
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
            let reserved = (|| {
                let pb = self.queues.buffers.allocate_render()?;
                match self.queues.render.reserve_pm_generation() {
                    Ok(pm_generation) => Ok((pb, pm_generation)),
                    Err(error) => {
                        self.queues.buffers.release(pb.0)?;
                        Err(error)
                    }
                }
            })();
            let (pb, pm_generation) = match reserved {
                Ok(reserved) => reserved,
                Err(error) => {
                    let qids = &mut self.queues.qids;
                    qids.cancel_unpublished(fragment)?;
                    qids.cancel_unpublished(tiling)?;
                    return Err(error);
                }
            };
            let reservation = PairReservation {
                slot,
                owner,
                ids: [tiling, fragment],
                pb,
                pm_generation,
            };
            self.queues.render.m5[usize::from(slot)] = Some(PairCell::Reserved(reservation));
            return Ok(PairBind::Fresh(
                reservation,
                PairResources {
                    uat: self.uat.clone(),
                    metrics,
                    metrics_ids: self.queues.metrics.clone(),
                },
            ));
        }
        // The slot's parked pair: its lanes, pool and command storage stay;
        // this binding gets its own parameter buffer, target and tiling state.
        let class = context.policy().priority();
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
        let cell = &mut self.queues.render.m5[usize::from(slot)];
        let mut lane = match cell.take() {
            Some(PairCell::Parked(lane)) => Some(lane),
            other => {
                *cell = other;
                return Err(EIO);
            }
        };
        let adopted = Pair::adopt(alloc, &mut lane, context, binding);
        // On failure the pair stays parked at its slot.
        *cell = lane.map(PairCell::Parked);
        let pair = match adopted {
            Ok(pair) => pair,
            Err(error) => {
                self.queues.buffers.release(pb.0)?;
                return Err(refused("adoption", error));
            }
        };
        if let Err(error) = pair.install_pb(&self.init) {
            // The lanes stay installed: parked at the slot again, never dropped.
            let (lane, _binding) = pair.park();
            self.queues.render.m5[usize::from(slot)] = Some(PairCell::Parked(lane));
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
        self.queues.render.m5[usize::from(slot)] = Some(PairCell::Built(pair));
        Ok(PairBind::Adopted)
    }

    /// Installs `built`, the pair of `reservation` built off the device mutex, and places it at
    /// the reserved slot, as `Registry::install_render` places a Neo pair. From its installation
    /// on the pair is retained whatever happens: an installed pair leaves `built`, whatever the
    /// result. Otherwise `built` keeps it and the reservation stands, for the caller to drop
    /// the pair off the device mutex and then cancel it ([`Self::cancel_m5_pair`]).
    fn install_m5_pair(
        &mut self,
        dev: &crate::driver::AsahiDrmDevice,
        reservation: PairReservation,
        built: &mut Option<KBox<Pair>>,
    ) -> Result {
        let slot = usize::from(reservation.slot);
        if !matches!(self.queues.render.m5.get(slot), Some(Some(PairCell::Reserved(held))) if *held == reservation)
        {
            return Err(EIO);
        }
        let pair = built.as_mut().ok_or(EINVAL)?;
        if pair.qids() != reservation.ids.map(crate::g17::kick::Id::qid)
            || pair.owner() != reservation.owner
        {
            return Err(EIO);
        }
        let result = pair.install(&mut Host {
            init: &self.init,
            port: &self.port,
            qids: &mut self.queues.qids,
            crashed: &self.primary.state.shared.crashed,
        });
        if pair.installed() {
            dev_info!(
                dev.as_ref(),
                "M5 render pair of queue {}: queues {:?}, slot {}\n",
                reservation.owner,
                pair.qids(),
                reservation.slot
            );
            self.queues.render.m5[slot] = built.take().map(PairCell::Built);
        }
        result.inspect_err(|error| {
            dev_warn!(
                dev.as_ref(),
                "M5 render pair of queue {}: installation failed: {:?}\n",
                reservation.owner,
                error
            )
        })
    }

    /// Returns what `reservation` holds when its pair was not installed: the slot, the two
    /// unpublished QIDs and the parameter-buffer ID, as `Registry::cancel_render_reservation`
    /// does for a Neo pair. Nothing when the slot no longer holds it (its pair was installed).
    fn cancel_m5_pair(&mut self, reservation: PairReservation) -> Result {
        let cell = self
            .queues
            .render
            .m5
            .get_mut(usize::from(reservation.slot))
            .ok_or(EINVAL)?;
        if !matches!(cell, Some(PairCell::Reserved(held)) if *held == reservation) {
            return Ok(());
        }
        *cell = None;
        let [tiling, fragment] = reservation.ids;
        self.queues.qids.cancel_unpublished(fragment)?;
        self.queues.qids.cancel_unpublished(tiling)?;
        self.queues.buffers.release(reservation.pb.0)
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
        if self.queues.m5_pairs().all(|pair| pair.replay().is_none()) {
            return Ok(());
        }
        if self.recovery.pending() || crate::g17::recovery::Memory::recovery_state(&self.init)? != 0
        {
            return Ok(());
        }
        let dev = self.primary.state.shared.drm()?;
        for slot in 0..self.queues.render.m5.len() {
            let Some(PairCell::Built(pair)) = self.queues.render.m5[slot].as_mut() else {
                continue;
            };
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
                    self.publish_m5_render(&dev, slot as u8, owner, &packet, &draw, None)
                }
                (None, _) => Err(EINVAL),
            };
            let Some(PairCell::Built(pair)) = self.queues.render.m5[slot].as_mut() else {
                return Err(EIO);
            };
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

    /// Publishes `packet`'s pass on `owner`'s pair at render slot `slot`, its pool grown first when
    /// the pass's plan needs more and `growth` allows it ([`Pair::grow`], the compute channel's
    /// rule; the old backing lands in its slot, for the caller to drop off the device mutex).
    /// Without `growth` a pass the pool is too small for is refused (EINVAL, `Pair::publish`).
    /// EAGAIN: the pair has no room yet, or is not idle for a growth.
    fn publish_m5_render(
        &mut self,
        dev: &crate::driver::AsahiDrmDevice,
        slot: u8,
        owner: u64,
        packet: &Arc<Packet>,
        draw: &Draw<'_>,
        growth: Option<M5Growth<'_, '_>>,
    ) -> Result {
        let work = packet.completion.work_state()?;
        let pair = slot_pair(&mut self.queues.render.m5, slot, owner)?.ok_or(EIO)?;
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
    /// Builds `reservation`'s pair off the device mutex, under the preparation `lease`, then
    /// installs it under the mutex ([`crate::g17::Firmware::install_m5_pair`]), as
    /// `install_fresh_pair` builds a Neo pair. The pair is mapped into this queue's VM but
    /// nothing is visible to the firmware until it is installed. A pair that is not installed
    /// (an error, a stale lease, this queue closed meanwhile) is dropped off the mutex, and then
    /// the reservation is cancelled. Not inlined: the build's frame stays off the scheduler's
    /// publication path when the pair exists.
    #[inline(never)]
    fn build_m5_pair(
        &self,
        lease: &crate::g17::preparation::Lease,
        reservation: PairReservation,
        resources: PairResources,
        gpu: &crate::g17::fw::m5::compute::Gpu,
        pass: &crate::g17::fw::render::RenderPass,
    ) -> Result {
        let dev = self.shared.drm()?;
        let variant = self.shared.variant;
        let alloc = Allocator {
            dev: &dev,
            uat: &resources.uat,
            layout: variant.vm_layout,
        };
        let refused = |step: &str, error: Error| {
            dev_warn!(
                dev.as_ref(),
                "M5 render pair of queue {}: {} failed: {:?}\n",
                self.owner,
                step,
                error
            );
            error
        };
        let built = (|| -> Result<KBox<Pair>> {
            let metrics = &resources.metrics;
            let view = metrics
                .alias_in(
                    self.context.vm(),
                    alloc.domain(crate::g17::vm_layout::Domain::Compact),
                    crate::mmu::UAT_PGSZ as u64,
                    crate::mmu::PROT_GPU_SHARED_RW,
                )
                .map_err(|error| refused("metrics view", error))?;
            let lease = crate::g17::buffer::MetricsLease::new(
                &resources.metrics_ids,
                metrics,
                crate::g17::fw::buffer::MetricsAddresses {
                    firmware: metrics.gpu_va(),
                    client: view.iova(),
                },
            )?;
            Pair::new(
                &alloc,
                self.context.clone(),
                PairArgs {
                    ids: reservation.ids,
                    gpu,
                    pointers: &variant.pointers,
                    binding: BindingArgs {
                        owner: self.owner,
                        metrics: lease,
                        metrics_view: view,
                        pb: reservation.pb,
                        pm_generation: reservation.pm_generation,
                        pass,
                        clusters: self.shared.clusters,
                        programs: variant.programs,
                    },
                },
            )
            .map_err(|error| refused("construction", error))
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
            // A queue closed meanwhile releases its slot (`Backend::release_queue`): its pair
            // is not installed.
            if self.closed.load(Ordering::Acquire) {
                return Err(ECANCELED);
            }
            firmware.install_m5_pair(&dev, reservation, &mut built)
        })();
        // A pair that was not installed goes before its reservation returns.
        drop(built);
        if result.is_err() {
            let mut state = self.shared.state.lock();
            (*state)
                .as_deref_mut()
                .ok_or(ENODEV)?
                .cancel_m5_pair(reservation)?;
        }
        result
    }

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
        let gpu = self.shared.m5.clone().ok_or(EINVAL)?;
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
        // fails it ([`Registry::fail_pair`]). The lease's slot is the pair's place: an owner
        // has one slot, and admission gives it one pass at a time (the pair's depth).
        let render_lease = self.admission.acquire_render(
            self.owner,
            crate::g17::queue::admission::Request::of(packet),
            deadline,
            &check,
        )?;
        let slot = render_lease.slot();
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
            let bound = match firmware.queues.m5_slot(slot, self.owner) {
                Ok(pair) => pair.is_some(),
                // A build at the slot is still in flight.
                Err(error) if error == EAGAIN => {
                    drop(state);
                    retry(lease)?;
                    continue;
                }
                Err(error) => return Err(error),
            };
            if !bound {
                let uat = firmware.uat.clone();
                let alloc = Allocator {
                    dev: &dev,
                    uat: &uat,
                    layout: firmware.variant.vm_layout,
                };
                match firmware.bind_m5_pair(
                    &alloc,
                    self.context.clone(),
                    slot,
                    self.owner,
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
                    Err(error) => return Err(error),
                    Ok(PairBind::Adopted) => {}
                    Ok(PairBind::Fresh(reservation, resources)) => {
                        drop(state);
                        match self.build_m5_pair(&lease, reservation, resources, &gpu, pass) {
                            // Its lease went stale, or the firmware had no room for it yet:
                            // nothing was installed, and the next turn builds it again.
                            Err(error) if error == EAGAIN => retry(lease)?,
                            result => result?,
                        }
                        continue;
                    }
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
                    let pair = firmware.queues.m5_slot(slot, self.owner)?.ok_or(EIO)?;
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
                .m5_slot(slot, self.owner)?
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
                slot,
                self.owner,
                packet,
                &draw,
                Some((&alloc, &*gpu, &mut grown)),
            );
            match result {
                Err(error) if error == EAGAIN && !packet.is_published() => {
                    drop(state);
                    drop(grown);
                    retry(lease)?;
                }
                // The pool's backing was refused, as for a compute command ([`Self::m5_packet`]).
                Err(error) if error == ENOMEM && !packet.is_published() => {
                    let releasing = firmware.queues.m5_pool_release_pending();
                    drop(state);
                    drop(grown);
                    if !self.reclaim_render_capacity()? && !releasing {
                        pr_warn!("G17 M5: render pass of queue {}: its pool's backing was refused and no idle pool can be given back\n", self.owner);
                        return Err(error);
                    }
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
