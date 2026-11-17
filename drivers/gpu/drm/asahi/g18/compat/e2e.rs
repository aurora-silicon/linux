// SPDX-License-Identifier: GPL-2.0-only OR MIT
//! Opt-in end-to-end timeline ring (v81 diagnostic, default OFF).
//!
//! `g17p_e2e` (root-only sysfs parameter) arms a preallocated ring of 64-byte
//! records. While disarmed, every hook is a single relaxed atomic load and a
//! branch; no production state, ordering, locking or firmware access changes.
//! While armed, `record` reserves one slot with an atomic increment and does
//! eight relaxed stores (the first word last, with Release). No allocation,
//! printk, sleeping lock or wait is ever taken from a hook.
//!
//! Record layout (u64 words):
//!   w0: CNTPCT_EL0 at the record point (host 1 GHz on T8140); written last.
//!   w1: kind | cpu << 8 | generation << 16 | aux << 32
//!   w2: identity (submission id, slot, ...; kind specific)
//!   w3..w7: payload a..e (kind specific; see the KIND_* constants).
//!
//! Commands written to `g17p_e2e`:
//!   "arm <mask>"  start a new generation from slot 0 (mask bit0 base events,
//!                 bit1 mailbox send/IRQ events, bit2 read-only firmware power
//!                 word + GPC pstate samples, bit3 device segment reads,
//!                 bit4 v91 fine submit-path laps, kinds 27..31)
//!   "stop"        stop recording (slots stay readable)
//!   "mark <hex>"  insert a marker record (only while armed)
//!   "seek <hex>"  set the read cursor
//! Reading returns up to 24 records from the cursor and advances it.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use kernel::{bindings, module_param::KernelParam};

pub(crate) const OPT_BASE: u32 = 1 << 0;
pub(crate) const OPT_MAILBOX: u32 = 1 << 1;
pub(crate) const OPT_POWER: u32 = 1 << 2;
pub(crate) const OPT_SEGMENTS: u32 = 1 << 3;
/// v91: fine submit-path laps (K_FINE_*). Default off; with the ring armed
/// without this bit, each lap site is one relaxed load and a branch.
pub(crate) const OPT_FINE: u32 = 1 << 4;

