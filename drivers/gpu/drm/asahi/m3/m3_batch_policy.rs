// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Bounds and retirement decisions for completion-worker batching.

pub(crate) const SLOTS: usize = 16;
pub(crate) const MAX_PACKETS: usize = 8;

/// T6030 may omit internal measurements; application queries always record.
/// Other chips retain their existing microsequences regardless of the switch.
pub(crate) fn stage_timestamps(internal:bool,chip:u32,queries:[u64;2])->bool {
    internal || chip!=0x6030 || queries!=[0;2]
}

/// One packet's bounded prefix may use both engines only after InitBM retired
/// and while every prior batch is idle. Zero selects the single-engine policy.
pub(crate) fn mixed_prefix(enabled:bool, chip:u32, initialized:bool, idle:bool,
    kinds:impl Iterator<Item=bool>) -> usize {
    if !enabled || chip!=0x6030 || !initialized || !idle {return 0;}
    let mut count=0;
    let mut render=false;
    let mut compute=false;
    for is_render in kinds.take(SLOTS) {
        count+=1;
        render|=is_render;
        compute|=!is_render;
    }
    if render && compute {count} else {0}
}

/// Only an unpublished T6030 batch may defer an intermediate stamp flush.
/// Its final pass and every pass with firmware-written user timestamps flush.
pub(crate) fn coalesce_render_stamps(enabled:bool, chip:u32, overlap:bool,
    slot:usize, count:usize, timestamps:[[u64;2];2]) -> bool {
    enabled && chip==0x6030 && !overlap && count>1 && count<=SLOTS
        && slot<count-1 && timestamps==[[0;2];2]
}

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

/// A failed packet that no batch ever included can drop its VM job guard: the firmware never saw
/// it. Any other keeps the guard, and its VM stays quarantined, until the runtime drops it.
pub(crate) const fn release_unpublished(may_be_published: bool) -> bool {
    !may_be_published
}

pub(crate) const fn retain_runtime(stop_failed: bool, gpu_pending: bool) -> bool {
    stop_failed || gpu_pending
}

/// One firmware start per boot. `stop_asc` only clears RUN: it doesn't reset the firmware or
/// give back a reusable RTKit or InitData state, and TTBAT context 0 still names the lower root
/// the first start adopted, which is freed once ASC has stopped. Claimed just before the first
/// ASC start and never released.
pub(crate) struct StartLatch(core::sync::atomic::AtomicBool);
impl StartLatch {
    pub(crate) const fn new() -> Self { Self(core::sync::atomic::AtomicBool::new(false)) }
    /// True for the first claim only.
    pub(crate) fn claim(&self) -> bool { !self.0.swap(true, core::sync::atomic::Ordering::AcqRel) }
}

/// Why a start was refused before ASC.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum StartRefusal {
    /// The module is being removed: nothing is claimed or started.
    Removing,
    /// This boot's start already happened: the extra module reference is dropped again.
    AlreadyStarted,
}

/// Pins the module, then claims `latch`. The pin comes first, so a probe racing an ordinary
/// rmmod fails closed instead of reviving the module's reference count. A start that loses the
/// claim drops only its own pin; the winner keeps its pin for good, so a reload can't forget the
/// latch.
pub(crate) fn claim_start(latch: &StartLatch, pin: impl FnOnce() -> bool, unpin: impl FnOnce())
    -> Result<(), StartRefusal> {
    if !pin() {
        return Err(StartRefusal::Removing);
    }
    if !latch.claim() {
        unpin();
        return Err(StartRefusal::AlreadyStarted);
    }
    Ok(())
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
