// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module parameters and the GET_PARAMS identity.
//!
//! With the shared `file.rs` additions (VM_STATUS and VM_STATUS_MIRROR from `supports_vm_status`,
//! SOFT_FAULTS suppressed by `soft_faults`), the feature word is 0x1c02: per-command input
//! readiness/errors and exact queue-local historical prefixes use explicit retained inputs
//! and successful command receipt frontiers. No independent hardware queue reservation,
//! fragment barriers or ray queries are advertised.

use kernel::{prelude::*, uapi};

use super::{hw, Gpu, GPU_GENERATION};
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

/// Fragment-only dependency publication has not been qualified on this backend.
pub(crate) fn early_tiling() -> bool {
    false
}

/// Fills the generation part of GET_PARAMS group 0.
pub(crate) fn fill(gpu: &Gpu, params: &mut uapi::drm_asahi_params_global) -> Result {
    // Match the working J713 C backend identity; SGX register revision fields are
    // not the userspace BCD revision.
    if gpu.cfg.chip_id != 0x8132 || gpu.cfg.gpu_revision != 0x10 {
        return Err(ENODEV);
    }
    params.features |= u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_PER_COMMAND_INPUT_SYNCS)
        | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_EXACT_PRIOR_BARRIERS);
    params.gpu_generation = GPU_GENERATION;
    params.gpu_variant = gpu.cfg.gpu_variant as u32;
    params.gpu_revision = gpu.cfg.gpu_revision;
    params.chip_id = gpu.cfg.chip_id;
    params.num_dies = 1;
    params.num_clusters_total = gpu.cfg.num_clusters;
    params.num_cores_per_cluster = gpu.cfg.num_cores_per_cluster;
    params.max_frequency_khz = gpu.max_frequency_khz;
    params.usc_generation = 3;
    params.gpu_hal_generation = uapi::drm_asahi_gpu_hal_generation_DRM_ASAHI_GPU_HAL_200;
    params.max_commands_per_submission = super::command::MAX_COMMANDS;
    params.max_attachments = hw::sizes::MAX_ATTACHMENTS as u32;
    for (i, mask) in gpu.id.core_masks.iter().enumerate() {
        *(params.core_masks.get_mut(i).ok_or(EIO)?) = (*mask).into();
    }
    Ok(())
}
