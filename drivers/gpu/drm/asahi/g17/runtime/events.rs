// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Event and restart callbacks borrow the complete firmware owner. The reusable
//! completion collector belongs to the worker and is drained outside its mutex.

use super::DeferredBatch;
use crate::g17::{
    channel, event,
    fw::channels::{Event, FreeListCompletion, TvbGrowRequest},
    recovery,
    status::VmStatus,
    Firmware, Role,
};
use kernel::{prelude::*, sync::Arc};

/// Wake interval while a tiler heap may still shrink: the native check period.
const SHRINK_WAKE_NS: u64 = 1_000_000_000;
/// Longest interval between two event passes that run every housekeeping step while
/// completion records alone keep the worker busy.
const HOUSEKEEPING_NS: u64 = 1_000_000;

struct Service<'a> {
    firmware: &'a mut Firmware,
    deferred: &'a mut DeferredBatch,
    render_masks: [u64; 2],
    settlement_error: Option<Error>,
}

impl event::Transport for Service<'_> {
    fn roles(&self) -> &'static [Role] {
        self.firmware.roles()
    }
    fn trace(&mut self, role: Role) {
        let firmware = &mut *self.firmware;
        firmware
            .rings
            .drain_trace(&firmware.primary.state.shared.dev, &firmware.init, role);
    }
    fn statistics(&mut self, role: Role) {
        let firmware = &mut *self.firmware;
        firmware
            .rings
            .drain_statistics(&firmware.primary.state.shared.dev, &firmware.init, role);
    }
    fn drain(
        &mut self,
        role: Role,
        mut consume: impl FnMut(&mut Self, Event) -> Result<bool>,
    ) -> Result {
        let mut events = channel::Events::new(role, self.firmware.primary.state.shared.variant.graph);
        while let Some(event) =
            events.next(&self.firmware.primary.state.shared.dev, &self.firmware.init)?
        {
            if !consume(self, event)? {
                break;
            }
        }
        Ok(())
    }
    fn recovery_state(&self) -> Result<u32> {
        recovery::Memory::recovery_state(&self.firmware.init)
    }
}

impl event::Host for Service<'_> {
    fn recovery(&self) -> &recovery::State {
        &self.firmware.recovery
    }
    fn recovery_mut(&mut self) -> &mut recovery::State {
        &mut self.firmware.recovery
    }
}

impl event::Queues for Service<'_> {
    fn context_killed(&mut self, cookie: u64) {
        self.firmware.queues.acknowledge_compute_exit(cookie);
    }
    fn compute_fault(&mut self, qid: u8, stamp: u32) {
        let mut lane = None;
        for entry in self.firmware.queues.compute.iter_mut().flatten() {
            entry.with_queue(|queue| {
                if queue.qid() == qid {
                    queue.note_fault(stamp);
                    lane = lane.or(queue.is_m5().then(|| queue.kicks()));
                }
            });
        }
        let window = self.firmware.bridge.regs.m5_fault_registers();
        // The M5 Ultra's lane (none elsewhere) logs the raw fault window (`regs.rs`
        // `m5_fault_registers`: 0xd800, the fault status 0xd8c0 and address 0xd8c8, 0xc078,
        // 0xc008, 0xc028): nothing else says what the GPU faulted on.
        if let Some(kicks) = lane {
            pr_warn!("G17 M5: compute fault on queue {} (stamp {:#x}, {} kicks published): fault window {:x?}\n",
                qid, stamp, kicks, window);
        }
    }
    fn flush_tvb_reply(&mut self) -> Result<bool> {
        self.firmware.flush_tvb_reply()
    }
    fn flush_freelist_release(&mut self) -> Result<bool> {
        self.firmware.flush_freelist_release()
    }
    fn observe_render_releases(&mut self) -> Result {
        self.firmware.observe_render_releases()
    }
    fn control_backpressured(&self) -> bool {
        self.firmware.render_control_backpressured()
    }
    fn queue_tvb_grow(&mut self, request: TvbGrowRequest) -> Result {
        self.firmware.queue_tvb_grow(request)
    }
    fn complete_freelist_grow(&mut self, completion: FreeListCompletion) -> Result {
        self.firmware.complete_freelist_grow(completion)
    }
    fn retain_render_completions(&mut self, masks: [u64; 2]) {
        // Inline timeout and drain callers may discard the worker hints. The
        // queue owner retains stamps until its borrowed pair returns to scan.
        self.firmware.queues.retain_render_completions(masks);
        self.render_masks[0] |= masks[0];
        self.render_masks[1] |= masks[1];
    }
    fn complete_compute(&mut self, masks: [u64; 2]) -> Result {
        // A batch without a queue identity still observes completions whose
        // notification was coalesced; identified batches retain QID filtering.
        let selected = (masks != [0; 2]).then_some(masks);
        self.firmware.scan_compute(selected, self.deferred)
    }
}

