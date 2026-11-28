// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G14 GPU firmware boot, submission and retirement.
//!
//! [`start`] builds the 27.2 root (`gpu/v27_2.rs`) on the records of a 13.5 G14 manager,
//! installs the driver's kernel window in the loader's UAT regions, starts the ASC, holds the
//! GPU powered and retires one kernel-owned compute job. [`Backend`] then serves clients
//! through the common submission front end (`m3_submit`). Compute uses the bootstrap
//! queue. A render owns a separate tiler and fragment submission and their parameter buffer
//! until both data masters have retired.
//!
//! The runtime is driven by the firmware's endpoint-0x20 notifications, which it sends after
//! publishing records on its event, statistics and log rings: each one runs the service, which
//! consumes those rings (the firmware waits for space in them) and retires completed commands.
//! Completion follows the queue's retired position, which the firmware advances in order. There
//! is a delayed service run while waiting for firmware to free a pool slot or make room on
//! the device-control ring. These changes need not generate a notification.

use core::ops::Range;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use kernel::{
    device,
    drm::sched,
    new_condvar, new_delayed_work, new_mutex, new_spinlock, new_work,
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::{aref::ARef, Arc, ArcBorrow, CondVar, CondVarTimeoutResult, Mutex, SpinLock},
    time::{delay::fsleep, msecs_to_jiffies, Delta, Instant, Monotonic},
    uapi,
    workqueue::{impl_has_delayed_work, impl_has_work, DelayedWork, OwnedQueue, Work, WorkItem},
};

use crate::{
    alloc, event, g14_v27_2_render as render, gem,
    gpu::{self, CommandPool, ComputeJob, GpuManagerG14V13_5, QueueInfo, RootV27_2, POOL_SLOTS},
    gpu::{FirmwareEvent, FRAGMENT, TILER},
    hw, mmu, queue,
    m3::m3_submit::{Command, Packet},
};

/// The recovery handshake is the one G16 answers: share its decisions (which phase the host may
/// answer, how a snapshot entry decodes, the recovery-storm window) instead of restating them.
/// Its FirmwareData offsets are G16's own and unused here.
#[path = "g16/recovery/logic.rs"]
#[allow(dead_code)]
mod recovery_logic;
use recovery_logic::{RecoveryAction, SnapshotEntry};
use crate::g14_v27_2_abi::{self, Stopped};

/// RTKit endpoints of the GPU firmware.
const EP_FIRMWARE: u8 = 0x20;
const EP_DOORBELL: u8 = 0x21;

/// ASC CPU control register and its run bit.
const ASC_CPU_CONTROL: usize = 0x44;
const ASC_RUN: u32 = 0x10;

#[cfg(CONFIG_APPLE_PMGR_PWRSTATE)]
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn apple_pmgr_ps_set_auto_pm(
        consumer: *mut kernel::bindings::device,
        enable: bool,
    ) -> core::ffi::c_int;
}

fn set_gpu_auto_pm(dev: &device::Device, enable: bool) {
    #[cfg(CONFIG_APPLE_PMGR_PWRSTATE)]
    {
        // SAFETY: `dev` is a live bound GPU platform device.
        let ret = unsafe { apple_pmgr_ps_set_auto_pm(dev.as_raw(), enable) };
        if ret != 0 {
            dev_warn!(dev, "GPU: apple_pmgr_ps_set_auto_pm({}) failed: {}\n", enable, ret);
        }
    }
    #[cfg(not(CONFIG_APPLE_PMGR_PWRSTATE))]
    let _ = (dev, enable);
}

#[cfg(CONFIG_APPLE_SOC_SLEEP)]
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn apple_sleep_commit_stops_coprocs() -> bool;
    fn apple_sleep_coproc_register_first(
        dev: *mut kernel::bindings::device,
        last_stop: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> core::ffi::c_int>,
        data: *mut core::ffi::c_void,
    ) -> core::ffi::c_int;
    fn apple_sleep_coproc_set_ready(dev: *mut kernel::bindings::device, ready: bool);
    fn apple_sleep_coproc_stopped(dev: *mut kernel::bindings::device) -> bool;
    fn apple_sleep_coproc_wake_error(dev: *mut kernel::bindings::device) -> core::ffi::c_int;
    fn apple_sleep_coproc_unregister(dev: *mut kernel::bindings::device);
}

#[cfg(CONFIG_APPLE_SOC_SLEEP)]
struct CommitStop {
    rtk: *mut kernel::bindings::apple_rtkit,
    asc_ctl: *mut u8,
    ran: bool,
    idle_ret: core::ffi::c_int,
    idle_us: u32,
}

#[cfg(CONFIG_APPLE_SOC_SLEEP)]
// SAFETY: Owned by `State` under `Runtime::state`; read and written in the commit callback with
// interrupts off on the single boot CPU.
unsafe impl Send for CommitStop {}

#[cfg(CONFIG_APPLE_SOC_SLEEP)]
unsafe extern "C" fn g14_v27_2_commit_stop(data: *mut core::ffi::c_void) -> core::ffi::c_int {
    // SAFETY: `data` points to the heap-allocated `CommitStop` owned by `State::commit_stop`
    // until `sleep_complete` (or `stop`) unregisters this callback.
    let c = unsafe { &mut *data.cast::<CommitStop>() };
    c.ran = true;
    // SAFETY: `rtk` and `asc_ctl` remain mapped and live for the boot; runs at commit time with
    // interrupts off and all refusals behind it.
    unsafe {
        c.idle_ret = kernel::bindings::apple_rtkit_sleep_idle_polled(
            c.rtk,
            c.asc_ctl.cast(),
            &mut c.idle_us,
        );
        if c.idle_ret != 0 {
            return c.idle_ret;
        }
        kernel::bindings::apple_rtkit_mailbox_recv_off(c.rtk);
        let ctl = kernel::bindings::readl(c.asc_ctl.cast());
        kernel::bindings::writel(ctl & !ASC_RUN, c.asc_ctl.cast());
    }
    0
}

/// Commands slower than this are a hang; the firmware state is then not trusted.
const COMMAND_TIMEOUT_MS: i64 = 5000;
/// How long `stop` lets published work retire before it fails what is left.
const STOP_DRAIN_MS: u32 = 2000;
/// Bound on the wait between a command's completion stamp and its retired position.
const RETIRE_WAIT_US: i64 = 2000;
/// Poll interval when firmware can release a resource without an interrupt.
const RESOURCE_RETRY_MS: u32 = 10;
/// Work item for the poll. Ordinary firmware notifications can still queue immediate work.
const RETRY_WORK: u64 = 1;
/// The firmware's recovery handshake (the G16/M3 order and limits): phase 1 follows the error
/// event within 50 ms; the whole handshake within 1 s; then the stopped work leaves its queue.
const PHASE1_WAIT_MS: i64 = 50;
const HANDSHAKE_MS: i64 = 1000;
const RECOVERY_DRAIN_MS: i64 = 1000;
/// Further recoveries answered while the stopped work drains (work behind it can time out in
/// turn) before the runtime gives up.
const FURTHER_RECOVERIES: u32 = 4;
/// Event ring record kinds kept for the logs of a timeout or a recovery.
const EVENT_HISTORY: usize = 8;
/// Rounds start only within this budget; one round takes at most PHASE1_WAIT_MS +
/// HANDSHAKE_MS + RECOVERY_DRAIN_MS, so the lock is released well within the 10 s scheduler
/// job timeout.
const RECOVERY_BUDGET_MS: i64 = 5000;

struct FirmwareWait {
    since: Instant<Monotonic>,
    last_kick: Option<Instant<Monotonic>>,
}

impl FirmwareWait {
    /// Bound the wait even if the blocked command has not been published yet.
    fn kick_due(wait: &mut Option<Self>) -> Result<bool> {
        let now = Instant::now();
        let wait = wait.get_or_insert(Self { since: now, last_kick: None });
        if now - wait.since >= Delta::from_millis(COMMAND_TIMEOUT_MS) {
            return Err(ETIMEDOUT);
        }
        if wait.last_kick.is_some_and(|last| {
            now - last < Delta::from_millis(RESOURCE_RETRY_MS.into())
        }) {
            return Ok(false);
        }
        wait.last_kick = Some(now);
        Ok(true)
    }
}

#[cfg(any(CONFIG_APPLE_PMGR_MISC = "y", all(CONFIG_APPLE_PMGR_MISC = "m", MODULE)))]
unsafe extern "C" {
    fn apple_pmgr_gpu_tracking_start();
    fn apple_pmgr_gpu_tracking_stop();
    fn apple_pmgr_gpu_busy_get() -> kernel::ffi::c_int;
    fn apple_pmgr_gpu_busy_put();
}

fn pmgr_gpu_tracking_start() {
    #[cfg(any(CONFIG_APPLE_PMGR_MISC = "y", all(CONFIG_APPLE_PMGR_MISC = "m", MODULE)))]
    // SAFETY: Exported symbol from reachable apple-pmgr-misc with no pointer arguments.
    unsafe { apple_pmgr_gpu_tracking_start() };
}

fn pmgr_gpu_tracking_stop() {
    #[cfg(any(CONFIG_APPLE_PMGR_MISC = "y", all(CONFIG_APPLE_PMGR_MISC = "m", MODULE)))]
    // SAFETY: Matches `pmgr_gpu_tracking_start`.
    unsafe { apple_pmgr_gpu_tracking_stop() };
}

fn pmgr_gpu_busy_get() -> Result {
    #[cfg(any(CONFIG_APPLE_PMGR_MISC = "y", all(CONFIG_APPLE_PMGR_MISC = "m", MODULE)))]
    {
        // SAFETY: Exported symbol from reachable apple-pmgr-misc; lock order is state -> pmgr_misc.
        let ret = unsafe { apple_pmgr_gpu_busy_get() };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
    }
    Ok(())
}

fn pmgr_gpu_busy_put() {
    #[cfg(any(CONFIG_APPLE_PMGR_MISC = "y", all(CONFIG_APPLE_PMGR_MISC = "m", MODULE)))]
    // SAFETY: Matches `pmgr_gpu_busy_get`.
    unsafe { apple_pmgr_gpu_busy_put() };
}

/// The ASC control page and Fender operating-point page, kept mapped until the firmware is stopped.
pub(crate) struct AscControl(*mut u8, *mut u8);
// SAFETY: device register pages; accesses are single 32-bit MMIO operations.
unsafe impl Send for AscControl {}
// SAFETY: as above.
unsafe impl Sync for AscControl {}

impl AscControl {
    fn new(phys: u64, sgx: u64) -> Result<Self> {
        // SAFETY: `phys` is the ASC register window of this platform device. Apple SoC fabrics
        // require non-posted MMIO (`nonposted-mmio`); a posted write to ASC_CPU_CONTROL resets
        // T8103 immediately.
        let ptr = unsafe { kernel::bindings::ioremap_np(phys, 0x4000) };
        if ptr.is_null() { return Err(ENOMEM); }
        // SAFETY: `sgx + 0xd01000` is the Fender operating-point register inside the SGX window.
        let fender = unsafe { kernel::bindings::ioremap_np(sgx + 0xd01000, 0x1000) };
        if fender.is_null() {
            // SAFETY: mapped above.
            unsafe { kernel::bindings::iounmap(ptr) };
            return Err(ENOMEM);
        }
        Ok(Self(ptr.cast(), fender.cast()))
    }
    fn read(&self, at: usize) -> u32 {
        // SAFETY: `at` is a register offset inside the mapped page.
        unsafe { kernel::bindings::readl(self.0.add(at).cast()) }
    }
    fn write(&self, at: usize, value: u32) {
        // SAFETY: as above.
        unsafe { kernel::bindings::writel(value, self.0.add(at).cast()) }
    }
    fn fender(&self) -> u32 {
        // SAFETY: mapped Fender register page.
        unsafe { kernel::bindings::readl(self.1.cast()) }
    }
    /// Clears the ASC run bit.
    fn halt(&self) {
        self.write(ASC_CPU_CONTROL, self.read(ASC_CPU_CONTROL) & !ASC_RUN);
    }
}

impl Drop for AscControl {
    fn drop(&mut self) {
        // SAFETY: mapped in `new`, unmapped once.
        unsafe {
            kernel::bindings::iounmap(self.1.cast());
            kernel::bindings::iounmap(self.0.cast());
        }
    }
}

