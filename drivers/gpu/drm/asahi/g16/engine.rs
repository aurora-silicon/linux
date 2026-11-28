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
use super::encode::{self, ComputeRefs, ComputeRt, Geometry, RenderRefs, StageRt};
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

/// Whether a compute command of `vm` that runs shader-context scratch (SCS) work (`scs`), or
/// does not, must wait for compute flight `f` to retire before it is published.
///
/// SCS compute (ray queries, and every compute command of a job with SCS scratch) shares the
/// compute engine only with SCS compute of its own VM. Each compute work item sets the
/// engine's SCS state in its register list, enabled for SCS work and disabled otherwise. An
/// SCS item that other compute overlaps is reported complete while its stores are still in
/// flight, and they land after its fence has signalled, also after its VM is gone.
fn scs_compute_conflict(f: &Flight, vm: &Arc<super::vm::VmState>, scs: bool) -> bool {
    f.computes != 0 && (f.rt_compute != scs || (scs && !Arc::ptr_eq(&f.job.vm, vm)))
}

/// Whether a render of `vm`, multisampled when `msaa`, must wait for render flight `f` to
/// retire before it is published.
///
/// Every VM's render passes share the one firmware TA/FS queue pair. While passes of two VMs
/// are both published there and one of the VMs renders multisampled
/// (`VmStateInner::renders_msaa`), that VM's passes can lose coverage (uncovered or zero
/// pixels and samples, also in single-sampled passes that read its multisampled images) and
/// its fragment shaders can read VA 0. Ordering the two passes' TA and FS with ring barriers
/// does not prevent it; publishing one only after the other retired does. Passes of one VM,
/// and passes of VMs that never render multisampled, still share the queues.
fn msaa_render_conflict(f: &Flight, vm: &Arc<super::vm::VmState>, msaa: bool) -> bool {
    f.renders != 0
        && !Arc::ptr_eq(&f.job.vm, vm)
        && (msaa || vm.renders_msaa() || f.job.vm.renders_msaa())
}

/// Resolved ring-barrier placement for one render pass (legacy, `FRAGMENT_BARRIERS`, or
/// `DRM_ASAHI_RENDER_STAGE_PREFIXES`).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
struct RenderBarrierPlan {
    early: bool,
    ta_prev_wait: bool,
    ta_fs_wait: Option<u32>,
    fs_render_wait: Option<u32>,
    ta_compute_wait: [Option<u32>; sizes::COMPUTE_PIPES],
    fs_compute_wait: [Option<u32>; sizes::COMPUTE_PIPES],
}

/// Computes TA/FS hardware ring barriers for a render command from its flags, prior render
/// FS stamp, resolved render waits (`fs_wait_render`, `ta_wait_fs`), resolved `[ta, fs]`
/// per-pipe compute wait stamps, and whether the immediately preceding in-flight render belongs
/// to a different VM (`cross_vm_prev`).
///
/// Same-VM pipelined renders share one parameter-buffer / UMA slot, and cross-VM pipelined
/// renders use distinct UMA slots (`RENDER_UMA_SLOT + slot`) on the shared TA/FS pipes: TA(N)
/// always waits on TA(N-1) (`ta_prev_wait`, matching G15's `prev_barrier`) whenever
/// `ta_fs_wait` does not already wait on `prev_fs` (which dominates `prev_ta` via
/// `own_barrier`). Same-VM independent FS passes omit `fs_dep`, while `cross_vm_prev` forces
/// `fs_render_wait = Some(prev_fs)` (whenever `ta_fs_wait != Some(prev_fs)`) so cross-VM TA
/// overlaps the previous VM's FS without ever overlapping two VMs' FS/PB teardowns.
#[expect(
    clippy::too_many_arguments,
    reason = "Pure barrier-plan helper mirrors the exact render barrier inputs."
)]
fn plan_render_barriers(
    flags: u32,
    class_behind: bool,
    early_tiling: bool,
    prev_fs: u32,
    fs_wait_render: Option<u32>,
    ta_wait_fs: Option<u32>,
    compute_waits: [[Option<u32>; sizes::COMPUTE_PIPES]; 2],
    cross_vm_prev: bool,
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
    let effective_fs_wait = if cross_vm_prev && class_behind {
        Some(prev_fs)
    } else {
        fs_wait_render
    };
    let fs_render_wait = if class_behind
        && (early || (cross_vm_prev && ta_fs_wait != Some(prev_fs)))
        && effective_fs_wait != ta_fs_wait
    {
        effective_fs_wait
    } else {
        None
    };
    let [ta_compute_wait, fs_wait] = compute_waits;
    let mut fs_compute_wait = [None; sizes::COMPUTE_PIPES];
    for p in 0..sizes::COMPUTE_PIPES {
        if fs_wait[p] != ta_compute_wait[p] {
            fs_compute_wait[p] = fs_wait[p];
        }
    }
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
    /// Compute pipe (`0..COMPUTE_PIPES`) this flight was published to.
    pub(crate) compute_pipe: u8,
    /// Compute: final FW stamp, ring end (exclusive) and ring position before the prefix.
    pub(crate) target: u32,
    pub(crate) head: u32,
    pub(crate) c_start: u32,
    /// Render: final TA/FS stamps, ring ends and ring positions.
    pub(crate) ta_target: u32,
    pub(crate) fs_target: u32,
    pub(crate) ta_head: u32,
    pub(crate) fs_head: u32,
    pub(crate) ta_start: u32,
    pub(crate) fs_start: u32,
    /// Channel producers (TA, FS, compute) after the prefix's records.
    pub(crate) rec: [u32; 3],
    /// Whether `Channel::Compute` read and completion cursors already passed `rec[2]` before
    /// the shared submit ring could wrap past this flight.
    pub(crate) chan_settled: bool,
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
    /// Whether this flight runs ray-tracing or SCS-scratch compute (pinned to compute pipe 0).
    pub(crate) rt_compute: bool,
    /// Whether this flight includes any heavy-TA render pass (multi-layer or large tilemap).
    pub(crate) heavy_render: bool,
    /// Whether this flight includes a multisampled render pass; its VM is marked
    /// (`VmStateInner::note_msaa_render`) once the flight becomes visible.
    pub(crate) msaa: bool,
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
            compute_pipe: 0,
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
            chan_settled: false,
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
            rt_compute: false,
            heavy_render: false,
            msaa: false,
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
    compute_pipe: usize,
    prev: Prev,
    /// This prefix (re)initializes the PB with InitBM.
    initbm: bool,
    c_head: u32,
    ta_head: u32,
    fs_head: u32,
    /// Last total compute ordinal, per-pipe compute ordinal, and draw ordinal.
    n: u64,
    pipe_n: u64,
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

/// Why a publication pass stopped at a ready job without publishing it (`Stats::hold_causes`).
///
/// A drain cause means the job waits for an empty engine; the others wait behind in-flight
/// work for headroom or ordering.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub(crate) enum HoldCause {
    /// The job's VM failed, or a flight is doomed or suspect in a recovery.
    DrainFailed,
    /// A cold `InitBM` render is in flight.
    DrainInitbm,
    /// Compute behind render of another VM, render behind compute of another VM, an SCS
    /// compute conflict, or a render beside another VM's render while one of the two VMs
    /// renders multisampled.
    DrainCross,
    /// No warm PB/UMA allocator slot of the VM can take the render behind active flights.
    DrainPb,
    /// The TA wave window is full.
    TaWave,
    /// Flight, render pass or compute unit headroom.
    Slot,
    /// The job's dependency check failed: it fails once it reaches publication.
    /// (`Engine::pick()` skips jobs still waiting for a dependency.)
    Dependency,
    /// Any other admission refusal behind in-flight work.
    Other,
}

impl HoldCause {
    pub(crate) const COUNT: usize = 8;
    pub(crate) const NAMES: [&'static str; Self::COUNT] = [
        "drain_failed",
        "drain_initbm",
        "drain_cross",
        "drain_pb",
        "ta_wave",
        "slot",
        "dependency",
        "other",
    ];
}

/// Engine statistics, printed by `aurora-g16/stats`.
#[derive(Default, Zeroable)]
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
    /// `hold[0]` split by [`HoldCause`].
    pub(crate) hold_causes: [u64; HoldCause::COUNT],
    /// Compute commands published behind in-flight ones.
    pub(crate) behind: u64,
    /// Most flights in flight at once.
    pub(crate) max_flights: u64,
}

/// Host-side phase timers of publication and polling, on with `debug_flags` bit 49
/// (`Debug1`); printed as the `prof:` line of `aurora-g16/stats`. A measurement aid: nothing is
/// timed while the flag is off.
#[derive(Default, Zeroable)]
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
    #[inline(never)]
    fn new_boxed() -> Result<KBox<Self>> {
        let z = |n| KVec::from_elem(0u8, n, GFP_KERNEL);
        KBox::try_init(
            try_init!(Self {
                cw: z(work::compute::WORK_SIZE)?,
                cq: z(work::compute::SEQ_SIZE)?,
                ta: z(work::ta::WORK_SIZE)?,
                tas: z(work::ta::SEQ_SIZE)?,
                fs: z(work::fs::WORK_SIZE)?,
                fss: z(work::fs::SEQ_SIZE)?,
                scene: z(work::scene::ACTIVE_SIZE)?,
            }),
            GFP_KERNEL,
        )
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
    /// Host compute ordinal, per-pipe compute ordinals, and draw ordinal of the last publication.
    pub(crate) ordinal: u64,
    pub(crate) pipe_ordinal: [u64; sizes::COMPUTE_PIPES],
    pub(crate) draw: u64,
    /// Newest published receipts, the tail.
    pub(crate) target: [u32; sizes::COMPUTE_PIPES],
    pub(crate) head: [u32; sizes::COMPUTE_PIPES],
    pub(crate) ta_target: u32,
    pub(crate) fs_target: u32,
    pub(crate) ta_head: u32,
    pub(crate) fs_head: u32,
    /// Event counts at the tail's publication.
    pub(crate) events: u32,
    pub(crate) stage_seen: u32,
    /// The compute pipes / render queues were registered by a first record (`new_queue`).
    pub(crate) queue_registered: [bool; sizes::COMPUTE_PIPES],
    pub(crate) next_compute_pipe: u8,
    /// Last VM id that published onto each compute pipe (warm per-VM `qinfo` affinity).
    pub(crate) pipe_last_vm: [Option<u64>; sizes::COMPUTE_PIPES],
    /// Monotonic last-use counter per compute pipe for LRU idle-pipe selection.
    pub(crate) pipe_last_used: [u64; sizes::COMPUTE_PIPES],
    pub(crate) pipe_use_seq: u64,
    /// Transient bitmask of compute pipes being drained for handoff to a waiting job that has
    /// no eligible pipe (e.g. a base-only VM limited to pipe 0).
    pub(crate) handoff_avoid_pipes: u32,
    pub(crate) render_registered: bool,
    pub(crate) ta_registered: bool,
    /// A ready prefix waits only for a record's consumption: repoll sooner.
    pub(crate) chan_wait: bool,
    /// A ready same-VM pending job is blocked solely on flight, pass/compute slot, or work-ring
    /// headroom (not cross-VM/InitBM drain, external fences, or CPU timestamp waits): repoll
    /// sooner to refill freed slots promptly.
    pub(crate) slot_wait: bool,
    /// A ready same-VM render job is paused by `TA_WAVE_PASSES` pacing until TA advances.
    pub(crate) ta_wave_wait: bool,
    /// An EP 0x20 event arrived with a flight still settling its final receipt: repoll sooner.
    pub(crate) short_repoll: bool,
    /// An idle poll pass inside the housekeeping cadence skipped housekeeping: queue a backup
    /// poll so it runs once the cadence elapses.
    pub(crate) housekeeping_owed: bool,
    /// A ready job is held behind flights that all belong to its own VM, for any cause but a
    /// failure, a cross-VM overlap or a failed dependency: one of those flights retiring is
    /// what releases it, so repoll soon instead of after `resnap_us`.
    pub(crate) held_wait: bool,
    /// Absolute deadline (`ktime_get_ns`) for busy-state same-queue horizon micro-coalescing
    /// (0 when no batch is held).
    pub(crate) coalesce_deadline_ns: u64,
    /// Progress tracking of the oldest compute pipes `0..COMPUTE_PIPES` and render flights.
    pub(crate) progress_flight: [u64; sizes::COMPUTE_PIPES + 1],
    pub(crate) progress: [Option<u32>; sizes::COMPUTE_PIPES + 2],
    /// Boxed with [`Self::prof`]: `Firmware` is built on the probe's stack.
    pub(crate) stats: KBox<Stats>,
    /// Phase timers (debug_flags bit 49).
    pub(crate) prof: KBox<Prof>,
    /// The `aurora-g16/stats` file.
    pub(crate) debugfs: Option<Pin<KBox<kernel::debugfs::File<Arc<Shared>>>>>,
    scratch: Option<KBox<Scratch>>,
}

