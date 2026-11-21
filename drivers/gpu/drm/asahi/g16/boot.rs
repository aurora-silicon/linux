// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! M4/J713 cold admission and the firmware start sequence (25G83; 26B123 once its unknowns are
//! known).
//!
//! The Rust owner validates firmware identity and all inherited regions before publication,
//! holds the PMP AGX vote (25G83; macOS 27.2 sends none), prepares owned objects and publishes
//! context0 while ASC RUN is clear.
//! It negotiates RTKit protocol12, accepts only the inherited EP1 crash buffer, then submits
//! initdata plus the consume-only NOP and policy (re-import Globals, start; 0x1a, 0x34 and
//! 0x0a on 25G83, 0x19, 0x35 and 0x0a on 26B123). Control+0x14 and exact retired indices
//! qualify READY; no M3 firmware constants or device-control sequence are reused.
//! Firmware-reachable backing and the RTKit cookie remain retained until reboot.

use core::ffi::c_void;
use core::sync::atomic::Ordering;

use kernel::{
    bindings, device,
    device::Core,
    error::{from_err_ptr, to_result},
    platform,
    prelude::*,
    sync::{aref::ARef, Arc, ArcBorrow},
    time::{
        delay::{fsleep, udelay},
        Delta, Instant, Monotonic,
    },
};

use super::adt::{self, GpuAdt};
use super::channel::{self, Channel, Channels};
use super::event::Events;
use super::fw::channels::DevCtl;
use super::hw::{self, LoaderTable};
use super::initdata::{self, InitData};
use super::object::{Class, Objects, Placement, Request};
use super::{regs, uat, Config, Shared};
use crate::driver::AsahiDrmDevice;
use crate::{mmu, regs as shared_regs};
use kernel::io::Io;

/// The C shim (`g16/glue.c`): helpers without Rust bindings.
pub(crate) mod glue {
    use core::sync::atomic::{AtomicBool, Ordering};
    use kernel::{device, prelude::*};

