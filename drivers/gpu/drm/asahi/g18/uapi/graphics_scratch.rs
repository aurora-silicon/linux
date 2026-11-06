// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! G18 render (TA/3D) thread-private scratch plan from per-render metadata.
//!
//! Pure arithmetic: no allocation, MMIO, firmware or VM access. Evidence and
//! differential checks: the graphics-scratch design notes.
//!
//! - Per-program accumulator: the native-verified compute arithmetic
//!   ([`G18ComputeInputs::merge`]) for one 32-thread warp reproduces all eight
//!   native 26A428 graphics shader-only descriptors (closed-form-check-v3).
//! - Fragment pass term: the native RenderContext framebuffer-resource
//!   constructor (AGXMetalG18G 1ad040548..1ad040614) and fragment pass merge
//!   (1ad027c8c..1ad027e68) for one sample, no user storage and no MSAA
//!   extra: 20/20 native comparisons (pass-closed-form-check).
//! - Render sizing: the TA kick, then the 3D kick on the same pool/FList.
//!   Native allocatePagesEnd stores the selected per-index parameters in
//!   FList+0xbc..0x19c and the next prepareLockedBegin max-merges them
//!   (8c91168..8c913f0), so the 3D requirement includes the TA requirement.
//!   Component maxima of descriptor words 0..18 then the ideal/minimum pair,
//!   chunk counter and bytes below match the original instructions for every
//!   in-capacity ideal case and all minimum cases of a 1,220-case grid at
//!   1 and 2 MiB blocks, and all four captured TA/3D register pairs
//!   (sequential-kicks, closed-form-check-v3).
//!
//! Not covered (callers must refuse): multisample or layered renders, user
//! (FramebufferDriverConfig+0x904) storage, tile storage above one 8 KiB
//! page, scratch in background/end-of-tile programs, and renders without any
//! scratch (those keep the existing fixed policy unchanged). The native
//! tile-program KiB terms are not fully reproduced (pass-terms-v2); they
//! affect only words 0/4, i.e. chunk counters and bytes, never the pair.

use crate::g18::uapi::compute_inputs::{G18ComputeDemand, G18ComputeInputs, G18_TGMEM_POLICY};

/// `drm_asahi_g18_render_resources.version`.
pub(crate) const G18_RENDER_RESOURCES_VERSION: u32 = 1;
/// Little-endian u32 words in `drm_asahi_g18_render_resources`.
pub(crate) const G18_RENDER_RESOURCE_WORDS: usize = 20;
const FLAG_VERTEX: u32 = 1 << 0;
const FLAG_FRAGMENT: u32 = 1 << 1;
const TILE_PROGRAMS: usize = 4;
const USCS: u64 = 12;
const POOL_MAX_BYTES: u64 = 0x8000_0000;
/// Native per-USC chunk capacity of a 2 GiB, twelve-USC pool
/// (g18_compute_plan.rs uses the same measured geometry).
const CHUNKS_PER_USC: u64 = (POOL_MAX_BYTES / 0x1_0000 / USCS) * 64;
/// Largest tile storage whose pass term is covered by native evidence.
const MAX_TILE_BYTES: u32 = 8192;

/// One program's demand in Mesa units (16-bit registers, IPR halfwords).
#[derive(Debug, Copy, Clone, Default, PartialEq, Eq)]
pub(crate) struct G18GraphicsProgram {
    pub(crate) registers_16: u32,
    pub(crate) scratch_bytes: u32,
    pub(crate) staging_16: u32,
}

impl G18GraphicsProgram {
    fn valid(&self) -> bool {
        (1..=256).contains(&self.registers_16)
            && self.scratch_bytes & 15 == 0
            && self.scratch_bytes <= 0xffff * 4
            && self.staging_16 <= 0xffff
    }

    /// Accumulator components of this program alone (one 32-thread warp).
    fn components(&self) -> Option<[u32; 58]> {
        let mut inputs = G18ComputeInputs::new();
        inputs.merge(G18ComputeDemand {
            scratch_bytes: self.scratch_bytes,
            constant_scratch_bytes: 0,
            registers_32: (self.registers_16 + 1) / 2,
            ipr_bytes: self.staging_16 * 2,
            constant_ipr_bytes: 0,
            threadgroup_bytes: 0,
            imageblock_bytes: 0,
            threads: 32,
        }, G18_TGMEM_POLICY)?;
        Some(*inputs.component_words())
    }
}

