// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! G14 GPUs on GPU firmware 27.2: start the firmware and run compute work on it.
//!
//! [`start`] builds the 27.2 root (`gpu/v27_2.rs`) on the records of a 13.5 G14 manager,
//! installs the driver's kernel window in the loader's UAT regions, starts the ASC, holds the
//! GPU powered and retires one kernel-owned compute job. [`Backend`] then serves clients
//! through the 26.x submission front end (`m3_submit`): each compute command is encoded in the
//! 27.2 layout and published, in order, on the root's queue. Render commands are refused.
//!
//! Completion follows the queue's retired position, which the firmware advances in order.
//! The firmware's event, statistics, log and trace rings are consumed as they fill, since
//! the firmware waits for space in them.

use core::ops::Range;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use kernel::{
    device,
    drm::sched,
    new_delayed_work, new_mutex,
    platform,
    prelude::*,
    soc::apple::rtkit,
    sync::{aref::ARef, Arc, Mutex},
    time::{delay::fsleep, msecs_to_jiffies, Delta, Instant, Monotonic},
    uapi,
    workqueue::{self, impl_has_delayed_work, DelayedWork, WorkItem},
};

use crate::{
    alloc, event, gem,
    gpu::{self, CommandPool, ComputeJob, GpuManagerG14V13_5, QueueInfo, RootV27_2, POOL_SLOTS},
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
/// Idle polling keeps the firmware's rings drained and bounds completion latency.
const IDLE_POLL_MS: u64 = 20;

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

/// RTKit operations of the GPU firmware: its crash log lives in the loader's data segment.
pub(crate) struct Firmware;

#[vtable]
impl rtkit::Operations for Firmware {
    type Data = Arc<ARef<device::Device>>;
    type Buffer = crate::g17::CrashLog;

    const COPROC_PLACES_BUFFERS: bool = true;

    fn shmem_map(dev: kernel::sync::ArcBorrow<'_, ARef<device::Device>>, iova: usize, size: usize) -> Result<Self::Buffer> {
        crate::g17::CrashLog::from_firmware(&dev, iova, size)
    }

    fn crashed(dev: kernel::sync::ArcBorrow<'_, ARef<device::Device>>, log: Option<&[u8]>) {
        unsafe extern "C" {
            fn asahi_save_rtkit_dump(dev: *mut core::ffi::c_void, data: *const u8, size: usize);
        }
        dev_err!(&**dev, "GPU firmware crashed\n");
        if let Some(log) = log {
            // SAFETY: RTKit and the device remain live throughout this synchronous copy.
            unsafe { asahi_save_rtkit_dump(dev.as_raw().cast(), log.as_ptr(), log.len()) };
        }
    }

    fn recv_message(_dev: kernel::sync::ArcBorrow<'_, ARef<device::Device>>, _ep: u8, _message: u64) {}
}

type Peer = Pin<KBox<rtkit::RtKit<Firmware>>>;

/// Polls `done` every millisecond, draining the firmware's rings, for at most `ms`.
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
pub(crate) fn start<'a>(pdev: &'a platform::Device<device::Core<'_>>, cfg: &'static hw::HwConfig)
    -> Result<Started<'a>> {
    use kernel::dma::{Device as _, DmaMask};
    let dev: &device::Device = pdev.as_ref();
    let (registers, writable) = match cfg.chip_id {
        0x8112 => (hw::t8112::REGISTERS_V27_2, hw::t8112::WRITABLE_V27_2),
        _ => return Err(ENODEV),
    };
    let geometry = mmu::UatGeometry { ias: cfg.uat_ias, oas: cfg.uat_oas };
    let mut window = mmu::FirmwareWindow::reserve(dev, geometry)?;
    let asc = AscControl::new(pdev.resource_by_name(c"asc").ok_or(EINVAL)?.start())?;
    let control = asc.read(ASC_CPU_CONTROL);
    if control & ASC_RUN != 0 { return Err(EBUSY); }
    let mut peer = KBox::pin(
        rtkit::RtKit::<Firmware>::new(dev, None, 0, Arc::new(ARef::from(dev), GFP_KERNEL)?)?,
        GFP_KERNEL)?;
    let resources = crate::regs::Resources::new(pdev)?;
    let sgx = pdev.resource_by_name(c"sgx").ok_or(EINVAL)?;
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
    window.mirror(graph.kernel_uat())?;
    window.install()?;
    window.announce();
    asc.write(ASC_CPU_CONTROL, control | ASC_RUN);

    // From here the firmware may read everything the window names: nothing is freed.
    let mut booted = false;
    let result = (|| -> Result {
        let start = Instant::<Monotonic>::now();
        while !window.answered() {
            if start.elapsed() >= Delta::from_millis(1000) { return Err(ETIMEDOUT); }
            fsleep(Delta::from_millis(1));
        }
        peer.as_mut().boot()?;
        booted = true;
        peer.as_mut().start_endpoint(EP_FIRMWARE)?;
        peer.as_mut().start_endpoint(EP_DOORBELL)?;
        if !window.intact() { return Err(EIO); }
        peer.as_mut().send_message(EP_FIRMWARE, gpu::init_message(root.gpu_va().get()))?;
        // Let initialization settle before the first control record.
        let _ = wait(&root, 1000, || Ok(false));
        set_idle_policy(&root, &mut peer, false)?;
        // Command-handler installation completes during powered initialization.
        let _ = wait(&root, 500, || Ok(root.command_service()? == (2, 0)));
        root.publish_boot_job(resources.operating_point())?;
        peer.as_mut().send_message(EP_DOORBELL, gpu::COMPUTE_KICK)?;
        wait(&root, 3000, || root.boot_job_complete())
    })();
    if let Err(e) = result {
        dev_err!(dev, "GPU firmware 27.2 did not start: {:?}\n", e);
        halt_unowned(dev, peer, root, asc, booted);
        core::mem::forget(window);
        core::mem::forget(pool);
        core::mem::forget(graph);
        return Err(e);
    }
    // The firmware keeps using the window until it is stopped; it stays for the life of the boot.
    core::mem::forget(window);
    let backend = Backend::start(&drm, dev, peer, root, graph, pool, asc, cfg)?;
    Ok(Started { backend, drm, binding, resources })
}

