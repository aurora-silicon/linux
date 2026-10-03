// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Detects sustained host refill gaps and temporarily requests CPU capacity.
//! Detector updates are lock-free. The device's single feed worker owns the
//! frequency requests; its lock never nests inside a driver publication lock.
//! Shutdown first closes the detector, joins its producers and the worker,
//! then removes requests. All request storage and policy references are owned
//! by this device, independently of other GPU instances.

use crate::hw::t8140::qos::CLOCK_HZ;
use core::{
    pin::Pin,
    ptr,
    sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
};
use kernel::{
    bindings, new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
    time::{delay::fsleep, ClockSource, Delta, Monotonic},
    types::Opaque,
};

const MAX_REFILL_TICKS: u64 = CLOCK_HZ / 500;
const STREAK_ON: u32 = 8;
const STREAK_MAX: u32 = 64;
const LINGER_NS: u64 = 8_000_000;
const POLL_MS: i64 = 2;
const FLOOR_PERCENT: u64 = 50;
const WAIT_UTIL_MIN: u32 = 512;
const WAIT_HINT_MS: u32 = 8;
const MAX_POLICIES: usize = 8;

struct Detector {
    last_end: AtomicU64,
    idle: AtomicU64,
    busy: AtomicU64,
    streak: AtomicU32,
    until: AtomicU64,
    active: AtomicBool,
    pending: AtomicBool,
    stopping: AtomicBool,
}

impl Detector {
    const fn new() -> Self {
        Self {
            last_end: AtomicU64::new(0),
            idle: AtomicU64::new(0),
            busy: AtomicU64::new(0),
            streak: AtomicU32::new(0),
            until: AtomicU64::new(0),
            active: AtomicBool::new(false),
            pending: AtomicBool::new(false),
            stopping: AtomicBool::new(false),
        }
    }

    fn decay_add(sum: &AtomicU64, add: u64) -> u64 {
        let mut next = 0;
        let _ = sum.fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
            next = value - value / 8 + add;
            Some(next)
        });
        next
    }

    fn note(&self, start: u64, end: u64, now: u64) {
        if end <= start {
            return;
        }
        let previous = self.last_end.fetch_max(end, Ordering::AcqRel);
        let (idle, busy, paced) = if previous == 0 {
            (0, end - start, true)
        } else if start >= previous {
            let gap = start - previous;
            if gap < MAX_REFILL_TICKS {
                (gap, end - start, false)
            } else {
                (0, end - start, true)
            }
        } else if end > previous {
            (0, end - previous, false)
        } else {
            (0, 0, false)
        };
        let idle_sum = Self::decay_add(&self.idle, idle);
        let busy_sum = Self::decay_add(&self.busy, busy);
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
        if streak >= STREAK_ON && idle_sum.saturating_mul(4) >= busy_sum {
            self.until.store(now + LINGER_NS, Ordering::Release);
            if !self.active.swap(true, Ordering::AcqRel) {
                self.pending.store(true, Ordering::Release);
            }
        }
    }

    fn wanted(&self, now: u64) -> bool {
        !self.stopping.load(Ordering::Acquire) && now < self.until.load(Ordering::Acquire)
    }
}

#[pin_data]
struct Requests {
    #[pin]
    requests: Opaque<[bindings::freq_qos_request; MAX_POLICIES]>,
    policies: [*mut bindings::cpufreq_policy; MAX_POLICIES],
    maxima: [u32; MAX_POLICIES],
    applied: [u32; MAX_POLICIES],
    count: usize,
}

// SAFETY: Each nonnull policy retains cpufreq_cpu_get's reference. Access to
// the pinned request array and metadata is exclusive under Feed::requests.
unsafe impl Send for Requests {}

impl Requests {
    fn new() -> Self {
        Self {
            requests: Opaque::zeroed(),
            policies: [ptr::null_mut(); MAX_POLICIES],
            maxima: [0; MAX_POLICIES],
            applied: [0; MAX_POLICIES],
            count: 0,
        }
    }

