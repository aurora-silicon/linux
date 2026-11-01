// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Serialized M4 execution over the shared Asahi frontend.
//!
//! A userspace submission retains all commands, mappings and inputs through its final fence.
//! One command at a time is encoded into one of two retained units, then published to the
//! classic engine queues. Reuse requires a complete wire receipt followed by replacement
//! by the other retired unit in the same UMA slot. TA and FS remain a paired publication
//! with the native explicit TA dependency. This total order satisfies Mesa's command barriers.
//! Firmware failure retains every possibly reachable owner. Completions are signalled only
//! after dropping the device lock.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use kernel::{prelude::*, sync::Arc};

use super::channel::Channel;
use super::command::{Command, ComputeCommand, RenderCommand};
use super::encode::{self, ComputeRefs, Geometry, RenderRefs};
use super::fw::channels::SubmitRecord;
use super::fw::graph::{notifier_page, qstate};
use super::fw::work::{self, BarrierType};
use super::hw::sizes;
use super::job::{Completions, Job};
use super::slots::Claims;
use super::{params, Firmware, Shared};

/// `wmb()`: `dsb st` (a full fence on other architectures).
#[inline(always)]
pub(crate) fn wmb() {
    #[cfg(target_arch = "aarch64")]
    // SAFETY: A barrier instruction with no other effects.
    unsafe {
        core::arch::asm!("dsb st", options(nostack, preserves_flags))
    };
    #[cfg(not(target_arch = "aarch64"))]
    core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
}

/// `rmb()`: `dsb ld` (a full fence on other architectures).
#[inline(always)]
pub(crate) fn rmb() {
    #[cfg(target_arch = "aarch64")]
    // SAFETY: A barrier instruction with no other effects.
    unsafe {
        core::arch::asm!("dsb ld", options(nostack, preserves_flags))
    };
    #[cfg(not(target_arch = "aarch64"))]
    core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
}

/// `ktime_get_ns()`.
pub(crate) fn now_ns() -> u64 {
    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get() as u64
}

/// Counter value carried by a native 25G83 submit-pipe record.
fn counter_ticks() -> u64 {
    #[cfg(target_arch = "aarch64")]
    {
        let value: u64;
        // SAFETY: CNTPCT_EL0 is the read-only architectural physical counter.
        unsafe { core::arch::asm!("mrs {value}, cntpct_el0", value = out(reg) value) };
        value
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        now_ns()
    }
}

/// Advance a command's first firmware-visible queue producer and count the successful
/// store, without allocating. Render begins at its TA WRITE; later FS/doorbell errors leave
/// this counter incremented and take the uncertain-publication fail-stop path. Preparation,
/// thresholds and private submitted counters alone do not count as command publication.
fn publish_producer(state: &super::object::FwObject, head: u32, publications: &mut u64) -> Result {
    state.w32(qstate::WRITE, head)?;
    *publications = publications.saturating_add(1);
    Ok(())
}

/// The engine's bookkeeping of one job, embedded in [`Job`]. Only the engine changes it, always
/// under the device lock (the atomics only make the shared `Arc<Job>` mutable).
pub(crate) struct Progress {
    /// Commands still to run: the job's command count, cut to `sent` when a failure drops its
    /// unpublished commands.
    pub(crate) count: AtomicU32,
    /// First command not yet published.
    pub(crate) sent: AtomicU32,
    /// First command not yet retired.
    pub(crate) next: AtomicU32,
    /// Waiting in `Engine::pending`.
    pub(crate) pending: AtomicBool,
    /// The fence was signalled: [`Job::complete`] runs once.
    pub(crate) signaled: AtomicBool,
}

impl Progress {
    pub(crate) fn new(commands: usize) -> Self {
        Self {
            count: AtomicU32::new(commands as u32),
            sent: AtomicU32::new(0),
            next: AtomicU32::new(0),
            pending: AtomicBool::new(false),
            signaled: AtomicBool::new(false),
        }
    }

    pub(crate) fn count(&self) -> u32 {
        self.count.load(Ordering::Relaxed)
    }

    pub(crate) fn sent(&self) -> u32 {
        self.sent.load(Ordering::Relaxed)
    }

    pub(crate) fn next(&self) -> u32 {
        self.next.load(Ordering::Relaxed)
    }

    pub(crate) fn is_pending(&self) -> bool {
        self.pending.load(Ordering::Relaxed)
    }

    fn set_sent(&self, v: u32) {
        self.sent.store(v, Ordering::Relaxed)
    }

    fn set_pending(&self, v: bool) {
        self.pending.store(v, Ordering::Relaxed)
    }
}

/// Engine of the previous member.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub(crate) enum Prev {
    #[default]
    None,
    Compute,
    Render,
}

/// A published prefix: its job and commands, its own receipts (final
/// targets, exclusive ring ends and the ring positions before it, channel producers after its
/// records) and the storage it holds until its own retirement.
pub(crate) struct Flight {
    /// The job whose commands the prefix holds, and horizon jobs folded into it (in order).
    pub(crate) job: Arc<Job>,
    pub(crate) group: KVec<Arc<Job>>,
    /// Index of the first command of the job in this prefix, and the number of commands.
    pub(crate) first: usize,
    pub(crate) count: usize,
    pub(crate) computes: u32,
    pub(crate) renders: u32,
    /// Compute: final FW stamp, ring end (exclusive) and ring position before the prefix.
    pub(crate) target: u32,
    pub(crate) head: u32,
    pub(crate) c_start: u32,
    /// Render: final TA/FS stamps, ring ends and ring positions before the prefix.
    pub(crate) ta_target: u32,
    pub(crate) fs_target: u32,
    pub(crate) ta_head: u32,
    pub(crate) fs_head: u32,
    pub(crate) ta_start: u32,
    pub(crate) fs_start: u32,
    /// Channel producers (TA, FS, compute) after the prefix's records.
    pub(crate) rec: [u32; 3],
    /// Compute member j's FW stamp target, and the FS target it waits on directly (0: none):
    /// its predecessors in the prefix (for the compute-hang check).
    pub(crate) cstamp: [u32; sizes::COMPUTE_SLOTS],
    pub(crate) cwait: [u32; sizes::COMPUTE_SLOTS],
    /// Compute slots, pass slots and clustering buffers it holds.
    pub(crate) claims: Claims,
    /// A kill or recovery ended this prefix's VM: later prefixes of it fail with EIO.
    pub(crate) doomed: bool,
    /// A firmware recovery saw this command unfinished. Full ordinary receipts can still
    /// prove success; otherwise an aborted receipt is accepted only after the grace period.
    pub(crate) uma_submitted: u64,
    pub(crate) recovery_suspect: bool,
    pub(crate) recovery_started_ns: u64,
    /// Publication time, ns.
    pub(crate) published_ns: u64,
}

