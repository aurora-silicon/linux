// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Detects sustained host refill gaps and temporarily requests CPU capacity.
//! Detector updates are lock-free. The worker and policy callbacks serialize
//! frequency requests without nesting their mutex inside a publication lock.
//! Shutdown first closes the detector, joins its producers and the worker,
//! then removes requests. All request storage and policy references are owned
//! by this device, independently of other GPU instances.
//! Policy creation and the initial snapshot take the policy rwsem before the
//! requests mutex. Removal callbacks and the worker take only requests;
//! notifier unregistration joins callbacks without holding that mutex.
//! Requests full CPU capacity only while refill pressure keeps the detector
//! active. Sustained demand can renew its deadline indefinitely; withdrawal
//! occurs on a later worker poll, not at an exact twenty-millisecond boundary.

use crate::hw::t8140::qos::CLOCK_HZ;
use core::{
    pin::Pin,
    sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
};
use kernel::{
    new_spinlock,
    prelude::*,
    sync::{Arc, SpinLock},
    time::{delay::fsleep, ClockSource, Delta, Monotonic},
};

#[cfg(CONFIG_CPU_FREQ)]
use core::ptr;
#[cfg(CONFIG_CPU_FREQ)]
use kernel::{bindings, new_mutex, sync::Mutex, types::Opaque};

unsafe extern "C" {
    /// The system time base in ticks of `hz`, the clock the firmware stamps command execution
    /// with.
    fn asahi_g17_firmware_clock(hz: u64) -> u64;
}

const MAX_REFILL_TICKS: u64 = CLOCK_HZ / 500;
const _: () = assert!(CLOCK_HZ == crate::hw::t8140::resources::COMMAND_TIMESTAMP_HZ);
const EWMA_WINDOW: u64 = 8;
const REFILL_BOUND_DIV: u64 = 4;
const STREAK_ON: u32 = 8;
const STREAK_MAX: u32 = 64;
const LINGER_NS: u64 = 20_000_000;
/// Upper EWMA busy-sum (~8 x 4 ms passes) below which measurable inter-pass
/// host refill gaps benefit from full CPU turnaround speed without boosting
/// long multi-millisecond GPU-bound frames.
const TURNAROUND_MAX_PASS_MS: u64 = 4;
const TURNAROUND_BUSY_SUM_TICKS: u64 =
    CLOCK_HZ * TURNAROUND_MAX_PASS_MS / 1000 * EWMA_WINDOW;
/// Refill-gap share to engage (1/16 = 6.25%) and hold (3x hysteresis -> 1/48 = 2.08%,
/// still above the 2.0% GPU-bound pass gap) CPU capacity on short turnaround-exposed
/// passes once boosting shrinks each queue's refill gap.
const TURNAROUND_ENTER_DIV: u64 = 16;
const TURNAROUND_HOLD_DIV: u64 = TURNAROUND_ENTER_DIV * 3;
/// Per-context completion timestamps so staggered concurrent depth-1 queues
/// still report each queue's own host refill gap instead of masking it behind
/// a sibling queue's overlapping pass. Only engaged while passes from distinct
/// contexts actually overlap by at least 1/OVERLAP_CONCURRENT_DIV of a pass so
/// serial multi-queue streams taking turns on a saturated GPU stay GPU-bound.
const CONTEXT_TRACKS: usize = 16;
const OVERLAP_CONCURRENT_DIV: u64 = 4;
const POLL_MS: i64 = 2;
#[cfg(CONFIG_CPU_FREQ)]
const FLOOR_PERCENT: u64 = 100;
const WAIT_UTIL_MIN: u32 = 512;
const WAIT_HINT_MS: u32 = 20;
#[cfg(CONFIG_CPU_FREQ)]
const MAX_POLICIES: usize = 8;

