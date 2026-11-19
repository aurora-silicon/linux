// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Submission queue management
//!
//! This module implements the userspace view of submission queues and the logic to map userspace
//! submissions to firmware queues.

use kernel::dma_fence::*;
use kernel::prelude::*;
use kernel::{
    c_str,
    dma_fence,
    drm::sched,
    macros::versions,
    sync::{
        Arc,
        LockClassKey,
        Mutex, //
    },
    uapi,
    xarray, //
};

use crate::alloc::Allocator;
use crate::debug::*;
use crate::driver::{AsahiDevRef, AsahiDevice};
use crate::file::MAX_COMMANDS_PER_SUBMISSION;
use crate::fw::types::*;
use crate::gpu::{
    Gpu,
    GpuManager, //
};
use crate::inner_weak_ptr;
use crate::microseq;
use crate::module_parameters;
use crate::stats;
use crate::util::{
    AnyBitPattern,
    Reader, //
};
use crate::{
    alloc,
    buffer,
    channel,
    driver::AsahiRegData,
    event,
    file,
    fw,
    gpu,
    mmu,
    workqueue, //
};

use core::sync::atomic::{
    AtomicU64,
    Ordering, //
};

const DEBUG_CLASS: DebugFlags = DebugFlags::Queue;

const WQ_SIZE: u32 = 0x500;

/// REFFLAKE4 test-only submission trace (debug bit 53, Debug5).
pub(crate) mod subtrace {
    use core::sync::atomic::{AtomicU64, Ordering};
    use kernel::prelude::*;

    const N: usize = 1024;
    const W: usize = 4;
    static NEXT: AtomicU64 = AtomicU64::new(0);
    static RING: [AtomicU64; N * W] = [const { AtomicU64::new(0) }; N * W];

    pub(crate) fn enabled() -> bool {
        crate::debug::debug_enabled(crate::debug::DebugFlags::Debug5)
    }

    fn now_ns() -> u64 {
        // SAFETY: `ktime_get()` may be called from any context.
        unsafe { kernel::bindings::ktime_get() as u64 }
    }

    /// One record per render submission. `a` packs: render index of the queue (bits 0..24),
    /// nr_render (24..32), vtx event slot (32..40), frag event slot (40..48), vm slot (48..56),
    /// buffer slot (56..63) ; `b`: bit0 inline, bit1 rebind, bit2 tvb grown, bits 8.. queue id.
    pub(crate) fn record(a: u64, b: u64, s: u64) {
        let i = (NEXT.fetch_add(1, Ordering::Relaxed) as usize) % N;
        RING[i * W].store(0, Ordering::Relaxed);
        RING[i * W + 1].store(a, Ordering::Relaxed);
        RING[i * W + 2].store(b, Ordering::Relaxed);
        RING[i * W + 3].store(s, Ordering::Relaxed);
        RING[i * W].store(now_ns(), Ordering::Release);
    }

    /// Teardown event: kind 1/2 context invalidation begin/end (val = context VA), 3 BM slot release
    /// (val = slot), 4/5 queue drop begin/end (val = queue id), 6 event slot release (val = slot).
    pub(crate) fn mark(kind: u64, val: u64) {
        if enabled() {
            record(0xff << 56 | kind, val, 0);
        }
    }

    fn line(tag: &str, now: u64, i: usize) {
        let ts = RING[i * W].load(Ordering::Acquire);
        let a = RING[i * W + 1].load(Ordering::Relaxed);
        let b = RING[i * W + 2].load(Ordering::Relaxed);
        if a >> 56 == 0xff {
            pr_err!("subtrace {}: -{}us EVT kind={} val={:#x}\n", tag, now.saturating_sub(ts) / 1000, a & 0xff, b);
            return;
        }
        let s = RING[i * W + 3].load(Ordering::Relaxed);
        pr_err!(
            "subtrace {}: -{}us q={} ridx={} nr={} vtx_ev={} frag_ev={} vm={} buf={} inline={} rebind={} grown={} init={} excl={} maxpg={} ovf={} blocks={} minblk={}\n",
            tag,
            now.saturating_sub(ts) / 1000,
            b >> 8,
            a & 0xff_ffff,
            (a >> 24) & 0xff,
            (a >> 32) & 0xff,
            (a >> 40) & 0xff,
            (a >> 48) & 0xff,
            (a >> 56) & 0x7f,
            b & 1,
            (b >> 1) & 1,
            (b >> 2) & 1,
            (b >> 3) & 1,
            (b >> 4) & 1,
            s & 0xffff,
            (s >> 16) & 0xffff,
            (s >> 32) & 0xffff,
            s >> 48
        );
    }

    /// Dump the newest records whose event slot or vm slot matches, plus the last few overall.
    pub(crate) fn dump(event_slot: Option<u32>, vm_slot: Option<u32>) {
        if !enabled() {
            return;
        }
        let now = now_ns();
        let next = NEXT.load(Ordering::Relaxed) as usize;
        let total = next.min(N);
        let mut shown = 0;
        // Per queue id: count how many records of the matching queues are in the ring.
        for k in 1..=total {
            let i = (next - k) % N;
            let a = RING[i * W + 1].load(Ordering::Relaxed);
            let ev_v = ((a >> 32) & 0xff) as u32;
            let ev_f = ((a >> 40) & 0xff) as u32;
            let vm = ((a >> 48) & 0xff) as u32;
            if a >> 56 == 0xff {
                continue;
            }
            let m_ev = event_slot.is_some_and(|e| e == ev_v || e == ev_f);
            let m_vm = vm_slot.is_some_and(|v| v == vm);
            if m_ev || m_vm {
                line(if m_ev { "match-ev" } else { "match-vm" }, now, i);
                shown += 1;
                if shown >= 12 {
                    break;
                }
            }
        }
        for k in 1..=total.min(24) {
            line("recent", now, (next - k) % N);
        }
        pr_err!("subtrace: total={} ev={:?} vm={:?}\n", next, event_slot, vm_slot);
    }
}

/// REFFLAKE4 test-only discriminator (debug bit 52, Debug4): serialise the first render of every queue.
pub(crate) mod firstgate {
    use core::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
    use kernel::prelude::*;
    use kernel::time::{delay::fsleep, Delta, Instant, Monotonic};

    /// Render work (submissions being built + render commands not yet completed) of all queues.
    pub(crate) static INFLIGHT: AtomicI64 = AtomicI64::new(0);
    /// A first render currently owns the GPU.
    pub(crate) static EXCL: AtomicBool = AtomicBool::new(false);
    pub(crate) static FIRSTS: AtomicU64 = AtomicU64::new(0);
    pub(crate) static HELD: AtomicU64 = AtomicU64::new(0);
    pub(crate) static TIMEOUTS: AtomicU64 = AtomicU64::new(0);
    /// Published render commands not yet completed (all queues).
    pub(crate) static RENDERING: AtomicI64 = AtomicI64::new(0);
    pub(crate) static TEARDOWNS: AtomicU64 = AtomicU64::new(0);
    const LIMIT_MS: i64 = 3000;

