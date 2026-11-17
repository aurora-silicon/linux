// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! One-shot diagnostic buffer. No allocation, printk, waiting or overwrite.
//! Atomic words avoid unsafe shared payload access; ready publishes each slot.
use core::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};
use core::fmt::{self, Write};
use kernel::{bindings, module_param::KernelParam};

pub(crate) const WORDS: usize = 24;
const CAPACITY: usize = 16384;
struct Slot { ready: AtomicU32, words: [AtomicU64; WORDS] }
static SLOTS: [Slot; CAPACITY] = [const { Slot {
    ready: AtomicU32::new(0), words: [const { AtomicU64::new(0) }; WORDS],
} }; CAPACITY];
// 0 unarmed, 1 capturing, 2 permanently frozen until module unload.
static STATE: AtomicU32 = AtomicU32::new(0);
// Freeze and reservation share one atomic: stop cannot miss a late reservation.
const FROZEN: usize = 1usize << (usize::BITS - 1);
static NEXT: AtomicUsize = AtomicUsize::new(FROZEN);
static FROZEN_BOUND: AtomicUsize = AtomicUsize::new(0);
static CURSOR: AtomicUsize = AtomicUsize::new(0);
static DROPPED: AtomicU64 = AtomicU64::new(0);

pub(crate) fn enabled() -> bool { STATE.load(Ordering::Relaxed) == 1 }
pub(crate) fn now() -> u64 {
    <kernel::time::Monotonic as kernel::time::ClockSource>::ktime_get() as u64
}

/// Record kinds. 1/2: render admission/completion. 3/4: compute publication
/// return and exact retirement. 5: render publication sub-stages. Records
/// carry the architectural counter. Host and GPU rates must be converted
/// separately before testing whether their epochs align.
pub(crate) const KIND_RENDER_ADMISSION: u64 = 1;
pub(crate) const KIND_RENDER_COMPLETION: u64 = 2;
pub(crate) const KIND_COMPUTE_PUBLISH: u64 = 3;
pub(crate) const KIND_COMPUTE_RETIRE: u64 = 4;
pub(crate) const KIND_RENDER_PUBLISH_STAGES: u64 = 5;
/// Kind6: exact post-maintenance descriptor observations, only while armed.
/// Words1..7: pair slot, ordinal, TA/FRG descriptor VAs, physical TA/FRG kick
/// timestamps, packed TA/FRG QIDs. Words8..9: external pass envelope;10..13:
/// latest qualifying adjusted TA/FRG pair-B segments (24MHz).14: packed observed
/// retirement stamps;15: host counter;16: read-success;17: host frequency.
/// A zero/reversed pair is unavailable. Qualifying segments may be overwritten
/// on rearm; these are not aggregate execution or shader-only measurements.
pub(crate) const KIND_RENDER_DEVICE_SEGMENTS: u64 = 6;
/// Layout of the render completion record's words 10..19: TA start, TA end,
/// 3D start, 3D end (absolute GPU ticks), the architectural counter when the
/// fence is signalled (14), this version (15), then the service pass that
/// proved retirement on the same counter: last notification ingress on the
/// primary and secondary RTKit roles (16, 17), service-pass entry (18) and
/// the observation under the runtime mutex (19). Version 2 had words 10..14
/// only, with word 14 sampled at observation.
pub(crate) const RENDER_COMPLETION_LAYOUT: u64 = 3;

/// Host CNTPCT_EL0 ticks; `arch_frequency` gives their rate. Only read while armed.
/// Neo Linux exposes a 1 GHz physical timer while GPU command timestamps use
/// 24 MHz. Consumers must convert each clock separately before comparing them.
pub(crate) fn arch_counter() -> u64 {
    let counter: u64;
    // SAFETY: CNTPCT_EL0 is readable at EL1 on this platform (the driver's
    // QoS publication already reads it) and has no memory side effects.
    unsafe {
        core::arch::asm!("mrs {c}, CNTPCT_EL0", c = out(reg) counter,
            options(nomem, nostack, preserves_flags));
    }
    counter
}

pub(crate) fn arch_frequency() -> u64 {
    let frequency: u64;
    // SAFETY: CNTFRQ_EL0 is a read-only system register.
    unsafe {
        core::arch::asm!("mrs {f}, CNTFRQ_EL0", f = out(reg) frequency,
            options(nomem, nostack, preserves_flags));
    }
    frequency
}
pub(crate) fn record(words: [u64; WORDS]) {
    if !enabled() { return; }
    let next = NEXT.load(Ordering::Relaxed);
    if next & FROZEN != 0 { return; }
    if next >= CAPACITY { DROPPED.fetch_add(1, Ordering::Relaxed); return; }
    // A freeze racing this reservation either includes this ticket in its
    // finite drain bound, or this ticket observes FROZEN and writes nothing.
    let index = NEXT.fetch_add(1, Ordering::Relaxed);
    if index & FROZEN != 0 { return; }
    if index >= CAPACITY { DROPPED.fetch_add(1, Ordering::Relaxed); return; }
    let slot = &SLOTS[index];
    for (dst, src) in slot.words.iter().zip(words) { dst.store(src, Ordering::Relaxed); }
    slot.ready.store(1, Ordering::Release);
}

