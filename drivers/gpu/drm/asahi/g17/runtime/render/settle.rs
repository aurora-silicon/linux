// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Exact packet timeout and paired firmware-drain settlement. Every fence and
//! admission release is collected under the device lock and finished outside it.

use crate::g17::{
    job::Packet,
    runtime::{Backend, DeferredBatch},
};
use kernel::{dma_fence::RawDmaFence, prelude::*, sync::Arc};

impl Backend {
    pub(in crate::g17) fn timeout_render(&self, packet: &Arc<Packet>) -> Result {
        // Without a collector the watchdog expires again and retries: memory
        // pressure must not stop the runtime for every client.
        let Ok(mut deferred) = DeferredBatch::worker() else {
            return Ok(());
        };
        let result = {
            let mut state = self.shared.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                return Ok(());
            };
            if firmware.queues.defer_render_timeout(packet, ETIMEDOUT) {
                return Ok(());
            }
            firmware.fail_render_packet(packet, ETIMEDOUT, &mut deferred)
        };
        deferred.finish();
        result
    }
    pub(in crate::g17) fn cancel_render(&self, packet: &Arc<Packet>) -> Result {
        let fence = packet.completion.scheduler_fence();
        // SAFETY: The retained fence remains alive across this bounded,
        // uninterruptible queue-teardown wait, without holding a device lock.
        let completed = unsafe {
            kernel::bindings::dma_fence_wait_timeout(
                fence.raw(),
                false,
                kernel::time::msecs_to_jiffies(2000) as isize,
            )
        };
        if completed > 0 {
            return Ok(());
        }
        if packet.is_published() {
            self.timeout_render(packet)
        } else {
            // The outer callback already completed a successful 0 -> 3
            // cancellation. A claimed publisher still owns its VM pin and
            // observes cancel_requested before committing or finishing.
            Ok(())
        }
    }
}

impl crate::g17::Firmware {
    pub(super) fn fail_render_packet(
        &mut self,
        packet: &Arc<Packet>,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        let Some(slot) = packet.completion.render_slot() else {
            return Ok(());
        };
        let slot = usize::from(slot);
        loop {
            let pair = self
                .queues
                .render
                .entries
                .get(slot)
                .and_then(Option::as_ref)
                .and_then(|entry| entry.pair.as_ref())
                .ok_or(EIO)?;
            if pair.replay_contains(packet) || !pair.contains_packet(packet) {
                return Ok(());
            }
            let generation = self.recovery.generation();
            let state = crate::g17::recovery::Memory::recovery_state(&self.init)?;
            if error == ETIMEDOUT {
                let qids = pair.qids();
                let stamps = [
                    self.init.stamp(qids[0])?.load(core::sync::atomic::Ordering::Relaxed),
                    self.init.stamp(qids[1])?.load(core::sync::atomic::Ordering::Relaxed),
                ];
                core::sync::atomic::fence(core::sync::atomic::Ordering::Acquire);
                if let Some(pair) = self.queues.render.entries[slot]
                    .as_mut()
                    .and_then(|entry| entry.pair.as_mut())
                {
                    pair.report_timeout(stamps, state);
                    pair.expire_stamp_wait(packet, stamps, state)?;
                }
            }
            let progressed = self.with_render(slot, |pair, host| {
                pair.retire(None, generation, state, host, &mut |event| {
                    deferred.push(event)
                })
            })?;
            if !progressed {
                break;
            }
            match self.release_render_lists() {
                Err(error) if error == EAGAIN => {}
                result => result?,
            }
        }
        self.with_render(slot, |pair, host| {
            pair.fail_after_observation(packet, error, host, &mut |event| deferred.push(event))
        })?;
        self.queues.render.collect_retired(slot)?;
        let terminal = self.queues.render.entries[slot]
            .as_mut()
            .and_then(|entry| entry.pair.as_mut())
            .and_then(|pair| pair.take_terminal());
        if let Some((status, error)) = terminal {
            status.record(error);
            self.scan_compute(None, deferred)?;
            self.settle_render_vm(&status, error, deferred)?;
            self.queues.fail_compute_vm(&status, error, deferred)?;
        }
        Ok(())
    }