    pub(crate) fn enabled() -> bool {
        crate::debug::debug_enabled(crate::debug::DebugFlags::Debug4)
    }

    /// Teardown gate (bit 49).
    pub(crate) fn teardown_enabled() -> bool {
        crate::debug::debug_enabled(crate::debug::DebugFlags::Debug1)
    }

    /// Any gate needs the in-flight accounting.
    pub(crate) fn tracking() -> bool {
        enabled() || teardown_enabled()
    }

    /// Counts one published render command until dropped (with its completion callback).
    pub(crate) struct RenderToken(Token);
    impl RenderToken {
        pub(crate) fn new() -> RenderToken {
            RENDERING.fetch_add(1, Ordering::SeqCst);
            RenderToken(Token::new())
        }
    }
    impl Drop for RenderToken {
        fn drop(&mut self) {
            RENDERING.fetch_sub(1, Ordering::SeqCst);
        }
    }

    /// Context invalidation: hold new render submissions back and wait for published renders to finish.
    pub(crate) fn enter_teardown() -> bool {
        if !teardown_enabled() {
            return false;
        }
        let start = Instant::<Monotonic>::now();
        while EXCL
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            if timed_out(&start, "teardown exclusive wait") {
                return false;
            }
            fsleep(Delta::from_micros(100));
        }
        while RENDERING.load(Ordering::SeqCst) != 0 {
            if timed_out(&start, "teardown drain wait") {
                break;
            }
            fsleep(Delta::from_micros(50));
        }
        true
    }

    pub(crate) fn leave_teardown(held: bool) {
        if held {
            EXCL.store(false, Ordering::SeqCst);
            let n = TEARDOWNS.fetch_add(1, Ordering::Relaxed) + 1;
            if n % 256 == 0 {
                pr_info!("firstgate: teardowns={} timeouts={}\n", n, TIMEOUTS.load(Ordering::Relaxed));
            }
        }
    }

    /// Counts one unit of in-flight render work until dropped.
    pub(crate) struct Token(());
    impl Token {
        pub(crate) fn new() -> Token {
            INFLIGHT.fetch_add(1, Ordering::SeqCst);
            Token(())
        }
    }
    impl Drop for Token {
        fn drop(&mut self) {
            INFLIGHT.fetch_sub(1, Ordering::SeqCst);
        }
    }

    fn timed_out(start: &Instant<Monotonic>, what: &str) -> bool {
        if start.elapsed() > Delta::from_millis(LIMIT_MS) {
            let n = TIMEOUTS.fetch_add(1, Ordering::Relaxed) + 1;
            pr_warn!("firstgate: {} timed out after {} ms (timeouts={})\n", what, LIMIT_MS, n);
            true
        } else {
            false
        }
    }

    /// Ordinary render submission: register as in flight, but never while a first render owns the GPU.
    pub(crate) fn enter_normal() -> Token {
        let start = Instant::<Monotonic>::now();
        let mut held = false;
        loop {
            let tok = Token::new();
            if !EXCL.load(Ordering::SeqCst) {
                if held {
                    HELD.fetch_add(1, Ordering::Relaxed);
                }
                return tok;
            }
            drop(tok);
            held = true;
            if timed_out(&start, "normal submission wait") {
                return Token::new();
            }
            fsleep(Delta::from_micros(100));
        }
    }

    /// First render of a queue: take exclusive ownership, then wait for all other render work to drain.
    /// Returns true if the exclusive window is held (release with `leave_first`).
    pub(crate) fn enter_first() -> bool {
        let start = Instant::<Monotonic>::now();
        while EXCL
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            if timed_out(&start, "first-render exclusive wait") {
                return false;
            }
            fsleep(Delta::from_micros(100));
        }
        while INFLIGHT.load(Ordering::SeqCst) != 0 {
            if timed_out(&start, "first-render drain wait") {
                break;
            }
            fsleep(Delta::from_micros(50));
        }
        true
    }

    pub(crate) fn leave_first() {
        EXCL.store(false, Ordering::SeqCst);
        let n = FIRSTS.fetch_add(1, Ordering::Relaxed) + 1;
        if n % 512 == 0 {
            pr_info!(
                "firstgate: firsts={} held={} timeouts={}\n",
                n,
                HELD.load(Ordering::Relaxed),
                TIMEOUTS.load(Ordering::Relaxed)
            );
        }
    }
}

mod common;
mod compute;
mod render;

fn can_submit_inline(dependencies_ready: bool, previous_status: Option<i32>) -> bool {
    dependencies_ready && previous_status.is_none_or(|status| status != 0)
}

/// Trait implemented by all versioned queues.
pub(crate) trait Queue: Send + Sync {
    /// Sets the watchdog allowance of the queue's compute commands to `requested_ms`
    /// (`drm_asahi_queue_create::compute_timeout_ms`, nonzero) and returns the allowance in
    /// effect. Only queues of GPUs that advertise DRM_ASAHI_FEATURE_COMPUTE_TIMEOUT take one.
    fn set_compute_timeout(&mut self, _requested_ms: u32) -> Result<u32> {
        Err(EINVAL)
    }

    fn reserve_render_scs(&mut self, layouts: [u64; 2]) -> Result {
        if layouts != [0; 2] {
            return Err(EINVAL);
        }
        Ok(())
    }

    fn submit(
        &mut self,
        reg_data: &AsahiRegData<'_>,
        id: u64,
        syncs: KVec<file::SyncItem>,
        in_sync_count: usize,
        cmdbuf_raw: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result;

    /// Waits for the work submitted so far to finish, before the mappings of a closing
    /// client go. Queues whose VMs keep user mappings for accepted jobs need not wait.
    fn drain(&mut self) {}
}

#[versions(AGX)]
struct SubQueue {
    wq: Arc<workqueue::WorkQueue::ver>,
}

#[versions(AGX)]
impl SubQueue::ver {
    fn new_job(&mut self, fence: dma_fence::Fence) -> SubQueueJob::ver {
        SubQueueJob::ver {
            wq: self.wq.clone(),
            fence: Some(fence),
            job: None,
        }
    }
}

#[versions(AGX)]
struct SubQueueJob {
    wq: Arc<workqueue::WorkQueue::ver>,
    job: Option<workqueue::Job::ver>,
    fence: Option<dma_fence::Fence>,
}

#[versions(AGX)]
impl SubQueueJob::ver {
    fn get(&mut self) -> Result<&mut workqueue::Job::ver> {
        if self.job.is_none() {
            mod_pr_debug!("SubQueueJob: Creating {:?} job\n", self.wq.pipe_type());
            self.job
                .replace(self.wq.new_job(self.fence.take().unwrap())?);
        }
        Ok(self.job.as_mut().expect("expected a Job"))
    }

