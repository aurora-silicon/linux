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

/// Compare waiting work with the running batch, not the sorted pending head.
pub(crate) fn urgent_waiting(urgencies: impl Iterator<Item=u8>, active:u8) -> bool {
    urgencies.into_iter().any(|p|p>active)
}

pub(crate) const fn may_join(render:bool, next:usize, same_vm:bool, same_urgency:bool) -> bool {
    render && next==0 && same_vm && same_urgency
}

pub(crate) const fn default_depth(t8122:bool) -> usize {if t8122 {2} else {1}}
pub(crate) const fn default_budget_us(t8122:bool) -> u64 {if t8122 {4000} else {0}}

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

/// Always admit one pass; subsequent passes must fit the measured budget.
/// Unknown cost permits one pass until a measurement is available.
pub(crate) fn within_budget(cost: Option<u64>, budget: Option<u64>, passes: usize) -> bool {
    if passes == 0 || budget.is_none() { return true; }
    match cost {
        None => false,
        Some(ns) => ns.saturating_mul(passes as u64 + 1) <= budget.unwrap_or(u64::MAX),
    }
}

/// A signaled cancellation is not proof that firmware stopped using the packet.
pub(crate) const fn packet_retired(pending: bool, active: bool) -> bool {
    !pending && !active
}

pub(crate) const fn retain_runtime(stop_failed: bool, gpu_pending: bool) -> bool {
    stop_failed || gpu_pending
}

#[derive(Default)]
pub(crate) struct PassCosts { recent: [Option<(u64, u64)>; 8] }
impl PassCosts {
    pub(crate) fn get(&self, vm: u64) -> Option<u64> {
        self.recent.iter().flatten().find(|(id, _)| *id == vm).map(|(_, ns)| *ns)
    }
    /// Update only from a valid, retired GPU timestamp span. Zero is unmeasured.
    pub(crate) fn record(&mut self, vm: u64, ns: u64) {
        if ns == 0 { return; }
        let at = self.recent.iter().position(|v| v.is_some_and(|(id, _)| id == vm));
        let slot = at.unwrap_or(7);
        let cost = at.map_or(ns, |i| {
            let old = self.recent[i].map_or(ns, |(_, n)| n);
            ((u128::from(old) * 3 + u128::from(ns)) / 4) as u64
        });
        self.recent[slot] = Some((vm, cost));
        self.recent[..=slot].rotate_right(1);
    }
}
