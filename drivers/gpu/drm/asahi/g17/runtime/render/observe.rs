// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Event-driven render snapshots. A pair's completion memory is read and the
//! pair retired in one device-mutex hold, inside the preparation epoch.

use super::super::DeferredBatch;
use core::sync::atomic::Ordering;
use kernel::{prelude::*, sync::Arc};

/// Outcome of attempting single-lock steady-state render completion in `runtime_event_turn()`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::g17::runtime) enum FastRenderOutcome {
    /// Non-steady state or additional render work requires the full `observe_renders()` path.
    Fallback,
    /// Healthy render state with no ready observation to retire.
    Idle,
    /// Retired the single ready render observation cleanly with no further render work pending.
    Done,
    /// Retired every ready observation in steady state, while published work whose completion
    /// was noted stays in flight (`render_needs_rescan`): its later completions are observed by
    /// their own events or by the rescan poll, which is all the full `observe_renders()` pass
    /// would add.
    Rescan,
}

impl crate::g17::Firmware {
    /// Whether `observe_renders` has any deferred settlement or in-flight render observation
    /// pending. Pure compute passes can skip dropping and re-acquiring the device mutex when false.
    pub(in crate::g17) fn render_observation_needed(&self) -> bool {
        for slot in 1..self.queues.render.entries.len() {
            let Some(entry) = self.queues.render.entries[slot].as_ref() else {
                continue;
            };
            let Some(pair) = entry.pair.as_ref() else {
                continue;
            };
            if entry.pending_error.is_some() || entry.pending_timeout || entry.closed {
                return true;
            }
            if !self.recovery.pending()
                && entry.preparing.is_none()
                && !entry.growing
                && pair.published_in_flight()
                && (crate::g17::queue::render::completion_noted(pair.qids(), entry.event_masks)
                    || pair.stamp_waiting())
            {
                return true;
            }
        }
        false
    }

    /// The first slot from `start` whose pair has news to observe: published work in flight, the
    /// fragment queue's notification seen (`completion_noted`), and an observation the pair accepts.
    fn next_render_observation(&mut self, start: usize) -> Result<Option<usize>> {
        let end = self.queues.render.entries.len();
        self.next_render_observation_in(start, end)
    }

    /// [`Self::next_render_observation`] among the slots `start..end`.
    fn next_render_observation_in(&mut self, start: usize, end: usize) -> Result<Option<usize>> {
        if self.recovery.pending() {
            return Ok(None);
        }
        let state = crate::g17::recovery::Memory::recovery_state(&self.init)?;
        for slot in start..end.min(self.queues.render.entries.len()) {
            let Some(entry) = self.queues.render.entries[slot].as_ref() else {
                continue;
            };
            if entry.preparing.is_some() || entry.growing {
                continue;
            }
            let Some(pair) = entry.pair.as_ref() else {
                continue;
            };
            let qids = pair.qids();
            if !pair.published_in_flight()
                || (!crate::g17::queue::render::completion_noted(qids, entry.event_masks)
                    && !pair.stamp_waiting())
            {
                continue;
            }
            let stamps = [
                self.init.stamp(qids[0])?.load(Ordering::Relaxed),
                self.init.stamp(qids[1])?.load(Ordering::Relaxed),
            ];
            core::sync::atomic::fence(Ordering::Acquire);
            if pair.observation_packet(entry.event_masks, stamps, state)?.is_some() {
                return Ok(Some(slot));
            }
            // The stamps gate this pair out. If its front executed and was consumed anyway, the
            // stamps may never move: after the bound, observe it and retire it on its record.
            let pair = self.queues.render.entries[slot]
                .as_mut()
                .and_then(|entry| entry.pair.as_mut())
                .ok_or(EIO)?;
            if pair.note_stamp_wait(stamps, state, crate::g17::now_ns())? {
                return Ok(Some(slot));
            }
        }
        Ok(None)
    }

    pub(super) fn settle_render_terminal(
        &mut self,
        slot: usize,
        deferred: &mut DeferredBatch,
    ) -> Result<bool> {
        let terminal = self.queues.render.entries[slot]
            .as_mut()
            .and_then(|entry| entry.pair.as_mut())
            .and_then(|pair| pair.take_terminal());
        if let Some((status, error)) = terminal {
            status.record(error);
            self.scan_compute(None, deferred)?;
            self.settle_render_vm(&status, error, deferred)?;
            self.queues.fail_compute_vm(&status, error, deferred)?;
            return Ok(true);
        }
        Ok(false)
    }