/// Whether a Fender word says the GPU is powered: a nonzero index and no request pending.
fn gpu_powered(fender: u32) -> bool {
    fender & 0x10 == 0 && fender & 0xf != 0
}

/// Whether a Fender word says the GPU is powered off: zero index and no request pending.
fn gpu_powered_off(fender: u32) -> bool {
    fender & 0x1f == 0
}

/// The GPU MMU fault status, read when a command times out: a pending fault names the faulting
/// address and unit, an empty status says the work stalled without a page fault.
struct FaultStatus {
    page: *mut u8,
    info: usize,
    addr: Option<usize>,
    g14x: bool,
}
// SAFETY: a device register page that is only read, one 32-bit MMIO load at a time.
unsafe impl Send for FaultStatus {}
// SAFETY: as above.
unsafe impl Sync for FaultStatus {}

impl FaultStatus {
    fn new(sgx: u64, cfg: &'static hw::HwConfig) -> Result<Self> {
        let (info, addr, g14x) = crate::regs::Resources::fault_registers(cfg);
        let base = info & !0xfff;
        if addr.is_some_and(|a| a & !0xfff != base) {
            return Err(EINVAL);
        }
        // SAFETY: the page lies inside this platform device's SGX register window.
        let page = unsafe { kernel::bindings::ioremap_np(sgx + base as u64, 0x1000) };
        if page.is_null() { return Err(ENOMEM); }
        Ok(Self { page: page.cast(), info: info - base, addr: addr.map(|a| a - base), g14x })
    }
    fn read64(&self, at: usize) -> u64 {
        // SAFETY: `at` and `at + 4` are register offsets inside the mapped page.
        let (lo, hi) = unsafe {
            (kernel::bindings::readl(self.page.add(at).cast()),
             kernel::bindings::readl(self.page.add(at + 4).cast()))
        };
        u64::from(lo) | (u64::from(hi) << 32)
    }
    fn log(&self, dev: &device::Device) {
        let info = self.read64(self.info);
        if info & 1 == 0 {
            dev_err!(dev, "GPU: no MMU fault pending at the timeout (FAULT_INFO={:#x})\n", info);
            return;
        }
        let addr = self.addr.map_or(info >> 30, |at| self.read64(at));
        dev_err!(dev, "GPU: MMU fault pending at the timeout: FAULT_INFO={:#x} {:?}\n", info,
                 crate::regs::decode_fault(info, addr, self.g14x));
    }
}

impl Drop for FaultStatus {
    fn drop(&mut self) {
        // SAFETY: mapped in `new`, unmapped once.
        unsafe { kernel::bindings::iounmap(self.page.cast()) }
    }
}

/// Where the firmware's messages go: the runtime once it exists, nowhere before or after.
#[pin_data]
pub(crate) struct Notify {
    dev: ARef<device::Device>,
    #[pin]
    runtime: SpinLock<Option<Arc<Runtime>>>,
    /// Endpoint-0x20 messages received, reported when the firmware stops.
    messages: AtomicU64,
}

impl Notify {
    fn new(dev: &device::Device) -> Result<Arc<Self>> {
        Arc::pin_init(pin_init!(Self {
            dev: dev.into(),
            runtime <- new_spinlock!(None, "g14_v27_2::Notify::runtime"),
            messages: AtomicU64::new(0),
        }), GFP_KERNEL)
    }

    /// Queues one service run if a runtime is attached. A run already queued covers this one.
    fn wake(&self) {
        if let Some(runtime) = self.runtime.lock().as_ref() {
            if !runtime.deep_sleeping.load(Ordering::Acquire) {
                let _ = runtime.queue.enqueue::<Arc<Runtime>, 0>(runtime.clone());
            }
        }
    }

    /// Queues a poll while attached. Detaching also fences off poll requests before teardown.
    fn retry(&self) {
        if let Some(runtime) = self.runtime.lock().as_ref() {
            if runtime.healthy() && !runtime.deep_sleeping.load(Ordering::Acquire) {
                let _ = runtime.queue.enqueue_delayed::<Arc<Runtime>, RETRY_WORK>(
                    runtime.clone(), msecs_to_jiffies(RESOURCE_RETRY_MS),
                );
            }
        }
    }

    fn attach(&self, runtime: Arc<Runtime>) {
        *self.runtime.lock() = Some(runtime);
    }

    /// Toggles `deep_sleeping` under the same lock that guards `wake` and `retry`, so no
    /// notification can race a queue drain after this returns with `deep == true`.
    #[cfg(CONFIG_APPLE_SOC_SLEEP)]
    fn set_deep_sleeping(&self, runtime: &Runtime, deep: bool) {
        let _guard = self.runtime.lock();
        runtime.deep_sleeping.store(deep, Ordering::Release);
    }

    /// After this returns, nothing queues service work.
    fn detach(&self) -> Option<Arc<Runtime>> {
        self.runtime.lock().take()
    }
}

/// RTKit operations of the GPU firmware: its crash log lives in the loader's data segment.
pub(crate) struct Firmware;

#[vtable]
impl rtkit::Operations for Firmware {
    type Data = Arc<Notify>;
    type Buffer = crate::g17::CrashLog;

    const COPROC_PLACES_BUFFERS: bool = true;

    fn shmem_map(notify: ArcBorrow<'_, Notify>, iova: usize, size: usize) -> Result<Self::Buffer> {
        crate::g17::CrashLog::from_firmware(&notify.dev, iova, size)
    }

    fn crashed(notify: ArcBorrow<'_, Notify>, log: Option<&[u8]>) {
        unsafe extern "C" {
            fn asahi_save_rtkit_dump(dev: *mut core::ffi::c_void, data: *const u8, size: usize);
        }
        dev_err!(&*notify.dev, "GPU firmware crashed\n");
        if let Some(log) = log {
            // SAFETY: RTKit and the device remain live throughout this synchronous copy.
            unsafe { asahi_save_rtkit_dump(notify.dev.as_raw().cast(), log.as_ptr(), log.len()) };
        }
        // The service sees the crash and fails outstanding work.
        notify.wake();
    }

    /// The firmware sends endpoint 0x20 a message after it publishes event, statistics or log
    /// records. Whatever the message, the service reads every ring and the queue's position,
    /// so each one is a wakeup.
    fn recv_message(notify: ArcBorrow<'_, Notify>, ep: u8, _message: u64) {
        if ep == EP_FIRMWARE {
            notify.messages.fetch_add(1, Ordering::Relaxed);
            notify.wake();
        }
    }
}

type Peer = Pin<KBox<rtkit::RtKit<Firmware>>>;

/// Waits for a shared-word acknowledgement, checking `done` every millisecond and draining the
/// firmware's rings, for at most `ms`. Only for the start/stop handshakes whose acknowledgement
/// is a word the firmware writes with no message: the ready status, the command-service
/// installation, device-control (idle-policy) consumption and the running flag. The existing G14
/// path waits for its device-control channel the same way (`channel::TxChannel::wait_for`).
fn wait(root: &RootV27_2, ms: i64, mut done: impl FnMut() -> Result<bool>) -> Result {
    let start = Instant::<Monotonic>::now();
    loop {
        if done()? { return Ok(()); }
        if start.elapsed() >= Delta::from_millis(ms) { return Err(ETIMEDOUT); }
        let _ = root.drain();
        fsleep(Delta::from_millis(1));
    }
}

/// Publishes an idle-policy record and waits until the firmware has consumed and
/// acknowledged it. On error, returns `(published, err)` where `published` is true if the
/// control record was already written to shared memory before the failure.
fn try_set_idle_policy(
    root: &RootV27_2,
    peer: &mut Peer,
    allow_off: bool,
) -> core::result::Result<(), (bool, Error)> {
    let (target, generation) = root.publish_idle_policy(allow_off).map_err(|e| (false, e))?;
    peer.as_mut()
        .send_message(EP_DOORBELL, gpu::DEVICE_CONTROL_KICK)
        .map_err(|e| (true, e))?;
    wait(root, 3000, || {
        Ok(root.idle_progress()? == [target; 3] && root.idle_acknowledgement()? == generation)
    })
    .map_err(|e| (true, e))
}

/// Publishes an idle-policy record and waits until the firmware has consumed and
/// acknowledged it.
fn set_idle_policy(root: &RootV27_2, peer: &mut Peer, allow_off: bool) -> Result {
    try_set_idle_policy(root, peer, allow_off).map_err(|(_, e)| e)
}

/// Stops firmware that no runtime owns: allow power-off, then RTKit power-off, then the ASC
/// run bit. Everything it could reach is kept for the rest of the boot.
fn halt_unowned(dev: &device::Device, mut peer: Peer, root: crate::fw::types::GpuObject<RootV27_2>,
                asc: AscControl, booted: bool) {
    if booted {
        let allowed = set_idle_policy(&root, &mut peer, true);
        // SAFETY: the RTKit instance is live and owned here.
        let off = kernel::error::to_result(unsafe { kernel::bindings::apple_rtkit_poweroff(peer.as_raw()) });
        dev_err!(dev, "GPU firmware halted: allow-off={:?} power-off={:?}\n", allowed, off);
    }
    asc.halt();
    core::mem::forget(peer);
    core::mem::forget(root);
    core::mem::forget(asc);
}

/// A started 27.2 GPU, ready for DRM registration.
pub(crate) struct Started<'a> {
    pub(crate) backend: Arc<Backend>,
    pub(crate) drm: kernel::drm::UnregisteredDevice<crate::driver::AsahiDrmDriver>,
    pub(crate) binding: crate::binding::Owner,
    pub(crate) resources: crate::regs::Resources<'a>,
}

/// Starts the firmware and retires the boot job.
pub(crate) fn start<'a>(
    pdev: &'a platform::Device<device::Core<'_>>,
    cfg: &'static hw::HwConfig,
    registers: &'static [hw::RegisterV27_2],
    writable: u64,
) -> Result<Started<'a>> {
    use kernel::dma::{Device as _, DmaMask};
    let dev: &device::Device = pdev.as_ref();
    let geometry = mmu::UatGeometry { ias: cfg.uat_ias, oas: cfg.uat_oas };
    let mut window = mmu::FirmwareWindow::reserve(dev, geometry)?;
    let sgx = pdev.resource_by_name(c"sgx").ok_or(EINVAL)?;
    let asc = AscControl::new(pdev.resource_by_name(c"asc").ok_or(EINVAL)?.start(), sgx.start())?;
    let control = asc.read(ASC_CPU_CONTROL);
    if control & ASC_RUN != 0 { return Err(EBUSY); }
    let notify = Notify::new(dev)?;
    let mut peer = KBox::pin(
        rtkit::RtKit::<Firmware>::new(dev, None, 0, notify.clone())?,
        GFP_KERNEL)?;
    let resources = crate::regs::Resources::new(pdev)?;
    let fault = FaultStatus::new(sgx.start(), cfg)?;
    let copy_bytes: u32 = dev.fwnode().ok_or(ENODEV)?
        .property_read(c"apple,coredump-copy-buffer").required_by(dev)?;
    let board = gpu::BoardV27_2 {
        sgx: sgx.start(), sgx_size: sgx.size() as u64, copy_bytes, registers, writable,
    };

    // SAFETY: the mask is the UAT's physical width.
    unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(geometry.oas)?)? };
    let drm = kernel::drm::UnregisteredDevice::<crate::driver::AsahiDrmDriver>::new(pdev, Ok(()))?;
    // SAFETY: the unregistered DRM instance is private to this bound probe.
    let binding = unsafe { crate::binding::Owner::new(pdev.as_ref(), &drm)? };
    let uat = mmu::Uat::new_adopted(&drm, geometry, &window.regions())?;
    let graph = GpuManagerG14V13_5::build_graph(&drm, &resources, cfg, KBox::new(uat, GFP_KERNEL)?,
                                                gpu::GraphProfile::G14V27_2)?;
    let root = RootV27_2::build(&graph, &board)?;
    let pool = CommandPool::new(&mut gpu::GpuManager::alloc(&*graph), cfg)?;
    // Timestamp buffers are bound in the timestamp aperture while the firmware runs, and the
    // window only carries the tables that exist now: link the aperture's tables first.
    graph.kernel_uat().prepare_kernel_tables(&[graph.timestamp_range()])?;
    window.mirror(graph.kernel_uat())?;
    window.install()?;
    window.announce();
    asc.write(ASC_CPU_CONTROL, control | ASC_RUN);

    // From here the firmware may read everything the window names: nothing is freed.
    let mut booted = false;
    let mut step = "loader handoff";
    let result = (|| -> Result {
        let start = Instant::<Monotonic>::now();
        while !window.answered() {
            if start.elapsed() >= Delta::from_millis(1000) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_millis(1));
        }
        step = "RTKit boot";
        peer.as_mut().boot()?;
        booted = true;
        peer.as_mut().start_endpoint(EP_FIRMWARE)?;
        peer.as_mut().start_endpoint(EP_DOORBELL)?;
        if !window.intact() { return Err(EIO); }
        step = "initialization";
        peer.as_mut().send_message(EP_FIRMWARE, gpu::init_message(root.gpu_va().get()))?;
        // The firmware sets status +0x14 when it has initialized; the first control record
        // waits for it.
        wait(&root, 2000, || root.initialized())?;
        step = "keep-powered policy";
        set_idle_policy(&root, &mut peer, false)?;
        // Powered initialization installs the command handlers and then clears the
        // installation request the root set (+0x4c24).
        step = "command-handler installation";
        wait(&root, 500, || root.commands_installed())?;
        step = "boot job";
        root.publish_boot_job(resources.operating_point())?;
        peer.as_mut().send_message(EP_DOORBELL, gpu::COMPUTE_KICK)?;
        wait(&root, 3000, || root.boot_job_complete())?;
        if root.has_pwr_controller() {
            step = "idle power-off policy";
            set_idle_policy(&root, &mut peer, true)?;
            set_gpu_auto_pm(dev, true);
        }
        Ok(())
    })();
    if let Err(e) = result {
        dev_err!(dev, "GPU firmware 27.2 did not start: {}: {:?}\n", step, e);
        halt_unowned(dev, peer, root, asc, booted);
        core::mem::forget(window);
        core::mem::forget(pool);
        core::mem::forget(graph);
        return Err(e);
    }
    // The firmware keeps using the window until it is stopped; it stays for the life of the boot.
    core::mem::forget(window);
    let backend = Backend::start(&drm, dev, notify, peer, root, graph, pool, asc, fault, cfg)?;
    Ok(Started { backend, drm, binding, resources })
}