struct Detector {
    last_end: AtomicU64,
    context_end: [AtomicU64; CONTEXT_TRACKS],
    overlap_until: AtomicU64,
    idle: AtomicU64,
    busy: AtomicU64,
    streak: AtomicU32,
    until: AtomicU64,
    /// Until when the GPU counts as the bottleneck (see `Feed::gpu_bound`).
    bound_until: AtomicU64,
    active: AtomicBool,
    pending: AtomicBool,
    stopping: AtomicBool,
}

impl Detector {
    const fn new() -> Self {
        Self {
            last_end: AtomicU64::new(0),
            context_end: [const { AtomicU64::new(0) }; CONTEXT_TRACKS],
            overlap_until: AtomicU64::new(0),
            idle: AtomicU64::new(0),
            busy: AtomicU64::new(0),
            streak: AtomicU32::new(0),
            until: AtomicU64::new(0),
            bound_until: AtomicU64::new(0),
            active: AtomicBool::new(false),
            pending: AtomicBool::new(false),
            stopping: AtomicBool::new(false),
        }
    }

    fn decay_add(sum: &AtomicU64, add: u64) -> u64 {
        let mut next = 0;
        let _ = sum.fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
            next = value - value / EWMA_WINDOW + add;
            Some(next)
        });
        next
    }

    fn note(&self, context: u32, start: u64, end: u64, now: u64) {
        if start == 0 || end <= start {
            return;
        }
        let global_prev = self.last_end.fetch_max(end, Ordering::AcqRel);
        let ctx_prev = self.context_end[(context as usize) % CONTEXT_TRACKS]
            .fetch_max(end, Ordering::AcqRel);
        let paced = global_prev == 0
            || (start >= global_prev && start - global_prev >= MAX_REFILL_TICKS);
        let pass = end - start;
        let overlap = if global_prev > ctx_prev && start < global_prev {
            end.min(global_prev) - start
        } else {
            0
        };
        if overlap.saturating_mul(OVERLAP_CONCURRENT_DIV) >= pass {
            self.overlap_until.store(now + LINGER_NS, Ordering::Release);
        }
        let prev = if start < global_prev && now < self.overlap_until.load(Ordering::Acquire) {
            ctx_prev
        } else {
            global_prev
        };
        let (idle, busy) = if prev == 0 {
            (0, pass)
        } else if start >= prev {
            let gap = start - prev;
            if gap < MAX_REFILL_TICKS {
                (gap, pass)
            } else {
                (0, pass)
            }
        } else if end > prev {
            (0, end - prev)
        } else {
            (0, 0)
        };
        let (idle_sum, busy_sum) = if idle == 0 && busy == 0 {
            (
                self.idle.load(Ordering::Acquire),
                self.busy.load(Ordering::Acquire),
            )
        } else {
            (
                Self::decay_add(&self.idle, idle),
                Self::decay_add(&self.busy, busy),
            )
        };
        let streak = if paced {
            self.streak.store(0, Ordering::Release);
            0
        } else {
            let previous = self
                .streak
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
                    Some((value + 1).min(STREAK_MAX))
                });
            // The closure always returns Some; fetch_update retries contention.
            match previous {
                Ok(value) => value + 1,
                Err(_) => 0,
            }
        };
        if streak < STREAK_ON {
            return;
        }
        let refill_bound = idle_sum.saturating_mul(REFILL_BOUND_DIV) >= busy_sum;
        let turnaround_div = if self.active.load(Ordering::Acquire) {
            TURNAROUND_HOLD_DIV
        } else {
            TURNAROUND_ENTER_DIV
        };
        let turnaround_exposed = busy_sum <= TURNAROUND_BUSY_SUM_TICKS
            && idle_sum.saturating_mul(turnaround_div) >= busy_sum;
        if refill_bound || turnaround_exposed {
            self.until.store(now + LINGER_NS, Ordering::Release);
            if !self.active.swap(true, Ordering::AcqRel) {
                self.pending.store(true, Ordering::Release);
            }
        } else {
            self.bound_until.store(now + LINGER_NS, Ordering::Release);
        }
    }

    fn wanted(&self, now: u64) -> bool {
        !self.stopping.load(Ordering::Acquire) && now < self.until.load(Ordering::Acquire)
    }

    fn bound(&self, now: u64) -> bool {
        !self.stopping.load(Ordering::Acquire) && now < self.bound_until.load(Ordering::Acquire)
    }
}

