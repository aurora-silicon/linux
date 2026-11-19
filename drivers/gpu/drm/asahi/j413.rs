// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! J413 (T8112, G14G) with GPU firmware 27.2: a compute-only DRM backend.
//!
//! The 27.2 firmware is brought up by the J413 bring-up path in `mmu.rs`
//! (`Uat::observe_t8112_peer`), which builds the 26B123 root from the 13.5 G14G graph and runs
//! one kernel-owned compute job. With `asahi.t8112_drm=1` that path hands the running firmware
//! over instead of stopping it. Clients reach it through the 26.x submission front end of the
//! M3 backend (`m3_submit`), whose compute commands are re-encoded here in the 26B123 G14G
//! layout and published, in order, on the bring-up queue.

use core::ops::Range;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use kernel::{
    device,
    drm::sched,
    new_delayed_work, new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
    time::{msecs_to_jiffies, Delta, Instant, Monotonic},
    sync::aref::ARef,
    uapi,
    workqueue::{self, impl_has_delayed_work, DelayedWork, WorkItem},
};

use crate::{
    alloc, event, gem, gpu, gpu::GpuManagerG14V13_5, mmu, queue,
    gpu::{G14TrialRoot, J413Compute, J413Pool, J413Queue, J413_POOL},
    m3::m3_submit::{Command, Packet},
};

/// The ASC control page, kept mapped until the firmware is stopped.
pub(crate) struct AscControl(*mut u8);
// SAFETY: a device register page; accesses are single 32-bit MMIO operations.
unsafe impl Send for AscControl {}
// SAFETY: as above.
unsafe impl Sync for AscControl {}

impl AscControl {
    /// Maps the ASC control page at `phys`.
    pub(crate) fn new(phys: u64) -> Result<Self> {
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
}

impl Drop for AscControl {
    fn drop(&mut self) {
        // SAFETY: mapped in `new`, unmapped once.
        unsafe { kernel::bindings::iounmap(self.0.cast()) }
    }
}

/// What the bring-up path hands over when it keeps the firmware running.
pub(crate) struct Live<'a> {
    pub(crate) peer: Pin<KBox<kernel::soc::apple::rtkit::RtKit<crate::mmu::T8112Peer>>>,
    pub(crate) root: crate::fw::types::GpuObject<G14TrialRoot>,
    pub(crate) graph: Arc<GpuManagerG14V13_5>,
    pub(crate) pool: J413Pool,
    pub(crate) asc: AscControl,
    pub(crate) drm: kernel::drm::UnregisteredDevice<crate::driver::AsahiDrmDriver>,
    pub(crate) binding: crate::binding::Owner,
    pub(crate) resources: crate::regs::Resources<'a>,
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
    peer: Pin<KBox<kernel::soc::apple::rtkit::RtKit<crate::mmu::T8112Peer>>>,
    root: crate::fw::types::GpuObject<G14TrialRoot>,
    pool: J413Pool,
    asc: AscControl,
    queue: J413Queue,
    last: event::EventValue,
    sequence: u64,
    next_slot: usize,
    waiting_unlink: bool,
    /// Publications and retirements counted on the queue's position block, which the
    /// firmware advances in order (the shared stamp can be written out of order).
    published: u64,
    retired_total: u64,
    retired_raw: u32,
    capacity: u32,
    inflight: KVec<Inflight>,
    pending: KVec<(Arc<Packet>, usize)>,
    submitted: u64,
    retired: u64,
}