    pub(in crate::g17) fn settle_blamed_render(
        &mut self,
        qid: u8,
        error: Error,
        deferred: &mut DeferredBatch,
    ) -> Result {
        let packet = self
            .queues
            .render
            .entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.pair.as_ref())
            .find(|pair| pair.qids().contains(&qid))
            .and_then(|pair| pair.oldest_packet());
        if let Some(packet) = packet {
            // Unclassified work counts as innocent, and a blamed group must never be replayed.
            // Work an earlier recovery spared is collateral here (classify() blames before its
            // collateral check) and stays spared, so its failure stays non-sticky; the blame
            // itself is recorded apart from the class, and refuses any replay of it.
            packet.completion.note_blamed();
            if !packet.completion.spared() {
                packet.completion.classify(crate::g17::recovery::Class::Guilty);
            }
            self.fail_render_packet(&packet, error, deferred)?;
        }
        Ok(())
    }

    pub(in crate::g17) fn settle_cancelled_renders(
        &mut self,
        mask: u128,
        deferred: &mut DeferredBatch,
    ) -> Result {
        for slot in 1..self.queues.render.entries.len() {
            let packet = self.queues.render.entries[slot]
                .as_ref()
                .and_then(|entry| entry.pair.as_ref())
                .filter(|pair| {
                    pair.qids()
                        .into_iter()
                        .any(|qid| mask & (1u128 << qid) != 0)
                        && pair.oldest_spared() == Some(true)
                })
                .and_then(|pair| pair.oldest_packet());
            if let Some(packet) = packet {
                self.fail_render_packet(&packet, ECANCELED, deferred)?;
            }
        }
        Ok(())
    }

    /// Settles every pair the firmware drained (`Pair::needs_drain`). A pair whose settlement
    /// fails is retried with a doubling backoff and, after `DRAIN_RETRY_LIMIT` failures, left
    /// out of service and re-checked rarely: its clients already failed with the quarantine.
    /// Each episode is logged at its first failure and when the pair is left out of service.
    /// Returns the delay to the next retry that waits for its backoff.
    pub(in crate::g17) fn reclaim_drained_renders(
        &mut self,
        deferred: &mut DeferredBatch,
    ) -> Result<Option<u64>> {
        if self.recovery.pending()
            || self
                .primary
                .state
                .shared
                .crashed
                .load(core::sync::atomic::Ordering::Acquire)
        {
            return Ok(None);
        }
        let now = crate::g17::now_ns();
        let mut serviced = false;
        let mut next: Option<u64> = None;
        for slot in 1..self.queues.render.entries.len() {
            let Some(pair) = self.queues.render.entries[slot]
                .as_ref()
                .and_then(|entry| entry.pair.as_ref())
                .filter(|pair| pair.needs_drain())
            else {
                continue;
            };
            if !pair.drain_due(now) {
                if let Some(delay) = pair.drain_delay(now) {
                    next = Some(next.map_or(delay, |next| next.min(delay)));
                }
                continue;
            }
            let failures = pair.drain_failures();
            match self.drain_render_slot(slot, &mut serviced, deferred) {
                Ok(()) => {
                    let settled = self.queues.render.entries[slot]
                        .as_ref()
                        .and_then(|entry| entry.pair.as_ref())
                        .is_none_or(|pair| !pair.needs_drain());
                    if failures != 0 && settled {
                        SETTLE_RECOVERED.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
                        dev_info!(
                            self.primary.state.shared.dev.as_ref(),
                            "G17: render pair {} settled after {} failed attempts\n",
                            slot,
                            failures
                        );
                    }
                }
                Err((step, error)) => {
                    SETTLE_FAILURES.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
                    let Some(pair) = self.queues.render.entries[slot]
                        .as_mut()
                        .and_then(|entry| entry.pair.as_mut())
                    else {
                        continue;
                    };
                    let failures = pair.note_drain_failure(now);
                    if let Some(delay) = pair.drain_delay(now) {
                        next = Some(next.map_or(delay, |next| next.min(delay)));
                    }
                    if failures == 1 {
                        dev_warn!(
                            self.primary.state.shared.dev.as_ref(),
                            "G17: could not settle drained render pair {} ({}: {:?}); retrying with backoff\n",
                            slot,
                            step,
                            error
                        );
                    } else if failures == crate::g17::queue::render::DRAIN_RETRY_LIMIT {
                        SETTLE_PARKED.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
                        dev_err!(
                            self.primary.state.shared.dev.as_ref(),
                            "G17: drained render pair {} still not settled after {} attempts ({}: {:?}); it stays out of service\n",
                            slot,
                            failures,
                            step,
                            error
                        );
                    } else {
                        continue;
                    }
                    let _ = self.with_render(slot, |pair, host| {
                        pair.report_drain(host);
                        Ok(())
                    });
                }
            }
        }
        Ok(next)
    }

    fn drain_render_slot(
        &mut self,
        slot: usize,
        serviced: &mut bool,
        deferred: &mut DeferredBatch,
    ) -> core::result::Result<(), (&'static str, Error)> {
        let at = |step: &'static str| move |error: Error| (step, error);
        if !self
            .with_render(slot, |pair, host| pair.firmware_drained(host))
            .map_err(at("drain proof"))?
        {
            return Ok(());
        }
        if !*serviced {
            self.service_runtime_events(deferred)
                .map_err(at("event service"))?;
            *serviced = true;
        }
        if self.recovery.pending()
            || self
                .primary
                .state
                .shared
                .crashed
                .load(core::sync::atomic::Ordering::Acquire)
        {
            return Ok(());
        }
        loop {
            let accounted = self
                .with_render(slot, |pair, host| pair.account_drained(host))
                .map_err(at("accounting"))?;
            if !accounted {
                break;
            }
            match self.release_render_lists() {
                Err(error) if error == EAGAIN => {}
                result => result.map_err(at("free-list release"))?,
            }
        }
        if self.recovery.pending()
            || self
                .primary
                .state
                .shared
                .crashed
                .load(core::sync::atomic::Ordering::Acquire)
        {
            return Ok(());
        }
        self.with_render(slot, |pair, host| {
            pair.release_witnessed(host, &mut |event| deferred.push(event))
        })
        .map_err(at("release"))?;
        self.queues
            .render
            .collect_retired(slot)
            .map_err(at("binding collection"))
    }
}

/// Device-lifetime counts of drained-pair settlement failures, of pairs that settled after
/// failing, and of pairs left out of service; diagnostics only (debugfs `capacity`).
pub(in crate::g17) static SETTLE_FAILURES: core::sync::atomic::AtomicU64 =
    core::sync::atomic::AtomicU64::new(0);
pub(in crate::g17) static SETTLE_RECOVERED: core::sync::atomic::AtomicU64 =
    core::sync::atomic::AtomicU64::new(0);
pub(in crate::g17) static SETTLE_PARKED: core::sync::atomic::AtomicU64 =
    core::sync::atomic::AtomicU64::new(0);