#[cfg(CONFIG_CPU_FREQ)]
#[pin_data]
struct Requests {
    #[pin]
    requests: Opaque<[bindings::freq_qos_request; MAX_POLICIES]>,
    policies: [*mut bindings::cpufreq_policy; MAX_POLICIES],
    maxima: [u32; MAX_POLICIES],
    applied: [u32; MAX_POLICIES],
}

// SAFETY: Each nonnull policy owns a kobject reference. The device requests
// mutex serializes the pinned slots with the worker and policy callbacks.
#[cfg(CONFIG_CPU_FREQ)]
unsafe impl Send for Requests {}

#[cfg(CONFIG_CPU_FREQ)]
impl Requests {
    fn new() -> Self {
        Self {
            requests: Opaque::zeroed(),
            policies: [ptr::null_mut(); MAX_POLICIES],
            maxima: [0; MAX_POLICIES],
            applied: [0; MAX_POLICIES],
        }
    }

    fn target_floor(maximum: u32, enabled: bool) -> u32 {
        if enabled {
            (u64::from(maximum) * FLOOR_PERCENT / 100) as u32
        } else {
            bindings::FREQ_QOS_MIN_DEFAULT_VALUE as u32
        }
    }

    /// Add a policy to the pinned requests.
    ///
    /// # Safety
    ///
    /// The caller holds the policy's read or write lock and keeps it alive.
    /// Policy removal must subsequently pass through this device's notifier.
    unsafe fn add(self: Pin<&mut Self>, policy: *mut bindings::cpufreq_policy) {
        let this = self.project();
        if this.policies.contains(&policy) {
            return;
        }
        let Some(index) = this.policies.iter().position(|entry| entry.is_null()) else {
            return;
        };
        // SAFETY: The caller keeps the initialized policy alive. The vacant
        // request slot is pinned and exclusively owned under the requests mutex.
        let result = unsafe {
            bindings::freq_qos_add_request(
                ptr::addr_of_mut!((*policy).constraints),
                this.requests
                    .get()
                    .cast::<bindings::freq_qos_request>()
                    .add(index),
                bindings::freq_qos_req_type_FREQ_QOS_MIN,
                bindings::FREQ_QOS_MIN_DEFAULT_VALUE as i32,
            )
        };
        if result >= 0 {
            // SAFETY: The caller's policy reference or CREATE callback keeps the
            // kobject alive. Retain it until REMOVE or device shutdown removes
            // this request; cpuinfo is protected by the caller's policy lock.
            unsafe {
                bindings::kobject_get(ptr::addr_of_mut!((*policy).kobj));
                this.maxima[index] = (*policy).cpuinfo.max_freq;
            }
            this.policies[index] = policy;
            this.applied[index] = bindings::FREQ_QOS_MIN_DEFAULT_VALUE;
        }
    }

    fn apply(self: Pin<&mut Self>, enabled: bool) -> bool {
        let this = self.project();
        let mut applied = false;
        for index in 0..MAX_POLICIES {
            if this.policies[index].is_null() {
                continue;
            }
            let value = Self::target_floor(this.maxima[index], enabled);
            if this.applied[index] != value {
                // SAFETY: This slot is registered, pinned and exclusively locked;
                // the policy reference keeps its constraints alive through REMOVE.
                let result = unsafe {
                    bindings::freq_qos_update_request(
                        this.requests
                            .get()
                            .cast::<bindings::freq_qos_request>()
                            .add(index),
                        value as i32,
                    )
                };
                if result >= 0 {
                    this.applied[index] = value;
                }
            }
            applied |= this.applied[index] != bindings::FREQ_QOS_MIN_DEFAULT_VALUE;
        }
        applied
    }

