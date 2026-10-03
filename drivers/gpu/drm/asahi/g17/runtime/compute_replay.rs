// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Ordered republication of witnessed, unexecuted compute commands.

use super::DeferredBatch;
use crate::g17::{completion::Deferred, qos, recovery, Firmware};
use kernel::prelude::*;

impl Firmware {
    /// Called only from an outer worker or client submission, never from event
    /// dispatch or a recovery callback nested within publication. All candidate
    /// queues are observed before any retained packet is published again.
    pub(in crate::g17) fn replay_compute(&mut self, deferred: &mut DeferredBatch) -> Result {
        if self.recovery.pending() {
            return Ok(());
        }
        let state = recovery::Memory::recovery_state(&self.init)?;
        if state != 0 {
            return Ok(());
        }
        let view = qos::View::new(self.init.qos()?)?;
        for entry in self.queues.compute.iter_mut().flatten() {
            if let Some(queue) = entry.queue.as_deref_mut() {
                queue.prepare_replays(
                    state,
                    |owner| self.queues.accounting.complete(&view, &[owner]),
                    &mut |event| deferred.push(event),
                )?;
            }
        }
        for index in 0..self.queues.compute.len() {
            loop {
                if self.recovery.pending() || recovery::Memory::recovery_state(&self.init)? != 0 {
                    return Ok(());
                }
                let Some(queue) = self.queues.compute[index]
                    .as_ref()
                    .and_then(|entry| entry.queue.as_deref())
                else {
                    break;
                };
                let Some(packet) = queue.replay_front() else {
                    break;
                };
                let owner = queue.owner();

                let result = (|| {
                    self.queues.compute[index]
                        .as_mut()
                        .and_then(|entry| entry.queue.as_deref_mut())
                        .ok_or(EIO)?
                        .prepare_replay(&packet)?;
                    let owner = owner.ok_or(EIO)?;
                    packet.check_dependencies()?;
                    let dependencies = self.queues.render_prefix(owner, &packet)?;
                    self.publish_compute(owner, packet.clone(), dependencies.as_slice(), deferred)
                })();
                let recovery_wait = matches!(result, Err(EAGAIN) | Err(EBUSY))
                    && (self.recovery.pending()
                        || recovery::Memory::recovery_state(&self.init)? != 0);
                let queue = self.queues.compute[index]
                    .as_mut()
                    .and_then(|entry| entry.queue.as_deref_mut())
                    .ok_or(EIO)?;
                if let Err(error) = result {
                    if recovery_wait && !queue.owns_packet(&packet) {
                        // Keep this prepared attempt and its one timeout credit
                        // while the outer worker closes the new recovery.
                        return Ok(());
                    }
                    queue.fail_replays(error);
                    if !queue.owns_packet(&packet) {
                        deferred
                            .push(Deferred::Retired(packet.completion.clone(), Err(ENODATA)))?;
                    }
                }
                queue.finish_replay(&packet)?;
            }
        }
        Ok(())
    }
}
