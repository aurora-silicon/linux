// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Process-exit controls for closed contexts whose compute commands remain owned.

use super::{now_ns, DeferredBatch};
use crate::g17::{
    channel::Rings,
    context::Context,
    fw::channels::{ContextKill, ControlRecord},
    queue::compute::exit,
    recovery::Memory as _,
    Firmware, MSG_CONTROL_NOTIFY,
};
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};

impl Firmware {
    pub(in crate::g17) fn service_compute_exit(&mut self, deferred: &mut DeferredBatch) -> Result {
        let recovery_closed = self.init.recovery_state()? == 0;
        for entry in self.queues.compute.iter_mut().flatten() {
            if let Some(queue) = entry.queue.as_deref_mut() {
                queue.settle_exit(&self.queues.exit_acks, recovery_closed, &mut |event| {
                    deferred.push(event)
                })?;
            }
        }
        for cookie in self.queues.exit_acks.snapshot() {
            if cookie != 0
                && !self
                    .queues
                    .compute
                    .iter()
                    .flatten()
                    .filter_map(|entry| entry.queue.as_deref())
                    .any(|queue| queue.exit_cookie() == Some(cookie))
            {
                self.queues.exit_acks.forget(cookie);
            }
        }
        if self.recovery.pending() || !recovery_closed {
            return Ok(());
        }
        let now = now_ns();
        for index in 0..self.queues.teardown.len() {
            let Some((context, age)) = self.queues.teardown.closed(index, now) else {
                continue;
            };
            if !self.queues.render.teardown_idle(&context)?
                || !self.queues.render.teardown_pools_released(&context)
            {
                continue;
            }
            if let Err(error) = self.publish_compute_exit(&context, age, now) {
                dev_err!(
                    self.primary.state.shared.dev.as_ref(),
                    "Scheduler {} exit request failed: {:?}\n",
                    context.id(),
                    error
                );
            }
        }
        Ok(())
    }

    fn publish_compute_exit(&mut self, context: &Arc<Context>, age: u64, now: u64) -> Result {
        let Some(index) = self.queues.compute.iter().position(|entry| {
            entry
                .as_ref()
                .and_then(|entry| entry.queue.as_deref())
                .is_some_and(|queue| queue.exit_candidate(context, age))
        }) else {
            return Ok(());
        };
        if self.render_control_backpressured() {
            return Ok(());
        }
        let queue = self.queues.compute[index]
            .as_ref()
            .and_then(|entry| entry.queue.as_deref())
            .ok_or(EIO)?;
        let Some(owner) = queue.exit_owner(context) else {
            return Ok(());
        };
        let target = queue.qid();
        let (mut own, mut foreign, slots) = self.queues.render.exit_scope(context);
        for entry in self.queues.compute.iter().flatten() {
            if let Some(queue) = entry.queue.as_deref() {
                let bit = 1u128 << queue.qid();
                if Arc::ptr_eq(queue.context(), context) {
                    own |= bit;
                } else {
                    foreign |= bit;
                }
            }
        }
        let state = exit::State::read(context)?;
        let Some(qids) = state.scope(target, own, foreign) else {
            return Ok(());
        };
        let mut hold = 0u64;
        for slot in 0..crate::hw::t8140::queues::RENDER_SLOTS as u8 {
            if slots & (1u64 << slot) != 0 && self.queues.render.qids_for_slot(slot) & qids != 0 {
                hold |= 1u64 << slot;
            }
        }
        if hold != 0
            && !self
                .queues
                .admission
                .hold_render_slots_for_exit_kill(owner, hold)
        {
            return Ok(());
        }
        let cookie = exit::next_cookie();
        let record = ControlRecord::ContextKill(ContextKill::new(context.scheduler_va(), cookie));
        if let Err(error) = Rings::publish_control(&self.init, &record) {
            self.queues.admission.release_exit_kill_hold(hold);
            return if error == EAGAIN { Ok(()) } else { Err(error) };
        }
        // Publication is irrevocable even if its doorbell fails. Keep both
        // the killed graph and the held render identities until processor stop.
        self.queues.compute[index]
            .as_mut()
            .and_then(|entry| entry.queue.as_deref_mut())
            .ok_or(EIO)?
            .record_exit(cookie, now);
        fence(Ordering::SeqCst);
        if let Err(error) = self.primary.notify(MSG_CONTROL_NOTIFY) {
            dev_warn!(
                self.primary.state.shared.dev.as_ref(),
                "Scheduler {} exit announcement failed: {:?}\n",
                context.id(),
                error
            );
        }
        Ok(())
    }

    pub(in crate::g17) fn compute_exit_polling(&self, now: u64) -> bool {
        if self
            .queues
            .compute
            .iter()
            .flatten()
            .filter_map(|entry| entry.queue.as_deref())
            .any(|queue| queue.exit_polling(now))
        {
            return true;
        }
        (0..self.queues.teardown.len()).any(|index| {
            self.queues
                .teardown
                .closed(index, now)
                .is_some_and(|(context, age)| {
                    age < exit::POLL_NS
                        && self
                            .queues
                            .compute
                            .iter()
                            .flatten()
                            .filter_map(|entry| entry.queue.as_deref())
                            .any(|queue| queue.exit_candidate(&context, u64::MAX))
                })
        })
    }
}
