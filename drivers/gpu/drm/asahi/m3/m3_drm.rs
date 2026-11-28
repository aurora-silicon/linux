// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! M3 through the common Asahi GEM, GPUVM and render-node frontend.
use core::ops::Range;
use core::any::Any;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};
use kernel::{
    bindings,
    device::Core,
    drm, new_mutex, platform, prelude::*,
    sync::{aref::ARef, Arc, Mutex},
    time::{delay::fsleep, Delta, Instant, Monotonic},
    uapi,
};
use crate::{alloc, driver, gem, gpu, m3::hw, mmu, queue};

extern "C" {
    fn apple_sleep_dry_run() -> bool;
    fn asahi_g17_commit_required() -> bool;
    fn asahi_g16_commit_prepare(
        dev: *mut core::ffi::c_void,
        rtk: *mut bindings::apple_rtkit,
    ) -> *mut core::ffi::c_void;
    fn asahi_g17_commit_finish(commit: *mut core::ffi::c_void, saved: *mut u32) -> bool;
    fn asahi_g17_power_auto_off(dev: *mut core::ffi::c_void, saved: *mut u32) -> core::ffi::c_int;
    fn asahi_g17_power_raise(dev: *mut core::ffi::c_void, saved: u32) -> core::ffi::c_int;
    fn asahi_g17_power_auto_on(dev: *mut core::ffi::c_void, saved: u32) -> core::ffi::c_int;
    fn asahi_sleep_pd_domain_set(dev: *mut core::ffi::c_void, on: bool) -> core::ffi::c_int;
}

const SLEEP_DRAIN_MS: i64 = 3000;
const SLEEP_POLL_MS: i64 = 1;

pub(crate) type Shared = Arc<Mutex<Option<crate::m3::m3_runtime::Runtime>>>;
struct ProgressView(Arc<crate::m3::m3_rtkit::Health>);
impl kernel::debugfs::Writer for ProgressView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (generation, completed, last_ns, healthy) = self.0.progress_snapshot();
        writeln!(f, "version=1 generation_ns={} completed={} last_completion_ns={} healthy={}",
            generation, completed, last_ns, u8::from(healthy))
    }
}
// Separate endpoint preserves the strict completion-v1 ABI for old guards.
struct ActivityView(Arc<crate::m3::m3_rtkit::Health>);
impl kernel::debugfs::Writer for ActivityView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (generation, epoch, healthy) = self.0.activity_snapshot();
        writeln!(f, "version=1 generation_ns={} epoch={} pending={} healthy={}",
            generation, epoch, epoch & 1, u8::from(healthy))
    }
}

struct TimingView(Arc<crate::m3::m3_rtkit::Health>);
impl kernel::debugfs::Writer for TimingView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (generation, _, _, healthy) = self.0.progress_snapshot();
        self.0.timing.write(f, generation, healthy)
    }
}

// Root-only writable debugfs policy; no mutable module-parameter references.
// Keep ownership independent of the runtime lock and remove the file on stop.
struct CompletionView(Arc<crate::m3::m3_rtkit::EventWait>);
impl kernel::debugfs::Writer for CompletionView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (us, irqs, sleeps, timers, preparations) = self.0.snapshot();
        writeln!(f, "version=2 policy=adaptive wait_us={} notifications={} sleeps={} timer_wakes={} cpu_preparations={}",
                 us, irqs, sleeps, timers, preparations)
    }
}
impl kernel::debugfs::Reader for CompletionView {
    fn read_from_slice(&self, reader: &mut kernel::uaccess::UserSliceReader) -> Result {
        let len = reader.len();
        if len == 0 || len > 8 { return Err(EINVAL); }
        let mut bytes = [0u8; 8];
        reader.read_slice(&mut bytes[..len])?;
        let text = core::str::from_utf8(&bytes[..len]).map_err(|_| EINVAL)?;
        let us = text.trim().parse::<u64>().map_err(|_| EINVAL)?;
        self.0.set_wait_us(us)
    }
}

