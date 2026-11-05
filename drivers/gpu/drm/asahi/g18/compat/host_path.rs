// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! v84 host refill-path controls.
//!
//! `g17p_host_path` (root-only sysfs, runtime-writable bitmask):
//!   bit0 EARLY_PUBLISH: a render whose submit ioctl has no unsignalled input,
//!        prefix or firmware dependency, whose queue has no job still inside
//!        the DRM scheduler, and for which a physical lease and a preparation
//!        epoch are available without waiting, is published (doorbell rung)
//!        from the submit ioctl. The DRM scheduler job is still created and
//!        pushed; its run_job returns the already-published hardware fence, so
//!        timeouts, credits, teardown and cancellation keep their existing
//!        owners. Anything else takes the unchanged scheduler path.
//!   bit1 FEED_PERF: GPU-feed CPU performance request (see below).
//!   bit2 EARLY_SHALLOW (v87): bit0's early publication, taken only while the
//!        queue has fewer than `g17p_early_depth` (default 2) of its own renders
//!        outstanding, counting this one. The count is the queue's existing
//!        pruned `render_frontier` (unsignalled hardware fences). A deeper
//!        queue takes exactly the v84 scheduler path (bit0 off).
//!   bit3 STATE_WORD (v87): publication and event-service recovery gates read
//!        only the firmware recovery *state* word (the only field they test)
//!        instead of the three-word handshake snapshot. Same predicate.
//!   bit4 SETTLE_ONCE (v87): within one Runtime hold of the publish
//!        reservation (warm checks + restage take), test the open-recovery
//!        gate once instead of once per helper. The final gate immediately
//!        before the doorbell (submit_translated_render) is unchanged.
//!   bit5 RING_CACHE (v87): firmware-event ring drains skip re-reading the two
//!        host-authored main-config ring pointers once they have been checked
//!        equal to their derivation; revalidated every RING_REVALIDATE drains.
//!   bit6 WORKER_TAIL (v87): event-worker bookkeeping after the drain (DVFS
//!        parity, teardown service, back-pressure, recovery/poll/requeue
//!        decisions) runs under one Runtime acquisition instead of six.
//!   bit7 OBS_SKIP (v87): the render observation loop is skipped when, under
//!        the drain's own Runtime hold, no pending slot has both type-1 masks.
//!   bit8 VM_STATUS_MIRROR (v87): GET_PARAMS group 3 registration of a
//!        userspace word that mirrors the sticky VM status (see g17_status).
//!   bit9 VALIDATE_PROOF (v91): the submit ioctl's UAPI address validation
//!        (one GPUVM exec lock per range before) records the ranges during
//!        translation and proves them with ONE exec lock, or with none when
//!        the queue's previous proof covers them at the VM's unchanged
//!        mapping-validation epoch. Any failed proof reruns the unchanged
//!        per-range translation, so errors are exactly those of v90.
//!   bit10 PENDING_EXACT (v91): the ioctl's pending-command vector grows to
//!        the commands actually present instead of preallocating 64 x ~1.2 KB
//!        (a kmalloc_large page allocation and free on every submission).
//!   bit11 WORD_STORES (v91): descriptor/record encoders store naturally
//!        aligned 16/32/64-bit fields with one store instead of byte stores
//!        (identical bytes; fewer write-combined stores).
//!   bit12 PCORE_NUDGE (v92, DIAGNOSTIC, default off): while the feed-perf
//!        request is applied, a submit ioctl that runs on a lower-capacity CPU
//!        moves its own thread onto the highest-capacity cluster once, by a
//!        transient affinity restriction that is restored immediately; later
//!        wakeups then prefer that (idle) previous CPU. It transiently rewrites
//!        a user-visible attribute, so it exists only to measure the value of
//!        big-core placement (what CLPC's P-core recommendation and, on Linux,
//!        a UCLAMP_TASK kernel would provide) before any kernel change.
//!   bit13 FW_DEPS (v93, default off until lab-qualified): an input syncobj
//!        whose fence (after unwrapping timeline chain links) consists only of
//!        already-signalled fences and aggregates of this same queue's recent
//!        fully enqueued ioctls is not waited on the host. Instead the
//!        command's logical render/compute prefixes are raised to cover those
//!        ioctls, which the existing prefix machinery turns into firmware
//!        barrier tokens (render->render at publish, compute<->render through
//!        publication fences plus tokens, compute->compute through the QID's
//!        implicit parent), and each matched aggregate is status-checked at
//!        publication. Anything else (another queue, another VM or client, a
//!        foreign or errored fence, a fence array, an ioctl that did not fully
//!        enqueue, a chain deeper than FW_DEP_LINKS_MAX) keeps the unchanged
//!        host wait for that whole sync. Same VM by construction, so a faulted
//!        predecessor's existing fail-outstanding/quarantine paths release the
//!        firmware waiter.
//!   bit14 AGG_FIRST (v93, default off until lab-qualified): a completion
//!        signals the ioctl's user-visible aggregate fence before its per-
//!        command hardware fence (whose inline callbacks are drm_sched job-done
//!        bookkeeping). Timestamp writes, sticky status, lease release and
//!        VM-job release still precede both: userspace reads timestamps and
//!        recycles BOs/VAs as soon as the aggregate signals.
//!   bit15 EARLY_DEPS (v93, default off until lab-qualified): early
//!        publication also admits a render whose host dependencies are all
//!        already signalled and whose remaining dependencies are firmware
//!        status checks (FW_DEPS aggregates, prefix hardware fences); it still
//!        declines any unsignalled host dependency, a scheduler backlog or an
//!        unavailable lease, so the ioctl never waits for a dependency.
//!   bit16 WAIT_UTIL_HINT: tag accepted feeder work for 8 ms while the
//!        existing starvation detector requests CPU performance. A blocking
//!        syncobj wait uses a scheduler capacity hint of 512 only until it
//!        returns. User clamps, cgroup limits and CPU affinity remain intact.
//!        Default on; clearing this bit is the same-kernel A/B control.
//!
//! v92 per-cluster feed levels: `g17p_feed_perf_pct_p` / `g17p_feed_perf_pct_e`
//! (0 = inherit `g17p_feed_perf_pct`) set the floor of the highest-capacity
//! (P) and the other (E) cpufreq policies separately while the GPU is starved.
//! Native IOGPU reports every command buffer to CLPC (IOPerfControl workSubmit
//! at IOGPUBlockFence::init, workBegin, workEnd with GPU data), and CLPC raises
//! the submitting thread group's CPU performance only while it gates the GPU.
//! The submitting thread and the ioctl run on E cores ~90% of the time and the
//! completion worker on CPU0 (P), so the two clusters bound different parts
//! of the host loop and need not share one level.
//!
//! GPU-feed CPU performance request. Native analogue: IOGPU reports GPU work
//! to CLPC (`IOGPUFenceMachine::notifyCLPCIOPerfControl`) so CPU performance
//! follows a GPU-bound feed loop. On Linux a dma-fence waiter's sleep is not
//! iowait, so schedutil sees ~10% utilisation and keeps both clusters at their
//! lowest OPP while the GPU idles waiting for that same CPU work (measured v81:
//! 91-99.7% lowest OPP, GPU idle 47-52% at depth 1/2).
//!
//! Detector (hot path, atomics only): every retired render reports its GPU pass
//! envelope. Decayed sums track GPU busy time and GPU idle time in short gaps
//! (0 < gap < MAX_REFILL: the GPU waited for the host refill path). A long gap
//! (paced client or idle desktop) resets the loop streak. The request is armed
//! while a closed feed loop keeps the GPU idle >= 20% of the time; it lingers
//! LINGER after the last such pass and is then withdrawn. GPU-bound
//! (overlapped) streams never arm it and no request exists at idle.
//! Actuator (process context, one work item): a cpufreq freq_qos MIN request on
//! each policy at `g17p_feed_perf_pct` percent of that policy's maximum.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam};

