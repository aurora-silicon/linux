// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Detects sustained host refill gaps on G16 and temporarily raises CPU frequency
//! floors via `cpufreq` `FREQ_QOS_MIN` constraints (mirroring G17's feed actuator).
//!
//! Detector updates are lock-free and run in the monotonic nanosecond domain so they
//! work identically whether or not firmware microsequence timestamps are enabled.
//! The dedicated unbound feed worker and cpufreq policy notifier serialize frequency
//! requests under `Feed::requests`, never nesting inside `Shared::state`.

pub(crate) mod detector;

use core::{
    pin::Pin,
    sync::atomic::{AtomicBool, Ordering},
};
use kernel::{
    prelude::*,
    sync::Arc,
    time::{delay::fsleep, ClockSource, Delta, Monotonic},
};

#[cfg(CONFIG_CPU_FREQ)]
use core::ptr;
#[cfg(CONFIG_CPU_FREQ)]
use kernel::{bindings, new_mutex, sync::Mutex, types::Opaque};

use detector::Detector;

const POLL_MS: i64 = 2;
#[cfg(CONFIG_CPU_FREQ)]
const FLOOR_PERCENT: u64 = 100;
const WAIT_UTIL_MIN: u32 = 512;
const WAIT_HINT_MS: u32 = 8;
#[cfg(CONFIG_CPU_FREQ)]
const MAX_POLICIES: usize = 8;

#[cfg(CONFIG_CPU_FREQ)]
#[pin_data]
struct Requests {
    #[pin]
    requests: Opaque<[bindings::freq_qos_request; MAX_POLICIES]>,
    policies: [*mut bindings::cpufreq_policy; MAX_POLICIES],
    maxima: [u32; MAX_POLICIES],
    applied: [u32; MAX_POLICIES],
}

// SAFETY: Each nonnull policy owns a kobject reference. The device requests mutex
// serializes the pinned slots with the worker and policy callbacks.
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
        // SAFETY: The caller keeps the initialized policy alive. The vacant request
        // slot is pinned and exclusively owned under the requests mutex.
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
            // kobject alive until REMOVE or shutdown removes this request.
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
        // SAFETY: This registered pinned slot is exclusively locked. Keep all other
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
                #[cfg(CONFIG_CPU_FREQ)]
                requests <- new_mutex!(Requests::new(), "G16 CPU feed"),
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
            return;
        }
        self.registered.store(true, Ordering::Release);
        // SAFETY: nr_cpu_ids is initialized before driver probe.
        let cpus = unsafe { bindings::nr_cpu_ids };
        for cpu in 0..cpus {
            // SAFETY: The bounded CPU index is valid; a nonnull result owns a reference.
            let policy = unsafe { bindings::cpufreq_cpu_get(cpu) };
            if policy.is_null() {
                continue;
            }
            // SAFETY: The retained reference keeps the policy and rwsem alive.
            unsafe { bindings::down_read(ptr::addr_of_mut!((*policy).rwsem)) };
            {
                let mut requests = self.requests.lock();
                // SAFETY: The CPU index is valid; balance this extra reference below.
                let current = unsafe { bindings::cpufreq_cpu_get(cpu) };
                // SAFETY: The policy read lock excludes unfinished CREATE.
                let initialized =
                    current == policy && unsafe { !(*policy).max_freq_req.qos.is_null() };
                if initialized {
                    // SAFETY: The reference and read lock keep the policy alive.
                    unsafe { requests.as_mut().add(policy) };
                }
                if !current.is_null() {
                    // SAFETY: Balances the second lookup.
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
    #[cfg(CONFIG_CPU_FREQ)]
    unsafe extern "C" fn policy_event(
        block: *mut bindings::notifier_block,
        event: c_ulong,
        data: *mut core::ffi::c_void,
    ) -> core::ffi::c_int {
        // SAFETY: Installed only on Feed::notifier.block; owner stays pinned until unregister.
        let notifier =
            unsafe { kernel::container_of!(Opaque::cast_from(block), PolicyNotifier, block) };
        // SAFETY: The notifier is the embedded, pinned field of this live Feed.
        let feed = unsafe { &*kernel::container_of!(notifier, Feed, notifier) };
        let policy = data.cast::<bindings::cpufreq_policy>();
        let mut requests = feed.requests.lock();
        if event == bindings::CPUFREQ_REMOVE_POLICY as c_ulong {
            requests.as_mut().remove(policy);
        } else if event == bindings::CPUFREQ_CREATE_POLICY as c_ulong
            && !feed.detector.is_stopped_or_paused()
        {
            // SAFETY: CREATE supplies an initialized policy with its write lock held.
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

    /// Records a completed flight's monotonic `[published_ns, retired_ns]` interval.
    pub(crate) fn note_flight(&self, published_ns: u64, retired_ns: u64) {
        if cfg!(CONFIG_CPU_FREQ) {
            self.detector.note(published_ns, retired_ns, retired_ns);
        }
    }

    /// Consumes a pending boost request after a poll/submit pass outside `Shared::state`.
    pub(crate) fn take_pending(&self) -> bool {
        self.detector.take_pending()
    }

    #[cfg(CONFIG_CPU_FREQ)]
    fn apply(&self, enabled: bool) {
        let mut requests = self.requests.lock();
        let enabled = enabled && !self.detector.is_stopped_or_paused();
        let applied = requests.as_mut().apply(enabled);
        self.applied.store(applied, Ordering::Relaxed);
    }

    #[cfg(not(CONFIG_CPU_FREQ))]
    fn apply(&self, _enabled: bool) {}

    /// Worker context; keeps the CPU floor active until the linger deadline expires.
    pub(crate) fn run(&self) {
        loop {
            if self.detector.wanted(Self::now()) {
                self.apply(true);
                fsleep(Delta::from_millis(POLL_MS));
                continue;
            }
            self.apply(false);
            self.detector.clear_active();
            if self.detector.wanted(Self::now()) && self.detector.try_claim_active() {
                continue;
            }
            return;
        }
    }

    pub(crate) fn wait_hint(&self) -> Option<(u32, u32)> {
        if cfg!(CONFIG_UCLAMP_TASK)
            && self.detector.wanted(Self::now())
            && self.applied.load(Ordering::Relaxed)
        {
            Some((WAIT_UTIL_MIN, WAIT_HINT_MS))
        } else {
            None
        }
    }

    /// Pauses boosting before entering system sleep; call before draining `feed_wq`.
    pub(crate) fn pause(&self) {
        self.detector.pause();
    }

    /// Releases any active CPU floor after `feed_wq` has drained for system sleep.
    pub(crate) fn release_floor(&self) {
        self.apply(false);
        self.detector.clear_active();
    }

    /// Resumes refill-pressure detection after system wake or a refused sleep.
    pub(crate) fn resume(&self) {
        self.detector.resume();
    }

    /// Closes the detector on fail-stop or teardown.
    pub(crate) fn begin_shutdown(&self) {
        self.detector.begin_shutdown();
    }

    /// Unregisters cpufreq policy callbacks and removes all frequency requests.
    pub(crate) fn shutdown(&self) {
        self.begin_shutdown();
        #[cfg(CONFIG_CPU_FREQ)]
        {
            if self.registered.swap(false, Ordering::AcqRel) {
                // SAFETY: The block was registered successfully and remains pinned until return.
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