    fn commit(&mut self) -> Result {
        match self.job.as_mut() {
            Some(job) => job.commit(),
            None => Ok(()),
        }
    }

    fn can_submit(&self) -> Option<Fence> {
        self.job.as_ref().and_then(|job| job.can_submit())
    }

    fn can_submit_inline(&self) -> bool {
        self.job.as_ref().is_none_or(|job| job.can_submit_inline())
    }
}

#[versions(AGX)]
pub(crate) struct Queue {
    dev: AsahiDevRef,
    _sched: sched::Scheduler<QueueJob::ver>,
    entity: sched::Entity<QueueJob::ver>,
    vm: mmu::Vm,
    q_vtx: Option<SubQueue::ver>,
    q_frag: Option<SubQueue::ver>,
    q_comp: Option<SubQueue::ver>,
    fence_ctx: FenceContexts,
    inner: QueueInner::ver,
    stats: Arc<stats::StatsSnapshot>,
    /// Finished fence of the last submitted job, waited for when the queue goes away.
    last_out: Option<Fence>,
    /// REFFLAKE4 test: this queue already submitted a render.
    first_render_done: bool,
}

#[versions(AGX)]
pub(crate) struct QueueInner {
    dev: AsahiDevRef,
    ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
    buffer: buffer::Buffer::ver,
    gpu_context: Arc<workqueue::GpuContext>,
    notifier_list: Arc<GpuObject<fw::event::NotifierList>>,
    notifier: Arc<GpuObject<fw::event::Notifier::ver>>,
    usc_exec_base: u64,
    usage: Arc<crate::usage::Client>,
    id: u64,
    #[ver(V >= V13_0B4)]
    counter: AtomicU64,
    /// REFFLAKE4 test: render submissions of this queue / last scene facts.
    sub_renders: AtomicU64,
    sub_last: AtomicU64,
    sub_stats: AtomicU64,
}

#[versions(AGX)]
#[derive(Default)]
pub(crate) struct JobFence {
    id: u64,
    pending: AtomicU64,
    stats: Option<Arc<stats::StatsSnapshot>>,
}

#[versions(AGX)]
impl JobFence::ver {
    fn add_command(self: &FenceObject<Self>) {
        self.pending.fetch_add(1, Ordering::Relaxed);
    }

    fn command_complete(self: &FenceObject<Self>) {
        let remain = self.pending.fetch_sub(1, Ordering::Relaxed) - 1;
        mod_pr_debug!(
            "JobFence[{}]: Command complete (remain: {})\n",
            self.id,
            remain
        );
        if remain == 0 {
            if let Some(stats) = &self.stats {
                stats.note_job();
            }
            mod_pr_debug!("JobFence[{}]: Signaling\n", self.id);
            self.signal();
        }
    }
}

#[versions(AGX)]
#[vtable]
impl dma_fence::FenceOps for JobFence::ver {
    fn get_driver_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("aurora")
    }
    fn get_timeline_name<'a>(self: &'a FenceObject<Self>) -> &'a CStr {
        c_str!("queue")
    }
}

#[versions(AGX)]
pub(crate) struct QueueJob {
    dev: AsahiDevRef,
    vm_bind: mmu::VmBind,
    op_guard: Option<gpu::OpGuard>,
    sj_vtx: Option<SubQueueJob::ver>,
    sj_frag: Option<SubQueueJob::ver>,
    sj_comp: Option<SubQueueJob::ver>,
    fence: UserFence<JobFence::ver>,
    notifier: Arc<GpuObject<fw::event::Notifier::ver>>,
    notification_count: u32,
    /// Zero until the ioctl tries publication, one for success, or a negative
    /// errno for a failed attempt. The scheduler consumes the recorded result.
    inline_result: i32,
    id: u64,
}

pub(crate) struct QueueTimeout {
    dev: AsahiDevRef,
    id: u64,
}

#[versions(AGX)]
impl QueueJob::ver {
    fn get_vtx(&mut self) -> Result<&mut workqueue::Job::ver> {
        self.sj_vtx
            .as_mut()
            .ok_or_else(|| {
                cls_pr_debug!(Errors, "No vertex queue\n");
                EINVAL
            })?
            .get()
    }
    fn get_frag(&mut self) -> Result<&mut workqueue::Job::ver> {
        self.sj_frag
            .as_mut()
            .ok_or_else(|| {
                cls_pr_debug!(Errors, "No fragment queue\n");
                EINVAL
            })?
            .get()
    }
    fn get_comp(&mut self) -> Result<&mut workqueue::Job::ver> {
        self.sj_comp
            .as_mut()
            .ok_or_else(|| {
                cls_pr_debug!(Errors, "No compute queue\n");
                EINVAL
            })?
            .get()
    }

    fn commit(&mut self) -> Result {
        mod_dev_dbg!(self.dev, "QueueJob {}: Committing\n", self.id);

        self.sj_vtx.as_mut().map(|a| a.commit()).unwrap_or(Ok(()))?;
        self.sj_frag
            .as_mut()
            .map(|a| a.commit())
            .unwrap_or(Ok(()))?;
        self.sj_comp.as_mut().map(|a| a.commit()).unwrap_or(Ok(()))
    }

    fn can_submit_inline(&self) -> bool {
        self.sj_vtx
            .as_ref()
            .is_none_or(|job| job.can_submit_inline())
            && self
                .sj_frag
                .as_ref()
                .is_none_or(|job| job.can_submit_inline())
            && self
                .sj_comp
                .as_ref()
                .is_none_or(|job| job.can_submit_inline())
    }
}

#[versions(AGX)]
impl sched::JobImpl for QueueJob::ver {
    type TimeoutData = QueueTimeout;
    const MODULE: Option<&'static kernel::ThisModule> =
        Some(kernel::module::this_module::<crate::LocalModule>());

    fn timeout_data(&self) -> QueueTimeout {
        QueueTimeout {
            dev: self.dev.clone(),
            id: self.id,
        }
    }

    fn prepare(job: &mut Self) -> Option<Fence> {
        mod_dev_dbg!(job.dev, "QueueJob {}: Checking runnability\n", job.id);

        if let Some(sj) = job.sj_vtx.as_ref() {
            if let Some(fence) = sj.can_submit() {
                mod_dev_dbg!(
                    job.dev,
                    "QueueJob {}: Blocking due to vertex queue full\n",
                    job.id
                );
                return Some(fence);
            }
        }
        if let Some(sj) = job.sj_frag.as_ref() {
            if let Some(fence) = sj.can_submit() {
                mod_dev_dbg!(
                    job.dev,
                    "QueueJob {}: Blocking due to fragment queue full\n",
                    job.id
                );
                return Some(fence);
            }
        }
        if let Some(sj) = job.sj_comp.as_ref() {
            if let Some(fence) = sj.can_submit() {
                mod_dev_dbg!(
                    job.dev,
                    "QueueJob {}: Blocking due to compute queue full\n",
                    job.id
                );
                return Some(fence);
            }
        }
        None
    }

