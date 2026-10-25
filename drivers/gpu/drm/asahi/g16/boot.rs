// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4/J713 cold admission and the qualified 25G83 firmware start sequence.
//!
//! The Rust owner validates firmware identity and all inherited regions before publication,
//! holds the PMP AGX vote, prepares owned objects and publishes context0 while ASC RUN is clear.
//! It negotiates RTKit protocol12, accepts only the inherited EP1 crash buffer, then submits
//! initdata plus NOP0x1a and policy0x34/0x0a. Control+0x14 and exact retired indices qualify
//! READY; no M3 firmware constants or device-control sequence are reused.
//! Firmware-reachable backing and the RTKit cookie remain retained until reboot.

use core::ffi::c_void;
use core::sync::atomic::Ordering;

use kernel::{
    bindings, device,
    device::Core,
    error::{from_err_ptr, to_result},
    platform,
    prelude::*,
    sync::{aref::ARef, Arc, ArcBorrow},
    time::{delay::fsleep, Delta, Instant, Monotonic},
};

use super::adt::{self, GpuAdt};
use super::channel::{self, Channel, Channels};
use super::event::Events;
use super::fw::channels::{devctl, DevCtl};
use super::fw::initdata::control;
use super::hw;
use super::initdata::{self, InitData};
use super::object::Objects;
use super::{regs, uat, Config, Shared};
use crate::driver::AsahiDrmDevice;
use crate::{mmu, regs as shared_regs};
use kernel::io::Io;

/// The C shim (`g16/glue.c`): helpers without Rust bindings.
pub(crate) mod glue {
    use core::sync::atomic::{AtomicBool, Ordering};
    use kernel::{device, prelude::*};