pub(crate) const EARLY_PUBLISH: u32 = 1 << 0;
pub(crate) const FEED_PERF: u32 = 1 << 1;
pub(crate) const EARLY_SHALLOW: u32 = 1 << 2;
pub(crate) const STATE_WORD: u32 = 1 << 3;
pub(crate) const SETTLE_ONCE: u32 = 1 << 4;
pub(crate) const RING_CACHE: u32 = 1 << 5;
pub(crate) const WORKER_TAIL: u32 = 1 << 6;
pub(crate) const OBS_SKIP: u32 = 1 << 7;
pub(crate) const VM_STATUS_MIRROR: u32 = 1 << 8;
pub(crate) const VALIDATE_PROOF: u32 = 1 << 9;
pub(crate) const PENDING_EXACT: u32 = 1 << 10;
pub(crate) const WORD_STORES: u32 = 1 << 11;
pub(crate) const PCORE_NUDGE: u32 = 1 << 12;
pub(crate) const FW_DEPS: u32 = 1 << 13;
pub(crate) const AGG_FIRST: u32 = 1 << 14;
pub(crate) const EARLY_DEPS: u32 = 1 << 15;
pub(crate) const WAIT_UTIL_HINT: u32 = 1 << 16;
/// Read only producer cursors when no completion/retirement decision is made.
pub(crate) const PRODUCER_READS: u32 = 1 << 17;
/// Count the retained graph as a PB owner, preserving its binding token.
pub(crate) const PB_OWNER_REF: u32 = 1 << 18;
/// Native pool sharing. Captured at logical-context creation, never changes a
/// live pool. Clearing this bit is a same-kernel per-pair allocation control.
pub(crate) const SHARED_FLIST: u32 = 1 << 19;
const ALL_BITS: u32 = (1 << 20) - 1;

