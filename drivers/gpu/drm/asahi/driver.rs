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
    new_mutex,
    of,
    platform,
    prelude::*,
    sync::{
        aref::ARef,
        Arc, SetOnce, //
    },
    workqueue::{self, impl_has_work, new_work, Work, WorkItem}, //
};

use crate::{
    agx_power_recovery,
    debug,
    drm_gpu,
    file,
    g15_boot,
    g16_firmware,
    g16_device,
    g16_resources,
    g17_boot,
    g17_drm,
    g17_firmware,
    g17_live_boot,
    g17_manager,
    g17_power_preflight,
    gem::AsahiObject,
    gpu,
    hw,
    identity,
    regs, //
};

use kernel::macros::vtable;

/// Holds a reference to the top-level `GpuManager` object.
#[pin_data]
pub(crate) struct AsahiData {
    pub(crate) gpu: SetOnce<Arc<dyn drm_gpu::DrmGpu>>,
    pub(crate) pdev: ARef<platform::Device>,
    pub(crate) resources: Option<regs::Resources>,
    #[pin]
    g16_completion_work: Work<AsahiDevice, 3>,
    g17_runtime: SetOnce<g17_drm::G17PRuntimeRef>,
    #[pin]
    g17_event_work: Work<AsahiDevice, 1>,
    #[pin]
    g17_recovery_work: Work<AsahiDevice, 2>,
}

impl AsahiData {
    pub(crate) fn new(pdev: &platform::Device<Core>, resources: Option<regs::Resources>) -> impl PinInit<Self, Error> {
        let pdev: ARef<platform::Device> = pdev.into();
        try_pin_init!(Self {
            gpu: SetOnce::new(),
            pdev,
            resources,
            g17_runtime: SetOnce::new(),
            g16_completion_work <- new_work!("AsahiData::g16_completion_work"),
            g17_event_work <- new_work!("AsahiData::g17_event_work"),
            g17_recovery_work <- new_work!("AsahiData::g17_recovery_work"),
        })
    }

    /// Runtime construction can use GEM before the GPU backend exists. All
    /// DRM operations fail with ENODEV until probe publishes the backend once.
    pub(crate) fn gpu(&self) -> Result<&Arc<dyn drm_gpu::DrmGpu>> {
        self.gpu.as_ref().ok_or(ENODEV)
    }
}

unsafe impl Send for AsahiData {}
unsafe impl Sync for AsahiData {}

impl_has_work! {
    impl HasWork<AsahiDevice, 3> for AsahiData { self.g16_completion_work }
    impl HasWork<AsahiDevice, 1> for AsahiData { self.g17_event_work }
    impl HasWork<AsahiDevice, 2> for AsahiData { self.g17_recovery_work }
}

impl WorkItem<3> for AsahiData {
    type Pointer = ARef<AsahiDevice>;
    fn run(dev: ARef<AsahiDevice>) {
        if let Ok(gpu) = dev.gpu() { gpu.service_g16_jobs(); }
    }
}
pub(crate) fn queue_g16_completion_worker(dev: ARef<AsahiDevice>) {
    let _ = workqueue::system_highpri().enqueue::<ARef<AsahiDevice>, 3>(dev);
}

impl WorkItem<1> for AsahiData {
    type Pointer = ARef<AsahiDevice>;

    fn run(dev: ARef<AsahiDevice>) {
        let Some(runtime) = dev.g17_runtime.copy() else {
            return;
        };
        if let Err(error) = runtime.service_primary_firmware_events() {
            dev_err!(
                dev.as_ref(),
                "G17P event worker: serialized drain/recovery failed ({:?})\n",
                error
            );
            return;
        }
        if runtime.primary_recovery_pending() {
            queue_g17p_recovery_worker(dev);
        } else if runtime.primary_firmware_event_pending() {
            queue_g17p_firmware_event_worker(dev);
        }
    }
}

impl WorkItem<2> for AsahiData {
    type Pointer = ARef<AsahiDevice>;

    fn run(dev: ARef<AsahiDevice>) {
        let Some(runtime) = dev.g17_runtime.copy() else {
            return;
        };
        if let Err(error) = runtime.service_primary_recovery() {
            dev_err!(
                dev.as_ref(),
                "G17P recovery worker: serialized recovery failed ({:?})\n",
                error
            );
            return;
        }
        if runtime.primary_firmware_event_pending() {
            queue_g17p_firmware_event_worker(dev);
        } else if runtime.primary_recovery_pending() {
            queue_g17p_recovery_worker(dev);
        }
    }
}

