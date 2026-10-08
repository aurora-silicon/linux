// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounds and retirement decisions for completion-worker batching.

pub(crate) const SLOTS: usize = 16;
pub(crate) const MAX_PACKETS: usize = 8;

pub(crate) fn pending_position(urgencies: impl Iterator<Item=u8>, incoming:u8) -> usize {
    let mut len=0;
    for queued in urgencies {
        if queued<incoming {return len;}
        len+=1;
    }
    len
}

pub(crate) const fn may_join(render:bool, next:usize, same_vm:bool, same_urgency:bool) -> bool {
    render && next==0 && same_vm && same_urgency
}

/// A signaled cancellation is not proof that firmware stopped using the packet.
pub(crate) const fn packet_retired(pending: bool, active: bool) -> bool {
    !pending && !active
}

pub(crate) const fn retain_runtime(stop_failed: bool, gpu_pending: bool) -> bool {
    stop_failed || gpu_pending
}

