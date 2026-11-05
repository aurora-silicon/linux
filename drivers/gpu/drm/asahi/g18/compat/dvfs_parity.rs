// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Root-only runtime control `g17p_dvfs_parity` for the opt-in native
//! DVFS-input parity diagnostic. Default "0": no RegionC or message change.
//!
//! Write `MASK [HOLD_US [RAMP_US [CE_MAX]]]` (decimal or 0x hex):
//! bit0 work-tied CLPC deadline control effort (RegionC+0xe4),
//! bit1 native submit bracket (RegionC+0x7c/+0x80),
//! bit2 native-gated 0x87 note (requires bit1).
//! Reading prints the configuration, the live effort and event counters.
//! A change takes effect at the next submission or completion service.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use kernel::{bindings, module_param::KernelParam};

use crate::g18::compat::g17p::{self as policy, Config};

static MASK: AtomicU32 = AtomicU32::new(policy::DEFAULT_CONFIG.mask);
static HOLD_NS: AtomicU64 = AtomicU64::new(policy::DEFAULT_CONFIG.hold_ns);
static RAMP_NS: AtomicU64 = AtomicU64::new(policy::DEFAULT_CONFIG.ramp_ns);
static CE_MAX: AtomicU32 = AtomicU32::new(policy::DEFAULT_CONFIG.ce_max);

/// Counters for hardware verification. Never reset except by module load.
pub(crate) static CE_PUBLISHES: AtomicU64 = AtomicU64::new(0);
pub(crate) static CE_CLEARS: AtomicU64 = AtomicU64::new(0);
pub(crate) static CE_LIVE: AtomicU32 = AtomicU32::new(0);
pub(crate) static CE_LAST_SET_NS: AtomicU64 = AtomicU64::new(0);
pub(crate) static CE_LAST_CLEAR_NS: AtomicU64 = AtomicU64::new(0);
pub(crate) static RECHECKS: AtomicU64 = AtomicU64::new(0);
pub(crate) static BRACKET_OPENS: AtomicU64 = AtomicU64::new(0);
pub(crate) static KICKS_SENT: AtomicU64 = AtomicU64::new(0);
pub(crate) static KICKS_GATED: AtomicU64 = AtomicU64::new(0);
pub(crate) static STATS_66C_LAST: AtomicU32 = AtomicU32::new(0);
pub(crate) static STATS_66C_NONZERO: AtomicU64 = AtomicU64::new(0);
pub(crate) static PUBLISH_ERRORS: AtomicU64 = AtomicU64::new(0);

pub(crate) fn config() -> Config {
    Config {
        mask: MASK.load(Ordering::Relaxed),
        hold_ns: HOLD_NS.load(Ordering::Relaxed),
        ramp_ns: RAMP_NS.load(Ordering::Relaxed),
        ce_max: CE_MAX.load(Ordering::Relaxed),
    }
}

pub(crate) fn enabled(bit: u32) -> bool {
    MASK.load(Ordering::Relaxed) & bit != 0
}

struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len();
        Ok(())
    }
}

unsafe extern "C" fn parity_set(value: *const core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 64];
    let mut len = 0;
    while len < command.len() {
        // SAFETY: the parameter core supplies a NUL-terminated value.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { break; }
        command[len] = byte;
        len += 1;
    }
    if len == command.len() { return -22; }
    let Ok(text) = core::str::from_utf8(&command[..len]) else { return -22; };
    let Some(parsed) = policy::parse(text.trim()) else { return -22; };
    // Order: parameters before the mask, so an enabling write never runs
    // with stale timing values.
    HOLD_NS.store(parsed.hold_ns, Ordering::Relaxed);
    RAMP_NS.store(parsed.ramp_ns, Ordering::Relaxed);
    CE_MAX.store(parsed.ce_max, Ordering::Relaxed);
    MASK.store(parsed.mask, Ordering::Release);
    kernel::pr_info!(
        "G17P dvfs parity mask={:#x} hold_us={} ramp_us={} ce_max={:#x}\n",
        parsed.mask, parsed.hold_ns / 1000, parsed.ramp_ns / 1000, parsed.ce_max
    );
    0
}

unsafe extern "C" fn parity_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1024) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let cfg = config();
    let _ = writeln!(out, "mask={:#x} hold_us={} ramp_us={} ce_max={:#x}",
        cfg.mask, cfg.hold_ns / 1000, cfg.ramp_ns / 1000, cfg.ce_max);
    let _ = writeln!(out, "ce_live={:#x} ce_publishes={} ce_clears={} rechecks={} publish_errors={}",
        CE_LIVE.load(Ordering::Relaxed), CE_PUBLISHES.load(Ordering::Relaxed),
        CE_CLEARS.load(Ordering::Relaxed), RECHECKS.load(Ordering::Relaxed),
        PUBLISH_ERRORS.load(Ordering::Relaxed));
    let _ = writeln!(out, "ce_last_set_ns={} ce_last_clear_ns={} now_ns={}",
        CE_LAST_SET_NS.load(Ordering::Relaxed), CE_LAST_CLEAR_NS.load(Ordering::Relaxed),
        crate::g18::compat::timing_buffer::now());
    let _ = writeln!(out, "bracket_opens={} kicks_sent={} kicks_gated={} stats_66c_last={:#x} stats_66c_nonzero={}",
        BRACKET_OPENS.load(Ordering::Relaxed), KICKS_SENT.load(Ordering::Relaxed),
        KICKS_GATED.load(Ordering::Relaxed), STATS_66C_LAST.load(Ordering::Relaxed),
        STATS_66C_NONZERO.load(Ordering::Relaxed));
    out.used as core::ffi::c_int
}

static PARITY_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(parity_set), get: Some(parity_get), free: None,
};
#[link_section = "__param"]
#[used]
static PARITY_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_dvfs_parity"
    } else { c"aurora.g17p_dvfs_parity" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&PARITY_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