pub(crate) fn queue_g17p_firmware_event_worker(dev: ARef<AsahiDevice>) {
    let _ = workqueue::system_highpri().enqueue::<ARef<AsahiDevice>, 1>(dev);
}

pub(crate) fn queue_g17p_recovery_worker(dev: ARef<AsahiDevice>) {
    let _ = workqueue::system_highpri().enqueue::<ARef<AsahiDevice>, 2>(dev);
}

#[allow(dead_code)]
enum AsahiRuntime {
    Legacy(ARef<drm::Device<AsahiDriver>>),
    G17P(g17_drm::SharedG17PRuntime),
    G16(crate::g16_drm::Registered),
    M3(crate::m3_drm::Registered),
    G15(crate::g15_probe::G15Manager),
}

pub(crate) struct AsahiDriver {
    runtime: AsahiRuntime,
}

/// A matched device either has a complete legacy hardware configuration or is
/// an exact AGX3 identification target whose firmware boundary remains closed.
pub(crate) enum ProbeConfig {
    Supported(&'static hw::HwConfig),
    /// AGX3 (G15/G16/G17) target: chip identification and the read-only
    /// topology decode run at probe time, then the probe fails closed before
    /// DMA setup, ASC start, or any MMIO write.
    Agx3Diagnostic(&'static hw::agx3::SocConfig),
}

unsafe impl Send for AsahiDriver {}
unsafe impl Sync for AsahiDriver {}

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type AsahiDevice = drm::device::Device<AsahiDriver>;
pub(crate) type AsahiDevRef = ARef<AsahiDevice>;

/// DRM Driver metadata
const INFO: drm::driver::DriverInfo = drm::driver::DriverInfo {
    major: 0,
    minor: 2,
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

// OF Device ID table.s
kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <AsahiDriver as platform::Driver>::IdInfo,
    [
        (
            of::DeviceId::new(c_str!("apple,agx-t8103")),
            ProbeConfig::Supported(&hw::t8103::HWCONFIG)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t8112")),
            ProbeConfig::Supported(&hw::t8112::HWCONFIG)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6000")),
            ProbeConfig::Supported(&hw::t600x::HWCONFIG_T6000)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6001")),
            ProbeConfig::Supported(&hw::t600x::HWCONFIG_T6001)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6002")),
            ProbeConfig::Supported(&hw::t600x::HWCONFIG_T6002)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6020")),
            ProbeConfig::Supported(&hw::t602x::HWCONFIG_T6020)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6021")),
            ProbeConfig::Supported(&hw::t602x::HWCONFIG_T6021)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6022")),
            ProbeConfig::Supported(&hw::t602x::HWCONFIG_T6022)
        ),
        // AGX3 (G15/G16/G17) identification targets. These intentionally
        // select no HwConfig: unproved power/MMIO/firmware-tuning fields
        // must never be represented by copied or zero-filled legacy values,
        // and the probe fails closed before any firmware handoff.
        (
            of::DeviceId::new(c_str!("apple,agx-t8122")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T8122)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6030")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6030)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6031")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6031)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6032")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6032)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t8132")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T8132)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6040")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6040)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6041")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6041)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t8140")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T8140)
        ),
        (
            of::DeviceId::new(c_str!("gpu,t8140")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T8140)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t8142")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T8142)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6050")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6050)
        ),
        (
            of::DeviceId::new(c_str!("apple,agx-t6051")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6051)
        ),
        (
            of::DeviceId::new(c_str!("gpu,t6050")),
            ProbeConfig::Agx3Diagnostic(&hw::agx3::T6050)
        ),
    ]
);

