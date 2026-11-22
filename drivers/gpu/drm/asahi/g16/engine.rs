// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Serialized M4 execution over the shared Asahi frontend.
//!
//! A userspace submission retains all commands, mappings and inputs through its final fence.
//! Each command is encoded into a retained unit, then published to the classic engine queues.
//! A render is published only with nothing else in flight, and nothing is published behind
//! it. Compute commands queue behind in-flight compute commands (up to
//! [`params::compute_flights`]), each in its own unit of UMA slot 1: the firmware runs its
//! compute queue in publication order, each command to its Finalize, so publication order is
//! execution order. A unit is reused only after a later publication fully retired (it
//! displaced the unit). TA and FS remain a paired publication with the native explicit TA
//! dependency. This total order satisfies Mesa's command barriers.
//! Firmware failure retains every possibly reachable owner. Completions are signalled only
//! after dropping the device lock.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use kernel::{prelude::*, sync::Arc};

use super::channel::Channel;
use super::command::{Command, ComputeCommand, RenderCommand};
use super::encode::{self, ComputeRefs, Geometry, RenderRefs};
use super::fw::channels::{SubmitRecord, RING_SLOTS};
use super::fw::graph::{notifier_page, qstate};
use super::fw::work::{self, BarrierType};
use super::hw::{engine as engine_ids, sizes};
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

/// Counter value carried by a native 25G83 submit-pipe record (the 24 MHz counter of the
/// firmware's internal timestamps).
pub(crate) fn counter_ticks() -> u64 {
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

/// Resolved in-flight producer target for a hardware ring barrier.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct WaitTarget {
    stamp: u32,
    /// True if the waited producer flight carries `InitBM` (buffer-manager initialization).
    initbm: bool,
}

/// Selects the hardware ring barrier type for a same-VM handoff.
///
/// Unlike G15 (`g15/hw.rs: CROSS_ENGINE_HOLD` — "Never both type 0"), G16 (M4 25G83/26B123)
/// does not require a software-job hold on one cross-engine direction when neither side
/// carries `InitBM`. On G16, every stage (CDM, TA, FS) executes its `FINALIZE`
/// micro-sequence — flushing stage caches to the shared coherent fabric — before firmware
/// advances `fw_stamp`, so waiting on `fw_stamp` with `BarrierType::Stamp` (type 0) in both
/// directions (`fs2c` and `c2ta`/`c2fs`) avoids hold bubbles within one VM (same ASID and
/// UAT page tables). Zero-fault M4 J773g runs (13,873 `fs2c` and 20,506 `c2ta` type-0
/// handoffs) confirm no GPU wedge or hang, while CTS synchronization / pipeline-barrier
/// coverage validates producer-to-consumer memory visibility.
///
/// Keep `BarrierType::Hold` (type 1) whenever either producer or consumer carries `InitBM`
/// (checked against both the waited target and any live `InitBM` flight). Cross-VM switches,
/// recovery paths, and `query_access` CPU-timestamp waits drain `flights` completely first.
const fn cross_engine_barrier_type(producer_initbm: bool, consumer_initbm: bool) -> BarrierType {
    BarrierType::from_stamp_only(!producer_initbm && !consumer_initbm)
}

/// Resolved ring-barrier placement for one render pass (legacy, `FRAGMENT_BARRIERS`, or
/// `DRM_ASAHI_RENDER_STAGE_PREFIXES`).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct RenderBarrierPlan {
    early: bool,
    ta_prev_wait: bool,
    ta_fs_wait: Option<u32>,
    fs_render_wait: Option<u32>,
    ta_compute_wait: Option<u32>,
    fs_compute_wait: Option<u32>,
}

/// Computes TA/FS hardware ring barriers for a render command from its flags, prior render
/// FS stamp, resolved render waits (`fs_wait_render`, `ta_wait_fs`), and resolved `[ta, fs]`
/// compute wait stamps.
///
/// Pipelined renders share one parameter-buffer / UMA epoch (`RENDER_UMA_SLOT`), so TA(N)
/// must still wait on TA(N-1) (`ta_prev_wait`, matching G15's `prev_barrier`) whenever
/// `ta_fs_wait` does not already wait on `prev_fs` (which dominates `prev_ta` via
/// `own_barrier`). Independent FS passes still omit `fs_dep`.
fn plan_render_barriers(
    flags: u32,
    class_behind: bool,
    early_tiling: bool,
    prev_fs: u32,
    fs_wait_render: Option<u32>,
    ta_wait_fs: Option<u32>,
    compute_waits: [Option<u32>; 2],
) -> RenderBarrierPlan {
    let staged =
        flags & kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES != 0;
    let frag_only =
        flags & kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS != 0;
    let no_render_barrier = fs_wait_render.is_none();
    let early = class_behind
        && early_tiling
        && if staged {
            ta_wait_fs != Some(prev_fs)
        } else {
            frag_only || no_render_barrier
        };
    let ta_fs_wait = if !class_behind {
        None
    } else if !early {
        if !early_tiling {
            Some(prev_fs)
        } else if staged {
            ta_wait_fs.or(Some(prev_fs))
        } else {
            fs_wait_render.or(Some(prev_fs))
        }
    } else if staged {
        ta_wait_fs
    } else {
        None
    };
    let ta_prev_wait = class_behind && ta_fs_wait != Some(prev_fs);
    let fs_render_wait = if class_behind && early && fs_wait_render != ta_fs_wait {
        fs_wait_render
    } else {
        None
    };
    let [ta_compute_wait, fs_wait] = compute_waits;
    let fs_compute_wait = if fs_wait != ta_compute_wait {
        fs_wait
    } else {
        None
    };
    RenderBarrierPlan {
        early,
        ta_prev_wait,
        ta_fs_wait,
        fs_render_wait,
        ta_compute_wait,
        fs_compute_wait,
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
    /// Compute member j's FW stamp target, the FS target it waits on directly (0: none), and
    /// its private UMA counter value after publication.
    pub(crate) cstamp: [u32; sizes::COMPUTE_SLOTS],
    pub(crate) cwait: [u32; sizes::COMPUTE_SLOTS],
    pub(crate) csubmitted: [u64; sizes::COMPUTE_SLOTS],
    /// Render member j's TA and FS FW stamp targets.
    pub(crate) r_ta_stamp: [u32; sizes::RENDER_SLOTS],
    pub(crate) r_fs_stamp: [u32; sizes::RENDER_SLOTS],
    /// Compute slots, pass slots and clustering buffers it holds.
    pub(crate) claims: Claims,
    /// A kill or recovery ended this prefix's VM: later prefixes of it fail with EIO.
    pub(crate) doomed: bool,
    /// A firmware recovery saw this command unfinished. Full ordinary receipts can still
    /// prove success; otherwise an aborted receipt is accepted only after the grace period.
    pub(crate) uma_submitted: u64,
    pub(crate) recovery_suspect: bool,
    pub(crate) recovery_started_ns: u64,
    /// First time this suspect flight passed its stage/queue gates with unsettled receipts, and
    /// how many consecutive unsettled polls have observed it since then.
    pub(crate) gate_passed_ns: Option<u64>,
    pub(crate) unsettled_polls: u32,
    /// Publication time, ns.
    pub(crate) published_ns: u64,
    /// Published behind another flight.
    pub(crate) behind: bool,
    /// The PB epoch this render prefix uses (its own, or a reused allocator-slot epoch).
    pub(crate) pb: Option<Arc<super::pools::PbEpoch>>,
    /// Its allocator slot (`0` or `1`, decoupled from `claims.pass[0]`).
    pub(crate) pb_slot: u32,
    /// Its allocator slot binding (`Pools::bind`).
    pub(crate) pb_binding: u64,
    /// Whether this render carries InitBM (priming renders always drain before pipelining).
    pub(crate) initbm: bool,
    /// Whether this flight publishes CPU-written command timestamps on retirement.
    pub(crate) has_timestamps: bool,
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
            csubmitted: [0; sizes::COMPUTE_SLOTS],
            r_ta_stamp: [0; sizes::RENDER_SLOTS],
            r_fs_stamp: [0; sizes::RENDER_SLOTS],
            claims: Claims::new(),
            doomed: false,
            uma_submitted: 0,
            recovery_suspect: false,
            recovery_started_ns: 0,
            gate_passed_ns: None,
            unsettled_polls: 0,
            published_ns: 0,
            behind: false,
            pb: None,
            pb_slot: 0,
            pb_binding: 0,
            initbm: false,
            has_timestamps: false,
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
    last_ta: u32,
    last_fs: u32,
}

/// Error provenance must remain independent of errno: EIO can be an ordinary failed VM
/// status before publication, or a transport error after firmware may see new producers.
#[derive(Copy, Clone, Debug)]
enum PublicationError {
    Unpublished(Error),
    Uncertain(Error),
}

/// Engine statistics, printed by `aurora-g16/stats`.
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
    /// Compute commands published behind in-flight ones.
    pub(crate) behind: u64,
    /// Most flights in flight at once.
    pub(crate) max_flights: u64,
}