    fn remove(self: Pin<&mut Self>, policy: *mut bindings::cpufreq_policy) {
        let this = self.project();
        let Some(index) = this.policies.iter().position(|entry| *entry == policy) else {
            return;
        };
        // SAFETY: This registered pinned slot is exclusively locked. The policy
        // cannot finish removal until this reference is dropped. Keep all other
        // request slots at their original addresses when releasing this slot.
        unsafe {
            bindings::freq_qos_remove_request(
                this.requests
                    .get()
                    .cast::<bindings::freq_qos_request>()
                    .add(index),
            );
            bindings::cpufreq_cpu_put(policy);
        }
        this.policies[index] = ptr::null_mut();
        this.maxima[index] = 0;
        this.applied[index] = bindings::FREQ_QOS_MIN_DEFAULT_VALUE;
    }

    fn clear(mut self: Pin<&mut Self>) {
        for index in 0..MAX_POLICIES {
            let policy = self.policies[index];
            if !policy.is_null() {
                self.as_mut().remove(policy);
            }
        }
    }
}

/// Only the blocking notifier core accesses this block after registration.
#[cfg(CONFIG_CPU_FREQ)]
#[pin_data]
struct PolicyNotifier {
    #[pin]
    block: Opaque<bindings::notifier_block>,
}

// SAFETY: The pinned notifier is registered once before publishing Feed and
// unregistered at shutdown. The blocking notifier core serializes its links;
// callbacks use only Feed's atomic state and request mutex.
#[cfg(CONFIG_CPU_FREQ)]
unsafe impl Send for PolicyNotifier {}
// SAFETY: As above; no Rust access mutates the registered block.
#[cfg(CONFIG_CPU_FREQ)]
unsafe impl Sync for PolicyNotifier {}

#[pin_data(PinnedDrop)]
pub(crate) struct Feed {
    detector: Detector,
    /// Every command's execution per client, for the performance effort
    /// ([`super::power::BusyRuns`]).
    #[pin]
    runs: SpinLock<super::power::BusyRuns>,
    #[cfg(CONFIG_CPU_FREQ)]
    #[pin]
    requests: Mutex<Requests>,
    #[cfg(CONFIG_CPU_FREQ)]
    #[pin]
    notifier: PolicyNotifier,
    #[cfg(CONFIG_CPU_FREQ)]
    registered: AtomicBool,
    applied: AtomicBool,
}

impl Feed {
    pub(crate) fn new() -> Result<Arc<Self>> {
        let feed = Arc::pin_init(
            pin_init!(Self {
                detector: Detector::new(),
                runs <- new_spinlock!(super::power::BusyRuns::new(), "G17 busy runs"),
                #[cfg(CONFIG_CPU_FREQ)]
                requests <- new_mutex!(Requests::new(), "G17 CPU feed"),
                #[cfg(CONFIG_CPU_FREQ)]
                notifier <- pin_init!(PolicyNotifier {
                    block: Opaque::new(bindings::notifier_block {
                        notifier_call: Some(Self::policy_event),
                        next: ptr::null_mut(),
                        priority: 0,
                    }),
                }),
                #[cfg(CONFIG_CPU_FREQ)]
                registered: AtomicBool::new(false),
                applied: AtomicBool::new(false),
            }),
            GFP_KERNEL,
        )?;
        #[cfg(CONFIG_CPU_FREQ)]
        feed.register_policies();
        Ok(feed)
    }

