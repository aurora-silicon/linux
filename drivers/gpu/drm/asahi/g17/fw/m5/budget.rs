// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! The private-pool size a queue's demand needs (S `g17/fw/m5/pool_budget.rs`), on the product's
//! pool policy. Counts describe hardware and simultaneous work; byte requirements come from the
//! submitted programs ([`super::demand`]). Compute and render queues size their pools through
//! this one definition.

use super::pool::{PoolError, PoolGeometry};

/// The product's pool policy: the normal allocation block per core type (the block table)
/// and the topology constant's product term. Core type 0x18 is G17S (S's 4 MiB blocks, no product
/// term), 0x19 G17C (8 MiB blocks, linux-aurora#32's fix), 0x1a G17D (16 MiB).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Product {
    pub(crate) block_bytes: usize,
    /// Pages the constant reservation adds per cluster: 16 x (core type - 0x18).
    constant_pages: u32,
}

impl Product {
    pub(crate) const fn of(core_type: u32) -> Option<Self> {
        let block_bytes = match core_type {
            0x18 => 0x40_0000,
            0x19 => 0x80_0000,
            0x1a => 0x100_0000,
            _ => return None,
        };
        Some(Self {
            block_bytes,
            constant_pages: 16 * (core_type - 0x18),
        })
    }
}

/// Pages of the minimum parameter buffer (HW data +0x26b0): 32 plus 16 per cluster
/// (linux-aurora#32), 64 on J714's two clusters (0x200000) and 160 on eight.
const PARAMETER_PAGE: u32 = 0x8000;

/// The minimum parameter buffer of a topology of `clusters` clusters, in its 32 KiB pages: the
/// last word of the tiling cluster controls.
pub(crate) const fn minimum_parameter_pages(clusters: u32) -> Option<u32> {
    if clusters == 0 {
        return None;
    }
    match clusters.checked_mul(16) {
        Some(pages) => pages.checked_add(32),
        None => None,
    }
}

/// The same minimum in bytes: HW data +0x26b0.
pub(crate) const fn minimum_parameter_bytes(clusters: u32) -> Option<u32> {
    match minimum_parameter_pages(clusters) {
        Some(pages) => pages.checked_mul(PARAMETER_PAGE),
        None => None,
    }
}

#[derive(Clone, Copy)]
pub(crate) struct RenderBudget {
    active_cores: u32,
    constant_pages: u32,
    active_kicks: u32,
    block_bytes: u64,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct Requirements {
    pub(crate) minimum_bytes: u64,
    pub(crate) ideal_bytes: u64,
    pub(crate) self_reserve_pages: u32,
    pub(crate) render_target_pages: u32,
    pub(crate) has_spill: bool,
}

/// Retained high-water marks, excluding per-kick reservations from the two
/// base byte counts. They may increase demand but never replace live demand.
#[derive(Clone, Copy, Default)]
pub(crate) struct History {
    pub(crate) minimum_base: u64,
    pub(crate) ideal_base: u64,
    pub(crate) self_reserve: u64,
    pub(crate) render_target: u64,
    pub(crate) minimum_pool: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Sizes {
    pub(crate) minimum: u64,
    pub(crate) ideal: u64,
    block_bytes: u64,
}

fn add(a: u64, b: u64) -> Result<u64, PoolError> {
    a.checked_add(b).ok_or(PoolError::Geometry)
}
fn mul(a: u64, b: u64) -> Result<u64, PoolError> {
    a.checked_mul(b).ok_or(PoolError::Geometry)
}

impl RenderBudget {
    /// `core_masks` contains the enabled cores of each physical cluster.
    /// `ptc_floor` is the configured per-core context count; `active_kicks`
    /// includes simultaneously resident tiling and fragment work.
    pub(crate) fn new(
        core_masks: &[u32],
        ptc_floor: u32,
        active_kicks: u32,
        product: Product,
    ) -> Result<Self, PoolError> {
        let mut active_cores = 0u32;
        let mut cluster_cores = 0u32;
        for mask in core_masks {
            let count = mask.count_ones();
            active_cores = active_cores.checked_add(count).ok_or(PoolError::Geometry)?;
            cluster_cores = cluster_cores.max(count);
        }
        if active_cores == 0 || active_kicks == 0 || ptc_floor == 0 {
            return Err(PoolError::Geometry);
        }
        let constant_pages = cluster_cores
            .checked_mul(ptc_floor)
            .and_then(|n| n.checked_add(32 + product.constant_pages))
            .and_then(|n| n.checked_mul(u32::try_from(core_masks.len()).ok()?))
            .ok_or(PoolError::Geometry)?;
        Ok(Self {
            active_cores,
            constant_pages,
            active_kicks,
            block_bytes: product.block_bytes as u64,
        })
    }