impl Flight {
    fn new(job: Arc<Job>) -> Self {
        Self {
            job,
            group: KVec::new(),
            first: 0,
            count: 0,
            computes: 0,
            renders: 0,
            target: 0,
            head: 0,
            c_start: 0,
            ta_target: 0,
            fs_target: 0,
            ta_head: 0,
            fs_head: 0,
            ta_start: 0,
            fs_start: 0,
            rec: [0; 3],
            cstamp: [0; sizes::COMPUTE_SLOTS],
            cwait: [0; sizes::COMPUTE_SLOTS],
            claims: Claims::new(),
            doomed: false,
            uma_submitted: 0,
            recovery_suspect: false,
            recovery_started_ns: 0,
            published_ns: 0,
        }
    }
}

/// A prefix under preparation: members in API order, each in a claimed
/// compute or pass slot. `prev` is the engine of the previous member, of this prefix or else
/// the last member of the newest published one; `last_ct`/`last_fs` its compute/FS target.
#[derive(Default)]
struct Prefix {
    computes: u32,
    renders: u32,
    prev: Prev,
    /// This prefix (re)initializes the PB with InitBM.
    initbm: bool,
    c_head: u32,
    ta_head: u32,
    fs_head: u32,
    /// Last compute ordinal / last draw ordinal.
    n: u64,
    draw: u64,
    last_ct: u32,
    last_fs: u32,
}

/// Error provenance must remain independent of errno: EIO can be an ordinary failed VM
/// status before publication, or a transport error after firmware may see new producers.
#[derive(Copy, Clone, Debug)]
enum PublicationError {
    Unpublished(Error),
    Uncertain(Error),
}

/// Engine statistics, printed by `asahi-g16/stats`.
#[derive(Default)]
pub(crate) struct Stats {
    pub(crate) pub_ns: u64,
    pub(crate) idle_from_ns: u64,
    pub(crate) starve_from_ns: u64,
    pub(crate) prep_from_ns: u64,
    /// Idle with nothing ready; prefix preparation.
    pub(crate) starved_ns: u64,
    pub(crate) prep_ns: u64,
    /// Internal stage durations of retired prefixes (24 MHz ticks).
    pub(crate) ta_ticks: u64,
    pub(crate) fs_ticks: u64,
    pub(crate) cdm_ticks: u64,
    pub(crate) span_ticks: u64,
    /// [0] compute, [1] render, [2] mixed.
    pub(crate) count: [u64; 3],
    /// Commands whose first queue WRITE advanced, compute then render. These counters are
    /// monotonic even if a later producer or doorbell fails; they are independent of receipts.
    pub(crate) publications: [u64; 2],
    pub(crate) prefixes: [u64; 3],
    pub(crate) busy_ns: [u64; 3],
    pub(crate) max_ns: [u64; 3],
    pub(crate) first_ns: [u64; 3],
    pub(crate) last_ns: [u64; 3],
    pub(crate) gap_ns: u64,
    pub(crate) gaps: u64,
    pub(crate) yields: u64,
    /// Prefixes published while another ran.
    pub(crate) overlaps: u64,
    pub(crate) jobs: u64,
    pub(crate) job_cmds: u64,
    pub(crate) job_max: u64,
    pub(crate) early: u64,
    /// Type-0 FS->compute / compute->TA waits.
    pub(crate) fs2c0: u64,
    pub(crate) c2ta0: u64,
    pub(crate) clustered: u64,
    /// Jobs published behind another in a compute horizon.
    pub(crate) grouped: u64,
    /// Compute-only prefixes by jobs; why grouping stopped (no further ready job, the cap,
    /// compute slots, a job of the queue that does not qualify); behind a running prefix; of
    /// those behind its unconsumed record.
    pub(crate) hz: [u64; sizes::HORIZON_JOBS as usize],
    pub(crate) hz_stop: [u64; 4],
    pub(crate) hz_behind: u64,
    pub(crate) hz_ahead: u64,
    /// Why ready jobs stayed unpublished: both flights published, a channel record unconsumed,
    /// no free slot.
    pub(crate) hold: [u64; 3],
}

/// Host-side phase timers of publication and polling, on with `debug_flags` bit 49
/// (`Debug1`); printed as the `prof:` line of `asahi-g16/stats`. A measurement aid: nothing is
/// timed while the flag is off.
#[derive(Default)]
pub(crate) struct Prof {
    /// Published prefixes timed.
    pub(crate) prefixes: u64,
    /// Member selection, channel checks, allocator and ring-head setup.
    pub(crate) select_ns: u64,
    /// Encoders (images into the scratch buffers).
    pub(crate) encode_ns: u64,
    /// Slot, pass and clustering-buffer claims, with synchronous scrubs and flush waits.
    pub(crate) claim_ns: u64,
    /// Object writes: images, scene, barriers, InitBM, ring entries.
    pub(crate) write_ns: u64,
    /// Thresholds, queue WRITEs, channel records and doorbells.
    pub(crate) post_ns: u64,
    /// Poll passes, and the time of their drain, retirement and whole pass.
    pub(crate) polls: u64,
    pub(crate) drain_ns: u64,
    pub(crate) retire_ns: u64,
    pub(crate) poll_ns: u64,
}

/// A phase timer: reads the clock only while profiling is on.
pub(crate) struct Lap(Option<u64>);

impl Lap {
    pub(crate) fn start() -> Self {
        Lap(crate::debug::debug_enabled(crate::debug::DebugFlags::Debug1).then(now_ns))
    }

    /// Adds the time since the start or the last lap to `acc`.
    pub(crate) fn lap(&mut self, acc: &mut u64) {
        if let Some(t) = self.0 {
            let now = now_ns();
            *acc += now.wrapping_sub(t);
            self.0 = Some(now);
        }
    }

    pub(crate) fn on(&self) -> bool {
        self.0.is_some()
    }
}

