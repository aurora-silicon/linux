// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! v93r post-recovery controls and control-ring diagnostics (`g17p_recovery_path`).
//!
//! Root-only sysfs, runtime-writable bitmask, also settable at load. Default
//! is RECLAIM_DRAINED; 0 is exactly v93f. Without bit0 a client whose render
//! was failed by a stamp-blamed recovery keeps its whole VM -- every BO it
//! ever mapped -- pinned until both firmware processors stop, which in
//! practice is the next reboot.
//!   bit0 RECLAIM_DRAINED: after a firmware stamp-blamed recovery the render
//!        whose group could not be proven retired, and every render queued
//!        behind it on the same physical pair, are failed with their guards
//!        retained (`fail_while_owned`) and the pair is quarantined until both
//!        firmware processors stop. With this bit, once the recovery is
//!        closed (host and firmware handshake state 0) and the firmware has
//!        demonstrably drained that pair -- both engines' post-maintenance
//!        completion stamps cover the pair's last published ordinal and the
//!        pair FList's completed-kick counter equals its submitted counter --
//!        the event worker retires the pair's trackers through the normal
//!        paired-completion transaction (or, for a group the recovery
//!        skipped, the existing firmware-retired classifier), releases the
//!        retained VM-job and lease guards, and returns the pair. The client's
//!        VM stays poisoned; only the physical pair, its FList/HWPB state and
//!        the closed queue's teardown are recovered.
//!   bit1 CONTROL_TRACE: remember opcode/argument/publish time of every
//!        primary device-control record and the time of the last EP 0x21
//!        announcement; a control re-announce then names the oldest pending
//!        record and whether it had been announced after its publication.
//!   bit2 GROW_REKICK (v96): firmware event 6 (TVB grow) pauses the pool's
//!        allocator until the opcode-8 reply. A request whose owning pair is
//!        borrowed (`preparing`: warm-up, restage, grow, completion
//!        observation) is parked, and the grow worker exits. Without this bit
//!        only the next firmware event re-queues the worker; if the stalled
//!        render was the only producer of events, nothing does. With this
//!        bit, every end of a borrow (and every clearing of control-ring
//!        backpressure) re-queues the worker while a request is parked.
//!   bit3 reserved (v97 INNOCENT_VICTIM, not in this build; rejected).
//!   bit4 STALL_WATCH (v97d, DIAGNOSTIC): while any render pair has
//!        outstanding work, a 4 ms delayed work reads the firmware per-QID
//!        dependency records (firmware DATA, read-only) of every busy pair.
//!        A 3D queue is a stall candidate when its TA's completed stamp has
//!        passed the 3D's next stamp (its TA parent is complete) while that
//!        queue has not advanced for >= STALL_MS. Other clients' progress
//!        must not hide starvation of this queue. The first sample of
//!        a stall logs the dependency records, the B1 scheduler masks
//!        (requested/sampled/deferred/next), the runnable mask and the park
//!        record. Log and count only: no firmware write, no message.
//!   bit5 STALL_REDOORBELL (v97d, DIAGNOSTIC, needs bit4): on a detected
//!        stall re-send the exact paired render doorbell a submission ends
//!        with (the only host action that activates a published render),
//!        once per stall. Nothing is sent while a firmware recovery is
//!        pending or the session is not accepting work. A doorbell names no
//!        queue: firmware rescans its own rings, as after any submission.
//!   bit6 STALL_FRAGMENT (v97d, DIAGNOSTIC, needs bits 4+5): if a stall is
//!        still present STALL_MS after the paired doorbell, also send the
//!        Fragment (3D) doorbell once. Native sends only the paired Vertex
//!        doorbell per render, so this arm is a discriminator, never a fix.
//! Grow ingress, deferral, re-kick and recovery-time grow state are always
//! logged (rate-limited) and counted, independent of the mask.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam};

pub(crate) const RECLAIM_DRAINED: u32 = 1 << 0;
pub(crate) const CONTROL_TRACE: u32 = 1 << 1;
pub(crate) const GROW_REKICK: u32 = 1 << 2;
pub(crate) const STALL_WATCH: u32 = 1 << 4;
pub(crate) const STALL_REDOORBELL: u32 = 1 << 5;
pub(crate) const STALL_FRAGMENT: u32 = 1 << 6;
const ALL_BITS: u32 = RECLAIM_DRAINED | CONTROL_TRACE | GROW_REKICK | STALL_WATCH | STALL_REDOORBELL
    | STALL_FRAGMENT;