impl recovery::Host for Service<'_> {
    fn recovery(&self) -> &recovery::State {
        &self.firmware.recovery
    }
    fn recovery_mut(&mut self) -> &mut recovery::State {
        &mut self.firmware.recovery
    }
}

impl recovery::Memory for Service<'_> {
    fn recovery_state(&self) -> Result<u32> {
        self.firmware.init.recovery_state()
    }
    fn host_recovery(&self) -> Result<u32> {
        self.firmware.init.host_recovery()
    }
    fn fault_sources(&self) -> Result<recovery::Sources> {
        // An M5 firmware restart is the one report a lost dispatch makes; the raw fault window
        // (`m5_fault_registers`) is what says what the GPU faulted on.
        if self.firmware.variant.submission == crate::g17::fw::variant::Submission::M5HostCommand {
            pr_warn!(
                "G17 M5: firmware restart: fault window {:x?}\n",
                self.firmware.bridge.regs.m5_fault_registers()
            );
        }
        self.firmware.init.fault_sources()
    }

    fn transition(&self, expected: u32, next: u32) -> Result {
        self.firmware.init.transition(expected, next)
    }
    fn clear_timestamps(&self) -> Result {
        self.firmware.init.clear_timestamps()
    }
    fn entry(&self, index: usize) -> Result<(u32, u32)> {
        self.firmware.init.entry(index)
    }
    fn wait_tick(&self) {
        self.firmware.init.wait_tick();
    }
}

/// One queue's retained work as recovery attributes it ([`recovery::Queues`]). Every kind of
/// queue that holds firmware-visible work implements it, and [`Registry::visit_retained`] walks
/// them all, so attribution and the interrupted marks cannot miss a kind. (M5's lanes once did:
/// a pass the firmware resumed after a restart retired as a success.)
pub(super) trait Retained {
    fn owns(&self, qid: u8) -> bool;
    /// Whether its oldest work in flight is spared; None without any.
    fn oldest_spared(&self) -> Option<bool>;
    fn note_interrupted(&self, stamp: u32) -> recovery::Interrupted;
    fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>));
    fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class);
    /// A render pair's QIDs when its oldest work is spared, for `settle_cancelled_renders`.
    fn spared_render(&self) -> u128 {
        0
    }
    /// Recovery found the firmware's blame of this queue's render QID guilty
    /// (`recovery::Queues::settle_blamed_render`). Neo's pairs settle through
    /// `Firmware::settle_blamed_render`, which needs their render slot.
    fn settle_blamed_render(&mut self, _error: Error, _deferred: &mut DeferredBatch) -> Result {
        Ok(())
    }
    /// The same for a compute QID (`recovery::Queues::settle_blamed_compute`). Neo's compute
    /// queues settle through the firmware's compute-fault event.
    fn settle_blamed_compute(&mut self, _error: Error, _deferred: &mut DeferredBatch) -> Result {
        Ok(())
    }
    /// The recovery table cancelled render QIDs `qids`
    /// (`recovery::Queues::settle_cancelled_renders`). Neo's pairs settle through
    /// `Firmware::settle_cancelled_renders`.
    fn settle_cancelled_render(&mut self, _qids: u128, _deferred: &mut DeferredBatch) -> Result {
        Ok(())
    }
    /// Notes its work in flight and whether it started, at the halted instant
    /// (`recovery::Queues::halted_progress`). Neo's queues are attributed by the fault sources.
    fn note_progress(&self, _progress: &mut recovery::Progress) {}
    /// Holds work the halted instant proved never started (`recovery::Queues::hold_unstarted`).
    fn hold_unstarted(&mut self, _progress: &recovery::Progress, _listed: u128) {}
    /// What kind of queue it is, for a recovery record's log line
    /// (`recovery::Queues::report_record`).
    fn kind(&self) -> &'static str;
}