/// Encoder output buffers, allocated once rather than per publication.
struct Scratch {
    cw: KVec<u8>,
    cq: KVec<u8>,
    ta: KVec<u8>,
    tas: KVec<u8>,
    fs: KVec<u8>,
    fss: KVec<u8>,
    scene: KVec<u8>,
}

impl Scratch {
    fn new() -> Result<Self> {
        let z = |n| KVec::from_elem(0u8, n, GFP_KERNEL);
        Ok(Self {
            cw: z(work::compute::WORK_SIZE)?,
            cq: z(work::compute::SEQ_SIZE)?,
            ta: z(work::ta::WORK_SIZE)?,
            tas: z(work::ta::SEQ_SIZE)?,
            fs: z(work::fs::WORK_SIZE)?,
            fss: z(work::fs::SEQ_SIZE)?,
            scene: z(work::scene::SIZE)?,
        })
    }
}

/// `&mut KVec<u8>` as a fixed-size image buffer.
fn image<const N: usize>(v: &mut KVec<u8>) -> Result<&mut [u8; N]> {
    <&mut [u8; N]>::try_from(&mut v[..]).map_err(|_| EINVAL)
}

/// Engine state.
pub(crate) struct Engine {
    /// Runnable jobs of every queue, in arrival order.
    pub(crate) pending: KVec<Arc<Job>>,
    /// Published prefixes, oldest first.
    pub(crate) flights: KVec<Flight>,
    /// Jobs whose storage a failed prefix may still reference, retained until reboot.
    pub(crate) retained: KVec<Arc<Job>>,
    /// Publication frozen by a kill request (recovery may use this or `Recovery::armed`; both
    /// freeze publication).
    pub(crate) frozen: bool,
    /// Service turn counter for the least-recently-served pick.
    pub(crate) turn: u64,
    /// The device's shared state, for the transport-error latch; set by the first poll or
    /// submission.
    pub(crate) shared: Option<Arc<Shared>>,
    /// Host compute ordinal and draw ordinal of the last publication.
    pub(crate) ordinal: u64,
    pub(crate) draw: u64,
    /// Newest published receipts, the tail.
    pub(crate) target: u32,
    pub(crate) head: u32,
    pub(crate) ta_target: u32,
    pub(crate) fs_target: u32,
    pub(crate) ta_head: u32,
    pub(crate) fs_head: u32,
    /// Event counts at the tail's publication.
    pub(crate) events: u32,
    pub(crate) stage_seen: u32,
    /// The compute queue / render queues were registered by a first record (`new_queue`).
    pub(crate) queue_registered: bool,
    pub(crate) render_registered: bool,
    pub(crate) ta_registered: bool,
    /// A ready prefix waits only for a record's consumption: repoll sooner.
    pub(crate) chan_wait: bool,
    /// Progress tracking of the oldest prefix.
    pub(crate) progress_flight: u64,
    pub(crate) progress: [Option<u32>; 3],
    pub(crate) stats: Stats,
    /// Phase timers (debug_flags bit 49).
    pub(crate) prof: Prof,
    /// The `asahi-g16/stats` file.
    pub(crate) debugfs: Option<Pin<KBox<kernel::debugfs::File<Arc<Shared>>>>>,
    scratch: Option<KBox<Scratch>>,
}

impl Engine {
    pub(crate) fn new() -> Result<Self> {
        // Wrapped native stamps are safe only while pending references fit within the
        // signed half-window. This backend publishes exactly one command at a time,
        // retires its full wire/owner receipt, then builds the next command/barrier.
        // Do not add submission-ahead without revisiting the dependency lifetime proof.
        if sizes::FLIGHTS != 1 {
            return Err(EINVAL);
        }
        Ok(Self {
            pending: KVec::new(),
            flights: KVec::with_capacity(sizes::FLIGHTS, GFP_KERNEL)?,
            retained: KVec::new(),
            frozen: false,
            turn: 0,
            shared: None,
            ordinal: 0,
            draw: 0,
            target: 0,
            head: 0,
            ta_target: 0,
            fs_target: 0,
            ta_head: 0,
            fs_head: 0,
            events: 0,
            stage_seen: 0,
            queue_registered: false,
            render_registered: false,
            ta_registered: false,
            chan_wait: false,
            progress_flight: 0,
            progress: [None; 3],
            stats: Stats::default(),
            prof: Prof::default(),
            debugfs: None,
            // Allocate host encoder buffers before any submitted fence can depend on
            // the device mutex. Publication must never enter direct reclaim here.
            scratch: Some(KBox::new(Scratch::new()?, GFP_KERNEL)?),
        })
    }

    /// The oldest published prefix.
    pub(crate) fn oldest(&self) -> Option<&Flight> {
        self.flights.first()
    }

    /// Jobs of the queue in published prefixes.
    fn queue_published(&self, job: &Job) -> u32 {
        self.flights
            .iter()
            .filter(|f| Arc::ptr_eq(&f.job.queue, &job.queue))
            .map(|f| 1 + f.group.len() as u32)
            .sum()
    }

    /// Index in `pending` of the next ready job: highest queue priority, then the queue served
    /// least recently, then submission order. `QueueState::priority` is
    /// `DRM_ASAHI_PRIORITY_REALTIME - priority` as `file.rs` passes it, so a smaller value
    /// outranks.
    fn pick(&self) -> Option<usize> {
        let mut best: Option<usize> = None;
        for (i, job) in self.pending.iter().enumerate() {
            // Exact per-command predecessors may retire before their entire job. Let the
            // predecessor job advance rather than idling behind a waiting younger job.
            if matches!(job.check_command_dependencies(job.progress.sent()), Err(e) if e == EAGAIN)
            {
                continue;
            }
            let better = match best {
                None => true,
                Some(b) => {
                    let (q, bq) = (&job.queue, &self.pending[b].queue);
                    let (t, bt) = (
                        q.turn.load(Ordering::Relaxed),
                        bq.turn.load(Ordering::Relaxed),
                    );
                    q.priority < bq.priority || (q.priority == bq.priority && t < bt)
                }
            };
            if better {
                best = Some(i);
            }
        }
        best
    }

    /// Removes `job` from `pending`.
    fn unpend(&mut self, job: &Arc<Job>) {
        job.progress.set_pending(false);
        self.pending.retain(|j| !Arc::ptr_eq(j, job));
    }
}

/// A command from the retained userspace snapshot.
fn command(job: &Job, k: u32) -> Option<&Command> {
    job.commands.get(k as usize)
}

