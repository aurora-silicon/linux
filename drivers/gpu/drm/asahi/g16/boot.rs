// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4/J713 cold admission and the firmware start sequence (25G83; 26B123 once its unknowns are
//! known).
//!
//! The Rust owner validates firmware identity and all inherited regions before publication,
//! holds the PMP AGX vote (25G83; 26B123 sends none), prepares owned objects and publishes
//! context0 while ASC RUN is clear.
//! It negotiates RTKit protocol12, accepts only the inherited EP1 crash buffer, then submits
//! initdata plus the consume-only NOP and policy (re-import Globals, start; 0x1a, 0x34 and
//! 0x0a on 25G83, 0x19, 0x35 and 0x0a on 26B123). Control+0x14 and exact retired indices
//! qualify READY; no M3 firmware constants or device-control sequence are reused.
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
use super::fw::channels::DevCtl;
use super::hw::{self, LoaderTable};
use super::initdata::{self, InitData};
use super::object::{Class, Objects, Placement, Request};
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
        fn asahi_g16_display_prepare(
            dev: *mut core::ffi::c_void,
            display_compat: *const core::ffi::c_char,
        ) -> core::ffi::c_int;
        fn asahi_g16_timebase_hz() -> u32;
        fn asahi_g16_hold_power(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
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

    /// A qualified T8132 board (J713, J773g).
    pub(crate) fn board() -> bool {
        // SAFETY: Reads the machine compatible strings only.
        unsafe { asahi_g16_board() }
    }

    /// Links the GPU to the published native display, or defers until it is.
    pub(crate) fn display_prepare(dev: &device::Device, display_compatible: &[u8]) -> Result {
        let Ok(cstr) = core::ffi::CStr::from_bytes_with_nul(display_compatible) else {
            return Err(EINVAL);
        };
        // SAFETY: `dev` is the bound GPU device; `cstr` is NUL-terminated.
        kernel::error::to_result(unsafe {
            asahi_g16_display_prepare(dev.as_raw().cast(), cstr.as_ptr().cast())
        })
    }

    /// The system counter frequency in Hz, as the boot loader programmed it.
    pub(crate) fn timebase_hz() -> u32 {
        // SAFETY: Reads the system counter frequency register only.
        unsafe { asahi_g16_timebase_hz() }
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

    /// Pins this module until reboot, once (`try_module_get(THIS_MODULE)`, see `retain.rs`).
    /// ENODEV, with nothing pinned or marked, once the module's removal has begun.
    pub(crate) fn retain_module() -> Result {
        crate::retain::retain_module(&RETAINED)
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
///
/// # Safety
///
/// `cookie` must be the live Shared cookie registered with these RTKit operations.
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
///
/// # Safety
///
/// `cookie` must be the live Shared cookie registered with these RTKit operations.
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
    // What a firmware says while it comes back from a sleep isn't known yet.
    if !shared.sleeping.load(Ordering::Acquire) {
        shared.fail_stop(eproto());
    }
}

/// EP 0x20 ring-service request, in the mailbox interrupt: wake only. The drain and the
/// backend's completion check run in the poll work; the shared state outlives the mailbox.
///
/// # Safety
///
/// `cookie` must be the live Shared cookie registered with these RTKit operations.
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
///
/// # Safety
///
/// `cookie` must be the registered live Shared cookie. `bfr` must point to the
/// writable RTKit buffer descriptor, exclusively borrowed for this callback.
unsafe extern "C" fn rtkit_shmem_setup(
    cookie: *mut c_void,
    endpoint: u8,
    bfr: *mut bindings::apple_rtkit_shmem,
) -> core::ffi::c_int {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    // SAFETY: RTKit passes a valid buffer descriptor it owns for the duration of the call.
    let bfr = unsafe { &mut *bfr };
    let (iova, size) = (bfr.iova, bfr.size as u64);
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
///
/// # Safety
///
/// This no-op callback accesses neither pointer and releases no storage.
unsafe extern "C" fn rtkit_shmem_destroy(
    _cookie: *mut c_void,
    _bfr: *mut bindings::apple_rtkit_shmem,
) {
}

/// The GPU's RTKit callbacks.
const RTKIT_OPS_BASE: bindings::apple_rtkit_ops = bindings::apple_rtkit_ops {
    flags: 0,
    crashed: Some(rtkit_crashed),
    recv_message: Some(rtkit_recv),
    recv_message_timed: None,
    recv_message_early: Some(rtkit_recv_early),
    shmem_setup: None,
    shmem_setup_ep: Some(rtkit_shmem_setup),
    shmem_destroy: Some(rtkit_shmem_destroy),
    syslog_is_debug: None,
    protocol_version: 12,
};

/// 25G83: the firmware's crash buffer is mapped without a reply.
static RTKIT_OPS: bindings::apple_rtkit_ops = RTKIT_OPS_BASE;

/// 26B123: the crash-log endpoint starts while the endpoint map is handled, and the firmware's
/// request for its own crash buffer is sent back unchanged.
static RTKIT_OPS_27_2: bindings::apple_rtkit_ops = bindings::apple_rtkit_ops {
    flags: bindings::APPLE_RTKIT_COPROC_PLACES_BUFFERS,
    ..RTKIT_OPS_BASE
};

/// The host log buffer message the 26B123 start sends from RTKit's `before_ap` hook.
struct LogBuffer {
    rtk: *mut bindings::apple_rtkit,
    shared: *const Shared,
    message: u64,
    result: core::ffi::c_int,
}

/// RTKit `before_ap` hook of the 26B123 start: once the firmware's own crash buffer has been
/// set up (and its request sent back), hands it the host log buffer before AP power-on.
///
/// # Safety
///
/// `data` must point to a live [`LogBuffer`] exclusively borrowed for this call, whose `rtk`
/// is the RTKit instance running the hook and whose `shared` outlives it.
unsafe extern "C" fn publish_log_buffer(data: *mut c_void) {
    // SAFETY: By the function's contract.
    let log = unsafe { &mut *data.cast::<LogBuffer>() };
    // SAFETY: By the function's contract.
    let shared = unsafe { &*log.shared };
    let start = Instant::<Monotonic>::now();
    while matches!(shared.rtkit_buffer.load(Ordering::Acquire), 0 | u64::MAX) {
        if start.elapsed() > Delta::from_millis(1000) {
            log.result = ETIMEDOUT.to_errno();
            return;
        }
        fsleep(Delta::from_millis(1));
    }
    // SAFETY: `log.rtk` is the live RTKit instance running this hook.
    log.result = unsafe {
        bindings::apple_rtkit_send_message(
            log.rtk,
            hw::start_27_2::EP_CRASHLOG,
            log.message,
            core::ptr::null_mut(),
            false,
        )
    };
}

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
    fn new(
        dev: &device::Device,
        shared: &Arc<Shared>,
        ops: &'static bindings::apple_rtkit_ops,
    ) -> Result<Self> {
        let cookie = Arc::into_raw(shared.clone()) as *mut c_void;
        // SAFETY: `dev` is valid, the name is a NUL-terminated string and the ops are static.
        let rtk = from_err_ptr(unsafe {
            bindings::apple_rtkit_init(dev.as_raw(), cookie, core::ptr::null(), 0, ops)
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

    pub(crate) fn wake(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_wake(self.rtk) })
    }

    /// The C handle, for the stop at a sleep's commit.
    pub(crate) fn as_raw(&self) -> *mut bindings::apple_rtkit {
        self.rtk
    }

    /// Prepares for HELLO from firmware restarting warm, which keeps its buffers and rings.
    pub(crate) fn warm_restart(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_warm_restart(self.rtk) })
    }

    /// Waits for the firmware to report in without an IOP power request, runs `before_ap`, then
    /// sets the AP on.
    ///
    /// # Safety
    ///
    /// `before_ap` must be safe to call with `data`, from this thread, during this call.
    unsafe fn boot_with(
        &mut self,
        before_ap: unsafe extern "C" fn(*mut c_void),
        data: *mut c_void,
    ) -> Result {
        // SAFETY: `rtk` is live by the type invariant; the caller vouches for the hook.
        to_result(unsafe { bindings::apple_rtkit_boot_with(self.rtk, Some(before_ap), data) })
    }

    /// Waits for the firmware to report in, and sets the AP on.
    pub(crate) fn boot(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_boot(self.rtk) })
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
    pub(crate) adt: KBox<GpuAdt>,
}

