// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Typed absolute-power inputs. These are separate firmware objects/fields.

/// Power-state layout shared by the admitted G17P and G16S builds.
/// Alignment stays one for embedding in packed PrimaryStatusB. The live
/// publisher separately validates the natural alignment of each u32 mapping.
#[repr(C, packed)]
pub(crate) struct PowerState {
    pub(crate) prefix: [u8; 0x128],
    pub(crate) ppm_enabled: u32,
    /// Absolute ApplePPM budget in milliwatts, independent of RegionC's maximum.
    pub(crate) ppm_target_mw: u32,
    pub(crate) suffix: [u8; 0x590],
}

/// G16S 26A428 has a different RegionC prefix. This type records that boundary
/// only: there is no admitted graph/variant for it, so it cannot be published.
#[repr(C)]
pub(crate) struct RegionCPowerG16S26A428 {
    pub(crate) prefix: [u8; 0x88],
    pub(crate) max_power_mw: u32,
}

const _: () = {
    assert!(core::mem::offset_of!(PowerState, ppm_enabled) == 0x128);
    assert!(core::mem::offset_of!(PowerState, ppm_target_mw) == 0x12c);
    assert!(core::mem::size_of::<PowerState>() == 0x6c0);
    assert!(core::mem::offset_of!(RegionCPowerG16S26A428, max_power_mw) == 0x88);
};