fn reserved() -> usize {
    if STATE.load(Ordering::Acquire) == 2 { FROZEN_BOUND.load(Ordering::Relaxed) }
    else { (NEXT.load(Ordering::Relaxed) & !FROZEN).min(CAPACITY) }
}

// Limit each sysfs response to 4095 bytes, below PAGE_SIZE even on 4K kernels.
struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}
unsafe extern "C" fn get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    // SAFETY: sysfs module parameter get supplies a PAGE_SIZE output buffer.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4095) };
    let mut out = Output { bytes, used: 0 };
    let mut cursor = CURSOR.load(Ordering::Relaxed);
    // The kernel parameter mutex serializes readers/setters, never writers.
    // Four records fit even when every u64 takes 20 decimal digits.
    for _ in 0..4 {
        if cursor >= reserved() { break; }
        let slot = &SLOTS[cursor];
        if slot.ready.load(Ordering::Acquire) == 0 { break; }
        let _ = write!(out, "record {}", cursor);
        for word in &slot.words { let _ = write!(out, " {}", word.load(Ordering::Relaxed)); }
        let _ = writeln!(out);
        cursor += 1;
    }
    CURSOR.store(cursor, Ordering::Relaxed);
    let reserved = reserved();
    let _ = writeln!(out, "status state={} cursor={} reserved={} dropped={} capacity={}",
        STATE.load(Ordering::Relaxed), cursor, reserved, DROPPED.load(Ordering::Relaxed), CAPACITY);
    out.used as core::ffi::c_int
}
unsafe extern "C" fn set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    // Bounded parser avoids strlen and rejects commands longer than 7 bytes.
    let mut command = [0u8; 8];
    let mut len = 0;
    while len < command.len() {
        // SAFETY: parameter core supplies a NUL-terminated value; never read
        // beyond the first NUL, and at most eight bytes are examined.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { break; }
        command[len] = byte;
        len += 1;
    }
    if len == command.len() { return -22; }
    let arg = &command[..len];
    match arg {
        b"1" | b"1\n" => {
            if crate::g18::params::g17p_completion_trace.value() != 0
                || crate::g18::params::g17p_trace_depth.value() != 0 { return -22; }
            if STATE.load(Ordering::Relaxed) != 0 { return -16; }
            NEXT.store(0, Ordering::Relaxed);
            STATE.store(1, Ordering::Relaxed);
        }
        b"0" | b"0\n" => {
            if STATE.load(Ordering::Relaxed) != 2 {
                FROZEN_BOUND.store((NEXT.fetch_or(FROZEN, Ordering::Relaxed) & !FROZEN).min(CAPACITY), Ordering::Relaxed);
                STATE.store(2, Ordering::Release);
            }
        }
        // A nonempty frozen capture proves that this module has published G17
        // work. Only the explicit root-only command reads fixed firmware DATA.
        b"clock" | b"clock\n" => {
            if STATE.load(Ordering::Acquire) != 2 || reserved() == 0 { return -22; }
            let before = arch_counter();
            let snapshot = crate::g18::compat::g17p::diagnostic_accounting_clocks();
            let after = arch_counter();
            kernel::pr_info!("G17P accounting clocks: host-before={} host-after={} host-hz={} fields(TA,FRG,CL,all,freq-word,offset,sign-word,abs-offset)={:x?}\n",
                before, after, arch_frequency(), snapshot);
            if let Err(error) = snapshot { return error.to_errno(); }
        }
        // Rewind readout only. Slots remain immutable, including while capturing.
        b"rewind" | b"rewind\n" => { CURSOR.store(0, Ordering::Relaxed); }
        _ => return -22,
    }
    0
}
static OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(set), get: Some(get), free: None,
};
#[link_section = "__param"]
#[used]
static PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_timing_buffer"
    } else { c"aurora.g17p_timing_buffer" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});

// One explicit post-window snapshot. No live ring reads occur in record().
// 0 unused, 1 requested, 2 copying, 3 complete, 4 failed; no reset until unload.
const RING_SLOTS: usize = 2048;
struct CompletionSlot { words: [AtomicU64; 8], changed: AtomicU32 }
static COMPLETION_SLOTS: [CompletionSlot; 2 * RING_SLOTS] = [const { CompletionSlot {
    words: [const { AtomicU64::new(0) }; 8], changed: AtomicU32::new(0),
} }; 2 * RING_SLOTS];
static COMPLETION_STATE: AtomicU32 = AtomicU32::new(0);
static COMPLETION_CURSOR: AtomicUsize = AtomicUsize::new(0);
static COMPLETION_COUNT: AtomicUsize = AtomicUsize::new(0);
static COMPLETION_ERROR: AtomicU32 = AtomicU32::new(0);
// request, copy-start/end CNTPCT, CNTFRQ, bias-before/after, ring0/2 DVA,
// accepted firmware recovery generation; then sixteen u32 table fields before
// and sixteen after the copy (eight per ordinal0/2 row). Non-atomic snapshots.
static COMPLETION_META: [AtomicU64; 49] = [const { AtomicU64::new(0) }; 49];

pub(crate) fn claim_completion_snapshot() -> bool {
    COMPLETION_STATE.compare_exchange(1, 2, Ordering::AcqRel, Ordering::Acquire).is_ok()
}

pub(crate) fn completion_snapshot_meta(index: usize, value: u64) {
    if let Some(word) = COMPLETION_META.get(index) { word.store(value, Ordering::Relaxed); }
}