// Kernel event kinds.
/// submit ioctl entry (head = file.rs submit entry). id=submission, aux=queue id,
/// a=in_syncs | out_syncs << 32, b=cmdbuf bytes, c=tgid, d=cmdbuf copied,
/// e=syncs parsed (both CNTPCT).
pub(crate) const K_IOCTL_ENTER: u8 = 1;
/// render command parsed. id=submission, a=command count,
/// b=render_barrier | compute_barrier << 32, c=render wait_through (MAX none),
/// d=compute wait_through (MAX none), e=render sequence.
pub(crate) const K_IOCTL_PARSED: u8 = 2;
/// render job pushed to drm_sched. id=submission, a=dependencies,
/// b=checked (input) dependencies, c=firmware fences, d=render sequence.
pub(crate) const K_SCHED_PUSHED: u8 = 3;
/// submit ioctl exit. id=submission, a=errno (0 ok).
pub(crate) const K_IOCTL_EXIT: u8 = 4;
/// run_job (publish) entry. id=submission, a=input fences, b=firmware fences,
/// c=input fences already signalled at entry.
pub(crate) const K_RUN_JOB: u8 = 5;
/// input fences waited/checked. id=submission.
pub(crate) const K_DEPS_DONE: u8 = 6;
/// physical lease attached. id=submission, a=render slot.
pub(crate) const K_LEASE: u8 = 7;
/// publication lock/prepare times (CNTPCT). id=submission, a=work state
/// acquired, b=first Runtime lock requested, c=first Runtime lock acquired,
/// d=prepare begin (Runtime dropped), e=prepare end. head=Runtime reacquired.
pub(crate) const K_PUB_LOCKS: u8 = 8;
/// publication stage summary. id=submission, aux=slot|retained<<8|hit<<9|ok<<10,
/// a=runtime_lock_ns | state_polls << 32, b=handoff_ns | prepare_ns << 32,
/// c=restage_ns | reachability_ns << 32, d=flist_request_ns | tvb_blocks << 32,
/// e=doorbell CNTPCT. head=submit_translated_render return (Runtime held).
pub(crate) const K_PUB_STAGES: u8 = 9;
/// publication sub-stage nanoseconds sub_ns[0..10] packed pairwise into a..e,
/// sub_ns[10] in aux. id=submission.
pub(crate) const K_PUB_SUB: u8 = 10;
/// publish() return after Runtime release. id=submission, a=ok.
pub(crate) const K_PUB_EXIT: u8 = 11;
/// mailbox send. a=endpoint, b=message, c=RTKit instance.
pub(crate) const K_SEND: u8 = 13;
/// hard-IRQ mailbox receive. a=endpoint | processor << 8, b=message.
pub(crate) const K_IRQ: u8 = 14;
/// event-worker firmware-event service wrapper. a=entry, b=Runtime acquired,
/// c=Runtime released, d=deferred completions signalled. head=after signals.
pub(crate) const K_WORKER: u8 = 15;
/// one render observation iteration. id=slot, a=Runtime acquired,
/// b=observation taken (Runtime released), c=observe() done,
/// d=Runtime reacquired, e=commit done (before release), aux=signalled|ok<<16.
pub(crate) const K_OBS_LOOP: u8 = 16;
/// completion proven under Runtime. id=submission, aux=slot, a=primary
/// notification ingress, b=secondary ingress, c=service-pass entry,
/// d=pass start (GPU 24 MHz), e=pass end (GPU 24 MHz).
pub(crate) const K_OBSERVED: u8 = 17;
/// G17PRenderCompletion::complete. id=submission, a=entry, b=UAPI timestamps
/// written, c=lease dropped, d=VM job dropped, e=hardware fence signalled.
/// head=aggregate member completed.
pub(crate) const K_COMPLETE: u8 = 18;
/// aggregate (output syncobj) fence signalled. id=submission, a=before signal,
/// aux=errno. head=after dma_fence_signal returned.
pub(crate) const K_AGG_SIGNAL: u8 = 19;
/// marker. id=user value.
pub(crate) const K_MARK: u8 = 20;
/// device identity at retirement. id=pair slot << 32 | ordinal, a=TA QID |
/// FRG QID << 32, b=observed TA stamp | FRG stamp << 32, c=expected stamp,
/// d=pass start, e=pass end.
pub(crate) const K_OBS_DEVICE: u8 = 21;
/// descriptor segments (opt bit3). id=pair slot << 32 | ordinal, a..d=TA
/// start/end, FRG start/end (24 MHz), e=read ok.
pub(crate) const K_SEGMENTS: u8 = 22;
/// read-only power samples (opt bit2). id=site (1 doorbell, 2 observed),
/// aux=slot, a=firmware power-up count, b=firmware power state word,
/// c=GPC performance state (MMIO, low 4 bits), d=read error mask.
pub(crate) const K_POWER: u8 = 23;
/// v84 feed-perf actuator change. aux=new pct (0 = withdrawn), id=score,
/// a=previous pct, b=starved passes, c=fed passes (cumulative).
pub(crate) const K_FEED_PERF: u8 = 26;
/// v91 ioctl laps (opt bit4), recorded after the scheduler push. id=submission,
/// aux=validation requests | proof hit << 8 | batch walks << 9 | pending cap
/// << 16. a=queue submit entry, b=output fences split, c=VM guard + parser +
/// pending vector, d=translate begin, e=translate end.
pub(crate) const K_FINE_IOCTL: u8 = 27;
/// v91 ioctl laps, continued. id=submission. a=parse finish + VM job guards,
/// b=aggregate fence + output syncobjs installed, c=timestamp destinations,
/// d=hardware fence + completion + dependency vectors, e=packet built.
pub(crate) const K_FINE_QUEUE: u8 = 28;
/// v91 locked reservation laps inside take_render_restage (Runtime held).
/// id=pair slot, a=entry, b=recovery gate done, c=dependency snapshot done,
/// d=VM handoff done, e=manager reservation done (graph taken).
pub(crate) const K_FINE_RESERVE: u8 = 29;
/// v91 restage_command laps (unlocked). id=pair slot << 32 | ordinal.
/// a=entry, b=render-state/AXFB/status/timestamp encode done, c=PM scene +
/// objects selected, d=TA descriptor + retained fields done, e=3D descriptor,
/// tail and MCache done. head=support record + fence done.
pub(crate) const K_FINE_RESTAGE: u8 = 30;
/// v91 SUB_CONFIG laps (Runtime held). id=pair slot << 32 | ordinal.
/// a=entry, b=3D QoS published, c=3D consistency validated, d=TA QoS
/// published, e=TA consistency validated. head=pair configure done.
pub(crate) const K_FINE_CONFIG: u8 = 31;
/// v94 retained-graph reachability revalidation (opt bit4), one per render
/// publication. id=render descriptor storage IOVA (per pair graph),
/// aux=path | reason << 4 | generation << 8 (path 0 exact, 1 lock-free walk,
/// 2 locked walk, 3 full walk; reason for a full walk 1 no proof, 2 no key,
/// 3 refused, 4 touched, 5 key moved), a=entry, b=app changes walked | kernel
/// changes walked << 32, c=app exec lock held (0 unlocked), d=kernel exec
/// lock held (0 unlocked), e=walks done (all CNTPCT). head=exit.
pub(crate) const K_FINE_REACH: u8 = 32;

