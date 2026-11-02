// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 compute register programs and descriptor (linux-aurora t6050-gpu's g17/fw/m5/compute.rs).
//! The operand and preemption layouts differ from the other G17 profile; sharing a register
//! count does not imply a shared register program.
//!
//! Only programs without a shader-context scratch (SCS) or matrix bracket and without
//! intersection tables are encoded: those brackets add writes and checkpoints, and the kick
//! entry would need the matrix flag.

use super::super::{
    kick::{RegisterArray, RegisterArrayBinding, RegisterWrite},
    offset_va, work_key,
};
use super::{
    budget::{History, Product, RenderBudget},
    demand::{ComputeStorage, FragmentStorage, RenderDemand, TileStorage, VertexStorage},
};
use kernel::{prelude::*, uapi};

pub(crate) const MAIN_COUNT: usize = 40;
pub(crate) const ALTERNATE_COUNT: usize = 4;
pub(crate) const PREEMPT_BYTES: usize = 0x4000;
/// Checkpoints of a program without scratch or matrix brackets.
pub(crate) const CHECKPOINTS: [u16; 4] = [37, 39, 39, 40];

/// The scratch registers of a program without shader-context scratch: no pages, the minimum
/// stack, no driver section and the empty window.
const NO_SCRATCH_SIZE: u64 = 0;
const NO_SCRATCH_STACK: u64 = 8;
const NO_SCRATCH_DRIVER: u64 = 0;
const NO_SCRATCH_WINDOW: u64 = 0xff_0000;

/// Kick QoS class, distinct from the policy's queue class. J714 value.
pub(crate) const KICK_QOS_CLASS: u8 = 8;
/// Completion class of a kick's implicit parent dependency. J714 value.
pub(crate) const PARENT_DEPENDENCY: u8 = 2;
/// Compute kick event mask. J714 value.
pub(crate) const KICK_EVENT_MASK: [u64; 4] = [0, 0, 8, 0];
/// Predecessor and current object identifiers of every kick. J714 value.
pub(crate) const OBJECT_IDS: u64 = 0x0000_0001_0000_0001;
/// Descriptor completion identity. J714 value.
pub(crate) const COMPLETION_IDENTITY: u64 = 1;

/// The descriptor parameters that follow the GPU rather than the command: one definition per
/// board ([`super::initdata::M5Board::compute`]), which the arithmetic qualification and DRM
/// submission both encode through [`crate::g17::queue::m5::Channel`].
///
/// The operand layout and the dispatch control's cluster bits follow the GPU's topology
/// ([`OperandLayout::of`], [`dispatch_control`]); the rest is the J714
/// tuple the qualification passes with. S's normal-compute tuple (`0x1b80`, `0x1e000`,
/// `0x3d800`, `0x14c08a60`, 1) is not used: it is unqualified on this GPU.
pub(crate) struct Profile {
    /// Start of the four eight-byte state words in the preemption buffer.
    pub(crate) preempt_state_offset: u32,
    pub(crate) operand: OperandLayout,
    /// Register 0x1a458.
    pub(crate) dispatch_control: u64,
    /// Register 0x0a731's first write.
    pub(crate) auxiliary_control: u64,
}

/// Operand bytes per core slot.
const OPERAND_SLOT_BYTES: u32 = 0x1800;
/// Core slots between clusters in the bank stride, whatever a cluster's own slot count.
const OPERAND_CLUSTER_SPACING: u32 = 10;

const fn align_up(value: u32, align: u32) -> u32 {
    (value + align - 1) & !(align - 1)
}

/// The operand-state buffer of a compute queue: two banks the programs exchange, then one
/// auxiliary record per cluster. It is sized from the GPU's topology, with nominal core
/// slots, not enabled cores: J775D's eight clusters of ten slots give bank 0x78000, stride 0x78400,
/// auxiliary 0xf4800 and 0xf8000 bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct OperandLayout {
    pub(crate) clusters: u32,
    /// Core slots per cluster (`ID_COUNTS_1[7:0]`).
    pub(crate) core_slots: u32,
    /// One bank: `L`, a slot's bytes for every nominal core.
    pub(crate) bank_bytes: u32,
    /// The second bank's offset: `D`.
    pub(crate) bank_stride: u32,
    /// The auxiliary records' offset: `U1`, after the shared record at `U0 = 2 D`.
    pub(crate) aux_offset: u32,
    /// The mapped backing: the extent `U1 + 0x400` in whole pages.
    pub(crate) bytes: usize,
}

