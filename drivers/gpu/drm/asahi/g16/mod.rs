// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Classic GPU support for T8132 G16G (25G83 and 26B123), T6030 G15S (26B123) and T8122 G15G
//! (26B123).
//!
//! Classic firmware uses one ring per engine (TA, FS, compute) and expresses ordering through
//! in-ring barrier records that wait on another queue's stamp, unlike the G17 kick-ring/QID
//! model. The boot loader leaves the firmware loaded; this driver identifies the exact build,
//! adopts the inherited UAT handoff and builds the InitData graph
//! and brings the firmware to READY. All logical queues and VMs share one persistent compute
//! graph and one render graph; the commands of a submission are encoded into compute and pass
//! slots and published as *prefixes* on the firmware's queues, then retired on their stamps.
//! Everything firmware-visible that a failed command may still reference is retained until
//! reboot.
//!
//! # Plugging into the shared driver
//!
//! - [`Driver`] matches `apple,agx-t8132`, `apple,agx-t6030` and `apple,agx-t8122`. T6030 and
//!   T8122 select this backend through the boot loader's handoff (marker, layout and, on
//!   T8122, `apple,firmware-compat` 27.2; see [`adt::claims`]). Probe links a handed-over
//!   native display and, on T8132, the PMP supplier before booting firmware. It calls
//!   [`crate::driver::AsahiPlatformDriver::probe_g16`].
//! - [`Gpu`] implements [`crate::gpu::Gpu`]: GET_PARAMS identity, VM creation (`new_vm`),
//!   queues (`new_queue`, see [`queue`]) and timestamp buffers.
//! - User VMs are shared-layer [`mmu::Vm`]s on a [`mmu::Uat::new_adopted`] UAT with tracked
//!   status ([`mmu::Vm::with_tracked_status`]); `file.rs` handles VM_BIND, GEM, syncobjs,
//!   VM_STATUS(_MIRROR) and MADVISE.
//!
//! # Locking
//!
//! [`Shared::state`] is the device lock. It serializes all firmware-visible publication, the
//! device-control ring and every firmware-shared object, including the bounded waits for the
//! firmware that publication, recovery and the device controls need (up to 2 s). Fences are
//! signalled after dropping it ([`job::Completions`]). Lock order: `Shared::state` -> UAT shared
//! state -> VM exec lock -> GEM reservation. The RTKit receive callbacks run in interrupt
//! context and only set atomics and queue the poll work.
//!
//! # Module structure
//!
//! - Platform, firmware boot and UAT handoff: `boot`, `hw`, `adt`, `identity`, `regs`, `uat`,
//!   `object`, `board`, `initdata`, `channel`, `event`, `fw/{mod,initdata,channels}`.
//! - Submission engine: `engine`, `encode`, `slots`, `graph`, `poll`, `fw/{work,graph}`.
//! - Recovery, parameter buffer and power: `recovery`, `pools`, `power`, `fw/buffer`.
//! - UAPI glue and VM integration: `queue`, `command`, `gate`, `job`, `fence`, `vm`,
//!   `timestamp`, `params`, and the hooks in the shared driver (`file.rs`, `gem.rs`, `gpu.rs`,
//!   `mmu.rs`, `pgtable.rs`).

mod adt;
mod board;
mod boot;
mod budget;
mod channel;
mod command;
mod encode;
mod engine;
mod event;
mod feed;
mod fence;
pub(crate) mod fw;
mod gate;
mod graph;
pub(crate) mod hw;
mod initdata;
mod job;
mod object;
mod params;
mod poll;
mod pools;
mod power;
mod queue;
mod recovery;
mod regs;
mod resources;
mod rt;
mod sleep;
mod slots;
mod timestamp;
mod uat;
mod vm;

use core::any::Any;
use core::ops::Range;
use core::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32, AtomicU64, Ordering};

use kernel::{
    c_str,
    device::{self, Core},
    impl_has_delayed_work, impl_has_work, new_delayed_work, new_mutex, new_spinlock, new_work, of,
    platform,
    prelude::*,
    sync::{aref::ARef, Arc, Mutex, SpinLock},
    time::hrtimer::{ArcHrTimerHandle, HrTimer},
    uapi,
    workqueue::{DelayedWork, OwnedQueue, Work, WorkItem},
};

use crate::driver::{AsahiDrmDevice, AsahiPlatformDriver};
use crate::{alloc, gem, gpu, mmu, regs as shared_regs};

/// Name of the G16 platform driver.
pub(crate) const DRIVER_NAME: &CStr = c_str!("aurora-g16");

pub(crate) use hw::Config;

/// OF match table of the G16 platform driver.
const OF_TABLE: kernel::device_id::IdArray<of::DeviceId, &'static Config, 3> =
    kernel::device_id::IdArray::new([
        (
            of::DeviceId::new(c_str!("apple,agx-t8132")),
            &&hw::t8132::CONFIG,
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6030")),
            &&hw::t6030::CONFIG,
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t8122")),
            &&hw::t8122::CONFIG,
        ),
    ]);

