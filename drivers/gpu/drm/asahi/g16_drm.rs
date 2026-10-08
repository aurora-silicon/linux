// SPDX-License-Identifier: GPL-2.0-only OR MIT


use core::ops::Range;
use kernel::{device::Core, drm, new_mutex, platform, prelude::*, sync::{Arc, Mutex}, uapi};
use crate::{alloc, driver, drm_gpu::{DrmGpu, DrmGpuParams}, gem, gpu, hw, mmu, queue};

pub(crate) type Shared = Arc<Mutex<Option<crate::g16_runtime::Runtime>>>;
pub(crate) const USER_TOP: u64 = (1u64 << 42) - 2 * mmu::UAT_PGSZ as u64;

struct ProgressView(Arc<crate::g16_rtkit::Health>);
impl kernel::debugfs::Writer for ProgressView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (generation, completed, last_ns, healthy) = self.0.progress_snapshot();
        writeln!(f, "version=1 generation_ns={} completed={} last_completion_ns={} healthy={}",
            generation, completed, last_ns, u8::from(healthy))
    }
}

struct ActivityView(Arc<crate::g16_rtkit::Health>);
impl kernel::debugfs::Writer for ActivityView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (generation, epoch, healthy) = self.0.activity_snapshot();
        writeln!(f, "version=1 generation_ns={} epoch={} pending={} healthy={}",
            generation, epoch, epoch & 1, u8::from(healthy))
    }
}

struct TimingView(Arc<crate::g16_rtkit::Health>);
impl kernel::debugfs::Writer for TimingView {
    fn write(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        let (generation, _, _, healthy) = self.0.progress_snapshot();
        self.0.timing.write(f, generation, healthy)
    }
}

pub(crate) struct Registered {
    registration: Pin<KBox<Mutex<Option<drm::driver::Registration<driver::AsahiDriver>>>>>,
    shared: Shared,
    health: Arc<crate::g16_rtkit::Health>,
    _progress: Pin<KBox<kernel::debugfs::File<ProgressView>>>,
    _activity: Pin<KBox<kernel::debugfs::File<ActivityView>>>,
    _memory: Pin<KBox<kernel::debugfs::File<crate::agx_memory_stats::View>>>,
    _timing: Pin<KBox<kernel::debugfs::File<TimingView>>>,
}

impl Registered {
    pub(crate) fn start(pdev: &platform::Device<Core>) -> Result<Self> {
        let resources = crate::g16_resources::from_device(pdev)?;
        let firmware = crate::g16_firmware::identify_loaded(pdev, resources)?;
        let device = crate::g16_device::Device::new(pdev, firmware)?;
        device.check_drm(pdev)?;
        device.check_mmu(pdev)?;
        device.check_tables(pdev)?;
        let mut runtime = crate::g16_runtime::Runtime::new(pdev, device)?;
        runtime.boot(pdev)?;
        let drm = runtime.drm();
        let mask = runtime.core_mask();
        let health = runtime.health();
        let directory = kernel::debugfs::Dir::new(c"asahi-m4");
        let progress = KBox::pin_init(directory.read_only_file(c"progress",
            ProgressView(health.clone())), GFP_KERNEL)?;
        let activity = KBox::pin_init(directory.read_only_file(c"activity",
            ActivityView(health.clone())), GFP_KERNEL)?;
        let memory = KBox::pin_init(directory.read_only_file(c"memory",
            crate::agx_memory_stats::View), GFP_KERNEL)?;
        let timing = KBox::pin_init(directory.read_only_file(c"timing",
            TimingView(health.clone())), GFP_KERNEL)?;
        let shared = Arc::pin_init(new_mutex!(Some(runtime)), GFP_KERNEL)?;
        let owner = Self { registration: KBox::pin_init(new_mutex!(None), GFP_KERNEL)?, shared: shared.clone(), health: health.clone(), _progress: progress, _activity: activity, _memory: memory, _timing: timing };
        let scheduler = Arc::new(drm::sched::Scheduler::new(drm.as_ref(), 4, 1, 0,
            3000, kernel::c_str!("asahi_m4_sched"))?, GFP_KERNEL)?;
        let backend: Arc<dyn DrmGpu> = Arc::new(Backend {
            shared, scheduler, ids: gpu::SequenceIDs::default(), core_mask: mask, health,
        }, GFP_KERNEL)?;
        if !drm.gpu.populate(crate::drm_gpu::Backend::G16(backend)) { return Err(EBUSY); }
        *owner.registration.lock() = Some(drm::driver::Registration::new(&drm, 0)?);
        dev_info!(pdev.as_ref(), "G16G: owned runtime retained; common DRM GEM/VM frontend registered\n");
        Ok(owner)
    }
}

impl Registered {
    pub(crate) fn stop(&self) {
        self.health.mark_failed();
        drop(self.registration.lock().take());
        let runtime = self.shared.lock().take();
        drop(runtime);
    }
}