pub(crate) fn store_completion_snapshot(index: usize, words: [u64; 8], changed: bool) {
    let Some(slot) = COMPLETION_SLOTS.get(index) else { return; };
    for (dst, value) in slot.words.iter().zip(words) { dst.store(value, Ordering::Relaxed); }
    slot.changed.store(u32::from(changed), Ordering::Relaxed);
    COMPLETION_COUNT.store(index + 1, Ordering::Relaxed);
}

pub(crate) fn finish_completion_snapshot(result: kernel::error::Result) {
    completion_snapshot_meta(2, arch_counter());
    let error = result.err().map_or(0, |error| error.to_errno());
    COMPLETION_ERROR.store(error as u32, Ordering::Relaxed);
    COMPLETION_STATE.store(if error == 0 { 3 } else { 4 }, Ordering::Release);
}

unsafe extern "C" fn completion_set(value: *const core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 8];
    let mut len = 0;
    while len < command.len() {
        // SAFETY: the parameter core supplies a NUL-terminated value.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { break; }
        command[len] = byte;
        len += 1;
    }
    if len == command.len() { return -22; }
    match &command[..len] {
        b"1" | b"1\n" => {
            if !matches!(STATE.load(Ordering::Acquire), 1 | 2) || reserved() == 0 { return -22; }
            // A claimed request is never overwritten. Reads remain unavailable
            // until COMPLETE/FAILED acquire, including concurrent sysfs reads.
            if COMPLETION_STATE.compare_exchange(0, 5, Ordering::AcqRel, Ordering::Acquire).is_err() {
                return -16;
            }
            completion_snapshot_meta(0, arch_counter());
            COMPLETION_STATE.store(1, Ordering::Release);
        }
        b"rewind" | b"rewind\n" => {
            if !matches!(COMPLETION_STATE.load(Ordering::Acquire), 3 | 4) { return -16; }
            COMPLETION_CURSOR.store(0, Ordering::Relaxed);
        }
        _ => return -22,
    }
    0
}

unsafe extern "C" fn completion_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4095) };
    let mut out = Output { bytes, used: 0 };
    let state = COMPLETION_STATE.load(Ordering::Acquire);
    let count = if state == 3 || state == 4 { COMPLETION_COUNT.load(Ordering::Relaxed) } else { 0 };
    let cursor = COMPLETION_CURSOR.load(Ordering::Relaxed).min(count);
    let end = (cursor + 12).min(count);
    for index in cursor..end {
        let slot = &COMPLETION_SLOTS[index];
        let ordinal = if index < RING_SLOTS { 0 } else { 2 };
        let _ = write!(out, "completion {} {} {}", ordinal, index % RING_SLOTS,
            slot.changed.load(Ordering::Relaxed));
        for word in &slot.words { let _ = write!(out, " {:016x}", word.load(Ordering::Relaxed)); }
        let _ = writeln!(out);
    }
    if end != cursor { COMPLETION_CURSOR.store(end, Ordering::Relaxed); }
    let _ = write!(out, "completion-status state={} cursor={} count={} error={} meta=",
        state, end, count, COMPLETION_ERROR.load(Ordering::Relaxed) as i32);
    for word in &COMPLETION_META { let _ = write!(out, " {}", word.load(Ordering::Relaxed)); }
    let _ = writeln!(out);
    out.used as core::ffi::c_int
}
static COMPLETION_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(completion_set), get: Some(completion_get), free: None,
};
#[link_section = "__param"]
#[used]
static COMPLETION_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_completion_buffer"
    } else { c"aurora.g17p_completion_buffer" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&COMPLETION_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});

/// Armed-only host observation gate; no additional device reads. Kind7 words:
/// slot, front ordinal (MAX if unavailable), reason (1borrowed,2restaged,3UMA,
/// 4stamp pending,5stamp ready,6notification mask pending), expected stamp,
/// observed TA/FRG stamps, host counter, host frequency, then four optional masks.
pub(crate) fn record_render_gate(slot: u8, ordinal: u64, reason: u64,
    expected: u32, stamps: [u32; 2], masks: [u64; 4]) {
    if !enabled() { return; }
    let mut words = [0; WORDS];
    words[0] = 7;
    words[1] = u64::from(slot);
    words[2] = ordinal;
    words[3] = reason;
    words[4] = u64::from(expected);
    words[5] = u64::from(stamps[0]);
    words[6] = u64::from(stamps[1]);
    words[7] = arch_counter();
    words[8] = arch_frequency();
    words[9..13].copy_from_slice(&masks);
    record(words);
}

static TSQ_GATE_SAMPLE: AtomicU32 = AtomicU32::new(0);
/// One in eight armed stamp-gate observations. Ordinary work never maps TSQ
/// diagnostic data. Kind8 is a non-atomic observation, never a completion gate.
pub(crate) fn sample_tsq_gate() -> bool {
    enabled() && TSQ_GATE_SAMPLE.fetch_add(1, Ordering::Relaxed) & 7 == 0
}