/// Production default for this candidate. v84-final had FEED_PERF only; v87
/// added bits 2-8 and v91 bits 9-11. `g17p_host_path=0x1fe` is exactly the
/// v90 behaviour and `=2` the v84-final behaviour, for same-boot A/B.
/// v93 bits 13-15 stay off. The scoped wait hint adds bit16 to the default;
/// `0xffe` disables only that hint for same-kernel comparison. `0xeffe`
/// enables the older three probes without the scoped hint.
// Producer-only reads extend 0x10ffe to 0x30ffe; clearing bit17 restores
// the original full snapshots for an otherwise identical comparison.
const DEFAULT_MASK: u32 = FEED_PERF | EARLY_SHALLOW | STATE_WORD | SETTLE_ONCE
    | RING_CACHE | WORKER_TAIL | OBS_SKIP | VM_STATUS_MIRROR
    | VALIDATE_PROOF | PENDING_EXACT | WORD_STORES | WAIT_UTIL_HINT | PRODUCER_READS | PB_OWNER_REF | SHARED_FLIST;
static MASK: AtomicU32 = AtomicU32::new(DEFAULT_MASK);
static FEED_PCT: AtomicU32 = AtomicU32::new(50);
/// v92 per-cluster overrides of FEED_PCT (0 = inherit).
static FEED_PCT_P: AtomicU32 = AtomicU32::new(0);
static FEED_PCT_E: AtomicU32 = AtomicU32::new(0);
/// EARLY_SHALLOW depth: early publication while fewer than this many renders
/// of the queue are outstanding, counting the one being submitted.
static EARLY_DEPTH: AtomicU32 = AtomicU32::new(2);
const EARLY_DEPTH_MAX: u32 = 64;
/// RING_CACHE: re-read and re-check the main-config ring pointers on at
/// least every this many drains of one role.
pub(crate) const RING_REVALIDATE: u32 = 256;

#[inline(always)]
pub(crate) fn enabled(bit: u32) -> bool { MASK.load(Ordering::Relaxed) & bit != 0 }

/// v87 counters (relaxed; observability only, printed by `g17p_host_path`).
pub(crate) static EARLY_TAKEN: AtomicU64 = AtomicU64::new(0);
pub(crate) static SETTLE_SKIPPED: AtomicU64 = AtomicU64::new(0);
pub(crate) static RING_CACHE_HITS: AtomicU64 = AtomicU64::new(0);
pub(crate) static TAIL_MERGED: AtomicU64 = AtomicU64::new(0);
pub(crate) static OBS_SKIPPED: AtomicU64 = AtomicU64::new(0);
pub(crate) static MIRRORS: AtomicU64 = AtomicU64::new(0);
pub(crate) static PROOF_HITS: AtomicU64 = AtomicU64::new(0);
pub(crate) static PROOF_BATCHES: AtomicU64 = AtomicU64::new(0);
pub(crate) static PROOF_FALLBACKS: AtomicU64 = AtomicU64::new(0);
pub(crate) static NUDGES: AtomicU64 = AtomicU64::new(0);
pub(crate) static NUDGE_FAILS: AtomicU64 = AtomicU64::new(0);
/// v93 FW_DEPS input syncs: converted to firmware prefixes, kept as host
/// waits, or dropped because already signalled without error.
pub(crate) static FW_DEP_CONVERTED: AtomicU64 = AtomicU64::new(0);
pub(crate) static FW_DEP_HOST: AtomicU64 = AtomicU64::new(0);
pub(crate) static FW_DEP_SATISFIED: AtomicU64 = AtomicU64::new(0);
/// v93 EARLY_DEPS: early publications that carried dependencies.
pub(crate) static EARLY_WITH_DEPS: AtomicU64 = AtomicU64::new(0);
/// v93 AGG_FIRST completions (aggregate member completed before hw fence).
pub(crate) static AGG_FIRST_TAKEN: AtomicU64 = AtomicU64::new(0);

#[inline(always)]
pub(crate) fn count(counter: &AtomicU64) { counter.fetch_add(1, Ordering::Relaxed); }

/// EARLY_SHALLOW gate. `outstanding_before` is the queue's unsignalled
/// renders excluding this one; admit while the total stays within `depth`.
#[inline(always)]
pub(crate) const fn early_shallow_admits(outstanding_before: usize, depth: u32) -> bool {
    outstanding_before < depth as usize
}

/// Whether a render submission may attempt early publication: bit0 alone
/// (v84, any depth) or bit2 within the depth. Dependencies always decline.
/// With v93 EARLY_DEPS the caller passes whether any host dependency is
/// still unsignalled instead of whether one exists.
#[inline(always)]
pub(crate) fn early_publish_allowed(outstanding_before: usize, has_dependencies: bool) -> bool {
    if has_dependencies { return false; }
    let mask = MASK.load(Ordering::Relaxed);
    if mask & EARLY_PUBLISH != 0 { return true; }
    if mask & EARLY_SHALLOW == 0 { return false; }
    // No counter here: a declined (deep) submission must stay the v84 path.
    early_shallow_admits(outstanding_before, EARLY_DEPTH.load(Ordering::Relaxed))
}

/// v93 FW_DEPS: longest timeline chain walked per input sync; ring size.
pub(crate) const FW_DEP_LINKS_MAX: u32 = 16;
pub(crate) const FW_DEP_RECENT: usize = 8;

