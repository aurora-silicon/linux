// SPDX-License-Identifier: GPL-2.0-only
use crate::agx_render::BuildError;

#[derive(Clone, Copy, Default)]
pub(crate) struct Program { pub address: u64, pub resources: u64 }
#[derive(Clone, Copy, Default)]
pub(crate) struct DepthStencil { pub base: u64, pub stride: u64 }

/// Encode an already-mapped client VA in the tiler address window.
pub(crate) fn compact(address: u64) -> Result<u64, BuildError> {
    if !(0x10_0000_0000..0x90_0000_0000).contains(&address) {
        return Err(BuildError::Address);
    }
    Ok(address - 0x10_0000_0000)
}
