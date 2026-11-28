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

/// Whether an event pass must run full housekeeping now rather than deferring it to the
/// periodic cadence. Transient idle gaps between sparse submissions defer housekeeping until
/// `HOUSEKEEPING_NS` elapses (bounded by [`deferred_housekeeping_delay`]) so completions avoid
/// repeated report drains and uncached power-register reads.
fn housekeeping_due(
    requested: bool,
    recover: bool,
    recovery_pending: bool,
    teardown_pending: bool,
    has_waiters: bool,
    now: u64,
    last_housekeeping_ns: u64,
) -> bool {
    requested
        || recover
        || recovery_pending
        || teardown_pending
        || has_waiters
        || now.saturating_sub(last_housekeeping_ns) >= HOUSEKEEPING_NS
}

/// Remaining delay (ns) before deferred housekeeping must run when no work remains in flight.
/// Returns `None` while work stays in flight or when housekeeping ran on this pass.
fn deferred_housekeeping_delay(
    in_flight: bool,
    housekeeping_ran: bool,
    now: u64,
    last_housekeeping_ns: u64,
) -> Option<u64> {
    if in_flight || housekeeping_ran {
        return None;
    }
    Some(
        HOUSEKEEPING_NS
            .saturating_sub(now.saturating_sub(last_housekeeping_ns))
            .max(1),
    )
}

struct Service<'a> {
    firmware: &'a mut Firmware,
    deferred: &'a mut DeferredBatch,
    render_masks: [u64; 2],
    compute_scan: Option<super::ComputeScan>,
    settlement_error: Option<Error>,
}

