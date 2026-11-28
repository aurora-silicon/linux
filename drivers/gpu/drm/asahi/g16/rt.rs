// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Hardware ray tracing on G16G (RENDER_RAY_QUERY and BOUNDED_AS_BUILD).
//!
//! The command state is the shared G17P UAPI: a compute command's packed shader-context
//! scratch (SCS) layout and intersection tables, a render's per-stage ray-query state, and the
//! compute AS-build flag. The validation, the register words they become and the SCS memory
//! sizing are those of the M4 Pro (T6041, the same G16 GPU generation) on its 26B123 firmware
//! (`g17::fw::variant::V26B123`), except where macOS 27.2 on the J773g (G16G, 26B123) has
//! been traced: a ray-query compute item's 0x017e1/0x017d9 records and 0xd479 tail
//! ([`super::encode`]), and the SCS pools' freelists (+0x1c, [`SCS_FREELIST_RECORDS`]) and
//! row words (+0x48). This module is pure: no kernel objects, no allocation.
//!
//! What neither settles for G16G stays off behind a named gate or is open, as trace questions
//! of mesa#577:
//!
//! - 0x017d9 (SCS enable) in a render stage, [`super::encode`]'s `RENDER_SCS_ENABLE` (T5,
//!   T6): the M4 Pro's 25G83 render programs write it in a stage with SCS. No G16G render with
//!   ray queries has been traced.
//! - 0x1a141 (SCS driver word), `COMPUTE_SCS_DRIVER_WORD` (T1): macOS's ray-query compute
//!   items on the J773g do not write it.
//! - The SCS-enable bit (T6, g17's kick header bit 61) is a u32 of each start command:
//!   StartCompute +0x40, StartTA +0x60, StartFS +0x78 (`fw::work`'s `START_SCS`). macOS 27.2 on
//!   the J773g sets it on every compute item whose list opens with 0x017e1 and on no other. A
//!   slot without it is not protected from the firmware's idle SCS teardown (0x017e0 = 0,
//!   compute and TA windows 0xff0000), which runs while its dispatch is still executing.
//! - A traversal exclusion between clients (T8): the A18 Pro needs one (`g17::traversal`), the
//!   M4 Pro's variants do not set it; G16G adds none.
//! - How macOS sizes the SCS pool for a given request (T3): it grows its compute pool beyond
//!   the M4 Pro's extras ([`pool_seeds`]); one request size does not give the rule.

use kernel::prelude::*;

/// Validated per-command shader-context scratch request (g17 `fw::compute::ScratchRequest`).
///
/// The layout word is Apple's packed SCS layout: depth (bits 0:15), levels (16:23), cliques
/// (24:31), user (32:47) and driver (48:63). The region the hardware needs is derived with the
/// Gen1 section model: fixed prefixes totalling [`Self::FIXED_BYTES`], then per-thread stack,
/// per-thread driver and per-thread user bytes, rounded up with a two-page guard.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ScratchRequest {
    pages: u16,
    window: u64,
    stack: u64,
    driver: u64,
    tail: u64,
}

impl ScratchRequest {
    /// Native ScsLayoutGen1::getSectionEnd(4).
    const FIXED_BYTES: u64 = 0x58000;
    const THREADS_PER_CLIQUE: u64 = 32;
    const BYTES_PER_STACK_ENTRY: u64 = 8;
    const PAGE_SHIFT: u32 = 12;
    const PAGE_SIZE: u64 = 1 << Self::PAGE_SHIFT;
    const PAGES_PER_MIB: u64 = 1 << (20 - Self::PAGE_SHIFT);
    /// Window word when enabled: bit 24 plus the clique count at 16:23.
    const WINDOW_ENABLED: u64 = 1 << 24;
    /// Disabled window word: the value G16G's work lists write today.
    pub(crate) const WINDOW_DISABLED: u64 = 0xff_0000;
    /// Disabled stack word: the value G16G's work lists write today.
    pub(crate) const STACK_DISABLED: u64 = 8;
    /// TA/FS tail reservation when enabled (0xd471, 0xd469), matching macOS 27.2's J773g
    /// ray-query tail reservation (0x2c).
    const TAIL_ENABLED: u64 = 0x2c;
    /// Compute tail reservation when enabled (0xd479), as macOS 27.2 on the J773g writes it in
    /// its ray-query compute work.
    const COMPUTE_TAIL_ENABLED: u64 = 0x2c;