    /// Seed existing policies once; later creation/removal is event driven.
    #[cfg(CONFIG_CPU_FREQ)]
    fn register_policies(&self) {
        // SAFETY: Feed is pinned in its Arc and has not been published. The
        // notifier block lives until shutdown unregisters and joins callbacks.
        let result = unsafe {
            bindings::cpufreq_register_notifier(
                self.notifier.block.get(),
                bindings::CPUFREQ_POLICY_NOTIFIER,
            )
        };
        if result < 0 {
            // CPUFreq can be disabled at boot independently of this driver.
            return;
        }
        self.registered.store(true, Ordering::Release);
        // SAFETY: nr_cpu_ids is initialized before driver probe.
        let cpus = unsafe { bindings::nr_cpu_ids };
        for cpu in 0..cpus {
            // SAFETY: The bounded CPU index is valid; a nonnull result owns a
            // reference even if removal begins before acquiring the policy lock.
            let policy = unsafe { bindings::cpufreq_cpu_get(cpu) };
            if policy.is_null() {
                continue;
            }
            // CREATE holds the policy write lock before taking requests. Use
            // the same lock order and wait out partial policy initialization.
            // SAFETY: The retained reference keeps the policy and rwsem alive.
            unsafe { bindings::down_read(ptr::addr_of_mut!((*policy).rwsem)) };
            {
                let mut requests = self.requests.lock();
                // Removal unpublishes the CPU pointer before its notifier takes
                // requests. Rechecking under this mutex makes either the seed
                // skip the policy or the subsequent REMOVE release its slot.
                // SAFETY: The CPU index is valid; balance this extra reference below.
                let current = unsafe { bindings::cpufreq_cpu_get(cpu) };
                // SAFETY: The policy read lock excludes unfinished CREATE. If
                // this policy is still published, REMOVE cannot yet have removed
                // the core max request without passing our held requests mutex.
                let initialized =
                    current == policy && unsafe { !(*policy).max_freq_req.qos.is_null() };
                if initialized {
                    // SAFETY: The reference and read lock keep the initialized
                    // policy alive; registered REMOVE callbacks cover its lifetime.
                    unsafe { requests.as_mut().add(policy) };
                }
                if !current.is_null() {
                    // SAFETY: Balances the second lookup, independently of its identity.
                    unsafe { bindings::cpufreq_cpu_put(current) };
                }
            }
            // SAFETY: Balance the read lock and first lookup after releasing requests.
            unsafe {
                bindings::up_read(ptr::addr_of_mut!((*policy).rwsem));
                bindings::cpufreq_cpu_put(policy);
            }
        }
    }

    /// Process a cpufreq policy notification.
    ///
    /// # Safety
    ///
    /// `block` must be this pinned Feed's registered notifier, and `data` must
    /// point to the live policy supplied by the cpufreq notifier core.
    /// CREATE must hold its write lock; REMOVE must precede policy release.
    #[cfg(CONFIG_CPU_FREQ)]
    unsafe extern "C" fn policy_event(
        block: *mut bindings::notifier_block,
        event: c_ulong,
        data: *mut core::ffi::c_void,
    ) -> core::ffi::c_int {
        // SAFETY: This callback is installed only on Feed::notifier.block. Its
        // owner remains pinned until unregister joins every callback.
        let notifier =
            unsafe { kernel::container_of!(Opaque::cast_from(block), PolicyNotifier, block) };
        // SAFETY: The notifier is the embedded, pinned field of this live Feed.
        let feed = unsafe { &*kernel::container_of!(notifier, Feed, notifier) };
        let policy = data.cast::<bindings::cpufreq_policy>();
        let mut requests = feed.requests.lock();
        if event == bindings::CPUFREQ_REMOVE_POLICY as c_ulong {
            requests.as_mut().remove(policy);
        } else if event == bindings::CPUFREQ_CREATE_POLICY as c_ulong
            && !feed.detector.stopping.load(Ordering::Acquire)
        {
            // SAFETY: CREATE supplies an initialized policy with its write lock
            // held and guarantees REMOVE before the core releases its storage.
            unsafe { requests.as_mut().add(policy) };
        } else {
            return bindings::NOTIFY_DONE as i32;
        }
        let enabled = feed.detector.wanted(Self::now());
        let applied = requests.as_mut().apply(enabled);
        feed.applied.store(applied, Ordering::Relaxed);
        bindings::NOTIFY_OK as i32
    }

    fn now() -> u64 {
        Monotonic::ktime_get() as u64
    }

    /// Firmware execution intervals in the device's 24 MHz timestamp domain.
    /// Missing, unexecuted and wrapped intervals must not affect the detector.
    pub(crate) fn note_pass(&self, context: u32, start: u64, end: u64) {
        if cfg!(CONFIG_CPU_FREQ) {
            self.detector.note(context, start, end, Self::now());
        }
    }