// Probe admission waits for the PMP supplier and the native display before
// firmware starts, so the module may be loaded at device creation.
kernel::of_device_table!(
    AUTOLOAD_OF_TABLE,
    @none,
    [
        of::DeviceId::new(c_str!("apple,agx-t8132")),
        of::DeviceId::new(c_str!("apple,agx-t6030")),
        of::DeviceId::new(c_str!("apple,agx-t8122")),
    ]
);

/// Whether this backend owns `dev` under the configuration of its compatible, so that another
/// backend serving the same compatible steps aside.
pub(crate) fn claims(dev: &device::Device, cfg: &Config) -> bool {
    adt::claims(dev, cfg)
}

/// The G16 platform driver: the shared [`AsahiPlatformDriver`] probed through
/// [`AsahiPlatformDriver::probe_g16`].
///
/// `None` after a probe that failed after retained power admission: the device stays bound
/// (unbinding would detach and power off the GFX domain under the running firmware), the
/// firmware session is retained until reboot and no DRM device exists.
pub(crate) struct Driver;

impl platform::Driver for Driver {
    type IdInfo = &'static Config;
    type Data<'bound> = Option<crate::driver::AsahiPlatformDriverData<'bound>>;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn pm_ops() -> Option<&'static kernel::bindings::dev_pm_ops> {
        Some(sleep::ops())
    }

    /// The firmware session cannot be stopped by an unbind.
    fn suppress_bind_attrs() -> bool {
        true
    }

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let cfg = *info.ok_or(ENODEV)?;
        if cfg.needs_handoff_marker && !adt::claims(pdev.as_ref(), cfg) {
            return Err(ENODEV);
        }
        dev_info!(pdev.as_ref(), "G16: probing\n");
        match AsahiPlatformDriver::probe_g16(pdev, cfg) {
            Ok(driver) => Ok(Some(driver)),
            Err(error) if boot::glue::retained() => {
                dev_err!(
                    pdev.as_ref(),
                    "G16: probe failed after retained power admission ({:?}); device kept bound until reboot\n",
                    error
                );
                Ok(None)
            }
            Err(error) => Err(error),
        }
    }

    fn unbind<'bound>(_pdev: &'bound platform::Device<Core<'_>>, this: Pin<&Self::Data<'bound>>) {
        if let Some(driver) = this.get_ref() {
            if let Some(gpu) = &driver.g16 {
                // Admission pinned the module; this only confirms the pin.
                let _ = gpu.retain();
            }
            driver.close_binding();
        }
    }
}

/// Work item ID of the backup delayed poll work.
const POLL_WORK: u64 = 0;
/// Work item ID of the submitter-pinned fast event poll work.
const FAST_POLL_WORK: u64 = 1;
/// Work item ID of the asynchronous displaced pass-slot scrub work.
const SCRUB_WORK: u64 = 2;
/// Work item ID of the asynchronous CPU feed-boost work.
const FEED_WORK: u64 = 3;
/// Work item ID of the unbound fast event poll work used when pipelined (`active_flights >= 2`).
const UNBOUND_POLL_WORK: u64 = 4;
/// Capacity hint for `drm_syncobj_wait` during active submit bursts (mirroring G17's wait hint).
const WAIT_UTIL_MIN: u32 = 512;
const WAIT_HINT_MS: u32 = 8;
const WAIT_HINT_GAP_NS: u64 = 4_000_000;
const WAIT_HINT_STREAK_ON: u32 = 8;
const WAIT_HINT_LINGER_NS: u64 = 8_000_000;

/// A firmware session retained until reboot after a failed boot or backend start.
pub(crate) enum Retained {
    Orphan(KBox<boot::Orphan>),
    Booted(KBox<boot::Booted>),
    Firmware(KBox<Firmware>),
}

impl Retained {
    fn drain(&mut self) -> Result {
        match self {
            Self::Orphan(orphan) => orphan.drain(),
            Self::Booted(booted) => {
                let booted = &mut **booted;
                booted
                    .channels
                    .drain_rings(&booted.session.dev, &mut booted.events)
            }
            Self::Firmware(firmware) => {
                let firmware = &mut **firmware;
                firmware
                    .channels
                    .drain_rings(&firmware.session.dev, &mut firmware.events)
            }
        }
    }
}