/// Start the persistent runtime for an exact T8140/G17P match.
fn probe_t8140_runtime(
    pdev: &platform::Device<Core>,
    soc: &'static hw::agx3::SocConfig,
) -> Result<g17_drm::SharedG17PRuntime> {
    let expected = identity::decode_gpu_identity(soc.hw_family, soc.hw_variant, soc.num_dies as u8)
        .ok_or(EINVAL)?;
    if soc.chip_id != 0x8140
        || expected.gpu_gen != hw::GpuGen::G17
        || expected.gpu_variant != identity::GpuVariant::P
        || expected.uat_input_address_bits != soc.uat_ias
    {
        return Err(EINVAL);
    }

    // Topology is read and admitted before mappings, DMA setup, or provider
    // start. The token is consumed by the retained runtime constructor.
    let topology = admit_t8140_live_topology(pdev, &expected)?;
    let registers = regs::Resources::new(pdev)?;
    let observed = registers.get_gpu_id()?;
    if observed.gpu_gen != soc.gpu_gen
        || observed.gpu_variant != soc.gpu_variant
        || observed.num_dies != soc.num_dies
    {
        dev_err!(
            pdev.as_ref(),
            "G17P: hardware identity {:?}{:?}/{} dies does not match exact T8140 {:?}{:?}/{} dies\n",
            observed.gpu_gen,
            observed.gpu_variant,
            observed.num_dies,
            soc.gpu_gen,
            soc.gpu_variant,
            soc.num_dies
        );
        return Err(ENODEV);
    }

    unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(soc.uat_oas)?)? };

    // GEM construction needs a DRM device before the runtime exists. Its
    // data is fully initialized now; the backend is published before registration.
    let drm: ARef<AsahiDevice> = drm::device::Device::new(pdev.as_ref(), AsahiData::new(pdev, None))?;

    let runtime = g17_live_boot::G17PLiveRuntime::start(
        pdev,
        drm.clone(),
        registers,
        soc,
        &expected,
        &observed,
        topology,
    )?;
    let runtime = Arc::pin_init(new_mutex!(runtime), GFP_KERNEL)?;
    let runtime_ref = g17_drm::G17PRuntimeRef::new(&runtime);
    let gpu: Arc<dyn drm_gpu::DrmGpu> = Arc::new(
        g17_drm::G17PDrmGpu::new(runtime_ref, soc, observed)?,
        GFP_KERNEL,
    )?;
    if !drm.g17_runtime.populate(runtime_ref) || !drm.gpu.populate(gpu) {
        return Err(EBUSY);
    }
    runtime.lock().arm_primary_firmware_event_worker()?;
    runtime.lock().mark_firmware_cache_flush_ready()?;
    (*drm).gpu()?.init()?;
    drm::driver::Registration::new_foreign_owned(&drm, pdev.as_ref(), 0)?;
    Ok(runtime)
}