impl Engine {
    #[inline(never)]
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
            pipe_ordinal: [0; sizes::COMPUTE_PIPES],
            draw: 0,
            target: [0; sizes::COMPUTE_PIPES],
            head: [0; sizes::COMPUTE_PIPES],
            ta_target: 0,
            fs_target: 0,
            ta_head: 0,
            fs_head: 0,
            events: 0,
            stage_seen: 0,
            queue_registered: [false; sizes::COMPUTE_PIPES],
            next_compute_pipe: 0,
            pipe_last_vm: [None; sizes::COMPUTE_PIPES],
            pipe_last_used: [0; sizes::COMPUTE_PIPES],
            pipe_use_seq: 0,
            handoff_avoid_pipes: 0,
            render_registered: false,
            ta_registered: false,
            chan_wait: false,
            slot_wait: false,
            ta_wave_wait: false,
            short_repoll: false,
            housekeeping_owed: false,
            held_wait: false,
            coalesce_deadline_ns: 0,
            progress_flight: [0; sizes::COMPUTE_PIPES + 1],
            progress: [None; sizes::COMPUTE_PIPES + 2],
            stats: KBox::zeroed(GFP_KERNEL)?,
            prof: KBox::zeroed(GFP_KERNEL)?,
            debugfs: None,
            // Allocate host encoder buffers before any submitted fence can depend on
            // the device mutex. Publication must never enter direct reclaim here.
            scratch: Some(Scratch::new_boxed()?),
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
    pub(crate) fn recovery_snapshot(&self) -> Result<[u32; 2 + sizes::COMPUTE_PIPES]> {
        let mut stage = [0u32; 2 + sizes::COMPUTE_PIPES];
        stage[0] = self.graph.render.fw_stamp[0].r32(0)?;
        stage[1] = self.graph.render.fw_stamp[1].r32(0)?;
        for (p, compute) in self.graph.compute.iter().enumerate() {
            stage[2 + p] = compute.fw_stamp.r32(0)?;
        }
        Ok(stage)
    }