impl Drop for Registered {
    fn drop(&mut self) { self.stop(); }
}

struct Backend {
    shared: Shared,
    health: Arc<crate::g16_rtkit::Health>,
    scheduler: Arc<drm::sched::Scheduler<crate::g16_submit::Job>>,
    ids: gpu::SequenceIDs,
    core_mask: u32,
}

impl DrmGpu for Backend {
    fn init(&self) -> Result { if self.is_crashed() { Err(ENODEV) } else { Ok(()) } }
    fn ids(&self) -> &gpu::SequenceIDs { &self.ids }
    fn is_crashed(&self) -> bool {
        !self.health.healthy()
    }
    fn update_globals(&self) {}
    fn extra_features(&self) -> u64 {
        (uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_RENDER_TIMESTAMP_COPIES
        | uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_COMPUTE_TIMESTAMP_COPIES
        | uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_FRAGMENT_DEPENDENCY
        | uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_FRAGMENT_DEPENDENCY_COMPUTE
        | uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_COMPUTE_WIDE_VISIBILITY) as u64
    }
    fn supports_vm_status(&self) -> bool { true }
    fn supports_scheduled_queues(&self) -> bool { true }
    fn submission_error(&self) -> i32 { if self.is_crashed() { EIO.to_errno() } else { 0 } }
    fn service_g16_jobs(&self) {
        use crate::g16_runtime::Service;
        const TRAILING_POLLS: u32 = 4;
        let mut trailing = 0;
        let mut waited_at = u64::MAX;
        let mut timed_out = false;
        loop {
            let (outcome, events) = {
                let mut guard = self.shared.lock();
                let Some(runtime) = Option::as_mut(&mut *guard) else { return; };
                if timed_out { runtime.note_wait_timeout(); timed_out = false; }
                (runtime.service_job(), runtime.events())
            };
            match outcome {
                Service::Idle => break,
                Service::Progress => { trailing = 0; waited_at = u64::MAX; }
                Service::Waiting(messages) => {
                    if messages != waited_at { waited_at = messages; trailing = 0; }
                    if trailing < TRAILING_POLLS {
                        trailing += 1;
                        kernel::time::delay::fsleep(kernel::time::Delta::from_micros(20));
                    } else {
                        timed_out = !events.wait_past(messages);
                        trailing = 0;
                    }
                }
            }
        }
    }
    fn params(&self) -> Result<DrmGpuParams> {
        let mut masks = [0; uapi::DRM_ASAHI_MAX_CLUSTERS as usize];
        masks[0] = self.core_mask;
        let board = crate::g16_board::get()?;
        Ok(DrmGpuParams {
            gpu_generation: board.gpu_gen as u32,
            gpu_variant: hw::GpuVariant::G as u32,
            gpu_revision: board.gpu_revision as u32,
            chip_id: board.chip_id, num_dies: 1, num_clusters_total: 1,
            num_cores_per_cluster: board.cores, core_masks: masks,
            max_frequency_khz: crate::g16_power::frequency_khz()?, usc_generation: 3,
            gpu_hal_generation: hw::GpuHalGeneration::Hal200 as u32,
            max_commands_per_submission: crate::file::MAX_COMMANDS_PER_SUBMISSION,
        })
    }
    fn user_range(&self) -> Result<Range<u64>> { Ok(mmu::UAT_PGSZ as u64..USER_TOP) }
    fn unknown_page(&self) -> Result<u64> { Ok(USER_TOP) }
    fn base_clock_hz(&self) -> u32 {
        let frequency: u64;
        // SAFETY: Reads the architected counter frequency register only.
        unsafe { core::arch::asm!("mrs {x}, CNTFRQ_EL0", x = out(reg) frequency) };
        u32::try_from(frequency & 0xffff_ffff).unwrap_or(24_000_000)
    }
    fn new_vm(&self, range: Range<u64>) -> Result<mmu::Vm> {
        Option::as_mut(&mut *self.shared.lock()).ok_or(ENODEV)?.new_user_vm(self.ids.vm.next(), range)
    }
    fn new_queue(&self, vm: mmu::Vm, _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        _ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>, priority: u32,
        usc_exec_base: u64) -> Result<KBox<dyn queue::Queue>> {
        let drm = self.shared.lock().as_ref().ok_or(ENODEV)?.drm();
        Ok(KBox::new(crate::g16_submit::Queue::new(self.shared.clone(),
            self.scheduler.clone(), drm, vm, priority, usc_exec_base)?, GFP_KERNEL)?)
    }
    fn map_timestamp_buffer(&self, bo: gem::ObjectRef, range: Range<usize>) -> Result<mmu::KernelMapping> {
        self.shared.lock().as_ref().ok_or(ENODEV)?.map_timestamp(bo, range)
    }
}