/// Pushes a job's completion; [`Job::complete`] only acts the first time.
pub(crate) fn complete(done: &mut Completions, job: &Arc<Job>, result: Result) {
    if let Err(error) = result {
        job.latch_failure(error);
    }
    done.push(job, result);
}

impl Firmware {
    /// Registers the one shared scheduling context using the proven queue-0 zero-stamp
    /// barrier before any shader work or user ASID is published (exec-runtime.h contract).
    pub(crate) fn register_context(&mut self) -> Result {
        let r = &self.graph.render;
        let barrier = &r.registration_barrier;
        let zero = r.legacy_empty.va;
        let mut bytes = [0u8; work::barrier::SIZE];
        bytes[0..4].copy_from_slice(&4u32.to_le_bytes());
        bytes[4..12].copy_from_slice(&zero.to_le_bytes());
        bytes[12..20].copy_from_slice(&zero.to_le_bytes());
        bytes[0x20..0x24].copy_from_slice(&0x80u32.to_le_bytes());
        barrier.write(0, &bytes)?;
        if r.qstate[0].r32(qstate::READ)? != 0
            || r.qstate[0].r32(qstate::WRITE)? != 0
            || r.sched_ctx.r32(0)? & 0xffffff != 0x00ffff
            || self.channels.read(Channel::Ta)? != 0
            || self.channels.producer(Channel::Ta) != 0
        {
            return Err(super::boot::eproto());
        }
        r.ring[0].w64(0, barrier.va)?;
        wmb();
        r.qstate[0].w32(qstate::WRITE, 1)?;
        wmb();
        self.channels.post(
            Channel::Ta,
            &SubmitRecord {
                unk_0: counter_ticks(),
                qinfo: r.qinfo[0].va,
                kind: 0,
                head: 1,
                event: 0,
                new_queue: 1,
            },
        )?;
        self.session.ring(Channel::Ta)?;
        let deadline = now_ns().saturating_add(2_000_000_000);
        let mut stable = None;
        loop {
            self.drain_rings()?;
            let now = now_ns();
            if now >= deadline {
                return Err(ETIMEDOUT);
            }
            let context = self.graph.render.sched_ctx.r32(0)?;
            let registered = context & 0xff != 0xff
                && (context >> 8) & 0xff != 0xff
                && (context >> 16) & 0xff == 4;
            if registered
                && self.queue_receipt(
                    Channel::Ta,
                    &self.graph.render.qstate[0],
                    &self.graph.render.qinfo[0],
                    1,
                    1,
                )?
            {
                let first = *stable.get_or_insert(now);
                if now - first >= 50_000_000 {
                    break;
                }
            } else {
                stable = None;
            }
            // SAFETY: probe/backend start holds only the sleepable device mutex.
            unsafe {
                kernel::bindings::usleep_range_state(
                    1000,
                    2000,
                    kernel::bindings::TASK_UNINTERRUPTIBLE,
                )
            };
        }
        self.engine.ta_head = 1;
        self.engine.ta_registered = true;
        Ok(())
    }

    /// The single in-order Finalize frontier for each held M4 event. These cells are never
    /// reset after first publication, including across client VM switches and recovery.
    pub(crate) fn recovery_snapshot(&self) -> Result<[u32; 3]> {
        Ok([
            self.graph.render.fw_stamp[0].r32(0)?,
            self.graph.render.fw_stamp[1].r32(0)?,
            self.graph.compute.fw_stamp.r32(0)?,
        ])
    }

    /// Authenticate one firmware affected-work snapshot entry against its retained owner.
    /// The pre-acknowledgement TPC cleanup is required only for an unfinished TA.
    pub(crate) fn recovery_prepare(
        &mut self,
        flags: u32,
        event: u32,
        target: u32,
        stage: [u32; 3],
    ) -> Result {
        if flags & 1 != 0
            && super::recovery::logic::last_retired_target(
                &self.recovery.last_retired,
                event,
                target,
            )
        {
            // A notification may follow the full receipt. It names the retained old
            // owner, not the next command that happens to occupy the flight list.
            return Ok(());
        }
        let f = self.engine.flights.first_mut().ok_or(ENOENT)?;
        let matches = match event {
            15 => f.computes != 0 && f.target == target,
            0x70 => f.renders != 0 && f.ta_target == target,
            0x71 => f.renders != 0 && f.fs_target == target,
            _ => false,
        };
        if !matches || flags & 1 == 0 {
            return Err(ENOENT);
        }
        if flags & 9 == 9
            && f.renders != 0
            && !super::poll::stamp_reached(stage[0], f.ta_target, self.engine.ta_target)
        {
            let k = f.claims.pass[0] as usize;
            let pass = f.job.resources.render()?.passes.get(k).ok_or(EINVAL)?;
            let bytes = pass.ta_work.r64(work::ta::TPC_BYTES)?;
            if bytes == 0 || bytes > pass.tpc.size as u64 {
                return Err(ERANGE);
            }
            pass.tpc.fill(0, 0, bytes as usize)?;
            wmb();
        }
        f.doomed = true;
        f.recovery_suspect = true;
        f.recovery_started_ns = now_ns();
        Ok(())
    }

    /// A command unfinished when recovery started may have been aborted without appearing
    /// in the snapshot. Keep it until either its full receipt or its passed-abort proof.
    pub(crate) fn recovery_prepare_suspects(
        &mut self,
        stage: [u32; 3],
        _event: u32,
        fault_asid: Option<u32>,
    ) -> Result {
        let Some(f) = self.engine.flights.first_mut() else {
            return Ok(());
        };
        let reached = super::recovery::logic::stamp_reached;
        let unfinished = (f.computes != 0 && !reached(stage[2], f.target, self.engine.target))
            || (f.renders != 0
                && (!reached(stage[0], f.ta_target, self.engine.ta_target)
                    || !reached(stage[1], f.fs_target, self.engine.fs_target)));
        if unfinished {
            f.recovery_suspect = true;
            f.recovery_started_ns = now_ns();
            if fault_asid.is_some_and(|asid| asid >= 1 && asid == f.job.vm.slot) {
                f.doomed = true;
            }
        }
        Ok(())
    }