    #[allow(unused_assignments)]
    fn run(job: &mut Self) -> Result<Option<dma_fence::Fence>> {
        if job.inline_result != 0 {
            return if job.inline_result == 1 {
                Ok(Some(Fence::from_fence(&job.fence)))
            } else {
                Err(Error::from_errno(job.inline_result))
            };
        }

        mod_dev_dbg!(job.dev, "QueueJob {}: Running Job\n", job.id);

        // We can only increase the notifier threshold here, now that we are
        // actually running the job. We cannot increase it while queueing the
        // job without introducing subtle race conditions. Suppose we did, as
        // early versions of drm/asahi did:
        //
        // 1. When processing the ioctl submit, a job is queued to drm_sched.
        //    Incorrectly, the notifier threshold is increased, gating firmware
        //    events.
        // 2. When DRM schedules an event, the hardware is kicked.
        // 3. When the number of processed jobs equals the threshold, the
        //    firmware signals the complete event to the kernel
        // 4. When the kernel gets a complete event, we signal the out-syncs.
        //
        // Does that work? There are a few scenarios.
        //
        // 1. There is nothing else ioctl submitted before the job completes.
        //    The job is scheduled, completes, and signals immediately.
        //    Everything works.
        // 2. There is nontrivial sync across different queues. Since each queue
        //    has a separate own notifier threshold, submitting one does not
        //    block scheduling of the other. Everything works the way you'd
        //    expect. drm/sched handles the wait/signal ordering.
        // 3. Two ioctls are submitted back-to-back. The first signals a fence
        //    that the second waits on. Due to the notifier threshold increment,
        //    the first job's completion event is deferred. But in good
        //    conditions, drm/sched will schedule the second submit anyway
        //    because it kills the pointless intra-queue sync. Then both
        //    commands execute and are signalled together.
        // 4. Two ioctls are submitted back-to-back as above, but conditions are
        //    bad. Reporting completion of the first job is still masked by the
        //    notifier threshold, but the intra-queue fences are not optimized
        //    out in drm/sched... drm/sched doesn't schedule the second job
        //    until the first is signalled, but the first isn't signalled until
        //    the second is completed, but the second can't complete until it's
        //    scheduled. We hang!
        //
        // In good conditions, everything works properly and/or we win the race
        // to mask the issue. So the issue here is challenging to hit.
        // Nevertheless, we do need to get it right.
        //
        // The intention with drm/sched is that jobs that are not yet scheduled
        // are "invisible" to the firmware. Incrementing the notifier threshold
        // earlier than this violates that which leads to circles like the
        // above. Deferring the increment to submit solves the race.
        let dev = job.dev.clone();
        let Some(guard) = dev.registration_guard() else {
            return Err(ENODEV);
        };

        guard.registration_data_with(|reg_data| {
            let gpu = match reg_data
                .gpu
                .clone()
                .arc_as_any()
                .downcast::<gpu::GpuManager::ver>()
            {
                Ok(gpu) => gpu,
                Err(_) => {
                    dev_crit!(job.dev.as_ref(), "GpuManager mismatched with QueueJob!\n");
                    return Err(EIO);
                }
            };

            if job.op_guard.is_none() {
                job.op_guard = Some(gpu.start_op()?);
            }

            // First submit all the commands for each queue. This can fail.

            let mut frag_job = None;
            let mut frag_sub = None;
            if let Some(sj) = job.sj_frag.as_mut() {
                frag_job = sj.job.take();
                if let Some(wqjob) = frag_job.as_mut() {
                    mod_dev_dbg!(job.dev, "QueueJob {}: Submit fragment\n", job.id);
                    frag_sub = Some(wqjob.submit()?);
                }
            }

            let mut vtx_job = None;
            let mut vtx_sub = None;
            if let Some(sj) = job.sj_vtx.as_mut() {
                vtx_job = sj.job.take();
                if let Some(wqjob) = vtx_job.as_mut() {
                    mod_dev_dbg!(job.dev, "QueueJob {}: Submit vertex\n", job.id);
                    vtx_sub = Some(wqjob.submit()?);
                }
            }

            let mut comp_job = None;
            let mut comp_sub = None;
            if let Some(sj) = job.sj_comp.as_mut() {
                comp_job = sj.job.take();
                if let Some(wqjob) = comp_job.as_mut() {
                    mod_dev_dbg!(job.dev, "QueueJob {}: Submit compute\n", job.id);
                    comp_sub = Some(wqjob.submit()?);
                }
            }

            // Prepared submissions still hold their workqueue locks and have not
            // published cpu_wptr. Only count them after every fallible submit()
            // succeeded, before the first run_job exposes commands to firmware.
            job.notifier.threshold.with(|raw, _inner| {
                raw.increase(job.notification_count);
            });

            // Now we fully commit to running the job
            mod_dev_dbg!(job.dev, "QueueJob {}: Run fragment\n", job.id);
            frag_sub.map(|a| gpu.run_job(a)).transpose()?;

            mod_dev_dbg!(job.dev, "QueueJob {}: Run vertex\n", job.id);
            vtx_sub.map(|a| gpu.run_job(a)).transpose()?;

            mod_dev_dbg!(job.dev, "QueueJob {}: Run compute\n", job.id);
            comp_sub.map(|a| gpu.run_job(a)).transpose()?;

            mod_dev_dbg!(job.dev, "QueueJob {}: Drop compute job\n", job.id);
            core::mem::drop(comp_job);
            mod_dev_dbg!(job.dev, "QueueJob {}: Drop vertex job\n", job.id);
            core::mem::drop(vtx_job);
            mod_dev_dbg!(job.dev, "QueueJob {}: Drop fragment job\n", job.id);
            core::mem::drop(frag_job);

            Ok(Some(Fence::from_fence(&job.fence)))
        })
    }

    fn timed_out(job: &mut QueueTimeout, _finished: bool) -> sched::Status {
        // FIXME: Handle timeouts properly
        dev_err!(
            job.dev.as_ref(),
            "QueueJob {}: Job timed out on the DRM scheduler, things will probably break\n",
            job.id
        );
        sched::Status::NoDevice
    }

    fn cancel(job: &mut Self) {
        dev_info!(
            job.dev.as_ref(),
            "QueueJob {}: Job canceled on DRM scheduler teardown\n",
            job.id
        );
    }
}

#[versions(AGX)]
impl Drop for QueueJob::ver {
    fn drop(&mut self) {
        mod_dev_dbg!(self.dev, "QueueJob {}: Dropping\n", self.id);
    }
}

