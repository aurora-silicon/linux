// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Scheduler cancellation and final retirement checks for retained compute work.

use super::{Backend, DeferredBatch};
use crate::g17::job::{self, Packet};
use kernel::{prelude::*, sync::Arc};

impl Backend {
    /// The caller owns cancellation of a packet that never claimed publication.
    /// A missing retained packet may already have a healthy callback waiting off-lock.
    pub(super) fn timeout_compute(&self, packet: &Arc<Packet>) -> Result {
        let mut deferred = DeferredBatch::worker()?;
        let result = (|| {
            let mut state = self.shared.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                // Teardown owns the detached queues and will settle their callbacks.
                return Ok(None);
            };
            firmware.scan_compute(None, &mut deferred)?;
            let Some(queue) = firmware.queues.compute_mut(self.owner) else {
                return Ok(None);
            };
            if !queue.owns_packet(packet) {
                return Ok(None);
            }
            queue.timeout(&mut |event| deferred.push(event))
        })();
        if result.as_ref().is_ok_and(|status| status.is_some()) {
            // VM status and peer settlement precede signalling this timeout. The backend
            // owns the same logical VM as the retained packet matched under the mutex.
            <Self as job::Backend>::fail_vm(self, ETIMEDOUT);
        }
        deferred.finish();
        result.map(|_| ())
    }

    /// Cancel only a scheduler waiter. Visible mappings stay pinned until their witness;
    /// cancellation neither quarantines hardware admission nor fans out a VM failure.
    pub(super) fn cancel_compute(&self, packet: &Arc<Packet>) -> Result {
        let mut deferred = DeferredBatch::worker()?;
        let result = (|| {
            let mut state = self.shared.state.lock();
            let Some(firmware) = (*state).as_deref_mut() else {
                return Ok(());
            };
            firmware.scan_compute(None, &mut deferred)?;
            if let Some(queue) = firmware.queues.compute_mut(self.owner) {
                queue.cancel(packet, &mut |event| deferred.push(event))?;
            }
            Ok(())
        })();
        deferred.finish();
        result
    }
}