/// One unwrapped input-fence leaf, as seen by `fw_dep_step`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FwDepLeaf {
    /// Signalled without error: satisfies nothing further.
    Signalled,
    /// Signalled with an error: the host path must consume it.
    Failed,
    /// Unsignalled; identity is the fence address (compared, never used).
    Pending(usize),
    /// Nested container, fence array, missing leaf or over-long chain.
    Unsupported,
}

/// A recent fully enqueued ioctl of this queue: aggregate identity and the
/// inclusive logical sequence ranges of its commands (0 = none of that kind).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FwDepRecent {
    pub(crate) identity: usize,
    pub(crate) render_last: u64,
    pub(crate) compute_last: u64,
}

/// Accumulated firmware expression of one input sync.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FwDepResolution {
    /// Highest render / compute logical sequence that must retire first.
    pub(crate) render_through: u64,
    pub(crate) compute_through: u64,
    /// Bit i: recent entry i supplied a pending leaf (status-checked later).
    pub(crate) used: u32,
    /// Leaves seen (Signalled included); 0 means an empty walk.
    pub(crate) leaves: u32,
}

/// Fold one leaf into `resolution`. False means the sync is not provably
/// expressible in firmware and must keep its original host wait.
pub(crate) fn fw_dep_step(
    resolution: &mut FwDepResolution, leaf: FwDepLeaf, recent: &[Option<FwDepRecent>],
) -> bool {
    resolution.leaves += 1;
    let identity = match leaf {
        FwDepLeaf::Signalled => return true,
        FwDepLeaf::Failed | FwDepLeaf::Unsupported => return false,
        FwDepLeaf::Pending(identity) => identity,
    };
    if identity == 0 { return false; }
    let Some(index) = recent.iter().position(|entry|
        entry.is_some_and(|entry| entry.identity == identity)) else { return false; };
    let Some(entry) = recent[index] else { return false; };
    resolution.render_through = resolution.render_through.max(entry.render_last);
    resolution.compute_through = resolution.compute_through.max(entry.compute_last);
    resolution.used |= 1u32 << index;
    true
}

/// Raise an optional logical prefix (`None` = independent) to cover `through`.
#[inline(always)]
pub(crate) const fn fw_dep_raise(prefix: Option<u64>, through: u64) -> Option<u64> {
    if through == 0 { return prefix; }
    match prefix {
        Some(limit) if limit >= through => Some(limit),
        _ => Some(through),
    }
}

/// RING_CACHE decision for one role. `entry` is (cursor, ring, drains left)
/// of the last successful check; `expected` is this drain's derivation.
/// Returns true when the two pointer reads may be skipped (and consumes one
/// drain of the budget); false means read and check them, then `ring_checked`.
#[inline(always)]
pub(crate) fn ring_cache_hit(entry: &mut (u64, u64, u32), expected: (u64, u64), enabled: bool) -> bool {
    if !enabled || entry.2 == 0 || entry.0 != expected.0 || entry.1 != expected.1 {
        return false;
    }
    entry.2 -= 1;
    true
}

/// Record a successful full pointer check for `ring_cache_hit`.
#[inline(always)]
pub(crate) fn ring_checked(entry: &mut (u64, u64, u32), checked: (u64, u64)) {
    *entry = (checked.0, checked.1, RING_REVALIDATE);
}

// ---------------------------------------------------------------------------
// Feed-perf detector state. GPU pass stamps are 24 MHz firmware ticks.
const MAX_REFILL_TICKS: u64 = 24_000 * 2; // 2 ms: longer gaps are paced/idle
const STREAK_ON: u32 = 8;
const STREAK_MAX: u32 = 64;
const LINGER_NS: u64 = 8_000_000;
const POLL_MS: i64 = 2;

static LAST_END: AtomicU64 = AtomicU64::new(0);
/// Exponentially decayed (1/8 per pass) GPU idle and busy ticks over recent
/// passes. Idle counts only short gaps: the GPU waited for the host.
static IDLE_EWMA: AtomicU64 = AtomicU64::new(0);
static BUSY_EWMA: AtomicU64 = AtomicU64::new(0);
/// Consecutive passes without a long (paced) gap.
static STREAK: AtomicU32 = AtomicU32::new(0);
static UNTIL_NS: AtomicU64 = AtomicU64::new(0);
/// A worker owns the actuator (running or queued).
static ACTIVE: AtomicBool = AtomicBool::new(false);
/// The detector asked for a worker; the event worker queues it.
static PENDING: AtomicBool = AtomicBool::new(false);
static UNBINDING: AtomicBool = AtomicBool::new(false);
static STARVED: AtomicU64 = AtomicU64::new(0);
static FED: AtomicU64 = AtomicU64::new(0);
static BOOSTS: AtomicU64 = AtomicU64::new(0);
static BOOSTED_NS: AtomicU64 = AtomicU64::new(0);

fn now_ns() -> u64 {
    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get() as u64
}