    /// No scratch.
    pub(crate) const NONE: Self = Self {
        pages: 0,
        window: Self::WINDOW_DISABLED,
        stack: Self::STACK_DISABLED,
        driver: 0,
        tail: 0,
    };

    /// Payload-free layouts request no scratch even if depth or cliques are set.
    pub(crate) fn new(layout: u64) -> Result<Self> {
        let depth = layout & u64::from(u16::MAX);
        let cliques = (layout >> 24) & u64::from(u8::MAX);
        let user = (layout >> 32) & u64::from(u16::MAX);
        let driver = (layout >> 48) & u64::from(u16::MAX);
        if user == 0 && driver == 0 {
            return Ok(Self::NONE);
        }
        let threads = cliques
            .checked_mul(Self::THREADS_PER_CLIQUE)
            .ok_or(EINVAL)?;
        let stack_bytes = threads
            .checked_mul(
                depth
                    .checked_mul(Self::BYTES_PER_STACK_ENTRY)
                    .ok_or(EINVAL)?,
            )
            .ok_or(EINVAL)?;
        let driver_bytes = threads.checked_mul(driver).ok_or(EINVAL)?;
        let user_bytes = threads.checked_mul(user).ok_or(EINVAL)?;
        let end = Self::FIXED_BYTES
            .checked_add(stack_bytes)
            .and_then(|bytes| bytes.checked_add(driver_bytes))
            .and_then(|bytes| bytes.checked_add(user_bytes))
            .and_then(|bytes| bytes.checked_add(2 * Self::PAGE_SIZE - 1))
            .ok_or(EINVAL)?;
        let pages = end >> Self::PAGE_SHIFT;
        Ok(Self {
            pages: u16::try_from(pages).map_err(|_| EINVAL)?,
            window: Self::WINDOW_ENABLED | (cliques << 16),
            stack: depth & !7,
            // The Gen1 register list publishes no level region: the word stays zero.
            driver: 0,
            tail: Self::TAIL_ENABLED,
        })
    }

    pub(crate) fn enabled(self) -> bool {
        self.pages != 0
    }

    /// Per-core 4-KiB pages of the request.
    pub(crate) fn pages(self) -> u16 {
        self.pages
    }

    /// 0x101d1 (compute), 0x101c9 (TA), 0x101c1 (FS): the pages and their 1-MiB count.
    pub(crate) fn size_word(self) -> u64 {
        let pages = u64::from(self.pages);
        pages | (pages.div_ceil(Self::PAGES_PER_MIB) << 16)
    }

    /// 0x107a1 (compute), 0x10799 (TA); the FS 0x10791 also sets bit 9.
    pub(crate) fn window_word(self) -> u64 {
        self.window
    }

    /// 0x1a0e9 (compute), 0x1a0f1 (TA), 0x1a0f9 (FS).
    pub(crate) fn stack_word(self) -> u64 {
        self.stack
    }

    /// 0x1a141 (compute), written only with `COMPUTE_SCS_DRIVER_WORD`.
    pub(crate) fn driver_word(self) -> u64 {
        self.driver
    }

    /// 0xd471 (TA), 0xd469 (FS).
    pub(crate) fn tail_word(self) -> u64 {
        self.tail
    }

    /// 0xd479 (compute).
    pub(crate) fn compute_tail_word(self) -> u64 {
        if self.enabled() {
            Self::COMPUTE_TAIL_ENABLED
        } else {
            0
        }
    }
}

