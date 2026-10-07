// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Top-level GPU driver implementation.

use kernel::{
    c_str,
    device::Core,
    dma::{
        Device,
        DmaMask, //
    },
    drm,
    drm::ioctl,
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
    drm: ARef<drm::Device<Self>>,
}

unsafe impl Send for AsahiDriver {}
unsafe impl Sync for AsahiDriver {}

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type AsahiDevice = drm::device::Device<AsahiDriver>;
pub(crate) type AsahiDevRef = ARef<AsahiDevice>;

/// DRM Driver metadata
const INFO: drm::driver::DriverInfo = drm::driver::DriverInfo {
    major: 0,
    minor: 0,
    patchlevel: 0,
    name: c_str!("asahi"),
    desc: c_str!("Apple AGX Graphics"),
};

/// DRM Driver implementation for `AsahiDriver`.
#[vtable]
impl drm::driver::Driver for AsahiDriver {
    /// Our `DeviceData` type, reference-counted
    type Data = AsahiData;
    /// Our `File` type.
    type File = file::File;
    /// Our `Object` type.
    type Object = drm::gem::shmem::Object<AsahiObject>;

    const INFO: drm::driver::DriverInfo = INFO;
    const MODULE: Option<&'static kernel::ThisModule> = Some(&crate::THIS_MODULE);
    const FEATURES: u32 = drm::driver::FEAT_GEM
        | drm::driver::FEAT_RENDER
        | drm::driver::FEAT_SYNCOBJ
        | drm::driver::FEAT_SYNCOBJ_TIMELINE
        | drm::driver::FEAT_GEM_GPUVA;

    kernel::declare_drm_ioctls! {
        (ASAHI_GET_PARAMS,      drm_asahi_get_params,
                          ioctl::RENDER_ALLOW, crate::file::File::get_params),
        (ASAHI_GET_TIME,        drm_asahi_get_time,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::get_time),
        (ASAHI_VM_CREATE,       drm_asahi_vm_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_create),
        (ASAHI_VM_DESTROY,      drm_asahi_vm_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_destroy),
        (ASAHI_VM_BIND,         drm_asahi_vm_bind,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::vm_bind),
        (ASAHI_GEM_CREATE,      drm_asahi_gem_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_create),
        (ASAHI_GEM_MMAP_OFFSET, drm_asahi_gem_mmap_offset,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_mmap_offset),
        (ASAHI_GEM_BIND_OBJECT, drm_asahi_gem_bind_object,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::gem_bind_object),
        (ASAHI_QUEUE_CREATE,    drm_asahi_queue_create,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::queue_create),
        (ASAHI_QUEUE_DESTROY,   drm_asahi_queue_destroy,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::queue_destroy),
        (ASAHI_SUBMIT,          drm_asahi_submit,
            ioctl::AUTH | ioctl::RENDER_ALLOW, crate::file::File::submit),
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
        (
            of::DeviceId::new(c_str!("apple,agx-t8103")),
            ProbeConfig::Legacy(&hw::t8103::HWCONFIG)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t8112")),
            ProbeConfig::Legacy(&hw::t8112::HWCONFIG)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6000")),
            ProbeConfig::Legacy(&hw::t600x::HWCONFIG_T6000)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6001")),
            ProbeConfig::Legacy(&hw::t600x::HWCONFIG_T6001)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6002")),
            ProbeConfig::Legacy(&hw::t600x::HWCONFIG_T6002)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6020")),
            ProbeConfig::Legacy(&hw::t602x::HWCONFIG_T6020)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6021")),
            ProbeConfig::Legacy(&hw::t602x::HWCONFIG_T6021)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6022")),
            ProbeConfig::Legacy(&hw::t602x::HWCONFIG_T6022)
        ),
    ]
);

/// Platform Driver implementation for `AsahiDriver`.
impl platform::Driver for AsahiDriver {
    type IdInfo = ProbeConfig;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn unbind(_pdev: &platform::Device<Core>, this: Pin<&Self>) {
        if let Some(gpu) = (*this.drm).gpu.as_any().downcast_ref::<g17::Gpu>() {
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

        // TODO: This is very temporary
        // SAFETY: This should be safe as data is not touched by the driver
        // untill it gets fully initialised.
        // Additionally drm::device::Device::release() will not drop data and
        // leaks instead.
        let uninit = unsafe {
            pin_init::pin_init_from_closure::<AsahiData, kernel::error::Error>(|_slot| Ok(()))
        };
        let drm: ARef<AsahiDevice> = drm::device::Device::new(pdev.as_ref(), uninit)?;

        let gpu = match (cfg.gpu_gen, cfg.gpu_variant, compat.as_slice()) {
            (hw::GpuGen::G13, _, &[12, 3, 0]) => {
                gpu::GpuManagerG13V12_3::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[12, 4, 0]) => {
                gpu::GpuManagerG14V12_4::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G13, _, &[13, 5, 0]) => {
                gpu::GpuManagerG13V13_5::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[13, 5, 0]) => {
                gpu::GpuManagerG14V13_5::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
            }
            (hw::GpuGen::G14, _, &[13, 5, 0]) => {
                gpu::GpuManagerG14XV13_5::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::Gpu>
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

        let ptr: *const AsahiData = &raw const **drm;
        unsafe {
            data.__pinned_init(ptr as *mut AsahiData)?;
        }

        (*drm).gpu.init()?;

        drm::driver::Registration::new_foreign_owned(&drm, pdev.as_ref(), 0)?;

        Ok(Self { drm })
    }
}

impl AsahiDriver {
    fn probe_g17(pdev: &platform::Device<Core>, cfg: &'static g17::Config) -> Result<Self> {
        // SAFETY: The GPU performs DMA through a UAT whose output width is part of the SoC config.
        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(cfg.uat_oas)?)? };
        let res = regs::Resources::new(pdev)?;
        // SAFETY: As in the legacy probe, the data is inaccessible until the fully initialized
        // manager is installed below and the DRM device is registered.
        // If construction fails, drm::device::Device::release leaves the data
        // untouched rather than dropping an uninitialized AsahiData.
        let uninit = unsafe { pin_init::pin_init_from_closure::<AsahiData, Error>(|_slot| Ok(())) };
        let drm: ARef<AsahiDevice> = drm::device::Device::new(pdev.as_ref(), uninit)?;
        let gpu = g17::Gpu::new(pdev, &drm, cfg, res.clone())?;
        let data_gpu = gpu.clone() as Arc<dyn gpu::Gpu>;
        let data = try_pin_init!(AsahiData {
            gpu: data_gpu,
            pdev: pdev.into(),
            resources: res,
        });
        let ptr = &raw const **drm;
        // SAFETY: The allocation is pinned and its data slot has not been initialized yet.
        if let Err(error) = unsafe { data.__pinned_init(ptr as *mut AsahiData) } {
            gpu.shutdown();
            return Err(error);
        }
        if let Err(error) = drm::driver::Registration::new_foreign_owned(&drm, pdev.as_ref(), 0) {
            gpu.shutdown();
            return Err(error);
        }
        Ok(Self { drm })
    }
}

impl Drop for AsahiDriver {
    fn drop(&mut self) {
        if let Some(gpu) = (*self.drm).gpu.as_any().downcast_ref::<g17::Gpu>() {
            // Also covers failure to allocate platform driver data after the probe body returned.
            gpu.shutdown();
        }
    }
}