struct Inflight {
    packet: Arc<Packet>,
    last: bool,
    stamp: event::EventValue,
    slot: usize,
    /// Queue publications up to and including this command, counted from take-over.
    ordinal: u64,
    started: Instant<Monotonic>,
    _bind: mmu::VmBind,
}

struct State {
    peer: Peer,
    root: crate::fw::types::GpuObject<RootV27_2>,
    pool: CommandPool,
    asc: AscControl,
    queue: QueueInfo,
    last: event::EventValue,
    sequence: u64,
    next_slot: usize,
    waiting_unlink: bool,
    /// Publications and retirements counted on the queue's position block.
    published: u64,
    retired_total: u64,
    retired_raw: u32,
    capacity: u32,
    inflight: KVec<Inflight>,
    pending: KVec<(Arc<Packet>, usize)>,
    submitted: u64,
    retired: u64,
}

#[pin_data(PinnedDrop)]
pub(crate) struct Runtime {
    dev: ARef<device::Device>,
    graph: Arc<GpuManagerG14V13_5>,
    healthy: AtomicBool,
    /// Poll work queued or running; `stop` waits for zero so no work outlives the module.
    queued: AtomicU32,
    #[pin]
    state: Mutex<Option<State>>,
    #[pin]
    poll: DelayedWork<Runtime>,
}

impl_has_delayed_work! {
    impl HasDelayedWork<Runtime> for Runtime { self.poll }
}

impl WorkItem for Runtime {
    type Pointer = Arc<Runtime>;
    fn run(this: Arc<Runtime>) {
        let busy = this.service();
        if this.healthy.load(Ordering::Acquire) {
            this.schedule(if busy { 1 } else { IDLE_POLL_MS });
        }
        this.queued.fetch_sub(1, Ordering::AcqRel);
    }
}

#[pinned_drop]
impl PinnedDrop for Runtime {
    fn drop(self: Pin<&mut Self>) {
        // The poll work holds a reference while scheduled, so this runs only once the
        // runtime is idle; stop the firmware first if no one has.
        if self.state.lock().is_some() { self.stop(); }
    }
}

impl Runtime {
    fn schedule(self: &Arc<Self>, ms: u64) {
        if !self.healthy() { return; }
        self.queued.fetch_add(1, Ordering::AcqRel);
        if workqueue::system_highpri()
            .enqueue_delayed::<Arc<Self>, 0>(self.clone(), msecs_to_jiffies(ms as u32)).is_err() {
            // Already queued: that run accounts for itself.
            self.queued.fetch_sub(1, Ordering::AcqRel);
        }
    }

    pub(crate) fn healthy(&self) -> bool { self.healthy.load(Ordering::Acquire) }