/// Root-only runtime diagnostic knobs, `g17p_diag_knobs` (u32 bitmask,
/// decimal or 0x hex). Read when each render descriptor/kick is built, so a
/// change applies to the next submission and permits same-boot A/B. Cached
/// family bits affect fresh resource owners only. bit0: FRG +0x222d = 0 (native light
/// captures); bit1: omit the per-pass KSM MCache aperture (native final
/// commands carry pointer/count zero).
/// v84 production defaults (native-matching, hardware-qualified since v64/v67
/// under 0x9000 in every throughput and stress run): bit12 native TA
/// force-drain value (+0x8c2 = 0) and bit15 aligned word record snapshots.
/// Bits27..30 cache native command-object families with reserved return storage
/// and final-stop/UAT lifetime checks. Writing the knob still permits A/B.
pub(crate) const DIAG_PRODUCTION_DEFAULT: u32 = (1 << 12) | (1 << 15) | (0xf << 27);
static DIAG_KNOBS: AtomicU32 = AtomicU32::new(DIAG_PRODUCTION_DEFAULT);
pub(crate) const DIAG_FRG_222D_CLEAR: u32 = 1 << 0;
pub(crate) const DIAG_NO_KSM_MCACHE: u32 = 1 << 1;
/// bit2: TA 0x0a5a1 PTC high word 0xfe (captured Metal value) instead of 0x60.
pub(crate) const DIAG_TA_PTC_NATIVE: u32 = 1 << 2;
/// bit3: FRG 0x0a5a9 PTC high word 0xd5 (captured Metal value) instead of 0x60.
pub(crate) const DIAG_FRG_PTC_NATIVE: u32 = 1 << 3;
/// bit4: hold firmware idle descent off (IdlePowerOff(0)) while set; the next
/// submission after clearing it restores the ordinary policy (arg 1).
pub(crate) const DIAG_IDLE_HOLD: u32 = 1 << 4;
/// bit5: copy the first 64 KiB of the host-owned primary state grid (Status-A
/// and Status-B) into `GRID` at the next submission, then clear this bit.
pub(crate) const DIAG_GRID_SNAPSHOT: u32 = 1 << 5;
/// bit6: native `updateSubmitInProgress` accounting. Maintain RegionC+0x7c
/// (submissions in progress) and +0x80 (monotonic submit count) around each
/// render/compute submission instead of leaving the static init values.
pub(crate) const DIAG_SUBMIT_ACCOUNTING: u32 = 1 << 6;
/// bit7: native kickFirmware note (EP 0x21 `0x0087_0000_0000_0010`) right after
/// the submit-in-progress bracket opens, as native `notifyFirmware(4, 1, 0)`.
pub(crate) const DIAG_SUBMIT_KICK: u32 = 1 << 7;
/// bit8: copy the live hardware-data block (shared cluster, 0x4000 bytes)
/// into `GRID` at the next submission, then clear this bit.
pub(crate) const DIAG_HWDATA_SNAPSHOT: u32 = 1 << 8;
/// bit9: CLPC deadline control effort. RegionC+0xe4 = 0x10000 (1.0 in 16.16)
/// with +0xe8 = 0 while set, as native CLPC writes during active streams;
/// clearing restores 0. GFX1 reads it live as a pstate floor (unit 9).
pub(crate) const DIAG_CLPC_EFFORT: u32 = 1 << 9;
pub(crate) fn clear_diag_knob(bit: u32) { DIAG_KNOBS.fetch_and(!bit, Ordering::Relaxed); }
pub(crate) const GRID_QWORDS: usize = 0x10000 / 8;
pub(crate) static GRID: [AtomicU64; GRID_QWORDS] = [const { AtomicU64::new(0) }; GRID_QWORDS];
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
/// Root-only firmware DATA peek, `g17p_fw_peek`: write a GFX LINK address in
/// the DATA segment (hex, 8-aligned); reading returns up to 64 qwords from it,
/// stopping at the page end. Bounded host reads of firmware RAM, no writes.
static PEEK_ADDR: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);
const PEEK_DATA_START: u64 = 0xffff_fc00_0004_c000;
const PEEK_DATA_END: u64 = 0xffff_fc00_0018_0000;
unsafe extern "C" fn peek_set(value: *const core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 24];
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
    let Ok(address) = u64::from_str_radix(text.strip_prefix("0x").unwrap_or(text), 16) else { return -22; };
    if address & 7 != 0 || !(PEEK_DATA_START..PEEK_DATA_END).contains(&address) { return -22; }
    PEEK_ADDR.store(address, Ordering::Relaxed);
    0
}
unsafe extern "C" fn peek_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let address = PEEK_ADDR.load(Ordering::Relaxed);
    if address < PEEK_DATA_START { return 0; }
    let page = crate::pgtable::UAT_PGSZ as u64;
    let physical = crate::g18::compat::g17p::g17p_gfx_link_to_physical(address);
    let room = ((page - (physical & (page - 1))) / 8).min(64) as usize;
    let room = room.min(((PEEK_DATA_END - address) / 8) as usize);
    let mut values = [0u64; 64];
    match crate::g18::compat::g17p::LiveFirmwareU64Probe::new(physical)
        .and_then(|probe| probe.read_run(&mut values[..room])) {
        Ok(()) => for (row, chunk) in values[..room].chunks(4).enumerate() {
            let _ = write!(out, "{:x}:", address + row as u64 * 32);
            for value in chunk { let _ = write!(out, " {:016x}", value); }
            let _ = writeln!(out);
        },
        Err(error) => { let _ = writeln!(out, "error {:?}", error); }
    }
    out.used as core::ffi::c_int
}
static PEEK_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(peek_set), get: Some(peek_get), free: None,
};
#[link_section = "__param"]
#[used]
static PEEK_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_fw_peek"
    } else { c"aurora.g17p_fw_peek" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&PEEK_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
/// Root-only `g17p_grid_peek`: write a byte offset (hex, 8-aligned) into the
/// last `DIAG_GRID_SNAPSHOT` copy; reading returns up to 64 qwords from it.
static GRID_OFFSET: AtomicUsize = AtomicUsize::new(0);
unsafe extern "C" fn grid_set(value: *const core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 24];
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
    let Ok(offset) = usize::from_str_radix(text.strip_prefix("0x").unwrap_or(text), 16) else { return -22; };
    if offset & 7 != 0 || offset >= GRID_QWORDS * 8 { return -22; }
    GRID_OFFSET.store(offset, Ordering::Relaxed);
    0
}
unsafe extern "C" fn grid_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let first = GRID_OFFSET.load(Ordering::Relaxed) / 8;
    let last = (first + 64).min(GRID_QWORDS);
    for row in (first..last).step_by(4) {
        let _ = write!(out, "{:x}:", row * 8);
        for slot in &GRID[row..(row + 4).min(last)] {
            let _ = write!(out, " {:016x}", slot.load(Ordering::Relaxed));
        }
        let _ = writeln!(out);
    }
    out.used as core::ffi::c_int
}
static GRID_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(grid_set), get: Some(grid_get), free: None,
};
#[link_section = "__param"]
#[used]
static GRID_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_grid_peek"
    } else { c"aurora.g17p_grid_peek" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&GRID_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
/// Root-only `g17p_gpc_sample`: writing N (1..=400000) samples the always-on
/// GPC performance state (sgx+0xe01000 & 0xf, J700 physical 0x4_80e0_1000)
/// N times, sleeping ~20 us between reads; reading prints the histogram of
/// states 0..15. Read-only MMIO of an always-on power register.
static GPC_HIST: [AtomicU64; 16] = [const { AtomicU64::new(0) }; 16];
static GPC_SAMPLED_NS: AtomicU64 = AtomicU64::new(0);
const GPC_STATE_PHYS: u64 = 0x4_80e0_1000;
unsafe extern "C" fn gpc_set(value: *const core::ffi::c_char,
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
    let Ok(samples) = text.parse::<u32>() else { return -22; };
    if samples == 0 || samples > 400_000 { return -22; }
    // SAFETY: maps one always-on 4-byte device register; unmapped below.
    let base = unsafe { bindings::ioremap(GPC_STATE_PHYS as bindings::phys_addr_t, 4) };
    if base.is_null() { return -12; }
    for slot in &GPC_HIST { slot.store(0, Ordering::Relaxed); }
    let start = now();
    for _ in 0..samples {
        // SAFETY: `base` is a live 4-byte MMIO mapping.
        let state = unsafe { bindings::readl(base) } & 0xf;
        GPC_HIST[state as usize].fetch_add(1, Ordering::Relaxed);
        kernel::time::delay::fsleep(kernel::time::Delta::from_micros(20));
    }
    GPC_SAMPLED_NS.store(now().wrapping_sub(start), Ordering::Relaxed);
    // SAFETY: `base` came from ioremap above and is not used afterwards.
    unsafe { bindings::iounmap(base) };
    0
}
unsafe extern "C" fn gpc_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 1000) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let _ = write!(out, "ns={}", GPC_SAMPLED_NS.load(Ordering::Relaxed));
    for (state, slot) in GPC_HIST.iter().enumerate() {
        let count = slot.load(Ordering::Relaxed);
        if count != 0 { let _ = write!(out, " s{}={}", state, count); }
    }
    let _ = writeln!(out);
    out.used as core::ffi::c_int
}
static GPC_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(gpc_set), get: Some(gpc_get), free: None,
};
#[link_section = "__param"]
#[used]
static GPC_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_gpc_sample"
    } else { c"aurora.g17p_gpc_sample" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&GPC_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
static DIAG_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(diag_set), get: Some(diag_get), free: None,
};
#[link_section = "__param"]
#[used]
static DIAG_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_diag_knobs"
    } else { c"aurora.g17p_diag_knobs" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&DIAG_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
/// Opt-in `g17p_diag_knobs` bit10: one-shot log of host<->firmware traffic.
/// Each entry is [counter | kind << 56, a, b, c]; the log stops at capacity
/// until `g17p_msglog` is written "reset". Logging only; changes no behaviour.
pub(crate) const DIAG_MSGLOG: u32 = 1 << 10;
/// a=endpoint, b=message, c=RTKit instance address (role tag).
pub(crate) const MSG_SEND: u64 = 1;
/// a=endpoint | processor << 8, b=message. Hard-IRQ callback.
pub(crate) const MSG_RECV_EARLY: u64 = 2;
/// a=endpoint | processor << 8, b=message. Workqueue callback.
pub(crate) const MSG_RECV: u64 = 3;
/// a=opcode, b=arg, c=producer before | consumer before << 32.
pub(crate) const MSG_DEVICE_CONTROL: u64 = 4;
/// a=role | consumer << 8 | producer << 32, b/c=record qwords 0/1.
pub(crate) const MSG_EVENT: u64 = 5;
/// a/b/c=record qwords 2/3/4 of the preceding MSG_EVENT.
pub(crate) const MSG_EVENT_TAIL: u64 = 6;
/// a=producer before, b=command VA, c=producer after (compute channel).
pub(crate) const MSG_CHANNEL: u64 = 7;
/// a=render slot | 1 << 32 for compute, b=phase (0 begin, 1 end).
pub(crate) const MSG_SUBMIT: u64 = 8;
/// a=role | consumer << 8 | producer << 32, b=consumed (stats ring).
pub(crate) const MSG_STATS: u64 = 9;
/// a/b/c=first three qwords of the statistics record at the consumer.
pub(crate) const MSG_STATS_RECORD: u64 = 10;
/// Opt-in bit11: consume both roles' statistics rings on every event-worker
/// pass, as native does on every interrupt. Diagnostic; default off.
pub(crate) const DIAG_STATS_DRAIN: u32 = 1 << 11;
/// Opt-in bit12: use the existing native-TA-drain probe for ordinary shared
/// TSQ render jobs. Keep the bootstrap and split-launch paths unchanged.
/// This permits same-boot A/B of combined TA/FRG visibility publication;
/// mandatory cache maintenance and both final-stamp retirement checks stay.
pub(crate) const DIAG_NATIVE_TA_DRAIN: u32 = 1 << 12;
/// Opt-in bit13: announce an eligible render submission before descriptor
/// preparation, to test overlap of native wake protocol with CPU preparation.
/// No clock, idle-policy, dependency or queue-capacity changes.
pub(crate) const DIAG_EARLY_RENDER_KICK: u32 = 1 << 13;
/// a=firmware timestamp (24 MHz), b=key | thread << 16 | secondary << 24
/// | flag << 32, c=arg0. Logged instead of printed while bit10 is set.
pub(crate) const MSG_KTRACE: u64 = 11;
/// a/b/c=args 1..3 of the preceding MSG_KTRACE.
pub(crate) const MSG_KTRACE_ARGS: u64 = 12;
/// v93: a=record qword 3 (+0x18; GPU pstate index in type-0x10 records),
/// b=role, c=consumer, following the MSG_STATS_RECORD of the same record.
pub(crate) const MSG_STATS_RECORD_Q3: u64 = 13;
const MSGLOG_ENTRIES: usize = 8192;
static MSGLOG: [[AtomicU64; 4]; MSGLOG_ENTRIES] =
    [const { [const { AtomicU64::new(0) }; 4] }; MSGLOG_ENTRIES];
static MSGLOG_NEXT: AtomicUsize = AtomicUsize::new(0);
static MSGLOG_READ: AtomicUsize = AtomicUsize::new(0);
pub(crate) fn msglog(kind: u64, a: u64, b: u64, c: u64) {
    if !diag_knob(DIAG_MSGLOG) { return; }
    let index = MSGLOG_NEXT.fetch_add(1, Ordering::Relaxed);
    let Some(slot) = MSGLOG.get(index) else { return; };
    slot[1].store(a, Ordering::Relaxed);
    slot[2].store(b, Ordering::Relaxed);
    slot[3].store(c, Ordering::Relaxed);
    slot[0].store((arch_counter() & 0x00ff_ffff_ffff_ffff) | (kind << 56), Ordering::Release);
}
/// Root-only `g17p_msglog`: write "reset" to clear, or a hex entry index to
/// choose where reading starts; reading prints up to 40 entries from there.
unsafe extern "C" fn msglog_set(value: *const core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 24];
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
    if text == "reset" {
        for slot in &MSGLOG { slot[0].store(0, Ordering::Relaxed); }
        MSGLOG_READ.store(0, Ordering::Relaxed);
        MSGLOG_NEXT.store(0, Ordering::Release);
        return 0;
    }
    let Ok(index) = usize::from_str_radix(text.strip_prefix("0x").unwrap_or(text), 16) else { return -22; };
    if index >= MSGLOG_ENTRIES { return -22; }
    MSGLOG_READ.store(index, Ordering::Relaxed);
    0
}
unsafe extern "C" fn msglog_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let next = MSGLOG_NEXT.load(Ordering::Acquire).min(MSGLOG_ENTRIES);
    let first = MSGLOG_READ.load(Ordering::Relaxed);
    let _ = writeln!(out, "n={:x}", next);
    for index in first..(first + 40).min(next) {
        let slot = &MSGLOG[index];
        let head = slot[0].load(Ordering::Acquire);
        let _ = writeln!(out, "{:x} {:x} {} {:x} {:x} {:x}", index,
            head & 0x00ff_ffff_ffff_ffff, head >> 56, slot[1].load(Ordering::Relaxed),
            slot[2].load(Ordering::Relaxed), slot[3].load(Ordering::Relaxed));
    }
    out.used as core::ffi::c_int
}
static MSGLOG_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(msglog_set), get: Some(msglog_get), free: None,
};
#[link_section = "__param"]
#[used]
static MSGLOG_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_msglog"
    } else { c"aurora.g17p_msglog" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&MSGLOG_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});