/// Whether the packet's VM has failed (`VmStatus` latched): its work is no longer admitted.
fn vm_failed(packet: &Packet) -> bool {
    packet.vm.status().is_some_and(|status| status.get() != 0)
}

/// Latches the VM of work a recovery stopped as guilty, before the work drains: from here on
/// `submit` refuses it and `pump` publishes none of its queued packets.
fn latch_guilty(packet: &Packet, error: Error) {
    if let Some(status) = packet.vm.status() {
        status.note_recovery_guilty();
        status.record(error);
    }
}

struct Inflight {
    packet: Arc<Packet>,
    last: bool,
    publication_error: Option<Error>,
    stamp: event::EventValue,
    slot: usize,
    /// Queue publications up to and including this command, counted from take-over.
    ordinal: u64,
    started: Instant<Monotonic>,
    /// Set when the firmware stopped this command: it fails its packet when it leaves the queue.
    doomed: Option<Error>,
    _bind: mmu::VmBind,
}

struct RenderFlight {
    packet: Arc<Packet>,
    last: bool,
    bind: mmu::VmBind,
    scene: render::Prep,
    slots: [usize; 2],
    stamps: [event::EventValue; 2],
    targets: [u32; 2],
    started: Instant<Monotonic>,
    doomed: Option<Error>,
}

/// Who a recovery round blamed, for its log line: the client, and the stopped work's stamps
/// (render: tiler and fragment; compute: the stopped command's and the queue's current one).
struct Blamed {
    owner: crate::m3::m3_submit::Owner,
    stamps: [u32; 2],
    /// The stopped compute command's queue ordinal: the drain waits for this command only. The
    /// packet's later commands were not stopped and retire (failed) through the normal path.
    stopped: Option<u64>,
}

struct State {
    peer: Peer,
    root: crate::fw::types::GpuObject<RootV27_2>,
    pool: CommandPool,
    asc: AscControl,
    fault: FaultStatus,
    queue: QueueInfo,
    last: event::EventValue,
    sequence: u64,
    next_slot: usize,
    waiting_unlink: Option<FirmwareWait>,
    waiting_control: Option<FirmwareWait>,
    /// Publications and retirements counted on the queue's position block.
    published: u64,
    /// Completion stamps seen before the retired position they precede.
    late: u64,
    /// Pool slots holding a published command, one bit each.
    busy: u64,
    /// Free slots passed over because the firmware still held them.
    skipped: u64,
    /// Retired slots whose cursor is not advanced yet: the firmware had not recorded the
    /// value to advance it to (`CommandPool::retire`). One bit each.
    unacked: u64,
    /// Retirements whose cursor had to wait for the firmware's record.
    deferred: u64,
    retired_total: u64,
    retired_raw: u32,
    capacity: u32,
    inflight: KVec<Inflight>,
    rendering: Option<RenderFlight>,
    pending: KVec<(Arc<Packet>, usize)>,
    accepting: bool,
    submitted: u64,
    retired: u64,
    /// Set when the DT power-controller calibration is present and runtime idle power-off is
    /// permitted (firmware powers the GPU off after `fender_idle_off_delay_ms` of inactivity).
    idle_off_allowed: bool,
    /// Set while a PMGR fabric/DCS busy reference is held for active GPU work.
    gpu_busy_held: bool,
    /// A fault or timeout the firmware reported (error, named slot), answered by `recover`.
    firmware_error: Option<(Error, i32)>,
    recoveries: u64,
    /// Recovery-storm window (`recovery_logic::admit`): its start and the recoveries in it.
    recovery_window: u64,
    recovery_count: u32,
    /// Reports read and rounds answered: tells a stale report from a new one.
    ledger: g14_v27_2_abi::RecoveryLedger,
    /// Kind words of the last event ring records read, and how many were read in all.
    events: [u32; EVENT_HISTORY],
    events_read: u64,
    /// Packets with a command the firmware stopped (`blame`), with their VM job guard, kept
    /// for the rest of the boot: the firmware may still reach that memory. Allocated with
    /// `g14_v27_2_abi::QUARANTINE_LIMIT` places at start, so keeping one never allocates.
    kept: KVec<Arc<Packet>>,
    /// The stopped commands of those packets, with their VM binding and scene, once they left
    /// their queue (`Runtime::keep`). Held, never read.
    kept_compute: KVec<Inflight>,
    kept_renders: KVec<RenderFlight>,
    /// Set when a sleep/wake failure may have left the GPU domain unpowered or stopped so
    /// teardown skips live-hardware MMIO and RTKit operations.
    hardware_unreachable: bool,
    #[cfg(CONFIG_APPLE_SOC_SLEEP)]
    commit_stop: Option<KBox<CommitStop>>,
}

impl State {
    /// Stop publishing this packet. Report failure only once any published prefix retires.
    fn fail_publication(&mut self, packet: &Arc<Packet>, err: Error) {
        let _ = self.pending.remove(0);
        if let Some(last) = self.inflight.iter_mut().rev()
            .find(|flight| Arc::ptr_eq(&flight.packet, packet))
        {
            last.publication_error = Some(err);
        } else {
            if g14_v27_2_abi::guard_releasable(false, self.kept(packet)) {
                packet.release_guard();
            }
            packet.finish(Err(err));
        }
    }

    /// The kinds of the last event ring records read, oldest first, for a log line.
    fn recent_events(&self) -> ([u32; EVENT_HISTORY], usize) {
        let count = self.events_read.min(EVENT_HISTORY as u64) as usize;
        let mut out = [0; EVENT_HISTORY];
        for (i, kind) in out.iter_mut().take(count).enumerate() {
            let at = (self.events_read - count as u64 + i as u64) % EVENT_HISTORY as u64;
            *kind = self.events[at as usize];
        }
        (out, count)
    }

    fn running(&self) -> bool {
        !self.inflight.is_empty() || self.rendering.is_some()
    }

    fn ensure_gpu_busy(&mut self) -> Result {
        if !self.gpu_busy_held {
            pmgr_gpu_busy_get()?;
            self.gpu_busy_held = true;
        }
        Ok(())
    }

    fn release_gpu_busy_if_idle(&mut self) {
        if !self.running() && self.gpu_busy_held {
            pmgr_gpu_busy_put();
            self.gpu_busy_held = false;
        }
    }

    /// Whether a queue holds any of the packet's commands.
    fn live(&self, packet: &Arc<Packet>) -> bool {
        self.inflight.iter().any(|flight| Arc::ptr_eq(&flight.packet, packet))
            || self.rendering.as_ref().is_some_and(|flight| Arc::ptr_eq(&flight.packet, packet))
    }

    /// Whether a recovery stopped a command of the packet: it stays owned for the boot.
    fn kept(&self, packet: &Arc<Packet>) -> bool {
        self.kept.iter().any(|kept| Arc::ptr_eq(kept, packet))
    }

    /// Whether the runtime owns the packet's VM job guard (`g14_v27_2_abi::guard_releasable`).
    fn owns(&self, packet: &Arc<Packet>) -> bool {
        !g14_v27_2_abi::guard_releasable(self.live(packet), self.kept(packet))
    }

    fn pending(&self, packet: &Arc<Packet>) -> bool {
        self.pending.iter().any(|(current, _)| Arc::ptr_eq(current, packet))
    }

    fn advance_packet(&mut self, last: bool) {
        if last {
            let _ = self.pending.remove(0);
        } else {
            self.pending[0].1 += 1;
        }
    }
}

#[pin_data(PinnedDrop)]
pub(crate) struct Runtime {
    dev: ARef<device::Device>,
    graph: Arc<GpuManagerG14V13_5>,
    healthy: AtomicBool,
    /// Set between [`Runtime::sleep_prepare`] and [`Runtime::sleep_complete`]: nothing is
    /// published across a system sleep.
    asleep: AtomicBool,
    /// Set across a firmware-driven deep sleep once workers are drained and the commit stop
    /// is armed; prevents workers from touching the stopped firmware or mailbox.
    deep_sleeping: AtomicBool,
    notify: Arc<Notify>,
    /// Runs the service. Only `Notify` queues on it, and `stop` drains it.
    queue: OwnedQueue,
    #[pin]
    state: Mutex<Option<State>>,
    /// Signalled when no work is pending or published, or the runtime fails.
    #[pin]
    idle: CondVar,
    #[pin]
    work: Work<Runtime>,
    #[pin]
    retry: DelayedWork<Runtime, RETRY_WORK>,
}

impl_has_work! {
    impl HasWork<Runtime> for Runtime { self.work }
}

impl WorkItem for Runtime {
    type Pointer = Arc<Runtime>;
    fn run(this: Arc<Runtime>) {
        this.service();
    }
}

impl_has_delayed_work! {
    impl HasDelayedWork<Runtime, RETRY_WORK> for Runtime { self.retry }
}

impl WorkItem<RETRY_WORK> for Runtime {
    type Pointer = Arc<Runtime>;
    fn run(this: Arc<Runtime>) {
        if this.healthy() {
            this.service();
        }
    }
}

#[pinned_drop]
impl PinnedDrop for Runtime {
    fn drop(self: Pin<&mut Self>) {
        // `Notify` holds a reference until `stop`, so this runs only after it, or for a
        // runtime that never took over the firmware.
        if self.state.lock().is_some() { self.stop(); }
    }
}

impl Runtime {
    pub(crate) fn healthy(&self) -> bool { self.healthy.load(Ordering::Acquire) }

    /// Runs the service once more: the DRM scheduler's job timeout calls this, so a command the
    /// firmware never completes still reaches the command timeout.
    pub(crate) fn wake(&self) { self.notify.wake(); }

    /// Queues a packet and publishes what the pool has room for; the firmware's completion
    /// notifications run the service, which retires it and publishes the rest.
    pub(crate) fn submit(self: &Arc<Self>, packet: Arc<Packet>) -> Result {
        if !self.healthy() { return Err(EIO); }
        if packet.commands.is_empty() {
            return Err(ENOTSUPP);
        }
        // A VM that lost its context (a recovery found it guilty, or its work failed) runs
        // nothing more on the GPU.
        if vm_failed(&packet) { return Err(EIO); }
        let mut guard = self.state.lock();
        let state = Option::as_mut(&mut *guard).ok_or(ENODEV)?;
        if !self.healthy() { return Err(EIO); }
        if !state.accepting { return Err(ENODEV); }
        state.pending.push((packet, 0), GFP_KERNEL)?;
        self.pump(state);
        Ok(())
    }