    extern "C" {
        fn asahi_g16_board() -> bool;
        fn asahi_g16_hold_power(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
        fn asahi_g16_module_get();
        fn asahi_g16_ram_disjoint(start: u64, size: u64) -> bool;
        fn asahi_g16_ioremap_wc(start: u64, size: usize) -> *mut core::ffi::c_void;
        fn asahi_g16_dma_unsupported(dev: *mut core::ffi::c_void) -> bool;
        fn asahi_g16_power_domain_state(
            dev: *mut core::ffi::c_void,
            offset: *mut u32,
            state: *mut u32,
        ) -> core::ffi::c_int;
        fn asahi_g16_reserved_region(
            dev: *mut core::ffi::c_void,
            name: *const core::ffi::c_char,
            start: *mut u64,
            size: *mut u64,
        ) -> core::ffi::c_int;
    }

    /// A qualified J713/T8132 board.
    pub(crate) fn board() -> bool {
        // SAFETY: Reads the machine compatible strings only.
        unsafe { asahi_g16_board() }
    }

    /// Holds the GFX power domain for the retained session.
    pub(crate) fn hold_power(dev: &device::Device) -> Result {
        // SAFETY: `dev` is a valid device; runtime-PM accounting on it has no other requirement.
        kernel::error::to_result(unsafe { asahi_g16_hold_power(dev.as_raw().cast()) })
    }

    static RETAINED: AtomicBool = AtomicBool::new(false);

    /// Whether power admission or firmware publication requires the device to remain bound.
    pub(crate) fn retained() -> bool {
        RETAINED.load(Ordering::Acquire)
    }

    /// Pins this module until reboot, once (`__module_get(THIS_MODULE)`).
    pub(crate) fn retain_module() {
        if !RETAINED.swap(true, Ordering::AcqRel) {
            // SAFETY: The module is live while its code runs; the reference is never put.
            unsafe { asahi_g16_module_get() };
        }
    }

    /// The physical range is disjoint from System RAM.
    pub(crate) fn ram_disjoint(start: u64, size: u64) -> bool {
        // SAFETY: Only queries the resource tree.
        unsafe { asahi_g16_ram_disjoint(start, size) }
    }

    /// Whether the device's DMA mappings skip cache maintenance.
    pub(crate) fn dma_unsupported(dev: &device::Device) -> bool {
        // SAFETY: Reads a flag of a valid device.
        unsafe { asahi_g16_dma_unsupported(dev.as_raw().cast()) }
    }

    /// Read the first PMGR power-domain register without changing its ownership or state.
    pub(crate) fn power_domain_state(dev: &device::Device) -> Result<(u32, u32)> {
        let (mut offset, mut state) = (0, 0);
        // SAFETY: The live device owns the OF reference; outputs are valid for this call.
        kernel::error::to_result(unsafe {
            asahi_g16_power_domain_state(dev.as_raw().cast(), &mut offset, &mut state)
        })?;
        Ok((offset, state))
    }

    /// Resolve a validated, non-reusable no-map reserved-memory phandle by name.
    pub(crate) fn reserved_region(dev: &device::Device, name: &CStr) -> Result<(u64, u64)> {
        let (mut start, mut size) = (0, 0);
        // SAFETY: The valid device and C string are borrowed for the call; output pointers live.
        kernel::error::to_result(unsafe {
            asahi_g16_reserved_region(
                dev.as_raw().cast(),
                name.as_char_ptr(),
                &mut start,
                &mut size,
            )
        })?;
        Ok((start, size))
    }

    /// `ioremap_wc`, for the RTKit crash buffer.
    pub(crate) fn ioremap_wc(start: u64, size: usize) -> *mut core::ffi::c_void {
        // SAFETY: Creates a new mapping of a device range; no other effect.
        unsafe { asahi_g16_ioremap_wc(start, size) }
    }
}

/// `EPROTO` (not among the Rust kernel's named error codes).
pub(crate) fn eproto() -> Error {
    Error::from_errno(-(bindings::EPROTO as i32))
}

/// `Shared` from an RTKit cookie.
///
/// # Safety
///
/// `cookie` must be the pointer [`Rtkit::new`] registered.
unsafe fn shared<'a>(cookie: *mut c_void) -> ArcBorrow<'a, Shared> {
    // SAFETY: The cookie is a leaked `Arc<Shared>` (see `Rtkit::new`), live until reboot.
    unsafe { ArcBorrow::from_raw(cookie as *const Shared) }
}

/// RTKit crash callback: latches fail-stop.
unsafe extern "C" fn rtkit_crashed(cookie: *mut c_void, _log: *const c_void, _size: usize) {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    dev_err!(
        shared.dev,
        "GPU firmware crashed; retaining session until reboot\n"
    );
    shared.fail_stop(EIO);
}

/// RTKit message callback.
unsafe extern "C" fn rtkit_recv(cookie: *mut c_void, endpoint: u8, message: u64) {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    // EP 0x20 0x0042... asks for receive-ring draining; nothing else expected.
    if endpoint == hw::ep::FIRMWARE && message == hw::ep::RING_SERVICE {
        if shared.started.load(Ordering::Acquire) {
            Arc::<Shared>::from(shared).kick();
        }
        return;
    }
    dev_err!(
        shared.dev,
        "unexpected firmware EP{:#x} message {:#x}\n",
        endpoint,
        message
    );
    shared.fail_stop(eproto());
}

/// EP 0x20 ring-service request, in the mailbox interrupt: wake only. The drain and the
/// backend's completion check run in the poll work; the shared state outlives the mailbox.
unsafe extern "C" fn rtkit_recv_early(cookie: *mut c_void, endpoint: u8, message: u64) -> bool {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    if !hw::tuning::EARLY_WAKE
        || endpoint != hw::ep::FIRMWARE
        || message != hw::ep::RING_SERVICE
        || !shared.started.load(Ordering::Acquire)
    {
        return false;
    }
    Arc::<Shared>::from(shared).kick();
    true
}

/// Only the inherited crash buffer inside this session's firmware DATA, once.
unsafe extern "C" fn rtkit_shmem_setup(
    cookie: *mut c_void,
    endpoint: u8,
    bfr: *mut bindings::apple_rtkit_shmem,
) -> core::ffi::c_int {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    // SAFETY: RTKit passes a valid buffer descriptor it owns for the duration of the call.
    let bfr = unsafe { &mut *bfr };
    let (iova, size) = (bfr.iova as u64, bfr.size as u64);
    let data = &shared.data;
    let data_size = data.end - data.start;
    if endpoint != 1
        || size != 0x8000
        || iova == 0
        || (iova | size) & 0xfff != 0
        || iova < data.start
        || size > data_size
        || iova - data.start > data_size - size
        || shared
            .rtkit_buffer
            .compare_exchange(0, u64::MAX, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
    {
        dev_err!(
            shared.dev,
            "refusing RTKit buffer {:#x}/{:#x}\n",
            iova,
            size
        );
        shared.fail_stop(EINVAL);
        return EINVAL.to_errno();
    }
    let iomem = glue::ioremap_wc(iova, bfr.size);
    if iomem.is_null() {
        shared.fail_stop(ENOMEM);
        return ENOMEM.to_errno();
    }
    bfr.iomem = iomem;
    bfr.is_mapped = true;
    shared.rtkit_buffer.store(iova, Ordering::Release);
    dev_info!(
        shared.dev,
        "mapped firmware crash storage {:#x}/{:#x}\n",
        iova,
        size
    );
    0
}

/// Retained until reboot: no qualified stop path.
unsafe extern "C" fn rtkit_shmem_destroy(
    _cookie: *mut c_void,
    _bfr: *mut bindings::apple_rtkit_shmem,
) {
}

/// The GPU's RTKit callbacks.
static RTKIT_OPS: bindings::apple_rtkit_ops = bindings::apple_rtkit_ops {
    flags: 0,
    crashed: Some(rtkit_crashed),
    recv_message: Some(rtkit_recv),
    recv_message_early: Some(rtkit_recv_early),
    shmem_setup: None,
    shmem_setup_ep: Some(rtkit_shmem_setup),
    shmem_destroy: Some(rtkit_shmem_destroy),
    syslog_is_debug: None,
    protocol_version: 12,
};

/// The GPU's RTKit client. Never freed: the firmware session lasts until reboot.
///
/// # Invariants
///
/// `rtk` is a live `struct apple_rtkit` whose cookie is a leaked `Arc<Shared>`.
pub(crate) struct Rtkit {
    rtk: *mut bindings::apple_rtkit,
}

// SAFETY: The C RTKit API is callable from any thread; mutating calls take `&mut self`.
unsafe impl Send for Rtkit {}
// SAFETY: Shared calls (`has_endpoint`) only read RTKit state.
unsafe impl Sync for Rtkit {}

impl Rtkit {
    /// Initializes the sole RTKit owner on the default mailbox (retained).
    fn new(dev: &device::Device, shared: &Arc<Shared>) -> Result<Self> {
        let cookie = Arc::into_raw(shared.clone()) as *mut c_void;
        // SAFETY: `dev` is valid, the name is a NUL-terminated string and the ops are static.
        let rtk = from_err_ptr(unsafe {
            bindings::apple_rtkit_init(dev.as_raw(), cookie, core::ptr::null(), 0, &RTKIT_OPS)
        });
        match rtk {
            // INVARIANT: The cookie stays leaked from here on.
            Ok(rtk) => Ok(Self { rtk }),
            Err(e) => {
                // SAFETY: Init failed, so RTKit holds no copy of the cookie from `into_raw`.
                drop(unsafe { Arc::from_raw(cookie as *const Shared) });
                Err(e)
            }
        }
    }

    fn wake(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_wake(self.rtk) })
    }

