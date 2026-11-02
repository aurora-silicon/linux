// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 render pairs: a tiling and a fragment hardware queue, each an M5 [`Lane`] as a compute
//! channel's is, and the storage a render pass on them names (S `m5_render_test::Pair` over its
//! M5 render memory): the command storage of both stages, the stage stamps and usage records,
//! the parameter buffer ([`buffer::Manager`] on the M5 block geometry), the target records and
//! TPC ([`target::Target`] with M5 slices), the per-pass tiling state, and the pair's private
//! pool ([`M5Pool`], [`pool::Engine::Render`]). The pool starts at what every pass without primary
//! storage needs and grows before a pass whose plan needs more, by the compute lane's rule
//! ([`super::m5::grow_pool`]); a parked pair's pool is vacated as a parked lane's is.
//!
//! Installation follows S: both pool and parameter-buffer rows, both configurations, both QIDs,
//! then the pair's two outer records in one publication (fragment first) and the wait for both
//! queues to stand idle past their configuration. The installed lanes stay with their firmware
//! session and are leased as a matched pair, as a compute channel's lane is:
//! a pair binds them to one logical queue with that owner's storage ([`Storage`]: parameter
//! buffer, target, tiling state, client views), parks once its context's scheduler release is
//! consumed ([`Pair::park`]) and a later queue adopts it ([`Pair::adopt`]). Both lanes keep
//! their kick timestamps, and the pair its pass ordinal, from which the stage stamps follow.

use super::{
    compute::retirement,
    compute::{awaits_witness, Failure},
    m5::{Grown, Host, Lane, PoolSite},
    pool_state::{PoolPhase, PoolState},
};
use crate::g17::{
    buffer::{self, target, BlockGeometry},
    context::{Context, WorkStateLease},
    freelist::{M5Backing, M5Pool},
    fw::{
        self,
        channels::TvbGrowRequest,
        kick::{KickArgs, KickDependency, KickQos},
        m5::{
            compute::{Gpu, RenderPlan},
            pool, queue as m5queue, render as cmd, render_assembly as assembly,
            render_registers as regs,
        },
        queue::*,
        render::{Deflake, RegisterPrograms, RenderArgs, RenderPass, RenderResources, RenderStage},
        variant::PointerLayout,
        ObjectIds,
    },
    job::Packet,
    kick,
    object::{Allocator, CpuMap, KernelObject, Placement},
    qos, recovery,
    status::VmStatus,
    vm_layout::Domain,
};
use crate::{hw::t8140::resources as cfg, mmu};
use core::sync::atomic::{fence, Ordering};
use kernel::{prelude::*, sync::Arc};

const PAGE: usize = mmu::UAT_PGSZ;
/// Passes a pair holds in flight.
pub(crate) const DEPTH: usize = 1;
/// One pass's command storage: the tiling command, then the fragment command at
/// [`fw::m5::render_assembly::FRAGMENT_OFFSET`].
const COMMAND_BYTES: usize = fw::m5::render_assembly::COMMAND_STORAGE_BYTES;
/// Support page: each stage's completion stamp and auxiliary stamp (0x80 apart), then each
/// pass's two usage records.
const STAMPS: usize = 0;
const STAGE_STRIDE: usize = 0x80;
const USAGE: usize = 0x200;
const USAGE_BYTES: usize = 0x40;
/// Each pass's four stage times (tiling start and end, fragment start and end).
const TIMES: usize = 0x400;
const _: () = assert!(USAGE + DEPTH * 2 * USAGE_BYTES <= TIMES && TIMES + DEPTH * 32 <= PAGE);
/// Kick QoS classes and dependency types of the two stages (S `draw_one`).
const QOS_CLASS: [u8; 2] = [12, 24];
const STATE_AUXILIARY: usize = 0x8000;
const VIEWPORT: usize = 0x9c0;
const VIEWPORT_CONTROL: u32 = 0x3000;
/// Per-pass tiling state (S `generation::Layout::M5`): deflake buffer, stage status,
/// auxiliary framebuffer.
const STATE_BYTES: usize = 0x10000;
const STATE_STATUS: usize = 0x4000;
/// Parameter-buffer block, in which the initial inventory is counted.
const PB_BLOCK: usize = 0x80000;
/// The discard target the parameter buffer names.
const DISCARD_BYTES: usize = 0x8000;
/// One tiling group of 64 segments, as S's qualification and its single-group policy.
const GROUPS: u32 = 1;
const SEGMENTS: u32 = 64;
/// Stages, in lane order.
pub(crate) const TILING: usize = 0;
pub(crate) const FRAGMENT: usize = 1;

/// The physical memory the parameter-buffer inventory follows (S `parameter_geometry`).
fn memory_bytes() -> Result<u64> {
    let mut info = core::mem::MaybeUninit::<kernel::bindings::sysinfo>::zeroed();
    // SAFETY: `info` is a complete, writable sysinfo.
    unsafe { kernel::bindings::si_meminfo(info.as_mut_ptr()) };
    // SAFETY: Zeroed above and filled by si_meminfo.
    let info = unsafe { info.assume_init() };
    (info.totalram as u64)
        .checked_mul(u64::from(info.mem_unit))
        .ok_or(EOVERFLOW)
}

/// `result`'s value, or a return of its error naming the step of a pass's publication that
/// refused it: the command fails with only its errno. It takes the `?` itself, matching the
/// result where it was built: through `inspect_err` every step's value (the pass's command
/// inputs among them, over a kilobyte) was copied in and out again on the ioctl's stack.
macro_rules! at {
    ($name:literal, $result:expr) => {
        match $result {
            Ok(value) => value,
            Err(error) => {
                pr_warn!("G17 M5: render pass {} refused: {:?}\n", $name, error);
                return Err(::core::convert::From::from(error));
            }
        }
    };
}

/// `result`'s value, or a return of its error naming `step` of a pair's construction that
/// refused it: construction has many steps that refuse the same way. As [`at!`], it takes the
/// `?` itself, so no step's value (the parameter buffer's manager among them) is copied
/// through an adapter.
macro_rules! step {
    ($name:literal, $result:expr) => {
        match $result {
            Ok(value) => value,
            Err(error) => {
                pr_err!("G17 M5: render pair {} refused: {:?}\n", $name, error);
                return Err(::core::convert::From::from(error));
            }
        }
    };
}

/// One owner's storage of a pair, in that owner's VM: dropped with the binding
/// ([`super::m5::Binding`]), off the device mutex.
pub(crate) struct Storage {
    manager: buffer::Manager,
    target: KBox<target::Target>,
    layout: target::Layout,
    /// The tiling state's stage status as the firmware sees it.
    status_fw: mmu::KernelMapping,
    state: KernelObject,
    scratch: KernelObject,
    discard: KernelObject,
}

pub(crate) struct Pair {
    /// The binding's client views, in the owner's VM.
    aliases: KVec<mmu::KernelMapping>,
    storage: KBox<Storage>,
    context: Arc<Context>,
    owner: u64,
    /// The previous owner's scheduler page and work storage, which the records named until
    /// this binding rewrote them.
    _previous_pages: Option<crate::g17::context::FirmwarePages>,
    lanes: [Lane; 2],
    /// Command storage of both stages and its canonical low view (the kernel's lower VM), and
    /// the support page (stage stamps, usage, times): they stay with the lanes.
    descriptors_lower: mmu::KernelMapping,
    descriptors: KernelObject,
    descriptors_low: u64,
    support: KernelObject,
    pool: M5Pool,
    /// The pool as the firmware sees it, carried to the parked pair and the next binding, as a
    /// compute lane's (`queue::m5`).
    pool_state: PoolState,
    /// The parameter buffer's ID and generation.
    pb: (u8, u64),
    pm_generation: u64,
    /// What each pass's target layout is derived from.
    clusters: u32,
    programs: RegisterPrograms,
    compact: [u64; 2],
    installed: bool,
    /// Passes published on the lanes since installation, across owners: each pass's stage
    /// stamp follows it, so a new owner's first pass never meets its predecessor's stamp.
    ordinal: u64,
    /// The binding's first pass is still to come: it binds the parameter buffer, the pool and
    /// the context's identity (S re-sends the configuration after a rebind).
    unbound: bool,
    /// Set by an adoption until the binding's first pass: whether the class (and so the outer
    /// work rings) changed.
    rebound: Option<bool>,
    /// The class whose outer rings last carried the lanes' first slots.
    class: u8,
    /// Set once this binding's client views are unmapped, after it parks.
    unmapped: super::m5::Unmapped,
    active: Option<Active>,
    /// Set once the pass failed or the pair's state became ambiguous. With `awaiting_witness`,
    /// Neo's compute-queue quarantine fields under their names, as for a lane (`queue::m5`).
    quarantine_error: Option<Error>,
    /// The quarantine is a settled failure of the pass in flight (recovery blamed or cancelled
    /// it: the firmware restarted and resumed it): the pair's state is not in doubt, so the pass's
    /// exact retirement, every [`Self::retire`] witness holding, is the drain witness that ends the
    /// quarantine, as a Neo pair's exact drain ends its slot's
    /// (`Completion::release_after_retirement`). A quarantine from a failure after a
    /// firmware-visible store, a timeout or another pass's VM failure stays, as does one whose
    /// context's release was consumed before the drain ([`Self::end_witness_wait`]). The failed
    /// pass keeps its render lease, quarantined, until it retires (Neo's
    /// `Completion::quarantine_render`); while the drain is awaited the firmware is still running
    /// the pass, so its footprint keeps ordering overlapping renders
    /// (`Completion::quarantine_resumed_render`).
    awaiting_witness: bool,
    /// A growth request of the pass in flight is being served off the device mutex
    /// ([`Self::begin_growth`]): the pair neither parks nor settles until it ends.
    growing: bool,
    /// A restart interrupted the pass in flight before any stage started
    /// ([`Self::hold_unstarted`]): recovery does not fail it with the restart's cancellation, and
    /// once the firmware has retired it without running it ([`Self::retire`]) it is published
    /// again.
    held: bool,
    /// The pass a restart cancelled before it started, retired by the firmware unrun, and when it
    /// was: it is published again ahead of its queue's later passes, which wait for the pair's
    /// room (`Firmware::replay_m5_render`). The firmware holds nothing of it.
    replay: Option<(Arc<Packet>, u64)>,
}