/// Validated intersection tables (g17 `fw::compute::IntersectionTables`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct IntersectionTables {
    pub(crate) pool: u64,
    pub(crate) pool_bytes: u64,
    first: u32,
    count: u32,
    pub(crate) default_program: u64,
    default_control: u64,
}

impl IntersectionTables {
    pub(crate) const NONE: Self = Self {
        pool: 0,
        pool_bytes: 0,
        first: 0,
        count: 0,
        default_program: 0,
        default_control: 0,
    };

    /// The HAL200 Gen1 table: a 16-KiB aligned pool of 16-KiB units below 2^48, a byte-sized
    /// table length of 25 bits, 128-byte entries, and a default program whose 48-bit code
    /// address is 64-byte aligned and whose control word has bit 40 set.
    pub(crate) fn new(
        pool: u64,
        pool_bytes: u64,
        first: u32,
        count: u32,
        default_program: u64,
        default_control: u64,
    ) -> Result<Self> {
        if pool == 0 {
            if pool_bytes != 0
                || first != 0
                || count != 0
                || default_program != 0
                || default_control != 0
            {
                return Err(EINVAL);
            }
            return Ok(Self::NONE);
        }
        let end = pool.checked_add(pool_bytes).ok_or(EINVAL)?;
        let blocks_end = u64::from(first) + u64::from(count);
        let code = default_program & 0x0000_ffff_ffff_ffff;
        if pool & 0x3fff != 0
            || pool_bytes == 0
            || pool_bytes & 0x3fff != 0
            || end > (1u64 << 48)
            || (pool_bytes >> 14) >= (1u64 << 25)
            || count == 0
            || count >= (1 << 18)
            || blocks_end * 128 > pool_bytes
            || code & 63 != 0
            || default_control & !0x0000_7fff_0000_3fff != 0
            || (code == 0 && (default_program != 0 || default_control != 0))
            || (code != 0 && default_control & (1u64 << 40) == 0)
        {
            return Err(EINVAL);
        }
        Ok(Self {
            pool,
            pool_bytes,
            first,
            count,
            default_program,
            default_control,
        })
    }

    /// A render stage's 48-byte table: the auxiliary word (zero or 8..=32, only with a pool),
    /// four MBZ bytes, then the fields of [`Self::new`]. Bytes past 48 must be zero. The
    /// auxiliary word is validated only: the M4 Pro's render programs write the scratch tail
    /// where its compute program would write it.
    pub(crate) fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes
            .get(48..)
            .is_some_and(|tail| tail.iter().any(|&b| b != 0))
        {
            return Err(EINVAL);
        }
        let mut tail = [0u8; 48];
        let count = bytes.len().min(tail.len());
        tail[..count].copy_from_slice(&bytes[..count]);
        let u32_at =
            |at: usize| u32::from_le_bytes([tail[at], tail[at + 1], tail[at + 2], tail[at + 3]]);
        let u64_at = |at: usize| u64::from(u32_at(at)) | u64::from(u32_at(at + 4)) << 32;
        let auxiliary = u32_at(0);
        if u32_at(4) != 0 || (auxiliary != 0 && !(8..=32).contains(&auxiliary)) {
            return Err(EINVAL);
        }
        let tables = Self::new(
            u64_at(8),
            u64_at(16),
            u32_at(24),
            u32_at(28),
            u64_at(32),
            u64_at(40),
        )?;
        if auxiliary != 0 && !tables.enabled() {
            return Err(EINVAL);
        }
        Ok(tables)
    }

    pub(crate) fn enabled(self) -> bool {
        self.pool != 0
    }

    /// The 48-bit address of the default program, or zero.
    pub(crate) fn default_code(self) -> u64 {
        self.default_program & 0x0000_ffff_ffff_ffff
    }

    /// The six table words: compute 0x1a089, 0x1a091, 0x1a059, 0x1a061, 0x1a0b9, 0x1a0c1; TA
    /// 0x1a099, 0x1a0a1, 0x1a069, 0x1a071, 0x1a0c9, 0x1a0d1; FS 0x1a0a9, 0x1a0b1, 0x1a079,
    /// 0x1a081, 0x1a0d9, 0x1a0e1. The M4 Pro's encoding (AGXMetalG17P endComputePass): the
    /// first entry, the byte length, and two fixed pool words. Zero without tables, the value
    /// G16G's work lists write today.
    pub(crate) fn words(self) -> [u64; 6] {
        if !self.enabled() {
            return [0; 6];
        }
        [
            self.pool + u64::from(self.first) * 128,
            0xc000_0000 | (u64::from(self.count) << 7),
            0x6f_0000_0000,
            0x3feff,
            self.default_program,
            self.default_control,
        ]
    }
}

