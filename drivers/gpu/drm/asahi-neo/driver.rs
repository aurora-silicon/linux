// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Top-level GPU driver implementation.

use crate::module_parameters;
use kernel::bindings;
use kernel::{
    c_str,
    device::Core,
    dma::{
        Device,
        DmaMask, //
    },
    drm_neo,
    drm_neo::ioctl,
    of,
    platform,
    prelude::*,
    sync::{
        aref::ARef,
        Arc, //
    }, //
};

use crate::{
    debug,
    file,
    g17,
    gem::AsahiObject,
    gpu,
    hw,
    regs, //
};

use kernel::macros::vtable;

/// Holds a reference to the top-level GPU object.
#[pin_data]
pub(crate) struct AsahiData {
    #[pin]
    pub(crate) gpu: Arc<dyn gpu::Gpu>,
    pub(crate) pdev: ARef<platform::Device>,
    pub(crate) resources: regs::Resources,
}

unsafe impl Send for AsahiData {}
unsafe impl Sync for AsahiData {}

pub(crate) struct AsahiDriver {
    drm_neo: ARef<drm_neo::Device<Self>>,
    /// Raw `struct device *` of the platform device. Stored once at probe
    /// time so the `Drop` impl can find the sysfs file on unregister.
    raw_dev: *mut bindings::device,
    raw_stats: *mut core::ffi::c_void,
    // Keep the allocation alive until sysfs readers have drained.
    stats: Option<Arc<crate::stats::StatsSnapshot>>,
}

unsafe impl Send for AsahiDriver {}
unsafe impl Sync for AsahiDriver {}

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type AsahiDevice = drm_neo::device::Device<AsahiDriver>;
pub(crate) type AsahiDevRef = ARef<AsahiDevice>;

/// DRM Driver metadata
const INFO: drm_neo::driver::DriverInfo = drm_neo::driver::DriverInfo {
    major: 0,
    minor: 0,
    patchlevel: 0,
    name: c_str!("asahi"),
    desc: c_str!("Apple AGX Graphics"),
};

/// DRM Driver implementation for `AsahiDriver`.
#[vtable]
impl drm_neo::driver::Driver for AsahiDriver {
    /// Our `DeviceData` type, reference-counted
    type Data = AsahiData;
    /// Our `File` type.
    type File = file::File;
    /// Our `Object` type.
    type Object = drm_neo::gem::shmem::Object<AsahiObject>;

    const INFO: drm_neo::driver::DriverInfo = INFO;
    const MODULE: Option<&'static kernel::ThisModule> = Some(&crate::THIS_MODULE);
    const FEATURES: u32 = drm_neo::driver::FEAT_GEM
        | drm_neo::driver::FEAT_RENDER
        | drm_neo::driver::FEAT_SYNCOBJ
        | drm_neo::driver::FEAT_SYNCOBJ_TIMELINE
        | drm_neo::driver::FEAT_GEM_GPUVA;

    kernel::declare_drm_neo_ioctls! {
        (ASAHI_NEO_GET_PARAMS,      drm_asahi_neo_get_params,
                          ioctl::RENDER_ALLOW, crate::file::File::get_params),
        (ASAHI_NEO_GET_TIME,        drm_asahi_neo_get_time,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::get_time),
        (ASAHI_NEO_VM_CREATE,       drm_asahi_neo_vm_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_create),
        (ASAHI_NEO_VM_DESTROY,      drm_asahi_neo_vm_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_destroy),
        (ASAHI_NEO_VM_BIND,         drm_asahi_neo_vm_bind,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_bind),
        (ASAHI_NEO_GEM_CREATE,      drm_asahi_neo_gem_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_create),
        (ASAHI_NEO_GEM_MMAP_OFFSET, drm_asahi_neo_gem_mmap_offset,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_mmap_offset),
        (ASAHI_NEO_GEM_BIND_OBJECT, drm_asahi_neo_gem_bind_object,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_bind_object),
        (ASAHI_NEO_QUEUE_CREATE,    drm_asahi_neo_queue_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::queue_create),
        (ASAHI_NEO_QUEUE_DESTROY,   drm_asahi_neo_queue_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::queue_destroy),
        (ASAHI_NEO_SUBMIT,          drm_asahi_neo_submit,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::submit),
        (ASAHI_NEO_GEM_MADVISE,     drm_asahi_neo_gem_madvise,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_madvise),
    }
}

/// Firmware interface selected by the hardware compatible.
pub(crate) enum ProbeConfig {
    Legacy(&'static hw::HwConfig),
    G17(&'static g17::Config),
}

// OF Device ID table.
kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <AsahiDriver as platform::Driver>::IdInfo,
    [
        (
            of::DeviceId::new(c_str!("apple,agx-t8140")),
            ProbeConfig::G17(&hw::t8140::CONFIG)
        ),
    ]
);

/// Platform Driver implementation for `AsahiDriver`.
impl platform::Driver for AsahiDriver {
    type IdInfo = ProbeConfig;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn unbind(_pdev: &platform::Device<Core>, this: Pin<&Self>) {
        if let Some(gpu) = (*this.drm_neo).gpu.as_any().downcast_ref::<g17::Gpu>() {
            // Driver data is dropped after devres, so CPU control must happen in unbind.
            gpu.shutdown();
        }
    }