/// An installed render pair without an owner: both lanes, the storage they name across owners,
/// and the previous owner's scheduler page and work storage the records name until the next
/// owner rewrites them. Never dropped while the session runs.
pub(crate) struct ParkedPair {
    lanes: [Lane; 2],
    descriptors_lower: mmu::KernelMapping,
    descriptors: KernelObject,
    descriptors_low: u64,
    support: KernelObject,
    pool: M5Pool,
    pool_state: PoolState,
    compact: [u64; 2],
    ordinal: u64,
    class: u8,
    pages: crate::g17::context::FirmwarePages,
    unmapped: super::m5::Unmapped,
    /// When it parked: the vacate pass releases the least recently parked pools first.
    parked_at: u64,
}

impl ParkedPair {
    /// Its previous binding's client views are unmapped, and no release of its pool is in
    /// flight (`super::compute::Queue::m5_reusable`).
    pub(crate) fn adoptable(&self) -> bool {
        self.unmapped.done() && !self.pool_state.pending()
    }
    /// Its pool is backed and kept: an adoption needs no fresh backing.
    pub(crate) fn backed(&self) -> bool {
        self.pool_state.phase() == PoolPhase::Populated && !self.pool.vacant()
    }
    /// An idle pool the vacate pass may give back (`Registry::vacate_idle_pools`), as a detached
    /// lane's (`super::compute::Queue::m5_vacate_candidate`).
    pub(crate) fn vacate_candidate(&self) -> bool {
        self.backed() && self.unmapped.done()
    }
    /// When the pair parked.
    pub(crate) fn released_at(&self) -> u64 {
        self.parked_at
    }
    /// Releases this parked pair's pool (the caller holds the device mutex and announces a
    /// published record), as a detached lane's (`super::compute::Queue::vacate`): the backing is
    /// freed once the firmware consumed the release ([`Self::observe_vacancy`]); the lanes stay
    /// installed, and the next binding's first pass backs the pool for its plan.
    pub(crate) fn vacate(&mut self, init: &crate::g17::initdata::InitData) -> Result<bool> {
        if !self.vacate_candidate() {
            return Err(EBUSY);
        }
        // Generation 1, as every binding's configuration publishes it.
        self.pool_state.release(init, self.pool.release_record(1))
    }
    /// The backing of a vacant pool no client maps any more, once the firmware consumed its
    /// release; the caller frees it off the device mutex.
    pub(crate) fn observe_vacancy(
        &mut self,
        consumer: u32,
        producer: u32,
    ) -> Result<Option<M5Backing>> {
        self.pool_state.observe(consumer, producer)?;
        if self.pool_state.phase() != PoolPhase::Vacant || !self.unmapped.done() {
            return Ok(None);
        }
        Ok(self.pool.vacate())
    }
    pub(crate) fn pool_release_pending(&self) -> bool {
        self.pool_state.pending()
    }
    pub(crate) fn pool_release_stalled(&self, now: u64) -> bool {
        self.pool_state.stalled(now)
    }
    pub(crate) fn qids(&self) -> [u8; 2] {
        [self.lanes[TILING].qid(), self.lanes[FRAGMENT].qid()]
    }
}

/// A growth of one pair's parameter buffer being served: a firmware request
/// ([`Pair::begin_growth`]) or a pre-growth before a pass ([`Pair::pregrowth`]). What is
/// allocated off the device mutex, and the pair and buffer it returns to.
pub(crate) struct PairGrowth {
    pub(crate) owner: u64,
    pub(crate) vm: mmu::Vm,
    pub(crate) geometry: BlockGeometry,
    pub(crate) from: usize,
    pub(crate) target: usize,
    /// The grown buffer's graph ([`buffer::Manager::state_va`]): a pre-growth commits only to
    /// the binding it was planned for.
    graph: u64,
}

/// A published pass, until its strict retirement.
struct Active {
    ordinal: u64,
    stamp: u32,
    /// Each lane's item-ring producer after the pass.
    targets: [u32; 2],
    pool_target: u64,
    work: Arc<WorkStateLease>,
    packet: Option<Arc<Packet>>,
    qos: [qos::Owner; 2],
}

/// What one pass runs: the validated render-pass state and its plan.
pub(crate) struct Draw<'a> {
    pub(crate) pass: &'a RenderPass,
    pub(crate) plan: RenderPlan,
}

/// A retired pass: its command, if a DRM one, and its stage times (tiling start and end,
/// fragment start and end).
pub(crate) struct Retired {
    pub(crate) packet: Option<Arc<Packet>>,
    pub(crate) times: [u64; 4],
    /// The pass had failed (`Pair::fail`): its error is reported again with the release, so that
    /// a fence its failure never reached signals that error, not success.
    pub(crate) failed: Option<Error>,
}

/// What one pair is built for: its queues and the GPU it runs on, and its first binding.
pub(crate) struct PairArgs<'a> {
    pub(crate) ids: [kick::Id; 2],
    pub(crate) gpu: &'a Gpu,
    pub(crate) pointers: &'a PointerLayout,
    pub(crate) binding: BindingArgs<'a>,
}

/// What one binding of a pair is built for: its owner and parameter buffer and the pass
/// geometry its target is first sized for.
pub(crate) struct BindingArgs<'a> {
    pub(crate) owner: u64,
    pub(crate) metrics: buffer::MetricsLease,
    /// The context's view of the metrics object the lease names, retained with the pair.
    pub(crate) metrics_view: mmu::KernelMapping,
    /// The parameter buffer's ID and allocation token, and the graph's PM generation.
    pub(crate) pb: (u8, u64),
    pub(crate) pm_generation: u64,
    pub(crate) pass: &'a RenderPass,
    pub(crate) clusters: u32,
    pub(crate) programs: RegisterPrograms,
}

impl Storage {
    /// Builds one binding's storage in `context`'s VM, pushing its client views (the metrics
    /// view, the command storage and the pool) to `aliases`. The parameter buffer's scenes
    /// start at the pair's next pass, `first_ordinal`.
    fn new(
        alloc: &Allocator<'_>,
        context: &Context,
        aliases: &mut KVec<mmu::KernelMapping>,
        (descriptors, descriptors_low): (&KernelObject, u64),
        pool: &M5Pool,
        args: BindingArgs<'_>,
        first_ordinal: u64,
    ) -> Result<KBox<Self>> {
        let dev = alloc.dev;
        let vm = context.vm();
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let cpu = CpuMap::WriteCombined;
        let compact = alloc.domain(Domain::Compact);
        aliases.push(args.metrics_view, GFP_KERNEL)?;
        aliases.push(
            descriptors.map_alias(vm, descriptors_low, mmu::PROT_GPU_SHARED_RW)?,
            GFP_KERNEL,
        )?;
        // A vacant pool is mapped when the binding's first pass backs it (`Pair::grow`).
        if !pool.vacant() {
            aliases.push(pool.map_client(vm)?, GFP_KERNEL)?;
        }
        // Target and parameter buffer, both in the client's compact window.
        let layout = step!(
            "layout",
            target::Layout::new(args.pass, args.clusters, args.programs)
        );
        let blocks = layout.tvb_blocks;
        let initial = blocks.checked_mul(PB_BLOCK).ok_or(EOVERFLOW)?;
        let minimum = fw::m5::budget::minimum_parameter_pages(args.clusters).ok_or(EINVAL)?;
        let geometry = step!(
            "parameter geometry",
            BlockGeometry::m5(compact.clone(), memory_bytes()?, initial as u64)
        );
        let layout = step!(
            "target slices",
            layout.with_m5_slices(
                args.pass,
                GROUPS,
                SEGMENTS,
                minimum,
                Pair::grown_bytes(geometry)?,
                compact.clone()
            )
        );
        let target = step!("target", target::Target::new(dev, vm, &layout));
        let (tvb, mappings) = step!(
            "parameter blocks",
            buffer::Tvb::with_geometry(dev, vm, blocks, geometry)
        );
        for mapping in mappings {
            aliases.push(mapping, GFP_KERNEL)?;
        }
        let scratch_bytes = step!(
            "scratch size",
            pool::parameter_scratch_stride(geometry.capacity, 0).map_err(|_| EINVAL)
        )
        .checked_mul(pool::SCENE_TABLE_ENTRIES)
        .ok_or(EOVERFLOW)?;
        let in_compact = |size: usize| {
            let size = size.checked_next_multiple_of(PAGE).ok_or(EOVERFLOW)?;
            KernelObject::new(
                dev,
                vm,
                Placement::In(compact.clone(), cfg::TVB_PAGE),
                size,
                mmu::PROT_GPU_SHARED_RW,
                cpu,
            )
        };
        let scratch = step!("scratch", in_compact(scratch_bytes));
        let discard = step!("discard", in_compact(DISCARD_BYTES));
        let manager = step!(
            "parameter buffer",
            buffer::Manager::new(
                alloc,
                vm,
                tvb,
                args.metrics,
                buffer::ManagerArgs {
                    scratch: scratch.gpu_va(),
                    discard: discard.gpu_va(),
                    buffer_id: args.pb.0,
                    generation: args.pm_generation,
                    first_ordinal,
                },
            )
        );
        let state = step!(
            "tiling state",
            KernelObject::new(
                dev,
                vm,
                Placement::In(compact, cfg::TVB_PAGE),
                STATE_BYTES,
                prot,
                cpu
            )
        );
        let status_fw = step!(
            "status view",
            state.map_range(
                alloc.uat.kernel_vm(),
                STATE_STATUS..STATE_STATUS + PAGE,
                alloc.domain(Domain::Upper),
                PAGE as u64,
                mmu::PROT_FW_SHARED_RW,
                0,
            )
        );
        // Boxed: the parameter buffer's manager is large, and a pair is built on the probe's
        // stack by the render qualification (by value, it overflowed that stack).
        Ok(KBox::new(
            Self {
                manager,
                target,
                layout,
                status_fw,
                state,
                scratch,
                discard,
            },
            GFP_KERNEL,
        )?)
    }
}