fn refuse_agx3_probe(pdev: &platform::Device<Core>, soc: &'static hw::agx3::SocConfig) -> Error {
    if soc.chip_id == 0x8132 {
        // Validate the normal device resources before touching SGX registers.
        // The private lab /chosen export is deliberately not a fallback.
        match g16_resources::from_device(pdev)
            .and_then(|resources| g16_firmware::identify_loaded(pdev, resources)) {
            Ok(firmware) => {
                dev_info!(
                    pdev.as_ref(),
                    "G16G: loaded {} text hash matches J713 evidence, PA={:#x} VA={:#x}; owned startup still required\n",
                    firmware.version(), firmware.resources.regions[4].base,
                    firmware.resources.firmware_vas[0]
                );
                match g16_device::Device::new(pdev, firmware) {
                    Ok(device) => {
                        dev_info!(pdev.as_ref(), "G16G: owned power and identity complete; testing common memory and RTKit startup\n");
                        if let Err(error) = device.check_drm(pdev).and_then(|()| device.check_mmu(pdev)).and_then(|()| device.check_tables(pdev)).and_then(|()| {
                            let mut runtime = crate::g16_runtime::Runtime::new(pdev, device)?;
                            runtime.boot(pdev)
                        }) {
                            dev_err!(pdev.as_ref(), "G16G: startup checks failed: {:?}\n", error);
                            return error;
                        }
                    },
                    Err(error) => return error,
                }
            },
            Err(error) => {
                dev_err!(pdev.as_ref(), "G16G: missing J713 resources or unsupported loaded firmware: {:?}\n", error);
                return error;
            }
        }
        // Runtime has stopped and released power. Initdata, jobs and DRM registration follow.
        return ENODEV;
    }

    let Some(expected) =
        identity::decode_gpu_identity(soc.hw_family, soc.hw_variant, soc.num_dies as u8)
    else {
        dev_err!(
            pdev.as_ref(),
            "AGX3: static identity for chip {:#x} does not decode\n",
            soc.chip_id
        );
        return EINVAL;
    };

    if expected.uat_input_address_bits != soc.uat_ias {
        dev_err!(
            pdev.as_ref(),
            "AGX3: static UAT width for chip {:#x} disagrees with the identity decode\n",
            soc.chip_id
        );
        return EINVAL;
    }

    dev_info!(
        pdev.as_ref(),
        "AGX3: matched {:?}{:?} (chip {:#x}, USC gen {}, HAL {:?}, {:?} firmware roles, {:?} submission, {}-bit UAT roots, {}-bit OAS)\n",
        expected.gpu_gen,
        expected.gpu_variant,
        soc.chip_id,
        expected.usc_generation,
        expected.gpu_hal_generation,
        expected.firmware_roles,
        expected.submission_transport,
        soc.uat_ias,
        soc.uat_oas
    );

    // The G17S loaded-firmware handoff is reserved memory, not GPU MMIO. It
    // must be mapped and hashed before touching the SGX register aperture so
    // firmware identity can never be inferred from a powered or partially
    // initialized GPU. Other G17 variants never consume these G17S carveouts.
    // This token grants identity only.
    let loaded_g17_firmware = if expected.gpu_gen == hw::GpuGen::G17
        && expected.gpu_variant == identity::GpuVariant::S
    {
        match g17_firmware::admit_g17_loaded_pair_from_handoff(expected.gpu_variant as u32 as u8) {
            Ok(admission) => {
                dev_info!(
                    pdev.as_ref(),
                    "G17: exact versioned GFX/GFX1 loaded-segment identity admitted before SGX MMIO\n"
                );
                Some(admission)
            }
            Err(e) => {
                dev_err!(
                    pdev.as_ref(),
                    "G17S: loaded-segment firmware identity was invalid; stopping before SGX MMIO: {:?}\n",
                    e
                );
                return ENODEV;
            }
        }
    } else {
        None
    };

    match regs::Resources::new(pdev) {
        Ok(res) => match res.get_gpu_id() {
            Ok(id) => {
                dev_info!(
                    pdev.as_ref(),
                    "AGX3: hardware reports {:?}{:?} rev {:?}, {} dies, {} clusters, {} cores/cluster, {} active cores\n",
                    id.gpu_gen,
                    id.gpu_variant,
                    id.gpu_rev,
                    id.num_dies,
                    id.num_clusters,
                    id.num_cores,
                    id.total_active_cores
                );
                if id.gpu_gen != soc.gpu_gen || id.gpu_variant != soc.gpu_variant {
                    dev_warn!(
                        pdev.as_ref(),
                        "AGX3: hardware identity {:?}{:?} does not match the matched compatible ({:?}{:?})\n",
                        id.gpu_gen,
                        id.gpu_variant,
                        soc.gpu_gen,
                        soc.gpu_variant
                    );
                }
            }
            Err(e) => dev_warn!(pdev.as_ref(), "AGX3: GPU ID decode failed: {:?}\n", e),
        },
        Err(e) => {
            dev_warn!(
                pdev.as_ref(),
                "AGX3: SGX MMIO region unavailable: {:?}\n",
                e
            );
        }
    }

    match expected.gpu_gen {
        hw::GpuGen::G15 => refuse_g15_probe(pdev),
        hw::GpuGen::G17 => refuse_g17_probe(pdev, soc, &expected, loaded_g17_firmware.as_ref()),
        gen => {
            dev_err!(
                pdev.as_ref(),
                "AGX3 setup unavailable for {:?}: firmware tuning, initdata, and handoff are incomplete; no DMA or processor state changed\n",
                gen
            );
            ENODEV
        }
    }
}

/// Report the grounded-vs-UNKNOWN state of the single-role G15 (M3) boot, then
/// stop the probe. G15 is the easiest AGX3 target — one `GFX` role, classic
/// device-control-ring submission — but the firmware image is not on disk and
/// the init-data/RTKit graph is not boot-complete offline.
fn refuse_g15_probe(pdev: &platform::Device<Core>) -> Error {
    let Err(missing) = g15_boot::pre_handoff_gate(hw::GpuGen::G15 as u32) else {
        dev_err!(
            pdev.as_ref(),
            "G15 diagnostic target remained selected after its preflight opened\n"
        );
        return EINVAL;
    };

    dev_info!(
        pdev.as_ref(),
        "G15 identity decoded (single-role GFX, classic submission); grounded init-data root and shared RTKit codec are diagnostic-only\n"
    );
    dev_err!(
        pdev.as_ref(),
        "G15 setup incomplete: evidence mask {:#x}; no DMA, MMIO, UAT, or processor state changed\n",
        missing.bits()
    );
    if missing.contains(g15_boot::missing::FIRMWARE_IMAGE) {
        dev_err!(
            pdev.as_ref(),
            "G15 preflight: the G15 GFX RTKit firmware image and its nested identity are not available offline\n"
        );
    }
    if missing.contains(g15_boot::missing::UAT_HANDOFF) {
        dev_err!(
            pdev.as_ref(),
            "G15 preflight: the dual-TTBR 42-bit UAT/Handoff firmware-shared state is not validated\n"
        );
    }
    if missing.contains(g15_boot::missing::INITDATA_GRAPH) {
        dev_err!(
            pdev.as_ref(),
            "G15 preflight: init-data root and runtime-pointer graph are grounded, but hw_globals and struct sizes are not boot-complete\n"
        );
    }
    if missing.contains(g15_boot::missing::RTKIT_TRANSPORT) {
        dev_err!(
            pdev.as_ref(),
            "G15 preflight: the GPU RTKit codec is grounded but HELLO/EPMAP transmit is unproved\n"
        );
    }
    if missing.contains(g15_boot::missing::INTERFACE_VERSION) {
        dev_err!(
            pdev.as_ref(),
            "G15 preflight: the EP0 RTKit interface version for this firmware/OS build is unknown\n"
        );
    }
    if missing.contains(g15_boot::missing::SUBMISSION_TRANSPORT) {
        dev_err!(
            pdev.as_ref(),
            "G15 preflight: classic register-list submission is not implemented\n"
        );
    }

    ENODEV
}