const DEFAULT_MASK: u32 = RECLAIM_DRAINED;
static MASK: AtomicU32 = AtomicU32::new(DEFAULT_MASK);

#[inline(always)]
pub(crate) fn enabled(bit: u32) -> bool { MASK.load(Ordering::Relaxed) & bit != 0 }

/// Why a reclaim attempt for one quarantined pair stopped (counters and log).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum ReclaimBlock {
    /// A firmware recovery is open or the handshake state is not 0.
    Recovery,
    /// The pair is being prepared by a publisher.
    Preparing,
    /// A completion stamp does not yet cover the last published ordinal.
    Stamps,
    /// The pair FList still has submitted kicks not completed.
    Flist,
    /// A tracked group is not yet provably retired.
    Tracker,
    /// A step returned an error; the pair stays quarantined.
    Error,
}

impl ReclaimBlock {
    const fn index(self) -> usize {
        match self {
            Self::Recovery => 0, Self::Preparing => 1, Self::Stamps => 2,
            Self::Flist => 3, Self::Tracker => 4, Self::Error => 5,
        }
    }
    const NAMES: [&'static str; 6] = ["recovery", "preparing", "stamps", "flist", "tracker", "error"];
}

/// Attempts, reclaimed pairs, retired trackers, released completions.
pub(crate) static RECLAIM_ATTEMPTS: AtomicU64 = AtomicU64::new(0);
pub(crate) static RECLAIM_PAIRS: AtomicU64 = AtomicU64::new(0);
pub(crate) static RECLAIM_TRACKERS: AtomicU64 = AtomicU64::new(0);
pub(crate) static RECLAIM_RELEASED: AtomicU64 = AtomicU64::new(0);
static RECLAIM_BLOCKED: [AtomicU64; 6] = [const { AtomicU64::new(0) }; 6];

/// v96 TVB grow accounting: firmware requests received, requests left parked
/// because their pair was borrowed when the worker looked, borrow ends that
/// found a parked request, and worker re-queues issued for them (bit2).
pub(crate) static GROW_INGRESS: AtomicU64 = AtomicU64::new(0);
pub(crate) static GROW_DEFERRED: AtomicU64 = AtomicU64::new(0);
pub(crate) static GROW_PARKED_AT_RETURN: AtomicU64 = AtomicU64::new(0);
pub(crate) static GROW_REKICKS: AtomicU64 = AtomicU64::new(0);
pub(crate) static GROW_REPLIED: AtomicU64 = AtomicU64::new(0);

/// Handshake steps 5-6: the last opcode-8 grow reply published to the
/// primary control ring (bit63 valid | producer_after << 32 | buffer_slot)
/// and its CNTPCT, until the firmware consumer is observed past it.
pub(crate) static GROW_REPLY_WATCH: AtomicU64 = AtomicU64::new(0);
pub(crate) static GROW_REPLY_AT: AtomicU64 = AtomicU64::new(0);
pub(crate) static GROW_REPLY_CONSUMED: AtomicU64 = AtomicU64::new(0);

/// v97d 3D stall watch (bits 4/5). Polls, detections, detections whose 3D
/// QID was in the B1 deferred mask, doorbells sent (paired / Fragment),
/// stalls that cleared after a doorbell, stalls that cleared unaided, and
/// stalls still present when a firmware recovery arrived.
pub(crate) static STALL_POLLS: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_DETECTED: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_DEFERRED: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_DOORBELLS: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_FRAGMENT_DOORBELLS: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_CLEARED_AFTER_DOORBELL: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_CLEARED_UNAIDED: AtomicU64 = AtomicU64::new(0);
pub(crate) static STALL_AT_RECOVERY: AtomicU64 = AtomicU64::new(0);
/// Longest detected stall (ns from candidate start to clearance).
pub(crate) static STALL_MAX_NS: AtomicU64 = AtomicU64::new(0);

/// Poll period and detection threshold. The native DM monitor escalates a
/// no-progress DM1 slot after two samples ~36 ms apart (T2 hang: +34/+70 ms
/// after the TA completed); detection must precede it.
pub(crate) const STALL_POLL_MS: u64 = 4;
pub(crate) const STALL_MS: u64 = 20;

/// Per-pair watch state, owned by the Runtime lock (atomics only for
/// static storage). `since` = ns when the pair became a candidate with this
/// 3D completed stamp (0 = not a candidate); `stage` 0 none, 1 detected,
/// 2 paired doorbell sent, 3 Fragment doorbell sent, 4 send failed.
pub(crate) struct StallSlot {
    pub(crate) fragment_completed: AtomicU64,
    pub(crate) since: AtomicU64,
    pub(crate) stage: AtomicU32,
    pub(crate) acted_at: AtomicU64,
}
pub(crate) const STALL_SLOTS: usize = 16;
pub(crate) static STALL_SLOT: [StallSlot; STALL_SLOTS] = [const {
    StallSlot { fragment_completed: AtomicU64::new(u64::MAX), since: AtomicU64::new(0),
        stage: AtomicU32::new(0), acted_at: AtomicU64::new(0) }
}; STALL_SLOTS];
/// A 3D queue is a stall candidate when it has published work beyond its
/// completed stamp and the paired TA queue (same stamp numbering) has
/// completed at least the 3D's next stamp: its TA parent is complete.
/// Stamps compare as signed 40-bit values, like the firmware's barriers.
pub(crate) fn stall_candidate(ta_completed: u64, fragment_completed: u64, latest: u64,
    next: Option<u64>) -> bool {
    fragment_completed != latest && next.is_some_and(|next| {
        let delta = ta_completed.wrapping_sub(next) & 0xff_ffff_ffff;
        delta < (1 << 39)
    })
}

/// Forget an interrupted observation without claiming the work completed.
/// A borrowed/absent graph or an unreadable dependency record is not a
/// retirement witness. A later sample must start a new contiguous interval.
pub(crate) fn discard_stall_observation(slot: &StallSlot) {
    slot.since.store(0, Ordering::Relaxed);
    slot.stage.store(0, Ordering::Relaxed);
    slot.acted_at.store(0, Ordering::Relaxed);
    slot.fragment_completed.store(u64::MAX, Ordering::Relaxed);
}

/// Observe one queue independently of every other queue's progress. Only a
/// changed completion stamp counts as clearance. Losing the candidate
/// predicate or its graph cannot establish that a doorbell rescued it.
pub(crate) fn sample_stall_slot(slot: &StallSlot, now: u64,
    fragment_completed: u64, candidate: bool) -> bool {
    if slot.fragment_completed.swap(fragment_completed, Ordering::Relaxed) != fragment_completed {
        note_stall_cleared(slot, now);
    }
    if !candidate {
        discard_stall_observation(slot);
        return false;
    }
    let since = slot.since.load(Ordering::Relaxed);
    if since == 0 {
        slot.since.store(now, Ordering::Relaxed);
        return false;
    }
    now.saturating_sub(since) >= STALL_MS * 1_000_000
}

pub(crate) fn note_stall_cleared(slot: &StallSlot, now: u64) {
    let since = slot.since.swap(0, Ordering::Relaxed);
    let stage = slot.stage.swap(0, Ordering::Relaxed);
    if stage == 0 || since == 0 { return; }
    let lasted = now.saturating_sub(since);
    STALL_MAX_NS.fetch_max(lasted, Ordering::Relaxed);
    // "After" is temporal association, never proof of a causal rescue.
    // Failed sends are deliberately not credited as sent doorbells.
    if matches!(stage, 2 | 3) {
        STALL_CLEARED_AFTER_DOORBELL.fetch_add(1, Ordering::Relaxed);
    } else {
        STALL_CLEARED_UNAIDED.fetch_add(1, Ordering::Relaxed);
    }
}

/// A firmware recovery arrived: count and describe every stall still open.
pub(crate) fn note_recovery_for_stalls() -> OpenStalls {
    let mut open = OpenStalls::default();
    for (index, slot) in STALL_SLOT.iter().enumerate() {
        let stage = slot.stage.load(Ordering::Relaxed);
        if stage != 0 {
            STALL_AT_RECOVERY.fetch_add(1, Ordering::Relaxed);
            if open.count < open.pairs.len() {
                open.pairs[open.count] = (index as u8, stage as u8,
                    slot.fragment_completed.load(Ordering::Relaxed));
                open.count += 1;
            }
        }
    }
    open
}

/// Fixed-capacity summary of open stalls (no allocation on the event path).
#[derive(Default)]
pub(crate) struct OpenStalls {
    pub(crate) pairs: [(u8, u8, u64); 4],
    pub(crate) count: usize,
}

pub(crate) fn watch_grow_reply(producer_after: u32, buffer_slot: u32) {
    GROW_REPLY_AT.store(crate::g18::compat::timing_buffer::arch_counter(), Ordering::Relaxed);
    GROW_REPLY_WATCH.store(1 << 63 | u64::from(producer_after) << 32 | u64::from(buffer_slot),
        Ordering::Release);
}

/// A bounded log budget: the first `first` events, then one in `every`.
pub(crate) fn log_budget(counter: u64, first: u64, every: u64) -> bool {
    counter <= first || counter % every == 0
}

pub(crate) fn note_blocked(reason: ReclaimBlock) {
    RECLAIM_BLOCKED[reason.index()].fetch_add(1, Ordering::Relaxed);
}

// ---- control-ring shadow (bit1) -----------------------------------------

const CONTROL_SLOTS: usize = crate::g18::fw::initdata::CONTROL_RING_ENTRIES;
/// opcode | arg << 32, and CNTPCT at publication, per ring slot.
static CONTROL_RECORD: [AtomicU64; CONTROL_SLOTS] = [const { AtomicU64::new(0) }; CONTROL_SLOTS];
static CONTROL_PUBLISHED_AT: [AtomicU64; CONTROL_SLOTS] = [const { AtomicU64::new(0) }; CONTROL_SLOTS];
static LAST_ANNOUNCE: AtomicU64 = AtomicU64::new(0);
/// Re-announces whose oldest pending record was (not) announced after it
/// was published.
static REANNOUNCE_ANNOUNCED: AtomicU64 = AtomicU64::new(0);
static REANNOUNCE_UNANNOUNCED: AtomicU64 = AtomicU64::new(0);
/// Re-announces by the pending record's opcode (0..0x3f, larger folded to 0x3f).
static REANNOUNCE_BY_OPCODE: [AtomicU32; 64] = [const { AtomicU32::new(0) }; 64];

/// Called for every primary device-control publication (ring slot index).
#[inline(always)]
pub(crate) fn note_control_publish(slot: u32, opcode: u32, arg: u32) {
    if !enabled(CONTROL_TRACE) { return; }
    let index = slot as usize % CONTROL_SLOTS;
    CONTROL_RECORD[index].store(u64::from(opcode) | u64::from(arg) << 32, Ordering::Relaxed);
    CONTROL_PUBLISHED_AT[index].store(crate::g18::compat::timing_buffer::arch_counter(), Ordering::Relaxed);
}

/// Called for every EP 0x21 device-control announcement actually sent.
#[inline(always)]
pub(crate) fn note_control_announce() {
    if !enabled(CONTROL_TRACE) { return; }
    LAST_ANNOUNCE.store(crate::g18::compat::timing_buffer::arch_counter(), Ordering::Relaxed);
}

/// The oldest pending record at `consumer`, if traced: (opcode, arg, age
/// in counter ticks, announced after publication).
pub(crate) fn describe_pending(consumer: u32) -> Option<(u32, u32, u64, bool)> {
    if !enabled(CONTROL_TRACE) { return None; }
    let index = consumer as usize % CONTROL_SLOTS;
    let published = CONTROL_PUBLISHED_AT[index].load(Ordering::Relaxed);
    if published == 0 { return None; }
    let record = CONTROL_RECORD[index].load(Ordering::Relaxed);
    let now = crate::g18::compat::timing_buffer::arch_counter();
    let announced = LAST_ANNOUNCE.load(Ordering::Relaxed) >= published;
    let opcode = record as u32;
    let counter = if announced { &REANNOUNCE_ANNOUNCED } else { &REANNOUNCE_UNANNOUNCED };
    counter.fetch_add(1, Ordering::Relaxed);
    REANNOUNCE_BY_OPCODE[(opcode as usize).min(63)].fetch_add(1, Ordering::Relaxed);
    Some((opcode, (record >> 32) as u32, now.saturating_sub(published), announced))
}

// ---- parameter ------------------------------------------------------------

struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

/// Accepts "reset" or a decimal/0x-hex mask of known bits.
fn parse_mask(text: &str) -> Option<u32> {
    let parsed = match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => text.parse::<u32>().ok(),
    }?;
    (parsed & !ALL_BITS == 0).then_some(parsed)
}

