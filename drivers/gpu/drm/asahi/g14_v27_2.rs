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

/// RTKit endpoints of the GPU firmware.
const EP_FIRMWARE: u8 = 0x20;
const EP_DOORBELL: u8 = 0x21;

/// ASC CPU control register and its run bit.
const ASC_CPU_CONTROL: usize = 0x44;
const ASC_RUN: u32 = 0x10;

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

/// The ASC control page, kept mapped until the firmware is stopped.
pub(crate) struct AscControl(*mut u8);
// SAFETY: a device register page; accesses are single 32-bit MMIO operations.
unsafe impl Send for AscControl {}
// SAFETY: as above.
unsafe impl Sync for AscControl {}

impl AscControl {
    fn new(phys: u64) -> Result<Self> {
        // SAFETY: `phys` is the ASC register window of this platform device.
        let ptr = unsafe { kernel::bindings::ioremap(phys, 0x4000) };
        if ptr.is_null() { return Err(ENOMEM); }
        Ok(Self(ptr.cast()))
    }
    fn read(&self, at: usize) -> u32 {
        // SAFETY: `at` is a register offset inside the mapped page.
        unsafe { kernel::bindings::readl(self.0.add(at).cast()) }
    }
    fn write(&self, at: usize, value: u32) {
        // SAFETY: as above.
        unsafe { kernel::bindings::writel(value, self.0.add(at).cast()) }
    }
    /// Clears the ASC run bit.
    fn halt(&self) {
        self.write(ASC_CPU_CONTROL, self.read(ASC_CPU_CONTROL) & !ASC_RUN);
    }
}