    extern "C" {
        fn asahi_g16_board() -> bool;
        fn asahi_g16_display_prepare(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
        fn asahi_g16_perf_rows() -> core::ffi::c_uint;
        fn asahi_g16_hold_power(dev: *mut core::ffi::c_void) -> core::ffi::c_int;
        fn asahi_g16_ram_disjoint(start: u64, size: u64) -> bool;
        fn asahi_g16_ioremap_wc(start: u64, size: usize) -> *mut core::ffi::c_void;
        fn asahi_g16_dma_unsupported(dev: *mut core::ffi::c_void) -> bool;
        fn asahi_g16_power_domain_state(
            dev: *mut core::ffi::c_void,
            offset: *mut u32,
            state: *mut u32,
        ) -> core::ffi::c_int;
        fn asahi_g16_reserved_region(
            dev: *mut core::ffi::c_void,
            name: *const core::ffi::c_char,
            start: *mut u64,
            size: *mut u64,
        ) -> core::ffi::c_int;
    }

    /// A qualified T8132 board (J713, J773g).
    pub(crate) fn board() -> bool {
        // SAFETY: Reads the machine compatible strings only.
        unsafe { asahi_g16_board() }
    }

    /// Links the GPU to the published native display, or defers until it is.
    pub(crate) fn display_prepare(dev: &device::Device) -> Result {
        // SAFETY: `dev` is the bound GPU device; the helper only adds a device link.
        kernel::error::to_result(unsafe { asahi_g16_display_prepare(dev.as_raw().cast()) })
    }

    /// The firmware DVFS row count qualified for this board, or zero.
    pub(crate) fn perf_rows() -> usize {
        // SAFETY: Reads the machine compatible strings only.
        unsafe { asahi_g16_perf_rows() as usize }
    }

    /// Holds the GFX power domain for the retained session.
    pub(crate) fn hold_power(dev: &device::Device) -> Result {
        // SAFETY: `dev` is a valid device; runtime-PM accounting on it has no other requirement.
        kernel::error::to_result(unsafe { asahi_g16_hold_power(dev.as_raw().cast()) })
    }

    static RETAINED: AtomicBool = AtomicBool::new(false);

    /// Whether power admission or firmware publication requires the device to remain bound.
    pub(crate) fn retained() -> bool {
        RETAINED.load(Ordering::Acquire)
    }

    /// Pins this module until reboot, once (`try_module_get(THIS_MODULE)`, see `retain.rs`).
    /// ENODEV, with nothing pinned or marked, once the module's removal has begun.
    pub(crate) fn retain_module() -> Result {
        crate::retain::retain_module(&RETAINED)
    }

    /// The physical range is disjoint from System RAM.
    pub(crate) fn ram_disjoint(start: u64, size: u64) -> bool {
        // SAFETY: Only queries the resource tree.
        unsafe { asahi_g16_ram_disjoint(start, size) }
    }

    /// Whether the device's DMA mappings skip cache maintenance.
    pub(crate) fn dma_unsupported(dev: &device::Device) -> bool {
        // SAFETY: Reads a flag of a valid device.
        unsafe { asahi_g16_dma_unsupported(dev.as_raw().cast()) }
    }

    /// Read the first PMGR power-domain register without changing its ownership or state.
    pub(crate) fn power_domain_state(dev: &device::Device) -> Result<(u32, u32)> {
        let (mut offset, mut state) = (0, 0);
        // SAFETY: The live device owns the OF reference; outputs are valid for this call.
        kernel::error::to_result(unsafe {
            asahi_g16_power_domain_state(dev.as_raw().cast(), &mut offset, &mut state)
        })?;
        Ok((offset, state))
    }

    /// Resolve a validated, non-reusable no-map reserved-memory phandle by name.
    pub(crate) fn reserved_region(dev: &device::Device, name: &CStr) -> Result<(u64, u64)> {
        let (mut start, mut size) = (0, 0);
        // SAFETY: The valid device and C string are borrowed for the call; output pointers live.
        kernel::error::to_result(unsafe {
            asahi_g16_reserved_region(
                dev.as_raw().cast(),
                name.as_char_ptr(),
                &mut start,
                &mut size,
            )
        })?;
        Ok((start, size))
    }

    /// `ioremap_wc`, for the RTKit crash buffer.
    pub(crate) fn ioremap_wc(start: u64, size: usize) -> *mut core::ffi::c_void {
        // SAFETY: Creates a new mapping of a device range; no other effect.
        unsafe { asahi_g16_ioremap_wc(start, size) }
    }
}

/// `EPROTO` (not among the Rust kernel's named error codes).
pub(crate) fn eproto() -> Error {
    Error::from_errno(-(bindings::EPROTO as i32))
}

/// `Shared` from an RTKit cookie.
///
/// # Safety
///
/// `cookie` must be the pointer [`Rtkit::new`] registered.
unsafe fn shared<'a>(cookie: *mut c_void) -> ArcBorrow<'a, Shared> {
    // SAFETY: The cookie is a leaked `Arc<Shared>` (see `Rtkit::new`), live until reboot.
    unsafe { ArcBorrow::from_raw(cookie as *const Shared) }
}

/// RTKit crash callback: latches fail-stop.
///
/// # Safety
///
/// `cookie` must be the live Shared cookie registered with these RTKit operations.
unsafe extern "C" fn rtkit_crashed(cookie: *mut c_void, log: *const c_void, size: usize) {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    dev_err!(
        shared.dev,
        "GPU firmware crashed; retaining session until reboot\n"
    );
    if !log.is_null() && size != 0 {
        // SAFETY: RTKit passes its shadow copy of the crash log, `size` bytes, valid and not
        // written for the duration of this call.
        let log = unsafe { core::slice::from_raw_parts(log.cast::<u8>(), size) };
        dump_crashlog(&shared.dev, log);
        dump_outer_frame(&shared, log);
    }
    shared.fail_stop(EIO);
}

fn le64(log: &[u8], at: usize) -> Option<u64> {
    Some(le32(log, at)? as u64 | (le32(log, at.checked_add(4)?)? as u64) << 32)
}

/// The first `tag` section of the first `CLHE` log: (offset, length).
fn crashlog_section(log: &[u8], tag: u32) -> Option<(usize, usize)> {
    let start = (0..log.len().saturating_sub(0x20))
        .step_by(4)
        .find(|&at| le32(log, at) == Some(CRASHLOG_HEADER))?;
    let end = match le32(log, start + 8) {
        Some(len) if len as usize >= 0x20 && start + len as usize <= log.len() => {
            start + len as usize
        }
        _ => log.len(),
    };
    let mut at = start + 0x20;
    while at + 16 <= end {
        let (t, len) = (le32(log, at)?, le32(log, at + 12)? as usize);
        if t == CRASHLOG_HEADER || len < 16 || len > end - at {
            return None;
        }
        if t == tag {
            return Some((at, len));
        }
        at += len;
    }
    None
}

/// Firmware VAs without their pointer-authentication bits (42-bit TTBR1 space).
const FW_VA_STRIP: u64 = !0x3ff_ffff_ffff;

/// A firmware VA -> PA through the crash log's own memory map (`Ccdp`: from +0x10,
/// 0x14-byte records {VA u64, PA u64, size u32}), only inside firmware DATA.
fn crash_va_pa(
    shared: &Shared,
    log: &[u8],
    ccdp: (usize, usize),
    va: u64,
    len: u64,
) -> Option<u64> {
    let va = va | FW_VA_STRIP;
    let (at, size) = ccdp;
    let mut rec = at + 0x10;
    while rec + 0x14 <= at + size {
        let (base, pa, n) = (
            le64(log, rec)?,
            le64(log, rec + 8)?,
            le32(log, rec + 16)? as u64,
        );
        if base == 0 {
            break;
        }
        if va >= base && va.checked_add(len)? <= base.checked_add(n)? {
            let pa = pa + (va - base);
            let data = &shared.data;
            if pa >= data.start && pa.checked_add(len)? <= data.end {
                return Some(pa);
            }
            return None;
        }
        rec += 0x14;
    }
    None
}

/// Reads `N` u64 words of firmware DATA at a firmware VA, after the crash.
fn crash_read<const N: usize>(
    shared: &Shared,
    log: &[u8],
    ccdp: (usize, usize),
    va: u64,
) -> Option<[u64; N]> {
    let pa = crash_va_pa(shared, log, ccdp, va, (N * 8) as u64)?;
    let page = pa & !0xfff;
    let span = ((pa - page) as usize + N * 8 + 0xfff) & !0xfff;
    let iomem = glue::ioremap_wc(page, span);
    if iomem.is_null() {
        return None;
    }
    let mut words = [0u64; N];
    for (i, w) in words.iter_mut().enumerate() {
        // SAFETY: `iomem` maps `span` bytes from `page`, which cover [pa, pa + N * 8); the
        // firmware is crashed and halted, the words are only read.
        *w = unsafe {
            core::ptr::read_volatile(
                iomem
                    .cast::<u8>()
                    .add((pa - page) as usize + i * 8)
                    .cast::<u64>(),
            )
        };
    }
    // SAFETY: Mapped above, not used after this call.
    unsafe { bindings::iounmap(iomem) };
    Some(words)
}

/// The 27.2 GPU firmware dies of a nested SError: its first exception (an SError in a
/// task, taken through the SP0 vector into the task's own context, `TPIDRRO_EL0`) finds no
/// handler, `__rtk_arch_exception`'s fatal path unmasks SError (`msr DAIFClr, #4`, depth 1)
/// and takes it again; the crash log only holds that second frame, whose x19 is the first
/// frame. Prints the first frame (ELR/SPSR/ESR/FAR, x0-x30, SP) and its frame-pointer
/// chain from firmware DATA. Text offsets are relative to the `Ccdp` TEXT base.
fn dump_outer_frame(shared: &Shared, log: &[u8]) {
    let dev = &shared.dev;
    let (Some(crg8), Some(ccdp)) = (
        crashlog_section(log, fourcc(b"Crg8")),
        crashlog_section(log, fourcc(b"Ccdp")),
    ) else {
        return;
    };
    // Crg8: +0x10 kind, +0x14 frame size, the 0x348-byte frame from +0x18.
    let Some(outer) = le64(log, crg8.0 + 0x18 + 19 * 8) else {
        return;
    };
    let text = le64(log, ccdp.0 + 0x10).unwrap_or(0);
    let off = |va: u64| (va | FW_VA_STRIP).wrapping_sub(text);
    let Some(frame) = crash_read::<0x69>(shared, log, ccdp, outer) else {
        dev_err!(
            dev,
            "G16 crash: outer frame {:#x} not in firmware DATA\n",
            outer
        );
        return;
    };
    let (elr, spsr, sp, far, esr) = (
        frame[0x100 / 8],
        frame[0x108 / 8],
        frame[0xf8 / 8],
        frame[0x328 / 8],
        frame[0x338 / 8] & 0xffff_ffff,
    );
    dev_err!(
        dev,
        "G16 crash: outer frame {:#x}: ELR text+{:#x} SPSR {:#x} ESR {:#x} FAR {:#x} SP {:#x} LR text+{:#x}\n",
        outer,
        off(elr),
        spsr,
        esr,
        far,
        sp,
        off(frame[30])
    );
    for r in (0..31).step_by(4) {
        let x = |i: usize| if i < 31 { frame[i] } else { 0 };
        dev_err!(
            dev,
            "G16 crash: outer x{:02}-x{:02} = {:016x} {:016x} {:016x} {:016x}\n",
            r,
            (r + 3).min(30),
            x(r),
            x(r + 1),
            x(r + 2),
            x(r + 3)
        );
    }
    // The task's frame records: {previous FP, LR}.
    let mut fp = frame[29];
    for depth in 0..16 {
        if fp == 0 || fp & 0xf != 0 {
            break;
        }
        let Some([next, lr]) = crash_read::<2>(shared, log, ccdp, fp) else {
            dev_err!(
                dev,
                "G16 crash: outer #{} FP {:#x} not in firmware DATA\n",
                depth,
                fp
            );
            break;
        };
        dev_err!(
            dev,
            "G16 crash: outer #{} FP {:#x} LR text+{:#x}\n",
            depth,
            fp,
            off(lr)
        );
        fp = next;
    }
    // The task stack above SP: spilled pointers and arguments.
    if let Some(stack) = crash_read::<0x20>(shared, log, ccdp, sp) {
        for row in (0..0x20).step_by(4) {
            dev_err!(
                dev,
                "G16 crash: outer SP+{:#05x}: {:016x} {:016x} {:016x} {:016x}\n",
                row * 8,
                stack[row],
                stack[row + 1],
                stack[row + 2],
                stack[row + 3]
            );
        }
    }
}

const fn fourcc(tag: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*tag)
}