    /// Accepts a job from the scheduler; it is published by [`Self::publish`]. Records how many
    /// jobs the queue owns at this arrival for diagnostics.
    pub(crate) fn enqueue(&mut self, job: Arc<Job>) -> Result {
        if self.recovery.failed().is_some() {
            return Err(EIO);
        }
        if job.vm.status.get() != 0 {
            return Err(EIO);
        }
        if job.progress.count() == 0 || job.commands.is_empty() {
            return Err(EINVAL);
        }
        if self.engine.flights.capacity() < sizes::FLIGHTS {
            // Capacity is reserved at startup. Refuse an invariant failure before any
            // publication rather than allocating beneath the signalling mutex.
            return Err(EIO);
        }
        self.engine.pending.push(job.clone(), GFP_NOWAIT)?;
        job.progress.set_pending(true);
        let q = &job.queue;
        let owned = self.engine.queue_published(&job)
            + self
                .engine
                .pending
                .iter()
                .filter(|j| Arc::ptr_eq(&j.queue, q))
                .count() as u32;
        let i = q.owned_index.fetch_add(1, Ordering::Relaxed) as usize % q.owned.len();
        q.owned[i].store(owned.min(255), Ordering::Relaxed);
        Ok(())
    }

    /// Publishes one command after the preceding command's complete wire receipt. Ordinary
    /// preparation failures fail the affected job, including after its earlier commands
    /// retired. A publication marked uncertain denotes potentially visible firmware state
    /// and latches the device failed. Dependency errors and exhausted resource classes are handled
    /// separately before preparation, without failing an otherwise healthy engine class.
    pub(crate) fn publish(&mut self, done: &mut Completions) -> Result {
        self.session.check()?;
        self.objects.health()?;
        self.service_power()?;
        self.drain_rings()?;
        self.check_hangs(done)?;
        // The initial G16 backend deliberately serializes hardware publication. Stronger
        // ordering satisfies Mesa's barriers while slot displacement and cache ownership
        // stay explicit. Never enable G15's submit-ahead policy on the M4 wire by accident.
        let mut result = Ok(());

        self.engine.chan_wait = false;
        while self.recovery.failed().is_none()
            && !self.engine.frozen
            && !self.engine.pending.is_empty()
        {
            let Some(index) = self.engine.pick() else {
                break;
            };
            let job = self.engine.pending[index].clone();
            // Even a failed job may have an earlier command in flight. An aggregate
            // error fence must not race that command's access to user buffers.
            if !self.engine.flights.is_empty() {
                self.engine.stats.hold[0] += 1;
                break;
            }
            if job.vm.status.get() != 0 {
                self.engine.unpend(&job);
                let _ = job.mark_terminal_retired();
                complete(done, &job, Err(EIO));
                continue;
            }
            // No earlier command of this or another job remains reachable in flight. A
            // quarantined unit cannot be reused, but must not poison the other engine class.
            let class_error = match command(&job, job.progress.sent()) {
                Some(Command::Compute(_)) => {
                    if self.slots.compute_exhausted() {
                        Some((super::command::COMPUTE, EIO))
                    } else if super::recovery::logic::next_ordinal(self.engine.ordinal).is_none() {
                        Some((super::command::COMPUTE, EOVERFLOW))
                    } else {
                        None
                    }
                }
                Some(Command::Render(_)) => {
                    if self.slots.render_exhausted() {
                        Some((super::command::RENDER, EIO))
                    } else if super::recovery::logic::next_ordinal(self.engine.draw).is_none() {
                        Some((super::command::RENDER, EOVERFLOW))
                    } else {
                        None
                    }
                }
                None => None,
            };
            if let Some((class, error)) = class_error {
                self.mark_class_unavailable(class, error);
                self.engine.unpend(&job);
                let _ = job.mark_terminal_retired();
                // No command is live. Cancel this queue, retaining the VM and the other
                // class; pretending this was its VM fault would disable both classes.
                complete(done, &job, Err(ECANCELED));
                continue;
            }
            match job.check_command_dependencies(job.progress.sent()) {
                Ok(()) => {}
                Err(e) if e == EAGAIN => break,
                Err(e) => {
                    self.engine.unpend(&job);
                    let _ = job.mark_terminal_retired();
                    complete(done, &job, Err(e));
                    continue;
                }
            }
            self.engine.turn += 1;
            job.queue.turn.store(self.engine.turn, Ordering::Relaxed);
            match self.publish_prefix(&job, done) {
                Ok(()) => {
                    self.recovery.publication_wait_deadline = None;
                    if job.progress.sent() == job.progress.count() {
                        self.engine.unpend(&job); // fully published
                    }
                    continue;
                }
                Err(PublicationError::Unpublished(e)) if e == EAGAIN => {
                    let i = if self.engine.chan_wait { 1 } else { 2 };
                    self.engine.stats.hold[i] += 1;
                    // No flight can wake an idle deferred publication. Re-poll and use
                    // the existing administrative deadline, never an unbounded stall.
                    self.engine.chan_wait = true;
                    let deadline =
                        *self
                            .recovery
                            .publication_wait_deadline
                            .get_or_insert_with(|| {
                                super::recovery::logic::renew(
                                    now_ns(),
                                    params::deadline_ms(),
                                    params::compute_deadline_ms(),
                                )
                                .0
                            });
                    if super::recovery::logic::expired(now_ns(), deadline) {
                        return Err(ETIMEDOUT);
                    }
                    break;
                }
                Err(PublicationError::Unpublished(e)) => {
                    self.engine.unpend(&job);
                    let _ = job.mark_terminal_retired();
                    complete(done, &job, Err(e));
                }
                Err(PublicationError::Uncertain(e)) => {
                    self.engine.unpend(&job);
                    // A counter or producer may be visible: retain and stop globally.
                    dev_err!(
                        self.graph.dev.as_ref(),
                        "G16: publication of ctx {} failed after visibility began ({:?})\n",
                        job.vm.slot,
                        e
                    );
                    complete(done, &job, Err(EIO));
                    self.retain(&job);
                    self.fail_stop(EIO, done);
                    result = Err(EIO);
                    break;
                }
            }
        }
        if !self.engine.chan_wait {
            self.recovery.publication_wait_deadline = None;
        }
        // Polling retires before it publishes, so this sees work start and
        // drain; a fail-stop reports from fail_flights().
        self.power.report_work(!self.engine.flights.is_empty());
        result
    }

    /// Whether some of `job`'s commands were published: such a job is retained after a
    /// fail-stop.
    pub(crate) fn job_partly_published(&self, job: &Job) -> bool {
        job.progress.sent() != 0
    }