impl Pair {
    /// Builds the pair for `context` and maps it into the context's VM. Nothing is visible to
    /// the firmware yet, so a failure here frees everything; the caller cancels the unpublished
    /// QIDs and returns the parameter-buffer ID. Boxed, as a channel is.
    pub(crate) fn new(
        alloc: &Allocator<'_>,
        context: Arc<Context>,
        args: PairArgs<'_>,
    ) -> Result<KBox<Self>> {
        let [tiling, fragment] = args.ids;
        if tiling.engine() != DataMaster::Tiling || fragment.engine() != DataMaster::Fragment {
            return Err(EINVAL);
        }
        let prot = mmu::PROT_GPU_FW_SHARED_RW;
        let cpu = CpuMap::WriteCombined;
        let compact = alloc.domain(Domain::Compact);
        let mut aliases = KVec::with_capacity(16, GFP_KERNEL)?;
        let lanes = [
            Lane::new(alloc, &context, tiling, args.pointers, &mut aliases)?,
            Lane::new(alloc, &context, fragment, args.pointers, &mut aliases)?,
        ];
        // Command storage in the canonical low arena, at the same address in the VM, as a
        // compute descriptor ring.
        let descriptors = alloc.kernel(DEPTH * COMMAND_BYTES, PAGE as u64, prot, cpu)?;
        let descriptors_lower = descriptors.alias_in(
            alloc.uat.kernel_lower_vm(),
            alloc.domain(Domain::Canonical),
            PAGE as u64,
            mmu::PROT_GPU_SHARED_RW,
        )?;
        let descriptors_low = descriptors_lower.iova();
        let support = alloc.kernel(PAGE, PAGE as u64, prot, cpu)?;
        // The pair's pool, at the tiling queue's pool ID, covers every pass without primary
        // storage; a pass that needs more grows it first ([`Self::grow`]).
        let geometry = step!(
            "pool geometry",
            pool::PoolGeometry::of(args.gpu.render_pool_blocks, args.gpu.block_bytes())
                .map_err(|_| EINVAL)
        );
        let pool = step!(
            "pool",
            M5Pool::new(alloc, tiling.qid(), geometry, pool::Engine::Render)
        );
        let binding = args.binding;
        let (owner, pb, pm_generation, clusters, programs) = (
            binding.owner,
            binding.pb,
            binding.pm_generation,
            binding.clusters,
            binding.programs,
        );
        let storage = Storage::new(
            alloc,
            &context,
            &mut aliases,
            (&descriptors, descriptors_low),
            &pool,
            binding,
            0,
        )?;
        let class = context.policy().priority();
        let pair = KBox::new(
            Self {
                aliases,
                storage,
                context,
                owner,
                _previous_pages: None,
                lanes,
                descriptors_lower,
                descriptors,
                descriptors_low,
                support,
                pool,
                pool_state: PoolState::FRESH,
                pb,
                pm_generation,
                clusters,
                programs,
                compact: [compact.start, compact.end],
                installed: false,
                ordinal: 0,
                unbound: true,
                rebound: None,
                class,
                unmapped: super::m5::Unmapped::new()?,
                active: None,
                quarantine_error: None,
                awaiting_witness: false,
                growing: false,
                held: false,
                replay: None,
            },
            GFP_KERNEL,
        )?;
        pair.publish_mappings()?;
        Ok(pair)
    }

    /// Adopts the parked pair in `parked` (its render slot's) for `context`'s logical queue
    ///: builds the binding's storage and client views in the new VM and
    /// rewrites both records' host-owned words. The binding's first pass reconfigures both
    /// queues; the kick timestamps, the stamps and the pass ordinal continue. The caller
    /// installs the binding's parameter-buffer row ([`Self::install_pb`]). On failure the pair
    /// stays parked in `parked`.
    pub(crate) fn adopt(
        alloc: &Allocator<'_>,
        parked: &mut Option<ParkedPair>,
        context: Arc<Context>,
        binding: BindingArgs<'_>,
    ) -> Result<KBox<Self>> {
        let lane = parked.as_mut().ok_or(EINVAL)?;
        if !lane.adoptable() {
            return Err(EAGAIN);
        }
        // A vacant pool's leftover backing (no registration, no client view) goes: the binding's
        // first pass backs the pool for its plan ([`Self::grow`]), as for an adopted lane.
        if lane.pool_state.phase() == PoolPhase::Vacant {
            drop(lane.pool.vacate());
        }
        let unmapped = super::m5::Unmapped::new()?;
        let mut aliases = KVec::with_capacity(16, GFP_KERNEL)?;
        for queue in &mut lane.lanes {
            aliases.push(queue.kick.map_client(context.vm())?, GFP_KERNEL)?;
        }
        let (owner, pb, pm_generation, clusters, programs) = (
            binding.owner,
            binding.pb,
            binding.pm_generation,
            binding.clusters,
            binding.programs,
        );
        let storage = Storage::new(
            alloc,
            &context,
            &mut aliases,
            (&lane.descriptors, lane.descriptors_low),
            &lane.pool,
            binding,
            lane.ordinal,
        )?;
        let slot = KBox::<Self>::new_uninit(GFP_KERNEL)?;
        // Both lanes are idle and their owner's release consumed: no reader of the records
        // remains.
        for queue in &mut lane.lanes {
            queue.set_owner(&context)?;
        }
        let lane = parked.take().ok_or(EINVAL)?;
        let class_changed = lane.class != context.policy().priority();
        let pair = KBox::write(
            slot,
            Self {
                aliases,
                storage,
                context,
                owner,
                _previous_pages: Some(lane.pages),
                lanes: lane.lanes,
                descriptors_lower: lane.descriptors_lower,
                descriptors: lane.descriptors,
                descriptors_low: lane.descriptors_low,
                support: lane.support,
                pool: lane.pool,
                pool_state: lane.pool_state,
                pb,
                pm_generation,
                clusters,
                programs,
                compact: lane.compact,
                installed: true,
                ordinal: lane.ordinal,
                unbound: true,
                rebound: Some(class_changed),
                class: lane.class,
                unmapped,
                active: None,
                quarantine_error: None,
                awaiting_witness: false,
                growing: false,
                held: false,
                replay: None,
            },
        );
        if let Err(error) = pair.publish_mappings() {
            let (lane, _binding) = pair.park();
            *parked = Some(lane);
            return Err(error);
        }
        // The rewritten records name this context's scheduler page.
        pair.context.mark_published();
        Ok(pair)
    }

    /// Installs the binding's parameter-buffer row, before its first pass names it.
    pub(crate) fn install_pb(&self, init: &crate::g17::initdata::InitData) -> Result {
        init.install_m5_pb(self.pb.0, self.pb_row()?)
    }

    /// The binding's parameter-buffer ID, returned when it parks.
    pub(crate) fn buffer_id(&self) -> u8 {
        self.pb.0
    }

    /// The pair is idle: healthy, no pass in flight, both lanes consumed to their producers,
    /// both stage stamps at the last pass's and the pool's count complete.
    pub(crate) fn parkable(&self) -> Result<bool> {
        if !self.installed || self.quarantine_error.is_some() || self.in_flight() || self.growing {
            return Ok(false);
        }
        Self::drained(&self.lanes, &self.support, self.ordinal, &self.pool)
    }

    /// The firmware is done with every pass published on `lanes`: both lanes consumed to their
    /// producers, `pool`'s count complete and both stage stamps at the last pass's (of
    /// `ordinal` passes). What parking and a pool's growth wait for.
    fn drained(
        lanes: &[Lane; 2],
        support: &KernelObject,
        ordinal: u64,
        pool: &M5Pool,
    ) -> Result<bool> {
        for lane in lanes {
            if !lane.idle_at(lane.cursors()?.producer)? {
                return Ok(false);
            }
        }
        if pool.completed_submissions()? != pool.submitted()? {
            return Ok(false);
        }
        if ordinal == 0 {
            return Ok(true);
        }
        fence(Ordering::Acquire);
        let stamp = stamp_of(ordinal - 1);
        Ok(support.word(STAMPS)?.load(Ordering::Relaxed) == stamp
            && support.word(STAMPS + STAGE_STRIDE)?.load(Ordering::Relaxed) == stamp)
    }

    /// Logs the park witnesses ([`Self::parkable`]) one by one, for a stalled release.
    pub(crate) fn report_park(&self) {
        let stamp = |offset| {
            self.support
                .word(offset)
                .map(|word| word.load(Ordering::Relaxed))
        };
        pr_warn!(
            "G17 M5: pair {:?}: quarantine {:?} active {} cursors {:x?}, pool {:?}/{:?}, stamps {:x?} {:x?} for pass {}\n",
            self.qids(), self.quarantine_error, self.active.is_some(), self.cursors(),
            self.pool.completed_submissions(), self.pool.submitted(),
            stamp(STAMPS), stamp(STAMPS + STAGE_STRIDE), self.ordinal,
        );
    }

