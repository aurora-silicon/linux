// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Logical queues: `crate::queue::Queue` for G16.
//!
//! Logical queues own no firmware state: they are independent scheduling entities over the one
//! shared graph, sharing the device's compute and render engines across queues and VMs. A
//! per-file host credit bounds each open DRM file's live logical queues to the advertised
//! `MAX_QUEUES` (ENOSPC when full) while `file.rs`'s `queue_share::Share` bounds multi-file
//! hoarding per process, so one client cannot exhaust logical-queue creation for another
//! process (such as a compositor). It reserves no hardware graph or execution context.
//! QUEUE_CREATE validates
//! `usc_exec_base` (4 GiB aligned, the window inside the user range) and refuses a failed VM or
//! GPU (EIO); HIGH/REALTIME are refused by the shared `file.rs`. `render_scs_layout` (the
//! largest ray-query scratch layouts of the queue's render stages) is reserved at creation:
//! EINVAL for a malformed layout, E2BIG beyond the largest SCS-capable UMA pool; a render
//! whose scratch exceeds the reservation is refused (EINVAL), as on the M4 Pro. The
//! reservation also sizes the SCS-capable render pools its VM creates for it
//! ([`super::rt::pool_seeds`]).
//!
//! QUEUE_DESTROY (dropping the queue) drains: the scheduler entity is torn down first, which
//! waits (killable) until every accepted job has been handed to the engine; the engine then runs
//! them to completion and their fences signal in order. A closing process waits at most a second
//! for jobs still waiting for inputs and cancels them (ECANCELED, in fence order).
//!
//! SUBMIT checks, in order: the stream's structure and the sync count (EINVAL), the VM's state
//! (a failed VM refuses with EIO), the timestamp handles and exact historical ordinals,
//! a job reference on the VM, then every
//! command's spans and stream gates ([`super::command`]). The accepted job is pushed to the
//! queue's scheduler and its fence published in every output sync; a refused SUBMIT publishes a
//! signalled error fence instead.
//!
//! A failed input cancels its unexecuted accepted ordinals (ECANCELED), preserving each
//! exact-prior hole. Earlier accepted work continues; later commands at/after a cancelled
//! ordinal, or depending on its failed prefix, cannot execute. The VM remains usable.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use kernel::{prelude::*, sync::Arc, uapi, xarray};

use super::{
    command::{self, USC_WINDOW},
    fence::{InputFences, Syncs, Timeline},
    hw,
    job::{Job, Scheduler},
    params,
    vm::{self, VmState},
    Gpu, Shared,
};
use crate::{file, mmu};

/// Per-queue state the engine reads.
pub(crate) struct QueueState {
    /// Host priority: `DRM_ASAHI_PRIORITY_REALTIME - priority` as `file.rs` passes it (MEDIUM 2,
    /// LOW 3; `file.rs` refuses HIGH and REALTIME). Smaller is served first.
    pub(crate) priority: u32,
    /// Completed stage time of the DRM file that created this queue (deduplicated per file).
    pub(crate) usage: Arc<crate::usage::Client>,
    /// USC executable window base.
    pub(crate) usc_base: u64,
    /// First cancelled accepted ordinal per engine; zero means none. Later scheduler
    /// work can fail while an older job is still running, so cancellation has a frontier.
    cancelled_at: [AtomicU64; 2],
    /// Engine service turn of this queue (least recently served first).
    pub(crate) turn: AtomicU64,
    /// Exact successful firmware receipts, independently for render and compute. These
    /// survive aggregate-fence publication and never advance on aborted/failed commands.
    pub(crate) completed: [AtomicU64; 2],
    /// Out-of-order full compute receipts strictly above `completed[COMPUTE] + 1` (bit `k`
    /// records ordinal `completed[COMPUTE] + 2 + k`), enabling concurrent 2-pipe completion.
    pub(crate) compute_ooo: AtomicU64,
    /// Published command frontier per engine. The firmware runs each queue pipe in
    /// publication order, each command to its Finalize, so a compute command may follow its
    /// published (not yet retired) compute predecessors (on the same pipe or via a cross-pipe
    /// barrier).
    pub(crate) published: [AtomicU64; 2],
    /// Owned-job counts at the queue's last 32 arrivals (compute horizon ahead), recorded by the
    /// engine when a job arrives.
    pub(crate) owned: [AtomicU32; 32],
    pub(crate) owned_index: AtomicU32,
    /// EWMA of retired per-command CDM GPU execution duration (microseconds) for adaptive
    /// 1-pipe affinity vs 2-pipe heavy compute fanout.
    pub(crate) compute_ewma_us: AtomicU32,
}