    /// A command of `client` (its context id) executed from `start` to `end`, in the firmware
    /// timestamp domain, for the performance effort's busy runs. A gap of the firmware's idle
    /// power-off delay between the client's commands ends its run.
    pub(crate) fn note_work(&self, client: u32, start: u64, end: u64) {
        let idle_ticks = u64::from(super::power::FIRMWARE_IDLE_OFF_MS) * (CLOCK_HZ / 1000);
        self.runs.lock().note(client, start, end, idle_ticks);
    }

    /// Whether a client that is still running has kept the GPU running without going idle for
    /// longer than the effort is held after work stops ([`super::power::BusyRuns`]).
    pub(crate) fn busy_run(&self) -> bool {
        let hold_ticks = super::power::IDLE_HOLD_NS * CLOCK_HZ / 1_000_000_000;
        let idle_ticks = u64::from(super::power::FIRMWARE_IDLE_OFF_MS) * (CLOCK_HZ / 1000);
        // SAFETY: Reads the system counter; plain value in and out.
        let now = unsafe { asahi_g17_firmware_clock(CLOCK_HZ) };
        self.runs.lock().bound(now, hold_ticks, idle_ticks)
    }

    /// The effort was taken down after the hold: every client's next command begins a new run.
    pub(crate) fn end_busy_run(&self) {
        self.runs.lock().end();
    }

    /// Whether the GPU itself is the bottleneck: passes follow each other with refill gaps
    /// under a quarter of their busy time, and the feed does not see host-refill-bound work.
    /// Lingers like streaming, so a pause of more than a few milliseconds ends it.
    pub(crate) fn gpu_bound(&self) -> bool {
        if !cfg!(CONFIG_CPU_FREQ) {
            return false;
        }
        let now = Self::now();
        self.detector.bound(now) && !self.detector.wanted(now)
    }

    /// Called after a firmware event pass, before queuing the single feed work item.
    pub(crate) fn take_pending(&self) -> bool {
        self.detector.pending.swap(false, Ordering::AcqRel)
    }

    #[cfg(CONFIG_CPU_FREQ)]
    fn apply(&self, enabled: bool) {
        let mut requests = self.requests.lock();
        let enabled = enabled && !self.detector.stopping.load(Ordering::Acquire);
        let applied = requests.as_mut().apply(enabled);
        self.applied.store(applied, Ordering::Relaxed);
    }

    #[cfg(not(CONFIG_CPU_FREQ))]
    fn apply(&self, _enabled: bool) {}

    /// Worker context; the linger deadline has no firmware event of its own.
    pub(crate) fn run(&self) {
        loop {
            if self.detector.wanted(Self::now()) {
                self.apply(true);
                fsleep(Delta::from_millis(POLL_MS));
                continue;
            }
            self.apply(false);
            self.detector.active.store(false, Ordering::SeqCst);
            // A detector can extend the deadline after observing the old active
            // bit. Reclaim that work unless it already queued another owner.
            if self.detector.wanted(Self::now())
                && !self.detector.active.swap(true, Ordering::AcqRel)
            {
                continue;
            }
            return;
        }
    }

    /// Temporary capacity hint for blocking syncobj waits on active GPU streams.
    ///
    /// Host-refill-bound streams (`wanted`) require an applied cpufreq floor,
    /// while GPU-bound streams (`bound`) return the hint without raising the
    /// cpufreq floor so EAS keeps the waiter on a P-core across long passes.
    pub(crate) fn wait_hint(&self) -> Option<(u32, u32)> {
        if !cfg!(CONFIG_UCLAMP_TASK) {
            return None;
        }
        let now = Self::now();
        if (self.detector.wanted(now) && self.applied.load(Ordering::Relaxed))
            || self.detector.bound(now)
        {
            Some((WAIT_UTIL_MIN, WAIT_HINT_MS))
        } else {
            None
        }
    }

