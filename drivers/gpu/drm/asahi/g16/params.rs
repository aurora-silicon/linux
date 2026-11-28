// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module parameters and the GET_PARAMS identity.
//!
//! With the shared `file.rs` additions (VM_STATUS and VM_STATUS_MIRROR from `supports_vm_status`,
//! SOFT_FAULTS from `soft_faults` and `fault_control`, VDM_USC_INVALIDATE, and
//! `INDEPENDENT_QUEUES` from `queue_limits`), G16 advertises per-command input readiness/errors,
//! exact queue-local historical prefixes, fragment-only and staged render barriers
//! (`RENDER_STAGE_PREFIXES_V2`), independent logical queues, `FEW_PRIMITIVES`,
//! `COMPUTE_NO_QUERY_ACCESS`, render ray queries (`RENDER_RAY_QUERY`) and bounded AS builds
//! (`BOUNDED_AS_BUILD`).

use kernel::{prelude::*, uapi};

use super::{hw, Gpu};
use crate::module_parameters;

/// Operations per VM_BIND batch at most: batches are atomic and may be large (sparse residency
/// updates); this bounds the kernel's staging allocation.
pub(crate) const MAX_BINDS: u32 = 4096;

/// Administrative completion-stamp deadline, ms (zero disables, otherwise at least 100).
pub(crate) fn deadline_ms() -> u32 {
    module_parameters::g16_deadline_ms.value()
}

/// Compute-only administrative deadline, ms. Zero permits long computes without a host
/// timeout; it does not disable authenticated firmware fault/recovery handling.
pub(crate) fn compute_deadline_ms() -> u32 {
    module_parameters::g16_compute_deadline_ms.value()
}

/// PB heap blocks of a new render arena (64 MiB in 128 KiB blocks).
pub(crate) fn pb_heap_blocks() -> u32 {
    64 * 8
}

/// PB blocks posted to a fresh epoch of a heap of `heap_blocks`.
pub(crate) fn pb_posted_blocks(heap_blocks: u32) -> u32 {
    2048u32.min(heap_blocks)
}

/// Compute units of a VM that submits compute back to back.
pub(crate) fn compute_units() -> usize {
    hw::sizes::COMPUTE_UNITS
}

/// Compute commands in flight at most.
pub(crate) fn compute_flights() -> usize {
    32usize.clamp(1, hw::sizes::FLIGHTS)
}

/// Render commands in flight at most: bounded by both retained pass slots
/// (`RENDER_SLOTS - 1`, leaving one slot for displacement) and firmware TA/FS work-ring
/// capacity (`(QUEUE_CAP - 1) / MAX_RENDER_RING_ENTRIES`, worst case 5 ring entries per pass).
/// Decoupled from `hw::sizes::FLIGHTS` because multi-pass render batches pack multiple passes
/// into one `Flight`.
pub(crate) fn render_flights() -> usize {
    let ring_cap =
        (hw::sizes::QUEUE_CAP as usize).saturating_sub(1) / hw::sizes::MAX_RENDER_RING_ENTRIES;
    hw::sizes::RENDER_SLOTS
        .saturating_sub(1)
        .min(ring_cap)
        .max(1)
}

/// Repoll while a ready prefix waits for record consumption, us.
pub(crate) fn repoll_us() -> u32 {
    module_parameters::g16_repoll_us.value().clamp(5, 100)
}

/// Completion repoll interval, us.
pub(crate) fn resnap_us() -> u32 {
    module_parameters::g16_resnap_us.value().clamp(25, 1000)
}

/// Whether firmware halts are answered with the handshake (else fail-stop).
pub(crate) fn recovery() -> bool {
    module_parameters::g16_recovery.value() != 0
}

/// Whether the CDM/VDM stream gates are on.
pub(crate) fn stream_gate() -> bool {
    module_parameters::g16_stream_gate.value() != 0
}

/// Fragment-only render barriers overlap a pipelined render's TA with the prior render's FS.
pub(crate) fn early_tiling() -> bool {
    true
}

/// Fills the generation part of GET_PARAMS group 0.
pub(crate) fn fill(gpu: &Gpu, params: &mut uapi::drm_asahi_params_global) -> Result {
    params.features |= u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_PER_COMMAND_INPUT_SYNCS)
        | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_EXACT_PRIOR_BARRIERS)
        | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_FRAGMENT_BARRIERS)
        | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_COMPUTE_NO_QUERY_ACCESS)
        | if gpu.cfg.gpu_generation >= 16 {
            u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_FEW_PRIMITIVES)
                | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_RENDER_RAY_QUERY)
                | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_BOUNDED_AS_BUILD)
        } else {
            0
        }
        | if gpu.cfg.max_tile_buffer >= hw::sizes::TILE_BUFFER_48K {
            crate::file::DRM_ASAHI_FEATURE_TILE_BUFFER_48K
        } else {
            0
        }
        | crate::g17::sync_output::FEATURE_CUMULATIVE_SIGNALS
        | crate::file::DRM_ASAHI_FEATURE_RENDER_STAGE_PREFIXES_V2;
    params.gpu_generation = gpu.cfg.gpu_generation;
    params.gpu_variant = gpu.cfg.gpu_variant as u32;
    params.gpu_revision = gpu.cfg.gpu_revision;
    params.chip_id = gpu.cfg.chip_id;
    params.num_dies = 1;
    params.num_clusters_total = gpu.cfg.num_clusters;
    params.num_cores_per_cluster = gpu.cfg.num_cores_per_cluster;
    params.max_frequency_khz = gpu.max_frequency_khz;
    params.usc_generation = 3;
    params.gpu_hal_generation = gpu.cfg.gpu_hal_generation;
    params.max_commands_per_submission = super::command::MAX_COMMANDS;
    params.max_attachments = hw::sizes::MAX_ATTACHMENTS as u32;
    for (i, mask) in gpu.id.core_masks.iter().enumerate() {
        *(params.core_masks.get_mut(i).ok_or(EIO)?) = (*mask).into();
    }
    Ok(())
}

/// QUEUE_LIMITS: a policy bound on logical queues (they own no firmware state) and the
/// number of prefixes in flight.
pub(crate) fn queue_limits() -> uapi::drm_asahi_queue_limits {
    uapi::drm_asahi_queue_limits {
        max_queues: hw::sizes::MAX_QUEUES,
        max_in_flight_per_queue: hw::sizes::FLIGHTS as u32,
        ..Default::default()
    }
}