impl QueueState {
    /// Preserve the earliest hole without cancelling commands accepted before it.
    pub(crate) fn cancel(&self, orders: &[command::Order], first: u32) {
        for order in orders.iter().skip(first as usize) {
            let _ = self.cancelled_at[order.engine].fetch_update(
                Ordering::AcqRel,
                Ordering::Acquire,
                |previous| (previous == 0 || order.sequence < previous).then_some(order.sequence),
            );
        }
    }

    /// A command at/after a cancelled ordinal, or depending on that failed prefix,
    /// cannot execute. Earlier accepted work has its own exact receipt obligations.
    pub(crate) fn check(&self, order: &command::Order) -> Result {
        for engine in 0..2 {
            let first = self.cancelled_at[engine].load(Ordering::Acquire);
            if first != 0
                && ((order.engine == engine && order.sequence >= first)
                    || order.wait_through[engine].is_some_and(|last| last >= first))
            {
                return Err(ECANCELED);
            }
        }
        Ok(())
    }

    /// Whether any accepted ordinal of `engine` at or before `seq` was cancelled.
    pub(crate) fn cancelled_through(&self, engine: usize, seq: u64) -> Result {
        let first = self.cancelled_at[engine].load(Ordering::Acquire);
        if first != 0 && first <= seq {
            return Err(ECANCELED);
        }
        Ok(())
    }

    /// Whether compute ordinal `seq` already has a full successful firmware receipt, either
    /// at or below the contiguous `completed[COMPUTE]` frontier or recorded in `compute_ooo`.
    pub(crate) fn is_compute_retired(&self, seq: u64) -> bool {
        let completed = self.completed[command::COMPUTE].load(Ordering::Acquire);
        if seq <= completed {
            return true;
        }
        let shift = seq.wrapping_sub(completed).wrapping_sub(2);
        shift < 64 && (self.compute_ooo.load(Ordering::Acquire) & (1u64 << shift)) != 0
    }

    /// Updates the lock-free EWMA of recent retired per-command CDM GPU execution duration
    /// (microseconds) for this queue using a symmetric 50% step so transitions between light
    /// and heavy compute streams adapt within 2-3 completions.
    pub(crate) fn note_compute_us(&self, us: u32) {
        let us = us.clamp(1, 100_000);
        let prev = self.compute_ewma_us.load(Ordering::Relaxed);
        let next = if prev == 0 {
            us
        } else {
            prev.saturating_add(us) / 2
        };
        self.compute_ewma_us.store(next, Ordering::Relaxed);
    }
}

/// One per-file logical-queue credit, released after the queue scheduler drains.
struct QueueCredit {
    usage: Arc<crate::usage::Client>,
}

impl QueueCredit {
    fn acquire(usage: &Arc<crate::usage::Client>) -> Result<Self> {
        if !usage.try_acquire_queue(hw::sizes::MAX_QUEUES) {
            return Err(ENOSPC);
        }
        Ok(Self {
            usage: usage.clone(),
        })
    }
}

impl Drop for QueueCredit {
    fn drop(&mut self) {
        self.usage.release_queue();
    }
}

/// A G16 logical queue.
pub(crate) struct Queue {
    // Dropped first: QUEUE_DESTROY drains the scheduler before anything else goes.
    scheduler: Scheduler,
    pub(crate) state: Arc<QueueState>,
    /// Commands accepted per engine (render, compute) since creation (checked for accounting overflow).
    pub(crate) accepted: [u64; 2],
    /// Epoch-validated GPUVM span cache reused across submits on this logical queue.
    proof: KBox<command::SpanProof>,
    vm: Arc<VmState>,
    shared: Arc<Shared>,
    timeline: Arc<Timeline>,
    /// Enabled GPU cores, which SCS memory is sized for.
    active_cores: u32,
    /// Hardware capabilities for stream admission and scratch reservations.
    cfg: &'static hw::Config,
    /// Reserved render scratch pages per stage (`render_scs_layout`).
    render_scs: [u16; 2],
    // Last field: all preceding fields (especially scheduler) drop before capacity returns.
    _credit: QueueCredit,
}