    /// Call before disabling/joining the event producer and actuator work items.
    pub(crate) fn begin_shutdown(&self) {
        self.detector.stopping.store(true, Ordering::Release);
    }

    /// Call after work items are joined. Removing requests withdraws their floor.
    pub(crate) fn shutdown(&self) {
        self.begin_shutdown();
        #[cfg(CONFIG_CPU_FREQ)]
        {
            // Unregister joins callbacks, which take requests: never hold that
            // mutex here. Device shutdown has already joined the feed worker.
            if self.registered.swap(false, Ordering::AcqRel) {
                // SAFETY: The block was registered successfully and remains
                // pinned and alive until this synchronous removal returns.
                unsafe {
                    bindings::cpufreq_unregister_notifier(
                        self.notifier.block.get(),
                        bindings::CPUFREQ_POLICY_NOTIFIER,
                    );
                }
            }
            self.requests.lock().as_mut().clear();
        }
        self.applied.store(false, Ordering::Relaxed);
    }
}

#[pinned_drop]
impl PinnedDrop for Feed {
    fn drop(self: Pin<&mut Self>) {
        self.shutdown();
    }
}

#[kernel::macros::kunit_tests(asahi_g17_feed_detector)]
mod detector_tests {
    use super::*;

    /// Notes `passes` passes of `busy` ticks, each `gap` ticks after the previous, 1 ms apart.
    fn run(detector: &Detector, passes: u64, busy: u64, gap: u64) -> u64 {
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..passes {
            detector.note(0, start, start + busy, now);
            start += busy + gap;
            now += 1_000_000;
        }
        now
    }

    #[test]
    fn back_to_back_long_passes_are_gpu_bound() {
        let detector = Detector::new();
        // 2 ms passes with 40 us refill gaps, as a deep heavy queue or a heavy row at depth 1.
        let now = run(&detector, 16, CLOCK_HZ / 500, CLOCK_HZ / 25_000);
        assert!(detector.bound(now));
        assert!(!detector.wanted(now));
        // Still bound across an 8-16 ms pass, then expires at the 20 ms linger boundary.
        assert!(detector.bound(now + 15_000_000));
        assert!(!detector.bound(now + LINGER_NS));
    }

    #[test]
    fn gpu_bound_long_pass_lingers_for_full_frame() {
        let detector = Detector::new();
        // 8 ms GPU passes with 1.7 ms host gaps stay GPU-bound without requesting a CPU floor,
        // and linger across a 16 ms frame until the 20 ms boundary.
        let busy = CLOCK_HZ / 125;
        let gap = CLOCK_HZ * 17 / 10_000;
        let step_ns = 9_700_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..16 {
            detector.note(0, start, start + busy, now);
            start += busy + gap;
            now += step_ns;
        }
        let last = now - step_ns;
        assert!(detector.bound(last));
        assert!(!detector.wanted(last));
        assert!(detector.bound(last + 16_000_000));
        assert!(detector.bound(last + LINGER_NS - 1));
        assert!(!detector.bound(last + LINGER_NS));
    }

    #[test]
    fn refill_gaps_stream_instead() {
        let detector = Detector::new();
        // 20 us passes with 60 us refill gaps: bound by host refills.
        let now = run(&detector, 16, CLOCK_HZ / 50_000, CLOCK_HZ / 16_667);
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
    }

    #[test]
    fn paced_or_short_runs_are_neither() {
        let detector = Detector::new();
        // Each pass starts after a pause of at least the refill limit.
        let now = run(&detector, 16, CLOCK_HZ / 500, MAX_REFILL_TICKS);
        assert!(!detector.bound(now) && !detector.wanted(now));
        // 60 Hz paced frames (16.6 ms gap) keep resetting the streak.
        let detector = Detector::new();
        let now = run(&detector, 16, CLOCK_HZ / 500, CLOCK_HZ * 166 / 10_000);
        assert!(!detector.bound(now) && !detector.wanted(now));
        // Fewer back-to-back passes than the streak needs.
        let detector = Detector::new();
        let now = run(&detector, u64::from(STREAK_ON) - 1, CLOCK_HZ / 500, 0);
        assert!(!detector.bound(now) && !detector.wanted(now));
    }