/// Crash log sections RTKit's parser does not decode on the 27.2 GPU firmware, and its
/// register section, whose fixed layout reads the 27.2 ESR as `0xfffffc00be000000`: the
/// context the SError interrupted is not in the registers it prints (those are
/// `__rtk_arch_exception`'s own frame).
const CRASHLOG_DUMP: [u32; 7] = [
    fourcc(b"Crg8"),
    fourcc(b"Csts"),
    fourcc(b"CasC"),
    fourcc(b"Crtk"),
    fourcc(b"Ccdp"),
    fourcc(b"Cpct"),
    fourcc(b"Csta"),
];
const CRASHLOG_HEADER: u32 = fourcc(b"CLHE");
/// Bytes printed per section; all-zero rows are counted, not printed.
const CRASHLOG_DUMP_MAX: usize = 0x800;

fn le32(log: &[u8], at: usize) -> Option<u32> {
    let b = log.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// Hex-dumps the undecoded sections of the crash log: the first `CLHE` log in the buffer
/// (inside any container), walked as RTKit walks it (fourcc at +0, size at +12, from +0x20).
fn dump_crashlog(dev: &kernel::device::Device, log: &[u8]) {
    let Some(start) = (0..log.len().saturating_sub(0x20))
        .step_by(4)
        .find(|&at| le32(log, at) == Some(CRASHLOG_HEADER))
    else {
        dev_err!(dev, "G16 crashlog: no CLHE log in {:#x} bytes\n", log.len());
        return;
    };
    let end = match le32(log, start + 8) {
        Some(len) if len as usize >= 0x20 && start + len as usize <= log.len() => {
            start + len as usize
        }
        _ => log.len(),
    };
    dev_err!(dev, "G16 crashlog: CLHE at {:#x}, end {:#x}\n", start, end);
    let mut at = start + 0x20;
    while at + 16 <= end {
        let (Some(tag), Some(len)) = (le32(log, at), le32(log, at + 12)) else {
            break;
        };
        let len = len as usize;
        if tag == CRASHLOG_HEADER || len < 16 || len > end - at {
            break;
        }
        let name = tag.to_be_bytes().map(|c| if c.is_ascii_graphic() { c } else { b'?' });
        let name = core::str::from_utf8(&name).unwrap_or("????");
        dev_err!(dev, "G16 crashlog {} at {:#x}, {:#x} bytes\n", name, at, len);
        if CRASHLOG_DUMP.contains(&tag) {
            let mut zeros = 0;
            let shown = len.min(CRASHLOG_DUMP_MAX);
            for row in (0..shown).step_by(32) {
                let w = |i: usize| le32(log, at + row + i * 4).unwrap_or(0);
                let words = [w(0), w(1), w(2), w(3), w(4), w(5), w(6), w(7)];
                if words.iter().all(|&x| x == 0) {
                    zeros += 1;
                    continue;
                }
                if zeros != 0 {
                    dev_err!(dev, "G16 crashlog {}: {} zero rows\n", name, zeros);
                    zeros = 0;
                }
                dev_err!(
                    dev,
                    "G16 crashlog {} +{:#05x}: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}\n",
                    name,
                    row,
                    words[0],
                    words[1],
                    words[2],
                    words[3],
                    words[4],
                    words[5],
                    words[6],
                    words[7]
                );
            }
            if zeros != 0 {
                dev_err!(dev, "G16 crashlog {}: {} zero rows\n", name, zeros);
            }
        }
        at += len;
    }
}

/// RTKit message callback.
///
/// # Safety
///
/// `cookie` must be the live Shared cookie registered with these RTKit operations.
unsafe extern "C" fn rtkit_recv(cookie: *mut c_void, endpoint: u8, message: u64) {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    // EP 0x20 0x0042... asks for receive-ring draining; nothing else expected.
    if endpoint == hw::ep::FIRMWARE && message == hw::ep::RING_SERVICE {
        if shared.started.load(Ordering::Acquire) {
            Arc::<Shared>::from(shared).kick();
        }
        return;
    }
    dev_err!(
        shared.dev,
        "unexpected firmware EP{:#x} message {:#x}\n",
        endpoint,
        message
    );
    // What a firmware says while it comes back from a sleep isn't known yet.
    if !shared.sleeping.load(Ordering::Acquire) {
        shared.fail_stop(eproto());
    }
}

/// EP 0x20 ring-service request, in the mailbox interrupt: wake only. The drain and the
/// backend's completion check run in the poll work; the shared state outlives the mailbox.
///
/// # Safety
///
/// `cookie` must be the live Shared cookie registered with these RTKit operations.
unsafe extern "C" fn rtkit_recv_early(cookie: *mut c_void, endpoint: u8, message: u64) -> bool {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    if !hw::tuning::EARLY_WAKE
        || endpoint != hw::ep::FIRMWARE
        || message != hw::ep::RING_SERVICE
        || !shared.started.load(Ordering::Acquire)
    {
        return false;
    }
    Arc::<Shared>::from(shared).kick();
    true
}

/// Only the inherited crash buffer inside this session's firmware DATA, once.
///
/// # Safety
///
/// `cookie` must be the registered live Shared cookie. `bfr` must point to the
/// writable RTKit buffer descriptor, exclusively borrowed for this callback.
unsafe extern "C" fn rtkit_shmem_setup(
    cookie: *mut c_void,
    endpoint: u8,
    bfr: *mut bindings::apple_rtkit_shmem,
) -> core::ffi::c_int {
    // SAFETY: RTKit passes back the registered cookie.
    let shared = unsafe { shared(cookie) };
    // SAFETY: RTKit passes a valid buffer descriptor it owns for the duration of the call.
    let bfr = unsafe { &mut *bfr };
    let (iova, size) = (bfr.iova, bfr.size as u64);
    let data = &shared.data;
    let data_size = data.end - data.start;
    if endpoint != 1
        || size != 0x8000
        || iova == 0
        || (iova | size) & 0xfff != 0
        || iova < data.start
        || size > data_size
        || iova - data.start > data_size - size
        || shared
            .rtkit_buffer
            .compare_exchange(0, u64::MAX, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
    {
        dev_err!(
            shared.dev,
            "refusing RTKit buffer {:#x}/{:#x}\n",
            iova,
            size
        );
        shared.fail_stop(EINVAL);
        return EINVAL.to_errno();
    }
    let iomem = glue::ioremap_wc(iova, bfr.size);
    if iomem.is_null() {
        shared.fail_stop(ENOMEM);
        return ENOMEM.to_errno();
    }
    bfr.iomem = iomem;
    bfr.is_mapped = true;
    shared.rtkit_buffer.store(iova, Ordering::Release);
    dev_info!(
        shared.dev,
        "mapped firmware crash storage {:#x}/{:#x}\n",
        iova,
        size
    );
    0
}

/// Retained until reboot: no qualified stop path.
///
/// # Safety
///
/// This no-op callback accesses neither pointer and releases no storage.
unsafe extern "C" fn rtkit_shmem_destroy(
    _cookie: *mut c_void,
    _bfr: *mut bindings::apple_rtkit_shmem,
) {
}

/// The GPU's RTKit callbacks.
const RTKIT_OPS_BASE: bindings::apple_rtkit_ops = bindings::apple_rtkit_ops {
    flags: 0,
    crashed: Some(rtkit_crashed),
    recv_message: Some(rtkit_recv),
    recv_message_early: Some(rtkit_recv_early),
    shmem_setup: None,
    shmem_setup_ep: Some(rtkit_shmem_setup),
    shmem_destroy: Some(rtkit_shmem_destroy),
    syslog_is_debug: None,
    protocol_version: 12,
};

/// 25G83: the firmware's crash buffer is mapped without a reply.
static RTKIT_OPS: bindings::apple_rtkit_ops = RTKIT_OPS_BASE;

/// macOS 27.2: the crash-log endpoint starts while the endpoint map is handled, and the
/// firmware's request for its own crash buffer is sent back unchanged.
static RTKIT_OPS_27_2: bindings::apple_rtkit_ops = bindings::apple_rtkit_ops {
    flags: bindings::APPLE_RTKIT_COPROC_PLACES_BUFFERS,
    ..RTKIT_OPS_BASE
};

// The log buffer the 27.2 start hands over is the crash copy buffer HW data sizes.
const _: () = assert!(hw::start_27_2::LOG_BUFFER_SIZE as u64 == super::fw::initdata::COREDUMP_COPY_SIZE);

/// The host log buffer message the 27.2 start sends from RTKit's `before_ap` hook.
struct LogBuffer {
    rtk: *mut bindings::apple_rtkit,
    shared: *const Shared,
    message: u64,
    result: core::ffi::c_int,
}

/// RTKit `before_ap` hook of the 27.2 start: once the firmware's own crash buffer has been
/// set up (and its request sent back), hands it the host log buffer, as macOS does right
/// before AP power-on.
///
/// # Safety
///
/// `data` must point to a live [`LogBuffer`] exclusively borrowed for this call, whose `rtk`
/// is the RTKit instance running the hook and whose `shared` outlives it.
unsafe extern "C" fn publish_log_buffer(data: *mut c_void) {
    // SAFETY: By the function's contract.
    let log = unsafe { &mut *data.cast::<LogBuffer>() };
    // SAFETY: By the function's contract.
    let shared = unsafe { &*log.shared };
    let start = Instant::<Monotonic>::now();
    while matches!(shared.rtkit_buffer.load(Ordering::Acquire), 0 | u64::MAX) {
        if start.elapsed() > Delta::from_millis(1000) {
            log.result = ETIMEDOUT.to_errno();
            return;
        }
        fsleep(Delta::from_millis(1));
    }
    // The request is sent back right after its setup returns; keep macOS's order.
    fsleep(Delta::from_millis(1));
    // SAFETY: `log.rtk` is the live RTKit instance running this hook.
    log.result = unsafe {
        bindings::apple_rtkit_send_message(
            log.rtk,
            hw::start_27_2::EP_CRASHLOG,
            log.message,
            core::ptr::null_mut(),
            false,
        )
    };
}

/// The GPU's RTKit client. Never freed: the firmware session lasts until reboot.
///
/// # Invariants
///
/// `rtk` is a live `struct apple_rtkit` whose cookie is a leaked `Arc<Shared>`.
pub(crate) struct Rtkit {
    rtk: *mut bindings::apple_rtkit,
}

// SAFETY: The C RTKit API is callable from any thread; mutating calls take `&mut self`.
unsafe impl Send for Rtkit {}
// SAFETY: Shared calls (`has_endpoint`) only read RTKit state.
unsafe impl Sync for Rtkit {}

impl Rtkit {
    /// Initializes the sole RTKit owner on the default mailbox (retained).
    fn new(
        dev: &device::Device,
        shared: &Arc<Shared>,
        ops: &'static bindings::apple_rtkit_ops,
    ) -> Result<Self> {
        let cookie = Arc::into_raw(shared.clone()) as *mut c_void;
        // SAFETY: `dev` is valid, the name is a NUL-terminated string and the ops are static.
        let rtk = from_err_ptr(unsafe {
            bindings::apple_rtkit_init(dev.as_raw(), cookie, core::ptr::null(), 0, ops)
        });
        match rtk {
            // INVARIANT: The cookie stays leaked from here on.
            Ok(rtk) => Ok(Self { rtk }),
            Err(e) => {
                // SAFETY: Init failed, so RTKit holds no copy of the cookie from `into_raw`.
                drop(unsafe { Arc::from_raw(cookie as *const Shared) });
                Err(e)
            }
        }
    }

    pub(crate) fn wake(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_wake(self.rtk) })
    }

    /// The C handle, for the stop at a sleep's commit.
    pub(crate) fn as_raw(&self) -> *mut bindings::apple_rtkit {
        self.rtk
    }

    /// Prepares for HELLO from firmware restarting warm, which keeps its buffers and rings.
    pub(crate) fn warm_restart(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_warm_restart(self.rtk) })
    }

    /// Waits for the firmware to report in without an IOP power request, runs `before_ap`, then
    /// sets the AP on.
    ///
    /// # Safety
    ///
    /// `before_ap` must be safe to call with `data`, from this thread, during this call.
    unsafe fn boot_with(
        &mut self,
        before_ap: unsafe extern "C" fn(*mut c_void),
        data: *mut c_void,
    ) -> Result {
        // SAFETY: `rtk` is live by the type invariant; the caller vouches for the hook.
        to_result(unsafe { bindings::apple_rtkit_boot_with(self.rtk, Some(before_ap), data) })
    }

    /// Waits for the firmware to report in, and sets the AP on.
    pub(crate) fn boot(&mut self) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_boot(self.rtk) })
    }

    fn running(&self) -> bool {
        // SAFETY: `rtk` is retained and valid for the session lifetime.
        unsafe {
            bindings::apple_rtkit_is_running(self.rtk)
                && !bindings::apple_rtkit_is_crashed(self.rtk)
        }
    }

    fn has_endpoint(&self, endpoint: u8) -> bool {
        // SAFETY: `rtk` is live by the type invariant.
        unsafe { bindings::apple_rtkit_has_endpoint(self.rtk, endpoint) }
    }

    fn start_endpoint(&mut self, endpoint: u8) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe { bindings::apple_rtkit_start_ep(self.rtk, endpoint) })
    }

    /// Sends a message without waiting.
    pub(crate) fn send(&mut self, endpoint: u8, message: u64) -> Result {
        // SAFETY: `rtk` is live by the type invariant.
        to_result(unsafe {
            bindings::apple_rtkit_send_message(
                self.rtk,
                endpoint,
                message,
                core::ptr::null_mut(),
                false,
            )
        })
    }
}