/// Commands slower than this are a hang; the firmware state is then not trusted.
const COMMAND_TIMEOUT_MS: i64 = 5000;
/// Idle polling keeps the firmware's statistics ring drained and bounds completion latency.
const IDLE_POLL_MS: u64 = 20;

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

    /// Queue a packet; the poll work publishes and retires it.
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

    /// Withdraw a packet's unpublished commands; published ones retire normally.
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

    /// Publish pending commands while pool slots are free.
    fn pump(&self, state: &mut State) {
        while state.inflight.len() < J413_POOL - 1 && !state.pending.is_empty() {
            // A slot is reused only once the firmware has unlinked it (tracking list).
            match state.pool.registered(state.next_slot) {
                Ok(false) => {}
                Ok(true) => {
                    if !state.waiting_unlink {
                        state.waiting_unlink = true;
                        dev_info!(self.dev, "J413: slot {} still in the firmware's tracking list; waiting\n", state.next_slot);
                        state.pool.dump(&self.dev, state.next_slot);
                        // Wake the firmware's service so its retirement scan runs.
                        let _ = state.peer.as_mut().send_message(0x21, G14TrialRoot::j413_kick());
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
                let bind = self.graph.trial_uat().bind(&packet.vm)?;
                let preempt = packet.j413_preempt.as_ref().ok_or(EINVAL)?.gpu_va().get();
                let stamp = state.last.next();
                let job = J413Compute {
                    context: bind.slot(),
                    sequence: state.sequence,
                    cdm_base: control.base,
                    cdm_end: control.end,
                    usc_base: control.usc_base,
                    preempt,
                };
                let command = state.pool.encode(state.next_slot, &state.queue, stamp.raw(), &job)?;
                packet.mark_published();
                state.root.j413_publish(command)?;
                state.peer.as_mut().send_message(0x21, G14TrialRoot::j413_kick())?;
                Ok((bind, stamp))
            })();
            match result {
                Ok((bind, stamp)) => {
                    state.last = stamp;
                    state.sequence += 1;
                    let slot = state.next_slot;
                    state.published += 1;
                    let ordinal = state.published;
                    state.next_slot = (state.next_slot + 1) % J413_POOL;
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
                    dev_err!(self.dev, "J413: compute publication failed {:?}\n", e);
                    let _ = state.pending.remove(0);
                    if packet.may_be_published() && index != 0 {
                        // Earlier commands of this packet are in flight; fail it when they retire.
                        packet.finish(Err(e));
                    } else {
                        packet.release_guard();
                        packet.finish(Err(e));
                    }
                }
            }
        }
    }

    /// Retire completed commands, publish more, keep the firmware's rings drained.
    /// Returns whether work remains.
    fn service(&self) -> bool {
        let mut guard = self.state.lock();
        let Some(state) = Option::as_mut(&mut *guard) else { return false; };
        let _ = state.root.drain_statistics();
        if state.peer.is_crashed() {
            dev_err!(self.dev, "J413: GPU firmware crashed; failing {} pending and {} published commands\n",
                     state.pending.len(), state.inflight.len());
            self.fail_all(state, EIO);
            return false;
        }
        let (current, raw) = match state.root.j413_progress() {
            Ok((_, raw, current)) => (current, raw),
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
                let done = state.inflight.remove(0).ok();
                if let Some(done) = done {
                    if let Err(e) = state.pool.retire(done.slot) {
                        dev_err!(self.dev, "J413: tracking retirement failed {:?}\n", e);
                    }
                    state.retired += 1;
                    if done.last {
                        done.packet.retired();
                    }
                }
                continue;
            }
            if front.started.elapsed() > Delta::from_millis(COMMAND_TIMEOUT_MS) {
                let (head, retired, _) = state.root.j413_progress().unwrap_or((0, 0, current));
                dev_err!(self.dev, "J413: compute command timed out: stamp={:#x} expected={:#x} head={} retired={} submitted={} completed={}\n",
                         current.raw(), front.stamp.raw(), head, retired, state.submitted, state.retired);
                // Diagnostics, all read-only: work records, MMU fault latch, private scheduler words.
                let _ = state.root.report_job_gates("drm-timeout");
                fault_latch(&self.dev);
                let _ = crate::mmu::t8112_private_scan(&self.dev, &state.root.job_anchors(), 3);
                let _ = state.root.j413_trace_tail(48);
                let _ = state.root.j413_rings();
                let stuck = state.inflight[0].slot;
                state.root.j413_record((state.inflight[0].stamp.raw() >> 8) as usize);
                state.root.j413_record((state.inflight[0].stamp.raw() >> 8) as usize - 1);
                state.pool.dump(&self.dev, stuck);
                state.pool.dump(&self.dev, 0);
                state.pool.dump(&self.dev, (stuck + 1) % J413_POOL);
                self.fail_all(state, ETIMEDOUT);
                return false;
            }
            break;
        }
        if self.healthy() { self.pump(state); }
        !state.inflight.is_empty() || !state.pending.is_empty()
    }

    /// Stop the firmware: retire or fail outstanding work, allow power-off, then halt the ASC
    /// with every mapping still in place (normal stop, forced stop as fallback).
    pub(crate) fn stop(&self) {
        let start = Instant::<Monotonic>::now();
        while start.elapsed() < Delta::from_millis(2000) {
            if !self.service() { break; }
            kernel::time::delay::fsleep(Delta::from_millis(1));
        }
        self.healthy.store(false, Ordering::Release);
        // No poll work may remain queued or running once this returns.
        let start = Instant::<Monotonic>::now();
        while self.queued.load(Ordering::Acquire) != 0 && start.elapsed() < Delta::from_millis(2000) {
            kernel::time::delay::fsleep(Delta::from_millis(5));
        }
        // Let a run that just accounted for itself return out of this module.
        kernel::time::delay::fsleep(Delta::from_millis(20));
        let mut guard = self.state.lock();
        let Some(mut state) = guard.take() else { return; };
        self.fail_all(&mut state, ENODEV);
        let root = &state.root;
        let allowed = (|| -> Result {
            let (target, generation) = root.publish_idle_policy(true)?;
            state.peer.as_mut().send_message(0x21, 0x0084_0000_0000_0011)?;
            let start = Instant::<Monotonic>::now();
            while root.idle_progress()? != [target; 3] || root.idle_acknowledgement()? != generation {
                if start.elapsed() >= Delta::from_millis(500) { return Err(ETIMEDOUT); }
                let _ = root.drain_statistics();
                kernel::time::delay::fsleep(Delta::from_millis(1));
            }
            Ok(())
        })();
        let start = Instant::<Monotonic>::now();
        while root.firmware_running() != Ok(0) && start.elapsed() < Delta::from_millis(500) {
            let _ = root.drain_statistics();
            kernel::time::delay::fsleep(Delta::from_millis(1));
        }
        let running = root.firmware_running();
        let forced = if running != Ok(0) {
            // SAFETY: the RTKit instance is live and owned here.
            Some(kernel::error::to_result(unsafe {
                kernel::bindings::apple_rtkit_poweroff(state.peer.as_raw())
            }))
        } else {
            None
        };
        state.asc.write(0x44, state.asc.read(0x44) & !0x10);
        dev_info!(self.dev, "J413: GPU stopped: allow-off={:?} status+0x10={:?} forced={:?} ASC={:#x} submitted={} completed={}\n",
                  allowed, running, forced, state.asc.read(0x44), state.submitted, state.retired);
        // Keep everything the firmware could reach for the rest of the boot, halted or not: its
        // page tables still name these pages, and a bring-up leak is cheaper than a stray write.
        core::mem::forget(state);
        core::mem::forget(self.graph.clone());
        if running != Ok(0) && forced != Some(Ok(())) {
            dev_err!(self.dev, "J413: firmware did not stop; memory retained, a reset is required\n");
        }
    }
}