    /// Retains newly reported render masks and drains any deferred per-slot errors, timeouts, or
    /// closes while the caller already holds the device mutex.
    pub(in crate::g17::runtime) fn drain_render_deferred(
        &mut self,
        masks: [u64; 2],
        deferred: &mut DeferredBatch,
    ) -> Result {
        let _ = self.queues.retain_render_completions(masks);
        // Returned preparers can have deferred a VM failure or timeout.
        // Only this full worker collector runs their final observations.
        for slot in 1..self.queues.render.entries.len() {
            if self.queues.render.entries[slot]
                .as_ref()
                .is_some_and(|entry| {
                    entry.pair.is_some()
                        && (entry.pending_error.is_some()
                            || entry.pending_timeout
                            || entry.closed)
                })
            {
                self.apply_render_deferred(slot as u8, deferred)?;
            }
        }
        Ok(())
    }

    /// Retires healthy steady-state render completions (across one or more coalesced ready slots,
    /// bounded by `RENDER_SLOTS` and preallocated `deferred` room) under the event worker's
    /// already-held device mutex with a live preparation lease. Any error, timeout, close,
    /// growth, replay, terminal fault, deferred-room shortage, or remaining render work returns
    /// `Fallback` so the full `observe_renders()` loop handles it.
    pub(in crate::g17::runtime) fn try_fast_observe_render(
        &mut self,
        deferred: &mut DeferredBatch,
    ) -> Result<FastRenderOutcome> {
        let end = self.queues.render.entries.len();
        self.fast_observe_renders(1, end, deferred)
    }

    /// The observation a borrower owes the pair it has just put back into `slot`, in the hold
    /// that put it back, with a live preparation lease and a collector of the event worker's size
    /// (`deferred`): the event worker's single-lock retirement ([`Self::try_fast_observe_render`])
    /// of that slot alone. `Fallback` leaves the slot to the worker's full pass; otherwise its
    /// ready completions are retired (`Rescan`: and the rescan poll covers what stays in flight).
    ///
    /// The event pass a completion of a lent pair would otherwise request runs on another CPU and
    /// takes the device mutex after the borrower's next lend of the same pair, so it finds the
    /// pair lent again; observing here retires every completion by the return of the loan it
    /// arrived during.
    pub(in crate::g17::runtime) fn observe_returned_render(
        &mut self,
        slot: u8,
        deferred: &mut DeferredBatch,
    ) -> Result<FastRenderOutcome> {
        let slot = usize::from(slot);
        self.fast_observe_renders(slot, slot + 1, deferred)
    }

    /// [`Self::try_fast_observe_render`] for the observations of the slots `start..end`; the
    /// steady-state checks still cover every slot.
    fn fast_observe_renders(
        &mut self,
        start: usize,
        end: usize,
        deferred: &mut DeferredBatch,
    ) -> Result<FastRenderOutcome> {
        if self.recovery.pending()
            || self.queues.render.replay_parked()
            || self.render_grow_pending()
            || self.render_grow_ready()
        {
            return Ok(FastRenderOutcome::Fallback);
        }
        for slot in 1..self.queues.render.entries.len() {
            let Some(entry) = self.queues.render.entries[slot].as_ref() else {
                continue;
            };
            if entry.pending_error.is_some()
                || entry.pending_timeout
                || entry.closed
                || entry.growing
            {
                return Ok(FastRenderOutcome::Fallback);
            }
        }
        let mut retired_any = false;
        for _ in 0..crate::hw::t8140::queues::RENDER_SLOTS {
            let Some(slot) = self.next_render_observation_in(start, end)? else {
                return Ok(if self.queues.render_needs_rescan() {
                    FastRenderOutcome::Rescan
                } else if retired_any {
                    FastRenderOutcome::Done
                } else {
                    FastRenderOutcome::Idle
                });
            };
            if !deferred.has_room(2) {
                return Ok(FastRenderOutcome::Fallback);
            }
            let observed = self.queues.render.entries[slot]
                .as_ref()
                .and_then(|entry| entry.pair.as_ref())
                .ok_or(EIO)?
                .observe_front();
            let missed = self.queues.render.take_missed_event(slot as u8);
            if missed || self.render_grow_pending() {
                self.primary.state.shared.queue_event_records();
            }
            let state = crate::g17::recovery::Memory::recovery_state(&self.init)?;
            let progressed = match observed {
                Ok(observed) => self.with_render(slot, |pair, host| {
                    pair.retire_observed(observed, state, host, &mut |event| {
                        deferred.push(event)
                    })
                })?,
                Err(error) if error == EAGAIN => false,
                Err(error) => return Err(error),
            };
            if progressed {
                match self.release_render_lists() {
                    Err(error) if error == EAGAIN => {}
                    result => result?,
                }
            }
            self.queues.render.collect_retired(slot)?;
            let had_terminal = self.settle_render_terminal(slot, deferred)?;
            if self
                .queues
                .render
                .entry(slot as u8)?
                .pair
                .as_ref()
                .is_some_and(|pair| !pair.in_flight())
            {
                self.queues.render.entry_mut(slot as u8)?.event_masks = [0; 2];
            }
            if !progressed
                || missed
                || had_terminal
                || self.recovery.pending()
                || self.render_grow_pending()
                || self.render_grow_ready()
            {
                return Ok(FastRenderOutcome::Fallback);
            }
            // Work still in flight behind this front keeps its noted completion: retire whatever
            // else is ready in this hold before deciding.
            retired_any = true;
        }
        if self.next_render_observation_in(start, end)?.is_some() {
            return Ok(FastRenderOutcome::Fallback);
        }
        if self.queues.render_needs_rescan() {
            return Ok(FastRenderOutcome::Rescan);
        }
        Ok(if retired_any {
            FastRenderOutcome::Done
        } else {
            FastRenderOutcome::Idle
        })
    }
}