impl OperandLayout {
    pub(crate) const fn of(clusters: u32, core_slots: u32) -> Self {
        assert!(clusters > 0 && core_slots > 0 && core_slots <= OPERAND_CLUSTER_SPACING);
        let bank_bytes = clusters * core_slots * OPERAND_SLOT_BYTES;
        let bank_stride = align_up(
            ((clusters - 1) * OPERAND_CLUSTER_SPACING + core_slots) * OPERAND_SLOT_BYTES
                + clusters * 0x40,
            0x400,
        );
        let aux_offset = 2 * bank_stride + clusters * 0x800;
        Self {
            clusters,
            core_slots,
            bank_bytes,
            bank_stride,
            aux_offset,
            bytes: align_up(aux_offset + 0x400, 0x4000) as usize,
        }
    }
}

// J775D's measured layout, and S's J714 production one (two clusters of ten slots).
const _: () = {
    let j775d = OperandLayout::of(8, 10);
    assert!(j775d.bank_bytes == 0x78000 && j775d.bank_stride == 0x78400);
    assert!(j775d.aux_offset == 0xf4800 && j775d.bytes == 0xf8000);
    let j714 = OperandLayout::of(2, 10);
    assert!(j714.bank_bytes == 0x1e000 && j714.aux_offset == 0x3d800 && j714.bytes == 0x40000);
};

/// Register 0x1a458 for `clusters`: above four, it carries the topology term `0x9c`, the choice for
/// feature bit 53 clear, which the configuration leaves clear.
pub(crate) const fn dispatch_control(base: u64, clusters: u32) -> u64 {
    if clusters > 4 {
        base | 0x9c
    } else {
        base
    }
}

/// The resource words of a compute kick and the queue pool they assume.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Resources {
    /// Register 0x0d4a1.
    pub(crate) control: u64,
    /// Pool target and resource policy of register 0x0a599's first write.
    pub(crate) policy: u64,
    /// Backed blocks of the queue's private pool this command needs ([`Gpu::plan`]).
    pub(crate) pool_blocks: usize,
}

/// Device policy of S's compute demand (`render_resources.rs` `plan_compute_scs`): warps resident
/// per core and the threadgroup storage they share. G17S values; the qualified storage-free
/// dispatch reproduces through them.
const RESIDENT_WARPS: u32 = 96;
const SHARED_BUDGET: u32 = 112 * 1024;
/// Resident compute kicks of a non-realtime queue: the PTC target is the pressure shared among
/// them, clamped to the firmware's range; the pool budget counts them too.
const ACTIVE_KICKS: u32 = 2;
/// Resident render kicks of a non-realtime pair (S `M5Policy::new`): tiling and fragment work.
const RENDER_ACTIVE_KICKS: u32 = 4;
const PTC_TARGET: core::ops::RangeInclusive<u32> = 96..=512;
/// The low half of register 0x0a599's first write, under the PTC target.
const RESOURCE_POLICY: u64 = 0x0040_0020;
/// The API's workgroup bound.
const MAX_THREADS: u32 = 1024;
/// Threadgroup storage of one workgroup: Honeykrisp's `maxComputeSharedMemorySize`.
const MAX_SHARED_BYTES: u32 = 32 * 1024;
/// Compute profiles one command may declare.
const MAX_PROGRAMS: usize = 16;
/// Tile storage of a pass in the M5 render subset (`command::m5_render`): bytes per sample times
/// samples times the tile's pixels.
pub(crate) const MAX_IMAGEBLOCK_BYTES: u32 = 65536;
/// The multisample storage a tile declaration carries beside its imageblock. Honeykrisp's M5
/// declaration (`hk_cmd_buffer.c`) is a quarter, the whole and four times one sample's tile
/// storage at 2x, 4x and 8x: at most half the imageblock.
const MAX_MULTISAMPLE_PER_IMAGEBLOCK: u32 = 2;
/// S's configured context storage per enabled core (`M5Policy::new` `CONTEXT_FLOOR`).
pub(crate) const CONTEXT_FLOOR: u32 = 96;