struct SubmitOverlapView(Arc<crate::m3::m3_rtkit::EventWait>);
impl kernel::debugfs::Writer for SubmitOverlapView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        writeln!(f, "{}", u8::from(self.0.cpu_overlap()))
    }
}
impl kernel::debugfs::Reader for SubmitOverlapView {
    fn read_from_slice(&self, reader: &mut kernel::uaccess::UserSliceReader) -> Result {
        let len=reader.len();
        if len==0 || len>2 {return Err(EINVAL);}
        let mut bytes=[0u8;2];reader.read_slice(&mut bytes[..len])?;
        let text=core::str::from_utf8(&bytes[..len]).map_err(|_|EINVAL)?.trim();
        match text {
            "0" => self.0.set_cpu_overlap(false),
            "1" => self.0.set_cpu_overlap(true),
            _ => return Err(EINVAL),
        }
        Ok(())
    }
}

/// The started T8122 runtime, as the shared DRM layer sees it ([`gpu::Gpu`]). It is created at
/// probe with the runtime it wraps and handed to the DRM registration, which owns it: dropping it
/// (unbind, or a failed registration) stops the runtime. The debugfs files share its lifetime.
pub(crate) struct Backend {
    dev: ARef<platform::Device>,
    shared: Shared,
    health: Arc<crate::m3::m3_rtkit::Health>,
    ids: gpu::SequenceIDs,
    scheduler: Arc<drm::sched::Scheduler<crate::m3::m3_submit::Job>>,
    core_mask: u64,
    max_frequency_khz: u32,
    soc: &'static crate::m3::m3_soc::Soc,
    profile: super::m3_profile::Profile,
    sleeping: AtomicBool,
    sleep_stopped: AtomicBool,
    holds_pd_in_firmware: AtomicBool,
    sleep_commit: AtomicPtr<core::ffi::c_void>,
    sleep_ps: AtomicU32,
    _progress: Pin<KBox<kernel::debugfs::File<ProgressView>>>,
    _activity: Pin<KBox<kernel::debugfs::File<ActivityView>>>,
    _memory: Pin<KBox<kernel::debugfs::File<crate::m3::agx_memory_stats::View>>>,
    _timing: Pin<KBox<kernel::debugfs::File<TimingView>>>,
    _completion: Pin<KBox<kernel::debugfs::File<CompletionView>>>,
    _overlap: Pin<KBox<kernel::debugfs::File<SubmitOverlapView>>>,
}

impl Backend {
    /// HIGH promises host publication ordering, not firmware preemption.
    pub(crate) fn supports_high_priority(&self) -> bool {
        self.soc.chip_id == 0x8122 && self.profile == super::m3_profile::Profile::Extended
    }