    /// Addresses and cumulative allocator state of the selected retained render unit.
    fn render_refs(&mut self, k: usize, job: &Job, draw: u64) -> Result<(RenderRefs, bool)> {
        let generation = job.vm.generation;
        let arena = job.resources.render()?;
        let mut a = arena.pools.prepare(k, draw)?;
        let epoch = job.resources.pb(job.progress.sent() as usize)?;
        epoch.prepare(k, arena.pools.heap_gva(k)?)?;
        a.pb_mgr = epoch.manager.va;
        a.mgr_misc = epoch.misc.va;
        let r = &self.graph.render;
        let stage = draw.checked_mul(2).ok_or(EOVERFLOW)?;
        Ok((
            RenderRefs {
                notifier: r.notifier.va,
                legacy_empty: r.legacy_empty.va,
                qinfo: [r.qinfo[0].va, r.qinfo[1].va],
                user_stamp: [
                    r.notifier.va + notifier_page::TA_STAMP as u64,
                    r.notifier.va + notifier_page::FS_STAMP as u64,
                ],
                fw_stamp: [r.fw_stamp[0].va, r.fw_stamp[1].va],
                uma_mgr: a.uma_mgr,
                uma_aux: a.uma_aux,
                uma_gpu: a.uma_gpu,
                pb_mgr: a.pb_mgr,
                scene_list: a.scene_list,
                mgr_misc: a.mgr_misc,
                pb_init_blocks: a.init_blocks,
                parameter_id: a.parameter_id,
                generation,
                activity: [self.init.ta_activity.va + 4, self.init.fs_activity.va + 8],
                counter: [stage, stage - 1],
                uma_submitted: a.uma_submitted,
                uuid: (draw as u32).wrapping_add(1),
                usc_base: job.queue.usc_base,
            },
            a.init_buffer,
        ))
    }

    /// Publishes the next command of `job`. All images and ring entries precede the host
    /// producer writes and queue kicks. EAGAIN defers while storage/channel state is held;
    /// The error records whether firmware may see a counter/producer update, independently
    /// of errno; unpublished EIO is still an ordinary job failure.
    fn publish_prefix(
        &mut self,
        job: &Arc<Job>,
        done: &mut Completions,
    ) -> core::result::Result<(), PublicationError> {
        let mut f = Flight::new(job.clone());
        let mut visible = false;
        let result = self.publish_prefix_into(job, &mut f, &mut visible);
        match result {
            Err(e) if !visible => {
                // Never published: its storage is free again, and horizon jobs taken into it
                // fail with it.
                self.slots.release(&mut f.claims);
                for j2 in f.group.iter() {
                    complete(done, j2, Err(e));
                }
                Err(PublicationError::Unpublished(e))
            }
            Err(e) => Err(PublicationError::Uncertain(e)),
            Ok(()) => Ok(()),
        }
    }

    fn publish_prefix_into(
        &mut self,
        job: &Arc<Job>,
        f: &mut Flight,
        visible: &mut bool,
    ) -> Result {
        if !self.engine.flights.is_empty() {
            return Err(EAGAIN);
        }
        let index = job.progress.sent();
        let cmd = command(job, index).ok_or(EINVAL)?;
        let render = matches!(cmd, Command::Render(_));
        if job.vm.status.get() != 0 {
            return Err(EIO);
        }
        if if render {
            self.slots.render_exhausted()
        } else {
            self.slots.compute_exhausted()
        } {
            return Err(EIO);
        }
        let channels: &[Channel] = if render {
            &[Channel::Fs, Channel::Ta]
        } else {
            &[Channel::Compute]
        };
        for &channel in channels {
            let pipe = self.channels.channel(channel);
            let producer = self.channels.producer(channel);
            if pipe.read()? != producer
                || pipe.completion()? != producer
                || pipe.write()? != producer
            {
                self.engine.chan_wait = true;
                return Err(EAGAIN);
            }
        }
        if self.engine.scratch.is_none() {
            return Err(EIO);
        }
        self.engine.stats.prep_from_ns = now_ns();
        let mut prefix = Prefix {
            n: self.engine.ordinal,
            draw: self.engine.draw,
            c_head: self.graph.compute.qstate.r32(qstate::WRITE)?,
            ta_head: self.graph.render.qstate[0].r32(qstate::WRITE)?,
            fs_head: self.graph.render.qstate[1].r32(qstate::WRITE)?,
            ..Default::default()
        };
        if prefix.c_head >= 256 || prefix.ta_head >= 256 || prefix.fs_head >= 256 {
            return Err(super::boot::eproto());
        }
        f.c_start = prefix.c_head;
        f.ta_start = prefix.ta_head;
        f.fs_start = prefix.fs_head;
        match cmd {
            Command::Compute(c) => self.prefix_compute(job, c, &mut prefix, f, false)?,
            Command::Render(r) => {
                let geometry = Geometry::new(&r.cmd, 0);
                if job.resources.render()?.passes.iter().all(|slot| {
                    geometry
                        .validate_capacity(slot.tpc.size as u64, slot.tilemap.size as u64)
                        .is_err()
                }) {
                    return Err(E2BIG);
                }
                let k = self
                    .slots
                    .pick_pass(job.resources.render()?, &geometry, 0)
                    .ok_or(EAGAIN)?;
                f.claims.pass[0] = k as u32;
                f.claims.pass_mask = 1 << k;
                self.slots.pass_busy |= 1 << k;
                self.prefix_render(job, r, &mut prefix, f)?;
            }
        }
        f.first = index as usize;
        f.count = 1;
        f.computes = prefix.computes;
        f.renders = prefix.renders;
        f.target = prefix.last_ct;
        f.ta_target = if render {
            encode::stamp(prefix.draw)
        } else {
            0
        };
        f.fs_target = prefix.last_fs;
        f.head = prefix.c_head;
        f.ta_head = prefix.ta_head;
        f.fs_head = prefix.fs_head;
        if self.recovery_pending()? {
            return Err(EAGAIN);
        }
        if render {
            job.resources
                .pb(index as usize)?
                .check_assignment(f.claims.pass[0] as usize)?;
        }
        // Reserve lifetime ownership before any counter or producer becomes reachable.
        job.vm.vm.mark_binding_published()?;
        self.slots.hold_vm(&f.claims, &job.vm)?;
        self.slots.publish(&f.claims)?;
        // Even a failed first store may have become visible. From this boundary preserve
        // every owner and return explicit uncertainty rather than guessing from errno.
        *visible = true;
        let publication = (|| -> Result {
            if render {
                let k = f.claims.pass[0] as usize;
                job.resources.pb(index as usize)?.expose();
                job.resources.render()?.pools.publish(k, f.uma_submitted)?;
                // Notifier service dereferences u32 counters; the host ordering ledger
                // remains u64. Eight-byte spacing is retained allocation padding.
                self.graph.render.threshold.w32(0, prefix.draw as u32)?;
                self.graph.render.threshold.w32(8, prefix.draw as u32)?;
            } else {
                let k = f.claims.slot[0] as usize;
                job.resources.compute()?.compute[k]
                    .counter
                    .w64(0, f.uma_submitted)?;
                self.graph.compute.threshold.w32(0, prefix.n as u32)?;
            }
            wmb();
            if render {
                publish_producer(
                    &self.graph.render.qstate[0],
                    prefix.ta_head,
                    &mut self.engine.stats.publications[1],
                )?;
                self.graph.render.qstate[1].w32(qstate::WRITE, prefix.fs_head)?;
            } else {
                publish_producer(
                    &self.graph.compute.qstate,
                    prefix.c_head,
                    &mut self.engine.stats.publications[0],
                )?;
            }
            wmb();
            for &channel in channels {
                let (qinfo, head, event, fresh) = match channel {
                    Channel::Compute => (
                        self.graph.compute.qinfo.va,
                        prefix.c_head,
                        15,
                        !self.engine.queue_registered,
                    ),
                    Channel::Ta => (
                        self.graph.render.qinfo[0].va,
                        prefix.ta_head,
                        0x70,
                        !self.engine.ta_registered,
                    ),
                    Channel::Fs => (
                        self.graph.render.qinfo[1].va,
                        prefix.fs_head,
                        0x71,
                        !self.engine.render_registered,
                    ),
                };
                let record = SubmitRecord {
                    unk_0: counter_ticks(),
                    qinfo,
                    kind: channel as u32,
                    head: head as u16,
                    event,
                    new_queue: fresh as u8,
                };
                self.channels.post(channel, &record)?;
                self.session.ring(channel)?;
            }
            Ok(())
        })();
        if publication.is_ok() {
            self.slots.note_published(&f.claims);
        }
        // From the first counter/WRITE onward an error is uncertain. Keep the complete
        // retained flight even when a later channel store or doorbell fails.
        f.rec = [
            self.channels.producer(Channel::Ta),
            self.channels.producer(Channel::Fs),
            self.channels.producer(Channel::Compute),
        ];
        self.engine.ordinal = prefix.n;
        self.engine.draw = prefix.draw;
        if render {
            self.engine.ta_target = f.ta_target;
            self.engine.fs_target = f.fs_target;
            self.engine.ta_head = f.ta_head;
            self.engine.fs_head = f.fs_head;
            self.engine.ta_registered = true;
            self.engine.render_registered = true;
        } else {
            self.engine.target = f.target;
            self.engine.head = f.head;
            self.engine.queue_registered = true;
        }
        self.engine.events = self.events.compute_events;
        self.engine.stage_seen = self.events.stage_events;
        self.stat_publish(f);
        self.renew_deadlines();
        job.progress.set_sent(index + 1);
        let mut flight = Flight::new(job.clone());
        core::mem::swap(&mut flight, f);
        self.engine
            .flights
            .push_within_capacity(flight)
            .map_err(|flight| {
                // Publication may be visible. Keep every owner alive even if the
                // startup capacity invariant was violated; the caller fail-stops.
                core::mem::forget(flight);
                EIO
            })?;
        publication.map_err(|_| EIO)
    }

