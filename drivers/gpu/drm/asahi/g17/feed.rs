// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Detects sustained per-context host refill gaps and temporarily requests CPU capacity.
//! Per-context interval tracking (`Tracker`) is serialized under its own spinlock after
//! fence signalling (off the wakeup critical path), while deadline and worker state stay
//! lock-free (`Detector`). Shutdown first closes the detector, joins its producers and the
//! worker, then removes requests. All request storage and policy references are owned by
//! this device, independently of other GPU instances.
//! Policy creation and the initial snapshot take the policy rwsem before the requests mutex.
//! Removal callbacks and the worker take only requests, never nesting the tracker spinlock
//! with the requests mutex; notifier unregistration joins callbacks without holding requests.
//! The floor and the hints it drives are one [`Policy`] per GPU generation.
//! Refill-bound requests require a sustained run (`FEED_MIN_RUN_PER_SEC`, 8 ms) and linger
//! briefly (`FEED_LINGER_NS`) so paced frame bursts do not pin CPU floors across 60/120 Hz
//! frames; GPU-bound streams keep `BOUND_LINGER_NS` so syncobj wait hints survive across
//! a 60 Hz frame without raising CPU floors.
//!
//! Pass intervals are firmware command timestamps, which count the system time
//! base at the firmware's own 24 MHz (not CNTFRQ_EL0, which is 1 GHz on T6040
//! and T8140); the detector takes that rate at creation and keeps its
//! thresholds in time.