/// Device state shared with the RTKit callbacks and the poll work.
#[pin_data]
pub(crate) struct Shared {
    pub(crate) dev: ARef<device::Device>,
    /// Keeps the DRM device alive for a deferred wedged uevent. Firmware-visible owners
    /// already retain the session until reboot; this reference follows the same lifetime.
    pub(crate) drm: ARef<AsahiDrmDevice>,
    /// A full fail-stop or a permanently exhausted hardware class needs a uevent.
    pub(crate) wedged: AtomicBool,
    pub(crate) wedged_notified: AtomicBool,
    /// Latched fail-stop: every VM fails with EIO until reboot.
    pub(crate) crashed: AtomicBool,
    /// Set by the firmware's EP 0x20 event message; consumed by the poll work.
    pub(crate) evented: AtomicBool,
    /// Set by external state changes (VM teardown, unstarted queue cancellation) so the next
    /// `poll_step()` re-evaluates `publish_checked()` even while unrelated flights are active.
    pub(crate) publish_wake: AtomicBool,
    /// Set on VM close and when its last unpublished render reservation is released.
    /// Re-armed while a closed VM still holds a lease awaiting reclaim.
    pub(crate) closed_vm_reclaim: AtomicBool,
    /// CPU that last submitted work, for placing the per-CPU completion worker.
    pub(crate) submitter: AtomicU32,
    /// CPU whose cluster currently restricts `events_unbound` (`u32::MAX` before first pin).
    unbound_cluster_cpu: AtomicU32,
    /// Serializes best-effort `events_unbound.restrict()` updates without blocking concurrent
    /// submitters on `wq_pool_mutex`.
    unbound_pinning: AtomicBool,
    /// Number of flights currently active under `Shared::state`, for poll worker placement.
    pub(crate) active_flights: AtomicU32,
    /// Smoothed recent flight duration in microseconds (0 before first retirement), used to
    /// shorten the fallback hrtimer interval on sub-100 us workloads.
    pub(crate) ewma_flight_us: AtomicU32,
    /// Monotonic timestamp (ns) of the previous submit, for active-burst wait hints.
    pub(crate) last_submit_ns: AtomicU64,
    /// Consecutive back-to-back submits within [`WAIT_HINT_GAP_NS`].
    pub(crate) wait_hint_streak: AtomicU32,
    /// Monotonic deadline (ns) until which `syncobj_wait_hint` stays active (0 when expired).
    pub(crate) wait_hint_until_ns: AtomicU64,
    /// Next armed hrtimer expiry (monotonic ns, 0 when disarmed).
    pub(crate) resnap_deadline_ns: AtomicU64,
    /// Next 1-jiffy backup delayed-work schedule bound (monotonic ns).
    pub(crate) backup_deadline_ns: AtomicU64,
    // --- RTKit callback state and a retained failed session. ---
    /// The first latched error, as a negative errno.
    pub(crate) error: AtomicI32,
    /// The RTKit receive callbacks may queue the poll work.
    pub(crate) started: AtomicBool,
    /// Firmware DATA, where the only RTKit buffer accepted lies.
    pub(crate) data: Range<u64>,
    /// Physical address of the accepted RTKit buffer, 0 before.
    pub(crate) rtkit_buffer: AtomicU64,
    /// A firmware session whose start or backend start failed: retained, rings still drained.
    pub(crate) orphaned: AtomicBool,
    /// From a system sleep's `.prepare` until the firmware restarted: nothing reads its
    /// registers and nothing is published.
    pub(crate) sleeping: AtomicBool,
    /// Firmware recovery is active under `Shared::state` (may sleep up to 1000 ms).
    pub(crate) recovering: AtomicBool,
    /// Whether any base compute unit (`0..BASE_COMPUTE_UNITS`) has been quarantined.
    pub(crate) base_compute_quarantined: AtomicBool,
    /// Bitmask of permanently quarantined device-wide compute units (`0..COMPUTE_UNITS`).
    pub(crate) quarantined_compute_mask: AtomicU32,
    /// Whether any base render pass slot (`0..BASE_RENDER_SLOTS`) has been quarantined.
    pub(crate) base_render_quarantined: AtomicBool,
    /// Bitmask of permanently quarantined device-wide PB/UMA allocator slots (`0..PB_SLOTS`).
    pub(crate) quarantined_pb_mask: AtomicU32,
    /// Round-robin cursor assigning each rendering VM a `[home, alt]` pair in `0..WARM_PB_SLOTS`.
    pub(crate) next_pb_slot: AtomicU32,
    /// Live open VMs that own a `RenderArena`, used to size multi-VM pass-slot headroom.
    pub(crate) active_render_vms: Arc<AtomicU32>,
    /// The GFX power state word from before a system sleep.
    sleep_ps: AtomicU32,
    /// The stop left to a system sleep's commit, until `.complete` finishes it.
    sleep_commit: AtomicPtr<core::ffi::c_void>,
    /// Dedicated high-priority per-CPU workqueue for immediate completion polling.
    pub(crate) events: OwnedQueue,
    /// Dedicated high-priority unbound workqueue for pipelined (`active_flights >= 2`) polling.
    pub(crate) events_unbound: OwnedQueue,
    /// Dedicated unbound workqueue for asynchronous off-lock pass-slot WC scrubbing.
    pub(crate) scrub_wq: OwnedQueue,
    /// Host-refill CPU frequency floor actuator (mirroring G17's feed worker).
    pub(crate) feed: Arc<feed::Feed>,
    /// Dedicated unbound workqueue for cpufreq `FREQ_QOS_MIN` updates outside `Shared::state`.
    pub(crate) feed_wq: OwnedQueue,
    #[pin]
    pub(crate) orphan: Mutex<Option<Retained>>,
    #[pin]
    pub(crate) poll: DelayedWork<Shared, POLL_WORK>,
    #[pin]
    pub(crate) fast_poll: Work<Shared, FAST_POLL_WORK>,
    #[pin]
    pub(crate) fast_poll_unbound: Work<Shared, UNBOUND_POLL_WORK>,
    #[pin]
    pub(crate) scrub_queue: SpinLock<KBox<slots::ScrubQueue>>,
    #[pin]
    pub(crate) scrub_work: Work<Shared, SCRUB_WORK>,
    #[pin]
    pub(crate) feed_work: Work<Shared, FEED_WORK>,
    // --- The completion/repoll hrtimer and the handle of its current expiry (dropping it
    // cancels the expiry; see `Shared::rearm`). ---
    #[pin]
    pub(crate) resnap: HrTimer<Shared>,
    #[pin]
    pub(crate) resnap_handle: Mutex<Option<ArcHrTimerHandle<Shared>>>,
    /// The device lock. `None` before boot completes.
    #[pin]
    pub(crate) state: Mutex<Option<KBox<Firmware>>>,
}