/// The result of admission: everything probe-time checks established, nothing written.
pub(crate) struct Admitted {
    pub(crate) regs: regs::Regs,
    pub(crate) adt: GpuAdt,
}

/// The running firmware session: RTKit, MMIO and the adopted UAT. (The
/// host window lives in [`Objects`], which maps through it.)
pub(crate) struct Session {
    pub(crate) dev: ARef<device::Device>,
    pub(crate) shared: Arc<Shared>,
    pub(crate) rtkit: Rtkit,
    pub(crate) regs: regs::Regs,
    pub(crate) adt: GpuAdt,
    uat: Arc<mmu::Uat>,
    /// The PPL handoff page, mapped here only to read the firmware's powered flag
    /// ([`hw::FwLayout::powered`]); `None` when the firmware keeps the GPU powered.
    handoff: Option<regs::Mapping>,
    /// The last host mailbox sends (CLOCK_MONOTONIC ns, endpoint, message), for stall dumps.
    pub(crate) sent: [(u64, u8, u64); SENT_LOG],
    pub(crate) sent_next: usize,
}

/// Host mailbox sends kept for stall diagnostics.
pub(crate) const SENT_LOG: usize = 32;

impl Session {
    /// The adopted UAT (user VMs are created and bound through it).
    pub(crate) fn uat(&self) -> Arc<mmu::Uat> {
        self.uat.clone()
    }