    #[test]
    fn medium_depth1_turnaround_requests_and_holds_floor() {
        let detector = Detector::new();
        // Unboosted 1.35 ms pass with 195 us host refill gap (~14.4% >= 1/16).
        let busy = CLOCK_HZ * 1350 / 1_000_000;
        let gap_unboosted = CLOCK_HZ * 195 / 1_000_000;
        let gap_boosted = CLOCK_HZ * 78 / 1_000_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..16 {
            detector.note(1, start, start + busy, now);
            start += busy + gap_unboosted;
            now += 1_550_000;
        }
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
        // Once boosted, the refill gap shrinks to 78 us (~5.8%) and must hold.
        for _ in 0..32 {
            detector.note(1, start, start + busy, now);
            start += busy + gap_boosted;
            now += 1_430_000;
        }
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
    }

    #[test]
    fn staggered_two_queue_depth1_holds_floor_without_masking() {
        let detector = Detector::new();
        let busy = CLOCK_HZ * 2280 / 1_000_000;
        let gap_unboosted = CLOCK_HZ * 165 / 1_000_000;
        let gap_boosted = CLOCK_HZ * 72 / 1_000_000;
        let stagger = CLOCK_HZ * 180 / 1_000_000;
        let mut s0 = 1000;
        let mut s1 = s0 + stagger;
        let mut now = 1_000_000;
        // Two depth-1 queues staggered by 180 us: GPU-wide gap is 0, but each
        // queue has a 165 us unboosted -> 72 us boosted host refill gap.
        for _ in 0..12 {
            detector.note(3, s0, s0 + busy, now);
            detector.note(4, s1, s1 + busy, now + 180_000);
            s0 += busy + gap_unboosted;
            s1 += busy + gap_unboosted;
            now += 2_450_000;
        }
        assert!(detector.wanted(now));
        for _ in 0..40 {
            detector.note(3, s0, s0 + busy, now);
            detector.note(4, s1, s1 + busy, now + 180_000);
            s0 += busy + gap_boosted;
            s1 += busy + gap_boosted;
            now += 2_350_000;
        }
        assert!(detector.wanted(now));
        assert!(!detector.bound(now));
    }

    #[test]
    fn pipelined_stream_drops_turnaround_floor_cleanly() {
        let detector = Detector::new();
        let busy = CLOCK_HZ * 1150 / 1_000_000;
        let gap_unboosted = CLOCK_HZ * 180 / 1_000_000;
        let mut start = 1000;
        let mut now = 1_000_000;
        for _ in 0..16 {
            detector.note(2, start, start + busy, now);
            start += busy + gap_unboosted;
            now += 1_330_000;
        }
        assert!(detector.wanted(now));
        // Transition to zero-gap pipelined submissions: idle_sum decays below
        // the hold threshold and the floor expires after LINGER_NS.
        for _ in 0..48 {
            detector.note(2, start, start + busy, now);
            start += busy;
            now += 1_150_000;
        }
        assert!(detector.bound(now));
        assert!(!detector.wanted(now));
    }

    #[test]
    fn alternating_multi_queue_back_to_back_stays_gpu_bound() {
        for &(contexts, pass_us) in &[
            (2u32, 1000u64),
            (2, 400),
            (3, 500),
            (2, 1900),
            (2, 2100),
        ] {
            let detector = Detector::new();
            let pass = CLOCK_HZ * pass_us / 1_000_000;
            let (mut start, mut now) = (1000u64, 1_000_000u64);
            for _ in 0..32 {
                for c in 0..contexts {
                    detector.note(c + 1, start, start + pass, now);
                    start += pass;
                    now += pass_us * 1000;
                }
            }
            assert!(detector.bound(now));
            assert!(!detector.wanted(now));
        }
    }
}