/// The PTC target of a demand's pressure shared among `kicks` resident kicks.
pub(crate) fn ptc_target(pressure: u32, kicks: u32) -> u32 {
    (pressure / kicks).clamp(*PTC_TARGET.start(), *PTC_TARGET.end())
}

/// A compute command's storage declarations (`DRM_ASAHI_SET_COMPUTE_RESOURCES`): each profile is
/// evaluated on the GPU's topology and the demands merged by component maxima, as S does
/// ([`Gpu::plan`]), since different profiles may maximize different terms.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Declared {
    programs: [ComputeStorage; MAX_PROGRAMS],
    count: usize,
    /// Some profile declares per-thread (scratch) storage.
    scratch: bool,
}

impl Declared {
    /// Adds one record: zero flags and a workgroup within the API's bound, as the UAPI requires.
    pub(crate) fn include(&mut self, program: &uapi::drm_asahi_compute_program) -> Result {
        if program.flags != 0
            || !(1..=MAX_THREADS).contains(&program.threads)
            || program.threadgroup_bytes > MAX_SHARED_BYTES
        {
            return Err(EINVAL);
        }
        let slot = self.programs.get_mut(self.count).ok_or(EINVAL)?;
        *slot = ComputeStorage {
            primary_bytes: program.primary_bytes,
            secondary_bytes: program.secondary_bytes,
            linked_bytes: program.linked_bytes,
            minimum_primary_bytes: program.minimum_primary_bytes,
            threadgroup_bytes: program.threadgroup_bytes,
            threads: program.threads,
        };
        self.count += 1;
        self.scratch |= program.primary_bytes != 0
            || program.secondary_bytes != 0
            || program.linked_bytes != 0
            || program.minimum_primary_bytes != 0;
        Ok(())
    }

    /// One storage-free profile of `threads`: what the arithmetic qualification declares.
    pub(crate) fn threads(threads: u32) -> Result<Self> {
        let mut declared = Self::default();
        declared.include(&uapi::drm_asahi_compute_program {
            threads,
            ..Default::default()
        })?;
        Ok(declared)
    }

    fn programs(&self) -> &[ComputeStorage] {
        &self.programs[..self.count]
    }
}

/// The GPU's pool planning: S's demand and budget formulas over its enabled-core masks
/// ([`RenderDemand::new`], [`RenderBudget::new`]) on the product's pool policy, for compute
/// queues and render pairs alike.
#[derive(Clone, Copy)]
pub(crate) struct Topology {
    /// The empty demand of this topology, which every plan starts from.
    pub(crate) demand: RenderDemand,
    pub(crate) compute: RenderBudget,
    pub(crate) render: RenderBudget,
    pub(crate) product: Product,
}

impl Topology {
    pub(crate) fn of(core_masks: &[u32], nominal_cores: u32, product: Product) -> Result<Self> {
        let budget = |kicks| RenderBudget::new(core_masks, CONTEXT_FLOOR, kicks, product);
        Ok(Self {
            demand: RenderDemand::new(core_masks, nominal_cores, CONTEXT_FLOOR)
                .map_err(|_| EINVAL)?,
            compute: budget(ACTIVE_KICKS).map_err(|_| EINVAL)?,
            render: budget(RENDER_ACTIVE_KICKS).map_err(|_| EINVAL)?,
            product,
        })
    }