    /// Live transport and powered-firmware health, plus any previously latched failure.
    pub(crate) fn check(&self) -> Result {
        self.transport()?;
        // The performance state is in the power controller, which the firmware itself reads
        // with the GPU powered down and admission reads before the firmware runs: it needs no
        // powered GPU.
        if self.regs.asc_control()? & regs::ASC_RUN == 0 || self.regs.pstate()? > self.regs.top()
        {
            return Err(EIO);
        }
        self.uat.check_adopted_bindings()
    }

    /// The transport alone: a latched failure, or RTKit stopped or crashed. No register or
    /// UAT read, so the retirement loop can ask between its receipts.
    pub(crate) fn transport(&self) -> Result {
        self.shared.check()?;
        if !self.rtkit.running() {
            return Err(EIO);
        }
        Ok(())
    }

    /// Runs `read` with the GPU held powered, or returns `None` while it is powered down. A
    /// firmware that powers the GPU down while idle changes GPU power only while it holds the
    /// PPL handoff lock, and records the result on the handoff page before releasing it; the
    /// AP side of the lock is held here across that record and `read`, so the GPU cannot go
    /// down between them. Reading a GPU register while it is down takes an SError.
    fn while_powered<T>(&self, read: impl FnOnce(&regs::Regs) -> Result<T>) -> Result<Option<T>> {
        let (Some(page), Some(offset)) = (&self.handoff, self.adt.layout.powered) else {
            return read(&self.regs).map(Some);
        };
        let _held = self.uat.lock_m4_handoff()?;
        if page.io().try_read8(offset)? == 0 {
            return Ok(None);
        }
        read(&self.regs).map(Some)
    }

    /// The engine receipts of a running firmware, or `None` while the GPU is powered down.
    pub(crate) fn engine_state(&self) -> Result<Option<regs::EngineState>> {
        self.while_powered(regs::Regs::engine_state)
    }

    /// The first engine fault bank holding a fault, for a firmware halted for recovery, or
    /// `None` while the GPU is powered down.
    pub(crate) fn fault_scan(&self) -> Result<Option<regs::EngineState>> {
        self.while_powered(regs::Regs::fault_scan)
    }

    /// Sends a mailbox message.
    pub(crate) fn send(&mut self, endpoint: u8, message: u64) -> Result {
        self.sent[self.sent_next % SENT_LOG] = (super::recovery::now_ns(), endpoint, message);
        self.sent_next = self.sent_next.wrapping_add(1);
        self.rtkit.send(endpoint, message)
    }

    /// Rings a submit channel's doorbell: EP 0x21 `0x0083 << 48 | channel`.
    pub(crate) fn ring(&mut self, channel: Channel) -> Result {
        self.send(hw::ep::DOORBELL, hw::engine::DOORBELL | channel as u64)
    }
}

/// The firmware at READY.
pub(crate) struct Booted {
    pub(crate) session: Session,
    pub(crate) init: InitData,
    pub(crate) objects: Objects,
    pub(crate) channels: Channels,
    pub(crate) events: Events,
    pub(crate) id: regs::Identity,
    pub(crate) max_frequency_khz: u32,
}

/// A firmware session that did not complete: retained until reboot, its receive rings still
/// drained so the firmware never waits on the host.
pub(crate) struct Orphan {
    pub(crate) session: Session,
    /// Retained only: the firmware may still use them.
    pub(crate) _objects: Objects,
    pub(crate) channels: Option<Channels>,
    pub(crate) _init: Option<InitData>,
    pub(crate) events: Events,
}

impl Orphan {
    /// Drains the receive rings, if they exist.
    pub(crate) fn drain(&mut self) -> Result {
        if let Some(channels) = &self.channels {
            channels.drain_rings(&self.session.dev, &mut self.events)?;
        }
        Ok(())
    }
}