/// The running firmware session: RTKit, MMIO and the adopted UAT. (The
/// host window lives in [`Objects`], which maps through it.)
pub(crate) struct Session {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) shared: Arc<Shared>,
    pub(crate) rtkit: Rtkit,
    pub(crate) regs: regs::Regs,
    pub(crate) adt: KBox<GpuAdt>,
    uat: Arc<mmu::Uat>,
    /// The PPL handoff page, mapped here only to read the firmware's powered flag
    /// ([`hw::FwLayout::powered`]); `None` when the firmware keeps the GPU powered.
    handoff: Option<regs::Mapping>,
    /// True once cold boot (`START`) has completed and the firmware may idle-power-off the GPU.
    booted: bool,
}

impl Session {
    /// The adopted UAT (user VMs are created and bound through it).
    pub(crate) fn uat(&self) -> Arc<mmu::Uat> {
        self.uat.clone()
    }

    /// Live transport and powered-firmware health, plus any previously latched failure.
    pub(crate) fn check(&self) -> Result {
        self.transport()?;
        if self.regs.asc_control()? & regs::ASC_RUN == 0 {
            return Err(EIO);
        }
        // Only read SGX pstate while the GPU is powered under the PPL handoff lock.
        if let Some(pstate) = self.while_powered(regs::Regs::pstate)? {
            if pstate > self.regs.top() {
                return Err(EIO);
            }
        }
        self.uat.check_adopted_bindings()
    }