    /// The merged demand of `programs` and the pool blocks it needs: S's ideal size
    /// (`RenderDemand::requirements`, `RenderBudget::sizes` without history), from which S
    /// allocates. No programs (a flush-only stream) is the storage-free minimum: one warp without
    /// storage, since a demand that includes nothing has no requirements.
    fn compute(&self, programs: &[ComputeStorage]) -> Result<(RenderDemand, usize)> {
        const STORAGE_FREE: [ComputeStorage; 1] = [ComputeStorage {
            primary_bytes: 0,
            secondary_bytes: 0,
            linked_bytes: 0,
            minimum_primary_bytes: 0,
            threadgroup_bytes: 0,
            threads: 32,
        }];
        let programs = if programs.is_empty() {
            &STORAGE_FREE[..]
        } else {
            programs
        };
        let mut demand = self.demand;
        for program in programs {
            demand
                .include_compute(*program, RESIDENT_WARPS, SHARED_BUDGET)
                .map_err(|_| EINVAL)?;
        }
        let request = demand.requirements().map_err(|_| EINVAL)?;
        let sizes = self
            .compute
            .sizes(request, History::default())
            .map_err(|_| EINVAL)?;
        Ok((demand, sizes.blocks().map_err(|_| EINVAL)?))
    }
}

/// One stage's planned words of a render pass: S `StagePlan` without its shader-context scratch,
/// which M5 render does not admit (`CommonPrograms::write_m5`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StagePlan {
    /// Register 0x0d4a1's resource flags.
    pub(crate) resource_flags: u64,
    pub(crate) ptc_max: u32,
}

/// A render pass's plan from its linked programs' storage (`DRM_ASAHI_SET_RENDER_RESOURCES`):
/// the pool its pair needs and the tiling and fragment words.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct RenderPlan {
    pub(crate) pool_blocks: usize,
    pub(crate) stages: [StagePlan; 2],
}

impl Topology {
    /// S `M5Policy::plan` at MEDIUM priority (four resident kicks): the pass's merged demand
    /// sizes the pool, and each stage's own demand its words. Every stage carries a default
    /// vertex or an empty fragment program, then each declared program of its stage.
    fn render(&self, programs: &[uapi::drm_asahi_render_program]) -> Result<RenderPlan> {
        let invalid = |_| EINVAL;
        let mut demand = self.demand;
        let mut stages = [demand; 2];
        let empty = FragmentStorage::default().parameters().map_err(invalid)?;
        let no_tile = TileStorage::default().parameters(false).map_err(invalid)?;
        stages[0]
            .include_vertex(VertexStorage::default())
            .map_err(invalid)?;
        stages[1]
            .include_fragment(empty, no_tile)
            .map_err(invalid)?;
        demand
            .include_vertex(VertexStorage::default())
            .map_err(invalid)?;
        demand.include_fragment(empty, no_tile).map_err(invalid)?;
        for program in programs {
            if program.stage > 1
                || program.flags != 0
                || (program.stage == 0
                    && (program.auxiliary_count != 0
                        || program.threadgroup_bytes != 0
                        || program.imageblock_bytes != 0
                        || program.multisample_bytes != 0))
            {
                return Err(EINVAL);
            }
            if program.stage == 0 {
                let storage = VertexStorage {
                    primary_bytes: program.primary_bytes,
                    secondary_bytes: program.secondary_bytes,
                    linked_bytes: program.linked_bytes,
                    minimum_primary_bytes: program.minimum_primary_bytes,
                };
                demand.include_vertex(storage).map_err(invalid)?;
                stages[0].include_vertex(storage).map_err(invalid)?;
            } else {
                let fragment = FragmentStorage {
                    primary_bytes: program.primary_bytes,
                    secondary_bytes: program.secondary_bytes,
                    linked_bytes: program.linked_bytes,
                    minimum_primary_bytes: program.minimum_primary_bytes,
                    auxiliary_count: program.auxiliary_count,
                }
                .parameters()
                .map_err(invalid)?;
                let tile = TileStorage {
                    threadgroup_bytes: program.threadgroup_bytes,
                    imageblock_bytes: program.imageblock_bytes,
                    multisample_bytes: program.multisample_bytes,
                }
                .parameters(false)
                .map_err(invalid)?;
                demand.include_fragment(fragment, tile).map_err(invalid)?;
                stages[1]
                    .include_fragment(fragment, tile)
                    .map_err(invalid)?;
            }
        }
        let request = demand.requirements().map_err(invalid)?;
        let sizes = self
            .render
            .sizes(request, History::default())
            .map_err(invalid)?;
        let stage = |demand: RenderDemand| -> Result<StagePlan> {
            Ok(StagePlan {
                resource_flags: demand.resource_flags().map_err(invalid)?,
                ptc_max: ptc_target(demand.pressure(), RENDER_ACTIVE_KICKS),
            })
        };
        Ok(RenderPlan {
            pool_blocks: sizes.blocks().map_err(invalid)?,
            stages: [stage(stages[0])?, stage(stages[1])?],
        })
    }
}