    /// Authenticate one firmware affected-work snapshot entry against its retained owner.
    /// The pre-acknowledgement TPC cleanup is required only for an unfinished TA.
    pub(crate) fn recovery_prepare(
        &mut self,
        flags: u32,
        event: u32,
        target: u32,
        stage: [u32; 2 + sizes::COMPUTE_PIPES],
    ) -> Result {
        if flags & 1 != 0 && self.recovery.last_retired.contains(event, target) {
            // A notification may follow the full receipt. It names the retained old
            // owner, not the next command that happens to occupy the flight list.
            return Ok(());
        }
        // The named owner: the oldest flight, or with compute/render queued behind it a later one.
        let compute_pipe = work::compute::pipe_of_event(event);
        let f = self
            .engine
            .flights
            .iter_mut()
            .find(|f| match event {
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
                _ => compute_pipe.is_some_and(|pipe| {
                    f.computes != 0
                        && f.compute_pipe as usize == pipe
                        && f.cstamp[..(f.computes as usize).min(f.cstamp.len())].contains(&target)
                }),
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
        stage: [u32; 2 + sizes::COMPUTE_PIPES],
        _event: u32,
        fault_asid: Option<u32>,
    ) -> Result {
        let reached = super::recovery::logic::stamp_reached;
        let (target, ta_target, fs_target) = (
            self.engine.target,
            self.engine.ta_target,
            self.engine.fs_target,
        );
        // Every unfinished flight, including compute queued behind the oldest on either pipe.
        for f in self.engine.flights.iter_mut() {
            let pipe = (f.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
            let unfinished = (f.computes != 0 && !reached(stage[2 + pipe], f.target, target[pipe]))
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
        job.origin.mark_enqueued();
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
        self.engine.ta_wave_wait = false;
        self.engine.held_wait = false;
        self.engine.handoff_avoid_pipes = 0;
        self.engine.coalesce_deadline_ns = 0;
        while self.recovery.failed().is_none()
            && !self.engine.frozen
            && !self.engine.pending.is_empty()
        {
            self.engine.handoff_avoid_pipes = 0;
            let Some(index) = self.engine.pick() else {
                break;
            };
            let mut job = self.engine.pending[index].clone();
            // Even a failed job may have an earlier command in flight. An aggregate
            // error fence must not race that command's access to user buffers: healthy
            // non-InitBM commands (compute or warm-PB render of any VM) queue behind
            // active flights; when the head queue cannot queue behind (e.g. its class
            // hit its flight cap), still allow another ready queue to run concurrently
            // while preserving per-queue FIFO, unless the head job itself requires
            // `flights` to drain to empty first (unprimed cold InitBM render, failed
            // VM, or recovery/InitBM in flight).
            let mut ta_pending: Option<usize> = None;
            if !self.engine.flights.is_empty()
                && !self.can_queue_behind_with_ta(&job, &mut ta_pending)
            {
                if self.needs_engine_drain(&job) {
                    self.note_hold(&job, true, 0);
                    break;
                }
                // If the selected head waiter has no eligible compute pipe (e.g. a base-only
                // VM limited to pipe 0 while an incumbent refills pipe 0, or a 9th VM when all
                // pipes are occupied), let one compatible pipe drain for handoff while other
                // pipes continue dispatching.
                self.engine.handoff_avoid_pipes = self
                    .compute_handoff_pipe(&job)
                    .map_or(0, |pipe| 1u32 << pipe);
                let ta_val = *ta_pending.get_or_insert_with(|| self.ta_pending_passes());
                let ta_wave_paused = ta_val >= self.active_ta_wave_cap();
                let mut alt: Option<usize> = None;
                for (i, cand) in self.engine.pending.iter().enumerate() {
                    if i == index {
                        continue;
                    }
                    // Fast O(1) skip for blocked head-queue followers: `Engine::pick` chose the
                    // earliest pending job of `job.queue`, so later jobs of the same queue are
                    // FIFO-blocked unless an independent non-query compute may bypass TA-wave
                    // paused renders.
                    if i > index
                        && Arc::ptr_eq(&cand.queue, &job.queue)
                        && !(ta_wave_paused
                            && matches!(
                                command(cand, cand.progress.sent()),
                                Some(Command::Compute(c)) if !c.query_access
                            ))
                    {
                        continue;
                    }
                    if !self.can_queue_behind_with_ta(cand, &mut ta_pending) {
                        continue;
                    }
                    // Preserve per-queue FIFO: never jump a later job of a queue ahead
                    // of an earlier pending job of the same queue, except when TA wave
                    // pacing pauses earlier ready renders and an independent non-query
                    // compute (such as pre-gfx culling) has all dependencies published
                    // (`can_queue_behind` already checked `check_command_dependencies`).
                    let same_queue_blocked = self.engine.pending[..i].iter().any(|earlier| {
                        if !Arc::ptr_eq(&earlier.queue, &cand.queue) {
                            return false;
                        }
                        let allow_pre_gfx = ta_wave_paused
                            && matches!(
                                command(cand, cand.progress.sent()),
                                Some(Command::Compute(c)) if !c.query_access
                            )
                            && matches!(
                                command(earlier, earlier.progress.sent()),
                                Some(Command::Render(r))
                                    if ta_val >= self.ta_wave_cap_for(r.is_heavy_ta())
                            )
                            && !self.needs_engine_drain(earlier)
                            && earlier
                                .check_command_dependencies(earlier.progress.sent())
                                .is_ok();
                        !allow_pre_gfx
                    });
                    if same_queue_blocked {
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
                        self.engine.handoff_avoid_pipes = 0;
                        self.engine.slot_wait = self
                            .engine
                            .pending
                            .iter()
                            .any(|cand| self.blocked_on_slot_headroom(cand));
                        self.engine.ta_wave_wait = self.engine.pending.iter().any(|cand| {
                            matches!(
                                command(cand, cand.progress.sent()),
                                Some(Command::Render(r))
                                    if ta_val >= self.ta_wave_cap_for(r.is_heavy_ta())
                            ) && !self.needs_engine_drain(cand)
                                && cand
                                    .check_command_dependencies(cand.progress.sent())
                                    .is_ok()
                        });
                        self.note_hold(&job, false, ta_val);
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
                    .compute_slot_mask()
                    .is_ok_and(|mask| self.slots.compute_exhausted_in_mask(mask))
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
                && (job.resources.pb_slot_mask().is_ok_and(|(mask, home)| {
                    self.pools.exhausted_for_mask_mode(
                        &job.vm,
                        mask,
                        home,
                        job.resources.render_scs(),
                    )
                }) || job
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
                Err(e) if e == EAGAIN => {
                    self.engine.handoff_avoid_pipes = 0;
                    break;
                }
                Err(e) => {
                    self.engine.unpend(&job);
                    let _ = job.mark_terminal_retired();
                    complete(done, &job, Err(e));
                    continue;
                }
            }
            let pub_res = self.publish_prefix(&job, done, &mut pending_rings);
            self.engine.handoff_avoid_pipes = 0;
            match pub_res {
                Ok(()) => {
                    self.engine.turn += 1;
                    job.queue.turn.store(self.engine.turn, Ordering::Relaxed);
                    batch_published += 1;
                    self.recovery.publication_wait_deadline = None;
                    if job.progress.sent() == job.progress.count() {
                        self.engine.unpend(&job); // fully published
                    }
                    // Ring TA/FS immediately once a render wave fills the active TA wave window so
                    // the GPU starts tiling right away instead of waiting behind another loop
                    // iteration or an alt-class compute publication.
                    if pending_rings & ((1u8 << (Channel::Fs as u8)) | (1u8 << (Channel::Ta as u8)))
                        != 0
                        && self.ta_pending_passes() >= self.active_ta_wave_cap()
                    {
                        if let Err(e) = self.flush_doorbells(&mut pending_rings) {
                            dev_err!(
                                self.graph.dev.as_ref(),
                                "G16: render wave doorbell ring failed after visibility began ({:?})\n",
                                e
                            );
                            self.fail_stop(EIO, done);
                            result = Err(EIO);
                            break;
                        }
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

    /// Rings each submit channel touched during a publication batch once, waking Compute
    /// first so pre-gfx compute starts immediately ahead of TA barriers while preserving
    /// FS-before-TA doorbell order for render batches.
    fn flush_doorbells(&mut self, pending_rings: &mut u8) -> Result {
        let mask = core::mem::take(pending_rings);
        for channel in [Channel::Compute, Channel::Fs, Channel::Ta] {
            if mask & (1u8 << (channel as u8)) != 0 {
                self.session.ring(channel)?;
            }
        }
        Ok(())
    }

    /// Whether render `r` of `vm` may be published beside the flights in flight
    /// ([`msaa_render_conflict`]). Every member of a prefix is checked, not only its first.
    fn msaa_member_allowed(&self, vm: &Arc<super::vm::VmState>, r: &RenderCommand) -> bool {
        !self
            .engine
            .flights
            .iter()
            .any(|f| msaa_render_conflict(f, vm, r.is_multisampled()))
    }

    /// Whether `job` requires `self.engine.flights` to drain to empty before it can
    /// publish (so `alt`-class bypass must not keep refilling the engine ahead of it).
    fn needs_engine_drain(&self, job: &Arc<Job>) -> bool {
        let cmd = command(job, job.progress.sent());
        let is_render = matches!(cmd, Some(Command::Render(_)));
        let is_compute = matches!(cmd, Some(Command::Compute(_)));
        let is_rt_compute = is_compute
            && (job.resources.compute_scs()
                || matches!(cmd, Some(Command::Compute(c)) if c.is_rt()));
        let is_msaa_render = matches!(cmd, Some(Command::Render(r)) if r.is_multisampled());
        job.vm.status.get() != 0
            || self.engine.flights.iter().any(|f| {
                f.doomed
                    || f.recovery_suspect
                    || f.initbm
                    || (is_compute && scs_compute_conflict(f, &job.vm, is_rt_compute))
                    || (is_render && msaa_render_conflict(f, &job.vm, is_msaa_render))
                    || (((is_compute && f.renders != 0) || (is_render && f.computes != 0))
                        && !Arc::ptr_eq(&f.job.vm, &job.vm))
            })
            || (is_render && !self.render_can_pipeline(job))
    }

    /// Whether `job`'s render can reuse a warm PB/UMA allocator slot of its VM behind active
    /// flights, without the drain that a fresh `InitBM` epoch needs.
    fn render_can_pipeline(&self, job: &Arc<Job>) -> bool {
        job.resources
            .pb_slot_mask()
            .and_then(|(mask, home)| Ok((mask, home, job.resources.pb_heap_blocks()?)))
            .is_ok_and(|(mask, home, blocks)| {
                self.pools.can_pipeline_in_mask_blocks_mode(
                    &job.vm,
                    mask,
                    home,
                    blocks,
                    job.resources.render_scs(),
                )
            })
    }

    /// Why a publication pass stopped at `job`: `drain` when [`Self::needs_engine_drain`]
    /// held it, otherwise the first refusal of [`Self::can_queue_behind_with_ta`] that
    /// applies, with `ta_pending` from that check. Of the drain causes, a failure, an `InitBM`
    /// flight and the PB check are tested directly; what remains of `needs_engine_drain()` is a
    /// cross-VM or SCS overlap.
    fn hold_cause(&self, job: &Arc<Job>, drain: bool, ta_pending: usize) -> HoldCause {
        let render = command(job, job.progress.sent());
        if drain {
            if job.vm.status.get() != 0
                || self
                    .engine
                    .flights
                    .iter()
                    .any(|f| f.doomed || f.recovery_suspect)
            {
                HoldCause::DrainFailed
            } else if self.engine.flights.iter().any(|f| f.initbm) {
                HoldCause::DrainInitbm
            } else if matches!(render, Some(Command::Render(_))) && !self.render_can_pipeline(job) {
                HoldCause::DrainPb
            } else {
                HoldCause::DrainCross
            }
        } else if job.check_command_dependencies(job.progress.sent()).is_err() {
            HoldCause::Dependency
        } else if matches!(render, Some(Command::Render(r))
            if ta_pending >= self.ta_wave_cap_for(r.is_heavy_ta()))
        {
            HoldCause::TaWave
        } else if self.blocked_on_slot_headroom(job) {
            HoldCause::Slot
        } else {
            HoldCause::Other
        }
    }

    /// Counts a publication pass that stopped at the ready `job` behind in-flight work, and
    /// marks it [`Engine::held_wait`] when only its own VM's flights hold it.
    fn note_hold(&mut self, job: &Arc<Job>, drain: bool, ta_pending: usize) {
        let cause = self.hold_cause(job, drain, ta_pending);
        let s = &mut self.engine.stats;
        s.hold[0] += 1;
        s.hold_causes[cause as usize] += 1;
        if !matches!(
            cause,
            HoldCause::DrainFailed | HoldCause::DrainCross | HoldCause::Dependency
        ) && self
            .engine
            .flights
            .iter()
            .all(|f| Arc::ptr_eq(&f.job.vm, &job.vm))
        {
            self.engine.held_wait = true;
        }
    }

    /// Whether `job` is ready to run without an engine drain but currently blocked solely on
    /// flight-depth or pass/compute-slot headroom (not on InitBM/recovery drain, external sync
    /// dependencies, or CPU timestamp-publish waits).
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
            Some(Command::Compute(c)) => {
                let compute_count: usize = e.flights.iter().map(|f| f.computes as usize).sum();
                let scs = job.resources.compute_scs() || c.is_rt();
                compute_count > 0
                    && !self.slots.compute_exhausted()
                    && (compute_count >= params::compute_flights()
                        || job
                            .resources
                            .compute_slot_mask()
                            .is_ok_and(|mask| !self.has_compute_pipe_for_vm(&job.vm, mask, scs)))
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

    /// Unfinished TA passes across active render flights. Returns immediately without reading
    /// WC memory when total active render passes are already below `TA_WAVE_PASSES`.
    pub(crate) fn ta_pending_passes(&self) -> usize {
        let active_renders: usize = self
            .engine
            .flights
            .iter()
            .map(|f| f.renders as usize)
            .sum();
        if active_renders < super::hw::sizes::TA_WAVE_PASSES {
            return active_renders;
        }
        let Ok(obs) = self.graph.render.fw_stamp[0].r32(0) else {
            return active_renders;
        };
        let tail = self.engine.ta_target;
        let mut pending = 0usize;
        for f in &self.engine.flights {
            for &stamp in &f.r_ta_stamp[..(f.renders as usize).min(f.r_ta_stamp.len())] {
                if !super::recovery::logic::stamp_reached(obs, stamp, tail) {
                    pending += 1;
                }
            }
        }
        pending
    }

    /// Adaptive TA wave window for a candidate render pass: conservative `TA_WAVE_PASSES` (4)
    /// when either the candidate or any active render flight is heavy (multi-layer or tilemap
    /// above `TA_LIGHT_TILEMAP_BYTES`), and `TA_WAVE_PASSES_LIGHT` (8) for light single-layer
    /// render bursts.
    fn ta_wave_cap_for(&self, cand_heavy: bool) -> usize {
        if cand_heavy
            || self
                .engine
                .flights
                .iter()
                .any(|fl| fl.renders != 0 && fl.heavy_render)
        {
            super::hw::sizes::TA_WAVE_PASSES
        } else {
            super::hw::sizes::TA_WAVE_PASSES_LIGHT
        }
    }

    /// Active TA wave window across in-flight renders and pending render heads, used for
    /// immediate wave doorbell flushes and `ta_wave_ready()` hysteresis.
    pub(crate) fn active_ta_wave_cap(&self) -> usize {
        if self
            .engine
            .flights
            .iter()
            .any(|fl| fl.renders != 0 && fl.heavy_render)
            || self.engine.pending.iter().any(|cand| {
                matches!(
                    command(cand, cand.progress.sent()),
                    Some(Command::Render(r)) if r.is_heavy_ta()
                )
            })
        {
            super::hw::sizes::TA_WAVE_PASSES
        } else {
            super::hw::sizes::TA_WAVE_PASSES_LIGHT
        }
    }

    /// Whether `job`'s next command may be published behind the flights in flight:
    ///
    /// - Compute queues behind healthy non-InitBM compute and render flights of its own VM
    ///   (up to [`params::compute_flights`] active computes), each in its own
    ///   `ComputeSlot` and hardware UMA slot (`COMPUTE_UMA_SLOT + index`), unless `query_access`
    ///   is set and a same-queue predecessor in its compute or render barrier prefix has
    ///   CPU-published timestamp outputs that have not retired yet
    ///   (`DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS` clears `query_access`).
    /// - Render queues behind healthy non-InitBM render flights of any VM or same-VM compute
    ///   flights (up to [`params::render_flights`] active renders), rotating `PassSlot`
    ///   (`0..RENDER_SLOTS`) while reusing that VM's warm PB/UMA allocator slot
    ///   (`Pools::can_pipeline_within`, each owning its own hardware UMA slot
    ///   `RENDER_UMA_SLOT + slot`). Hardware barriers on the CDM/TA/FS rings preserve exact
    ///   same-engine, cross-engine, and cross-VM FS ordering while allowing cross-VM TA to
    ///   overlap the preceding VM's FS.
    /// - Cross-VM compute/render overlap is disallowed in both directions so a long compute
    ///   dispatch from one client cannot starve another client's fragment shading into a
    ///   firmware DM1 timeout. Cold InitBM priming renders and every failure or recovery path
    ///   still drain the engine first.
    /// - Cross-VM non-SCS compute runs concurrently across disjoint per-VM firmware workqueues
    ///   (`graph.compute[0..COMPUTE_PIPES]`): two different VMs/ASIDs are never queued into the
    ///   same in-flight `qinfo`, avoiding cross-VM engine drains without sharing an active
    ///   workqueue ring. SCS/RT compute still drains against non-SCS or other-VM compute.
    fn can_queue_behind(&self, job: &Arc<Job>) -> bool {
        let mut ta_pending = None;
        self.can_queue_behind_with_ta(job, &mut ta_pending)
    }

    /// [`Self::can_queue_behind`] with an optional caller-hoisted cache of
    /// [`Self::ta_pending_passes`] so scanning many pending jobs performs at most one WC stamp
    /// read per pass.
    fn can_queue_behind_with_ta(&self, job: &Arc<Job>, ta_pending: &mut Option<usize>) -> bool {
        let e = &self.engine;
        let sent = job.progress.sent();
        if job.vm.status.get() != 0
            || e.flights.len() >= super::hw::sizes::FLIGHTS
            || e.flights
                .iter()
                .any(|f| f.doomed || f.recovery_suspect || f.initbm)
        {
            return false;
        }
        if job.check_command_dependencies(sent).is_err() {
            return false;
        }
        match command(job, sent) {
            Some(Command::Compute(c)) => {
                // G16 publishes timestamp results into user query-pool BOs on CPU during flight
                // retirement, so dispatches without DRM_ASAHI_COMPUTE_NO_QUERY_ACCESS must wait
                // for prior timestamped flights in their dependency prefix to retire first.
                let waits_for_timestamps = c.query_access
                    && (job.command_order(sent).is_some_and(|o| {
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
                    }) || job.fw_waits.iter().any(|w| {
                        (0..2).any(|class| {
                            w.last_ordinal[class].is_some_and(|last| {
                                e.flights.iter().any(|f| {
                                    let matches_class = if class == super::command::RENDER {
                                        f.renders != 0
                                    } else {
                                        f.computes != 0
                                    };
                                    matches_class
                                        && f.has_timestamps
                                        && Arc::ptr_eq(&f.job.queue, &w.queue)
                                        && f.job
                                            .command_order(f.first as u32)
                                            .is_none_or(|fo| fo.sequence <= last)
                                })
                            })
                        })
                    }));
                let cross_vm_render = e
                    .flights
                    .iter()
                    .any(|f| f.renders != 0 && !Arc::ptr_eq(&f.job.vm, &job.vm));
                let scs = job.resources.compute_scs() || c.is_rt();
                let scs_conflict = e
                    .flights
                    .iter()
                    .any(|f| scs_compute_conflict(f, &job.vm, scs));
                let compute_count: usize = e.flights.iter().map(|f| f.computes as usize).sum();
                !cross_vm_render
                    && !scs_conflict
                    && !waits_for_timestamps
                    && compute_count < params::compute_flights()
                    && !self.slots.compute_exhausted()
                    && super::recovery::logic::next_ordinal(e.ordinal).is_some()
                    && job
                        .resources
                        .compute_slot_mask()
                        .is_ok_and(|mask| self.has_compute_pipe_for_vm(&job.vm, mask, scs))
            }
            Some(Command::Render(r)) => {
                let cross_vm_compute = e
                    .flights
                    .iter()
                    .any(|f| f.computes != 0 && !Arc::ptr_eq(&f.job.vm, &job.vm));
                let msaa_conflict = !self.msaa_member_allowed(&job.vm, r);
                let render_count: usize = e.flights.iter().map(|f| f.renders as usize).sum();
                let ta_cap = self.ta_wave_cap_for(r.is_heavy_ta());
                !cross_vm_compute
                    && !msaa_conflict
                    && render_count < params::render_flights()
                    && *ta_pending.get_or_insert_with(|| self.ta_pending_passes()) < ta_cap
                    && !self.slots.render_exhausted()
                    && !self.pools.exhausted()
                    && super::recovery::logic::next_ordinal(e.draw).is_some()
                    && self.render_can_pipeline(job)
                    && job
                        .resources
                        .render_passes()
                        .is_ok_and(|passes| self.slots.pass_available(passes))
            }
            None => false,
        }
    }

    /// Whether `self.engine.pending` is an eligible single-queue, single-VM busy horizon
    /// batch that can accumulate more same-queue commands before publishing.
    fn can_defer_busy_coalesce(&self, now: u64) -> bool {
        if self.recovery.failed().is_some()
            || self.engine.frozen
            || self.engine.flights.is_empty()
            || self.engine.pending.is_empty()
            || self.engine.flights.len() >= super::hw::sizes::FLIGHTS
            || self.engine.chan_wait
            || self.engine.slot_wait
            || self.engine.ta_wave_wait
            || self.engine.held_wait
            || self
                .engine
                .flights
                .iter()
                .any(|f| f.doomed || f.recovery_suspect || f.initbm)
        {
            return false;
        }
        let head = &self.engine.pending[0];
        let head_sent = head.progress.sent();
        if head.vm.status.get() != 0
            || head.long()
            || !self.can_queue_behind(head)
            || self
                .engine
                .flights
                .iter()
                .any(|fl| !Arc::ptr_eq(&fl.job.queue, &head.queue))
            || self
                .engine
                .pending
                .iter()
                .any(|j| !Arc::ptr_eq(&j.queue, &head.queue) || !Arc::ptr_eq(&j.vm, &head.vm))
        {
            return false;
        }
        let Some(head_order) = head.command_order(head_sent) else {
            return false;
        };
        match command(head, head_sent) {
            Some(Command::Compute(c)) => {
                if head.resources.compute_scs()
                    || c.is_rt()
                    || head_order.wait_through != [None, None]
                    || !head.fw_waits.is_empty()
                    || self.compute_queue_is_heavy(&head.queue, now)
                {
                    return false;
                }
                let queue_active: usize = self
                    .engine
                    .flights
                    .iter()
                    .filter(|fl| fl.computes != 0 && Arc::ptr_eq(&fl.job.queue, &head.queue))
                    .map(|fl| fl.computes as usize)
                    .sum();
                if queue_active < 2 {
                    return false;
                }
                let Some(mut next_seq) = head_order.sequence.checked_add(1) else {
                    return false;
                };
                let mut pending_cmds = 1usize;
                for k in (head_sent + 1)..head.progress.count() {
                    let Some(Command::Compute(c2)) = command(head, k) else {
                        return false;
                    };
                    if c2.is_rt() || !head.can_fold_horizon_cmd(k, next_seq) {
                        return false;
                    }
                    let Some(s) = next_seq.checked_add(1) else {
                        return false;
                    };
                    next_seq = s;
                    pending_cmds += 1;
                }
                for j in &self.engine.pending[1..] {
                    let n = j.progress.count();
                    if j.long()
                        || j.resources.compute_scs()
                        || !j.can_fold_horizon_job(next_seq)
                        || (0..n).any(|k| {
                            matches!(command(j, k), Some(Command::Compute(c2)) if c2.is_rt())
                        })
                    {
                        return false;
                    }
                    let Some(s) = next_seq.checked_add(u64::from(n)) else {
                        return false;
                    };
                    next_seq = s;
                    pending_cmds += n as usize;
                }
                let active_computes: usize = self
                    .engine
                    .flights
                    .iter()
                    .map(|fl| fl.computes as usize)
                    .sum();
                let Ok(slot_mask) = head.resources.compute_slot_mask() else {
                    return false;
                };
                let Some(pipe) = self.choose_compute_pipe(
                    &head.queue,
                    &head.vm,
                    [None; sizes::COMPUTE_PIPES],
                    slot_mask,
                    false,
                    false,
                ) else {
                    return false;
                };
                let free_c = self.slots.free_compute_in_mask_for_pipe(slot_mask, pipe) as usize;
                let cap = super::hw::sizes::COALESCE_MAX_CMDS
                    .min(free_c)
                    .min(params::compute_flights().saturating_sub(active_computes))
                    .min(super::hw::sizes::COMPUTE_SLOTS);
                cap > 1 && pending_cmds < cap
            }
            Some(Command::Render(r)) => {
                if head.resources.render_scs() || !head.fw_waits.is_empty() {
                    return false;
                }
                let queue_active: usize = self
                    .engine
                    .flights
                    .iter()
                    .filter(|fl| fl.renders != 0 && Arc::ptr_eq(&fl.job.queue, &head.queue))
                    .map(|fl| fl.renders as usize)
                    .sum();
                if queue_active < 2 {
                    return false;
                }
                let Some(mut next_seq) = head_order.sequence.checked_add(1) else {
                    return false;
                };
                let mut wave_heavy = r.is_heavy_ta();
                let mut pending_cmds = 1usize;
                for k in (head_sent + 1)..head.progress.count() {
                    if !head.can_fold_render_cmd(k, next_seq) {
                        return false;
                    }
                    if matches!(command(head, k), Some(Command::Render(r2)) if r2.is_heavy_ta()) {
                        wave_heavy = true;
                    }
                    let Some(s) = next_seq.checked_add(1) else {
                        return false;
                    };
                    next_seq = s;
                    pending_cmds += 1;
                }
                for j in &self.engine.pending[1..] {
                    let n = j.progress.count();
                    if j.long()
                        || j.resources.render_scs()
                        || !j.can_fold_render_horizon_job(next_seq)
                    {
                        return false;
                    }
                    if (0..n).any(|k| {
                        matches!(command(j, k), Some(Command::Render(r2)) if r2.is_heavy_ta())
                    }) {
                        wave_heavy = true;
                    }
                    let Some(s) = next_seq.checked_add(u64::from(n)) else {
                        return false;
                    };
                    next_seq = s;
                    pending_cmds += n as usize;
                }
                let active_renders: usize = self
                    .engine
                    .flights
                    .iter()
                    .map(|fl| fl.renders as usize)
                    .sum();
                let wave_cap = self.ta_wave_cap_for(wave_heavy);
                let ta_room = wave_cap.saturating_sub(self.ta_pending_passes());
                let cap = super::hw::sizes::COALESCE_MAX_CMDS
                    .min(params::render_flights().saturating_sub(active_renders))
                    .min(ta_room)
                    .min(super::hw::sizes::RENDER_SLOTS);
                cap > 1 && pending_cmds < cap
            }
            None => false,
        }
    }

    /// Defers immediate publication for up to `COALESCE_WINDOW_US` while a single queue already
    /// has `>= 2` active commands in flight so consecutive same-queue `SUBMIT` ioctls accumulate
    /// into one multi-command flight (up to `COALESCE_MAX_CMDS`), cutting `SubmitRecord`s, WC
    /// producer updates, and doorbells without delaying `d=1`/`d=2`, barriered streams, RT/SCS
    /// work, or multi-queue/cross-VM handoffs.
    pub(crate) fn defer_busy_coalesce(&mut self, now: u64) -> Option<u32> {
        if !self.can_defer_busy_coalesce(now) {
            self.engine.coalesce_deadline_ns = 0;
            return None;
        }
        if self.engine.coalesce_deadline_ns == 0 {
            self.engine.coalesce_deadline_ns =
                now.saturating_add(super::hw::sizes::COALESCE_WINDOW_NS);
        }
        if now >= self.engine.coalesce_deadline_ns {
            self.engine.coalesce_deadline_ns = 0;
            return None;
        }
        let rem_ns = self.engine.coalesce_deadline_ns.saturating_sub(now);
        Some((rem_ns.div_ceil(1000) as u32).clamp(1, super::hw::sizes::COALESCE_WINDOW_US))
    }

    /// Resolves the newest in-flight target stamp and producer `InitBM` state for `class`
    /// (`RENDER` -> `r_fs_stamp`, `COMPUTE` -> `cstamp`) across `job`'s same-queue `target_seq`
    /// and any cross-queue `job.fw_waits` (plus an earlier member of an in-progress render batch
    /// `current`). Returns `Ok(None)` when no barrier is needed or every waited prefix retired.
    fn wait_stamp_for_with(
        &self,
        job: &Arc<Job>,
        class: usize,
        target_seq: Option<u64>,
        current: Option<&Flight>,
    ) -> Result<Option<WaitTarget>> {
        let mut targets: [Option<(&Arc<super::queue::QueueState>, u64, bool)>;
            1 + super::fence::MAX_FW_WAITS] = [None; 1 + super::fence::MAX_FW_WAITS];
        let mut count = 0usize;
        if let Some(last) = target_seq {
            if job.queue.completed[class].load(Ordering::Acquire) < last {
                targets[count] = Some((&job.queue, last, false));
                count += 1;
            }
        }
        for wait in job.fw_waits.iter() {
            let Some(last) = wait.last_ordinal[class] else {
                continue;
            };
            if wait.queue.completed[class].load(Ordering::Acquire) >= last {
                continue;
            }
            if let Some(slot) = targets[..count]
                .iter_mut()
                .flatten()
                .find(|(q, _, _)| Arc::ptr_eq(q, &wait.queue))
            {
                slot.1 = slot.1.max(last);
            } else if count < targets.len() {
                targets[count] = Some((&wait.queue, last, false));
                count += 1;
            } else {
                return Err(EAGAIN);
            }
        }
        if count == 0 {
            return Ok(None);
        }
        let mut newest: Option<WaitTarget> = None;
        let scan_flight = |flight: &Flight,
                           targets: &mut [Option<(&Arc<super::queue::QueueState>, u64, bool)>],
                           newest: &mut Option<WaitTarget>|
         -> Result {
            if class == super::command::COMPUTE {
                if flight.computes == 0 {
                    return Ok(());
                }
            } else if flight.renders == 0 {
                return Ok(());
            }
            if !targets
                .iter()
                .flatten()
                .any(|(q, _, _)| Arc::ptr_eq(&flight.job.queue, q))
            {
                return Ok(());
            }
            let max_members = if class == super::command::COMPUTE {
                (flight.computes as usize).min(flight.cstamp.len())
            } else {
                (flight.renders as usize).min(flight.r_fs_stamp.len())
            };
            let mut member = 0usize;
            for k in flight.first..flight.first + flight.count {
                if member >= max_members {
                    break;
                }
                if let Some(fo) = flight.job.command_order(k as u32) {
                    if fo.engine == class {
                        for (q, last, found) in targets.iter_mut().flatten() {
                            if Arc::ptr_eq(&flight.job.queue, q) && fo.sequence == *last {
                                *found = true;
                                let stamp = if class == super::command::COMPUTE {
                                    flight.cstamp[member]
                                } else {
                                    flight.r_fs_stamp[member]
                                };
                                *newest = Some(WaitTarget {
                                    stamp,
                                    initbm: flight.initbm,
                                });
                            }
                        }
                    }
                }
                member += 1;
            }
            for j2 in flight.group.iter() {
                for k in 0..j2.progress.count() {
                    if member >= max_members {
                        break;
                    }
                    if let Some(fo) = j2.command_order(k) {
                        if fo.engine == class {
                            for (q, last, found) in targets.iter_mut().flatten() {
                                if Arc::ptr_eq(&flight.job.queue, q) && fo.sequence == *last {
                                    *found = true;
                                    let stamp = if class == super::command::COMPUTE {
                                        flight.cstamp[member]
                                    } else {
                                        flight.r_fs_stamp[member]
                                    };
                                    *newest = Some(WaitTarget {
                                        stamp,
                                        initbm: flight.initbm,
                                    });
                                }
                            }
                        }
                    }
                    member += 1;
                }
            }
            Ok(())
        };
        for flight in &self.engine.flights {
            scan_flight(flight, &mut targets[..count], &mut newest)?;
        }
        if let Some(cur) = current {
            scan_flight(cur, &mut targets[..count], &mut newest)?;
        }
        if targets[..count].iter().flatten().any(|(_, _, found)| !*found) {
            return Err(EAGAIN);
        }
        Ok(newest)
    }

    /// Resolves a same-queue or cross-queue target stamp for `class` across published flights.
    fn wait_stamp_for(
        &self,
        job: &Arc<Job>,
        class: usize,
        target_seq: Option<u64>,
    ) -> Result<Option<WaitTarget>> {
        self.wait_stamp_for_with(job, class, target_seq, None)
    }

    /// Resolves a cross-engine barrier target when `job`'s command `index` waits on an in-flight
    /// command of `other` engine (`RENDER` -> `r_fs_stamp`, `COMPUTE` -> `cstamp`), including
    /// cross-queue `fw_waits`. Returns `Ok(None)` when no barrier is needed or retired.
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

    /// Resolves the newest in-flight compute producer on each compute pipe across `job`'s
    /// same-queue `target_seq` and any cross-queue `job.fw_waits`. Returns
    /// `Ok([None; COMPUTE_PIPES])` when no barrier is needed or every waited prefix retired,
    /// and `Err(EAGAIN)` if any unretired target ordinal is missing from `flights`.
    fn wait_compute_stamps_for(
        &self,
        job: &Arc<Job>,
        target_seq: Option<u64>,
    ) -> Result<[Option<WaitTarget>; sizes::COMPUTE_PIPES]> {
        let mut targets: [Option<(&Arc<super::queue::QueueState>, u64, bool)>;
            1 + super::fence::MAX_FW_WAITS] = [None; 1 + super::fence::MAX_FW_WAITS];
        let mut count = 0usize;
        if let Some(last) = target_seq {
            if job.queue.completed[super::command::COMPUTE].load(Ordering::Acquire) < last {
                targets[count] = Some((&job.queue, last, false));
                count += 1;
            }
        }
        for wait in job.fw_waits.iter() {
            let Some(last) = wait.last_ordinal[super::command::COMPUTE] else {
                continue;
            };
            if wait.queue.completed[super::command::COMPUTE].load(Ordering::Acquire) >= last {
                continue;
            }
            if let Some(slot) = targets[..count]
                .iter_mut()
                .flatten()
                .find(|(q, _, _)| Arc::ptr_eq(q, &wait.queue))
            {
                slot.1 = slot.1.max(last);
            } else if count < targets.len() {
                targets[count] = Some((&wait.queue, last, false));
                count += 1;
            } else {
                return Err(EAGAIN);
            }
        }
        if count == 0 {
            return Ok([None; sizes::COMPUTE_PIPES]);
        }
        let mut waits = [None; sizes::COMPUTE_PIPES];
        for flight in &self.engine.flights {
            if flight.computes == 0
                || !targets[..count]
                    .iter()
                    .flatten()
                    .any(|(q, _, _)| Arc::ptr_eq(&flight.job.queue, q))
            {
                continue;
            }
            let pipe = flight.compute_pipe as usize;
            if pipe >= sizes::COMPUTE_PIPES {
                return Err(EIO);
            }
            let mut member = 0usize;
            for k in flight.first..flight.first + flight.count {
                if let Some(fo) = flight.job.command_order(k as u32) {
                    if fo.engine == super::command::COMPUTE {
                        let mut matched = false;
                        for (q, last, found) in targets[..count].iter_mut().flatten() {
                            if Arc::ptr_eq(&flight.job.queue, q) && fo.sequence <= *last {
                                if fo.sequence == *last {
                                    *found = true;
                                }
                                matched = true;
                            }
                        }
                        if matched {
                            waits[pipe] = Some(WaitTarget {
                                stamp: *flight.cstamp.get(member).ok_or(EAGAIN)?,
                                initbm: flight.initbm,
                            });
                        }
                    }
                }
                member += 1;
            }
            for j2 in flight.group.iter() {
                for k in 0..j2.progress.count() {
                    if let Some(fo) = j2.command_order(k) {
                        if fo.engine == super::command::COMPUTE {
                            let mut matched = false;
                            for (q, last, found) in targets[..count].iter_mut().flatten() {
                                if Arc::ptr_eq(&flight.job.queue, q) && fo.sequence <= *last {
                                    if fo.sequence == *last {
                                        *found = true;
                                    }
                                    matched = true;
                                }
                            }
                            if matched {
                                waits[pipe] = Some(WaitTarget {
                                    stamp: *flight.cstamp.get(member).ok_or(EAGAIN)?,
                                    initbm: flight.initbm,
                                });
                            }
                        }
                    }
                    member += 1;
                }
            }
        }
        for (q, last, found) in targets[..count].iter().flatten() {
            if !*found && !q.is_compute_retired(*last) {
                return Err(EAGAIN);
            }
        }
        Ok(waits)
    }

    /// Whether `queue`'s recent or active compute work is heavy enough to benefit from 2-pipe
    /// CDM fanout rather than 1-pipe affinity and horizon folding:
    /// - per-queue retired CDM GPU duration EWMA `>= COMPUTE_HEAVY_EMA_US`, or
    /// - a sole active single-command compute flight of `queue` on its pipe has already been
    ///   executing for `>= COMPUTE_HEAVY_INFLIGHT_NS` without completing (instant detection
    ///   before retirement updates the EWMA, while queued multi-flight light bursts never false-
    ///   trigger from queueing age).
    fn compute_queue_is_heavy(&self, queue: &Arc<super::queue::QueueState>, now: u64) -> bool {
        if queue.compute_ewma_us.load(Ordering::Relaxed) >= super::hw::sizes::COMPUTE_HEAVY_EMA_US {
            return true;
        }
        let mut pipe_flights = [0usize; super::hw::sizes::COMPUTE_PIPES];
        for fl in &self.engine.flights {
            if fl.computes != 0 {
                let p = (fl.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
                pipe_flights[p] += 1;
            }
        }
        self.engine.flights.iter().any(|fl| {
            let p = (fl.compute_pipe as usize).min(super::hw::sizes::COMPUTE_PIPES - 1);
            fl.computes == 1
                && pipe_flights[p] == 1
                && Arc::ptr_eq(&fl.job.queue, queue)
                && fl.published_ns != 0
                && now.saturating_sub(fl.published_ns)
                    >= super::hw::sizes::COMPUTE_HEAVY_INFLIGHT_NS
        })
    }

    /// When a ready compute job cannot queue behind active flights because every compatible
    /// compute pipe is currently occupied by another VM (for example a base-only VM limited to
    /// pipe 0, or a 9th VM when all 8 pipes are occupied), pick one compatible occupied pipe to
    /// drain for handoff while other pipes continue dispatching.
    fn compute_handoff_pipe(&self, job: &Arc<Job>) -> Option<usize> {
        let sent = job.progress.sent();
        let Some(Command::Compute(c)) = command(job, sent) else {
            return None;
        };
        if job.vm.status.get() != 0
            || self.slots.compute_exhausted()
            || job.check_command_dependencies(sent).is_err()
        {
            return None;
        }
        let Ok(slot_mask) = job.resources.compute_slot_mask() else {
            return None;
        };
        let scs = job.resources.compute_scs() || c.is_rt();
        if self.has_compute_pipe_for_vm(&job.vm, slot_mask, scs) {
            return None;
        }
        let mut other_load = [0usize; sizes::COMPUTE_PIPES];
        for fl in &self.engine.flights {
            if fl.computes != 0 && !Arc::ptr_eq(&fl.job.vm, &job.vm) {
                let p = (fl.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
                other_load[p] += fl.computes as usize;
            }
        }
        let limit = if scs { 1 } else { sizes::COMPUTE_PIPES };
        let mut best: Option<(usize, usize)> = None;
        for (p, &load) in other_load.iter().enumerate().take(limit) {
            if load == 0 {
                continue;
            }
            let allowed = self.slots.compute_allowed_for_pipe(slot_mask, p);
            if allowed == 0 || self.slots.compute_exhausted_in_mask(allowed) {
                continue;
            }
            if best.is_none_or(|(_, prev)| load < prev) {
                best = Some((p, load));
            }
        }
        best.map(|(p, _)| p)
    }

    /// Whether `vm` can acquire a compute pipe (`0..COMPUTE_PIPES`) without sharing an active
    /// `qinfo` with another VM, exceeding `MAX_VM_COMPUTE_PIPES`, using a pipe draining for
    /// handoff, or lacking an available unit in `slot_mask`.
    fn has_compute_pipe_for_vm(
        &self,
        vm: &Arc<super::vm::VmState>,
        slot_mask: u32,
        rt_compute: bool,
    ) -> bool {
        let avoid = self.engine.handoff_avoid_pipes;
        let mut active_same = [false; sizes::COMPUTE_PIPES];
        let mut active_other = [false; sizes::COMPUTE_PIPES];
        let mut any_rt = rt_compute;
        for fl in &self.engine.flights {
            if fl.computes != 0 {
                let p = (fl.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
                if Arc::ptr_eq(&fl.job.vm, vm) {
                    active_same[p] = true;
                } else {
                    active_other[p] = true;
                }
                any_rt |= fl.rt_compute;
            }
        }
        if any_rt {
            return (avoid & 1) == 0
                && !active_other[0]
                && self.slots.compute_available_in_mask_for_pipe(slot_mask, 0);
        }
        let vm_active_pipes = active_same.iter().filter(|&&a| a).count();
        for p in 0..sizes::COMPUTE_PIPES {
            if (avoid & (1u32 << p)) != 0 || active_other[p] {
                continue;
            }
            if !active_same[p] && vm_active_pipes >= sizes::MAX_VM_COMPUTE_PIPES {
                continue;
            }
            if self.slots.compute_available_in_mask_for_pipe(slot_mask, p) {
                return true;
            }
        }
        false
    }

    /// Minimum active compute count on another pipe eligible for `vm` besides `pipe`, used to
    /// balance heavy barrier-free compute across up to `MAX_VM_COMPUTE_PIPES` pipes of `vm`.
    fn other_vm_compute_pipe_load(
        &self,
        vm: &Arc<super::vm::VmState>,
        pipe: usize,
        slot_mask: u32,
    ) -> Option<usize> {
        let mut active_same = [0usize; sizes::COMPUTE_PIPES];
        let mut active_other = [false; sizes::COMPUTE_PIPES];
        for fl in &self.engine.flights {
            if fl.computes != 0 {
                let p = (fl.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
                if Arc::ptr_eq(&fl.job.vm, vm) {
                    active_same[p] += fl.computes as usize;
                } else {
                    active_other[p] = true;
                }
            }
        }
        let vm_active_pipes = active_same.iter().filter(|&&n| n != 0).count();
        let mut best: Option<usize> = None;
        for p in 0..sizes::COMPUTE_PIPES {
            if p == pipe || active_other[p] {
                continue;
            }
            if active_same[p] == 0 && vm_active_pipes >= sizes::MAX_VM_COMPUTE_PIPES {
                continue;
            }
            if !self.slots.compute_available_in_mask_for_pipe(slot_mask, p) {
                continue;
            }
            best = Some(match best {
                Some(prev) => prev.min(active_same[p]),
                None => active_same[p],
            });
        }
        best
    }

    /// Chooses which compute pipe (`0..COMPUTE_PIPES`) executes the next compute command
    /// without ever mixing two different VMs/ASIDs inside one active `qinfo`:
    /// - RT/SCS compute (or any flight while RT/SCS compute is active) stays on pipe 0.
    /// - If the command waits only on one pipe of its own VM, reuse that pipe (in-order ring,
    ///   zero `c2c` barrier).
    /// - If barrier-free (`waits` all `None`), `!heavy_compute`, and `queue` already has an active
    ///   compute flight on a single pipe with free units, keep that light queue pipe-affine so
    ///   consecutive horizon submits fold onto one workqueue without splitting across pipes.
    /// - Otherwise, lease an idle pipe (preferring `vm`'s warm-affine `pipe_last_vm`, then never-
    ///   used, then LRU, preferring secondary pipes `1..COMPUTE_PIPES` for fresh/LRU picks so
    ///   pipe 0 stays free for base-only/SCS VMs) up to `MAX_VM_COMPUTE_PIPES` active pipes per
    ///   VM, or pick `vm`'s active eligible pipe with fewest commands in flight.
    /// - Returns `None` when every eligible pipe is currently active for another VM or has no
    ///   available compute unit.
    fn choose_compute_pipe(
        &self,
        queue: &Arc<super::queue::QueueState>,
        vm: &Arc<super::vm::VmState>,
        waits: [Option<WaitTarget>; sizes::COMPUTE_PIPES],
        slot_mask: u32,
        rt_compute: bool,
        heavy_compute: bool,
    ) -> Option<usize> {
        let mut active_same = [0usize; sizes::COMPUTE_PIPES];
        let mut active_other = [false; sizes::COMPUTE_PIPES];
        let mut any_rt = rt_compute;
        for fl in &self.engine.flights {
            if fl.computes != 0 {
                let p = (fl.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
                if Arc::ptr_eq(&fl.job.vm, vm) {
                    active_same[p] += fl.computes as usize;
                } else {
                    active_other[p] = true;
                }
                any_rt |= fl.rt_compute;
            }
        }
        let avoid = self.engine.handoff_avoid_pipes;
        let vm_active_pipes = active_same.iter().filter(|&&n| n != 0).count();
        let pipe_ok = |p: usize| -> bool {
            (avoid & (1u32 << p)) == 0
                && !active_other[p]
                && (active_same[p] != 0 || vm_active_pipes < sizes::MAX_VM_COMPUTE_PIPES)
                && self.slots.compute_available_in_mask_for_pipe(slot_mask, p)
        };
        if any_rt {
            return pipe_ok(0).then_some(0);
        }
        let mut wait_pipe: Option<usize> = None;
        let mut wait_count = 0usize;
        for (p, w) in waits.iter().enumerate() {
            if w.is_some() {
                wait_pipe = Some(p);
                wait_count += 1;
            }
        }
        if wait_count == 1 {
            if let Some(p) = wait_pipe {
                if pipe_ok(p) {
                    return Some(p);
                }
            }
        }
        if wait_count == 0 && !heavy_compute {
            let mut q_pipe: Option<usize> = None;
            for fl in &self.engine.flights {
                if fl.computes != 0 && Arc::ptr_eq(&fl.job.queue, queue) {
                    let p = (fl.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
                    match q_pipe {
                        None => q_pipe = Some(p),
                        Some(prev) if prev == p => {}
                        Some(_) => {
                            q_pipe = None;
                            break;
                        }
                    }
                }
            }
            if let Some(p) = q_pipe {
                if pipe_ok(p) {
                    return Some(p);
                }
            }
        }
        let pick_idle = |prefer_secondary: bool| -> Option<usize> {
            // 1. Warm VM-affine idle pipe (keeps the same VM on the same `qinfo`).
            for p in 0..sizes::COMPUTE_PIPES {
                if active_same[p] == 0
                    && pipe_ok(p)
                    && self.engine.pipe_last_vm[p] == Some(vm.id)
                {
                    return Some(p);
                }
            }
            // 2. Never-used idle pipe, preferring secondary pipes for extension-capable VMs so
            //    pipe 0 remains immediately available to base-only and SCS VMs.
            let mut order = [0usize; sizes::COMPUTE_PIPES];
            if prefer_secondary {
                for i in 0..(sizes::COMPUTE_PIPES - 1) {
                    order[i] = i + 1;
                }
                order[sizes::COMPUTE_PIPES - 1] = 0;
            } else {
                for (i, slot) in order.iter_mut().enumerate() {
                    *slot = i;
                }
            }
            for &p in &order {
                if active_same[p] == 0 && pipe_ok(p) && self.engine.pipe_last_vm[p].is_none() {
                    return Some(p);
                }
            }
            // 3. Least-recently-used idle pipe, preferring secondary pipes when `prefer_secondary`
            //    is set so pipe 0 remains available to base-only and SCS VMs.
            if prefer_secondary {
                let mut best_sec: Option<(usize, u64)> = None;
                for p in 1..sizes::COMPUTE_PIPES {
                    if active_same[p] == 0 && pipe_ok(p) {
                        let used = self.engine.pipe_last_used[p];
                        if best_sec.is_none_or(|(_, prev)| used < prev) {
                            best_sec = Some((p, used));
                        }
                    }
                }
                if let Some((p, _)) = best_sec {
                    return Some(p);
                }
                if active_same[0] == 0 && pipe_ok(0) {
                    return Some(0);
                }
            } else {
                let mut best: Option<(usize, u64)> = None;
                for p in 0..sizes::COMPUTE_PIPES {
                    if active_same[p] == 0 && pipe_ok(p) {
                        let used = self.engine.pipe_last_used[p];
                        if best.is_none_or(|(_, prev)| used < prev) {
                            best = Some((p, used));
                        }
                    }
                }
                if let Some((p, _)) = best {
                    return Some(p);
                }
            }
            None
        };
        let has_secondary = self.slots.compute_allowed_for_pipe(slot_mask, 1) != 0;
        let prefer_secondary = has_secondary && active_other.iter().any(|&a| a);
        if vm_active_pipes < sizes::MAX_VM_COMPUTE_PIPES {
            if let Some(p) = pick_idle(prefer_secondary) {
                return Some(p);
            }
        }
        // Reuse this VM's least-loaded eligible active pipe, breaking ties by `next_compute_pipe`.
        let start = (self.engine.next_compute_pipe as usize) % sizes::COMPUTE_PIPES;
        let mut best: Option<(usize, usize)> = None;
        for i in 0..sizes::COMPUTE_PIPES {
            let p = (start + i) % sizes::COMPUTE_PIPES;
            if active_same[p] != 0 && pipe_ok(p) {
                let load = active_same[p];
                if best.is_none_or(|(_, prev)| load < prev) {
                    best = Some((p, load));
                }
            }
        }
        best.map(|(p, _)| p)
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
        let (pb_mask, _) = job.resources.pb_slot_mask()?;
        let heap_blocks = job.resources.pb_heap_blocks()?;
        let render_scs = job.resources.render_scs();
        // Reuse this VM's retired epoch of the allocator slot when still bound, matching the
        // SCS mode, and sized for the VM's current PB heap (no reseed/InitBM).
        let (epoch, reused) = match self.pools.reusable_for_mask_mode(
            alloc_slot,
            &job.vm,
            pb_mask,
            heap_blocks,
            render_scs,
        ) {
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
                if let Err(err) = e.prepare_with_scs_for(
                    self.init.abi.pb_manager(),
                    alloc_slot,
                    job.resources.pb_heap_gva(alloc_slot)?,
                    heap_blocks,
                    render_scs,
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
                pb_init_blocks: epoch.heap_blocks(),
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
            pb_init_blocks: epoch.heap_blocks(),
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
    #[inline(never)]
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
        let (compute_pipe, c_waits, heavy_compute) = if render {
            (0usize, [None; sizes::COMPUTE_PIPES], false)
        } else {
            let slot_mask = job.resources.compute_slot_mask()?;
            let rt_compute = job.resources.compute_scs()
                || matches!(cmd, Command::Compute(c) if c.is_rt());
            let waits = self.wait_compute_stamps_for(
                job,
                job.command_order(index)
                    .and_then(|o| o.wait_through[super::command::COMPUTE]),
            )?;
            let heavy = self.compute_queue_is_heavy(&job.queue, now_ns());
            let Some(pipe) = self.choose_compute_pipe(
                &job.queue,
                &job.vm,
                waits,
                slot_mask,
                rt_compute,
                heavy,
            ) else {
                self.engine.slot_wait = true;
                return Err(EAGAIN);
            };
            (pipe, waits, heavy)
        };
        let pipe_behind = !render
            && self
                .engine
                .flights
                .iter()
                .any(|fl| fl.computes != 0 && fl.compute_pipe as usize == compute_pipe);
        if !render && class_behind {
            self.latch_compute_channel_receipts()?;
        }
        for &channel in channels {
            let pipe = self.channels.channel(channel);
            let producer = self.channels.producer(channel);
            let busy = if class_behind {
                // Behind in-flight commands on this class: post after their unconsumed records
                // and never lap an unsettled compute flight's shared submit-channel record.
                let wrap_bound = channel == Channel::Compute
                    && self.engine.flights.iter().any(|fl| {
                        fl.computes != 0
                            && !fl.chan_settled
                            && producer
                                .wrapping_sub((fl.rec[2] + RING_SLOTS - 1) % RING_SLOTS)
                                % RING_SLOTS
                                >= RING_SLOTS - 2
                    });
                pipe.write()? != producer
                    || producer.wrapping_sub(pipe.read()?) % RING_SLOTS >= sizes::AHEAD_RECORDS
                    || wrap_bound
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
            (
                self.graph.compute[compute_pipe].qstate.r32(qstate::WRITE)?,
                0,
                0,
            )
        };
        let mut prefix = Prefix {
            compute_pipe,
            n: self.engine.ordinal,
            pipe_n: if render {
                0
            } else {
                self.engine.pipe_ordinal[compute_pipe]
            },
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
        if render && class_behind {
            // Room for up to 5 TA entries (prev_barrier, ta_fs_barrier, 2x compute_barrier,
            // ta_work) and 5 FS entries (fs_dep, 2x fs_compute_barrier, own_barrier, fs_work).
            ta_read = self.graph.render.qstate[0].r32(qstate::READ)?;
            fs_read = self.graph.render.qstate[1].r32(qstate::READ)?;
            if ta_read >= 256
                || fs_read >= 256
                || prefix.ta_head.wrapping_sub(ta_read) % sizes::QUEUE_CAP >= sizes::QUEUE_CAP - 6
                || prefix.fs_head.wrapping_sub(fs_read) % sizes::QUEUE_CAP >= sizes::QUEUE_CAP - 6
            {
                self.engine.slot_wait = true;
                return Err(EAGAIN);
            }
        } else if !render && pipe_behind {
            // Room for optional fs2c + up to COMPUTE_PIPES-1 cross-pipe barriers + compute work.
            let min_free = 2 + sizes::COMPUTE_PIPES as u32;
            c_read = self.graph.compute[compute_pipe].qstate.r32(qstate::READ)?;
            if c_read >= 256
                || prefix.c_head.wrapping_sub(c_read) % sizes::QUEUE_CAP
                    >= sizes::QUEUE_CAP - min_free
            {
                self.engine.slot_wait = true;
                return Err(EAGAIN);
            }
        }
        f.first = index as usize;
        f.compute_pipe = compute_pipe as u8;
        f.c_start = prefix.c_head;
        f.ta_start = prefix.ta_head;
        f.fs_start = prefix.fs_head;
        let mut end = index + 1;
        let mut hz_stop: Option<usize> = None;
        match cmd {
            Command::Compute(c) => {
                self.prefix_compute(
                    job,
                    &job.resources,
                    index,
                    c,
                    c_waits,
                    &mut prefix,
                    f,
                    false,
                )?;
                let mut last_seq = job.command_order(index).map(|o| o.sequence);
                let slot_mask = job.resources.compute_slot_mask()?;
                let free_c = self
                    .slots
                    .free_compute_in_mask_for_pipe(slot_mask, compute_pipe)
                    as usize;
                let mut active_pipe = [0usize; sizes::COMPUTE_PIPES];
                for fl in &self.engine.flights {
                    if fl.computes != 0 {
                        let p = (fl.compute_pipe as usize).min(sizes::COMPUTE_PIPES - 1);
                        active_pipe[p] += fl.computes as usize;
                    }
                }
                let active_computes: usize = active_pipe.iter().sum();
                // Keep heavy barrier-free compute balanced across this VM's CDM pipes while light
                // single-queue compute streams stay pipe-affine and fold freely into one batch.
                let pipe_fold_cap = if heavy_compute && c_waits == [None; sizes::COMPUTE_PIPES] {
                    match self.other_vm_compute_pipe_load(&job.vm, compute_pipe, slot_mask) {
                        Some(other_load) => {
                            other_load.saturating_sub(active_pipe[compute_pipe] + 1)
                        }
                        None => usize::MAX,
                    }
                } else {
                    usize::MAX
                };
                let used = prefix.c_head.wrapping_sub(c_read) % sizes::QUEUE_CAP;
                let ring_more = (sizes::QUEUE_CAP.saturating_sub(2 + sizes::COMPUTE_PIPES as u32))
                    .saturating_sub(used) as usize;
                let mut more_slots = free_c
                    .min(params::compute_flights().saturating_sub(active_computes + 1))
                    .min(sizes::COMPUTE_SLOTS.saturating_sub(1))
                    .min(ring_more)
                    .min(pipe_fold_cap);
                let rt_head = job.resources.compute_scs() || c.is_rt();
                // Fold trailing barrier-free compute commands of the base job first.
                while !rt_head && more_slots > 0 && end < job.progress.count() {
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
                    if c2.is_rt() {
                        break;
                    }
                    self.prefix_compute(
                        job,
                        &job.resources,
                        end,
                        c2,
                        [None; sizes::COMPUTE_PIPES],
                        &mut prefix,
                        f,
                        false,
                    )?;
                    last_seq = Some(next_seq);
                    end += 1;
                    more_slots -= 1;
                }
                // Compute horizon: fold subsequent ready, barrier-free compute-only jobs of
                // the same queue and VM into the same CDM producer advance and doorbell.
                // Any non-qualifying job of this queue ends the scan so no later job overtakes it.
                if !rt_head && !job.long() && end == job.progress.count() {
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
                            || j2.long()
                            || j2.resources.compute_scs()
                            || (0..n2).any(|k| {
                                matches!(command(&j2, k), Some(Command::Compute(c2)) if c2.is_rt())
                            })
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
                                        [None; sizes::COMPUTE_PIPES],
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
                let geometry =
                    Geometry::new_for(&r.cmd, 0, self.cfg.num_clusters, self.cfg.gpu_generation);
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
                let Some(k) =
                    self.slots
                        .pick_pass_in_for(&job.resources, &geometry, 0, Some(job.vm.id))
                else {
                    self.engine.slot_wait = true;
                    return Err(EAGAIN);
                };
                f.claims.pass[0] = k as u32;
                f.claims.pass_count = 1;
                f.claims.pass_mask = 1u64 << k;
                self.slots.pass_busy |= 1u64 << k;
                self.prefix_render(
                    job,
                    &job.resources,
                    index,
                    r,
                    geometry,
                    &mut prefix,
                    f,
                    behind,
                    class_behind,
                )?;
                f.count = 1;
                // Batch consecutive ready non-InitBM render passes of the same SUBMIT into
                // one TA/FS producer advance, one UMA submitted update, and one SubmitRecord.
                if !f.initbm {
                    let stage_prefix =
                        kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES;
                    let mut last_seq = job.command_order(index).map(|o| o.sequence);
                    let active_renders: usize = self
                        .engine
                        .flights
                        .iter()
                        .map(|fl| fl.renders as usize)
                        .sum();
                    let ta_pending_before = self.ta_pending_passes();
                    let mut wave_heavy =
                        self.ta_wave_cap_for(r.is_heavy_ta()) == sizes::TA_WAVE_PASSES;
                    let wave_cap = if wave_heavy {
                        sizes::TA_WAVE_PASSES
                    } else {
                        sizes::TA_WAVE_PASSES_LIGHT
                    };
                    let mut more_renders = params::render_flights()
                        .saturating_sub(active_renders + 1)
                        .min(sizes::RENDER_SLOTS.saturating_sub(1))
                        .min(wave_cap.saturating_sub(ta_pending_before + 1));
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
                        if !self.msaa_member_allowed(&job.vm, r2) {
                            break;
                        }
                        if r2.is_heavy_ta() && !wave_heavy {
                            if ta_pending_before + prefix.renders as usize + 1
                                > sizes::TA_WAVE_PASSES
                            {
                                break;
                            }
                            wave_heavy = true;
                            more_renders = more_renders.min(
                                sizes::TA_WAVE_PASSES
                                    .saturating_sub(ta_pending_before + prefix.renders as usize),
                            );
                        }
                        let Some(o2) = job.command_order(end) else {
                            break;
                        };
                        let ta_used = prefix.ta_head.wrapping_sub(ta_read) % sizes::QUEUE_CAP;
                        let fs_used = prefix.fs_head.wrapping_sub(fs_read) % sizes::QUEUE_CAP;
                        if ta_used >= sizes::QUEUE_CAP - 6 || fs_used >= sizes::QUEUE_CAP - 6 {
                            break;
                        }
                        let geo2 = Geometry::new_for(
                            &r2.cmd,
                            0,
                            self.cfg.num_clusters,
                            self.cfg.gpu_generation,
                        );
                        let Some(k2) = self.slots.pick_pass_in_for(
                            &job.resources,
                            &geo2,
                            f.claims.pass_mask,
                            Some(job.vm.id),
                        ) else {
                            break;
                        };
                        let staged2 = r2.cmd.flags
                            & kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_STAGE_PREFIXES
                            != 0;
                        if self
                            .wait_compute_stamps_for(
                                job,
                                o2.ta_wait_through[super::command::COMPUTE],
                            )
                            .is_err()
                            || self
                                .wait_compute_stamps_for(
                                    job,
                                    o2.wait_through[super::command::COMPUTE],
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
                            &job.resources,
                            end,
                            r2,
                            geo2,
                            &mut prefix,
                            f,
                            behind,
                            class_behind,
                        )?;
                        last_seq = Some(next_seq);
                        end += 1;
                        f.count = (end - index) as usize;
                        more_renders -= 1;
                    }
                    // Cross-job render horizon: fold subsequent ready render-only jobs of
                    // the same queue and VM into the same adaptive TA wave flight, sharing
                    // one TA/FS producer advance, one UMA submitted update, and one doorbell.
                    // Any non-qualifying job of this queue ends the scan so no later job
                    // overtakes it.
                    if end == job.progress.count() && more_renders > 0 {
                        let mut idx = 0;
                        while more_renders > 0 && idx < self.engine.pending.len() {
                            let j2 = self.engine.pending[idx].clone();
                            idx += 1;
                            if Arc::ptr_eq(&j2, job) || !Arc::ptr_eq(&j2.queue, &job.queue) {
                                continue;
                            }
                            if 1 + f.group.len() >= sizes::HORIZON_JOBS as usize {
                                break;
                            }
                            let n2 = j2.progress.count();
                            let n2_usize = n2 as usize;
                            let Some(first_seq) = last_seq.and_then(|s| s.checked_add(1)) else {
                                break;
                            };
                            if !Arc::ptr_eq(&j2.vm, &job.vm)
                                || j2.long() != job.long()
                                || j2.resources.render_scs() != job.resources.render_scs()
                                || n2 == 0
                                || n2_usize > more_renders
                                || !j2.can_fold_render_horizon_job(first_seq)
                            {
                                break;
                            }
                            let mut test_draw = prefix.draw;
                            let mut ord_ok = true;
                            for _ in 0..n2 {
                                match super::recovery::logic::next_ordinal(test_draw) {
                                    Some(next) => test_draw = next,
                                    None => {
                                        ord_ok = false;
                                        break;
                                    }
                                }
                            }
                            if !ord_ok {
                                break;
                            }
                            let ta_used =
                                (prefix.ta_head.wrapping_sub(ta_read) % sizes::QUEUE_CAP) as usize;
                            let fs_used =
                                (prefix.fs_head.wrapping_sub(fs_read) % sizes::QUEUE_CAP) as usize;
                            let need_ring = n2_usize * (sizes::MAX_RENDER_RING_ENTRIES + 1);
                            if ta_used + need_ring >= sizes::QUEUE_CAP as usize
                                || fs_used + need_ring >= sizes::QUEUE_CAP as usize
                            {
                                break;
                            }
                            let mut picked = [0usize; sizes::TA_WAVE_PASSES_LIGHT];
                            let mut picked_mask = f.claims.pass_mask;
                            let mut precheck_ok = true;
                            let mut job_heavy = false;
                            for k in 0..n2 {
                                let Some(Command::Render(r2)) = command(&j2, k) else {
                                    precheck_ok = false;
                                    break;
                                };
                                if !self.msaa_member_allowed(&job.vm, r2) {
                                    precheck_ok = false;
                                    break;
                                }
                                if r2.is_heavy_ta() {
                                    job_heavy = true;
                                }
                                let Some(o2) = j2.command_order(k) else {
                                    precheck_ok = false;
                                    break;
                                };
                                let geo2 = Geometry::new_for(
                            &r2.cmd,
                            0,
                            self.cfg.num_clusters,
                            self.cfg.gpu_generation,
                        );
                                let Some(slot) = self.slots.pick_pass_in_for(
                                    &job.resources,
                                    &geo2,
                                    picked_mask,
                                    Some(job.vm.id),
                                ) else {
                                    precheck_ok = false;
                                    break;
                                };
                                let staged2 = r2.cmd.flags & stage_prefix != 0;
                                let render_wait_ok = |target: Option<u64>| -> bool {
                                    match target {
                                        None => true,
                                        Some(seq) if seq >= first_seq => {
                                            seq < first_seq + u64::from(k)
                                        }
                                        Some(seq) => self
                                            .wait_stamp_for_with(
                                                &j2,
                                                super::command::RENDER,
                                                Some(seq),
                                                Some(f),
                                            )
                                            .is_ok(),
                                    }
                                };
                                if self
                                    .wait_compute_stamps_for(
                                        &j2,
                                        o2.ta_wait_through[super::command::COMPUTE],
                                    )
                                    .is_err()
                                    || self
                                        .wait_compute_stamps_for(
                                            &j2,
                                            o2.wait_through[super::command::COMPUTE],
                                        )
                                        .is_err()
                                    || !render_wait_ok(o2.wait_through[super::command::RENDER])
                                    || (staged2
                                        && params::early_tiling()
                                        && !render_wait_ok(
                                            o2.ta_wait_through[super::command::RENDER],
                                        ))
                                {
                                    precheck_ok = false;
                                    break;
                                }
                                picked[k as usize] = slot;
                                picked_mask |= 1u64 << slot;
                            }
                            if !precheck_ok
                                || (job_heavy
                                    && !wave_heavy
                                    && ta_pending_before + prefix.renders as usize + n2_usize
                                        > sizes::TA_WAVE_PASSES)
                                || f.group.reserve(1, GFP_NOWAIT).is_err()
                            {
                                break;
                            }
                            let _ = f.group.push_within_capacity(j2.clone());
                            for k in 0..n2 {
                                let Some(Command::Render(r2)) = command(&j2, k) else {
                                    return Err(EINVAL);
                                };
                                let geo2 = Geometry::new_for(
                            &r2.cmd,
                            0,
                            self.cfg.num_clusters,
                            self.cfg.gpu_generation,
                        );
                                let slot = picked[k as usize];
                                let member = prefix.renders as usize;
                                f.claims.pass[member] = slot as u32;
                                f.claims.pass_count += 1;
                                f.claims.pass_mask |= 1u64 << slot;
                                self.slots.pass_busy |= 1u64 << slot;
                                self.prefix_render(
                                    &j2,
                                    &job.resources,
                                    k,
                                    r2,
                                    geo2,
                                    &mut prefix,
                                    f,
                                    behind,
                                    class_behind,
                                )?;
                            }
                            last_seq = Some(first_seq + u64::from(n2 - 1));
                            more_renders -= n2_usize;
                            if job_heavy && !wave_heavy {
                                wave_heavy = true;
                                let ta_in_wave = ta_pending_before + prefix.renders as usize;
                                more_renders = more_renders
                                    .min(sizes::TA_WAVE_PASSES.saturating_sub(ta_in_wave));
                            }
                        }
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
                job.resources.pb_uma_lease(f.pb_slot as usize),
            )?;
        }
        // Reserve lifetime ownership before any counter or producer becomes reachable.
        job.vm.vm.mark_binding_published()?;
        self.slots.verify_held(&f.claims)?;
        self.slots.publish(&mut f.claims)?;
        // Even a failed first store may have become visible. From this boundary preserve
        // every owner and return explicit uncertainty rather than guessing from errno.
        *visible = true;
        if f.msaa {
            job.vm.note_msaa_render();
        }
        let publication = (|| -> Result {
            if render {
                let a = f.pb_slot as usize;
                f.pb.as_ref().ok_or(EINVAL)?.expose();
                job.resources.commit_render(f.count as u32);
                for j2 in f.group.iter() {
                    j2.resources.commit_render(j2.progress.count());
                }
                job.resources
                    .publish_pb_batch(a, f.uma_submitted, f.renders)?;
                self.pools.publish_flight(a);
                // Notifier service dereferences u32 counters; the host ordering ledger
                // remains u64. Eight-byte spacing is retained allocation padding. Always
                // advance both TA and FS thresholds to the newest published draw ordinal
                // so firmware never skips updating `TA_STAMP` on fast tail flights.
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
                self.graph.compute[compute_pipe]
                    .threshold
                    .w32(0, prefix.pipe_n as u32)?;
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
                    &self.graph.compute[compute_pipe].qstate,
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
                        self.graph.compute[compute_pipe].qinfo.va,
                        prefix.c_head,
                        work::compute::EVENTS[compute_pipe] as u8,
                        !self.engine.queue_registered[compute_pipe],
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
            self.engine.pipe_ordinal[compute_pipe] = prefix.pipe_n;
            self.engine.target[compute_pipe] = f.target;
            self.engine.head[compute_pipe] = f.head;
            self.engine.queue_registered[compute_pipe] = true;
            self.engine.pipe_use_seq = self.engine.pipe_use_seq.wrapping_add(1);
            self.engine.pipe_last_vm[compute_pipe] = Some(job.vm.id);
            self.engine.pipe_last_used[compute_pipe] = self.engine.pipe_use_seq;
            self.engine.next_compute_pipe = ((compute_pipe + 1) % sizes::COMPUTE_PIPES) as u8;
        }
        self.engine.events = self.events.compute_events;
        self.engine.stage_seen = self.events.stage_events;
        f.behind = behind;
        self.stat_publish(f);
        // First in-flight command on this render class or compute pipe starts about now;
        // commands queued behind the same class/pipe keep its oldest deadline.
        if render {
            if !class_behind {
                self.renew_render_deadline();
            }
        } else if !pipe_behind {
            self.renew_compute_pipe_deadline(compute_pipe);
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
        } else if !f.group.is_empty() {
            let s = &mut self.engine.stats;
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
        c_waits: [Option<WaitTarget>; sizes::COMPUTE_PIPES],
        pf: &mut Prefix,
        f: &mut Flight,
        _later_render: bool,
    ) -> Result {
        let member = pf.computes as usize;
        if member >= sizes::COMPUTE_SLOTS {
            return Err(EAGAIN);
        }
        let pipe = pf.compute_pipe;
        let n = super::recovery::logic::next_ordinal(pf.n).ok_or(EOVERFLOW)?;
        let pipe_n = super::recovery::logic::next_ordinal(pf.pipe_n).ok_or(EOVERFLOW)?;
        let target = encode::stamp(pipe_n);
        let index = self
            .slots
            .claim_compute_for_pipe(resources, &mut f.claims, Some(job.vm.id), pipe)?
            .ok_or(EAGAIN)?;
        let generation = job.vm.generation;
        self.slots.hold_compute_slot(index, &job.vm, resources)?;
        let s = resources.compute_unit(index)?;
        let g = &self.graph.compute[pipe];
        let before = s.counter.r64(0)?;
        let submitted = before.checked_add(1).ok_or(EOVERFLOW)?;
        let refs = ComputeRefs {
            notifier: g.notifier.va + notifier_page::COMPUTE_RECORD as u64,
            qinfo: g.qinfo.va,
            user_stamp: g.user_stamp.va,
            fw_stamp: g.fw_stamp.va,
            work: s.work.va,
            work_gva: s.work.lower_gva.ok_or(EINVAL)?,
            // Separate device-lifetime firmware-cached microsequence record for this unit index.
            seq: s.seq.va,
            // Separate per-VM uncached UMA manager descriptor inside `s.uma_lease`.
            descriptor_fw: s.descriptor.va,
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
            event: work::compute::EVENTS[pipe],
            // Preserve the already qualified packet bytes while expressing the native
            // fields separately. This compatibility identifier is not proof of native
            // global command-ID allocation or uniqueness after compute-index wrap.
            command_id: (u64::from((n - 1) as u32) << 32) | 1,
            compute_index: (n - 1) as u32,
            work_818: 0,
            target_stamp: target,
            submitted,
        };
        let rt = compute_rt(&cmd.ray, s.seeds);
        let scratch = self.engine.scratch.as_deref_mut().ok_or(ENOMEM)?;
        let w = image(&mut scratch.cw)?;
        let q = image(&mut scratch.cq)?;
        // User results are published coherently after the complete retirement receipt.
        // The firmware's internal timestamp pair remains enabled.
        encode::encode_compute_rt(
            self.init.abi.compute(),
            &refs,
            &cmd.cmd,
            &cmd.attachments,
            &[0; 2],
            job.vm.slot,
            n,
            1,
            &rt,
            w,
            q,
        )
        .map_err(|_| EINVAL)?;
        s.seq.write(0, q)?;
        let full_work = self.slots.compute_needs_full_work(index) || before == 0;
        if full_work {
            if before != 0 {
                // Slots::claim_compute proved a different fully retired unit displaced this
                // descriptor in the same hardware slot. Validate the retained descriptor in place.
                let mut descriptor = [0u8; work::compute::DESCRIPTOR_SIZE];
                s.descriptor.read(0, &mut descriptor)?;
                if encode::validate_compute_descriptor(&refs, &descriptor, before).is_err() {
                    // This is retained allocator identity/completion corruption, not a client
                    // refusal. Keep the unit and its previous VM lease mapped forever; cancelling
                    // preparation would make the corrupt descriptor available to another job.
                    self.slots.quarantine(&mut f.claims);
                    self.sync_quarantine_hints();
                    return Err(EIO);
                }
            }
            s.work.write(0, &w[..work::compute::BODY_ACTIVE_END])?;
        } else {
            // Clean same-VM reuse: `s.descriptor` stays intact in place and was authenticated on
            // retirement for this exact unit and VM, static NOTIFIER (0xc00..0xd18) and tail
            // (0xee0..0xf08) remain intact in place, and the live microsequence lives in
            // firmware-cached `s.seq`. Write only the active head and body before `SEQ_EMBEDDED`.
            s.work.write(0, &w[..work::compute::HEAD_ACTIVE_SIZE])?;
            s.work.write(
                work::compute::BODY_ACTIVE_START,
                &w[work::compute::BODY_ACTIVE_START..work::compute::SEQ_EMBEDDED],
            )?;
        }
        let wait_render = self.cross_engine_wait(job, cmd_index, super::command::RENDER)?;
        let fs2c_bar = if let Some(wait_fs) = wait_render {
            // Check producer InitBM explicitly in addition to the scheduler drain guard so
            // FS -> CDM handoff: downgrade to `BarrierType::Stamp` (type 0) only when neither
            // the waited render nor any live flight carries `InitBM` and the GPU does not need
            // G15's cross-engine hold; G16 FS `FINALIZE` flushes caches before advancing
            // `fw_stamp[1]` (see `cross_engine_barrier_type`).
            let producer_initbm = self.cfg.cross_engine_hold
                || wait_fs.initbm
                || self.engine.flights.iter().any(|fl| fl.initbm);
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
        let mut c2c_mask = 0u32;
        for (p_idx, wait_opt) in c_waits.iter().enumerate() {
            if p_idx == pipe {
                continue;
            }
            if let Some(wait_c) = *wait_opt {
                let producer_initbm =
                    wait_c.initbm || self.engine.flights.iter().any(|fl| fl.initbm);
                let bar_type = cross_engine_barrier_type(producer_initbm, false);
                let bar = encode::barrier(
                    self.graph.compute[p_idx].fw_stamp.va,
                    wait_c.stamp,
                    work::compute::EVENTS[p_idx],
                    target,
                    0,
                    bar_type,
                );
                let off = (1 + p_idx) * work::barrier::SIZE;
                s.barrier.write(off, &bar)?;
                c2c_mask |= 1u32 << p_idx;
            }
        }
        f.uma_submitted = submitted;
        f.csubmitted[member] = submitted;
        f.has_timestamps |= !cmd.timestamp_outputs.is_empty();
        f.rt_compute |= resources.compute_scs() || cmd.is_rt();
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
        let mut rem = c2c_mask;
        while rem != 0 {
            let p_idx = rem.trailing_zeros() as usize;
            rem &= rem - 1;
            g.ring.w64(
                8 * pf.c_head as usize,
                s.barrier.va + ((1 + p_idx) * work::barrier::SIZE) as u64,
            )?;
            pf.c_head = (pf.c_head + 1) % sizes::QUEUE_CAP;
        }
        g.ring.w64(8 * pf.c_head as usize, s.work.va)?;
        pf.c_head = (pf.c_head + 1) % sizes::QUEUE_CAP;
        f.cstamp[member] = target;
        pf.computes += 1;
        pf.n = n;
        pf.pipe_n = pipe_n;
        pf.last_ct = target;
        pf.prev = Prev::Compute;
        Ok(())
    }

    /// Encodes paired TA/FS work and the fragment's explicit TA dependency in one retained
    /// pass unit.
    #[expect(
        clippy::too_many_arguments,
        reason = "Encodes one render member into a shared flight and prefix state."
    )]
    fn prefix_render(
        &mut self,
        job: &Arc<Job>,
        resources: &super::resources::Prepared,
        cmd_index: u32,
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
        let ta_wait_compute = self.wait_compute_stamps_for(
            job,
            order.and_then(|o| o.ta_wait_through[super::command::COMPUTE]),
        )?;
        let fs_wait_compute = self.wait_compute_stamps_for(
            job,
            order.and_then(|o| o.wait_through[super::command::COMPUTE]),
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
        let cross_vm_prev = member == 0
            && class_behind
            && self
                .engine
                .flights
                .iter()
                .rev()
                .find(|fl| fl.renders != 0)
                .is_some_and(|fl| !Arc::ptr_eq(&fl.job.vm, &job.vm));
        let plan_flags = if !staged
            && job.fw_waits.iter().any(|w| {
                w.last_ordinal[super::command::RENDER].is_some_and(|last| {
                    w.queue.completed[super::command::RENDER].load(Ordering::Acquire) < last
                })
            })
        {
            cmd.cmd.flags
                & !kernel::uapi::drm_asahi_render_flags_DRM_ASAHI_RENDER_FRAGMENT_BARRIERS
        } else {
            cmd.cmd.flags
        };
        let plan = plan_render_barriers(
            plan_flags,
            has_prior_render,
            params::early_tiling(),
            prev_fs,
            fs_wait_render.map(|w| w.stamp),
            ta_wait_fs.map(|w| w.stamp),
            [
                ta_wait_compute.map(|w| w.map(|t| t.stamp)),
                fs_wait_compute.map(|w| w.map(|t| t.stamp)),
            ],
            cross_vm_prev,
        );
        // Claim performs only a qualified per-use scrub, never allocator/list reset.
        self.slots
            .claim_pass_in(resources, k, Some(job.vm.id), geometry.tilemap_bytes)?;
        self.slots.hold_render_pass(k, &job.vm, resources)?;
        let (refs, init_buffer) = if member == 0 {
            let (pb_mask, pb_home) = resources.pb_slot_mask()?;
            let heap_blocks = resources.pb_heap_blocks()?;
            let alloc_slot = self
                .pools
                .pick_slot_in_mask_blocks_mode(
                    &job.vm,
                    behind,
                    pb_mask,
                    pb_home,
                    heap_blocks,
                    resources.render_scs(),
                )
                .ok_or(EAGAIN)?;
            let slot_pipelined = self.pools.slot_inflight(alloc_slot) > 0;
            let (refs, init_buffer, epoch, binding) =
                self.render_refs(alloc_slot, job, draw, slot_pipelined, behind)?;
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
                &f.job,
                draw,
                epoch,
                f.uma_submitted,
            )?;
            (refs, false)
        };
        f.has_timestamps |= !cmd.timestamp_outputs.is_empty();
        f.heavy_render |= cmd.is_heavy_ta();
        f.msaa |= cmd.is_multisampled();
        let p = resources.render_pass(k)?;
        let pass = p.refs();
        let heap_bytes = if cmd.cmd.layers > 1 { 0x1000 } else { 0x200 };
        geometry.layer_metadata = pass.heap_gva + if cmd.cmd.layers > 1 { 0x200 } else { 0 };
        geometry.validate_capacity(p.tpc.size as u64, p.tilemap.size as u64)?;
        resources.set_pass_dirty(k, geometry.tpc_bytes, geometry.tilemap_bytes, heap_bytes)?;
        let scratch = self.engine.scratch.as_deref_mut().ok_or(ENOMEM)?;
        let [vertex_rt, fragment_rt] = cmd.ray_query.map(|stage| stage_rt(&stage));
        encode::encode_ta_rt(
            self.init.abi.ta(),
            &refs,
            &pass,
            &cmd.cmd,
            &[0; 4],
            job.vm.slot,
            target,
            &geometry,
            None,
            &vertex_rt,
            image(&mut scratch.ta)?,
            image(&mut scratch.tas)?,
        );
        encode::encode_fs_rt(
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
            &fragment_rt,
            image(&mut scratch.fs)?,
            &mut scratch.fss[..],
        );
        encode::render_attachments(&mut scratch.tas[..], 0, &cmd.vertex_attachments);
        encode::render_attachments_for(
            &mut scratch.fss[..],
            1,
            self.init.abi.fs().finalize,
            &cmd.fragment_attachments,
        );
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
        // Pipelined render->render and cross-engine (CDM->TA, CDM->FS) barriers use
        // `BarrierType::Stamp` (type 0) only when neither producer, any live flight, nor this
        // consumer carries `InitBM`: G16 `FINALIZE` flushes producer caches before advancing
        // `fw_stamp` (unlike G15's `CROSS_ENGINE_HOLD`; see `cross_engine_barrier_type`).
        // Cold `InitBM` transitions and recovery paths drain completely before publishing.
        let producer_initbm = pf.initbm
            || self.engine.flights.iter().any(|fl| fl.initbm)
            || ta_wait_compute.iter().flatten().any(|w| w.initbm)
            || fs_wait_compute.iter().flatten().any(|w| w.initbm)
            || fs_wait_render.is_some_and(|w| w.initbm)
            || ta_wait_fs.is_some_and(|w| w.initbm);
        let render_bar = cross_engine_barrier_type(producer_initbm, init_buffer);
        // Pipelined renders share TA/FS pipes across same-VM and cross-VM flights, so keep
        // the TA(N-1)->TA(N) `prev_barrier` (matching G15) whenever `plan.ta_fs_wait` does not
        // already serialize behind `prev_fs`. Emit `ta_fs_barrier` only when `plan.ta_fs_wait`
        // names an in-flight FS stamp, and `fs_dep` when `plan.fs_render_wait` names an
        // in-flight FS dependency (or cross-VM `prev_fs`) not already dominated by TA.
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
        for p_idx in 0..sizes::COMPUTE_PIPES {
            let off = p_idx * work::barrier::SIZE;
            if let Some(wait_c) = plan.ta_compute_wait[p_idx] {
                let cbar = encode::barrier(
                    self.graph.compute[p_idx].fw_stamp.va,
                    wait_c,
                    work::compute::EVENTS[p_idx],
                    target,
                    work::barrier::RENDER_WAIT_UUID,
                    render_bar,
                );
                p.compute_barrier.write(off, &cbar)?;
                r.ring[0].w64(8 * pf.ta_head as usize, p.compute_barrier.va + off as u64)?;
                pf.ta_head = (pf.ta_head + 1) % cap;
                if render_bar == BarrierType::Stamp {
                    self.engine.stats.c2ta0 += 1;
                }
            }
            if let Some(wait_c) = plan.fs_compute_wait[p_idx] {
                let fcbar = encode::barrier(
                    self.graph.compute[p_idx].fw_stamp.va,
                    wait_c,
                    work::compute::EVENTS[p_idx],
                    target,
                    work::barrier::RENDER_WAIT_UUID,
                    render_bar,
                );
                p.fs_compute_barrier.write(off, &fcbar)?;
                r.ring[1].w64(8 * pf.fs_head as usize, p.fs_compute_barrier.va + off as u64)?;
                pf.fs_head = (pf.fs_head + 1) % cap;
                if render_bar == BarrierType::Stamp {
                    self.engine.stats.c2ta0 += 1;
                }
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

/// The compute register words of a validated command's ray state ([`super::rt`]) in a unit
/// whose UMA pool has `seeds` freelist entries, in a freelist of
/// [`super::rt::freelist_records`]. Without ray state in a base unit these are the words of
/// [`ComputeRt::DISABLED`].
fn compute_rt(ray: &super::rt::ComputeRay, seeds: u32) -> ComputeRt {
    ComputeRt {
        intersection: ray.intersection.words(),
        size: ray.scratch.size_word(),
        tail: ray.scratch.compute_tail_word(),
        stack: ray.scratch.stack_word(),
        window: ray.scratch.window_word(),
        driver: ray.scratch.driver_word(),
        scs: ray.scratch.enabled(),
        seeds,
        records: super::rt::freelist_records(seeds),
    }
}

/// The TA or FS register words of a validated render stage's ray state. As the M4 Pro's
/// render programs, a stage's tail word is its scratch tail; its auxiliary intersection word
/// is validated but not written.
fn stage_rt(stage: &super::rt::RayQueryStage) -> StageRt {
    StageRt {
        intersection: stage.intersection.words(),
        size: stage.scratch.size_word(),
        tail: stage.scratch.tail_word(),
        stack: stage.scratch.stack_word(),
        window: stage.scratch.window_word(),
        scs: stage.scratch.enabled(),
    }
}