const CAPACITY: usize = 1 << 17;
#[repr(C, align(64))]
struct Slot { w: [AtomicU64; 8] }
static SLOTS: [Slot; CAPACITY] = [const { Slot { w: [const { AtomicU64::new(0) }; 8] } }; CAPACITY];
static ARMED: AtomicU32 = AtomicU32::new(0);
static GENERATION: AtomicU32 = AtomicU32::new(0);
static NEXT: AtomicUsize = AtomicUsize::new(0);
static BOUND: AtomicUsize = AtomicUsize::new(0);
static CURSOR: AtomicUsize = AtomicUsize::new(0);
static DROPPED: AtomicU64 = AtomicU64::new(0);

#[inline(always)]
pub(crate) fn armed() -> bool { ARMED.load(Ordering::Relaxed) != 0 }
#[inline(always)]
pub(crate) fn option(bit: u32) -> bool { ARMED.load(Ordering::Relaxed) & bit != 0 }

/// Host CNTPCT_EL0 (same register the existing timing buffer uses).
#[inline(always)]
pub(crate) fn now() -> u64 { crate::g18::compat::timing_buffer::arch_counter() }

/// Only reads the counter while armed; otherwise returns 0.
#[inline(always)]
pub(crate) fn stamp() -> u64 { if armed() { now() } else { 0 } }

/// v91: counter for a fine lap (option bit4), otherwise 0.
#[inline(always)]
pub(crate) fn fine() -> u64 { if option(OPT_FINE) { now() } else { 0 } }

#[inline(always)]
fn cpu() -> u64 {
    // SAFETY: raw_smp_processor_id has no preconditions; a stale value after
    // migration is acceptable for a diagnostic label.
    u64::from(unsafe { bindings::raw_smp_processor_id() }) & 0xff
}

pub(crate) fn record_at(head: u64, kind: u8, aux: u32, id: u64, payload: [u64; 5]) {
    if !armed() { return; }
    let generation = GENERATION.load(Ordering::Relaxed);
    let index = NEXT.fetch_add(1, Ordering::Relaxed);
    if index >= CAPACITY { DROPPED.fetch_add(1, Ordering::Relaxed); return; }
    let slot = &SLOTS[index];
    slot.w[1].store(u64::from(kind) | cpu() << 8 | (u64::from(generation) & 0xffff) << 16
        | u64::from(aux) << 32, Ordering::Relaxed);
    slot.w[2].store(id, Ordering::Relaxed);
    for (word, value) in slot.w[3..].iter().zip(payload) { word.store(value, Ordering::Relaxed); }
    slot.w[0].store(head.max(1), Ordering::Release);
}

#[inline(always)]
pub(crate) fn record(kind: u8, aux: u32, id: u64, payload: [u64; 5]) {
    if !armed() { return; }
    record_at(now(), kind, aux, id, payload);
}

// Read-only power samples (option bit2). Mappings are created on the first
// arm with the bit set and kept until module unload (never torn down while a
// hook could still use them). Same physical words and access method as the
// existing `g17p_diag_knobs` read (memremap WB + dc ivac + load), and the
// same always-on GPC register as `g17p_gpc_sample`.
static POWERUPS_PAGE: AtomicPtr<u8> = AtomicPtr::new(core::ptr::null_mut());
static STATE_PAGE: AtomicPtr<u8> = AtomicPtr::new(core::ptr::null_mut());
static GPC_REG: AtomicPtr<u8> = AtomicPtr::new(core::ptr::null_mut());
const POWERUPS_LINK: u64 = 0xffff_fc00_0012_4ba0;
const STATE_LINK: u64 = 0xffff_fc00_0017_71f8;
const GPC_STATE_PHYS: u64 = 0x4_80e0_1000;
const PAGE: u64 = crate::pgtable::UAT_PGSZ as u64;