    /// Render's allocator publishes the already rounded auxiliary page count
    /// at descriptor +0x38, including its eight-page tail. This is separate
    /// from the layout-derived small/large SCS inventories. The
    /// USC spill-buffer allocation is their sum times the core count.
    pub(crate) fn render_scs_requirements(
        &self,
        mut request: Requirements,
        pages: u16,
        auxiliary_pages: u32,
    ) -> Result<Requirements, PoolError> {
        if pages != 0 {
            let small = u64::from(pages);
            let per_core = add(add(small, small.div_ceil(256))?, u64::from(auxiliary_pages))?;
            let bytes = mul(mul(per_core, u64::from(self.active_cores))?, 4096)?;
            request.minimum_bytes = add(request.minimum_bytes, bytes)?;
            request.ideal_bytes = add(request.ideal_bytes, bytes)?;
            request.has_spill = true;
        }
        Ok(request)
    }

    fn aligned(&self, bytes: u64) -> Result<u64, PoolError> {
        Ok(add(bytes, self.block_bytes - 1)? & !(self.block_bytes - 1))
    }

    pub(crate) fn sizes(
        &self,
        request: Requirements,
        history: History,
    ) -> Result<Sizes, PoolError> {
        let reserve = u64::from(request.self_reserve_pages) << 12;
        let rt = u64::from(request.render_target_pages) << 12;
        let reserved = add(reserve, rt)?;
        if request.minimum_bytes < reserved || request.ideal_bytes < reserved {
            return Err(PoolError::Geometry);
        }
        // Every enabled USC has execution-state storage. Programs with spill
        // state add twelve pages; shared queues reserve another twenty-four.
        let execution = mul(
            u64::from(self.active_cores),
            if request.has_spill {
                76 << 12
            } else {
                64 << 12
            },
        )?;
        let constant = mul(
            u64::from(self.constant_pages),
            u64::from(self.active_kicks - 1) << 12,
        )?;
        let per_kick = mul(
            add(
                reserve.max(history.self_reserve),
                rt.max(history.render_target),
            )?,
            u64::from(self.active_kicks),
        )?;
        let size = |base: u64, high_water: u64| {
            let base = add(base, execution)?
                .checked_sub(reserved)
                .ok_or(PoolError::Geometry)?
                .max(high_water);
            add(add(base, constant)?, per_kick)
        };
        let ideal = self.aligned(size(request.ideal_bytes, history.ideal_base)?)?;
        let minimum = self.aligned(
            size(request.minimum_bytes, history.minimum_base)?.min(ideal.max(history.minimum_pool)),
        )?;
        if minimum > ideal {
            return Err(PoolError::Geometry);
        }
        Ok(Sizes {
            minimum,
            ideal,
            block_bytes: self.block_bytes,
        })
    }
}

impl Sizes {
    /// Backed blocks of the ideal size, from which S allocates.
    pub(crate) fn blocks(self) -> Result<usize, PoolError> {
        if self.minimum == 0 || self.minimum > self.ideal || self.ideal % self.block_bytes != 0 {
            return Err(PoolError::Geometry);
        }
        usize::try_from(self.ideal / self.block_bytes).map_err(|_| PoolError::Geometry)
    }

    /// Reserve the full address inventory but allocate only the desired
    /// physical blocks. The hardware directory limit is checked by geometry.
    pub(crate) fn geometry(self) -> Result<PoolGeometry, PoolError> {
        PoolGeometry::of(self.blocks()?, self.block_bytes as usize)
    }
}