/// Read one T8140 ASCWrap v6 provider from a `mboxes` phandle node.
///
/// Reads the node's two `reg` windows (CPU physical, #address/#size-cells = 2
/// each), the two named `send-empty`/`recv-not-empty` AIC interrupt numbers
/// (3 cells per interrupt), and the `apple,firmware-role` string. Anything
/// missing or malformed fails closed (returns `None`) rather than guessing.
fn read_t8140_asc_observation(
    pdev: &platform::Device<Core>,
    index: usize,
) -> Option<agx_power_recovery::T8140G17PLinuxDtAscObservation> {
    let node = pdev.as_ref().of_node()?;
    let mbox = node.parse_phandle(c_str!("mboxes"), index)?;

    // Require the exact provider ABI rather than accepting a node with the
    // same numeric resources under a different owner contract.
    let compatible: KVec<u8> = mbox.get_property(c_str!("compatible")).ok()?;
    if compatible.as_slice() != b"apple,t8140-ascwrap-v6\0" {
        return None;
    }
    let reg_names: KVec<u8> = mbox.get_property(c_str!("reg-names")).ok()?;
    if reg_names.as_slice() != b"wrapper\0iop-vbar\0" {
        return None;
    }
    let interrupt_names: KVec<u8> = mbox.get_property(c_str!("interrupt-names")).ok()?;
    if interrupt_names.as_slice() != b"send-empty\0recv-not-empty\0" {
        return None;
    }

    // reg = <wrapper_hi wrapper_lo wsize_hi wsize_lo iovbar_hi iovbar_lo isize_hi isize_lo>
    let reg: KVec<u32> = mbox.get_property(c_str!("reg")).ok()?;
    if reg.len() != 8 {
        return None;
    }
    let cell64 = |hi: u32, lo: u32| ((hi as u64) << 32) | lo as u64;

    // interrupts = <type send flags type recv flags>, AIC number is cell 1/4.
    let interrupts: KVec<u32> = mbox.get_property(c_str!("interrupts")).ok()?;
    const AIC_IRQ: u32 = 0;
    const IRQ_TYPE_LEVEL_HIGH: u32 = 4;
    if interrupts.len() != 6
        || interrupts[0] != AIC_IRQ
        || interrupts[2] != IRQ_TYPE_LEVEL_HIGH
        || interrupts[3] != AIC_IRQ
        || interrupts[5] != IRQ_TYPE_LEVEL_HIGH
    {
        return None;
    }

    // apple,firmware-role is "GFX" (primary) or "GFX1" (secondary).
    let role: KVec<u8> = mbox.get_property(c_str!("apple,firmware-role")).ok()?;
    let role_is_gfx1 = match role.as_slice() {
        b"GFX\0" | b"GFX" => false,
        b"GFX1\0" | b"GFX1" => true,
        _ => return None,
    };

    Some(agx_power_recovery::T8140G17PLinuxDtAscObservation {
        role_is_gfx1,
        wrapper_cpu_base: cell64(reg[0], reg[1]),
        wrapper_size: cell64(reg[2], reg[3]),
        iorvbar_cpu_base: cell64(reg[4], reg[5]),
        iorvbar_size: cell64(reg[6], reg[7]),
        send_empty_irq: interrupts[1],
        recv_not_empty_irq: interrupts[4],
    })
}

