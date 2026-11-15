// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Native G18 / 26A428 compute channel allocation geometry.
//!
//! AGXCLChannel::init 0x8be3934..0x8be3970 computes the extents;
//! 0x8be399c / 0x8be39ac form the allocation size, and
//! 0x8be3ad0..0x8be3ae8 bind its four pointers. These working buffers
//! are distinct from process UAT page tables and scheduler records.

#![allow(dead_code)]

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18ComputeChannelLayout {
    pub(crate) clusters: u32,
    pub(crate) width: u32,
    pub(crate) operand_bytes: u64,
    pub(crate) control_bytes: u64,
    pub(crate) tail_bytes: u64,
    pub(crate) allocation_bytes: u64,
}

#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub(crate) struct G18ComputeChannelPointers {
    /// Native channel +0x178, +0x180, in that order.
    pub(crate) operands: [u64; 2],
    /// Native channel +0x188 and +0x190.
    pub(crate) control: u64,
    pub(crate) tail: u64,
}

impl G18ComputeChannelLayout {
    pub(crate) fn new(clusters: u32, width: u32) -> Option<Self> {
        // The register encoder admits the same bounded topology. Arithmetic
        // cannot wrap within these bounds; native's u32 operations agree.
        if clusters == 0 || clusters > 16 || width == 0 || width > 16 {
            return None;
        }
        let operand_bytes =
            (u64::from(width + 10 * clusters - 10) * 0x1800 + u64::from(clusters) * 64 + 0x3ff)
                & !0x3ff;
        let control_bytes = u64::from(clusters) * 0x800;
        let tail_bytes = 0x400;
        let allocation_bytes = (2 * operand_bytes + control_bytes) | tail_bytes;
        Some(Self {
            clusters,
            width,
            operand_bytes,
            control_bytes,
            tail_bytes,
            allocation_bytes,
        })
    }

    /// The native register fields consume lower GPU VAs. A firmware-high
    /// address must not be silently truncated into a different lower address.
    pub(crate) fn bind(&self, base: u64) -> Option<G18ComputeChannelPointers> {
        let end = base.checked_add(self.allocation_bytes)?;
        if base == 0 || base & 0x3ff != 0 || end > 1u64 << 42 {
            return None;
        }
        Some(G18ComputeChannelPointers {
            operands: [base + self.operand_bytes, base],
            control: base + 2 * self.operand_bytes,
            tail: base + 2 * self.operand_bytes + self.control_bytes,
        })
    }
}