static QUEUE_NAME: &CStr = c_str!("aurora_fence");
static QUEUE_CLASS_KEY: Pin<&LockClassKey> = kernel::static_lock_class!();

#[versions(AGX)]
impl Queue::ver {
    /// Create a new user queue.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn new(
        dev: AsahiDevice,
        gpu: &dyn GpuManager,
        vm: mmu::Vm,
        alloc: &mut gpu::KernelAllocators,
        ualloc: Arc<Mutex<alloc::DefaultAllocator>>,
        ualloc_priv: Arc<Mutex<alloc::DefaultAllocator>>,
        event_manager: Arc<event::EventManager>,
        stats: Arc<stats::StatsSnapshot>,
        mgr: &buffer::BufferManager::ver,
        id: u64,
        priority: u32,
        usc_exec_base: u64,
        usage: Arc<crate::usage::Client>,
    ) -> Result<Queue::ver> {
        mod_dev_dbg!(dev, "[Queue {}] Creating queue\n", id);

        // Must be shared, no cache management on this one!
        let mut notifier_list = alloc.shared.new_default::<fw::event::NotifierList>()?;

        let self_ptr = notifier_list.weak_pointer();
        notifier_list.with_mut(|raw, _inner| {
            raw.list_head.next = Some(inner_weak_ptr!(self_ptr, list_head));
        });

        let threshold = alloc.shared.new_default::<fw::event::Threshold>()?;

        let notifier: Arc<GpuObject<fw::event::Notifier::ver>> = Arc::new(
            alloc.private.new_init(
                /*try_*/ init!(fw::event::Notifier::ver { threshold }),
                |inner, _p| {
                    try_init!(fw::event::raw::Notifier::ver {
                        threshold: inner.threshold.gpu_pointer(),
                        generation: AtomicU32::new(id as u32),
                        cur_count: AtomicU32::new(0),
                        unk_10: AtomicU32::new(0x50),
                        state: Default::default()
                    })
                },
            )?,
            GFP_KERNEL,
        )?;

        // Priorities are handled by the AGX scheduler, there is no meaning within a
        // per-queue scheduler. Use a single run queue wth Kernel priority.
        let sched = sched::Scheduler::new(
            dev.as_ref().as_ref(),
            1,
            WQ_SIZE,
            0,
            100000,
            c_str!("aurora_sched"),
        )?;
        let entity = sched::Entity::new(&sched, sched::Priority::Kernel)?;

        let buffer = buffer::Buffer::ver::new(gpu, alloc, ualloc.clone(), ualloc_priv, mgr)?;

        let gpu_context = Arc::new(
            workqueue::GpuContext::new(dev.clone(), alloc, buffer.any_ref())?,
            GFP_KERNEL,
        )?;
        // Every command of this queue points the firmware at the notifier, and the notifier links
        // into the notifier list of the work queues. Keep it as long as they are kept, until the
        // context is invalidated, instead of freeing it with the last command.
        gpu_context.retain(KBox::new(notifier.clone(), GFP_KERNEL)?)?;

        let mut ret = Queue::ver {
            dev: dev.clone(),
            _sched: sched,
            entity,
            vm,
            q_vtx: None,
            q_frag: None,
            q_comp: None,
            fence_ctx: FenceContexts::new(1, QUEUE_NAME, QUEUE_CLASS_KEY)?,
            stats,
            last_out: None,
            first_render_done: false,
            inner: QueueInner::ver {
                dev: dev.clone(),
                ualloc,
                gpu_context,
                buffer,
                notifier_list: Arc::new(notifier_list, GFP_KERNEL)?,
                notifier,
                usc_exec_base,
                usage,
                id,
                #[ver(V >= V13_0B4)]
                counter: AtomicU64::new(0),
                sub_renders: AtomicU64::new(0),
                sub_last: AtomicU64::new(0),
                sub_stats: AtomicU64::new(0),
            },
        };

        // Rendering structures
        let tvb_blocks = module_parameters::initial_tvb_size.value();

        ret.inner.buffer.ensure_blocks(tvb_blocks)?;

        ret.q_vtx = Some(SubQueue::ver {
            wq: workqueue::WorkQueue::ver::new(
                dev.clone(),
                gpu.get_cfg(),
                alloc,
                event_manager.clone(),
                ret.inner.gpu_context.clone(),
                ret.inner.notifier_list.clone(),
                channel::PipeType::Vertex,
                id,
                priority,
                WQ_SIZE,
            )?,
        });

        ret.q_frag = Some(SubQueue::ver {
            wq: workqueue::WorkQueue::ver::new(
                dev.clone(),
                gpu.get_cfg(),
                alloc,
                event_manager.clone(),
                ret.inner.gpu_context.clone(),
                ret.inner.notifier_list.clone(),
                channel::PipeType::Fragment,
                id,
                priority,
                WQ_SIZE,
            )?,
        });

        // Compute structures
        ret.q_comp = Some(SubQueue::ver {
            wq: workqueue::WorkQueue::ver::new(
                dev.clone(),
                gpu.get_cfg(),
                alloc,
                event_manager,
                ret.inner.gpu_context.clone(),
                ret.inner.notifier_list.clone(),
                channel::PipeType::Compute,
                id,
                priority,
                WQ_SIZE,
            )?,
        });

        mod_dev_dbg!(dev, "[Queue {}] Queue created\n", id);
        Ok(ret)
    }
}

const SQ_RENDER: usize = 0;
const SQ_COMPUTE: usize = 1;
const SQ_COUNT: usize = 2;

/// Bytes of a render command this queue implements. The fields from `ts_frag_end_copies` on
/// belong to other runtimes and must be zero here, as must any bytes beyond the kernel's
/// structure.
const RENDER_SUPPORTED_SIZE: usize =
    core::mem::offset_of!(uapi::drm_asahi_cmd_render, ts_frag_end_copies);
/// Bytes of a compute command this queue implements; see [`RENDER_SUPPORTED_SIZE`].
const COMPUTE_SUPPORTED_SIZE: usize =
    core::mem::offset_of!(uapi::drm_asahi_cmd_compute, ts_end_copies);

// SAFETY: All bit patterns are valid by construction.
unsafe impl AnyBitPattern for uapi::drm_asahi_cmd_header {}
// SAFETY: This UAPI record consists of integer fields and nested integer records.
unsafe impl AnyBitPattern for uapi::drm_asahi_cmd_render {}
// SAFETY: This UAPI record consists of integer fields and nested integer records.
unsafe impl AnyBitPattern for uapi::drm_asahi_cmd_compute {}
// SAFETY: This UAPI record consists of integer fields and nested integer records.
unsafe impl AnyBitPattern for uapi::drm_asahi_attachment {}