/// Probe-time admission.
#[inline(never)]
pub(crate) fn admit(
    pdev: &platform::Device<Core<'_>>,
    _dev: &AsahiDrmDevice,
    cfg: &'static Config,
    res: shared_regs::G16Resources,
) -> Result<Admitted> {
    let dev = pdev.as_ref();
    if !glue::board() {
        return Err(ENODEV);
    }
    // Retain the PMP supplier (and its AGX bridge) before any vote.
    // SAFETY: `dev` is the bound GPU device.
    let ret = unsafe { bindings::apple_pmp_j713_gpu_prepare(dev.as_raw()) };
    if ret != 0 {
        let err = Error::from_errno(ret);
        if err != EPROBE_DEFER {
            dev_err!(dev, "PMP supplier not ready: {:?}\n", err);
        }
        return Err(err);
    }
    glue::display_prepare(dev).inspect_err(|e| {
        if *e != EPROBE_DEFER {
            dev_err!(dev, "native display not admitted: {:?}\n", e);
        }
    })?;
    let adt =
        adt::read(dev).inspect_err(|e| dev_err!(dev, "ADT GPU regions not admitted: {:?}\n", e))?;
    crate::fwid::identify(
        dev,
        adt.text.clone(),
        adt.data.end - adt.data.start,
        cfg.firmware,
    )
    .inspect_err(|e| dev_err!(dev, "loaded GFX TEXT identity mismatch: {:?}\n", e))?;
    dev_info!(
        dev,
        "M4: firmware identity accepted ({} layout)\n",
        adt.layout.name
    );
    // Refuse a firmware whose start needs something not known yet before anything is held.
    let mut unknown = false;
    for what in adt.layout.unknown() {
        dev_err!(
            dev,
            "G16: the {} firmware cannot be started yet; not known: {}\n",
            adt.layout.name,
            what
        );
        unknown = true;
    }
    if unknown {
        return Err(ENOTSUPP);
    }
    // Firmware objects are cleaned to the point of coherency by their DMA mapping: that needs
    // a device that is not DMA-coherent, as the synthesized node is.
    if glue::dma_unsupported(dev) {
        dev_err!(dev, "G16: unsupported coherent, IOMMU or custom DMA path\n");
        return Err(EINVAL);
    }
    let regs = regs::Regs::new(pdev, res)?;
    // Once power admission starts, even a rejected cold check must not detach the GFX
    // domain under an inherited firmware owner. Keep the module and platform binding; once
    // the module's removal has begun nothing is held.
    glue::retain_module()?;
    glue::hold_power(dev)?;
    let (power_offset, power_state) = glue::power_domain_state(dev)?;
    if power_offset != 0x250 || (power_state >> 4) & 0xf != 0xf {
        dev_err!(
            dev,
            "M4 GFX domain not admitted: register{:#x} state{:#x}\n",
            power_offset,
            power_state
        );
        return Err(Error::from_errno(-(bindings::EHOSTDOWN as i32)));
    }
    dev_info!(dev, "T8132 G16G admitted, ASC={:#x}\n", regs.asc_control()?);
    Ok(Admitted { regs, adt })
}

/// Posts one device control with only the opcode and waits for it.
fn devctl_op(
    session: &mut Session,
    channels: &mut Channels,
    events: &mut Events,
    init: &InitData,
    opcode: u32,
) -> Result {
    channel::devctl_submit(session, channels, events, init, &DevCtl::op(opcode), true)
}

/// 27.2: waits for one posted boot control to retire, polling the indices every 5 us without
/// draining or printing the rings, and returns how long it took. The firmware powers down about
/// 0.5 ms after its last work, so a boot control posted later wakes it from idle first.
fn await_boot_control_prompt(
    session: &Session,
    channels: &Channels,
    init: &InitData,
    target: u32,
) -> Result<Delta> {
    let start = Instant::<Monotonic>::now();
    loop {
        session.check()?;
        let read = channels.devctl.read()?;
        let done = channels.devctl.completion()?;
        if read == target && done == target && init.readiness()? == 1 {
            return Ok(start.elapsed());
        }
        if read > target || done > target || channels.devctl.write()? != target {
            dev_err!(session.dev, "G16 boot control {}: unexpected indices\n", target);
            channels.log_stop(session, init, "boot control indices");
            return Err(eproto());
        }
        if init.halted()? {
            dev_err!(session.dev, "G16 boot control {}: firmware halted\n", target);
            channels.log_stop(session, init, "halted awaiting boot control");
            return Err(EBUSY);
        }
        if start.elapsed() > Delta::from_millis(20) {
            dev_err!(
                session.dev,
                "G16 boot control {} not retired in 20 ms (read {} completion {})\n",
                target,
                read,
                done
            );
            channels.log_stop(session, init, "boot control timeout");
            return Err(ETIMEDOUT);
        }
        udelay(Delta::from_micros(5));
    }
}