impl Queue {
    /// Creates a logical queue. `priority` is `DRM_ASAHI_PRIORITY_REALTIME - priority`, as
    /// `file.rs` passes it.
    pub(crate) fn new(
        gpu: &Gpu,
        vm: mmu::Vm,
        priority: u32,
        usc_exec_base: u64,
        usage: Arc<crate::usage::Client>,
    ) -> Result<Self> {
        if priority > uapi::drm_asahi_priority_DRM_ASAHI_PRIORITY_REALTIME {
            return Err(EINVAL);
        }
        let range = hw::USER_RANGE;
        if usc_exec_base & (USC_WINDOW - 1) != 0
            || usc_exec_base < range.start
            || usc_exec_base
                .checked_add(USC_WINDOW)
                .is_none_or(|end| end > range.end)
        {
            return Err(EINVAL);
        }
        if gpu.shared.crashed.load(Ordering::Acquire) {
            return Err(EIO);
        }
        let vm_state = vm::lookup(gpu, &vm)?;
        if vm_state.status.get() != 0 {
            return Err(EIO);
        }
        let credit = QueueCredit::acquire(&usage)?;
        let timeline = Timeline::new()?;
        let scheduler = Scheduler::new(&gpu.shared.dev)?;
        let state = Arc::new(
            QueueState {
                priority,
                usage,
                usc_base: usc_exec_base,
                cancelled_at: [const { AtomicU64::new(0) }; 2],
                turn: AtomicU64::new(0),
                completed: [const { AtomicU64::new(0) }; 2],
                compute_ooo: AtomicU64::new(0),
                published: [const { AtomicU64::new(0) }; 2],
                owned: [const { AtomicU32::new(0) }; 32],
                owned_index: AtomicU32::new(0),
                compute_ewma_us: AtomicU32::new(0),
            },
            GFP_KERNEL,
        )?;
        let proof = command::SpanProof::new_boxed()?;
        Ok(Self {
            scheduler,
            state,
            accepted: [0; 2],
            proof,
            vm: vm_state,
            shared: gpu.shared.clone(),
            timeline,
            active_cores: gpu.id.core_masks.iter().map(|mask| mask.count_ones()).sum(),
            cfg: gpu.cfg,
            render_scs: [0; 2],
            _credit: credit,
        })
    }