    /// Queues a packet; the poll work publishes and retires it.
    pub(crate) fn submit(self: &Arc<Self>, packet: Arc<Packet>) -> Result {
        if !self.healthy() { return Err(EIO); }
        if packet.commands.is_empty() {
            return Err(ENOTSUPP);
        }
        {
            let mut guard = self.state.lock();
            let state = Option::as_mut(&mut *guard).ok_or(ENODEV)?;
            state.pending.push((packet, 0), GFP_KERNEL)?;
            self.pump(state);
        }
        self.schedule(1);
        Ok(())
    }

    /// Withdraws a packet's unpublished commands; published ones retire normally.
    pub(crate) fn withdraw(&self, packet: &Arc<Packet>) {
        let mut guard = self.state.lock();
        if let Some(state) = Option::as_mut(&mut *guard) {
            if let Some(i) = state.pending.iter().position(|(p, _)| Arc::ptr_eq(p, packet)) {
                let _ = state.pending.remove(i);
            }
            if !state.inflight.iter().any(|f| Arc::ptr_eq(&f.packet, packet)) {
                packet.release_guard();
            }
        }
    }

    fn fail_all(&self, state: &mut State, error: Error) {
        self.healthy.store(false, Ordering::Release);
        for (packet, _) in state.pending.drain(..) {
            packet.release_guard();
            packet.finish(Err(error));
        }
        // Published work stays owned: the firmware may still read it.
        for f in state.inflight.iter() {
            if let Some(status) = f.packet.vm.status() { status.record(error); }
            f.packet.finish(Err(error));
        }
    }