fn render_qids(qids: [u8; 2], spared: Option<bool>) -> u128 {
    match spared {
        Some(true) => qids.into_iter().fold(0, |mask, qid| mask | 1u128 << qid),
        _ => 0,
    }
}

impl Retained for crate::g17::queue::render::Pair {
    fn owns(&self, qid: u8) -> bool { self.qids().contains(&qid) }
    fn oldest_spared(&self) -> Option<bool> { self.oldest_spared() }
    fn note_interrupted(&self, stamp: u32) -> recovery::Interrupted { self.note_interrupted(stamp) }
    fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) { self.visit_owners(visit) }
    fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) { self.classify(class) }
    fn spared_render(&self) -> u128 { render_qids(self.qids(), self.oldest_spared()) }
    fn kind(&self) -> &'static str { "render pair" }
}

impl Retained for crate::g17::queue::compute::Queue {
    /// The M5 Ultra's lane settles a guilty blame itself (`Queue::settle_blamed_m5`); Neo's compute
    /// queues settle through the firmware's compute-fault event.
    fn settle_blamed_compute(&mut self, error: Error, deferred: &mut DeferredBatch) -> Result {
        if self.is_m5() {
            return self.settle_blamed_m5(error, &mut |event| deferred.push(event));
        }
        Ok(())
    }
    fn owns(&self, qid: u8) -> bool { self.qid() == qid }
    fn oldest_spared(&self) -> Option<bool> { self.oldest_spared() }
    fn note_interrupted(&self, stamp: u32) -> recovery::Interrupted { self.note_interrupted(stamp) }
    fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) { self.visit_owners(visit) }
    fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) { self.classify(class) }
    /// The M5 Ultra's lane notes its kicks' start times (`Queue::note_progress` in `queue::m5`).
    fn note_progress(&self, progress: &mut recovery::Progress) {
        if self.is_m5() {
            self.note_progress(progress);
        }
    }
    fn kind(&self) -> &'static str {
        if self.is_m5() { "compute lane" } else { "compute queue" }
    }
}

impl Retained for crate::g17::queue::m5_render::Pair {
    fn settle_blamed_render(&mut self, error: Error, deferred: &mut DeferredBatch) -> Result {
        self.settle_blamed(error, deferred)
    }
    fn settle_cancelled_render(&mut self, qids: u128, deferred: &mut DeferredBatch) -> Result {
        self.settle_cancelled(qids, deferred)
    }
    fn owns(&self, qid: u8) -> bool { self.qids().contains(&qid) }
    fn oldest_spared(&self) -> Option<bool> { self.oldest_spared() }
    fn note_interrupted(&self, stamp: u32) -> recovery::Interrupted { self.note_interrupted(stamp) }
    fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) { self.visit_owners(visit) }
    fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) { self.classify(class) }
    fn spared_render(&self) -> u128 { render_qids(self.qids(), self.oldest_spared()) }
    fn note_progress(&self, progress: &mut recovery::Progress) { self.note_progress(progress) }
    fn hold_unstarted(&mut self, progress: &recovery::Progress, listed: u128) {
        self.hold_unstarted(progress, listed)
    }
    fn kind(&self) -> &'static str { "render pair" }
}

impl super::Registry {
    /// Every queue holding firmware-visible work: Neo render pairs and compute queues, M5
    /// compute channels and render pairs (bound ones; a parked lane holds none).
    pub(super) fn visit_retained(&self, visit: &mut dyn FnMut(&dyn Retained)) {
        for pair in self.render_pairs() {
            visit(pair);
        }
        for queue in self.compute.iter().flatten().filter_map(|entry| entry.queue.as_deref()) {
            visit(&*queue);
        }
        for channel in super::m5::channels(&self.compute) {
            visit(&*channel);
        }
        for pair in super::m5::pairs(&self.render.m5) {
            visit(pair);
        }
    }