/// Opt-in bit14: request a work-ring scan before eligible render preparation.
pub(crate) const DIAG_EARLY_RENDER_DOORBELL: u32 = 1 << 14;

/// Retired bit15: published firmware records are always snapshotted with
/// aligned word reads (g17p_snapshot_fw_record); the bit is ignored.

/// Opt-in bit16: early work scan only when host tracks no submitted GPU work.
pub(crate) const DIAG_IDLE_RENDER_DOORBELL: u32 = 1 << 16;

/// v92c bits20..22: DISABLE switches for the default-on fault guards (all
/// clear = guarded; the lab's 0x9000/0x4009000 writes keep every guard on).
/// bit20: the 1 ms teardown poll no longer settles consumed FList releases.
pub(crate) const DIAG_TEARDOWN_SETTLE_OFF: u32 = 1 << 20;
/// bit21: a new owner context may take over a render pair whose FList still
/// carries an unobserved 0x2e release witness (the pre-v92c behaviour).
pub(crate) const DIAG_FLIST_TAKEOVER_GUARD_OFF: u32 = 1 << 21;
/// bit22: no device-control re-announcement while control records stay
/// unconsumed (teardown poll and takeover wait).
pub(crate) const DIAG_CONTROL_REANNOUNCE_OFF: u32 = 1 << 22;