    /// Device probe function.
    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        debug::update_debug_flags();

        dev_info!(pdev.as_ref(), "Probing...\n");

        let cfg = match info.ok_or(ENODEV)? {
            ProbeConfig::Legacy(cfg) => *cfg,
            ProbeConfig::G17(cfg) => return Self::probe_g17(pdev, cfg),
        };

        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(cfg.uat_oas)?)? };

        let res = regs::Resources::new(pdev)?;

        // Initialize misc MMIO
        res.init_mmio()?;

        // Start the coprocessor CPU, so UAT can initialize the handoff
        regs::Resources::start_cpu(pdev)?;

        let fwnode = pdev.as_ref().fwnode().ok_or(EIO)?;
        let compat: KVec<u32> = fwnode
            .property_read_array_vec(c_str!("apple,firmware-compat"), 3)?
            .required_by(pdev.as_ref())?;

        // SAFETY: GPU construction uses the DRM allocation but not its driver data.
        // No userspace or driver-data work is published before init_data() succeeds.
        let drm_neo: ARef<AsahiDevice> = unsafe { drm_neo::device::Device::new_uninit(pdev.as_ref())? };

        let gpu = match (cfg.gpu_gen, cfg.gpu_variant, compat.as_slice()) {
            (hw::GpuGen::G13, _, &[12, 3, 0]) => {
                gpu::GpuManagerG13V12_3::new(&drm_neo.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[12, 4, 0]) => {
                gpu::GpuManagerG14V12_4::new(&drm_neo.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G13, _, &[13, 5, 0]) => {
                gpu::GpuManagerG13V13_5::new(&drm_neo.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[13, 5, 0]) => {
                gpu::GpuManagerG14V13_5::new(&drm_neo.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G14, _, &[13, 5, 0]) => {
                gpu::GpuManagerG14XV13_5::new(&drm_neo.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            _ => {
                dev_info!(
                    pdev.as_ref(),
                    "Unsupported GPU/firmware combination ({:?}, {:?}, {:?})\n",
                    cfg.gpu_gen,
                    cfg.gpu_variant,
                    compat
                );
                return Err(ENODEV);
            }
        };

        let data = try_pin_init!(AsahiData {
            gpu,
            pdev: pdev.into(),
            resources: res,
        });

        // SAFETY: This is the sole initializer; driver data has not been accessed yet.
        unsafe { drm_neo.init_data(data)? };

        (*drm_neo).gpu.init()?;

        drm_neo::driver::Registration::new_foreign_owned(&drm_neo, pdev.as_ref(), 0)?;

        // Register the sysfs file on the platform device. Must happen after
        // the DRM device is registered so the device is fully bound.
        let raw_dev = pdev.as_ref().as_raw();
        let stats = (*drm_neo).gpu.stats_snapshot();
        let snapshot = stats.as_ref().map_or(core::ptr::null(), Arc::as_ptr);
        let raw_stats = crate::sysfs_exports::register(
            raw_dev,
            snapshot,
            stats.is_some() && *module_parameters::stats_export.value() != 0,
        )?;

        Ok(Self { drm_neo, raw_dev, raw_stats, stats })
    }
}

impl AsahiDriver {
    fn probe_g17(pdev: &platform::Device<Core>, cfg: &'static g17::Config) -> Result<Self> {
        // SAFETY: The GPU performs DMA through a UAT whose output width is part of the SoC config.
        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(cfg.uat_oas)?)? };
        let res = regs::Resources::new(pdev)?;
        // SAFETY: GPU construction cannot access driver data before init_data().
        // Failed construction releases the allocation without dropping uninitialized data.
        let drm_neo: ARef<AsahiDevice> = unsafe { drm_neo::device::Device::new_uninit(pdev.as_ref())? };
        let gpu = g17::Gpu::new(pdev, &drm_neo, cfg, res.clone())?;
        let data_gpu = gpu.clone() as Arc<dyn gpu::Gpu>;
        let data = try_pin_init!(AsahiData {
            gpu: data_gpu,
            pdev: pdev.into(),
            resources: res,
        });
        // SAFETY: This is the sole initializer; no driver-data users have been published.
        if let Err(error) = unsafe { drm_neo.init_data(data) } {
            gpu.shutdown();
            return Err(error);
        }
        if let Err(error) = drm_neo::driver::Registration::new_foreign_owned(&drm_neo, pdev.as_ref(), 0) {
            gpu.shutdown();
            return Err(error);
        }
        // G17's firmware channel format has no supported stats decoder yet.
        let raw_dev = pdev.as_ref().as_raw();
        let raw_stats = match crate::sysfs_exports::register(
            raw_dev, core::ptr::null(), false,
        ) {
            Ok(handle) => handle,
            Err(error) => {
                gpu.shutdown();
                return Err(error);
            }
        };
        Ok(Self { drm_neo, raw_dev, raw_stats, stats: None })
    }
}

impl Drop for AsahiDriver {
    fn drop(&mut self) {
        crate::sysfs_exports::unregister(self.raw_dev, self.raw_stats);
        drop(self.stats.take());
        if let Some(gpu) = (*self.drm_neo).gpu.as_any().downcast_ref::<g17::Gpu>() {
            // Also covers failure to allocate platform driver data after the probe body returned.
            gpu.shutdown();
        }
    }
}