    /// Settles every queue [`Self::visit_retained`] walks, in the same order, through `settle`.
    /// A failed queue does not skip the ones after it
    /// (`recovery::Queues::publish_recovery_targets`): the first error is returned once all were
    /// visited.
    pub(super) fn settle_retained(
        &mut self,
        settle: &mut dyn FnMut(&mut dyn Retained) -> Result,
    ) -> Result {
        let mut failure = None;
        for pair in self.render_pairs_mut() {
            failure = failure.or(settle(pair).err());
        }
        for mut queue in self.compute.iter_mut().flatten().filter_map(|entry| entry.queue.as_deref_mut()) {
            failure = failure.or(settle(&mut *queue).err());
        }
        for mut channel in super::m5::channels_mut(&mut self.compute) {
            failure = failure.or(settle(&mut *channel).err());
        }
        for pair in super::m5::pairs_mut(&mut self.render.m5) {
            failure = failure.or(settle(pair).err());
        }
        failure.map_or(Ok(()), Err)
    }
}

impl recovery::Queues for Service<'_> {
    fn render_pass_started(&self, qid: u8, stamp: u64) -> Option<bool> {
        self.firmware.queues.render_pass_started(qid, stamp)
    }
    fn halted_progress(&self) -> Option<recovery::Progress> {
        // M5 only: its restarts report no reason, so the fault sources cannot attribute.
        if self.firmware.variant.submission != crate::g17::fw::variant::Submission::M5HostCommand {
            return None;
        }
        let mut progress = recovery::Progress::default();
        self.firmware.queues.visit_retained(&mut |queue| queue.note_progress(&mut progress));
        Some(progress)
    }
    fn hold_unstarted(&mut self, progress: &recovery::Progress, listed: u128) {
        // M5 only: `halted_progress` read the evidence there alone.
        if self.firmware.variant.submission != crate::g17::fw::variant::Submission::M5HostCommand {
            return;
        }
        let _ = self.firmware.queues.settle_retained(&mut |queue| {
            queue.hold_unstarted(progress, listed);
            Ok(())
        });
    }
    fn publish_recovery_targets(&self) -> Option<Error> {
        let mut failure = self.firmware.queues.publish_render_recovery_targets();
        for queue in self
            .firmware
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
        {
            if let Err(error) = queue.publish_recovery_target() {
                failure = failure.or(Some(error));
            }
        }
        core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
        failure
    }
    fn render_scheduler_active(&self) -> bool {
        self.firmware.queues.render_scheduler_active() || !self.firmware.queues.teardown.is_empty()
    }
    fn oldest_spared(&self, qid: u8) -> Option<bool> {
        let mut spared = None;
        self.firmware.queues.visit_retained(&mut |queue| {
            if spared.is_none() && queue.owns(qid) {
                spared = queue.oldest_spared();
            }
        });
        spared
    }
    fn visit_owners(&self, qid: u8, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
        self.firmware.queues.visit_retained(&mut |queue| {
            if queue.owns(qid) {
                queue.visit_owners(visit);
            }
        });
    }
    fn classify_work(&mut self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) -> u128 {
        let mut spared = 0;
        self.firmware.queues.visit_retained(&mut |queue| {
            queue.classify(class);
            spared |= queue.spared_render();
        });
        spared
    }
    fn note_interrupted(&mut self, qid: u8, stamp: u32) -> recovery::Interrupted {
        let mut interrupted = recovery::Interrupted::None;
        self.firmware.queues.visit_retained(&mut |queue| {
            if interrupted == recovery::Interrupted::None && queue.owns(qid) {
                interrupted = queue.note_interrupted(stamp);
            }
        });
        interrupted
    }
    fn report_record(&self, index: usize, flags: u32, stamp: u32) {
        // M5 only: each record says which of its lanes and pairs the restart caught in flight.
        if self.firmware.variant.submission != crate::g17::fw::variant::Submission::M5HostCommand {
            return;
        }
        let qid = ((flags >> 4) & 0x7f) as u8;
        let mut kind = None;
        self.firmware.queues.visit_retained(&mut |queue| {
            if kind.is_none() && queue.owns(qid) {
                kind = Some(queue.kind());
            }
        });
        let kind = kind.unwrap_or("no M5 queue");
        let guilty = match flags & 2 { 0 => "", _ => " guilty" };
        let cancelled = match flags & 4 { 0 => "", _ => " cancelled" };
        let now = crate::g17::qos::clock().0;
        pr_info!("G17 M5: recovery record {}: queue {} ({}), stamp {:#x}, flags {:#x}{}{}, now {:#x}\n",
            index, qid, kind, stamp, flags, guilty, cancelled, now);
    }
    fn report_blamed_render(&self, qid: u8) {
        self.firmware.queues.report_render_recovery(qid, &self.firmware.port);
    }
    fn settle_blamed_render(&mut self, qid: u8, error: Error) {
        // Disjoint QIDs: each settlement runs whatever the other's result; the first error stays.
        let neo = self.firmware.settle_blamed_render(qid, error, self.deferred);
        let deferred = &mut *self.deferred;
        let rest = self.firmware.queues.settle_retained(&mut |queue| match queue.owns(qid) {
            true => queue.settle_blamed_render(error, deferred),
            false => Ok(()),
        });
        self.settlement_error = self.settlement_error.or(neo.err()).or(rest.err());
    }
    fn settle_blamed_compute(&mut self, qid: u8, error: Error) {
        let deferred = &mut *self.deferred;
        let result = self.firmware.queues.settle_retained(&mut |queue| match queue.owns(qid) {
            true => queue.settle_blamed_compute(error, deferred),
            false => Ok(()),
        });
        self.settlement_error = self.settlement_error.or(result.err());
    }
    fn settle_cancelled_renders(&mut self, qids: u128) {
        let neo = self.firmware.settle_cancelled_renders(qids, self.deferred);
        let deferred = &mut *self.deferred;
        let rest = self.firmware.queues
            .settle_retained(&mut |queue| queue.settle_cancelled_render(qids, deferred));
        self.settlement_error = self.settlement_error.or(neo.err()).or(rest.err());
    }
    fn rescan_compute(&mut self) -> Result {
        self.firmware.scan_compute(None, self.deferred)
    }
    fn release_retired_compute(&mut self) {
        let result = self.firmware.release_compute_witnesses(self.deferred);
        self.settlement_error = self.settlement_error.or(result.err());
    }
}