/// v92d DIAGNOSTIC bit23 (default off): write MCache selector 1 (native's
/// captured value) into every fragment MCache aperture entry instead of the
/// render's own application-GART context ID. Native's value is its process's
/// GART context ID (AGXContextIDManager::alloc -> TA descriptor +0x1274 ->
/// halMCacheApertureSetup bits 41..46); on Linux context 1 is a different
/// address space, so this arm tests whether the aperture match matters at
/// all. It is not native parity. Each patched table is read back and must
/// differ from the default only in those six bits.
pub(crate) const DIAG_MCACHE_SELECTOR_NATIVE: u32 = 1 << 23;

/// v89 A/B-only bit26: suspend the default native both-role trace/statistics
/// ring consumers at runtime (the pre-v89 diagnostic-gated behaviour).
pub(crate) const DIAG_NATIVE_RING_DRAIN_OFF: u32 = 1 << 26;
/// bit25: A/B only. Publish the compute tag-16 EntrySignal on every repeat CL
/// kick, as before the native per-command policy. Default clear: only a kick
/// whose barriers name another hardware queue publishes one.
pub(crate) const DIAG_CL_ENTRY_SIGNAL_EVERY_KICK: u32 = 1 << 25;

/// v90 bits27..30: allocate render descriptors / TSQ node page / ChannelState
/// prefix / ConfigUpdate as pooled firmware-cached objects (native bit41).
/// Sampled for fresh allocations only; existing objects never change.
pub(crate) fn fw_cached_family_mask() -> u32 {
    (DIAG_KNOBS.load(Ordering::Relaxed) >> 27) & 0xf
}