fn decay_add(sum: &AtomicU64, add: u64) -> u64 {
    let mut next = 0;
    let _ = sum.fetch_update(Ordering::AcqRel, Ordering::Acquire, |v| {
        next = v - v / 8 + add;
        Some(next)
    });
    next
}

/// Hot path: one retired render's GPU pass envelope [start, end] (24 MHz).
/// Atomics only. Concurrent callers may interleave; the estimate tolerates it.
pub(crate) fn note_render_pass(start: u64, end: u64) {
    if !enabled(FEED_PERF) || end <= start { return; }
    let previous = LAST_END.fetch_max(end, Ordering::AcqRel);
    let (idle, busy, paced) = if previous == 0 {
        (0, end - start, true)
    } else if start >= previous {
        let gap = start - previous;
        if gap < MAX_REFILL_TICKS { (gap, end - start, false) } else { (0, end - start, true) }
    } else if end > previous {
        (0, end - previous, false) // overlapped: only the new busy tail counts
    } else {
        (0, 0, false)
    };
    let idle_sum = decay_add(&IDLE_EWMA, idle);
    let busy_sum = decay_add(&BUSY_EWMA, busy);
    let streak = if paced {
        STREAK.store(0, Ordering::Release);
        0
    } else {
        STREAK.fetch_update(Ordering::AcqRel, Ordering::Acquire,
            |s| Some((s + 1).min(STREAK_MAX))).unwrap_or(0) + 1
    };
    // Host-starved feed loop: a closed loop (no paced gaps) in which the GPU
    // is idle at least 20% of the time waiting for the host.
    if streak >= STREAK_ON && idle_sum.saturating_mul(4) >= busy_sum {
        STARVED.fetch_add(1, Ordering::Relaxed);
        UNTIL_NS.store(now_ns() + LINGER_NS, Ordering::Release);
        if !ACTIVE.swap(true, Ordering::AcqRel) {
            PENDING.store(true, Ordering::Release);
        }
    } else {
        FED.fetch_add(1, Ordering::Relaxed);
    }
}

/// Called by the firmware-event worker after each pass (process context).
pub(crate) fn take_pending() -> bool { PENDING.swap(false, Ordering::AcqRel) }

// ---------------------------------------------------------------------------
// Actuator. Only the single feed-perf work item (and unbind after that item is
// disabled) touches REQS, so accesses are serialised by construction.
const MAX_POLICIES: usize = 8;
struct Reqs {
    policy: [AtomicPtr<bindings::cpufreq_policy>; MAX_POLICIES],
    max_khz: [AtomicU32; MAX_POLICIES],
    req: core::cell::UnsafeCell<[bindings::freq_qos_request; MAX_POLICIES]>,
    count: AtomicU32,
    registered: AtomicBool,
    applied_pct: AtomicU32,
    /// v92: this policy has the highest maximum frequency (the P cluster).
    big: [AtomicBool; MAX_POLICIES],
    /// v92: value last written to each request (skips redundant updates).
    applied_khz: [AtomicU32; MAX_POLICIES],
    /// v92: CPUs (bit per CPU id, ids < 64) of the highest-capacity policy.
    big_cpus: AtomicU64,
}
// SAFETY: `req` is only accessed by the serialised actuator (see above).
unsafe impl Sync for Reqs {}
static REQS: Reqs = Reqs {
    policy: [const { AtomicPtr::new(core::ptr::null_mut()) }; MAX_POLICIES],
    max_khz: [const { AtomicU32::new(0) }; MAX_POLICIES],
    // SAFETY: freq_qos_request is plain C data; all-zero is its unregistered state.
    req: core::cell::UnsafeCell::new(unsafe { core::mem::zeroed() }),
    count: AtomicU32::new(0),
    registered: AtomicBool::new(false),
    applied_pct: AtomicU32::new(0),
    big: [const { AtomicBool::new(false) }; MAX_POLICIES],
    applied_khz: [const { AtomicU32::new(0) }; MAX_POLICIES],
    big_cpus: AtomicU64::new(0),
};

/// v92: the floor (kHz, 0 = withdrawn) one policy gets for request level
/// `base` percent. A big policy uses the P override, others the E override;
/// an override of 0 inherits `base`. Level 0 always withdraws.
pub(crate) const fn cluster_floor_khz(max_khz: u32, big: bool, base: u32, pct_p: u32, pct_e: u32) -> u32 {
    if base == 0 { return 0; }
    let over = if big { pct_p } else { pct_e };
    let pct = if over != 0 { over } else { base };
    let pct = if pct > 100 { 100 } else { pct };
    ((max_khz as u64 * pct as u64) / 100) as u32
}

/// v92: index of the policy with the highest maximum frequency (first on a
/// tie), or None when there are fewer than two distinct maxima (no big/little
/// distinction to make).
pub(crate) fn big_policy(max_khz: &[u32]) -> Option<usize> {
    let top = *max_khz.iter().max()?;
    if max_khz.iter().all(|&k| k == top) { return None; }
    max_khz.iter().position(|&k| k == top)
}

