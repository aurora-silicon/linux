// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Root-only runtime diagnostic knobs, `g17p_diag_knobs`.
use core::sync::atomic::{AtomicU32, Ordering};
use core::fmt::{self, Write};
use kernel::bindings;

// Limit each sysfs response to 4095 bytes, below PAGE_SIZE even on 4K kernels.
struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

/// Root-only runtime diagnostic knobs, `g17p_diag_knobs` (u32 bitmask,
/// decimal or 0x hex). Read when each render descriptor is built, so a
/// change applies to the next submission and permits same-boot A/B.
/// bit0: FRG +0x222d = 0 (native light captures); see
/// `g18::fw::render_params`. That is the only bit this driver consumes. The
/// module-1134 production default (bit12 native TA force-drain, bit15 aligned
/// word record snapshots, bits27..30 cached command-object families) is kept
/// unchanged so the parameter reads back the same value; those bits gate
/// module-1134 paths that were never wired into this driver.
const DIAG_PRODUCTION_DEFAULT: u32 = (1 << 12) | (1 << 15) | (0xf << 27);
static DIAG_KNOBS: AtomicU32 = AtomicU32::new(DIAG_PRODUCTION_DEFAULT);
pub(crate) const DIAG_FRG_222D_CLEAR: u32 = 1 << 0;
pub(crate) fn diag_knob(bit: u32) -> bool { DIAG_KNOBS.load(Ordering::Relaxed) & bit != 0 }
unsafe extern "C" fn diag_set(value: *const core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 16];
    let mut len = 0;
    while len < command.len() {
        // SAFETY: the parameter core supplies a NUL-terminated value.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { break; }
        command[len] = byte;
        len += 1;
    }
    if len == command.len() { return -22; }
    let text = match core::str::from_utf8(&command[..len]) { Ok(text) => text.trim(), Err(_) => return -22 };
    let parsed = match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16),
        None => text.parse::<u32>(),
    };
    let Ok(value) = parsed else { return -22; };
    DIAG_KNOBS.store(value, Ordering::Relaxed);
    kernel::pr_info!("G17P diagnostic knobs = {:#x}\n", value);
    0
}
unsafe extern "C" fn diag_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 512) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let _ = writeln!(out, "{:#x}", DIAG_KNOBS.load(Ordering::Relaxed));
    // Firmware DATA power bookkeeping (RAM, not MMIO; bounded single loads):
    // 124ba0 power-up count, 1771f8 power state (2 on, 4 stage-1 idle),
    // 177838 idle-off policy (RegionC+0x30 copy), 128a68 keep-on bits.
    for (name, link) in [("powerups", 0xffff_fc00_0012_4ba0u64), ("state", 0xffff_fc00_0017_71f8),
        ("idleoff", 0xffff_fc00_0017_7838), ("keepon", 0xffff_fc00_0012_8a68)] {
        let value = crate::g18::compat::g17p::LiveFirmwareU64Probe::new(
            crate::g18::compat::g17p::g17p_gfx_link_to_physical(link))
            .and_then(|probe| probe.observe_mask(u64::MAX, 1))
            .map(|sample| sample.first);
        let _ = writeln!(out, "{} {:x?}", name, value);
    }
    out.used as core::ffi::c_int
}
static DIAG_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(diag_set), get: Some(diag_get), free: None,
};
crate::g18::params::g18_kernel_param!(
    DIAG_PARAM,
    "g17p_diag_knobs",
    ops: core::ptr::from_ref(&DIAG_OPS),
    perm: 0o600,
    arg: core::ptr::null_mut(),
);
