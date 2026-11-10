// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! H1 admission and retained-owner construction. H2 is deliberately excluded.
//! The caller must execute every runtime readback gate before publication.
use crate::{driver::AsahiDrmDevice, g18::runtime::graphics_job::G18GraphicsJob,
    g18::fw::render_assembly::RenderVariant, mmu};
use kernel::prelude::*;

pub(crate) const STAGE_RENDER: u32 = 9;
pub(crate) const H1_ADMITTED: bool = true;

pub(crate) fn options(variant: u32, profile: u32) -> Result<RenderVariant> {
    let variant = RenderVariant::parse(variant).ok_or(EINVAL)?;
    let admitted = ((1..=2).contains(&profile)
        && matches!(variant, RenderVariant::Color | RenderVariant::Clear))
        || (profile == 4 && variant == RenderVariant::Color);
    if !admitted { return Err(EINVAL); }
    Ok(variant)
}

/// Called before prepare_t8152_device: rejection precedes MMIO, allocation,
/// firmware startup and the init-data publication boundary.
pub(crate) fn admit(dev: &kernel::device::Device, variant: u32, profile: u32) -> Result {
    let variant = options(variant, profile)?;
    if !H1_ADMITTED {
        dev_err!(dev, "G18_RENDER_BLOCKED variant={:?} profile={} frames={} stages={}: native command policy, graphics UMA backing, PB/queue binding and complete pointer/difference ledger are not admitted; refused before MMIO\n",
            variant, profile, variant.frames(), variant.stage_count());
        return Err(ENOTSUPP);
    }
    dev_info!(dev, "G18_RENDER_ADMIT variant={:?} profile={} policy=captured-M5-H1 single-pass\n", variant, profile);
    dev_info!(dev, "G18_RENDER_RESOURCE_PAIRS {:?}\n", crate::g18::uapi::render_policy::resource_pairs());
    dev_info!(dev, "G18_RENDER_TA_BARRIER_FLAG {}\n", crate::g18::uapi::render_policy::ta_barrier_flag());
    Ok(())
}

#[inline(never)]
pub(crate) fn prepare(dev: &AsahiDrmDevice, uat: &mmu::Uat) -> Result<KBox<G18GraphicsJob>> {
    let variant = crate::g18::params::g18_render_variant.value();
    let profile = crate::g18::params::g18_render_profile.value();
    let options = options(variant, profile)?;
    G18GraphicsJob::from_payload(dev, uat, profile, options)
}