use core::{
    pin::Pin,
    sync::atomic::{AtomicBool, AtomicU64, Ordering},
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

/// The longest gap between one pass's end and the next one's start that still counts as a
/// refill is 1/500 s (2 ms).
const MAX_REFILL_PER_SEC: u64 = 500;
const STREAK_ON: u32 = 8;
const STREAK_MAX: u32 = 64;
const CONTEXT_SLOTS: usize = 16;
/// A per-context refill run must span at least 1/125 s (8 ms) before requesting a CPU floor.
const FEED_MIN_RUN_PER_SEC: u64 = 125;
const FEED_LINGER_NS: u64 = 8_000_000;
const BOUND_LINGER_NS: u64 = 20_000_000;
const POLL_MS: i64 = 2;
#[cfg(CONFIG_CPU_FREQ)]
const MAX_POLICIES: usize = 8;

/// What the feed does on one GPU generation. Every value cites where it was measured; a
/// generation without its own measurement of a value takes [`Policy::DEFAULT`]'s.
pub(crate) struct Policy {
    /// CPU frequency floor while host refills bound the GPU, in percent of each cpufreq
    /// policy's top frequency.
    floor_percent: u64,
    /// Capacity hint for the submitter's syncobj waits while the floor is applied or the GPU
    /// is the bottleneck, and how long it stays valid after each submission
    /// (`Gpu::syncobj_wait_hint`).
    wait_util_min: u32,
    wait_hint_ms: u32,
    /// Minimum utilization of the completion receive thread while the GPU itself is the
    /// bottleneck (`Feed::gpu_bound`), or None to keep the thread at the RT default.
    gpu_bound_rx_util_min: Option<u32>,
}

impl Policy {
    /// The G17P policy. Measured on the A18 Pro (J700); every other G17P SoC (the M4 family)
    /// has no measurement of its own and uses it unchanged.
    pub(crate) const DEFAULT: Self = Self {
        // Half of each cluster's top frequency: the original feed policy, developed on J700
        // (commit "drm/asahi: request temporary CPU capacity for sustained GPU feeds").
        floor_percent: 50,
        // Half of the capacity scale, from the same commit; on G13/G14 the same hint was
        // measured on an M2 (commit "drm/asahi: hint CPU capacity for G13/G14 syncobj
        // waits"). Held 20 ms since GPU-bound streams take the hint too (commit "drm/asahi:
        // g17: provide syncobj wait hint on GPU-bound streams").
        wait_util_min: 512,
        wait_hint_ms: 20,
        // Measured on J700 (commit "drm/asahi: g17: drop the completion thread's frequency
        // boost while the GPU is the bottleneck"): heavy render/compute rows 1.7-3.3% faster,
        // 0.4-0.5 W less; 512 left idle.wake 5% slower after a pause and 0 cost one
        // completion per 250 ms on single-submit heavy compute.
        gpu_bound_rx_util_min: Some(256),
    };

    /// The M5 policy (M5 protocol firmware, J714/J716C/J775D/J813).
    pub(crate) const M5: Self = Self {
        // The full refill floor, carried from the M5 bring-up (commit "drm/asahi: g17: M5:
        // submit compute on M5 queues").
        floor_percent: 100,
        // The receive thread keeps its default: on the M5 Ultra (J775D) completions handled
        // at a lowered clock were slow (commit "drm/asahi: g17: M5: Fix M5 GPU completions
        // being handled at a lowered CPU clock").
        gpu_bound_rx_util_min: None,
        ..Self::DEFAULT
    };
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Sample {
    None,
    RefillBound,
    GpuBound,
}

#[derive(Copy, Clone)]
struct ContextRun {
    used: bool,
    context: u32,
    run_start: u64,
    last_end: u64,
    idle: u64,
    busy: u64,
    streak: u32,
}

impl ContextRun {
    const EMPTY: Self = Self {
        used: false,
        context: 0,
        run_start: 0,
        last_end: 0,
        idle: 0,
        busy: 0,
        streak: 0,
    };
}

struct Tracker {
    /// [`MAX_REFILL_PER_SEC`] in counter ticks; 0 (no counter rate) never detects a refill.
    max_refill_ticks: u64,
    /// [`FEED_MIN_RUN_PER_SEC`] in counter ticks.
    min_run_ticks: u64,
    global_last_end: u64,
    slots: [ContextRun; CONTEXT_SLOTS],
}

impl Tracker {
    const fn new(counter_hz: u64) -> Self {
        Self {
            max_refill_ticks: counter_hz / MAX_REFILL_PER_SEC,
            min_run_ticks: counter_hz / FEED_MIN_RUN_PER_SEC,
            global_last_end: 0,
            slots: [ContextRun::EMPTY; CONTEXT_SLOTS],
        }
    }

    fn decay_add(sum: u64, add: u64) -> u64 {
        (sum - sum / 8).saturating_add(add)
    }

    fn note(&mut self, context: u32, start: u64, end: u64) -> Sample {
        if self.max_refill_ticks == 0 || start == 0 || end <= start {
            return Sample::None;
        }
        let global_prev = self.global_last_end;
        let (gpu_idle, gpu_busy) = if global_prev == 0 {
            (0, end - start)
        } else if start >= global_prev {
            let gap = start - global_prev;
            (if gap < self.max_refill_ticks { gap } else { 0 }, end - start)
        } else if end > global_prev {
            // Overlapping parallel waves share no GPU idle; credit actual busy span.
            (0, end - start)
        } else {
            return Sample::None;
        };
        self.global_last_end = global_prev.max(end);

        let index = self
            .slots
            .iter()
            .position(|slot| slot.used && slot.context == context)
            .or_else(|| {
                (0..CONTEXT_SLOTS).min_by_key(|&index| {
                    let slot = &self.slots[index];
                    (slot.used, slot.last_end)
                })
            })
            .unwrap_or(0);
        let slot = &mut self.slots[index];
        let paced = !slot.used
            || slot.context != context
            || start >= slot.last_end.saturating_add(self.max_refill_ticks);
        if paced {
            *slot = ContextRun {
                used: true,
                context,
                run_start: start,
                last_end: end,
                idle: 0,
                busy: gpu_busy,
                streak: 0,
            };
            return Sample::None;
        }

        slot.run_start = slot.run_start.min(start);
        slot.last_end = slot.last_end.max(end);
        slot.idle = Self::decay_add(slot.idle, gpu_idle);
        slot.busy = Self::decay_add(slot.busy, gpu_busy);
        slot.streak = (slot.streak + 1).min(STREAK_MAX);

        if slot.streak < STREAK_ON {
            return Sample::None;
        }
        if slot.idle.saturating_mul(4) >= slot.busy {
            if slot.last_end.saturating_sub(slot.run_start) >= self.min_run_ticks {
                Sample::RefillBound
            } else {
                Sample::None
            }
        } else {
            Sample::GpuBound
        }
    }
}

struct Detector {
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
            until: AtomicU64::new(0),
            bound_until: AtomicU64::new(0),
            active: AtomicBool::new(false),
            pending: AtomicBool::new(false),
            stopping: AtomicBool::new(false),
        }
    }

    fn record(&self, sample: Sample, now: u64) {
        match sample {
            Sample::None => {}
            Sample::RefillBound => {
                self.until
                    .fetch_max(now.saturating_add(FEED_LINGER_NS), Ordering::SeqCst);
                if !self.active.swap(true, Ordering::SeqCst) {
                    self.pending.store(true, Ordering::Release);
                }
            }
            Sample::GpuBound => {
                self.bound_until
                    .fetch_max(now.saturating_add(BOUND_LINGER_NS), Ordering::AcqRel);
            }
        }
    }

    fn wanted(&self, now: u64) -> bool {
        !self.stopping.load(Ordering::Acquire) && now < self.until.load(Ordering::SeqCst)
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
    /// [`Policy::floor_percent`].
    floor_percent: u64,
}

// SAFETY: Each nonnull policy owns a kobject reference. The device requests
// mutex serializes the pinned slots with the worker and policy callbacks.
#[cfg(CONFIG_CPU_FREQ)]
unsafe impl Send for Requests {}

#[cfg(CONFIG_CPU_FREQ)]
impl Requests {
    fn new(floor_percent: u64) -> Self {
        Self {
            requests: Opaque::zeroed(),
            policies: [ptr::null_mut(); MAX_POLICIES],
            maxima: [0; MAX_POLICIES],
            applied: [0; MAX_POLICIES],
            floor_percent,
        }
    }

    fn target_floor(maximum: u32, floor_percent: u64, enabled: bool) -> u32 {
        if enabled {
            (u64::from(maximum) * floor_percent / 100) as u32
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
            let value = Self::target_floor(this.maxima[index], *this.floor_percent, enabled);
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
    policy: &'static Policy,
    /// The rate of the firmware command timestamps, in Hz; 0 if unknown.
    counter_hz: u64,
    detector: Detector,
    #[pin]
    tracker: SpinLock<Tracker>,
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
    /// `counter_hz` is the rate of the firmware command timestamps passed to [`Feed::note_pass`].
    pub(crate) fn new(policy: &'static Policy, counter_hz: u64) -> Result<Arc<Self>> {
        let feed = Arc::pin_init(
            pin_init!(Self {
                policy,
                counter_hz,
                detector: Detector::new(),
                tracker <- new_spinlock!(Tracker::new(*counter_hz), "G17 feed tracker"),
                runs <- new_spinlock!(super::power::BusyRuns::new(), "G17 busy runs"),
                #[cfg(CONFIG_CPU_FREQ)]
                requests <- new_mutex!(Requests::new(policy.floor_percent), "G17 CPU feed"),
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

    /// Firmware execution intervals in system counter ticks for `context`.
    /// Missing, unexecuted and wrapped intervals must not affect the detector.
    pub(crate) fn note_pass(&self, context: u32, start: u64, end: u64) {
        if cfg!(CONFIG_CPU_FREQ) {
            let sample = self.tracker.lock().note(context, start, end);
            if sample != Sample::None {
                self.detector.record(sample, Self::now());
            }
        }
    }

    /// A command of `client` (its context id) executed from `start` to `end`, in the firmware
    /// timestamp domain, for the performance effort's busy runs. A gap of the firmware's idle
    /// power-off delay between the client's commands ends its run.
    pub(crate) fn note_work(&self, client: u32, start: u64, end: u64) {
        let idle_ticks = u64::from(super::power::FIRMWARE_IDLE_OFF_MS) * (self.counter_hz / 1000);
        self.runs.lock().note(client, start, end, idle_ticks);
    }

    /// Whether a client that is still running has kept the GPU running without going idle for
    /// longer than the effort is held after work stops ([`super::power::BusyRuns`]).
    /// Without a counter rate no run is ever long enough.
    pub(crate) fn busy_run(&self) -> bool {
        let hz = self.counter_hz;
        if hz == 0 {
            return false;
        }
        let hold_ticks = super::power::IDLE_HOLD_NS * hz / 1_000_000_000;
        let idle_ticks = u64::from(super::power::FIRMWARE_IDLE_OFF_MS) * (hz / 1000);
        // SAFETY: Reads the system counter; plain value in and out.
        let now = unsafe { asahi_g17_firmware_clock(hz) };
        self.runs.lock().bound(now, hold_ticks, idle_ticks)
    }

    /// The effort was taken down after the hold: every client's next command begins a new run.
    pub(crate) fn end_busy_run(&self) {
        self.runs.lock().end();
    }

    /// Whether the GPU itself is the bottleneck: passes follow each other with refill gaps
    /// under a quarter of their busy time, and the feed does not see host-refill-bound work.
    /// Lingers for `BOUND_LINGER_NS` so a pause of more than a frame ends it.
    ///
    /// The receive thread's minimum utilization while [`Feed::gpu_bound`], if this generation
    /// sets one.
    pub(crate) fn gpu_bound_rx_util_min(&self) -> Option<u32> {
        self.policy.gpu_bound_rx_util_min
    }

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
            Some((self.policy.wait_util_min, self.policy.wait_hint_ms))
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

    /// The 24 MHz system counter of the Apple SoCs.
    const CLOCK_HZ: u64 = 24_000_000;
    const MAX_REFILL_TICKS: u64 = CLOCK_HZ / MAX_REFILL_PER_SEC;

    const fn us(v: u64) -> u64 {
        CLOCK_HZ * v / 1_000_000
    }

    const fn ms(v: u64) -> u64 {
        CLOCK_HZ * v / 1_000
    }

    const fn ticks_to_ns(ticks: u64) -> u64 {
        ticks * 1_000_000_000 / CLOCK_HZ
    }

    /// Notes `passes` passes of `busy` ticks, each `gap` ticks after the previous,
    /// returning `(next_start_tick, last_now_ns)`.
    fn run_stream(
        tracker: &mut Tracker,
        detector: &Detector,
        context: u32,
        passes: u64,
        busy: u64,
        gap: u64,
        start_tick: u64,
        start_ns: u64,
        step_ns: u64,
    ) -> (u64, u64) {
        let mut start = start_tick;
        let mut now = start_ns;
        let mut last_now = start_ns;
        for _ in 0..passes {
            let sample = tracker.note(context, start, start + busy);
            detector.record(sample, now);
            last_now = now;
            start += busy + gap;
            now += step_ns;
        }
        (start, last_now)
    }

    fn run(
        tracker: &mut Tracker,
        detector: &Detector,
        context: u32,
        passes: u64,
        busy: u64,
        gap: u64,
    ) -> u64 {
        run_stream(
            tracker,
            detector,
            context,
            passes,
            busy,
            gap,
            1000,
            1_000_000,
            ticks_to_ns(busy + gap),
        )
        .1
    }

    #[test]
    fn back_to_back_long_passes_are_gpu_bound() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        // 2 ms passes with 40 us refill gaps, as a deep heavy queue or a heavy row at depth 1.
        let last = run(&mut tracker, &detector, 1, 16, ms(2), us(40));
        assert!(detector.bound(last));
        assert!(!detector.wanted(last));
        // Still bound across an 8-16 ms pass, then expires at the 20 ms linger boundary.
        assert!(detector.bound(last + 16_000_000));
        assert!(detector.bound(last + BOUND_LINGER_NS - 1));
        assert!(!detector.bound(last + BOUND_LINGER_NS));
    }

    #[test]
    fn gpu_bound_long_pass_lingers_for_full_frame() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        // 8 ms GPU passes with 1.7 ms host gaps stay GPU-bound without requesting a CPU floor,
        // and linger across a 16 ms frame until the 20 ms boundary.
        let last = run(&mut tracker, &detector, 1, 16, ms(8), us(1700));
        assert!(detector.bound(last));
        assert!(!detector.wanted(last));
        assert!(detector.bound(last + 16_000_000));
        assert!(detector.bound(last + BOUND_LINGER_NS - 1));
        assert!(!detector.bound(last + BOUND_LINGER_NS));
    }

    #[test]
    fn refill_gaps_stream_instead() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        // 20 us passes with 60 us refill gaps: must cross FEED_MIN_RUN_PER_SEC (8 ms)
        // before boosting.
        let (next_tick, short_last) = run_stream(
            &mut tracker,
            &detector,
            1,
            20,
            us(20),
            us(60),
            ms(10),
            10_000_000,
            ticks_to_ns(us(80)),
        );
        assert!(!detector.wanted(short_last));
        assert!(!detector.bound(short_last));
        let (_, last) = run_stream(
            &mut tracker,
            &detector,
            1,
            100,
            us(20),
            us(60),
            next_tick,
            short_last + ticks_to_ns(us(80)),
            ticks_to_ns(us(80)),
        );
        assert!(detector.wanted(last));
        assert!(!detector.bound(last));
    }

    #[test]
    fn paced_or_short_runs_are_neither() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        // Each pass starts after a pause of at least the refill limit.
        let now = run(&mut tracker, &detector, 1, 16, ms(2), MAX_REFILL_TICKS);
        assert!(!detector.bound(now) && !detector.wanted(now));
        // 60 Hz paced frames (16.6 ms gap) keep resetting the streak.
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let now = run(&mut tracker, &detector, 1, 16, ms(2), us(16_600));
        assert!(!detector.bound(now) && !detector.wanted(now));
        // Fewer back-to-back passes than the streak needs.
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let now = run(&mut tracker, &detector, 1, u64::from(STREAK_ON) - 1, ms(2), 0);
        assert!(!detector.bound(now) && !detector.wanted(now));
    }

    #[test]
    fn short_desktop_bursts_stay_quiet() {
        for &period_ms in &[17u64, 8u64] {
            let mut tracker = Tracker::new(CLOCK_HZ);
            let detector = Detector::new();
            let mut tick = ms(10);
            let mut now = 10_000_000;
            for _ in 0..10 {
                let (_, last) = run_stream(
                    &mut tracker,
                    &detector,
                    18,
                    25,
                    us(20),
                    us(50),
                    tick,
                    now,
                    ticks_to_ns(us(70)),
                );
                assert!(!detector.wanted(last));
                assert!(!detector.bound(last));
                tick += ms(period_ms);
                now += period_ms * 1_000_000;
            }
        }
    }

    #[test]
    fn interleaved_paced_clients_do_not_chain() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        for frame in 0..6u64 {
            let mut tick = ms(10) + frame * ms(17);
            for ctx in 10..16u32 {
                let (next_tick, last) = run_stream(
                    &mut tracker,
                    &detector,
                    ctx,
                    10,
                    us(20),
                    us(130),
                    tick,
                    ticks_to_ns(tick),
                    ticks_to_ns(us(150)),
                );
                assert!(!detector.wanted(last));
                assert!(!detector.bound(last));
                // Next client starts within MAX_REFILL_TICKS (100 us < 2 ms), chaining globally
                // across > 9 ms per frame while each client runs only 1.5 ms.
                tick = next_tick - us(30);
            }
        }
    }

    #[test]
    fn alternating_saturated_clients_stay_gpu_bound() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let mut tick = ms(10);
        let mut last = 0;
        for i in 0..40u64 {
            let ctx = if i % 2 == 0 { 1 } else { 2 };
            let sample = tracker.note(ctx, tick, tick + us(1000));
            last = ticks_to_ns(tick + us(1000));
            detector.record(sample, last);
            assert!(!detector.wanted(last));
            tick += us(1000);
        }
        assert!(detector.bound(last));
    }

    #[test]
    fn background_compositor_does_not_break_refill_stream() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let mut tick = ms(10);
        let mut last = 0;
        for p in 0..250u64 {
            let s1 = tracker.note(1, tick, tick + us(20));
            last = ticks_to_ns(tick + us(20));
            detector.record(s1, last);
            if p % 100 == 50 {
                let c_start = tick + us(25);
                let s2 = tracker.note(2, c_start, c_start + us(30));
                detector.record(s2, ticks_to_ns(c_start + us(30)));
            }
            tick += us(80);
        }
        assert!(detector.wanted(last));
        assert!(!detector.bound(last));
    }

    #[test]
    fn subsumed_and_out_of_order_passes_do_not_corrupt() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        assert_eq!(tracker.note(1, ms(10), ms(15)), Sample::None);
        for w in 0..16u64 {
            let s = ms(10) + us(100) + w * us(50);
            assert_eq!(tracker.note(1, s, s + us(20)), Sample::None);
        }
        assert_eq!(tracker.note(1, ms(15) + us(100), ms(16)), Sample::None);

        let mut tracker2 = Tracker::new(CLOCK_HZ);
        for ctx in 1..=24u32 {
            let s = ms(10) + u64::from(ctx) * us(50);
            assert_eq!(tracker2.note(ctx, s, s + us(10)), Sample::None);
        }
        let detector2 = Detector::new();
        let (_, last) = run_stream(
            &mut tracker2,
            &detector2,
            24,
            180,
            us(20),
            us(60),
            ms(20),
            20_000_000,
            ticks_to_ns(us(80)),
        );
        assert!(detector2.wanted(last));
    }

    #[test]
    fn invalid_extreme_and_monotonic_edge_cases() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        assert_eq!(tracker.note(1, 0, ms(1)), Sample::None);
        assert_eq!(tracker.note(1, ms(1), 0), Sample::None);
        assert_eq!(tracker.note(1, ms(1), ms(1)), Sample::None);
        assert_eq!(tracker.note(1, ms(2), ms(1)), Sample::None);

        let mut extreme = Tracker::new(CLOCK_HZ);
        assert_eq!(extreme.note(1, 1, u64::MAX - 100), Sample::None);
        assert_eq!(extreme.note(1, 2, u64::MAX - 50), Sample::None);

        let mut ctx0_tracker = Tracker::new(CLOCK_HZ);
        let ctx0_det = Detector::new();
        let (_, last) = run_stream(
            &mut ctx0_tracker,
            &ctx0_det,
            0,
            180,
            us(20),
            us(60),
            ms(10),
            10_000_000,
            ticks_to_ns(us(80)),
        );
        assert!(ctx0_det.wanted(last));

        let detector = Detector::new();
        detector.record(Sample::RefillBound, 100_000_000);
        detector.record(Sample::RefillBound, 50_000_000);
        assert!(detector.wanted(100_000_000 + FEED_LINGER_NS - 1));

        detector.record(Sample::GpuBound, 200_000_000);
        detector.record(Sample::GpuBound, 120_000_000);
        assert!(detector.bound(200_000_000 + BOUND_LINGER_NS - 1));

        detector.record(Sample::RefillBound, u64::MAX);
        detector.record(Sample::GpuBound, u64::MAX);
        assert!(detector.wanted(u64::MAX - 1));
        assert!(detector.bound(u64::MAX - 1));

        detector.stopping.store(true, Ordering::Release);
        assert!(!detector.wanted(u64::MAX - 1));
        assert!(!detector.bound(u64::MAX - 1));
    }

    #[test]
    fn refill_linger_is_shorter_than_gpu_bound_linger() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let last_refill = run(&mut tracker, &detector, 1, 180, us(20), us(60));
        assert!(detector.wanted(last_refill + FEED_LINGER_NS - 1));
        assert!(!detector.wanted(last_refill + FEED_LINGER_NS));

        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let last_bound = run(&mut tracker, &detector, 1, 16, ms(2), us(40));
        assert!(detector.bound(last_bound + FEED_LINGER_NS));
        assert!(detector.bound(last_bound + BOUND_LINGER_NS - 1));
        assert!(!detector.bound(last_bound + BOUND_LINGER_NS));
    }

    #[test]
    fn overlapping_parallel_waves_stay_gpu_bound() {
        let mut tracker = Tracker::new(CLOCK_HZ);
        let detector = Detector::new();
        let s0 = ms(10);
        let d0 = us(2500);
        detector.record(tracker.note(1, s0, s0 + d0), ticks_to_ns(s0 + d0));
        let mut wave_end = s0 + d0;
        let mut last_now = 0;
        for _ in 0..6 {
            let t = wave_end + us(1000);
            for &(off, dur) in &[(0, us(2500)), (us(50), us(2650)), (us(100), us(2800))] {
                let s = t + off;
                let e = s + dur;
                let now = ticks_to_ns(e);
                let sample = tracker.note(1, s, e);
                detector.record(sample, now);
                assert!(!detector.wanted(now));
                wave_end = e;
                last_now = now;
            }
        }
        assert!(detector.bound(last_now));
    }

    #[test]
    fn zero_counter_hz_never_detects_refill_or_bound() {
        let mut tracker = Tracker::new(0);
        let detector = Detector::new();
        let last = run(&mut tracker, &detector, 1, 180, us(20), us(60));
        assert!(!detector.wanted(last) && !detector.bound(last));
        let last = run(&mut tracker, &detector, 2, 16, ms(2), us(40));
        assert!(!detector.wanted(last) && !detector.bound(last));
        assert_eq!(tracker.note(3, ms(10), ms(15)), Sample::None);
        assert_eq!(tracker.note(3, ms(11), ms(16)), Sample::None);
    }
}