/// Read the full observed T8140 dual-ASC topology from the platform device
/// tree, exactly as a Linux platform decoder sees it. `None` when the two
/// `mboxes` providers cannot be read; the caller then reports missing
/// topology rather than admitting an inferred one.
fn read_t8140_linux_dt_topology(
    pdev: &platform::Device<Core>,
) -> Option<agx_power_recovery::T8140G17PLinuxDtTopology> {
    let node = pdev.as_ref().of_node()?;
    let gpu_reg_names: KVec<u8> = node.get_property(c_str!("reg-names")).ok()?;
    let mboxes: KVec<u32> = node.get_property(c_str!("mboxes")).ok()?;
    if gpu_reg_names.as_slice() != b"asc\0sgx\0"
        || mboxes.len() != 2
        || node.parse_phandle(c_str!("mboxes"), 2).is_some()
    {
        return None;
    }

    Some(agx_power_recovery::T8140G17PLinuxDtTopology {
        gfx: read_t8140_asc_observation(pdev, 0)?,
        gfx1: read_t8140_asc_observation(pdev, 1)?,
    })
}

/// Proof that the live T8140 DT matched both pinned provider roles exactly.
///
/// Construction is private to the read-only admission above; live boot must
/// receive this token before it can query either provider.
pub(crate) struct T8140LiveTopologyAdmission {
    _private: (),
}

fn admit_t8140_live_topology(
    pdev: &platform::Device<Core>,
    identity: &identity::GpuIdentity,
) -> Result<T8140LiveTopologyAdmission> {
    let observed = read_t8140_linux_dt_topology(pdev).ok_or(EINVAL)?;
    let preflight =
        g17_power_preflight::G17PowerPreflight::new_with_linux_dt(identity, Some(&observed))
            .map_err(|_| EINVAL)?;
    if preflight.t8140_strategy().is_none() {
        return Err(EINVAL);
    }

    Ok(T8140LiveTopologyAdmission { _private: () })
}

