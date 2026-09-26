// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J713 frontend through the shared Asahi file/GEM/GPUVM implementation.
//! Platform teardown empties a shared runtime cell before releasing hardware;
//! existing DRM files retain a safe, disconnected backend rather than a raw
//! pointer into the platform driver's former allocation.

use core::ops::Range;
use kernel::{device::Core, drm, new_mutex, platform, prelude::*, sync::{Arc, Mutex}, uapi};
use crate::{alloc, driver, drm_gpu::{DrmGpu, DrmGpuParams}, gem, gpu, hw, mmu, queue};

pub(crate) type Shared = Arc<Mutex<Option<crate::g16_runtime::Runtime>>>;
pub(crate) const USER_TOP: u64 = (1u64 << 42) - 2 * mmu::UAT_PGSZ as u64;

pub(crate) struct Registered {
    registration: Pin<KBox<Mutex<Option<drm::driver::Registration<driver::AsahiDriver>>>>>,
    shared: Shared,
    health: Arc<crate::g16_rtkit::Health>,
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
        let shared = Arc::pin_init(new_mutex!(Some(runtime)), GFP_KERNEL)?;
        // Install the teardown owner before publishing the backend. On any
        // later construction failure Drop breaks the runtime -> DRM cycle.
        let owner = Self { registration: KBox::pin_init(new_mutex!(None), GFP_KERNEL)?, shared: shared.clone(), health: health.clone() };
        // Several packets may be handed to the runtime at once; it admits
        // them in order and keeps the rest pending, so the firmware queues
        // can hold more than one command where the runtime allows it.
        let scheduler = Arc::new(drm::sched::Scheduler::new(drm.as_ref(), 4, 8, 0,
            3000, kernel::c_str!("asahi_m4_sched"))?, GFP_KERNEL)?;
        let backend: Arc<dyn DrmGpu> = Arc::new(Backend {
            shared, scheduler, ids: gpu::SequenceIDs::default(), core_mask: mask, health,
        }, GFP_KERNEL)?;
        if !drm.gpu.populate(backend) { return Err(EBUSY); }
        *owner.registration.lock() = Some(drm::driver::Registration::new(&drm, 0)?);
        dev_info!(pdev.as_ref(), "G16G: owned runtime retained; common DRM GEM/VM frontend registered\n");
        Ok(owner)
    }
}

impl Registered {
    // Called by platform unbind while managed MMIO and mailbox resources are
    // still accessible. Final driver Drop occurs after devres revocation.
    pub(crate) fn stop(&self) {
        // Publish disconnect before waiting for the hardware mutex. Status
        // queries remain nonblocking, including during device removal.
        self.health.mark_failed();
        drop(self.registration.lock().take());
        // The same mutex serializes every hardware operation. After taking
        // the runtime no surviving file can acquire it. Drop stops ASC, or
        // retains all backing and pins module code if retirement is unproven.
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
        // Vulkan checks device status even for an already signaled fence.
        // Do not wait for unrelated command allocation or retirement here.
        !self.health.healthy()
    }
    fn update_globals(&self) {}
    fn supports_vm_status(&self) -> bool { true }
    // Distinct DRM entities select ready jobs before the shared one-credit
    // scheduler publishes them to the serialized firmware runtime.
    fn supports_scheduled_queues(&self) -> bool { true }
    fn submission_error(&self) -> i32 { if self.is_crashed() { EIO.to_errno() } else { 0 } }
    fn service_g16_jobs(&self) {
        use crate::g16_runtime::Service;
        // Snapshots taken after a notification before sleeping for the next
        // one: the user stamp can trail the firmware's event message.
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
                // Successors were published while retiring: snapshot again
                // without yielding, the firmware runs commands 80 us apart.
                Service::Progress => { trailing = 0; waited_at = u64::MAX; }
                Service::Waiting(messages) => {
                    if messages != waited_at { waited_at = messages; trailing = 0; }
                    if trailing < TRAILING_POLLS {
                        trailing += 1;
                        kernel::time::delay::fsleep(kernel::time::Delta::from_micros(20));
                    } else {
                        // Sleep until the firmware's next event notification.
                        // The bounded fallback keeps the timeout check alive
                        // and covers a stamp that trails its message further.
                        timed_out = !events.wait_past(messages, 1);
                        trailing = 0;
                    }
                }
            }
        }
    }
    fn params(&self) -> Result<DrmGpuParams> {
        let mut masks = [0; uapi::DRM_ASAHI_MAX_CLUSTERS as usize];
        masks[0] = self.core_mask;
        Ok(DrmGpuParams {
            gpu_generation: hw::GpuGen::G16 as u32,
            gpu_variant: hw::GpuVariant::G as u32,
            gpu_revision: hw::GpuRevision::B0 as u32,
            chip_id: 0x8132, num_dies: 1, num_clusters_total: 1,
            num_cores_per_cluster: 10, core_masks: masks,
            // Match the configured firmware performance-state ceiling.
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