/// The ray state of a compute command.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ComputeRay {
    pub(crate) intersection: IntersectionTables,
    pub(crate) scratch: ScratchRequest,
}

impl ComputeRay {
    #[allow(dead_code, reason = "The empty state of the host regressions.")]
    pub(crate) const NONE: Self = Self {
        intersection: IntersectionTables::NONE,
        scratch: ScratchRequest::NONE,
    };

    /// As the M4 Pro (G17P command set): an auxiliary allocation is M5-only and must be zero,
    /// as must the reserved word; tables and scratch are independent of each other.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn new(
        scs_layout: u64,
        auxiliary: u32,
        reserved: u32,
        pool: u64,
        pool_bytes: u64,
        first: u32,
        count: u32,
        default_program: u64,
        default_control: u64,
    ) -> Result<Self> {
        if auxiliary != 0 || reserved != 0 {
            return Err(EINVAL);
        }
        Ok(Self {
            intersection: IntersectionTables::new(
                pool,
                pool_bytes,
                first,
                count,
                default_program,
                default_control,
            )?,
            scratch: ScratchRequest::new(scs_layout)?,
        })
    }
}

/// The ray state of one render stage (g17 `fw::render_rt::RayQueryStage`): index 0 the
/// vertex (tiling) stage, 1 the fragment stage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct RayQueryStage {
    pub(crate) intersection: IntersectionTables,
    pub(crate) scratch: ScratchRequest,
}

impl RayQueryStage {
    /// Size of one `drm_asahi_render_ray_query_stage`.
    pub(crate) const SIZE: usize = 56;

    #[allow(dead_code, reason = "The empty state of the host regressions.")]
    pub(crate) const NONE: Self = Self {
        intersection: IntersectionTables::NONE,
        scratch: ScratchRequest::NONE,
    };

    fn parse(wire: &[u8; Self::SIZE]) -> Result<Self> {
        let layout = u64::from_le_bytes([
            wire[0], wire[1], wire[2], wire[3], wire[4], wire[5], wire[6], wire[7],
        ]);
        let intersection = IntersectionTables::parse(&wire[8..])?;
        let scratch = ScratchRequest::new(layout)?;
        if scratch.enabled() != intersection.enabled() {
            return Err(EINVAL);
        }
        Ok(Self {
            intersection,
            scratch,
        })
    }

    /// Both stages from the render payload bytes after `prior_cdm`: shorter payloads are read
    /// zero-filled, bytes past the two stages must be zero.
    pub(crate) fn parse_pair(bytes: &[u8]) -> Result<[Self; 2]> {
        if bytes
            .get(2 * Self::SIZE..)
            .is_some_and(|tail| tail.iter().any(|&b| b != 0))
        {
            return Err(EINVAL);
        }
        let mut wire = [[0u8; Self::SIZE]; 2];
        for (index, stage) in wire.iter_mut().enumerate() {
            let start = (index * Self::SIZE).min(bytes.len());
            let end = ((index + 1) * Self::SIZE).min(bytes.len());
            stage[..end - start].copy_from_slice(&bytes[start..end]);
        }
        Ok([Self::parse(&wire[0])?, Self::parse(&wire[1])?])
    }