    /// The transport alone: a latched failure, or RTKit stopped or crashed. No register or
    /// UAT read, so the retirement loop can ask between its receipts.
    pub(crate) fn transport(&self) -> Result {
        self.shared.check()?;
        if !self.rtkit.running() {
            return Err(EIO);
        }
        Ok(())
    }

    /// Runs `read` with the GPU held powered, or returns `None` while it is powered down. A
    /// firmware that powers the GPU down while idle changes GPU power only while it holds the
    /// PPL handoff lock, and records the result on the handoff page before releasing it. The
    /// AP side of the lock is held here across that record and `read`, so the GPU cannot go
    /// down between them. Reading a GPU register while it is down takes an SError.
    fn while_powered<T>(&self, read: impl FnOnce(&regs::Regs) -> Result<T>) -> Result<Option<T>> {
        let (Some(page), Some(offset)) = (&self.handoff, self.adt.layout.powered) else {
            return read(&self.regs).map(Some);
        };
        let _held = self.uat.lock_m4_handoff()?;
        if page.io().try_read8(offset)? == 0 {
            return Ok(None);
        }
        read(&self.regs).map(Some)
    }

    /// The engine receipts of a running firmware, or `None` while the GPU is powered down.
    pub(crate) fn engine_state(&self) -> Result<Option<regs::EngineState>> {
        self.while_powered(regs::Regs::engine_state)
    }

    /// All supported engine fault banks while firmware is halted for recovery,
    /// or `None` while the GPU is powered down.
    pub(crate) fn fault_scan(&self) -> Result<Option<regs::FaultScan>> {
        self.while_powered(regs::Regs::fault_scan)
    }