    /// Validates a SUBMIT, publishes its output syncs, and pushes its job.
    fn submit_job(
        &mut self,
        id: u64,
        syncs: &mut Syncs,
        sync_count: usize,
        bytes: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        if sync_count > command::MAX_SYNCS {
            return Err(EINVAL);
        }
        let parsed = if self.cfg.gpu_generation < 16 {
            command::parse_stream_for(
                bytes,
                params::early_tiling(),
                self.cfg.num_clusters,
                self.cfg.gpu_generation,
            )?
        } else {
            command::parse_stream(bytes, params::early_tiling())?
        };
        if self.shared.crashed.load(Ordering::Acquire) {
            return Err(EIO);
        }
        if self.vm.status.get() != 0 {
            return Err(EIO);
        }
        let prepared = parsed.prepare(self.accepted, objects)?;
        // The job reference first, so that the mappings checked below stay retained.
        let guard = self.vm.vm.retain_first_job()?;
        let submission = prepared.validate(
            &command::Context {
                vm: &self.vm.vm,
                usc_base: self.state.usc_base,
                active_cores: self.active_cores,
                render_scs: self.render_scs,
                max_tile_buffer: self.cfg.max_tile_buffer,
            },
            &mut self.proof,
        )?;
        // Refused jobs add no accepted ordinals; check every prepared dependency before
        // publishing a fence or extending the queue's accepted frontier.
        for order in submission.orders.iter() {
            self.state.check(order)?;
        }
        let mut inputs = InputFences::new();
        for sync in syncs.inputs() {
            if let Some(fence) = &sync.fence {
                inputs.push(fence.clone())?;
            }
        }
        // Allocate and map private backing before a fence or accepted ordinal exists.
        // No GPU completion lock is held; reclaim can wait for earlier hardware work.
        let quarantined_compute_mask = self.shared.quarantined_compute_mask.load(Ordering::Acquire);
        let scs_compute_mask = self.shared.scs_compute_mask.load(Ordering::Acquire);
        let hints = super::resources::PrepareHints {
            expand_compute_ext: self.shared.base_compute_quarantined.load(Ordering::Acquire),
            quarantined_compute_mask,
            scs_compute_mask,
            expand_render_ext: self.shared.base_render_quarantined.load(Ordering::Acquire),
            quarantined_pb_mask: self.shared.quarantined_pb_mask.load(Ordering::Acquire),
            next_pb_slot: &self.shared.next_pb_slot,
            active_render_vms: &self.shared.active_render_vms,
        };
        let resources = self
            .vm
            .resources
            .as_ref()
            .ok_or(EIO)?
            .prepare(&self.vm, &submission.commands, hints)?;
        let gpu_id = Arc::as_ptr(&self.shared) as usize;
        // Record cumulative queue ordinals across both engines so coalescing multiple
        // same-queue fences in a timeline semaphore chain to the latest fence preserves
        // any earlier unretired render or compute command on that queue.
        let total_render = self.accepted[command::RENDER] + submission.counts[command::RENDER];
        let total_compute = self.accepted[command::COMPUTE] + submission.counts[command::COMPUTE];
        let last_ordinal = [
            (total_render != 0).then_some(total_render),
            (total_compute != 0).then_some(total_compute),
        ];
        let origin = Arc::new(
            super::fence::FenceOrigin::new(gpu_id, self.vm.id, self.state.clone(), last_ordinal),
            GFP_KERNEL,
        )?;
        let (inputs, fw_waits) = match super::fence::resolve_fw_waits(&inputs, gpu_id, self.vm.id) {
            Some(waits) => (InputFences::new(), waits),
            None => (inputs, super::fence::FwWaits::Empty),
        };
        let fence = self.timeline.next(origin.clone())?;
        let job = Job::new(
            id,
            self.state.clone(),
            self.vm.clone(),
            submission.commands,
            submission.orders,
            resources,
            fence,
            guard,
            inputs,
            origin,
            fw_waits,
        )?;
        let published = job.fence.fence();
        // Publish output syncs (including fallible cumulative scope allocation) before pushing
        // to the scheduler so ENOMEM refuses cleanly without leaving an accepted job behind.
        // If `scheduler.push` later fails, dropping `job` signals `published` with ECANCELED.
        syncs.publish(&published)?;
        self.scheduler.push(job, self.shared.clone())?;
        // Accepted: retain bounded per-engine accounting for later submissions.
        self.accepted[command::RENDER] += submission.counts[command::RENDER];
        self.accepted[command::COMPUTE] += submission.counts[command::COMPUTE];
        Ok(())
    }
}

impl crate::queue::Queue for Queue {
    /// RENDER_RAY_QUERY: the largest scratch layouts of the queue's vertex and fragment stages.
    /// A render's UMA pool serves both stages of one render at a time, so their extra pages add
    /// up ([`super::rt::required_seeds`]).
    fn reserve_render_scs(&mut self, layouts: [u64; 2]) -> Result {
        if !self.cfg.supports_render_scs {
            if layouts == [0; 2] {
                return Ok(());
            }
            return Err(Error::from_errno(-(kernel::bindings::EOPNOTSUPP as i32)));
        }
        if self.accepted != [0; 2] {
            return Err(EBUSY);
        }
        let pages = [
            super::rt::ScratchRequest::new(layouts[0])?.pages(),
            super::rt::ScratchRequest::new(layouts[1])?.pages(),
        ];
        super::rt::required_seeds(self.active_cores, &pages)?;
        self.render_scs = pages;
        Ok(())
    }

    fn submit(
        &mut self,
        _reg_data: &crate::driver::AsahiRegData<'_>,
        id: u64,
        syncs: KVVec<file::SyncItem>,
        in_sync_count: usize,
        cmdbuf_raw: &[u8],
        objects: Pin<&xarray::XArray<KBox<file::Object>>>,
    ) -> Result {
        let sync_count = syncs.len();
        let mut syncs = Syncs::new(syncs, in_sync_count);
        match self.submit_job(id, &mut syncs, sync_count, cmdbuf_raw, objects) {
            Ok(()) => Ok(()),
            Err(error) => {
                // A refused submission leaves the VM usable unless the device is lost.
                self.vm.status.record_if_device_loss(error);
                // No work has been accepted. A restarted ioctl must leave output
                // syncobjs untouched; do not expose an internal restart errno fence.
                if error != ERESTARTSYS {
                    syncs.refuse(&self.timeline, error);
                }
                Err(error)
            }
        }
    }
}
