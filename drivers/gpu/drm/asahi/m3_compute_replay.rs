// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Typed compute-builder replay offsets; see tools/m3-gpu/export-compute.py.
pub(crate) const RING: usize = 33;
pub(crate) const COUNTER: usize = 26;
pub(crate) const RESET: &[usize] = &[37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49];