    fn register(self: Pin<&mut Self>) {
        let this = self.project();
        if *this.count != 0 {
            return;
        }
        // SAFETY: nr_cpu_ids is initialized before driver probe.
        let cpus = unsafe { bindings::nr_cpu_ids };
        for cpu in 0..cpus {
            // SAFETY: Accepts any CPU number; a nonnull result owns one reference.
            let policy = unsafe { bindings::cpufreq_cpu_get(cpu) };
            if policy.is_null() {
                continue;
            }
            let count = *this.count;
            if count < MAX_POLICIES && !this.policies[..count].contains(&policy) {
                // SAFETY: The policy is referenced. This zeroed, unregistered
                // slot is pinned and remains alive until remove_request.
                let result = unsafe {
                    bindings::freq_qos_add_request(
                        ptr::addr_of_mut!((*policy).constraints),
                        this.requests
                            .get()
                            .cast::<bindings::freq_qos_request>()
                            .add(count),
                        bindings::freq_qos_req_type_FREQ_QOS_MIN,
                        bindings::FREQ_QOS_MIN_DEFAULT_VALUE as i32,
                    )
                };
                if result >= 0 {
                    this.policies[count] = policy;
                    // SAFETY: cpuinfo is immutable while this policy is referenced.
                    this.maxima[count] = unsafe { (*policy).cpuinfo.max_freq };
                    this.applied[count] = bindings::FREQ_QOS_MIN_DEFAULT_VALUE as u32;
                    *this.count += 1;
                    continue;
                }
            }
            // SAFETY: Balances the reference acquired above for a duplicate,
            // unavailable slot or failed registration. Successful slots retain it.
            unsafe { bindings::cpufreq_cpu_put(policy) };
        }
    }

    fn apply(mut self: Pin<&mut Self>, enabled: bool) -> bool {
        if enabled {
            self.as_mut().register();
        }
        let this = self.project();
        for index in 0..*this.count {
            let floor = if enabled {
                (u64::from(this.maxima[index]) * FLOOR_PERCENT / 100) as u32
            } else {
                0
            };
            let value = if floor == 0 {
                bindings::FREQ_QOS_MIN_DEFAULT_VALUE as u32
            } else {
                floor
            };
            if this.applied[index] == value {
                continue;
            }
            this.applied[index] = value;
            // SAFETY: This slot is registered, pinned and exclusively locked;
            // its referenced policy keeps the constraints alive.
            unsafe {
                bindings::freq_qos_update_request(
                    this.requests
                        .get()
                        .cast::<bindings::freq_qos_request>()
                        .add(index),
                    value as i32,
                )
            };
        }
        *this.count != 0 && enabled
    }

    fn clear(self: Pin<&mut Self>) {
        let this = self.project();
        for index in 0..*this.count {
            // SAFETY: Registered pinned slot, exclusively locked. Remove the
            // constraint link before releasing its policy reference.
            unsafe {
                bindings::freq_qos_remove_request(
                    this.requests
                        .get()
                        .cast::<bindings::freq_qos_request>()
                        .add(index),
                );
                bindings::cpufreq_cpu_put(this.policies[index]);
            }
            this.policies[index] = ptr::null_mut();
        }
        *this.count = 0;
    }
}

#[pin_data(PinnedDrop)]
pub(crate) struct Feed {
    detector: Detector,
    #[pin]
    requests: Mutex<Requests>,
    applied: AtomicBool,
}

impl Feed {
    pub(crate) fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
                detector: Detector::new(),
                requests <- new_mutex!(Requests::new(), "G17 CPU feed"),
                applied: AtomicBool::new(false),
            }),
            GFP_KERNEL,
        )
    }

    fn now() -> u64 {
        Monotonic::ktime_get() as u64
    }

    pub(crate) fn note_render_pass(&self, start: u64, end: u64) {
        self.detector.note(start, end, Self::now());
    }

    /// Called after a firmware event pass, before queuing the single feed work item.
    pub(crate) fn take_pending(&self) -> bool {
        self.detector.pending.swap(false, Ordering::AcqRel)
    }

    fn apply(&self, enabled: bool) {
        let mut requests = self.requests.lock();
        let enabled = enabled && !self.detector.stopping.load(Ordering::Acquire);
        let applied = requests.as_mut().apply(enabled);
        self.applied.store(applied, Ordering::Relaxed);
    }

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

    /// Call before disabling/joining the event producer and actuator work items.
    pub(crate) fn begin_shutdown(&self) {
        self.detector.stopping.store(true, Ordering::Release);
    }

    /// Call after work items are joined. Removing requests withdraws their floor.
    pub(crate) fn shutdown(&self) {
        self.begin_shutdown();
        self.requests.lock().as_mut().clear();
        self.applied.store(false, Ordering::Relaxed);
    }
}

#[pinned_drop]
impl PinnedDrop for Feed {
    fn drop(self: Pin<&mut Self>) {
        self.shutdown();
    }
}