impl_has_delayed_work! {
    impl HasDelayedWork<Shared, POLL_WORK> for Shared { self.poll }
}

impl_has_work! {
    impl HasWork<Shared, FAST_POLL_WORK> for Shared { self.fast_poll }
}

impl_has_work! {
    impl HasWork<Shared, SCRUB_WORK> for Shared { self.scrub_work }
}

impl_has_work! {
    impl HasWork<Shared, FEED_WORK> for Shared { self.feed_work }
}

impl_has_work! {
    impl HasWork<Shared, UNBOUND_POLL_WORK> for Shared { self.fast_poll_unbound }
}

impl WorkItem<POLL_WORK> for Shared {
    type Pointer = Arc<Shared>;
    fn run(this: Arc<Shared>) {
        if this.orphaned.load(Ordering::Acquire) {
            this.drain_orphan();
            return;
        }
        this.run_poll();
    }
}

impl WorkItem<FAST_POLL_WORK> for Shared {
    type Pointer = Arc<Shared>;
    fn run(this: Arc<Shared>) {
        if this.orphaned.load(Ordering::Acquire) {
            this.drain_orphan();
            return;
        }
        this.run_poll();
    }
}

impl WorkItem<SCRUB_WORK> for Shared {
    type Pointer = Arc<Shared>;
    fn run(this: Arc<Shared>) {
        this.run_scrubs();
    }
}

impl WorkItem<FEED_WORK> for Shared {
    type Pointer = Arc<Shared>;
    fn run(this: Arc<Shared>) {
        this.feed.run();
    }
}

impl WorkItem<UNBOUND_POLL_WORK> for Shared {
    type Pointer = Arc<Shared>;
    fn run(this: Arc<Shared>) {
        if this.orphaned.load(Ordering::Acquire) {
            this.drain_orphan();
            return;
        }
        this.run_poll();
    }
}

