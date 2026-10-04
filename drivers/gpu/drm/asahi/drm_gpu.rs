// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Generation-neutral DRM-facing GPU operations.
//!
//! The legacy manager remains unchanged behind [`LegacyDrmGpu`]. T8140 uses
//! the same file and ioctl layer through its own adapter without pretending to
//! be an AGX2 firmware manager.

use core::ops::Range;

use kernel::{
    prelude::*,
    sync::{Arc, Mutex},
    uapi,
};

use crate::{alloc, file, fw, gem, gpu, mmu, queue};

/// Hardware facts returned through `DRM_ASAHI_GET_PARAMS`.
pub(crate) struct DrmGpuParams {
    pub(crate) gpu_generation: u32,
    pub(crate) gpu_variant: u32,
    pub(crate) gpu_revision: u32,
    pub(crate) chip_id: u32,
    pub(crate) num_dies: u32,
    pub(crate) num_clusters_total: u32,
    pub(crate) num_cores_per_cluster: u32,
    pub(crate) core_masks: [u32; uapi::DRM_ASAHI_MAX_CLUSTERS as usize],
    pub(crate) max_frequency_khz: u32,
    pub(crate) usc_generation: u32,
    pub(crate) gpu_hal_generation: u32,
    pub(crate) max_commands_per_submission: u32,
}

/// Operations needed by the DRM file layer, independent of firmware ABI.
pub(crate) trait DrmGpu: Send + Sync {
    fn init(&self) -> Result;
    fn ids(&self) -> &gpu::SequenceIDs;
    fn is_crashed(&self) -> bool;
    fn supports_vm_status(&self) -> bool { false }
    fn independent_queue_limits(&self) -> Option<[u8; 16]> { None }
    fn supports_scheduled_queues(&self) -> bool { false }
    /// Permanent submission-domain failure; querying must not reset firmware.
    fn submission_error(&self) -> i32 { 0 }
    fn update_globals(&self);
    fn service_g16_jobs(&self) {}
    fn params(&self) -> Result<DrmGpuParams>;
    fn user_range(&self) -> Result<Range<u64>>;
    fn unknown_page(&self) -> Result<u64>;
    fn base_clock_hz(&self) -> u32;
    fn new_vm(&self, kernel_range: Range<u64>) -> Result<mmu::Vm>;
    fn new_queue(
        &self,
        vm: mmu::Vm,
        ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        priority: u32,
        usc_exec_base: u64,
    ) -> Result<KBox<dyn queue::Queue>>;
    fn map_timestamp_buffer(
        &self,
        bo: gem::ObjectRef,
        range: Range<usize>,
    ) -> Result<mmu::KernelMapping>;

    /// Legacy-only operations reached by objects that cannot exist on G17P.
    fn legacy_manager(&self) -> Option<&Arc<dyn gpu::GpuManager>> {
        None
    }
    fn free_context(&self, _data: KBox<fw::types::GpuObject<fw::workqueue::GpuContextData>>) {}
    fn fwctl(&self, _msg: fw::channels::FwCtlMsg) -> Result {
        Err(ENODEV)
    }
}

/// DRM adapter for the existing AGX2 manager implementations.
pub(crate) struct LegacyDrmGpu {
    manager: Arc<dyn gpu::GpuManager>,
}

impl LegacyDrmGpu {
    pub(crate) fn new(manager: Arc<dyn gpu::GpuManager>) -> Self {
        Self { manager }
    }
}

impl DrmGpu for LegacyDrmGpu {
    fn init(&self) -> Result {
        self.manager.init()
    }

    fn ids(&self) -> &gpu::SequenceIDs {
        self.manager.ids()
    }

    fn is_crashed(&self) -> bool {
        self.manager.is_crashed()
    }

    fn update_globals(&self) {
        self.manager.update_globals()
    }

    fn params(&self) -> Result<DrmGpuParams> {
        let cfg = self.manager.get_cfg();
        let dyncfg = self.manager.get_dyncfg();
        let mut core_masks = [0u32; uapi::DRM_ASAHI_MAX_CLUSTERS as usize];
        for (index, mask) in dyncfg.id.core_masks.iter().enumerate() {
            *core_masks.get_mut(index).ok_or(EIO)? = *mask;
        }
        Ok(DrmGpuParams {
            gpu_generation: dyncfg.id.gpu_gen as u32,
            gpu_variant: dyncfg.id.gpu_variant as u32,
            gpu_revision: dyncfg.id.gpu_rev as u32,
            chip_id: cfg.chip_id,
            num_dies: if cfg.gpu_gen == crate::hw::GpuGen::G15 { dyncfg.id.num_dies } else { cfg.num_dies },
            num_clusters_total: dyncfg.id.num_clusters,
            num_cores_per_cluster: dyncfg.id.num_cores,
            core_masks,
            // The performance cap on G15.
            max_frequency_khz: crate::initdata::g15_manager_max_frequency_khz(cfg, &dyncfg.pwr)
                .unwrap_or(dyncfg.pwr.max_frequency_khz()),
            usc_generation: dyncfg.id.usc_generation,
            gpu_hal_generation: dyncfg.id.gpu_hal_generation as u32,
            max_commands_per_submission: file::MAX_COMMANDS_PER_SUBMISSION,
        })
    }

    fn user_range(&self) -> Result<Range<u64>> {
        mmu::iova_user_usable_range(self.manager.get_cfg())
    }

    fn unknown_page(&self) -> Result<u64> {
        mmu::iova_unk_page(self.manager.get_cfg())
    }

    fn base_clock_hz(&self) -> u32 {
        self.manager.get_cfg().base_clock_hz
    }

    fn new_vm(&self, kernel_range: Range<u64>) -> Result<mmu::Vm> {
        self.manager.new_vm(kernel_range)
    }

    fn new_queue(
        &self,
        vm: mmu::Vm,
        ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        priority: u32,
        usc_exec_base: u64,
    ) -> Result<KBox<dyn queue::Queue>> {
        self.manager
            .new_queue(vm, ualloc, ualloc_priv, priority, usc_exec_base)
    }

    fn map_timestamp_buffer(
        &self,
        bo: gem::ObjectRef,
        range: Range<usize>,
    ) -> Result<mmu::KernelMapping> {
        self.manager.map_timestamp_buffer(bo, range)
    }

    fn legacy_manager(&self) -> Option<&Arc<dyn gpu::GpuManager>> {
        Some(&self.manager)
    }

    fn free_context(&self, data: KBox<fw::types::GpuObject<fw::workqueue::GpuContextData>>) {
        self.manager.free_context(data)
    }

    fn fwctl(&self, msg: fw::channels::FwCtlMsg) -> Result {
        self.manager.fwctl(msg)
    }
}