    /// The pair needs nothing more before its context's release: parkable, or quarantined with
    /// the firmware idle on both rings. A drain still awaited does not hold the release back, as
    /// for a compute lane (`Queue::settled`): the context's healthy lanes park, and this pair
    /// parks only if its pass drained before the release was consumed.
    pub(crate) fn settled(&self) -> Result<bool> {
        if self.growing {
            return Ok(false);
        }
        if self.quarantine_error.is_none() {
            return self.parkable();
        }
        for lane in &self.lanes {
            if !lane.idle_at(lane.cursors()?.producer)? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Splits a parked pair ([`Self::parkable`], its context's release consumed) into its lanes
    /// and its binding. The lanes keep the context's scheduler page and work storage, which the
    /// records name until the next owner rewrites them; the caller returns the binding's
    /// parameter-buffer ID.
    pub(crate) fn park(self: KBox<Self>) -> (ParkedPair, super::m5::Binding) {
        let Self {
            aliases,
            storage,
            context,
            owner,
            lanes,
            descriptors_lower,
            descriptors,
            descriptors_low,
            support,
            pool,
            pool_state,
            compact,
            ordinal,
            class,
            unmapped,
            ..
        } = KBox::into_inner(self);
        let pages = context.retain_firmware_pages();
        (
            ParkedPair {
                lanes,
                descriptors_lower,
                descriptors,
                descriptors_low,
                support,
                pool,
                pool_state,
                compact,
                ordinal,
                class,
                pages,
                unmapped: unmapped.clone(),
                parked_at: crate::g17::now_ns(),
            },
            super::m5::Binding::render(aliases, storage, context, owner, unmapped),
        )
    }

    /// An adopted pair whose class differs from its previous binding's, until its first pass.
    pub(crate) fn class_changed(&self) -> bool {
        self.rebound == Some(true)
    }

    /// The pass ordinal the pair's next pass takes.
    pub(crate) fn next_ordinal(&self) -> u64 {
        self.ordinal
    }

    /// Publishes the complete mapping set to every context naming its roots, before any record
    /// names it.
    fn publish_mappings(&self) -> Result {
        self.descriptors_lower.publish_to_contexts();
        for alias in &self.aliases {
            alias.publish_to_contexts();
        }
        let storage = &self.storage;
        for object in [
            &self.lanes[TILING].graph,
            &self.lanes[FRAGMENT].graph,
            &self.descriptors,
            &self.support,
            &storage.state,
            &storage.scratch,
            &storage.discard,
        ] {
            object.publish_to_contexts()?;
        }
        self.pool.publish_to_contexts()
    }

    pub(crate) fn qids(&self) -> [u8; 2] {
        [self.lanes[TILING].qid(), self.lanes[FRAGMENT].qid()]
    }
    pub(crate) fn owner(&self) -> u64 {
        self.owner
    }
    pub(crate) fn context(&self) -> &Arc<Context> {
        &self.context
    }
    pub(crate) fn installed(&self) -> bool {
        self.installed
    }
    pub(crate) fn quarantined(&self) -> Option<Error> {
        self.quarantine_error
    }
    /// The pool's backed blocks.
    pub(crate) fn pool_blocks(&self) -> usize {
        self.pool.blocks()
    }
    pub(crate) fn parameter_blocks(&self) -> usize {
        self.storage.manager.blocks()
    }

    /// The installation configuration of `stage`: unbound, as S installs it; the first pass's
    /// update binds the parameter buffer and the pool.
    fn install_config(&self, stage: usize) -> Result<QueueConfig> {
        let lane = &self.lanes[stage];
        m5queue::config(
            &QueueConfigArgs {
                target: match stage {
                    TILING => QueueConfigTarget::TilingUnbound,
                    _ => QueueConfigTarget::Fragment,
                },
                kick_ring_va: lane.kick.low_va(),
                kick_ring_fw_va: lane.kick.firmware_va(),
                qid: lane.qid(),
                install: true,
                completion_seed: None,
                context_id: self.context.id().try_into()?,
                free_list: FreeListBinding {
                    va: 0,
                    generation: u64::MAX,
                    slot: u32::MAX,
                },
                scheduler_va: self.context.scheduler_va(),
                policy: self.context.policy(),
                qos_slot: self.context.qos_id(),
                qos_update: true,
                owner_pid: self.context.owner_pid(),
                context_update: false,
            },
            m5queue::ConfigProfile {
                soft_fault_enabled: false,
                extended: false,
                parameter_manager: None,
            },
        )
    }

    /// The parameter buffer's descriptor row (S `reload_descriptor` over the page list).
    fn pb_row(&self) -> Result<[u32; 4]> {
        let manager = &self.storage.manager;
        let pages = u32::try_from(manager.blocks() * manager.geometry().pages())?;
        Ok(fw::buffer::reload_descriptor(
            [0; 4],
            self.storage.manager.page_list_va(),
            pages,
        ))
    }

    /// Installs the pair: its pool and parameter-buffer rows, both configurations and QIDs,
    /// both outer records (fragment first) and the wait for both queues to stand idle past
    /// their configuration. From the publication on the pair is retained whatever happens; a
    /// failed installation leaves it quarantined.
    pub(crate) fn install(&mut self, host: &mut Host<'_>) -> Result {
        if self.installed {
            return Err(EINVAL);
        }
        let priority = self.context.policy().priority();
        let claims = [
            host.port.reserve_compute(priority)?,
            host.port.reserve_compute(priority)?,
        ];
        host.init
            .install_m5_pool(self.pool.id(), self.pool.descriptor()?)?;
        host.init.install_m5_pb(self.pb.0, self.pb_row()?)?;
        for stage in [TILING, FRAGMENT] {
            let config = self.install_config(stage)?;
            self.lanes[stage].stage_install(config)?;
        }
        let records = [
            self.lanes[FRAGMENT].work_slot(1, true)?,
            self.lanes[TILING].work_slot(1, true)?,
        ];
        for lane in &self.lanes {
            host.qids.publish(lane.id)?;
        }
        self.installed = true;
        let lanes = &self.lanes;
        let result = host
            .port
            .publish_m5_pair(claims, &records, None, || {
                lanes[FRAGMENT].set_producer(1)?;
                lanes[TILING].set_producer(1)
            })
            .and_then(|_| {
                super::m5::await_install(host.crashed, |_| {
                    let tiling = lanes[TILING].idle_at(1)?;
                    let fragment = lanes[FRAGMENT].idle_at(1)?;
                    Ok(tiling && fragment)
                })
            });
        if let Err(error) = result {
            self.quarantine_error = Some(error);
        }
        result
    }

    /// The cursors of both lanes, for diagnostics.
    pub(crate) fn cursors(&self) -> Result<[retirement::Cursors; 2]> {
        Ok([
            self.lanes[TILING].cursors()?,
            self.lanes[FRAGMENT].cursors()?,
        ])
    }
}

impl Pair {
    /// The target layout of `pass` on this pair (S `m5_render_submission::layout`).
    fn layout(&self, pass: &RenderPass) -> Result<target::Layout> {
        let layout = target::Layout::new(pass, self.clusters, self.programs)?;
        let groups = if pass.few_primitives {
            GROUPS
        } else {
            self.clusters
        };
        let minimum = fw::m5::budget::minimum_parameter_pages(self.clusters).ok_or(EINVAL)?;
        layout.with_m5_slices(
            pass,
            groups,
            SEGMENTS,
            minimum,
            Self::grown_bytes(self.storage.manager.geometry())?,
            self.compact[0]..self.compact[1],
        )
    }

    /// The parameter-buffer extent a pass's target slices are sized for: the whole inventory,
    /// so that growth during the pass ([`Self::begin_growth`]) never outgrows the target storage
    /// the pass already names.
    fn grown_bytes(geometry: BlockGeometry) -> Result<usize> {
        geometry.max_blocks().checked_mul(PB_BLOCK).ok_or(EOVERFLOW)
    }

    /// The pass's register controls (S `m5_render_submission::controls`) with the plan's stage
    /// words.
    fn controls(pass: &RenderPass, plan: &RenderPlan, clusters: u32) -> regs::M5RenderControls {
        let topology = |base| fw::m5::compute::dispatch_control(base, clusters);
        let stage = |plan: fw::m5::compute::StagePlan| regs::M5RenderStageControls {
            scs: regs::StageScratch::NONE,
            resource_flags: plan.resource_flags,
            resource_control: 0x2_0000_0001,
            ptc_max: plan.ptc_max,
            work_key: 0,
        };
        regs::M5RenderControls {
            fragment_event_control: regs::fragment_event_control(true, 1),
            ta: stage(plan.stages[TILING]),
            fragment: stage(plan.stages[FRAGMENT]),
            ta_tile_control: topology(0x10c0_8860),
            fragment_tile_control: topology(fw::UNK_1C8F8),
            ta_dispatch_control: 0x700,
            fragment_raster_control: 0x2000_00ff_0200,
            fragment_tile_flags: 0x20_0003_717f,
            store_pipeline_2_bind: pass.eot_rsrc_spec << 32,
            store_pipeline_2: pass.usc_base + u64::from(pass.eot_offset & !7),
            partial_store_pipeline_2_bind: pass.partial_eot_rsrc_spec << 32,
            partial_store_pipeline_2: pass.usc_base + u64::from(pass.partial_eot_offset & !7),
            partial_tile_flags: 0x20_0000,
            aux_fb_flags: 0x10_c000
                | u64::from(pass.few_primitives)
                | (u64::from(pass.depth_bias_is_int) << 18),
            aux_fb_control: 0x10_0000,
        }
    }

    /// The command inputs of a pass (S `m5_render_submission::inputs`).
    fn inputs(
        args: &RenderArgs<'_>,
        usage: [u64; 2],
        context: u16,
        generation: u8,
    ) -> Result<cmd::M5RenderCommandInputs> {
        let r = args.resources;
        let mut fields = cmd::M5RenderCommandInputs::default();
        let token = fw::m5::render_tail::context_token(u32::from(context), u64::from(generation))
            .ok_or(EINVAL)?;
        fields.ta.d1298 = token;
        fields.fragment.d158 = token;
        fields.ta.resolved_tail_value = usage[0];
        fields.fragment.resolved_tail_value = Some(usage[1]);
        fields.scheduler_record = r.kick_record_va;
        fields.ta.d1177 = cmd::shader_write_class(false, 0);
        fields.ta.d0938 = cmd::ta_word_938(0);
        fields.ta.sequence = args
            .ordinal
            .checked_mul(2)
            .and_then(|n| n.checked_add(1))
            .ok_or(EOVERFLOW)?;
        fields.fragment.sequence = args.ordinal.checked_mul(2).ok_or(EOVERFLOW)?;
        fields.fragment.pass = 1;
        fields.ta.d08a4 = r.ta_ids.predecessor;
        fields.fragment.d08a4 = r.fragment_ids.predecessor;
        fields.fragment.dc68 = 1;
        fields.fragment.dc6c = 1;
        fields.fragment.d0518[22][1] = 0x0404_0404;
        fields.fragment.d08c5_flags = [0; 4];
        fields.ta.d08b0 = r.timestamp_va;
        fields.ta.diagnostic_ta_timestamp_upper = r.timestamp_va + 8;
        fields.fragment.d08b0 = [r.timestamp_va + 16, r.timestamp_va + 24];
        let stamp = stamp_of(args.ordinal);
        fields.ta.linked_3d = cmd::M5TaLinked3d {
            queue_index: u32::from(args.fragment.qid),
            stamp,
            ..Default::default()
        };
        let completion = |stage: &RenderStage, key| cmd::M5StageCompletion {
            stamp_addresses: [stage.stamp_va, stage.aux_stamp_va],
            stamp,
            queue_index: u32::from(stage.qid),
            ordinal: args.ordinal as u32,
            key,
            ..Default::default()
        };
        fields.ta.completion = completion(&args.ta, u64::from(r.ta_ids.current));
        fields.fragment.completion = completion(&args.fragment, u64::from(r.fragment_ids.current));
        fields.ta.pb_slot = r.pb_slot;
        fields.ta.pb_state = r.pb_va;
        fields.fragment.pm_state = r.pb_va;
        fields.ta.pm_scene = r.scene_va;
        fields.fragment.pm_scene = r.scene_va;
        fields.ta.pm_b8 = r.scene_end_va;
        fields.fragment.pm_b8 = r.scene_end_va;
        fields.ta.uma_state = r.free_list_control_va;
        fields.fragment.uma_state = r.free_list_control_va;
        fields.ta.d1444 = 1;
        fields.fragment.dbc4 = 1;
        // The pair serves its growth requests ([`Self::begin_growth`]).
        fields.ta.host_growth = 1;
        Ok(fields)
    }

    /// The bound configuration of `stage` for the first pass: the parameter buffer and the pool,
    /// soft faults on, the first kick's completion seed and the context update (S `draw_one`).
    fn bound_config(&self, stage: usize, payload: u8) -> Result<QueueConfig> {
        let lane = &self.lanes[stage];
        let pb_slot = u32::from(self.pb.0);
        m5queue::config(
            &QueueConfigArgs {
                target: match stage {
                    TILING => QueueConfigTarget::Tiling(TilingBinding {
                        pb_va: self.storage.manager.state_va(),
                        pb_token: self.pb.1,
                        pb_slot,
                        fragment_qid: self.lanes[FRAGMENT].qid(),
                    }),
                    _ => QueueConfigTarget::Fragment,
                },
                kick_ring_va: lane.kick.low_va(),
                kick_ring_fw_va: lane.kick.firmware_va(),
                qid: lane.qid(),
                // An adopted pair whose class changed is reinstalled on its new class, as a
                // compute lane is (`Queue::config` in `queue::m5`).
                install: self.rebound == Some(true),
                completion_seed: Some(
                    lane.kick
                        .timestamp()
                        .prev()
                        .completion_value(payload.wrapping_sub(1)),
                ),
                context_id: self.context.id().try_into()?,
                free_list: FreeListBinding {
                    va: self.pool.control_va(),
                    generation: 1,
                    slot: u32::from(self.pool.id()),
                },
                scheduler_va: self.context.scheduler_va(),
                policy: self.context.policy(),
                qos_slot: self.context.qos_id(),
                qos_update: self.rebound.is_some(),
                owner_pid: self.context.owner_pid(),
                context_update: true,
            },
            m5queue::ConfigProfile {
                soft_fault_enabled: true,
                extended: false,
                parameter_manager: (stage == TILING).then_some(pb_slot),
            },
        )
    }

    /// No pass in flight and none awaiting its replay: a queue's later pass waits for the pass
    /// a restart cancelled to run again first.
    pub(crate) fn has_room(&self) -> bool {
        !self.in_flight()
    }

    /// Whether a pass whose plan needs `blocks` must back the pool first ([`Self::grow`]).
    pub(crate) fn needs_pool(&self, blocks: usize) -> bool {
        super::m5::needs_pool(&self.pool, &self.pool_state, blocks)
    }

    /// Backs the pair's pool for a pass whose plan needs `blocks` (spills and private arrays of
    /// its vertex and fragment programs), before that pass is published, by the compute lane's
    /// rule ([`super::m5::grow_pool`]): between `gpu`'s fresh render pool and the inventory, once
    /// the pair is idle as it is to park ([`Self::drained`]: both lanes consumed, the pool's
    /// count complete, both stage stamps at the last pass's). Both stages name the pool, and the
    /// next pass binds the new backing with both configurations ([`Self::publish`]). Returns the
    /// old backing and view, which the caller drops off the device mutex.
    pub(crate) fn grow(
        &mut self,
        alloc: &Allocator<'_>,
        host: &mut Host<'_>,
        notify: impl FnOnce() -> Result,
        gpu: &Gpu,
        blocks: usize,
    ) -> Result<Option<Grown>> {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        if !self.installed {
            return Err(EINVAL);
        }
        if self.in_flight() || self.growing {
            return Err(EAGAIN);
        }
        let (lanes, support, ordinal) = (&self.lanes, &self.support, self.ordinal);
        let idle = |pool: &M5Pool| Self::drained(lanes, support, ordinal, pool);
        let site = PoolSite {
            qid: lanes[TILING].qid(),
            pool: &mut self.pool,
            state: &mut self.pool_state,
            vm: self.context.vm(),
            views: super::m5::PoolView::Aliases(&mut self.aliases),
            quarantine: &mut self.quarantine_error,
        };
        let grown = super::m5::grow_pool(
            alloc,
            host.init,
            notify,
            site,
            idle,
            blocks,
            (gpu.render_pool_blocks, gpu.max_pool_blocks),
        )?;
        if let Some(grown) = &grown {
            pr_info!(
                "G17 M5: render pair {:?}: pool backed with {} blocks (from {}) for queue {}\n",
                self.qids(),
                grown.to,
                grown.from,
                self.owner
            );
        }
        Ok(grown)
    }

    /// An idle bound pair's pool that backing pressure may give back, as Neo's
    /// `Registry::reclaim_idle_render_pools` gives back an idle bound pair's
    /// (`Registry::vacate_idle_pools` at cap 0): healthy and installed, no pass in flight, held
    /// or awaiting its replay, no growth, the pool backed, and the firmware done with every
    /// pass as for parking ([`Self::drained`]). The pair keeps its lanes, QIDs, binding and
    /// owner; its next pass backs the pool again for its plan ([`Self::grow`]).
    pub(crate) fn vacate_candidate(&self) -> bool {
        self.installed
            && self.quarantine_error.is_none()
            && !self.in_flight()
            && !self.growing
            && !self.held
            && self.pool_state.phase() == PoolPhase::Populated
            && !self.pool.vacant()
            && Self::drained(&self.lanes, &self.support, self.ordinal, &self.pool).unwrap_or(false)
    }
    /// Releases the bound pair's pool ([`Self::vacate_candidate`]; the caller holds the device
    /// mutex and announces a published record), as a parked pair's
    /// ([`ParkedPair::vacate`]).
    pub(crate) fn vacate(&mut self, init: &crate::g17::initdata::InitData) -> Result<bool> {
        if !self.vacate_candidate() {
            return Err(EBUSY);
        }
        // Generation 1, as every binding's configuration publishes it.
        self.pool_state.release(init, self.pool.release_record(1))
    }
    /// Once the firmware consumed the release of the bound pair's pool, its backing and the
    /// client's view of it, which the caller drops off the device mutex, the view first. A
    /// growth that backed the pool again first (`grow_pool`, which waits for the same
    /// consumption) leaves nothing here.
    pub(crate) fn observe_vacancy(
        &mut self,
        consumer: u32,
        producer: u32,
    ) -> Result<Option<(Option<mmu::KernelMapping>, M5Backing)>> {
        self.pool_state.observe(consumer, producer)?;
        if self.pool_state.phase() != PoolPhase::Vacant || self.pool.vacant() {
            return Ok(None);
        }
        let va = self.pool.backing_va()?;
        let view = match self.aliases.iter().position(|alias| alias.iova() == va) {
            Some(index) => Some(self.aliases.remove(index).map_err(|_| EIO)?),
            None => None,
        };
        Ok(self.pool.vacate().map(|backing| (view, backing)))
    }
    /// A release of the pool is in flight (the event worker reports its consumption as
    /// progress).
    pub(crate) fn pool_release_pending(&self) -> bool {
        self.pool_state.pending()
    }
    pub(crate) fn pool_release_stalled(&self, now: u64) -> bool {
        self.pool_state.stalled(now)
    }
    /// The event worker polls this pair's retirement: a healthy pass in flight, or one awaiting
    /// its replay. A quarantined pair's pass is not in flight, as a quarantined Neo queue's
    /// commands are not (`Queue::in_flight`): each event pass checks its drain ([`Self::retire`])
    /// without keeping the worker polling, so the idle check can reach idle.
    pub(crate) fn polls(&self) -> bool {
        self.replay.is_some() || (self.active.is_some() && self.quarantine_error.is_none())
    }
    pub(crate) fn in_flight(&self) -> bool {
        self.active.is_some() || self.replay.is_some()
    }

    // An M5 pair serves the shared growth worker (`runtime/render/grow.rs`) through these three,
    // beside Neo's render entries: a twin of their growth while M5 pairs are not render
    // entries. Its two hooks are the `growing` flag, which stands for the worker's borrow of a
    // Neo pair, and the `aliases` install in `finish_growth`, which stands for Neo's VM
    // render-pool cache.

    /// Whether growth request `request` names this pair's parameter buffer: its ID, and its
    /// context's VM slot unless the request names none (as `RenderState::grow_owner`).
    pub(crate) fn grows(&self, request: &TvbGrowRequest) -> bool {
        u32::from(self.pb.0) == request.buffer_slot
            && (request.vm_slot == -1 || request.vm_slot as u32 == self.context.id())
    }

    /// Starts serving a growth request of the pass in flight, which the firmware holds until
    /// the reply: the pair stays in place, marked growing, while the worker allocates
    /// `target - from` blocks off the device mutex (`buffer::Tvb::allocate_growth`) for
    /// [`Self::finish_growth`]. ENOENT without a pass in flight or with one already served.
    pub(crate) fn begin_growth(&mut self) -> Result<PairGrowth> {
        if self.active.is_none() || self.growing || self.quarantine_error.is_some() {
            return Err(ENOENT);
        }
        let target = self.storage.manager.firmware_target();
        let growth = self.growth_to(target);
        self.growing = true;
        Ok(growth)
    }

    /// A growth of this binding's parameter buffer to `target` blocks.
    fn growth_to(&self, target: usize) -> PairGrowth {
        let manager = &self.storage.manager;
        PairGrowth {
            owner: self.owner,
            vm: self.context.vm().clone(),
            geometry: manager.geometry(),
            from: manager.blocks(),
            target,
            graph: manager.state_va(),
        }
    }

    /// Whether the pair is idle for a pre-growth: installed and healthy, no pass in flight or
    /// growth being served, both lanes consumed to their producers (as [`Self::publish`]
    /// requires of a pass).
    fn idle(&self) -> Result<bool> {
        if !self.installed || self.quarantine_error.is_some() || self.in_flight() || self.growing {
            return Ok(false);
        }
        for lane in &self.lanes {
            if !lane.idle_at(lane.cursors()?.producer)? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// The pre-growth `pass` needs before its publication (S `host_growth_target`): the
    /// parameter buffer grows to the pass layout's blocks and, from the binding's second pass
    /// on, to the high-water feedback of the passes before it, when it holds less. A binding's
    /// buffer is sized for its first pass, so a later, larger pass (more tiles or layers) needs
    /// this. None when the buffer holds enough, or while the pair is not idle: publication then
    /// waits for room. The caller allocates `target - from` blocks off the device mutex
    /// (`buffer::Tvb::allocate_growth`) for [`Self::finish_pregrowth`].
    pub(crate) fn pregrowth(&mut self, pass: &RenderPass) -> Result<Option<PairGrowth>> {
        if !self.idle()? {
            return Ok(None);
        }
        let required = target::Layout::new(pass, self.clusters, self.programs)?.tvb_blocks;
        let manager = &mut self.storage.manager;
        let feedback = if self.unbound {
            0
        } else {
            manager.automatic_target()?.unwrap_or(0)
        };
        let target = required.max(feedback);
        if target <= manager.blocks() {
            return Ok(None);
        }
        Ok(Some(self.growth_to(target)))
    }

    /// Ends [`Self::pregrowth`] under the device mutex. When the pair is still idle with the
    /// same binding at `growth.from` blocks, it publishes `blocks` as an idle-host growth (S's
    /// pre-growth, Neo's idle path): the block table, the page list rebuilt from the read
    /// cursor, the buffer state and the cursors (`buffer::Manager::commit_growth` with the
    /// binding's descriptor row), then the row reloaded for the new page count, before the pass
    /// names it. The blocks' mappings join the binding's `aliases`. Returns whether the heap
    /// grew; Ok(false) leaves everything unchanged. Whatever is not published stays in `blocks`
    /// or `prepared`, for the caller to drop off the mutex.
    pub(crate) fn finish_pregrowth(
        &mut self,
        init: &crate::g17::initdata::InitData,
        growth: &PairGrowth,
        blocks: &mut Option<buffer::TvbGrowth>,
        prepared: &mut Option<buffer::PreparedGrowth>,
    ) -> Result<bool> {
        let manager = &self.storage.manager;
        if blocks.is_none()
            || growth.owner != self.owner
            || growth.graph != manager.state_va()
            || growth.from != manager.blocks()
            || !self.idle()?
        {
            return Ok(false);
        }
        // Read (and so bounds-checked) before the heap changes: the reload cannot fail after.
        let row = init.m5_pb_row(self.pb.0)?;
        let row = self
            .commit_blocks(growth, blocks, prepared, Some(row))?
            .ok_or(EIO)?;
        init.install_m5_pb(self.pb.0, row)?;
        Ok(true)
    }

    /// Plans `blocks` into the heap and commits them (`buffer::Manager::accept_growth`, then
    /// `commit_growth`), their mappings joining `aliases`: a firmware-request growth without
    /// `descriptor`, an idle-host growth with it, whose reloaded row is returned.
    fn commit_blocks(
        &mut self,
        growth: &PairGrowth,
        blocks: &mut Option<buffer::TvbGrowth>,
        prepared: &mut Option<buffer::PreparedGrowth>,
        descriptor: Option<[u32; 4]>,
    ) -> Result<Option<[u32; 4]>> {
        self.aliases
            .reserve(growth.target - growth.from, GFP_KERNEL)?;
        let manager = &mut self.storage.manager;
        *prepared = Some(manager.accept_growth(
            self.context.vm(),
            blocks,
            growth.from,
            growth.target,
            descriptor,
        )?);
        let aliases = &mut self.aliases;
        manager.commit_growth(prepared, |_, _, mappings| {
            while let Some(mapping) = mappings.pop() {
                aliases.push_within_capacity(mapping).map_err(|_| ENOMEM)?;
            }
            Ok(())
        })
    }

    /// Ends [`Self::begin_growth`]. With `commit` (the request is still current) it publishes
    /// `blocks` as a firmware-request growth: the block table, then the committed cursor; the
    /// page list and the descriptor stay the firmware's, which takes the blocks into the live
    /// ring when it reads the reply. The blocks' mappings join the binding's `aliases`.
    /// Whatever is not published stays in `blocks` or `prepared`, for the caller to drop off
    /// the mutex. Returns whether the heap grew.
    pub(crate) fn finish_growth(
        &mut self,
        growth: &PairGrowth,
        blocks: &mut Option<buffer::TvbGrowth>,
        prepared: &mut Option<buffer::PreparedGrowth>,
        commit: bool,
    ) -> Result<bool> {
        if !self.growing {
            return Err(EIO);
        }
        self.growing = false;
        if !commit || blocks.is_none() {
            return Ok(false);
        }
        self.commit_blocks(growth, blocks, prepared, None)?;
        pr_info!(
            "G17 M5: render pair of queue {}: parameter buffer grew from {} to {} blocks\n",
            self.owner,
            growth.from,
            growth.target
        );
        Ok(true)
    }

    /// Publishes one pass (S `draw_one`): its target records, tiling state and scene, both
    /// commands, both kicks, the inner items and the pair's outer records with the host
    /// command. Everything fallible precedes the first firmware-visible store; a failure after
    /// it quarantines the pair. EAGAIN: a pass is in flight, or no room yet.
    pub(crate) fn publish(
        &mut self,
        dev: &crate::driver::AsahiDrmDevice,
        host: &mut Host<'_>,
        draw: &Draw<'_>,
        work: &Arc<WorkStateLease>,
        packet: Option<Arc<Packet>>,
    ) -> Result {
        if let Some(error) = self.quarantine_error {
            return Err(error);
        }
        if !self.installed {
            return Err(EINVAL);
        }
        if self.active.is_some() {
            return Err(EAGAIN);
        }
        // A pass that needs a larger pool, or a pool at all, backs it first ([`Self::grow`]).
        if self.needs_pool(draw.plan.pool_blocks) {
            return Err(EINVAL);
        }
        // The binding's first pass, not the lanes': it binds this owner's parameter buffer,
        // pool and context.
        let first = self.unbound;
        // A later pass after a growth rebinds the grown pool with the same configurations, its
        // completion value continuing, as a compute lane's next kick does (`Queue::transact`).
        let bind = first || !self.pool_state.registered();
        for lane in &self.lanes {
            let cursors = lane.cursors()?;
            if !lane.idle_at(cursors.producer)? {
                return Err(EAGAIN);
            }
        }
        let layout = at!("layout", self.layout(draw.pass));
        // The caller pre-grows the buffer for the pass under the same hold of the device mutex
        // ([`Self::pregrowth`]): a pass larger than the buffer is never published.
        if layout.tvb_blocks > self.storage.manager.blocks() {
            pr_warn!("G17 M5: render pass of {} blocks reached publication without its pre-growth ({} blocks)\n",
                layout.tvb_blocks, self.storage.manager.blocks());
            return Err(EINVAL);
        }
        let priority = self.context.policy().priority();
        let claims = [
            host.port.reserve_compute(priority)?,
            host.port.reserve_compute(priority)?,
        ];
        let ordinal = self.ordinal;
        let slot = (ordinal as usize) % DEPTH;
        let pair = self.lanes[TILING].qid();
        let work_slot = at!("work slot", work.render_slot(pair));
        let payload = at!("payload", work.channel_payload(pair, ordinal));
        let qids = self.qids();
        // The pass's storage: target records, tiling state, scene.
        let storage = &mut self.storage;
        // The target fits each pass, as on the other render queues; the parameter buffer was
        // grown for it before publication ([`Self::pregrowth`]).
        let owners = storage.target.owners();
        at!(
            "target capacity",
            storage
                .target
                .ensure_capacity(dev, self.context.vm(), &layout, &owners)
        );
        let targets = at!("target", storage.target.prepare(ordinal, &layout));
        let extra = storage.target.m5_extra(&layout)?;
        let viewport = fw::render::storage::Viewport::new(draw.pass.width, draw.pass.height)?
            .with_control(VIEWPORT_CONTROL);
        {
            let bytes = storage.state.bytes_mut();
            bytes[STATE_STATUS..STATE_STATUS + PAGE].fill(0);
            bytes[STATE_AUXILIARY..STATE_AUXILIARY + fw::render::storage::AUXILIARY_SIZE].fill(0);
        }
        let view = storage
            .state
            .pointer(VIEWPORT, size_of::<fw::render::storage::Viewport>())?
            .cast::<fw::render::storage::Viewport>();
        if view.align_offset(align_of::<fw::render::storage::Viewport>()) != 0 {
            return Err(EINVAL);
        }
        // SAFETY: The integer-only view is aligned, inside the retained mapping, and the pass's
        // tiling state is unpublished: the previous pass retired.
        unsafe { viewport.apply(&mut *view) };
        let scene = at!("scene", storage.manager.prepare_scene(ordinal));
        let (pm_scratch, pm_metric) = storage.manager.scene_registers(scene)?;
        let scene_va = storage.manager.scene_va(scene)?;
        let support = self.support.gpu_va();
        let usage = support + (USAGE + slot * 2 * USAGE_BYTES) as u64;
        let times = support + (TIMES + slot * 32) as u64;
        self.support.bytes_mut()[TIMES + slot * 32..TIMES + slot * 32 + 32].fill(0);
        self.support.bytes_mut()
            [USAGE + slot * 2 * USAGE_BYTES..USAGE + (slot + 1) * 2 * USAGE_BYTES]
            .fill(0);
        let state = storage.state.gpu_va();
        let predecessor = at!("object ids", crate::g17::ids::OBJECTS.object());
        let (fragment_ids, ta_ids): (ObjectIds, ObjectIds) =
            crate::g17::ids::OBJECTS.render_pair(predecessor)?;
        let work_node = at!("work node", work.node_va());
        let resources = RenderResources {
            context_id: self.context.id().try_into()?,
            context_generation: self.context.generation(),
            pb_slot: u32::from(self.pb.0),
            free_list_slot: u32::from(self.pool.id()),
            free_list_control_va: self.pool.control_va(),
            kick_record_va: work_node,
            pb_va: storage.manager.state_va(),
            scene_va,
            scene_end_va: storage.manager.trailer_va(),
            pm_scratch,
            pm_metric,
            tilemap_va: targets.tilemap,
            layermeta_va: targets.layer,
            heapmeta_va: targets.heap,
            tpc_va: storage.target.tpc_va(),
            tpc_size: layout.tpc_size as u64,
            gtp_va: [0; 8],
            deflake_va: state,
            ta_status_va: state + STATE_STATUS as u64,
            fragment_status_va: state + STATE_STATUS as u64 + 8,
            fragment_status_fw_va: storage.status_fw.iova() + 8,
            aux_fb_va: state + STATE_AUXILIARY as u64,
            timestamp_va: times,
            ta_ids,
            fragment_ids,
            unk_2104: 0,
            unk_210c: 0,
        };
        let offset = slot * COMMAND_BYTES;
        let low = self.descriptors_low + offset as u64;
        let high = self.descriptors.gpu_va() + offset as u64;
        let stage = |n: usize| RenderStage {
            qid: qids[n],
            descriptor_va: low + (n * assembly::FRAGMENT_OFFSET) as u64,
            state_word: work.word(),
            stamp_va: support + (STAMPS + n * STAGE_STRIDE) as u64,
            aux_stamp_va: support + (STAMPS + n * STAGE_STRIDE + 0x40) as u64,
        };
        let args = RenderArgs {
            pass: draw.pass,
            ray_query: [fw::render_rt::RayQueryStage::NONE; 2],
            resources: &resources,
            ordinal,
            ta: stage(TILING),
            fragment: stage(FRAGMENT),
            programs: self.programs,
            deflake: Deflake::M5,
        };
        let controls = Self::controls(draw.pass, &draw.plan, self.clusters);
        let mut inputs = at!(
            "inputs",
            Self::inputs(
                &args,
                [usage, usage + USAGE_BYTES as u64],
                resources.context_id,
                resources.context_generation
            )
        );
        at!("registers", regs::apply_pass(&args, &controls, &mut inputs));
        let mut workspace = KBox::<regs::Workspace>::new_uninit(GFP_KERNEL)?;
        // SAFETY: The workspace is integer storage whose all-zero representation is valid.
        let mut workspace = unsafe {
            core::ptr::write_bytes(workspace.as_mut_ptr(), 0, 1);
            workspace.assume_init()
        };
        let commands = self
            .descriptors
            .bytes_mut()
            .get_mut(offset..offset + COMMAND_BYTES)
            .ok_or(EINVAL)?;
        let encoded = at!(
            "encoding",
            assembly::encode_into(
                commands,
                low,
                &args,
                &controls,
                extra,
                regs::FragmentDispatch { words: [0; 2] },
                &inputs,
                &mut workspace
            )
        );
        at!(
            "bind work slot",
            assembly::bind_work_slot(commands, work_slot)
        );
        let timestamps = [
            self.lanes[TILING].kick.timestamp(),
            self.lanes[FRAGMENT].kick.timestamp(),
        ];
        at!(
            "completions",
            assembly::link_completions(commands, timestamps, payload, false)
        );
        // Configurations, kicks and items, still invisible.
        let configs = match bind {
            true => [
                Some(self.bound_config(TILING, payload)?),
                Some(self.bound_config(FRAGMENT, payload)?),
            ],
            false => [None, None],
        };
        let dependency = at!(
            "dependency",
            KickDependency::new(qids[TILING], timestamps[TILING]).ok_or(EINVAL)
        );
        let context = self.context.clone();
        let policy = context.policy();
        let mut targets = [0u32; 2];
        let mut gates = [None; 2];
        for n in [TILING, FRAGMENT] {
            let lane = &mut self.lanes[n];
            let producer = lane.cursors()?.producer;
            let (target, gate) = at!(
                "stage",
                lane.stage_kick(producer, configs[n], slot, timestamps[n])
            );
            targets[n] = target;
            // The tiling kick waits on its notification's gate (S's runtime); the fragment does
            // not.
            gates[n] = (n == TILING).then_some(gate);
        }
        let kick_args = |n: usize, lane: &Lane| KickArgs {
            qid: qids[n],
            timestamp: timestamps[n],
            parent: lane.kick.parent(),
            barriers: match n {
                TILING => &[][..],
                _ => core::slice::from_ref(&dependency),
            },
            dependency_types: Some(match n {
                TILING => &[1u8][..],
                _ => &[1u8, 3][..],
            }),
            descriptor_va: high + (n * assembly::FRAGMENT_OFFSET) as u64,
            queue_va: lane.graph.gpu_va(),
            qos: KickQos {
                slot: context.qos_id(),
                class: QOS_CLASS[n],
            },
            mcache: None,
            event_mask: gates[n].map_or([0; 4], m5queue::EntryGate::events),
            register_arrays: encoded.rce[n],
            scs_enabled: encoded.scs[n],
            priority: policy.priority(),
        };
        let tiling_args = kick_args(TILING, &self.lanes[TILING]);
        let fragment_args = kick_args(FRAGMENT, &self.lanes[FRAGMENT]);
        let submission = m5queue::HardwareSubmission::render(
            (qids[FRAGMENT], self.lanes[FRAGMENT].kick.low_va()),
            (qids[TILING], self.lanes[TILING].kick.low_va()),
            policy.priority(),
            first,
        )?;
        // A class's outer ring first naming these queues says so: an adopted pair whose class
        // changed starts on its new rings.
        let new_class = self.class != policy.priority();
        let records = [
            self.lanes[FRAGMENT].work_slot(targets[FRAGMENT], new_class)?,
            self.lanes[TILING].work_slot(targets[TILING], new_class)?,
        ];
        let owners = core::array::from_fn::<_, 2, _>(|n| qos::Owner {
            qid: qids[n],
            qos: context.qos_id(),
            data_master: n as u8,
        });
        let [tiling, fragment] = &mut self.lanes;
        let prepared = [
            tiling.kick.prepare_entry(&tiling_args)?,
            fragment.kick.prepare_entry(&fragment_args)?,
        ];
        if first {
            for kick in &prepared {
                kick.prepare_lower_install()?;
            }
        }
        at!("submitted kicks", work.add_submitted_kicks(2));
        let pool_target = match self
            .pool
            .reserve_submission()
            .and_then(|_| self.pool.reserve_submission())
        {
            Ok(target) => target,
            Err(error) => {
                work.cancel_unpublished_kicks(2)?;
                return Err(error);
            }
        };
        // From here on the firmware may see this pass.
        for kick in prepared {
            kick.commit();
        }
        self.pool_state.register();
        at!(
            "claim scene",
            self.storage.manager.claim_scene(ordinal, scene)
        );
        self.storage.manager.prepare_submission()?.commit();
        if let Some(packet) = &packet {
            packet.mark_published();
        }
        let lanes = &self.lanes;
        let result = (|| {
            host.port
                .qos_publish_pair(owners, context.scheduler_va(), policy)?;
            context.mark_published();
            fence(Ordering::SeqCst);
            host.port
                .publish_m5_pair(claims, &records, Some(&submission), || {
                    lanes[FRAGMENT].set_producer(targets[FRAGMENT])?;
                    lanes[TILING].set_producer(targets[TILING])
                })
        })();
        self.ordinal = ordinal + 1;
        self.unbound = false;
        self.rebound = None;
        self.class = policy.priority();
        self.active = Some(Active {
            ordinal,
            stamp: stamp_of(ordinal),
            targets,
            pool_target,
            work: work.clone(),
            packet,
            qos: owners,
        });
        if let Err(error) = result {
            self.quarantine_error = Some(error);
            return Err(error);
        }
        Ok(())
    }

    /// Retires the pass in flight once every witness holds (S's predicate): the work node
    /// drained, both stage stamps at the pass's stamp, both lanes idle at their targets and the
    /// pool's completed count at the pass's target. `complete` closes the pass's QoS. Nothing
    /// retires while a recovery runs (`state`, the device's recovery state): recovery marks the
    /// passes it interrupted before the firmware resumes them (`Self::note_interrupted`).
    pub(crate) fn retire(
        &mut self,
        state: u32,
        complete: &mut dyn FnMut(&[qos::Owner]) -> Result,
        retired: &mut dyn FnMut(Retired) -> Result,
    ) -> Result {
        let Some(active) = self.active.as_ref() else {
            return Ok(());
        };
        if state != 0 {
            return Ok(());
        }
        fence(Ordering::Acquire);
        let support = &self.support;
        let stamps = [
            support.word(STAMPS)?.load(Ordering::Relaxed),
            support.word(STAMPS + STAGE_STRIDE)?.load(Ordering::Relaxed),
        ];
        let done = active.work.drained()?
            && stamps == [active.stamp; 2]
            && self.lanes[TILING].idle_at(active.targets[TILING])?
            && self.lanes[FRAGMENT].idle_at(active.targets[FRAGMENT])?
            && self.pool.completed_submissions()? == active.pool_target;
        if !done {
            return Ok(());
        }
        // Neo's order (`Pair::account_drained`, `release_witnessed`): every fallible step and the
        // report come before the pair gives the pass up and ends its quarantine; an error leaves
        // the pass in flight, for `Registry::retire_m5` to fail (Ambiguous).
        complete(&active.qos)?;
        self.storage.manager.retire_scene(active.ordinal)?;
        self.storage.target.retire(active.ordinal)?;
        let slot = active.ordinal as usize % DEPTH;
        let mut times = [0u64; 4];
        for (index, time) in times.iter_mut().enumerate() {
            *time = support
                .dword(TIMES + slot * 32 + index * 8)?
                .load(Ordering::Relaxed);
        }
        let failed = self.quarantine_error;
        // A pass held at the restart (it had not started) that the firmware retired without
        // starting it either: no stage time was written from its publication to its retirement.
        // Every witness above says the firmware is done with it, so it runs again from the start
        // rather than failing (Neo's rule, `queue::render` `replay_abandoned`: interrupted work is
        // run again only if it never started).
        let replay = core::mem::take(&mut self.held)
            && failed.is_none()
            && times == [0; 4]
            && active
                .packet
                .as_ref()
                .is_some_and(|packet| replayable(&packet.completion));
        if let Some(packet) = active
            .packet
            .as_ref()
            .filter(|p| p.completion.interrupted())
        {
            // An interrupted pass fails however its witnesses read
            // (`Completion::note_interrupted`), unless it never started. Logged with its stage
            // times, all zero for a pass that never started.
            let started = match times == [0; 4] {
                true => "never started",
                false => "started",
            };
            let fate = match replay {
                true => "it runs again",
                false => "it fails",
            };
            pr_info!("G17 M5: render pair {:?}: pass {} ({}), interrupted by a restart, retired by the firmware: times {:x?} ({}); {}\n",
                self.qids(), active.ordinal, innocence(&packet.completion), times, started, fate);
        }
        if replay {
            self.replay = active
                .packet
                .clone()
                .map(|packet| (packet, crate::g17::now_ns()));
        } else {
            retired(Retired {
                packet: active.packet.clone(),
                times,
                failed,
            })?;
        }
        self.active = None;
        if failed.is_none() {
            // The GPU is retiring work: a watchdog of a queue beside it renews
            // (`timeout::renew_shared`), as for a Neo render's retirement.
            crate::g17::recovery::note_retired();
        }
        if self.awaiting_witness {
            // The failed pass drained exactly: the firmware is done with the pair.
            self.awaiting_witness = false;
            self.quarantine_error = None;
            pr_info!(
                "G17 M5: render pair {:?}: its failed pass ({:?}) drained; the pair returns\n",
                self.qids(),
                failed
            );
        }
        Ok(())
    }

    /// Whether a settled failure's drain is awaited.
    pub(crate) fn awaiting_witness(&self) -> bool {
        self.awaiting_witness
    }

    /// The context's scheduler release was consumed while this pair still awaited its failed
    /// pass's drain, with the firmware idle on both rings ([`Self::settled`]): no witness of the
    /// drain can come any more (on M5 a kill's acknowledgement does not prove the firmware
    /// finished reading, S `queue/compute/exit.rs` `settle_exit`), so the pair stays quarantined,
    /// leaked under the cap, the pass pinned with its lease, and the firmware no longer runs
    /// the pass: its footprint stops ordering others (`Completion::quarantine_render`). Returns
    /// whether a wait ended.
    pub(crate) fn end_witness_wait(&mut self) -> bool {
        if !self.awaiting_witness {
            return false;
        }
        self.awaiting_witness = false;
        if let Some(packet) = self
            .active
            .as_ref()
            .and_then(|active| active.packet.as_ref())
        {
            packet.completion.quarantine_render();
        }
        pr_warn!("G17 M5: render pair {:?}: its context was released before its failed pass drained; the pair stays quarantined\n",
            self.qids());
        true
    }

    /// The witnesses of the pass in flight against what retirement waits for, for a
    /// diagnostic: each stamp, the work node's completed kicks, each lane's cursors and target,
    /// and the pool's completed count, each read on its own.
    pub(crate) fn report(&self) -> Report {
        let support = &self.support;
        let stamp = |offset| {
            support
                .word(offset)
                .map(|word| word.load(Ordering::Relaxed))
        };
        let active = self.active.as_ref();
        Report {
            stamps: [stamp(STAMPS), stamp(STAMPS + STAGE_STRIDE)],
            stamp: active.map(|active| active.stamp),
            drained: active.map(|active| active.work.drained()),
            completed: active.map(|active| active.work.completed_kicks()),
            cursors: [self.lanes[TILING].cursors(), self.lanes[FRAGMENT].cursors()],
            targets: active.map(|active| active.targets),
            pool: self.pool.completed_submissions(),
            pool_target: active.map(|active| active.pool_target),
        }
    }

    /// Fails the pass in flight with `error` and quarantines the pair; its command stays pinned
    /// by the pair, with its render lease quarantined, as Neo quarantines a failed render before
    /// deferring its error (`Completion::quarantine_render`). Returns that command the first
    /// time, for its fence. A first [`Failure::Settled`] lets the pass's drain end the quarantine
    /// ([`Self::awaiting_witness`]); until then the firmware runs the pass on, so its lease keeps
    /// its footprint (`Completion::quarantine_resumed_render`). The lease goes at the drain
    /// (`Completion::release_after_retirement`).
    pub(crate) fn fail(&mut self, error: Error, failure: Failure) -> Option<Arc<Packet>> {
        self.held = false;
        let first = self.quarantine_error.is_none();
        // The M5 Ultra's rule (`awaits_witness`), as for a lane (`Queue::fail`): the pass
        // carries its stamp and targets from publication.
        self.awaiting_witness = awaits_witness(Some(failure), first, self.awaiting_witness, true);
        if let Some(packet) = self
            .active
            .as_ref()
            .and_then(|active| active.packet.as_ref())
        {
            match self.awaiting_witness {
                true => packet.completion.quarantine_resumed_render(),
                false => packet.completion.quarantine_render(),
            }
        }
        self.quarantine_error.get_or_insert(error);
        match first {
            true => self
                .active
                .as_ref()
                .and_then(|active| active.packet.clone()),
            false => None,
        }
    }
    pub(crate) fn owns_packet(&self, packet: &Arc<Packet>) -> bool {
        self.active
            .as_ref()
            .and_then(|active| active.packet.as_ref())
            .or(self.replay.as_ref().map(|(own, _)| own))
            .is_some_and(|own| Arc::ptr_eq(own, packet))
    }

    /// The pass awaiting its replay ([`Self::retire`]), and since when.
    pub(crate) fn replay(&self) -> Option<&(Arc<Packet>, u64)> {
        self.replay.as_ref()
    }
    /// Takes the pass awaiting its replay, to publish or fail it.
    pub(crate) fn take_replay(&mut self) -> Option<(Arc<Packet>, u64)> {
        self.replay.take()
    }
    /// Puts back a replay its publication found no room for yet; nothing of it was published.
    pub(crate) fn restore_replay(&mut self, replay: (Arc<Packet>, u64)) {
        self.replay = Some(replay);
    }
    /// The pass in flight is held for replay ([`Self::hold_unstarted`]): the restart's
    /// cancellation does not fail it (`Pair::settle_cancelled`).
    pub(crate) fn held(&self) -> bool {
        self.held
    }
}

/// Whether `completion`, interrupted by a restart, may run again: innocent and under Neo's
/// replay limit (`completion::MAX_REPLAYS`). Innocent as Neo's replay has it
/// (`queue::render` `replay_abandoned`): not classified guilty and never blamed; work a recovery
/// left unclassified (its guilty VMs unresolved) counts as innocent, as for Neo
/// (`Completion::settle_blamed`).
fn replayable(completion: &crate::g17::completion::Completion) -> bool {
    completion.interrupted()
        && !completion.guilty()
        && !completion.blamed()
        && completion.replay_count() < crate::g17::completion::MAX_REPLAYS
}

/// Recovery attribution (`recovery::Queues`), as a Neo render pair takes part in it. A pair
/// runs one pass at a time, so its pass in flight is its oldest; the qualification's own
/// passes carry no command and take no part.
impl Pair {
    fn command(&self) -> Option<&Arc<Packet>> {
        self.active
            .as_ref()
            .and_then(|active| active.packet.as_ref())
    }
    /// The DRM command of the pass in flight, which a blamed QID of this pair fails.
    pub(crate) fn oldest_packet(&self) -> Option<Arc<Packet>> {
        self.command().cloned()
    }
    pub(crate) fn oldest_spared(&self) -> Option<bool> {
        self.command().map(|packet| packet.completion.spared())
    }
    /// A recovery record names one of this pair's queues and the completion stamp of the pass
    /// it interrupted, `(ordinal + 1) << 8` like the pair's stamps. The firmware may resume
    /// that pass after the restart and leave every retirement witness looking like success;
    /// marked, it retires failed (`Completion::complete`). Without a stamp match the pass in
    /// flight is marked anyway: it is the pair's only one.
    pub(crate) fn note_interrupted(&self, stamp: u32) -> recovery::Interrupted {
        let Some(active) = self.active.as_ref() else {
            return recovery::Interrupted::None;
        };
        let Some(packet) = active.packet.as_ref() else {
            return recovery::Interrupted::None;
        };
        packet.completion.note_interrupted();
        match active.stamp == stamp {
            true => recovery::Interrupted::Exact,
            false => recovery::Interrupted::Oldest,
        }
    }
    pub(crate) fn visit_owners(&self, visit: &mut dyn FnMut(&Arc<VmStatus>)) {
        if let Some(packet) = self.command() {
            visit(packet.completion.status());
        }
    }
    pub(crate) fn classify(&self, class: &dyn Fn(&Arc<VmStatus>) -> recovery::Class) {
        if let Some(packet) = self.command() {
            packet
                .completion
                .classify(class(packet.completion.status()));
        }
    }
    /// Notes the DRM pass in flight for recovery's attribution, read while the firmware is
    /// halted ([`recovery::Progress`]): it started if the firmware wrote any of its stage times
    /// (all are cleared at its publication; [`Self::retire`] logs the same words).
    pub(crate) fn note_progress(&self, progress: &mut recovery::Progress) {
        let Some(active) = self
            .active
            .as_ref()
            .filter(|active| active.packet.is_some())
        else {
            return;
        };
        fence(Ordering::Acquire);
        let slot = active.ordinal as usize % DEPTH;
        let mut start = None;
        for index in 0..4 {
            let Ok(word) = self.support.dword(TIMES + slot * 32 + index * 8) else {
                return;
            };
            let time = word.load(Ordering::Relaxed);
            if time != 0 {
                start = Some(start.map_or(time, |start: u64| start.min(time)));
            }
        }
        progress.note(self.qids(), recovery::Work::Pass, active.ordinal, start);
    }
    /// Holds the DRM pass in flight for replay when the restart's table listed it (it was
    /// interrupted), it is innocent ([`replayable`]), and the halted instant proved it had not
    /// started: none
    /// of its stage times was written ([`recovery::Progress::never_started`]). Missing evidence,
    /// or a pass blamed, guilty, failed or past its replays, fails as before.
    pub(crate) fn hold_unstarted(&mut self, progress: &recovery::Progress, listed: u128) {
        self.held = false;
        if self.quarantine_error.is_some() {
            return;
        }
        let Some(active) = self.active.as_ref() else {
            return;
        };
        let Some(packet) = active.packet.as_ref() else {
            return;
        };
        let qids = self.qids();
        if qids.iter().all(|qid| listed & (1u128 << qid) == 0)
            || !replayable(&packet.completion)
            || !progress.never_started(qids)
        {
            return;
        }
        self.held = true;
        pr_info!("G17 M5: render pair {:?}: pass {} ({}) had not started at the restart; it is held to run again\n",
            qids, active.ordinal, innocence(&packet.completion));
    }
}

/// [`Pair::report`]'s witnesses.
#[derive(Debug)]
#[allow(dead_code)]
pub(crate) struct Report {
    stamps: [Result<u32>; 2],
    stamp: Option<u32>,
    drained: Option<Result<bool>>,
    completed: Option<Result<u32>>,
    cursors: [Result<retirement::Cursors>; 2],
    targets: Option<[u32; 2]>,
    pool: Result<u64>,
    pool_target: Option<u64>,
}

/// The completion stamp of pass `ordinal` (S: the pass's sequence number, ordinal + 1, << 8).
fn stamp_of(ordinal: u64) -> u32 {
    (ordinal as u32).wrapping_add(1).wrapping_shl(8)
}

/// How a recovery classified an interrupted pass, for its log line.
fn innocence(completion: &crate::g17::completion::Completion) -> &'static str {
    if completion.blamed() {
        "blamed"
    } else if completion.guilty() {
        "guilty"
    } else if completion.spared() {
        "spared"
    } else {
        "unclassified"
    }
}
