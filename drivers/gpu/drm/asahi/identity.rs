// SPDX-License-Identifier: GPL-2.0-only OR MIT


/// Supported GPU generation enumeration. Note: Part of the UABI.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuGen {
    G13 = 13,
    G14 = 14,
    G15 = 15,
}

/// GPU variant enumeration. Note: Part of the UABI.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuVariant {
    P = 'P' as u32,
    G = 'G' as u32,
    S = 'S' as u32,
    C = 'C' as u32,
    D = 'D' as u32,
}

#[derive(Debug, PartialEq, Eq, Copy, Clone)]
#[repr(u32)]
pub(crate) enum GpuHalGeneration {
    Legacy = 0,
}

/// Public architecture axes for the G15 ID-register decoder.
#[derive(Debug, PartialEq, Eq, Copy, Clone)]
pub(crate) struct GpuIdentity {
    pub(crate) gpu_gen: GpuGen,
    pub(crate) gpu_variant: GpuVariant,
    pub(crate) usc_generation: u32,
    pub(crate) gpu_hal_generation: GpuHalGeneration,
}

pub(crate) fn decode_gpu_identity(family: u8, variant: u8, num_dies: u8) -> Option<GpuIdentity> {
    use GpuVariant::*;
    if family != 7 { return None; }
    let (gpu_variant, usc_generation) = match variant {
        0 => (P, 2),
        2 => (G, 3),
        3 => (S, 3),
        4 => match num_dies { 1 => (C, 3), 2 => (D, 3), _ => return None },
        _ => return None,
    };
    Some(GpuIdentity { gpu_gen: GpuGen::G15, gpu_variant, usc_generation,
        gpu_hal_generation: GpuHalGeneration::Legacy })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn g15_variants_keep_their_public_identity() {
        assert_eq!(decode_gpu_identity(7, 3, 1).unwrap().gpu_variant, GpuVariant::S);
        assert_eq!(decode_gpu_identity(7, 4, 1).unwrap().gpu_variant, GpuVariant::C);
        assert_eq!(decode_gpu_identity(7, 4, 2).unwrap().gpu_variant, GpuVariant::D);
        assert_eq!(decode_gpu_identity(7, 2, 1).unwrap().usc_generation, 3);
        assert!(decode_gpu_identity(7, 4, 0).is_none());
        assert!(decode_gpu_identity(10, 2, 1).is_none());
    }
}