fn register() -> bool {
    if REQS.registered.load(Ordering::Acquire) { return true; }
    // SAFETY: plain read of a kernel global set at boot.
    let cpus = unsafe { bindings::nr_cpu_ids };
    let mut count = 0usize;
    let mut policy_cpus = [0u64; MAX_POLICIES];
    for cpu in 0..cpus {
        // SAFETY: cpufreq_cpu_get tolerates any cpu index and takes a reference.
        let policy = unsafe { bindings::cpufreq_cpu_get(cpu) };
        if policy.is_null() { continue; }
        let known = (0..count).position(|i| REQS.policy[i].load(Ordering::Relaxed) == policy);
        if let (Some(index), true) = (known, cpu < 64) { policy_cpus[index] |= 1u64 << cpu; }
        if known.is_none() && count < MAX_POLICIES {
            // SAFETY: `policy` is referenced; the request slot is unregistered
            // and lives in static storage for the module lifetime.
            let ret = unsafe {
                let slot = (*REQS.req.get()).as_mut_ptr().add(count);
                bindings::freq_qos_add_request(&mut (*policy).constraints, slot,
                    bindings::freq_qos_req_type_FREQ_QOS_MIN,
                    bindings::FREQ_QOS_MIN_DEFAULT_VALUE as i32)
            };
            if ret >= 0 {
                REQS.policy[count].store(policy, Ordering::Relaxed);
                // SAFETY: referenced policy; cpuinfo is immutable after init.
                REQS.max_khz[count].store(unsafe { (*policy).cpuinfo.max_freq }, Ordering::Relaxed);
                REQS.applied_khz[count].store(bindings::FREQ_QOS_MIN_DEFAULT_VALUE as u32, Ordering::Relaxed);
                if cpu < 64 { policy_cpus[count] |= 1u64 << cpu; }
                count += 1;
            }
        }
        // SAFETY: balances cpufreq_cpu_get above. The request keeps its own
        // pointer to the policy's constraints, as cpufreq cooling devices do.
        unsafe { bindings::cpufreq_cpu_put(policy) };
    }
    let mut maxima = [0u32; MAX_POLICIES];
    for (i, slot) in maxima.iter_mut().enumerate().take(count) {
        *slot = REQS.max_khz[i].load(Ordering::Relaxed);
    }
    let big = big_policy(&maxima[..count]);
    for i in 0..count { REQS.big[i].store(Some(i) == big, Ordering::Relaxed); }
    // CPU ids >= 64 are not represented; the nudge then stays disabled.
    REQS.big_cpus.store(match big { Some(i) if cpus <= 64 => policy_cpus[i], _ => 0 }, Ordering::Relaxed);
    REQS.count.store(count as u32, Ordering::Release);
    REQS.registered.store(count != 0, Ordering::Release);
    count != 0
}

fn apply(pct: u32) {
    if pct != 0 && !register() { return; }
    if !REQS.registered.load(Ordering::Acquire) { return; }
    let (pct_p, pct_e) = (FEED_PCT_P.load(Ordering::Relaxed), FEED_PCT_E.load(Ordering::Relaxed));
    for i in 0..REQS.count.load(Ordering::Acquire) as usize {
        let max = REQS.max_khz[i].load(Ordering::Relaxed);
        let floor = cluster_floor_khz(max, REQS.big[i].load(Ordering::Relaxed), pct, pct_p, pct_e);
        let value = if floor == 0 { bindings::FREQ_QOS_MIN_DEFAULT_VALUE as u32 } else { floor };
        // v92: per-cluster levels may change while applied; write on change only.
        if REQS.applied_khz[i].swap(value, Ordering::Relaxed) == value { continue; }
        // SAFETY: slot `i` was registered by `register` and is only touched here.
        unsafe { bindings::freq_qos_update_request((*REQS.req.get()).as_mut_ptr().add(i), value as i32) };
    }
    let before = REQS.applied_pct.swap(pct, Ordering::Relaxed);
    if before == pct { return; }
    let now = now_ns();
    if before == 0 && pct != 0 {
        BOOSTS.fetch_add(1, Ordering::Relaxed);
        BOOST_SINCE.store(now, Ordering::Relaxed);
    } else if before != 0 && pct == 0 {
        BOOSTED_NS.fetch_add(now.saturating_sub(BOOST_SINCE.load(Ordering::Relaxed)), Ordering::Relaxed);
    }
    crate::g18::compat::e2e::record(crate::g18::compat::e2e::K_FEED_PERF, pct, STREAK.load(Ordering::Relaxed) as u64,
        [before as u64, STARVED.load(Ordering::Relaxed), FED.load(Ordering::Relaxed), 0, 0]);
}
static BOOST_SINCE: AtomicU64 = AtomicU64::new(0);