    /// Withdraws a packet's unpublished commands; published ones retire normally.
    pub(crate) fn withdraw(&self, packet: &Arc<Packet>) {
        let mut guard = self.state.lock();
        if let Some(state) = Option::as_mut(&mut *guard) {
            if let Some(i) = state.pending.iter().position(|(p, _)| Arc::ptr_eq(p, packet)) {
                let _ = state.pending.remove(i);
            }
            if !state.owns(packet) {
                packet.release_guard();
            }
            if state.pending.is_empty() {
                state.waiting_unlink = None;
            }
        }
    }

    fn fail_all(&self, state: &mut State, error: Error) {
        self.healthy.store(false, Ordering::Release);
        self.idle.notify_all();
        while let Some((packet, _)) = state.pending.pop() {
            if !state.owns(&packet) {
                packet.release_guard();
            }
            packet.finish(Err(error));
        }
        if let Some(flight) = &state.rendering {
            if let Some(status) = flight.packet.vm.status() { status.record(error); }
            flight.packet.finish(Err(error));
        }
        // Published work stays owned: the firmware may still read it.
        for f in state.inflight.iter() {
            if let Some(status) = f.packet.vm.status() { status.record(error); }
            f.packet.finish(Err(error));
        }
    }

    /// Advances the cursors of retired slots that were waiting for the firmware's record.
    fn acknowledge(&self, state: &mut State) {
        let mut pending = state.unacked;
        while pending != 0 {
            let slot = pending.trailing_zeros() as usize;
            pending &= pending - 1;
            match state.pool.retire(slot) {
                Ok(false) => {}
                Ok(true) => state.unacked &= !(1 << slot),
                Err(e) => {
                    dev_err!(self.dev, "GPU: tracking retirement failed {:?}\n", e);
                    state.unacked &= !(1 << slot);
                }
            }
        }
    }

    /// Finds unlinked slots without moving any publication cursor.
    fn free_slots(&self, state: &mut State, count: usize) -> Result<Option<[usize; 2]>> {
        let mut slots = [0; 2];
        let mut found = 0;
        for k in 0..POOL_SLOTS {
            let slot = (state.next_slot + k) % POOL_SLOTS;
            if state.busy & (1 << slot) != 0 { continue; }
            if state.pool.registered(slot)? {
                // The firmware can count another record for a retired slot after its cursor
                // was advanced, and keeps the slot listed until the cursor matches again:
                // advance it to the current count. The slot's last command stays intact in
                // its buffer, so an early unlink is harmless.
                state.pool.retire(slot)?;
                state.skipped += 1;
                continue;
            }
            slots[found] = slot;
            found += 1;
            if found == count {
                state.waiting_unlink = None;
                return Ok(Some(slots));
            }
        }
        if FirmwareWait::kick_due(&mut state.waiting_unlink)? {
            state.peer.as_mut().send_message(EP_DOORBELL, gpu::COMPUTE_KICK)?;
            for dm in [TILER, FRAGMENT] {
                if state.root.render_queue_announced(dm) {
                    state.peer.as_mut().send_message(EP_DOORBELL, RootV27_2::render_kick(dm))?;
                }
            }
        }
        self.notify.retry();
        Ok(None)
    }

    /// Compute commands may pipeline. A render pair is a scheduling boundary so its buffer,
    /// timestamps and dependencies remain owned until both data masters retire.
    fn pump(&self, state: &mut State) {
        self.acknowledge(state);
        if !self.healthy() || self.asleep.load(Ordering::Acquire) || state.rendering.is_some() {
            return;
        }
        while state.inflight.len() < POOL_SLOTS - 1 && !state.pending.is_empty() {
            let (packet, index) = (state.pending[0].0.clone(), state.pending[0].1);
            if vm_failed(&packet) {
                // Queued before its VM failed: none of it is published from now on.
                state.fail_publication(&packet, EIO);
                continue;
            }
            let command = packet.commands[index];
            let paired = matches!(command, Command::Render { .. });
            if paired && state.running() { return; }
            let count = if paired { 2 } else { 1 };
            let slots = match self.free_slots(state, count) {
                Ok(Some(slots)) => slots,
                Ok(None) => return,
                Err(err) => {
                    dev_err!(self.dev, "GPU: command pool stalled: {:?} busy={:#x} unacked={:#x} inflight={} rendering={}\n",
                             err, state.busy, state.unacked, state.inflight.len(), state.rendering.is_some());
                    for slot in 0..POOL_SLOTS {
                        if let Ok((count, cursor, linked)) = state.pool.describe(slot) {
                            if linked != 0 || count != cursor {
                                dev_err!(self.dev, "GPU: slot {}: records={} cursor={} linked={}\n",
                                         slot, count, cursor, linked);
                            }
                        }
                    }
                    self.fail_all(state, err);
                    return;
                }
            };
            let last = index + 1 == packet.commands.len();
            let result = match command {
                Command::Compute(control) =>
                    self.publish_compute(state, &packet, index, control, last, slots[0]),
                Command::Render { command, usc } =>
                    self.publish_render(state, &packet, index, &command, usc, last, slots),
            };
            match result {
                Ok(()) if paired || !self.healthy() => return,
                Ok(()) => {}
                Err(EBUSY) if state.running() => return,
                Err(err) => {
                    dev_err!(self.dev, "GPU: command publication failed {:?}\n", err);
                    state.fail_publication(&packet, err);
                }
            }
        }
    }