fn build_attachments(reader: &mut Reader<'_>, size: usize) -> Result<microseq::Attachments> {
    const STRIDE: usize = core::mem::size_of::<uapi::drm_asahi_attachment>();
    let count = size / STRIDE;

    if count > microseq::MAX_ATTACHMENTS {
        return Err(EINVAL);
    }

    let mut attachments = microseq::Attachments {
        count: count as u32,
        ..Default::default()
    };

    for i in 0..count {
        let att: uapi::drm_asahi_attachment = reader.read()?;

        if att.flags != 0 || att.pad != 0 {
            return Err(EINVAL);
        }

        // Some kind of power-of-2 exponent related to attachment size, in
        // bounds [1, 6]? We don't know what this is exactly yet.
        let unk_e = 1;

        let cache_lines = (att.size + 127) >> 7;
        attachments.list[i] = microseq::Attachment {
            address: U64(att.pointer),
            size: cache_lines.try_into()?,
            unk_c: 0x17,
            unk_e: unk_e as u16,
        };
    }

    Ok(attachments)
}

#[versions(AGX)]
impl Queue::ver {
    /// Waits, for at most [`QUEUE_CLOSE_WAIT_MS`], for the last submitted job to finish. The
    /// jobs of one queue complete in order, so its finished fence covers all of them.
    fn wait_submitted(&self) {
        let Some(fence) = self.last_out.as_ref() else {
            return;
        };
        // SAFETY: `fence` holds a reference for the duration of the call. No lock is held,
        // so completion can signal it.
        let left = unsafe {
            kernel::bindings::dma_fence_wait_timeout(
                fence.raw(),
                false,
                kernel::time::msecs_to_jiffies(QUEUE_CLOSE_WAIT_MS) as _,
            )
        };
        if left == 0 {
            dev_warn!(
                self.dev.as_ref(),
                "[Queue {}] Closing with work still running after {} ms\n",
                self.inner.id,
                QUEUE_CLOSE_WAIT_MS
            );
        }
    }
}

#[versions(AGX)]
impl Queue for Queue::ver {
    fn drain(&mut self) {
        self.wait_submitted();
    }

    fn submit<'drm>(
        &mut self,
        reg_data: &AsahiRegData<'drm>,
        id: u64,
        mut syncs: KVec<file::SyncItem>,
        in_sync_count: usize,
        cmdbuf_raw: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        let gpu = match reg_data
            .gpu
            .clone()
            .arc_as_any()
            .downcast::<gpu::GpuManager::ver>()
        {
            Ok(gpu) => gpu,
            Err(_) => {
                dev_crit!(self.dev.as_ref(), "GpuManager mismatched with JobImpl!\n");
                return Err(EIO);
            }
        };

        mod_dev_dbg!(self.dev, "[Submission {}] Submit job\n", id);

        if gpu.is_crashed() {
            dev_err!(
                self.dev.as_ref(),
                "[Submission {}] GPU is crashed, cannot submit\n",
                id
            );
            return Err(ENODEV);
        }

        let op_guard = if in_sync_count > 0 {
            Some(gpu.start_op()?)
        } else {
            None
        };

        let mut events: [KVec<Option<workqueue::QueueEventInfo::ver>>; SQ_COUNT] =
            Default::default();

        events[SQ_RENDER].push(
            self.q_frag.as_ref().and_then(|a| a.wq.event_info()),
            GFP_KERNEL,
        )?;
        events[SQ_COMPUTE].push(
            self.q_comp.as_ref().and_then(|a| a.wq.event_info()),
            GFP_KERNEL,
        )?;

        let vm_bind = gpu.bind_vm(&self.vm)?;
        let vm_slot = vm_bind.slot();

        mod_dev_dbg!(self.dev, "[Submission {}] Creating job\n", id);

        // FIXME: I think this can violate the fence seqno ordering contract.
        // If we have e.g. a render submission with no barriers and then a compute submission
        // with no barriers, it's possible for the compute submission to complete first, and
        // therefore its fence. Maybe we should have separate fence contexts for render
        // and compute, and then do a ? (Vert+frag should be fine since there is no vert
        // without frag, and frag always serializes.)
        let fence: UserFence<JobFence::ver> = self
            .fence_ctx
            .new_fence::<JobFence::ver>(
                0,
                JobFence::ver {
                    id,
                    pending: Default::default(),
                    stats: Some(self.stats.clone()),
                },
            )?
            .into();

        let mut cmdbuf = Reader::new(cmdbuf_raw);

        // First, parse the headers to determine the number of compute/render
        // commands. This will be used to determine when to flush stamps.
        //
        // We also use it to determine how many notifications the job will
        // generate. We could calculate that in the second pass since we don't
        // need until much later, but it's convenient to gather everything at
        // the same time.
        let mut nr_commands = 0;
        let mut last_compute = 0;
        let mut last_render = 0;
        let mut nr_render = 0;
        let mut nr_compute = 0;
        let idle_grow = debug_enabled(DebugFlags::Debug7);
        let mut max_tvb_blocks = 0usize;