/// Decoded `drm_asahi_g18_render_resources` plus the command's tile storage.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderMetadata {
    pub(crate) vertex: G18GraphicsProgram,
    pub(crate) fragment: G18GraphicsProgram,
    /// Background, end-of-tile, partial background, partial end-of-tile.
    pub(crate) tiles: [Option<G18GraphicsProgram>; TILE_PROGRAMS],
    pub(crate) tile_bytes: u32,
}

impl G18RenderMetadata {
    /// `tile_bytes` comes from the validated render command (utile area x
    /// bytes per sample x samples), never from userspace metadata.
    pub(crate) fn decode(words: &[u32; G18_RENDER_RESOURCE_WORDS], tile_bytes: u32) -> Option<Self> {
        if words[0] != G18_RENDER_RESOURCES_VERSION
            || words[1] != (FLAG_VERTEX | FLAG_FRAGMENT)
            || words[16..].iter().any(|&w| w != 0)
            || tile_bytes == 0 || tile_bytes > MAX_TILE_BYTES
        {
            return None;
        }
        let stage = |at: usize| G18GraphicsProgram {
            registers_16: words[at], scratch_bytes: words[at + 1], staging_16: words[at + 2],
        };
        let (vertex, fragment) = (stage(2), stage(5));
        if !vertex.valid() || !fragment.valid()
            || (vertex.scratch_bytes == 0 && fragment.scratch_bytes == 0)
        {
            return None;
        }
        let mut tiles = [None; TILE_PROGRAMS];
        for (index, slot) in tiles.iter_mut().enumerate() {
            let program = G18GraphicsProgram {
                registers_16: words[8 + index], scratch_bytes: 0, staging_16: words[12 + index],
            };
            if program.registers_16 == 0 {
                if program.staging_16 != 0 { return None; }
            } else if program.valid() {
                *slot = Some(program);
            } else {
                return None;
            }
        }
        Some(Self { vertex, fragment, tiles, tile_bytes })
    }

    /// TA (vertex stage) descriptor: the vertex program alone.
    pub(crate) fn ta_descriptor(&self) -> Option<[u32; 23]> {
        Some(serialize(&self.vertex.components()?))
    }

    /// 3D (fragment stage) descriptor: component maxima of the fragment and
    /// tile programs, then the native pass merge of (fragment + pass terms).
    pub(crate) fn fragment_descriptor(&self) -> Option<[u32; 23]> {
        let main = self.fragment.components()?;
        let mut acc = main;
        for program in self.tiles.iter().flatten() {
            let words = program.components()?;
            for (a, b) in acc.iter_mut().zip(words.iter()) { *a = (*a).max(*b); }
        }
        let kib = (self.tile_bytes + 1023) / 1024;
        let pages = (self.tile_bytes + 8191) / 8192;
        let mut passed = main;
        passed[0x44 / 4] = passed[0x44 / 4].checked_add(kib)?;
        passed[0x84 / 4] = passed[0x84 / 4].checked_add(kib.checked_mul(11)?)?;
        for (offset, value) in [(0x28, pages), (0x68, pages * 6), (0x74, pages)] {
            passed[offset / 4] = passed[offset / 4].max(value);
        }
        for (a, b) in acc.iter_mut().zip(passed.iter()) { *a = (*a).max(*b); }
        Some(serialize(&acc))
    }

    /// Per-render plan for a pool of `block_bytes` blocks.
    pub(crate) fn plan(&self, block_bytes: u64) -> Option<G18RenderScratchPlan> {
        if !matches!(block_bytes, 0x10_0000 | 0x20_0000) { return None; }
        let ta = self.ta_descriptor()?;
        let fragment = self.fragment_descriptor()?;
        // The 3D kick follows the TA kick on the same FList: its requirement
        // is the component maximum of both descriptors (words 0..18).
        let mut d3 = fragment;
        for (a, b) in d3[..19].iter_mut().zip(ta[..19].iter()) { *a = (*a).max(*b); }
        Some(G18RenderScratchPlan {
            ideal: [size(&ta, false, block_bytes)?, size(&d3, false, block_bytes)?],
            minimum: [size(&ta, true, block_bytes)?, size(&d3, true, block_bytes)?],
        })
    }
}