/// Logs the G14 MMU fault latch (read-only).
fn fault_latch(dev: &device::Device) {
    // SAFETY: SGX register page holding the G14 MMU fault latch, read once.
    let latch = unsafe {
        let p = kernel::bindings::ioremap(0x204000000 + 0x17000, 0x1000);
        if p.is_null() { u64::MAX } else {
            let lo = kernel::bindings::readl(p.cast::<u8>().add(0x30).cast()) as u64;
            let hi = kernel::bindings::readl(p.cast::<u8>().add(0x34).cast()) as u64;
            kernel::bindings::iounmap(p);
            lo | hi << 32
        }
    };
    dev_err!(dev, "J413: fault latch={:#x} valid={} reason={} client={} address={:#x}\n",
             latch, latch & 1, (latch >> 1) & 7, (latch >> 9) & 0xff, (latch >> 24) & !0x3f);
}

/// Halts firmware that no runtime owns yet (allow-off, then RTKit power-off, then the ASC run
/// bit), and retains every object it could reach.
fn halt_unowned(
    dev: &device::Device,
    mut peer: Pin<KBox<kernel::soc::apple::rtkit::RtKit<crate::mmu::T8112Peer>>>,
    root: crate::fw::types::GpuObject<G14TrialRoot>,
    asc: AscControl,
) {
    let allowed = (|| -> Result {
        let (target, generation) = root.publish_idle_policy(true)?;
        peer.as_mut().send_message(0x21, 0x0084_0000_0000_0011)?;
        let start = Instant::<Monotonic>::now();
        while root.idle_progress()? != [target; 3] || root.idle_acknowledgement()? != generation {
            if start.elapsed() >= Delta::from_millis(500) { return Err(ETIMEDOUT); }
            kernel::time::delay::fsleep(Delta::from_millis(1));
        }
        Ok(())
    })();
    // SAFETY: the RTKit instance is live and owned here.
    let forced = kernel::error::to_result(unsafe { kernel::bindings::apple_rtkit_poweroff(peer.as_raw()) });
    asc.write(0x44, asc.read(0x44) & !0x10);
    dev_err!(dev, "J413: firmware halted after a failed take-over: allow-off={:?} power-off={:?} ASC={:#x}\n",
             allowed, forced, asc.read(0x44));
    core::mem::forget(peer);
    core::mem::forget(root);
    core::mem::forget(asc);
}