impl Shared {
    /// Records the submitting CPU and burst cadence for completion placement and wait hints.
    /// Runs in process context during SUBMIT; keeps `events_unbound` pinned to the submitter's
    /// CPU cluster so pipelined completion polls stay on the same cluster (avoiding E-core
    /// wake bounces while still sparing the submitter CPU itself).
    pub(crate) fn note_submitter(&self) {
        let cpu = kernel::cpu::CpuId::current();
        self.submitter.store(cpu.as_u32(), Ordering::Relaxed);
        let pinned = self.unbound_cluster_cpu.load(Ordering::Acquire);
        if pinned != cpu.as_u32()
            && !kernel::cpu::CpuId::from_u32(pinned)
                .is_some_and(|p| kernel::cpumask::Cpumask::cluster_of(p).test(cpu))
            && self
                .unbound_pinning
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Relaxed)
                .is_ok()
        {
            if self
                .events_unbound
                .restrict(kernel::cpumask::Cpumask::cluster_of(cpu))
                .is_ok()
            {
                self.unbound_cluster_cpu
                    .store(cpu.as_u32(), Ordering::Release);
            }
            self.unbound_pinning.store(false, Ordering::Release);
        }
        let now = engine::now_ns();
        let prev = self.last_submit_ns.swap(now, Ordering::Relaxed);
        if prev != 0 && now.wrapping_sub(prev) <= WAIT_HINT_GAP_NS {
            let streak = self
                .wait_hint_streak
                .fetch_add(1, Ordering::Relaxed)
                .saturating_add(1);
            if streak >= WAIT_HINT_STREAK_ON {
                self.wait_hint_until_ns
                    .store(now.saturating_add(WAIT_HINT_LINGER_NS), Ordering::Relaxed);
            }
        } else {
            self.wait_hint_streak.store(1, Ordering::Relaxed);
        }
    }

    /// Updates the lock-free EWMA of recent retired flight durations (microseconds).
    /// Decays faster downward than upward so transitions from heavy flights into short
    /// bursts tighten the adaptive repoll window within a few completions.
    pub(crate) fn note_flight_us(&self, us: u32) {
        let prev = self.ewma_flight_us.load(Ordering::Relaxed);
        let next = if prev == 0 {
            us
        } else if us < prev {
            prev.saturating_add(us) / 2
        } else {
            (prev.saturating_mul(3).saturating_add(us)) / 4
        };
        self.ewma_flight_us.store(next, Ordering::Relaxed);
    }

    /// Temporary `drm_syncobj_wait` capacity hint while a client is submitting back to back.
    pub(crate) fn syncobj_wait_hint(&self) -> Option<(u32, u32)> {
        self.feed.wait_hint().or_else(|| {
            let until = self.wait_hint_until_ns.load(Ordering::Relaxed);
            if cfg!(CONFIG_UCLAMP_TASK) && until != 0 && engine::now_ns() <= until {
                Some((WAIT_UTIL_MIN, WAIT_HINT_MS))
            } else {
                None
            }
        })
    }

    /// Wakes the CPU feed-boost worker outside `Shared::state` when refill gaps cross threshold.
    pub(crate) fn queue_feed(self: &Arc<Self>) {
        if !self.sleeping.load(Ordering::Acquire) && self.feed.take_pending() {
            let _ = self.feed_wq.enqueue::<Arc<Shared>, FEED_WORK>(self.clone());
        }
    }

    /// Last recorded submitting CPU, if valid.
    pub(crate) fn submitter_cpu(&self) -> Option<kernel::cpu::CpuId> {
        kernel::cpu::CpuId::from_u32(self.submitter.load(Ordering::Relaxed))
    }

    /// Queues the poll work immediately. Callable from any context.
    pub(crate) fn kick(self: &Arc<Self>) {
        self.evented.store(true, Ordering::Release);
        self.poll_now();
    }

    /// Marks external publish-relevant state dirty and queues a poll so pending jobs re-evaluate
    /// without waiting for the 1 ms housekeeping tick.
    pub(crate) fn wake_publish(self: &Arc<Self>) {
        self.publish_wake.store(true, Ordering::Release);
        if self.started.load(Ordering::Acquire) && !self.sleeping.load(Ordering::Acquire) {
            self.poll_now();
        }
    }

    /// Arms and wakes the poll worker to reclaim closed VM leases.
    pub(crate) fn wake_closed_vm_reclaim(self: &Arc<Self>) {
        self.closed_vm_reclaim.store(true, Ordering::Release);
        self.wake_publish();
    }

    /// Latches the fail-stop state.
    pub(crate) fn fail_stop(&self, error: Error) {
        self.feed.begin_shutdown();
        let _ =
            self.error
                .compare_exchange(0, error.to_errno(), Ordering::AcqRel, Ordering::Acquire);
        self.wedged.store(true, Ordering::Release);
        if !self.crashed.swap(true, Ordering::AcqRel) {
            dev_err!(
                self.dev.as_ref(),
                "G16: fail-stop ({:?}); GPU unusable until reboot\n",
                error
            );
        }
    }

    /// The latched error, if any.
    pub(crate) fn check(&self) -> Result {
        if !self.crashed.load(Ordering::Acquire) {
            return Ok(());
        }
        match self.error.load(Ordering::Acquire) {
            0 => Err(EIO),
            e => Err(Error::from_errno(e)),
        }
    }

    /// Keeps a firmware session that did not complete until reboot; its receive rings stay
    /// drained by the poll work.
    #[cold]
    #[inline(never)]
    pub(crate) fn retain_failed(&self, retained: Retained) {
        let mut slot = self.orphan.lock();
        if slot.is_some() {
            // Never replace and drop an already firmware-reachable session.
            core::mem::forget(retained);
            dev_err!(
                self.dev.as_ref(),
                "G16: additional failed session retained without drain\n"
            );
            return;
        }
        *slot = Some(retained);
        self.orphaned.store(true, Ordering::Release);
    }

    /// Drains the receive rings of an orphaned session.
    fn drain_orphan(&self) {
        if let Some(orphan) = &mut *self.orphan.lock() {
            let _ = orphan.drain();
        }
    }
}