fn map_power_sources() -> bool {
    let map_page = |slot: &AtomicPtr<u8>, link: u64| -> bool {
        if !slot.load(Ordering::Acquire).is_null() { return true; }
        let physical = crate::g18::compat::g17p::g17p_gfx_link_to_physical(link);
        // SAFETY: same page as the existing bounded diag_knobs probe; WB
        // read-only use; retained until module unload.
        let ptr = unsafe { bindings::memremap(physical & !(PAGE - 1), PAGE as usize,
            bindings::MEMREMAP_WB as _) };
        if ptr.is_null() { return false; }
        slot.store(ptr.cast::<u8>(), Ordering::Release);
        true
    };
    let ok = map_page(&POWERUPS_PAGE, POWERUPS_LINK) && map_page(&STATE_PAGE, STATE_LINK);
    if GPC_REG.load(Ordering::Acquire).is_null() {
        // SAFETY: the always-on 4-byte register already sampled by
        // `g17p_gpc_sample`; retained until module unload.
        let base = unsafe { bindings::ioremap(GPC_STATE_PHYS as bindings::phys_addr_t, 4) };
        if base.is_null() { return false; }
        GPC_REG.store(base.cast::<u8>(), Ordering::Release);
    }
    ok
}

fn read_firmware_word(page: &AtomicPtr<u8>, link: u64) -> Option<u64> {
    let base = page.load(Ordering::Acquire);
    if base.is_null() { return None; }
    let physical = crate::g18::compat::g17p::g17p_gfx_link_to_physical(link);
    let offset = (physical & (PAGE - 1)) as usize;
    // SAFETY: `base` maps the whole containing page for the module lifetime;
    // `offset` is 8-aligned (both links are) and below the page size. The
    // invalidate touches only AP cachelines of this read-only mapping,
    // exactly like LiveFirmwareU64Probe::observe_mask.
    unsafe {
        let address = base.add(offset);
        core::arch::asm!("dsb osh", "dc ivac, {a}", "dsb osh", a = in(reg) address,
            options(nostack, preserves_flags));
        Some(core::ptr::read_volatile(address.cast::<u64>()))
    }
}

/// Opt-in bit2 sample at a named site. Read-only; never called otherwise.
pub(crate) fn power_sample(site: u64, slot: u8) {
    if !option(OPT_POWER) { return; }
    let powerups = read_firmware_word(&POWERUPS_PAGE, POWERUPS_LINK);
    let state = read_firmware_word(&STATE_PAGE, STATE_LINK);
    let gpc = {
        let base = GPC_REG.load(Ordering::Acquire);
        // SAFETY: live 4-byte MMIO mapping retained until module unload.
        if base.is_null() { None } else { Some(u64::from(unsafe { bindings::readl(base.cast()) })) }
    };
    let errors = u64::from(powerups.is_none()) | u64::from(state.is_none()) << 1
        | u64::from(gpc.is_none()) << 2;
    record(K_POWER, u32::from(slot), site,
        [powerups.unwrap_or(0), state.unwrap_or(0), gpc.unwrap_or(0) & 0xf, errors, 0]);
}

struct Output<'a> { bytes: &'a mut [u8], used: usize }
impl Write for Output<'_> {
    fn write_str(&mut self, value: &str) -> fmt::Result {
        if value.len() > self.bytes.len() - self.used { return Err(fmt::Error); }
        self.bytes[self.used..self.used + value.len()].copy_from_slice(value.as_bytes());
        self.used += value.len(); Ok(())
    }
}

fn bounded_command(value: *const core::ffi::c_char, out: &mut [u8; 32]) -> Option<usize> {
    let mut len = 0;
    while len < out.len() {
        // SAFETY: the parameter core supplies a NUL-terminated value; at most
        // 32 bytes are examined and never beyond the first NUL.
        let byte = unsafe { *value.add(len) } as u8;
        if byte == 0 { return Some(len); }
        out[len] = byte;
        len += 1;
    }
    None
}