// ---------------------------------------------------------------------------
/// v93 statistics pstate sampler: root-only `g17p_stats_pstate`, default 0.
///
/// Writing 1 makes the (default, native-parity) statistics-ring drain also
/// read four u32 words of every drained record -- +0x04/+0x08 (timestamp, as
/// one u64) and +0x18/+0x1c (qword 3; the GPU pstate index of type-0x10
/// records) -- before it publishes the consumer, and record them here. The
/// drain's legality check, consumer publication and order are unchanged; a
/// failed sample read is counted and skipped, never propagated. Written 0
/// (default) the drain pays one relaxed load per pass. "reset" clears.
///
/// Reading prints cumulative counters (difference two reads for a window;
/// the lab samples at 4 Hz), the latest type-0x10 sample, per-pstate record
/// counts and timestamp ticks (each type-0x10 delta is attributed to the
/// previous record's pstate, per role), then the newest ring entries.
/// Buckets are the low byte of qword 3, 16 meaning >= 16. The pstate
/// meaning of qword 3 is taken from native RE, not verified on Linux.
pub(crate) const STATS_PSTATE_TYPE: u32 = 0x10;
const PSTATE_BUCKETS: usize = 17;
const PSTATE_RING: usize = 64;
static PSTATE_ON: AtomicU32 = AtomicU32::new(0);
static PSTATE_RECORDS: AtomicU64 = AtomicU64::new(0);
static PSTATE_TYPE10: AtomicU64 = AtomicU64::new(0);
static PSTATE_READ_FAILS: AtomicU64 = AtomicU64::new(0);
/// Latest type-0x10 sample: timestamp, qword 3, role.
static PSTATE_LAST: [AtomicU64; 3] = [const { AtomicU64::new(0) }; 3];
static PSTATE_COUNT: [AtomicU64; PSTATE_BUCKETS] = [const { AtomicU64::new(0) }; PSTATE_BUCKETS];
static PSTATE_TICKS: [AtomicU64; PSTATE_BUCKETS] = [const { AtomicU64::new(0) }; PSTATE_BUCKETS];
/// Per role: previous type-0x10 timestamp and bucket + 1 (0 = none yet).
static PSTATE_PREV: [[AtomicU64; 2]; 2] = [const { [const { AtomicU64::new(0) }; 2] }; 2];
/// (timestamp, qword 3 with role in bit 63).
static PSTATE_SAMPLES: [[AtomicU64; 2]; PSTATE_RING] = [const { [const { AtomicU64::new(0) }; 2] }; PSTATE_RING];
static PSTATE_NEXT: AtomicUsize = AtomicUsize::new(0);