    fn running(&self) -> bool {
        // SAFETY: `rtk` is retained and valid for the session lifetime.
        unsafe {
            bindings::apple_rtkit_is_running(self.rtk)
                && !bindings::apple_rtkit_is_crashed(self.rtk)
        }
    }

    fn has_endpoint(&self, endpoint: u8) -> bool {
        // SAFETY: `rtk` is live by the type invariant.
        unsafe { bindings::apple_rtkit_has_endpoint(self.rtk, endpoint) }
    }

    fn start_endpoint(&mut self, endpoint: u8) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_start_ep(self.rtk, endpoint) })
    }

    /// Sends a message without waiting.
    pub(crate) fn send(&mut self, endpoint: u8, message: u64) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe {
            bindings::apple_rtkit_send_message(
                self.rtk,
                endpoint,
                message,
                core::ptr::null_mut(),
                false,
            )
        })
    }
}

/// The result of admission: everything probe-time checks established, nothing written.
pub(crate) struct Admitted {
    pub(crate) regs: regs::Regs,
    pub(crate) adt: GpuAdt,
}

/// The running firmware session: RTKit, MMIO and the adopted UAT. (The
/// host window lives in [`Objects`], which maps through it.)
pub(crate) struct Session {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) shared: Arc<Shared>,
    pub(crate) rtkit: Rtkit,
    pub(crate) regs: regs::Regs,
    pub(crate) adt: GpuAdt,
    uat: Arc<mmu::Uat>,
}

impl Session {
    /// The adopted UAT (user VMs are created and bound through it).
    pub(crate) fn uat(&self) -> Arc<mmu::Uat> {
        self.uat.clone()
    }

    /// Live transport and powered-firmware health, plus any previously latched failure.
    pub(crate) fn check(&self) -> Result {
        self.shared.check()?;
        if !self.rtkit.running()
            || self.regs.asc_control()? & regs::ASC_RUN == 0
            || self.regs.pstate()? > 9
        {
            return Err(EIO);
        }
        self.uat.check_adopted_bindings()
    }