/// The board's descriptor profile on this GPU's topology: what M5 channels and command validation
/// take. Each compute queue's pool covers every command the plan accepts.
#[derive(Clone, Copy)]
pub(crate) struct Gpu {
    pub(crate) profile: &'static Profile,
    pub(crate) topology: Topology,
    pub(crate) pool_blocks: usize,
    /// Likewise each render pair's pool, over the subset's tile storage.
    pub(crate) render_pool_blocks: usize,
}

impl Gpu {
    pub(crate) fn new(
        board: &'static super::initdata::M5Board,
        core_masks: &[u32],
    ) -> Result<Self> {
        let product = Product::of(board.core_revision.0).ok_or(ENODEV)?;
        let topology = Topology::of(core_masks, board.nominal_cores, product)?;
        // The largest pool an accepted declaration needs: every warp count and threadgroup size
        // in 64-byte steps (the demand rounds storage to them).
        let mut blocks = 0;
        for warps in 1..=MAX_THREADS / 32 {
            for shared in (0..=MAX_SHARED_BYTES).step_by(64) {
                let program = ComputeStorage {
                    threads: warps * 32,
                    threadgroup_bytes: shared,
                    ..Default::default()
                };
                blocks = blocks.max(topology.compute(&[program])?.1);
            }
        }
        // And the largest a render pass's tile record needs: every imageblock size in 64-byte
        // steps (the tile storage rounds to them), with no multisample storage and with the most
        // a multisample pass of that imageblock declares, and the default programs.
        let mut render_blocks = 0;
        for imageblock_bytes in (0..=MAX_IMAGEBLOCK_BYTES).step_by(64) {
            for multisample_bytes in [0, imageblock_bytes / MAX_MULTISAMPLE_PER_IMAGEBLOCK] {
                let tile = uapi::drm_asahi_render_program {
                    stage: 1,
                    imageblock_bytes,
                    multisample_bytes,
                    ..Default::default()
                };
                render_blocks = render_blocks.max(topology.render(&[tile])?.pool_blocks);
            }
        }
        Ok(Self {
            profile: &board.compute,
            topology,
            pool_blocks: blocks,
            render_pool_blocks: render_blocks,
        })
    }

    /// The plan of a render pass whose linked programs declared `programs`, refused when it needs
    /// more than a pair's pool.
    pub(crate) fn plan_render(
        &self,
        programs: &[uapi::drm_asahi_render_program],
    ) -> Result<RenderPlan> {
        let plan = self.topology.render(programs)?;
        if plan.pool_blocks > self.render_pool_blocks {
            pr_warn!(
                "G17 M5: render pass needs {} pool blocks, a pair has {}\n",
                plan.pool_blocks,
                self.render_pool_blocks
            );
            return Err(EINVAL);
        }
        Ok(plan)
    }

    /// Bytes of one block of every M5 pool on this product.
    pub(crate) fn block_bytes(&self) -> usize {
        self.topology.product.block_bytes
    }

    /// The resources of a command with `declared` storage: register 0x0d4a1 packs the stack
    /// frame's and threadgroup storage's 64 KiB pages (S `resource_flags`), the PTC target is the
    /// pressure over the resident kicks, and the pool is what the demand needs on this topology.
    /// Scratch is refused: its backing (the shader-context storage the scratch words name) is not
    /// planned. An empty declaration (a flush-only stream, as for a barrier) plans the
    /// storage-free minimum; a command with no declaration at all is refused by the parser's
    /// caller (`command::m5_compute`): no declaration is not a declaration of none.
    pub(crate) fn plan(&self, declared: Declared) -> Result<Resources> {
        if declared.scratch {
            kernel::pr_warn_once!("G17 M5: compute scratch storage refused (not planned yet)\n");
            return Err(EOPNOTSUPP);
        }
        let (demand, pool_blocks) = self.topology.compute(declared.programs())?;
        if pool_blocks > self.pool_blocks {
            return Err(EINVAL);
        }
        Ok(Resources {
            control: demand.resource_flags().map_err(|_| EINVAL)?,
            policy: u64::from(ptc_target(demand.pressure(), ACTIVE_KICKS)) << 32 | RESOURCE_POLICY,
            pool_blocks,
        })
    }
}