        while !cmdbuf.is_empty() {
            let header: uapi::drm_asahi_cmd_header = cmdbuf.read()?;
            let payload_size = header.size as usize;
            match u32::from(header.cmd_type) {
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER => {
                    let render: uapi::drm_asahi_cmd_render = cmdbuf.read_up_to(payload_size)?;
                    if render
                        .ts_frag_end_copies
                        .iter()
                        .any(|ts| ts.handle != 0 || ts.offset != 0)
                    {
                        return Err(EINVAL);
                    }
                    if idle_grow {
                        max_tvb_blocks = max_tvb_blocks
                            .max(QueueInner::ver::render_min_tvb_blocks(&gpu, &render)?);
                    }
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE => {
                    let compute: uapi::drm_asahi_cmd_compute = cmdbuf.read_up_to(payload_size)?;
                    if compute
                        .ts_end_copies
                        .iter()
                        .any(|ts| ts.handle != 0 || ts.offset != 0)
                    {
                        return Err(EINVAL);
                    }
                }
                _ => cmdbuf.skip(payload_size),
            }
            nr_commands += 1;

            match u32::from(header.cmd_type) {
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER => {
                    last_render = nr_commands;
                    nr_render += 1;
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE => {
                    last_compute = nr_commands;
                    nr_compute += 1;
                }
                _ => {}
            }
        }

        let gate_on = firstgate::tracking() && nr_render > 0;
        let gate_first = firstgate::enabled() && gate_on && !self.first_render_done;
        let gate_excl = gate_first && firstgate::enter_first();
        let _gate_tok = if gate_on && !gate_first {
            Some(firstgate::enter_normal())
        } else {
            None
        };
        // Release the exclusive window on every early-return path too.
        struct ExclGuard(bool);
        impl Drop for ExclGuard {
            fn drop(&mut self) {
                if self.0 {
                    firstgate::leave_first();
                }
            }
        }
        let excl_guard = ExclGuard(gate_excl);

        let mut job = self.entity.new_job(
            1,
            QueueJob::ver {
                dev: self.dev.clone(),
                vm_bind,
                op_guard,
                sj_vtx: self
                    .q_vtx
                    .as_mut()
                    .map(|a| a.new_job(Fence::from_fence(&fence))),
                sj_frag: self
                    .q_frag
                    .as_mut()
                    .map(|a| a.new_job(Fence::from_fence(&fence))),
                sj_comp: self
                    .q_comp
                    .as_mut()
                    .map(|a| a.new_job(Fence::from_fence(&fence))),
                fence,
                notifier: self.inner.notifier.clone(),

                // Each render command generates 2 notifications: 1 for the
                // vertex part, 1 for the fragment part. Each compute command
                // generates 1 notification. Sum up to calculate the total
                // notification count for the job.
                notification_count: (2 * nr_render) + nr_compute,

                inline_result: 0,

                id,
            },
        )?;

        mod_dev_dbg!(
            self.dev,
            "[Submission {}] Adding {} in_syncs\n",
            id,
            in_sync_count
        );
        let mut dependencies_ready = true;
        for sync in syncs.drain(0..in_sync_count) {
            if let Some(fence) = sync.fence {
                // SAFETY: `fence` owns the live dma_fence reference used for
                // this concurrent status query.
                dependencies_ready &=
                    unsafe { kernel::bindings::dma_fence_get_status(fence.raw()) != 0 };
                job.add_dependency(fence)?;
            }
        }

        // Validate the number of hardware commands, ignoring software commands
        let nr_hw_commands = nr_render + nr_compute;
        if nr_hw_commands == 0 || nr_hw_commands > MAX_COMMANDS_PER_SUBMISSION {
            cls_pr_debug!(
                Errors,
                "submit: Command count {} out of valid range [1, {}]\n",
                nr_hw_commands,
                MAX_COMMANDS_PER_SUBMISSION - 1
            );
            return Err(EINVAL);
        }

        // Grow the TVB only while the firmware is not using it: the buffer manager owns a buffer
        // while scenes on it are in flight, and growing it then (new blocks plus an InitBuffer of
        // the bound slot) corrupts their tiling on G13 clustered rendering. Size the buffer for every
        // render of this submission before any of it is published, after this queue's earlier work
        // has finished. Required growth waits for that work; stats-driven growth only happens on an
        // idle queue, as the firmware copes with a full heap itself.
        let mut pre_grown = false;
        if idle_grow && nr_render > 0 {
            let busy = self.last_out.as_ref().is_some_and(|fence| {
                // SAFETY: `last_out` owns the live dma_fence reference used for this query.
                unsafe { kernel::bindings::dma_fence_get_status(fence.raw()) == 0 }
            });
            let need = max_tvb_blocks > self.inner.buffer.block_count() as usize;
            if need && busy {
                let fence = self.last_out.as_ref().expect("busy queue has a last fence");
                // SAFETY: `fence` holds a reference for the duration of the call; no lock that its
                // completion needs is held.
                let left = unsafe {
                    kernel::bindings::dma_fence_wait_timeout(
                        fence.raw(),
                        false,
                        kernel::time::msecs_to_jiffies(QUEUE_CLOSE_WAIT_MS) as _,
                    )
                };
                if left <= 0 {
                    dev_err!(
                        self.dev.as_ref(),
                        "[Queue {}] Earlier work still running after {} ms, cannot grow the TVB\n",
                        self.inner.id,
                        QUEUE_CLOSE_WAIT_MS
                    );
                    return Err(ETIMEDOUT);
                }
            }
            if need || !busy {
                if !busy {
                    pre_grown |= self.inner.buffer.auto_grow_idle()?;
                }
                pre_grown |= self.inner.buffer.ensure_blocks_idle(max_tvb_blocks)?;
            }
        }

        cmdbuf.rewind();

        let mut command_index = 0;
        let mut vertex_attachments: microseq::Attachments = Default::default();
        let mut fragment_attachments: microseq::Attachments = Default::default();
        let mut compute_attachments: microseq::Attachments = Default::default();

        // Parse the full command buffer submitting as we go
        while !cmdbuf.is_empty() {
            let header: uapi::drm_asahi_cmd_header = cmdbuf.read()?;
            let header_size = header.size as usize;

            // Pre-increment command index to match last_compute/last_render
            command_index += 1;

            for (queue_idx, index) in [header.vdm_barrier, header.cdm_barrier].iter().enumerate() {
                if *index == uapi::DRM_ASAHI_BARRIER_NONE as u16 {
                    continue;
                }
                if let Some(event) = events[queue_idx].get(*index as usize).ok_or_else(|| {
                    cls_pr_debug!(Errors, "Invalid barrier #{}: {}\n", queue_idx, index);
                    EINVAL
                })? {
                    let mut alloc = gpu.alloc();
                    let queue_job = match u32::from(header.cmd_type) {
                        uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER => job.get_vtx()?,
                        uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE => job.get_comp()?,
                        _ => return Err(EINVAL),
                    };
                    mod_dev_dbg!(self.dev, "[Submission {}] Create Explicit Barrier\n", id);
                    let barrier = alloc.private.new_init(
                        pin_init::zeroed::<fw::workqueue::Barrier>(),
                        |_inner, _p| {
                            let queue_job = &queue_job;
                            try_init!(fw::workqueue::raw::Barrier {
                                tag: fw::workqueue::CommandType::Barrier,
                                wait_stamp: event.fw_stamp_pointer,
                                wait_value: event.value,
                                wait_slot: event.slot,
                                stamp_self: queue_job.event_info().value.next(),
                                uuid: 0xffffbbbb,
                                external_barrier: 0,
                                internal_barrier_type: 1,
                                padding: Default::default(),
                            })
                        },
                    )?;
                    mod_dev_dbg!(self.dev, "[Submission {}] Add Explicit Barrier\n", id);
                    queue_job.add(barrier, vm_slot)?;
                } else {
                    assert!(*index == 0);
                }
            }

            match u32::from(header.cmd_type) {
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_RENDER => {
                    let render: uapi::drm_asahi_cmd_render = cmdbuf
                        .read_supported(header_size, RENDER_SUPPORTED_SIZE)
                        .inspect_err(|_| {
                            cls_pr_debug!(Errors, "Unsupported render command fields\n")
                        })?;

                    self.inner.submit_render(
                        &gpu,
                        &mut job,
                        &render,
                        &vertex_attachments,
                        &fragment_attachments,
                        objects,
                        id,
                        command_index == last_render,
                        core::mem::take(&mut pre_grown),
                    )?;
                    events[SQ_RENDER].push(
                        Some(
                            job.sj_frag
                                .as_ref()
                                .expect("No frag queue?")
                                .job
                                .as_ref()
                                .expect("No frag job?")
                                .event_info(),
                        ),
                        GFP_KERNEL,
                    )?;
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_CMD_COMPUTE => {
                    let compute: uapi::drm_asahi_cmd_compute = cmdbuf
                        .read_supported(header_size, COMPUTE_SUPPORTED_SIZE)
                        .inspect_err(|_| {
                            cls_pr_debug!(Errors, "Unsupported compute command fields\n")
                        })?;

                    self.inner.submit_compute(
                        &gpu,
                        &mut job,
                        &compute,
                        &compute_attachments,
                        objects,
                        id,
                        command_index == last_compute,
                    )?;
                    events[SQ_COMPUTE].push(
                        Some(
                            job.sj_comp
                                .as_ref()
                                .expect("No comp queue?")
                                .job
                                .as_ref()
                                .expect("No comp job?")
                                .event_info(),
                        ),
                        GFP_KERNEL,
                    )?;
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_VERTEX_ATTACHMENTS => {
                    vertex_attachments = build_attachments(&mut cmdbuf, header_size)?;
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_FRAGMENT_ATTACHMENTS => {
                    fragment_attachments = build_attachments(&mut cmdbuf, header_size)?;
                }
                uapi::drm_asahi_cmd_type_DRM_ASAHI_SET_COMPUTE_ATTACHMENTS => {
                    compute_attachments = build_attachments(&mut cmdbuf, header_size)?;
                }
                _ => {
                    cls_pr_debug!(Errors, "Unknown command type {}\n", header.cmd_type);
                    return Err(EINVAL);
                }
            }
        }

        let sub_slots = if subtrace::enabled() && nr_render > 0 {
            let v = job.get_vtx().map(|j| j.event_info().slot as u64).unwrap_or(0xff);
            let f = job.get_frag().map(|j| j.event_info().slot as u64).unwrap_or(0xff);
            Some((v & 0xff) << 32 | (f & 0xff) << 40 | (vm_slot as u64 & 0xff) << 48)
        } else {
            None
        };
        mod_dev_dbg!(
            self.dev,
            "Queue {}: Committing job {}\n",
            self.inner.id,
            job.id
        );
        job.commit()?;

        mod_dev_dbg!(self.dev, "Queue {}: Arming job {}\n", self.inner.id, job.id);
        let mut job = job.arm();
        let out_fence = job.fences().finished();
        let previous_status = self.last_out.as_ref().map(|fence| {
            // SAFETY: `last_out` owns the live dma_fence reference used for
            // this concurrent status query.
            unsafe { kernel::bindings::dma_fence_get_status(fence.raw()) }
        });
        if module_parameters::legacy_inline_submit.value() != 0
            && can_submit_inline(dependencies_ready, previous_status)
            && job.can_submit_inline()
        {
            // All fallible construction and scheduler arming has completed.
            // Regardless of this result the scheduler takes ownership below,
            // preserving timeout, credit, error, and teardown bookkeeping.
            let result = <QueueJob::ver as sched::JobImpl>::run(&mut job).map(|_| ());
            job.inline_result = result.map_or_else(|error| error.to_errno(), |()| 1);
        }
        if let Some(slots) = sub_slots {
            let ridx = self.inner.sub_renders.fetch_add(1, Ordering::Relaxed);
            let last = self.inner.sub_last.load(Ordering::Relaxed);
            let a = (ridx & 0xff_ffff) | ((nr_render as u64 & 0xff) << 24) | slots | ((last & 0x7f) << 56);
            let b = u64::from(job.inline_result != 0) | ((last >> 7) & 0b11110) | (self.inner.id << 8);
            subtrace::record(a, b, self.inner.sub_stats.load(Ordering::Relaxed));
        }
        self.last_out = Some(out_fence.clone());
        mod_dev_dbg!(
            self.dev,
            "Queue {}: Pushing job {}\n",
            self.inner.id,
            job.id
        );
        job.push();

        mod_dev_dbg!(
            self.dev,
            "Queue {}: Adding {} out_syncs\n",
            self.inner.id,
            syncs.len()
        );
        for mut sync in syncs {
            if let Some(chain) = sync.chain_fence.take() {
                sync.syncobj
                    .add_point(chain, &out_fence, sync.timeline_value);
            } else {
                sync.syncobj.replace_fence(Some(&out_fence));
            }
        }

        if gate_first {
            self.first_render_done = true;
            if excl_guard.0 {
                // SAFETY: `out_fence` holds a reference for the duration of the call; no lock is
                // held that its completion needs.
                let left = unsafe {
                    kernel::bindings::dma_fence_wait_timeout(
                        out_fence.raw(),
                        false,
                        kernel::time::msecs_to_jiffies(3000) as _,
                    )
                };
                if left == 0 {
                    let n = firstgate::TIMEOUTS.fetch_add(1, core::sync::atomic::Ordering::Relaxed) + 1;
                    pr_warn!("firstgate: first render not done after 3000 ms (timeouts={})\n", n);
                }
            }
        }
        drop(excl_guard);

        Ok(())
    }
}

#[kunit_tests(asahi_legacy_inline_queue)]
mod tests {
    use super::can_submit_inline;

    #[test]
    fn depth_192_only_publishes_the_idle_head_inline() {
        const DEPTH: usize = 192;
        let mut previous_status = None;
        let mut inline = 0;

        for _ in 0..DEPTH {
            if can_submit_inline(true, previous_status) {
                inline += 1;
            }
            // Every later ioctl sees the preceding logical queue fence. A
            // burst cannot publish around that unsignalled scheduler owner.
            previous_status = Some(0);
        }

        assert_eq!(inline, 1);
    }

    #[test]
    fn serial_round_trips_stay_eligible() {
        const SUBMITS: usize = 192;

        for index in 0..SUBMITS {
            let previous_status = (index != 0).then_some(1);
            assert!(can_submit_inline(true, previous_status));
        }
    }

    #[test]
    fn unsignalled_dependency_or_predecessor_falls_back() {
        assert!(!can_submit_inline(false, None));
        assert!(!can_submit_inline(true, Some(0)));
        assert!(can_submit_inline(true, Some(-5)));
    }
}

#[versions(AGX)]
impl Drop for Queue::ver {
    fn drop(&mut self) {
        mod_dev_dbg!(self.dev, "[Queue {}] Dropping queue\n", self.inner.id);
        subtrace::mark(4, self.inner.id);
        // A client that exits or is killed drops its queues with jobs still queued or running
        // on the GPU, and its VMs right after (see `file::File`). Those VMs do not track jobs:
        // dropping one unmaps the user range at once, and the firmware then faults on the
        // next access of the running job and times out. Let the submitted work finish first.
        self.wait_submitted();
        subtrace::mark(5, self.inner.id);
    }
}

/// How long a closing queue waits for its submitted work to finish.
const QUEUE_CLOSE_WAIT_MS: u32 = 5000;