impl event::Transport for Service<'_> {
    fn roles(&self) -> &'static [Role] {
        self.firmware.roles()
    }
    fn drain(
        &mut self,
        role: Role,
        mut consume: impl FnMut(&mut Self, Event) -> Result<bool>,
    ) -> Result {
        let mut events =
            channel::Events::new(role, self.firmware.primary.state.shared.variant.graph);
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
        for entry in self.firmware.queues.compute.iter_mut().flatten() {
            if let Some(mut queue) = entry.queue.as_deref_mut() {
                if queue.qid() == qid {
                    queue.note_fault(stamp);
                }
            }
        }
    }
    fn flush_tvb_reply(&mut self) -> Result<bool> {
        self.firmware.flush_tvb_reply()
    }
    fn channel_error(&mut self, engine: crate::g17::fw::channels::Engine, qid: u8, stamp: u32) -> Result {
        self.firmware.queue_channel_error(engine, qid, stamp)
    }
    fn flush_channel_errors(&mut self) -> Result<bool> {
        self.firmware.flush_channel_errors()
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
        let matched = self.firmware.queues.retain_render_completions(masks);
        self.render_masks[0] |= matched[0];
        self.render_masks[1] |= matched[1];
    }
    fn complete_compute(&mut self, masks: [u64; 2]) -> Result {
        // A batch without a queue identity still observes completions whose
        // notification was coalesced; identified batches retain QID filtering.
        let selected = (masks != [0; 2]).then_some(masks);
        self.compute_scan = Some(self.firmware.scan_compute_summary(selected, self.deferred)?);
        Ok(())
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
    fn is_m5(&self) -> bool {
        self.firmware.init.is_m5()
    }
    fn recovery_state(&self) -> Result<u32> {
        self.firmware.init.recovery_state()
    }
    fn host_recovery(&self) -> Result<u32> {
        self.firmware.init.host_recovery()
    }
    fn fault_sources(&self) -> Result<recovery::Sources> {
        let mut mmu_fault = false;
        if self.firmware.init.is_m5() {
            let registers = self.firmware.bridge.regs.m5_fault_registers();
            pr_err!("asahi: recovery fault registers: {:x?}\n", registers);
            // MMU fault status: valid (bit 0), reason (1..3, 0 = unmapped), read
            // (4), level (7..8), requestor (9..16), VM slot (17..22); the address
            // word holds the faulting address shifted right by 6.
            if let Ok([_, status, address, ..]) = registers {
                if status & 1 != 0 {
                    mmu_fault = true;
                    let access = if status & 0x10 != 0 { "read" } else { "write" };
                    pr_err!("asahi: recovery: GPU MMU fault: {} reason {} at {:#x}, level {}, requestor {:#x}, VM slot {}\n",
                        access, (status >> 1) & 7,
                        (address & ((1 << 42) - 1)) << 6, (status >> 7) & 3, (status >> 9) & 0xff,
                        (status >> 17) & 0x3f);
                }
            }
        }
        let mut sources = self.firmware.init.fault_sources()?;
        sources.settle_reason(mmu_fault, || self.firmware.port.fault_banks().ok());
        Ok(sources)
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

impl recovery::Queues for Service<'_> {
    fn render_pass_started(&self, qid: u8, stamp: u64) -> Option<bool> {
        self.firmware.queues.render_pass_started(qid, stamp)
    }
    fn render_culprit(&self, blamed: u8) -> Option<crate::g17::recovery::Attribution> {
        self.firmware.queues.render_culprit(blamed)
    }
    fn fault_culprit(&self, slots: u64) -> Option<u128> {
        self.firmware.queues.fault_culprit(slots)
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
        self.firmware.queues.render_oldest_spared(qid).or_else(|| {
            self.firmware
                .queues
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .find(|queue| queue.qid() == qid)
                .and_then(|queue| queue.oldest_spared())
        })
    }
    // Not on M5: it takes published work back through its recovery target, and its spared
    // work keeps the old collateral rule.
    fn collateral_before(&self, qid: u8, generation: u64) -> bool {
        !self.firmware.init.is_m5() && self.firmware.queues.render_collateral_before(qid, generation)
    }
    fn note_collateral(&mut self, qid: u8, generation: u64) {
        if !self.firmware.init.is_m5() {
            self.firmware.queues.note_render_collateral(qid, generation);
        }
    }
    fn marks_collateral(&self, qid: u8) -> bool {
        !self.firmware.init.is_m5() && self.firmware.queues.render_oldest_spared(qid).is_some()
    }
    fn visit_owners(&self, qid: u8, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
        self.firmware.queues.visit_render_owners(qid, visit);
        for queue in self
            .firmware
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
        {
            if queue.qid() == qid {
                queue.visit_owners(visit);
            }
        }
    }
    fn classify_work(&mut self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) -> u128 {
        for queue in self
            .firmware
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
        {
            queue.classify(class);
        }
        self.firmware.queues.classify_render(class)
    }
    fn note_interrupted(&mut self, qid: u8, stamp: u32) -> recovery::Interrupted {
        let render = self.firmware.queues.note_render_interrupted(qid, stamp);
        if render != recovery::Interrupted::None {
            return render;
        }
        self.firmware
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .find(|queue| queue.qid() == qid)
            .map_or(recovery::Interrupted::None, |queue| {
                queue.note_interrupted(stamp)
            })
    }
    fn note_kill_sightings(&self, generation: u64) {
        self.firmware.queues.note_render_kill_sightings(generation);
    }
    fn report_blamed_render(&self, qid: u8) {
        self.firmware
            .queues
            .report_render_recovery(qid, &self.firmware.port);
    }
    fn report_mmu_fault(&self) -> u64 {
        let banks = match self.firmware.port.fault_banks() {
            Ok(banks) => banks,
            Err(error) => {
                pr_warn!("G17: recovery MMU fault banks unreadable: {:?}\n", error);
                return 0;
            }
        };
        let mut slots = 0u64;
        for (bank, &(status, address)) in banks.iter().enumerate() {
            match crate::regs::decode_fault(status, address, true) {
                Some(info) => {
                    pr_warn!(
                        "G17: recovery MMU fault bank {}: address {:#x} {} {:?} unit {:?} ({:#x}) level {} vm_slot {} sideband {:#x} (status {:#x})\n",
                        bank, info.address, if info.read { "read" } else { "write" }, info.reason,
                        info.unit, info.unit_code, info.level, info.vm_slot, info.sideband, status
                    );
                    // Slot 0 is the firmware's own context, never a client's.
                    if info.vm_slot != 0 {
                        slots |= 1u64.checked_shl(info.vm_slot).unwrap_or(0);
                    }
                }
                None => pr_info!("G17: recovery MMU fault bank {}: no fault (status {:#x})\n", bank, status),
            }
        }
        slots
    }
    fn settle_blamed_render(&mut self, qid: u8, error: Error) {
        let result = self
            .firmware
            .settle_blamed_render(qid, error, self.deferred);
        self.settlement_error = self.settlement_error.or(result.err());
    }
    fn settle_cancelled_renders(&mut self, qids: u128) {
        let result = self.firmware.settle_cancelled_renders(qids, self.deferred);
        self.settlement_error = self.settlement_error.or(result.err());
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
            queue.release_witnessed(state, |owner| port.qos_complete(&[owner]), &mut |event| {
                deferred.push(event)
            })?;
        }
        Ok(())
    }

    /// Drains every role's trace ring, then their statistics rings. The event worker calls this
    /// after it has signalled the completions of its pass: the reports are not scheduling inputs.
    /// While work stays in flight it does so at the housekeeping cadence, and a recovery pass
    /// does so before the restart resets the report rings.
    fn drain_reports(&mut self) {
        let dev = &self.primary.state.shared.dev;
        for &role in self.roles() {
            self.rings.drain_trace(dev, &self.init, role);
        }
        // Each drain is bracketed by power observations; consecutive roles share the one
        // between them (each reads the GPU's power register).
        self.observe_statistics_power();
        for &role in self.roles() {
            self.rings.drain_statistics(&self.primary.state.shared.dev, &self.init, role);
            self.observe_statistics_power();
        }
    }

    /// Non-compute checks of [`Self::runtime_polling`], reused on the single-lock fast path.
    fn non_compute_runtime_polling(&self, now: u64) -> bool {
        self.render_control_backpressured()
            || self.init.outer_waiting()
            || self.queues.replay_outer_wait.is_some()
            || self.queues.replay_consumer_wait.is_some()
            || self.queues.render.replay_polling()
            || self.queues.teardown.polling(now)
            || self.compute_exit_polling(now)
    }

    /// A step of the event pass is waiting on the firmware with no event to wake it.
    fn runtime_polling(&self, now: u64) -> bool {
        self.non_compute_runtime_polling(now)
            || self
                .queues
                .compute
                .iter()
                .flatten()
                .filter_map(|entry| entry.queue.as_deref())
                .any(|queue| queue.retirement_polling() || queue.pool_release_pending())
    }

    /// Nothing is published that hasn't retired, nothing is waiting on the firmware, its rings
    /// are empty, and the idle check has taken the effort and power holds down.
    pub(in crate::g17) fn sleep_quiet(&self, now: u64) -> Result<bool> {
        use crate::g17::recovery::Memory;
        let compute = self
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.in_flight());
        if compute
            || self.queues.render_in_flight()
            || !self.queues.render_settled()
            || self.recovery.pending()
            || self.init.recovery_state()? != 0
            || self.runtime_polling(now)
            || self.hold.active()
            || self.registration_power_release_pending
            || self.effort.applied() != 0
            || self.effort.streaming()
        {
            return Ok(false);
        }
        let [consumer, unknown, producer] = self.init.control_counters()?;
        if consumer != producer || unknown != producer {
            return Ok(false);
        }
        for &role in self.roles() {
            if channel::Events::pending(&self.init, role)? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    pub(super) fn service_runtime_events(
        &mut self,
        deferred: &mut DeferredBatch,
    ) -> Result<(event::Pending, [u64; 2])> {
        let (pending, masks, _) = self.service_runtime_events_scan(deferred)?;
        Ok((pending, masks))
    }

    fn service_runtime_events_scan(
        &mut self,
        deferred: &mut DeferredBatch,
    ) -> Result<(event::Pending, [u64; 2], Option<super::ComputeScan>)> {
        let mut service = Service {
            firmware: self,
            deferred,
            render_masks: [0; 2],
            compute_scan: None,
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
        Ok((pending, service.render_masks, service.compute_scan))
    }

    /// Preparation admission is already closed and all builders have joined.
    pub(super) fn service_runtime_recovery(&mut self, deferred: &mut DeferredBatch) -> Result {
        self.rings.invalidate_statistics();
        let shared = self.primary.state.shared.clone();
        let mut service = Service {
            firmware: self,
            deferred,
            render_masks: [0; 2],
            compute_scan: None,
            settlement_error: None,
        };
        let result = recovery::service(&mut service, || {
            shared.crashed.load(core::sync::atomic::Ordering::Acquire)
        });
        // Publishers without the device mutex stamp their tickets with this
        // mirror; they rejoin only after preparation admission reopens.
        service
            .firmware
            .port
            .set_generation(service.firmware.recovery.generation());
        service.firmware.port.reset_m5_history();
        service.firmware.rings.invalidate_statistics();
        let result = result?;
        // Counted once the restart is classified, so the restart that reaches the limit has
        // still been serviced. One charged to its guilty clients costs them their devices and
        // nobody else anything; counted for the device, a program that faulted the GPU eight
        // times, opening it again each time, or that had queued eight faulting renders, stopped
        // it for every client until reboot.
        if !result.charged {
            if service.firmware.recovery.restart_limit(super::now_ns()) {
                pr_warn!("G17: restart storm limit reached\n");
                return Err(ETIMEDOUT);
            }
            if result.leftover && service.firmware.recovery.forgive_restart() {
                pr_warn!("G17: recovery dropped queued work of an already failed VM; not counted as a restart storm\n");
            }
        }
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
        // The way back queues a pass, which also handles a crash noticed meanwhile.
        if self.sleeping() {
            return;
        }
        if self.crashed.load(core::sync::atomic::Ordering::Acquire) {
            self.stop_runtime();
            return;
        }
        let Some(mut deferred) = self.worker_deferred.lock().take() else {
            self.lose_device_quietly();
            return;
        };
        crate::g17::trace::event(0);
        let result = self.runtime_event_turn(&mut deferred);
        deferred.finish();
        crate::g17::trace::event(2);
        self.queue_feed();
        *self.worker_deferred.lock() = Some(deferred);
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
        let (recover, masks, fast_done, render_changed, render_rescan) = {
            // Taken before `state` so single-lock render retirement observes a live preparation
            // epoch and drops it before `wait_drained_until()` if recovery opens.
            let fast_epoch = self.preparations.try_enter().ok();
            let mut state = self.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                return Ok(());
            };
            let (pending, masks, compute_scan) = firmware.service_runtime_events_scan(deferred)?;
            let recover = pending.recovery || firmware.recovery.pending();
            if recover {
                // Epoch closure and event consumption are serialized with every
                // publisher. Joining builders must happen after unlocking.
                self.preparations.close();
            }
            // Steady-state compute or render completions with no replays, no waiters and
            // housekeeping not yet due finish in this single device-mutex hold; fences are
            // signalled right after unlocking. Transient idle intervals defer housekeeping to the
            // cadence timer rather than draining reports on every completion.
            let mut fast_done = false;
            let mut render_changed = false;
            let mut rescan = false;
            let mut render_clean = false;
            if !recover
                && !self.crashed.load(core::sync::atomic::Ordering::Acquire)
            {
                let needs_render = masks != [0; 2] || firmware.render_observation_needed();
                render_clean = if !needs_render {
                    true
                } else if fast_epoch.as_ref().is_some_and(|epoch| epoch.is_current()) {
                    let before = deferred.pending();
                    match firmware.try_fast_observe_render(deferred)? {
                        super::render::observe::FastRenderOutcome::Done => {
                            render_changed = true;
                            true
                        }
                        super::render::observe::FastRenderOutcome::Idle => true,
                        // The full pass would find nothing more and arm the rescan poll.
                        super::render::observe::FastRenderOutcome::Rescan => {
                            render_changed = deferred.pending() != before;
                            rescan = true;
                            true
                        }
                        super::render::observe::FastRenderOutcome::Fallback => {
                            render_changed = deferred.pending() != before;
                            false
                        }
                    }
                } else {
                    false
                };
                // Render completions that found nothing to retire here (Idle) are owed to the
                // borrowers of their pairs, which observe them on return.
                if render_clean && (compute_scan.is_some() || render_changed || masks != [0; 2]) {
                    let scan = match compute_scan {
                        Some(scan) => scan,
                        None => {
                            let mut scan = super::ComputeScan::default();
                            for queue in firmware
                                .queues
                                .compute
                                .iter()
                                .flatten()
                                .filter_map(|entry| entry.queue.as_deref())
                            {
                                scan.in_flight |= queue.in_flight();
                                scan.replay |= queue.replay_commands() != 0;
                                scan.polling |=
                                    queue.retirement_polling() || queue.pool_release_pending();
                            }
                            scan
                        }
                    };
                    let now = super::now_ns();
                    let in_flight = scan.in_flight || firmware.queues.render_in_flight();
                    let no_replay = !scan.replay && !firmware.queues.render.replay_parked();
                    let last_hk = self
                        .housekeeping_ns
                        .load(core::sync::atomic::Ordering::Relaxed);
                    let has_waiters = self.channels.has_waiters()
                        || self.progress.has_waiters()
                        || self.queues.has_waiters()
                        || self.renders.has_waiters();
                    let no_housekeeping = !housekeeping_due(
                        requested,
                        false,
                        false,
                        !firmware.queues.teardown.is_empty(),
                        has_waiters,
                        now,
                        last_hk,
                    );
                    let no_outer_or_grow =
                        !firmware.init.outer_waiting() && !firmware.render_grow_ready();
                    if no_replay && no_housekeeping && no_outer_or_grow {
                        if let Some(delay) = firmware.idle_check(now, in_flight)? {
                            self.queue_idle(delay);
                        }
                        if let Some(delay) =
                            deferred_housekeeping_delay(in_flight, false, now, last_hk)
                        {
                            self.queue_housekeeping(delay);
                        }
                        let polling =
                            rescan || scan.polling || firmware.non_compute_runtime_polling(now);
                        if polling {
                            self.queue_poll();
                            if firmware.render_control_backpressured() {
                                self.queue_poll_after(1);
                            }
                        }
                        if channel::Events::pending(&firmware.init, Role::Primary)?
                            || (firmware.secondary.is_some()
                                && channel::Events::pending(&firmware.init, Role::Secondary)?)
                        {
                            self.queue_event_records();
                        }
                        fast_done = true;
                    }
                }
            }
            // Once try_fast_observe_render() has retired every ready render in this pass, do
            // not re-retain the same masks and re-enter observe_renders() on the slow path.
            let remaining_masks = if render_clean && !recover {
                [0; 2]
            } else {
                masks
            };
            (recover, remaining_masks, fast_done, render_changed, rescan)
        };
        if render_changed {
            self.note_changed();
        }
        deferred.finish();
        crate::g17::trace::event(1);
        if fast_done {
            return Ok(());
        }
        if recover {
            self.preparations
                .wait_drained_until(super::now_ns().saturating_add(super::WORK_RING_WAIT_NS))?;
        }
        let mut state = self.state.lock();
        let Some(firmware) = (*state).as_deref_mut() else {
            return Ok(());
        };
        if self.crashed.load(core::sync::atomic::Ordering::Acquire) {
            return Err(EIO);
        }
        // Capture pre-recovery trace/statistics reports before firmware restart resets the
        // report rings, matching the non-throttled post-signal ordering.
        let mut reports_drained = false;
        if recover {
            firmware.drain_reports();
            reports_drained = true;
            firmware.service_runtime_recovery(deferred)?;
            if !firmware.recovery.pending()
                && !self.sleep_gate.load(core::sync::atomic::Ordering::Acquire)
            {
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
        // Skip dropping and re-acquiring the device mutex around a no-op render observation.
        if masks != [0; 2] || firmware.render_observation_needed() {
            firmware.drain_render_deferred(masks, deferred)?;
            drop(state);
            self.observe_renders(deferred)?;
            state = self.state.lock();
            if self.crashed.load(core::sync::atomic::Ordering::Acquire) {
                return Err(EIO);
            }
            if let Some(firmware) = (*state).as_deref() {
                if !firmware.recovery.pending() && firmware.queues.render_needs_rescan() {
                    self.queue_poll();
                }
            }
        } else if render_rescan && !firmware.recovery.pending() {
            self.queue_poll();
        }
        let Some(firmware) = (*state).as_deref_mut() else {
            return Ok(());
        };
        let now = super::now_ns();
        let compute = firmware
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.in_flight());
        let in_flight = compute || firmware.queues.render_in_flight();
        let idle = firmware.idle_check(now, in_flight)?;
        // Expiries, drained pairs, exits, pool and heap trimming, teardown and M5 report
        // drains give back what retirement freed; a completion needs none of them to retire
        // or to signal. While nothing waits for capacity and `HOUSEKEEPING_NS` has not elapsed,
        // a pass for firmware records alone skips them; when no work stays in flight, a bounded
        // housekeeping timer guarantees cleanup runs once the interval expires.
        let last_hk = self
            .housekeeping_ns
            .load(core::sync::atomic::Ordering::Relaxed);
        let has_waiters = self.channels.has_waiters()
            || self.progress.has_waiters()
            || self.queues.has_waiters()
            || self.renders.has_waiters();
        let housekeeping = housekeeping_due(
            requested,
            recover,
            firmware.recovery.pending(),
            !firmware.queues.teardown.is_empty(),
            has_waiters,
            now,
            last_hk,
        );
        let mut lingers = None;
        let mut drains = None;
        let mut kills = None;
        let mut pools = None;
        let mut shrinking = false;
        if housekeeping {
            self.housekeeping_ns
                .store(now, core::sync::atomic::Ordering::Relaxed);
            if !reports_drained {
                // The completions above are signalled; reports are throttled to the
                // housekeeping cadence while work stays in flight.
                firmware.drain_reports();
            }
            if !firmware.recovery.pending() {
                lingers = firmware.expire_render_lingers(now)?;
            }
            drains = firmware.reclaim_drained_renders(deferred)?;
            firmware.service_compute_exit(deferred)?;
            kills = firmware.kill_retry_delay();
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
        // The firmware may consume M5 work-ring slots without a completion interrupt: while
        // a preparation waits for unclaimed room, every pass polls the consumers.
        let outer = firmware.init.poll_outer_progress()?;
        let polling = firmware.runtime_polling(now);
        if polling {
            self.queue_poll();
            if firmware.render_control_backpressured() {
                self.queue_poll_after(1);
            }
        }
        // A heap with growth to give back is re-evaluated at least once per
        // shrink period, also while the device is otherwise idle.
        let idle = if shrinking {
            Some(idle.map_or(SHRINK_WAKE_NS, |delay| delay.min(SHRINK_WAKE_NS)))
        } else {
            idle
        };
        // A retained compute pool gives its backing back when it expires, and
        // so does an idle render free-list registration. A drained render pair
        // whose settlement failed is retried when its backoff ends, and a closed
        // context's kill that waited for other work is looked at again.
        let idle = [pools, lingers, drains, kills]
            .into_iter()
            .fold(idle, |idle, expiry| match (idle, expiry) {
                (Some(delay), Some(expiry)) => Some(delay.min(expiry)),
                (delay, expiry) => delay.or(expiry),
            });
        if let Some(delay) = idle {
            self.queue_idle(delay);
        }
        if let Some(delay) = deferred_housekeeping_delay(in_flight, housekeeping, now, last_hk) {
            self.queue_housekeeping(delay);
        }
        if firmware.recovery.pending() {
            self.preparations.close();
            self.queue_events();
        } else if !recover
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
        // Outside the device mutex: the notification can run DMA-fence callbacks.
        if let Some(domain) = outer {
            domain.notify();
        }
        // Firmware ring room, consumed releases and teardown steps have no
        // event of their own: recheck render publishers' conditions every pass.
        if self.renders.has_waiters() {
            self.renders.notify();
        }
        Ok(())
    }
}
