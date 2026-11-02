// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M5 (26A428) firmware layouts, from dj's linux-aurora t6050-gpu (`g17/fw/m5`), the
//! encoders this driver uses: the bootstrap graph, the compute queue, pool and
//! descriptor encodings of the arithmetic qualification (`g17/m5_test.rs`), the shared pool
//! planning, and the render commands, register programs and target slices.

pub(crate) mod budget;
pub(crate) mod compute;
pub(crate) mod demand;
pub(crate) mod initdata;
pub(crate) mod pool;
pub(crate) mod power;
pub(crate) mod queue;
pub(crate) mod render;
pub(crate) mod render_assembly;
pub(crate) mod render_registers;
pub(crate) mod render_tail;
pub(crate) mod target;

/// Statistics record tags the M5 firmware sends: the common ones and tag 32, beyond the legacy
/// 32-bit mask.
pub(crate) const STATISTICS_TYPES: u64 = 0x1_5fff_0000;

/// Stores `writes` into a zeroed register array at GPU address `address`. M5's size word
/// is the entry count alone, with its upper half clear; the register storage and the self
/// pointer use the shared layout.
pub(crate) fn set_register_array(
    array: &mut super::kick::RegisterArray,
    address: u64,
    writes: &[super::kick::RegisterWrite],
) -> kernel::prelude::Result {
    array.set(address, writes)?;
    array.size = writes.len() as u32;
    Ok(())
}