/// Host-side phase timers of publication and polling, on with `debug_flags` bit 49
/// (`Debug1`); printed as the `prof:` line of `aurora-g16/stats`. A measurement aid: nothing is
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
            scene: z(work::scene::ACTIVE_SIZE)?,
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
    /// Authenticated GPU tick tuple captured by [`Firmware::retired_snapshot`] for the oldest
    /// flight (`[start, end, 0, 0]` for compute; `[ta_start, ta_end, fs_start, fs_end]` for
    /// render), reused by accounting without re-reading WC cells.
    pub(crate) retired_ticks: [u64; 4],
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
    /// A ready same-VM pending job is blocked solely on flight, pass/compute slot, or work-ring
    /// headroom (not cross-VM/InitBM drain, external fences, or CPU timestamp waits): repoll
    /// sooner to refill freed slots promptly.
    pub(crate) slot_wait: bool,
    /// An EP 0x20 event arrived with a flight still settling its final receipt: repoll sooner.
    pub(crate) short_repoll: bool,
    /// Progress tracking of the oldest compute and render flights.
    pub(crate) progress_flight: [u64; 2],
    pub(crate) progress: [Option<u32>; 3],
    /// Boxed with [`Self::prof`]: `Firmware` is built on the probe's stack.
    pub(crate) stats: KBox<Stats>,
    /// Phase timers (debug_flags bit 49).
    pub(crate) prof: KBox<Prof>,
    /// The `aurora-g16/stats` file.
    pub(crate) debugfs: Option<Pin<KBox<kernel::debugfs::File<Arc<Shared>>>>>,
    scratch: Option<KBox<Scratch>>,
}