impl Firmware {
    fn release_compute_witnesses(&mut self, deferred: &mut DeferredBatch) -> Result {
        let state = recovery::Memory::recovery_state(&self.init)?;
        let port = &self.port;
        let registry = &mut self.queues;
        for mut queue in registry
            .compute
            .iter_mut()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref_mut())
        {
            queue.release_witnessed(
                state,
                |owner| port.qos_complete(&[owner]),
                &mut |event| deferred.push(event),
            )?;
        }
        Ok(())
    }

    pub(super) fn service_runtime_events(
        &mut self,
        deferred: &mut DeferredBatch,
    ) -> Result<(event::Pending, [u64; 2])> {
        let mut service = Service {
            firmware: self,
            deferred,
            render_masks: [0; 2],
            settlement_error: None,
        };
        let pending = event::service(&mut service)?;
        if pending.recovery {
            // Inline timeout/drain service can consume the restart record too.
            // Close publication in this same critical section and ensure the
            // event worker will join builders before the restart handshake.
            let shared = &service.firmware.primary.state.shared;
            shared.preparations.close();
            shared.queue_events();
        }
        Ok((pending, service.render_masks))
    }

    /// Preparation admission is already closed and all builders have joined.
    pub(super) fn service_runtime_recovery(&mut self, deferred: &mut DeferredBatch) -> Result {
        if self.recovery.restart_limit(super::now_ns()) {
            pr_warn!("G17: restart storm limit reached\n");
            return Err(ETIMEDOUT);
        }
        let shared = self.primary.state.shared.clone();
        let mut service = Service {
            firmware: self,
            deferred,
            render_masks: [0; 2],
            settlement_error: None,
        };
        let result = recovery::service(&mut service, || {
            shared.crashed.load(core::sync::atomic::Ordering::Acquire)
        });
        // Publishers without the device mutex stamp their tickets with this
        // mirror; they rejoin only after preparation admission reopens.
        service.firmware.port.set_generation(service.firmware.recovery.generation());
        let result = result?;
        if let Some(error) = result.target_error {
            dev_warn!(
                shared.dev.as_ref(),
                "Recovery target copy failed: {:?}\n",
                error
            );
        }
        if let Some(error) = result.scan_error {
            dev_warn!(
                shared.dev.as_ref(),
                "Post-recovery compute scan failed: {:?}\n",
                error
            );
        }
        if let Some(error) = service.settlement_error {
            return Err(error);
        }
        Ok(())
    }
}