/// Native AGXSMADescRec serialization of the 58-word accumulator
/// (identical to `G18ComputeInputs::descriptor`).
fn serialize(c: &[u32; 58]) -> [u32; 23] {
    let w = |off: usize| c[off / 4];
    [w(0x44), w(0x0c) + w(0x10) + w(0x14) + w(0x24) + w(0x28), 0, 0,
     w(0x84), w(0x4c) + w(0x50) + w(0x54) + w(0x64) + w(0x68), 0, 0,
     w(0x58), w(0x5c), w(0x60), w(0x70), w(0x74), 0, 0, 0,
     w(0xc8), 0, 0, w(0xcc), w(0xc4), 0, 0]
}

/// One kick's selected native result.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderStagePlan {
    /// Launch pair, packed `[0] | [1] << 16` in 0xa5e9 (TA) / 0xa5f1 (3D).
    pub(crate) register_pair: [u32; 2],
    /// Native FList chunk counter (+0xb0) for this kick.
    pub(crate) chunk_counter: u32,
    /// Native per-USC chunk reservation (64-aligned).
    pub(crate) chunks_per_usc: u32,
    pub(crate) bytes: u64,
}

impl G18RenderStagePlan {
    pub(crate) fn resource_word(&self) -> u64 {
        u64::from(self.register_pair[0]) | u64::from(self.register_pair[1]) << 16
    }
}

fn size(d: &[u32; 23], minimum: bool, block_bytes: u64) -> Option<G18RenderStagePlan> {
    let d = d.map(u64::from);
    // Original SIMD sequence at 8c916c0..8c91724 (see g18_compute_plan.rs):
    // each header component rounds separately.
    let h = &d[8..16];
    let header = h.iter().sum::<u64>() + d[16]
        + ((h[1] + 131) >> 7)
        + ((h[2] + h[3] + 127) >> 7)
        + ((h[4] + h[5] + 127) >> 7)
        + ((h[6] + h[2] + h[7] + d[16] + 127) >> 7);
    let (first, counter) = if minimum { (d[1], d[0] + d[19]) } else { (d[5], d[4] + d[1] + d[19]) };
    let chunks = (counter + header + 3 + 63) & !63;
    if chunks > CHUNKS_PER_USC || first > 0xffff || header > 0xffff { return None; }
    let bytes = (chunks * USCS * 1024 + block_bytes - 1) & !(block_bytes - 1);
    if bytes == 0 || bytes > POOL_MAX_BYTES { return None; }
    Some(G18RenderStagePlan {
        register_pair: [u32::try_from(first).ok()?, u32::try_from(header).ok()?],
        chunk_counter: u32::try_from(counter).ok()?,
        chunks_per_usc: u32::try_from(chunks).ok()?,
        bytes,
    })
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) enum G18RenderScratchMode {
    Ideal,
    /// Native's below-ideal selection (allocatePagesEnd minimum flag).
    Minimum,
}

/// Both native results for `[TA, 3D]`.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderScratchPlan {
    pub(crate) ideal: [G18RenderStagePlan; 2],
    pub(crate) minimum: [G18RenderStagePlan; 2],
}

/// The selection the job applies to its TA/3D resource words.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18RenderScratchSelection {
    pub(crate) mode: G18RenderScratchMode,
    pub(crate) stages: [G18RenderStagePlan; 2],
}

impl G18RenderScratchPlan {
    /// Ideal when both kicks fit the owned pool, else the native minimum
    /// when both fit, else none. `owned_chunks_per_usc` is the chunk counter
    /// already published in the owned pool state; neither the pool nor its
    /// state is resized by this selection.
    pub(crate) fn select(&self, owned_bytes: u64, owned_chunks_per_usc: u32)
        -> Option<G18RenderScratchSelection>
    {
        let fits = |stages: &[G18RenderStagePlan; 2]| stages.iter()
            .all(|s| s.bytes <= owned_bytes && s.chunks_per_usc <= owned_chunks_per_usc);
        if fits(&self.ideal) {
            Some(G18RenderScratchSelection { mode: G18RenderScratchMode::Ideal, stages: self.ideal })
        } else if fits(&self.minimum) {
            Some(G18RenderScratchSelection { mode: G18RenderScratchMode::Minimum, stages: self.minimum })
        } else {
            None
        }
    }
}