impl Engine {
    pub(crate) fn new() -> Result<Self> {
        // Wrapped native stamps are safe only while pending references fit within the
        // signed half-window: at most FLIGHTS commands are published at once, far inside it.
        Ok(Self {
            pending: KVec::new(),
            flights: KVec::with_capacity(sizes::FLIGHTS, GFP_KERNEL)?,
            retained: KVec::new(),
            frozen: false,
            turn: 0,
            shared: None,
            retired_ticks: [0; 4],
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
            slot_wait: false,
            short_repoll: false,
            progress_flight: [0; 2],
            progress: [None; 3],
            stats: KBox::new(Stats::default(), GFP_KERNEL)?,
            prof: KBox::new(Prof::default(), GFP_KERNEL)?,
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
        if flags & 1 != 0 && self.recovery.last_retired.contains(event, target) {
            // A notification may follow the full receipt. It names the retained old
            // owner, not the next command that happens to occupy the flight list.
            return Ok(());
        }
        // The named owner: the oldest flight, or with compute/render queued behind it a later one.
        let f = self
            .engine
            .flights
            .iter_mut()
            .find(|f| match event {
                15 => {
                    f.computes != 0
                        && f.cstamp[..(f.computes as usize).min(f.cstamp.len())].contains(&target)
                }
                0x70 => {
                    f.renders != 0
                        && f.r_ta_stamp[..(f.renders as usize).min(f.r_ta_stamp.len())]
                            .contains(&target)
                }
                0x71 => {
                    f.renders != 0
                        && f.r_fs_stamp[..(f.renders as usize).min(f.r_fs_stamp.len())]
                            .contains(&target)
                }
                _ => false,
            })
            .ok_or(ENOENT)?;
        if flags & 1 == 0 {
            return Err(ENOENT);
        }
        if flags & 9 == 9 && f.renders != 0 {
            let mut cleaned = false;
            let count = (f.renders as usize).min(f.r_ta_stamp.len());
            for m in 0..count {
                if super::poll::stamp_reached(stage[0], f.r_ta_stamp[m], self.engine.ta_target) {
                    continue;
                }
                let k = f.claims.pass[m] as usize;
                let pass = f.job.resources.render_pass(k)?;
                let bytes = pass.ta_work.r64(work::ta::TPC_BYTES)?;
                if bytes == 0 || bytes > pass.tpc.size as u64 {
                    return Err(ERANGE);
                }
                pass.tpc.fill(0, 0, bytes as usize)?;
                cleaned = true;
            }
            if cleaned {
                wmb();
            }
        }
        f.doomed = true;
        f.recovery_suspect = true;
        f.recovery_started_ns = now_ns();
        f.gate_passed_ns = None;
        f.unsettled_polls = 0;
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
        let reached = super::recovery::logic::stamp_reached;
        let (target, ta_target, fs_target) = (
            self.engine.target,
            self.engine.ta_target,
            self.engine.fs_target,
        );
        // Every unfinished flight, including compute queued behind the oldest.
        for f in self.engine.flights.iter_mut() {
            let unfinished = (f.computes != 0 && !reached(stage[2], f.target, target))
                || (f.renders != 0
                    && (!reached(stage[0], f.ta_target, ta_target)
                        || !reached(stage[1], f.fs_target, fs_target)));
            if unfinished {
                f.recovery_suspect = true;
                f.recovery_started_ns = now_ns();
                f.gate_passed_ns = None;
                f.unsettled_polls = 0;
                if fault_asid.is_some_and(|asid| asid >= 1 && asid == f.job.vm.slot) {
                    f.doomed = true;
                }
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
        // What arrives during a system sleep waits for its end.
        if self.session.shared.sleeping.load(core::sync::atomic::Ordering::Acquire) {
            return Ok(());
        }
        self.session.check()?;
        self.objects.health()?;
        self.service_power()?;
        self.drain_rings()?;
        self.check_hangs(done)?;
        self.publish_checked(done)
    }

    /// [`Self::publish`] for a fast submit or poll pass that already drained/retired under this
    /// device lock; verifies RTKit/transport liveness without repeating the 1 ms `Session::check`
    /// register and UAT table walk on every command.
    pub(crate) fn publish_checked(&mut self, done: &mut Completions) -> Result {
        if self
            .session
            .shared
            .sleeping
            .load(core::sync::atomic::Ordering::Acquire)
        {
            return Ok(());
        }
        self.session.transport()?;
        // The initial G16 backend deliberately serializes hardware publication. Stronger
        // ordering satisfies Mesa's barriers while slot displacement and cache ownership
        // stay explicit. Never enable G15's submit-ahead policy on the M4 wire by accident.
        let mut result = Ok(());
        let mut pending_rings = 0u8;
        let mut batch_published = 0u64;

        self.engine.chan_wait = false;
        self.engine.slot_wait = false;
        while self.recovery.failed().is_none()
            && !self.engine.frozen
            && !self.engine.pending.is_empty()
        {
            let Some(index) = self.engine.pick() else {
                break;
            };
            let mut job = self.engine.pending[index].clone();
            // Even a failed job may have an earlier command in flight. An aggregate
            // error fence must not race that command's access to user buffers: only
            // healthy same-VM non-InitBM commands queue behind active flights; when the
            // head queue cannot queue behind (e.g. its class hit its flight cap), still
            // allow another ready queue of the other class to run concurrently, unless
            // the head job itself requires `flights` to drain to empty first (cross-VM
            // switch, unprimed InitBM render, failed VM, or recovery/InitBM in flight).
            if !self.engine.flights.is_empty() && !self.can_queue_behind(&job) {
                if self.needs_engine_drain(&job) {
                    self.engine.stats.hold[0] += 1;
                    break;
                }
                let mut alt: Option<usize> = None;
                for (i, cand) in self.engine.pending.iter().enumerate() {
                    if i == index || !self.can_queue_behind(cand) {
                        continue;
                    }
                    let better = match alt {
                        None => true,
                        Some(best) => {
                            let prev = &self.engine.pending[best];
                            cand.queue.priority < prev.queue.priority
                                || (cand.queue.priority == prev.queue.priority
                                    && cand.queue.turn.load(Ordering::Relaxed)
                                        < prev.queue.turn.load(Ordering::Relaxed))
                        }
                    };
                    if better {
                        alt = Some(i);
                    }
                }
                match alt {
                    Some(i) => job = self.engine.pending[i].clone(),
                    None => {
                        self.engine.slot_wait = self
                            .engine
                            .pending
                            .iter()
                            .any(|cand| self.blocked_on_slot_headroom(cand));
                        self.engine.stats.hold[0] += 1;
                        break;
                    }
                }
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
                    if self.slots.render_exhausted() || self.pools.exhausted() {
                        Some((super::command::RENDER, EIO))
                    } else if super::recovery::logic::next_ordinal(self.engine.draw).is_none() {
                        Some((super::command::RENDER, EOVERFLOW))
                    } else {
                        None
                    }
                }
                None => None,
            };
            // A VM whose own units were all quarantined (without the class being exhausted)
            // cannot run compute again: cancel its queue like an exhausted class would.
            if class_error.is_none()
                && matches!(command(&job, job.progress.sent()), Some(Command::Compute(_)))
                && job
                    .resources
                    .compute_units()
                    .is_ok_and(|n| self.slots.compute_exhausted_within(n))
            {
                self.engine.unpend(&job);
                let _ = job.mark_terminal_retired();
                complete(done, &job, Err(ECANCELED));
                continue;
            }
            // A VM whose render pass slots were all quarantined, or that cannot acquire a
            // physical PB allocator slot because the alternate slot is quarantined and the
            // active slot belongs to another VM, cannot render again: cancel its queue
            // immediately instead of spinning on EAGAIN and stalling unrelated compute.
            if class_error.is_none()
                && matches!(command(&job, job.progress.sent()), Some(Command::Render(_)))
                && (job
                    .resources
                    .pb_slots()
                    .is_ok_and(|n| self.pools.exhausted_for_within(&job.vm, n))
                    || job
                        .resources
                        .render_passes()
                        .is_ok_and(|n| self.slots.render_exhausted_within(n)))
            {
                self.engine.unpend(&job);
                let _ = job.mark_terminal_retired();
                complete(done, &job, Err(ECANCELED));
                continue;
            }
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
            match self.publish_prefix(&job, done, &mut pending_rings) {
                Ok(()) => {
                    self.engine.turn += 1;
                    job.queue.turn.store(self.engine.turn, Ordering::Relaxed);
                    batch_published += 1;
                    self.recovery.publication_wait_deadline = None;
                    if job.progress.sent() == job.progress.count() {
                        self.engine.unpend(&job); // fully published
                    }
                    continue;
                }
                Err(PublicationError::Unpublished(e))
                    if e == EAGAIN && !self.engine.flights.is_empty() =>
                {
                    // Waits for an in-flight command: its retirement repolls publication,
                    // and the flights' own deadlines apply.
                    let i = if self.engine.chan_wait { 1 } else { 2 };
                    self.engine.stats.hold[i] += 1;
                    break;
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
                        // `self.engine.flights.is_empty()` already implies `pending_rings == 0`
                        // today, but flush defensively so records posted earlier in the batch
                        // cannot be stranded if this error ever stops being fatal.
                        if pending_rings != 0 {
                            let _ = self.flush_doorbells(&mut pending_rings);
                        }
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
                    pending_rings = 0;
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
        if batch_published > 1 {
            self.engine.stats.grouped = self
                .engine
                .stats
                .grouped
                .saturating_add(batch_published - 1);
        }
        if pending_rings != 0 {
            if let Err(e) = self.flush_doorbells(&mut pending_rings) {
                dev_err!(
                    self.graph.dev.as_ref(),
                    "G16: batched doorbell ring failed after visibility began ({:?})\n",
                    e
                );
                self.fail_stop(EIO, done);
                result = Err(EIO);
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

    /// Rings each submit channel touched during a publication batch once, preserving
    /// FS-before-TA doorbell order for render batches.
    fn flush_doorbells(&mut self, pending_rings: &mut u8) -> Result {
        let mask = core::mem::take(pending_rings);
        for channel in [Channel::Fs, Channel::Ta, Channel::Compute] {
            if mask & (1u8 << (channel as u8)) != 0 {
                self.session.ring(channel)?;
            }
        }
        Ok(())
    }

    /// Whether `job` requires `self.engine.flights` to drain to empty before it can
    /// publish (so `alt`-class bypass must not keep refilling the engine ahead of it).
    fn needs_engine_drain(&self, job: &Arc<Job>) -> bool {
        job.vm.status.get() != 0
            || self.engine.flights.iter().any(|f| {
                f.doomed || f.recovery_suspect || f.initbm || !Arc::ptr_eq(&f.job.vm, &job.vm)
            })
            || (matches!(command(job, job.progress.sent()), Some(Command::Render(_)))
                && !job
                    .resources
                    .pb_slots()
                    .is_ok_and(|n| self.pools.can_pipeline_within(&job.vm, n)))
    }

    /// Whether `job` is ready to run on the same VM but currently blocked solely on
    /// flight-depth or pass/compute-slot headroom (not on cross-VM/InitBM/recovery drain,
    /// external sync dependencies, or CPU timestamp-publish waits).
    fn blocked_on_slot_headroom(&self, job: &Arc<Job>) -> bool {
        let e = &self.engine;
        if e.flights.is_empty() || self.needs_engine_drain(job) {
            return false;
        }
        let sent = job.progress.sent();
        if job.check_command_dependencies(sent).is_err() {
            return false;
        }
        if e.flights.len() >= super::hw::sizes::FLIGHTS {
            return true;
        }
        match command(job, sent) {
            Some(Command::Compute(_)) => {
                let compute_count: usize = e.flights.iter().map(|f| f.computes as usize).sum();
                compute_count > 0
                    && !self.slots.compute_exhausted()
                    && (compute_count >= params::compute_flights()
                        || job
                            .resources
                            .compute_units()
                            .is_ok_and(|units| !self.slots.compute_available(units)))
            }
            Some(Command::Render(_)) => {
                let render_count: usize = e.flights.iter().map(|f| f.renders as usize).sum();
                render_count > 0
                    && !self.slots.render_exhausted()
                    && !self.pools.exhausted()
                    && (render_count >= params::render_flights()
                        || job
                            .resources
                            .render_passes()
                            .is_ok_and(|passes| !self.slots.pass_available(passes)))
            }
            None => false,
        }
    }

    /// Whether `job`'s next command may be published behind the flights in flight:
    ///
    /// - Compute queues behind healthy same-VM non-InitBM compute and/or render flights (up to
    ///   [`params::compute_flights`] active computes), each in its own `ComputeSlot`
    ///   (`its`/`work`), unless `query_access` is set and a same-queue predecessor in its
    ///   compute or render barrier prefix has CPU-published timestamp outputs that have not
    ///   retired yet (`DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS` clears `query_access`).
    /// - Render queues behind healthy same-VM non-InitBM render and/or compute flights (up to
    ///   [`params::render_flights`] active renders), rotating `PassSlot` (`0..RENDER_SLOTS`)
    ///   while sharing the active exposed PB/UMA allocator slot (`Pools::can_pipeline`).
    ///   Hardware barriers on the CDM/TA/FS rings preserve exact same-engine and cross-engine
    ///   ordering while barrier-free or independent-queue compute and render run concurrently.
    /// - Cross-VM switches, InitBM priming renders, and every failure or recovery path still
    ///   drain the engine first.
    fn can_queue_behind(&self, job: &Arc<Job>) -> bool {
        let e = &self.engine;
        if job.vm.status.get() != 0
            || e.flights.len() >= super::hw::sizes::FLIGHTS
            || e.flights.iter().any(|f| {
                f.doomed || f.recovery_suspect || f.initbm || !Arc::ptr_eq(&f.job.vm, &job.vm)
            })
        {
            return false;
        }
        let sent = job.progress.sent();
        if job.check_command_dependencies(sent).is_err() {
            return false;
        }
        match command(job, sent) {
            Some(Command::Compute(c)) => {
                // G16 publishes timestamp results into user query-pool BOs on CPU during flight
                // retirement, so dispatches without DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS must wait
                // for prior timestamped flights in their dependency prefix to retire first.
                let waits_for_timestamps = c.query_access
                    && job.command_order(sent).is_some_and(|o| {
                        let waits_class = |class: usize| {
                            o.wait_through[class].is_some_and(|last| {
                                e.flights.iter().any(|f| {
                                    let matches_class = if class == super::command::RENDER {
                                        f.renders != 0
                                    } else {
                                        f.computes != 0
                                    };
                                    matches_class
                                        && f.has_timestamps
                                        && Arc::ptr_eq(&f.job.queue, &job.queue)
                                        && f.job
                                            .command_order(f.first as u32)
                                            .is_none_or(|fo| fo.sequence <= last)
                                })
                            })
                        };
                        waits_class(super::command::COMPUTE) || waits_class(super::command::RENDER)
                    });
                let compute_count: usize = e.flights.iter().map(|f| f.computes as usize).sum();
                !waits_for_timestamps
                    && compute_count < params::compute_flights()
                    && !self.slots.compute_exhausted()
                    && super::recovery::logic::next_ordinal(e.ordinal).is_some()
                    && job
                        .resources
                        .compute_units()
                        .is_ok_and(|units| self.slots.compute_available(units))
            }
            Some(Command::Render(_)) => {
                let render_count: usize = e.flights.iter().map(|f| f.renders as usize).sum();
                render_count < params::render_flights()
                    && !self.slots.render_exhausted()
                    && !self.pools.exhausted()
                    && super::recovery::logic::next_ordinal(e.draw).is_some()
                    && job
                        .resources
                        .pb_slots()
                        .is_ok_and(|n| self.pools.can_pipeline_within(&job.vm, n))
                    && job
                        .resources
                        .render_passes()
                        .is_ok_and(|passes| self.slots.pass_available(passes))
            }
            None => false,
        }
    }

    /// Resolves a same-queue target stamp and producer `InitBM` state for `class` (`RENDER` ->
    /// `r_fs_stamp`, `COMPUTE` -> `cstamp`) when `target_seq` names an in-flight command of
    /// `job.queue` or an earlier member of an in-progress render batch `current`. Returns
    /// `Ok(None)` when no barrier was requested or the waited prefix has already retired.
    fn wait_stamp_for_with(
        &self,
        job: &Arc<Job>,
        class: usize,
        target_seq: Option<u64>,
        current: Option<&Flight>,
    ) -> Result<Option<WaitTarget>> {
        let Some(last) = target_seq else {
            return Ok(None);
        };
        if job.queue.completed[class].load(Ordering::Acquire) >= last {
            return Ok(None);
        }
        if class == super::command::COMPUTE {
            for flight in &self.engine.flights {
                if flight.computes == 0 || !Arc::ptr_eq(&flight.job.queue, &job.queue) {
                    continue;
                }
                let mut member = 0usize;
                for k in flight.first..flight.first + flight.count {
                    if flight.job.command_order(k as u32).is_some_and(|fo| {
                        fo.engine == super::command::COMPUTE && fo.sequence == last
                    }) {
                        return Ok(Some(WaitTarget {
                            stamp: *flight.cstamp.get(member).ok_or(EAGAIN)?,
                            initbm: flight.initbm,
                        }));
                    }
                    member += 1;
                }
                for j2 in flight.group.iter() {
                    for k in 0..j2.progress.count() {
                        if j2.command_order(k).is_some_and(|fo| {
                            fo.engine == super::command::COMPUTE && fo.sequence == last
                        }) {
                            return Ok(Some(WaitTarget {
                                stamp: *flight.cstamp.get(member).ok_or(EAGAIN)?,
                                initbm: flight.initbm,
                            }));
                        }
                        member += 1;
                    }
                }
            }
            return Err(EAGAIN);
        }
        let find_render = |flight: &Flight| -> Option<WaitTarget> {
            if flight.renders == 0 || !Arc::ptr_eq(&flight.job.queue, &job.queue) {
                return None;
            }
            let count = (flight.renders as usize).min(flight.r_fs_stamp.len());
            for member in 0..count {
                let k = (flight.first + member) as u32;
                if flight
                    .job
                    .command_order(k)
                    .is_some_and(|fo| fo.engine == super::command::RENDER && fo.sequence == last)
                {
                    return Some(WaitTarget {
                        stamp: flight.r_fs_stamp[member],
                        initbm: flight.initbm,
                    });
                }
            }
            None
        };
        if let Some(target) = current.and_then(find_render) {
            return Ok(Some(target));
        }
        for flight in &self.engine.flights {
            if let Some(target) = find_render(flight) {
                return Ok(Some(target));
            }
        }
        Err(EAGAIN)
    }

    /// Resolves a same-queue target stamp for `class` across published in-flight flights.
    fn wait_stamp_for(
        &self,
        job: &Arc<Job>,
        class: usize,
        target_seq: Option<u64>,
    ) -> Result<Option<WaitTarget>> {
        self.wait_stamp_for_with(job, class, target_seq, None)
    }

    /// Resolves a same-queue cross-engine barrier target when `job`'s command `index` waits on
    /// an in-flight command of `other` engine (`RENDER` -> `r_fs_stamp`, `COMPUTE` -> `cstamp`).
    /// Returns `Ok(None)` when no cross-engine barrier was requested or the waited prefix has
    /// already retired.
    fn cross_engine_wait(
        &self,
        job: &Arc<Job>,
        index: u32,
        other: usize,
    ) -> Result<Option<WaitTarget>> {
        let target_seq = job
            .command_order(index)
            .and_then(|o| o.wait_through[other]);
        self.wait_stamp_for(job, other, target_seq)
    }

    /// Whether some of `job`'s commands were published: such a job is retained after a
    /// fail-stop.
    pub(crate) fn job_partly_published(&self, job: &Job) -> bool {
        job.progress.sent() != 0
    }

    /// Addresses and cumulative allocator state of the selected retained render allocator slot.
    fn render_refs(
        &mut self,
        alloc_slot: usize,
        job: &Job,
        draw: u64,
        render_pipelined: bool,
        engine_behind: bool,
    ) -> Result<(RenderRefs, bool, Arc<super::pools::PbEpoch>, u64)> {
        let generation = job.vm.generation;
        let mut a = job
            .resources
            .prepare_pb_in_flight(alloc_slot, draw, render_pipelined)?;
        // Reuse this VM's retired epoch of the allocator slot when still bound (no reseed/InitBM).
        let (epoch, reused) = match self.pools.reusable(alloc_slot, &job.vm) {
            Some(e) => {
                self.pools.stats.reused += 1;
                a.init_buffer = false;
                (e, true)
            }
            None => {
                if engine_behind {
                    return Err(EAGAIN);
                }
                let e = job.resources.take_pb()?;
                if let Err(err) = e.prepare(
                    alloc_slot,
                    job.resources.pb_heap_gva(alloc_slot)?,
                    job.resources.pb_heap_blocks()?,
                ) {
                    job.resources.return_pb(e);
                    return Err(err);
                }
                self.pools.stats.fresh += 1;
                (e, false)
            }
        };
        let binding = self.pools.bind(alloc_slot, reused);
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
                scene_list: 0,
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
            epoch,
            binding,
        ))
    }

    /// Allocator references for subsequent passes (`member > 0`) in a non-InitBM multi-pass
    /// render flight, reusing the active slot and PB epoch while advancing `uma_submitted` by 2.
    fn render_refs_batched(
        &self,
        alloc_slot: usize,
        job: &Job,
        draw: u64,
        epoch: &super::pools::PbEpoch,
        prev_uma_submitted: u64,
    ) -> Result<RenderRefs> {
        let a = job.resources.pb_allocator_refs(alloc_slot)?;
        let r = &self.graph.render;
        let stage = draw.checked_mul(2).ok_or(EOVERFLOW)?;
        let uma_submitted = prev_uma_submitted.checked_add(2).ok_or(EOVERFLOW)?;
        Ok(RenderRefs {
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
            pb_mgr: epoch.manager.va,
            scene_list: 0,
            mgr_misc: epoch.misc.va,
            pb_init_blocks: a.init_blocks,
            parameter_id: a.parameter_id,
            generation: job.vm.generation,
            activity: [self.init.ta_activity.va + 4, self.init.fs_activity.va + 8],
            counter: [stage, stage - 1],
            uma_submitted,
            uuid: (draw as u32).wrapping_add(1),
            usc_base: job.queue.usc_base,
        })
    }

    /// Publishes the next command of `job`. All images and ring entries precede the host
    /// producer writes and queue kicks. EAGAIN defers while storage/channel state is held;
    /// The error records whether firmware may see a counter/producer update, independently
    /// of errno; unpublished EIO is still an ordinary job failure.
    fn publish_prefix(
        &mut self,
        job: &Arc<Job>,
        _done: &mut Completions,
        pending_rings: &mut u8,
    ) -> core::result::Result<(), PublicationError> {
        let mut f = Flight::new(job.clone());
        let mut visible = false;
        let result = self.publish_prefix_into(job, &mut f, &mut visible, pending_rings);
        match result {
            Err(e) if !visible => {
                // Never published: its storage is free again. Horizon candidates are removed
                // from `pending` only after visibility begins, so they remain untouched here.
                self.slots.release(&mut f.claims);
                f.group.clear();
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
        pending_rings: &mut u8,
    ) -> Result {
        let result = self.publish_prefix_body(job, f, visible, pending_rings);
        if result.is_err() && !*visible {
            if f.pb.is_some() {
                self.pools.cancel_alloc(f.pb_slot as usize);
            }
            if let Some(epoch) = f.pb.take() {
                if epoch.unexposed() {
                    job.resources.return_pb(epoch);
                }
            }
        }
        result
    }

    fn publish_prefix_body(
        &mut self,
        job: &Arc<Job>,
        f: &mut Flight,
        visible: &mut bool,
        pending_rings: &mut u8,
    ) -> Result {
        let behind = !self.engine.flights.is_empty();
        if behind && !self.can_queue_behind(job) {
            return Err(EAGAIN);
        }
        let index = job.progress.sent();
        let cmd = command(job, index).ok_or(EINVAL)?;
        let render = matches!(cmd, Command::Render(_));
        let class_behind = self
            .engine
            .flights
            .iter()
            .any(|f| if render { f.renders != 0 } else { f.computes != 0 });
        if job.vm.status.get() != 0 {
            return Err(EIO);
        }
        if if render {
            self.slots.render_exhausted() || self.pools.exhausted()
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
            let busy = if class_behind {
                // Behind in-flight commands on this class: post after their unconsumed records.
                pipe.write()? != producer
                    || producer.wrapping_sub(pipe.read()?) % RING_SLOTS >= sizes::AHEAD_RECORDS
            } else {
                pipe.read()? != producer
                    || pipe.completion()? != producer
                    || pipe.write()? != producer
            };
            if busy {
                self.engine.chan_wait = true;
                return Err(EAGAIN);
            }
        }
        if self.engine.scratch.is_none() {
            return Err(EIO);
        }
        self.engine.stats.prep_from_ns = now_ns();
        let (c_head, ta_head, fs_head) = if render {
            (
                0,
                self.graph.render.qstate[0].r32(qstate::WRITE)?,
                self.graph.render.qstate[1].r32(qstate::WRITE)?,
            )
        } else {
            (self.graph.compute.qstate.r32(qstate::WRITE)?, 0, 0)
        };
        let mut prefix = Prefix {
            n: self.engine.ordinal,
            draw: self.engine.draw,
            c_head,
            ta_head,
            fs_head,
            ..Default::default()
        };
        if prefix.c_head >= 256 || prefix.ta_head >= 256 || prefix.fs_head >= 256 {
            return Err(super::boot::eproto());
        }
        let mut c_read = prefix.c_head;
        let mut ta_read = prefix.ta_head;
        let mut fs_read = prefix.fs_head;
        if class_behind {
            if render {
                // Room for up to 4 TA entries (prev_barrier, ta_fs_barrier, compute_barrier,
                // ta_work) and 4 FS entries (fs_dep, fs_compute_barrier, own_barrier, fs_work).
                ta_read = self.graph.render.qstate[0].r32(qstate::READ)?;
                fs_read = self.graph.render.qstate[1].r32(qstate::READ)?;
                if ta_read >= 256
                    || fs_read >= 256
                    || prefix.ta_head.wrapping_sub(ta_read) % sizes::QUEUE_CAP
                        >= sizes::QUEUE_CAP - 5
                    || prefix.fs_head.wrapping_sub(fs_read) % sizes::QUEUE_CAP
                        >= sizes::QUEUE_CAP - 5
                {
                    self.engine.slot_wait = true;
                    return Err(EAGAIN);
                }
            } else {
                // Room for an optional cross-engine barrier plus compute work entry.
                c_read = self.graph.compute.qstate.r32(qstate::READ)?;
                if c_read >= 256
                    || prefix.c_head.wrapping_sub(c_read) % sizes::QUEUE_CAP >= sizes::QUEUE_CAP - 3
                {
                    self.engine.slot_wait = true;
                    return Err(EAGAIN);
                }
            }
        }
        f.first = index as usize;
        f.c_start = prefix.c_head;
        f.ta_start = prefix.ta_head;
        f.fs_start = prefix.fs_head;
        let mut end = index + 1;
        let mut hz_stop: Option<usize> = None;
        match cmd {
            Command::Compute(c) => {
                self.prefix_compute(job, &job.resources, index, c, &mut prefix, f, false)?;
                let mut last_seq = job.command_order(index).map(|o| o.sequence);
                let vm_units = job.resources.compute_units()?.min(sizes::COMPUTE_UNITS);
                let free_c = self.slots.free_compute_in(vm_units) as usize;
                let active_computes: usize =
                    self.engine.flights.iter().map(|fl| fl.computes as usize).sum();
                let used = prefix.c_head.wrapping_sub(c_read) % sizes::QUEUE_CAP;
                let ring_more = (sizes::QUEUE_CAP.saturating_sub(3)).saturating_sub(used) as usize;
                let mut more_slots = free_c
                    .min(params::compute_flights().saturating_sub(active_computes + 1))
                    .min(sizes::COMPUTE_SLOTS.saturating_sub(1))
                    .min(ring_more);
                // Fold trailing barrier-free compute commands of the base job first.
                while more_slots > 0 && end < job.progress.count() {
                    let Some(next_seq) = last_seq.and_then(|s| s.checked_add(1)) else {
                        break;
                    };
                    if !job.can_fold_horizon_cmd(end, next_seq)
                        || super::recovery::logic::next_ordinal(prefix.n).is_none()
                    {
                        break;
                    }
                    let Some(Command::Compute(c2)) = command(job, end) else {
                        break;
                    };
                    self.prefix_compute(job, &job.resources, end, c2, &mut prefix, f, false)?;
                    last_seq = Some(next_seq);
                    end += 1;
                    more_slots -= 1;
                }
                // Compute horizon: fold subsequent ready, barrier-free compute-only jobs of
                // the same queue and VM into the same CDM producer advance and doorbell.
                // Any non-qualifying job of this queue ends the scan so no later job overtakes it.
                if end == job.progress.count() {
                    hz_stop = Some(0);
                    let mut idx = 0;
                    while idx < self.engine.pending.len() {
                        let j2 = self.engine.pending[idx].clone();
                        idx += 1;
                        if Arc::ptr_eq(&j2, job) || !Arc::ptr_eq(&j2.queue, &job.queue) {
                            continue;
                        }
                        if 1 + f.group.len() >= sizes::HORIZON_JOBS as usize {
                            hz_stop = Some(1);
                            break;
                        }
                        let n2 = j2.progress.count();
                        let Some(first_seq) = last_seq.and_then(|s| s.checked_add(1)) else {
                            hz_stop = Some(3);
                            break;
                        };
                        if !Arc::ptr_eq(&j2.vm, &job.vm)
                            || j2.progress.sent() != 0
                            || n2 == 0
                            || !j2.can_fold_horizon_job(first_seq)
                        {
                            hz_stop = Some(3);
                            break;
                        }
                        let n2_usize = n2 as usize;
                        if n2_usize > more_slots {
                            hz_stop = Some(2);
                            break;
                        }
                        let mut test_n = prefix.n;
                        let mut ord_ok = true;
                        for _ in 0..n2 {
                            match super::recovery::logic::next_ordinal(test_n) {
                                Some(next) => test_n = next,
                                None => {
                                    ord_ok = false;
                                    break;
                                }
                            }
                        }
                        if !ord_ok {
                            hz_stop = Some(2);
                            break;
                        }
                        if f.group.reserve(1, GFP_NOWAIT).is_err() {
                            hz_stop = Some(1);
                            break;
                        }
                        for k in 0..n2 {
                            match command(&j2, k).ok_or(EINVAL)? {
                                Command::Compute(c2) => {
                                    self.prefix_compute(
                                        &j2,
                                        &job.resources,
                                        k,
                                        c2,
                                        &mut prefix,
                                        f,
                                        false,
                                    )?;
                                }
                                Command::Render(_) => return Err(EINVAL),
                            }
                        }
                        last_seq = Some(first_seq + u64::from(n2 - 1));
                        more_slots -= n2_usize;
                        let _ = f.group.push_within_capacity(j2);
                    }
                }
            }
            Command::Render(r) => {
                let geometry = Geometry::new(&r.cmd, 0);
                let n = job.resources.render_passes()?;
                if (0..n).all(|k| {
                    job.resources.render_pass(k).is_err_and(|_| true)
                        || job.resources.render_pass(k).is_ok_and(|slot| {
                            geometry
                                .validate_capacity(slot.tpc.size as u64, slot.tilemap.size as u64)
                                .is_err()
                        })
                }) {
                    return Err(E2BIG);
                }
                let Some(k) = self.slots.pick_pass_in(&job.resources, &geometry, 0) else {
                    self.engine.slot_wait = true;
                    return Err(EAGAIN);
                };
                f.claims.pass[0] = k as u32;
                f.claims.pass_count = 1;
                f.claims.pass_mask = 1u64 << k;
                self.slots.pass_busy |= 1u64 << k;
                self.prefix_render(job, r, geometry, &mut prefix, f, behind, class_behind)?;
                // Batch consecutive ready non-InitBM render passes of the same SUBMIT into
                // one TA/FS producer advance, one UMA submitted update, and one SubmitRecord.
                if !f.initbm {
                    let mut last_seq = job.command_order(index).map(|o| o.sequence);
                    let active_renders: usize = self
                        .engine
                        .flights
                        .iter()
                        .map(|fl| fl.renders as usize)
                        .sum();
                    let mut more_renders = params::render_flights()
                        .saturating_sub(active_renders + 1)
                        .min(sizes::RENDER_SLOTS.saturating_sub(1));
                    while more_renders > 0 && end < job.progress.count() {
                        let Some(next_seq) = last_seq.and_then(|s| s.checked_add(1)) else {
                            break;
                        };
                        if !job.can_fold_render_cmd(end, next_seq)
                            || super::recovery::logic::next_ordinal(prefix.draw).is_none()
                        {
                            break;
                        }
                        let Some(Command::Render(r2)) = command(job, end) else {
                            break;
                        };
                        let Some(o2) = job.command_order(end) else {
                            break;
                        };
                        let ta_used = prefix.ta_head.wrapping_sub(ta_read) % sizes::QUEUE_CAP;
                        let fs_used = prefix.fs_head.wrapping_sub(fs_read) % sizes::QUEUE_CAP;
                        if ta_used >= sizes::QUEUE_CAP - 5 || fs_used >= sizes::QUEUE_CAP - 5 {
                            break;
                        }
                        let geo2 = Geometry::new(&r2.cmd, 0);
                        let Some(k2) = self.slots.pick_pass_in(
                            &job.resources,
                            &geo2,
                            f.claims.pass_mask,
                        ) else {
                            break;
                        };
                        let staged2 = r2.cmd.flags
                            & kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES
                            != 0;
                        if self
                            .wait_stamp_for_with(
                                job,
                                super::command::COMPUTE,
                                o2.ta_wait_through[super::command::COMPUTE],
                                None,
                            )
                            .is_err()
                            || self
                                .wait_stamp_for_with(
                                    job,
                                    super::command::COMPUTE,
                                    o2.wait_through[super::command::COMPUTE],
                                    None,
                                )
                                .is_err()
                            || self
                                .wait_stamp_for_with(
                                    job,
                                    super::command::RENDER,
                                    o2.wait_through[super::command::RENDER],
                                    Some(f),
                                )
                                .is_err()
                            || (staged2
                                && params::early_tiling()
                                && self
                                    .wait_stamp_for_with(
                                        job,
                                        super::command::RENDER,
                                        o2.ta_wait_through[super::command::RENDER],
                                        Some(f),
                                    )
                                    .is_err())
                        {
                            break;
                        }
                        let member = prefix.renders as usize;
                        f.claims.pass[member] = k2 as u32;
                        f.claims.pass_count += 1;
                        f.claims.pass_mask |= 1u64 << k2;
                        self.slots.pass_busy |= 1u64 << k2;
                        self.prefix_render(
                            job,
                            r2,
                            geo2,
                            &mut prefix,
                            f,
                            behind,
                            class_behind,
                        )?;
                        last_seq = Some(next_seq);
                        end += 1;
                        more_renders -= 1;
                    }
                }
            }
        }
        f.first = index as usize;
        f.count = (end - index) as usize;
        f.computes = prefix.computes;
        f.renders = prefix.renders;
        f.target = prefix.last_ct;
        f.ta_target = if render { prefix.last_ta } else { 0 };
        f.fs_target = prefix.last_fs;
        f.head = prefix.c_head;
        f.ta_head = prefix.ta_head;
        f.fs_head = prefix.fs_head;
        if self.recovery_pending()? {
            return Err(EAGAIN);
        }
        if render {
            f.pb.as_ref()
                .ok_or(EINVAL)?
                .check_assignment(f.pb_slot as usize)?;
            self.pools.hold_alloc(
                f.pb_slot as usize,
                &job.vm,
                job.resources.pb_owner_arena(f.pb_slot as usize),
            )?;
        }
        // Reserve lifetime ownership before any counter or producer becomes reachable.
        job.vm.vm.mark_binding_published()?;
        self.slots.verify_held(&f.claims)?;
        self.slots.publish(&mut f.claims)?;
        // Even a failed first store may have become visible. From this boundary preserve
        // every owner and return explicit uncertainty rather than guessing from errno.
        *visible = true;
        let publication = (|| -> Result {
            if render {
                let a = f.pb_slot as usize;
                f.pb.as_ref().ok_or(EINVAL)?.expose();
                job.resources.commit_render(f.renders);
                job.resources
                    .publish_pb_batch(a, f.uma_submitted, f.renders)?;
                self.pools.publish_flight(a);
                // Notifier service dereferences u32 counters; the host ordering ledger
                // remains u64. Eight-byte spacing is retained allocation padding.
                self.graph.render.threshold.w32(0, prefix.draw as u32)?;
                self.graph.render.threshold.w32(8, prefix.draw as u32)?;
            } else {
                for m in 0..f.claims.slot_count as usize {
                    let k = f.claims.slot[m] as usize;
                    job.resources
                        .compute_unit(k)?
                        .counter
                        .w64(0, f.csubmitted[m])?;
                }
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
            // Every command posts its own record: the firmware does not pick up a WRITE
            // advance without one, also while the queue is running.
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
                    new_queue: u8::from(fresh),
                };
                if class_behind {
                    self.channels.post_ahead(channel, &record)?;
                } else {
                    self.channels.post(channel, &record)?;
                }
                *pending_rings |= 1u8 << (channel as u8);
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
        f.behind = behind;
        self.stat_publish(f);
        if !class_behind {
            // First in-flight command on this class starts about now; commands queued behind
            // their own class keep that class's oldest deadline without renewing the other.
            if render {
                self.renew_render_deadline();
            } else {
                self.renew_compute_deadline();
            }
        }
        job.progress.set_sent(end);
        for k in index..end {
            job.command_published(k);
        }
        for j2 in f.group.iter() {
            let n2 = j2.progress.count();
            j2.progress.set_sent(n2);
            for k in 0..n2 {
                j2.command_published(k);
            }
            self.engine.turn += 1;
            j2.queue.turn.store(self.engine.turn, Ordering::Relaxed);
            j2.progress.set_pending(false);
            self.engine.pending.retain(|p| !Arc::ptr_eq(p, j2));
        }
        if !render {
            let s = &mut self.engine.stats;
            let jobs = 1 + f.group.len();
            let bucket = (jobs - 1).min(s.hz.len() - 1);
            s.hz[bucket] += 1;
            if let Some(stop) = hz_stop {
                s.hz_stop[stop] += 1;
            }
            if class_behind {
                s.hz_behind += 1;
            }
            s.grouped = s.grouped.saturating_add(f.group.len() as u64);
        }
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
        let s = &mut self.engine.stats;
        s.max_flights = s.max_flights.max(self.engine.flights.len() as u64);
        publication.map_err(|_| EIO)
    }

    /// Encodes one command into its retained compute unit and appends it to the CDM ring.
    #[expect(
        clippy::too_many_arguments,
        reason = "Encodes one compute member into a shared flight and prefix state."
    )]
    fn prefix_compute(
        &mut self,
        job: &Arc<Job>,
        resources: &super::resources::Prepared,
        cmd_index: u32,
        cmd: &ComputeCommand,
        pf: &mut Prefix,
        f: &mut Flight,
        _later_render: bool,
    ) -> Result {
        let member = pf.computes as usize;
        if member >= sizes::COMPUTE_SLOTS {
            return Err(EAGAIN);
        }
        let n = super::recovery::logic::next_ordinal(pf.n).ok_or(EOVERFLOW)?;
        let target = encode::stamp(n);
        let index = self
            .slots
            .claim_compute_for(resources, &mut f.claims, Some(job.vm.id))?
            .ok_or(EAGAIN)?;
        let generation = job.vm.generation;
        self.slots.hold_compute_slot(index, &job.vm, resources)?;
        let s = resources.compute_unit(index)?;
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
            // Exact embedded sequence: every self-reference then matches the qualified layout.
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
            // Each unit owns its UMA slot: the firmware starts a queued command while the
            // one before it still runs, and two owners of one slot then corrupt each other.
            uma_slot: engine_ids::COMPUTE_UMA_SLOT + index as u32,
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
        encode::encode_compute_for(
            self.init.abi.compute(),
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
        let full_work = self.slots.compute_needs_full_work(index) || before == 0;
        if full_work {
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
                    self.sync_quarantine_hints();
                    return Err(EIO);
                }
            }
            s.work.write(0, w)?;
        } else {
            // Clean same-VM reuse: DESCRIPTOR (0xe00..0xea0) was already authenticated on
            // retirement for this exact unit and VM, and static NOTIFIER (0xc00..0xd18) and
            // tail (0xee0..0xf08) remain intact in place. Skip the 160-byte WC read-back and
            // write only the active head and encoded body through the microsequence end.
            let body_end = work::compute::SEQ_EMBEDDED + work::compute::SEQ_SIZE;
            s.work.write(0, &w[..work::compute::HEAD_ACTIVE_SIZE])?;
            s.work.write(
                work::compute::BODY_ACTIVE_START,
                &w[work::compute::BODY_ACTIVE_START..body_end],
            )?;
        }
        let wait_render = self.cross_engine_wait(job, cmd_index, super::command::RENDER)?;
        let fs2c_bar = if let Some(wait_fs) = wait_render {
            // Check producer InitBM explicitly in addition to the scheduler drain guard so
            // FS -> CDM handoff: downgrade to `BarrierType::Stamp` (type 0) only when neither
            // the waited render nor any live flight carries `InitBM`; G16 FS `FINALIZE` flushes
            // caches before advancing `fw_stamp[1]` (see `cross_engine_barrier_type`).
            let producer_initbm = wait_fs.initbm || self.engine.flights.iter().any(|fl| fl.initbm);
            let bar_type = cross_engine_barrier_type(producer_initbm, false);
            let bar = encode::barrier(
                self.graph.render.fw_stamp[1].va,
                wait_fs.stamp,
                0x71,
                target,
                0,
                bar_type,
            );
            s.barrier.write(0, &bar)?;
            Some((wait_fs.stamp, bar_type))
        } else {
            None
        };
        f.uma_submitted = submitted;
        f.csubmitted[member] = submitted;
        f.has_timestamps |= !cmd.timestamp_outputs.is_empty();
        wmb();
        if let Some((wait_fs, bar_type)) = fs2c_bar {
            g.ring.w64(8 * pf.c_head as usize, s.barrier.va)?;
            pf.c_head = (pf.c_head + 1) % sizes::QUEUE_CAP;
            if bar_type == BarrierType::Stamp {
                self.engine.stats.fs2c0 += 1;
            }
            f.cwait[member] = wait_fs;
        } else {
            f.cwait[member] = 0;
        }
        g.ring.w64(8 * pf.c_head as usize, s.work.va)?;
        pf.c_head = (pf.c_head + 1) % sizes::QUEUE_CAP;
        f.cstamp[member] = target;
        pf.computes += 1;
        pf.n = n;
        pf.last_ct = target;
        pf.prev = Prev::Compute;
        Ok(())
    }

    /// Encodes paired TA/FS work and the fragment's explicit TA dependency in one retained
    /// pass unit.
    fn prefix_render(
        &mut self,
        job: &Arc<Job>,
        cmd: &RenderCommand,
        mut geometry: Geometry,
        pf: &mut Prefix,
        f: &mut Flight,
        behind: bool,
        class_behind: bool,
    ) -> Result {
        let member = pf.renders as usize;
        if member >= sizes::RENDER_SLOTS {
            return Err(EAGAIN);
        }
        let cmd_index = (f.first + member) as u32;
        let k = f.claims.pass[member] as usize;
        let draw = super::recovery::logic::next_ordinal(pf.draw).ok_or(EOVERFLOW)?;
        let target = encode::stamp(draw);
        let has_prior_render = class_behind || member > 0;
        let prev_ta = if member > 0 {
            pf.last_ta
        } else {
            self.engine.ta_target
        };
        let prev_fs = if member > 0 {
            pf.last_fs
        } else {
            self.engine.fs_target
        };
        let order = job.command_order(cmd_index);
        let staged = cmd.cmd.flags
            & kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES
            != 0;
        let ta_wait_compute = self.wait_stamp_for_with(
            job,
            super::command::COMPUTE,
            order.and_then(|o| o.ta_wait_through[super::command::COMPUTE]),
            None,
        )?;
        let fs_wait_compute = self.wait_stamp_for_with(
            job,
            super::command::COMPUTE,
            order.and_then(|o| o.wait_through[super::command::COMPUTE]),
            None,
        )?;
        let fs_wait_render = if has_prior_render {
            match order {
                Some(o) => self.wait_stamp_for_with(
                    job,
                    super::command::RENDER,
                    o.wait_through[super::command::RENDER],
                    Some(f),
                )?,
                None => Some(WaitTarget {
                    stamp: prev_fs,
                    initbm: false,
                }),
            }
        } else {
            None
        };
        let ta_wait_fs = if has_prior_render && staged && params::early_tiling() {
            self.wait_stamp_for_with(
                job,
                super::command::RENDER,
                order.and_then(|o| o.ta_wait_through[super::command::RENDER]),
                Some(f),
            )?
        } else {
            None
        };
        let plan = plan_render_barriers(
            cmd.cmd.flags,
            has_prior_render,
            params::early_tiling(),
            prev_fs,
            fs_wait_render.map(|w| w.stamp),
            ta_wait_fs.map(|w| w.stamp),
            [
                ta_wait_compute.map(|w| w.stamp),
                fs_wait_compute.map(|w| w.stamp),
            ],
        );
        // Claim performs only a qualified per-use scrub, never allocator/list reset.
        self.slots
            .claim_pass_in(&job.resources, k, Some(job.vm.id), geometry.tilemap_bytes)?;
        self.slots.hold_render_pass(k, &job.vm, &job.resources)?;
        let (refs, init_buffer) = if member == 0 {
            let pb_slots = job.resources.pb_slots()?;
            let alloc_slot = self
                .pools
                .pick_slot_within(&job.vm, behind, pb_slots)
                .ok_or(EAGAIN)?;
            let (refs, init_buffer, epoch, binding) =
                self.render_refs(alloc_slot, job, draw, class_behind, behind)?;
            f.pb = Some(epoch);
            f.pb_slot = alloc_slot as u32;
            f.pb_binding = binding;
            f.initbm = init_buffer;
            (refs, init_buffer)
        } else {
            if f.initbm {
                return Err(EAGAIN);
            }
            let epoch = f.pb.as_ref().ok_or(EINVAL)?;
            let refs = self.render_refs_batched(
                f.pb_slot as usize,
                job,
                draw,
                epoch,
                f.uma_submitted,
            )?;
            (refs, false)
        };
        f.has_timestamps |= !cmd.timestamp_outputs.is_empty();
        let p = job.resources.render_pass(k)?;
        let pass = p.refs();
        let heap_bytes = if cmd.cmd.layers > 1 { 0x1000 } else { 0x200 };
        geometry.layer_metadata = pass.heap_gva + if cmd.cmd.layers > 1 { 0x200 } else { 0 };
        geometry.validate_capacity(p.tpc.size as u64, p.tilemap.size as u64)?;
        job.resources
            .set_pass_dirty(k, geometry.tpc_bytes, geometry.tilemap_bytes, heap_bytes)?;
        let scratch = self.engine.scratch.as_deref_mut().ok_or(ENOMEM)?;
        encode::encode_ta_for(
            self.init.abi.ta(),
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
        encode::encode_fs_for(
            self.init.abi.fs(),
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
        let full_work = self.slots.render_needs_full_work(k);
        if full_work {
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
        } else {
            p.ta_work
                .write(0, &scratch.ta[..work::ta::HEAD_ACTIVE_SIZE])?;
            p.ta_work.write(
                work::ta::TAIL_ACTIVE_START,
                &scratch.ta[work::ta::TAIL_ACTIVE_START..],
            )?;
            p.fs_work
                .write(0, &scratch.fs[..work::fs::HEAD_ACTIVE_SIZE])?;
            p.fs_work.write(
                work::fs::TAIL_ACTIVE_START,
                &scratch.fs[work::fs::TAIL_ACTIVE_START..],
            )?;
        }
        p.ta_seq.write(0, &scratch.tas[..])?;
        p.fs_seq.write(0, &scratch.fss[..])?;
        if full_work {
            p.scene.write(0, &scratch.scene[..])?;
        } else {
            let hdr = encode::scene(&refs, &pass, 0);
            let pfx = encode::scene_prefix(&refs, job.vm.slot, target, init_buffer);
            p.scene.write(0, &hdr)?;
            p.scene.fill(0x80, 0, 0x280)?;
            p.scene.write(0x300, &pfx)?;
            p.scene.w64(0x500, refs.uma_submitted)?;
            // Zero TA/FS internal timestamp cells and targets so recovery FS execution
            // proof and timestamp queries can never observe stale ticks from a prior pass.
            p.scene.fill(0x600, 0, 0x40)?;
        }
        // One retained descriptor is shared by TA and FS; its private submitted counter
        // advances by two per pass. Reused descriptors and their pool/list bytes remain intact.
        f.uma_submitted = refs.uma_submitted;
        let r = &self.graph.render;
        let cap = sizes::QUEUE_CAP;
        if init_buffer {
            pf.initbm = true;
            let bm = encode::initbm(job.vm.slot, refs.parameter_id, refs.pb_mgr, target);
            let epoch = f.pb.as_ref().ok_or(EINVAL)?;
            epoch.initbm.write(0, &bm)?;
            r.ring[0].w64(8 * pf.ta_head as usize, epoch.initbm.va)?;
            pf.ta_head = (pf.ta_head + 1) % cap;
        }
        // Pipelined same-VM render->render and cross-engine (CDM->TA, CDM->FS) barriers use
        // `BarrierType::Stamp` (type 0) only when neither producer, any live flight, nor this
        // consumer carries `InitBM`: G16 `FINALIZE` flushes producer caches before advancing
        // `fw_stamp` (unlike G15's `CROSS_ENGINE_HOLD`; see `cross_engine_barrier_type`).
        // Cross-VM transitions and recovery paths drain completely before publishing.
        let producer_initbm = pf.initbm
            || self.engine.flights.iter().any(|fl| fl.initbm)
            || ta_wait_compute.is_some_and(|w| w.initbm)
            || fs_wait_compute.is_some_and(|w| w.initbm)
            || fs_wait_render.is_some_and(|w| w.initbm)
            || ta_wait_fs.is_some_and(|w| w.initbm);
        let render_bar = cross_engine_barrier_type(producer_initbm, init_buffer);
        // Pipelined renders share one parameter-buffer / UMA epoch (`RENDER_UMA_SLOT`), so keep
        // the TA(N-1)->TA(N) `prev_barrier` (matching G15) whenever `plan.ta_fs_wait` does not
        // already serialize behind `prev_fs`. Emit `ta_fs_barrier` only when `plan.ta_fs_wait`
        // names an in-flight FS stamp, and `fs_dep` only when `plan.fs_render_wait` names an
        // in-flight FS dependency not already dominated by TA.
        if plan.ta_prev_wait {
            let prev = encode::barrier(
                r.fw_stamp[0].va,
                prev_ta,
                0x70,
                target,
                work::barrier::RENDER_WAIT_UUID,
                render_bar,
            );
            p.prev_barrier.write(0, &prev)?;
            r.ring[0].w64(8 * pf.ta_head as usize, p.prev_barrier.va)?;
            pf.ta_head = (pf.ta_head + 1) % cap;
        }
        if let Some(wait_fs) = plan.ta_fs_wait {
            let ta_fs = encode::barrier(
                r.fw_stamp[1].va,
                wait_fs,
                0x71,
                target,
                work::barrier::RENDER_WAIT_UUID,
                render_bar,
            );
            p.ta_fs_barrier.write(0, &ta_fs)?;
            r.ring[0].w64(8 * pf.ta_head as usize, p.ta_fs_barrier.va)?;
            pf.ta_head = (pf.ta_head + 1) % cap;
        }
        if let Some(wait_fs) = plan.fs_render_wait {
            let dep = encode::barrier(
                r.fw_stamp[1].va,
                wait_fs,
                0x71,
                target,
                work::barrier::RENDER_WAIT_UUID,
                render_bar,
            );
            p.fs_dep.write(0, &dep)?;
            r.ring[1].w64(8 * pf.fs_head as usize, p.fs_dep.va)?;
            pf.fs_head = (pf.fs_head + 1) % cap;
        }
        if plan.early {
            self.engine.stats.early += 1;
        }
        if let Some(wait_c) = plan.ta_compute_wait {
            let cbar = encode::barrier(
                self.graph.compute.fw_stamp.va,
                wait_c,
                15,
                target,
                work::barrier::RENDER_WAIT_UUID,
                render_bar,
            );
            p.compute_barrier.write(0, &cbar)?;
            r.ring[0].w64(8 * pf.ta_head as usize, p.compute_barrier.va)?;
            pf.ta_head = (pf.ta_head + 1) % cap;
            if render_bar == BarrierType::Stamp {
                self.engine.stats.c2ta0 += 1;
            }
        }
        if let Some(wait_c) = plan.fs_compute_wait {
            let fcbar = encode::barrier(
                self.graph.compute.fw_stamp.va,
                wait_c,
                15,
                target,
                work::barrier::RENDER_WAIT_UUID,
                render_bar,
            );
            p.fs_compute_barrier.write(0, &fcbar)?;
            r.ring[1].w64(8 * pf.fs_head as usize, p.fs_compute_barrier.va)?;
            pf.fs_head = (pf.fs_head + 1) % cap;
            if render_bar == BarrierType::Stamp {
                self.engine.stats.c2ta0 += 1;
            }
        }
        // The paired fragment always requires the native explicit TA dependency.
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
        f.r_ta_stamp[member] = target;
        f.r_fs_stamp[member] = target;
        pf.renders += 1;
        f.renders = pf.renders;
        pf.draw = draw;
        pf.last_ta = target;
        pf.last_fs = target;
        pf.prev = Prev::Render;
        Ok(())
    }

    /// Records the publication time of `f` and the preparation, gap and starvation times.
    fn stat_publish(&mut self, f: &mut Flight) {
        let s = &mut self.engine.stats;
        s.pub_ns = now_ns();
        f.published_ns = s.pub_ns;
        if f.behind {
            s.behind += 1;
            s.overlaps += 1;
        }
        s.prep_ns += s.pub_ns.wrapping_sub(s.prep_from_ns);
        if s.idle_from_ns != 0 {
            s.gap_ns += s.pub_ns.wrapping_sub(s.idle_from_ns);
            s.gaps += 1;
            s.idle_from_ns = 0;
        } else if s.starve_from_ns != 0 {
            s.starved_ns += s.pub_ns.wrapping_sub(s.starve_from_ns);
        }
        s.starve_from_ns = 0;
        let now = s.pub_ns;
        self.power.utilization.busy(now);
    }
}