    /// Admits and starts the M3 runtime on `soc` (the T8122), with the DRM device `drm` that the
    /// caller registers afterwards.
    pub(crate) fn start(
        pdev: &platform::Device<Core<'_>>,
        drm: &driver::AsahiDrmDevice,
        soc: &'static crate::m3::m3_soc::Soc,
    ) -> Result<Arc<Self>> {
        // The runtime only exists to back its render node: without one (`asahi.m3_expose=0`)
        // nothing is started, rather than leaving a running firmware behind a failed probe.
        if !crate::m3::m3_board::expose_render_node(pdev.as_ref()) {
            dev_info!(pdev.as_ref(), "M3: render node not registered: the boot loader did not hand the GPU over (asahi.m3_expose=1 registers it); GPU not started\n");
            return Err(ENODEV);
        }
        let resources = crate::m3::m3_resources::from_device(pdev, soc).inspect_err(|_| {
            crate::m3::t8122_start::not_admitted(pdev.as_ref(), soc);
        })?;
        dev_info!(pdev.as_ref(), "M3: resource admission complete\n");
        // Extended T8122 uses its dedicated initialization graph. Keep the
        // legacy guarded-start and completeness checks for older interfaces.
        let extended=crate::m3::t8122_start::is_t8122(soc)
            && crate::m3::t8122_admission::loaded_profile(pdev)?==super::m3_profile::Profile::Extended;
        let experiment = if extended {None} else {crate::m3::t8122_start::arm(pdev.as_ref(), soc)?};
        if experiment.is_none() && !extended {
            soc.require_complete(pdev.as_ref())?;
        }
        let experiment = experiment.as_ref();
        let refused = |stage: &'static str| {
            move |e: &Error| crate::m3::t8122_start::refused(pdev.as_ref(), experiment, stage, *e)
        };
        let firmware = crate::m3::m3_firmware::identify_loaded(pdev, soc, resources, experiment)
            .inspect_err(refused("the firmware identity"))?;
        dev_info!(pdev.as_ref(), "M3: firmware identity accepted\n");
        let profile = firmware.profile();
        // InitData source and contents, before any GPU register access.
        let contents = crate::m3::m3_adt_config::Contents::select(pdev, &firmware, soc, experiment)
            .inspect_err(refused("the InitData contents"))?;
        let device = crate::m3::m3_device::Device::new(pdev, firmware, soc, experiment)
            .inspect_err(refused("the GPU registers and identity"))?;
        let core_mask = device.core_mask();
        let max_frequency_khz = 1000 * contents.pstates.reported_max_mhz();
        let mut runtime = crate::m3::m3_runtime::Runtime::new(pdev, drm, device, contents)?;
        runtime.boot(pdev)?;
        dev_info!(pdev.as_ref(), "M3: runtime switches: asahi.m3_unlocked_wait={} asahi.m3_timeout_nohang={} resume-after-fault={}\n",
            u8::from(crate::m3::m3_params::unlocked_wait()), u8::from(crate::m3::m3_params::timeout_nohang()),
            u8::from(crate::m3::m3_params::g15_debug(crate::m3::m3_params::G15Debug::M3ResumeAfterFault)));
        let health = runtime.health();
        let state = runtime.rtkit_state();
        // Health holds no device references. The read-only files own an Arc and are removed
        // with the backend; teardown latches failure first.
        let directory = kernel::debugfs::Dir::new(c"asahi-m3");
        let progress = KBox::pin_init(directory.read_only_file(c"progress",
            ProgressView(health.clone())), GFP_KERNEL)?;
        let activity = KBox::pin_init(directory.read_only_file(c"activity",
            ActivityView(health.clone())), GFP_KERNEL)?;
        let memory = KBox::pin_init(directory.read_only_file(c"memory",
            crate::m3::agx_memory_stats::View), GFP_KERNEL)?;
        let timing = KBox::pin_init(directory.read_only_file(c"timing",
            TimingView(health.clone())), GFP_KERNEL)?;
        let completion = KBox::pin_init(directory.read_write_file(c"completion_wait",
            CompletionView(runtime.completion_wait())), GFP_KERNEL)?;
        let overlap = KBox::pin_init(directory.read_write_file(c"submit_overlap",
            SubmitOverlapView(runtime.completion_wait())), GFP_KERNEL)?;
        let scheduler = Arc::new(drm::sched::Scheduler::new(drm.as_ref().as_ref(), 4, 8, 0, 3000,
            kernel::c_str!("asahi_m3_sched"))?, GFP_KERNEL)?;
        let shared: Shared = Arc::pin_init(new_mutex!(Some(runtime)), GFP_KERNEL)?;
        let completion_work = crate::m3::m3_completion::Completion::new(shared.clone())?;
        // From here on every failure drops the backend, which stops the runtime.
        let backend = Arc::new(Self { dev: pdev.into(), shared, health, ids: gpu::SequenceIDs::default(),
            scheduler, core_mask, max_frequency_khz, soc, profile,
            sleeping: AtomicBool::new(false),
            sleep_stopped: AtomicBool::new(false),
            holds_pd_in_firmware: AtomicBool::new(false),
            sleep_commit: AtomicPtr::new(core::ptr::null_mut()),
            sleep_ps: AtomicU32::new(0),
            _progress: progress, _activity: activity, _memory: memory, _timing: timing,
            _completion: completion, _overlap: overlap }, GFP_KERNEL)?;
        // The completion worker services this runtime from now on; the RTKit callbacks and
        // the submission path queue it through the runtime's state.
        if !state.completion.populate(completion_work) {
            return Err(EBUSY);
        }
        Ok(backend)
    }

    pub(crate) fn holds_pd_in_firmware_sleep(&self) -> bool {
        self.holds_pd_in_firmware.load(Ordering::Acquire)
    }

    /// Closes new GPU publication, waits for active work to retire, and for a firmware-driven
    /// sleep registers the commit-time coprocessor stop.
    pub(crate) fn sleep_prepare(&self, deep: bool) -> Result {
        if deep {
            if self.soc.chip_id != 0x8122 || self.profile != super::m3_profile::Profile::Extended {
                return Err(EBUSY);
            }
            // SAFETY: Reads global sleep-controller state.
            if !unsafe { asahi_g17_commit_required() } {
                return Err(EBUSY);
            }
        }
        let completion = {
            let mut guard = self.shared.lock();
            let runtime = Option::as_mut(&mut *guard).ok_or(ENODEV)?;
            runtime.sleep_hold()?;
            runtime.completion()
        };
        let deadline = Instant::<Monotonic>::now();
        loop {
            let quiet = {
                let mut guard = self.shared.lock();
                let Some(runtime) = Option::as_mut(&mut *guard) else {
                    return Err(ENODEV);
                };
                match runtime.sleep_is_quiet() {
                    Ok(quiet) => Ok(quiet),
                    Err(error) => {
                        runtime.sleep_unhold();
                        Err(error)
                    }
                }
            }?;
            if quiet {
                break;
            }
            if deadline.elapsed() >= Delta::from_millis(SLEEP_DRAIN_MS) {
                let mut guard = self.shared.lock();
                if let Some(runtime) = Option::as_mut(&mut *guard) {
                    runtime.sleep_unhold();
                }
                dev_warn!(self.dev.as_ref(), "M3 GPU: timed out draining active work for sleep\n");
                return Err(EBUSY);
            }
            fsleep(Delta::from_millis(SLEEP_POLL_MS));
        }
        if !deep {
            self.sleeping.store(true, Ordering::Release);
            return Ok(());
        }
        {
            let mut guard = self.shared.lock();
            let Some(runtime) = Option::as_mut(&mut *guard) else {
                return Err(ENODEV);
            };
            runtime.set_completion_paused(true);
        }
        if let Some(work) = completion.as_ref() {
            work.flush();
        }
        let mut guard = self.shared.lock();
        let Some(runtime) = Option::as_mut(&mut *guard) else {
            return Err(ENODEV);
        };
        match runtime.sleep_is_quiet() {
            Ok(true) => {}
            Ok(false) => {
                runtime.sleep_unhold();
                return Err(EBUSY);
            }
            Err(error) => {
                runtime.sleep_unhold();
                return Err(error);
            }
        }
        // On a dry run, keep firmware alive and only register the commit-time
        // descriptor so the gate check verifies readiness without stopping GPU.
        // On a real deep sleep, hold the gfx genpd across suspend_noirq/resume_noirq
        // so genpd does not touch PS_GFX out of order; sleep_suspend() lowers PS_GFX
        // cleanly after releasing PMP GPU device 5, and sleep_complete() raises it
        // back after PMP has resumed.
        // SAFETY: Reads global sleep-controller state.
        if !unsafe { apple_sleep_dry_run() } {
            self.holds_pd_in_firmware.store(true, Ordering::Release);
            self.sleeping.store(true, Ordering::Release);
            return Ok(());
        }
        // SAFETY: `self.dev` is the bound GPU platform device and `rtkit` lives for the runtime.
        let commit = unsafe {
            asahi_g16_commit_prepare(self.dev.as_ref().as_raw().cast(), runtime.sleep_rtkit_raw())
        };
        match kernel::error::from_err_ptr(commit) {
            Ok(ptr) => {
                self.sleep_commit.store(ptr, Ordering::Release);
                self.holds_pd_in_firmware.store(true, Ordering::Release);
                self.sleeping.store(true, Ordering::Release);
                Ok(())
            }
            Err(error) => {
                dev_warn!(
                    self.dev.as_ref(),
                    "M3 GPU: commit stop prepare failed ({:?}); refusing sleep\n",
                    error
                );
                runtime.sleep_unhold();
                Err(error)
            }
        }
    }

    /// Stops GPU firmware, releases the PMP GPU device 5 vote while PMP is still running,
    /// and lowers PS_GFX cleanly before the noirq window.
    pub(crate) fn sleep_suspend(&self) -> Result {
        if !self.sleeping.load(Ordering::Acquire) {
            return Ok(());
        }
        // SAFETY: Reads a global flag word.
        if !unsafe { bindings::pm_suspend_via_firmware() } {
            return Ok(());
        }
        if !self.sleep_commit.load(Ordering::Acquire).is_null() {
            return Ok(());
        }
        let mut guard = self.shared.lock();
        let Some(runtime) = Option::as_mut(&mut *guard) else {
            return Err(ENODEV);
        };
        runtime.sleep_suspend()?;
        let dev = self.dev.as_ref().as_raw().cast();
        let mut saved = 0u32;
        // SAFETY: `dev` is the bound GPU platform device and `saved` is writable.
        let auto_ret = unsafe { asahi_g17_power_auto_off(dev, &mut saved) };
        self.sleep_ps.store(saved, Ordering::Relaxed);
        // Mark ps_gfx driver-down in apple-sleep and lower it so pmgr-pwrstate does not
        // re-raise it in resume_noirq before PMP has resumed.
        // SAFETY: `dev` is the bound GPU platform device.
        let lower_ret = unsafe { asahi_sleep_pd_domain_set(dev, false) };
        dev_info!(
            self.dev.as_ref(),
            "M3 GPU: PS_GFX lowered via apple-sleep (saved={:#x}, auto_off={}, lower={})\n",
            saved,
            auto_ret,
            lower_ret
        );
        self.sleep_stopped.store(true, Ordering::Release);
        Ok(())
    }

    /// Reopens publication after a suspend-to-idle or refused deep sleep, or raises `ps_gfx`
    /// and warm-restarts the GPU firmware after a firmware-driven wake.
    pub(crate) fn sleep_complete(&self) {
        self.holds_pd_in_firmware.store(false, Ordering::Release);
        if !self.sleeping.swap(false, Ordering::AcqRel) {
            return;
        }
        if self.sleep_stopped.swap(false, Ordering::AcqRel) {
            let dev = self.dev.as_ref().as_raw().cast();
            let saved = self.sleep_ps.load(Ordering::Relaxed);
            // SAFETY: Reads a global flag word.
            let real = unsafe { bindings::pm_resume_via_firmware() };
            let started = Instant::<Monotonic>::now();
            // SAFETY: `dev` is the bound GPU platform device.
            let raise_res = kernel::error::to_result(unsafe { asahi_g17_power_raise(dev, saved) })
                .and_then(|()| {
                    // SAFETY: `dev` is the bound GPU platform device.
                    kernel::error::to_result(unsafe { asahi_g17_power_auto_on(dev, saved) })
                });
            if let Err(error) = raise_res {
                dev_err!(
                    self.dev.as_ref(),
                    "M3 GPU: PS_GFX raise/auto_on failed (saved={:#x}): {:?}\n",
                    saved,
                    error
                );
            }
            // SAFETY: PMP resumed before this consumer via the device link established at probe.
            let power_res = raise_res.and_then(|()| {
                kernel::error::to_result(unsafe { bindings::apple_pmp_set_device_power(0x0f, 5, 1) })
                    .inspect_err(|e| {
                        dev_err!(
                            self.dev.as_ref(),
                            "M3 GPU: PMP device 5 acquire failed: {:?}\n",
                            e
                        );
                    })
            });
            let mut guard = self.shared.lock();
            let Some(runtime) = Option::as_mut(&mut *guard) else {
                return;
            };
            match power_res.and_then(|()| runtime.sleep_warm_restart()) {
                Ok(()) => {
                    // SAFETY: `dev` is the bound GPU platform device.
                    let up_ret = unsafe { asahi_sleep_pd_domain_set(dev, true) };
                    if up_ret != 0 {
                        dev_warn!(
                            self.dev.as_ref(),
                            "M3 GPU: PS_GFX domain_set(on) failed: {}\n",
                            up_ret
                        );
                    }
                    dev_info!(
                        self.dev.as_ref(),
                        "M3 GPU: firmware back in {} us (real={})\n",
                        started.elapsed().as_micros_ceil(),
                        real
                    );
                    runtime.sleep_unhold();
                }
                Err(error) => {
                    // SAFETY: `dev` is the bound GPU platform device.
                    let _ = unsafe { asahi_sleep_pd_domain_set(dev, true) };
                    dev_err!(
                        self.dev.as_ref(),
                        "M3 GPU: firmware did not come back after sleep (real={}): {:?}\n",
                        real,
                        error
                    );
                    runtime.sleep_fail(error);
                }
            }
            return;
        }
        let commit = self.sleep_commit.swap(core::ptr::null_mut(), Ordering::AcqRel);
        if commit.is_null() {
            let mut guard = self.shared.lock();
            if let Some(runtime) = Option::as_mut(&mut *guard) {
                runtime.sleep_unhold();
            }
            return;
        }
        let mut saved = self.sleep_ps.load(Ordering::Relaxed);
        // SAFETY: `commit` came from `asahi_g16_commit_prepare` and is finished once.
        let stopped = unsafe { asahi_g17_commit_finish(commit, &mut saved) };
        self.sleep_ps.store(saved, Ordering::Relaxed);
        // SAFETY: Reads a global flag word.
        let real = unsafe { bindings::pm_resume_via_firmware() };
        if !stopped && !real {
            dev_info!(
                self.dev.as_ref(),
                "M3 GPU: sleep refused before commit; firmware never stopped\n"
            );
            let mut guard = self.shared.lock();
            if let Some(runtime) = Option::as_mut(&mut *guard) {
                runtime.sleep_unhold();
            }
            return;
        }
        if !real {
            dev_warn!(
                self.dev.as_ref(),
                "M3 GPU: sleep refused after commit stop; firmware stays stopped\n"
            );
            let mut guard = self.shared.lock();
            if let Some(runtime) = Option::as_mut(&mut *guard) {
                runtime.sleep_fail(EIO);
            }
            return;
        }
        let started = Instant::<Monotonic>::now();
        // SAFETY: PMP resumed before this consumer via the device link established at probe.
        let power_res = kernel::error::to_result(unsafe {
            bindings::apple_pmp_set_device_power(0x0f, 5, 1)
        });
        let mut guard = self.shared.lock();
        let Some(runtime) = Option::as_mut(&mut *guard) else {
            return;
        };
        match power_res.and_then(|()| runtime.sleep_warm_restart()) {
            Ok(()) => {
                dev_info!(
                    self.dev.as_ref(),
                    "M3 GPU: firmware back in {} us\n",
                    started.elapsed().as_micros_ceil()
                );
                runtime.sleep_unhold();
            }
            Err(error) => {
                dev_err!(
                    self.dev.as_ref(),
                    "M3 GPU: firmware did not come back after sleep: {:?}\n",
                    error
                );
                runtime.sleep_fail(error);
            }
        }
    }

    /// Stops the runtime: called on unbind, while the device's resources are still live, and
    /// again (doing nothing) when the backend is dropped.
    pub(crate) fn stop(&self) {
        self.holds_pd_in_firmware.store(false, Ordering::Release);
        let commit = self.sleep_commit.swap(core::ptr::null_mut(), Ordering::AcqRel);
        if !commit.is_null() {
            let mut saved = 0;
            // SAFETY: `commit` came from `asahi_g16_commit_prepare` and is finished once.
            let _ = unsafe { asahi_g17_commit_finish(commit, &mut saved) };
        }
        self.health.mark_failed();
        drop(self.shared.lock().take());
    }
}