impl Drop for AscControl {
    fn drop(&mut self) {
        // SAFETY: mapped in `new`, unmapped once.
        unsafe { kernel::bindings::iounmap(self.0.cast()) }
    }
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
        let page = unsafe { kernel::bindings::ioremap(sgx + base as u64, 0x1000) };
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
            let _ = runtime.queue.enqueue::<Arc<Runtime>, 0>(runtime.clone());
        }
    }

    /// Queues a poll while attached. Detaching also fences off poll requests before teardown.
    fn retry(&self) {
        if let Some(runtime) = self.runtime.lock().as_ref() {
            if runtime.healthy() {
                let _ = runtime.queue.enqueue_delayed::<Arc<Runtime>, RETRY_WORK>(
                    runtime.clone(), msecs_to_jiffies(RESOURCE_RETRY_MS),
                );
            }
        }
    }

    fn attach(&self, runtime: Arc<Runtime>) {
        *self.runtime.lock() = Some(runtime);
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
/// acknowledged it.
fn set_idle_policy(root: &RootV27_2, peer: &mut Peer, allow_off: bool) -> Result {
    let (target, generation) = root.publish_idle_policy(allow_off)?;
    peer.as_mut().send_message(EP_DOORBELL, gpu::DEVICE_CONTROL_KICK)?;
    wait(root, 500, || Ok(root.idle_progress()? == [target; 3] && root.idle_acknowledgement()? == generation))
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
    let asc = AscControl::new(pdev.resource_by_name(c"asc").ok_or(EINVAL)?.start())?;
    let control = asc.read(ASC_CPU_CONTROL);
    if control & ASC_RUN != 0 { return Err(EBUSY); }
    let notify = Notify::new(dev)?;
    let mut peer = KBox::pin(
        rtkit::RtKit::<Firmware>::new(dev, None, 0, notify.clone())?,
        GFP_KERNEL)?;
    let resources = crate::regs::Resources::new(pdev)?;
    let sgx = pdev.resource_by_name(c"sgx").ok_or(EINVAL)?;
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
        wait(&root, 3000, || root.boot_job_complete())
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

struct Inflight {
    packet: Arc<Packet>,
    last: bool,
    publication_error: Option<Error>,
    stamp: event::EventValue,
    slot: usize,
    /// Queue publications up to and including this command, counted from take-over.
    ordinal: u64,
    started: Instant<Monotonic>,
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
            packet.release_guard();
            packet.finish(Err(err));
        }
    }

    fn running(&self) -> bool {
        !self.inflight.is_empty() || self.rendering.is_some()
    }

    fn owns(&self, packet: &Arc<Packet>) -> bool {
        self.inflight.iter().any(|flight| Arc::ptr_eq(&flight.packet, packet))
            || self.rendering.as_ref().is_some_and(|flight| Arc::ptr_eq(&flight.packet, packet))
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
            let command = packet.commands[index];
            let paired = matches!(command, Command::Render { .. });
            if paired && state.running() { return; }
            let count = if paired { 2 } else { 1 };
            let slots = match self.free_slots(state, count) {
                Ok(Some(slots)) => slots,
                Ok(None) => return,
                Err(err) => {
                    dev_err!(self.dev, "GPU: command pool stalled: {:?}\n", err);
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
        };
        let command = state.pool.encode(slot, &state.queue, stamp.raw(), &job)?;
        let submission = state.root.stage_compute(command)?;
        let ordinal = state.published + 1;
        // Commit ownership before the firmware can see the new work.
        state.inflight.push(Inflight { packet: packet.clone(), last, stamp, slot, ordinal,
            publication_error: None, started: Instant::now(), _bind: bind }, GFP_KERNEL)?;
        packet.mark_published();
        submission.publish();
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
        let ta_post = state.root.stage_render(TILER, &ta_work[usize::from(!with_buffer)..])?;
        let fs_post = state.root.stage_render(FRAGMENT, &fs_work)?;
        state.rendering = Some(RenderFlight {
            packet: packet.clone(), last, bind, scene, slots, stamps,
            targets: [ta_post.target(), fs_post.target()], started: Instant::now(),
        });
        queue.published();
        packet.mark_published();
        // Queue the fragment dependency before kicking the tiler that satisfies it.
        fs_post.publish();
        ta_post.publish();
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
        let mut ta_done = state.root.render_done(TILER, flight.targets[TILER], flight.stamps[TILER])?;
        let mut fs_done = state.root.render_done(FRAGMENT, flight.targets[FRAGMENT], flight.stamps[FRAGMENT])?;
        if !(ta_done && fs_done)
            && state.root.render_queue(TILER).1 >= flight.stamps[TILER]
            && state.root.render_queue(FRAGMENT).1 >= flight.stamps[FRAGMENT]
        {
            // As on the compute queue, the final stamp precedes the retired cursor and
            // there may be no separate notification when the cursor changes.
            let start = Instant::<Monotonic>::now();
            while !(ta_done && fs_done) && start.elapsed() < Delta::from_micros(RETIRE_WAIT_US) {
                fsleep(Delta::from_micros(20));
                ta_done = state.root.render_done(TILER, flight.targets[TILER], flight.stamps[TILER])?;
                fs_done = state.root.render_done(FRAGMENT, flight.targets[FRAGMENT], flight.stamps[FRAGMENT])?;
            }
        }
        if ta_done && fs_done {
            let flight = state.rendering.take().ok_or(EIO)?;
            for slot in flight.slots {
                self.retire_slot(state, slot);
            }
            if flight.last {
                flight.packet.retired();
            } else if !state.pending(&flight.packet) {
                flight.packet.release_guard();
            }
        } else if flight.started.elapsed() > Delta::from_millis(COMMAND_TIMEOUT_MS) {
            dev_err!(self.dev, "GPU: render timed out: TA retired={} fragment retired={}\n", ta_done, fs_done);
            state.root.log_timeout(&self.dev);
            state.fault.log(&self.dev);
            return Err(ETIMEDOUT);
        }
        Ok(())
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
        // Limit one pass to the ring capacity. Further arrivals notify the worker again.
        for _ in 0..256 {
            let Some((cursor, event)) = state.root.peek_event()? else { break; };
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
            if state.retired_total >= front.ordinal {
                if let Ok(done) = state.inflight.remove(0) {
                    self.retire_slot(state, done.slot);
                    if let Some(err) = done.publication_error {
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
            if front.started.elapsed() > Delta::from_millis(COMMAND_TIMEOUT_MS) {
                dev_err!(self.dev, "GPU: compute command timed out: stamp={:#x} expected={:#x} submitted={} completed={}\n",
                         state.queue_stamp(), front.stamp.raw(), state.submitted, state.retired);
                state.root.log_timeout(&self.dev);
                state.fault.log(&self.dev);
                self.fail_all(state, ETIMEDOUT);
                return;
            }
            break;
        }
        if !self.healthy() { return; }
        self.pump(state);
    }

    /// Before a suspend-to-idle: holds new publications and lets published work retire, so the
    /// sleep starts with nothing in flight. The firmware keeps running with its keep-powered
    /// policy: its GPU power domain is kept through the sleep (`apple,sleep-retained`), and the
    /// ASC, its memory and the root stay as they are. Refuses the sleep (EBUSY) if published work
    /// does not retire within the stop drain time.
    ///
    /// The firmware's own power-off handshake (idle policy allow-off, then keep-powered again
    /// after the wake) is not used: on phellos the firmware crashed in the wake that followed it.
    pub(crate) fn sleep_prepare(&self) -> Result {
        let mut guard = self.state.lock();
        if !self.healthy() || guard.is_none() { return Ok(()); }
        // Hold publications first, then let what is published retire. User space is frozen by
        // now; only the scheduler's queued jobs can still arrive, and they wait in `pending`.
        self.asleep.store(true, Ordering::Release);
        let mut remaining = msecs_to_jiffies(STOP_DRAIN_MS);
        loop {
            let Some(state) = Option::as_mut(&mut *guard) else { return Ok(()); };
            if !self.healthy() { return Ok(()); }
            if !state.running() {
                dev_info!(self.dev, "GPU held for sleep: submitted={} completed={} pending={}\n",
                          state.submitted, state.retired, state.pending.len());
                return Ok(());
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
    }

    /// After the sleep: publishes the work held during it.
    pub(crate) fn sleep_complete(&self) {
        let mut guard = self.state.lock();
        if !self.asleep.swap(false, Ordering::AcqRel) { return; }
        let Some(state) = Option::as_mut(&mut *guard) else { return; };
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
        self.fail_all(&mut state, ENODEV);
        let allowed = set_idle_policy(&state.root, &mut state.peer, true);
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
        let checked = (|| -> Result<(u32, u32)> {
            let (head, retired) = root.progress()?;
            if head != retired { return Err(EBUSY); }
            Ok((head, root.capacity()?))
        })();
        let (head, capacity) = match checked {
            Ok(v) => v,
            Err(e) => {
                halt_unowned(dev, peer, root, asc, true);
                core::mem::forget(pool);
                core::mem::forget(graph);
                return Err(e);
            }
        };
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
    pub(crate) fn sleep_prepare(&self) -> Result { self.runtime.sleep_prepare() }
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
    fn map_timestamp_buffer(&self, bo: gem::ObjectRef, range: Range<usize>) -> Result<crate::timestamp::Buffer> {
        // The aperture's tables were linked before the firmware window was mirrored (start()).
        <GpuManagerG14V13_5 as gpu::Gpu>::map_timestamp_buffer(&self.graph, bo, range)
    }
}