/// Wait for one boot control to retire, requiring stable consumer+completion indices.
fn wait_boot_control(
    session: &Session,
    channels: &Channels,
    events: &mut Events,
    init: &InitData,
    target: u32,
    timeout_ms: i64,
) -> Result {
    let start = Instant::<Monotonic>::now();
    let mut stable = None;
    loop {
        channels.drain_rings(&session.dev, events)?;
        session.check()?;
        let ready = init.readiness()?;
        let read = channels.devctl.read()?;
        let done = channels.devctl.completion()?;
        let write = channels.devctl.write()?;
        if ready > 1
            || read < target - 1
            || read > target
            || done < target - 1
            || done > target
            || write != target
        {
            dev_err!(session.dev, "G16 boot control {}: unexpected indices\n", target);
            channels.log_stop(session, init, "boot control indices");
            return Err(eproto());
        }
        if ready == 1 && read == target && done == target {
            let at = stable.get_or_insert_with(Instant::<Monotonic>::now);
            if at.elapsed() >= Delta::from_millis(50) {
                dev_info!(
                    session.dev,
                    "G16 boot control {} retired ({} ms)\n",
                    target,
                    start.elapsed().as_millis()
                );
                return Ok(());
            }
        } else {
            stable = None;
        }
        if start.elapsed() > Delta::from_millis(timeout_ms) {
            dev_err!(
                session.dev,
                "M4 boot control target{} ready{} indices{}/{}/{}\n",
                target,
                ready,
                read,
                done,
                write
            );
            channels.log_stop(session, init, "boot control timeout");
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
}

/// Writes the register-map records into the firmware's table in loaded DATA, which must be
/// empty on cold entry. This is the only host write to the loaded DATA segment.
fn publish_loader_table(session: &Session, init: &InitData, offset: u64) -> Result {
    let table = init.loader_bytes();
    let loader = regs::Mapping::write_combined(session.adt.data.start + offset, table.len())?;
    for _ in 0..2 {
        for offset in (0..table.len()).step_by(4) {
            if loader.io().try_read32(offset)? != 0 {
                return Err(EBUSY);
            }
        }
        regs::rmb();
    }
    for (i, word) in table.chunks_exact(4).enumerate() {
        loader
            .io()
            .try_write32(u32::from_le_bytes(word.try_into().unwrap()), i * 4)?;
    }
    regs::wmb();
    Ok(())
}

/// Wake the sole firmware instance, publish its loader table, then qualify NOP and policy.
fn run(
    session: &mut Session,
    events: &mut Events,
    built: &mut (InitData, Channels, u32),
    log_buffer: Option<u64>,
) -> Result {
    let dev = session.dev.clone();
    let (init, channels, _) = built;
    let kind = session.adt.layout.start;
    match kind {
        hw::Start::Wake25G83 => session.rtkit.wake()?,
        hw::Start::MacOs27_2 => {
            use hw::start_27_2 as s;
            let va = log_buffer.ok_or(EINVAL)?;
            let mut log = LogBuffer {
                rtk: session.rtkit.as_raw(),
                shared: &*session.shared,
                message: s::MSG_LOG_BUFFER
                    | (((s::LOG_BUFFER_SIZE >> 12) as u64) << s::MSG_LOG_BUFFER_PAGES_SHIFT)
                    | (va & s::MSG_LOG_BUFFER_ADDR_MASK),
                result: 0,
            };
            // SAFETY: `log` lives on this stack frame across the call and nothing else
            // borrows it; the hook runs on this thread before `boot_with` returns.
            unsafe {
                session
                    .rtkit
                    .boot_with(publish_log_buffer, (&raw mut log).cast())?
            };
            to_result(log.result)?;
        }
    }
    let start = Instant::<Monotonic>::now();
    while matches!(
        session.shared.rtkit_buffer.load(Ordering::Acquire),
        0 | u64::MAX
    ) {
        session.check()?;
        if start.elapsed() > Delta::from_millis(1000) {
            return Err(ETIMEDOUT);
        }
        fsleep(Delta::from_micros(1000));
    }
    session.check()?;
    if !session.rtkit.running()
        || !session.rtkit.has_endpoint(1)
        || !session.rtkit.has_endpoint(hw::ep::FIRMWARE)
        || !session.rtkit.has_endpoint(hw::ep::DOORBELL)
    {
        return Err(eproto());
    }
    if kind == hw::Start::MacOs27_2 {
        let probe = session.regs.publish_host_block()?;
        dev_info!(dev, "G16: 27.2 host block published, scratch {:#x}\n", probe);
    }
    session.rtkit.start_endpoint(hw::ep::FIRMWARE)?;
    session.rtkit.start_endpoint(hw::ep::DOORBELL)?;
    let devctl = init.abi.devctl();
    if kind == hw::Start::Wake25G83 {
        for _ in 0..10 {
            fsleep(Delta::from_millis(10));
            channels.drain_rings(&dev, events)?;
            session.check()?;
        }
        session.regs.set_generation(0)?;
    }
    // 27.2 (candidate): no boot NOP. macOS writes no host generation before the InitData
    // doorbell, and on the J713 the firmware crashed at the first device-control doorbell after
    // idling behind the NOP; IMPORT_GLOBALS goes out right after InitData instead.
    let nop = kind == hw::Start::Wake25G83;
    if nop {
        channels.stage_boot_nop(session, devctl.nop)?;
    }
    match session.adt.layout.loader_table {
        Some(LoaderTable::Host(offset)) => publish_loader_table(session, init, offset)?,
        // The firmware copies the register map from HW data while it parses InitData.
        Some(LoaderTable::Firmware) => {}
        // Admission refuses such a layout; never guess here.
        None => return Err(ENOTSUPP),
    }
    let root = init.root_va()?;
    session.send(
        hw::ep::FIRMWARE,
        hw::ep::INITDATA | (root & hw::ep::INITDATA_ADDR_MASK),
    )?;
    session.send(hw::ep::DOORBELL, 0x0083000000000011)?;
    let base = if nop {
        wait_boot_control(session, channels, events, init, 1, 5000)?;
        1
    } else {
        dev_info!(dev, "G16: 27.2 boot: no boot NOP, IMPORT_GLOBALS right after InitData\n");
        0
    };
    let scheduler = init.control.as_ref().ok_or(EINVAL)?;
    let scheduler_word = init.firmware_data().scheduler;
    if kind == hw::Start::MacOs27_2 {
        // 27.2 (candidate): IMPORT_GLOBALS and START behind one doorbell, so the firmware never
        // idles between them. IMPORT_GLOBALS (0x35, fn 0x50cc) does not load region C into
        // firmware state; only START's power-up (0x36ec4 -> 0x3434) does. A separate
        // IMPORT_GLOBALS kick leaves the idle power-off delay at 0, and the J773g crashed in the
        // next kick's wake from that power-off, 29 us or 52 ms later (g16b, g16a); on the J713
        // the pair retired both.
        //
        // 26B123: only the firmware's GPU power-up path (0x36ec4, scheduler re-init 0x1d9b4) and
        // its recovery path consume the scheduler request, so InitData's request (1) is pending
        // here. Post START with it pending; never stack a second request.
        let pending = scheduler.r32(scheduler_word)?;
        if pending > 1 {
            channels.log_stop(session, init, "scheduler word unexpected before the boot pair");
            return Err(EBUSY);
        }
        if pending == 0 {
            scheduler.w32(scheduler_word, 1)?;
        }
        regs::wmb();
        channel::devctl_submit_batch(
            session,
            channels,
            events,
            init,
            &[DevCtl::op(devctl.import_globals), DevCtl::op(devctl.start)],
        )?;
        let retired = await_boot_control_prompt(session, channels, init, base + 2)?;
        session.send(hw::ep::DOORBELL, 0x0083000000000010)?;
        dev_info!(
            dev,
            "G16: 27.2 boot pair: IMPORT_GLOBALS and START behind one doorbell with scheduler request {} pending, both retired in {} us\n",
            pending,
            retired.as_micros_ceil()
        );
        // Settled: the same stable-index check as 25G83, with no boot doorbell left to send.
        wait_boot_control(session, channels, events, init, base + 2, 2000)?;
    } else {
        devctl_op(session, channels, events, init, devctl.import_globals)?;
        wait_boot_control(session, channels, events, init, base + 1, 5000)?;
        if scheduler.r32(scheduler_word)? != 0 {
            channels.log_stop(session, init, "scheduler word set after IMPORT_GLOBALS");
            return Err(EBUSY);
        }
        scheduler.w32(scheduler_word, 1)?;
        regs::wmb();
        devctl_op(session, channels, events, init, devctl.start)?;
        wait_boot_control(session, channels, events, init, base + 2, 2000)?;
        session.send(hw::ep::DOORBELL, 0x0083000000000010)?;
    }
    for _ in 0..10 {
        fsleep(Delta::from_millis(10));
        channels.drain_rings(&dev, events)?;
        session.check()?;
        if init.readiness()? != 1 {
            return Err(eproto());
        }
    }
    if channels.devctl.read()? != base + 2
        || channels.devctl.completion()? != base + 2
        || channels.devctl.write()? != base + 2
        || (kind == hw::Start::Wake25G83 && scheduler.r32(scheduler_word)? != 0)
    {
        channels.log_stop(session, init, "boot controls not settled after START");
        return Err(EBUSY);
    }
    dev_info!(
        dev,
        "M4 firmware accepted owned initdata root{:#x}, NOP and policy; READY (scheduler {})\n",
        root,
        scheduler.r32(scheduler_word)?
    );
    Ok(())
}

/// Brings the firmware to READY.
#[inline(never)]
pub(crate) fn start(
    admitted: Admitted,
    dev: &AsahiDrmDevice,
    cfg: &'static Config,
    shared: &Arc<Shared>,
) -> Result<Booted> {
    let Admitted { regs, adt } = admitted;
    let d: ARef<device::Device> = shared.dev.clone();
    // Cold ownership must hold before the PMP vote as well as immediately before RUN.
    if regs.asc_control()? & regs::ASC_RUN != 0 {
        return Err(EBUSY);
    }
    let kind = adt.layout.start;
    if kind == hw::Start::MacOs27_2 {
        // macOS 27.2 sends no PMP AGX vote: it reads the identification words and starts the
        // firmware without one, and the firmware acknowledges its own IOP power-on. A J713
        // 27.2 boot without the vote reached the same point as one with it.
        dev_info!(d, "G16: 27.2: no PMP AGX vote, as macOS\n");
        regs.enable_axi2af()?;
    } else {
        // SAFETY: `d` is the bound GPU device.
        let ret = unsafe { bindings::apple_pmp_j713_gpu_get(d.as_raw()) };
        if ret != 0 {
            dev_err!(d, "PMP AGX vote failed: {}\n", ret);
            return Err(Error::from_errno(ret));
        }
    }
    let id = regs.identity()?;
    dev_info!(
        d,
        "M4 G16G: power acknowledged, version={:#x} counts={:#x} core-mask={:#x}\n",
        id.version,
        id.counts,
        id.cores
    );
    if !cfg
        .hw_ids
        .iter()
        .any(|h| h.version == id.version && h.counts == id.counts)
    {
        dev_err!(
            d,
            "unsupported G16G hardware: version {:#x} counts {:#x}\n",
            id.version,
            id.counts
        );
        return Err(eproto());
    }
    // M4 has one cluster with up to ten enabled cores.
    if id.core_masks[0] == 0 || id.cores >> hw::CLUSTER_BITS != 0 {
        dev_err!(d, "implausible G16G core mask {:#x}\n", id.cores);
        return Err(eproto());
    }
    dev_info!(
        d,
        "M4 G16G: {} cores enabled\n",
        id.core_masks[0].count_ones()
    );
    if regs.pstate()? > regs.top() {
        return Err(eproto());
    }
    // Cold only: ASC RUN clear and an all-zero TTBAT before ownership.
    let control = regs.asc_control()?;
    if control & regs::ASC_RUN != 0 {
        return Err(EBUSY);
    }
    let window = uat::Window::new(&adt)?;
    if !window.ttbat_is_clear()? {
        return Err(EBUSY);
    }
    let handoff = match adt.layout.powered {
        Some(_) => Some(regs::Mapping::write_combined(
            adt.handoff.start,
            hw::HANDOFF_SIZE as usize,
        )?),
        None => None,
    };
    // Adopt the UAT and prove the leaf words before anything firmware-visible is written: no
    // context names the shadow window or the lower root yet.
    let uat = uat::adopt(dev, cfg, &adt)?;
    let mut objects = Objects::new(dev, uat.clone(), window)?;
    if let Err(e) = objects.self_check() {
        // Mapped with cached firmware classes: never unmapped.
        core::mem::forget(objects);
        return Err(e);
    }

    // From here on the firmware is touched: nothing it can reach is freed. Admission pinned
    // the module, so this only confirms it.
    if let Err(e) = glue::retain_module() {
        core::mem::forget(objects);
        return Err(e);
    }
    let ops = match kind {
        hw::Start::Wake25G83 => &RTKIT_OPS,
        hw::Start::MacOs27_2 => &RTKIT_OPS_27_2,
    };
    let rtkit = match Rtkit::new(&d, shared, ops) {
        Ok(rtkit) => rtkit,
        Err(e) => {
            core::mem::forget(objects);
            return Err(e);
        }
    };
    let mut session = Session {
        dev: d.clone(),
        shared: shared.clone(),
        rtkit,
        regs,
        adt,
        uat,
        handoff,
        sent: [(0, 0, 0); SENT_LOG],
        sent_next: 0,
    };
    let mut events = Events::new();
    let mut built = None;
    let result = (|| {
        events.reserve()?;
        // Every host object and every context-0 alias is prepared before RUN is set.
        built = Some(initdata::build(&d, &session.adt, &mut objects)?);
        if let Some((init, _, _)) = built.as_ref() {
            // The running firmware may report any state of the board's table, up to S10 on J773g.
            session.regs.set_table(init.rows);
        }
        let log_buffer = match kind {
            hw::Start::MacOs27_2 => Some(
                objects
                    .alloc(Request {
                        size: hw::start_27_2::LOG_BUFFER_SIZE,
                        class: Class::Fw,
                        placement: Placement::Anywhere,
                        offset: 0,
                        client: None,
                        lower: None,
                        cpu: true,
                    })?
                    .va,
            ),
            hw::Start::Wake25G83 => None,
        };
        objects.window.lock().bootstrap(&d, &session.uat)?;
        uat::tlbi_all();
        if session.regs.asc_control()? & regs::ASC_RUN != 0 {
            return Err(EBUSY);
        }
        shared.started.store(true, Ordering::Release);
        session.regs.set_asc_control(control | regs::ASC_RUN)?;
        regs::mb();
        if session.regs.asc_control()? & regs::ASC_RUN == 0 {
            return Err(EIO);
        }
        objects.window.lock().await_firmware_magic(&d)?;
        run(
            &mut session,
            &mut events,
            built.as_mut().ok_or(EINVAL)?,
            log_buffer,
        )
    })();
    if let Err(e) = result {
        dev_err!(
            d,
            "G16 start failed: {:?}; session retained until reboot\n",
            e
        );
        let (init, channels) = match built {
            Some((init, channels, _)) => (Some(init), Some(channels)),
            None => (None, None),
        };
        shared.orphan(Orphan {
            session,
            _objects: objects,
            channels,
            _init: init,
            events,
        });
        return Err(e);
    }
    let (init, channels, max_frequency_khz) = built.ok_or(EINVAL)?;
    Ok(Booted {
        session,
        init,
        objects,
        channels,
        events,
        id,
        max_frequency_khz,
    })
}