unsafe extern "C" fn e2e_set(value: *const core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if value.is_null() { return -22; }
    let mut command = [0u8; 32];
    let Some(len) = bounded_command(value, &mut command) else { return -22; };
    let Ok(text) = core::str::from_utf8(&command[..len]) else { return -22; };
    let text = text.trim();
    let (verb, argument) = match text.split_once(' ') {
        Some((verb, argument)) => (verb, argument.trim()),
        None => (text, ""),
    };
    let hex = |argument: &str| u64::from_str_radix(argument.strip_prefix("0x").unwrap_or(argument), 16).ok();
    match verb {
        "arm" => {
            let Some(mask) = hex(if argument.is_empty() { "1" } else { argument }) else { return -22; };
            let mask = (mask as u32) | OPT_BASE;
            if mask & OPT_POWER != 0 && !map_power_sources() { return -12; }
            ARMED.store(0, Ordering::Relaxed);
            GENERATION.fetch_add(1, Ordering::Relaxed);
            DROPPED.store(0, Ordering::Relaxed);
            CURSOR.store(0, Ordering::Relaxed);
            NEXT.store(0, Ordering::Relaxed);
            ARMED.store(mask, Ordering::Release);
        }
        "stop" => {
            ARMED.store(0, Ordering::Release);
            BOUND.store(NEXT.load(Ordering::Relaxed).min(CAPACITY), Ordering::Relaxed);
        }
        "mark" => {
            let Some(value) = hex(argument) else { return -22; };
            record(K_MARK, 0, value, [0; 5]);
        }
        "seek" => {
            let Some(index) = hex(argument) else { return -22; };
            CURSOR.store((index as usize).min(CAPACITY), Ordering::Relaxed);
        }
        _ => return -22,
    }
    0
}

unsafe extern "C" fn e2e_get(buf: *mut core::ffi::c_char, _: *const bindings::kernel_param) -> core::ffi::c_int {
    if buf.is_null() { return -22; }
    // SAFETY: sysfs parameter output is PAGE_SIZE, at least 4096 bytes.
    let bytes = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), 4095) };
    let mut out = Output { bytes, used: 0 };
    let armed = ARMED.load(Ordering::Acquire);
    let bound = if armed != 0 { NEXT.load(Ordering::Relaxed).min(CAPACITY) } else { BOUND.load(Ordering::Relaxed) };
    let generation = GENERATION.load(Ordering::Relaxed) as u64 & 0xffff;
    let mut cursor = CURSOR.load(Ordering::Relaxed);
    let _ = writeln!(out, "s {:x} {:x} {:x} {:x} {:x} {}", armed, generation, cursor, bound,
        DROPPED.load(Ordering::Relaxed), crate::g18::compat::timing_buffer::arch_frequency());
    for _ in 0..24 {
        if cursor >= bound { break; }
        let slot = &SLOTS[cursor];
        let head = slot.w[0].load(Ordering::Acquire);
        let meta = slot.w[1].load(Ordering::Relaxed);
        if head == 0 || (meta >> 16) & 0xffff != generation {
            // Reserved but not yet published (or a stale late writer).
            let _ = writeln!(out, "u {:x}", cursor);
        } else {
            let _ = write!(out, "r {:x} {:x} {:x}", cursor, head, meta);
            for word in &slot.w[2..] { let _ = write!(out, " {:x}", word.load(Ordering::Relaxed)); }
            let _ = writeln!(out);
        }
        cursor += 1;
    }
    CURSOR.store(cursor, Ordering::Relaxed);
    out.used as core::ffi::c_int
}

static E2E_OPS: bindings::kernel_param_ops = bindings::kernel_param_ops {
    flags: 0, set: Some(e2e_set), get: Some(e2e_get), free: None,
};
#[link_section = "__param"]
#[used]
static E2E_PARAM: KernelParam = KernelParam::new(bindings::kernel_param {
    name: kernel::str::as_char_ptr_in_const_context(if cfg!(MODULE) {
        c"g17p_e2e"
    } else { c"aurora.g17p_e2e" }),
    #[cfg(MODULE)]
    mod_: unsafe { core::ptr::from_ref(&bindings::__this_module).cast_mut() },
    #[cfg(not(MODULE))]
    mod_: core::ptr::null_mut(),
    ops: core::ptr::from_ref(&E2E_OPS), perm: 0o600, level: -1, flags: 0,
    __bindgen_anon_1: bindings::kernel_param__bindgen_ty_1 { arg: core::ptr::null_mut() },
});