    fn publish_compute(
        &self, state: &mut State, packet: &Arc<Packet>, index: usize,
        control: crate::m3::agx_compute::Control, last: bool, slot: usize,
    ) -> Result {
        let bind = self.graph.kernel_uat().bind(&packet.vm)?;
        let preempt = packet.preempt.as_ref().ok_or(EINVAL)?.gpu_va().get();
        let stamp = state.last.next();
        let job = ComputeJob {
            context: bind.slot(),
            sequence: state.sequence,
            cdm_base: control.base,
            cdm_end: control.end,
            usc_base: control.usc_base,
            preempt,
            timestamps: packet.timestamp_addresses(index),
            helper: control.helper,
            sampler_heap: control.sampler_heap,
            sampler_count: control.sampler_count,
        };
        let command = state.pool.encode(slot, &state.queue, stamp.raw(), &job)?;
        // Raise fabric/DCS bandwidth before the firmware can see the new producer.
        state.ensure_gpu_busy()?;
        let submission = match state.root.stage_compute(command) {
            Ok(s) => s,
            Err(err) => {
                state.release_gpu_busy_if_idle();
                return Err(err);
            }
        };
        let ordinal = state.published + 1;
        // Commit ownership before the firmware can see the new work.
        if let Err(err) = state.inflight.push(Inflight { packet: packet.clone(), last, stamp, slot, ordinal,
            publication_error: None, started: Instant::now(), doomed: None, _bind: bind }, GFP_KERNEL) {
            drop(submission);
            state.release_gpu_busy_if_idle();
            return Err(err.into());
        }
        packet.mark_published();
        submission.publish();
        state.pool.mark_published(slot);
        state.last = stamp;
        state.sequence += 1;
        state.unacked &= !(1 << slot);
        state.busy |= 1 << slot;
        state.published = ordinal;
        state.next_slot = (slot + 1) % POOL_SLOTS;
        state.submitted += 1;
        state.advance_packet(last);
        if let Err(err) = state.peer.as_mut().send_message(EP_DOORBELL, gpu::COMPUTE_KICK) {
            self.fail_all(state, err);
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn publish_render(
        &self, state: &mut State, packet: &Arc<Packet>, index: usize,
        command: &crate::m3::agx_uapi::UapiRenderCommand, usc: u64, last: bool, slots: [usize; 2],
    ) -> Result {
        let queue = packet.render_queue.as_ref().ok_or(EINVAL)?;
        let scene = queue.prepare(command, usc)?;
        let bind = self.graph.kernel_uat().bind(&packet.vm)?;
        let context = bind.slot();
        let (tiler, tiler_stamp) = state.root.render_queue(TILER);
        let (fragment, fragment_stamp) = state.root.render_queue(FRAGMENT);
        let stamps = [tiler_stamp.next(), fragment_stamp.next()];
        let with_buffer = scene.buffer_op.is_some();
        let timestamps = packet.render_timestamp_addresses(index);
        let attachments = &packet.render_attachments[index];
        let ta = state.pool.encode_with(slots[TILER], context, state.sequence, stamps[TILER].raw(),
            |cmd, stream, common| {
                render::encode_vertex(cmd, stream, &common, &tiler, tiler.compute_state,
                    command, &scene, stamps[FRAGMENT].raw(), fragment.slot,
                    timestamps[TILER], &attachments[TILER])?;
                if let Some(op) = scene.buffer_op {
                    render::encode_buffer_op(&mut cmd[render::INITBUF_AT..][..0x20], &common, &scene, op);
                }
                Ok(())
            })?;
        let fs = state.pool.encode_with(slots[FRAGMENT], context, state.sequence + 1, stamps[FRAGMENT].raw(),
            |cmd, stream, common| {
                render::encode_fragment(cmd, stream, &common, &fragment, fragment.compute_state,
                    command, &scene, timestamps[FRAGMENT], &attachments[FRAGMENT])?;
                render::encode_own_barrier(&mut cmd[render::OWN_BARRIER_AT..][..render::OWN_BARRIER_SIZE],
                    &tiler, stamps[TILER].raw(), stamps[FRAGMENT].raw());
                Ok(())
            })?;
        let ta_work = [ta + render::INITBUF_AT as u64, ta];
        let fs_work = [fs + render::OWN_BARRIER_AT as u64, fs];
        // Raise fabric/DCS bandwidth before the firmware can see the new producers.
        state.ensure_gpu_busy()?;
        let ta_post = match state.root.stage_render(TILER, &ta_work[usize::from(!with_buffer)..]) {
            Ok(p) => p,
            Err(err) => {
                state.release_gpu_busy_if_idle();
                return Err(err);
            }
        };
        let fs_post = match state.root.stage_render(FRAGMENT, &fs_work) {
            Ok(p) => p,
            Err(err) => {
                drop(ta_post);
                state.release_gpu_busy_if_idle();
                return Err(err);
            }
        };
        state.rendering = Some(RenderFlight {
            packet: packet.clone(), last, bind, scene, slots, stamps,
            targets: [ta_post.target(), fs_post.target()], started: Instant::now(), doomed: None,
        });
        queue.published();
        packet.mark_published();
        // Queue the fragment dependency before kicking the tiler that satisfies it.
        fs_post.publish();
        ta_post.publish();
        for slot in slots {
            state.pool.mark_published(slot);
        }
        for slot in slots {
            state.busy |= 1 << slot;
            state.unacked &= !(1 << slot);
        }
        state.next_slot = (slots[FRAGMENT] + 1) % POOL_SLOTS;
        state.sequence += 2;
        state.submitted += 2;
        state.advance_packet(last);
        for dm in [FRAGMENT, TILER] {
            if let Err(err) = state.peer.as_mut().send_message(EP_DOORBELL, RootV27_2::render_kick(dm)) {
                self.fail_all(state, err);
                break;
            }
        }
        Ok(())
    }

    fn retire_slot(&self, state: &mut State, slot: usize) {
        match state.pool.retire(slot) {
            Ok(true) => {}
            Ok(false) => {
                state.unacked |= 1 << slot;
                state.deferred += 1;
            }
            Err(err) => dev_err!(self.dev, "GPU: tracking retirement failed {:?}\n", err),
        }
        state.busy &= !(1 << slot);
        state.retired += 1;
    }

    fn retire_render(&self, state: &mut State) -> Result {
        let Some(flight) = state.rendering.as_ref() else { return Ok(()); };
        let (targets, stamps, doomed, started) =
            (flight.targets, flight.stamps, flight.doomed, flight.started);
        let done = |root: &RootV27_2, dm: usize| -> Result<bool> {
            // A command the firmware stopped leaves its queue without reaching its stamp.
            if doomed.is_some() { root.render_retired(dm, targets[dm]) }
            else { root.render_done(dm, targets[dm], stamps[dm]) }
        };
        let mut ta_done = done(&state.root, TILER)?;
        let mut fs_done = done(&state.root, FRAGMENT)?;
        if !(ta_done && fs_done)
            && state.root.render_queue(TILER).1 >= stamps[TILER]
            && state.root.render_queue(FRAGMENT).1 >= stamps[FRAGMENT]
        {
            // As on the compute queue, the final stamp precedes the retired cursor and
            // there may be no separate notification when the cursor changes.
            let start = Instant::<Monotonic>::now();
            while !(ta_done && fs_done) && start.elapsed() < Delta::from_micros(RETIRE_WAIT_US) {
                fsleep(Delta::from_micros(20));
                ta_done = done(&state.root, TILER)?;
                fs_done = done(&state.root, FRAGMENT)?;
            }
        }
        if ta_done && fs_done {
            let flight = state.rendering.take().ok_or(EIO)?;
            for slot in flight.slots {
                self.retire_slot(state, slot);
            }
            if let Some(err) = flight.doomed {
                self.fail_doomed(state, &flight.packet, err);
                self.keep(&mut state.kept_renders, flight);
            } else if flight.last {
                flight.packet.retired();
            } else if !state.pending(&flight.packet) {
                flight.packet.release_guard();
            }
        } else if started.elapsed() > Delta::from_millis(COMMAND_TIMEOUT_MS) {
            dev_err!(self.dev, "GPU: render timed out: TA retired={} fragment retired={}\n", ta_done, fs_done);
            if g14_v27_2_abi::command_timeout_action(state.root.recovery_phase()?)
                == g14_v27_2_abi::TimeoutAction::Recover
            {
                // The firmware has a round open whose event was not seen. Recovery blames the
                // work it stopped and waits until that left its queue. Then retire again, so a
                // stopped render's fence is signalled and the queue behind it runs without
                // waiting for another message; an innocent render got its clock restarted.
                if self.recover(state, ETIMEDOUT, -1)? {
                    return self.retire_render(state);
                }
            }
            self.log_silent_hang(state, "render");
            return Err(ETIMEDOUT);
        }
        Ok(())
    }

    /// Fails a packet the firmware stopped, once none of its commands is in a queue: its
    /// unpublished commands are withdrawn, and its VM records the error (the client sees a
    /// lost context). Other clients' work is not touched.
    ///
    /// A stopped command left its queue without a completion stamp: that proves queue progress,
    /// not that the GPU no longer reaches its memory (the G16 rule, `Retirement::Aborted`). So
    /// its VM job guard is not released: the error completion quarantines it (no mapping
    /// changes), the packet stays in `State::kept` (`blame`), and the caller keeps the flight
    /// with its VM binding and scene (`keep`), all for the rest of the boot. Pool slots follow
    /// the normal rule: reused only after the firmware unregisters them (`free_slots`), and
    /// then in the slot's other buffer, so the stopped command stays intact.
    fn fail_doomed(&self, state: &mut State, packet: &Arc<Packet>, err: Error) {
        if state.live(packet) { return; }
        if let Some(i) = state.pending.iter().position(|(p, _)| Arc::ptr_eq(p, packet)) {
            let _ = state.pending.remove(i);
        }
        // Record it even if a racing scheduler timeout already claimed the fence (as fail_all).
        if let Some(status) = packet.vm.status() { status.record(err); }
        packet.finish(Err(err));
    }

    /// Keeps a stopped command that left its queue, with its VM binding and scene, for the rest
    /// of the boot (`State::kept_compute`, `kept_renders`). Its packet is already in
    /// `State::kept`, so ownership does not depend on this allocation. If it fails, the flight
    /// is leaked, which keeps it as well; it is never dropped.
    fn keep<T>(&self, list: &mut KVec<T>, flight: T) {
        if list.reserve(1, GFP_KERNEL).is_err() {
            dev_warn!(self.dev, "GPU: no memory to track a stopped command; leaking it\n");
            core::mem::forget(flight);
            return;
        }
        if let Err(kernel::alloc::kvec::PushError(flight)) = list.push_within_capacity(flight) {
            core::mem::forget(flight);
        }
    }

    /// Adds the packet of stopped work to `State::kept` before anything is marked, so it stays
    /// owned from here on. Err(EIO) once `g14_v27_2_abi::QUARANTINE_LIMIT` packets are kept:
    /// the caller then fails all work, and published work stays owned.
    fn reserve_kept(&self, state: &mut State, packet: &Arc<Packet>) -> Result {
        if state.kept(packet) { return Ok(()); }
        if !g14_v27_2_abi::quarantine_fits(state.kept.len()) {
            dev_err!(self.dev, "GPU: {} clients' stopped work kept this boot; not recovering more\n",
                     state.kept.len());
            return Err(EIO);
        }
        // Within the capacity allocated at start: never allocates.
        state.kept.push_within_capacity(packet.clone()).map_err(|_| EIO)
    }

    /// Marks the work the firmware's snapshot named (`g14_v27_2_abi::Attribution`): the
    /// published render pair, or the compute command with that completion stamp and the rest of
    /// its packet. Err(EIO) if that work is no longer tracked: the caller then fails all work.
    fn blame(&self, state: &mut State, error: Error, work: Stopped) -> Result<Blamed> {
        let blamed = match work {
            Stopped::Render => {
                let Some(packet) = state.rendering.as_ref().map(|f| f.packet.clone())
                    else { return Err(EIO); };
                self.reserve_kept(state, &packet)?;
                let Some(flight) = state.rendering.as_mut() else { return Err(EIO); };
                flight.doomed = Some(error);
                latch_guilty(&flight.packet, error);
                Blamed { owner: flight.packet.owner,
                         stamps: [flight.stamps[TILER].raw(), flight.stamps[FRAGMENT].raw()],
                         stopped: None }
            }
            Stopped::Compute(stamp) => {
                let flights = state.inflight.iter().map(|f| (f.stamp.raw(), f.ordinal));
                let Some((packet, stopped)) = g14_v27_2_abi::stopped_compute(flights, state.retired_total, stamp)
                    .map(|i| (state.inflight[i].packet.clone(), state.inflight[i].ordinal))
                    else { return Err(EIO); };
                self.reserve_kept(state, &packet)?;
                let owned = |f: &&mut Inflight| Arc::ptr_eq(&f.packet, &packet);
                for flight in state.inflight.iter_mut().filter(owned) {
                    flight.doomed.get_or_insert(error);
                }
                latch_guilty(&packet, error);
                Blamed { owner: packet.owner, stamps: [stamp, state.queue_stamp()],
                         stopped: Some(stopped) }
            }
        };
        Ok(blamed)
    }

    /// The last resort of a command timeout: the firmware has no recovery round open, so it
    /// reported nothing the host can answer. Logs what a row needs to tell a silent hang from a
    /// missed report: queue positions and recovery words (`log_timeout`), the MMU fault status,
    /// and the last event ring kinds with the report and round counts.
    fn log_silent_hang(&self, state: &State, work: &str) {
        state.root.log_timeout(&self.dev);
        state.fault.log(&self.dev);
        let (events, seen) = state.recent_events();
        let (reports, answered) = state.ledger.counts();
        dev_err!(self.dev, "GPU: {} timed out with no recovery round open: last events {:?} ({} read), {} reports, {} rounds answered (last {:?}); failing all work\n",
                 work, &events[..seen], state.events_read, reports, answered, state.ledger.last());
    }

    /// Whether the stopped work has left its queue: the render pair, or the compute command at
    /// `stopped` (its packet's later commands run normally and are failed as they retire).
    fn doomed_retired(&self, state: &mut State, stopped: Option<u64>) -> Result<bool> {
        if let Some(flight) = state.rendering.as_ref() {
            if flight.doomed.is_some() {
                return Ok(state.root.render_retired(TILER, flight.targets[TILER])?
                    && state.root.render_retired(FRAGMENT, flight.targets[FRAGMENT])?);
            }
        }
        let Some(ordinal) = stopped.or_else(||
            state.inflight.iter().rev().find(|f| f.doomed.is_some()).map(|f| f.ordinal))
            else { return Ok(true); };
        self.advance(state)?;
        Ok(state.retired_total >= ordinal)
    }

    /// Waits up to `ms` for the firmware's next recovery phase: 1 before the host answered,
    /// 3 with the same `generation` after it answered 2. Ok(None) on timeout; Err(EPROTO) on
    /// any phase the protocol does not allow (`recovery_logic::action`).
    fn wait_recovery(&self, state: &mut State, answered: bool, generation: u64, ms: i64)
        -> Result<Option<RecoveryAction>> {
        let start = Instant::<Monotonic>::now();
        loop {
            let phase = state.root.recovery_phase()?;
            let same = !answered || state.root.halt_count()? == generation;
            match recovery_logic::action(phase, answered, same) {
                RecoveryAction::Wait => {}
                RecoveryAction::ProtocolError => {
                    dev_err!(self.dev, "GPU: recovery phase {} (answered {}, same generation {})\n",
                             phase, answered, same);
                    return Err(EPROTO);
                }
                found => return Ok(Some(found)),
            }
            if start.elapsed() > Delta::from_millis(ms) { return Ok(None); }
            if state.peer.is_crashed() { return Err(EIO); }
            let _ = state.root.drain();
            fsleep(Delta::from_millis(1));
        }
    }

    /// The firmware recovery handshake after a fault or command timeout (the G16/M3 protocol).
    /// The firmware stops every queue and publishes phase 1 with a generation and a snapshot of
    /// the stopped work; the host marks that work (`blame`), clears the tiler storage of a
    /// stopped tiler, answers 2, waits for 3 with the same generation and answers 0. The stopped
    /// work then leaves its queue and fails only its own packet and VM; the next recoveries
    /// while it drains are answered the same way, a bounded number of times. Errors here fail
    /// all work (the runtime is not trusted). Ok(false) if the firmware opened no round within
    /// PHASE1_WAIT_MS: the caller decides whether its report was stale.
    fn recover(&self, state: &mut State, error: Error, slot: i32) -> Result<bool> {
        state.root.log_timeout(&self.dev);
        let mut rounds = 0;
        // A round the host already answered must not be answered again.
        let mut previous = state.ledger.last();
        let began = Instant::<Monotonic>::now();
        loop {
            // The state lock is held throughout: keep the whole recovery, including a last round
            // started at the budget, inside the DRM scheduler's job timeout.
            if began.elapsed() > Delta::from_millis(RECOVERY_BUDGET_MS) {
                dev_err!(self.dev, "GPU: recovery took more than {} ms\n", RECOVERY_BUDGET_MS);
                return Err(ETIMEDOUT);
            }
            if self.wait_recovery(state, false, 0, PHASE1_WAIT_MS)?.is_none() {
                // A further round starts at a phase 1 the drain saw: it cannot go missing.
                if rounds > 0 { return Err(EPROTO); }
                return Ok(false);
            }
            let now = <Monotonic as kernel::time::ClockSource>::ktime_get() as u64;
            if !recovery_logic::admit(now, &mut state.recovery_window, &mut state.recovery_count) {
                dev_err!(self.dev, "GPU: {} recoveries in {} s; not answering more\n",
                         recovery_logic::WINDOW_LIMIT, recovery_logic::WINDOW_NS / 1_000_000_000);
                return Err(EIO);
            }
            let generation = state.root.halt_count()?;
            if !g14_v27_2_abi::new_round(previous, generation) {
                dev_err!(self.dev, "GPU: recovery generation {} repeated\n", generation);
                return Err(EPROTO);
            }
            previous = Some(generation);
            // Retired work must not be blamed: refresh the retired position first.
            self.advance(state)?;
            let render = state.rendering.as_ref()
                .map(|f| [f.stamps[TILER].raw(), f.stamps[FRAGMENT].raw()]);
            let mut attribution = g14_v27_2_abi::Attribution::default();
            let mut entries = 0;
            let mut first = [(0u32, 0u32); 4];
            let mut stopped_tiler = false;
            for i in 0..gpu::RECOVERY_SNAPSHOT_ENTRIES {
                let (flags, target) = state.root.recovery_entry(i)?;
                let Some(entry) = SnapshotEntry::decode(flags, target) else { break; };
                entries += 1;
                stopped_tiler |= gpu::stopped_tiler(entry.flags);
                let retired = state.retired_total;
                let inflight = &state.inflight;
                attribution.add(entry.flags, entry.target, render, |stamp| {
                    let flights = inflight.iter().map(|f| (f.stamp.raw(), f.ordinal));
                    g14_v27_2_abi::stopped_compute(flights, retired, stamp).is_some()
                });
                if let Some(at) = first.get_mut(i) { *at = (entry.flags, entry.target); }
            }
            let shown = &first[..entries.min(first.len())];
            let (events, seen) = state.recent_events();
            // Every round is attributed: work behind the first stopped command can time out in
            // turn. Unnamed or ambiguous stopped work keeps the fail-stop.
            let Some(work) = attribution.result() else {
                dev_err!(self.dev, "GPU: recovery {} ({:?}, slot {}) names no single tracked command: snapshot ({} entries) {:x?}, events {:?}\n",
                         generation, error, slot, entries, shown, &events[..seen]);
                return Err(EIO);
            };
            let blamed = self.blame(state, error, work)?;
            let (what, second) = match work {
                Stopped::Render => ("render, tiler", "fragment"),
                Stopped::Compute(_) => ("compute", "queue at"),
            };
            dev_warn!(self.dev, "GPU: recovery {} ({:?}, slot {}, events {:?}): snapshot ({} entries) {:x?}; stopped {} {:#x} ({} {:#x}) of {}[{}]\n",
                      generation, error, slot, &events[..seen], entries, shown, what,
                      blamed.stamps[0], second, blamed.stamps[1],
                      blamed.owner.process(), blamed.owner.pid);
            if stopped_tiler {
                if let Some(flight) = state.rendering.as_ref() {
                    let ta_left = state.root.render_retired(TILER, flight.targets[TILER])?;
                    if !ta_left {
                        // SAFETY: the firmware is in recovery phase 1 with every queue stopped,
                        // so the GPU does not access the stopped tiler's TPC; the CPU reads it
                        // nowhere, and the state lock (held) serializes every render flight.
                        unsafe { flight.scene.clear_tpc()? };
                        dev_err!(self.dev, "GPU: cleared the stopped tiler's TPC storage\n");
                    }
                }
            }
            let start = Instant::<Monotonic>::now();
            state.root.answer_recovery(2)?;
            let mut reached = self.wait_recovery(state, true, generation, HANDSHAKE_MS / 2)?;
            if reached.is_none() {
                dev_warn!(self.dev, "GPU: no recovery phase 3 in {} ms; ringing the device-control doorbell\n",
                          HANDSHAKE_MS / 2);
                let _ = state.peer.as_mut().send_message(EP_DOORBELL, gpu::DEVICE_CONTROL_KICK);
                reached = self.wait_recovery(state, true, generation, HANDSHAKE_MS / 2)?;
            }
            if reached != Some(RecoveryAction::Resume) {
                return Err(ETIMEDOUT);
            }
            state.root.answer_recovery(0)?;
            state.ledger.answered(generation);
            dev_info!(self.dev, "GPU: recovery {} answered in {} ms\n",
                      generation, start.elapsed().as_millis());
            let start = Instant::<Monotonic>::now();
            loop {
                let _ = state.root.drain();
                if self.doomed_retired(state, blamed.stopped)? {
                    state.recoveries += 1;
                    // Work behind the stopped command waited out the recovery: restart its clocks.
                    let now = Instant::now();
                    for flight in state.inflight.iter_mut() { flight.started = now; }
                    if let Some(flight) = state.rendering.as_mut() { flight.started = now; }
                    dev_err!(self.dev,
                             "GPU: recovered ({} so far): stopped work left the queue after {} ms\n",
                             state.recoveries, start.elapsed().as_millis());
                    return Ok(true);
                }
                if state.peer.is_crashed() { return Err(EIO); }
                // Phase 0 = still draining; 1 = a new recovery (a new generation is checked
                // at the next round); the host's own phases mean the handshake was misread.
                match recovery_logic::action(state.root.recovery_phase()?, false, true) {
                    RecoveryAction::Wait => {}
                    RecoveryAction::MarkAndAcknowledge => break,
                    _ => {
                        dev_err!(self.dev, "GPU: recovery phase {} while the stopped work drained\n",
                                 state.root.recovery_phase()?);
                        return Err(EPROTO);
                    }
                }
                if start.elapsed() > Delta::from_millis(RECOVERY_DRAIN_MS) {
                    dev_err!(self.dev, "GPU: stopped work did not leave the queue\n");
                    state.root.log_timeout(&self.dev);
                    return Err(ETIMEDOUT);
                }
                fsleep(Delta::from_millis(1));
            }
            rounds += 1;
            dev_err!(self.dev, "GPU: firmware entered recovery again while the stopped work drained ({})\n",
                     rounds);
            if rounds > FURTHER_RECOVERIES { return Err(EIO); }
        }
    }

    /// Reads the queue's retired position into `retired_total`.
    fn advance(&self, state: &mut State) -> Result {
        let (_, raw) = state.root.progress()?;
        if state.capacity != 0 && raw < state.capacity {
            let delta = (raw + state.capacity - state.retired_raw) % state.capacity;
            state.retired_total += u64::from(delta);
            state.retired_raw = raw;
        }
        Ok(())
    }

    /// Retires completed commands, publishes more and consumes the firmware's rings.
    fn service(&self) {
        let mut guard = self.state.lock();
        let Some(state) = Option::as_mut(&mut *guard) else { return; };
        self.service_locked(state);
        if !state.running() && (state.pending.is_empty() || self.asleep.load(Ordering::Acquire)) {
            self.idle.notify_all();
        }
    }

    fn process_events(&self, state: &mut State) -> Result {
        // A fault and a timeout read in one pass are one report (`RecoveryLedger`).
        let mut reported = false;
        // Limit one pass to the ring capacity. Further arrivals notify the worker again.
        for _ in 0..256 {
            let Some((cursor, kind, event)) = state.root.peek_event()? else { break; };
            state.events[(state.events_read % EVENT_HISTORY as u64) as usize] = kind;
            state.events_read += 1;
            match event {
                FirmwareEvent::Grow(request) if request.halt_count == state.root.halt_count()? => {
                    let Some(reply) = state.root.reserve_control_reply()? else {
                        // Leave the GrowTVB request at the head of the event ring until
                        // there is space for its reply.
                        if FirmwareWait::kick_due(&mut state.waiting_control)? {
                            state.peer.as_mut().send_message(
                                EP_DOORBELL, gpu::DEVICE_CONTROL_KICK,
                            )?;
                        }
                        self.notify.retry();
                        return Ok(());
                    };
                    let granted = state.rendering.as_ref()
                        .filter(|flight| flight.bind.slot() == request.vm && flight.scene.slot() == request.buffer)
                        .and_then(|flight| flight.packet.render_queue.as_ref())
                        .is_some_and(|queue| queue.grow().unwrap_or(false));
                    reply.grow(request, granted)?;
                    state.peer.as_mut().send_message(EP_DOORBELL, gpu::DEVICE_CONTROL_KICK)?;
                }
                FirmwareEvent::ChannelError => {
                    dev_err!(self.dev, "GPU firmware rejected a channel record\n");
                    state.root.consume_event(cursor)?;
                    return Err(EIO);
                }
                FirmwareEvent::Timeout { slot } => {
                    dev_err!(self.dev, "GPU firmware timed out a command (slot {})\n", slot);
                    state.firmware_error.get_or_insert((ETIMEDOUT, slot));
                    if !core::mem::replace(&mut reported, true) { state.ledger.report(); }
                }
                FirmwareEvent::Fault => {
                    dev_err!(self.dev, "GPU firmware reported a GPU fault\n");
                    state.fault.log(&self.dev);
                    state.firmware_error.get_or_insert((EIO, -1));
                    if !core::mem::replace(&mut reported, true) { state.ledger.report(); }
                }
                FirmwareEvent::Grow(_) | FirmwareEvent::Notification => {}
            }
            state.waiting_control = None;
            state.root.consume_event(cursor)?;
        }
        Ok(())
    }

    fn service_locked(&self, state: &mut State) {
        let _ = state.root.drain();
        if !self.healthy() { return; }
        if state.peer.is_crashed() {
            dev_err!(self.dev, "GPU firmware crashed; failing {} pending and {} published commands\n",
                     state.pending.len(), state.inflight.len());
            self.fail_all(state, EIO);
            return;
        }
        if let Err(err) = self.process_events(state) {
            dev_err!(self.dev, "GPU: event service failed: {:?}\n", err);
            self.fail_all(state, err);
            return;
        }
        if let Some((error, slot)) = state.firmware_error.take() {
            match self.recover(state, error, slot) {
                Ok(true) => {}
                // The host answered this report's round before it read the report (a round
                // opened while stopped work drained, or found at a command timeout).
                Ok(false) if state.ledger.report_answered() => {
                    dev_warn!(self.dev, "GPU: firmware report {:?} (slot {}) is for recovery {:?}, already answered\n",
                              error, slot, state.ledger.last());
                }
                Ok(false) => {
                    dev_err!(self.dev, "GPU: firmware reported {:?} (slot {}) but did not enter recovery\n",
                             error, slot);
                    self.fail_all(state, ETIMEDOUT);
                    return;
                }
                Err(err) => {
                    dev_err!(self.dev, "GPU: recovery failed ({:?}); failing all work\n", err);
                    self.fail_all(state, err);
                    return;
                }
            }
        }
        if let Err(err) = self.retire_render(state) { self.fail_all(state, err); return; }
        if let Err(e) = self.advance(state) { self.fail_all(state, e); return; }
        // The completion stamp is written before the firmware's post-processing advances the
        // retired position, and that advance sends no message. If the front command's stamp
        // is there but its position is not, the advance is imminent: wait for it, bounded,
        // rather than for a notification that does not come.
        if let Some((ordinal, stamp)) = state.inflight.first().map(|f| (f.ordinal, f.stamp)) {
            if state.retired_total < ordinal && state.root.queue().1 >= stamp {
                state.late += 1;
                let start = Instant::<Monotonic>::now();
                while state.retired_total < ordinal && start.elapsed() < Delta::from_micros(RETIRE_WAIT_US) {
                    fsleep(Delta::from_micros(20));
                    if let Err(e) = self.advance(state) { self.fail_all(state, e); return; }
                }
            }
        }
        while !state.inflight.is_empty() {
            let front = &state.inflight[0];
            let (ordinal, started, expected) = (front.ordinal, front.started, front.stamp.raw());
            if state.retired_total >= ordinal {
                if let Ok(done) = state.inflight.remove(0) {
                    self.retire_slot(state, done.slot);
                    if let Some(err) = done.doomed {
                        self.fail_doomed(state, &done.packet, err);
                        self.keep(&mut state.kept_compute, done);
                    } else if let Some(err) = done.publication_error {
                        done.packet.release_guard();
                        done.packet.finish(Err(err));
                    } else if done.last {
                        done.packet.retired();
                    } else if !state.owns(&done.packet) && !state.pending(&done.packet) {
                        done.packet.release_guard();
                    }
                }
                continue;
            }
            if started.elapsed() > Delta::from_millis(COMMAND_TIMEOUT_MS) {
                dev_err!(self.dev, "GPU: compute command timed out: stamp={:#x} expected={:#x} submitted={} completed={}\n",
                         state.queue_stamp(), expected, state.submitted, state.retired);
                let phase = state.root.recovery_phase().unwrap_or(0);
                if g14_v27_2_abi::command_timeout_action(phase) == g14_v27_2_abi::TimeoutAction::Recover {
                    // The firmware has a round open whose event was not seen. Recovery blames
                    // the command it names by stamp, which need not be this front one, and
                    // fails only that client; the others' work resumes after phase 0.
                    match self.recover(state, ETIMEDOUT, -1) {
                        Ok(true) => continue,
                        Ok(false) => {}
                        Err(err) => {
                            dev_err!(self.dev, "GPU: recovery failed ({:?}); failing all work\n", err);
                            self.fail_all(state, err);
                            return;
                        }
                    }
                }
                self.log_silent_hang(state, "compute");
                self.fail_all(state, ETIMEDOUT);
                return;
            }
            break;
        }
        if !self.healthy() {
            state.release_gpu_busy_if_idle();
            return;
        }
        self.pump(state);
        state.release_gpu_busy_if_idle();
    }

    /// Rolls back a failed deep-sleep prepare. If an allow-off policy record was published,
    /// keep-powered must be confirmed before reopening submissions; otherwise the backend is
    /// failed rather than resuming with an unconfirmed idle policy.
    #[cfg(CONFIG_APPLE_SOC_SLEEP)]
    fn abort_deep_prepare(&self, state: &mut State, policy_published: bool, err: Error) -> Error {
        if policy_published {
            if let Err(rb_err) = set_idle_policy(&state.root, &mut state.peer, false) {
                dev_err!(
                    self.dev,
                    "GPU: deep sleep rollback failed to restore keep-powered: {:?}\n",
                    rb_err
                );
                self.fail_all(state, rb_err);
                self.notify.set_deep_sleeping(self, false);
                self.asleep.store(false, Ordering::Release);
                return err;
            }
        }
        if state.idle_off_allowed {
            set_gpu_auto_pm(self.dev.as_ref(), true);
        }
        self.notify.set_deep_sleeping(self, false);
        self.asleep.store(false, Ordering::Release);
        if self.healthy() {
            self.pump(state);
            self.notify.wake();
        }
        err
    }

    /// Before a system sleep: holds new publications and lets published work retire.
    ///
    /// For suspend-to-idle (`!deep`), the firmware keeps running with its keep-powered policy
    /// (`apple,sleep-retained`). For a firmware-driven deep sleep (`deep`), workers are drained,
    /// idle power-off is permitted, and a commit-time stop is registered with the SoC sleep
    /// driver so the firmware is idled and halted only after all refusals have passed, then
    /// warm-restarted in [`sleep_complete`].
    pub(crate) fn sleep_prepare(&self, deep: bool) -> Result {
        let mut guard = self.state.lock();
        if !self.healthy() || guard.is_none() { return Ok(()); }
        if self.deep_sleeping.load(Ordering::Acquire) {
            return Err(EBUSY);
        }
        // Hold publications first, then let what is published retire. User space is frozen by
        // now; only the scheduler's queued jobs can still arrive, and they wait in `pending`.
        self.asleep.store(true, Ordering::Release);
        let mut remaining = msecs_to_jiffies(STOP_DRAIN_MS);
        loop {
            let Some(state) = Option::as_mut(&mut *guard) else { return Ok(()); };
            if !self.healthy() { return Ok(()); }
            if !state.running() {
                // No recovery round crosses a sleep: a round open now (the firmware stopped
                // without anything of ours running) is answered by the service first.
                let phase = state.root.recovery_phase().unwrap_or(0);
                if phase != 0 {
                    dev_err!(self.dev, "GPU: sleep refused, firmware recovery phase {}\n", phase);
                    self.asleep.store(false, Ordering::Release);
                    self.pump(state);
                    self.notify.wake();
                    return Err(EBUSY);
                }
                break;
            }
            if remaining == 0 {
                dev_err!(self.dev, "GPU: sleep refused, {} compute commands and {} render pairs still running\n",
                         state.inflight.len(), usize::from(state.rendering.is_some()));
                self.asleep.store(false, Ordering::Release);
                self.pump(state);
                return Err(EBUSY);
            }
            remaining = match self.idle.wait_timeout(&mut guard, remaining) {
                CondVarTimeoutResult::Woken { jiffies } => jiffies,
                _ => 0,
            };
        }
        if !deep {
            if let Some(state) = Option::as_ref(&*guard) {
                dev_info!(self.dev, "GPU held for sleep: submitted={} completed={} pending={}\n",
                          state.submitted, state.retired, state.pending.len());
            }
            return Ok(());
        }
        #[cfg(not(CONFIG_APPLE_SOC_SLEEP))]
        {
            if let Some(state) = Option::as_mut(&mut *guard) {
                self.asleep.store(false, Ordering::Release);
                self.pump(state);
            }
            return Err(EBUSY);
        }
        #[cfg(CONFIG_APPLE_SOC_SLEEP)]
        {
            let has_pwr_controller =
                Option::as_ref(&*guard).is_some_and(|s| s.root.has_pwr_controller());
            // SAFETY: Reads global sleep-driver availability and suspend mode.
            if !has_pwr_controller || !unsafe { apple_sleep_commit_stops_coprocs() } {
                if let Some(state) = Option::as_mut(&mut *guard) {
                    self.asleep.store(false, Ordering::Release);
                    self.pump(state);
                }
                return Err(EBUSY);
            }
            // Close Notify admission under its spinlock before dropping `state` and draining.
            self.notify.set_deep_sleeping(self, true);
            drop(guard);
            // Flush any queued retry or service work; `deep_sleeping` keeps new work out.
            // SAFETY: `self.retry` is an initialized, pinned delayed work item.
            unsafe {
                let work = Work::raw_get(DelayedWork::raw_as_work(&self.retry));
                let delayed = kernel::container_of!(work, kernel::bindings::delayed_work, work);
                kernel::bindings::flush_delayed_work(delayed);
            }
            self.queue.drain();

            let mut guard = self.state.lock();
            let Some(state) = Option::as_mut(&mut *guard) else {
                self.notify.set_deep_sleeping(self, false);
                return Ok(());
            };
            if !self.healthy() || state.running() {
                return Err(self.abort_deep_prepare(state, false, EBUSY));
            }
            let mut policy_published = false;
            if !state.idle_off_allowed {
                match try_set_idle_policy(&state.root, &mut state.peer, true) {
                    Ok(()) => policy_published = true,
                    Err((published, err)) => {
                        dev_err!(self.dev, "GPU: deep sleep refused, allow-off failed: {:?}\n", err);
                        return Err(self.abort_deep_prepare(state, published, err));
                    }
                }
            }
            if let Err(err) = wait(&state.root, 500, || Ok(gpu_powered_off(state.asc.fender()))) {
                dev_err!(
                    self.dev,
                    "GPU: deep sleep refused, GPU did not power off (fender={:#x}): {:?}\n",
                    state.asc.fender(),
                    err
                );
                return Err(self.abort_deep_prepare(state, policy_published, err));
            }
            if state.idle_off_allowed {
                set_gpu_auto_pm(self.dev.as_ref(), false);
            }
            // SAFETY: `ASC_CPU_CONTROL` lies within the mapped ASC page.
            let asc_ctl = unsafe { state.asc.0.add(ASC_CPU_CONTROL) };
            let commit = match KBox::new(CommitStop {
                rtk: state.peer.as_raw(),
                asc_ctl,
                ran: false,
                idle_ret: 0,
                idle_us: 0,
            }, GFP_KERNEL) {
                Ok(c) => c,
                Err(err) => return Err(self.abort_deep_prepare(state, policy_published, err.into())),
            };
            state.commit_stop = Some(commit);
            let data = state.commit_stop.as_deref_mut().map_or(core::ptr::null_mut(), |c| {
                core::ptr::from_mut(c).cast::<core::ffi::c_void>()
            });
            // SAFETY: `dev` is the live GPU device; `data` points to `state.commit_stop` which
            // remains allocated at the same address until unregistered in `sleep_complete`/`stop`.
            let ret = unsafe {
                apple_sleep_coproc_register_first(
                    self.dev.as_raw(),
                    Some(g14_v27_2_commit_stop),
                    data,
                )
            };
            if ret != 0 {
                state.commit_stop = None;
                return Err(self.abort_deep_prepare(state, policy_published, Error::from_errno(ret)));
            }
            // SAFETY: `dev` was just registered above.
            unsafe { apple_sleep_coproc_set_ready(self.dev.as_raw(), true) };
            dev_info!(self.dev, "GPU prepared for deep sleep: submitted={} completed={} pending={}\n",
                      state.submitted, state.retired, state.pending.len());
            Ok(())
        }
    }

    /// After the sleep: warm-restarts the firmware after a real deep sleep, then publishes held work.
    pub(crate) fn sleep_complete(&self) {
        let mut guard = self.state.lock();
        let Some(state) = Option::as_mut(&mut *guard) else {
            #[cfg(CONFIG_APPLE_SOC_SLEEP)]
            self.notify.set_deep_sleeping(self, false);
            self.asleep.store(false, Ordering::Release);
            return;
        };
        #[cfg(CONFIG_APPLE_SOC_SLEEP)]
        if let Some(commit) = state.commit_stop.take() {
            // SAFETY: `dev` was registered in `sleep_prepare`; `commit` stays live until after
            // `apple_sleep_coproc_unregister` completes.
            let stopped = commit.ran
                || unsafe { apple_sleep_coproc_stopped(self.dev.as_raw()) };
            // SAFETY: `dev` is still registered until the unregister below.
            let wake_err = unsafe { apple_sleep_coproc_wake_error(self.dev.as_raw()) };
            // SAFETY: Matches `apple_sleep_coproc_register_first` in `sleep_prepare`.
            unsafe { apple_sleep_coproc_unregister(self.dev.as_raw()) };
            // SAFETY: Reads a global flag word.
            let real = unsafe { kernel::bindings::pm_resume_via_firmware() };
            if !stopped && !real {
                if !state.idle_off_allowed {
                    dev_info!(self.dev, "GPU: deep sleep refused before commit; restoring keep-powered\n");
                    if let Err(err) = set_idle_policy(&state.root, &mut state.peer, false) {
                        dev_err!(self.dev, "GPU: failed to restore keep-powered policy: {:?}\n", err);
                        self.fail_all(state, err);
                    }
                } else {
                    dev_info!(self.dev, "GPU: deep sleep refused before commit\n");
                    set_gpu_auto_pm(self.dev.as_ref(), true);
                }
                self.notify.set_deep_sleeping(self, false);
                self.asleep.store(false, Ordering::Release);
                state.waiting_unlink = None;
                if self.healthy() {
                    self.pump(state);
                    self.notify.wake();
                }
                return;
            }
            if !real {
                dev_warn!(self.dev, "GPU: sleep refused after commit stop; firmware stays stopped\n");
                state.hardware_unreachable = true;
                self.notify.set_deep_sleeping(self, false);
                self.asleep.store(false, Ordering::Release);
                self.fail_all(state, EIO);
                return;
            }
            if commit.idle_ret != 0 || wake_err != 0 {
                dev_err!(
                    self.dev,
                    "GPU: skipping warm restart after commit/wake error (idle_ret={} wake_err={})\n",
                    commit.idle_ret,
                    wake_err
                );
                state.hardware_unreachable = true;
                self.notify.set_deep_sleeping(self, false);
                self.asleep.store(false, Ordering::Release);
                self.fail_all(state, EIO);
                return;
            }
            let started = Instant::<Monotonic>::now();
            let ctl_before = state.asc.read(ASC_CPU_CONTROL);
            let restart = (|| -> Result {
                // SAFETY: `peer` is live and the firmware was stopped at the commit.
                kernel::error::to_result(unsafe {
                    kernel::bindings::apple_rtkit_warm_restart(state.peer.as_raw())
                })?;
                state.asc.write(ASC_CPU_CONTROL, ctl_before | ASC_RUN);
                state.peer.as_mut().boot()?;
                set_idle_policy(&state.root, &mut state.peer, false)?;
                wait(&state.root, 500, || Ok(gpu_powered(state.asc.fender())))?;
                if state.idle_off_allowed {
                    set_idle_policy(&state.root, &mut state.peer, true)?;
                    set_gpu_auto_pm(self.dev.as_ref(), true);
                }
                Ok(())
            })();
            self.notify.set_deep_sleeping(self, false);
            self.asleep.store(false, Ordering::Release);
            state.waiting_unlink = None;
            state.waiting_control = None;
            match restart {
                Ok(()) => {
                    // No round crosses a sleep (sleep_prepare refuses one), and the restarted
                    // firmware may count generations again from its start: forget old rounds.
                    state.ledger = g14_v27_2_abi::RecoveryLedger::default();
                    dev_info!(self.dev, "GPU firmware 27.2 back in {} us (commit idle={} {} us, pending={})\n",
                              started.elapsed().as_micros_ceil(), commit.idle_ret, commit.idle_us,
                              state.pending.len());
                    if self.healthy() {
                        self.pump(state);
                        self.notify.wake();
                    }
                }
                Err(err) => {
                    dev_err!(self.dev, "GPU firmware 27.2 warm restart failed: {:?} (crashed={})\n",
                             err, state.peer.is_crashed());
                    self.fail_all(state, err);
                }
            }
            return;
        }
        if !self.asleep.swap(false, Ordering::AcqRel) { return; }
        dev_info!(self.dev, "GPU released after sleep: running={:?} pending={}\n",
                  state.root.firmware_running(), state.pending.len());
        state.waiting_unlink = None;
        if self.healthy() { self.pump(state); }
    }

    /// Stops the firmware: retire or fail outstanding work, allow power-off, then halt the ASC
    /// with every mapping still in place (RTKit power-off if the firmware does not stop).
    pub(crate) fn stop(&self) {
        // Close admission while the service retires work already accepted.
        {
            let mut guard = self.state.lock();
            let Some(state) = Option::as_mut(&mut *guard) else { return; };
            state.accepting = false;
            let start = Instant::<Monotonic>::now();
            while self.healthy() && start.elapsed() < Delta::from_millis(STOP_DRAIN_MS.into()) {
                let Some(state) = guard.as_ref() else { break; };
                if !state.running() && state.pending.is_empty() { break; }
                let _ = self.idle.wait_timeout(&mut guard, msecs_to_jiffies(STOP_DRAIN_MS));
            }
            // Serialize the last permitted publication with the transition to shutdown.
            self.healthy.store(false, Ordering::Release);
        }
        // Detach prevents new enqueues. Flush the delayed item before draining the queue,
        // since a pending timer may not have queued its work yet.
        drop(self.notify.detach());
        // SAFETY: This is the initialized, pinned retry item. It cannot rearm after detach.
        unsafe {
            let work = Work::raw_get(DelayedWork::raw_as_work(&self.retry));
            let delayed = kernel::container_of!(work, kernel::bindings::delayed_work, work);
            kernel::bindings::flush_delayed_work(delayed);
        }
        self.queue.drain();
        let mut guard = self.state.lock();
        let Some(mut state) = guard.take() else { return; };
        #[cfg(CONFIG_APPLE_SOC_SLEEP)]
        if let Some(_commit) = state.commit_stop.take() {
            // SAFETY: Matches `apple_sleep_coproc_register_first` in `sleep_prepare`; PM
            // unbind/shutdown does not run concurrently with the IRQ-off commit, and `_commit`
            // stays alive until after unregister completes.
            unsafe { apple_sleep_coproc_unregister(self.dev.as_raw()) };
        }
        self.fail_all(&mut state, ENODEV);
        if state.hardware_unreachable {
            dev_info!(
                self.dev,
                "GPU stopped: skipping hardware teardown after failed sleep/wake (submitted={} completed={} notifications={})\n",
                state.submitted,
                state.retired,
                self.notify.messages.load(Ordering::Relaxed)
            );
        } else {
            if state.idle_off_allowed {
                set_gpu_auto_pm(self.dev.as_ref(), false);
            }
            let allowed = if state.idle_off_allowed {
                Ok(())
            } else {
                set_idle_policy(&state.root, &mut state.peer, true)
            };
            let root = &state.root;
            let _ = wait(root, 500, || Ok(root.firmware_running()? == 0));
            let running = root.firmware_running();
            let forced = (running != Ok(0)).then(|| {
                // SAFETY: the RTKit instance is live and owned here.
                kernel::error::to_result(unsafe { kernel::bindings::apple_rtkit_poweroff(state.peer.as_raw()) })
            });
            state.asc.halt();
            dev_info!(self.dev, "GPU stopped: allow-off={:?} running={:?} forced-off={:?} submitted={} completed={} notifications={} late={} skipped-slots={} deferred-cursors={}\n",
                      allowed, running, forced, state.submitted, state.retired,
                      self.notify.messages.load(Ordering::Relaxed), state.late, state.skipped, state.deferred);
            if running != Ok(0) && forced != Some(Ok(())) {
                dev_err!(self.dev, "GPU firmware did not stop; a reset is required\n");
            }
        }
        if state.gpu_busy_held {
            pmgr_gpu_busy_put();
            state.gpu_busy_held = false;
        }
        pmgr_gpu_tracking_stop();
        // Keep everything the firmware could reach for the rest of the boot, halted or not:
        // its page tables still name these pages, and the boot handoff window that start()
        // keeps for the life of the boot still references the graph's tables. Releasing them
        // needs that window torn down first, which this runtime does not do.
        core::mem::forget(state);
        core::mem::forget(self.graph.clone());
    }
}

impl State {
    fn queue_stamp(&self) -> u32 {
        self.root.queue().1.raw()
    }
}

pub(crate) struct Backend {
    graph: Arc<GpuManagerG14V13_5>,
    runtime: Arc<Runtime>,
    scheduler: Arc<sched::Scheduler<crate::m3::m3_submit::Job>>,
    ids: gpu::SequenceIDs,
    preempt_bytes: usize,
}

impl Backend {
    /// Takes over the running firmware after the boot job.
    #[allow(clippy::too_many_arguments)]
    fn start(
        drm: &crate::driver::AsahiDrmDevice,
        dev: &device::Device,
        notify: Arc<Notify>,
        peer: Peer,
        root: crate::fw::types::GpuObject<RootV27_2>,
        graph: Arc<GpuManagerG14V13_5>,
        pool: CommandPool,
        asc: AscControl,
        fault: FaultStatus,
        cfg: &'static hw::HwConfig,
    ) -> Result<Arc<Self>> {
        // The runtime is created empty first, so no allocation failure can drop firmware-visible
        // state. Every failure after the state is moved in goes through Runtime::stop, which
        // halts the firmware and keeps all of its memory.
        let runtime_graph = graph.clone();
        let runtime_notify = notify.clone();
        let runtime = match OwnedQueue::new_ordered(c"asahi-g14-v27-2").and_then(|queue|
            Arc::pin_init(try_pin_init!(Runtime {
                dev: dev.into(),
                graph: runtime_graph,
                healthy: AtomicBool::new(true),
                asleep: AtomicBool::new(false),
                deep_sleeping: AtomicBool::new(false),
                notify: runtime_notify,
                queue,
                state <- new_mutex!(None, "g14_v27_2::Runtime::state"),
                idle <- new_condvar!("g14_v27_2::Runtime::idle"),
                work <- new_work!("g14_v27_2::Runtime::work"),
                retry <- new_delayed_work!("g14_v27_2::Runtime::retry"),
            }), GFP_KERNEL)) {
            Ok(runtime) => runtime,
            Err(e) => {
                halt_unowned(dev, peer, root, asc, true);
                core::mem::forget(pool);
                core::mem::forget(graph);
                return Err(e);
            }
        };
        let (queue, last) = root.queue();
        let checked = (|| -> Result<(u32, u32, KVec<Arc<Packet>>)> {
            let (head, retired) = root.progress()?;
            if head != retired { return Err(EBUSY); }
            let kept = KVec::with_capacity(g14_v27_2_abi::QUARANTINE_LIMIT, GFP_KERNEL)?;
            Ok((head, root.capacity()?, kept))
        })();
        let (head, capacity, kept) = match checked {
            Ok(v) => v,
            Err(e) => {
                halt_unowned(dev, peer, root, asc, true);
                core::mem::forget(pool);
                core::mem::forget(graph);
                return Err(e);
            }
        };
        let idle_off_allowed = root.has_pwr_controller();
        pmgr_gpu_tracking_start();
        *runtime.state.lock() = Some(State {
            peer, root, pool, asc, fault, queue, last,
            sequence: u64::from(head),
            next_slot: 0,
            waiting_unlink: None,
            waiting_control: None,
            published: 0,
            late: 0,
            busy: 0,
            skipped: 0,
            unacked: 0,
            deferred: 0,
            retired_total: 0,
            retired_raw: head,
            capacity,
            inflight: KVec::new(),
            rendering: None,
            pending: KVec::new(),
            accepting: true,
            submitted: 0,
            retired: 0,
            idle_off_allowed,
            gpu_busy_held: false,
            firmware_error: None,
            recoveries: 0,
            recovery_window: 0,
            recovery_count: 0,
            ledger: g14_v27_2_abi::RecoveryLedger::default(),
            events: [0; EVENT_HISTORY],
            events_read: 0,
            kept,
            kept_compute: KVec::new(),
            kept_renders: KVec::new(),
            hardware_unreachable: false,
            #[cfg(CONFIG_APPLE_SOC_SLEEP)]
            commit_stop: None,
        });
        // From here the firmware's messages run the service; one run consumes anything it
        // published before.
        notify.attach(runtime.clone());
        notify.wake();
        // One run queue per DRM scheduler priority (the front end maps queue priorities 0-3).
        let scheduler = match sched::Scheduler::new(drm.as_ref().as_ref(), 4, POOL_SLOTS as u32 - 1, 0, 10000,
            c"asahi_g14_sched").and_then(|s| Arc::new(s, GFP_KERNEL).map_err(Into::into)) {
            Ok(scheduler) => scheduler,
            Err(e) => {
                runtime.stop();
                return Err(e);
            }
        };
        let preempt_bytes = cfg.compute_preempt1_size + 0x20;
        match Arc::new(Self { graph, runtime: runtime.clone(), scheduler, ids: gpu::SequenceIDs::default(), preempt_bytes },
                       GFP_KERNEL) {
            Ok(backend) => Ok(backend),
            Err(e) => { runtime.stop(); Err(e.into()) }
        }
    }

    pub(crate) fn stop(&self) { self.runtime.stop(); }
    pub(crate) fn sleep_prepare(&self, deep: bool) -> Result { self.runtime.sleep_prepare(deep) }
    pub(crate) fn sleep_complete(&self) { self.runtime.sleep_complete() }
}

impl Drop for Backend {
    fn drop(&mut self) { self.runtime.stop(); }
}

impl gpu::Gpu for Backend {
    fn stats_snapshot(&self) -> Option<Arc<crate::stats::StatsSnapshot>> { None }
    fn as_any(&self) -> &dyn core::any::Any { self }
    fn arc_as_any(self: Arc<Self>) -> Arc<dyn core::any::Any + Sync + Send> { self as Arc<dyn core::any::Any + Sync + Send> }
    fn manager(&self) -> Option<&dyn gpu::GpuManager> { None }
    fn init(&self) -> Result { if self.runtime.healthy() { Ok(()) } else { Err(ENODEV) } }
    fn ids(&self) -> &gpu::SequenceIDs { &self.ids }
    fn is_crashed(&self) -> bool { !self.runtime.healthy() }
    fn get_params(&self, params: &mut uapi::drm_asahi_params_global) -> Result {
        <GpuManagerG14V13_5 as gpu::Gpu>::get_params(&self.graph, params)?;
        params.max_commands_per_submission = crate::m3::agx_uapi::UAPI_MAX_HARDWARE_COMMANDS;
        params.max_attachments = crate::m3::agx_uapi::UAPI_MAX_ATTACHMENTS as u32;
        Ok(())
    }
    fn supports_vm_status(&self) -> bool { true }
    fn feature_flags(&self) -> u64 {
        uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_VM_STATUS as u64
            | uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_SCHEDULED_QUEUES as u64
    }
    fn tracked_submission(&self) -> bool { true }
    fn base_clock_hz(&self) -> u32 { <GpuManagerG14V13_5 as gpu::Gpu>::base_clock_hz(&self.graph) }
    fn uat_geometry(&self) -> mmu::UatGeometry { <GpuManagerG14V13_5 as gpu::Gpu>::uat_geometry(&self.graph) }
    fn new_vm(&self, range: Range<u64>) -> Result<mmu::Vm> {
        self.graph.kernel_uat().new_vm(self.ids.vm.next(), range)?.with_tracked_status()
    }
    fn new_queue(&self, vm: mmu::Vm, ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>, priority: u32,
        usc_exec_base: u64, _usage: Arc<crate::usage::Client>) -> Result<KBox<dyn queue::Queue>> {
        let render = Arc::new(render::QueueRender::new(self.graph.clone(), ualloc, ualloc_priv.clone())?,
                              GFP_KERNEL)?;
        Ok(KBox::new(crate::m3::m3_submit::Queue::new_g14_v27_2(self.runtime.clone(), self.scheduler.clone(),
            vm, priority, usc_exec_base, ualloc_priv, self.preempt_bytes, render)?, GFP_KERNEL)?)
    }
    fn timestamp_window_size(&self) -> u64 {
        <GpuManagerG14V13_5 as gpu::Gpu>::timestamp_window_size(&self.graph)
    }
    fn map_timestamp_buffer(
        &self,
        bo: gem::ObjectRef,
        range: Range<usize>,
        window: crate::timestamp::WindowPermit,
    ) -> Result<crate::timestamp::Buffer> {
        // The aperture's tables were linked before the firmware window was mirrored (start()).
        <GpuManagerG14V13_5 as gpu::Gpu>::map_timestamp_buffer(&self.graph, bo, range, window)
    }
}