fn refuse_g17_probe(
    pdev: &platform::Device<Core>,
    soc: &'static hw::agx3::SocConfig,
    identity: &identity::GpuIdentity,
    firmware: Option<&g17_firmware::G17FirmwareIdentityAdmission>,
) -> Error {
    // G17P binds its power preflight to the exact D93AP/T8140 topology the
    // Linux device tree actually exposes (its two `mboxes` providers). Never
    // substitute the expected constant: only a real observation that decodes
    // to the pinned providers admits the A18 Pro strategy.
    let power_preflight = if identity.gpu_variant == identity::GpuVariant::P {
        let observed = read_t8140_linux_dt_topology(pdev);
        if observed.is_none() {
            dev_warn!(
                pdev.as_ref(),
                "G17P: could not read the T8140 dual-ASC topology from the device tree mboxes\n"
            );
        }
        match g17_power_preflight::G17PowerPreflight::new_with_linux_dt(identity, observed.as_ref())
        {
            Ok(preflight) => preflight,
            Err(e) => {
                dev_err!(
                    pdev.as_ref(),
                    "G17P topology mismatch: {:?}\n",
                    e
                );
                return ENODEV;
            }
        }
    } else {
        match g17_power_preflight::G17PowerPreflight::new(identity, None) {
            Ok(preflight) => preflight,
            Err(e) => {
                dev_err!(
                    pdev.as_ref(),
                    "G17 target setup unavailable: {:?}\n",
                    e
                );
                return ENODEV;
            }
        }
    };
    let Err(power_missing) = power_preflight.execution_gate() else {
        dev_err!(
            pdev.as_ref(),
            "G17 diagnostic target remained selected after its power execution gate opened\n"
        );
        return EINVAL;
    };
    dev_info!(
        pdev.as_ref(),
        "G17 {:?} cold-boot/power model selected; live execution evidence still missing {:#x}\n",
        power_preflight.target(),
        power_missing.bits()
    );

    let gpu_variant = identity.gpu_variant;
    let gate = match gpu_variant {
        identity::GpuVariant::S => g17_boot::pre_handoff_gate_with_identity(
            hw::GpuGen::G17 as u32,
            g17_boot::G17FirmwareVariant::G17S,
            firmware,
        ),
        identity::GpuVariant::P => g17_boot::pre_handoff_gate_for_variant(
            hw::GpuGen::G17 as u32,
            g17_boot::G17FirmwareVariant::G17P,
        ),
        _ => g17_boot::pre_handoff_gate(hw::GpuGen::G17 as u32),
    };
    let Err(missing) = gate else {
        dev_err!(
            pdev.as_ref(),
            "G17 diagnostic target remained selected after its preflight opened\n"
        );
        return EINVAL;
    };

    dev_info!(
        pdev.as_ref(),
        "G17 identity decoded; grounded init-data and RTKit codecs are diagnostic-only\n"
    );
    if firmware.is_some() {
        let Err(default_missing) = g17_boot::pre_handoff_gate(hw::GpuGen::G17 as u32) else {
            dev_err!(
                pdev.as_ref(),
                "G17: default pre-handoff unexpectedly opened\n"
            );
            return EINVAL;
        };
        if missing.bits() != default_missing.bits() & !g17_boot::missing::FIRMWARE_IDENTITY {
            dev_err!(
                pdev.as_ref(),
                "G17 firmware admission changed an unrelated setup gate\n"
            );
            return EINVAL;
        }
        dev_info!(
            pdev.as_ref(),
            "G17: loaded GFX/GFX1 evidence cleared the firmware-identity bit; remaining operations are reported below\n"
        );
    }
    dev_err!(
        pdev.as_ref(),
        "G17 setup incomplete: evidence mask {:#x}; no DMA, MMIO, UAT, or processor state changed\n",
        missing.bits()
    );
    if missing.contains(g17_boot::missing::FIRMWARE_IDENTITY) {
        dev_err!(
            pdev.as_ref(),
            "G17 preflight: no exact versioned m1n1 loaded-segment GFX/GFX1 identity was admitted\n"
        );
    }
    if missing.contains(g17_boot::missing::ROLE_RESOURCES) {
        dev_err!(
            pdev.as_ref(),
            "G17 preflight: distinct GFX/GFX1 resource plumbing is not wired\n"
        );
    }
    if missing.contains(g17_boot::missing::TOPOLOGY_REGISTERS) {
        dev_err!(
            pdev.as_ref(),
            "G17 preflight: G17 core-mask register bank is not proven\n"
        );
    }
    if missing.contains(g17_boot::missing::UAT_HANDOFF) {
        dev_err!(
            pdev.as_ref(),
            "G17 preflight: SPTM/UAT ownership handoff is not implemented\n"
        );
    }
    if missing.contains(g17_boot::missing::INITDATA_GRAPH) {
        dev_err!(
            pdev.as_ref(),
            "G17 preflight: init-data graph has grounded fields but is not boot-complete\n"
        );
    }
    if missing.contains(g17_boot::missing::RTKIT_TRANSPORT) {
        dev_err!(
            pdev.as_ref(),
            "G17 preflight: RTKit codec is grounded but HELLO/EPMAP transmit is unproved\n"
        );
    }
    if missing.contains(g17_boot::missing::GFX1_POWER_SEQUENCE) {
        dev_err!(
            pdev.as_ref(),
            "{}{}",
            "G17 preflight: G17S PIO type identities, transition order, intervening state/sync order, and one 250us-reported poll are grounded, ",
            "but type 0/0xb physical bases, opaque helper ownership, and G17G/G17P firmware parity are incomplete\n"
        );
    }
    if missing.contains(g17_boot::missing::RECOVERY_HANDSHAKE) {
        dev_err!(
            pdev.as_ref(),
            "{}{}",
            "G17 preflight: recovery word states 0..3, host writes 1->2/3->0, and firmware writes 1 at begin/0 at completion are grounded, ",
            "but host-visible DRAM causality and the cross-role 0x22/0x23 handshake are incomplete\n"
        );
    }
    if missing.contains(g17_boot::missing::SUBMISSION_TRANSPORT) {
        dev_err!(
            pdev.as_ref(),
            "{}{}",
            "G17 preflight: submission codecs include the grounded EP 0x21 doorbell, SKSM queue/configure words, fixed-slot scratch geometry, and kick-record table; ",
            "Linux resource-0 ordered publication is implemented; firmware execution, the round trip, and completion IRQ causality remain unproved\n"
        );
    }

    if identity.gpu_variant == identity::GpuVariant::P {
        let target = g17_manager::TargetObservation {
            chip_id: soc.chip_id,
            declared_gen: soc.gpu_gen,
            declared_variant: soc.gpu_variant,
            hw_family: soc.hw_family,
            hw_variant: soc.hw_variant,
            num_dies: soc.num_dies,
            uat_ias: soc.uat_ias,
            uat_oas: soc.uat_oas,
        };
        let platform = g17_manager::T8140_G17P_PLATFORM_CONFIG;
        match g17_manager::validate_target(target)
            .and_then(|_| g17_manager::validate_platform(platform))
            .and_then(|_| platform.sksm_scratch.fifo_offsets())
        {
            Ok(scratch) => dev_info!(
                pdev.as_ref(),
                "G17P manager inputs admitted without AGX2 HwConfig: {}-bit UAT, {:#x}-byte pages, dual-role resource geometry, SKSM scratch {:#x}/{:#x}; resource-0 ordered publisher available\n",
                g17_manager::T8140_MANAGER_CONFIG.uat_ias,
                g17_manager::T8140_MANAGER_CONFIG.uat_page_size,
                scratch.write0,
                scratch.write1
            ),
            Err(e) => dev_err!(
                pdev.as_ref(),
                "G17P manager inputs were invalid before resource allocation: {:?}\n",
                e
            ),
        }
    }

    ENODEV
}