unsafe extern "C" fn mask_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    // SAFETY: the parameter core supplies a NUL-terminated string.
    let Ok(text) = unsafe { core::ffi::CStr::from_ptr(value) }.to_str() else { return -22; };
    let text = text.trim();
    if text == "reset" {
        for counter in [&RECLAIM_ATTEMPTS, &RECLAIM_PAIRS, &RECLAIM_TRACKERS, &RECLAIM_RELEASED,
                        &REANNOUNCE_ANNOUNCED, &REANNOUNCE_UNANNOUNCED, &GROW_INGRESS,
                        &GROW_DEFERRED, &GROW_PARKED_AT_RETURN, &GROW_REKICKS, &GROW_REPLIED,
                        &GROW_REPLY_CONSUMED, &STALL_POLLS, &STALL_DETECTED, &STALL_DEFERRED,
                        &STALL_DOORBELLS, &STALL_FRAGMENT_DOORBELLS, &STALL_CLEARED_AFTER_DOORBELL,
                        &STALL_CLEARED_UNAIDED, &STALL_AT_RECOVERY, &STALL_MAX_NS] {
            counter.store(0, Ordering::Relaxed);
        }
        for counter in RECLAIM_BLOCKED.iter() { counter.store(0, Ordering::Relaxed); }
        for counter in REANNOUNCE_BY_OPCODE.iter() { counter.store(0, Ordering::Relaxed); }
        return 0;
    }
    let Some(mask) = parse_mask(text) else { return -22; };
    MASK.store(mask, Ordering::Release);
    0
}