/// Everything that exists while the firmware runs, under the device lock.
///
/// Nothing in here is ever freed while the firmware can reach it: a G16 firmware session cannot
/// be stopped, so dropping this structure leaks the firmware-visible parts on purpose
/// ([`Gpu::retain`]).
pub(crate) struct Firmware {
    /// Static hardware configuration for this GPU instance.
    pub(crate) cfg: &'static hw::Config,
    /// RTKit, MMIO, regions, the adopted UAT and the host window.
    pub(crate) session: boot::Session,
    /// The initdata graph and its live words (Globals, Control, HardwareData, ...).
    pub(crate) init: initdata::InitData,
    /// Firmware objects: the firmware VA allocator and the retained objects.
    pub(crate) objects: object::Objects,
    pub(crate) private_factory: object::Factory,
    /// Submit channels, the device-control ring and the receive rings.
    pub(crate) channels: channel::Channels,
    /// Decoded firmware events (stage counters, last error, growth requests).
    pub(crate) events: event::Events,
    /// The compute and render graphs.
    pub(crate) graph: graph::Graphs,
    /// Compute slots, pass slots and clustering buffers.
    pub(crate) slots: slots::Slots,
    /// Pending jobs, prefixes, horizons and flights.
    pub(crate) engine: engine::Engine,
    /// Halt/resume, kills, deadlines and fail-stop.
    pub(crate) recovery: recovery::Recovery,
    /// PMP thermal vote and firmware DVFS ceiling state.
    pub(crate) power: power::Power,
    /// Render UMA/PB allocators, their spare set and parameter-buffer growth.
    pub(crate) pools: KBox<pools::Pools>,
    /// Live VMs and their exclusive TTBAT slots, and the client aliases of every user VM.
    pub(crate) vms: Arc<vm::Registry>,
    /// Monotonic timestamp (ns) of the last full housekeeping pass (power, health, hang poll).
    pub(crate) last_housekeeping_ns: u64,
    /// Monotonic timestamp (ns) of the last auxiliary (log/ktrace/stats) ring drain.
    pub(crate) last_aux_drain_ns: u64,
}

/// A G16 GPU.
pub(crate) struct Gpu {
    cfg: &'static Config,
    /// Identification read at boot.
    pub(crate) id: regs::Identity,
    /// Highest firmware performance state, kHz.
    pub(crate) max_frequency_khz: u32,
    ids: gpu::SequenceIDs,
    pub(crate) shared: Arc<Shared>,
    /// The adopted UAT, for creating and binding user VMs without the device lock.
    pub(crate) uat: Arc<mmu::Uat>,
    /// Live VM registry, kept directly on `Gpu` so `vm_closed()` still breaks resource-cache
    /// cycles even if `shared.state` is absent (fail-stop or teardown).
    vms: Arc<vm::Registry>,
}

