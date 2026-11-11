// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Versioned per-dispatch compiler metadata, before any GPU allocation.

use crate::g18::uapi::compute_inputs::{G18ComputeDemand, G18ComputeInputs, G18ComputeShader,
    G18_TGMEM_POLICY};

pub(crate) struct G18ComputeMetadata {
    pub(crate) demand: G18ComputeDemand,
    pub(crate) descriptor: [u32; 23],
}

impl G18ComputeMetadata {
    /// `drm_asahi_g18_compute_resources`, sixteen little-endian u32 words.
    /// Unknown local geometry has zero dimensions and its own flag; it must
    /// not accidentally become a one-thread group in the native accumulator.
    pub(crate) fn decode(words: [u32; 16]) -> Option<Self> {
        let flags = words[1];
        if !matches!(words[0], 1 | 2) || flags & !0xf != 0 || words[15] != 0 {
            return None;
        }
        let local = if flags & 8 != 0 {
            if words[12..15] != [0; 3] { return None; }
            None
        } else {
            Some([words[12], words[13], words[14]])
        };
        let shader = G18ComputeShader {
            registers_16: words[2],
            scratch_bytes: words[3],
            spectpr_scratch_bytes: words[4],
            staging_16: words[5],
            staging_known: flags & 1 != 0,
            constant_present: flags & 2 != 0,
            constant_registers_16: words[6],
            constant_scratch_bytes: words[7],
            constant_staging_16: words[8],
            constant_staging_known: flags & 4 != 0,
            local_bytes: words[9],
            imageblock_stride: words[10],
        };
        let demand = if words[0] == 2 {
            G18ComputeDemand::from_shader_element_indices(shader, local, words[11])?
        } else {
            G18ComputeDemand::from_shader(shader, local, words[11])?
        };
        let mut inputs = G18ComputeInputs::new();
        inputs.merge(demand, G18_TGMEM_POLICY)?;
        inputs.finish()?;
        Some(Self { demand, descriptor: inputs.descriptor() })
    }
}