    /// Publishes pending commands while pool slots are free.
    fn pump(&self, state: &mut State) {
        while state.inflight.len() < POOL_SLOTS - 1 && !state.pending.is_empty() {
            // A slot is reused only once the firmware has unlinked it from its tracking list.
            match state.pool.registered(state.next_slot) {
                Ok(false) => {}
                Ok(true) => {
                    if !state.waiting_unlink {
                        // Wake the firmware's service once so its retirement scan runs.
                        state.waiting_unlink = true;
                        let _ = state.peer.as_mut().send_message(EP_DOORBELL, gpu::COMPUTE_KICK);
                    }
                    return;
                }
                Err(e) => { self.fail_all(state, e); return; }
            }
            state.waiting_unlink = false;
            let (packet, index) = (state.pending[0].0.clone(), state.pending[0].1);
            let Command::Compute(control) = packet.commands[index] else {
                // Queues of this backend accept only compute commands (Queue::submit).
                let _ = state.pending.remove(0);
                if !(packet.may_be_published() && index != 0) { packet.release_guard(); }
                packet.finish(Err(EINVAL));
                continue;
            };
            let result = (|| -> Result<(mmu::VmBind, event::EventValue)> {
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
                };
                let command = state.pool.encode(state.next_slot, &state.queue, stamp.raw(), &job)?;
                packet.mark_published();
                state.root.publish(command)?;
                state.peer.as_mut().send_message(EP_DOORBELL, gpu::COMPUTE_KICK)?;
                Ok((bind, stamp))
            })();
            match result {
                Ok((bind, stamp)) => {
                    state.last = stamp;
                    state.sequence += 1;
                    let slot = state.next_slot;
                    state.published += 1;
                    let ordinal = state.published;
                    state.next_slot = (state.next_slot + 1) % POOL_SLOTS;
                    state.submitted += 1;
                    let last = index + 1 == packet.commands.len();
                    if state.inflight.push(Inflight { packet: packet.clone(), last, stamp, slot, ordinal,
                        started: Instant::now(), _bind: bind }, GFP_KERNEL).is_err() {
                        self.fail_all(state, ENOMEM);
                        return;
                    }
                    if last { let _ = state.pending.remove(0); } else { state.pending[0].1 += 1; }
                }
                Err(e) => {
                    dev_err!(self.dev, "GPU: compute publication failed {:?}\n", e);
                    let _ = state.pending.remove(0);
                    // Earlier commands of a published packet fail it when they retire.
                    if !(packet.may_be_published() && index != 0) {
                        packet.release_guard();
                    }
                    packet.finish(Err(e));
                }
            }
        }
    }

    /// Retires completed commands, publishes more and keeps the firmware's rings drained.
    /// Returns whether work remains.
    fn service(&self) -> bool {
        let mut guard = self.state.lock();
        let Some(state) = Option::as_mut(&mut *guard) else { return false; };
        let _ = state.root.drain();
        if state.peer.is_crashed() {
            dev_err!(self.dev, "GPU firmware crashed; failing {} pending and {} published commands\n",
                     state.pending.len(), state.inflight.len());
            self.fail_all(state, EIO);
            return false;
        }
        let raw = match state.root.progress() {
            Ok((_, raw)) => raw,
            Err(e) => { self.fail_all(state, e); return false; }
        };
        if state.capacity != 0 && raw < state.capacity {
            let delta = (raw + state.capacity - state.retired_raw) % state.capacity;
            state.retired_total += u64::from(delta);
            state.retired_raw = raw;
        }
        while !state.inflight.is_empty() {
            let front = &state.inflight[0];
            if state.retired_total >= front.ordinal {
                if let Ok(done) = state.inflight.remove(0) {
                    if let Err(e) = state.pool.retire(done.slot) {
                        dev_err!(self.dev, "GPU: tracking retirement failed {:?}\n", e);
                    }
                    state.retired += 1;
                    if done.last {
                        done.packet.retired();
                    }
                }
                continue;
            }
            if front.started.elapsed() > Delta::from_millis(COMMAND_TIMEOUT_MS) {
                dev_err!(self.dev, "GPU: compute command timed out: stamp={:#x} expected={:#x} submitted={} completed={}\n",
                         state.queue_stamp(), front.stamp.raw(), state.submitted, state.retired);
                self.fail_all(state, ETIMEDOUT);
                return false;
            }
            break;
        }
        if self.healthy() { self.pump(state); }
        !state.inflight.is_empty() || !state.pending.is_empty()
    }

    /// Stops the firmware: retire or fail outstanding work, allow power-off, then halt the ASC
    /// with every mapping still in place (RTKit power-off if the firmware does not stop).
    pub(crate) fn stop(&self) {
        let start = Instant::<Monotonic>::now();
        while start.elapsed() < Delta::from_millis(2000) {
            if !self.service() { break; }
            fsleep(Delta::from_millis(1));
        }
        self.healthy.store(false, Ordering::Release);
        // No poll work may remain queued or running once this returns.
        let start = Instant::<Monotonic>::now();
        while self.queued.load(Ordering::Acquire) != 0 && start.elapsed() < Delta::from_millis(2000) {
            fsleep(Delta::from_millis(5));
        }
        // Let a run that just accounted for itself return out of this module.
        fsleep(Delta::from_millis(20));
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
        dev_info!(self.dev, "GPU stopped: allow-off={:?} running={:?} forced-off={:?} submitted={} completed={}\n",
                  allowed, running, forced, state.submitted, state.retired);
        if running != Ok(0) && forced != Some(Ok(())) {
            dev_err!(self.dev, "GPU firmware did not stop; a reset is required\n");
        }
        // Keep everything the firmware could reach for the rest of the boot, halted or not:
        // its page tables still name these pages.
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
        peer: Peer,
        root: crate::fw::types::GpuObject<RootV27_2>,
        graph: Arc<GpuManagerG14V13_5>,
        pool: CommandPool,
        asc: AscControl,
        cfg: &'static hw::HwConfig,
    ) -> Result<Arc<Self>> {
        // The runtime is created empty first, so no allocation failure can drop firmware-visible
        // state. Every failure after the state is moved in goes through Runtime::stop, which
        // halts the firmware and keeps all of its memory.
        let runtime_graph = graph.clone();
        let runtime = match Arc::pin_init(try_pin_init!(Runtime {
            dev: dev.into(),
            graph: runtime_graph,
            healthy: AtomicBool::new(true),
            queued: AtomicU32::new(0),
            state <- new_mutex!(None, "g14_v27_2::Runtime::state"),
            poll <- new_delayed_work!("g14_v27_2::Runtime::poll"),
        }), GFP_KERNEL) {
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
            peer, root, pool, asc, queue, last,
            sequence: u64::from(head),
            next_slot: 0,
            waiting_unlink: false,
            published: 0,
            retired_total: 0,
            retired_raw: head,
            capacity,
            inflight: KVec::new(),
            pending: KVec::new(),
            submitted: 0,
            retired: 0,
        });
        runtime.schedule(IDLE_POLL_MS);
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
    fn new_queue(&self, vm: mmu::Vm, _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>, priority: u32,
        usc_exec_base: u64, _usage: Arc<crate::usage::Client>) -> Result<KBox<dyn queue::Queue>> {
        Ok(KBox::new(crate::m3::m3_submit::Queue::new_g14_v27_2(self.runtime.clone(), self.scheduler.clone(),
            vm, priority, usc_exec_base, ualloc_priv, self.preempt_bytes)?, GFP_KERNEL)?)
    }
    fn map_timestamp_buffer(&self, _bo: gem::ObjectRef, _range: Range<usize>) -> Result<crate::timestamp::Buffer> {
        // Timestamp buffers would need new tables in the firmware window after boot.
        Err(ENOTSUPP)
    }
}