impl Gpu {
    /// Admits and boots a G16 GPU: identity checks, PMP AGX vote, RTKit, UAT handoff, initdata,
    /// READY, then the compute and render graphs. On success the module is pinned: the
    /// firmware session lasts until reboot.
    #[inline(never)]
    pub(crate) fn new(
        pdev: &platform::Device<Core<'_>>,
        dev: &AsahiDrmDevice,
        cfg: &'static Config,
        res: shared_regs::G16Resources,
    ) -> Result<Arc<Gpu>> {
        // Admission (probe-time checks), then the firmware start to READY.
        let power = power::Power::new(dev.as_ref().as_ref())?;
        let admitted = boot::admit(pdev, dev, cfg, res)?;
        let data = admitted.adt.data.clone();
        // pin_init exposes initialized fields by name, so `dev` is shadowed after
        // the platform-device field. Retain the typed DRM device before that point.
        let drm_device: ARef<AsahiDrmDevice> = dev.into();
        let events = OwnedQueue::new_percpu_reclaim(c_str!("aurora_g16_events"), true)?;
        let events_unbound =
            OwnedQueue::new_unbound_reclaim(c_str!("aurora_g16_events_unbound"), true)?;
        let scrub_wq = OwnedQueue::new_unbound_reclaim(c_str!("aurora_g16_scrub"), false)?;
        let feed = feed::Feed::new()?;
        let feed_wq = OwnedQueue::new_unbound_reclaim(c_str!("aurora_g16_feed"), false)?;
        let active_render_vms = Arc::new(AtomicU32::new(0), GFP_KERNEL)?;
        let scrub_queue = slots::ScrubQueue::new_boxed()?;
        let shared = Arc::pin_init(
            pin_init!(Shared {
                dev: dev.as_ref().as_ref().into(),
                drm: drm_device,
                wedged: AtomicBool::new(false),
                wedged_notified: AtomicBool::new(false),
                crashed: AtomicBool::new(false),
                evented: AtomicBool::new(false),
                publish_wake: AtomicBool::new(false),
                closed_vm_reclaim: AtomicBool::new(false),
                submitter: AtomicU32::new(u32::MAX),
                unbound_cluster_cpu: AtomicU32::new(u32::MAX),
                unbound_pinning: AtomicBool::new(false),
                active_flights: AtomicU32::new(0),
                ewma_flight_us: AtomicU32::new(0),
                last_submit_ns: AtomicU64::new(0),
                wait_hint_streak: AtomicU32::new(0),
                wait_hint_until_ns: AtomicU64::new(0),
                resnap_deadline_ns: AtomicU64::new(0),
                backup_deadline_ns: AtomicU64::new(0),
                error: AtomicI32::new(0),
                started: AtomicBool::new(false),
                data,
                rtkit_buffer: AtomicU64::new(0),
                orphaned: AtomicBool::new(false),
                sleeping: AtomicBool::new(false),
                recovering: AtomicBool::new(false),
                base_compute_quarantined: AtomicBool::new(false),
                quarantined_compute_mask: AtomicU32::new(0),
                base_render_quarantined: AtomicBool::new(false),
                quarantined_pb_mask: AtomicU32::new(0),
                next_pb_slot: AtomicU32::new(0),
                active_render_vms,
                sleep_ps: AtomicU32::new(0),
                sleep_commit: AtomicPtr::new(core::ptr::null_mut()),
                events,
                events_unbound,
                scrub_wq,
                feed,
                feed_wq,
                orphan <- new_mutex!(None, "g16::Shared::orphan"),
                poll <- new_delayed_work!("g16::Shared::poll"),
                fast_poll <- new_work!("g16::Shared::fast_poll"),
                fast_poll_unbound <- new_work!("g16::Shared::fast_poll_unbound"),
                scrub_queue <- new_spinlock!(scrub_queue, "g16::Shared::scrub_queue"),
                scrub_work <- new_work!("g16::Shared::scrub_work"),
                feed_work <- new_work!("g16::Shared::feed_work"),
                resnap <- HrTimer::new(),
                resnap_handle <- new_mutex!(None, "g16::Shared::resnap_handle"),
                state <- new_mutex!(None, "g16::Shared::state"),
            }),
            GFP_KERNEL,
        )?;
        let booted = boot::start(admitted, dev, cfg, &shared)?;
        let uat = booted.session.uat();
        let (id, max_frequency_khz) = (booted.id, booted.max_frequency_khz);
        // The backend: graphs, slots, pools, engine, recovery and VM registry.
        let firmware = Firmware::start_backend(dev, cfg, booted, &shared, power)?;
        let vms = firmware.vms.clone();
        *shared.state.lock() = Some(firmware);
        // The engine's statistics file (a debugfs failure does not fail the probe).
        if let Err(error) = shared.start_engine() {
            dev_warn!(dev.as_ref(), "G16: no aurora-g16 debugfs ({:?})\n", error);
        }
        let gpu = Arc::new(
            Gpu {
                cfg,
                id,
                max_frequency_khz,
                ids: Default::default(),
                shared,
                uat,
                vms,
            },
            GFP_KERNEL,
        )?;
        gpu.retain()?;
        Ok(gpu)
    }

    /// Keeps the firmware session and this module alive until reboot.
    ///
    /// Takes the module reference once; admission already took it, so this only confirms it.
    /// Nothing firmware-reachable is freed on any path: the RTKit callbacks hold [`Shared`],
    /// which holds the [`Firmware`] (or the [`boot::Orphan`] of a failed start) until reboot.
    pub(crate) fn retain(&self) -> Result {
        boot::glue::retain_module()
    }
}

impl Firmware {
    /// Builds the graphs and the engine after READY. On failure the firmware stays at READY,
    /// retained as an orphan of `shared`.
    ///
    /// Out of line, like [`boot::admit`] and [`boot::start`]: each of [`Gpu::new`]'s phases
    /// has a large frame, and the probe's stack holds them only one at a time.
    #[inline(never)]
    fn start_backend(
        dev: &AsahiDrmDevice,
        cfg: &'static hw::Config,
        mut booted: KBox<boot::Booted>,
        shared: &Arc<Shared>,
        mut power: power::Power,
    ) -> Result<KBox<Firmware>> {
        dev_info!(
            dev.as_ref(),
            "G16: firmware at READY, starting the backend\n"
        );
        power.set_table(booted.init.rows);
        let backend = (|| -> Result<_> {
            let graph_session = graph::Graphs::prepare(&mut booted.objects)?;
            let slots = slots::Slots::new()?;
            let pools = pools::Pools::new_boxed(dev)?;
            let vms = vm::Registry::new()?;
            let engine = engine::Engine::new()?;
            let recovery = recovery::Recovery::new()?;
            let render_cmds = resources::RenderCmds::new()?;
            let compute_cmds = resources::ComputeCmds::new()?;
            let private_factory = booted.objects.private_factory(render_cmds, compute_cmds);
            let graph = graph::Graphs::finish(dev, graph_session, &private_factory)?;
            let slot = KBox::<Firmware>::new_uninit(GFP_KERNEL)?;
            Ok((graph, private_factory, slots, pools, vms, engine, recovery, slot))
        })();
        let (graph, private_factory, slots, pools, vms, engine, recovery, slot) = match backend {
            Ok(backend) => backend,
            Err(error) => {
                dev_err!(
                    dev.as_ref(),
                    "G16: backend start failed ({:?}); firmware retained at READY until reboot\n",
                    error
                );
                shared.retain_failed(Retained::Booted(booted));
                return Err(error);
            }
        };
        let boot::Booted {
            session,
            init,
            objects,
            channels,
            events,
            ..
        } = KBox::into_inner(booted);
        let mut firmware = slot
            .write_init(init!(Firmware {
                cfg,
                session,
                init,
                private_factory,
                objects,
                channels,
                events,
                graph,
                slots,
                engine,
                recovery,
                power,
                pools,
                vms,
                last_housekeeping_ns: 0,
                last_aux_drain_ns: 0,
            }))
            .unwrap_or_else(|e| match e {});
        if let Err(error) = firmware.init_graphs() {
            dev_err!(
                dev.as_ref(),
                "G16: backend start failed ({:?}); firmware retained at READY until reboot\n",
                error
            );
            shared.retain_failed(Retained::Firmware(firmware));
            return Err(error);
        }
        Ok(firmware)
    }
}