unsafe extern "C" fn mask_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes, used: 0 };
    let _ = write!(out, "{:#x} reclaim attempts={} pairs={} trackers={} released={} blocked:",
        MASK.load(Ordering::Relaxed), RECLAIM_ATTEMPTS.load(Ordering::Relaxed),
        RECLAIM_PAIRS.load(Ordering::Relaxed), RECLAIM_TRACKERS.load(Ordering::Relaxed),
        RECLAIM_RELEASED.load(Ordering::Relaxed));
    for (name, counter) in ReclaimBlock::NAMES.iter().zip(RECLAIM_BLOCKED.iter()) {
        let _ = write!(out, " {}={}", name, counter.load(Ordering::Relaxed));
    }
    let _ = write!(out, " tvb-grow ingress={} replied={} reply-consumed={} deferred={} parked-at-return={} rekicks={}",
        GROW_INGRESS.load(Ordering::Relaxed), GROW_REPLIED.load(Ordering::Relaxed),
        GROW_REPLY_CONSUMED.load(Ordering::Relaxed),
        GROW_DEFERRED.load(Ordering::Relaxed), GROW_PARKED_AT_RETURN.load(Ordering::Relaxed),
        GROW_REKICKS.load(Ordering::Relaxed));
    let _ = write!(out, " stall polls={} detected={} deferred={} doorbells={} fragment-doorbells={} cleared-after-doorbell={} cleared-unaided={} at-recovery={} max-ms={}",
        STALL_POLLS.load(Ordering::Relaxed), STALL_DETECTED.load(Ordering::Relaxed),
        STALL_DEFERRED.load(Ordering::Relaxed), STALL_DOORBELLS.load(Ordering::Relaxed),
        STALL_FRAGMENT_DOORBELLS.load(Ordering::Relaxed),
        STALL_CLEARED_AFTER_DOORBELL.load(Ordering::Relaxed),
        STALL_CLEARED_UNAIDED.load(Ordering::Relaxed), STALL_AT_RECOVERY.load(Ordering::Relaxed),
        STALL_MAX_NS.load(Ordering::Relaxed) / 1_000_000);
    let _ = write!(out, " reannounce announced={} unannounced={} by-opcode:",
        REANNOUNCE_ANNOUNCED.load(Ordering::Relaxed), REANNOUNCE_UNANNOUNCED.load(Ordering::Relaxed));
    for (opcode, counter) in REANNOUNCE_BY_OPCODE.iter().enumerate() {
        let count = counter.load(Ordering::Relaxed);
        if count != 0 { let _ = write!(out, " {:#x}={}", opcode, count); }
    }
    let _ = writeln!(out);
    out.used as core::ffi::c_int
}

static MASK_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(mask_set), get: Some(mask_get), free: None,
};
#[link_section = "__param"]
#[used]
static MASK_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_recovery_path"
    } else { c"aurora.g17p_recovery_path" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&MASK_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