    /// Whether the stage carries intersection tables.
    pub(crate) fn traces(self) -> bool {
        self.intersection.enabled()
    }
}

/// Only the explicit AS-build bit (bit 2) is accepted; reserved and unrelated compute flags
/// stay invalid on G16G, as on the M4 Pro (g17 `timeout::as_build`).
pub(crate) fn as_build(flags: u32) -> Option<bool> {
    const AS_BUILD: u32 = 1 << 2;
    if flags & !AS_BUILD != 0 {
        None
    } else {
        Some(flags & AS_BUILD != 0)
    }
}

/// Whether a command needs the long execution allowance (g17 `timeout::select`): a compute
/// command with intersection tables, a render with tables in either stage, or an AS build.
pub(crate) fn select(compute_ray: bool, render_ray: [bool; 2], as_build: bool) -> bool {
    compute_ray || render_ray.into_iter().any(|enabled| enabled) || as_build
}

/// Freelist entries (4-KiB pages) of a UMA pool without SCS, the value G16G seeds today.
pub(crate) const BASE_SEEDS: u32 = 8192;
/// Records of a base UMA freelist: the capacity G16G's managers report at +0x1c for their
/// 0x20000-byte freelist (`UNK_1C_VALUE`).
pub(crate) const FREELIST_RECORDS: u32 = 16384;
/// Records of an SCS-capable pool's freelist: the +0x1c macOS 27.2 on the J773g writes on
/// every pool, over a 4-MiB freelist, whatever the pool's rows (20 to 108 seen).
pub(crate) const SCS_FREELIST_RECORDS: u32 = 0x80000;
/// Bytes of one freelist record, the 64-bit address of one 4-KiB seed.
pub(crate) const RECORD_BYTES: u64 = 8;
/// Bytes of one seed.
pub(crate) const SEED_BYTES: u64 = 0x1000;
/// Freelist entries of one 2-MiB pool-table row.
pub(crate) const SEEDS_PER_ROW: u32 = 512;
/// The most freelist entries of an SCS-capable UMA pool: the whole rows whose pool and
/// freelist fit one SCS pool window (62 rows, 124 MiB, and the 4-MiB freelist). The 256-row
/// pool table holds more.
pub(crate) const MAX_SCS_SEEDS: u32 = max_window_seeds(super::hw::client::SCS_POOL_WINDOW);
const _: () = assert!(BASE_SEEDS % SEEDS_PER_ROW == 0 && MAX_SCS_SEEDS % SEEDS_PER_ROW == 0);
const _: () = assert!(BASE_SEEDS < FREELIST_RECORDS && FREELIST_RECORDS < MAX_SCS_SEEDS);
const _: () = assert!(MAX_SCS_SEEDS < SCS_FREELIST_RECORDS);
const _: () = assert!(freelist_records(BASE_SEEDS) as u64 * RECORD_BYTES == 0x20000);

/// The freelist records (+0x1c) of a UMA pool with `seeds` entries. +0x1c is the record count
/// of the freelist +0x14 points at. A base pool keeps the 16384 G16G has always written; an
/// SCS-capable pool (more than the base seeds) gets [`SCS_FREELIST_RECORDS`], as macOS 27.2 on
/// the J773g gives every pool.
pub(crate) const fn freelist_records(seeds: u32) -> u32 {
    if seeds > BASE_SEEDS {
        SCS_FREELIST_RECORDS
    } else {
        FREELIST_RECORDS
    }
}

/// Bytes of the freelist of a pool with `seeds` entries, in whole 16-KiB pages.
pub(crate) const fn freelist_bytes(seeds: u32) -> u64 {
    (freelist_records(seeds) as u64 * RECORD_BYTES).next_multiple_of(0x4000)
}