/// Platform Driver implementation for `AsahiDriver`.
impl platform::Driver for AsahiDriver {
    type IdInfo = ProbeConfig;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn unbind(_pdev: &platform::Device<Core>, this: Pin<&Self>) {
        if let AsahiRuntime::G16(runtime) = &this.runtime {
            runtime.stop();
        }
        if let AsahiRuntime::M3(runtime) = &this.runtime {
            runtime.stop();
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
            ProbeConfig::Supported(cfg) => *cfg,
            ProbeConfig::Agx3Diagnostic(soc) if soc.chip_id == 0x6030 => {
                match crate::m3_params::t6030_backend(pdev) {
                    crate::m3_params::T6030Backend::Manager => {
                        let manager = crate::g15_probe::probe(pdev)?;
                        return Ok(Self { runtime: AsahiRuntime::G15(manager) });
                    }
                    crate::m3_params::T6030Backend::Off => return Err(ENODEV),
                    crate::m3_params::T6030Backend::Runtime => {}
                }
                let runtime = crate::m3_drm::Registered::start(pdev)?;
                return Ok(Self { runtime: AsahiRuntime::M3(runtime) });
            }
            ProbeConfig::Agx3Diagnostic(soc) if soc.chip_id == 0x8132 => {
                let runtime = crate::g16_drm::Registered::start(pdev)?;
                return Ok(Self { runtime: AsahiRuntime::G16(runtime) });
            }
            ProbeConfig::Agx3Diagnostic(soc) if soc.chip_id == 0x8140 => {
                let runtime = probe_t8140_runtime(pdev, soc)?;
                dev_info!(
                    pdev.as_ref(),
                    "G17P: persistent dual-ASC runtime bound and DRM render node registered\n"
                );
                return Ok(Self {
                    runtime: AsahiRuntime::G17P(runtime),
                });
            }
            ProbeConfig::Agx3Diagnostic(soc) => return Err(refuse_agx3_probe(pdev, soc)),
        };

        if let Err(missing) = g17_boot::pre_handoff_gate(cfg.gpu_gen as u32) {
            dev_err!(
                pdev.as_ref(),
                "G17 dual-role setup incomplete: evidence mask {:#x}; processors were not started\n",
                missing.bits()
            );
            return Err(ENODEV);
        }

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

        let drm: ARef<AsahiDevice> = drm::device::Device::new(
            pdev.as_ref(), AsahiData::new(pdev, Some(res)))?;
        let res = drm.resources.as_ref().ok_or(ENODEV)?;

        let legacy_gpu = match (cfg.gpu_gen, cfg.gpu_variant, compat.as_slice()) {
            (hw::GpuGen::G13, _, &[12, 3, 0]) => {
                gpu::GpuManagerG13V12_3::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[12, 4, 0]) => {
                gpu::GpuManagerG14V12_4::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G13, _, &[13, 5, 0]) => {
                gpu::GpuManagerG13V13_5::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G14, hw::GpuVariant::G, &[13, 5, 0]) => {
                gpu::GpuManagerG14V13_5::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::GpuManager>
            }
            (hw::GpuGen::G14, _, &[13, 5, 0]) => {
                gpu::GpuManagerG14XV13_5::new(&drm.clone(), &res, cfg)? as Arc<dyn gpu::GpuManager>
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
        let gpu: Arc<dyn drm_gpu::DrmGpu> =
            Arc::new(drm_gpu::LegacyDrmGpu::new(legacy_gpu), GFP_KERNEL)?;

        if !drm.gpu.populate(gpu) { return Err(EBUSY); }

        (*drm).gpu()?.init()?;

        drm::driver::Registration::new_foreign_owned(&drm, pdev.as_ref(), 0)?;

        Ok(Self {
            runtime: AsahiRuntime::Legacy(drm),
        })
    }
}