    /// Sends a mailbox message.
    pub(crate) fn send(&mut self, endpoint: u8, message: u64) -> Result {
        self.rtkit.send(endpoint, message)
    }

    /// Rings a submit channel's doorbell: EP 0x21 `0x0083 << 48 | channel`.
    pub(crate) fn ring(&mut self, channel: Channel) -> Result {
        self.send(hw::ep::DOORBELL, hw::engine::DOORBELL | channel as u64)
    }
}

/// The firmware at READY.
pub(crate) struct Booted {
    pub(crate) session: Session,
    pub(crate) init: InitData,
    pub(crate) objects: Objects,
    pub(crate) channels: Channels,
    pub(crate) events: Events,
    pub(crate) id: regs::Identity,
    pub(crate) max_frequency_khz: u32,
}

/// A firmware session that did not complete: retained until reboot, its receive rings still
/// drained so the firmware never waits on the host.
pub(crate) struct Orphan {
    pub(crate) session: Session,
    /// Retained only: the firmware may still use them.
    pub(crate) _objects: Objects,
    pub(crate) channels: Option<Channels>,
    pub(crate) _init: Option<InitData>,
    pub(crate) events: Events,
}

impl Orphan {
    /// Drains the receive rings, if they exist.
    pub(crate) fn drain(&mut self) -> Result {
        if let Some(channels) = &self.channels {
            channels.drain_rings(&self.session.dev, &mut self.events)?;
        }
        Ok(())
    }
}

/// Probe-time admission.
pub(crate) fn admit(
    pdev: &platform::Device<Core<'_>>,
    _dev: &AsahiDrmDevice,
    cfg: &'static Config,
    res: shared_regs::G16Resources,
) -> Result<Admitted> {
    let dev = pdev.as_ref();
    if !glue::board() {
        return Err(ENODEV);
    }
    // Retain the PMP supplier (and its AGX bridge) before any vote.
    // SAFETY: `dev` is the bound GPU device.
    let ret = unsafe { bindings::apple_pmp_j713_gpu_prepare(dev.as_raw()) };
    if ret != 0 {
        let err = Error::from_errno(ret);
        if err != EPROBE_DEFER {
            dev_err!(dev, "PMP supplier not ready: {:?}\n", err);
        }
        return Err(err);
    }
    let adt =
        adt::read(dev).inspect_err(|e| dev_err!(dev, "ADT GPU regions not admitted: {:?}\n", e))?;
    crate::fwid::identify(dev, adt.text.clone(), adt.data.end - adt.data.start, cfg.firmware)
        .inspect_err(|e| dev_err!(dev, "loaded GFX TEXT identity mismatch: {:?}\n", e))?;
    dev_info!(dev, "M4: firmware identity accepted\n");
    // Firmware objects are cleaned to the point of coherency by their DMA mapping: that needs
    // a device that is not DMA-coherent, as the synthesized node is.
    if glue::dma_unsupported(dev) {
        dev_err!(dev, "G16: unsupported coherent, IOMMU or custom DMA path\n");
        return Err(EINVAL);
    }
    let regs = regs::Regs::new(pdev, res)?;
    // Once power admission starts, even a rejected cold check must not detach the GFX
    // domain under an inherited firmware owner. Keep the module and platform binding.
    glue::retain_module();
    glue::hold_power(dev)?;
    let (power_offset, power_state) = glue::power_domain_state(dev)?;
    if power_offset != 0x250 || (power_state >> 4) & 0xf != 0xf {
        dev_err!(
            dev,
            "M4 GFX domain not admitted: register{:#x} state{:#x}\n",
            power_offset,
            power_state
        );
        return Err(Error::from_errno(-(bindings::EHOSTDOWN as i32)));
    }
    dev_info!(dev, "T8132 G16G admitted, ASC={:#x}\n", regs.asc_control()?);
    Ok(Admitted { regs, adt })
}

/// Posts one device control with only the opcode and waits for it.
fn devctl_op(
    session: &mut Session,
    channels: &mut Channels,
    events: &mut Events,
    init: &InitData,
    opcode: u32,
) -> Result {
    channel::devctl_submit(session, channels, events, init, &DevCtl::op(opcode), true)
}