/// Addresses and policy owned by a queued compute command.
pub(crate) struct RegisterArgs {
    /// Opaque allocation word, independent of the SCS layout.
    pub(crate) auxiliary_allocation: u32,
    pub(crate) preempt_va: u64,
    pub(crate) cdm_va: u64,
    pub(crate) operand_va: u64,
    pub(crate) usage_va: u64,
    /// Private-memory pool slot, as named by the usage record.
    pub(crate) free_list_slot: u32,
    pub(crate) context_id: u16,
    pub(crate) work_state: u32,
    /// Packed predecessor/current object identifiers.
    pub(crate) object_ids: u64,
    /// Pool target and resource policy for the first 0x0a599 write.
    pub(crate) resource_policy: u64,
    /// Start of the four eight-byte state words in the preemption buffer.
    pub(crate) preempt_state_offset: u32,
    pub(crate) operand: OperandLayout,
    pub(crate) resource_control: u64,
    pub(crate) dispatch_control: u64,
    pub(crate) auxiliary_control: u64,
}

impl RegisterArgs {
    fn validate(&self) -> Result {
        if self.preempt_va & 0x3fff != 0
            || self.operand_va & 0x3fff != 0
            || self.cdm_va & 7 != 0
            || self.usage_va & 7 != 0
            || self.context_id >= 64
        {
            return Err(EINVAL);
        }
        offset_va(self.preempt_va, PREEMPT_BYTES as u64)?;
        offset_va(self.operand_va, self.operand.bytes as u64)?;
        if self.preempt_state_offset & 7 != 0
            || u64::from(self.preempt_state_offset) + 32 > PREEMPT_BYTES as u64
            || self.operand.aux_offset as usize + 0x400 > self.operand.bytes
        {
            return Err(EINVAL);
        }
        Ok(())
    }
}

/// Construct the complete primary program before publishing its descriptor.
pub(crate) fn main(args: &RegisterArgs) -> Result<[RegisterWrite; MAIN_COUNT]> {
    args.validate()?;
    let preempt = args.preempt_va;
    let operand = args.operand_va;
    let ids = args.object_ids;
    let state = u64::from(args.preempt_state_offset);
    let bank = u64::from(args.operand.bank_bytes);
    let stride = u64::from(args.operand.bank_stride);
    let key = work_key(args.context_id, args.work_state);
    Ok([
        RegisterWrite::new(0x1a510, preempt),
        RegisterWrite::new(0x1a420, args.cdm_va),
        RegisterWrite::new(0x1a4d0, offset_va(preempt, state)?),
        RegisterWrite::new(0x1a4d8, offset_va(preempt, state + 8)?),
        RegisterWrite::new(0x1a4e0, offset_va(preempt, state + 16)?),
        RegisterWrite::new(0x1a4e8, offset_va(preempt, state + 24)?),
        RegisterWrite::new(0x1a440, 0x154024201),
        RegisterWrite::new(0x0d4a1, args.resource_control),
        RegisterWrite::new(0x0d411, 0x200000001),
        RegisterWrite::new(0x0a599, args.resource_policy),
        RegisterWrite::new(0x1a458, args.dispatch_control),
        RegisterWrite::new(0x101d9, 0x1c),
        // No intersection tables.
        RegisterWrite::new(0x1a089, 0),
        RegisterWrite::new(0x1a091, 0),
        RegisterWrite::new(0x1a059, 0),
        RegisterWrite::new(0x1a061, 0),
        RegisterWrite::new(0x1a0b9, 0),
        RegisterWrite::new(0x1a0c1, 0),
        RegisterWrite::new(0x101d1, NO_SCRATCH_SIZE),
        RegisterWrite::new(0x0d479, u64::from(args.auxiliary_allocation)),
        RegisterWrite::new(0x1a0e9, NO_SCRATCH_STACK),
        RegisterWrite::new(0x1a141, NO_SCRATCH_DRIVER),
        // Bit 25 would request the matrix bracket.
        RegisterWrite::new(0x107a1, NO_SCRATCH_WINDOW),
        RegisterWrite::new(0x1a540, ids),
        RegisterWrite::new(0x014a9, ids),
        RegisterWrite::new(0x0a351, ids),
        RegisterWrite::new(0x0a731, args.auxiliary_control),
        RegisterWrite::new(0x10201, key),
        RegisterWrite::new(0x10428, key),
        RegisterWrite::new(0x14028, args.free_list_slot.into()),
        RegisterWrite::new(0x14070, args.usage_va | 1),
        // Bank bases are 0x400 aligned; the low 5 are address flags.
        RegisterWrite::new(0x10229, offset_va(operand, stride * 2)?),
        RegisterWrite::new(0x140a8, offset_va(operand, args.operand.aux_offset.into())?),
        RegisterWrite::new(0x10099, offset_va(operand, stride | 5)?),
        RegisterWrite::new(0x10091, offset_va(operand, stride + bank)?),
        RegisterWrite::new(0x0a5c1, offset_va(operand, 5)?),
        RegisterWrite::new(0x0a5c9, offset_va(operand, bank)?),
        RegisterWrite::new(0x0a731, 0),
        RegisterWrite::new(0x1a440, 0x154024209),
        RegisterWrite::new(0x0a599, 0x6000400020),
    ])
}