impl crate::g17::Shared {
    pub(in crate::g17) fn observe_renders(
        self: &Arc<Self>,
        deferred: &mut DeferredBatch,
    ) -> Result {
        if deferred.pending() != 0 {
            deferred.finish();
        }
        let mut cursor = 1;
        let mut remaining = crate::hw::t8140::queues::RENDER_DEPTH_MAX;
        let mut retries = 0;
        loop {
            let epoch = match self.preparations.try_enter() {
                Err(error) if error == EAGAIN || error == ENODEV => break,
                result => result?,
            };
            // Selecting, reading and retiring the front share one hold: a
            // separate hold for the read would let publishers and the
            // retirement contend twice per observed pair.
            let mut state = self.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                return Ok(());
            };
            if !epoch.is_current()
                || firmware.recovery.pending()
                || self.crashed.load(Ordering::Acquire)
            {
                break;
            }
            let Some(slot) = firmware.next_render_observation(cursor)? else {
                break;
            };
            if slot != cursor {
                remaining = crate::hw::t8140::queues::RENDER_DEPTH_MAX;
                retries = 0;
            }
            let observed = firmware.queues.render.entries[slot]
                .as_ref()
                .and_then(|entry| entry.pair.as_ref())
                .ok_or(EIO)?
                .observe_front();
            // Publishers waiting for render capacity recheck once this hold ends.
            self.note_changed();
            if firmware.render_grow_pending() || firmware.queues.render.take_missed_event(slot as u8) {
                // Observation and the growth check run on every pass.
                self.queue_event_records();
            }
            let result = (|| {
                let state = crate::g17::recovery::Memory::recovery_state(&firmware.init)?;
                let progressed = match observed {
                    Ok(observed) => firmware.with_render(slot, |pair, host| {
                        pair.retire_observed(observed, state, host, &mut |event| {
                            deferred.push(event)
                        })
                    })?,
                    Err(error) if error == EAGAIN => false,
                    Err(error) => return Err(error),
                };
                if progressed {
                    match firmware.release_render_lists() {
                        Err(error) if error == EAGAIN => {}
                        result => result?,
                    }
                }
                firmware.queues.render.collect_retired(slot)?;
                // An observation of the old front is committed before a queued
                // exact timeout is permitted to advance that same tracker.
                firmware.apply_render_deferred(slot as u8, deferred)?;
                let _ = firmware.settle_render_terminal(slot, deferred)?;
                if firmware
                    .queues
                    .render
                    .entry(slot as u8)?
                    .pair
                    .as_ref()
                    .is_some_and(|pair| !pair.in_flight())
                {
                    firmware.queues.render.entry_mut(slot as u8)?.event_masks = [0; 2];
                }
                Ok(progressed)
            })();
            drop(state);
            drop(epoch);
            deferred.finish();
            if result? {
                remaining -= 1;
                retries = 0;
                cursor = if remaining == 0 { slot + 1 } else { slot };
            } else {
                retries += 1;
                cursor = if retries >= 2 { slot + 1 } else { slot };
            }
            if cursor != slot {
                remaining = crate::hw::t8140::queues::RENDER_DEPTH_MAX;
                retries = 0;
            }
        }
        Ok(())
    }
}