/// Wait for one boot control to retire, requiring stable consumer+completion indices.
fn wait_boot_control(
    session: &Session,
    channels: &Channels,
    events: &mut Events,
    init: &InitData,
    target: u32,
    timeout_ms: i64,
) -> Result {
    let start = Instant::<Monotonic>::now();
    let mut stable = None;
    loop {
        channels.drain_rings(&session.dev, events)?;
        session.check()?;
        let ready = init.readiness()?;
        let read = channels.devctl.read()?;
        let done = channels.devctl.completion()?;
        let write = channels.devctl.write()?;
        if ready > 1
            || read < target - 1
            || read > target
            || done < target - 1
            || done > target
            || write != target
        {
            return Err(eproto());
        }
        if ready == 1 && read == target && done == target {
            let at = stable.get_or_insert_with(Instant::<Monotonic>::now);
            if at.elapsed() >= Delta::from_millis(50) {
                return Ok(());
            }
        } else {
            stable = None;
        }
        if start.elapsed() > Delta::from_millis(timeout_ms) {
            dev_err!(
                session.dev,
                "M4 boot control target{} ready{} indices{}/{}/{}\n",
                target,
                ready,
                read,
                done,
                write
            );
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
}

/// Wake the sole firmware instance, publish its loader table, then qualify NOP and policy.
fn run(
    session: &mut Session,
    events: &mut Events,
    built: &mut (InitData, Channels, u32),
) -> Result {
    let dev = session.dev.clone();
    let (init, channels, _) = built;
    session.rtkit.wake()?;
    let start = Instant::<Monotonic>::now();
    while matches!(
        session.shared.rtkit_buffer.load(Ordering::Acquire),
        0 | u64::MAX
    ) {
        session.check()?;
        if start.elapsed() > Delta::from_millis(1000) {
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
    session.check()?;
    if !session.rtkit.running()
        || !session.rtkit.has_endpoint(1)
        || !session.rtkit.has_endpoint(hw::ep::FIRMWARE)
        || !session.rtkit.has_endpoint(hw::ep::DOORBELL)
    {
        return Err(eproto());
    }
    session.rtkit.start_endpoint(hw::ep::FIRMWARE)?;
    session.rtkit.start_endpoint(hw::ep::DOORBELL)?;
    for _ in 0..10 {
        fsleep(Delta::from_millis(10));
        channels.drain_rings(&dev, events)?;
        session.check()?;
    }
    session.regs.set_generation(0)?;
    channels.stage_boot_nop(session)?;
    // This table is the only host write to the loaded DATA segment. It is empty on cold entry.
    let table = init.loader_bytes();
    let loader = regs::Mapping::write_combined(session.adt.data.start + 0x1332f0, table.len())?;
    for _ in 0..2 {
        for offset in (0..table.len()).step_by(4) {
            if loader.io().try_read32(offset)? != 0 {
                return Err(EBUSY);
            }
        }
        regs::rmb();
    }
    for (i, word) in table.chunks_exact(4).enumerate() {
        loader
            .io()
            .try_write32(u32::from_le_bytes(word.try_into().unwrap()), i * 4)?;
    }
    regs::wmb();
    let root = init.root_va()?;
    session.send(
        hw::ep::FIRMWARE,
        hw::ep::INITDATA | (root & hw::ep::INITDATA_ADDR_MASK),
    )?;
    session.send(hw::ep::DOORBELL, 0x0083000000000011)?;
    wait_boot_control(session, channels, events, init, 1, 5000)?;
    devctl_op(session, channels, events, init, devctl::IMPORT_GLOBALS)?;
    wait_boot_control(session, channels, events, init, 2, 2000)?;
    let scheduler = init.control.as_ref().ok_or(EINVAL)?;
    if scheduler.r32(control::SCHEDULER)? != 0 {
        return Err(EBUSY);
    }
    scheduler.w32(control::SCHEDULER, 1)?;
    regs::wmb();
    devctl_op(session, channels, events, init, devctl::START)?;
    wait_boot_control(session, channels, events, init, 3, 2000)?;
    session.send(hw::ep::DOORBELL, 0x0083000000000010)?;
    for _ in 0..10 {
        fsleep(Delta::from_millis(10));
        channels.drain_rings(&dev, events)?;
        session.check()?;
        if init.readiness()? != 1 {
            return Err(eproto());
        }
    }
    if channels.devctl.read()? != 3
        || channels.devctl.completion()? != 3
        || channels.devctl.write()? != 3
        || scheduler.r32(control::SCHEDULER)? != 0
    {
        return Err(EBUSY);
    }
    dev_info!(
        dev,
        "M4 firmware accepted owned initdata root{:#x}, NOP and policy; READY\n",
        root
    );
    Ok(())
}

/// Brings the firmware to READY.
pub(crate) fn start(
    admitted: Admitted,
    dev: &AsahiDrmDevice,
    cfg: &'static Config,
    shared: &Arc<Shared>,
) -> Result<Booted> {
    let Admitted { regs, adt } = admitted;
    let d: ARef<device::Device> = shared.dev.clone();
    // Cold ownership must hold before the PMP vote as well as immediately before RUN.
    if regs.asc_control()? & regs::ASC_RUN != 0 {
        return Err(EBUSY);
    }
    // SAFETY: `d` is the bound GPU device.
    let ret = unsafe { bindings::apple_pmp_j713_gpu_get(d.as_raw()) };
    if ret != 0 {
        dev_err!(d, "PMP AGX vote failed: {}\n", ret);
        return Err(Error::from_errno(ret));
    }
    let id = regs.identity()?;
    dev_info!(
        d,
        "M4 G16G: power acknowledged, version={:#x} counts={:#x} core-mask={:#x}\n",
        id.version,
        id.counts,
        id.cores
    );
    if !cfg
        .hw_ids
        .iter()
        .any(|h| h.version == id.version && h.counts == id.counts)
    {
        dev_err!(
            d,
            "unsupported G16G hardware: version {:#x} counts {:#x}\n",
            id.version,
            id.counts
        );
        return Err(eproto());
    }
    // M4 has one cluster with up to ten enabled cores.
    if id.core_masks[0] == 0 || id.cores >> hw::CLUSTER_BITS != 0 {
        dev_err!(d, "implausible G16G core mask {:#x}\n", id.cores);
        return Err(eproto());
    }
    dev_info!(
        d,
        "M4 G16G: {} cores enabled\n",
        id.core_masks[0].count_ones()
    );
    if regs.pstate()? > 9 {
        return Err(eproto());
    }
    // Cold only: ASC RUN clear and an all-zero TTBAT before ownership.
    let control = regs.asc_control()?;
    if control & regs::ASC_RUN != 0 {
        return Err(EBUSY);
    }
    let window = uat::Window::new(&adt)?;
    if !window.ttbat_is_clear()? {
        return Err(EBUSY);
    }
    // Adopt the UAT and prove the leaf words before anything firmware-visible is written: no
    // context names the shadow window or the lower root yet.
    let uat = uat::adopt(dev, cfg, &adt)?;
    let mut objects = Objects::new(dev, uat.clone(), window)?;
    if let Err(e) = objects.self_check() {
        // Mapped with cached firmware classes: never unmapped.
        core::mem::forget(objects);
        return Err(e);
    }

    // From here on the firmware is touched: nothing it can reach is freed.
    glue::retain_module();
    let rtkit = match Rtkit::new(&d, shared) {
        Ok(rtkit) => rtkit,
        Err(e) => {
            core::mem::forget(objects);
            return Err(e);
        }
    };
    let mut session = Session {
        dev: d.clone(),
        shared: shared.clone(),
        rtkit,
        regs,
        adt,
        uat,
    };
    let mut events = Events::new();
    let mut built = None;
    let result = (|| {
        events.reserve()?;
        // Every host object and every context-0 alias is prepared before RUN is set.
        built = Some(initdata::build(&d, &session.adt, &mut objects)?);
        objects.window.lock().bootstrap(&d, &session.uat)?;
        uat::tlbi_all();
        if session.regs.asc_control()? & regs::ASC_RUN != 0 {
            return Err(EBUSY);
        }
        shared.started.store(true, Ordering::Release);
        session.regs.set_asc_control(control | regs::ASC_RUN)?;
        regs::mb();
        if session.regs.asc_control()? & regs::ASC_RUN == 0 {
            return Err(EIO);
        }
        run(&mut session, &mut events, built.as_mut().ok_or(EINVAL)?)
    })();
    if let Err(e) = result {
        dev_err!(
            d,
            "G16 start failed: {:?}; session retained until reboot\n",
            e
        );
        let (init, channels) = match built {
            Some((init, channels, _)) => (Some(init), Some(channels)),
            None => (None, None),
        };
        shared.orphan(Orphan {
            session,
            _objects: objects,
            channels,
            _init: init,
            events,
        });
        return Err(e);
    }
    let (init, channels, max_frequency_khz) = built.ok_or(EINVAL)?;
    Ok(Booted {
        session,
        init,
        objects,
        channels,
        events,
        id,
        max_frequency_khz,
    })
}