/// Operand-state exchange used by the alternate program.
pub(crate) fn alternate(args: &RegisterArgs) -> Result<[RegisterWrite; ALTERNATE_COUNT]> {
    args.validate()?;
    let operand = args.operand_va;
    let bank = u64::from(args.operand.bank_bytes);
    let stride = u64::from(args.operand.bank_stride);
    Ok([
        RegisterWrite::new(0x10099, offset_va(operand, 5)?),
        RegisterWrite::new(0x10091, offset_va(operand, bank)?),
        RegisterWrite::new(0x0a5c1, offset_va(operand, stride | 5)?),
        RegisterWrite::new(0x0a5c9, offset_va(operand, stride + bank)?),
    ])
}

/// Register-array bindings in a compute kick. The first checkpoint follows
/// the initial 0x0a5c9 write, before the second 0x0a731 write.
pub(crate) fn bindings(descriptor_va: u64) -> Result<[Option<RegisterArrayBinding>; 4]> {
    Ok([
        Some(RegisterArrayBinding::new(
            offset_va(descriptor_va, 0x40)?,
            CHECKPOINTS[0] as u8,
        )?),
        Some(RegisterArrayBinding::new(
            offset_va(descriptor_va, 0x760)?,
            ALTERNATE_COUNT as u8,
        )?),
        None,
        None,
    ])
}

/// The described prefix of a compute descriptor. Storage beyond this prefix is
/// owned and initialized by the runtime allocation, not described here.
pub(crate) const DESCRIPTOR_PREFIX_BYTES: usize = 0x1000;

/// Per-command fields outside the two register programs. Fields whose bit
/// meanings are not established remain explicit opaque values.
pub(crate) struct DescriptorArgs {
    pub(crate) registers: RegisterArgs,
    pub(crate) descriptor_va: u64,
    pub(crate) submit_sequence: u64,
    pub(crate) work_state_va: u64,
    pub(crate) cdm_end_va: u64,
    pub(crate) stamp_va: [u64; 2],
    pub(crate) stamp_value: u32,
    pub(crate) qid: u32,
    pub(crate) kick_timestamp: u64,
    pub(crate) completion_ordinal: u32,
    pub(crate) status_va: [u64; 2],
    pub(crate) timestamp_va: [u64; 2],
    pub(crate) shared_control_va: u64,
    pub(crate) event_metadata: [u8; 16],
    pub(crate) usage_fw_va: u64,
    pub(crate) completion_identity: u64,
}

