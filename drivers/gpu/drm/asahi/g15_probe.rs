// SPDX-License-Identifier: GPL-2.0-only OR MIT


use core::sync::atomic::{
    AtomicBool,
    Ordering, //
};

use kernel::{
    c_str,
    device::Core,
    dma::{
        Device,
        DmaMask, //
    },
    drm,
    platform,
    prelude::*,
    sync::{
        aref::ARef,
        Arc, //
    },
};

use crate::{
    driver::{
        AsahiData,
        AsahiDevice, //
    },
    drm_gpu,
    fw,
    gpu,
    hw,
    m3_params::{
        g15_debug,
        G15Debug, //
    },
    regs, //
};

/// Whether the G15 / 14.8.3 firmware structures have the sizes the firmware expects.
///
/// Handing the firmware a wrong layout would at best make it reject InitData and at worst
/// corrupt memory, so the probe is refused (before any MMIO is touched) unless every top-level
/// structure has the firmware's size: InitData 0xc0, RuntimePointers 0x490, Globals 0xe00,
/// HwDataA 0x4360, HwDataB 0x1868.
const G15_FW_LAYOUT_READY: bool = {
    use core::mem::size_of;
    use fw::initdata::raw;
    size_of::<raw::InitDataG15V14_8_3<'static>>() == 0xc0
        && size_of::<raw::RuntimePointersG15V14_8_3<'static>>() == 0x490
        && size_of::<raw::GlobalsG15V14_8_3>() == 0xe00
        && size_of::<raw::HwDataAG15V14_8_3>() == 0x4360
        && size_of::<raw::HwDataBG15V14_8_3>() == 0x1868
};

/// A bound G15 manager backend. Dropping it (on unbind) turns a still-pending firmware health
/// report into a no-op; the DRM device owns the GpuManager.
pub(crate) struct G15Manager {
    _drm: ARef<AsahiDevice>,
    report_cancel: Option<Arc<AtomicBool>>,
}

impl Drop for G15Manager {
    fn drop(&mut self) {
        if let Some(cancel) = &self.report_cancel {
            cancel.store(true, Ordering::Release);
        }
    }
}

/// Whether the DRM device (card/render nodes) is registered for the manager backend: only when
/// asked for with `asahi.m3_expose=1` (or `asahi.g15_debug` bit 42). The manager backend is not
/// validated for userspace on any board, so `auto` keeps it hidden.
fn expose_drm() -> bool {
    *crate::module_parameters::m3_expose.value() > 0 || g15_debug(G15Debug::ExposeDrm)
}

/// Probe the G15 manager backend on a t6030 described by the static device tree.
pub(crate) fn probe(pdev: &platform::Device<Core>) -> Result<G15Manager> {
    let cfg: &'static hw::HwConfig = &hw::t6030::HWCONFIG_T6030;

    if !G15_FW_LAYOUT_READY {
        dev_err!(
            pdev.as_ref(),
            "G15 firmware structure layouts are incomplete; refusing to probe\n"
        );
        return Err(ENODEV);
    }

    // Check the firmware ABI before any MMIO is touched or the ASC is started. The only
    // supported ABI is <14 8 3>; the placeholder DT value <0 0 0> (no bootloader support) or a
    // fallback OS version must not reach the hardware setup.
    let fwnode = pdev.as_ref().fwnode().ok_or(EIO)?;
    let compat: KVec<u32> = fwnode
        .property_read_array_vec(c_str!("apple,firmware-compat"), 3)?
        .required_by(pdev.as_ref())?;
    if compat.as_slice() != [14, 8, 3] {
        dev_info!(
            pdev.as_ref(),
            "Unsupported G15 firmware ABI {:?} (need [14, 8, 3])\n",
            compat
        );
        return Err(ENODEV);
    }

    // The G15 MMIO setup needs the TTBAT base (the `ttbs` reserved-memory region).
    let node = pdev.as_ref().of_node().ok_or(EINVAL)?;
    let ttbs = node
        .reserved_mem_region_to_resource_byname(c_str!("ttbs"))
        .inspect_err(|_| dev_err!(pdev.as_ref(), "Missing ttbs memory region\n"))?;

    // SAFETY: The DMA mask is set before any DMA mapping is created for this device.
    unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(cfg.uat_oas)?)? };

    let res = regs::Resources::new(pdev)?;

    // Initialize misc MMIO, then the G15 Fender MMU/TTBAT block.
    res.init_mmio()?;
    res.init_mmio_g15(ttbs.start() as u64)?;

    // A/B experiment only (asahi.g15_debug bit 41): the Fender kick (0x11 at probe, 0x10 at
    // start). The G15 firmware boots without it.
    if g15_debug(G15Debug::FenderKick) {
        res.g15_fender_kick(regs::FENDER_KICK_PROBE)?;
        res.g15_fender_kick(regs::FENDER_KICK_START)?;
    }

    // Start the coprocessor CPU, so UAT can initialize the handoff
    regs::Resources::start_cpu(pdev)?;

    let drm: ARef<AsahiDevice> =
        drm::device::Device::new(pdev.as_ref(), AsahiData::new(pdev, Some(res), true))?;
    let res = drm.resources.as_ref().ok_or(ENODEV)?;

    let legacy_gpu =
        gpu::GpuManagerG15V14_8_3::new(&drm.clone(), res, cfg)? as Arc<dyn gpu::GpuManager>;
    let gpu = drm_gpu::Backend::Legacy(drm_gpu::LegacyDrmGpu::new(legacy_gpu.clone()));

    if !drm.gpu.populate(gpu) {
        return Err(EBUSY);
    }

    (*drm).gpu()?.init()?;

    // The firmware is booted above, but userspace job submission is not ready on this path, and
    // system Mesa / compositors that find a render node submit work and hang the display
    // (observed on J516S). Only register the DRM device when explicitly asked to. Nothing in the
    // driver depends on the registration: `G15Manager` owns the `drm::Device` (and so the
    // GpuManager) either way, and unbinding just drops it.
    if expose_drm() {
        drm::driver::Registration::new_foreign_owned(&drm, pdev.as_ref(), 0)?;
        dev_warn!(
            pdev.as_ref(),
            "G15: DRM device registered: userspace can submit jobs\n"
        );
    } else {
        dev_info!(
            pdev.as_ref(),
            "G15: firmware bring-up only; DRM device not registered (no card/render node)\n"
        );
    }

    // Scheduled only once probe can no longer fail.
    let report_cancel = gpu::schedule_g15_health_report(legacy_gpu);

    Ok(G15Manager {
        _drm: drm,
        report_cancel,
    })
}