/// v92 PCORE_NUDGE (diagnostic, opt-in): see the module documentation.
/// Called at the start of the submit ioctl (process context, sleepable, no
/// driver lock held). Only while the feed request is applied, only from a CPU
/// outside the big cluster, and only when the thread may run there at all.
pub(crate) fn nudge_feeding_thread() {
    if !enabled(PCORE_NUDGE) || REQS.applied_pct.load(Ordering::Relaxed) == 0 { return; }
    let big = REQS.big_cpus.load(Ordering::Relaxed);
    // SAFETY: raw_smp_processor_id has no preconditions; a stale value only
    // makes this diagnostic skip or nudge once more.
    let cpu = unsafe { bindings::raw_smp_processor_id() } as u32;
    if big == 0 || cpu >= 64 || big & (1u64 << cpu) != 0 { return; }
    let task = kernel::task::Task::current_raw();
    // SAFETY: `task` is the current task, alive for this call. Its effective
    // mask changes only through set_cpus_allowed_ptr; a concurrent
    // sched_setaffinity() from another thread of this process during these two
    // calls could be overwritten by the restore (documented diagnostic limit).
    let saved = unsafe { core::ptr::read(core::ptr::addr_of!((*task).cpus_mask)) };
    let mut target = saved;
    target.bits[0] &= big as usize;
    for word in target.bits.iter_mut().skip(1) { *word = 0; }
    if target.bits[0] == 0 { return; }
    // SAFETY: valid current task and masks; both calls may sleep (ioctl context).
    let moved = unsafe { bindings::set_cpus_allowed_ptr(task, &target) };
    if moved != 0 { count(&NUDGE_FAILS); return; }
    // SAFETY: as above; restores the exact effective mask read before.
    let restored = unsafe { bindings::set_cpus_allowed_ptr(task, &saved) };
    if restored != 0 { count(&NUDGE_FAILS); } else { count(&NUDGES); }
}

/// Capacity hint for a client actively replenishing a CPU-starved GPU.
/// A per-file expiry and a scoped syncobj wait keep this out of idle/non-GPU
/// work. No task attribute, affinity or saved userspace clamp is rewritten.
pub(crate) fn feeder_wait_util_min() -> u32 {
    if !cfg!(CONFIG_UCLAMP_TASK) { return 0; }
    if enabled(WAIT_UTIL_HINT) && wanted()
        && REQS.applied_pct.load(Ordering::Relaxed) != 0 { 512 } else { 0 }
}

fn wanted() -> bool {
    enabled(FEED_PERF) && !UNBINDING.load(Ordering::Acquire)
        && now_ns() < UNTIL_NS.load(Ordering::Acquire)
}

/// Body of the feed-perf work item. Holds the request while wanted, polling
/// the linger deadline; withdraws it and exits otherwise.
pub(crate) fn run_actuator() {
    loop {
        if wanted() {
            apply(FEED_PCT.load(Ordering::Relaxed).clamp(1, 100));
            kernel::time::delay::fsleep(kernel::time::Delta::from_millis(POLL_MS));
            continue;
        }
        apply(0);
        ACTIVE.store(false, Ordering::SeqCst);
        // Close the race with a detector that saw ACTIVE still set.
        if wanted() && !ACTIVE.swap(true, Ordering::AcqRel) { continue; }
        return;
    }
}

/// Unbind, before the work item is disabled: makes a running actuator exit.
pub(crate) fn begin_shutdown() { UNBINDING.store(true, Ordering::Release); }

/// Unbind: after the work item is disabled, withdraw and remove requests.
pub(crate) fn shutdown() {
    UNBINDING.store(true, Ordering::Release);
    if !REQS.registered.load(Ordering::Acquire) { return; }
    for i in 0..REQS.count.load(Ordering::Acquire) as usize {
        // SAFETY: registered slot; the actuator work item is disabled.
        unsafe { bindings::freq_qos_remove_request((*REQS.req.get()).as_mut_ptr().add(i)) };
        REQS.policy[i].store(core::ptr::null_mut(), Ordering::Relaxed);
    }
    REQS.registered.store(false, Ordering::Release);
}

// ---------------------------------------------------------------------------
struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

fn parse_u32(value: *const core::ffi::c_char) -> Option<u32> {
    if value.is_null() { return None; }
    let mut command = [0u8; 16];
    let mut len = 0;
    while len < command.len() {
        // SAFETY: NUL-terminated parameter string; at most 16 bytes examined.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { break; }
        command[len] = byte;
        len += 1;
    }
    if len == command.len() { return None; }
    let text = core::str::from_utf8(&command[..len]).ok()?.trim();
    match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => text.parse::<u32>().ok(),
    }
}