/// Bytes an SCS pool window needs for a pool of `seeds` entries and, after it, its freelist.
pub(crate) const fn window_bytes(seeds: u32) -> u64 {
    seeds as u64 * SEED_BYTES + freelist_bytes(seeds)
}

/// The most whole-row seeds whose pool and freelist fit `window` bytes.
const fn max_window_seeds(window: u64) -> u32 {
    let mut seeds = (window / SEED_BYTES) as u32 / SEEDS_PER_ROW * SEEDS_PER_ROW;
    while seeds != 0 && window_bytes(seeds) > window {
        seeds -= SEEDS_PER_ROW;
    }
    seeds
}

/// The 4-KiB pages one SCS request adds to a UMA pool for `active_cores` cores, as the M4 Pro
/// sizes its free list (g17 `fw::compute_pool::extra_bytes`): per core the request's pages,
/// their 1-MiB large-page inventory (pages / 256, rounded up) and eight tail pages.
pub(crate) fn extra_pages(active_cores: u32, scs_pages: u16) -> Result<u64> {
    if active_cores == 0 {
        return Err(EINVAL);
    }
    if scs_pages == 0 {
        return Ok(0);
    }
    let small = u64::from(scs_pages);
    small
        .checked_add(small.div_ceil(256))
        .and_then(|pages| pages.checked_add(8))
        .and_then(|pages| pages.checked_mul(u64::from(active_cores)))
        .ok_or(EOVERFLOW)
}

/// Freelist entries a UMA pool needs to serve SCS requests of `pages` per stage at once (one
/// compute dispatch, or the two stages of one render): the base pool plus every stage's extra
/// pages, rounded up to whole pool-table rows. E2BIG beyond the largest SCS-capable pool
/// ([`MAX_SCS_SEEDS`]); without SCS it is [`BASE_SEEDS`].
pub(crate) fn required_seeds(active_cores: u32, pages: &[u16]) -> Result<u32> {
    let mut seeds = u64::from(BASE_SEEDS);
    for &stage in pages {
        seeds = seeds
            .checked_add(extra_pages(active_cores, stage)?)
            .ok_or(EOVERFLOW)?;
    }
    let row = u64::from(SEEDS_PER_ROW);
    let seeds = seeds.checked_add(row - 1).ok_or(EOVERFLOW)? / row * row;
    if seeds > u64::from(MAX_SCS_SEEDS) {
        return Err(E2BIG);
    }
    u32::try_from(seeds).map_err(|_| E2BIG)
}

/// The SCS-capable UMA pool a command runs on: one of at least `needed` freelist entries, or
/// if its VM has none yet, a new one of `pool` ([`pool_seeds`]). Both zero without scratch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ScsSeeds {
    pub(crate) needed: u32,
    pub(crate) pool: u32,
}

impl ScsSeeds {
    /// No scratch.
    pub(crate) const NONE: Self = Self { needed: 0, pool: 0 };
}

/// Freelist entries of the SCS-capable pools created for requests of `pages` per stage (a
/// render's two stages, or a compute dispatch and zero). The pools serve every later job of the
/// VM and cannot grow once the hardware may hold them, so they hold two stages at the larger
/// one where a window holds that, otherwise the request itself; E2BIG if neither fits.
///
/// - A render pool then takes a later queue that reserves the other stage too (Mesa replaces
///   its queue to add one).
/// - A compute pool gets two dispatches' extras. One is less than macOS provisions: for its
///   own ray-query kernel of 1703 pages per core, macOS 27.2 on the J773g grew the compute pool
///   to 108 rows, about 2.6 times the extras of [`extra_pages`].
pub(crate) fn pool_seeds(active_cores: u32, pages: [u16; 2]) -> Result<u32> {
    let largest = pages[0].max(pages[1]);
    required_seeds(active_cores, &[largest, largest])
        .or_else(|_| required_seeds(active_cores, &pages))
}