    /// Encodes one command into its retained compute unit and appends it to the CDM ring.
    fn prefix_compute(
        &mut self,
        job: &Arc<Job>,
        cmd: &ComputeCommand,
        pf: &mut Prefix,
        f: &mut Flight,
        _later_render: bool,
    ) -> Result {
        let n = super::recovery::logic::next_ordinal(pf.n).ok_or(EOVERFLOW)?;
        let target = encode::stamp(n);
        let index = self
            .slots
            .claim_compute(job.resources.compute()?, &mut f.claims, 0)?
            .ok_or(EAGAIN)?;
        let generation = job.vm.generation;
        self.slots.hold_vm(&f.claims, &job.vm)?;
        let s = &job.resources.compute()?.compute[index];
        let g = &self.graph.compute;
        let before = s.counter.r64(0)?;
        let submitted = before.checked_add(1).ok_or(EOVERFLOW)?;
        let refs = ComputeRefs {
            notifier: g.notifier.va + notifier_page::COMPUTE_RECORD as u64,
            qinfo: g.qinfo.va,
            user_stamp: g.notifier.va + notifier_page::COMPUTE_STAMP as u64,
            fw_stamp: g.fw_stamp.va,
            work: s.work.va,
            work_gva: s.work.lower_gva.ok_or(EINVAL)?,
            // Exact embedded sequence: every self-reference then matches the 25G83 layout.
            seq: s.work.va + 0x900,
            preempt_gva: s.preempt.gva.ok_or(EINVAL)?,
            its: s.its.va,
            stats_fw: self.init.compute_activity.va,
            auxiliary_fw: s.aux.va,
            auxiliary_gva: s.aux.gva.ok_or(EINVAL)?,
            context_store_gva: s.context_store.gva.ok_or(EINVAL)?,
            counter_fw: s.counter.va,
            pool_state_gva: s.pool_state.gva.ok_or(EINVAL)?,
            page_list_gva: s.page_list.gva.ok_or(EINVAL)?,
            usc_base: job.queue.usc_base,
            record_fw: g.notifier.va + notifier_page::COMPUTE_RECORD as u64,
            binding_generation: generation,
            uma_slot: 1,
            // Preserve the already qualified packet bytes while expressing the native
            // fields separately. This compatibility identifier is not proof of native
            // global command-ID allocation or uniqueness after compute-index wrap.
            command_id: (u64::from((n - 1) as u32) << 32) | 1,
            compute_index: (n - 1) as u32,
            work_818: 0,
            target_stamp: target,
            submitted,
        };
        let scratch = self.engine.scratch.as_deref_mut().ok_or(ENOMEM)?;
        let w = image(&mut scratch.cw)?;
        // User results are published coherently after the complete retirement receipt.
        // The firmware's internal timestamp pair remains enabled.
        encode::encode_compute(
            &refs,
            &cmd.cmd,
            &cmd.attachments,
            &[0; 2],
            job.vm.slot,
            n,
            1,
            w,
            image(&mut scratch.cq)?,
        )
        .map_err(|_| EINVAL)?;
        if before != 0 {
            // Slots::claim_compute proved a different fully retired unit displaced this
            // descriptor in the same hardware slot. Keep every firmware-mutated byte.
            let mut descriptor = [0u8; work::compute::DESCRIPTOR_SIZE];
            s.work.read(work::compute::DESCRIPTOR, &mut descriptor)?;
            if encode::preserve_compute_descriptor(&refs, &descriptor, before, w).is_err() {
                // This is retained allocator identity/completion corruption, not a client
                // refusal. Keep the unit and its previous VM lease mapped forever; cancelling
                // preparation would make the corrupt descriptor available to another job.
                self.slots.quarantine(&mut f.claims);
                return Err(EIO);
            }
        }
        s.work.write(0, w)?;
        f.uma_submitted = submitted;
        wmb();
        g.ring.w64(8 * pf.c_head as usize, s.work.va)?;
        pf.c_head = (pf.c_head + 1) % sizes::QUEUE_CAP;
        f.cstamp[0] = target;
        f.cwait[0] = 0;
        pf.computes = 1;
        pf.n = n;
        pf.last_ct = target;
        pf.prev = Prev::Compute;
        Ok(())
    }