pub(crate) struct Backend {
    graph: Arc<GpuManagerG14V13_5>,
    runtime: Arc<Runtime>,
    scheduler: Arc<sched::Scheduler<crate::m3::m3_submit::Job>>,
    ids: gpu::SequenceIDs,
}

impl Backend {
    /// Takes over the running firmware from the bring-up path.
    pub(crate) fn start(
        drm: &crate::driver::AsahiDrmDevice,
        dev: &device::Device,
        peer: Pin<KBox<kernel::soc::apple::rtkit::RtKit<crate::mmu::T8112Peer>>>,
        root: crate::fw::types::GpuObject<G14TrialRoot>,
        graph: Arc<GpuManagerG14V13_5>,
        pool: J413Pool,
        asc: AscControl,
    ) -> Result<Arc<Self>> {
        // The runtime is created empty first, so no allocation failure can drop firmware-visible
        // state. Every failure after the state is moved in goes through Runtime::stop, which halts
        // the firmware and retains all of its memory.
        let runtime_graph = graph.clone();
        let runtime = match Arc::pin_init(try_pin_init!(Runtime {
            dev: dev.into(),
            graph: runtime_graph,
            healthy: AtomicBool::new(true),
            queued: AtomicU32::new(0),
            state <- new_mutex!(None, "j413::Runtime::state"),
            poll <- new_delayed_work!("j413::Runtime::poll"),
        }), GFP_KERNEL) {
            Ok(runtime) => runtime,
            Err(e) => {
                dev_err!(dev, "J413: runtime allocation failed {:?}; halting the firmware\n", e);
                halt_unowned(dev, peer, root, asc);
                core::mem::forget(pool);
                core::mem::forget(graph);
                return Err(e);
            }
        };
        let checked = (|| -> Result<(J413Queue, event::EventValue, u32, u32)> {
            let (queue, last) = root.j413_queue()?;
            let (head, retired, _) = root.j413_progress()?;
            if head != retired { return Err(EBUSY); }
            Ok((queue, last, head, root.j413_capacity()?))
        })();
        let (queue, last, head, capacity) = match checked {
            Ok(v) => v,
            Err(e) => {
                dev_err!(dev, "J413: take-over refused {:?}; halting the firmware\n", e);
                halt_unowned(dev, peer, root, asc);
                core::mem::forget(pool);
                core::mem::forget(graph);
                return Err(e);
            }
        };
        dev_info!(dev, "J413: taking over the running firmware: queue={:#x} head={} stamp={:#x}\n",
                  queue.queue, head, last.raw());
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
        if crate::module_parameters::t8112_drm_ring_skew.value() {
            // Diagnostic: offset the submission ring by one entry against the stamps.
            let mut guard = runtime.state.lock();
            if let Some(state) = Option::as_mut(&mut *guard) {
                let r = state.root.j413_publish_entry()
                    .and_then(|_| state.peer.as_mut().send_message(0x21, G14TrialRoot::j413_kick()));
                dev_info!(dev, "J413: ring skew entry {:?}\n", r);
            }
        }
        runtime.schedule(IDLE_POLL_MS);
        let scheduler = match sched::Scheduler::new(drm.as_ref().as_ref(), 4, J413_POOL as u32 - 1, 0, 10000,
            c"asahi_j413_sched").and_then(|s| Arc::new(s, GFP_KERNEL).map_err(Into::into)) {
            Ok(scheduler) => scheduler,
            Err(e) => {
                dev_err!(dev, "J413: scheduler setup failed {:?}; stopping the firmware\n", e);
                runtime.stop();
                return Err(e);
            }
        };
        match Arc::new(Self { graph, runtime: runtime.clone(), scheduler, ids: gpu::SequenceIDs::default() }, GFP_KERNEL) {
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
        self.graph.trial_uat().new_vm(self.ids.vm.next(), range)?.with_tracked_status()
    }
    fn new_queue(&self, vm: mmu::Vm, _ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>, priority: u32,
        usc_exec_base: u64, _usage: Arc<crate::usage::Client>) -> Result<KBox<dyn queue::Queue>> {
        Ok(KBox::new(crate::m3::m3_submit::Queue::new_j413(self.runtime.clone(), self.scheduler.clone(),
            vm, priority, usc_exec_base, ualloc_priv)?, GFP_KERNEL)?)
    }
    fn map_timestamp_buffer(&self, _bo: gem::ObjectRef, _range: Range<usize>) -> Result<crate::timestamp::Buffer> {
        // Timestamp buffers would need new tables in the firmware window after boot.
        Err(ENOTSUPP)
    }
}
