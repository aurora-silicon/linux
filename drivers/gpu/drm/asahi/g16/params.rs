// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Module parameters and the GET_PARAMS identity.
//!
//! With the shared `file.rs` additions (VM_STATUS and VM_STATUS_MIRROR from `supports_vm_status`,
//! SOFT_FAULTS suppressed by `soft_faults`), the feature word is 0x9c02: per-command input
//! readiness/errors and exact queue-local historical prefixes use explicit retained inputs
//! and successful command receipt frontiers; RENDER_RAY_QUERY (bit 15) is the M4 Pro's
//! ray-query command set ([`super::rt`]). No independent hardware queue reservation or
//! fragment barriers are advertised.

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

/// Runtime tunables, written through `aurora-g16/tune` (debugfs, root only) as `name=value`.
pub(crate) mod tune {
    use core::sync::atomic::{AtomicU32, Ordering};

    /// Compute units of a VM that submits compute back to back
    /// (2..=[`super::hw::sizes::COMPUTE_UNITS`]); applies to VMs that grow after the change.
    pub(crate) static COMPUTE_UNITS: AtomicU32 = AtomicU32::new(6);
    /// Compute commands in flight at most (1 = the serialized engine).
    pub(crate) static COMPUTE_FLIGHTS: AtomicU32 = AtomicU32::new(16);
    /// Completion repoll interval override, us (0: `g16_resnap_us`).
    pub(crate) static RESNAP_US: AtomicU32 = AtomicU32::new(0);
    /// PB heap of a VM's render arena, MiB (VMs created after a change).
    pub(crate) static PB_HEAP_MB: AtomicU32 = AtomicU32::new(64);
    /// PB blocks posted to a fresh epoch at most (2048: the whole heap).
    pub(crate) static PB_BLOCKS: AtomicU32 = AtomicU32::new(2048);
    const NAMES: [(&str, &AtomicU32, u32, u32); 5] = [
        ("compute_units", &COMPUTE_UNITS, 2, super::hw::sizes::COMPUTE_UNITS as u32),
        ("compute_flights", &COMPUTE_FLIGHTS, 1, super::hw::sizes::FLIGHTS as u32),
        ("resnap_us", &RESNAP_US, 0, 1000),
        ("pb_heap_mb", &PB_HEAP_MB, 8, 256),
        ("pb_blocks", &PB_BLOCKS, 16, 2048),
    ];

    /// Applies one `name=value` line.
    pub(crate) fn set(line: &str) -> kernel::error::Result {
        let (name, value) = line.trim().split_once('=').ok_or(kernel::error::code::EINVAL)?;
        let value: u32 = value
            .trim()
            .parse()
            .map_err(|_| kernel::error::code::EINVAL)?;
        for (n, cell, lo, hi) in NAMES {
            if n == name.trim() {
                if value < lo || value > hi {
                    return Err(kernel::error::code::ERANGE);
                }
                cell.store(value, Ordering::Relaxed);
                return Ok(());
            }
        }
        Err(kernel::error::code::ENOENT)
    }

    pub(crate) fn show(f: &mut impl core::fmt::Write) -> core::fmt::Result {
        for (n, cell, _, _) in NAMES {
            write!(f, "{}={} ", n, cell.load(Ordering::Relaxed))?;
        }
        writeln!(f)
    }
}

/// PB heap blocks of a new render arena (128 KiB each).
pub(crate) fn pb_heap_blocks() -> u32 {
    let mb = tune::PB_HEAP_MB.load(core::sync::atomic::Ordering::Relaxed).clamp(8, 256);
    mb * 8
}

/// PB blocks posted to a fresh epoch of a heap of `heap_blocks`.
pub(crate) fn pb_posted_blocks(heap_blocks: u32) -> u32 {
    tune::PB_BLOCKS
        .load(core::sync::atomic::Ordering::Relaxed)
        .clamp(16, 2048)
        .min(heap_blocks)
}

/// Compute units of a VM that submits compute back to back.
pub(crate) fn compute_units() -> usize {
    (tune::COMPUTE_UNITS.load(core::sync::atomic::Ordering::Relaxed) as usize)
        .clamp(2, hw::sizes::COMPUTE_UNITS)
}

/// Compute commands in flight at most.
pub(crate) fn compute_flights() -> usize {
    (tune::COMPUTE_FLIGHTS.load(core::sync::atomic::Ordering::Relaxed) as usize)
        .clamp(1, hw::sizes::FLIGHTS)
}

/// Repoll while a ready prefix waits for record consumption, us.
pub(crate) fn repoll_us() -> u32 {
    module_parameters::g16_repoll_us.value().clamp(5, 100)
}

/// Completion repoll interval, us.
pub(crate) fn resnap_us() -> u32 {
    match tune::RESNAP_US.load(core::sync::atomic::Ordering::Relaxed) {
        0 => module_parameters::g16_resnap_us.value().clamp(25, 1000),
        us => us.clamp(10, 1000),
    }
}

/// Whether firmware halts are answered with the handshake (else fail-stop).
pub(crate) fn recovery() -> bool {
    module_parameters::g16_recovery.value() != 0
}

/// Whether a pass slot's retired PB epoch may serve the next render of its VM.
pub(crate) fn pb_reuse() -> bool {
    module_parameters::g16_pb_reuse.value() != 0
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
        | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_EXACT_PRIOR_BARRIERS)
        | u64::from(uapi::drm_asahi_feature_DRM_ASAHI_FEATURE_RENDER_RAY_QUERY);
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