    /// Encodes paired TA/FS work and the fragment's explicit TA dependency in one retained
    /// pass unit. The preceding command already has a complete wire receipt.
    fn prefix_render(
        &mut self,
        job: &Arc<Job>,
        cmd: &RenderCommand,
        pf: &mut Prefix,
        f: &mut Flight,
    ) -> Result {
        let k = f.claims.pass[0] as usize;
        let draw = super::recovery::logic::next_ordinal(pf.draw).ok_or(EOVERFLOW)?;
        let target = encode::stamp(draw);
        // Claim performs only a qualified per-use scrub, never allocator/list reset.
        self.slots.claim_pass(job.resources.render()?, k)?;
        self.slots.hold_vm(&f.claims, &job.vm)?;
        let (refs, init_buffer) = self.render_refs(k, job, draw)?;
        let p = &job.resources.render()?.passes[k];
        let pass = p.refs();
        let geometry = Geometry::new(&cmd.cmd, pass.heap_gva);
        geometry.validate_capacity(p.tpc.size as u64, p.tilemap.size as u64)?;
        job.resources
            .render()?
            .set_dirty(k, geometry.tpc_bytes, geometry.tilemap_bytes)?;
        let scratch = self.engine.scratch.as_deref_mut().ok_or(ENOMEM)?;
        encode::encode_ta(
            &refs,
            &pass,
            &cmd.cmd,
            &[0; 4],
            job.vm.slot,
            target,
            &geometry,
            None,
            image(&mut scratch.ta)?,
            image(&mut scratch.tas)?,
        );
        encode::encode_fs(
            &refs,
            &pass,
            &cmd.cmd,
            &[0; 4],
            job.vm.slot,
            job.queue.usc_base,
            draw,
            target,
            &geometry,
            image(&mut scratch.fs)?,
            &mut scratch.fss[..],
        );
        encode::render_attachments(&mut scratch.tas[..], 0, &cmd.vertex_attachments);
        encode::render_attachments(&mut scratch.fss[..], 1, &cmd.fragment_attachments);
        encode::fill_scene(&refs, &pass, &mut scratch.scene[..]);
        encode::scene_render(
            &refs,
            &pass,
            &[0; 4],
            job.vm.slot,
            target,
            init_buffer,
            &mut scratch.scene[..],
        );
        p.ta_work.write(0, &scratch.ta[..])?;
        p.fs_work.write(0, &scratch.fs[..])?;
        p.ta_seq.write(0, &scratch.tas[..])?;
        p.fs_seq.write(0, &scratch.fss[..])?;
        p.scene.write(0, &scratch.scene[..])?;
        // One retained descriptor is shared by TA and FS; its private submitted counter
        // advances by two. Reused descriptors and their pool/list bytes remain intact.
        f.uma_submitted = refs.uma_submitted;
        let r = &self.graph.render;
        let cap = sizes::QUEUE_CAP;
        if init_buffer {
            pf.initbm = true;
            let bm = encode::initbm(job.vm.slot, refs.parameter_id, refs.pb_mgr, target);
            let epoch = job.resources.pb(job.progress.sent() as usize)?;
            epoch.initbm.write(0, &bm)?;
            r.ring[0].w64(8 * pf.ta_head as usize, epoch.initbm.va)?;
            pf.ta_head = (pf.ta_head + 1) % cap;
        }
        // Every previous command has its complete receipt before this publication.
        // The paired fragment still requires the native explicit TA dependency.
        let own = encode::barrier(
            r.fw_stamp[0].va,
            target,
            0x70,
            target,
            refs.uuid,
            BarrierType::Stamp,
        );
        p.own_barrier.write(0, &own)?;
        r.ring[0].w64(8 * pf.ta_head as usize, p.ta_work.va)?;
        pf.ta_head = (pf.ta_head + 1) % cap;
        r.ring[1].w64(8 * pf.fs_head as usize, p.own_barrier.va)?;
        pf.fs_head = (pf.fs_head + 1) % cap;
        r.ring[1].w64(8 * pf.fs_head as usize, p.fs_work.va)?;
        pf.fs_head = (pf.fs_head + 1) % cap;
        pf.renders = 1;
        pf.draw = draw;
        pf.last_fs = target;
        pf.prev = Prev::Render;
        Ok(())
    }

    /// Records the publication time of `f` and the preparation, gap and starvation times.
    fn stat_publish(&mut self, f: &mut Flight) {
        let s = &mut self.engine.stats;
        s.pub_ns = now_ns();
        f.published_ns = s.pub_ns;
        s.prep_ns += s.pub_ns.wrapping_sub(s.prep_from_ns);
        if s.idle_from_ns != 0 {
            s.gap_ns += s.pub_ns.wrapping_sub(s.idle_from_ns);
            s.gaps += 1;
            s.idle_from_ns = 0;
        } else if s.starve_from_ns != 0 {
            s.starved_ns += s.pub_ns.wrapping_sub(s.starve_from_ns);
        }
        s.starve_from_ns = 0;
    }
}
