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

pub(crate) const fn default_depth(_t8122:bool) -> usize {1}

pub(crate) const fn overlap_ready(initialized:bool, render:bool, all_render:bool,
    same_vm:bool, pipes_consumed:bool) -> bool {
    initialized && render && all_render && same_vm && pipes_consumed
}

/// The circular free tail of contiguous, ordered active batches.
pub(crate) fn pipeline_room(depth: usize, batches: usize, passes: usize,
    last_base: usize, last_count: usize) -> Option<(usize, usize)> {
    if batches >= depth || passes >= SLOTS || last_base >= SLOTS
        || last_count == 0 || last_count > passes { return None; }
    Some(((last_base + last_count) % SLOTS, SLOTS - passes))
}

/// A signaled cancellation is not proof that firmware stopped using the packet.
pub(crate) const fn packet_retired(pending: bool, active: bool) -> bool {
    !pending && !active
}

pub(crate) const fn retain_runtime(stop_failed: bool, gpu_pending: bool) -> bool {
    stop_failed || gpu_pending
}