/// The M5 descriptor has a shorter reserved region before shared-control
/// state. Register-array offsets are shared with G17, tail offsets are not.
#[repr(C, packed)]
pub(crate) struct Descriptor {
    tag: u32,
    submit_sequence: u64,
    context_id: u32,
    work_state_va: u64,
    checkpoints: [u16; 4],
    reserved_20: [u8; 0x20],
    registers: RegisterArray,
    alternate: RegisterArray,
    reserved_e80: [u8; 0x58],
    preempt_va: u64,
    cdm_end_va: u64,
    reserved_ee8: [u8; 0x20],
    control: u64,
    reserved_f10: [u8; 0x10],
    predecessor_id: u32,
    reserved_f24: u32,
    unk_f28: u32,
    firmware_timestamps: [u64; 2],
    reserved_f3c: u32,
    stamp_va: [u64; 2],
    stamp_value: u32,
    qid: u32,
    reserved_f58: u64,
    kick_timestamp: u64,
    current_id: u64,
    completion_ordinal: u32,
    reserved_f74: [u8; 8],
    status_va: [u64; 2],
    timestamp_va: [u64; 2],
    reserved_f9c: [u8; 0x10],
    shared_control_va: u64,
    shared_control_enabled: u8,
    event_metadata: [u8; 16],
    usage_fw_va: u64,
    completion_identity: u64,
    reserved_fd5: [u8; 0x2b],
}

static_assert!(size_of::<Descriptor>() == DESCRIPTOR_PREFIX_BYTES);
static_assert!(core::mem::offset_of!(Descriptor, registers) == 0x40);
static_assert!(core::mem::offset_of!(Descriptor, alternate) == 0x760);
static_assert!(core::mem::offset_of!(Descriptor, stamp_va) == 0xf40);
static_assert!(core::mem::offset_of!(Descriptor, completion_ordinal) == 0xf70);
static_assert!(core::mem::offset_of!(Descriptor, shared_control_va) == 0xfac);
static_assert!(core::mem::offset_of!(Descriptor, usage_fw_va) == 0xfc5);
static_assert!(core::mem::offset_of!(Descriptor, completion_identity) == 0xfcd);

// SAFETY: Every field consists only of integer values and their arrays.
unsafe impl Zeroable for Descriptor {}

impl Descriptor {
    /// Fill the entire prefix; no address or register becomes visible before
    /// validation completes. The owner publishes only after this returns.
    pub(crate) fn write(&mut self, args: &DescriptorArgs) -> Result {
        if args.cdm_end_va < args.registers.cdm_va || args.qid >= 128 {
            return Err(EINVAL);
        }
        let main = main(&args.registers)?;
        let alternate = alternate(&args.registers)?;
        let main_va = offset_va(args.descriptor_va, 0x40)?;
        let alternate_va = offset_va(args.descriptor_va, 0x760)?;
        // Also validate the kick's encodable addresses before mutating storage.
        bindings(args.descriptor_va)?;
        super::super::clear(self);
        self.tag = 3;
        self.submit_sequence = args.submit_sequence;
        self.context_id = args.registers.context_id.into();
        self.work_state_va = args.work_state_va;
        self.checkpoints = CHECKPOINTS;
        super::set_register_array(&mut self.registers, main_va, &main)?;
        super::set_register_array(&mut self.alternate, alternate_va, &alternate)?;
        self.preempt_va = args.registers.preempt_va;
        self.cdm_end_va = args.cdm_end_va;
        self.control = 0x154024201;
        self.predecessor_id = (args.registers.object_ids >> 32) as u32;
        self.unk_f28 = u32::MAX;
        self.stamp_va = args.stamp_va;
        self.stamp_value = args.stamp_value;
        self.qid = args.qid;
        self.kick_timestamp = args.kick_timestamp;
        self.current_id = args.registers.object_ids as u32 as u64;
        self.completion_ordinal = args.completion_ordinal;
        self.status_va = args.status_va;
        self.timestamp_va = args.timestamp_va;
        self.shared_control_va = args.shared_control_va;
        self.shared_control_enabled = u8::from(args.shared_control_va != 0);
        self.event_metadata = args.event_metadata;
        self.usage_fw_va = args.usage_fw_va;
        self.completion_identity = args.completion_identity;
        Ok(())
    }
}