#[inline(always)]
pub(crate) fn stats_pstate_enabled() -> bool { PSTATE_ON.load(Ordering::Relaxed) != 0 }

pub(crate) fn stats_pstate_read_failed() { PSTATE_READ_FAILS.fetch_add(1, Ordering::Relaxed); }

/// Pure bucket choice (low byte of qword 3; 16 = out of range).
pub(crate) const fn stats_pstate_bucket(qword3: u64) -> usize {
    let index = (qword3 & 0xff) as usize;
    if index < PSTATE_BUCKETS - 1 { index } else { PSTATE_BUCKETS - 1 }
}

/// Record one drained statistics record (single writer: the drain runs
/// under the runtime mutex; readers only load).
pub(crate) fn stats_pstate_note(role: usize, entry_type: u32, timestamp: u64, qword3: u64) {
    PSTATE_RECORDS.fetch_add(1, Ordering::Relaxed);
    if entry_type != STATS_PSTATE_TYPE { return; }
    let role = role.min(1);
    PSTATE_TYPE10.fetch_add(1, Ordering::Relaxed);
    let bucket = stats_pstate_bucket(qword3);
    PSTATE_COUNT[bucket].fetch_add(1, Ordering::Relaxed);
    let previous = PSTATE_PREV[role][1].load(Ordering::Relaxed);
    let previous_ts = PSTATE_PREV[role][0].load(Ordering::Relaxed);
    if previous != 0 && timestamp > previous_ts {
        PSTATE_TICKS[(previous - 1) as usize % PSTATE_BUCKETS]
            .fetch_add(timestamp - previous_ts, Ordering::Relaxed);
    }
    PSTATE_PREV[role][0].store(timestamp, Ordering::Relaxed);
    PSTATE_PREV[role][1].store(bucket as u64 + 1, Ordering::Relaxed);
    PSTATE_LAST[0].store(timestamp, Ordering::Relaxed);
    PSTATE_LAST[1].store(qword3, Ordering::Relaxed);
    PSTATE_LAST[2].store(role as u64, Ordering::Relaxed);
    let slot = PSTATE_NEXT.fetch_add(1, Ordering::Relaxed) % PSTATE_RING;
    PSTATE_SAMPLES[slot][0].store(timestamp, Ordering::Relaxed);
    PSTATE_SAMPLES[slot][1].store((qword3 & !(1 << 63)) | (role as u64) << 63, Ordering::Relaxed);
}

unsafe extern "C" fn pstate_set(value: *const core::ffi::c_char,
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
    match text {
        "0" => PSTATE_ON.store(0, Ordering::Release),
        "1" => PSTATE_ON.store(1, Ordering::Release),
        "reset" => {
            for counter in [&PSTATE_RECORDS, &PSTATE_TYPE10, &PSTATE_READ_FAILS] {
                counter.store(0, Ordering::Relaxed);
            }
            for counter in PSTATE_LAST.iter().chain(PSTATE_COUNT.iter()).chain(PSTATE_TICKS.iter()) {
                counter.store(0, Ordering::Relaxed);
            }
            for role in &PSTATE_PREV { role[0].store(0, Ordering::Relaxed); role[1].store(0, Ordering::Relaxed); }
            for slot in &PSTATE_SAMPLES { slot[0].store(0, Ordering::Relaxed); slot[1].store(0, Ordering::Relaxed); }
            PSTATE_NEXT.store(0, Ordering::Release);
        }
        _ => return -22,
    }
    0
}
unsafe extern "C" fn pstate_get(buf: *mut core::ffi::c_char,
    _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4000) };
    let mut out = Output { bytes: &mut bytes[..], used: 0 };
    let _ = writeln!(out, "on={} records={} type10={} read_fails={} last_ts={:#x} last_q3={:#x} last_role={} now={:#x}",
        PSTATE_ON.load(Ordering::Relaxed), PSTATE_RECORDS.load(Ordering::Relaxed),
        PSTATE_TYPE10.load(Ordering::Relaxed), PSTATE_READ_FAILS.load(Ordering::Relaxed),
        PSTATE_LAST[0].load(Ordering::Relaxed), PSTATE_LAST[1].load(Ordering::Relaxed),
        PSTATE_LAST[2].load(Ordering::Relaxed), arch_counter());
    let _ = write!(out, "count");
    for counter in &PSTATE_COUNT { let _ = write!(out, " {}", counter.load(Ordering::Relaxed)); }
    let _ = write!(out, "\nticks");
    for counter in &PSTATE_TICKS { let _ = write!(out, " {}", counter.load(Ordering::Relaxed)); }
    let _ = writeln!(out);
    let next = PSTATE_NEXT.load(Ordering::Acquire);
    let first = next.saturating_sub(PSTATE_RING);
    for index in first..next {
        let slot = &PSTATE_SAMPLES[index % PSTATE_RING];
        let _ = writeln!(out, "{:x} {:x}", slot[0].load(Ordering::Relaxed), slot[1].load(Ordering::Relaxed));
    }
    out.used as core::ffi::c_int
}
static PSTATE_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(pstate_set), get: Some(pstate_get), free: None,
};
#[link_section = "__param"]
#[used]
static PSTATE_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_stats_pstate"
    } else { c"aurora.g17p_stats_pstate" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&PSTATE_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