impl gpu::Gpu for Gpu {
    fn syncobj_wait_hint(&self) -> Option<(u32, u32)> {
        self.shared.syncobj_wait_hint()
    }

    fn max_cmdbuf_size(&self) -> usize {
        command::MAX_CMDBUF
    }

    fn stats_snapshot(&self) -> Option<Arc<crate::stats::StatsSnapshot>> {
        None
    }
    fn as_any(&self) -> &dyn Any {
        self
    }

    fn arc_as_any(self: Arc<Self>) -> Arc<dyn Any + Sync + Send> {
        self as Arc<dyn Any + Sync + Send>
    }

    fn manager(&self) -> Option<&dyn gpu::GpuManager> {
        None
    }

    fn init(&self) -> Result {
        Ok(())
    }

    fn ids(&self) -> &gpu::SequenceIDs {
        &self.ids
    }

    fn is_crashed(&self) -> bool {
        self.shared.crashed.load(Ordering::Acquire)
    }

    fn get_params(&self, params: &mut uapi::drm_asahi_params_global) -> Result {
        params::fill(self, params)
    }

    fn supports_vm_status(&self) -> bool {
        true
    }

    fn queue_limits(&self) -> Option<uapi::drm_asahi_queue_limits> {
        Some(params::queue_limits())
    }

    fn base_clock_hz(&self) -> u32 {
        let frequency: u64;
        // SAFETY: CNTFRQ_EL0 is a read-only register containing the architectural counter rate.
        unsafe { core::arch::asm!("mrs {frequency}, CNTFRQ_EL0", frequency = out(reg) frequency) };
        frequency as u32
    }

    fn uat_geometry(&self) -> mmu::UatGeometry {
        mmu::UatGeometry {
            ias: self.cfg.uat_ias,
            oas: self.cfg.uat_oas,
        }
    }

    fn user_range(&self) -> Range<u64> {
        hw::USER_RANGE
    }

    fn tracked_submission(&self) -> bool {
        true
    }

    /// G16 programs no fault mode into the firmware: an unmapped shader access behaves as the
    /// firmware's default fault mode makes it. `fault_control` (default 0xb) stays the opt-out.
    fn soft_faults(&self) -> bool {
        true
    }

    fn prepare_new_objects(&self) -> bool {
        true
    }

    // VM_BIND operations per call at most.
    fn bind_batch_limit(&self) -> Option<u32> {
        Some(params::MAX_BINDS)
    }

    fn new_vm(&self, kernel_range: Range<u64>) -> Result<mmu::Vm> {
        vm::new_vm(self, kernel_range, 0)
    }
    fn new_vm_for_file(&self, kernel_range: Range<u64>, file_id: u64) -> Result<mmu::Vm> {
        vm::new_vm(self, kernel_range, file_id)
    }
    fn vm_closed(&self, vm: &mmu::Vm) {
        self.vms.close(vm.id());
        self.shared.wake_closed_vm_reclaim();
    }

    fn new_queue(
        &self,
        vm: mmu::Vm,
        _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        _ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        priority: u32,
        usc_exec_base: u64,
        usage: Arc<crate::usage::Client>,
    ) -> Result<KBox<dyn crate::queue::Queue>> {
        Ok(KBox::new(
            queue::Queue::new(self, vm, priority, usc_exec_base, usage)?,
            GFP_KERNEL,
        )?)
    }

    fn map_timestamp_buffer(
        &self,
        bo: gem::ObjectRef,
        range: Range<usize>,
        _window: crate::timestamp::WindowPermit,
    ) -> Result<crate::timestamp::Buffer> {
        timestamp::map(self, bo, range)
    }
}