unsafe extern "C" fn mask_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(mask) = parse_u32(value) else { return -22; };
    if mask & !ALL_BITS != 0 { return -22; }
    MASK.store(mask, Ordering::Release);
    if mask & FEED_PERF == 0 { UNTIL_NS.store(0, Ordering::Release); STREAK.store(0, Ordering::Release); }
    0
}
unsafe extern "C" fn mask_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1000) };
    let mut out = Output { bytes, used: 0 };
    let _ = writeln!(out, "{:#x} pct={} applied={} streak={} idle={} busy={} starved={} fed={} boosts={} boosted_ns={} active={} policies={} depth={} early={} settle_skip={} ring_hits={} tail={} obs_skip={} mirrors={} proof_hits={} proof_batches={} proof_fallbacks={} pct_p={} pct_e={} big_cpus={:#x} applied_khz={}/{} nudges={} nudge_fails={} fwdeps={}/{}/{} early_deps={} agg_first={}",
        MASK.load(Ordering::Relaxed), FEED_PCT.load(Ordering::Relaxed), REQS.applied_pct.load(Ordering::Relaxed),
        STREAK.load(Ordering::Relaxed), IDLE_EWMA.load(Ordering::Relaxed), BUSY_EWMA.load(Ordering::Relaxed),
        STARVED.load(Ordering::Relaxed), FED.load(Ordering::Relaxed),
        BOOSTS.load(Ordering::Relaxed), BOOSTED_NS.load(Ordering::Relaxed), ACTIVE.load(Ordering::Relaxed),
        REQS.count.load(Ordering::Relaxed), EARLY_DEPTH.load(Ordering::Relaxed),
        EARLY_TAKEN.load(Ordering::Relaxed),
        SETTLE_SKIPPED.load(Ordering::Relaxed), RING_CACHE_HITS.load(Ordering::Relaxed),
        TAIL_MERGED.load(Ordering::Relaxed), OBS_SKIPPED.load(Ordering::Relaxed),
        MIRRORS.load(Ordering::Relaxed), PROOF_HITS.load(Ordering::Relaxed),
        PROOF_BATCHES.load(Ordering::Relaxed), PROOF_FALLBACKS.load(Ordering::Relaxed),
        FEED_PCT_P.load(Ordering::Relaxed), FEED_PCT_E.load(Ordering::Relaxed),
        REQS.big_cpus.load(Ordering::Relaxed), REQS.applied_khz[0].load(Ordering::Relaxed),
        REQS.applied_khz[1].load(Ordering::Relaxed), NUDGES.load(Ordering::Relaxed),
        NUDGE_FAILS.load(Ordering::Relaxed), FW_DEP_CONVERTED.load(Ordering::Relaxed),
        FW_DEP_HOST.load(Ordering::Relaxed), FW_DEP_SATISFIED.load(Ordering::Relaxed),
        EARLY_WITH_DEPS.load(Ordering::Relaxed), AGG_FIRST_TAKEN.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}
unsafe extern "C" fn pct_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(pct) = parse_u32(value) else { return -22; };
    if pct == 0 || pct > 100 { return -22; }
    FEED_PCT.store(pct, Ordering::Release);
    0
}
unsafe extern "C" fn pct_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 64) };
    let mut out = Output { bytes, used: 0 };
    let _ = writeln!(out, "{}", FEED_PCT.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}
static MASK_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(mask_set), get: Some(mask_get), free: None,
};
static PCT_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(pct_set), get: Some(pct_get), free: None,
};
unsafe extern "C" fn depth_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(depth) = parse_u32(value) else { return -22; };
    if depth == 0 || depth > EARLY_DEPTH_MAX { return -22; }
    EARLY_DEPTH.store(depth, Ordering::Release);
    0
}
unsafe extern "C" fn depth_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 64) };
    let mut out = Output { bytes, used: 0 };
    let _ = writeln!(out, "{}", EARLY_DEPTH.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}
static DEPTH_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(depth_set), get: Some(depth_get), free: None,
};
unsafe extern "C" fn pct_p_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(pct) = parse_u32(value) else { return -22; };
    if pct > 100 { return -22; }
    FEED_PCT_P.store(pct, Ordering::Release);
    0
}
unsafe extern "C" fn pct_e_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    let Some(pct) = parse_u32(value) else { return -22; };
    if pct > 100 { return -22; }
    FEED_PCT_E.store(pct, Ordering::Release);
    0
}
fn pct_get_from(counter: &AtomicU32, buf: *mut core::ffi::c_char) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 64) };
    let mut out = Output { bytes, used: 0 };
    let _ = writeln!(out, "{}", counter.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}
unsafe extern "C" fn pct_p_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    pct_get_from(&FEED_PCT_P, buf)
}
unsafe extern "C" fn pct_e_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    pct_get_from(&FEED_PCT_E, buf)
}
static PCT_P_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(pct_p_set), get: Some(pct_p_get), free: None,
};
static PCT_E_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(pct_e_set), get: Some(pct_e_get), free: None,
};
#[link_section = "__param"]
#[used]
static MASK_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_host_path"
    } else { c"aurora.g17p_host_path" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&MASK_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
#[link_section = "__param"]
#[used]
static PCT_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_feed_perf_pct"
    } else { c"aurora.g17p_feed_perf_pct" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&PCT_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
#[link_section = "__param"]
#[used]
static DEPTH_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_early_depth"
    } else { c"aurora.g17p_early_depth" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&DEPTH_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
#[link_section = "__param"]
#[used]
static PCT_P_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_feed_perf_pct_p"
    } else { c"aurora.g17p_feed_perf_pct_p" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&PCT_P_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
#[link_section = "__param"]
#[used]
static PCT_E_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_feed_perf_pct_e"
    } else { c"aurora.g17p_feed_perf_pct_e" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&PCT_E_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