    /// Writes the host-owned device-control generation register (`sgx + 0xd60000`). Once cold
    /// boot has completed and the firmware may idle-power-off the GPU, the access is guarded by
    /// [`Self::while_powered`], returning `EAGAIN` if the GPU is currently powered down.
    pub(crate) fn set_generation(&self, value: u32) -> Result {
        // On G15S this word belongs to submit channel 0. The device-control
        // producer is supplied by its separate InitData pointer.
        if self.adt.layout.initdata.tx == super::fw::initdata::TxLayout::RingHeader {
            return Ok(());
        }
        if !self.booted {
            return self.regs.set_generation(value);
        }
        match self.while_powered(|regs| regs.set_generation(value))? {
            Some(()) => Ok(()),
            None => Err(EAGAIN),
        }
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
#[inline(never)]
pub(crate) fn admit(
    pdev: &platform::Device<Core<'_>>,
    _dev: &AsahiDrmDevice,
    cfg: &'static Config,
    res: shared_regs::G16Resources,
) -> Result<KBox<Admitted>> {
    let dev = pdev.as_ref();
    // The board check guards the PMP AGX vote, which only the 25G83 start sends. A firmware
    // whose handoff names the start without the vote (26B123) rests on the firmware identity
    // checks below instead.
    let votes = !matches!(
        adt::layout(dev, cfg),
        Some(layout) if layout.start == hw::Start::MacOs27_2
    );
    if votes && !glue::board() {
        return Err(ENODEV);
    }
    if cfg.prepare_pmp {
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
    }
    glue::display_prepare(dev, cfg.display_compatible).inspect_err(|e| {
        if *e != EPROBE_DEFER {
            dev_err!(dev, "native display not admitted: {:?}\n", e);
        }
    })?;
    let adt = adt::read(dev, cfg)
        .inspect_err(|e| dev_err!(dev, "ADT GPU regions not admitted: {:?}\n", e))?;
    crate::fwid::identify(
        dev,
        adt.text.clone(),
        adt.data.end - adt.data.start,
        cfg.firmware,
    )
    .inspect_err(|e| dev_err!(dev, "loaded GFX TEXT identity mismatch: {:?}\n", e))?;
    dev_info!(
        dev,
        "M4: firmware identity accepted ({} layout)\n",
        adt.layout.name
    );
    // Refuse a firmware whose start needs something not known yet before anything is held.
    let mut unknown = false;
    for what in adt.layout.unknown() {
        dev_err!(
            dev,
            "G16: the {} firmware cannot be started yet; not known: {}\n",
            adt.layout.name,
            what
        );
        unknown = true;
    }
    if unknown {
        return Err(ENOTSUPP);
    }
    // Firmware objects are cleaned to the point of coherency by their DMA mapping: that needs
    // a device that is not DMA-coherent, as the synthesized node is.
    if glue::dma_unsupported(dev) {
        dev_err!(dev, "G16: unsupported coherent, IOMMU or custom DMA path\n");
        return Err(EINVAL);
    }
    let regs = regs::Regs::new(pdev, res, cfg)?;
    // Once power admission starts, even a rejected cold check must not detach the GFX
    // domain under an inherited firmware owner. Keep the module and platform binding; once
    // the module's removal has begun nothing is held.
    glue::retain_module()?;
    glue::hold_power(dev)?;
    let (power_offset, power_state) = glue::power_domain_state(dev)?;
    if power_offset != cfg.power_offset || (power_state >> 4) & 0xf != 0xf {
        dev_err!(
            dev,
            "GFX domain not admitted: register{:#x} state{:#x}\n",
            power_offset,
            power_state
        );
        return Err(Error::from_errno(-(bindings::EHOSTDOWN as i32)));
    }
    dev_info!(
        dev,
        "T{:x} GPU admitted, ASC={:#x}\n",
        cfg.chip_id,
        regs.asc_control()?
    );
    Ok(KBox::init(init!(Admitted { regs, adt }), GFP_KERNEL)?)
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
            channels.log_stop(session, init, "unexpected boot-control indices");
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
            channels.log_stop(session, init, "boot control not retired in time");
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
}

/// Writes the register-map records into the firmware's table in loaded DATA, which must be
/// empty on cold entry. This is the only host write to the loaded DATA segment.
fn publish_loader_table(session: &Session, init: &InitData, offset: u64) -> Result {
    let table = init.loader_bytes();
    let loader = regs::Mapping::write_combined(session.adt.data.start + offset, table.len())?;
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
    Ok(())
}

/// Wake the sole firmware instance, publish its loader table, then qualify NOP and policy.
#[inline(never)]
fn run(
    session: &mut Session,
    events: &mut Events,
    built: &mut (InitData, Channels, u32),
    cfg: &Config,
    log_buffer: Option<(u64, usize)>,
) -> Result {
    let dev = session.dev.clone();
    let (init, channels, _) = built;
    let kind = session.adt.layout.start;
    match kind {
        hw::Start::Wake25G83 => session.rtkit.wake()?,
        hw::Start::MacOs27_2 => {
            use hw::start_27_2 as s;
            let (va, log_size) = log_buffer.ok_or(EINVAL)?;
            let mut log = LogBuffer {
                rtk: session.rtkit.as_raw(),
                shared: &*session.shared,
                message: s::MSG_LOG_BUFFER
                    | (((log_size >> 12) as u64) << s::MSG_LOG_BUFFER_PAGES_SHIFT)
                    | (va & s::MSG_LOG_BUFFER_ADDR_MASK),
                result: 0,
            };
            // SAFETY: `log` lives on this stack frame across the call and nothing else
            // borrows it; the hook runs on this thread before `boot_with` returns.
            unsafe {
                session
                    .rtkit
                    .boot_with(publish_log_buffer, (&raw mut log).cast())?
            };
            to_result(log.result)?;
        }
    }
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
    if kind == hw::Start::MacOs27_2 {
        if let Some(probe) = session.regs.publish_host_block(cfg)? {
            dev_info!(
                dev,
                "G16: 27.2 host block published, scratch {:#x}\n",
                probe
            );
        }
    }
    session.rtkit.start_endpoint(hw::ep::FIRMWARE)?;
    session.rtkit.start_endpoint(hw::ep::DOORBELL)?;
    let devctl = init.abi.devctl();
    if kind == hw::Start::Wake25G83 {
        for _ in 0..10 {
            fsleep(Delta::from_millis(10));
            channels.drain_rings(&dev, events)?;
            session.check()?;
        }
        session.regs.set_generation(0)?;
    }
    channels.stage_boot_nop(session, devctl.nop)?;
    match session.adt.layout.loader_table {
        Some(LoaderTable::Host(offset)) => publish_loader_table(session, init, offset)?,
        // The firmware copies the register map from HW data while it parses InitData.
        Some(LoaderTable::Firmware) => {}
        // Admission refuses such a layout; never guess here.
        None => return Err(ENOTSUPP),
    }
    let root = init.root_va()?;
    session.send(
        hw::ep::FIRMWARE,
        hw::ep::INITDATA | (root & hw::ep::INITDATA_ADDR_MASK),
    )?;
    let g15s_channels = init.layout.tx == super::fw::initdata::TxLayout::RingHeader;
    let init_kick = if g15s_channels {
        hw::engine::DEVCTL_DOORBELL
    } else {
        hw::engine::DEVCTL_INIT_DOORBELL
    };
    session.send(hw::ep::DOORBELL, init_kick)?;
    wait_boot_control(session, channels, events, init, 1, 5000)?;
    let base = 1;
    let scheduler = init.control.as_ref().ok_or(EINVAL)?;
    let scheduler_word = init.firmware_data().scheduler;
    devctl_op(session, channels, events, init, devctl.import_globals)?;
    wait_boot_control(session, channels, events, init, base + 1, 5000)?;
    let pending = scheduler.r32(scheduler_word)?;
    if kind == hw::Start::MacOs27_2 {
        // The InitData request (1) can still be pending here on 26B123: the firmware consumes it
        // when it powers the GPU up or recovers, and the boot records need not power it up. Post
        // START with it pending; never stack a second request.
        if pending > 1 {
            channels.log_stop(
                session,
                init,
                "scheduler word unexpected after IMPORT_GLOBALS",
            );
            return Err(EBUSY);
        }
        if pending == 0 {
            scheduler.w32(scheduler_word, 1)?;
        }
    } else {
        if pending != 0 {
            channels.log_stop(session, init, "scheduler word set after IMPORT_GLOBALS");
            return Err(EBUSY);
        }
        scheduler.w32(scheduler_word, 1)?;
    }
    regs::wmb();
    devctl_op(session, channels, events, init, devctl.start)?;
    wait_boot_control(session, channels, events, init, base + 2, 2000)?;
    let receive_kick = if g15s_channels {
        hw::engine::G15S_RECEIVE_DOORBELL
    } else {
        hw::engine::RECEIVE_DOORBELL
    };
    session.send(hw::ep::DOORBELL, receive_kick)?;
    for _ in 0..10 {
        fsleep(Delta::from_millis(10));
        channels.drain_rings(&dev, events)?;
        session.check()?;
        if init.readiness()? != 1 {
            return Err(eproto());
        }
    }
    if channels.devctl.read()? != base + 2
        || channels.devctl.completion()? != base + 2
        || channels.devctl.write()? != base + 2
        || (kind == hw::Start::Wake25G83 && scheduler.r32(scheduler_word)? != 0)
    {
        channels.log_stop(session, init, "boot controls not settled after START");
        return Err(EBUSY);
    }
    dev_info!(
        dev,
        "M4 firmware accepted owned initdata root{:#x}, NOP and policy; READY (scheduler {})\n",
        root,
        scheduler.r32(scheduler_word)?
    );
    Ok(())
}

/// Retains a failed firmware start out-of-line so [`start`] never reserves an [`Orphan`] slot on
/// its normal stack frame.
#[cold]
#[inline(never)]
fn retain_start_failure(
    shared: &Shared,
    session: Session,
    objects: Objects,
    built: Option<(InitData, Channels, u32)>,
    events: Events,
) {
    let (init, channels) = match built {
        Some((init, channels, _)) => (Some(init), Some(channels)),
        None => (None, None),
    };
    match KBox::<Orphan>::new_uninit(GFP_KERNEL) {
        Ok(slot) => {
            let orphan = slot
                .write_init(init!(Orphan {
                    session,
                    _objects: objects,
                    channels,
                    _init: init,
                    events,
                }))
                .unwrap_or_else(|e| match e {});
            shared.retain_failed(super::Retained::Orphan(orphan));
        }
        Err(_) => {
            dev_err!(
                shared.dev.as_ref(),
                "G16: cannot keep the failed session; leaked\n"
            );
            // Without memory for the box, leak the firmware-reachable parts.
            core::mem::forget((session, objects, channels, init, events));
        }
    }
}

/// Brings the firmware to READY.
#[inline(never)]
pub(crate) fn start(
    admitted: KBox<Admitted>,
    dev: &AsahiDrmDevice,
    cfg: &'static Config,
    shared: &Arc<Shared>,
) -> Result<KBox<Booted>> {
    let Admitted { regs, adt } = KBox::into_inner(admitted);
    let d: ARef<device::Device> = shared.dev.clone();
    // Cold ownership must hold before the PMP vote as well as immediately before RUN.
    if regs.asc_control()? & regs::ASC_RUN != 0 {
        return Err(EBUSY);
    }
    let kind = adt.layout.start;
    if kind == hw::Start::MacOs27_2 {
        // 26B123 sends no PMP AGX vote: the firmware acknowledges its own IOP power-on once
        // GFXCAXI2AF is enabled and RUN is raised.
        dev_info!(d, "G16: 26B123: starting without PMP AGX vote\n");
        regs.enable_axi2af()?;
    } else {
        // SAFETY: `d` is the bound GPU device.
        let ret = unsafe { bindings::apple_pmp_j713_gpu_get(d.as_raw()) };
        if ret != 0 {
            dev_err!(d, "PMP AGX vote failed: {}\n", ret);
            return Err(Error::from_errno(ret));
        }
    }
    let id = regs.identity()?;
    dev_info!(
        d,
        "T{:x}: power acknowledged, version={:#x} counts={:#x} core-mask={:#x}\n",
        cfg.chip_id,
        id.version,
        id.counts,
        id.cores
    );
    if !cfg.hw_ids.iter().any(|h| h.matches(id.version, id.counts)) {
        dev_err!(
            d,
            "unsupported GPU hardware: version {:#x} counts {:#x}\n",
            id.version,
            id.counts
        );
        return Err(eproto());
    }
    if id.cores >> cfg.core_slots() != 0 || id.core_masks[..cfg.num_clusters as usize].contains(&0)
    {
        dev_err!(d, "implausible GPU core mask {:#x}\n", id.cores);
        return Err(eproto());
    }
    dev_info!(
        d,
        "T{:x}: {} cores enabled\n",
        cfg.chip_id,
        id.cores.count_ones()
    );
    if regs.pstate()? > regs.top() {
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
    let handoff = match adt.layout.powered {
        Some(_) => Some(regs::Mapping::write_combined(
            adt.handoff.start,
            hw::HANDOFF_SIZE as usize,
        )?),
        None => None,
    };
    // Adopt the UAT and prove the leaf words before anything firmware-visible is written: no
    // context names the shadow window or the lower root yet.
    let uat = uat::adopt(dev, cfg, &adt)?;
    let mut objects = Objects::new(dev, uat.clone(), window)?;
    if let Err(e) = objects.self_check() {
        // Mapped with cached firmware classes: never unmapped.
        core::mem::forget(objects);
        return Err(e);
    }

    // From here on the firmware is touched: nothing it can reach is freed. Admission pinned
    // the module, so this only confirms it.
    if let Err(e) = glue::retain_module() {
        core::mem::forget(objects);
        return Err(e);
    }
    let ops = match kind {
        hw::Start::Wake25G83 => &RTKIT_OPS,
        hw::Start::MacOs27_2 => &RTKIT_OPS_27_2,
    };
    let rtkit = match Rtkit::new(&d, shared, ops) {
        Ok(rtkit) => rtkit,
        Err(e) => {
            core::mem::forget(objects);
            return Err(e);
        }
    };
    let booted_slot = match KBox::<Booted>::new_uninit(GFP_KERNEL) {
        Ok(slot) => slot,
        Err(e) => {
            core::mem::forget(objects);
            return Err(e.into());
        }
    };
    let mut session = Session {
        dev: d.clone(),
        shared: shared.clone(),
        rtkit,
        regs,
        adt,
        uat,
        handoff,
        booted: false,
    };
    let mut events = Events::new();
    let mut built = None;
    let result = (|| {
        events.reserve()?;
        // Every host object and every context-0 alias is prepared before RUN is set.
        built = Some(initdata::build(
            &d,
            &session.adt,
            cfg,
            &session.regs,
            id.cores.count_ones(),
            &mut objects,
        )?);
        if let Some((init, _, _)) = built.as_ref() {
            // The running firmware may report any state of the board's table, up to S10 on J773g.
            session.regs.set_table(init.rows);
        }
        let log_buffer = match kind {
            hw::Start::MacOs27_2 => {
                let log_size = session.adt.performance.coredump_copy_size.ok_or(EINVAL)? as usize;
                let va = objects
                    .alloc(Request {
                        size: log_size,
                        class: Class::Fw,
                        placement: Placement::Anywhere,
                        offset: 0,
                        client: None,
                        lower: None,
                        cpu: true,
                    })?
                    .va;
                Some((va, log_size))
            }
            hw::Start::Wake25G83 => None,
        };
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
        objects.window.lock().await_firmware_magic(&d)?;
        run(
            &mut session,
            &mut events,
            built.as_mut().ok_or(EINVAL)?,
            cfg,
            log_buffer,
        )?;
        session.booted = true;
        Ok(())
    })();
    if let Err(e) = result {
        dev_err!(
            d,
            "G16 start failed: {:?}; session retained until reboot\n",
            e
        );
        retain_start_failure(shared, session, objects, built, events);
        return Err(e);
    }
    let (init, channels, max_frequency_khz) = built.ok_or(EINVAL)?;
    Ok(booted_slot
        .write_init(init!(Booted {
            session,
            init,
            objects,
            channels,
            events,
            id,
            max_frequency_khz,
        }))
        .unwrap_or_else(|e| match e {}))
}