impl Drop for Backend {
    fn drop(&mut self) {
        self.stop();
    }
}

impl gpu::Gpu for Backend {
    fn stats_snapshot(&self) -> Option<Arc<crate::stats::StatsSnapshot>> { None }
    fn as_any(&self) -> &dyn Any { self }
    fn arc_as_any(self: Arc<Self>) -> Arc<dyn Any + Sync + Send> { self as Arc<dyn Any + Sync + Send> }
    fn manager(&self) -> Option<&dyn gpu::GpuManager> { None }
    fn init(&self) -> Result { if self.is_crashed() { Err(ENODEV) } else { Ok(()) } }
    fn ids(&self) -> &gpu::SequenceIDs { &self.ids }
    fn is_crashed(&self) -> bool { !self.health.healthy() }
    fn get_params(&self, params: &mut uapi::drm_asahi_params_global) -> Result {
        let soc = self.soc;
        let masks = crate::m3::m3_board::core_masks(soc, self.core_mask);
        params.gpu_generation = hw::GpuGen::G15 as u32;
        params.gpu_variant = soc.gpu_variant as u32;
        params.gpu_revision = soc.gpu_revision as u32;
        params.chip_id = soc.chip_id;
        params.num_dies = 1;
        params.num_clusters_total = soc.clusters;
        params.num_cores_per_cluster = soc.cores_per_cluster;
        for (out, mask) in params.core_masks.iter_mut().zip(masks.iter()) {
            *out = (*mask).into();
        }
        params.max_frequency_khz = self.max_frequency_khz;
        params.usc_generation = 3;
        params.gpu_hal_generation = self.profile.hal();
        params.max_commands_per_submission = super::agx_uapi::UAPI_MAX_HARDWARE_COMMANDS;
        params.max_attachments = super::agx_uapi::UAPI_MAX_ATTACHMENTS as u32;
        Ok(())
    }
    fn supports_vm_status(&self) -> bool { true }
    /// The features the 7.1.12 file layer reported for the M3 runtime: VM status (not its
    /// mirror), scheduled queues, and the SoC's optional render and compute features.
    fn feature_flags(&self) -> u64 {
        let mut flags = uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_VM_STATUS as u64
            | uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_SCHEDULED_QUEUES as u64;
        if self.profile == super::m3_profile::Profile::Legacy
            && self.soc.features.fragment_dependency && crate::m3::m3_params::early_tiling() {
            flags |= uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_FRAGMENT_DEPENDENCY as u64;
        }
        if self.profile == super::m3_profile::Profile::Legacy
            && self.soc.features.compute_wide_visibility {
            flags |= uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_COMPUTE_WIDE_VISIBILITY as u64;
        }
        flags
    }
    fn base_clock_hz(&self) -> u32 {
        let value: u64;
        // SAFETY: read-only architectural counter frequency.
        unsafe { core::arch::asm!("mrs {x}, CNTFRQ_EL0", x=out(reg) value) };
        value as u32
    }
    fn uat_geometry(&self) -> mmu::UatGeometry { mmu::UatGeometry { ias: 42, oas: 42 } }
    fn user_range(&self) -> Range<u64> {
        mmu::UAT_PGSZ as u64..self.profile.user_end()
    }
    /// The dummy page sits where InitData tells the firmware it is (unk_30), as on 7.1.12; the
    /// 42-bit geometry's own unk_page() is a different address.
    fn unknown_page(&self) -> u64 {
        self.profile.user_end()
    }
    fn tracked_submission(&self) -> bool { true }
    /// Only M3-aware userspace may create a VM (7.1.12's per-file client gate).
    fn admit_vm(&self, dev: &kernel::device::Device, params_size: u64, flags: u32) -> Result {
        crate::m3::m3_client::admit_vm_for(dev, self.profile, params_size, flags)
    }
    fn new_vm(&self, range: Range<u64>) -> Result<mmu::Vm> {
        Option::as_mut(&mut *self.shared.lock()).ok_or(ENODEV)?.new_vm(self.ids.vm.next(), range)
    }
    fn new_queue(&self, vm: mmu::Vm, _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        _ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>, priority: u32,
        usc_exec_base: u64, _usage: Arc<crate::usage::Client>) -> Result<KBox<dyn queue::Queue>> {
        Ok(KBox::new(crate::m3::m3_submit::Queue::new(self.shared.clone(),self.scheduler.clone(),vm,priority,usc_exec_base,
            self.feature_flags() & uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_FRAGMENT_DEPENDENCY as u64 != 0,
            self.feature_flags() & uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_COMPUTE_WIDE_VISIBILITY as u64 != 0)?,GFP_KERNEL)?)
    }
    fn timestamp_window_size(&self) -> u64 {
        crate::m3::agx_memory::TIMESTAMP_RANGE.end - crate::m3::agx_memory::TIMESTAMP_RANGE.start
    }

    fn map_timestamp_buffer(&self, bo: gem::ObjectRef, range: Range<usize>, window: crate::timestamp::WindowPermit) -> Result<crate::timestamp::Buffer> {
        if !window.is_allowed() { return Err(ENOSPC); }
        crate::timestamp::Buffer::firmware(
            Option::as_mut(&mut *self.shared.lock()).ok_or(ENODEV)?.map_timestamp(bo,range)?,
            window.into_share())
    }
}