impl crate::g17::Shared {
    /// The single event work item owns this preallocated collector until all
    /// callbacks are finished. Shutdown can detach Firmware during an off-lock
    /// boundary; the collector itself still owns every deferred completion.
    pub(in crate::g17) fn run_runtime_events(self: &Arc<Self>) {
        if self.crashed.load(core::sync::atomic::Ordering::Acquire) {
            self.stop_runtime();
            return;
        }
        let mut deferred = {
            let mut state = self.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                return;
            };
            let Some(deferred) = firmware.queues.worker_deferred.take() else {
                drop(state);
                self.lose_device_quietly();
                return;
            };
            deferred
        };
        crate::g17::trace::event(0);
        let result = self.runtime_event_turn(&mut deferred);
        deferred.finish();
        crate::g17::trace::event(2);
        self.queue_feed();
        if let Some(firmware) = (*self.state.lock()).as_deref_mut() {
            firmware.queues.worker_deferred = Some(deferred);
        }
        if let Err(error) = result {
            dev_err!(self.dev.as_ref(), "GPU event service failed: {:?}\n", error);
            self.lose_device_quietly();
            self.stop_runtime();
        }
    }

    fn runtime_event_turn(self: &Arc<Self>, deferred: &mut DeferredBatch) -> Result {
        // Taken before consuming records: a request made during this pass gets the next one.
        let requested = self
            .housekeeping
            .swap(false, core::sync::atomic::Ordering::AcqRel);
        let (recover, masks) = {
            let mut state = self.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                return Ok(());
            };
            let (pending, masks) = firmware.service_runtime_events(deferred)?;
            let recover = pending.recovery || firmware.recovery.pending();
            if recover {
                // Epoch closure and event consumption are serialized with every
                // publisher. Joining builders must happen after unlocking.
                self.preparations.close();
            }
            (recover, masks)
        };
        deferred.finish();
        crate::g17::trace::event(1);
        if recover {
            self.preparations.wait_drained_until(super::now_ns().saturating_add(super::WORK_RING_WAIT_NS))?;
        }
        let mut state = self.state.lock();
        let Some(firmware) = (*state).as_deref_mut() else {
            return Ok(());
        };
        if self.crashed.load(core::sync::atomic::Ordering::Acquire) {
            return Err(EIO);
        }
        if recover {
            firmware.service_runtime_recovery(deferred)?;
            if !firmware.recovery.pending() {
                if !self.preparations.reopen() {
                    return Ok(());
                }
                // Restart stopped consumption before its following record.
                // Resume builders before observing pairs outside this lock.
                self.queue_events();
            }
        }
        // Replay only at this outer boundary, after recovery has settled.
        // Publication may service events but must never recurse into replay.
        firmware.replay_compute(deferred)?;
        firmware.replay_render(deferred)?;
        firmware.replay_m5_render(deferred)?;
        drop(state);
        self.observe_renders(masks, deferred)?;
        let mut state = self.state.lock();
        let Some(firmware) = (*state).as_deref_mut() else {
            return Ok(());
        };
        if self.crashed.load(core::sync::atomic::Ordering::Acquire) {
            return Err(EIO);
        }
        let now = super::now_ns();
        // Each entry's queue, Neo's or the M5 Ultra's lane, then the M5 render pairs.
        let compute = firmware
            .queues
            .compute
            .iter()
            .flatten()
            .any(|entry| entry.inspect_queue(|queue| queue.in_flight()).unwrap_or(false))
            || firmware.queues.m5_in_flight();
        let in_flight = compute || firmware.queues.render_in_flight();
        let idle = firmware.idle_check(now, in_flight)?;
        // Expiries, drained pairs, exits, pool and heap trimming and teardown give back what
        // retirement freed; a completion needs none of them to retire or to signal. While work
        // is in flight and nothing waits for capacity, a pass for firmware records alone skips
        // them. Any requested pass, recovery, teardown, a waiter, the last retirement and, at
        // the latest, the first pass after HOUSEKEEPING_NS run them all.
        let housekeeping = requested
            || recover
            || !in_flight
            || firmware.recovery.pending()
            || !firmware.queues.teardown.is_empty()
            || self.channels.has_waiters()
            || self.progress.has_waiters()
            || self.queues.has_waiters()
            || self.renders.has_waiters()
            || now.saturating_sub(self.housekeeping_ns.load(core::sync::atomic::Ordering::Relaxed))
                >= HOUSEKEEPING_NS;
        let mut lingers = None;
        let mut pools = None;
        let mut shrinking = false;
        if housekeeping {
            self.housekeeping_ns
                .store(now, core::sync::atomic::Ordering::Relaxed);
            if !firmware.recovery.pending() {
                lingers = firmware.expire_render_lingers(now)?;
            }
            firmware.reclaim_drained_renders(deferred)?;
            firmware.service_compute_exit(deferred)?;
            pools = firmware.service_compute_pools(deferred, now)?;
            if !firmware.recovery.pending() {
                firmware
                    .queues
                    .vacate_evicted_render_pools(&firmware.init, deferred)?;
                shrinking = firmware
                    .queues
                    .shrink_render_heaps(&firmware.init, now, deferred);
            }
            let mut reclaim = self.reclaim.lock();
            firmware.service_teardown(&mut reclaim)?;
            if !reclaim.is_empty() {
                self.queue_reclaims();
            }
        }
        let backpressured = firmware.render_control_backpressured();
        let polling = backpressured
            || firmware.queues.replay_outer_wait.is_some()
            || firmware.queues.render.replay_polling()
            || firmware.queues.teardown.polling(now)
            || firmware.compute_exit_polling(now)
            || firmware.queues.m5_polling()
            || firmware
                .queues
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .any(|queue| queue.retirement_polling() || queue.pool_release_pending());
        if polling {
            self.queue_poll();
        }
        // A heap with growth to give back is re-evaluated at least once per
        // shrink period, also while the device is otherwise idle.
        let idle = if shrinking {
            Some(idle.map_or(SHRINK_WAKE_NS, |delay| delay.min(SHRINK_WAKE_NS)))
        } else {
            idle
        };
        // A retained compute pool gives its backing back when it expires, and
        // so does an idle render free-list registration.
        let idle = [pools, lingers]
            .into_iter()
            .fold(idle, |idle, expiry| match (idle, expiry) {
                (Some(delay), Some(expiry)) => Some(delay.min(expiry)),
                (delay, expiry) => delay.or(expiry),
            });
        if let Some(delay) = idle {
            self.queue_idle(delay);
        }
        if firmware.recovery.pending() {
            self.preparations.close();
            self.queue_events();
        } else if !recover
            && !polling
            && (channel::Events::pending(&firmware.init, Role::Primary)?
                || (firmware.secondary.is_some()
                    && channel::Events::pending(&firmware.init, Role::Secondary)?))
        {
            // A full bounded pass must not depend on a fresh notification for
            // records that arrived while it was consuming the same ring.
            self.queue_event_records();
        }
        let grow_ready = firmware.render_grow_ready();
        drop(state);
        if grow_ready {
            self.queue_grow();
        }
        // Firmware ring room, consumed releases and teardown steps have no
        // event of their own: recheck render publishers' conditions every pass.
        if self.renders.has_waiters() {
            self.renders.notify();
        }
        Ok(())
    }
}
