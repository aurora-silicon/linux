// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Republication of render groups that firmware retired without executing.
//! The pair proves the abandonment (retired, unlinked, stamps covering it, no
//! recovery, fragment end never written) and parks the packet; this pass
//! prepares and publishes the same command again under the device mutex.

use super::super::DeferredBatch;
use crate::g17::{object::Allocator, queue::render::RenderAddresses, Firmware};
use kernel::prelude::*;

impl super::RenderState {
    pub(in crate::g17) fn replay_polling(&self) -> bool {
        self.entries
            .iter()
            .flatten()
            .filter_map(|entry| entry.pair.as_ref())
            .any(|pair| pair.replay_polling())
    }
}

impl Firmware {
    pub(in crate::g17) fn replay_render(&mut self, deferred: &mut DeferredBatch) -> Result {
        if self.recovery.pending()
            || crate::g17::recovery::Memory::recovery_state(&self.init)? != 0
        {
            return Ok(());
        }
        for slot in 0..self.queues.render.entries.len() {
            let Some((packet, owner, qids, closed, busy)) =
                self.queues.render.entries[slot].as_ref().and_then(|entry| {
                    let pair = entry.pair.as_ref()?;
                    let packet = pair.replay_front()?;
                    Some((
                        packet,
                        entry.reservation.owner,
                        pair.qids(),
                        entry.closed,
                        entry.preparing.is_some()
                            || entry.growing
                            || entry.pending_timeout
                            || entry.pending_error.is_some(),
                    ))
                })
            else {
                continue;
            };
            if busy {
                continue;
            }
            if closed || packet.cancel_requested() || !packet.context.is_current() {
                self.with_render(slot, |pair, _| {
                    pair.fail_replay(ECANCELED, &mut |event| deferred.push(event))
                })?;
                continue;
            }
            if packet.completion.status().get() != 0 {
                self.with_render(slot, |pair, _| {
                    pair.fail_replay(EIO, &mut |event| deferred.push(event))
                })?;
                continue;
            }
            let addresses = RenderAddresses {
                stamps: [
                    self.init.stamp_addresses(qids[0])?,
                    self.init.stamp_addresses(qids[1])?,
                ],
            };
            let dev = self.primary.state.shared.drm()?;
            let uat = self.uat.clone();
            let prepared = self.with_render(slot, |pair, _| {
                if !pair.room() {
                    return Err(EBUSY);
                }
                let alloc = Allocator {
                    dev: &dev,
                    uat: &uat,
                };
                pair.prepare(&alloc, &packet, addresses)
            });
            match prepared {
                Ok(()) => {}
                Err(error) if error == EBUSY || error == EAGAIN => continue,
                Err(error) => {
                    self.with_render(slot, |pair, _| {
                        pair.fail_replay(error, &mut |event| deferred.push(event))
                    })?;
                    continue;
                }
            }
            match self.publish_render(owner, slot as u8, &packet, deferred) {
                Ok(()) => {
                    self.with_render(slot, |pair, _| pair.finish_replay(&packet))?;
                }
                Err(error) if error == EBUSY || error == EAGAIN => continue,
                Err(error) => {
                    self.with_render(slot, |pair, _| {
                        pair.fail_replay(error, &mut |event| deferred.push(event))
                    })?;
                }
            }
        }
        Ok(())
    }
}
